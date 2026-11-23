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

// Returns true if `bytes` looks like UTF-16 (UTF-16 LE/BE BOM or embedded NUL
// bytes). ED-UP only edits UTF-8 / ASCII d3dx.ini files and refuses UTF-16
// rather than appending ASCII bytes onto a UTF-16 file.
bool isUtf16D3dx(const std::string& bytes);

// Reads the first active (uncommented) occurrence of `key` in the first
// `[section]` of that name, matching 2DMigoto's IniHandler.cpp (first section
// wins, first key wins via emplace). Section and key matching are ASCII
// case-insensitive. Lines whose first non-whitespace character is ';' are
// comments; inline ';' comments are stripped from the returned value.
D3dxKeyRead readD3dxKey(const std::string& iniText, const char* section, const char* key);

// Sets `key` in the first `[section]` of that name to `value`, preserving
// comments, unrelated keys, inline comments, indentation, UTF-8 BOM, and
// line-ending style (CRLF or LF).
// - Only the first active occurrence in the first `[section]` (the effective
//   first-wins entry read by 2DMigoto and readD3dxKey) is updated in place,
//   so duplicate keys or repeated sections are not collapsed.
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

// Returns true when d3dx.ini is UTF-8/ASCII and its effective compatibility
// values already satisfy ED-UP + EDHM_2DMigoto v0.1.9 (commit 716157b8):
//   [System] allow_platform_update  absent or truthy (1/true/yes/on); v0.1.9
//                                   IniHandler.cpp:5051 defaults true, while an
//                                   explicit false causes HackerDevice.cpp:1704
//                                   and HackerContext.cpp:1374 to return
//                                   E_NOINTERFACE for Device1/Context1, breaking
//                                   flat_mono_resolve.cpp:604-619.
//   [Device] upscaling              absent or 0 (IniHandler.cpp:5141 &
//                                   HackerDXGI.cpp legacy swapchain upscaler).
// (v0.1.9 forces load_library_redirect = 0 automatically when loaded as
// d3d11_edhm.dll at IniHandler.cpp:5070-5075 & D3D11Wrapper.cpp:1228, so
// ED-UP does not touch load_library_redirect.)
bool edhmD3dxCompatSatisfied(const std::string& iniText);

// Captures the original values of ONLY the compatibility keys that currently
// fail edhmD3dxCompatSatisfied (and will therefore be changed by
// applyEdhmD3dxCompat).
EdhmD3dxBackup snapshotEdhmD3dxCompat(const std::string& iniText);

// Fixes only incompatible values in d3dx.ini:
//   [System] allow_platform_update -> 1 (only when explicitly present and non-truthy)
//   [Device] upscaling -> 0 (only when explicitly present and non-zero)
// Leaves already-compatible d3dx.ini text byte-identical.
std::string applyEdhmD3dxCompat(const std::string& iniText);

// Formats the backed-up keys and their original values for manual restoration
// notices (e.g. when uninstall cannot edit a read-only or UTF-16 d3dx.ini).
std::string describeEdhmD3dxBackup(const EdhmD3dxBackup& backup);

// Restores any backed-up ED-UP compatibility keys (and backed-up [Logging]
// keys, if recorded) ONLY while each key still holds ED-UP's applied value,
// leaving later player or EDHM-UI edits intact.
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
