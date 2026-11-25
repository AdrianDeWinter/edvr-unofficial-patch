// The rig for advanced.game_vram (docs/memory-growth-2026-10-22.md): the setting parser, the arithmetic that
// decides what Elite is told (src\common\game_vram.h), the call line's exact text and its buffer limits, the setup
// key, the learned store and the session rules (src\common\game_vram_learn.h), and by source text the glue that
// wires it (edup.ini, d3d11_proxy.cpp, vram_tick.cpp, runtime_profile.h, game_vram_hook.cpp).
//
// Every check's label starts with its case id (V1..V10):
//
//   V1   parseSetting: the words and numbers it recognises, and the ones it refuses
//   V2   compute, Off: never changes
//   V3   compute, Fixed: the cap, the card-or-under case, the floor is not applied
//   V4   compute, Auto: the arithmetic with different reserves, the OS budget (or 95% of the card), the floor,
//        the card's size
//   V5   formatCallLine and settingText: the exact text, and a short buffer is terminated and not overrun
//   V6   the glue, by source text: the ini key and its comment, both device creators call the hook, vram_tick feeds
//        the observer, runtime_profile.h lets every profile read the key, and the hook acts only on the exe's calls,
//        installs only in a profile that runs fixes and in the game, stops learning at a profile change and never
//        writes over a store it could not read (skipped without a root)
//   V7   the setup key, the AA slots and each slot's starting reserve and limits
//   V8   the learned store: round trip, skipped lines, duplicates, upserts, the 64-record cap, v0.23.0's records
//        (read, kept, taken over inside the new limits) and the flight budget another
//        engine left
//   V9   SessionWatch and learn: the rules and each outcome, with synthetic 1 s samples (the length gate, the
//        limits, the flight budget), and the learn line's text
//   V10  auto with other reserves (including none), with no OS budget, and with what other programs hold of the card
//   V11  a re-read merges onto that record and keeps a higher reserve, and a session's own saves count once; a tighten stops at the reserve that pins
//        the 4096 MB floor; fixed sizes under 4 GB lower shared memory to match; a different device id is not the
//        install key
//
//   game_vram_test.exe --dry-run            touches nothing
//   game_vram_test.exe --self-test [root]   root is the repository, for the V6 source pins (skipped without it)
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "adapter_memory.h"
#include "game_vram.h"
#include "game_vram_learn.h"

using namespace edvr::gamevram;

namespace {
unsigned g_checks = 0, g_failures = 0;

void check(bool ok, const char* label) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s\n", label);
    }
}

constexpr uint64_t kMiB = 1024ull * 1024ull;

Setting setting(Mode m, uint32_t mb = 0) {
    Setting s;
    s.mode = m;
    s.fixedMb = mb;
    return s;
}

bool parsedAs(const char* text, Mode mode, uint32_t mb, bool recognised) {
    const Setting s = parseSetting(text);
    return s.mode == mode && s.fixedMb == mb && s.recognised == recognised;
}

void parseCases() {
    check(parsedAs("off", Mode::Off, 0, true), "V1 'off' is Off, recognised");
    check(parsedAs(" OFF ", Mode::Off, 0, true), "V1 ' OFF ' is Off, recognised");
    check(parsedAs("", Mode::Off, 0, true), "V1 empty is Off, recognised");
    check(parsedAs("auto", Mode::Auto, 0, true), "V1 'auto' is Auto, recognised");
    check(parsedAs("Auto", Mode::Auto, 0, true), "V1 'Auto' is Auto, recognised");
    check(parsedAs("8192", Mode::Fixed, 8192, true), "V1 '8192' is Fixed 8192, recognised");
    check(parsedAs("256", Mode::Fixed, 256, true), "V1 '256' is Fixed 256, recognised");
    check(parsedAs("1048576", Mode::Fixed, 1048576, true), "V1 '1048576' is Fixed 1048576, recognised");
    check(parsedAs("255", Mode::Off, 0, false), "V1 '255' is refused");
    check(parsedAs("1048577", Mode::Off, 0, false), "V1 '1048577' is refused");
    check(parsedAs("8 GB", Mode::Off, 0, false), "V1 '8 GB' is refused");
    check(parsedAs("-1", Mode::Off, 0, false), "V1 '-1' is refused");
    check(parsedAs("on", Mode::Off, 0, false), "V1 'on' is refused");
    check(parsedAs("12.5", Mode::Off, 0, false), "V1 '12.5' is refused");
    check(parsedAs("99999999999999999999", Mode::Off, 0, false), "V1 an overlong number is refused, no overflow");
}

void offCases() {
    Inputs in;
    in.realDedicatedBytes = 12216 * kMiB;
    in.osBudgetBytes = 11350 * kMiB;
    in.reserveMb = 2560;
    const Result r = compute(setting(Mode::Off), in);
    check(!r.change, "V2 Off never changes");
    check(r.reportedBytes == in.realDedicatedBytes, "V2 Off reports the real size");
    check(std::strcmp(r.why, "off") == 0, "V2 Off says why 'off'");
}

void fixedCases() {
    Inputs in;
    in.realDedicatedBytes = 12216 * kMiB;
    Result r = compute(setting(Mode::Fixed, 8192), in);
    check(r.change && r.reportedBytes == 8192 * kMiB, "V3 Fixed 8192 on 12216 MB: change, reported 8192 MB");
    check(std::strcmp(r.why, "fixed") == 0, "V3 Fixed 8192 says 'fixed'");

    r = compute(setting(Mode::Fixed, 16384), in);
    check(!r.change && r.reportedBytes == in.realDedicatedBytes, "V3 Fixed 16384 on 12216 MB: no change");
    check(std::strcmp(r.why, "fixed, not below the card") == 0, "V3 Fixed above the card says 'fixed, not below the card'");

    r = compute(setting(Mode::Fixed, 1024), in);
    check(r.change && r.reportedBytes == 1024 * kMiB, "V3 Fixed 1024 is allowed below the auto floor: reported 1024 MB");
}

void autoCases() {
    Inputs in;
    in.realDedicatedBytes = 12216 * kMiB;
    in.osBudgetBytes = 11350 * kMiB;
    in.reserveMb = 2560;
    Result r = compute(setting(Mode::Auto), in);
    check(r.change && r.targetMb == 8278, "V4 Auto, reserve 2560: target 11350-2560-512 = 8278 MB");
    check(r.change && r.reportedBytes == 9738 * kMiB, "V4 Auto, reserve 2560: reported floor(8278/0.85) = 9738 MB");
    check(std::strcmp(r.why, "auto") == 0, "V4 Auto says 'auto'");

    in.reserveMb = 1024;
    r = compute(setting(Mode::Auto), in);
    check(r.change && r.targetMb == 9814, "V4 Auto, reserve 1024: target 11350-1024-512 = 9814 MB");
    check(r.change && r.reportedBytes == 11545 * kMiB, "V4 Auto, reserve 1024: reported floor(9814/0.85) = 11545 MB");

    // No OS budget: 95% of 12216 = 11605 MB stands in; reserve 1024, so target 11605-1024-512 = 10069 MB, and
    // floor(10069/0.85) = 11845 MB (10069/0.85 = 11845.9).
    in.osBudgetBytes = 0;
    r = compute(setting(Mode::Auto), in);
    check(r.change && r.targetMb == 10069, "V4 Auto, no OS budget: 95% of the card (11605) gives target 10069 MB");
    check(r.change && r.reportedBytes == 11845 * kMiB, "V4 Auto, no OS budget: reported floor(10069/0.85) = 11845 MB");
    check(std::strcmp(r.why, "auto, no OS budget: 95% of the card") == 0,
          "V4 Auto, no OS budget says 'auto, no OS budget: 95% of the card'");

    in.osBudgetBytes = 3000 * kMiB;
    in.reserveMb = 2560;
    r = compute(setting(Mode::Auto), in);
    check(r.targetMb == 0, "V4 Auto, tiny budget: target 0");
    check(r.change && r.reportedBytes == 4096 * kMiB, "V4 Auto, tiny budget: reported is the 4096 MB floor");

    in.realDedicatedBytes = 4000 * kMiB;
    in.osBudgetBytes = 3800 * kMiB;
    r = compute(setting(Mode::Auto), in);
    check(!r.change && r.reportedBytes == 4000 * kMiB, "V4 Auto, card 4000 MB: floor then min(real), no change");
    check(std::strcmp(r.why, "auto, the card already fits") == 0, "V4 Auto, card already fits says so");
}

void zeroCardCases() {
    Inputs in;
    in.realDedicatedBytes = 0;
    in.osBudgetBytes = 11350 * kMiB;
    Result r = compute(setting(Mode::Auto), in);
    check(!r.change && std::strcmp(r.why, "no card size") == 0, "V4 realDedicatedBytes 0: Auto says 'no card size', no change");
    r = compute(setting(Mode::Fixed, 8192), in);
    check(!r.change && std::strcmp(r.why, "no card size") == 0, "V4 realDedicatedBytes 0: Fixed says 'no card size', no change");
}

