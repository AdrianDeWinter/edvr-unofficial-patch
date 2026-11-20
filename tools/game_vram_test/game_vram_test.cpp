// The rig for advanced.game_vram (docs/memory-growth-2026-10-22.md): the setting parser, the arithmetic that
// decides what Elite is told (src\common\game_vram.h), the call line's exact text and its buffer limits, the setup
// key, the learned store and the session rules (src\common\game_vram_learn.h), and by source text the glue that
// wires it (edvr.ini, d3d11_proxy.cpp, vram_tick.cpp, runtime_profile.h, game_vram_hook.cpp).
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
//   V7   the setup key, the AA classes and the safe starting reserves
//   V8   the learned store: round trip, skipped lines, duplicates, upserts and the 64-record cap
//   V9   SessionWatch and learn: the rules and each outcome, with synthetic 1 s samples, and the learn line's text
//   V10  auto with other reserves (including none), and with no OS budget
//
//   game_vram_test.exe --dry-run            touches nothing
//   game_vram_test.exe --self-test [root]   root is the repository, for the V6 source pins (skipped without it)
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

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
        "game vram: call=1 caller=exe+0x2000 real_mb=12216 reported_mb=9738 setting=auto budget_mb=11350 target_mb=8278"
        " reserve_mb=2560 (auto)";
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
        "game vram: call=2 caller=exe+0x3000 real_mb=12118 reported_mb=11736 setting=auto budget_mb=11512 target_mb=9976"
        " reserve_mb=1024 (auto, no OS budget: 95% of the card)";
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
    check(makeKey(0x10DE, 0x2684, drv, false, false, "") ==
              "v=10DE d=2684 drv=32.15.6084.0 layer=windows prof=flat aa=off",
          "V7 key for a GeForce on Windows, flat, no AA");
    check(makeKey(0x10DE, 0x2684, drv, true, false, "") ==
              "v=10DE d=2684 drv=32.15.6084.0 layer=wine prof=flat aa=off",
          "V7 key under Wine says layer=wine");
    check(makeKey(0x10DE, 0x2684, drv, false, true, "") ==
              "v=10DE d=2684 drv=32.15.6084.0 layer=windows prof=vr aa=off",
          "V7 key for the VR profile says prof=vr");
    check(makeKey(0x10DE, 0x2684, drv, false, false, "XeSS") ==
              "v=10DE d=2684 drv=32.15.6084.0 layer=windows prof=flat aa=xess",
          "V7 key lowercases the AA mode");
    check(makeKey(0x10DE, 0x2684, 0, false, false, "") ==
              "v=10DE d=2684 drv=unknown layer=windows prof=flat aa=off",
          "V7 driver version 0 is drv=unknown");

    check(aaClassOf("") == AaClass::Off, "V7 aaClassOf '' is Off");
    check(aaClassOf("off") == AaClass::Off, "V7 aaClassOf 'off' is Off");
    check(aaClassOf("on") == AaClass::Taa, "V7 aaClassOf 'on' is Taa");
    check(aaClassOf("TAA") == AaClass::Taa, "V7 aaClassOf 'TAA' is Taa");
    check(aaClassOf("dlss") == AaClass::Upscaler, "V7 aaClassOf 'dlss' is Upscaler");
    check(aaClassOf("xess") == AaClass::Upscaler, "V7 aaClassOf 'xess' is Upscaler");
    check(aaClassOf("dittosr") == AaClass::Upscaler, "V7 aaClassOf 'dittosr' is Upscaler");
    check(std::strcmp(aaClassName(AaClass::Off), "off") == 0, "V7 aaClassName Off is 'off'");
    check(std::strcmp(aaClassName(AaClass::Taa), "taa") == 0, "V7 aaClassName Taa is 'taa'");
    check(std::strcmp(aaClassName(AaClass::Upscaler), "upscaler") == 0, "V7 aaClassName Upscaler is 'upscaler'");

    check(defaultReserveMb(false, AaClass::Off) == 1536, "V7 flat, Off: 1536 MB");
    check(defaultReserveMb(false, AaClass::Taa) == 2048, "V7 flat, Taa: 2048 MB");
    check(defaultReserveMb(false, AaClass::Upscaler) == 3584, "V7 flat, Upscaler: 3584 MB");
    check(defaultReserveMb(true, AaClass::Off) == 3072, "V7 VR, Off: 3072 MB");
    check(defaultReserveMb(true, AaClass::Taa) == 3584, "V7 VR, Taa: 3584 MB");
    check(defaultReserveMb(true, AaClass::Upscaler) == 4608, "V7 VR, Upscaler: 4608 MB");
}

