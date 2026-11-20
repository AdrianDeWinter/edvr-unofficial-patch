#pragma once
// What advanced.game_vram = auto learns about each setup (docs/memory-growth-2026-10-22.md).
//
// Elite sizes its streaming budget once, in its first ~30 s, from the card size our GetDesc hook reports. How much
// room EDVR, the upscaler and the driver need beside that budget differs by OS (Windows or Wine/Proton), upscaler,
// card and driver. So auto starts from a safe default per profile and anti-aliasing class, watches the process's own
// DXGI usage and budget during a session (QueryVideoMemoryInfo, no game addresses), and keeps a reserve per setup
// in %LOCALAPPDATA%\EDVR\game_vram.txt. A session that came close to the budget tightens the reserve; a long,
// settled session with spare room relaxes it, never below the floor.
//
// Nothing here throws. game_vram.h is the floor the reserve must not tighten past.
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "game_vram.h"

namespace edvr {
namespace gamevram {

// Anti-aliasing class: the reserve's starting point depends on how much the AA or upscaler adds.
enum class AaClass : uint32_t { Off = 0, Taa, Upscaler };

// "" or "off" -> Off; "on" or "taa" -> Taa; anything else (dlss, dlaa, fsr, xess, dittosr, ...) -> Upscaler.
// Case-insensitive.
inline AaClass aaClassOf(const std::string& aaMode) {
    const std::string m = lowerCopy(aaMode);
    if (m.empty() || m == "off") return AaClass::Off;
    if (m == "on" || m == "taa") return AaClass::Taa;
    return AaClass::Upscaler;
}

inline const char* aaClassName(AaClass c) {
    static const char* const kNames[] = {"off", "taa", "upscaler"};
    const size_t i = static_cast<size_t>(c);
    return kNames[i < 3 ? i : 0];
}

// Safe starting reserves in MB, before anything is learned, by [VR profile][AA class: Off, Taa, Upscaler]. They err
// towards giving Elite less: a flight at 8192 MB on a 12 GB card looked unchanged.
constexpr uint32_t kDefaultReserveMb[2][3] = {
    {1536, 2048, 3584},  // flat
    {3072, 3584, 4608},  // VR
};

inline uint32_t defaultReserveMb(bool vrProfile, AaClass aa) {
    const size_t i = static_cast<size_t>(aa);
    return kDefaultReserveMb[vrProfile ? 1 : 0][i < 3 ? i : 0];
}

constexpr uint32_t kReserveFloorMb = 768;
constexpr uint32_t kReserveCeilMb  = 16384;
constexpr uint32_t kLearnStepMb    = 256;    // extra added on every tightening
constexpr uint32_t kRelaxMaxMb     = 512;    // most a reserve shrinks per session
constexpr uint32_t kMinSamples     = 300;    // 5 min of 1 s samples before a session teaches anything except a crossing
constexpr uint64_t kPlateauSessionMs = 20ull * 60 * 1000;
constexpr uint64_t kPlateauLeadMs    = 5ull * 60 * 1000;
constexpr size_t   kMaxRecords     = 64;

// "v=<VENDOR hex4> d=<DEVICE hex4> drv=<a.b.c.d|unknown> layer=<windows|wine> prof=<flat|vr> aa=<mode lowercased, empty -> off>"
// driverVersion is the LARGE_INTEGER from IDXGIAdapter::CheckInterfaceSupport: a = bits 63..48, b = 47..32,
// c = 31..16, d = 15..0; 0 -> "unknown". Hex digits uppercase, zero-padded to 4.
inline std::string makeKey(uint32_t vendorId, uint32_t deviceId, uint64_t driverVersion,
                           bool wine, bool vrProfile, const std::string& aaMode) {
    char drv[48];
    if (driverVersion == 0) {
        std::snprintf(drv, sizeof drv, "unknown");
    } else {
        std::snprintf(drv, sizeof drv, "%u.%u.%u.%u",
                      static_cast<unsigned>((driverVersion >> 48) & 0xFFFFull),
                      static_cast<unsigned>((driverVersion >> 32) & 0xFFFFull),
                      static_cast<unsigned>((driverVersion >> 16) & 0xFFFFull),
                      static_cast<unsigned>(driverVersion & 0xFFFFull));
    }
    char head[160];
    std::snprintf(head, sizeof head, "v=%04X d=%04X drv=%s layer=%s prof=%s aa=",
                  static_cast<unsigned>(vendorId & 0xFFFFu), static_cast<unsigned>(deviceId & 0xFFFFu), drv,
                  wine ? "wine" : "windows", vrProfile ? "vr" : "flat");
    std::string aa = lowerCopy(aaMode);
    if (aa.empty()) aa = "off";
    return std::string(head) + aa;
}

// Vendor and device only: the key's identity of the GPU. Driver text can differ when one read failed; two
// different GPUs do not share a vendor and device id.
inline bool sameAdapterIdentity(uint32_t vendorA, uint32_t deviceA, uint32_t vendorB, uint32_t deviceB) {
    return (vendorA & 0xFFFFu) == (vendorB & 0xFFFFu) && (deviceA & 0xFFFFu) == (deviceB & 0xFFFFu);
}

struct Record {
    std::string key;
    uint32_t reserveMb = 0;
    uint32_t sessions = 0;    // sessions that produced an update or a hold
    uint32_t tightened = 0;
    uint32_t relaxed = 0;
};

// File format: UTF-8 text, one record per line:
//   <reserveMb>\t<sessions>\t<tightened>\t<relaxed>\t<key>\n
// Lines starting with '#', empty lines, and malformed lines (wrong field count, a non-number, reserve outside
// [kReserveFloorMb, kReserveCeilMb], empty key) are skipped. CR before LF is tolerated. A later line with the same
// key replaces the earlier one.
inline std::vector<Record> parseStore(const std::string& text) {
    std::vector<Record> out;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        std::string line = text.substr(pos, eol - pos);
        pos = eol + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;

        std::vector<std::string> fields;
        size_t start = 0;
        for (;;) {
            const size_t tab = line.find('\t', start);
            if (tab == std::string::npos) {
                fields.push_back(line.substr(start));
                break;
            }
            fields.push_back(line.substr(start, tab - start));
            start = tab + 1;
        }
        if (fields.size() != 5 || fields[4].empty()) continue;

        Record r;
        uint32_t reserve = 0;
        if (!parseCount(fields[0], reserve) || reserve < kReserveFloorMb || reserve > kReserveCeilMb) continue;
        if (!parseCount(fields[1], r.sessions) || !parseCount(fields[2], r.tightened) ||
            !parseCount(fields[3], r.relaxed))
            continue;
        r.reserveMb = reserve;
        r.key = fields[4];

        bool replaced = false;
        for (Record& existing : out) {
            if (existing.key == r.key) {
                existing = r;
                replaced = true;
                break;
            }
        }
        if (!replaced) out.push_back(r);
    }
    return out;
}

// First line: "# EDVR: what advanced.game_vram = auto learned per setup. Safe to delete.\n", then one line per record in order.
inline std::string serializeStore(const std::vector<Record>& records) {
    std::string out = "# EDVR: what advanced.game_vram = auto learned per setup. Safe to delete.\n";
    for (const Record& r : records) {
        out += std::to_string(r.reserveMb);
        out += '\t';
        out += std::to_string(r.sessions);
        out += '\t';
        out += std::to_string(r.tightened);
        out += '\t';
        out += std::to_string(r.relaxed);
        out += '\t';
        out += r.key;
        out += '\n';
    }
    return out;
}

inline const Record* findRecord(const std::vector<Record>& records, const std::string& key) {
    for (const Record& r : records) {
        if (r.key == key) return &r;
    }
    return nullptr;
}

// Replace the record with the same key (moving it to the end), else append.
// Then, while more than kMaxRecords, erase the first (oldest) record.
inline void upsertRecord(std::vector<Record>& records, const Record& r) {
    for (size_t i = 0; i < records.size(); ++i) {
        if (records[i].key == r.key) {
            records.erase(records.begin() + static_cast<std::ptrdiff_t>(i));
            break;
        }
    }
    records.push_back(r);
    while (records.size() > kMaxRecords) records.erase(records.begin());
}

// A missing store is normal. Error means the file may exist (sharing violation, a read failure, over the cap)
// and must not be replaced by whatever this process managed to parse.
enum class StoreRead : uint32_t { Ok = 0, Missing, Error };

// Win32 ERROR_FILE_NOT_FOUND is 2, ERROR_PATH_NOT_FOUND is 3. Any other open failure is Error.
inline StoreRead classifyStoreOpen(uint32_t win32Error) {
    if (win32Error == 2u || win32Error == 3u) return StoreRead::Missing;
    return StoreRead::Error;
}

// One session's DXGI figures, fed once a second.
class SessionWatch {
public:
    // Ignored when budgetBytes == 0. Usage and budget are this process's
    // local segment (QueryVideoMemoryInfo). nowMs is monotonic.
    void sample(uint64_t nowMs, uint64_t usageBytes, uint64_t budgetBytes) {
        if (budgetBytes == 0) return;
        if (samples_ == 0) firstMs_ = nowMs;
        if (nowMs < firstMs_) return;
        lastMs_ = nowMs;
        lastBudgetBytes_ = budgetBytes;

        const size_t minute = static_cast<size_t>((nowMs - firstMs_) / 60000ull);
        if (minuteMax_.size() <= minute) minuteMax_.resize(minute + 1, 0);
        if (usageBytes > minuteMax_[minute]) minuteMax_[minute] = usageBytes;

        if (samples_ == 0 || usageBytes > peakBytes_) {
            peakBytes_ = usageBytes;
            budgetAtPeakBytes_ = budgetBytes;
        }
        const int64_t headroom = static_cast<int64_t>(budgetBytes) - static_cast<int64_t>(usageBytes);
        if (samples_ == 0 || headroom < minHeadroomBytes_) minHeadroomBytes_ = headroom;
        if (usageBytes > budgetBytes) ++crossings_;
        ++samples_;
    }

