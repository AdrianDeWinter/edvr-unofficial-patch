#pragma once
// TEMPORARY build-334925 streaming diagnostic probe + 3-part acceleration experiment:
//   1. StreamLoad (exe+0x9E86A0): raises concurrency cap at exe+0x9E8935 (cmp rsi, 0x1e -> 0x7f: 30 -> 127)
//      with 16-byte pin and clean restore on shutdown; logs per-category budgets & backlog every 5 s.
//   2. fResStreamPool: raises max concurrent stream reader slots from 16 -> 32 once via Elite's own
//      fResService::SetMaxConcurrentStreams (exe+0x6FBDF0, 28-byte pin) under the scheduler lock when
//      all 16 slots are free (poolFree == poolAlloc && activeCount == 0).
//   3. fRes completion pump (exe+0x7F6EB0, owner +0xF0): while wantLoad > 64 (or pending > 8), passes a
//      stack-local {max(orig, 4000 us), max(orig, 256 iter)} budget to forward(); stands down immediately
//      to Elite's exact {1000 us, 64} once the scene backlog drains.
// Stands down cleanly on any build other than 334925 or on any byte-pin mismatch.

#include "../common/code_hook.h"
#include "../common/game_build.h"
#include "../common/log.h"

#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace edvr {
namespace stream_load_probe_detail {

// Build 334925 targets (hook_signatures.py AUDIT_EXACT_ONLY; installs only when gameBuildKnown()).
constexpr uint32_t kStreamLoadRva = 0x9E86A0u;
constexpr uint32_t kStreamLoadCapPinRva = 0x9E892Cu;
constexpr size_t kStreamLoadCapImmOffset = 12u;  // 0x9E8938: imm8 of `cmp rsi, 0x1e` (48 83 FE 1E)
constexpr uint8_t kStreamLoadCapOrig = 0x1Eu;    // 30
constexpr uint8_t kStreamLoadCapBoost = 0x7Fu;   // 127

constexpr uint32_t kTexUploadInFlightRva = 0x5F2D1B8u;
constexpr uint32_t kFresPumpRva = 0x7F6EB0u;
constexpr uint32_t kFresServiceGlobalRva = 0x5F2C3A8u;
constexpr uint32_t kFresSetMaxStreamsRva = 0x6FBDF0u;

constexpr uint64_t kTargetPoolCap = 32u;
constexpr uint32_t kBoostWantLoadThreshold = 64u;
constexpr uint32_t kBoostPendingThreshold = 8u;
constexpr uint32_t kBoostTimeUs = 4000u;
constexpr uint32_t kBoostMaxIter = 256u;

constexpr size_t kPrologueBytes = 16;
constexpr size_t kSetMaxStreamsPinBytes = 28;
// StreamLoad (0x9E86A0): push rbp; push r12; push r13; push r14; push r15; sub rsp, 0x110
constexpr uint8_t kStreamLoadPrologue[kPrologueBytes] = {
    0x40, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56,
    0x41, 0x57, 0x48, 0x81, 0xEC, 0x10, 0x01, 0x00,
};
// StreamLoad cap site (0x9E892C..0x9E893B):
//   mov eax, ebx; shr eax, 0x1f; or al, cl; xor ecx, ecx; cmp rsi, 0x1e; movzx eax, al
constexpr uint8_t kStreamLoadCapPin[kPrologueBytes] = {
    0x8B, 0xC3, 0xC1, 0xE8, 0x1F, 0x0A, 0xC1, 0x33,
    0xC9, 0x48, 0x83, 0xFE, 0x1E, 0x0F, 0xB6, 0xC0,
};
// fRes pump (0x7F6EB0): mov [rsp+8], rbx; mov [rsp+0x10], rbp; mov [rsp+0x18], rsi; push rdi
constexpr uint8_t kFresPumpPrologue[kPrologueBytes] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C,
    0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57,
};
// fResService::SetMaxConcurrentStreams (0x6FBDF0..0x6FBE0B)
constexpr uint8_t kSetMaxStreamsPin[kSetMaxStreamsPinBytes] = {
    0x48, 0x8B, 0x49, 0x10, 0x48, 0x8B, 0xC2, 0xBA,
    0x01, 0x00, 0x00, 0x00, 0x48, 0x3B, 0xC2, 0x48,
    0x8B, 0x49, 0x18, 0x48, 0x0F, 0x43, 0xD0, 0xE9,
    0x14, 0x00, 0x00, 0x00,
};

// StreamCategory and related read-only offsets (build 334925).
constexpr uintptr_t kMinValidPtr = 0x10000u;
constexpr size_t kOffCategoryId = 0x08u;
constexpr size_t kOffCpuBudgetBytes = 0x10u;
constexpr size_t kOffGpuBudgetBytes = 0x18u;
constexpr size_t kOffProviderCount = 0x50u;
constexpr size_t kOffEntriesPtr = 0x90u;
constexpr size_t kEntryStride = 24u;
constexpr size_t kOffEntryCpuBytes = 0x08u;
constexpr size_t kOffEntryGpuBytes = 0x0Cu;
constexpr size_t kOffEntryFlags = 0x14u;
constexpr size_t kOffPrioCount = 0xB8u;
constexpr size_t kOffPrioListPtr = 0xC0u;
constexpr size_t kPrioStride = 8u;
constexpr size_t kOffOverBudgetEvicting = 0xF0u;
constexpr size_t kOffExtMemPtr = 0xF8u;
constexpr size_t kOffExtCpuBytes = 0x10u;
constexpr size_t kOffExtGpuBytes = 0x18u;