void formatCases() {
    Inputs in;
    in.realDedicatedBytes = 12216 * kMiB;
    in.osBudgetBytes = 0;
    Setting fixed = setting(Mode::Fixed, 8192);
    Result rf = compute(fixed, in);
    char buf[256];
    int n = formatCallLine(buf, sizeof buf, 3, 0x1A2B30, fixed, in, rf);
    const char* wantFixed = "game vram: call=3 caller=exe+0x1A2B30 real_mb=12216 reported_mb=8192 setting=8192 (fixed)";
    check(std::strcmp(buf, wantFixed) == 0, "V5 fixed call line has the exact text");
    check(n == static_cast<int>(std::strlen(wantFixed)), "V5 fixed call line returns its length");

    Inputs ina;
    ina.realDedicatedBytes = 12216 * kMiB;
    ina.osBudgetBytes = 11350 * kMiB;
    ina.reserveMb = 2560;
    Setting autoS = setting(Mode::Auto);
    Result ra = compute(autoS, ina);
    n = formatCallLine(buf, sizeof buf, 1, 0x2000, autoS, ina, ra);
    const char* wantAuto =
        "game vram: call=1 caller=exe+0x2000 real_mb=12216 reported_mb=9738 setting=auto budget_mb=11350 others_mb=unknown target_mb=8278"
        " reserve_mb=2560 planned_mb=11350 (auto)";
    check(std::strcmp(buf, wantAuto) == 0, "V5 auto call line has the exact text, budget, target and reserve included");

    // No OS budget: the line shows the 95% estimate as the budget, and the reserve used (1024).
    // 95% of 12118 = 11512 MB; target 11512-1024-512 = 9976 MB; floor(9976/0.85) = 11736 MB.
    Inputs inb;
    inb.realDedicatedBytes = 12118 * kMiB;
    inb.osBudgetBytes = 0;
    inb.reserveMb = 1024;
    Result rb = compute(autoS, inb);
    n = formatCallLine(buf, sizeof buf, 2, 0x3000, autoS, inb, rb);
    const char* wantNoBudget =
        "game vram: call=2 caller=exe+0x3000 real_mb=12118 reported_mb=11736 setting=auto budget_mb=11512 others_mb=unknown target_mb=9976"
        " reserve_mb=1024 planned_mb=11512 (auto, no OS budget: 95% of the card)";
    check(std::strcmp(buf, wantNoBudget) == 0, "V5 auto call line without an OS budget shows the 95% budget");

    char guarded[32];
    std::memset(guarded, 0x7F, sizeof guarded);
    n = formatCallLine(guarded, 16, 3, 0x1A2B30, fixed, in, rf);
    check(guarded[15] == '\0', "V5 a 16-byte buffer is NUL-terminated");
    check(n == 15, "V5 a 16-byte buffer returns 15 characters");
    bool overrun = false;
    for (size_t i = 16; i < sizeof guarded; ++i) overrun = overrun || guarded[i] != 0x7F;
    check(!overrun, "V5 a 16-byte buffer is not overrun");

    char word[16];
    check(std::strcmp(settingText(setting(Mode::Off), word, sizeof word), "off") == 0, "V5 settingText Off is 'off'");
    check(std::strcmp(settingText(setting(Mode::Auto), word, sizeof word), "auto") == 0, "V5 settingText Auto is 'auto'");
    check(std::strcmp(settingText(setting(Mode::Fixed, 8192), word, sizeof word), "8192") == 0,
          "V5 settingText Fixed is the MB number");
}

// --- V10: auto with another reserve, and with no OS budget -----------------------------------------------------

void autoReserveCases() {
    // Card 12118 MB, OS budget 11349 MB, reserve 3584, margin 512: target 11349-3584-512 = 7253 MB,
    // floor(7253/0.85) = 8532 MB (7253/0.85 = 8532.9).
    Inputs in;
    in.realDedicatedBytes = 12118 * kMiB;
    in.osBudgetBytes = 11349 * kMiB;
    in.reserveMb = 3584;
    Setting autoS = setting(Mode::Auto);
    Result r = compute(autoS, in);
    check(r.targetMb == 7253, "V10 auto with reserve 3584, margin 512: target 11349-3584-512 = 7253 MB");
    check(r.change && r.reportedBytes == 8532 * kMiB, "V10 auto with reserve 3584: reported floor(7253/0.85) = 8532 MB");

    // Reserve 2560: target 11349-2560-512 = 8277, floor(8277/0.85) = 9737.
    in.reserveMb = 2560;
    r = compute(autoS, in);
    check(r.targetMb == 8277 && r.change && r.reportedBytes == 9737 * kMiB,
          "V10 reserve 2560: target 8277, reported 9737 MB");

    // A flight budget of 10325 stands for the 11349: target 10325-2560-512 = 7253, as with reserve 3584.
    in.flightBudgetMb = 10325;
    r = compute(autoS, in);
    check(r.targetMb == 7253 && r.reportedBytes == 8532 * kMiB && r.budgetBytes == 11349 * kMiB,
          "V10 reserve 2560, flight budget 10325: target 7253, and the budget kept is the one read");
    in.flightBudgetMb = 0;
    check(plannedBudgetMb(11349, 0) == 11349 && plannedBudgetMb(11349, 9152) == 9152, "V10 the planned budget is the flight budget, when there is one");
    check(plannedBudgetMb(9200, 9152) == 9152 && plannedBudgetMb(8000, 9152) == 8000,
          "V10 a start that is already low is not lowered a second time");
    check(plannedBudgetMb(11349, 5000) == 8512, "V10 at most a quarter of the budget at start is given up: 8512 of 11349");

    // What other programs hold: the 2026-11-24 card, 12118 MB with 2747 in use before the game. The budget read is 11349, and
    // 9371 is left; flights had 8900-9600. Reserve 4096, the flight's own, aims at 4763 instead of 6741.
    check(startBudgetMb(11349, 12118, true, 2747) == 9371 && startBudgetMb(11349, 12118, false, 2747) == 11349 &&
              startBudgetMb(11349, 12118, true, 300) == 11349,
          "V10 the start budget is what other programs leave of the card, when that is less and known");
    check(startBudgetMb(11349, 12118, true, 9000) == 5674 && startBudgetMb(7123, 7891, true, 20000) == 3561,
          "V10 the start budget is never under half the OS budget");
    Inputs others = in;
    others.othersKnown = true;
    others.othersBytes = 2747 * kMiB;
    others.reserveMb = 4096;
    r = compute(autoS, others);
    check(r.targetMb == 4763 && r.budgetBytes == 11349 * kMiB && r.startBytes == 9371 * kMiB,
          "V10 other programs holding 2747 MB: target 9371-4096-512 = 4763");
    char call[320];
    formatCallLine(call, sizeof call, 1, 0x2000, autoS, others, r);
    check(std::strstr(call, " budget_mb=11349 others_mb=2747 target_mb=4763 reserve_mb=4096 planned_mb=9371 (auto)") != nullptr,
          "V10 the call line names what other programs held and the planned budget");
    uint32_t high = 1, low = 1;
    check(edvr::parseAdapterInstance(L"luid_0x00000000_0x00017be5_phys_0", high, low) && high == 0 && low == 0x17BE5,
          "V10 a GPU Adapter Memory instance name gives its LUID");
    check(!edvr::parseAdapterInstance(nullptr, high, low) && !edvr::parseAdapterInstance(L"", high, low) &&
              !edvr::parseAdapterInstance(L"luid_0x0_0x1", high, low) && !edvr::parseAdapterInstance(L"pid_4_luid_0x0_0x1_phys_0", high, low),
          "V10 any other name is refused");
    uint32_t pid = 1;
    check(edvr::parseProcessInstance(L"pid_10072_luid_0x00000000_0x0001771b_phys_0", pid, high, low) && pid == 10072 &&
              high == 0 && low == 0x1771B,
          "V10 a GPU Process Memory instance name gives its process and LUID");
    check(!edvr::parseProcessInstance(nullptr, pid, high, low) && !edvr::parseProcessInstance(L"pid__luid_0x0_0x1_phys_0", pid, high, low) &&
              !edvr::parseProcessInstance(L"pid_4luid_0x0_0x1_phys_0", pid, high, low) &&
              !edvr::parseProcessInstance(L"luid_0x0_0x1_phys_0", pid, high, low) &&
              !edvr::parseProcessInstance(L"pid_4_luid_0x0_0x1", pid, high, low),
          "V10 ...and any other name is refused");
    const std::vector<edvr::AdapterUsageRow> rows{{0, 0x10, 5000 * kMiB, 0}, {0, 0x1771B, 3103 * kMiB, 40 * kMiB}};
    edvr::AdapterUsageRow row;
    check(edvr::findAdapterRow(rows, 0, 0x1771B, &row) && row.allBytes == 3103 * kMiB && row.ownBytes == 40 * kMiB &&
              !edvr::findAdapterRow(rows, 1, 0x1771B, &row),
          "V10 a reading's row is found by the card's whole LUID");

    // Reserve 0 is no reserve, not a fallback: target 11349-512 = 10837, 10837/0.85 = 12749 is over the card, so the
    // card's 12118 MB stands and nothing changes.
    in.reserveMb = 0;
    r = compute(autoS, in);
    check(r.targetMb == 10837 && !r.change && r.reportedBytes == 12118 * kMiB,
          "V10 reserve 0: target 10837, over the card, no change");

    // No OS budget: 95% of 12118 = 11512 MB; target 11512-3584-512 = 7416 MB; floor(7416/0.85) = 8724 MB.
    Inputs nb;
    nb.realDedicatedBytes = 12118 * kMiB;
    nb.osBudgetBytes = 0;
    nb.reserveMb = 3584;
    r = compute(autoS, nb);
    check(r.budgetBytes == 11512 * kMiB, "V10 no OS budget: the budget used is 95% of 12118 = 11512 MB");
    check(r.targetMb == 7416, "V10 no OS budget, reserve 3584: target 11512-3584-512 = 7416 MB");
    check(r.change && r.reportedBytes == 8724 * kMiB, "V10 no OS budget, reserve 3584: reported floor(7416/0.85) = 8724 MB");
    check(std::strcmp(r.why, "auto, no OS budget: 95% of the card") == 0,
          "V10 no OS budget, change: why 'auto, no OS budget: 95% of the card'");

    // The Wine guard's test: a budget at or above the card is not a budget; one below it, or an unknown one, is left alone.
    check(budgetIsWholeCard(12288 * kMiB, 12118 * kMiB), "V10 whole-card budget: the heap sum above GetDesc's size is no budget");
    check(budgetIsWholeCard(12118 * kMiB, 12118 * kMiB), "V10 whole-card budget: a budget equal to the card is no budget");
    check(!budgetIsWholeCard(11349 * kMiB, 12118 * kMiB), "V10 whole-card budget: a budget below the card is a real one");
    check(!budgetIsWholeCard(0, 12118 * kMiB), "V10 whole-card budget: an unknown budget (0) is not flagged");
    check(!budgetIsWholeCard(12288 * kMiB, 0), "V10 whole-card budget: an unknown card size (0) is not flagged");
}

