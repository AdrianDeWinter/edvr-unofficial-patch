// Offline unit tests for src/d3d11/stream_boost_core.h:
//   - storage classification (HDD, NVMe, SATA/SAS, USB/SD/MMC, TRIM fallback, unknown)
//   - hardware-tier scaling across high-end desktop, Steam Deck/Proton, SATA mid-range,
//     flash drive, 4-core/8GB, low-RAM clamp, and spinning HDD stand-down
//   - render-thread pump budget selection (flat vs VR, normal vs tight frame,
//     steady-state stand-down, loading-screen non-shrinking, VRAM eviction brake)
//   - frame-time EMA smoothing and outlier rejection

#include "../../src/d3d11/stream_boost_core.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

using namespace edvr::stream_boost;

int g_failed = 0;
int g_passed = 0;

void check(bool cond, const char* expr, int line) {
    if (cond) {
        ++g_passed;
    } else {
        ++g_failed;
        std::fprintf(stderr, "FAIL line %d: %s\n", line, expr);
    }
}

#define CHECK(expr) check((expr), #expr, __LINE__)

void testClassifyStorage() {
    // Seek penalty == true is always HDD regardless of bus type.
    CHECK(classifyStorage(true, true, false, false, 11u) == StorageClass::Hdd);
    CHECK(classifyStorage(true, true, true, true, kBusTypeNvme) == StorageClass::Hdd);
    CHECK(classifyStorage(true, true, false, false, kBusTypeUsb) == StorageClass::Hdd);

    // Seek penalty == false differentiates NVMe, SATA/SAS, and USB/SD/MMC.
    CHECK(classifyStorage(true, false, false, false, kBusTypeNvme) == StorageClass::NvmeSsd);
    CHECK(classifyStorage(true, false, false, false, 11u) == StorageClass::SataSsd);
    CHECK(classifyStorage(true, false, false, false, 10u) == StorageClass::SataSsd);
    CHECK(classifyStorage(true, false, false, false, 0u) == StorageClass::SataSsd);
    CHECK(classifyStorage(true, false, false, false, kBusTypeUsb) == StorageClass::FlashSsd);
    CHECK(classifyStorage(true, false, false, false, kBusTypeSd) == StorageClass::FlashSsd);
    CHECK(classifyStorage(true, false, false, false, kBusTypeMmc) == StorageClass::FlashSsd);

    // TRIM fallback when seek-penalty IOCTL is unavailable.
    CHECK(classifyStorage(false, false, true, true, kBusTypeNvme) == StorageClass::NvmeSsd);
    CHECK(classifyStorage(false, false, true, true, 11u) == StorageClass::SataSsd);
    CHECK(classifyStorage(false, false, true, true, kBusTypeUsb) == StorageClass::FlashSsd);
    CHECK(classifyStorage(false, false, true, false, kBusTypeNvme) == StorageClass::Unknown);
    CHECK(classifyStorage(false, false, false, false, 0u) == StorageClass::Unknown);
}

