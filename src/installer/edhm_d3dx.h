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

// Captures the original values of the three ED-UP compatibility keys
// (verified against pinned Fenris159/EDHM_2DMigoto@716157b8, v0.1.9):
//   [System] allow_platform_update  (required guard: v0.1.9 IniHandler.cpp:5051 defaults true, but an explicit 0 in d3dx.ini makes HackerDevice.cpp:1704 & HackerContext.cpp:1374 return E_NOINTERFACE for Device1/Context1, breaking flat_mono_resolve.cpp:604-619)
//   [System] load_library_redirect  (defensive: v0.1.9 IniHandler.cpp:5070-5075 & D3D11Wrapper.cpp:1228 also force 0 when loaded as chained d3d11_edhm.dll; ED-UP handles System32 via system_d3d11.h)
//   [Device] upscaling              (guard on default 0: IniHandler.cpp:5141 & HackerDXGI.cpp legacy swapchain upscaler)
EdhmD3dxBackup snapshotEdhmD3dxCompat(const std::string& iniText);

// Applies ED-UP compatibility values to d3dx.ini:
//   [System] load_library_redirect = 0
//   [System] allow_platform_update = 1
//   [Device] upscaling = 0
std::string applyEdhmD3dxCompat(const std::string& iniText);

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