// --- V7: the setup key and the classes -------------------------------------------------------------------------

void keyCases() {
    // 0x0020000F17C40000: 0x0020 = 32, 0x000F = 15, 0x17C4 = 6084, 0x0000 = 0.
    const uint64_t drv = 0x0020000F17C40000ull;
    check(makeKey(0x10DE, 0x2684, drv, false, false, AaSlot::Off) ==
              "v=10DE d=2684 drv=32.15.6084.0 layer=windows prof=flat aa=off",
          "V7 key for a GeForce on Windows, flat, no AA");
    check(makeKey(0x10DE, 0x2684, drv, true, false, AaSlot::Off) ==
              "v=10DE d=2684 drv=32.15.6084.0 layer=wine prof=flat aa=off",
          "V7 key under Wine says layer=wine");
    check(makeKey(0x10DE, 0x2684, drv, false, true, AaSlot::Off) ==
              "v=10DE d=2684 drv=32.15.6084.0 layer=windows prof=vr aa=off",
          "V7 key for the VR profile says prof=vr");
    check(makeKey(0x10DE, 0x2684, 0, false, false, AaSlot::Xess) ==
              "v=10DE d=2684 drv=unknown layer=windows prof=flat aa=xess",
          "V7 driver version 0 is drv=unknown, and the key ends in the slot's name");

    const struct { const char* mode; AaSlot slot; const char* name; } kModes[] = {
        {"", AaSlot::Off, "off"},           {"off", AaSlot::Off, "off"},       {"nonsense", AaSlot::Off, "off"},
        {"on", AaSlot::Taa, "on"},          {"ON", AaSlot::Taa, "on"},        {"dlss", AaSlot::Dlss, "dlss"},
        {"DLAA", AaSlot::Dlaa, "dlaa"},     {"fsr", AaSlot::Fsr3, "fsr"},     {"fsr4", AaSlot::Fsr4, "fsr4"},
        {"dittosr", AaSlot::DittoSr, "dittosr"}, {"XeSS", AaSlot::Xess, "xess"},
    };
    for (const auto& m : kModes) {
        const std::string label = std::string("V7 fix.temporal_aa '") + m.mode + "' is slot " + m.name;
        check(aaSlotOf(m.mode) == m.slot && std::strcmp(aaSlotWord(m.slot), m.name) == 0, label.c_str());
    }
    check(makeKey(1, 2, 3, false, false, AaSlot::Taa) == "v=0001 d=0002 drv=0.0.0.3 layer=windows prof=flat aa=on" &&
              makeKey(1, 2, 3, false, false, AaSlot::Fsr3) == "v=0001 d=0002 drv=0.0.0.3 layer=windows prof=flat aa=fsr",
          "V7 TAA and FSR 3.1 keep v0.23.0's key words, so its records match");
    for (size_t i = 0; i < kAaSlots; ++i) {
        for (size_t j = i + 1; j < kAaSlots; ++j)
            check(makeKey(1, 2, 3, false, false, static_cast<AaSlot>(i)) != makeKey(1, 2, 3, false, false, static_cast<AaSlot>(j)),
                  "V7 no two slots share a key");
    }

    for (int vr = 0; vr < 2; ++vr) {
        for (size_t i = 0; i < kAaSlots; ++i) {
            const AaSlot s = static_cast<AaSlot>(i);
            const ReserveBounds& b = reserveBounds(vr != 0, s);
            const std::string at = std::string("V7 ") + (vr ? "vr " : "flat ") + aaSlotWord(s);
            check(b.minMb < b.startMb && b.startMb < b.maxMb, (at + ": the start leaves room both ways").c_str());
            check(b.minMb >= kReserveFloorMb && b.maxMb <= kReserveCeilMb, (at + ": a stored reserve in its limits parses").c_str());
            check(b.minMb % kReserveStepMb == 0 && b.startMb % kReserveStepMb == 0 && b.maxMb % kReserveStepMb == 0,
                  (at + ": whole steps").c_str());
        }
        check(reserveBounds(vr != 0, AaSlot::Off).maxMb < 2048 && reserveBounds(vr != 0, AaSlot::Taa).maxMb <= 2560,
              "V7 Off can never hold back two gigabytes, nor TAA more than two and a half");
    }
    check(reserveBounds(false, AaSlot::Off).startMb == 1152 && reserveBounds(true, AaSlot::Off).startMb == 1024 &&
              reserveBounds(false, AaSlot::Taa).startMb == 2112,
          "V7 Off starts at 1152 MB flat, 1024 MB in VR, TAA at 2112 flat");
    // The highest readings, flat: parked on 2026-11-25, and for XeSS while Elite streamed. The start and the margin cover them.
    const struct { AaSlot slot; uint32_t highest; } kSeen[] = {
        {AaSlot::Off, 1606}, {AaSlot::Taa, 2590}, {AaSlot::Fsr3, 2479}, {AaSlot::DittoSr, 2757}, {AaSlot::Xess, 3644},
    };
    for (const auto& seen : kSeen) {
        const std::string label = std::string("V7 flat ") + aaSlotWord(seen.slot) + ": start and margin cover the highest share seen";
        check(reserveBounds(false, seen.slot).startMb + kMarginMb >= seen.highest, label.c_str());
    }
    check(reserveBounds(false, AaSlot::Xess).startMb == 3584, "V7 XeSS starts at 3584 MB flat");
    const ReserveBounds& off = reserveBounds(true, AaSlot::Off);
    check(clampReserve(off, 4000) == 1536 && clampReserve(off, 64) == 256 && clampReserve(off, 900) == 900,
          "V7 a stored reserve is used inside its slot's limits");
}

// --- V8: the learned store -------------------------------------------------------------------------------------

Record rec(const char* key, uint32_t reserve, uint32_t sessions, uint32_t tightened, uint32_t relaxed, uint32_t flight = 0) {
    Record r;
    r.key = key;
    r.reserveMb = reserve;
    r.flightBudgetMb = flight;
    r.sessions = sessions;
    r.tightened = tightened;
    r.relaxed = relaxed;
    return r;
}

