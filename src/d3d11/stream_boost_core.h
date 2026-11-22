#pragma once
// Pure, platform-agnostic hardware scaling and runtime pump-budget decisions
// for Elite Dangerous's asset streaming boost (build 334925).
//
// No Win32 headers, no global state: unit-tested directly by stream_boost_test.

#include <algorithm>
#include <cstdint>

namespace edvr {
namespace stream_boost {

// Stock Elite Dangerous (build 334925) streaming parameters.
constexpr uint8_t kStockStreamLoadCap = 30u;     // cmp rsi, 0x1e at StreamLoad (exe+0x9E8938)
constexpr uint8_t kMaxStreamLoadCap = 127u;      // max signed imm8 for `cmp rsi, imm8`
constexpr uint32_t kStockPoolCap = 16u;          // SetMaxConcurrentStreams(16) at startup
constexpr uint32_t kMaxPoolCap = 32u;
constexpr uint32_t kStockPumpTimeUs = 1000u;     // active gameplay clamp on fRes pump (exe+0x7F6EB0)
constexpr uint32_t kStockPumpMaxIter = 64u;

// Backlog thresholds to engage the render-thread completion pump boost.
constexpr uint32_t kBoostWantLoadThreshold = 64u;
constexpr uint32_t kBoostPendingThreshold = 8u;

// Frame-time thresholds (ms) where the render-thread pump boost steps down to
// avoid amplifying stutter on tight frames.
constexpr double kFlatTightFrameMs = 20.0;
constexpr double kVrTightFrameMs = 11.0;

// Storage bus types matching Windows STORAGE_BUS_TYPE enum values.
constexpr uint32_t kBusTypeUsb = 7u;
constexpr uint32_t kBusTypeSd = 12u;
constexpr uint32_t kBusTypeMmc = 13u;
constexpr uint32_t kBusTypeNvme = 17u;

// RAM tier boundaries in GiB, with headroom for UMA/BIOS reservations
// (e.g. a 16 GB APU/Steam Deck typically reports 14.5-15.5 GiB usable).
constexpr uint64_t kGiB = 1024ull * 1024ull * 1024ull;
constexpr uint64_t kRamMidMinBytes = 10ull * kGiB;
constexpr uint64_t kRamHighMinBytes = 22ull * kGiB;

enum class StorageClass : uint8_t {
    Hdd = 0,       // IncursSeekPenalty == true -> stand down completely
    FlashSsd = 1,  // Solid-state over USB / SD / MMC
    SataSsd = 2,   // Solid-state over SATA / SAS / RAID / virtual / other non-NVMe
    NvmeSsd = 3,   // Solid-state over NVMe
    Unknown = 4,   // IOCTL unavailable (e.g. Wine/Proton or opaque controller)
};

inline const char* storageClassName(StorageClass cls) noexcept {
    switch (cls) {
        case StorageClass::Hdd:      return "hdd";
        case StorageClass::FlashSsd: return "flash_ssd";
        case StorageClass::SataSsd:  return "sata_ssd";
        case StorageClass::NvmeSsd:  return "nvme";
        case StorageClass::Unknown:  return "unknown";
    }
    return "unknown";
}

inline StorageClass classifyStorage(bool hasSeekPenalty, bool incursSeekPenalty,
                                    bool hasTrim, bool trimEnabled,
                                    uint32_t busType) noexcept {
    if (hasSeekPenalty) {
        if (incursSeekPenalty) return StorageClass::Hdd;
        if (busType == kBusTypeNvme) return StorageClass::NvmeSsd;
        if (busType == kBusTypeUsb || busType == kBusTypeSd || busType == kBusTypeMmc) {
            return StorageClass::FlashSsd;
        }
        return StorageClass::SataSsd;
    }
    if (hasTrim && trimEnabled) {
        if (busType == kBusTypeNvme) return StorageClass::NvmeSsd;
        if (busType == kBusTypeUsb || busType == kBusTypeSd || busType == kBusTypeMmc) {
            return StorageClass::FlashSsd;
        }
        return StorageClass::SataSsd;
    }
    return StorageClass::Unknown;
}

struct SystemProfile {
    StorageClass storage = StorageClass::Unknown;
    bool underWine = false;
    uint32_t logicalCpus = 0u;
    uint64_t totalRamBytes = 0u;
};

struct StreamBoostPlan {
    bool active = false;
    const char* standDownReason = nullptr;
    uint8_t streamLoadCap = kStockStreamLoadCap;
    uint32_t poolCap = kStockPoolCap;
    uint32_t flatPumpTimeUs = kStockPumpTimeUs;
    uint32_t flatPumpMaxIter = kStockPumpMaxIter;
    uint32_t flatTightTimeUs = kStockPumpTimeUs;
    uint32_t flatTightMaxIter = kStockPumpMaxIter;
    uint32_t vrPumpTimeUs = kStockPumpTimeUs;
    uint32_t vrPumpMaxIter = kStockPumpMaxIter;
    uint32_t vrTightTimeUs = kStockPumpTimeUs;
    uint32_t vrTightMaxIter = kStockPumpMaxIter;
};

inline StreamBoostPlan computeStreamBoostPlan(const SystemProfile& sys) noexcept {
    StreamBoostPlan plan{};
    if (sys.storage == StorageClass::Hdd) {
        plan.active = false;
        plan.standDownReason = "spinning_hdd";
        return plan;
    }

    // 1. Storage concurrency ceiling.
    uint8_t sCap = 64u;
    uint32_t sPool = 20u;
    switch (sys.storage) {
        case StorageClass::NvmeSsd:
            sCap = kMaxStreamLoadCap;
            sPool = kMaxPoolCap;
            break;
        case StorageClass::SataSsd:
            sCap = 96u;
            sPool = 20u;
            break;
        case StorageClass::FlashSsd:
            sCap = 64u;
            sPool = kStockPoolCap;
            break;
        case StorageClass::Unknown:
            if (sys.underWine) {
                sCap = 96u;
                sPool = 24u;
            } else {
                sCap = 64u;
                sPool = 20u;
            }
            break;
        case StorageClass::Hdd:
            break;
    }

    // 2. CPU logical-core concurrency ceiling.
    uint8_t cCap = 48u;
    uint32_t cPool = kStockPoolCap;
    if (sys.logicalCpus >= 16u) {
        cCap = kMaxStreamLoadCap;
        cPool = kMaxPoolCap;
    } else if (sys.logicalCpus >= 10u) {
        cCap = 112u;
        cPool = 24u;
    } else if (sys.logicalCpus >= 6u) {
        cCap = 96u;
        cPool = 20u;
    }

    // 3. System RAM concurrency ceiling (guards staging memory against paging).
    uint8_t rCap = 48u;
    uint32_t rPool = kStockPoolCap;
    if (sys.totalRamBytes >= kRamHighMinBytes) {
        rCap = kMaxStreamLoadCap;
        rPool = kMaxPoolCap;
    } else if (sys.totalRamBytes >= kRamMidMinBytes) {
        rCap = 96u;
        rPool = 24u;
    }

    const uint8_t chosenCap = std::min(sCap, std::min(cCap, rCap));
    const uint32_t chosenPool = std::min(sPool, std::min(cPool, rPool));

    plan.streamLoadCap = std::max(kStockStreamLoadCap, std::min(kMaxStreamLoadCap, chosenCap));
    plan.poolCap = std::max(kStockPoolCap, std::min(kMaxPoolCap, chosenPool));

    // 4. Render-thread completion pump budgets (flat vs VR, normal vs tight frame).
    if (sys.logicalCpus >= 10u) {
        plan.flatPumpTimeUs = 3500u;
        plan.flatPumpMaxIter = 256u;
    } else if (sys.logicalCpus >= 6u) {
        plan.flatPumpTimeUs = 3000u;
        plan.flatPumpMaxIter = 192u;
    } else {
        plan.flatPumpTimeUs = 2000u;
        plan.flatPumpMaxIter = 128u;
    }
    if (sys.storage == StorageClass::FlashSsd && plan.flatPumpTimeUs > 2500u) {
        plan.flatPumpTimeUs = 2500u;
        plan.flatPumpMaxIter = std::min(plan.flatPumpMaxIter, 192u);
    }

    plan.flatTightTimeUs = std::min(plan.flatPumpTimeUs, 2000u);
    plan.flatTightMaxIter = std::min(plan.flatPumpMaxIter, 128u);

    plan.vrPumpTimeUs = 2000u;
    plan.vrPumpMaxIter = 128u;
    plan.vrTightTimeUs = 1500u;
    plan.vrTightMaxIter = 96u;

    plan.active = true;
    plan.standDownReason = nullptr;
    return plan;
}

struct PumpInput {
    uint32_t origTimeUs = kStockPumpTimeUs;
    uint32_t origMaxIter = kStockPumpMaxIter;
    uint32_t wantLoad = 0u;
    uint32_t pending = 0u;
    bool overBudgetEvicting = false;
    bool flatProfile = true;
    double frameMsEma = 0.0;
};

struct PumpBudget {
    uint32_t timeUs = kStockPumpTimeUs;
    uint32_t maxIter = kStockPumpMaxIter;
    bool boosted = false;
    bool evictBraked = false;
    bool tightThrottled = false;
};

inline PumpBudget selectPumpBudget(const StreamBoostPlan& plan, const PumpInput& in) noexcept {
    PumpBudget out{};
    out.timeUs = in.origTimeUs;
    out.maxIter = in.origMaxIter;

    if (!plan.active) return out;

    const bool hasBacklog = (in.wantLoad > kBoostWantLoadThreshold) ||
                            (in.pending > kBoostPendingThreshold);
    if (!hasBacklog) return out;

    if (in.overBudgetEvicting) {
        out.evictBraked = true;
        return out;
    }

    uint32_t targetTimeUs = kStockPumpTimeUs;
    uint32_t targetMaxIter = kStockPumpMaxIter;
    if (in.flatProfile) {
        const bool tight = (in.frameMsEma > kFlatTightFrameMs);
        out.tightThrottled = tight;
        targetTimeUs = tight ? plan.flatTightTimeUs : plan.flatPumpTimeUs;
        targetMaxIter = tight ? plan.flatTightMaxIter : plan.flatPumpMaxIter;
    } else {
        const bool tight = (in.frameMsEma > kVrTightFrameMs);
        out.tightThrottled = tight;
        targetTimeUs = tight ? plan.vrTightTimeUs : plan.vrPumpTimeUs;
        targetMaxIter = tight ? plan.vrTightMaxIter : plan.vrPumpMaxIter;
    }

    // Never shrink Elite's own budget (e.g. loading screens already allow 10000 us / 1024).
    if (targetTimeUs > out.timeUs) out.timeUs = targetTimeUs;
    if (targetMaxIter > out.maxIter) out.maxIter = targetMaxIter;
    out.boosted = (out.timeUs > in.origTimeUs) || (out.maxIter > in.origMaxIter);
    return out;
}

inline double updateFrameMsEma(double prevEma, double sampleMs) noexcept {
    if (sampleMs <= 0.0 || sampleMs > 250.0) return prevEma;
    if (prevEma <= 0.0) return sampleMs;
    return prevEma * 0.90 + sampleMs * 0.10;
}

}  // namespace stream_boost
}  // namespace edvr