void testSystemPlans() {
    // 1. Spinning HDD stands down completely, even on a 32-thread / 64 GiB machine.
    {
        SystemProfile hdd{};
        hdd.storage = StorageClass::Hdd;
        hdd.logicalCpus = 32u;
        hdd.totalRamBytes = 64ull * kGiB;
        const StreamBoostPlan p = computeStreamBoostPlan(hdd);
        CHECK(!p.active);
        CHECK(p.standDownReason != nullptr && std::strcmp(p.standDownReason, "spinning_hdd") == 0);
        CHECK(p.streamLoadCap == kStockStreamLoadCap);
        CHECK(p.poolCap == kStockPoolCap);
        CHECK(p.flatPumpTimeUs == kStockPumpTimeUs);
        CHECK(p.vrPumpTimeUs == kStockPumpTimeUs);
    }

    // 2. High-end NVMe desktop (32 logical CPUs, 64 GiB).
    {
        SystemProfile hi{};
        hi.storage = StorageClass::NvmeSsd;
        hi.logicalCpus = 32u;
        hi.totalRamBytes = 64ull * kGiB;
        const StreamBoostPlan p = computeStreamBoostPlan(hi);
        CHECK(p.active);
        CHECK(p.standDownReason == nullptr);
        CHECK(p.streamLoadCap == 256u);
        CHECK(p.poolCap == 63u);
        CHECK(p.flatPumpTimeUs == 4000u);
        CHECK(p.flatPumpMaxIter == 256u);
        CHECK(p.flatTightTimeUs == 2000u);
        CHECK(p.vrPumpTimeUs == 2000u);
        CHECK(p.vrTightTimeUs == 1500u);
    }

    // 3. Steam Deck under Proton (unknown IOCTL, Wine=true, 8 logical CPUs, ~14.8 GiB UMA).
    {
        SystemProfile deck{};
        deck.storage = StorageClass::Unknown;
        deck.underWine = true;
        deck.logicalCpus = 8u;
        deck.totalRamBytes = 15ull * kGiB;
        const StreamBoostPlan p = computeStreamBoostPlan(deck);
        CHECK(p.active);
        CHECK(p.streamLoadCap == 160u);
        CHECK(p.poolCap == 24u);
        CHECK(p.flatPumpTimeUs == 3000u);
        CHECK(p.flatPumpMaxIter == 192u);
        CHECK(p.vrPumpTimeUs == 2000u);
    }

    // 4. Mid-range 6C12T + 16 GiB + SATA SSD.
    {
        SystemProfile mid{};
        mid.storage = StorageClass::SataSsd;
        mid.logicalCpus = 12u;
        mid.totalRamBytes = 16ull * kGiB;
        const StreamBoostPlan p = computeStreamBoostPlan(mid);
        CHECK(p.active);
        CHECK(p.streamLoadCap == 192u);
        CHECK(p.poolCap == 24u);
        CHECK(p.flatPumpTimeUs == 3500u);
    }

    // 5. USB / SD flash storage on a fast desktop stays capped by storage ceiling.
    {
        SystemProfile usb{};
        usb.storage = StorageClass::FlashSsd;
        usb.logicalCpus = 16u;
        usb.totalRamBytes = 32ull * kGiB;
        const StreamBoostPlan p = computeStreamBoostPlan(usb);
        CHECK(p.active);
        CHECK(p.streamLoadCap == 96u);
        CHECK(p.poolCap == 16u);
        CHECK(p.flatPumpTimeUs == 2500u);
    }

    // 6. Low-RAM 8 GiB machine with NVMe + 16 CPUs stays clamped by RAM ceiling.
    {
        SystemProfile lowRam{};
        lowRam.storage = StorageClass::NvmeSsd;
        lowRam.logicalCpus = 16u;
        lowRam.totalRamBytes = 8ull * kGiB;
        const StreamBoostPlan p = computeStreamBoostPlan(lowRam);
        CHECK(p.active);
        CHECK(p.streamLoadCap == 64u);
        CHECK(p.poolCap == 16u);
    }

    // 7. 4-core / 4-thread CPU stays clamped by CPU ceiling and conservative flat pump.
    {
        SystemProfile quad{};
        quad.storage = StorageClass::NvmeSsd;
        quad.logicalCpus = 4u;
        quad.totalRamBytes = 32ull * kGiB;
        const StreamBoostPlan p = computeStreamBoostPlan(quad);
        CHECK(p.active);
        CHECK(p.streamLoadCap == 64u);
        CHECK(p.poolCap == 16u);
        CHECK(p.flatPumpTimeUs == 2000u);
        CHECK(p.flatPumpMaxIter == 128u);
    }

    // 8. Native unknown storage with 0 CPU/RAM (failed queries) falls back safely.
    {
        SystemProfile unk{};
        const StreamBoostPlan p = computeStreamBoostPlan(unk);
        CHECK(p.active);
        CHECK(p.streamLoadCap == 64u);
        CHECK(p.poolCap == 16u);
    }
}

void testPumpBudgetSelection() {
    SystemProfile hi{};
    hi.storage = StorageClass::NvmeSsd;
    hi.logicalCpus = 32u;
    hi.totalRamBytes = 64ull * kGiB;
    const StreamBoostPlan plan = computeStreamBoostPlan(hi);

    // Inactive plan (HDD) never boosts.
    {
        SystemProfile hdd = hi;
        hdd.storage = StorageClass::Hdd;
        const StreamBoostPlan off = computeStreamBoostPlan(hdd);
        PumpInput in{};
        in.wantLoad = 2000u;
        in.pending = 32u;
        const PumpBudget b = selectPumpBudget(off, in);
        CHECK(!b.boosted);
        CHECK(b.timeUs == kStockPumpTimeUs);
        CHECK(b.maxIter == kStockPumpMaxIter);
    }

    // Exact threshold boundary: wantLoad == 64 && pending == 8 stays at stock.
    {
        PumpInput in{};
        in.wantLoad = kBoostWantLoadThreshold;
        in.pending = kBoostPendingThreshold;
        const PumpBudget b = selectPumpBudget(plan, in);
        CHECK(!b.boosted);
        CHECK(b.timeUs == 1000u);
        CHECK(b.maxIter == 64u);
    }

    // Backlog via wantLoad > 64 in flat mode with headroom (including 24.5 ms spawn frames).
    {
        PumpInput in{};
        in.wantLoad = kBoostWantLoadThreshold + 1u;
        in.flatProfile = true;
        in.frameMsEma = 24.5;
        const PumpBudget b = selectPumpBudget(plan, in);
        CHECK(b.boosted);
        CHECK(!b.tightThrottled);
        CHECK(b.timeUs == 4000u);
        CHECK(b.maxIter == 256u);
    }

    // Backlog via pending > 8 in flat mode on genuinely tight frame (> 33 ms / < 30 FPS).
    {
        PumpInput in{};
        in.wantLoad = 0u;
        in.pending = kBoostPendingThreshold + 1u;
        in.flatProfile = true;
        in.frameMsEma = 36.0;
        const PumpBudget b = selectPumpBudget(plan, in);
        CHECK(b.boosted);
        CHECK(b.tightThrottled);
        CHECK(b.timeUs == 2000u);
        CHECK(b.maxIter == 128u);
    }

    // VR profile with headroom vs tight frame (> 11 ms).
    {
        PumpInput in{};
        in.wantLoad = 500u;
        in.flatProfile = false;
        in.frameMsEma = 8.5;
        const PumpBudget b = selectPumpBudget(plan, in);
        CHECK(b.boosted);
        CHECK(!b.tightThrottled);
        CHECK(b.timeUs == 2000u);
        CHECK(b.maxIter == 128u);

        in.frameMsEma = 12.5;
        const PumpBudget tight = selectPumpBudget(plan, in);
        CHECK(tight.boosted);
        CHECK(tight.tightThrottled);
        CHECK(tight.timeUs == 1500u);
        CHECK(tight.maxIter == 96u);
    }

    // VRAM eviction safety brake stands down completely during backlog.
    {
        PumpInput in{};
        in.wantLoad = 1200u;
        in.pending = 24u;
        in.overBudgetEvicting = true;
        in.flatProfile = true;
        const PumpBudget b = selectPumpBudget(plan, in);
        CHECK(!b.boosted);
        CHECK(b.evictBraked);
        CHECK(b.timeUs == 1000u);
        CHECK(b.maxIter == 64u);
    }

    // Loading screen already requests {10000, 1024}: never shrink it.
    {
        PumpInput in{};
        in.origTimeUs = 10000u;
        in.origMaxIter = 1024u;
        in.wantLoad = 3000u;
        in.flatProfile = true;
        const PumpBudget b = selectPumpBudget(plan, in);
        CHECK(!b.boosted);
        CHECK(b.timeUs == 10000u);
        CHECK(b.maxIter == 1024u);
    }
}

