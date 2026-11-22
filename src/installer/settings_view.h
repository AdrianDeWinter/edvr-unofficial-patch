// The scrolling list of settings.
//
// A child window that draws itself: one row per setting, grouped by the ini's
// own sections, with the control on the right, the word it is set to under it,
// and one link back to the recommended value when it is not there already.
// Custom-drawn for the same reason the rest of the window is --
// nothing to install, and a hundred real child controls to build, move and
// theme is more machinery than one paint function. A number is typed into
// one shared text box. A choice with more than three options, or one whose
// labels would be cut off as buttons, uses one shared dropdown the same way.
//
// Editing writes straight into edvr.ini. There is no Apply button because there
// is nothing to apply to: EDVR re-reads that file about once a second, so the
// change is live by the time you have let go of the mouse.
#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

#include "settings.h"

namespace edvr::installer {

enum class LauncherRowTone { Muted, Accent, Good, Warn, Bad };
enum class LauncherControlKind { Toggle, Checkbox };

// A launcher-managed option rendered cleanly inside the Settings options list
// (outside edup.ini), with wrapped label, inline status, optional action link,
// and an optional warning strip below the row (e.g. antivirus interference).
struct LauncherToggleRow {
    bool visible = false;
    LauncherControlKind control = LauncherControlKind::Toggle;
    bool enabled = true;
    std::wstring group;
    std::wstring label;
    std::wstring description;
    std::wstring searchTerms;
    bool on = false;
    bool busy = false;
    std::wstring statusText;
    LauncherRowTone statusTone = LauncherRowTone::Muted;
    std::wstring actionLink;
    std::wstring warning;
    std::function<void(bool)> onToggle;
    std::function<void()> onAction;
};

HWND createSettingsList(HWND parent, int id, UINT dpi);

// mirrorDir is where a change gets echoed outside the game folder (see
// mirror.h); empty is a valid value meaning "no mirror for this install",
// which a write simply skips.
void settingsListSetModel(HWND list, SettingsModel* model, const std::wstring& mirrorDir);
void settingsListSetLauncherToggle(HWND list, const LauncherToggleRow& row);
void settingsListSetLauncherToggles(HWND list, std::vector<LauncherToggleRow> rows);
void settingsListSetFilter(HWND list, const std::wstring& needle);
void settingsListRescale(HWND list, UINT dpi);

// Set when a write fails, so the screen above can say so.
const std::string& settingsListLastError(HWND list);

}  // namespace edvr::installer