// --- V8: the learned store -------------------------------------------------------------------------------------

Record rec(const char* key, uint32_t reserve, uint32_t sessions, uint32_t tightened, uint32_t relaxed) {
    Record r;
    r.key = key;
    r.reserveMb = reserve;
    r.sessions = sessions;
    r.tightened = tightened;
    r.relaxed = relaxed;
    return r;
}

void storeCases() {
    std::vector<Record> recs;
    recs.push_back(rec("keyA", 1536, 3, 1, 0));
    recs.push_back(rec("keyB", 3584, 5, 2, 1));
    recs.push_back(rec("keyC", 768, 0, 0, 0));
    const std::string text = serializeStore(recs);
    const std::string want =
        "# EDVR: what advanced.game_vram = auto learned per setup. Safe to delete.\n"
        "1536\t3\t1\t0\tkeyA\n"
        "3584\t5\t2\t1\tkeyB\n"
        "768\t0\t0\t0\tkeyC\n";
    check(text == want, "V8 serializeStore writes the header and three exact lines");

    const std::vector<Record> back = parseStore(text);
    check(back.size() == 3, "V8 round trip keeps three records");
    check(serializeStore(back) == text, "V8 round trip serializes to the same text");
    const Record* b = findRecord(back, "keyB");
    check(b != nullptr && b->reserveMb == 3584 && b->sessions == 5 && b->tightened == 2 && b->relaxed == 1,
          "V8 round trip keeps keyB's four numbers");
    check(findRecord(back, "missing") == nullptr, "V8 findRecord of an unknown key is null");

    const std::string messy =
        "# a comment\n"
        "\n"
        "1536\t1\t0\t0\tkeyD\r\n"   // CRLF: valid
        "1536\t1\t0\tkeyE\n"        // four fields: skipped
        "100\t1\t0\t0\tkeyF\n"      // reserve below the floor: skipped
        "abc\t1\t0\t0\tkeyG\n"      // not a number: skipped
        "1536\t1\t0\t0\t\n"         // empty key: skipped
        "2048\t1\t0\t0\tkeyH\n"
        "3072\t2\t0\t0\tkeyH\n";    // duplicate: the later wins
    const std::vector<Record> m = parseStore(messy);
    check(m.size() == 2, "V8 messy store keeps two records");
    const Record* d = findRecord(m, "keyD");
    check(d != nullptr && d->reserveMb == 1536 && d->sessions == 1, "V8 a CRLF line is parsed");
    const Record* h = findRecord(m, "keyH");
    check(h != nullptr && h->reserveMb == 3072 && h->sessions == 2, "V8 a duplicate key: the later line wins");
    check(findRecord(m, "keyE") == nullptr, "V8 a four-field line is skipped");
    check(findRecord(m, "keyF") == nullptr, "V8 a reserve of 100 is skipped");
    check(findRecord(m, "keyG") == nullptr, "V8 a non-number line is skipped");

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

// Usage 2000 MB, budget 4000 MB, one sample per second, for n samples.
SessionWatch steady(uint32_t n, uint64_t usageMb, uint64_t budgetMb) {
    SessionWatch w;
    for (uint32_t i = 0; i < n; ++i) w.sample(i * 1000ull, usageMb * kMiB, budgetMb * kMiB);
    return w;
}

// 1500 samples (25 min): usage 1000 MB rising by 1 MB a second; peak 2499 MB at the last sample. Not plateaued.
SessionWatch rising(uint64_t budgetMb) {
    SessionWatch w;
    for (uint32_t i = 0; i < 1500; ++i) w.sample(i * 1000ull, (1000ull + i) * kMiB, budgetMb * kMiB);
    return w;
}

// 1500 samples: usage climbs to 2000 MB by minute 8 (second 480) and stays there. Plateaued.
SessionWatch settled(uint64_t budgetMb) {
    SessionWatch w;
    for (uint32_t i = 0; i < 1500; ++i) {
        const uint64_t usageMb = i < 480 ? 1000ull + (i * 1000ull) / 480ull : 2000ull;
        w.sample(i * 1000ull, usageMb * kMiB, budgetMb * kMiB);
    }
    return w;
}

void learnCases() {
    // Short session, no crossing, big headroom: held.
    const SessionWatch a = steady(120, 2000, 4000);
    check(a.samples() == 120, "V9 120 samples are counted");
    const LearnResult ra = learn(3584, a);
    check(!ra.changed && ra.reserveMb == 3584 && std::strcmp(ra.why, "held: session too short") == 0,
          "V9 120 samples, big headroom: held, too short");
    check(ra.outcome == LearnResult::Outcome::TooShort, "V9 120 samples, big headroom: outcome TooShort");

    // 60 samples, one over the budget by 100 MB: tightened even though short. 3584 + (512 + 100) + 256 = 4452.
    SessionWatch b;
    for (uint32_t i = 0; i < 60; ++i) b.sample(i * 1000ull, (i == 30 ? 4100ull : 2000ull) * kMiB, 4000 * kMiB);
    check(b.crossings() == 1, "V9 one crossing is counted");
    check(b.minHeadroomMb() == -100, "V9 the closest headroom is -100 MB");
    const LearnResult rb = learn(3584, b);
    check(rb.changed && rb.reserveMb == 4452, "V9 crossing: tightened 3584 -> 4452");
    check(std::strcmp(rb.why, "tightened: crossed the budget") == 0, "V9 crossing says 'tightened: crossed the budget'");
    check(rb.outcome == LearnResult::Outcome::Tightened, "V9 crossing: outcome Tightened");

    // 1500 samples rising to the peak at the last sample: not plateaued, held.
    const SessionWatch c = rising(4000);
    check(!c.plateaued(), "V9 a session still rising at its end is not plateaued");
    const LearnResult rc = learn(3584, c);
    check(!rc.changed && rc.reserveMb == 3584 && std::strcmp(rc.why, "held: usage still rising or session under 20 min") == 0,
          "V9 rising usage: held");
    check(rc.outcome == LearnResult::Outcome::Held, "V9 rising usage: outcome Held");

    // Settled at 2000 MB by minute 8, budget 4000 MB, so min headroom 2000 MB: relaxed by min(2000-512-256, 512) = 512.
    const SessionWatch d = settled(4000);
    check(d.plateaued(), "V9 settled from minute 8 is plateaued");
    check(d.minHeadroomMb() == 2000, "V9 settled at 2000 MB of a 4000 MB budget: closest 2000 MB");
    const LearnResult rd = learn(3584, d);
    check(rd.changed && rd.reserveMb == 3072, "V9 settled, headroom 2000: relaxed 3584 -> 3072");
    check(std::strcmp(rd.why, "relaxed: room to spare") == 0, "V9 relaxed says 'relaxed: room to spare'");
    check(rd.outcome == LearnResult::Outcome::Relaxed, "V9 relaxed: outcome Relaxed");

    // Same, reserve 1000: 1000 - 512 = 488, clamped to the floor 768.
    const LearnResult rd2 = learn(1000, d);
    check(rd2.changed && rd2.reserveMb == 768, "V9 settled, reserve 1000: relaxed to the floor 768");

    // Same, reserve 768: already at the floor.
    const LearnResult rd3 = learn(768, d);
    check(!rd3.changed && rd3.reserveMb == 768 && std::strcmp(rd3.why, "held: at the floor") == 0,
          "V9 settled, reserve 768: held at the floor");
    check(rd3.outcome == LearnResult::Outcome::Held, "V9 held at the floor: outcome Held");

    // Settled, min headroom 600 (budget 2600): not below the margin of 512, not above 768: held within the margin.
    const SessionWatch e = settled(2600);
    check(e.plateaued() && e.minHeadroomMb() == 600, "V9 settled at 2000 MB of 2600 MB: plateaued, closest 600 MB");
    const LearnResult re = learn(3584, e);
    check(!re.changed && re.reserveMb == 3584 && std::strcmp(re.why, "held: headroom within the margin") == 0,
          "V9 headroom 600 with margin 512: held within the margin");
    check(re.outcome == LearnResult::Outcome::Held, "V9 held within the margin: outcome Held");

    // Within the margin but not crossed: tightened by (512 - 300) + 256 = 468, 3584 -> 4052.
    const SessionWatch g = steady(10, 3700, 4000);
    const LearnResult rg = learn(3584, g);
    check(rg.changed && rg.reserveMb == 4052 && rg.outcome == LearnResult::Outcome::Tightened &&
              std::strcmp(rg.why, "tightened: came within the margin") == 0,
          "V9 closest 300 MB: tightened 3584 -> 4052, outcome Tightened");

    // Budget 0 samples are ignored.
    SessionWatch f;
    for (uint32_t i = 0; i < 5; ++i) f.sample(i * 1000ull, 2000 * kMiB, 0);
    check(f.samples() == 0 && f.durationMs() == 0, "V9 budget 0 samples are ignored");

    // The learn line, exactly, for case b.
    const std::string key = makeKey(0x10DE, 0x2684, 0x0020000F17C40000ull, false, false, "");
    const LearnResult lb = learn(3584, b);
    char line[512];
    formatLearnLine(line, sizeof line, key, 3584, b, lb);
    check(std::strcmp(line,
                      "game vram: learned for v=10DE d=2684 drv=32.15.6084.0 layer=windows prof=flat aa=off: "
                      "peak 4100 MB of a 4000 MB budget, closest -100 MB, crossings 1, plateau no, 59 s "
                      "-> reserve 3584 -> 4452 MB next session (tightened: crossed the budget)") == 0,
          "V9 the learn line has the exact text");

    char guarded[32];
    std::memset(guarded, 0x7F, sizeof guarded);
    const int n = formatLearnLine(guarded, 16, key, 3584, b, lb);
    check(guarded[15] == '\0', "V9 a 16-byte learn line is NUL-terminated");
    check(n == 15, "V9 a 16-byte learn line returns 15 characters");
    bool overrun = false;
    for (size_t i = 16; i < sizeof guarded; ++i) overrun = overrun || guarded[i] != 0x7F;
    check(!overrun, "V9 a 16-byte learn line is not overrun");
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
    const std::string iniPath = root + "/edvr.ini";
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
              "V6 edvr.ini has '#game_vram = auto' inside [advanced]");
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
        check(countOf(hook, "== StoreRead::Unreadable") >= 2,
              "V6 both the install and the save leave a store they could not read alone");
        const size_t prepare = hook.find("void prepareAuto(");
        const size_t guard = prepare == std::string::npos ? std::string::npos : hook.find("budgetIsWholeCard(", prepare);
        const size_t storeOpen = prepare == std::string::npos ? std::string::npos : hook.find("storePath(path)", prepare);
        check(guard != std::string::npos && storeOpen != std::string::npos && guard < storeOpen &&
                  hook.find("if (wine)", prepare) < guard,
              "V6 prepareAuto checks for a whole-card budget under Wine before it opens the store");
        check(hook.find("!g_budgetIgnored) ? osLocalBudget(self)") != std::string::npos,
              "V6 hookedGetDesc ignores the OS budget when the Wine guard fired");
    }
}

const char* const kCells[] = {
    "V1 parseSetting: the recognised words and numbers, the refused text",
    "V2 compute Off: never changes",
    "V3 compute Fixed: cap, card-or-under, below the auto floor",
    "V4 compute Auto: different reserves, no OS budget (95% of the card), tiny budget, card already fits, no card size",
    "V5 formatCallLine and settingText: exact text, 16-byte buffer terminated and not overrun",
    "V6 glue by source text: edvr.ini key and comment, d3d11_proxy.cpp calls, vram_tick.cpp feeds, runtime_profile.h's every-profile key list, "
    "game_vram_hook.cpp's caller test, profile and game gates, profile-change stop, unreadable-store guard, Wine whole-card budget guard",
    "V7 setup key, AA classes, safe starting reserves",
    "V8 learned store: round trip, skipped lines, duplicates, upserts, 64-record cap",
    "V9 SessionWatch and learn: the rules and outcomes with synthetic samples, the learn line",
    "V10 auto with other reserves (including none), with no OS budget, and the whole-card budget test",
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
    if (root.empty()) {
        std::printf("[game_vram_test] V6 source cells skipped: no repository root given\n");
    } else {
        sourceCases(root);
    }
    std::printf("[game_vram_test] %u checks, %u failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
