#pragma once
// The video memory Elite is told the graphics card has (advanced.game_vram).
//
// Elite sizes its texture and mesh streaming budget at 0.85 x the card's
// DedicatedVideoMemory, read through IDXGIAdapter::GetDesc at the moment it
// sizes the budget. It never asks Windows how much the process may really
// use, and EDVR's own 2-2.8 GB of work sits on top of that budget, so the
// streaming can outgrow what Windows allows, spill into shared memory and
// slow to a crawl. This header holds the arithmetic for telling Elite a
// smaller size: which size, from the setting and the OS budget. The hook that
// applies it (src\d3d11\game_vram_hook.cpp) changes only the answers to the
// game's own GetDesc calls. See docs/memory-growth-2026-10-22.md.
//
// The three settings:
//   off    -- Elite is told the card's real size, as without EDVR.
//   auto   -- (shipped) Elite is told a size whose 85% fits the OS budget for
//             the game, or what other programs leave of the card when that is
//             less, or the budget it was seen to fall to in flight, less a
//             learned reserve (both game_vram_learn.h) and a margin;
//             never under 4096 MB, never over the card.
//   number -- Elite is told that many MB (256 or more), or the card's real size
//             if that is smaller.
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace edvr {
namespace gamevram {

enum class Mode : uint32_t { Off = 0, Auto, Fixed };

struct Setting {
    Mode     mode = Mode::Off;
    uint32_t fixedMb = 0;       // Fixed only
    bool     recognised = true; // false: the text was not off/auto/a number in range; mode is then Off
};

constexpr double   kEliteBudgetFactor = 0.85;  // Elite's own factor (build 334925, .data 0x145E78ADC)
constexpr uint32_t kAutoFloorMb = 4096;        // auto never reports less: Elite takes other branches below 4 GB
constexpr uint32_t kFixedMinMb = 256;
constexpr uint32_t kFixedMaxMb = 1048576;
constexpr uint32_t kMarginMb = 512;            // Auto: room left beyond the reserve

inline std::string lowerCopy(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return out;
}

// Reads an unsigned decimal field no larger than UINT32_MAX. Empty, signed, spaced or longer text is refused.
inline bool parseCount(const std::string& field, uint32_t& out) {
    if (field.empty()) return false;
    uint64_t v = 0;
    for (char c : field) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + static_cast<uint64_t>(c - '0');
        if (v > 0xFFFFFFFFull) return false;
    }
    out = static_cast<uint32_t>(v);
    return true;
}

// "off", "auto" (case-insensitive, surrounding spaces ignored), or a whole
// number of MB in [kFixedMinMb, kFixedMaxMb]. Empty text is Off and
// recognised. Anything else is Off with recognised = false.
inline Setting parseSetting(const std::string& text) {
    size_t b = 0;
    size_t e = text.size();
    while (b < e && std::isspace(static_cast<unsigned char>(text[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(text[e - 1]))) --e;
    const std::string t = text.substr(b, e - b);

    Setting s;
    if (t.empty()) return s;
    const std::string lower = lowerCopy(t);
    if (lower == "off") return s;
    if (lower == "auto") {
        s.mode = Mode::Auto;
        return s;
    }

    uint32_t v = 0;
    if (!parseCount(t, v) || v < kFixedMinMb || v > kFixedMaxMb) {
        s.recognised = false;
        return s;
    }
    s.mode = Mode::Fixed;
    s.fixedMb = v;
    return s;
}

struct Inputs {
    uint64_t realDedicatedBytes = 0;  // what GetDesc really returned
    uint64_t osBudgetBytes = 0;       // QueryVideoMemoryInfo LOCAL Budget; 0 = unknown
    bool     othersKnown = false;     // Auto: othersBytes was read
    uint64_t othersBytes = 0;         // Auto: what other processes held of the card when auto was prepared
    uint32_t reserveMb = 0;           // Auto: EDVR's, the upscaler's and the driver's share to leave free
    uint32_t flightBudgetMb = 0;      // Auto: what the OS budget was seen to fall to in flight; 0 = no fall seen
};

struct Result {
    bool        change = false;     // true: write reportedBytes into DedicatedVideoMemory
    uint64_t    reportedBytes = 0;  // == realDedicatedBytes when change is false
    uint32_t    targetMb = 0;       // auto only: the streaming budget aimed at; else 0
    uint64_t    budgetBytes = 0;    // auto only: the OS budget used (95% of the card when unknown); else 0
    uint64_t startBytes = 0;        // auto only: startBudgetMb() of it; else 0
    const char* why = "";           // short reason, static string, no spaces at the ends
};

// A LOCAL budget that is not a budget. Under Wine, DXVK without VK_EXT_memory_budget (and likely wined3d) reports the
// budget as the sum of the device-local heaps and the usage as its own allocations only. Such a budget is at least
// DedicatedVideoMemory (the largest heap), never moves, and usage never comes near it, so auto would lower the reserve
// every settled session down to the floor. A WDDM budget on Windows is below the card, so the caller asks only under Wine.
// Known false positive: an AMD card on RADV without Resizable BAR has a second, 256 MB CPU-visible device-local heap; DXVK
// sums both budgets, which can reach the GetDesc size on a quiet desktop although the driver reports a real budget. That
// setup then keeps the engine's starting reserve and learns nothing. docs/memory-growth-2026-10-22.md has the reasoning.
inline bool budgetIsWholeCard(uint64_t osBudgetBytes, uint64_t realDedicatedBytes) {
    return osBudgetBytes != 0 && realDedicatedBytes != 0 && osBudgetBytes >= realDedicatedBytes;
}

// The budget to start from. Windows' budget at start leaves out most of what other programs hold: it is generous until the
// card is in demand, and then falls to about what they leave. On the 12 GB card of the 2026-11-23 and -24 flights it read
// 11349 MB at start and 8900-9600 MB in flight; other programs held 2747 MB of the 12118 (a browser 2303 of it), which
// leaves 9371. Never under half the OS budget: programs that large are likely to be pushed out, or gone, by flight.
inline uint64_t startBudgetMb(uint64_t osBudgetMb, uint64_t realMb, bool othersKnown, uint64_t othersMb) {
    if (!othersKnown) return osBudgetMb;
    const uint64_t left = realMb > othersMb ? realMb - othersMb : 0;
    const uint64_t half = osBudgetMb / 2;
    const uint64_t start = left < osBudgetMb ? left : osBudgetMb;
    return start > half ? start : half;
}

// At most a quarter of the budget at start is given up to a fall: another program large enough for more is not this
// game's normal company, and one session beside it must not starve the next.
inline uint64_t flightBudgetFloorMb(uint64_t startBudgetMb) { return startBudgetMb - startBudgetMb / 4; }

// The budget Elite is sized from: the one read at start, or the one it falls to in flight when that is lower. The flight
// figure is a size, not a difference, so a start that is already low (another program holds the card) is not lowered twice.
inline uint64_t plannedBudgetMb(uint64_t startBudgetMb, uint32_t flightBudgetMb) {
    if (flightBudgetMb == 0) return startBudgetMb;
    const uint64_t floor = flightBudgetFloorMb(startBudgetMb);
    const uint64_t flight = flightBudgetMb > floor ? flightBudgetMb : floor;
    return flight < startBudgetMb ? flight : startBudgetMb;
}

inline Result compute(const Setting& s, const Inputs& in) {
    Result r;
    r.reportedBytes = in.realDedicatedBytes;

    if (s.mode == Mode::Off) {
        r.why = "off";
        return r;
    }
    if (in.realDedicatedBytes == 0) {
        r.why = "no card size";
        return r;
    }
    const uint64_t realMb = in.realDedicatedBytes >> 20;

    if (s.mode == Mode::Fixed) {
        const uint64_t reportedMb = s.fixedMb < realMb ? s.fixedMb : realMb;
        r.change = reportedMb < realMb;
        r.why = r.change ? "fixed" : "fixed, not below the card";
        if (r.change) r.reportedBytes = reportedMb << 20;
        return r;
    }

    // Auto. Without an OS budget, 95% of the card stands in for it.
    const bool osKnown = in.osBudgetBytes != 0;
    const uint64_t budgetBytes = osKnown ? in.osBudgetBytes : ((realMb * 95 / 100) << 20);
    const uint64_t startMb = startBudgetMb(budgetBytes >> 20, realMb, in.othersKnown, in.othersBytes >> 20);
    const uint64_t budgetMb = plannedBudgetMb(startMb, in.flightBudgetMb);
    const uint64_t reserveAndMargin = static_cast<uint64_t>(in.reserveMb) + kMarginMb;
    uint64_t target = budgetMb > reserveAndMargin ? budgetMb - reserveAndMargin : 0;
    if (target > 0xFFFFFFFFull) target = 0xFFFFFFFFull;
    r.targetMb = static_cast<uint32_t>(target);
    r.budgetBytes = budgetBytes;
    r.startBytes = startMb << 20;

    // Non-negative, so the conversion truncates to the floor.
    uint64_t reportedMb = static_cast<uint64_t>(static_cast<double>(target) / kEliteBudgetFactor);
    if (reportedMb < kAutoFloorMb) reportedMb = kAutoFloorMb;
    if (reportedMb > realMb) reportedMb = realMb;

    r.change = reportedMb < realMb;
    if (r.change) {
        r.why = osKnown ? "auto" : "auto, no OS budget: 95% of the card";
        r.reportedBytes = reportedMb << 20;
    } else {
        r.why = "auto, the card already fits";
    }
    return r;
}

// Build 334925 sizes from DedicatedVideoMemory alone only at 4 GB and above. Under 1 GB it takes the larger of
// dedicated and SharedSystemMemory; from 1 GB up to 4 GB it averages the two when shared is larger. A reported
// dedicated size in that range therefore also lowers shared to the same size, so both branches yield the size
// asked for. Shared is never raised, and at or above the auto floor it is left alone (auto itself never reports
// less than that floor). DedicatedSystemMemory and the adapter LUID are not this function's concern.
inline uint64_t sharedForReported(uint64_t reportedDedicatedBytes, uint64_t realSharedBytes) {
    constexpr uint64_t kFourGb = static_cast<uint64_t>(kAutoFloorMb) << 20;
    if (reportedDedicatedBytes >= kFourGb) return realSharedBytes;
    if (realSharedBytes > reportedDedicatedBytes) return reportedDedicatedBytes;
    return realSharedBytes;
}

// Smallest reserve in MB that already makes auto report kAutoFloorMb for this budget and margin, on a card at
// least that large. The arithmetic is compute()'s: target = budget - reserve - margin, then floor(target / 0.85),
// raised to the floor when that is lower. A result that lands on the floor without being raised is already
// pinned: a further reserve does not change what Elite is told. reserveCeilMb when even that reserve still
// reports more (the budget is too large for the ceiling to pin).
inline uint32_t reservePinningFloorMb(uint64_t budgetBytes, uint32_t marginMb, uint32_t reserveCeilMb) {
    if (reserveCeilMb == 0) return 0;
    const uint64_t budgetMb = budgetBytes >> 20;
    const uint64_t margin = marginMb != 0 ? marginMb : kMarginMb;
    auto pins = [&](uint32_t reserve) {
        const uint64_t sum = static_cast<uint64_t>(reserve) + margin;
        const uint64_t target = budgetMb > sum ? budgetMb - sum : 0;
        uint64_t reported = static_cast<uint64_t>(static_cast<double>(target) / kEliteBudgetFactor);
        if (reported < kAutoFloorMb) reported = kAutoFloorMb;
        return reported == kAutoFloorMb;
    };
    if (!pins(reserveCeilMb)) return reserveCeilMb;
    uint32_t lo = 0;
    uint32_t hi = reserveCeilMb;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;
        if (pins(mid)) hi = mid;
        else lo = mid + 1;
    }
    return lo;
}

// The setting as the log names it: "off", "auto", or the number of MB.
// Writes into buf (always NUL-terminated); returns buf.
inline const char* settingText(const Setting& s, char* buf, size_t len) {
    if (buf == nullptr || len == 0) return buf;
    switch (s.mode) {
    case Mode::Auto:
        std::snprintf(buf, len, "auto");
        break;
    case Mode::Fixed:
        std::snprintf(buf, len, "%u", static_cast<unsigned>(s.fixedMb));
        break;
    case Mode::Off:
    default:
        std::snprintf(buf, len, "off");
        break;
    }
    return buf;
}

// One line per GetDesc call that came from the game's exe. Exact format:
// "game vram: call=<n> caller=exe+0x<RVA hex uppercase> real_mb=<R> reported_mb=<P> setting=<settingText> (<why>)"
// plus, for auto only, " budget_mb=<B> others_mb=<O> target_mb=<T> reserve_mb=<R> planned_mb=<L>" appended before " (". B is the
// budget read (the 95% estimate when the OS gave none), O what other programs held ("unknown" when not read), R is
// in.reserveMb, L is plannedBudgetMb().
// Returns the number of characters written (snprintf semantics, clamped to len-1).
inline int formatCallLine(char* buf, size_t len, uint32_t callIndex, uint64_t callerRva,
                          const Setting& s, const Inputs& in, const Result& r) {
    if (buf == nullptr || len == 0) return 0;
    char setting[16];
    settingText(s, setting, sizeof setting);

    char line[320];
    // Each piece appends at n, kept inside line: snprintf returns the length it wanted, not what it wrote.
    size_t n = 0;
    auto clampTo = [&](int wrote) {
        if (wrote > 0) n += static_cast<size_t>(wrote);
        if (n >= sizeof line) n = sizeof line - 1;
    };
    clampTo(std::snprintf(
        line, sizeof line, "game vram: call=%u caller=exe+0x%llX real_mb=%llu reported_mb=%llu setting=%s",
        static_cast<unsigned>(callIndex), static_cast<unsigned long long>(callerRva),
        static_cast<unsigned long long>(in.realDedicatedBytes >> 20),
        static_cast<unsigned long long>(r.reportedBytes >> 20), setting));
    if (s.mode == Mode::Auto) {
        char others[24] = "unknown";
        if (in.othersKnown)
            std::snprintf(others, sizeof others, "%llu", static_cast<unsigned long long>(in.othersBytes >> 20));
        clampTo(std::snprintf(
            line + n, sizeof line - n, " budget_mb=%llu others_mb=%s target_mb=%u reserve_mb=%u planned_mb=%llu",
            static_cast<unsigned long long>(r.budgetBytes >> 20), others, static_cast<unsigned>(r.targetMb),
            static_cast<unsigned>(in.reserveMb),
            static_cast<unsigned long long>(plannedBudgetMb(r.startBytes >> 20, in.flightBudgetMb))));
    }
    clampTo(std::snprintf(line + n, sizeof line - n, " (%s)", r.why));

    if (n >= len) n = len - 1;
    std::memcpy(buf, line, n);
    buf[n] = '\0';
    return static_cast<int>(n);
}

}  // namespace gamevram
}  // namespace edvr
