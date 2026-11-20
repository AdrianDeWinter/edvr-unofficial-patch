// F209 checks (native on-foot VR): the Pioneer Supplies buy screens found by the game's own open flag, and F107's rule that a buy screen open holds the camera away at any distance.
// Header only, run by native_frame_test (run209). The pure half only: the constants, the once-a-frame re-check's verdicts, the signal, F107's rule (native_on_foot_f107.h Away::step with
// the shop input), the lines and their cap, and the wiring read as text. The real glue (the five hooks over synthetic functions with the game's prologues, the callbacks' reads, the ring, the
// frame thread's tick and its lines) is run by native_on_foot_hooks_test.
//
// The figures are the flights' (edvr_gfx_20261101_114724 and _141044, v0.22.0-rc.5): the suit shop's showroom camera 3.6 to 3.7 m from the neck with a second camera 46 cm over it taking turns at
// 45 calls a second; the weapon showroom's camera the commander's own, 0.9 cm under the neck (no distance test could see it).
#pragma once

#include "../../src/d3d11/native_on_foot_f107.h"
#include "../../src/d3d11/preview_headlook_core.h"
#include "../../src/d3d11/shop_screen_core.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>

namespace f209test {

namespace shop = edvr::shop;
namespace f107 = edvr::nofoot::f107;

inline std::string slurp(const char* path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    std::string s = ss.str();
    s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());   // a checkout with CRLF reads as LF
    return s;
}
inline bool has_(const std::string& text, const char* needle) { return text.find(needle) != std::string::npos; }
inline size_t countOf(const std::string& text, const std::string& needle) {
    size_t n = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size())) ++n;
    return n;
}

// What a remembered object looks like to the frame thread's guarded reads: whether its vtable and its byte could be read, and their values.
struct FakeObject {
    bool vtableReadable = true, byteReadable = true;
    uint64_t vtable = 0;
    uint8_t byte = 0;
};
inline shop::Verdict recheck(const FakeObject* o, int cls, uintptr_t base) {
    if (!o) return shop::judgeObject(false, false, 0, 0, false, 0);
    return shop::judgeObject(true, o->vtableReadable, o->vtable, base + shop::vtableRva(cls), o->byteReadable, o->byte);
}

