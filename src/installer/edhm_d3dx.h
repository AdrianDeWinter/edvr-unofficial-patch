// Surgical reading and editing of EDHM / 3Dmigoto's d3dx.ini beside the game.
//
// 3Dmigoto's d3dx.ini uses ';' for comments, holds user theme and keybinding
// settings, and must never be overwritten wholesale on a toggle or restore.
// Everything here reads or updates individual [Section] keys in place while
// preserving unrelated sections, comments, indentation, inline comments, BOM,
// and line endings.
#pragma once

#include <string>

#include "state.h"

namespace edvr::installer {

struct D3dxKeyRead {
    bool present = false;
    std::string value;
};

// Result of evaluating 2DMigoto's effective compatibility settings across d3dx.ini,
// any [Include] files (include / include_recursive, honoring exclude_recursive and
// preamble condition), and [Include] user_config (default d3dx_user.ini).
struct EdhmD3dxEffectiveCompat {
    bool satisfied = true;
    bool allowPlatformUpdateOk = true;
    bool upscalingOk = true;
    // Relative path under the game folder (e.g. "d3dx_user.ini" or "EDHM-ini\\override.ini")
    // when the effective incompatible value came from an included or user config file rather
    // than the main d3dx.ini.
    std::string allowPlatformUpdateOverrideFile;
    std::string upscalingOverrideFile;