void storeCases() {
    std::vector<Record> recs;
    recs.push_back(rec("keyA", 1536, 3, 1, 0));
    recs.push_back(rec("keyB", 3584, 5, 2, 1, 2112));
    recs.push_back(rec("keyC", 768, 0, 0, 0));
    const std::string text = serializeStore(recs);
    const std::string want =
        "# EDVR: what advanced.game_vram = auto learned per setup. Safe to delete.\n"
        "1536\t0\t3\t1\t0\tkeyA\n"
        "3584\t2112\t5\t2\t1\tkeyB\n"
        "768\t0\t0\t0\t0\tkeyC\n";
    check(text == want, "V8 serializeStore writes the header and three exact lines");

    const std::vector<Record> back = parseStore(text);
    check(back.size() == 3, "V8 round trip keeps three records");
    check(serializeStore(back) == text, "V8 round trip serializes to the same text");
    const Record* b = findRecord(back, "keyB");
    check(b != nullptr && b->reserveMb == 3584 && b->sessions == 5 && b->tightened == 2 && b->relaxed == 1 &&
              b->flightBudgetMb == 2112,
          "V8 round trip keeps keyB's five numbers");
    check(findRecord(back, "missing") == nullptr, "V8 findRecord of an unknown key is null");

    const std::string messy =
        "# a comment\n"
        "\n"
        "1536\t0\t1\t0\t0\tkeyD\r\n"   // CRLF: valid
        "2841\t9\t1\t3\tkeyE\n"        // v0.23.0's five fields: an old record
        "1536\t1\t0\tkeyJ\n"            // four fields: skipped
        "5561\t0\t1\t0\t0\tkeyK\n"      // six fields over the new ceiling: skipped
        "20000\t1\t1\t0\tkeyL\n"        // five fields over the old ceiling: skipped
        "100\t0\t1\t0\t0\tkeyF\n"      // reserve below the floor: skipped
        "abc\t0\t1\t0\t0\tkeyG\n"      // not a number: skipped
        "1536\t2000000\t1\t0\t0\tkeyI\n"  // flight budget over the ceiling: skipped
        "1536\t0\t1\t0\t0\t\n"         // empty key: skipped
        "2048\t0\t1\t0\t0\tkeyH\n"
        "3072\t0\t2\t0\t0\tkeyH\n";    // duplicate: the later wins
    const std::vector<Record> m = parseStore(messy);
    check(m.size() == 3, "V8 messy store keeps three records");
    const Record* d = findRecord(m, "keyD");
    check(d != nullptr && d->reserveMb == 1536 && d->sessions == 1, "V8 a CRLF line is parsed");
    const Record* h = findRecord(m, "keyH");
    check(h != nullptr && h->reserveMb == 3072 && h->sessions == 2, "V8 a duplicate key: the later line wins");
    const Record* e = findRecord(m, "keyE");
    check(e != nullptr && e->old && e->reserveMb == 2841 && e->sessions == 9 && e->tightened == 1 && e->relaxed == 3,
          "V8 a v0.23.0 line (five fields) is read as an old record");
    check(findRecord(m, "keyJ") == nullptr && findRecord(m, "keyK") == nullptr && findRecord(m, "keyL") == nullptr,
          "V8 four fields, and a reserve over its line's ceiling, are skipped");
    check(serializeStore(m).find("2841\t9\t1\t3\tkeyE\n") != std::string::npos, "V8 an old record is written back as it was");

    // The records the reports came with, and some beside them. facts: what other programs left at start.
    const ReserveBounds vrOff = reserveBounds(true, AaSlot::Off);
    const ReserveBounds flatXess = reserveBounds(false, AaSlot::Xess);
    const std::vector<Record> olds = parseStore("2841\t9\t1\t3\toff\n5561\t24\t12\t1\txess\n3072\t2\t0\t0\tunmoved\n1200\t4\t0\t2\tlow\n");
    StartFacts arc;
    arc.osBudgetMb = 11349;
    arc.startMb = 9371;
    StartFacts arcUnknown;
    arcUnknown.osBudgetMb = arcUnknown.startMb = 11349;
    Record got;
    check(adoptRecord(olds, "xess", flatXess, arc, got) && got.reserveMb == 3584 && got.flightBudgetMb == 0 && got.sessions == 24 && !got.old,
          "V8 an old XeSS reserve of 5561 less the 1978 other programs hold is 3583, and the slot's own start of 3584 stands");
    check(adoptRecord(olds, "xess", flatXess, arcUnknown, got) && got.reserveMb == 4608 && got.flightBudgetMb == 11349 - 953 &&
              plannedBudgetMb(11349, got.flightBudgetMb) - got.reserveMb - 512 == 5276,
          "V8 ...and with the others unknown, what is over the limit becomes a flight budget: 5276 again");
    check(adoptRecord(olds, "off", vrOff, StartFacts{7123, 7123}, got) && got.reserveMb == 1024 && got.flightBudgetMb == 0 && got.sessions == 9,
          "V8 an old VR Off reserve of 2841, under that version's 3072, shows no need: the slot's start stands");
    check(adoptRecord(olds, "low", vrOff, StartFacts{7123, 7123}, got) && got.reserveMb == 1024, "V8 nor does one that was only ever lowered");
    check(adoptRecord(olds, "low", vrOff, StartFacts{7123, 5000}, got) && got.reserveMb == 1024, "V8 ...and a fall does not take it under the slot's start");
    const std::vector<Record> raised1 = parseStore("3400\t9\t2\t0\tk\n");
    check(adoptRecord(raised1, "k", vrOff, StartFacts{7123, 7123}, got) && got.reserveMb == 1536 && got.flightBudgetMb == 7123 - (3400 - 1536),
          "V8 one that was raised to above the 3072 does: the limit, and the rest as a flight budget");
    const std::vector<Record> small1 = parseStore("700\t4\t0\t2\tk\n");
    check(adoptRecord(small1, "k", vrOff, StartFacts{7123, 5000}, got) && got.reserveMb == 700, "V8 an old reserve under the slot's start is itself the most the share can be");

    // Flight 20261125_013401: an older file's 3963, raised once with an idle desktop, and 2189 MB of other programs today.
    // Taking the fall off it left 1774 and spilled by a gigabyte.
    const std::vector<Record> idle = parseStore("3963\t2\t1\t0\tk\n");
    check(adoptRecord(idle, "k", flatXess, StartFacts{11349, 9160}, got) && got.reserveMb == 3584,
          "V8 an old 3963 learned with an idle desktop is not cut by today's fall: the start stands");
    Inputs flown;
    flown.realDedicatedBytes = 12118 * kMiB;
    flown.osBudgetBytes = 11349 * kMiB;
    flown.othersKnown = true;
    flown.othersBytes = 2958 * kMiB;
    flown.reserveMb = got.reserveMb;
    const Result sized = compute(setting(Mode::Auto), flown);
    check(sized.targetMb == 5064 && sized.targetMb + 3494 < 9140, "V8 ...which with that flight's highest share stays under its lowest budget");
    check(adoptRecord(olds, "unmoved", vrOff, arc, got) && got.reserveMb == vrOff.startMb,
          "V8 an old reserve that never moved is v0.23.0's default: the new start stands for it");
    check(!adoptRecord(olds, "missing", vrOff, arc, got), "V8 no record: nothing to take over");
    std::vector<Record> live2;
    live2.push_back(rec("k", 4096, 25, 12, 1, 9152));
    check(adoptRecord(live2, "k", flatXess, arc, got) && got.reserveMb == 4096 && got.flightBudgetMb == 9152,
          "V8 a new record is taken as it is: nothing comes off it twice");
    // The store of 2026-11-25 after one session each: Off and TAA held their starts, and learned only the flight budget.
    const ReserveBounds flatOff = reserveBounds(false, AaSlot::Off);
    const std::vector<Record> heldStarts = parseStore("1472\t9088\t1\t0\t0\tk\n");
    check(adoptRecord(heldStarts, "k", flatOff, arc, got) && got.reserveMb == flatOff.startMb && got.flightBudgetMb == 9088 &&
              got.sessions == 1 && !got.old,
          "V8 a new record that was never raised or lowered takes the current start, and keeps its flight budget");
    const std::vector<Record> heldLowered = parseStore("900\t0\t3\t0\t1\tk\n");
    check(adoptRecord(heldLowered, "k", flatOff, arc, got) && got.reserveMb == 900, "V8 ...and one that was lowered keeps its own");
    check(seedFlightBudgetMb(parseStore("2112\t1\t1\t0\tp aa=xess\n"), "p aa=off") == 0, "V8 an old record gives no flight budget to another engine");

    // Learning goes on from it: the first save replaces the old line and is not held up by its larger reserve.
    std::vector<Record> live = olds;
    SaveBasis basis;
    adoptRecord(live, "off", vrOff, StartFacts{7123, 7123}, got);
    LearnResult relax;
    relax.reserveMb = 1024;
    relax.flightBudgetMb = 6528;
    relax.outcome = LearnResult::Outcome::Relaxed;
    upsertRecord(live, mergeLearned("off", got.reserveMb, 0, relax, basis.basisFor(&got)));
    const Record* now = findRecord(live, "off");
    check(now != nullptr && !now->old && now->reserveMb == 1024 && now->flightBudgetMb == 6528 && now->sessions == 10 && now->relaxed == 4,
          "V8 a session that started from an old record saves over it and keeps counting");
    check(findRecord(live, "xess")->old && serializeStore(live).find("5561\t24\t12\t1\txess\n") != std::string::npos,
          "V8 ...and leaves the other setups' old records alone");
    check(findRecord(m, "keyI") == nullptr, "V8 a flight budget of 2000000 is skipped");
    check(findRecord(m, "keyF") == nullptr, "V8 a reserve of 100 is skipped");
    check(findRecord(m, "keyG") == nullptr, "V8 a non-number line is skipped");

    std::vector<Record> seeds;
    seeds.push_back(rec("v=8086 d=E20B drv=1 layer=windows prof=flat aa=xess", 2560, 1, 0, 0, 9152));
    seeds.push_back(rec("v=8086 d=E20B drv=1 layer=windows prof=flat aa=off", 768, 1, 0, 0, 9216));
    seeds.push_back(rec("v=8086 d=E20B drv=1 layer=windows prof=vr aa=off", 768, 1, 0, 0, 9280));
    seeds.push_back(rec("v=8086 d=E20B drv=1 layer=windows prof=flat aa=dlss", 768, 1, 0, 0, 0));
    check(seedFlightBudgetMb(seeds, "v=8086 d=E20B drv=1 layer=windows prof=flat aa=on") == 9216,
          "V8 a new engine starts from the newest flight budget of the same machine and profile");
    check(seedFlightBudgetMb(seeds, "v=8086 d=E20B drv=2 layer=windows prof=flat aa=on") == 0 && seedFlightBudgetMb(seeds, "odd") == 0,
          "V8 another driver's flight budget is not taken");

    std::vector<Record> u = back;
    Record b2 = *b;
    b2.reserveMb = 4096;
    upsertRecord(u, b2);
    check(u.size() == 3 && u[0].key == "keyA" && u[1].key == "keyC" && u[2].key == "keyB" && u[2].reserveMb == 4096,
          "V8 upsert replaces keyB and moves it to the end");
    upsertRecord(u, rec("keyNew", 1024, 0, 0, 0));
    check(u.size() == 4 && u[3].key == "keyNew", "V8 upsert of a new key appends it");

    std::vector<Record> many;
    for (int i = 0; i < 65; ++i) upsertRecord(many, rec(("k" + std::to_string(i)).c_str(), 1024, 0, 0, 0));
    check(many.size() == 64, "V8 65 distinct upserts keep 64 records");
    check(findRecord(many, "k0") == nullptr, "V8 the first (oldest) record is dropped");
    check(many.front().key == "k1" && many.back().key == "k64", "V8 the kept records are k1 to k64, in order");
}

// --- V9: SessionWatch and learn --------------------------------------------------------------------------------

// n one-second samples.
template <typename Usage, typename Budget>
SessionWatch session(uint32_t n, uint64_t planMb, Usage usageMb, Budget budgetMb) {
    SessionWatch w;
    for (uint32_t i = 0; i < n; ++i)
        w.sample(i * 1000ull, static_cast<uint64_t>(usageMb(i)) * kMiB, static_cast<uint64_t>(budgetMb(i)) * kMiB, planMb * kMiB);
    return w;
}

