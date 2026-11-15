// menu_policy_test: pure regression rig for the F8 menu's profile policy and the decisions menuTick takes from it (src/d3d11/menu_policy.h).
// It pins what the flat and the VR profiles each do today, so the one menuTick that serves both cannot drift either: the policy's fields, the summon
// key's edge, Escape, the draw the gate follows, and the flat screen's notice machine with its queue (flat_notice_queue.h).
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include "../../src/d3d11/flat_notice_queue.h"
#include "../../src/d3d11/menu_policy.h"

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
    check(flat.sizing == MenuSizing::FixedPx && vr.sizing == MenuSizing::HeadsetPpd, "flat is a fixed pixel card, VR is sized from the headset's pixels per degree");
    check(flat.noticePlacement == MenuNoticePlacement::ScreenCorner && vr.noticePlacement == MenuNoticePlacement::WorldToast,
          "flat notices go to the screen corner, VR notices are toasts in the world");
    check(flat.noticesAlwaysOn, "the flat policy keeps notices on: menu.toasts is not readable in flat, and a refused key reads as off");
    check(!vr.noticesAlwaysOn, "VR notices obey menu.toasts");
    check(flat.keyboardAlwaysPrivate && !vr.keyboardAlwaysPrivate, "flat always takes the keyboard; VR reads menu.keyboard");
    check(!flat.fade && vr.fade, "only the VR panel fades");
    check(!flat.headAim && vr.headAim, "only the VR panel aims with the head");
    check(!flat.adoptGameKeys && vr.adoptGameKeys, "only VR adopts Elite's panel keys");
    check(!flat.readsMenuKeys && vr.readsMenuKeys, "flat reads no [menu] key, VR reads them");
    check(!flat.hotkeysPage && vr.hotkeysPage, "only VR has the Hotkeys page today");
    check(flat.padNavigation && !vr.padNavigation, "only the flat panel is navigated by the pad");
    check(flat.closesOnFocusLoss && !vr.closesOnFocusLoss, "only the flat panel closes when Elite loses the focus");
    check(flat.primesHotkeysOnClose && !vr.primesHotkeysOnClose, "only flat primes the hotkeys on close");
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
    noticeCases();
    std::printf("menu_policy_test: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
