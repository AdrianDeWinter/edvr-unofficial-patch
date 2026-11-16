#pragma once
// What differs between the F8 menu's two profiles, as data, and the pure decisions menuTick makes from it (menu.cpp).
//
// The flat profile's menu is a card on the screen; the VR profile's is a panel in the world. menuConfigure, menuTick, openMenu and closeMenu used to be
// two branches each, one per profile. They are one body each now, and every place the profiles differ asks a MenuProfilePolicy. A later step of
// docs\design-f8-menu-unification-2026-11-15.md turns one VR-only capability at a time into a policy field that flat switches on.
//
// Pure: no windows.h, no D3D, no clock, no config. tools\menu_policy_test drives all of it. A policy field is what the profile's menu IS, not what a user sets.
#include <cstdint>

namespace edvr {

enum class MenuAnchor { Screen, World };
enum class MenuSizing { OutputPx, HeadsetPpd };
enum class MenuNoticePlacement { ScreenCorner, WorldToast };

// (Whether a profile has a Hotkeys page is not a policy field: the page table says it, menu_pages.h menuProfileHasPage.)
struct MenuProfilePolicy {
  MenuAnchor anchor = MenuAnchor::Screen;          // a card on the screen, or a panel anchored in the world
  MenuSizing sizing = MenuSizing::OutputPx;         // sized from the output's own pixels, or sized from the headset's pixels per degree
  bool fade = false;                               // the panel fades in and out
  bool headAim = false;                            // the head aims at rows; the aiming choice page exists
  bool adoptGameKeys = false;                      // Elite's own panel keys are adopted as menu keys
  MenuNoticePlacement noticePlacement = MenuNoticePlacement::ScreenCorner;
  bool keyboardAlwaysPrivate = false;              // menu.keyboard is not read: the panel always takes the keyboard
  bool readsMenuKeys = false;                      // the [menu] keys are read (flat reads none of them)
  bool padNavigation = false;                      // the panel is navigated by a pad
  bool closesOnFocusLoss = false;                  // the menu closes when Elite loses the focus
  bool primesHotkeysOnClose = false;               // a held navigation key must not become a hotkey on close
  bool monitorPanel = false;                       // the Monitor is a panel of its own, on its own key (hotkey.monitor_panel), not a tab of the menu
};

inline MenuProfilePolicy menuPolicyFor(bool flatProfile) {
  MenuProfilePolicy p;
  if (flatProfile) {
    p.anchor = MenuAnchor::Screen;
    p.sizing = MenuSizing::OutputPx;
    p.noticePlacement = MenuNoticePlacement::ScreenCorner;
    p.keyboardAlwaysPrivate = true;
    p.padNavigation = true;
    p.closesOnFocusLoss = true;
    p.primesHotkeysOnClose = true;
    p.monitorPanel = true;
    return p;
  }
  p.anchor = MenuAnchor::World;
  p.sizing = MenuSizing::HeadsetPpd;
  p.fade = true;
  p.headAim = true;
  p.adoptGameKeys = true;
  p.noticePlacement = MenuNoticePlacement::WorldToast;
  p.readsMenuKeys = true;
  return p;
}

// ---- The summon key ----------------------------------------------------------------------------------------------------------------------------
enum class MenuSummonAction { None, Open, Close, Recentre };

// One pressed edge of the summon key. A hotkey row waiting for its input (captureActive) takes the edge as a candidate, not a command. A pad binding that
// overlaps panel navigation still opens the panel, but once the panel owns the pad (padNavSummon && padReady) it is navigation, not a close. With the
// menu open, Shift recentres where the world anchors the panel (canRecentre); otherwise the key closes.
inline MenuSummonAction menuSummonAction(bool pressed, bool open, bool padNavSummon, bool padReady, bool captureActive, bool shiftDown, bool canRecentre) {
  if (!pressed || captureActive) return MenuSummonAction::None;
  if (open && padNavSummon && padReady) return MenuSummonAction::None;
  if (!open) return MenuSummonAction::Open;
  if (canRecentre && shiftDown) return MenuSummonAction::Recentre;
  return MenuSummonAction::Close;
}

// ---- Escape --------------------------------------------------------------------------------------------------------------------------------------
enum class MenuEscapeAction { None, CancelEdit, Close };

// One tick with the menu open. `down` is the tracker of the key: updated every call, so the press that was consumed (by a capture, or a typed value) cannot act
// again a tick later. A hotkey row waiting for its input owns Escape (captureOwns): it cancels the capture itself. Escape ends a value being typed before it
// closes the menu, so a typed value can be abandoned without losing the panel.
inline MenuEscapeAction menuEscapeAction(bool escDown, bool& down, bool captureOwns, bool editing) {
  const bool edge = escDown && !down;
  down = escDown;
  if (!edge || captureOwns) return MenuEscapeAction::None;
  return editing ? MenuEscapeAction::CancelEdit : MenuEscapeAction::Close;
}

// ---- The draw the gate follows ----------------------------------------------------------------------------------------------------------------
// A panel whose export ran within freshMs is being seen; before the first draw (lastDrawnMs == 0) it is not.
inline bool menuDrawnFresh(bool open, uint64_t now, uint64_t lastDrawnMs, uint64_t freshMs) {
  return open && lastDrawnMs != 0 && now - lastDrawnMs <= freshMs;
}
// An open menu that has not been drawn at all, since it opened or since the last draw, for closeMs closes itself.
inline bool menuNotDrawn(bool open, uint64_t now, uint64_t openedMs, uint64_t lastDrawnMs, uint64_t closeMs) {
  return open && now - openedMs > closeMs && now - lastDrawnMs > closeMs;
}

// ---- The notice on the flat screen -------------------------------------------------------------------------------------------------------------
// The flat screen draws no toasts: a notice is the panel's own raster at a toast's size, composited in the upper left corner while it is up. `shown` is set
// once that raster (not the menu's) is the live one; `rasterStale` holds the panel back after a notice until the menu's own content has been rasterised, so a
// toast-sized card never flashes as the menu. The queue that keeps a notice raised while the menu is open is flat_notice_queue.h; this is the machine after it.
struct ScreenNoticeState {
  bool up = false;
  bool shown = false;
  bool rasterStale = false;
  uint64_t untilMs = 0;
  uint32_t serial = 0;