void learnCases() {
    using Outcome = LearnResult::Outcome;
    const ReserveBounds off = reserveBounds(true, AaSlot::Off);   // 1024, 256 to 1536
    const ReserveBounds xess = reserveBounds(false, AaSlot::Xess);
    const auto budget7123 = [](uint32_t) { return 7123; };

    // Under five minutes, without a spill: too short to judge.
    const SessionWatch a = session(299, 7123, [](uint32_t) { return 4000; }, budget7123);
    const LearnResult ra = learn(1024, 0, 7123, off, a);
    check(ra.outcome == Outcome::TooShort && ra.reserveMb == 1024 && std::strcmp(ra.why, "held: session too short") == 0,
          "V9 299 samples: held, too short");
    check(!session(149, 7123, [](uint32_t) { return 9000; }, budget7123).held() &&
              session(150, 7123, [](uint32_t) { return 9000; }, budget7123).held(),
          "V9 a level can be held once a whole window was seen");

    // The gate, from both sides: a spike of 119 s is not held, one of 120 s is, wherever it comes.
    const auto spike = [&](uint32_t at, uint32_t seconds) {
        return session(1500, 7123, [at, seconds](uint32_t i) { return i >= at && i < at + seconds ? 6900 : 5000; }, budget7123);
    };
    check(spike(84, 119).heldUsageMb() == 5000 && spike(84, 120).heldUsageMb() == 6900, "V9 a load of 119 s is gated, 120 s is held");
    check(spike(900, 119).heldUsageMb() == 5000 && spike(900, 120).heldUsageMb() == 6900, "V9 the same for one in mid-flight");
    // Trims: Windows takes the resident memory under Elite's own total for some seconds, and it comes back.
    const SessionWatch trims = session(1000, 9096, [](uint32_t i) { return i % 75 < 10 ? 3634 : 7000; }, [](uint32_t) { return 11349; });
    check(trims.heldUsageMb() == 7000 && learn(4096, 0, 9096, xess, trims).outcome == Outcome::Relaxed,
          "V9 trims do not hide the level, and the session teaches");
    // A pause of the sampling starts a window over.
    SessionWatch gap;
    for (uint32_t i = 0; i < 100; ++i) gap.sample(i * 1000ull, 6900 * kMiB, 7123 * kMiB, 7123 * kMiB);
    for (uint32_t i = 0; i < 100; ++i) gap.sample(200000 + i * 1000ull, 6900 * kMiB, 7123 * kMiB, 7123 * kMiB);
    check(!gap.held(), "V9 samples on both sides of a gap are not one window");

    // Loading is over the budget for 100 s, flight holds 5000 MB of 7123: no spill, nothing is raised, and the session
    // relaxes by the cap.
    const SessionWatch b = session(1500, 7123, [](uint32_t i) { return i >= 80 && i < 180 ? 7600 : 5000; }, budget7123);
    check(b.crossings() == 100 && b.spills() == 0 && b.minHeadroomMb() == -477 && b.heldHeadroomMb() == 2123 && b.plateaued(),
          "V9 a 100 s load over the budget is logged but is no spill");
    const LearnResult rb = learn(1536, 0, 7123, off, b);   // half of 2123 - 512 is 805, in steps 768
    check(rb.outcome == Outcome::Relaxed && rb.reserveMb == 768 && std::strcmp(rb.why, "relaxed: room to spare") == 0,
          "V9 a loading spike does not raise the reserve: relaxed by half the spare room, 1536 -> 768");
    check(learn(640, 0, 7123, off, b).reserveMb == 256, "V9 relaxing stops at the slot's minimum");
    const LearnResult atMin = learn(256, 0, 7123, off, b);
    check(atMin.reserveMb == 256 && std::strcmp(atMin.why, "held: at the minimum") == 0, "V9 at the minimum: held");

    // Bursts of 100 s over the budget, every four minutes, hold no window: no spill, and the held level has room to spare. Over
    // it for two minutes in all, the session gives nothing back.
    const SessionWatch bursts = session(1500, 7123, [](uint32_t i) { return i % 240 < 100 ? 8100 : 5000; }, budget7123);
    const LearnResult ru = learn(1024, 0, 7123, off, bursts);
    check(bursts.spills() == 0 && bursts.crossings() == 660 && bursts.plateaued() && bursts.heldHeadroomMb() == 2123,
          "V9 repeated bursts over the budget are not held");
    check(ru.reserveMb == 1024 && ru.outcome == Outcome::Held && std::strcmp(ru.why, "held: over the budget for two minutes in all") == 0,
          "V9 ...but do not lower the reserve");

    // Flight 20261124_222529, reserve 4096 on a planned 8640: Elite at its target and 2746 MB beside it, 6778 held. Flown again
    // and again with that share, half the spare room each time closes in and stops inside the band.
    const auto steady = [&](uint32_t reserve) {
        const uint32_t use = 8640 - reserve - 512 + 2746;
        return learn(reserve, 0, 8640, xess, session(1500, 8640, [use](uint32_t) { return static_cast<int>(use); }, [](uint32_t) { return 11349; }));
    };
    check(steady(4096).reserveMb == 3456 && steady(3456).reserveMb == 3136 && steady(3136).reserveMb == 2944,
          "V9 half the spare room: 4096 -> 3456 -> 3136 -> 2944");
    check(steady(2944).reserveMb == 2944 && std::strcmp(steady(2944).why, "held: headroom within the margin") == 0,
          "V9 ...and there it stops, 198 MB over the margin");
    for (uint32_t r = 2944; r <= 4608; r += 64) {
        const LearnResult one = steady(r);
        check(one.reserveMb + 512 - 2746 >= 512, "V9 no step of it ever takes the room under the margin");
    }

    // 300 MB of room held for four minutes: 512 - 300 = 212, in steps 256.
    const SessionWatch d = session(1500, 7123, [](uint32_t i) { return i >= 300 && i < 540 ? 6823 : 5000; }, budget7123);
    const LearnResult rd = learn(1024, 0, 7123, off, d);
    check(rd.outcome == Outcome::Tightened && rd.reserveMb == 1280 &&
              std::strcmp(rd.why, "tightened: less room than the margin") == 0,
          "V9 300 MB of room held: tightened 1024 -> 1280");
    check(learn(1472, 0, 7123, off, d).reserveMb == 1536, "V9 tightening stops at the slot's maximum");
    const LearnResult atMax = learn(1536, 0, 7123, off, d);
    check(atMax.reserveMb == 1536 && std::strcmp(atMax.why, "held: at this engine's limit") == 0, "V9 at the maximum: held");
    uint32_t walked = 1024;
    for (int i = 0; i < 40; ++i) walked = learn(walked, 0, 7123, off, d).reserveMb;
    check(walked == 1536, "V9 forty such sessions end at the slot's maximum");

    const ReserveBounds wide{1024, 256, 4096, 4096};
    const SessionWatch deep = session(1500, 7123, [](uint32_t) { return 7700; }, [](uint32_t) { return 8000; });
    check(learn(1024, 0, 7123, wide, deep).reserveMb == 1536, "V9 without a spill one session raises the reserve by 512 MB at most");
    const SessionWatch over = session(1500, 7123, [](uint32_t) { return 7700; }, budget7123);   // 512 + 577 = 1089, in steps 1152
    check(over.spills() == 1 && learn(1024, 0, 7123, wide, over).reserveMb == 2176, "V9 with a spill it is raised by all it was short of");

    // A spill in a session of three minutes teaches; one second less of it does not.
    const auto tail = [&](uint32_t seconds) {
        return session(180, 7123, [seconds](uint32_t i) { return i >= 180 - seconds ? 7200 : 6000; }, budget7123);
    };
    const LearnResult spilt = learn(1024, 0, 7123, off, tail(120));   // 512 + 77 = 589, in steps 640, to the slot's maximum
    check(tail(120).spills() == 1 && spilt.outcome == Outcome::Tightened && spilt.reserveMb == 1536 &&
              std::strcmp(spilt.why, "tightened: over the budget") == 0,
          "V9 120 s over the budget: tightened at once");
    check(tail(119).spills() == 0 && learn(1024, 0, 7123, off, tail(119)).outcome == Outcome::TooShort, "V9 119 s over the budget are not a spill");
    const SessionWatch twice = session(1500, 7123, [](uint32_t i) { return (i >= 300 && i < 600) || (i >= 900 && i < 1200) ? 7200 : 6000; }, budget7123);
    check(twice.spills() == 2, "V9 two spills are counted as two, each once");

    // Still rising at the end.
    const SessionWatch rising = session(1500, 7123, [](uint32_t i) { return 2000 + i; }, budget7123);
    const LearnResult rr = learn(1024, 0, 7123, off, rising);
    check(!rising.plateaued() && rr.reserveMb == 1024 && std::strcmp(rr.why, "held: usage still rising or session under 15 min") == 0,
          "V9 usage that still rises: held");
    const SessionWatch brief = session(899, 7123, [](uint32_t) { return 5000; }, budget7123);
    check(!brief.plateaued() && session(900, 7123, [](uint32_t) { return 5000; }, budget7123).plateaued(), "V9 15 minutes can have levelled off");

    // Headroom 600: between the margin and the band above it.
    const SessionWatch e = session(1500, 7123, [](uint32_t) { return 6523; }, budget7123);
    const LearnResult re = learn(1024, 0, 7123, off, e);
    check(re.reserveMb == 1024 && re.outcome == Outcome::Held && std::strcmp(re.why, "held: headroom within the margin") == 0,
          "V9 headroom 600 with margin 512: held within the margin");

    // Already pinned at the 4096 MB floor for the planned budget: a tighter reserve would not change what Elite is told.
    const SessionWatch small = session(1500, 4200, [](uint32_t) { return 4100; }, [](uint32_t) { return 4200; });
    const LearnResult rs = learn(1024, 0, 4200, off, small);
    check(rs.reserveMb == 1024 && std::strcmp(rs.why, "held: reported size already at the floor") == 0, "V9 pinned at the floor: held");

    // A tighten stops at the reserve that pins the floor: 4800 - 806 - 512 = 3482, the most that still reports 4096. The slot
    // would allow 1536, and the step 512.
    const SessionWatch tight = session(1500, 4800, [](uint32_t) { return 4900; }, [](uint32_t) { return 8000; });
    check(reservePinningFloorMb(4800 * kMiB, kMarginMb, off.maxMb) == 806 && learn(700, 0, 4800, off, tight).reserveMb == 806,
          "V9 a tighten stops at the reserve that pins the 4096 MB floor");

    // The flight budget. Flights 2026-11-23: 11349 MB at start, 9200 MB while the process holds more than 7 GB. What it fell
    // to is learned, in whole steps 9152, and leaves the reserve alone.
    const auto contended = [](uint32_t i) { return i >= 300 && i < 700 ? 9200 : 11349; };
    const SessionWatch f = session(1500, 11349, [](uint32_t) { return 8700; }, contended);
    const LearnResult rf = learn(3584, 0, 11349, xess, f);
    check(f.heldBudgetMb() == 9200 && rf.flightBudgetMb == 9152, "V9 what the budget falls to in flight is learned");
    check(rf.outcome != Outcome::Tightened, "V9 ...and is not charged to the reserve");
    check(learnFlightBudget(10000, 11349, f) == 9152, "V9 a deeper fall than the one in use is taken at once");
    check(learnFlightBudget(0, 11349, session(1500, 11349, [](uint32_t) { return 8700; },
                                              [](uint32_t i) { return i >= 300 && i < 419 ? 9200 : 11349; })) == 0,
          "V9 a fall of 119 s is gated like any other level");
    const SessionWatch hog = session(1500, 11349, [](uint32_t) { return 4000; }, [](uint32_t) { return 5000; });
    check(learnFlightBudget(0, 11349, hog) == 8512, "V9 a fall counts down to three quarters of the budget, 8512 of 11349");
    const SessionWatch jitter = session(1500, 11349, [](uint32_t) { return 4000; }, [](uint32_t) { return 11320; });
    check(learnFlightBudget(0, 11349, jitter) == 0, "V9 a fall smaller than a step is no fall");

    // Rising again: 512 MB a session, not past what the session saw (11349, in whole steps 11328).
    const SessionWatch quiet = session(1500, 9152, [](uint32_t) { return 4000; }, [](uint32_t) { return 11349; });
    check(learnFlightBudget(9152, 11349, quiet) == 9664 && learnFlightBudget(11200, 11349, quiet) == 11328,
          "V9 a levelled session that never met the fall raises it by 512 MB, at most to what it saw");
    check(learnFlightBudget(5000, 11349, quiet) == 9024, "V9 ...from the three quarters that were in use");
    check(learnFlightBudget(9152, 11349, session(899, 9152, [](uint32_t) { return 4000; }, [](uint32_t) { return 11349; })) == 9152,
          "V9 a shorter session does not");

    // Another program already holds the card at start: 9200 from the first sample on. That is no fall, so nothing is learned
    // and nothing is lowered twice.
    const SessionWatch lowStart = session(1500, 9152, [](uint32_t) { return 4000; }, [](uint32_t) { return 9200; });
    check(learnFlightBudget(0, 9200, lowStart) == 0 && learnFlightBudget(9152, 9200, lowStart) == 9152 &&
              learnFlightBudget(10000, 9200, lowStart) == 10000,
          "V9 a start that is already low teaches no fall and does not move the figure");

    // A spill because the budget fell: the flight budget answers, the reserve does not move.
    const SessionWatch spill = session(1500, 11349, [](uint32_t) { return 9100; },
                                       [](uint32_t i) { return i >= 600 && i < 800 ? 9000 : 11349; });
    const LearnResult rp = learn(3584, 0, 11349, xess, spill);
    check(spill.spills() == 1 && rp.flightBudgetMb == 8960 && rp.reserveMb == 3584 && rp.outcome == Outcome::Held &&
              std::strcmp(rp.why, "held: the budget fell under what was planned") == 0,
          "V9 a spill with room to the planned budget lowers the flight budget, not the reserve's room");

    // Flight 20261124_210604: menu to 190 s, a load from 3638 MB, over the budget from 236 s to the end at 361 s. It was
    // sized from 11349 MB with reserve 4096 (target 6741), and that build learned nothing from it. Now the figures are there
    // before its end, and the next session, if nothing else were known, is sized 2.3 GB lower.
    const auto flightUse = [](uint32_t i) {
        return i < 190 ? (i < 180 ? 5000 : 3638) : (i < 214 ? 3638 + static_cast<int>(i - 190) * 182 : (i < 240 ? 8010 + static_cast<int>(i - 214) * 70 : 10000));
    };
    const auto flightBudget = [](uint32_t i) { return i < 205 ? 11349 : (i < 290 ? 9220 : 9050); };
    const SessionWatch flight = session(361, 11349, flightUse, flightBudget);
    const LearnResult rl = learn(4096, 0, 11349, xess, flight);
    check(flight.spills() == 1 && rl.outcome == Outcome::Held && rl.reserveMb == 4096 && rl.flightBudgetMb == 9216,
          "V9 flight 20261124_210604 is a spill and lowers the flight budget to 9216");
    check(plannedBudgetMb(11349, rl.flightBudgetMb) - rl.reserveMb - 512 == 4608, "V9 ...and the next target from 6741 to 4608");

    // Samples with no budget, or before Elite was sized, are ignored.
    SessionWatch none;
    none.sample(0, 2000 * kMiB, 0, 7123 * kMiB);
    none.sample(1000, 2000 * kMiB, 7123 * kMiB, 0);
    check(none.samples() == 0 && none.durationMs() == 0 && !none.held() && none.heldBudgetMb() == 0, "V9 samples without a budget are ignored");

    // The learn line, exactly, for case d.
    const std::string key = makeKey(0x10DE, 0x2684, 0x0020000F17C40000ull, false, true, AaSlot::Off);
    char line[768];
    formatLearnLine(line, sizeof line, key, 1024, 0, d, rd);
    check(std::strcmp(line,
                      "game vram: learned for v=10DE d=2684 drv=32.15.6084.0 layer=windows prof=vr aa=off: "
                      "peak 6823 MB of a 7123 MB budget, closest 300 MB, crossings 0; held 6823 MB, 300 MB under the planned 7123 MB, "
                      "budget held 7123 MB, spills 0, plateau yes, 1499 s "
                      "-> reserve 1024 -> 1280 MB, flight budget 0 -> 0 MB next session (tightened: less room than the margin)") == 0,
          "V9 the learn line has the exact text");

    char guarded[32];
    std::memset(guarded, 0x7F, sizeof guarded);
    const int n = formatLearnLine(guarded, 16, key, 1024, 0, d, rd);
    check(guarded[15] == '\0', "V9 a 16-byte learn line is NUL-terminated");
    check(n == 15, "V9 a 16-byte learn line returns 15 characters");
    bool overrun = false;
    for (size_t i = 16; i < sizeof guarded; ++i) overrun = overrun || guarded[i] != 0x7F;
    check(!overrun, "V9 a 16-byte learn line is not overrun");
}

