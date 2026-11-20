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
//             the game, less a learned reserve (game_vram_learn.h) and a margin;
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
    uint32_t reserveMb = 0;           // Auto: EDVR's, the upscaler's and the driver's share to leave free
};

struct Result {
    bool        change = false;     // true: write reportedBytes into DedicatedVideoMemory
    uint64_t    reportedBytes = 0;  // == realDedicatedBytes when change is false
    uint32_t    targetMb = 0;       // auto only: the streaming budget aimed at; else 0
    uint64_t    budgetBytes = 0;    // auto only: the OS budget used (95% of the card when unknown); else 0
    const char* why = "";           // short reason, static string, no spaces at the ends
};

// A LOCAL budget that is not a budget. Under Wine, DXVK without VK_EXT_memory_budget (and likely wined3d) reports the
// budget as the sum of the device-local heaps and the usage as its own allocations only. Such a budget is at least
// DedicatedVideoMemory (the largest heap), never moves, and usage never comes near it, so auto would lower the reserve
// every settled session down to the floor. A WDDM budget on Windows is below the card, so the caller asks only under Wine.
// Known false positive: an AMD card on RADV without Resizable BAR has a second, 256 MB CPU-visible device-local heap; DXVK
// sums both budgets, which can reach the GetDesc size on a quiet desktop although the driver reports a real budget. That
// setup then keeps the safe default reserve and learns nothing. docs/memory-growth-2026-10-22.md has the reasoning.
inline bool budgetIsWholeCard(uint64_t osBudgetBytes, uint64_t realDedicatedBytes) {
    return osBudgetBytes != 0 && realDedicatedBytes != 0 && osBudgetBytes >= realDedicatedBytes;
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
    const uint64_t budgetMb = budgetBytes >> 20;
    const uint64_t reserveAndMargin = static_cast<uint64_t>(in.reserveMb) + kMarginMb;
    uint64_t target = budgetMb > reserveAndMargin ? budgetMb - reserveAndMargin : 0;
    if (target > 0xFFFFFFFFull) target = 0xFFFFFFFFull;
    r.targetMb = static_cast<uint32_t>(target);
    r.budgetBytes = budgetBytes;

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
// plus, for auto only, " budget_mb=<B> target_mb=<T> reserve_mb=<R>" appended before " (". B is the budget used
// (the 95% estimate when the OS gave none), R is in.reserveMb.
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
        clampTo(std::snprintf(
            line + n, sizeof line - n, " budget_mb=%llu target_mb=%u reserve_mb=%u",
            static_cast<unsigned long long>(r.budgetBytes >> 20), static_cast<unsigned>(r.targetMb),
            static_cast<unsigned>(in.reserveMb)));
    }
    clampTo(std::snprintf(line + n, sizeof line - n, " (%s)", r.why));

    if (n >= len) n = len - 1;
    std::memcpy(buf, line, n);
    buf[n] = '\0';
    return static_cast<int>(n);
}

}  // namespace gamevram
}  // namespace edvr
