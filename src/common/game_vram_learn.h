#pragma once
// What advanced.game_vram = auto learns about each setup (docs/memory-growth-2026-10-22.md).
//
// Elite sizes its streaming budget once, in its first ~30 s, from the card size our GetDesc hook reports. Two things then
// decide how much of the OS budget Elite may really use, and auto learns each on its own:
//
//   the reserve      what EDVR, the anti-aliasing engine and the driver hold beside Elite. It differs by engine, so each
//                    engine (Off included) has its own starting point and its own limits (kSlots).
//   the flight budget  what the OS budget falls to in flight, when that is less than game_vram.h's startBudgetMb already
//                    expects from what other programs hold. It is the machine's, not the engine's, so it must not be
//                    charged to the reserve. It is kept as a size, not as a difference: a start that is already low is
//                    then not lowered a second time (plannedBudgetMb).
//
// Both are kept per setup in %LOCALAPPDATA%\EDUP\game_vram.txt and come from the process's own DXGI usage and budget
// (QueryVideoMemoryInfo, no game addresses). One gate decides what counts: a level has to be held for kHoldSeconds of a
// kHoldWindow. Loading and transitions hold 1-2 GB more than flight does for up to 152 s (flights 2026-11-22 to -24), and a
// reserve sized for that starves the streaming for the rest of the session; the window lets them through no more than it
// must. It also looks past the seconds in which Windows trims the process's resident memory. A spill, a window held over the
// budget, teaches at once: a session that spills must not be followed by one that spills the same way.
//
// Nothing here throws. game_vram.h is the floor the reserve must not tighten past.
#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "game_vram.h"
#include "temporal_mode.h"   // temporalModeEnabled, temporalEngineFor: which fix.temporal_aa words run which engine