  // A notice was taken from the queue and its raster submitted; `rasterSerial` is the panel's raster serial read before that submit.
  void start(uint64_t now, uint64_t durationMs, uint32_t rasterSerial) {
    up = true;
    shown = false;
    untilMs = now + durationMs;
    serial = rasterSerial;
  }
  // One tick after the content was submitted. Returns true when the menu's content must be re-sent (the notice ended). `rasterIsNoticeSized` is asked only
  // while a notice waits to be shown: whether the live raster is as small as a notice (the menu's smallest page is far taller).
  template <class F>
  bool tick(bool menuOpen, uint64_t now, uint32_t rasterSerial, F rasterIsNoticeSized) {
    bool resend = false;
    if (up) {
      if (menuOpen || now >= untilMs) {
        up = false;
        shown = false;
        rasterStale = true;
        serial = rasterSerial;
        resend = true;
      } else if (!shown && rasterSerial != serial && rasterIsNoticeSized()) {
        shown = true;
      }
    }
    if (rasterStale && !up && rasterSerial != serial) rasterStale = false;
    return resend;
  }
  // The panel is on screen: the menu, once its own raster is back, or the notice, once its raster is.
  bool panelVisible(bool menuOpen) const { return (menuOpen && !rasterStale) || (up && shown); }
  // The compositor has something to draw this present.
  bool compositing(bool menuOpen) const { return menuOpen || shown; }
  // A profile switch: the outgoing profile's notice goes with it.
  void clear() {
    up = false;
    shown = false;
    untilMs = 0;
    rasterStale = false;
  }
};

// ---- The frame-rate readout on the flat screen ------------------------------------------------------------------------------------------------------
// One raster, one thing on screen: the menu, then a notice, then the readout (docs\design-f8-menu-unification-2026-11-15.md). The readout's raster is only
// composited once it is the live one -- never the menu's or a notice's left over in the corner. `wanted` is: the readout is on, the menu is closed and no notice is
// up. While it is not wanted the readout is suspended and its next raster is submitted from nothing.
struct ScreenReadoutState {
  bool submitted = false;  // a readout raster was handed to the panel since the last suspension
  bool shown = false;      // that raster (or a later one) is the live one
  uint32_t serial = 0;     // the panel's raster serial read before the first submit

  // Returns true when the caller must build and submit a raster now: the first one after a suspension, or `refreshDue`.
  bool tick(bool wanted, bool refreshDue, uint32_t rasterSerial) {
    if (!wanted) {
      submitted = false;
      shown = false;
      return false;
    }
    if (submitted && !shown && rasterSerial != serial) shown = true;
    return !submitted || refreshDue;
  }
  // The raster was submitted. The serial is that read before the submit (kept from the first one only: a refresh does not make the card disappear).
  void didSubmit(uint32_t serialBefore) {
    if (!submitted) {
      submitted = true;
      shown = false;
      serial = serialBefore;
    }
  }
  bool compositing() const { return shown; }
  void clear() {
    submitted = false;
    shown = false;
  }
};

// ---- The monitor panel on the flat screen -------------------------------------------------------------------------------------------------------------
// One pressed edge of hotkey.monitor_panel: the panel goes up or down, whatever the menu is doing (it takes no input, so it never waits on the menu). A hotkey
// row waiting for its input (captureActive) takes the edge as a candidate, not a command.
inline bool menuMonitorPanelToggle(bool pressed, bool panelUp, bool captureActive) {
  if (!pressed || captureActive) return panelUp;
  return !panelUp;
}

// The flat frame-rate readout is wanted: it is on, the menu is closed, no notice is up or about to start, and the monitor panel is down (the panel sits in the
// same corner and shows everything the readout does).
inline bool flatReadoutWanted(bool readoutOn, bool menuOpen, bool noticeUp, bool noticeDue, bool monitorPanelUp) {
  return readoutOn && !menuOpen && !noticeUp && !noticeDue && !monitorPanelUp;
}

}  // namespace edvr