    bool hasExternalOverrideFailure() const {
        return (!allowPlatformUpdateOk && !allowPlatformUpdateOverrideFile.empty()) ||
               (!upscalingOk && !upscalingOverrideFile.empty());
    }
};

// Returns true if `bytes` looks like UTF-16 (UTF-16 LE/BE BOM or embedded NUL
// bytes). ED-UP only edits UTF-8 / ASCII d3dx.ini files and refuses UTF-16
// rather than appending ASCII bytes onto a UTF-16 file.
bool isUtf16D3dx(const std::string& bytes);

// Reads the first active (uncommented, non-empty) occurrence of `key` in the first
// `[section]` of that name, matching 2DMigoto's IniHandler.cpp (first section
// wins, first non-empty key wins via emplace). Section and key matching are ASCII
// case-insensitive. Lines whose first non-whitespace character is ';' are
// comments; inline ';' comments are stripped from the returned value for surgical
// editing helpers.
D3dxKeyRead readD3dxKey(const std::string& iniText, const char* section, const char* key);

// Sets `key` in the first `[section]` of that name to `value`, preserving
// comments, unrelated keys, inline comments, indentation, UTF-8 BOM, and
// line-ending style (CRLF or LF).
// - Only the first active occurrence in the first `[section]` (preferring the
//   first non-empty value, matching 2DMigoto) is updated in place, so duplicate
//   keys or repeated sections are not collapsed.
// - Otherwise, if a commented `; key = ...` line exists in that first
//   `[section]`, `key = value` is inserted immediately after the first
//   commented occurrence so the template comment stays intact.
// - Otherwise, if `[section]` exists, `key = value` is appended at the end of
//   the first `[section]`.
// - Otherwise, `[section]` and `key = value` are appended at the end of the file.
std::string setD3dxKey(const std::string& iniText, const char* section, const char* key,
                       const std::string& value);

// Removes the first active (uncommented) occurrence of `key` in the first
// `[section]` of that name while keeping commented lines and any later
// duplicate keys or sections intact.
std::string removeD3dxKey(const std::string& iniText, const char* section, const char* key);

// Evaluates 2DMigoto's effective compatibility values across d3dx.ini, its [Include]
// graph (contained within `gameDir`), and its user_config file (default d3dx_user.ini),
// matching EDHM_2DMigoto v0.1.9 (commit 716157b8, DirectX11/IniHandler.cpp):
//   - Full-line ';' comments only; mid-line ';' is NOT stripped (IniHandler.cpp:718-729).
//   - Empty values are ignored by ParseIniKeyValLine (IniHandler.cpp:637-645).
//   - [System] allow_platform_update defaults to true in v0.1.9+ (IniHandler.cpp:5051;
//     enforced by MIN_ALLOW_PLATFORM_UPDATE_DEFAULT_TRUE_VERSION in
//     tools/fetch_edhm_2dmigoto.py) and parses via ParseBoolValue + static expression
//     evaluation with fallback to default true (IniHandler.cpp:1299-1312, 1413-1449).
//   - [Device] upscaling defaults to 0 (IniHandler.cpp:5141) and parses via
//     ParseIntValue + static expression evaluation with fallback to default 0
//     (IniHandler.cpp:1359-1411).
//   - Included files and user_config override global [System]/[Device] keys last-write-wins
//     (IniHandler.cpp:488-492, 647-652, 1682-1793, 5027-5034).
// (v0.1.9 forces load_library_redirect = 0 automatically when loaded as
// d3d11_edhm.dll at IniHandler.cpp:5070-5075 & D3D11Wrapper.cpp:1228, so
// ED-UP does not touch load_library_redirect.)
EdhmD3dxEffectiveCompat evaluateEdhmD3dxCompat(const std::wstring& gameDir, const std::string& d3dxIniText);

// Single-file compatibility check over in-memory d3dx.ini text (no filesystem includes).
bool edhmD3dxCompatSatisfied(const std::string& iniText);

// Formats external include/user_config compatibility failures for user-facing problem text
// (e.g. "[System] allow_platform_update in d3dx_user.ini").
std::string describeEdhmD3dxOverrideFailure(const EdhmD3dxEffectiveCompat& compat);

// Captures the original values of ONLY the compatibility keys in d3dx.ini that currently
// fail effective compatibility (and will therefore be changed by applyEdhmD3dxCompat).
EdhmD3dxBackup snapshotEdhmD3dxCompat(const std::string& iniText);
EdhmD3dxBackup snapshotEdhmD3dxCompat(const std::string& iniText, const EdhmD3dxEffectiveCompat& effective);

// Fixes only incompatible values in d3dx.ini:
//   [System] allow_platform_update -> 1 (only when effectively failing from d3dx.ini)
//   [Device] upscaling -> 0 (only when effectively failing from d3dx.ini)
// Leaves already-compatible d3dx.ini text byte-identical.
std::string applyEdhmD3dxCompat(const std::string& iniText);
std::string applyEdhmD3dxCompat(const std::string& iniText, const EdhmD3dxEffectiveCompat& effective);

// Formats the backed-up keys and their original values for manual restoration
// notices (e.g. when uninstall cannot edit a read-only or UTF-16 d3dx.ini).
std::string describeEdhmD3dxBackup(const EdhmD3dxBackup& backup);

// Restores any backed-up ED-UP compatibility keys (including migrated legacy
// load_library_redirect backups, and backed-up [Logging] keys if recorded) ONLY
// while each key still holds ED-UP's applied value, leaving later player or
// EDHM-UI edits intact.
std::string restoreEdhmD3dxCompat(const std::string& iniText, const EdhmD3dxBackup& backup);

// Restores a single backed-up [Logging] key when unchecking its checkbox.
// When `hasSaved` is true, restores the recorded value (or removes the key if
// originally absent) only while it still holds "0". Otherwise sets "1".
std::string restoreEdhmLoggingKey(const std::string& iniText, const char* key,
                                  const EdhmD3dxKeyBackup& saved, bool hasSaved);

// Reads [Logging] show_reload_message from d3dx.ini.
// Returns true ("On" / hidden) when show_reload_message is "0";
// returns false ("Off" / shown) when "1", another value, or absent.
bool readEdhmHideReloadMessages(const std::string& iniText);

// Sets [Logging] show_reload_message in d3dx.ini to "0" (hide = true) or "1" (hide = false).
std::string setEdhmHideReloadMessages(const std::string& iniText, bool hide);

// Reads [Logging] show_warnings from d3dx.ini.
// Returns true ("On" / hidden) when show_warnings is "0";
// returns false ("Off" / shown) when "1", another value, or absent.
bool readEdhmHideWarnings(const std::string& iniText);

// Sets [Logging] show_warnings in d3dx.ini to "0" (hide = true) or "1" (hide = false).
std::string setEdhmHideWarnings(const std::string& iniText, bool hide);

}  // namespace edvr::installer
