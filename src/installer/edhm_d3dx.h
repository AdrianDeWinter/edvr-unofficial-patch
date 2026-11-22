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

// Reads the last active (uncommented) occurrence of `key` under `[section]`.
// Section and key matching are ASCII case-insensitive. Lines whose first
// non-whitespace character is ';' are comments; inline ';' comments are
// stripped from the returned value.
D3dxKeyRead readD3dxKey(const std::string& iniText, const char* section, const char* key);

// Sets `key` under `[section]` to `value`, preserving comments, unrelated keys,
// inline comments, indentation, UTF-8 BOM, and line-ending style (CRLF or LF).
// - Active occurrence(s) in `[section]` are updated in place.
// - Otherwise, if a commented `; key = ...` line exists in `[section]`,
//   `key = value` is inserted immediately after the last commented occurrence
//   so the template comment stays intact.
// - Otherwise, if `[section]` exists, `key = value` is appended at the end of
//   that section.
// - Otherwise, `[section]` and `key = value` are appended at the end of the file.
std::string setD3dxKey(const std::string& iniText, const char* section, const char* key,
                       const std::string& value);

// Removes all active (uncommented) occurrences of `key` under `[section]`
// while keeping commented lines and all other keys/sections intact.
std::string removeD3dxKey(const std::string& iniText, const char* section, const char* key);

// Captures the original values of the three ED-UP compatibility keys:
//   [System] load_library_redirect
//   [System] allow_platform_update
//   [Device] upscaling
EdhmD3dxBackup snapshotEdhmD3dxCompat(const std::string& iniText);

// Applies ED-UP compatibility values to d3dx.ini:
//   [System] load_library_redirect = 0
//   [System] allow_platform_update = 1
//   [Device] upscaling = 0
std::string applyEdhmD3dxCompat(const std::string& iniText);

// Restores the three ED-UP compatibility keys from `backup`, leaving all other
// keys (including [Logging] show_reload_message and EDHM themes) untouched.
std::string restoreEdhmD3dxCompat(const std::string& iniText, const EdhmD3dxBackup& backup);

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