namespace edvr {
namespace gamevram {

// One slot per anti-aliasing engine: the reserve's starting point, its limits and what is learned are all per slot.
enum class AaSlot : uint32_t { Off = 0, Taa, Dlss, Dlaa, Fsr3, Fsr4, DittoSr, Xess };
constexpr size_t kAaSlots = 8;

// The slot of a fix.temporal_aa value. The words are temporal_mode.h's; a value that runs no engine there is Off here.
inline AaSlot aaSlotOf(const std::string& aaMode) {
    if (!temporalModeEnabled(aaMode)) return AaSlot::Off;
    switch (temporalEngineFor(aaMode)) {
    case TemporalEngine::Nvidia:  return lowerCopy(aaMode) == "dlaa" ? AaSlot::Dlaa : AaSlot::Dlss;
    case TemporalEngine::Amd:     return AaSlot::Fsr3;
    case TemporalEngine::AmdFsr4: return AaSlot::Fsr4;
    case TemporalEngine::Ditto:   return AaSlot::DittoSr;
    case TemporalEngine::Intel:   return AaSlot::Xess;
    case TemporalEngine::Own:     break;
    }
    return AaSlot::Taa;
}

struct ReserveBounds {
    uint32_t startMb;   // before anything is learned
    uint32_t minMb;     // relaxing stops here
    uint32_t maxMb;     // tightening stops here
    uint32_t oldMb;     // what v0.23.0 started from: a record of its is evidence of need only above this
};

// What a slot is: its word in the setup key and the log (fix.temporal_aa's own, as v0.23.0 wrote it, so its records still
// match), and its bounds in MB, all multiples of kReserveStepMb. kMarginMb comes on top of each. One row per AaSlot, in order.
//
// MEASURED (docs/memory-growth-2026-10-22.md, "Per-engine reserves"): VR Off on an 8 GB NVIDIA card at 2688x2880 an eye,
// six sessions: the non-Elite share is 90-610 MB at the median and at most about 1200 MB at the 95th percentile after
// loading. Flat on a 12 GB Arc at a 3264x1836 render, every engine that card runs in turn from one place (2026-11-25, with
// XeSS frame generation and without): Off 1.1-1.6 GB, TAA 1.8-2.4, FSR 3.1 1.5-2.5, DittoSR 2.2-2.8, XeSS 1.1-2.8. Parked
// readings are low: while Elite streams hard the share of XeSS reads 3.4-3.6 GB, of Off 1.6 and of TAA 2.6 (busy station,
// flights 20261125_015932, 020521 and 021144). Off and TAA start at their highest reading less the margin, rounded up; the
// other flat starts are the parked ones raised by about what XeSS rose, as far as the slot's limit allows.
// ESTIMATED: the other flat engines and the rest of VR, from where they sit between those by what they allocate. Each
// learns from there.
struct SlotInfo {
    const char*   word;
    ReserveBounds flat;
    ReserveBounds vr;
};
constexpr SlotInfo kSlots[] = {
    {"off",     {1152, 256, 1536, 1536},  {1024, 256, 1536, 3072}},
    {"on",      {2112, 256, 2560, 2048},  {1280, 384, 1792, 3584}},   // TAA
    {"dlss",    {2048, 512, 3584, 3584},  {2560, 768, 4096, 4608}},
    {"dlaa",    {2048, 512, 3584, 3584},  {2560, 768, 4096, 4608}},
    {"fsr",     {2816, 512, 3584, 3584},  {2560, 768, 4096, 4608}},   // FSR 3.1
    {"fsr4",    {2560, 768, 4096, 3584},  {3072, 768, 4608, 4608}},
    {"dittosr", {2816, 384, 3072, 3584},  {2048, 512, 3072, 4608}},
    {"xess",    {3584, 1536, 4608, 3584}, {3584, 1536, 4608, 4608}},
};
static_assert(sizeof(kSlots) / sizeof(kSlots[0]) == kAaSlots, "one row per AaSlot");

inline const SlotInfo& slotInfo(AaSlot s) {
    const size_t i = static_cast<size_t>(s);
    return kSlots[i < kAaSlots ? i : 0];
}

inline const char* aaSlotWord(AaSlot s) { return slotInfo(s).word; }

inline const ReserveBounds& reserveBounds(bool vrProfile, AaSlot s) {
    return vrProfile ? slotInfo(s).vr : slotInfo(s).flat;
}

// A stored reserve is used only inside its slot's limits.
inline uint32_t clampReserve(const ReserveBounds& b, uint32_t reserveMb) {
    return reserveMb < b.minMb ? b.minMb : (reserveMb > b.maxMb ? b.maxMb : reserveMb);
}

constexpr uint32_t kReserveFloorMb = 256;    // the smallest reserve any slot allows
constexpr uint32_t kReserveCeilMb  = 4608;   // the largest
constexpr uint32_t kOldReserveCeilMb = 16384;   // the largest v0.23.0 stored
constexpr uint32_t kReserveStepMb  = 64;     // reserves and flight budgets move in these, so a pass a minute is not a save a minute
constexpr uint32_t kFlightBudgetCeilMb = kFixedMaxMb;   // a stored flight budget above this is not ours
constexpr uint32_t kChangeMaxMb    = 512;    // most a reserve grows without a spill, and most a flight budget rises, per session
constexpr uint32_t kRelaxBandMb    = 256;    // room beyond the margin before a reserve shrinks
constexpr size_t   kHoldWindow     = 150;    // seconds looked back over, one sample each
constexpr size_t   kHoldSeconds    = 120;    // ...of which this many, not needing to be in a row, make a level held
constexpr uint64_t kGapMs          = 10000;  // samples further apart than this do not belong to one window
constexpr uint32_t kMinSamples     = 300;    // 5 min before a session is judged, unless it spilled
constexpr uint32_t kPlateauSamples = 15 * 60;   // ...and before it can have levelled off
constexpr uint32_t kPlateauLead    = 5 * 60;    // the highest held level must be this old
constexpr size_t   kMaxRecords     = 64;

// "v=<VENDOR hex4> d=<DEVICE hex4> drv=<a.b.c.d|unknown> layer=<windows|wine> prof=<flat|vr> aa=<aaSlotWord>"
// driverVersion is the LARGE_INTEGER from IDXGIAdapter::CheckInterfaceSupport: a = bits 63..48, b = 47..32,
// c = 31..16, d = 15..0; 0 -> "unknown". Hex digits uppercase, zero-padded to 4.
inline std::string makeKey(uint32_t vendorId, uint32_t deviceId, uint64_t driverVersion,
                           bool wine, bool vrProfile, AaSlot slot) {
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
    return std::string(head) + aaSlotWord(slot);
}

// Vendor and device only: the key's identity of the GPU. Driver text can differ when one read failed; two
// different GPUs do not share a vendor and device id.
inline bool sameAdapterIdentity(uint32_t vendorA, uint32_t deviceA, uint32_t vendorB, uint32_t deviceB) {
    return (vendorA & 0xFFFFu) == (vendorB & 0xFFFFu) && (deviceA & 0xFFFFu) == (deviceB & 0xFFFFu);
}

struct Record {
    std::string key;
    uint32_t reserveMb = 0;
    uint32_t flightBudgetMb = 0;   // 0: no fall seen
    uint32_t sessions = 0;    // sessions that produced an update or a hold
    uint32_t tightened = 0;
    uint32_t relaxed = 0;
    bool     old = false;     // a v0.23.0 line: one reserve for everything, no flight budget. adoptRecord() reads it

