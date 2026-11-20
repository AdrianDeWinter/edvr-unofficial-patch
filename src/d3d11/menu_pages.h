// The F8 menu's pages as one table, for both profiles (docs\design-f8-menu-unification-2026-11-15.md, "One page table").
//
// buildPages (menu.cpp) is one loop over kMenuPages, keeping the entries the running profile shows (and, for a developer page, only with developer mode on).
// What a page's rows are is its `source`: the generated schema's rows of one tier and `menu` token (Settings), one of the flat profile's three row tables
// (menu_flat_rows.h), the hotkey rows, the plugin rows, the status and monitor panes, or the registered actions. The rows come from those sources and are
// covered where they are generated; this table is only which pages exist, in what order, for whom.
//
// Pure: no windows.h, no D3D. tools\menu_policy_test pins the page lists it yields.
#pragma once
#include <cstddef>
#include "menu_flat_rows.h"

namespace edvr {

enum class MenuPageSource { Settings, FlatPerformance, FlatVisual, FlatDebug, Hotkeys, Plugins, Monitor, Status, Actions };
enum class MenuPageTier { Fix, Advanced, Experimental };   // the generated schema's MenuTier, by name, so this header needs no schema
enum class MenuPagePost { None, AimChoiceAfterLookToAim };  // a step run on the page's entries once they are built

struct MenuPageDef {
    const char* name;
    bool flat;                 // the flat profile shows it
    bool vr;                   // the VR profile shows it
    bool developer;            // only with developer mode on
    MenuPageSource source;
    MenuPageTier tier;         // Settings only
    const char* token;         // Settings only: the `menu` token naming the page in edvr.ini; nullptr takes every page's rows
    bool grouped;              // Settings only: the ini's heading blocks head the rows
    MenuPagePost post;
};

// Order is the tab order. Flat's Performance, Visual and Debug names are kFlatTabNames (the Bloom note finds the Visual tab by it); its Hotkeys and Status are the VR
// ones, shown to both, and the flat Debug entry sits after it so that Debug stays last.
inline constexpr MenuPageDef kMenuPages[] = {
    {kFlatTabNames[0], true, false, false, MenuPageSource::FlatPerformance, MenuPageTier::Fix, nullptr, false, MenuPagePost::None},
    {kFlatTabNames[1], true, false, false, MenuPageSource::FlatVisual, MenuPageTier::Fix, nullptr, false, MenuPagePost::None},
    {"Performance", false, true, false, MenuPageSource::Settings, MenuPageTier::Fix, "performance", false, MenuPagePost::None},
    {"Fixes", false, true, false, MenuPageSource::Settings, MenuPageTier::Fix, "fixes", true, MenuPagePost::None},
    {"Visual", false, true, false, MenuPageSource::Settings, MenuPageTier::Fix, "visual", true, MenuPagePost::None},
    {"Mirror window", false, true, false, MenuPageSource::Settings, MenuPageTier::Fix, "desktop_window", false, MenuPagePost::None},
    {"Native on foot", false, true, false, MenuPageSource::Settings, MenuPageTier::Fix, "native_on_foot", true, MenuPagePost::AimChoiceAfterLookToAim},
    {"Hotkeys", true, true, false, MenuPageSource::Hotkeys, MenuPageTier::Fix, nullptr, false, MenuPagePost::None},
    {"Plugins", false, true, false, MenuPageSource::Plugins, MenuPageTier::Fix, nullptr, false, MenuPagePost::None},
    // Flat shows the Monitor as a panel of its own (hotkey.monitor_panel; menu_policy.h monitorPanel), not as a tab.
    {"Monitor", false, true, false, MenuPageSource::Monitor, MenuPageTier::Fix, nullptr, false, MenuPagePost::None},
    {"Status", true, true, false, MenuPageSource::Status, MenuPageTier::Fix, nullptr, false, MenuPagePost::None},
    {kFlatTabNames[2], true, false, false, MenuPageSource::FlatDebug, MenuPageTier::Fix, nullptr, false, MenuPagePost::None},
    {"Advanced", false, true, true, MenuPageSource::Settings, MenuPageTier::Advanced, "advanced", true, MenuPagePost::None},
    {"Experimental", false, true, true, MenuPageSource::Settings, MenuPageTier::Experimental, "experimental", true, MenuPagePost::None},
    {"Instruments", false, true, true, MenuPageSource::Actions, MenuPageTier::Fix, nullptr, false, MenuPagePost::None},
};
inline constexpr size_t kMenuPageCount = sizeof(kMenuPages) / sizeof(kMenuPages[0]);

inline bool menuPageShown(const MenuPageDef& d, bool flatProfile, bool developer) {
    return (flatProfile ? d.flat : d.vr) && (!d.developer || developer);
}

// Whether a profile has a page of this source at all (developer pages excluded: they are not the profile's own). This is the one answer to "does this profile
// have a Hotkeys page", which menuTick asks to run a row's capture, its note timer and the clash reading (the policy of menu_policy.h no longer carries it).
inline constexpr bool menuProfileHasPage(MenuPageSource source, bool flatProfile) {
    for (size_t i = 0; i < kMenuPageCount; ++i)
        if (kMenuPages[i].source == source && !kMenuPages[i].developer && (flatProfile ? kMenuPages[i].flat : kMenuPages[i].vr)) return true;
    return false;
}

}  // namespace edvr