    uint32_t samples() const { return samples_; }

    uint64_t durationMs() const { return samples_ < 2 ? 0 : lastMs_ - firstMs_; }

    uint32_t peakUsageMb() const { return static_cast<uint32_t>(peakBytes_ >> 20); }

    uint32_t budgetAtPeakMb() const { return static_cast<uint32_t>(budgetAtPeakBytes_ >> 20); }

    // (budget - usage) in MB, truncated toward zero, minimum over the samples. Truncation is monotone, so the
    // minimum of the bytes converts to the minimum of the MB values.
    int32_t minHeadroomMb() const {
        if (samples_ == 0) return INT32_MAX;
        return static_cast<int32_t>(minHeadroomBytes_ / 1048576);
    }

    uint32_t crossings() const { return crossings_; }

    // The budget of the latest sample kept. Learning caps a tighten against this, not against an earlier dip.
    uint64_t lastBudgetBytes() const { return lastBudgetBytes_; }

    // True when durationMs() >= kPlateauSessionMs and the peak usage had already been reached (to within 3%) at
    // least kPlateauLeadMs before the last sample. Per-minute maxima stand in for the samples: only whole minutes
    // that end at or before that point count, so the test can be up to a minute stricter than the sample-exact rule.
    bool plateaued() const {
        if (durationMs() < kPlateauSessionMs) return false;
        const uint64_t span = lastMs_ - firstMs_;
        if (span < kPlateauLeadMs) return false;
        const uint64_t cutoff = span - kPlateauLeadMs;
        const uint64_t fullMinutes = cutoff / 60000ull;
        uint64_t early = 0;
        for (size_t m = 0; m < minuteMax_.size() && static_cast<uint64_t>(m) < fullMinutes; ++m) {
            early = std::max(early, minuteMax_[m]);
        }
        return early * 100 >= peakBytes_ * 97;
    }

private:
    // Keep per-minute maxima (minute = (t - first) / 60000) so plateaued()
    // needs no per-sample history; plus the scalars above.
    uint32_t samples_ = 0;
    uint64_t firstMs_ = 0;
    uint64_t lastMs_ = 0;
    uint64_t peakBytes_ = 0;
    uint64_t budgetAtPeakBytes_ = 0;
    uint64_t lastBudgetBytes_ = 0;
    int64_t  minHeadroomBytes_ = 0;
    uint32_t crossings_ = 0;
    std::vector<uint64_t> minuteMax_;
};

struct LearnResult {
    enum class Outcome { Tightened, Relaxed, Held, TooShort };