void testFrameMsEma() {
    double ema = 0.0;
    ema = updateFrameMsEma(ema, 0.0);
    CHECK(ema == 0.0);
    ema = updateFrameMsEma(ema, 500.0);  // outlier ignored
    CHECK(ema == 0.0);
    ema = updateFrameMsEma(ema, 10.0);
    CHECK(std::fabs(ema - 10.0) < 1e-6);
    ema = updateFrameMsEma(ema, 20.0);
    CHECK(std::fabs(ema - 11.0) < 1e-6);
    ema = updateFrameMsEma(ema, 999.0);  // pause spike ignored
    CHECK(std::fabs(ema - 11.0) < 1e-6);
}

void testEntryWantLoadFlags() {
    // Unloaded (state 0 in bits 28..24) + loadable bit + positive prio -> true,
    // even when unrelated lower flag bits are non-zero.
    CHECK(isEntryWantLoad(1.0f, kFlagLoadableBit));
    CHECK(isEntryWantLoad(0.25f, kFlagLoadableBit | 0x00000003u));
    CHECK(isEntryWantLoad(100.0f, kFlagLoadableBit | 0x00FFFFFFu));

    // Zero or negative priority -> false.
    CHECK(!isEntryWantLoad(0.0f, kFlagLoadableBit));
    CHECK(!isEntryWantLoad(-1.0f, kFlagLoadableBit));

    // Missing loadable bit -> false.
    CHECK(!isEntryWantLoad(1.0f, 0u));

    // Non-zero states (1=req, 2=loading, 4=loaded, 5/7=unloading) -> false.
    CHECK(!isEntryWantLoad(1.0f, kFlagLoadableBit | (1u << 24u)));
    CHECK(!isEntryWantLoad(1.0f, kFlagLoadableBit | (2u << 24u)));
    CHECK(!isEntryWantLoad(1.0f, kFlagLoadableBit | (4u << 24u)));
    CHECK(!isEntryWantLoad(1.0f, kFlagLoadableBit | (5u << 24u)));
    CHECK(!isEntryWantLoad(1.0f, kFlagLoadableBit | (7u << 24u)));
}

void testEncodeCmpRsiImm32() {
    uint8_t b[7] = {};
    encodeCmpRsiImm32(256u, b);
    const uint8_t want256[7] = {0x48u, 0x81u, 0xFEu, 0x00u, 0x01u, 0x00u, 0x00u};
    CHECK(std::memcmp(b, want256, 7u) == 0);

    encodeCmpRsiImm32(160u, b);
    const uint8_t want160[7] = {0x48u, 0x81u, 0xFEu, 0xA0u, 0x00u, 0x00u, 0x00u};
    CHECK(std::memcmp(b, want160, 7u) == 0);
}

}  // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--dry-run") == 0) {
            std::printf("stream_boost_test: dry-run ok\n");
            return 0;
        }
    }
    testClassifyStorage();
    testSystemPlans();
    testPumpBudgetSelection();
    testFrameMsEma();
    testEntryWantLoadFlags();
    testEncodeCmpRsiImm32();
    std::printf("stream_boost_test: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