// --- V11: the five review fixes ------------------------------------------------------------------------------------

// Build 334925: under 1 GB the larger of dedicated and shared; from 1 GB up to 4 GB the average when shared is
// larger; at 4 GB and above, dedicated alone. Sizes are MB.
uint32_t eliteBranchMb(uint32_t dedicatedMb, uint32_t sharedMb) {
    const uint64_t dedicated = static_cast<uint64_t>(dedicatedMb) * kMiB;
    const uint64_t shared = static_cast<uint64_t>(sharedMb) * kMiB;
    const uint64_t oneGb = 1024ull * kMiB;
    const uint64_t fourGb = 4096ull * kMiB;
    if (dedicated < oneGb) return static_cast<uint32_t>((dedicated > shared ? dedicated : shared) >> 20);
    if (dedicated < fourGb && shared > dedicated) return static_cast<uint32_t>(((dedicated + shared) / 2) >> 20);
    return dedicatedMb;
}

LearnResult learned(uint32_t reserve, LearnResult::Outcome outcome, uint32_t flight = 0) {
    LearnResult r;
    r.reserveMb = reserve;
    r.flightBudgetMb = flight;
    r.outcome = outcome;
    return r;
}

void reviewCases() {
    check(classifyStoreOpen(ERROR_FILE_NOT_FOUND) == StoreRead::Missing, "V11 file-not-found is a missing store");
    check(classifyStoreOpen(ERROR_PATH_NOT_FOUND) == StoreRead::Missing, "V11 path-not-found is a missing store");
    check(classifyStoreOpen(ERROR_SHARING_VIOLATION) == StoreRead::Error, "V11 a sharing violation is a read error");
    check(classifyStoreOpen(ERROR_ACCESS_DENIED) == StoreRead::Error, "V11 access denied is a read error");

    const LearnResult hold = learned(3584, LearnResult::Outcome::Held);
    const Record other = rec("key", 5000, 4, 2, 0);
    const Record heldOver = mergeLearned("key", 3584, 0, hold, &other);
    check(heldOver.reserveMb == 5000 && heldOver.sessions == 5 && heldOver.tightened == 2 && heldOver.relaxed == 0,
          "V11 a hold keeps the other session's higher reserve and its counters");

    const LearnResult tight = learned(6000, LearnResult::Outcome::Tightened);
    const Record raised = mergeLearned("key", 3584, 0, tight, &other);
    check(raised.reserveMb == 6000 && raised.sessions == 5 && raised.tightened == 3,
          "V11 the greater tighten is the one stored, counted on the re-read");

    const LearnResult tightLess = learned(4200, LearnResult::Outcome::Tightened);
    const Record notLower = mergeLearned("key", 3584, 0, tightLess, &other);
    check(notLower.reserveMb == 5000 && notLower.tightened == 3,
          "V11 a smaller tighten does not replace the other session's reserve");

    const Record same = rec("key", 3584, 1, 0, 0);
    const LearnResult relax = learned(3072, LearnResult::Outcome::Relaxed);
    const Record lowered = mergeLearned("key", 3584, 0, relax, &same);
    check(lowered.reserveMb == 3072 && lowered.sessions == 2 && lowered.relaxed == 1,
          "V11 a relax is kept when the other session did not raise the reserve");
    const Record relaxKept = mergeLearned("key", 3584, 0, relax, &other);
    check(relaxKept.reserveMb == 5000 && relaxKept.relaxed == 1,
          "V11 a relax does not replace a higher reserve");

    const Record fresh = mergeLearned("key", 3584, 0, hold, nullptr);
    check(fresh.reserveMb == 3584 && fresh.sessions == 1 && fresh.tightened == 0 && fresh.key == "key",
          "V11 no record yet: this session's result starts the key");

    const Record fell = rec("key", 3584, 1, 0, 0, 9152);
    const auto merged = [&](uint32_t started, uint32_t now, const Record& there) {
        return mergeLearned("key", 3584, started, learned(3584, LearnResult::Outcome::Held, now), &there).flightBudgetMb;
    };
    check(merged(0, 0, fell) == 9152 && merged(10240, 10240, fell) == 9152,
          "V11 a lower flight budget another session learned is kept");
    check(merged(9152, 9664, fell) == 9664 && merged(0, 8960, fell) == 8960,
          "V11 the one this session started from is replaced, and so is a higher one");
    check(merged(9152, 9664, rec("key", 3584, 1, 0, 0, 0)) == 9664, "V11 no flight budget there is not a lower one");

    // A session's own saves: three passes (tightened, held, relaxed) count one session, and the last one stands.
    SaveBasis own;
    Record stored = rec("key", 1024, 4, 1, 1);
    const LearnResult passes[] = {learned(1280, LearnResult::Outcome::Tightened), learned(1024, LearnResult::Outcome::Held),
                                  learned(768, LearnResult::Outcome::Relaxed)};
    for (const LearnResult& pass : passes) {
        stored = mergeLearned("key", 1024, 0, pass, own.basisFor(&stored));
        own.landed(stored);
    }
    check(stored.reserveMb == 768 && stored.sessions == 5 && stored.tightened == 1 && stored.relaxed == 2,
          "V11 a session that saves three times counts once and keeps its last result");
    const Record theirs = rec("key", 1408, 6, 2, 2);
    const Record after = mergeLearned("key", 1024, 0, passes[1], own.basisFor(&theirs));
    check(after.reserveMb == 1408 && after.sessions == 7 && after.tightened == 2, "V11 another session's write in between is kept");
    SaveBasis first;
    Record born = mergeLearned("key", 1024, 0, passes[0], first.basisFor(nullptr));
    first.landed(born);
    born = mergeLearned("key", 1024, 0, passes[1], first.basisFor(&born));
    check(born.reserveMb == 1024 && born.sessions == 1 && born.tightened == 0, "V11 the same for a key with no record before");

    check(sameAdapterIdentity(0x10DE, 0x2684, 0x10DE, 0x2684), "V11 the same vendor and device is the install key");
    check(!sameAdapterIdentity(0x10DE, 0x2684, 0x1002, 0x73BF), "V11 another device id is not the install key");
    check(!sameAdapterIdentity(0x10DE, 0x2684, 0x8086, 0x2684), "V11 the same device id on another vendor is not the install key");

    const uint64_t heap = 32768ull * kMiB;
    const uint32_t asked[] = {512, 1023, 1024, 2048, 4095};
    for (uint32_t mb : asked) {
        const uint64_t shared = sharedForReported(static_cast<uint64_t>(mb) * kMiB, heap);
        const std::string label = "V11 " + std::to_string(mb) + " MB lowers shared so Elite's under-4 GB branch yields it";
        check(shared == static_cast<uint64_t>(mb) * kMiB && eliteBranchMb(mb, static_cast<uint32_t>(shared >> 20)) == mb,
              label.c_str());
        const std::string untouched = "V11 " + std::to_string(mb) + " MB with the real shared heap is not that card";
        check(eliteBranchMb(mb, static_cast<uint32_t>(heap >> 20)) != mb, untouched.c_str());
    }
    check(sharedForReported(4096ull * kMiB, heap) == heap, "V11 4096 MB, the auto floor, leaves shared alone");
    check(sharedForReported(8192ull * kMiB, heap) == heap, "V11 a fixed size at or above 4 GB leaves shared alone");
    check(sharedForReported(1024ull * kMiB, 256ull * kMiB) == 256ull * kMiB, "V11 shared is not raised when it is already smaller");
    check(eliteBranchMb(1024, 256) == 1024, "V11 a smaller shared heap already yields the dedicated size");

    Inputs fixed;
    fixed.realDedicatedBytes = 8192ull * kMiB;
    const Result f = compute(setting(Mode::Fixed, 1024), fixed);
    check(f.change && f.reportedBytes == 1024ull * kMiB, "V11 fixed 1024 is not raised to the 4096 floor");

    const uint32_t budgets[] = {3000, 4096, 7000, 7500, 8000, 8192, 11350, 12000};
    for (uint32_t b : budgets) {
        const uint32_t cap = reservePinningFloorMb(static_cast<uint64_t>(b) * kMiB, kMarginMb, 16384);
        Inputs in;
        in.realDedicatedBytes = 16384ull * kMiB;
        in.osBudgetBytes = static_cast<uint64_t>(b) * kMiB;
        // reserveMb 0 in compute() means "use the default", so a cap of 0 is checked as 1, and the step
        // below the cap is only asked when that reserve is a real explicit value.
        in.reserveMb = cap == 0 ? 1u : cap;
        const Result at = compute(setting(Mode::Auto), in);
        const std::string atLabel = "V11 budget " + std::to_string(b) + " MB: the pinning reserve reports 4096";
        check(at.reportedBytes == static_cast<uint64_t>(kAutoFloorMb) * kMiB, atLabel.c_str());
        if (cap > 1) {
            in.reserveMb = cap - 1;
            const Result under = compute(setting(Mode::Auto), in);
            const std::string underLabel = "V11 budget " + std::to_string(b) + " MB: one MB under the cap reports more than 4096";
            check(under.reportedBytes > static_cast<uint64_t>(kAutoFloorMb) * kMiB, underLabel.c_str());
        }
    }

    Inputs wide;
    wide.realDedicatedBytes = 16384ull * kMiB;
    wide.osBudgetBytes = 12000ull * kMiB;
    wide.reserveMb = 3584;
    const Result wideBefore = compute(setting(Mode::Auto), wide);
    wide.reserveMb = 4452;
    const Result wideAfter = compute(setting(Mode::Auto), wide);
    check(wideBefore.reportedBytes > wideAfter.reportedBytes &&
              wideAfter.reportedBytes > static_cast<uint64_t>(kAutoFloorMb) * kMiB,
          "V11 a crossing below the floor cap still changes the reported size in one sample");
}