    uint32_t reserveMb = 0;   // what to use next session
    bool changed = false;
    Outcome outcome = Outcome::Held;
    const char* why = "";     // static string
};

// The record to write for this key after a successful re-read. `reread` is that key's record, or null when the
// key is absent. Counters come from the re-read, not from the copy taken at install: a hold here must not put
// the older counters back. A reserve the other session raised above the one this session started from is kept
// when it is the greater, so a hold does not replace that tighten.
inline Record mergeLearned(const std::string& key, uint32_t startedReserveMb, const LearnResult& learned,
                           const Record* reread) {
    Record rec;
    if (reread) rec = *reread;
    rec.key = key;
    const bool tightened = learned.why != nullptr && std::strncmp(learned.why, "tightened", 9) == 0;
    const bool relaxed = learned.why != nullptr && std::strncmp(learned.why, "relaxed", 7) == 0;
    uint32_t reserve = learned.reserveMb;
    if (reread && reread->reserveMb > startedReserveMb && reread->reserveMb > reserve) reserve = reread->reserveMb;
    rec.reserveMb = reserve;
    rec.sessions = (reread ? reread->sessions : 0u) + 1u;
    rec.tightened = (reread ? reread->tightened : 0u) + (tightened ? 1u : 0u);
    rec.relaxed = (reread ? reread->relaxed : 0u) + (relaxed ? 1u : 0u);
    return rec;
}

// Rules, in order (margin = kMarginMb):
//  1. crossings() > 0 or minHeadroomMb() < margin -> tighten:
//     reserve + (margin - minHeadroom) + kLearnStepMb, clamped to the smaller of kReserveCeilMb and the
//     reserve that already pins auto at kAutoFloorMb for lastBudgetBytes(). Once the reserve is there, Elite
//     is already told the floor, so the reserve is held ("held: reported size already at the floor") instead
//     of walking on to the ceiling. A crossing that still changes the reported size still tightens on this sample.
//     why "tightened: came within the margin" (or "tightened: crossed the budget" when crossings() > 0).
//     This applies even to a short session.
//  2. samples() < kMinSamples -> hold (outcome TooShort), why "held: session too short".
//  3. !plateaued() -> hold, why "held: usage still rising or session under 20 min".
//  4. minHeadroom > margin + kLearnStepMb -> relax by
//     min(minHeadroom - margin - kLearnStepMb, kRelaxMaxMb), not below kReserveFloorMb.
//     why "relaxed: room to spare" (changed=false and why "held: at the floor" if already at the floor).
//  5. otherwise hold, why "held: headroom within the margin".
inline LearnResult learn(uint32_t reserveMb, const SessionWatch& s) {
    LearnResult r;
    r.reserveMb = reserveMb;
    const int64_t margin = static_cast<int64_t>(kMarginMb);
    const int64_t minHead = static_cast<int64_t>(s.minHeadroomMb());

    if (s.crossings() > 0 || minHead < margin) {
        // Past this reserve, compute() is already pinned at the floor for the budget just sampled, so another
        // step would not change what Elite is told. A one-second dip must not walk the stored reserve to the ceiling.
        const uint32_t cap = reservePinningFloorMb(s.lastBudgetBytes(), kMarginMb, kReserveCeilMb);
        if (reserveMb >= cap) {
            r.why = "held: reported size already at the floor";
            return r;
        }
        int64_t deficit = margin - minHead;
        if (deficit < 0) deficit = 0;
        int64_t next = static_cast<int64_t>(reserveMb) + deficit + static_cast<int64_t>(kLearnStepMb);
        if (next > static_cast<int64_t>(cap)) next = static_cast<int64_t>(cap);
        if (next > static_cast<int64_t>(kReserveCeilMb)) next = static_cast<int64_t>(kReserveCeilMb);
        r.reserveMb = static_cast<uint32_t>(next);
        r.changed = r.reserveMb != reserveMb;
        r.outcome = LearnResult::Outcome::Tightened;
        r.why = s.crossings() > 0 ? "tightened: crossed the budget" : "tightened: came within the margin";
        return r;
    }
    if (s.samples() < kMinSamples) {
        r.outcome = LearnResult::Outcome::TooShort;
        r.why = "held: session too short";
        return r;
    }
    if (!s.plateaued()) {
        r.why = "held: usage still rising or session under 20 min";
        return r;
    }
    if (minHead > margin + static_cast<int64_t>(kLearnStepMb)) {
        if (reserveMb <= kReserveFloorMb) {
            r.why = "held: at the floor";
            return r;
        }
        int64_t relax = minHead - margin - static_cast<int64_t>(kLearnStepMb);
        if (relax > static_cast<int64_t>(kRelaxMaxMb)) relax = static_cast<int64_t>(kRelaxMaxMb);
        int64_t next = static_cast<int64_t>(reserveMb) - relax;
        if (next < static_cast<int64_t>(kReserveFloorMb)) next = static_cast<int64_t>(kReserveFloorMb);
        r.reserveMb = static_cast<uint32_t>(next);
        r.changed = r.reserveMb != reserveMb;
        r.outcome = LearnResult::Outcome::Relaxed;
        r.why = "relaxed: room to spare";
        return r;
    }
    r.why = "held: headroom within the margin";
    return r;
}

// "game vram: learned for <key>: peak <P> MB of a <B> MB budget, closest <H> MB, crossings <C>, plateau <yes|no>, <N> s -> reserve <old> -> <new> MB next session (<why>)"
// Writes into buf (always NUL-terminated); returns the characters written, clamped to len-1.
inline int formatLearnLine(char* buf, size_t len, const std::string& key, uint32_t oldReserveMb,
                           const SessionWatch& s, const LearnResult& r) {
    if (buf == nullptr || len == 0) return 0;
    char tail[256];
    std::snprintf(tail, sizeof tail,
                  ": peak %u MB of a %u MB budget, closest %d MB, crossings %u, plateau %s, %llu s"
                  " -> reserve %u -> %u MB next session (%s)",
                  static_cast<unsigned>(s.peakUsageMb()), static_cast<unsigned>(s.budgetAtPeakMb()),
                  static_cast<int>(s.minHeadroomMb()), static_cast<unsigned>(s.crossings()),
                  s.plateaued() ? "yes" : "no",
                  static_cast<unsigned long long>(s.durationMs() / 1000),
                  static_cast<unsigned>(oldReserveMb), static_cast<unsigned>(r.reserveMb), r.why);
    const std::string line = "game vram: learned for " + key + tail;
    const size_t n = line.size() < len ? line.size() : len - 1;
    std::memcpy(buf, line.data(), n);
    buf[n] = '\0';
    return static_cast<int>(n);
}

}  // namespace gamevram
}  // namespace edvr