    // Kept beside the fields, so a new one is not left out of "is this still what I wrote" (SaveBasis).
    bool sameAs(const Record& o) const {
        return key == o.key && reserveMb == o.reserveMb && flightBudgetMb == o.flightBudgetMb && sessions == o.sessions &&
               tightened == o.tightened && relaxed == o.relaxed && old == o.old;
    }
};

// File format: UTF-8 text, one record per line:
//   <reserveMb>\t<flightBudgetMb>\t<sessions>\t<tightened>\t<relaxed>\t<key>\n
// Lines starting with '#', empty lines, and malformed lines (wrong field count, a non-number, reserve outside
// [kReserveFloorMb, kReserveCeilMb], flight budget over kFlightBudgetCeilMb, empty key) are skipped. CR before LF is tolerated. A later
// line with the same key replaces the earlier one.
//
// v0.23.0 wrote five fields, without the flight budget. Such a line (reserve up to kOldReserveCeilMb) is read as an old record, and
// is written back as it was until this key's own session saves over it.
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
        if ((fields.size() != 5 && fields.size() != 6) || fields.back().empty()) continue;

        Record r;
        r.old = fields.size() == 5;
        if (r.old) fields.insert(fields.begin() + 1, "0");   // no flight budget
        if (!parseCount(fields[0], r.reserveMb) || r.reserveMb < kReserveFloorMb ||
            r.reserveMb > (r.old ? kOldReserveCeilMb : kReserveCeilMb))
            continue;
        if (!parseCount(fields[1], r.flightBudgetMb) || r.flightBudgetMb > kFlightBudgetCeilMb ||
            !parseCount(fields[2], r.sessions) || !parseCount(fields[3], r.tightened) || !parseCount(fields[4], r.relaxed))
            continue;
        r.key = fields[5];

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
        if (!r.old) {
            out += std::to_string(r.flightBudgetMb);
            out += '\t';
        }
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

// What the machine's budget looked like when auto was prepared, in MB: the OS's own, and startBudgetMb() of it.
struct StartFacts {
    uint32_t osBudgetMb = 0;
    uint32_t startMb = 0;
};

// The key's record as this session may use it: the reserve inside its slot's limits. False when there is no record. A record
// that was never raised or lowered holds a start and has learned nothing, so the slot's current start stands for it, old
// record or new: a start that a build corrects reaches the setups that only ever held the one before.
//
// An old record's one figure is the reserve and the fall of the budget together, held back from the OS's budget at start,
// and how much of it was fall is not known: it depended on what else ran when it was learned. The share is therefore between
// the figure less today's fall (osBudgetMb - startMb) and the figure itself. The slot's own start stands unless the figure
// shows it wrong. It is too high when even the figure is less. It is too low when the figure less the fall is more, but only
// if the figure was raised to above where that version started: at or below that it is a default nothing has shown to be
// needed. What is then over the slot's limit becomes a flight budget, so it is not lost. Subtracting the fall alone left 1774
// MB of a 3963 learned with an idle desktop, and the flight spilled by 1 GB (20261125_013401).
inline bool adoptRecord(const std::vector<Record>& records, const std::string& key, const ReserveBounds& bounds,
                        const StartFacts& facts, Record& out) {
    const Record* found = findRecord(records, key);
    if (!found) return false;
    out = *found;
    out.old = false;
    if (found->tightened == 0 && found->relaxed == 0) {
        out.reserveMb = bounds.startMb;
        return true;
    }
    if (!found->old) {
        out.reserveMb = clampReserve(bounds, found->reserveMb);
        return true;
    }
    const uint32_t fall = facts.osBudgetMb > facts.startMb ? facts.osBudgetMb - facts.startMb : 0u;
    const uint32_t share = found->reserveMb > fall ? found->reserveMb - fall : 0u;
    const bool needed = found->tightened > 0 && found->reserveMb > bounds.oldMb;
    out.reserveMb = clampReserve(bounds, std::max(needed ? share : 0u, std::min(bounds.startMb, found->reserveMb)));
    if (needed && share > bounds.maxMb && facts.startMb > share - bounds.maxMb)
        out.flightBudgetMb = facts.startMb - (share - bounds.maxMb);
    return true;
}

// The flight budget to start from when a key has none of its own: the newest one of the same machine and profile under
// another engine (the keys differ only after " aa="), because the fall is the machine's. 0 when there is none.
inline uint32_t seedFlightBudgetMb(const std::vector<Record>& records, const std::string& key) {
    const size_t cut = key.rfind(" aa=");
    if (cut == std::string::npos) return 0;
    for (size_t i = records.size(); i-- > 0;) {
        if (records[i].flightBudgetMb != 0 && records[i].key.compare(0, cut + 4, key, 0, cut + 4) == 0)
            return records[i].flightBudgetMb;
    }
    return 0;
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
    // Ignored when a budget is 0. Usage and budget are this process's local segment (QueryVideoMemoryInfo).
    // plannedBudgetBytes is the budget auto sized Elite from (plannedBudgetMb). It is the same for every sample of a
    // session. nowMs is monotonic.
    void sample(uint64_t nowMs, uint64_t usageBytes, uint64_t budgetBytes, uint64_t plannedBudgetBytes) {
        if (budgetBytes == 0 || plannedBudgetBytes == 0) return;
        if (samples_ == 0) firstMs_ = nowMs;
        if (samples_ != 0 && nowMs < lastMs_) return;
        if (samples_ != 0 && nowMs - lastMs_ > kGapMs) filled_ = 0;
        lastMs_ = nowMs;
        plannedBytes_ = plannedBudgetBytes;

        if (samples_ == 0 || usageBytes > peakBytes_) {
            peakBytes_ = usageBytes;
            budgetAtPeakBytes_ = budgetBytes;
        }
        const int64_t headroom = static_cast<int64_t>(budgetBytes) - static_cast<int64_t>(usageBytes);
        if (samples_ == 0 || headroom < minHeadroomBytes_) minHeadroomBytes_ = headroom;
        if (usageBytes > budgetBytes) ++crossings_;

        const size_t at = samples_ % kHoldWindow;
        usage_[at] = static_cast<int64_t>(usageBytes);
        budget_[at] = static_cast<int64_t>(budgetBytes);
        over_[at] = -headroom;
        ++samples_;
        if (filled_ < kHoldWindow) ++filled_;
        if (filled_ < kHoldWindow) {
            spilling_ = false;
            return;
        }

        // Use at or above this, and the budget at or under that, for kHoldSeconds of the window.
        const int64_t heldUsage = ranked(usage_, kHoldWindow - kHoldSeconds);
        const int64_t heldBudget = ranked(budget_, kHoldSeconds - 1);
        if (!held_ || heldUsage > heldUsageMax_) heldUsageMax_ = heldUsage;
        if (!held_ || heldBudget < heldBudgetMin_) heldBudgetMin_ = heldBudget;
        held_ = true;
        const bool spilling = ranked(over_, kHoldWindow - kHoldSeconds) > 0;
        if (spilling && !spilling_) ++spills_;
        spilling_ = spilling;

        const size_t minute = (samples_ - 1) / 60;
        if (minuteMax_.size() <= minute) minuteMax_.resize(minute + 1, 0);
        minuteMax_[minute] = std::max(minuteMax_[minute], heldUsage);
    }

    uint32_t samples() const { return samples_; }

    uint64_t durationMs() const { return samples_ < 2 ? 0 : lastMs_ - firstMs_; }

    uint32_t peakUsageMb() const { return static_cast<uint32_t>(peakBytes_ >> 20); }

    uint32_t budgetAtPeakMb() const { return static_cast<uint32_t>(budgetAtPeakBytes_ >> 20); }

    // (budget - usage) in MB, truncated toward zero, minimum over all samples. For the log.
    int32_t minHeadroomMb() const {
        if (samples_ == 0) return INT32_MAX;
        return static_cast<int32_t>(minHeadroomBytes_ / 1048576);
    }

    // Samples over the budget, in a row or not.
    uint32_t crossings() const { return crossings_; }

    // Windows held over the budget, each counted once however long it goes on.
    uint32_t spills() const { return spills_; }

    // False until a whole window was seen: the next three are then 0.
    bool held() const { return held_; }
    // The highest use and the lowest budget that were held.
    uint32_t heldUsageMb() const { return static_cast<uint32_t>(heldUsageMax_ >> 20); }
    uint32_t heldBudgetMb() const { return static_cast<uint32_t>(heldBudgetMin_ >> 20); }
    // The room that use left to the planned budget, so a fall of the OS budget, which the flight budget answers for, is not
    // read as EDVR's share.
    int64_t heldHeadroomMb() const {
        return held_ ? (static_cast<int64_t>(plannedBytes_) - heldUsageMax_) / 1048576 : 0;
    }

    uint64_t plannedBudgetBytes() const { return plannedBytes_; }

    // True when the highest held use had already been reached (to within 3%) at least kPlateauLead seconds before the last
    // sample, in a session of kPlateauSamples or more. Per-minute maxima stand in for the samples: only whole minutes that
    // end at or before that point count, so the test can be up to a minute stricter than the sample-exact rule.
    bool plateaued() const {
        if (samples_ < kPlateauSamples) return false;
        const uint32_t fullMinutes = (samples_ - kPlateauLead) / 60;
        int64_t early = 0;
        for (size_t m = 0; m < minuteMax_.size() && m < fullMinutes; ++m) early = std::max(early, minuteMax_[m]);
        return early * 100 >= heldUsageMax_ * 97;
    }

private:
    using Window = std::array<int64_t, kHoldWindow>;

    // The value with `rank` values of the window under it. A copy of 150 values, once a second.
    static int64_t ranked(Window w, size_t rank) {
        std::nth_element(w.begin(), w.begin() + static_cast<std::ptrdiff_t>(rank), w.end());
        return w[rank];
    }

    uint32_t samples_ = 0;
    uint64_t firstMs_ = 0;
    uint64_t lastMs_ = 0;
    uint64_t plannedBytes_ = 0;
    uint64_t peakBytes_ = 0;
    uint64_t budgetAtPeakBytes_ = 0;
    int64_t  minHeadroomBytes_ = 0;
    uint32_t crossings_ = 0;
    Window   usage_{};               // the last kHoldWindow samples, oldest overwritten; order does not matter
    Window   budget_{};
    Window   over_{};                // usage - budget
    size_t   filled_ = 0;            // samples since the last gap, to kHoldWindow
    bool     held_ = false;
    int64_t  heldUsageMax_ = 0;
    int64_t  heldBudgetMin_ = 0;
    bool     spilling_ = false;
    uint32_t spills_ = 0;
    std::vector<int64_t> minuteMax_;   // of the held use, so plateaued() needs no per-sample history
};

static_assert(kRelaxBandMb / 2 >= kReserveStepMb, "half of any spare room that relaxes is at least one step");
static_assert(kHoldSeconds < kHoldWindow && kPlateauSamples > kPlateauLead,
              "kHoldSeconds ranks inside the window, and plateaued() subtracts its lead");

struct LearnResult {
    enum class Outcome { Tightened, Relaxed, Held, TooShort };