template <class Check>
inline void run209(Check&& check) {
    const uintptr_t base = 0x7FF600000000ull;
    // ---- 1. the constants: build 334925's addresses, offsets and prologues (the RE note's, read from the exe) ----------------------------------------------------------------------------
    {
        check(shop::kShowRva == 0x9C3960 && shop::kCloseRva == 0x9C22A0 && shop::kSetStateRva == 0xA237B0 && shop::kKioskStartRva == 0x1973820 && shop::kKioskStopRva == 0x1916440 &&
                  shop::kSuitVtableRva == 0x5255D20 && shop::kWeaponsVtableRva == 0x5255A20 && shop::kKioskVtableRva == 0x51E9770 && shop::kOffOpen == 0x79 && shop::kOffKiosk == 0x1F8,
              "F209: the addresses are build 334925's (Show 0x9C3960, Close 0x9C22A0, SetState 0xA237B0, kiosk start 0x1973820 and stop 0x1916440; vtables suit 0x5255D20, weapons 0x5255A20, kiosk 0x51E9770; bytes +0x79 and +0x1F8)");
        check(shop::kLiveryVtableRva == 0x5612F30 && shop::kWeaponLiveryVtableRva == 0x5614328 && shop::kClassCount == 6 && shop::kLivery == 3 && shop::kWeaponLivery == 4 && shop::kOutfitting == 5 &&
                  shop::kOutfittingVtableRva == 0x524D908,
              "F228: the Livery screens' vtables are build 334925's (HumanoidLiveryActivity 0x5612F30, HumanoidWeaponLiveryActivity 0x5614328; their Show, IsOpen and Close slots hold the buy screens' functions; F234: and the loadout browser's 0x524D908), six classes");
        check(shop::kShowPrologueBytes == 14 && shop::kShowPrologue[0] == 0x40 && shop::kShowPrologue[1] == 0x53 && shop::kShowPrologue[13] == 0xE8 && shop::kClosePrologueBytes == 15 &&
                  shop::kClosePrologue[6] == 0x80 && shop::kClosePrologue[7] == 0x79 && shop::kClosePrologue[8] == 0x79 && shop::kSetStatePrologueBytes == 10 && shop::kSetStatePrologue[9] == 0xE8 &&
                  shop::kKioskStartPrologueBytes == 16 && shop::kKioskStartPrologue[0] == 0x48 && shop::kKioskStartPrologue[2] == 0xC4 && shop::kKioskStopPrologueBytes == 16 &&
                  shop::kKioskStopPrologue[0] == 0x48 && shop::kKioskStopPrologue[4] == 0x10,
              "F209: the prologues are the exe's (Show 14 bytes ending before its call, Close 15 with `cmp byte [rcx+79h],0`, SetState 10, the kiosk's 16 and 16)");
        check(std::strcmp(shop::kStateMachineName, "SuitUIStateMachine") == 0 && shop::kOffHandleName == 0x08 && shop::kOffStringChars == 0x14, "F209: the state machine handle's name is read at [handle+8]+0x14 and is SuitUIStateMachine");
    }
    // ---- 2. the Show/Close filter: the two buy screens by their vtables, every other class of the 40 that share the base is returned at once -----------------------------------------------------------
    {
        const uint64_t suit = base + shop::kSuitVtableRva, weapons = base + shop::kWeaponsVtableRva, kiosk = base + shop::kKioskVtableRva, other = base + 0x5255000;
        check(shop::buyScreenOfVtable(suit, base) == shop::kSuit && shop::buyScreenOfVtable(weapons, base) == shop::kWeapons && shop::buyScreenOfVtable(kiosk, base) == shop::kNotOurs &&
                  shop::buyScreenOfVtable(other, base) == shop::kNotOurs && shop::buyScreenOfVtable(0, base) == shop::kNotOurs && shop::buyScreenOfVtable(suit, base + 0x1000) == shop::kNotOurs,
              "F209: Show and Close are filtered on *(this): the suit and the weapon buy screens' vtables are ours; the kiosk, any other class, a null vtable and another image base are not");
        check(shop::vtableRva(shop::kSuit) == shop::kSuitVtableRva && shop::vtableRva(shop::kWeapons) == shop::kWeaponsVtableRva && shop::vtableRva(shop::kKiosk) == shop::kKioskVtableRva &&
                  std::strcmp(shop::className(shop::kSuit), "SuitVendorInteraction") == 0 && std::strcmp(shop::className(shop::kWeapons), "WeaponsVendorInteraction") == 0 &&
                  std::strcmp(shop::className(shop::kKiosk), "ArmouryVendorActivity") == 0,
              "F209: each class names its vtable and its own name");
    }
    {   // F228: the Livery screens join the filter; the buy screens' filter is unchanged
        const uint64_t liv = base + shop::kLiveryVtableRva, wliv = base + shop::kWeaponLiveryVtableRva, suit = base + shop::kSuitVtableRva, weapons = base + shop::kWeaponsVtableRva,
                       kiosk = base + shop::kKioskVtableRva, other = base + 0x5255000;
        check(shop::screenOfVtable(liv, base) == shop::kLivery && shop::screenOfVtable(wliv, base) == shop::kWeaponLivery && shop::screenOfVtable(suit, base) == shop::kSuit &&
                  shop::screenOfVtable(weapons, base) == shop::kWeapons && shop::screenOfVtable(kiosk, base) == shop::kNotOurs && shop::screenOfVtable(other, base) == shop::kNotOurs &&
                  shop::screenOfVtable(0, base) == shop::kNotOurs && shop::screenOfVtable(liv, base + 0x1000) == shop::kNotOurs &&
                  shop::buyScreenOfVtable(liv, base) == shop::kNotOurs && shop::buyScreenOfVtable(wliv, base) == shop::kNotOurs,
              "F228: Show and Close are filtered on *(this): the two Livery vtables are ours too (and not buy screens); the kiosk, any other class, null and another base are not");
        const uint64_t outf = base + shop::kOutfittingVtableRva;
        check(shop::screenOfVtable(outf, base) == shop::kOutfitting && shop::buyScreenOfVtable(outf, base) == shop::kNotOurs && shop::vtableRva(shop::kOutfitting) == shop::kOutfittingVtableRva &&
                  std::strcmp(shop::className(shop::kOutfitting), "HumanoidOutfittingInteraction") == 0 && !shop::isLivery(shop::kOutfitting) && shop::screenOfVtable(outf, base + 0x1000) == shop::kNotOurs,
              "F234: the loadout browser joins the Show/Close filter as a class of its own (not a buy screen, not a Livery screen)");
        check(shop::vtableRva(shop::kLivery) == shop::kLiveryVtableRva && shop::vtableRva(shop::kWeaponLivery) == shop::kWeaponLiveryVtableRva &&
                  std::strcmp(shop::className(shop::kLivery), "HumanoidLiveryActivity") == 0 && std::strcmp(shop::className(shop::kWeaponLivery), "HumanoidWeaponLiveryActivity") == 0 &&
                  shop::isLivery(shop::kLivery) && shop::isLivery(shop::kWeaponLivery) && !shop::isLivery(shop::kSuit) && !shop::isLivery(shop::kWeapons) && !shop::isLivery(shop::kKiosk) && !shop::isLivery(shop::kNotOurs),
              "F228: each Livery class names its vtable and its own name");
    }
    // ---- 3. the signal from Show and Close, and the once-a-frame re-check ----------------------------------------------------------------------------------------------------------------------
    {
        FakeObject suit, weapons, kiosk;
        suit.vtable = base + shop::kSuitVtableRva;
        weapons.vtable = base + shop::kWeaponsVtableRva;
        kiosk.vtable = base + shop::kKioskVtableRva;
        auto sig = [&](const FakeObject* s, const FakeObject* w, const FakeObject* k) {
            return shop::signalOf(recheck(s, shop::kSuit, base), recheck(w, shop::kWeapons, base), recheck(k, shop::kKiosk, base));
        };
        check(!sig(nullptr, nullptr, nullptr).buyScreenOpen(), "F209: nothing remembered: the signal is off");
        suit.byte = 1;   // Show sets +0x79
        check(sig(&suit, nullptr, nullptr).buyScreenOpen() && sig(&suit, nullptr, nullptr).open[shop::kSuit] && !sig(&suit, nullptr, nullptr).open[shop::kWeapons], "F209: the suit buy screen's Show: the signal is on, for the suit");
        weapons.byte = 1;
        check(sig(&suit, &weapons, nullptr).open[shop::kSuit] && sig(&suit, &weapons, nullptr).open[shop::kWeapons], "F209: both buy screens open: both are in the signal");
        suit.byte = 0;   // Close clears it
        check(sig(&suit, &weapons, nullptr).buyScreenOpen() && !sig(&suit, &weapons, nullptr).open[shop::kSuit], "F209: the suit screen's Close: the weapon screen still holds the signal");
        weapons.byte = 0;
        check(!sig(&suit, &weapons, nullptr).buyScreenOpen(), "F209: both Closed: the signal is off");
        kiosk.byte = 1;
        const shop::Signal onlyKiosk = sig(&suit, &weapons, &kiosk);
        check(onlyKiosk.kiosk && !onlyKiosk.buyScreenOpen(), "F209: the kiosk session alone is not a buy screen: it is reported, and it does not hold F107 away");
        // a missed Close: the event never came, the game cleared the byte: the frame thread's read says closed
        suit.byte = 1;
        check(sig(&suit, nullptr, nullptr).buyScreenOpen(), "F209 (control): the byte reads open");
        suit.byte = 0;
        check(!sig(&suit, nullptr, nullptr).buyScreenOpen() && recheck(&suit, shop::kSuit, base) == shop::Verdict::kClosed && !shop::forgets(shop::Verdict::kClosed),
              "F209: a Close the hook missed cannot stick the signal: the byte read each frame says closed, and the object is kept (a screen opens again at the same address)");
        // a freed object whose address was reused: the vtable is no longer the class's
        suit.byte = 1;
        suit.vtable = base + 0x5255000;
        check(recheck(&suit, shop::kSuit, base) == shop::Verdict::kStaleVtable && shop::forgets(recheck(&suit, shop::kSuit, base)) && !sig(&suit, nullptr, nullptr).buyScreenOpen(),
              "F209: a stale `this` whose vtable changed (freed and reused) is forgotten and the signal goes off though its byte reads 1");
        // a vtable of the other buy screen at the same address is just as stale
        suit.vtable = base + shop::kWeaponsVtableRva;
        check(recheck(&suit, shop::kSuit, base) == shop::Verdict::kStaleVtable, "F209: the weapon screen's vtable at the suit's remembered address is stale for the suit");
        // memory that cannot be read
        FakeObject gone;
        gone.vtableReadable = false;
        FakeObject noByte;
        noByte.vtable = base + shop::kSuitVtableRva;
        noByte.byteReadable = false;
        check(recheck(&gone, shop::kSuit, base) == shop::Verdict::kUnreadable && recheck(&noByte, shop::kSuit, base) == shop::Verdict::kUnreadable && shop::forgets(shop::Verdict::kUnreadable) &&
                  !sig(&gone, nullptr, nullptr).buyScreenOpen() && !sig(&noByte, nullptr, nullptr).buyScreenOpen(),
              "F209: an object whose vtable or byte cannot be read is forgotten and never holds the signal");
        check(!shop::forgets(shop::Verdict::kNone) && !shop::forgets(shop::Verdict::kOpen) && !shop::forgets(shop::Verdict::kClosed), "F209: only a stale or unreadable object is forgotten");
        // the game's own tests are `!= 0`
        suit.vtable = base + shop::kSuitVtableRva;
        suit.byte = 2;
        check(recheck(&suit, shop::kSuit, base) == shop::Verdict::kOpen, "F209: the byte is read as the game reads it: any non-zero value is open");
    }
    {   // F228/F234: a Livery screen or the loadout browser open holds the signal, like a buy screen
        FakeObject liv, wliv, loadout;
        liv.vtable = base + shop::kLiveryVtableRva;
        wliv.vtable = base + shop::kWeaponLiveryVtableRva;
        loadout.vtable = base + shop::kOutfittingVtableRva;
        auto sig = [&](const FakeObject* l, const FakeObject* w, const FakeObject* o = nullptr) {
            return shop::signalOf(shop::Verdict::kNone, shop::Verdict::kNone, shop::Verdict::kNone, recheck(l, shop::kLivery, base), recheck(w, shop::kWeaponLivery, base), recheck(o, shop::kOutfitting, base));
        };
        check(!sig(nullptr, nullptr).holds() && !sig(nullptr, nullptr).liveryOpen() && !sig(nullptr, nullptr).loadout, "F228: no Livery screen remembered: nothing holds");
        liv.byte = 1;   // Show sets +0x79
        const shop::Signal one = sig(&liv, nullptr);
        check(one.liveryOpen() && one.livery[0] && !one.livery[1] && !one.buyScreenOpen() && one.holds(), "F228: the suit Livery's Show: it holds the signal; it is not a buy screen");
        wliv.byte = 1;
        const shop::Signal both = sig(&liv, &wliv);
        check(both.livery[0] && both.livery[1] && both.holds() && std::strcmp(shop::liveryText(both), "suit and weapon open") == 0 && std::strcmp(shop::liveryText(one), "suit open") == 0,
              "F228: both Livery screens open are both in the signal");
        liv.byte = 0;   // Close clears it
        check(sig(&liv, &wliv).holds() && std::strcmp(shop::liveryText(sig(&liv, &wliv)), "weapon open") == 0, "F228: the suit Livery's Close: the weapon Livery still holds");
        wliv.byte = 0;
        check(!sig(&liv, &wliv).holds() && std::strcmp(shop::liveryText(sig(&liv, &wliv)), "closed") == 0, "F228: both Closed: nothing holds");
        liv.byte = 1;
        liv.vtable = base + shop::kLiveryVtableRva + 0x100;
        check(recheck(&liv, shop::kLivery, base) == shop::Verdict::kStaleVtable && !sig(&liv, nullptr).holds(), "F228: a Livery object whose vtable changed is forgotten and holds nothing");
        liv.vtable = base + shop::kWeaponLiveryVtableRva;
        check(recheck(&liv, shop::kLivery, base) == shop::Verdict::kStaleVtable, "F228: the weapon Livery's vtable at the suit Livery's address is stale");
        loadout.byte = 1;
        const shop::Signal load = sig(nullptr, nullptr, &loadout);
        check(load.loadout && !load.liveryOpen() && !load.buyScreenOpen() && load.holds() && std::strcmp(shop::liveryText(load), "loadout open") == 0, "F234: the loadout browser's Show holds the signal; it is neither a Livery screen nor a buy screen");
        liv.vtable = base + shop::kLiveryVtableRva;
        const shop::Signal two = sig(&liv, nullptr, &loadout);
        check(two.holds() && std::strcmp(shop::liveryText(two), "suit and loadout open") == 0, "F234: a Livery screen and the loadout browser together name both");
        loadout.byte = 0;
        check(!sig(nullptr, nullptr, &loadout).holds(), "F234: the loadout browser's Close: nothing holds");
        loadout.vtable = base + shop::kOutfittingVtableRva + 0x100;
        loadout.byte = 1;
        check(recheck(&loadout, shop::kOutfitting, base) == shop::Verdict::kStaleVtable && !sig(nullptr, nullptr, &loadout).holds(), "F234: a loadout object whose vtable changed is forgotten and holds nothing");
        FakeObject suit, kioskObj;
        suit.vtable = base + shop::kSuitVtableRva;
        suit.byte = 1;
        const shop::Signal buy = shop::signalOf(recheck(&suit, shop::kSuit, base), shop::Verdict::kNone, shop::Verdict::kNone);
        check(buy.holds() && !buy.liveryOpen() && !buy.loadout, "F228: a buy screen holds: its behaviour is as in F209");
        kioskObj.vtable = base + shop::kKioskVtableRva;
        kioskObj.byte = 1;
        check(!shop::signalOf(shop::Verdict::kNone, shop::Verdict::kNone, recheck(&kioskObj, shop::kKiosk, base)).holds(), "F209: the kiosk does not hold the signal");
    }
    // ---- 4. F107: a buy screen open holds the camera away at any distance --------------------------------------------------------------------------------------------------------------------
    {   // the weapon showroom: the commander's own camera, 0.9 cm under the neck, 90 calls (the flight's) while the screen is open; the screen closes, the camera stays near
        f107::Away a;
        int left = 0, back = 0;
        double t = 0.0, backAt = -1.0;
        for (int i = 0; i < 20; ++i, t += 0.0222) a.step(0.2, t, false);   // walking up to the shop's counter
        const double openAt = t;
        for (int i = 0; i < 90; ++i, t += 0.0222) {
            const f107::Away::Edge e = a.step(0.009, t, true);
            left += e == f107::Away::kLeft;
            back += e == f107::Away::kBack;
        }
        const bool awayThroughout = a.away && a.shopCalls == 90 && a.distanceCalls == 0 && a.beganByShop && !a.beganByDistance && a.calls == 90 && a.farCalls == 90;
        const double closeAt = t;
        for (int i = 0; i < 200; ++i, t += 0.0222) {   // the screen closed: the camera stays near for 4.4 s
            const f107::Away::Edge e = a.step(0.009, t, false);
            left += e == f107::Away::kLeft;
            if (e == f107::Away::kBack) {
                ++back;
                backAt = t;
            }
        }
        check(left == 1 && awayThroughout && back == 1 && a.episodes == 1 && !a.away && openAt < closeAt && backAt >= closeAt + f107::kReturnHoldS - 1e-9 && backAt < closeAt + f107::kReturnHoldS + 0.03,
              "F209: the weapon showroom (the commander's own camera, 0.9 cm under the neck): the buy screen open is one episode from the first call, every call refused, and it ends through F208's hold (2.0 s) after the screen closes");
    }
    {   // the suit shop: the showroom camera 3.7 m out and the near camera 46 cm over the neck alternating for 18 s with the screen open: one episode, no flap counted as a rejoin, the hold unchanged
        f107::Away a;
        int left = 0, back = 0;
        double t = 0.0;
        for (int i = 0; i < 800; ++i, t += 1.0 / 45.0) {
            const double d = (i / 3) % 2 == 0 ? 3.7 : 0.46;
            const f107::Away::Edge e = a.step(d, t, true);
            left += e == f107::Away::kLeft;
            back += e == f107::Away::kBack;
        }
        check(left == 1 && back == 0 && a.away && a.episodes == 1 && a.rejoins == 0 && std::fabs(a.holdS - f107::kReturnHoldS) < 1e-12 && a.shopCalls == 800 && a.distanceCalls > 0 && a.distanceCalls < 800 &&
                  a.beganByShop && a.beganByDistance,
              "F209: the suit shop's alternating cameras with the screen open are one episode by both reasons, with no rejoin and the base hold (the flap that doubled the hold in F208b needs no hold here)");
        const double closeAt = t;
        double backAt = -1.0;
        for (int i = 0; i < 200; ++i, t += 1.0 / 45.0) {
            const f107::Away::Edge e = a.step(0.46, t, false);
            if (e == f107::Away::kBack) backAt = t;
        }
        check(backAt >= closeAt + f107::kReturnHoldS - 1e-9 && backAt < closeAt + f107::kReturnHoldS + 0.03 && !a.away, "F209: closing the suit screen with the camera on the commander ends the episode after the 2.0 s hold");
    }
    {   // the screen closes but the camera stays at the showroom (3.7 m): the distance test keeps the episode, as before
        f107::Away a;
        double t = 0.0;
        for (int i = 0; i < 100; ++i, t += 0.02) a.step(3.7, t, true);
        for (int i = 0; i < 400; ++i, t += 0.02) a.step(3.7, t, false);
        check(a.away && a.episodes == 1 && a.shopCalls == 100 && a.distanceCalls == 500, "F209: the screen closed with the camera still 3.7 m out: the distance test keeps the episode open");
        double backAt = -1.0, nearAt = t;
        for (int i = 0; i < 200; ++i, t += 0.02)
            if (a.step(0.3, t, false) == f107::Away::kBack) backAt = t;
        check(backAt >= nearAt + f107::kReturnHoldS - 1e-9 && !a.away, "F209: ... and the camera's return ends it after the hold");
    }
    {   // normal play unchanged: the same distances with the shop input absent, false, or true-then-never give what F208/F208b gave; the distance test is the only reason
        const double seq[] = {0.2, 0.2, 3367.4, 3367.6, 139.2, 0.19, 0.19, 3.7, 0.46, 0.46, 3.6, 0.5, 0.2, 710.5, 0.3, 0.2, 0.2, 0.2, 0.2, 0.2, 0.2, 0.2};
        f107::Away dflt, explicitOff;
        bool same = true;
        double t = 0.0;
        for (int round = 0; round < 12; ++round)
            for (const double d : seq) {
                t += 0.4;
                const f107::Away::Edge e1 = dflt.step(d, t);
                const f107::Away::Edge e2 = explicitOff.step(d, t, false);
                same = same && e1 == e2 && dflt.away == explicitOff.away && dflt.calls == explicitOff.calls && dflt.farCalls == explicitOff.farCalls && dflt.episodes == explicitOff.episodes &&
                       dflt.alternations == explicitOff.alternations && dflt.rejoins == explicitOff.rejoins && dflt.holdS == explicitOff.holdS;
            }
        check(same && dflt.shopCalls == 0 && dflt.episodes > 1 && !dflt.beganByShop && dflt.beganByDistance, "F209: with no buy screen open F107 behaves exactly as before: the shop input defaults off, and the distance test is the only reason an episode begins");
        f107::Away fresh;
        for (int i = 0; i < 10; ++i) fresh.step(0.2, i * 0.02, false);
        const f107::Away::Edge e = fresh.step(0.5, 1.0, false);
        check(e == f107::Away::kNone && !fresh.away && fresh.episodes == 0, "F209: the near camera 46 cm over the neck with no buy screen open is the commander's own: no episode");
        f107::Away carrier;
        const f107::Away::Edge left = carrier.step(3367.4, 0.0, false);
        check(left == f107::Away::kLeft && carrier.beganByDistance && !carrier.beganByShop && carrier.distanceCalls == 1 && carrier.shopCalls == 0,
              "F209: the carrier management screen (3367 m) is still away by the distance test alone");
        f107::Away nan;
        const f107::Away::Edge leftNan = nan.step(std::numeric_limits<double>::quiet_NaN(), 0.0, false);
        check(leftNan == f107::Away::kLeft && nan.notNumber == 1 && nan.beganByDistance, "F209: a camera that is not a number is still away by the distance test");
    }
    // ---- 5. the lines ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        char line[512];
        shop::Event e;
        e.kind = shop::kShow;
        e.cls = shop::kSuit;
        e.before = 0;
        e.after = 1;
        e.tid = 7;
        e.thisLow = 0x1A2B3C4D;
        shop::formatEvent(line, sizeof(line), e);
        check(std::strcmp(line, "shop screens (F209): SuitVendorInteraction Show: +0x79 0 -> 1 (this 0x1A2B3C4D, thread 7)") == 0, "F209: a buy screen's Show line says the class, +0x79 before and after, the object and the thread");
        e.kind = shop::kClose;
        e.cls = shop::kWeapons;
        e.before = 1;
        e.after = 0;
        shop::formatEvent(line, sizeof(line), e);
        check(std::strcmp(line, "shop screens (F209): WeaponsVendorInteraction Close: +0x79 1 -> 0 (this 0x1A2B3C4D, thread 7)") == 0, "F209: a Close line likewise");
        e = shop::Event();
        e.kind = shop::kState;
        e.tid = 9;
        std::snprintf(e.from, sizeof(e.from), "%s", "OnFoot");
        std::snprintf(e.to, sizeof(e.to), "%s", "Suit Vendor");
        shop::formatEvent(line, sizeof(line), e);
        check(std::strcmp(line, "shop screens (F209): SuitUIStateMachine: \"OnFoot\" -> \"Suit Vendor\" (thread 9)") == 0, "F209: a SuitUIStateMachine state change says from, to and the thread");
        e = shop::Event();
        e.kind = shop::kKioskStart;
        e.cls = shop::kKiosk;
        e.before = 0;
        e.after = 1;
        e.tid = 4;
        e.thisLow = 0x3C4D5E6F;
        shop::formatEvent(line, sizeof(line), e);
        check(std::strcmp(line, "shop screens (F209): ArmouryVendorActivity started: +0x1F8 0 -> 1 (this 0x3C4D5E6F, thread 4)") == 0, "F209: the kiosk's start line says +0x1F8 before and after");
        e.kind = shop::kKioskStop;
        e.before = 1;
        e.after = 0;
        shop::formatEvent(line, sizeof(line), e);
        check(std::strcmp(line, "shop screens (F209): ArmouryVendorActivity stopped: +0x1F8 1 -> 0 (this 0x3C4D5E6F, thread 4)") == 0, "F209: ... and its stop line");
        e.kind = shop::kShow;
        e.cls = shop::kSuit;
        e.before = -1;
        e.after = -1;
        shop::formatEvent(line, sizeof(line), e);
        check(std::strstr(line, "+0x79 -1 -> -1") != nullptr, "F209: an unreadable byte reads -1 in the line");
        shop::Signal s;
        s.open[shop::kSuit] = true;
        shop::formatSignal(line, sizeof(line), s, true, 3);
        check(std::strcmp(line, "shop screens (F209): the shop signal is ON: F107 holds the camera away (suit open, weapons closed, kiosk not running, livery closed; change 3)") == 0, "F209: the signal's ON line says which screen and the change count (F228: and the Livery screens)");
        s = shop::Signal();
        s.kiosk = true;
        shop::formatSignal(line, sizeof(line), s, false, 4);
        check(std::strcmp(line, "shop screens (F209): the shop signal is OFF (suit closed, weapons closed, kiosk running, livery closed; change 4)") == 0, "F209: the OFF line likewise");
        s = shop::Signal();
        s.livery[1] = true;
        shop::formatSignal(line, sizeof(line), s, true, 5);
        check(std::strcmp(line, "shop screens (F209): the shop signal is ON: F107 holds the camera away (suit closed, weapons closed, kiosk not running, livery weapon open; change 5)") == 0, "F228: the ON line says which Livery screen");
        e = shop::Event();
        e.kind = shop::kShow;
        e.cls = shop::kLivery;
        e.before = 0;
        e.after = 1;
        e.tid = 7;
        e.thisLow = 0x4D5E6F70;
        shop::formatEvent(line, sizeof(line), e);
        check(std::strcmp(line, "shop screens (F209): HumanoidLiveryActivity Show: +0x79 0 -> 1 (this 0x4D5E6F70, thread 7)") == 0, "F228: a Livery screen's Show line names the class");
        e.kind = shop::kClose;
        e.cls = shop::kWeaponLivery;
        e.before = 1;
        e.after = 0;
        shop::formatEvent(line, sizeof(line), e);
        check(std::strcmp(line, "shop screens (F209): HumanoidWeaponLiveryActivity Close: +0x79 1 -> 0 (this 0x4D5E6F70, thread 7)") == 0, "F228: the weapon Livery's Close line names the class");
        shop::formatHeldOpened(line, sizeof(line), shop::kLivery);
        check(std::strcmp(line, "shop screens (F209): HumanoidLiveryActivity opened: F107 holds the camera away, the view is the game's own") == 0, "F228: a Livery screen opening says it holds the camera away");
        shop::formatHeldOpened(line, sizeof(line), shop::kWeaponLivery);
        check(std::strcmp(line, "shop screens (F209): HumanoidWeaponLiveryActivity opened: F107 holds the camera away, the view is the game's own") == 0, "F228: ... so does the weapon Livery's");
        shop::formatHeldOpened(line, sizeof(line), shop::kOutfitting);
        check(std::strcmp(line, "shop screens (F209): HumanoidOutfittingInteraction opened: F107 holds the camera away, the view is the game's own") == 0, "F234: ... and the loadout browser's");
        shop::Signal onlyLoad = shop::signalOf(shop::Verdict::kNone, shop::Verdict::kNone, shop::Verdict::kNone, shop::Verdict::kNone, shop::Verdict::kNone, shop::Verdict::kOpen);
        shop::formatSignal(line, sizeof(line), onlyLoad, true, 3);
        check(std::strcmp(line, "shop screens (F209): the shop signal is ON: F107 holds the camera away (suit closed, weapons closed, kiosk not running, livery loadout open; change 3)") == 0, "F234: the ON line names the loadout browser");
        shop::formatForgotten(line, sizeof(line), shop::kSuit, shop::Verdict::kStaleVtable, 0x7FF600001234ull, 0x7FF605255D20ull, 0x1A2B3C4D);
        check(std::strstr(line, "the remembered SuitVendorInteraction (this 0x1A2B3C4D) is forgotten: its vtable is no longer the class's (vtable 0x7FF600001234, the class's 0x7FF605255D20)") != nullptr,
              "F209: the forgotten line says the class, the object and both vtables");
        shop::formatSecond(line, sizeof(line), 1, -1, 0, -1, -1, "Suit Vendor", true, "away (episode 1, 90 calls refused, 90 with a buy screen open)");
        check(std::strcmp(line, "shop screens (F209): 1 s: suit +0x79 1, weapons +0x79 none, kiosk +0x1F8 0, livery +0x79 none, weapon livery +0x79 none, SuitUIStateMachine \"Suit Vendor\", signal on; F107: away (episode 1, 90 calls refused, 90 with a buy screen open)") == 0,
              "F209: the once-a-second line says the remembered bytes, the state, the kiosk byte, the signal and F107's state (F228: and the Livery bytes)");
        shop::formatSecond(line, sizeof(line), -1, -1, -1, 1, 0, "", true, "away");
        check(std::strstr(line, "livery +0x79 1, weapon livery +0x79 0, SuitUIStateMachine \"unknown\", signal on") != nullptr, "F228: the Livery bytes read in the once-a-second line");
        shop::formatSecond(line, sizeof(line), -1, -1, -1, -1, -1, "", false, "not away");
        check(std::strstr(line, "SuitUIStateMachine \"unknown\", signal off; F107: not away") != nullptr && std::strstr(line, "suit +0x79 none") != nullptr, "F209: no state yet reads unknown, no object reads none");
    }
    // ---- 6. the cap and the pacer; what is "set" -------------------------------------------------------------------------------------------------------------------------------------------------
    {
        shop::LineCap cap;
        uint32_t summary = 99;
        int written = 0;
        for (int i = 0; i < 100; ++i)
            if (cap.allow(1000 + static_cast<uint64_t>(i), &summary)) ++written;
        check(written == static_cast<int>(shop::kLinesPer10s) && cap.dropped == 100 - shop::kLinesPer10s, "F209: a flapping screen writes at most 40 event lines in ten seconds; the rest are counted");
        const bool next = cap.allow(1000 + 10000, &summary);
        check(next && summary == 100 - shop::kLinesPer10s, "F209: the next window says how many the last one dropped, once");
        const bool again = cap.allow(1000 + 10001, &summary);
        check(again && summary == 0, "F209: ... and not again");
        shop::LineCap quiet;
        bool all = true;
        for (int w = 0; w < 5; ++w)
            for (int i = 0; i < 10; ++i) all = all && quiet.allow(5000 + static_cast<uint64_t>(w) * 10000 + static_cast<uint64_t>(i) * 100, &summary) && summary == 0;
        check(all, "F209: ordinary shop traffic (a handful of lines a window) is never capped");
        shop::Pacer p;
        check(p.due(5000) && !p.due(5500) && !p.due(5999) && p.due(6000) && !p.due(6001) && p.due(7001) && p.due(0) && !p.due(500), "F209: the once-a-second line is due on the first look, then every 1000 ms (a clock that went back is due once)");
        check(shop::stateIsVendor("Suit Vendor") && shop::stateIsVendor("Weapons Upgrade Vendor") && shop::stateIsVendor("Armoury Vendor") && !shop::stateIsVendor("OnFoot") && !shop::stateIsVendor("") &&
                  !shop::stateIsVendor(nullptr) && !shop::stateIsVendor("Cockpit"),
              "F209: a vendor state is one whose name has Vendor in it; OnFoot, an empty state and a state the build does not name are not");
        check(shop::anySet(true, false, "OnFoot") && shop::anySet(false, true, "") && shop::anySet(false, false, "Suit Vendor") && !shop::anySet(false, false, "OnFoot") && !shop::anySet(false, false, ""),
              "F209: the once-a-second line runs while a buy screen is open, the kiosk runs, or the machine is in a vendor state, and is silent otherwise");
        check(shop::anySet(false, false, "OnFoot", true) && shop::anySet(false, false, "", true) && !shop::anySet(false, false, "OnFoot", false) && !shop::anySet(false, false, "OnFoot"),
              "F228: the once-a-second line also runs while a Livery screen is open (held or only watched)");
        check(shop::isSuitUiStateMachine("SuitUIStateMachine") && !shop::isSuitUiStateMachine("SuitUIStateMachineX") && !shop::isSuitUiStateMachine("SuitUIState") && !shop::isSuitUiStateMachine("") &&
                  !shop::isSuitUiStateMachine(nullptr) && !shop::isSuitUiStateMachine("OnFoot"),
              "F209: only the handle named exactly SuitUIStateMachine is recorded");
    }
    // ---- 7. the wiring, read as text -----------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        const std::string hooks = slurp("src/d3d11/native_on_foot_hooks.cpp"), cpp = slurp("src/d3d11/native_on_foot.cpp");
        check(countOf(hooks, "shop::kShowRva") >= 1 && countOf(hooks, "shop::kCloseRva") >= 1 && countOf(hooks, "shop::kSetStateRva") >= 1 && countOf(hooks, "shop::kKioskStartRva") >= 1 &&
                  countOf(hooks, "shop::kKioskStopRva") >= 1 && countOf(hooks, "case kHkShow: return reinterpret_cast<const void*>(&shopShowHooked)") == 1 &&
                  countOf(hooks, "case kHkKioskStop: return reinterpret_cast<const void*>(&shopKioskStopHooked)") == 1,
              "F209: the five hooks are in the hook table with their addresses and callbacks");
        check(hooks.find("tryShopHooks(sink);") != std::string::npos && hooks.find("shopFrameTick(sink);") != std::string::npos && hooks.find("shopFrameTick(sink);") < hooks.find("    updateGates();\n}\navh::Sink g_frameSink"),
              "F209: the frame boundary tries the shop hooks (while the native key is on) and re-reads the remembered objects every frame, before the gates are set");
        check(cpp.find("g_f107Away.step(eyeNeckM, static_cast<double>(f30QpcUs(in.qpc)) * 1e-6, nativeOnFootShopOpen())") != std::string::npos,
              "F209: the Animate post-hook gives F107 the shop signal with every call");
        const std::string ini = slurp("edvr.ini");
        const std::string gone[] = {std::string("livery") + "_game_view", std::string("loadout") + "_game_view"};   // the retired keys' names, built so that the source holds no trace of them
        check(ini.find(gone[0]) == std::string::npos && ini.find(gone[1]) == std::string::npos && cpp.find(gone[0]) == std::string::npos && cpp.find(gone[1]) == std::string::npos &&
                  hooks.find("g_liveryHold") == std::string::npos && hooks.find("g_loadoutHold") == std::string::npos,
              "F228/F234: the two hold keys are gone from the ini, the frame thread and the hooks: the holds are behaviour");
        check(has_(hooks, "return shop::screenOfVtable(vtable, base);") && has_(hooks, "const bool on = s.holds();") &&
                  has_(hooks, "bool nativeOnFootShopOpen() noexcept { return shopBuyScreenOpen() || nativeOnFootLiveryOpen() || g_shopOpen[shop::kOutfitting].load(std::memory_order_acquire) != 0; }") &&
                  has_(hooks, "std::atomic<int8_t> g_shopByte[shop::kClassCount] = {{-1}, {-1}, {-1}, {-1}, {-1}, {-1}};") &&
                  has_(hooks, "if (e.kind == shop::kShow && (shop::isLivery(e.cls) || e.cls == shop::kOutfitting) && e.after == 1) {"),
              "F228/F234: the Show/Close filter takes the Livery and loadout vtables, the frame thread's signal and F107's input hold them, their opening is said once, and every per-class array is sized by kClassCount");
        // every shop callback forwards to the original: the original's result is returned unchanged (nothing is written to the game)
        const size_t at = hooks.find("// ---- F209: the Pioneer Supplies buy screens");
        const size_t end = hooks.find("// ---- hiding the head parts", at);
        const std::string block = at != std::string::npos && end != std::string::npos ? hooks.substr(at, end - at) : std::string();
        check(!block.empty() && countOf(block, "forward(a, b, c, d)") >= 5 && block.find("VirtualProtect") == std::string::npos && block.find("WriteProcessMemory") == std::string::npos &&
                  block.find("Log::get()") == std::string::npos && block.find("= new ") == std::string::npos && block.find("std::vector") == std::string::npos && block.find("std::string") == std::string::npos &&
                  block.find("malloc(") == std::string::npos && block.find("lock_guard") == std::string::npos && block.find("std::mutex") == std::string::npos,
              "F209: the hook threads' code forwards every call and writes nothing to the game, takes no lock, allocates nothing and logs nothing");
    }
    // ---- 8. the preview camera's roster, HeadlookCamera and the refusal lines (F232, F233) are behaviour: no keys, no handover flag; the wiring read as text -------------------------------------------
    {
        namespace hl = edvr::previewcam::headlook;
        check(hl::kProducerRva == 0x1074650 && hl::kIfaceRva == 0x518C350 && hl::kObjectRva == 0x518C2E0 && hl::kProducerPinBytes == 15 && hl::kProducerPin[0] == 0x48 && hl::kProducerPin[4] == 0x08 &&
                  hl::kProducerPin[10] == 0x57 && hl::kProducerPin[11] == 0x48 && hl::kProducerPin[14] == 0x60 && hl::kOffRotation0 == 0x250 && hl::kOffRotation1 == 0x260 && hl::kOffRotation2 == 0x270 &&
                  hl::kOffDisplacement == 0x280,
              "F233: HeadlookCamera's producer 0x1074650 (15 pinned bytes), its vtables 0x518C350 and 0x518C2E0, the look rotation's and displacement's offsets");
        const std::string cpp = slurp("src/d3d11/native_on_foot.cpp"), cam = slurp("src/d3d11/native_preview_camera.cpp"), camh = slurp("src/d3d11/native_preview_camera.h"),
                          core = slurp("src/d3d11/native_preview_camera_core.h"), ini = slurp("edvr.ini"), hooks = slurp("src/d3d11/native_on_foot_hooks.cpp");
        const std::string retired[] = {std::string("preview") + "_handover", std::string("preview") + "_headlook", std::string("preview") + "_camera"};   // the retired keys' names, built so that the source holds no trace of them
        check(ini.find(retired[0]) == std::string::npos && ini.find(retired[1]) == std::string::npos && ini.find(retired[2]) == std::string::npos && cpp.find(retired[0]) == std::string::npos &&
                  cpp.find(retired[1]) == std::string::npos && cpp.find("SetHandover") == std::string::npos && cpp.find("SetHeadlook") == std::string::npos && cam.find("SetHandover") == std::string::npos &&
                  cam.find("SetHeadlook") == std::string::npos && camh.find("SetHandover") == std::string::npos && core.find("handover") == std::string::npos && core.find("headlook.load") == std::string::npos,
              "F232/F233: the handover and headlook keys, their setters and the handover flag are gone: the scan and HeadlookCamera are behaviour");
        check(has_(cpp, "nativePreviewCameraDiagTick(nativeOnFootPreviewScreenOpen());") && has_(cam, "g_previewEngine.diagWanted.store(wanted,std::memory_order_relaxed);") && has_(cam, "g_diagCap.allow(now)") &&
                  has_(cam, "if(g_diagAt&&now-g_diagAt<1000)return;") && has_(hooks, "bool nativeOnFootPreviewScreenOpen() noexcept {"),
              "F232: the refused roster is logged once a second under the cap while a watched preview screen is open");
        check(has_(core, "inline bool identity(const Memory& m,const Game& g,uintptr_t child,Identity& out) noexcept {\n    Refuse why=Refuse::kNone;\n    return identityWhy(m,g,child,out,why);\n}") &&
                  has_(core, "if(!get(m,child+0x58,count)||count<2||count>kMaxRoster){why=Refuse::kCount;return false;}") && has_(core, "else if(headlookOn&&headlookEntry(m,g,e,child)){++heads;head=e;headClass=1;}") &&
                  has_(core, "const bool headlookOn=g.headlookIface.load(std::memory_order_relaxed)!=0;") && core.find("Log::get()") == std::string::npos && core.find("std::mutex") == std::string::npos &&
                  has_(core, "if(d.busy.test_and_set(std::memory_order_acquire))return;"),
              "F232/F233: identity() is identityWhy(), the roster is two to six in any order, a Headlook entry is a head entry once its hook published the vtables, the snapshot is a try-claim, and the core logs and locks nothing");
        check(has_(core, "(headlook?true:!equal(s.primary,s.base))") && has_(core, "else if(!(equal(output,h.primary)||equal(output,p.primary)))why=FinalWhy::kOutputMismatch;") &&
                  has_(core, "const uintptr_t headIface=q.headClass==1?g.headlookIface.load(std::memory_order_relaxed):g.at[HmdIface];"),
              "F233: HeadlookCamera is asked only for the base unchanged and a finite primary, the finalizer accepts the head's or the preview's pose, the weights are those of the class present");
        check(has_(cam, "previewcam::hmd(g_previewEngine,g_previewMemory,g_previewGame,object,[&]{return original(object);},true);") && has_(cam, "gamePinMatches(hl::kProducerRva,bytes,hl::kProducerPin,hl::kProducerPinBytes)") &&
                  has_(cam, "HeadlookCamera producer hook stood down") && has_(cam, "g_previewGame.headlookIface.store(base+gameRva(hl::kIfaceRva),std::memory_order_release);") &&
                  has_(cam, "if(seen)g_previewEngine.eyeSeen(carriedRead?&carried:nullptr,published?&snapshot:nullptr,replaced);"),
              "F233: the producer hook is pinned and installed like the HMD one, routed to hmd() as the head role, stands down with one line, publishes the vtables only when armed; the eye stub keeps its snapshot");
    }
}

}  // namespace f209test