// --- V6: source text ---------------------------------------------------------------------------------------------

bool readFile(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

size_t countOf(const std::string& text, const std::string& needle) {
    size_t n = 0;
    for (size_t p = text.find(needle); p != std::string::npos; p = text.find(needle, p + needle.size())) ++n;
    return n;
}

void sourceCases(const std::string& root) {
    std::string ini, proxy, tick;
    const std::string iniPath = root + "/edup.ini";
    const std::string proxyPath = root + "/src/d3d11/d3d11_proxy.cpp";
    const std::string tickPath = root + "/src/d3d11/vram_tick.cpp";
    if (!readFile(iniPath, ini)) {
        check(false, ("V6 cannot read " + iniPath).c_str());
    } else {
        const size_t advanced = ini.find("\n[advanced]");
        const size_t experimental = ini.find("\n[experimental]");
        const size_t key = ini.find("\n#game_vram = auto");
        check(advanced != std::string::npos && key != std::string::npos && experimental != std::string::npos &&
                  advanced < key && key < experimental,
              "V6 edup.ini has '#game_vram = auto' inside [advanced]");
        // The comment block directly above the key: the run of lines starting with '#' before it.
        std::vector<std::string> lines;
        std::istringstream lineStream(ini);
        for (std::string line; std::getline(lineStream, line);) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(line);
        }
        size_t keyLine = lines.size();
        for (size_t i = 0; i < lines.size(); ++i)
            if (lines[i] == "#game_vram = auto") keyLine = i;
        bool hidden = false;
        if (keyLine < lines.size()) {
            for (size_t i = keyLine; i-- > 0 && !lines[i].empty() && lines[i][0] == '#';)
                if (lines[i] == "# dev: hidden") hidden = true;
        }
        check(hidden, "V6 the comment block above '#game_vram = auto' contains '# dev: hidden'");
    }

    if (!readFile(proxyPath, proxy)) {
        check(false, ("V6 cannot read " + proxyPath).c_str());
    } else {
        check(countOf(proxy, "gameVramInstall(adapter") >= 2,
              "V6 d3d11_proxy.cpp calls gameVramInstall(adapter in both device creators");
        check(countOf(proxy, "gameVramNoteDevice(") >= 2,
              "V11 both device creators key the reserve from the device after D3D11CreateDevice returns");
        const auto noteAfterCreate = [&](const char* fn) {
            const size_t at = proxy.find(fn);
            if (at == std::string::npos) return false;
            const size_t next = proxy.find("\nextern \"C\"", at + 1);
            const std::string chunk = proxy.substr(at, next == std::string::npos ? std::string::npos : next - at);
            const size_t call = chunk.find("target(");
            const size_t noted = chunk.find("gameVramNoteDevice(");
            return call != std::string::npos && noted != std::string::npos && call < noted;
        };
        check(noteAfterCreate("edvr_impl_D3D11CreateDevice(") &&
                  noteAfterCreate("edvr_impl_D3D11CreateDeviceAndSwapChain("),
              "V11 gameVramNoteDevice runs after the real create returns, in both exports");
    }

    if (!readFile(tickPath, tick)) {
        check(false, ("V6 cannot read " + tickPath).c_str());
    } else {
        check(tick.find("gameVramObserve(") != std::string::npos, "V6 vram_tick.cpp feeds gameVramObserve(");
    }

    // Under the flat profile a key missing from runtimeProfileAllowsKey reads as "off" with no line in the log, which is how
    // the first test flight (2026-11-20) ran without the setting it was flown for.
    std::string profile;
    const std::string profilePath = root + "/src/common/runtime_profile.h";
    if (!readFile(profilePath, profile)) {
        check(false, ("V6 cannot read " + profilePath).c_str());
    } else {
        // The name must sit inside the braces of kEveryProfileKeys, not in a comment beside it.
        const size_t decl = profile.find("kEveryProfileKeys[] = {");
        const size_t close = decl == std::string::npos ? std::string::npos : profile.find("};", decl);
        const size_t key = decl == std::string::npos ? std::string::npos : profile.find("\"advanced.game_vram\"", decl);
        check(decl != std::string::npos && close != std::string::npos && key != std::string::npos && key < close,
              "V6 runtime_profile.h's kEveryProfileKeys lets every profile read advanced.game_vram");
    }

    // Every profile reads the key, passthrough and an invalid install included, and its default is auto: the hook itself
    // must stand down where no fix runs. And it acts on the main executable's calls, which outside the game are not Elite's.
    std::string hook;
    const std::string hookPath = root + "/src/d3d11/game_vram_hook.cpp";
    if (!readFile(hookPath, hook)) {
        check(false, ("V6 cannot read " + hookPath).c_str());
    } else {
        const size_t install = hook.find("void gameVramInstall(");
        const size_t setting = install == std::string::npos ? std::string::npos : hook.find("getString(\"advanced.game_vram\"", install);
        const size_t features = install == std::string::npos ? std::string::npos : hook.find("runtimeFeaturesAllowed()", install);
        const size_t game = install == std::string::npos ? std::string::npos : hook.find("gameBuildKnown()", install);
        check(features != std::string::npos && setting != std::string::npos && features < setting,
              "V6 gameVramInstall stands down in a profile that forwards only, before it reads the setting");
        check(game != std::string::npos && setting != std::string::npos && game < setting,
              "V6 gameVramInstall stands down in a process that is not the game, before it reads the setting");
        check(hook.find("_ReturnAddress()") != std::string::npos && hook.find("callerRva >= exe.size") != std::string::npos,
              "V6 hookedGetDesc acts only when its return address is inside the game executable");
        const size_t observe = hook.find("void gameVramObserve(");
        check(observe != std::string::npos && hook.find("!= g_learn.profileAtInstall", observe) != std::string::npos,
              "V6 gameVramObserve stops learning when the profile changes");
        check(countOf(hook, "== gamevram::StoreRead::Error") >= 2,
              "V6 both the install and the save leave a store they could not read alone");
        const size_t prepare = hook.find("void prepareAuto(");
        const size_t guard = prepare == std::string::npos ? std::string::npos : hook.find("budgetIsWholeCard(", prepare);
        const size_t storeOpen = prepare == std::string::npos ? std::string::npos : hook.find("storePath(path)", prepare);
        check(guard != std::string::npos && storeOpen != std::string::npos && guard < storeOpen &&
                  hook.find("if (wine)", prepare) < guard,
              "V6 prepareAuto checks for a whole-card budget under Wine before it opens the store");
        check(countOf(hook, "gamevram::adoptRecord(") == 2, "V6 both the install and the save read a record inside its slot's limits");
        check(hook.find("in.othersBytes = g_othersBytes;") != std::string::npos && hook.find("g_othersRead = adapterUsageStart();") != std::string::npos,
              "V6 hookedGetDesc sizes with what other programs held, read from when auto is prepared");
        const size_t hooked = hook.find("HRESULT STDMETHODCALLTYPE hookedGetDesc(");
        const size_t ready = hooked == std::string::npos ? std::string::npos : hook.find("ensureAutoReady();", hooked);
        check(ready != std::string::npos && ready < hook.find("in.reserveMb = g_reserveMb;", hooked),
              "V6 the game's first question waits for auto's start before it reads the reserve");
        const size_t keyedAt = hook.find("const bool keyed = gamevram::sameAdapterIdentity(");
        const size_t keyedIf = keyedAt == std::string::npos ? std::string::npos : hook.find("if (keyed) {", keyedAt);
        check(keyedIf != std::string::npos && keyedIf < hook.find("in.othersBytes = g_othersBytes;") && keyedIf < hook.find("in.flightBudgetMb = g_flightMb;"),
              "V6 ...and only for the adapter they were read for");
        check(hook.find("adapterUsageFinished(g_othersRead, kOthersWaitMs") != std::string::npos, "V6 the start waits for that reading a bounded time");
        check(prepare != std::string::npos && hook.find("g_startBudgetBytes.store(0,", prepare) != std::string::npos,
              "V6 prepareAuto forgets the start budget of the adapter it was keyed to before");
        const size_t finish = hook.find("void finishAuto(");
        const size_t factsAt = finish == std::string::npos ? std::string::npos : hook.find("g_learn.facts.startMb =", finish);
        const size_t factsGuard = finish == std::string::npos ? std::string::npos : hook.find("if (g_learn.haveGpu && !g_learn.wine) readOthers();", finish);
        const size_t adoptAt = finish == std::string::npos ? std::string::npos : hook.find("if (!g_learn.adopt) return;", finish);
        check(factsAt != std::string::npos && factsGuard != std::string::npos && factsGuard < factsAt && factsAt < adoptAt,
              "V6 an old record's takeover has its facts under Wine too: only the reading is skipped there");
        const size_t observeStart = observe == std::string::npos ? std::string::npos : hook.find("g_startBudgetBytes.load(", observe);
        check(observeStart != std::string::npos && observeStart < hook.find("g_learn.active", observe),
              "V6 a sample reads the published start budget before anything finishAuto wrote");
        check(hook.find("desc.AdapterLuid.LowPart != g_learn.luid.LowPart") != std::string::npos,
              "V6 a second card of the same kind re-keys auto");
        check(hook.find("game vram: watching %s") != std::string::npos, "V6 the first sample is said in the log");
        check(hook.find("in.flightBudgetMb = g_flightMb;") != std::string::npos,
              "V6 hookedGetDesc sizes with the flight budget");
        check(hook.find("g_startBudgetBytes.compare_exchange_strong(") != std::string::npos,
              "V6 hookedGetDesc keeps the budget of its first answer about the keyed adapter");
        check(hook.find("!g_budgetIgnored) ? osLocalBudget(self)") != std::string::npos,
              "V6 hookedGetDesc ignores the OS budget when the Wine guard fired");
        check(hook.find("classifyStoreOpen(") != std::string::npos,
              "V11 a missing store is told from one that could not be read by classifyStoreOpen");
        check(hook.find("mergeLearned(") != std::string::npos,
              "V11 a save merges onto the record just re-read instead of the install-time copy");
        check(hook.find("g_saveBasis.basisFor(") != std::string::npos && hook.find("g_saveBasis.landed(") != std::string::npos,
              "V11 a session's own earlier save is not merged as another session's");
        check(hook.find("sharedForReported(") != std::string::npos && hook.find("SharedSystemMemory") != std::string::npos,
              "V11 a reported size under 4 GB also lowers SharedSystemMemory");
        check(hook.find("desc->DedicatedSystemMemory") == std::string::npos && hook.find("desc->AdapterLuid") == std::string::npos,
              "V11 DedicatedSystemMemory and the adapter LUID are not changed");
        check(hook.find("GetAdapter(") != std::string::npos && hook.find("sameAdapterIdentity(") != std::string::npos &&
                  hook.find("deviceNoted") != std::string::npos,
              "V11 the device's adapter is read with GetAdapter and a different id does not keep the install key");
        {
            const size_t noteAt = hook.find("void gameVramNoteDevice(");
            const size_t noteEnd = noteAt == std::string::npos ? std::string::npos : hook.find("\n}\n", noteAt);
            const std::string note = noteEnd == std::string::npos ? std::string() : hook.substr(noteAt, noteEnd - noteAt);
            const size_t finishAt = hook.find("void finishAuto(");
            const size_t finishEnd = finishAt == std::string::npos ? std::string::npos : hook.find("\n}\n", finishAt);
            const std::string finish = finishEnd == std::string::npos ? std::string() : hook.substr(finishAt, finishEnd - finishAt);
            check(note.find("prepareAuto(adapter, g_learn.aaAtInstall)") != std::string::npos &&
                      note.find("g_learn.active =") == std::string::npos && finish.find("g_learn.active = true;") != std::string::npos,
                  "V11 a different device adapter re-keys auto: its own record or the default, and learning goes on under it "
                  "once Elite's first question finishes auto's start");
            check(note.find("g_learn.stopped = true") != std::string::npos,
                  "V11 a device adapter that cannot be read stops learning");
            const size_t descAt = prepare == std::string::npos ? std::string::npos : hook.find("DXGI_ADAPTER_DESC", prepare);
            check(prepare != std::string::npos && hook.find("g_budgetIgnored = false;", prepare) < descAt &&
                      hook.find("g_learn.storePath.clear();", prepare) < descAt,
                  "V11 prepareAuto starts clean, so running it again for the device's adapter keeps nothing from install");
        }
    }
}