    uint32_t reserveMb = 0;   // what to use next session
    uint32_t flightBudgetMb = 0;   // likewise; 0 = no fall seen
    Outcome outcome = Outcome::Held;   // of the reserve
    const char* why = "";     // of the reserve; static string
};

// The record to write for this key after a successful re-read. `reread` is that key's record, or null when the
// key is absent. Counters come from the re-read, not from the copy taken at install: a hold here must not put
// the older counters back. A reserve the other session raised above the one this session started from is kept
// when it is the greater, so a hold does not replace that tighten; a lower flight budget likewise.
//
// This session's own earlier save is not another session: the caller passes the record from before that save
// (SaveBasis), so a session counts once however often it saves, and its later judgement replaces its earlier one.
inline Record mergeLearned(const std::string& key, uint32_t startedReserveMb, uint32_t startedFlightMb,
                           const LearnResult& learned, const Record* reread) {
    Record rec;
    if (reread) rec = *reread;
    rec.key = key;
    rec.old = false;
    const bool tightened = learned.outcome == LearnResult::Outcome::Tightened;
    const bool relaxed = learned.outcome == LearnResult::Outcome::Relaxed;
    uint32_t reserve = learned.reserveMb;
    if (reread && reread->reserveMb > startedReserveMb && reread->reserveMb > reserve) reserve = reread->reserveMb;
    rec.reserveMb = reserve;
    uint32_t flight = learned.flightBudgetMb;
    const auto lower = [](uint32_t a, uint32_t b) { return a != 0 && (b == 0 || a < b); };   // 0 is no limit at all
    if (reread && lower(reread->flightBudgetMb, startedFlightMb) && lower(reread->flightBudgetMb, flight))
        flight = reread->flightBudgetMb;
    rec.flightBudgetMb = flight;
    rec.sessions = (reread ? reread->sessions : 0u) + 1u;
    rec.tightened = (reread ? reread->tightened : 0u) + (tightened ? 1u : 0u);
    rec.relaxed = (reread ? reread->relaxed : 0u) + (relaxed ? 1u : 0u);
    return rec;
}

// The record a save merges onto. `found` is the key's record as just re-read, or null. While it is still the one this
// session wrote last, the basis is what was there before that write (null when the key was absent); once another session
// has written over it, the basis is that session's record.
class SaveBasis {
public:
    const Record* basisFor(const Record* found) {
        const bool ours = landed_ && found && found->sameAs(last_);
        if (!ours) {
            had_ = found != nullptr;
            if (found) before_ = *found;
        }
        return had_ ? &before_ : nullptr;
    }
    void landed(const Record& written) {
        last_ = written;
        landed_ = true;
    }

private:
    Record before_;
    Record last_;
    bool had_ = false;
    bool landed_ = false;
};

inline uint32_t roundUpMb(int64_t mb) {
    if (mb <= 0) return 0;
    const int64_t step = static_cast<int64_t>(kReserveStepMb);
    return static_cast<uint32_t>((mb + step - 1) / step * step);
}

// A session that can be judged: long enough, or one that spilled, however short.
inline bool judged(const SessionWatch& s) { return s.held() && (s.samples() >= kMinSamples || s.spills() > 0); }

// The flight budget. The lowest OS budget that was held is what it falls to; a fall under kReserveStepMb from startMb, the
// one Elite was sized from, is jitter. A fall to less than the figure in use is taken at once, down to flightBudgetFloorMb.
// The figure rises again only after a session without a spill that levelled off above it, kChangeMaxMb at a time and not
// past what that session saw, because a session that kept clear of the budget may simply not have met the fall. 0 is "no
// fall seen".
inline uint32_t learnFlightBudget(uint32_t flightMb, uint32_t startMb, const SessionWatch& s) {
    if (startMb == 0 || !judged(s)) return flightMb;
    const uint32_t low = s.heldBudgetMb();
    const uint32_t floor = static_cast<uint32_t>(flightBudgetFloorMb(startMb));
    const bool fell = low + kReserveStepMb <= startMb;
    const uint32_t seen = std::max(low / kReserveStepMb * kReserveStepMb, floor);
    if (fell && (flightMb == 0 || seen <= flightMb)) return seen;
    if (flightMb == 0 || s.spills() > 0 || !s.plateaued()) return flightMb;
    return std::max(flightMb, std::min(std::max(flightMb, floor) + kChangeMaxMb, seen));
}

// The reserve's rules, in order, within the slot's bounds (margin = kMarginMb, low = heldHeadroomMb()):
//  1. !judged() -> hold (outcome TooShort), why "held: session too short".
//  2. low < margin -> tighten by margin - low, rounded up to kReserveStepMb, at most kChangeMaxMb unless it spilled, and not
//     past the smaller of the slot's maximum and the reserve that already pins auto at kAutoFloorMb for the planned budget.
//     why "tightened: less room than the margin" or "tightened: over the budget". At that limit: hold, "held: reported size
//     already at the floor" or "held: at this engine's limit".
//  3. spilled -> hold, why "held: the budget fell under what was planned". learnFlightBudget answers for it.
//  4. !plateaued() -> hold, why "held: usage still rising or session under 15 min".
//  5. crossings() >= kHoldSeconds -> hold, why "held: over the budget for two minutes in all": no window was held over it,
//     but a session that met the budget that long in bursts has no room to give back.
//  6. low > margin + kRelaxBandMb -> relax by half the spare room, low - margin, rounded down to kReserveStepMb, not below
//     the slot's minimum: a large step far from the mark and small ones near it. why "relaxed: room to spare" ("held: at
//     the minimum" when already there).
//  7. otherwise hold, why "held: headroom within the margin".
inline LearnResult learn(uint32_t reserveMb, uint32_t flightMb, uint32_t startMb, const ReserveBounds& bounds,
                         const SessionWatch& s) {
    LearnResult r;
    r.reserveMb = reserveMb;
    r.flightBudgetMb = learnFlightBudget(flightMb, startMb, s);
    if (!judged(s)) {
        r.outcome = LearnResult::Outcome::TooShort;
        r.why = "held: session too short";
        return r;
    }
    const int64_t margin = static_cast<int64_t>(kMarginMb);
    const bool spilled = s.spills() > 0;
    const int64_t low = s.heldHeadroomMb();

    if (low < margin) {
        // Past this reserve, compute() is already pinned at the floor for the budget in use, so another step would not
        // change what Elite is told.
        const uint32_t cap = reservePinningFloorMb(s.plannedBudgetBytes(), kMarginMb, bounds.maxMb);
        if (reserveMb >= cap) {
            r.why = reserveMb >= bounds.maxMb ? "held: at this engine's limit" : "held: reported size already at the floor";
            return r;
        }
        const uint32_t need = roundUpMb(margin - low);
        r.reserveMb = std::min(reserveMb + (spilled ? need : std::min(need, kChangeMaxMb)), cap);
        r.outcome = LearnResult::Outcome::Tightened;
        r.why = spilled ? "tightened: over the budget" : "tightened: less room than the margin";
        return r;
    }
    if (spilled) {
        r.why = "held: the budget fell under what was planned";
        return r;
    }
    if (!s.plateaued()) {
        r.why = "held: usage still rising or session under 15 min";
        return r;
    }
    if (s.crossings() >= kHoldSeconds) {
        r.why = "held: over the budget for two minutes in all";
        return r;
    }
    const int64_t spare = low - margin;
    if (spare > static_cast<int64_t>(kRelaxBandMb)) {
        if (reserveMb <= bounds.minMb) {
            r.why = "held: at the minimum";
            return r;
        }
        const uint32_t step = static_cast<uint32_t>(spare / 2) / kReserveStepMb * kReserveStepMb;
        r.reserveMb = reserveMb > bounds.minMb + step ? reserveMb - step : bounds.minMb;
        r.outcome = LearnResult::Outcome::Relaxed;
        r.why = "relaxed: room to spare";
        return r;
    }
    r.why = "held: headroom within the margin";
    return r;
}

// "game vram: learned for <key>: peak <P> MB of a <B> MB budget, closest <H> MB, crossings <C>; held <U> MB, <L> MB under the
//  planned <PB> MB, budget held <LB> MB, spills <N>, plateau <yes|no>, <S> s
//  -> reserve <old> -> <new> MB, flight budget <old> -> <new> MB next session (<why>)"
// Writes into buf (always NUL-terminated); returns the characters written, clamped to len-1.
inline int formatLearnLine(char* buf, size_t len, const std::string& key, uint32_t oldReserveMb, uint32_t oldFlightMb,
                           const SessionWatch& s, const LearnResult& r) {
    if (buf == nullptr || len == 0) return 0;
    char tail[384];
    std::snprintf(tail, sizeof tail,
                  ": peak %u MB of a %u MB budget, closest %d MB, crossings %u; held %u MB, %d MB under the planned %u MB,"
                  " budget held %u MB, spills %u, plateau %s, %llu s"
                  " -> reserve %u -> %u MB, flight budget %u -> %u MB next session (%s)",
                  static_cast<unsigned>(s.peakUsageMb()), static_cast<unsigned>(s.budgetAtPeakMb()),
                  static_cast<int>(s.minHeadroomMb()), static_cast<unsigned>(s.crossings()),
                  static_cast<unsigned>(s.heldUsageMb()), static_cast<int>(s.heldHeadroomMb()),
                  static_cast<unsigned>(s.plannedBudgetBytes() >> 20), static_cast<unsigned>(s.heldBudgetMb()),
                  static_cast<unsigned>(s.spills()), s.plateaued() ? "yes" : "no",
                  static_cast<unsigned long long>(s.durationMs() / 1000),
                  static_cast<unsigned>(oldReserveMb), static_cast<unsigned>(r.reserveMb),
                  static_cast<unsigned>(oldFlightMb), static_cast<unsigned>(r.flightBudgetMb), r.why);
    const std::string line = "game vram: learned for " + key + tail;
    const size_t n = line.size() < len ? line.size() : len - 1;
    std::memcpy(buf, line.data(), n);
    buf[n] = '\0';
    return static_cast<int>(n);
}

}  // namespace gamevram
}  // namespace edvr
