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

// Reads the last active (uncommented) occurrence of `key` under `[section]`.
// Section and key matching are ASCII case-insensitive. Lines whose first
// non-whitespace character is ';' are comments; inline ';' comments are
// stripped from the returned value.
D3dxKeyRead readD3dxKey(const std::string& iniText, const char* section, const char* key);

// Sets `key` under `[section]` to `value`, preserving comments, unrelated keys,
// inline comments, indentation, UTF-8 BOM, and line-ending style (CRLF or LF).
// - Only the last active occurrence in `[section]` (the effective last-wins
//   entry read by readD3dxKey) is updated in place, so duplicate keys across
//   repeated sections are not collapsed.
// - Otherwise, if a commented `; key = ...` line exists in `[section]`,
//   `key = value` is inserted immediately after the last commented occurrence
//   so the template comment stays intact.
// - Otherwise, if `[section]` exists, `key = value` is appended at the end of
//   that section.
// - Otherwise, `[section]` and `key = value` are appended at the end of the file.
std::string setD3dxKey(const std::string& iniText, const char* section, const char* key,
                       const std::string& value);

// Removes the last active (uncommented) occurrence of `key` under `[section]`
// while keeping commented lines and any earlier duplicate lines intact.
std::string removeD3dxKey(const std::string& iniText, const char* section, const char* key);

// Captures the original values of the three ED-UP compatibility keys:
//   [System] load_library_redirect  (EDHM_2DMigoto DirectX11/D3D11Wrapper.cpp: >1 redirects LoadLibrary("d3d11.dll") to outer ED-UP)
//   [System] allow_platform_update  (EDHM_2DMigoto DirectX11/HackerDevice.cpp & HackerContext.cpp: 0 returns E_NOINTERFACE for Device1/Context1)
//   [Device] upscaling              (EDHM_2DMigoto DirectX11/HackerDXGI.cpp: legacy swapchain/backbuffer upscaler)
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