// fResService / fResManager offsets (build 334925).
constexpr size_t kOffFresServiceSched = 0x08u;
constexpr size_t kOffFresServiceMgr = 0x10u;
constexpr size_t kOffFresMgrPool = 0x18u;
constexpr size_t kOffFresMgrActive = 0x20u;
constexpr size_t kOffFresMgrPendingHead = 0x28u;
constexpr size_t kOffFresMgrReadyHead = 0x58u;
constexpr size_t kOffFresMgrIncomingHead = 0xC0u;
constexpr size_t kOffFresMgrWorkB9 = 0xB9u;
constexpr size_t kOffFresMgrWorkBA = 0xBAu;
constexpr size_t kOffFresPoolAlloc = 0x10u;
constexpr size_t kOffFresPoolFree = 0x20u;
constexpr size_t kOffFresPoolCap = 0x28u;
constexpr size_t kOffFresActiveCount = 0x18u;

constexpr uint32_t kFlagLoadableBit = 0x20000000u;
constexpr uint64_t kMaxPrioScan = 8192u;
constexpr uint32_t kMaxEntryIndex = 1u << 24;
constexpr uint32_t kMaxListWalk = 256u;
constexpr uint64_t kReportIntervalMs = 5000u;
constexpr size_t kMaxCategories = 8;

// Relay layout matching the other engine hooks (44 bytes: gate check -> detour or trampoline).
constexpr size_t kRelayBytes = 44;
constexpr size_t kOriginalLiteral = 36;

using StreamLoadFn = void(__fastcall*)(uintptr_t category, uintptr_t jobSys);
using FresPumpFn = void(__fastcall*)(uintptr_t owner, const uint32_t* budgetPair);
using SetMaxStreamsFn = void(__fastcall*)(uintptr_t service, uint64_t count);
using SchedLockFn = void(__fastcall*)(uintptr_t sched);

struct HookSlot {
    alignas(8) std::atomic<uintptr_t> gate{0};
    std::atomic<uintptr_t> forward{0};
    std::atomic<bool> installed{false};
    uint8_t* relay = nullptr;
    CodeHook hook;
};
static_assert(decltype(HookSlot::gate)::is_always_lock_free, "The relay reads the aligned atomic cell directly.");

struct CategoryHeader {
    uint16_t categoryId;
    uint8_t overBudgetEvicting;
    uint64_t cpuBudgetBytes;
    uint64_t gpuBudgetBytes;
    uint64_t extCpuBytes;
    uint64_t extGpuBytes;
    uint64_t providerCount;
    uint64_t prioCount;
    uintptr_t prioListPtr;
    uintptr_t entriesPtr;
    uint64_t texUploadInFlightBytes;
};

struct PrioScan {
    uint32_t wantLoad;
    uint32_t reqLoad;
    uint32_t loading;
    uint32_t loaded;
    uint32_t unloading;
    uint64_t loadedCpuBytes;
    uint64_t loadedGpuBytes;
};

struct CategoryBucket {
    bool used = false;
    uintptr_t categoryPtr = 0;
    uint16_t categoryId = 0;

    uint64_t calls = 0;
    uint64_t cpuBudgetBytes = 0;
    uint64_t gpuBudgetBytes = 0;
    uint64_t extCpuBytes = 0;
    uint64_t extGpuBytes = 0;
    uint64_t prioCount = 0;
    uint64_t providerCount = 0;
    uint64_t evictTicks = 0;
    uint32_t wantLoadMax = 0;
    uint32_t reqLoadMax = 0;
    uint32_t loadingMax = 0;
    uint32_t loadedLatest = 0;
    uint32_t unloadingLatest = 0;
    uint64_t loadedCpuBytes = 0;
    uint64_t loadedGpuBytes = 0;
    uint64_t texUploadMaxBytes = 0;

    void resetWindow() noexcept {
        calls = 0;
        evictTicks = 0;
        wantLoadMax = 0;
        reqLoadMax = 0;
        loadingMax = 0;
        loadedLatest = 0;
        unloadingLatest = 0;
        loadedCpuBytes = 0;
        loadedGpuBytes = 0;
        texUploadMaxBytes = 0;
    }
};

struct FresState {
    bool valid;
    uintptr_t service;
    uintptr_t sched;
    uint8_t workFlags;
    uint64_t poolAlloc;
    uint64_t poolFree;
    uint64_t poolCap;
    uint64_t activeCount;
    uint32_t pendingCount;
    uint32_t readyCount;
    uint32_t incomingCount;
};

struct FresPumpWindow {
    uint64_t calls = 0;
    uint64_t busyCalls = 0;
    uint64_t boostedCalls = 0;
    uint32_t budgetUsMin = 0xFFFFFFFFu;
    uint32_t budgetUsMax = 0;
    uint32_t maxIterMin = 0xFFFFFFFFu;
    uint32_t maxIterMax = 0;
    double elapsedUsSum = 0.0;
    double elapsedUsMax = 0.0;
    double busyElapsedUsSum = 0.0;
    double busyElapsedUsMax = 0.0;
    uint64_t drained = 0;
    uint64_t hitTime = 0;
    uint64_t hitIter = 0;
    uint64_t slotBlockedBefore = 0;
    uint64_t slotBlockedAfter = 0;
    uint64_t poolCap = 0;
    uint64_t poolAlloc = 0;
    uint64_t poolFreeMin = 0xFFFFFFFFFFFFFFFFull;
    uint64_t activeMax = 0;
    uint32_t pendingMax = 0;
    uint32_t readyMax = 0;
    uint32_t incomingMax = 0;

    void reset() noexcept { *this = FresPumpWindow{}; }
};

inline HookSlot g_streamSlot{};
inline HookSlot g_fresSlot{};
inline std::atomic<uintptr_t> g_exeBase{0};
inline std::atomic<bool> g_attempted{false};
inline std::atomic<bool> g_capPatched{false};
inline std::atomic<uintptr_t> g_capPatchAddr{0};
inline std::atomic<bool> g_setMaxStreamsPinned{false};
inline std::atomic<bool> g_poolBoostDone{false};
inline std::atomic<bool> g_poolBoostLogged{false};
inline std::atomic<uint32_t> g_latestWantLoad{0};