const char* const kCells[] = {
    "V1 parseSetting: the recognised words and numbers, the refused text",
    "V2 compute Off: never changes",
    "V3 compute Fixed: cap, card-or-under, below the auto floor",
    "V4 compute Auto: different reserves, no OS budget (95% of the card), tiny budget, card already fits, no card size",
    "V5 formatCallLine and settingText: exact text, 16-byte buffer terminated and not overrun",
    "V6 glue by source text: edup.ini key and comment, d3d11_proxy.cpp calls, vram_tick.cpp feeds, runtime_profile.h's every-profile key list, "
    "game_vram_hook.cpp's caller test, profile and game gates, profile-change stop, unreadable-store guard, Wine whole-card budget guard",
    "V7 setup key, AA slots, each slot's starting reserve and limits",
    "V8 learned store: round trip, skipped lines, duplicates, upserts, 64-record cap, v0.23.0's records, the flight budget another engine left",
    "V9 SessionWatch and learn: the length gate, the rules and outcomes, the limits, the flight budget, the learn line",
    "V10 auto with other reserves (including none), with no OS budget, what other programs hold, and the whole-card budget test",
    "V11 re-read merge, own saves, floor cap, shared memory under 4 GB, device adapter key",
};

}  // namespace

int main(int argc, char** argv) {
    bool selfTest = false, dryRun = false;
    std::string root;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--self-test") selfTest = true;
        else if (a == "--dry-run") dryRun = true;
        else if (selfTest && root.empty() && a.rfind("--", 0) != 0) root = a;
    }
    if (dryRun) {
        std::printf("[game_vram_test] dry-run: touches nothing (no log, no file, no process)\n");
        for (const char* cell : kCells) std::printf("  %s\n", cell);
        return 0;
    }
    if (!selfTest) {
        std::printf("usage: game_vram_test.exe --dry-run | --self-test [root]\n");
        return 2;
    }
    parseCases();
    offCases();
    fixedCases();
    autoCases();
    zeroCardCases();
    formatCases();
    autoReserveCases();
    keyCases();
    storeCases();
    learnCases();
    reviewCases();
    if (root.empty()) {
        std::printf("[game_vram_test] V6 source cells skipped: no repository root given\n");
    } else {
        sourceCases(root);
    }
    std::printf("[game_vram_test] %u checks, %u failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
