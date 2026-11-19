// menu_policy_test: pure regression rig for the F8 menu's profile policy and the decisions menuTick takes from it (src/d3d11/menu_policy.h).
// It pins what the flat and the VR profiles each do today, so the one menuTick that serves both cannot drift either: the policy's fields, the summon
// key's edge, Escape, the draw the gate follows, and the flat screen's notice machine with its queue (flat_notice_queue.h).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include "../../src/common/flat_profile_keys.h"
#include "../../src/d3d11/flat_notice_queue.h"
#include "../../src/d3d11/menu_flat_status.h"
#include "../../src/d3d11/menu_restart.h"
#include "../../src/d3d11/menu_pages.h"
#include "../../src/d3d11/menu_policy.h"
#include "../../src/d3d11/menu_flat_layout.h"
#include "../../src/d3d11/menu_panel.h"

using namespace edvr;

namespace {
unsigned checks = 0, failures = 0;
void check(bool ok, const char* name) {
    ++checks;
    if (!ok) { ++failures; std::printf("FAIL: %s\n", name); }
}

void policyCases() {
    const MenuProfilePolicy flat = menuPolicyFor(true);
    const MenuProfilePolicy vr = menuPolicyFor(false);
    check(flat.anchor == MenuAnchor::Screen && vr.anchor == MenuAnchor::World, "flat is anchored on the screen, VR in the world");
    check(flat.sizing == MenuSizing::OutputPx && vr.sizing == MenuSizing::HeadsetPpd, "flat is sized from the output's pixels, VR is sized from the headset's pixels per degree");
    check(flat.noticePlacement == MenuNoticePlacement::ScreenCorner && vr.noticePlacement == MenuNoticePlacement::WorldToast,
          "flat notices go to the screen corner, VR notices are toasts in the world");
    check(edvr::flatProfileReadsKey("menu.toasts"), "flat reads menu.toasts, so its notices follow it like VR's toasts");
    check(!edvr::flatProfileReadsKey("menu.toastz"), "the flat allow-list is exact: a misspelt toasts key is not read");
    check(flat.keyboardAlwaysPrivate && !vr.keyboardAlwaysPrivate, "flat always takes the keyboard; VR reads menu.keyboard");
    check(!flat.fade && vr.fade, "only the VR panel fades");
    check(!flat.headAim && vr.headAim, "only the VR panel aims with the head");
    check(!flat.adoptGameKeys && vr.adoptGameKeys, "only VR adopts Elite's panel keys");
    check(!flat.readsMenuKeys && vr.readsMenuKeys, "flat reads no [menu] key, VR reads them");
    check(flat.padNavigation && !vr.padNavigation, "only the flat panel is navigated by the pad");
    check(flat.closesOnFocusLoss && !vr.closesOnFocusLoss, "only the flat panel closes when Elite loses the focus");
    check(flat.primesHotkeysOnClose && !vr.primesHotkeysOnClose, "only flat primes the hotkeys on close");
    check(flat.monitorPanel && !vr.monitorPanel, "only the flat profile has the monitor panel");
    check(flatProfileReadsKey("hotkey.monitor_panel") && flatProfileOnlyKey("hotkey.monitor_panel") && !flatProfileOnlyKey("hotkey.menu") &&
          !flatProfileOnlyKey("hotkey.compare_aa") && !flatProfileOnlyKey(nullptr),
          "the flat gate reads the monitor panel's key, and it is the one key only the flat profile reads");
    check(menuPolicyFor(true).anchor == flat.anchor && menuPolicyFor(false).anchor == vr.anchor, "the policy is a pure function of the profile");
}

void summonCases() {
    using A = MenuSummonAction;
    //                         pressed open padNav padReady capture shift recentre
    check(menuSummonAction(false, false, false, false, false, false, false) == A::None, "no edge, no action");
    check(menuSummonAction(true, false, false, false, false, false, true) == A::Open, "the edge opens a closed menu");
    check(menuSummonAction(true, true, false, false, false, false, true) == A::Close, "the edge closes an open menu");
    check(menuSummonAction(true, true, false, false, false, true, true) == A::Recentre, "Shift with the menu open recentres where the world anchors the panel");
    check(menuSummonAction(true, true, false, false, false, true, false) == A::Close, "Shift does nothing special on the screen: the edge closes");
    check(menuSummonAction(true, false, false, false, false, true, true) == A::Open, "Shift with the menu closed opens");
    check(menuSummonAction(true, false, false, false, true, false, true) == A::None, "a hotkey row waiting for its input takes the edge: no open");
    check(menuSummonAction(true, true, false, false, true, false, true) == A::None, "a hotkey row waiting for its input takes the edge: no close");
    check(menuSummonAction(true, false, true, true, false, false, false) == A::Open, "a pad summon that overlaps navigation still opens");
    check(menuSummonAction(true, true, true, true, false, false, false) == A::None, "once the panel owns the pad, the overlapping summon button is navigation");
    check(menuSummonAction(true, true, true, false, false, false, false) == A::Close, "a pad summon closes while the pad is not yet the panel's");
    check(menuSummonAction(true, true, false, true, false, false, false) == A::Close, "a summon that does not overlap navigation closes even with the pad ready");
}

void escapeCases() {
    using E = MenuEscapeAction;
    bool down = false;
    check(menuEscapeAction(false, down, false, false) == E::None && !down, "Escape up: nothing");
    check(menuEscapeAction(true, down, false, false) == E::Close && down, "the Escape edge closes the menu");
    check(menuEscapeAction(true, down, false, false) == E::None && down, "a held Escape acts once");
    check(menuEscapeAction(false, down, false, false) == E::None && !down, "release re-arms the edge");
    check(menuEscapeAction(true, down, false, true) == E::CancelEdit, "Escape ends a value being typed before it closes the menu");
    menuEscapeAction(false, down, false, false);
    check(menuEscapeAction(true, down, true, false) == E::None && down, "a hotkey row waiting for its input owns Escape");
    check(menuEscapeAction(true, down, false, false) == E::None, "...and the Escape that cancelled the capture cannot close the menu a tick later");
    down = true;
    check(menuEscapeAction(true, down, false, false) == E::None, "an Escape already down when the menu opened does not close it (the flat open seeds the tracker)");
}

void drawnCases() {
    check(!menuDrawnFresh(false, 1000, 900, 250), "a closed menu is never drawn fresh");
    check(!menuDrawnFresh(true, 1000, 0, 250), "before the first draw there is no fresh draw");
    check(menuDrawnFresh(true, 1000, 900, 250), "a draw within the window is fresh");
    check(menuDrawnFresh(true, 1150, 900, 250), "a draw exactly at the window's edge is fresh");
    check(!menuDrawnFresh(true, 1151, 900, 250), "a draw past the window is stale");
    check(!menuNotDrawn(false, 9000, 100, 100, 1500), "a closed menu is never reported not drawn");
    check(!menuNotDrawn(true, 1500, 0, 0, 1500), "an open menu is not closed before the limit");
    check(menuNotDrawn(true, 1501, 0, 0, 1500), "an open menu never drawn is closed past the limit");
    check(!menuNotDrawn(true, 3000, 0, 2000, 1500), "a menu that was drawn recently stays");
    check(!menuNotDrawn(true, 1600, 1000, 0, 1500), "a menu that opened recently stays whatever the draw clock says");
}

// The flat monitor panel (menu_policy.h): its key toggles it whatever the menu does, a waiting capture takes the edge, and the readout gives way to it.
void monitorPanelCases() {
    check(!menuMonitorPanelToggle(false, false, false) && menuMonitorPanelToggle(false, true, false), "no edge: the panel stays as it is");
    check(menuMonitorPanelToggle(true, false, false), "the edge raises the panel");
    check(!menuMonitorPanelToggle(true, true, false), "the edge lowers the panel");
    check(!menuMonitorPanelToggle(true, false, true) && menuMonitorPanelToggle(true, true, true), "a hotkey row waiting for its input takes the edge: no change");
    check(menuHeaderFpsLine(true, true), "menu.fps_overlay_lock: the open menu's card carries the frame-rate line");
    check(!menuHeaderFpsLine(false, true), "the header line needs the menu open");
    check(!menuHeaderFpsLine(true, false), "the header line is off when its switch is off");
    check(menuStatusPageReleasesKeyboard(true, false) && !menuStatusPageReleasesKeyboard(false, false), "VR: a status page releases the keyboard to the game, other pages do not");
    check(!menuStatusPageReleasesKeyboard(true, true) && !menuStatusPageReleasesKeyboard(false, true), "flat (keyboard always private): the Status page keeps the keyboard private too");
    check(!menuStatusPageReleasesKeyboard(true, menuPolicyFor(true).keyboardAlwaysPrivate) && menuStatusPageReleasesKeyboard(true, menuPolicyFor(false).keyboardAlwaysPrivate), "the policy's own flag drives it: flat private on a status page, VR released");
}

// The flat notice, driven the way menuTick drives it: take from the queue while the menu is closed, start, then tick after the content went out.
struct Rig {
    FlatNoticeQueue queue;
    ScreenNoticeState notice;
    uint32_t serial = 10;     // the panel's raster serial
    bool noticeSized = false; // the live raster is as small as a notice
    bool contentResent = false;
    int starts = 0;
    void tick(bool open, uint64_t now) {
        if (const std::string* n = queue.take(open)) {
            (void)n;
            ++starts;
            notice.start(now, 2500, serial);
        }
        if (notice.tick(open, now, serial, [&] { return noticeSized; })) contentResent = true;
    }
};

void noticeCases() {
    {   // A notice with the menu closed: shown once its own raster is the live one, then ended after 2.5 s, and the stale menu raster never flashes.
        Rig r;
        r.queue.show("AA off");
        r.tick(false, 1000);
        check(r.starts == 1 && r.notice.up && !r.notice.shown, "a notice with the menu closed starts at once, not yet shown");
        check(!r.notice.panelVisible(false) && !r.notice.compositing(false), "...and nothing is drawn before its raster is live");
        r.tick(false, 1016);
        check(!r.notice.shown, "...the raster serial has not moved: still not shown");
        r.serial = 11;
        r.noticeSized = false;
        r.tick(false, 1032);
        check(!r.notice.shown, "a new raster that is menu-sized is not the notice");
        r.noticeSized = true;
        r.tick(false, 1048);
        check(r.notice.shown && r.notice.panelVisible(false) && r.notice.compositing(false), "the notice's raster is live: shown, visible and composited");
        r.tick(false, 3499);
        check(r.notice.up && r.notice.shown, "it stays until its 2.5 s are over");
        r.tick(false, 3500);
        check(!r.notice.up && !r.notice.shown && r.notice.rasterStale && r.contentResent, "it ends at 2.5 s, the menu's content is re-sent, the panel is held back as stale");
        check(!r.notice.panelVisible(false) && !r.notice.compositing(false), "a stale raster is not drawn");
        r.tick(false, 3516);
        check(r.notice.rasterStale, "the panel stays held back while the raster serial has not moved");
        r.serial = 12;
        r.tick(false, 3532);
        check(!r.notice.rasterStale, "the menu's own raster arrived: no longer stale");
    }
    {   // Raised with the menu open: queued, started when it closes.
        Rig r;
        r.queue.show("Windows: mode 2");
        r.tick(true, 100);
        r.tick(true, 116);
        check(r.starts == 0 && r.queue.due() && !r.notice.up, "a notice raised with the menu open stays queued");
        check(r.notice.panelVisible(true) && r.notice.compositing(true), "the open menu is drawn");
        r.tick(false, 132);
        check(r.starts == 1 && r.notice.up, "it starts when the menu closes");
    }
    {   // Opening the menu over a notice ends it.
        Rig r;
        r.queue.show("AA on");
        r.tick(false, 1000);
        r.serial = 11;
        r.noticeSized = true;
        r.tick(false, 1016);
        check(r.notice.shown, "the notice is up");
        r.contentResent = false;
        r.tick(true, 1032);
        check(!r.notice.up && !r.notice.shown && r.notice.rasterStale && r.contentResent, "the menu opening ends the notice and re-sends its content");
        check(!r.notice.panelVisible(true), "the open menu is held back until its own raster is live");
        r.serial = 12;
        r.tick(true, 1048);
        check(r.notice.panelVisible(true), "...then it is drawn");
    }
    {   // The raster-size question is asked only while a notice waits to be shown.
        Rig r;
        int asked = 0;
        ScreenNoticeState n;
        n.start(0, 2500, 5);
        n.tick(false, 10, 5, [&] { ++asked; return true; });
        check(asked == 0, "the size is not asked for while the serial has not moved");
        n.tick(false, 20, 6, [&] { ++asked; return true; });
        check(asked == 1 && n.shown, "the size is asked once the serial moved");
        n.tick(false, 30, 7, [&] { ++asked; return true; });
        check(asked == 1, "...and not again once shown");
    }
    {   // A newer notice replaces an older one still waiting; a profile switch clears the machine and the queue.
        Rig r;
        r.queue.show("one");
        r.queue.show("two");
        const std::string* n = r.queue.take(false);
        check(n && *n == "two", "a newer notice replaces an older one still waiting");
        r.notice.start(0, 2500, 1);
        r.notice.rasterStale = true;
        r.notice.clear();
        r.queue.show("three");
        r.queue.clear();
        check(!r.notice.up && !r.notice.shown && !r.notice.rasterStale && r.notice.untilMs == 0 && !r.queue.due(), "a profile switch clears the notice, queued or shown");
    }
}
// The page table (menu_pages.h): the pages each profile shows, in order. A reorder or a lost page fails here.
std::string pageList(bool flat, bool developer) {
    std::string out;
    for (size_t i = 0; i < kMenuPageCount; ++i) {
        if (!menuPageShown(kMenuPages[i], flat, developer)) continue;
        if (!out.empty()) out += ",";
        out += kMenuPages[i].name;
    }
    return out;
}

void pageCases() {
    const std::string vr = "Performance,Fixes,Visual,Mirror window,Native on foot,Hotkeys,Plugins,Monitor,Status";
    check(pageList(true, false) == "Performance,Visual,Hotkeys,Status,Debug", "the flat profile's pages are Performance, Visual, Hotkeys, Status, Debug (Debug last): the Monitor is a panel of its own there");
    check(!menuProfileHasPage(MenuPageSource::Monitor, true) && menuProfileHasPage(MenuPageSource::Monitor, false) &&
              menuPolicyFor(true).monitorPanel && !menuPolicyFor(false).monitorPanel,
          "the Monitor is a tab where it is not a panel: VR's tab, flat's panel");
    check(menuProfileHasPage(MenuPageSource::Hotkeys, true) && menuProfileHasPage(MenuPageSource::Hotkeys, false), "both profiles have a Hotkeys page: the capture, note and clash work in menuTick follows the page table, not a policy field");
    check(!menuProfileHasPage(MenuPageSource::Plugins, true) && menuProfileHasPage(MenuPageSource::Plugins, false), "only VR has a Plugins page");
    check(!menuProfileHasPage(MenuPageSource::Actions, false), "a developer-only page (Instruments) is not a profile's own");
    check(pageList(true, true) == "Performance,Visual,Hotkeys,Status,Debug", "developer mode adds no flat page");
    check(pageList(false, false) == vr, "the VR profile's pages without developer mode");
    check(pageList(false, true) == vr + ",Advanced,Experimental,Instruments", "developer mode adds Advanced, Experimental and Instruments to VR, after Status");
    check(kMenuPages[6].post == MenuPagePost::AimChoiceAfterLookToAim && std::strcmp(kMenuPages[6].name, "Native on foot") == 0, "the Native on foot page moves the aiming row under Look to aim");
    size_t posts = 0;
    for (size_t i = 0; i < kMenuPageCount; ++i) posts += kMenuPages[i].post != MenuPagePost::None;
    check(posts == 1, "only Native on foot has a post step");
}

// The flat Status page's lines (menu_flat_status.h): which are there for each situation, and what they say.
std::string statusCaptions(const FlatStatusInput& in) {
    FlatStatusLine lines[kFlatStatusMaxLines + 4];
    const int n = flatStatusLines(in, lines, kFlatStatusMaxLines + 4);
    std::string out;
    for (int i = 0; i < n; ++i) {
        if (i) out += ",";
        out += lines[i].caption;
    }
    return out;
}
std::string statusValue(const FlatStatusInput& in, const char* caption) {
    FlatStatusLine lines[kFlatStatusMaxLines + 4];
    const int n = flatStatusLines(in, lines, kFlatStatusMaxLines + 4);
    for (int i = 0; i < n; ++i)
        if (std::strcmp(lines[i].caption, caption) == 0) return lines[i].value;
    return "<absent>";
}
FlatStatusInput statusBase() {
    FlatStatusInput in;
    in.version = "v1.2.3";
    in.gameBuild = "334925";
    in.outW = 2560;
    in.outH = 1440;
    in.refreshHz = 144.0f;
    in.displayMode = FlatDisplayMode::Windowed;
    in.aaOn = true;
    in.aaLabel = "DLSS";
    in.aaModel = "K";
    in.aaRun = FlatAaRun::Running;
    in.sceneKnown = true;
    in.renderW = 1706;
    in.renderH = 960;
    in.sceneOutW = 2560;
    in.sceneOutH = 1440;
    in.sharpening = "off";
    in.hudQuality = "High";
    in.hudAntiAliasing = "on";
    in.keyboardReady = true;
    in.lastWrite = "";
    return in;
}

void statusCases() {
    const char* all = "ED-UP,Profile,Display,Anti-aliasing,Upscaling,Frame generation,Sharpening,HUD quality,Keyboard,Restart pending,Last write";
    FlatStatusInput in = statusBase();
    check(statusCaptions(in) == all, "flat Status, no bridge and nothing waiting: version, profile, display, AA, upscaling, frame generation, sharpening, HUD, keyboard, waiting, last write");
    check(statusValue(in, "ED-UP") == "v1.2.3 / game build 334925" && statusValue(in, "Profile") == "flat", "the version carries the game build, and the profile is flat");
    check(statusValue(in, "Display") == "2560x1440, 144 Hz, windowed/borderless", "the display says size, refresh rate and mode");
    check(statusValue(in, "Anti-aliasing") == "DLSS K, running" && statusValue(in, "Upscaling") == "1706x960 to 2560x1440", "AA names the mode, its model and that it runs; upscaling the sizes");
    check(statusValue(in, "Frame generation") == "off" && statusValue(in, "Restart pending") == "none" && statusValue(in, "Last write") == "none this session",
          "frame generation off, nothing waiting, no write yet");
    check(statusValue(in, "Keyboard") == "ready" && statusValue(in, "HUD quality") == "High, edges on", "the keyboard gate and the HUD");

    // Sources that do not exist are lines that are not there.
    in = statusBase();
    in.outW = in.outH = 0;
    in.refreshHz = 0.0f;
    in.displayMode = FlatDisplayMode::Unknown;
    check(statusValue(in, "Display") == "<absent>", "no output size, refresh rate or mode yet: no Display line");
    in.refreshHz = 60.0f;
    check(statusValue(in, "Display") == "60 Hz", "a refresh rate alone is shown alone");
    in = statusBase();
    in.sceneKnown = false;
    check(statusValue(in, "Upscaling") == "<absent>", "no measured scene: no Upscaling line");
    in = statusBase();
    in.renderW = in.sceneOutW;
    in.renderH = in.sceneOutH;
    check(statusValue(in, "Upscaling") == "none, 2560x1440", "a scene at the output's size is not upscaled");
    in = statusBase();
    in.aaOn = false;
    check(statusValue(in, "Anti-aliasing") == "off" && statusValue(in, "Upscaling") == "<absent>", "anti-aliasing off: no upscaling line");
    in = statusBase();
    in.aaRun = FlatAaRun::Paused;
    check(statusValue(in, "Anti-aliasing") == "DLSS K, paused: Elite's settings", "a stood-down mode says it is paused");
    in.aaRun = FlatAaRun::NotRunning;
    in.aaModel = "";
    in.aaLabel = "XeSS-SR";
    check(statusValue(in, "Anti-aliasing") == "XeSS-SR, not running", "a mode with no model and not running yet");
    in = statusBase();
    in.hudAntiAliasing = "";
    check(statusValue(in, "HUD quality") == "High", "no HUD edge smoothing while the HUD quality is off");
    in = statusBase();
    in.sharpening = "";
    in.hudQuality = "";
    check(statusCaptions(in).find("Sharpening") == std::string::npos && statusCaptions(in).find("HUD quality") == std::string::npos, "a row that is not there leaves no line");

    // Frame generation: the bridge cases.
    in = statusBase();
    in.fg = FlatFgState::On;
    in.fgFactor = 2;
    check(statusValue(in, "Frame generation") == "on, 2x", "a bridge making one generated frame per rendered frame: on, 2x");
    in.fgFactor = 3;
    check(statusValue(in, "Frame generation") == "on, 3x", "...and two: on, 3x");
    in.fg = FlatFgState::OnIdle;
    check(statusValue(in, "Frame generation") == "on, no frames made now", "a bridge that makes none now");
    in.fgExclusiveFullscreen = true;
    check(statusValue(in, "Frame generation") == "on, none in exclusive fullscreen", "...in exclusive fullscreen, and says why");
    in.fg = FlatFgState::Stopped;
    in.fgProblem = "a wait on the queue timed out";
    check(statusValue(in, "Frame generation") == "stopped: a wait on the queue timed out", "a bridge that stopped says why");
    in.fg = FlatFgState::NextLaunch;
    check(statusValue(in, "Frame generation") == "at next launch", "frame generation on without a bridge, with XeSS-SR chosen: at next launch");
    in.fg = FlatFgState::NeedsXess;
    check(statusValue(in, "Frame generation") == "on, needs XeSS-SR", "frame generation on without a bridge, without XeSS-SR: says what it needs");

    // Restart pending, wrapper, keyboard, warning.
    in = statusBase();
    in.waitingCount = 2;
    in.waitingLabels = "XeSS Frame Generation, Anti-aliasing";
    check(statusValue(in, "Restart pending") == "2: XeSS Frame Generation, Anti-aliasing", "changes waiting for a restart are counted and named");
    in.lastWrite = "fix.temporal_aa = xess";
    check(statusValue(in, "Last write") == "fix.temporal_aa = xess", "the last write is its own line");
    in = statusBase();
    in.wrapper = "ReShade64.dll";
    check(statusCaptions(in) == "ED-UP,Profile,Display,Anti-aliasing,Upscaling,Frame generation,Sharpening,HUD quality,Graphics wrapper,Keyboard,Restart pending,Last write" &&
          statusValue(in, "Graphics wrapper") == "ReShade64.dll", "a graphics wrapper that was found is a line, ahead of the keyboard");
    in = statusBase();
    in.keyboardReady = false;
    check(statusValue(in, "Keyboard") == "gate unavailable", "a keyboard that has not reached the gate says so");
    in = statusBase();
    in.eliteWarningActive = true;
    check(statusCaptions(in).find("Keyboard,Elite settings,Restart pending") != std::string::npos, "the settings warning has its line, after the keyboard, ahead of the restarts");

    // Every line at once is the most the page has, and every caption and value fits its buffer.
    in = statusBase();
    in.wrapper = "ReShade64.dll";
    in.eliteWarningActive = true;
    in.fg = FlatFgState::Stopped;
    in.fgProblem = "a wait on the queue timed out";
    in.waitingCount = 3;
    in.waitingLabels = "XeSS Frame Generation, Anti-ali...";
    in.lastWrite = "fix.xess_generated_frames = 3";
    FlatStatusLine lines[kFlatStatusMaxLines + 4];
    const int n = flatStatusLines(in, lines, kFlatStatusMaxLines + 4);
    check(n == kFlatStatusMaxLines, "with every source present the page has kFlatStatusMaxLines lines");
    bool fits = true;
    for (int i = 0; i < n; ++i) fits &= std::strlen(lines[i].caption) < sizeof(lines[i].caption) && std::strlen(lines[i].value) < sizeof(lines[i].value) - 1;
    check(fits, "no caption or value was cut at its buffer");
    FlatStatusLine few[3];
    check(flatStatusLines(in, few, 3) == 3, "a short output holds only what it can");
}

// The conditional-restart table (menu_restart.h): a truth table over the inputs, row by row, with the outcome named for each.
RestartInput base() {
    RestartInput in;
    in.flatProfile = true;
    in.aaIsXess = true;
    in.fgOn = true;
    in.bridged = false;
    in.problemPresent = false;
    in.aaChangedSinceLaunch = false;
    return in;
}
const char* const kToggle = "xess_frame_generation";
const char* const kFrames = "xess_generated_frames";
const char* const kAa = "temporal_aa";

void restartCases() {
    using S = RestartState;
    auto st = [](const RestartInput& in, const char* key) { return conditionalRestartState(in, "fix", key); };
    check(sizeof(kConditionalRestarts) / sizeof(kConditionalRestarts[0]) == 3, "the table names three rows: the FG toggle, the FG frames, Anti-aliasing");
    check(conditionalRestartFor("fix", kToggle) && conditionalRestartFor("fix", kFrames) && conditionalRestartFor("fix", kAa), "all three rows are in the table");
    check(!conditionalRestartFor("fix", "ui_quality") && !conditionalRestartFor(nullptr, kAa) && !conditionalRestartFor("fix", nullptr), "any other row, and a null name, is not in it");

    // FG on, XeSS-SR now, no bridge, no problem: the bridge can only be made at the next game start.
    RestartInput in = base();
    check(st(in, kToggle) == S::PendingRestart, "FG on, XeSS-SR, no bridge: the toggle waits for a restart");
    check(st(in, kFrames) == S::PendingRestart, "FG on, XeSS-SR, no bridge: the frames row waits for a restart");
    check(st(in, kAa) == S::NotPending, "...and Anti-aliasing does not, while its value is the launch value");
    in.aaChangedSinceLaunch = true;
    check(st(in, kAa) == S::PendingRestart, "...but does once the choice differs from the launch value: it is the change that needs the bridge");
    check(st(in, kToggle) == S::PendingRestart && st(in, kFrames) == S::PendingRestart, "an AA change leaves the FG rows waiting");
    // Bridged: live.
    in = base(); in.bridged = true; in.aaChangedSinceLaunch = true;
    check(st(in, kToggle) == S::NotPending && st(in, kFrames) == S::NotPending && st(in, kAa) == S::NotPending, "bridged: nothing waits, however AA changed");
    // FG off: nothing needs the bridge.
    in = base(); in.fgOn = false; in.aaChangedSinceLaunch = true;
    check(st(in, kToggle) == S::NotPending && st(in, kFrames) == S::NotPending && st(in, kAa) == S::NotPending, "FG off: nothing waits");
    // AA other than XeSS: the bridge would not be made even after a restart.
    in = base(); in.aaIsXess = false; in.aaChangedSinceLaunch = true;
    check(st(in, kToggle) == S::NotPending && st(in, kFrames) == S::NotPending && st(in, kAa) == S::NotPending, "AA is not XeSS-SR: nothing waits (the FG rows are dimmed instead)");
    // A problem: a restart alone does not fix it.
    in = base(); in.problemPresent = true;
    check(st(in, kToggle) == S::Stopped && st(in, kFrames) == S::Stopped, "FG on and failed: the FG rows are Stopped, not waiting");
    check(st(in, kAa) == S::NotPending, "...and Anti-aliasing is neither");
    in.aaChangedSinceLaunch = true; in.bridged = true;
    check(st(in, kToggle) == S::Stopped && st(in, kAa) == S::NotPending, "a problem outranks the bridge and an AA change");
    in = base(); in.problemPresent = true; in.fgOn = false;
    check(st(in, kToggle) == S::NotPending && st(in, kFrames) == S::NotPending, "a problem with FG off says nothing");
    // VR: no XeSS Frame Generation.
    in = base(); in.flatProfile = false; in.aaChangedSinceLaunch = true;
    check(st(in, kToggle) == S::NotPending && st(in, kFrames) == S::NotPending && st(in, kAa) == S::NotPending, "VR: the rules never apply");
    in.problemPresent = true;
    check(st(in, kToggle) == S::NotPending, "VR: not even Stopped");
    // A row outside the table.
    in = base(); in.aaChangedSinceLaunch = true;
    check(conditionalRestartState(in, "fix", "ui_quality") == S::NotPending, "a row outside the table is never judged here");

    // The words.
    char b[96];
    menuRestartCountText(0, false, b, sizeof(b));
    check(b[0] == 0, "no count, no text");
    menuRestartCountText(1, false, b, sizeof(b));
    check(std::strcmp(b, "1 change waits for a game restart") == 0, "one change: \"1 change waits for a game restart\"");
    menuRestartCountText(2, false, b, sizeof(b));
    check(std::strcmp(b, "2 changes wait for a game restart") == 0, "two changes: \"2 changes wait for a game restart\"");
    menuRestartCountText(2, true, b, sizeof(b));
    check(std::strcmp(b, "2 changes at next launch") == 0, "the short form is the VR footer's wording");
    menuRestartCloseNotice(0, b, sizeof(b));
    check(b[0] == 0, "no change waiting, no close notice");
    menuRestartCloseNotice(1, b, sizeof(b));
    check(std::strcmp(b, "1 change applies at next game start") == 0, "close notice for one");
    menuRestartCloseNotice(3, b, sizeof(b));
    check(std::strcmp(b, "3 changes apply at next game start") == 0, "close notice for three");

    char s[160];
    using W = FooterWrite;
    menuFlatFooterStatus(W::Saved, true, 0, false, s, sizeof(s));
    check(std::strcmp(s, " | saved") == 0, "flat footer, nothing waiting: \" | saved\" as before");
    menuFlatFooterStatus(W::Saved, true, 1, false, s, sizeof(s));
    check(std::strcmp(s, " | 1 change waits for a game restart | saved") == 0, "flat footer, one waiting: the count comes ahead of saved");
    menuFlatFooterStatus(W::Saved, true, 2, false, s, sizeof(s));
    check(std::strcmp(s, " | 2 changes wait for a game restart | saved") == 0, "flat footer, two waiting");
    menuFlatFooterStatus(W::None, true, 1, false, s, sizeof(s));
    check(std::strcmp(s, " | 1 change waits for a game restart") == 0, "flat footer, nothing written yet but one waiting: the count alone");
    menuFlatFooterStatus(W::None, true, 0, false, s, sizeof(s));
    check(s[0] == 0, "flat footer, nothing at all: empty");
    menuFlatFooterStatus(W::Failed, true, 2, false, s, sizeof(s));
    check(std::strcmp(s, " | write failed") == 0, "a failed write keeps priority over the count");
    menuFlatFooterStatus(W::Saving, true, 2, false, s, sizeof(s));
    check(std::strcmp(s, " | saving") == 0, "saving keeps priority over the count");
    menuFlatFooterStatus(W::Saved, false, 1, false, s, sizeof(s));
    check(std::strcmp(s, " | 1 change waits for a game restart | keyboard gate unavailable") == 0, "the gate note follows the count");
    menuFlatFooterStatus(W::Saved, true, 1, true, s, sizeof(s));
    check(std::strcmp(s, " | 1 change at next launch | saved") == 0, "the short form in the footer");
}

// The flat card sized from the output's pixels and laid 1:1 (menu_flat_layout.h). Pure functions of the output size.
void flatLayoutCases() {
    struct Res { int w, h, card, cap; const char* name; };
    const Res res[] = {
        {1920, 1080, 357, 14, "1920x1080"},
        {2560, 1440, 476, 19, "2560x1440"},
        {3840, 2160, 714, 28, "3840x2160"},
        {3440, 1440, 476, 19, "3440x1440"},
    };
    // The Performance tab's bitmap is 1106 high at the 860 raster (native_menu_test --layout measures it): its height over its width is the aspect.
    const float aspect = 1106.0f / 860.0f;
    for (const Res& r : res) {
        const FlatCardSize fs = flatCardSize(r.w, r.h, kFlatMenuCardHeightFrac);
        char name[160];
        std::snprintf(name, sizeof(name), "the flat card at %s is %d px wide with a %d px cap", r.name, r.card, r.cap);
        check(fs.cardPx == r.card && fs.capPx == r.cap, name);
        // The card and the strip: the same card, the strip extends to the right.
        const int tipW = flatBitmapWidth(fs.cardPx, true, kTipRatio);
        const int plainW = flatBitmapWidth(fs.cardPx, false, kTipRatio);
        std::snprintf(name, sizeof(name), "%s: without the tooltip the bitmap is the card; with it, the card's width times the strip ratio", r.name);
        check(plainW == fs.cardPx && tipW == static_cast<int>(fs.cardPx * kTipRatio + 0.5f) && tipW > plainW, name);
        // The composite: the card's rect is identical with and without the strip, and every bitmap maps 1:1.
        const int bh = static_cast<int>(fs.cardPx * aspect + 0.5f);
        const FlatPlacement plain = flatPlace(r.w, r.h, plainW, bh);
        const FlatPlacement tip = flatPlace(r.w, r.h, tipW, bh);
        std::snprintf(name, sizeof(name), "%s: the bitmap is placed at exactly its own pixel size, 1:1", r.name);
        check(plain.oneToOne && plain.w == plainW && plain.h == bh && tip.oneToOne && tip.w == tipW && tip.h == bh, name);
        std::snprintf(name, sizeof(name), "%s: the card's rect (x, y, width, height) is the same with and without the strip", r.name);
        check(plain.x == tip.x && plain.y == tip.y && plain.w == fs.cardPx && std::min(plain.w, tip.w) == fs.cardPx && plain.h == tip.h, name);
        // The flat monitor panel (menu_panel.h, the monitor slot): a card of the menu's width, 1:1 in the upper right corner, level with the menu card and
        // clear of it and its tooltip strip.
        const FlatPlacement monitor = flatPlace(r.w, r.h, fs.cardPx, bh, FlatCorner::UpperRight);
        std::snprintf(name, sizeof(name), "%s: the monitor panel is 1:1 in the upper right corner, level with the menu card, clear of it and its strip", r.name);
        check(monitor.oneToOne && monitor.w == fs.cardPx && monitor.y == tip.y &&
                  monitor.x + monitor.w == r.w - static_cast<int>(std::lround(0.02 * r.w)) && tip.x + tip.w < monitor.x,
              name);
        std::snprintf(name, sizeof(name), "%s: the corner is two percent in, in whole pixels", r.name);
        check(plain.x == static_cast<int>(std::lround(0.02 * r.w)) && plain.y == static_cast<int>(std::lround(0.02 * r.h)), name);
        // The geometry the shader takes, read back to output pixels: the left edge, the top edge and the width.
        for (const FlatPlacement* pl : {&plain, &tip}) {
            const float tanY = static_cast<float>(r.h) / static_cast<float>(r.w);
            const float halfH = pl->halfW * static_cast<float>(bh) / static_cast<float>(pl->w);
            const float leftPx = (pl->shift - pl->halfW + 1.0f) * 0.5f * static_cast<float>(r.w);
            const float topPx = (1.0f - (halfH - pl->originY) / tanY) * 0.5f * static_cast<float>(r.h);
            const float widthPx = pl->halfW * static_cast<float>(r.w);
            std::snprintf(name, sizeof(name), "%s: the shader geometry puts the bitmap at its pixel rect", r.name);
            check(std::fabs(leftPx - static_cast<float>(pl->x)) < 0.01f && std::fabs(topPx - static_cast<float>(pl->y)) < 0.01f &&
                  std::fabs(widthPx - static_cast<float>(pl->w)) < 0.01f, name);
        }
    }
    // The reference: at 1080p the card is the size the old composite drew it (the old formula, with the measured aspect).
    check(std::fabs(flatLegacyCardPx(1920, 1080, aspect) - static_cast<float>(flatCardSize(1920, 1080, kFlatMenuCardHeightFrac).cardPx)) < 2.0f,
          "the flat card at 1080p is the width the old composite gave it");
    check(flatCardSize(3440, 1440, kFlatMenuCardHeightFrac).cardPx == flatCardSize(2560, 1440, kFlatMenuCardHeightFrac).cardPx,
          "an ultrawide output does not grow the card: it follows the output's height");
    check(flatCardSize(0, 0, kFlatMenuCardHeightFrac).cardPx == 357, "an output not known yet is sized as 1920x1080");
    // Bounds: text never below the minimum cap, the card never past 45 % of the output's width.
    check(flatCardSize(1280, 720, kFlatMenuCardHeightFrac).capPx == kFlatMinCapPx, "a small output keeps the minimum text height");
    check(flatCardSize(1000, 2000, kFlatMenuCardHeightFrac).cardPx <= 450, "a narrow output keeps the card under 45 percent of its width");
    // The notice's card: the old toast's 0.31 of the width at 1080p, from the height.
    check(flatCardSize(1920, 1080, kFlatNoticeHeightFrac).cardPx == 595, "the flat notice at 1080p is 595 px wide, as before");
    check(flatCardSize(2560, 1440, kFlatNoticeHeightFrac).cardPx == 793 && flatCardSize(3440, 1440, kFlatNoticeHeightFrac).cardPx == 793,
          "the flat notice follows the output's height too");
    check(flatNoticeMaxRasterPx(1080) == 216 && flatNoticeMaxRasterPx(2160) == 432, "the notice's raster bound follows the output's height");
    // A page taller than the output allows is the one case scaled down, and still in the corner.
    const FlatPlacement tall = flatPlace(1920, 1080, 700, 1400);
    check(!tall.oneToOne && tall.h <= static_cast<int>(0.85 * 1080 + 1) && tall.w < 700 && tall.x == 38 && tall.y == 22,
          "a bitmap taller than the screen allows is scaled to fit, upper-left kept");
    check(flatPlace(0, 1080, 100, 100).w == 0 && flatPlace(1920, 1080, 0, 100).w == 0, "an unknown output or an empty bitmap places nothing");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--dry-run") == 0) {
        std::puts("menu_policy_test: dry-run (no checks or files)");
        return 0;
    }
    if (argc > 2 || (argc == 2 && std::strcmp(argv[1], "--self-test") != 0)) {
        std::fputs("usage: menu_policy_test [--self-test | --dry-run]\n", stderr);
        return 2;
    }
    policyCases();
    summonCases();
    escapeCases();
    drawnCases();
    monitorPanelCases();
    noticeCases();
    pageCases();
    restartCases();
    statusCases();
    flatLayoutCases();
    std::printf("menu_policy_test: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