inline std::atomic_flag g_bucketsLock = ATOMIC_FLAG_INIT;
inline CategoryBucket g_buckets[kMaxCategories]{};

inline std::atomic_flag g_fresLock = ATOMIC_FLAG_INIT;
inline FresPumpWindow g_fresWindow{};

inline bool g_pollStarted = false;
inline uint64_t g_windowStartMs = 0;

inline double bytesToMb(uint64_t bytes) noexcept {
    return static_cast<double>(bytes) / (1024.0 * 1024.0);
}

inline double qpcUsPerTick() noexcept {
    static const double kUsPerTick = [] {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return f.QuadPart > 0 ? (1000000.0 / static_cast<double>(f.QuadPart)) : 0.0;
    }();
    return kUsPerTick;
}

inline int64_t qpcNow() noexcept {
    LARGE_INTEGER t{};
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

inline uint8_t* allocateRelay(uintptr_t target) noexcept {
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    const uintptr_t granularity = info.dwAllocationGranularity;
    const uintptr_t floor = reinterpret_cast<uintptr_t>(info.lpMinimumApplicationAddress);
    const uintptr_t ceiling = reinterpret_cast<uintptr_t>(info.lpMaximumApplicationAddress);
    const uintptr_t distance = uintptr_t(INT32_MAX) - 0x10000u;
    uintptr_t at = target > distance ? target - distance : floor;
    if (at < floor) at = floor;
    const uintptr_t limit = target > ceiling - distance ? ceiling : target + distance;
    while (at < limit) {
        MEMORY_BASIC_INFORMATION region{};
        if (!VirtualQuery(reinterpret_cast<void*>(at), &region, sizeof(region))) break;
        const uintptr_t start = reinterpret_cast<uintptr_t>(region.BaseAddress);
        if (region.RegionSize > UINTPTR_MAX - start) break;
        const uintptr_t end = start + region.RegionSize;
        if (region.State == MEM_FREE) {
            uintptr_t candidate = at > start ? at : start;
            if (candidate > UINTPTR_MAX - (granularity - 1)) break;
            candidate = (candidate + granularity - 1) & ~(granularity - 1);
            if (candidate < limit && candidate < end && end - candidate >= 4096) {
                auto* p = static_cast<uint8_t*>(
                    VirtualAlloc(reinterpret_cast<void*>(candidate), 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
                if (p) return p;
            }
        }
        if (end <= at) break;
        at = end;
    }
    return nullptr;
}

inline void buildRelay(uint8_t* code, const void* gate, void* callback) noexcept {
    const uint8_t body[kRelayBytes] = {
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,
        0x48, 0x83, 0x38, 0,
        0x74, 0x0E,
        0xFF, 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0xFF, 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    };
    std::memcpy(code, body, sizeof(body));
    const uintptr_t gateAddress = reinterpret_cast<uintptr_t>(gate);
    const uintptr_t callbackAddress = reinterpret_cast<uintptr_t>(callback);
    std::memcpy(code + 2, &gateAddress, 8);
    std::memcpy(code + 22, &callbackAddress, 8);
}

inline bool prepareRelay(void* trampoline, void* ctx) noexcept {
    auto* slot = static_cast<HookSlot*>(ctx);
    if (!slot || !slot->relay) return false;
    const uintptr_t address = reinterpret_cast<uintptr_t>(trampoline);
    std::memcpy(slot->relay + kOriginalLiteral, &address, 8);
    DWORD oldProtect = 0;
    if (!VirtualProtect(slot->relay, 4096, PAGE_EXECUTE_READ, &oldProtect) ||
        !FlushInstructionCache(GetCurrentProcess(), slot->relay, kRelayBytes)) {
        return false;
    }
    slot->forward.store(address, std::memory_order_release);
    return true;
}

// --- SEH-isolated plain-POD readers/writers (no C++ unwindable objects in scope) ---

__declspec(noinline) inline bool readBytesGuarded(uintptr_t addr, uint8_t* out, size_t n) noexcept {
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(addr), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

inline bool patchSingleByte(uintptr_t addr, uint8_t expected, uint8_t replacement) noexcept {
    uint8_t cur = 0;
    if (!readBytesGuarded(addr, &cur, 1) || cur != expected) return false;
    DWORD oldProtect = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(addr), 1, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return false;
    }
    *reinterpret_cast<uint8_t*>(addr) = replacement;
    DWORD dummy = 0;
    VirtualProtect(reinterpret_cast<void*>(addr), 1, oldProtect, &dummy);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<const void*>(addr), 1);
    return true;
}

__declspec(noinline) inline bool callSetMaxStreamsLocked(uintptr_t exeBase, uintptr_t service, uintptr_t sched, uint64_t newCap) noexcept {
    if (exeBase < kMinValidPtr || service < kMinValidPtr) return false;
    __try {
        SchedLockFn lockFn = nullptr;
        SchedLockFn unlockFn = nullptr;
        if (sched >= kMinValidPtr) {
            uintptr_t vtbl = 0;
            std::memcpy(&vtbl, reinterpret_cast<const void*>(sched), sizeof(vtbl));
            if (vtbl >= kMinValidPtr) {
                std::memcpy(&lockFn, reinterpret_cast<const void*>(vtbl), sizeof(lockFn));
                std::memcpy(&unlockFn, reinterpret_cast<const void*>(vtbl + 8u), sizeof(unlockFn));
            }
        }
        const auto setFn = reinterpret_cast<SetMaxStreamsFn>(exeBase + kFresSetMaxStreamsRva);
        if (lockFn && unlockFn) lockFn(sched);
        setFn(service, newCap);
        if (lockFn && unlockFn) unlockFn(sched);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

__declspec(noinline) inline bool readCategoryHeader(uintptr_t category, uintptr_t exeBase, CategoryHeader* out) noexcept {
    if (category < kMinValidPtr || !out) return false;
    std::memset(out, 0, sizeof(*out));
    __try {
        const auto* cat = reinterpret_cast<const uint8_t*>(category);
        std::memcpy(&out->categoryId, cat + kOffCategoryId, sizeof(out->categoryId));
        std::memcpy(&out->cpuBudgetBytes, cat + kOffCpuBudgetBytes, sizeof(out->cpuBudgetBytes));
        std::memcpy(&out->gpuBudgetBytes, cat + kOffGpuBudgetBytes, sizeof(out->gpuBudgetBytes));
        std::memcpy(&out->providerCount, cat + kOffProviderCount, sizeof(out->providerCount));
        std::memcpy(&out->entriesPtr, cat + kOffEntriesPtr, sizeof(out->entriesPtr));
        std::memcpy(&out->prioCount, cat + kOffPrioCount, sizeof(out->prioCount));
        std::memcpy(&out->prioListPtr, cat + kOffPrioListPtr, sizeof(out->prioListPtr));
        std::memcpy(&out->overBudgetEvicting, cat + kOffOverBudgetEvicting, sizeof(out->overBudgetEvicting));

        uintptr_t extMemPtr = 0;
        std::memcpy(&extMemPtr, cat + kOffExtMemPtr, sizeof(extMemPtr));
        if (extMemPtr >= kMinValidPtr) {
            const auto* ext = reinterpret_cast<const uint8_t*>(extMemPtr);
            std::memcpy(&out->extCpuBytes, ext + kOffExtCpuBytes, sizeof(out->extCpuBytes));
            std::memcpy(&out->extGpuBytes, ext + kOffExtGpuBytes, sizeof(out->extGpuBytes));
        }

        if (exeBase >= kMinValidPtr) {
            std::memcpy(&out->texUploadInFlightBytes,
                        reinterpret_cast<const void*>(exeBase + kTexUploadInFlightRva),
                        sizeof(out->texUploadInFlightBytes));
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

__declspec(noinline) inline bool readPrioScan(uintptr_t prioListPtr, uint64_t prioCount, uintptr_t entriesPtr, PrioScan* out) noexcept {
    if (!out) return false;
    std::memset(out, 0, sizeof(*out));
    if (prioCount == 0) return true;
    if (prioListPtr < kMinValidPtr || entriesPtr < kMinValidPtr) return false;
    __try {
        const uint64_t scanN = prioCount <= kMaxPrioScan ? prioCount : kMaxPrioScan;
        const auto* prioBase = reinterpret_cast<const uint8_t*>(prioListPtr);
        const auto* entriesBase = reinterpret_cast<const uint8_t*>(entriesPtr);
        for (uint64_t i = 0; i < scanN; ++i) {
            const uint8_t* item = prioBase + i * kPrioStride;
            float prio = 0.0f;
            uint32_t entryIdx = 0;
            std::memcpy(&prio, item, sizeof(prio));
            std::memcpy(&entryIdx, item + 4u, sizeof(entryIdx));
            if (entryIdx >= kMaxEntryIndex) continue;

            const uint8_t* entry = entriesBase + static_cast<size_t>(entryIdx) * kEntryStride;
            uint32_t cpuBytes = 0;
            uint32_t gpuBytes = 0;
            uint32_t flags = 0;
            std::memcpy(&cpuBytes, entry + kOffEntryCpuBytes, sizeof(cpuBytes));
            std::memcpy(&gpuBytes, entry + kOffEntryGpuBytes, sizeof(gpuBytes));
            std::memcpy(&flags, entry + kOffEntryFlags, sizeof(flags));

            const uint32_t state = (flags >> 24u) & 0x1Fu;
            if (state == 0u) {
                if (prio > 0.0f && (flags & kFlagLoadableBit) != 0u) {
                    out->wantLoad += 1u;
                }
            } else if (state == 1u) {
                out->reqLoad += 1u;
            } else if (state == 2u) {
                out->loading += 1u;
            } else if (state == 4u) {
                out->loaded += 1u;
                out->loadedCpuBytes += static_cast<uint64_t>(cpuBytes);
                out->loadedGpuBytes += static_cast<uint64_t>(gpuBytes);
            } else if (state == 5u || state == 7u) {
                out->unloading += 1u;
            }
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

__declspec(noinline) inline bool readBudgetPair(const uint32_t* budgetPair, uint32_t* outTimeUs, uint32_t* outMaxIter) noexcept {
    if (reinterpret_cast<uintptr_t>(budgetPair) < kMinValidPtr || !outTimeUs || !outMaxIter) return false;
    __try {
        std::memcpy(outTimeUs, budgetPair, sizeof(uint32_t));
        std::memcpy(outMaxIter, budgetPair + 1, sizeof(uint32_t));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

inline uint32_t countSentinelListUnchecked(uintptr_t head) noexcept {
    uint32_t count = 0;
    uintptr_t node = head;
    while (node >= kMinValidPtr && count < kMaxListWalk) {
        uintptr_t next = 0;
        std::memcpy(&next, reinterpret_cast<const void*>(node), sizeof(next));
        if (next < kMinValidPtr || next == node) break;
        ++count;
        node = next;
    }
    return count;
}

__declspec(noinline) inline bool readFresState(uintptr_t exeBase, FresState* out) noexcept {
    if (exeBase < kMinValidPtr || !out) return false;
    std::memset(out, 0, sizeof(*out));
    __try {
        uintptr_t service = 0;
        std::memcpy(&service, reinterpret_cast<const void*>(exeBase + kFresServiceGlobalRva), sizeof(service));
        if (service < kMinValidPtr) return false;
        out->service = service;

        uintptr_t sched = 0;
        std::memcpy(&sched, reinterpret_cast<const uint8_t*>(service) + kOffFresServiceSched, sizeof(sched));
        out->sched = sched;

        uintptr_t mgr = 0;
        std::memcpy(&mgr, reinterpret_cast<const uint8_t*>(service) + kOffFresServiceMgr, sizeof(mgr));
        if (mgr < kMinValidPtr) return false;

        const auto* m = reinterpret_cast<const uint8_t*>(mgr);
        uint8_t b9 = 0;
        uint8_t ba = 0;
        std::memcpy(&b9, m + kOffFresMgrWorkB9, sizeof(b9));
        std::memcpy(&ba, m + kOffFresMgrWorkBA, sizeof(ba));
        out->workFlags = static_cast<uint8_t>((b9 ? 2u : 0u) | (ba ? 1u : 0u));

        uintptr_t pool = 0;
        std::memcpy(&pool, m + kOffFresMgrPool, sizeof(pool));
        if (pool >= kMinValidPtr) {
            const auto* p = reinterpret_cast<const uint8_t*>(pool);
            std::memcpy(&out->poolAlloc, p + kOffFresPoolAlloc, sizeof(out->poolAlloc));
            std::memcpy(&out->poolFree, p + kOffFresPoolFree, sizeof(out->poolFree));
            std::memcpy(&out->poolCap, p + kOffFresPoolCap, sizeof(out->poolCap));
        }

        uintptr_t active = 0;
        std::memcpy(&active, m + kOffFresMgrActive, sizeof(active));
        if (active >= kMinValidPtr) {
            std::memcpy(&out->activeCount,
                        reinterpret_cast<const uint8_t*>(active) + kOffFresActiveCount,
                        sizeof(out->activeCount));
        }

        uintptr_t pendingHead = 0;
        uintptr_t readyHead = 0;
        uintptr_t incomingHead = 0;
        std::memcpy(&pendingHead, m + kOffFresMgrPendingHead, sizeof(pendingHead));
        std::memcpy(&readyHead, m + kOffFresMgrReadyHead, sizeof(readyHead));
        std::memcpy(&incomingHead, m + kOffFresMgrIncomingHead, sizeof(incomingHead));
        out->pendingCount = countSentinelListUnchecked(pendingHead);
        out->readyCount = countSentinelListUnchecked(readyHead);
        out->incomingCount = countSentinelListUnchecked(incomingHead);
        out->valid = true;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

inline CategoryBucket* findBucket(uintptr_t categoryPtr) noexcept {
    for (size_t i = 0; i < kMaxCategories; ++i) {
        if (g_buckets[i].used && g_buckets[i].categoryPtr == categoryPtr) return &g_buckets[i];
    }
    return nullptr;
}

inline CategoryBucket* findOrAllocBucket(uintptr_t categoryPtr, uint16_t categoryId) noexcept {
    if (CategoryBucket* existing = findBucket(categoryPtr)) {
        existing->categoryId = categoryId;
        return existing;
    }
    for (size_t i = 0; i < kMaxCategories; ++i) {
        if (!g_buckets[i].used) {
            g_buckets[i] = CategoryBucket{};
            g_buckets[i].used = true;
            g_buckets[i].categoryPtr = categoryPtr;
            g_buckets[i].categoryId = categoryId;
            return &g_buckets[i];
        }
    }
    return nullptr;
}

__declspec(noinline) inline void __fastcall streamLoadDetour(uintptr_t category, uintptr_t jobSys) noexcept {
    const auto forward = reinterpret_cast<StreamLoadFn>(g_streamSlot.forward.load(std::memory_order_acquire));
    if (!forward) return;
    forward(category, jobSys);

    if (g_streamSlot.gate.load(std::memory_order_acquire) == 0) return;

    const uintptr_t exeBase = g_exeBase.load(std::memory_order_relaxed);
    CategoryHeader hdr{};
    if (!readCategoryHeader(category, exeBase, &hdr)) return;

    bool needScan = false;
    if (!g_bucketsLock.test_and_set(std::memory_order_acquire)) {
        if (CategoryBucket* b = findOrAllocBucket(category, hdr.categoryId)) {
            b->calls += 1u;
            b->cpuBudgetBytes = hdr.cpuBudgetBytes;
            b->gpuBudgetBytes = hdr.gpuBudgetBytes;
            b->extCpuBytes = hdr.extCpuBytes;
            b->extGpuBytes = hdr.extGpuBytes;
            b->prioCount = hdr.prioCount;
            b->providerCount = hdr.providerCount;
            if (hdr.overBudgetEvicting != 0u) b->evictTicks += 1u;
            if (hdr.texUploadInFlightBytes > b->texUploadMaxBytes) {
                b->texUploadMaxBytes = hdr.texUploadInFlightBytes;
            }
            if ((b->calls % 8u) == 1u) {
                needScan = true;
            }
        }
        g_bucketsLock.clear(std::memory_order_release);
    }

    if (!needScan) return;

    PrioScan scan{};
    if (!readPrioScan(hdr.prioListPtr, hdr.prioCount, hdr.entriesPtr, &scan)) return;
    g_latestWantLoad.store(scan.wantLoad, std::memory_order_relaxed);

    if (!g_bucketsLock.test_and_set(std::memory_order_acquire)) {
        if (CategoryBucket* b = findBucket(category)) {
            if (scan.wantLoad > b->wantLoadMax) b->wantLoadMax = scan.wantLoad;
            if (scan.reqLoad > b->reqLoadMax) b->reqLoadMax = scan.reqLoad;
            if (scan.loading > b->loadingMax) b->loadingMax = scan.loading;
            b->loadedLatest = scan.loaded;
            b->unloadingLatest = scan.unloading;
            b->loadedCpuBytes = scan.loadedCpuBytes;
            b->loadedGpuBytes = scan.loadedGpuBytes;
        }
        g_bucketsLock.clear(std::memory_order_release);
    }
}

__declspec(noinline) inline void __fastcall fresPumpDetour(uintptr_t owner, const uint32_t* budgetPair) noexcept {
    const auto forward = reinterpret_cast<FresPumpFn>(g_fresSlot.forward.load(std::memory_order_acquire));
    if (!forward) return;

    if (g_fresSlot.gate.load(std::memory_order_acquire) == 0) {
        forward(owner, budgetPair);
        return;
    }

    uint32_t origTimeUs = 0;
    uint32_t origMaxIter = 0;
    const bool gotBudget = readBudgetPair(budgetPair, &origTimeUs, &origMaxIter);

    const uintptr_t exeBase = g_exeBase.load(std::memory_order_relaxed);
    FresState before{};
    readFresState(exeBase, &before);

    // Part 2: raise fResStreamPool 16 -> 32 once, strictly when 0 stream slots are checked out
    // (poolFree == poolAlloc && activeCount == 0), under the fRes scheduler lock.
    if (before.valid &&
        !g_poolBoostDone.load(std::memory_order_relaxed) &&
        g_setMaxStreamsPinned.load(std::memory_order_acquire) &&
        before.poolCap >= 16u &&
        before.poolCap < kTargetPoolCap &&
        before.poolAlloc == before.poolCap &&
        before.poolFree == before.poolAlloc &&
        before.activeCount == 0u) {
        if (callSetMaxStreamsLocked(exeBase, before.service, before.sched, kTargetPoolCap)) {
            g_poolBoostDone.store(true, std::memory_order_release);
            readFresState(exeBase, &before);
        }
    }

    // Part 3: while streaming backlog is active, relax clamped gameplay budget to {4000 us, 256}.
    uint32_t usedPair[2] = {origTimeUs, origMaxIter};
    const uint32_t* callPair = budgetPair;
    bool boosted = false;
    if (gotBudget) {
        const uint32_t wantLoad = g_latestWantLoad.load(std::memory_order_relaxed);
        const bool hasBacklog = (wantLoad > kBoostWantLoadThreshold) ||
                                (before.valid && before.pendingCount > kBoostPendingThreshold);
        if (hasBacklog && (origTimeUs < kBoostTimeUs || origMaxIter < kBoostMaxIter)) {
            usedPair[0] = origTimeUs < kBoostTimeUs ? kBoostTimeUs : origTimeUs;
            usedPair[1] = origMaxIter < kBoostMaxIter ? kBoostMaxIter : origMaxIter;
            callPair = usedPair;
            boosted = true;
        }
    }
    const uint32_t activeTimeUs = boosted ? usedPair[0] : origTimeUs;

    const int64_t t0 = qpcNow();
    forward(owner, callPair);
    const int64_t t1 = qpcNow();

    FresState after{};
    readFresState(exeBase, &after);

    const double elapsedUs = (t1 > t0) ? (static_cast<double>(t1 - t0) * qpcUsPerTick()) : 0.0;

    if (!g_fresLock.test_and_set(std::memory_order_acquire)) {
        FresPumpWindow& w = g_fresWindow;
        w.calls += 1u;
        if (boosted) w.boostedCalls += 1u;
        if (gotBudget) {
            if (origTimeUs < w.budgetUsMin) w.budgetUsMin = origTimeUs;
            if (origTimeUs > w.budgetUsMax) w.budgetUsMax = origTimeUs;
            if (origMaxIter < w.maxIterMin) w.maxIterMin = origMaxIter;
            if (origMaxIter > w.maxIterMax) w.maxIterMax = origMaxIter;
        }
        w.elapsedUsSum += elapsedUs;
        if (elapsedUs > w.elapsedUsMax) w.elapsedUsMax = elapsedUs;

        const bool wasBusy = before.valid && (before.workFlags != 0u || before.pendingCount > 0u ||
                                              before.activeCount > 0u || before.incomingCount > 0u ||
                                              before.readyCount > 0u);
        if (wasBusy) {
            w.busyCalls += 1u;
            w.busyElapsedUsSum += elapsedUs;
            if (elapsedUs > w.busyElapsedUsMax) w.busyElapsedUsMax = elapsedUs;
        }
        if (before.valid && after.valid) {
            if (before.workFlags != 0u && after.workFlags == 0u) {
                w.drained += 1u;
            } else if (after.workFlags != 0u) {
                if (gotBudget && elapsedUs >= static_cast<double>(activeTimeUs)) {
                    w.hitTime += 1u;
                } else {
                    w.hitIter += 1u;
                }
            }
            if (before.pendingCount > 0u && before.poolFree == 0u) {
                w.slotBlockedBefore += 1u;
            }
            if (after.pendingCount > 0u && after.poolFree == 0u) {
                w.slotBlockedAfter += 1u;
            }
            w.poolCap = after.poolCap;
            w.poolAlloc = after.poolAlloc;
            if (before.poolFree < w.poolFreeMin) w.poolFreeMin = before.poolFree;
            if (after.poolFree < w.poolFreeMin) w.poolFreeMin = after.poolFree;
            if (before.activeCount > w.activeMax) w.activeMax = before.activeCount;
            if (after.activeCount > w.activeMax) w.activeMax = after.activeCount;
            if (before.pendingCount > w.pendingMax) w.pendingMax = before.pendingCount;
            if (after.pendingCount > w.pendingMax) w.pendingMax = after.pendingCount;
            if (before.readyCount > w.readyMax) w.readyMax = before.readyCount;
            if (after.readyCount > w.readyMax) w.readyMax = after.readyCount;
            if (before.incomingCount > w.incomingMax) w.incomingMax = before.incomingCount;
            if (after.incomingCount > w.incomingMax) w.incomingMax = after.incomingCount;
        }
        g_fresLock.clear(std::memory_order_release);
    }
}

inline bool armSlot(HookSlot& slot, uintptr_t target, const uint8_t expected[kPrologueBytes],
                    void* detourFn, const char* hookTag, const char* label) noexcept {
    uint8_t got[kPrologueBytes] = {};
    if (!readBytesGuarded(target, got, kPrologueBytes)) {
        Log::get().note("%s: inert (%s)", label, "prologue unreadable");
        return false;
    }
    if (std::memcmp(got, expected, kPrologueBytes) != 0) {
        Log::get().note("%s: inert (%s)", label, "prologue mismatch");
        return false;
    }
    uint8_t* relay = allocateRelay(target);
    if (!relay) {
        Log::get().note("%s: inert (%s)", label, "no relay memory within 2 GB of target");
        return false;
    }
    slot.relay = relay;
    buildRelay(relay, &slot.gate, detourFn);
    if (!slot.hook.install(reinterpret_cast<void*>(target), relay, nullptr, hookTag, &prepareRelay, &slot)) {
        VirtualFree(relay, 0, MEM_RELEASE);
        slot.relay = nullptr;
        const char* why = slot.hook.refusal();
        Log::get().note("%s: inert (%s)", label, (why && why[0]) ? why : "CodeHook refused target");
        return false;
    }
    slot.installed.store(true, std::memory_order_release);
    slot.gate.store(1, std::memory_order_release);
    return true;
}

inline void disarmSlot(HookSlot& slot) noexcept {
    slot.gate.store(0, std::memory_order_release);
    slot.installed.store(false, std::memory_order_release);
    slot.hook.uninstall();
    if (slot.relay) {
        VirtualFree(slot.relay, 0, MEM_RELEASE);
        slot.relay = nullptr;
    }
    slot.forward.store(0, std::memory_order_release);
}

}  // namespace stream_load_probe_detail

inline void streamLoadProbeInstall() {
    namespace d = stream_load_probe_detail;
    if (d::g_attempted.exchange(true, std::memory_order_acq_rel)) return;

    const uintptr_t exeBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (!exeBase) {
        Log::get().note("stream load probe: inert (%s)", "main module base unavailable");
        return;
    }
    if (!gameBuildKnown()) {
        Log::get().note("stream load probe: inert (%s)", gameBuildText());
        return;
    }
    d::g_exeBase.store(exeBase, std::memory_order_release);

    // Part 1: verify 16-byte pin at StreamLoad cap site and patch 0x1E (30) -> 0x7F (127).
    uint8_t capGot[d::kPrologueBytes] = {};
    const uintptr_t capPinAddr = exeBase + d::kStreamLoadCapPinRva;
    if (d::readBytesGuarded(capPinAddr, capGot, d::kPrologueBytes) &&
        std::memcmp(capGot, d::kStreamLoadCapPin, d::kPrologueBytes) == 0) {
        const uintptr_t immAddr = capPinAddr + d::kStreamLoadCapImmOffset;
        if (d::patchSingleByte(immAddr, d::kStreamLoadCapOrig, d::kStreamLoadCapBoost)) {
            d::g_capPatchAddr.store(immAddr, std::memory_order_release);
            d::g_capPatched.store(true, std::memory_order_release);
            Log::get().note("stream boost: StreamLoad cap patched at exe+0x9E8938 (30 -> 127)");
        } else {
            Log::get().note("stream boost: StreamLoad cap patch failed (%s)", "VirtualProtect/write failed");
        }
    } else {
        Log::get().note("stream boost: StreamLoad cap not patched (%s)", "16-byte pin mismatch at exe+0x9E892C");
    }

    // Verify 28-byte pin on fResService::SetMaxConcurrentStreams (0x6FBDF0) for Part 2.
    uint8_t setGot[d::kSetMaxStreamsPinBytes] = {};
    if (d::readBytesGuarded(exeBase + d::kFresSetMaxStreamsRva, setGot, d::kSetMaxStreamsPinBytes) &&
        std::memcmp(setGot, d::kSetMaxStreamsPin, d::kSetMaxStreamsPinBytes) == 0) {
        d::g_setMaxStreamsPinned.store(true, std::memory_order_release);
        Log::get().note("stream boost: SetMaxConcurrentStreams pinned at exe+0x6FBDF0 (armed for 16 -> 32 on idle pool)");
    } else {
        Log::get().note("stream boost: SetMaxConcurrentStreams pin mismatch at exe+0x6FBDF0; pool stays at 16");
    }

    if (d::armSlot(d::g_streamSlot, exeBase + d::kStreamLoadRva, d::kStreamLoadPrologue,
                   reinterpret_cast<void*>(&d::streamLoadDetour), "stream-load-probe", "stream load probe")) {
        Log::get().note("stream load probe: armed at exe+0x9E86A0 (build 334925 verified)");
    }
    if (d::armSlot(d::g_fresSlot, exeBase + d::kFresPumpRva, d::kFresPumpPrologue,
                   reinterpret_cast<void*>(&d::fresPumpDetour), "fres-pump-probe", "fres pump probe")) {
        Log::get().note("fres pump probe: armed at exe+0x7F6EB0 (backlog boost 4000us/256 when wantLoad>64)");
    }
}

inline void streamLoadProbePoll(uint64_t nowMs = 0) {
    namespace d = stream_load_probe_detail;
    const bool streamLive = d::g_streamSlot.installed.load(std::memory_order_acquire);
    const bool fresLive = d::g_fresSlot.installed.load(std::memory_order_acquire);
    if (!streamLive && !fresLive) return;
    if (nowMs == 0) nowMs = GetTickCount64();

    if (d::g_poolBoostDone.load(std::memory_order_acquire) &&
        !d::g_poolBoostLogged.exchange(true, std::memory_order_acq_rel)) {
        Log::get().note("stream boost: fResStreamPool raised 16 -> 32 via SetMaxConcurrentStreams");
    }

    if (!d::g_pollStarted) {
        d::g_pollStarted = true;
        d::g_windowStartMs = nowMs;
        return;
    }
    if (nowMs < d::g_windowStartMs) {
        d::g_windowStartMs = nowMs;
        return;
    }
    if (nowMs - d::g_windowStartMs < d::kReportIntervalMs) return;
    d::g_windowStartMs = nowMs;

    if (streamLive) {
        d::CategoryBucket snap[d::kMaxCategories]{};
        size_t snapCount = 0;

        while (d::g_bucketsLock.test_and_set(std::memory_order_acquire)) {
            YieldProcessor();
        }
        for (size_t i = 0; i < d::kMaxCategories; ++i) {
            if (d::g_buckets[i].used && d::g_buckets[i].calls > 0) {
                snap[snapCount++] = d::g_buckets[i];
                d::g_buckets[i].resetWindow();
            }
        }
        d::g_bucketsLock.clear(std::memory_order_release);

        for (size_t i = 0; i < snapCount; ++i) {
            const d::CategoryBucket& b = snap[i];
            const unsigned prios = static_cast<unsigned>(b.prioCount > 0xFFFFFFFFull ? 0xFFFFFFFFull : b.prioCount);
            Log::get().note("stream load probe 5s: cat=%u calls=%llu gpuBudget=%.1fMB extGpu=%.1fMB loadedGpu=%.1fMB "
                            "cpuBudget=%.1fMB extCpu=%.1fMB loadedCpu=%.1fMB evictTicks=%llu prios=%u "
                            "wantLoad=%u req=%u loading=%u loaded=%u texUpload=%.2fMB",
                static_cast<unsigned>(b.categoryId),
                static_cast<unsigned long long>(b.calls),
                d::bytesToMb(b.gpuBudgetBytes),
                d::bytesToMb(b.extGpuBytes),
                d::bytesToMb(b.loadedGpuBytes),
                d::bytesToMb(b.cpuBudgetBytes),
                d::bytesToMb(b.extCpuBytes),
                d::bytesToMb(b.loadedCpuBytes),
                static_cast<unsigned long long>(b.evictTicks),
                prios,
                static_cast<unsigned>(b.wantLoadMax),
                static_cast<unsigned>(b.reqLoadMax),
                static_cast<unsigned>(b.loadingMax),
                static_cast<unsigned>(b.loadedLatest),
                d::bytesToMb(b.texUploadMaxBytes));
        }
    }

    if (fresLive) {
        d::FresPumpWindow fw{};
        while (d::g_fresLock.test_and_set(std::memory_order_acquire)) {
            YieldProcessor();
        }
        fw = d::g_fresWindow;
        d::g_fresWindow.reset();
        d::g_fresLock.clear(std::memory_order_release);

        if (fw.calls > 0) {
            const double avgUs = fw.elapsedUsSum / static_cast<double>(fw.calls);
            const double busyAvgUs = fw.busyCalls > 0 ? (fw.busyElapsedUsSum / static_cast<double>(fw.busyCalls)) : 0.0;
            const unsigned budgetLo = fw.budgetUsMin == 0xFFFFFFFFu ? 0u : fw.budgetUsMin;
            const unsigned iterLo = fw.maxIterMin == 0xFFFFFFFFu ? 0u : fw.maxIterMin;
            const unsigned long long freeMin = fw.poolFreeMin == 0xFFFFFFFFFFFFFFFFull ? 0ull : fw.poolFreeMin;
            Log::get().note("fres pump probe 5s: calls=%llu busy=%llu boosted=%llu origBudgetUs=%u..%u origMaxIter=%u..%u "
                            "elapsedAvg=%.0fus busyAvg=%.0fus max=%.0fus drained=%llu hitTime=%llu hitIter=%llu "
                            "slotBlocked=%llu/%llu poolCap=%llu alloc=%llu freeMin=%llu activeMax=%llu "
                            "pendingMax=%u readyMax=%u incomingMax=%u",
                static_cast<unsigned long long>(fw.calls),
                static_cast<unsigned long long>(fw.busyCalls),
                static_cast<unsigned long long>(fw.boostedCalls),
                budgetLo,
                static_cast<unsigned>(fw.budgetUsMax),
                iterLo,
                static_cast<unsigned>(fw.maxIterMax),
                avgUs,
                busyAvgUs,
                fw.elapsedUsMax,
                static_cast<unsigned long long>(fw.drained),
                static_cast<unsigned long long>(fw.hitTime),
                static_cast<unsigned long long>(fw.hitIter),
                static_cast<unsigned long long>(fw.slotBlockedBefore),
                static_cast<unsigned long long>(fw.slotBlockedAfter),
                static_cast<unsigned long long>(fw.poolCap),
                static_cast<unsigned long long>(fw.poolAlloc),
                freeMin,
                static_cast<unsigned long long>(fw.activeMax),
                static_cast<unsigned>(fw.pendingMax),
                static_cast<unsigned>(fw.readyMax),
                static_cast<unsigned>(fw.incomingMax));
        } else {
            Log::get().note("fres pump probe 5s: calls=0 (not invoked in this window)");
        }
    }
}

inline void streamLoadProbeShutdown() {
    namespace d = stream_load_probe_detail;
    d::disarmSlot(d::g_fresSlot);
    d::disarmSlot(d::g_streamSlot);
    if (d::g_capPatched.exchange(false, std::memory_order_acq_rel)) {
        const uintptr_t addr = d::g_capPatchAddr.load(std::memory_order_acquire);
        if (addr >= d::kMinValidPtr) {
            d::patchSingleByte(addr, d::kStreamLoadCapBoost, d::kStreamLoadCapOrig);
        }
    }
}

}  // namespace edvr
