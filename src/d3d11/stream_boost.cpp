#include "stream_boost.h"
#include "stream_boost_core.h"

#include "../common/code_hook.h"
#include "../common/game_build.h"
#include "../common/log.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace edvr {
namespace {

using namespace stream_boost;

// Build 334925 targets (hook_signatures.py AUDIT_EXACT_ONLY; installs only when gameBuildKnown()).
constexpr uint32_t kStreamLoadRva = 0x9E86A0u;
constexpr uint32_t kStreamLoadCapPinRva = 0x9E892Cu;
constexpr size_t kStreamLoadCapImmOffset = 12u;  // 0x9E8938: imm8 of `cmp rsi, 0x1e` (48 83 FE 1E)

constexpr uint32_t kFresPumpRva = 0x7F6EB0u;
constexpr uint32_t kFresServiceGlobalRva = 0x5F2C3A8u;
constexpr uint32_t kFresSetMaxStreamsRva = 0x6FBDF0u;

constexpr size_t kPrologueBytes = 16u;
constexpr size_t kSetMaxStreamsPinBytes = 28u;

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

// StreamCategory and fResService read-only offsets (build 334925).
constexpr uintptr_t kMinValidPtr = 0x10000u;
constexpr size_t kOffEntriesPtr = 0x90u;
constexpr size_t kEntryStride = 24u;
constexpr size_t kOffEntryFlags = 0x14u;
constexpr size_t kOffPrioCount = 0xB8u;
constexpr size_t kOffPrioListPtr = 0xC0u;
constexpr size_t kPrioStride = 8u;
constexpr size_t kOffOverBudgetEvicting = 0xF0u;

constexpr size_t kOffFresServiceSched = 0x08u;
constexpr size_t kOffFresServiceMgr = 0x10u;
constexpr size_t kOffFresMgrPool = 0x18u;
constexpr size_t kOffFresMgrActive = 0x20u;
constexpr size_t kOffFresMgrPendingHead = 0x28u;
constexpr size_t kOffFresPoolAlloc = 0x10u;
constexpr size_t kOffFresPoolFree = 0x20u;
constexpr size_t kOffFresPoolCap = 0x28u;
constexpr size_t kOffFresActiveCount = 0x18u;

constexpr uint32_t kFlagLoadableBit = 0x20000000u;
constexpr uint64_t kMaxPrioScan = 8192u;
constexpr uint32_t kMaxEntryIndex = 1u << 24;
constexpr size_t kMaxCategories = 8u;
constexpr uint64_t kBurstQuietMs = 2000u;

// Relay layout matching the other engine hooks (44 bytes: gate check -> detour or trampoline).
constexpr size_t kRelayBytes = 44u;
constexpr size_t kOriginalLiteral = 36u;

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

struct StorageProbeDetails {
    bool hasSeekPenalty = false;
    bool incursSeekPenalty = false;
    bool hasTrim = false;
    bool trimEnabled = false;
    bool hasBusType = false;
    uint32_t busType = 0u;
};

struct CategorySignal {
    bool used = false;
    uintptr_t categoryPtr = 0u;
    uint64_t calls = 0u;
    uint32_t wantLoad = 0u;
    bool evicting = false;
};

struct BurstStats {
    uint64_t boostedCalls = 0u;
    uint64_t evictBrakes = 0u;
    uint64_t tightThrottles = 0u;
    double elapsedUsSum = 0.0;
    double elapsedUsMax = 0.0;
    uint64_t lastBoostTickMs = 0u;
    bool lastFlat = true;

    void reset() noexcept { *this = BurstStats{}; }
};

struct CategoryQuickHeader {
    uint8_t overBudgetEvicting;
    uint64_t prioCount;
    uintptr_t prioListPtr;
    uintptr_t entriesPtr;
};

struct PoolSnapshot {
    bool valid;
    uintptr_t service;
    uintptr_t sched;
    uint64_t poolAlloc;
    uint64_t poolFree;
    uint64_t poolCap;
    uint64_t activeCount;
};

HookSlot g_streamSlot{};
HookSlot g_fresSlot{};
std::atomic<uintptr_t> g_exeBase{0};
std::atomic<bool> g_attempted{false};
std::atomic<bool> g_capPatched{false};
std::atomic<uintptr_t> g_capPatchAddr{0};
std::atomic<uint8_t> g_capPatchedValue{kStockStreamLoadCap};
std::atomic<bool> g_setMaxStreamsPinned{false};
std::atomic<bool> g_poolBoostDone{false};
std::atomic<bool> g_poolBoostLogged{false};
std::atomic<uint64_t> g_poolRaisedFrom{kStockPoolCap};
std::atomic<uint64_t> g_poolRaisedTo{kStockPoolCap};

StreamBoostPlan g_plan{};

std::atomic_flag g_catLock = ATOMIC_FLAG_INIT;
CategorySignal g_catSignals[kMaxCategories]{};
std::atomic<uint32_t> g_maxWantLoad{0};
std::atomic<bool> g_anyEvicting{false};

std::atomic<bool> g_flatProfile{true};
std::atomic<uint64_t> g_frameMsEmaBits{0};  // IEEE-754 double bit-pattern
int64_t g_lastPresentQpc = 0;

std::atomic_flag g_burstLock = ATOMIC_FLAG_INIT;
BurstStats g_burst{};

double qpcUsPerTick() noexcept {
    static const double kUsPerTick = [] {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return f.QuadPart > 0 ? (1000000.0 / static_cast<double>(f.QuadPart)) : 0.0;
    }();
    return kUsPerTick;
}

double loadFrameMsEma() noexcept {
    const uint64_t bits = g_frameMsEmaBits.load(std::memory_order_relaxed);
    double v = 0.0;
    static_assert(sizeof(v) == sizeof(bits), "double must be 64 bits");
    std::memcpy(&v, &bits, sizeof(v));
    return v;
}

void storeFrameMsEma(double v) noexcept {
    uint64_t bits = 0u;
    std::memcpy(&bits, &v, sizeof(bits));
    g_frameMsEmaBits.store(bits, std::memory_order_relaxed);
}

bool detectWine() noexcept {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return false;
    return GetProcAddress(ntdll, "wine_get_version") != nullptr;
}

uint32_t detectLogicalCpus() noexcept {
    const DWORD active = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if (active > 0u) return static_cast<uint32_t>(active);
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    return static_cast<uint32_t>(si.dwNumberOfProcessors);
}

uint64_t detectTotalRamBytes() noexcept {
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (!GlobalMemoryStatusEx(&ms)) return 0u;
    return static_cast<uint64_t>(ms.ullTotalPhys);
}

HANDLE openExeVolumeHandle() noexcept {
    wchar_t exePath[MAX_PATH] = {};
    const DWORD exeLen = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (exeLen == 0u || exeLen >= MAX_PATH) return INVALID_HANDLE_VALUE;

    wchar_t volRoot[MAX_PATH] = {};
    if (!GetVolumePathNameW(exePath, volRoot, MAX_PATH)) return INVALID_HANDLE_VALUE;

    wchar_t devPath[MAX_PATH] = {};
    if (GetVolumeNameForVolumeMountPointW(volRoot, devPath, MAX_PATH)) {
        size_t len = wcslen(devPath);
        if (len > 0u && devPath[len - 1u] == L'\\') devPath[len - 1u] = L'\0';
        HANDLE h = CreateFileW(devPath, 0u, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0u, nullptr);
        if (h != INVALID_HANDLE_VALUE) return h;
    }

    // Fallback for drive-letter roots (e.g. subst or Wine where GUID mount lookup may fail).
    if (volRoot[0] != L'\0' && volRoot[1] == L':') {
        wchar_t driveDev[8] = {L'\\', L'\\', L'.', L'\\', volRoot[0], L':', L'\0', L'\0'};
        return CreateFileW(driveDev, 0u, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0u, nullptr);
    }
    return INVALID_HANDLE_VALUE;
}

StorageClass probeExeStorage(StorageProbeDetails* details) noexcept {
    if (details) *details = StorageProbeDetails{};
    HANDLE h = openExeVolumeHandle();
    if (h == INVALID_HANDLE_VALUE) return StorageClass::Unknown;

    StorageProbeDetails d{};
    DWORD returned = 0u;

    STORAGE_PROPERTY_QUERY qSeek{};
    qSeek.PropertyId = StorageDeviceSeekPenaltyProperty;
    qSeek.QueryType = PropertyStandardQuery;
    DEVICE_SEEK_PENALTY_DESCRIPTOR seekDesc{};
    if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &qSeek, sizeof(qSeek),
                        &seekDesc, sizeof(seekDesc), &returned, nullptr) &&
        returned >= sizeof(DEVICE_SEEK_PENALTY_DESCRIPTOR)) {
        d.hasSeekPenalty = true;
        d.incursSeekPenalty = (seekDesc.IncursSeekPenalty != FALSE);
    }

    STORAGE_PROPERTY_QUERY qBus{};
    qBus.PropertyId = StorageAdapterProperty;
    qBus.QueryType = PropertyStandardQuery;
    STORAGE_ADAPTER_DESCRIPTOR adapterDesc{};
    if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &qBus, sizeof(qBus),
                        &adapterDesc, sizeof(adapterDesc), &returned, nullptr) &&
        returned >= offsetof(STORAGE_ADAPTER_DESCRIPTOR, BusType) + sizeof(adapterDesc.BusType)) {
        d.hasBusType = true;
        d.busType = static_cast<uint32_t>(adapterDesc.BusType);
    }

    if (!d.hasSeekPenalty) {
        STORAGE_PROPERTY_QUERY qTrim{};
        qTrim.PropertyId = StorageDeviceTrimProperty;
        qTrim.QueryType = PropertyStandardQuery;
        DEVICE_TRIM_DESCRIPTOR trimDesc{};
        if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &qTrim, sizeof(qTrim),
                            &trimDesc, sizeof(trimDesc), &returned, nullptr) &&
            returned >= sizeof(DEVICE_TRIM_DESCRIPTOR)) {
            d.hasTrim = true;
            d.trimEnabled = (trimDesc.TrimEnabled != FALSE);
        }
    }

    CloseHandle(h);
    if (details) *details = d;
    return classifyStorage(d.hasSeekPenalty, d.incursSeekPenalty,
                           d.hasTrim, d.trimEnabled,
                           d.hasBusType ? d.busType : 0u);
}

uint8_t* allocateRelay(uintptr_t target) noexcept {
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

void buildRelay(uint8_t* code, const void* gate, void* callback) noexcept {
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

bool prepareRelay(void* trampoline, void* ctx) noexcept {
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

__declspec(noinline) bool readBytesGuarded(uintptr_t addr, uint8_t* out, size_t n) noexcept {
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(addr), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool patchSingleByte(uintptr_t addr, uint8_t expected, uint8_t replacement) noexcept {
    uint8_t cur = 0;
    if (!readBytesGuarded(addr, &cur, 1u) || cur != expected) return false;
    DWORD oldProtect = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(addr), 1u, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return false;
    }
    *reinterpret_cast<uint8_t*>(addr) = replacement;
    DWORD dummy = 0;
    VirtualProtect(reinterpret_cast<void*>(addr), 1u, oldProtect, &dummy);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<const void*>(addr), 1u);
    return true;
}

__declspec(noinline) bool callSetMaxStreamsLocked(uintptr_t exeBase, uintptr_t service,
                                                  uintptr_t sched, uint64_t newCap) noexcept {
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

__declspec(noinline) bool readCategoryQuickHeader(uintptr_t category, CategoryQuickHeader* out) noexcept {
    if (category < kMinValidPtr || !out) return false;
    std::memset(out, 0, sizeof(*out));
    __try {
        const auto* cat = reinterpret_cast<const uint8_t*>(category);
        std::memcpy(&out->entriesPtr, cat + kOffEntriesPtr, sizeof(out->entriesPtr));
        std::memcpy(&out->prioCount, cat + kOffPrioCount, sizeof(out->prioCount));
        std::memcpy(&out->prioListPtr, cat + kOffPrioListPtr, sizeof(out->prioListPtr));
        std::memcpy(&out->overBudgetEvicting, cat + kOffOverBudgetEvicting, sizeof(out->overBudgetEvicting));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Counts unloaded loadable entries, stopping as soon as count exceeds kBoostWantLoadThreshold.
__declspec(noinline) bool countWantLoadCapped(uintptr_t prioListPtr, uint64_t prioCount,
                                              uintptr_t entriesPtr, uint32_t* outCount) noexcept {
    if (!outCount) return false;
    *outCount = 0u;
    if (prioCount == 0u || prioListPtr < kMinValidPtr || entriesPtr < kMinValidPtr) return true;
    const uint64_t n = prioCount > kMaxPrioScan ? kMaxPrioScan : prioCount;
    const uint32_t stopAt = kBoostWantLoadThreshold + 1u;
    __try {
        const auto* prio = reinterpret_cast<const uint8_t*>(prioListPtr);
        const auto* entries = reinterpret_cast<const uint8_t*>(entriesPtr);
        uint32_t want = 0u;
        for (uint64_t i = 0; i < n; ++i) {
            uint32_t idx = 0;
            std::memcpy(&idx, prio + i * kPrioStride, sizeof(idx));
            if (idx >= kMaxEntryIndex) continue;
            uint32_t flags = 0;
            std::memcpy(&flags, entries + static_cast<size_t>(idx) * kEntryStride + kOffEntryFlags, sizeof(flags));
            if ((flags & kFlagLoadableBit) == 0u) continue;
            if ((flags & 3u) == 0u) {
                ++want;
                if (want >= stopAt) break;
            }
        }
        *outCount = want;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

__declspec(noinline) bool readBudgetPair(const uint32_t* pair, uint32_t* outTimeUs, uint32_t* outMaxIter) noexcept {
    if (reinterpret_cast<uintptr_t>(pair) < kMinValidPtr || !outTimeUs || !outMaxIter) return false;
    __try {
        *outTimeUs = pair[0];
        *outMaxIter = pair[1];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

__declspec(noinline) uint32_t countPendingCapped(uintptr_t exeBase, uint32_t limit) noexcept {
    if (exeBase < kMinValidPtr || limit == 0u) return 0u;
    __try {
        uintptr_t service = 0;
        std::memcpy(&service, reinterpret_cast<const void*>(exeBase + kFresServiceGlobalRva), sizeof(service));
        if (service < kMinValidPtr) return 0u;

        uintptr_t mgr = 0;
        std::memcpy(&mgr, reinterpret_cast<const void*>(service + kOffFresServiceMgr), sizeof(mgr));
        if (mgr < kMinValidPtr) return 0u;

        const uintptr_t sentinel = mgr + kOffFresMgrPendingHead;
        uintptr_t cur = 0;
        std::memcpy(&cur, reinterpret_cast<const void*>(sentinel), sizeof(cur));
        uint32_t count = 0u;
        while (cur >= kMinValidPtr && cur != sentinel && count < limit) {
            ++count;
            uintptr_t next = 0;
            std::memcpy(&next, reinterpret_cast<const void*>(cur), sizeof(next));
            if (next == cur) break;
            cur = next;
        }
        return count;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0u;
    }
}

__declspec(noinline) bool readPoolSnapshot(uintptr_t exeBase, PoolSnapshot* out) noexcept {
    if (!out) return false;
    std::memset(out, 0, sizeof(*out));
    if (exeBase < kMinValidPtr) return false;
    __try {
        uintptr_t service = 0;
        std::memcpy(&service, reinterpret_cast<const void*>(exeBase + kFresServiceGlobalRva), sizeof(service));
        if (service < kMinValidPtr) return false;

        uintptr_t sched = 0;
        uintptr_t mgr = 0;
        std::memcpy(&sched, reinterpret_cast<const void*>(service + kOffFresServiceSched), sizeof(sched));
        std::memcpy(&mgr, reinterpret_cast<const void*>(service + kOffFresServiceMgr), sizeof(mgr));
        if (mgr < kMinValidPtr) return false;

        uintptr_t pool = 0;
        uintptr_t active = 0;
        std::memcpy(&pool, reinterpret_cast<const void*>(mgr + kOffFresMgrPool), sizeof(pool));
        std::memcpy(&active, reinterpret_cast<const void*>(mgr + kOffFresMgrActive), sizeof(active));
        if (pool < kMinValidPtr) return false;

        out->service = service;
        out->sched = sched;
        std::memcpy(&out->poolAlloc, reinterpret_cast<const void*>(pool + kOffFresPoolAlloc), sizeof(out->poolAlloc));
        std::memcpy(&out->poolFree, reinterpret_cast<const void*>(pool + kOffFresPoolFree), sizeof(out->poolFree));
        std::memcpy(&out->poolCap, reinterpret_cast<const void*>(pool + kOffFresPoolCap), sizeof(out->poolCap));
        if (active >= kMinValidPtr) {
            std::memcpy(&out->activeCount, reinterpret_cast<const void*>(active + kOffFresActiveCount), sizeof(out->activeCount));
        }
        out->valid = true;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// --- Category signal aggregation & detours ---

void publishAggregatedSignalsLocked() noexcept {
    uint32_t maxWant = 0u;
    bool anyEvict = false;
    for (size_t i = 0; i < kMaxCategories; ++i) {
        if (!g_catSignals[i].used) continue;
        if (g_catSignals[i].wantLoad > maxWant) maxWant = g_catSignals[i].wantLoad;
        if (g_catSignals[i].evicting) anyEvict = true;
    }
    g_maxWantLoad.store(maxWant, std::memory_order_relaxed);
    g_anyEvicting.store(anyEvict, std::memory_order_relaxed);
}

CategorySignal* findOrAllocSignalLocked(uintptr_t categoryPtr) noexcept {
    for (size_t i = 0; i < kMaxCategories; ++i) {
        if (g_catSignals[i].used && g_catSignals[i].categoryPtr == categoryPtr) return &g_catSignals[i];
    }
    for (size_t i = 0; i < kMaxCategories; ++i) {
        if (!g_catSignals[i].used) {
            g_catSignals[i] = CategorySignal{};
            g_catSignals[i].used = true;
            g_catSignals[i].categoryPtr = categoryPtr;
            return &g_catSignals[i];
        }
    }
    return nullptr;
}

__declspec(noinline) void __fastcall streamLoadDetour(uintptr_t category, uintptr_t jobSys) noexcept {
    const auto forward = reinterpret_cast<StreamLoadFn>(g_streamSlot.forward.load(std::memory_order_acquire));
    if (!forward) return;
    forward(category, jobSys);

    if (g_streamSlot.gate.load(std::memory_order_acquire) == 0) return;

    CategoryQuickHeader hdr{};
    if (!readCategoryQuickHeader(category, &hdr)) return;

    bool needScan = false;
    if (!g_catLock.test_and_set(std::memory_order_acquire)) {
        if (CategorySignal* sig = findOrAllocSignalLocked(category)) {
            sig->calls += 1u;
            sig->evicting = (hdr.overBudgetEvicting != 0u);
            needScan = ((sig->calls % 8u) == 1u);
            publishAggregatedSignalsLocked();
        }
        g_catLock.clear(std::memory_order_release);
    }

    if (!needScan) return;

    uint32_t want = 0u;
    if (!countWantLoadCapped(hdr.prioListPtr, hdr.prioCount, hdr.entriesPtr, &want)) return;

    if (!g_catLock.test_and_set(std::memory_order_acquire)) {
        if (CategorySignal* sig = findOrAllocSignalLocked(category)) {
            sig->wantLoad = want;
            publishAggregatedSignalsLocked();
        }
        g_catLock.clear(std::memory_order_release);
    }
}

__declspec(noinline) void __fastcall fresPumpDetour(uintptr_t owner, const uint32_t* budgetPair) noexcept {
    const auto forward = reinterpret_cast<FresPumpFn>(g_fresSlot.forward.load(std::memory_order_acquire));
    if (!forward) return;

    if (g_fresSlot.gate.load(std::memory_order_acquire) == 0) {
        forward(owner, budgetPair);
        return;
    }

    const uintptr_t exeBase = g_exeBase.load(std::memory_order_relaxed);

    // One-time fResStreamPool expansion when 0 slots are checked out, under the fRes scheduler lock.
    if (!g_poolBoostDone.load(std::memory_order_relaxed) &&
        g_plan.poolCap > kStockPoolCap &&
        g_setMaxStreamsPinned.load(std::memory_order_acquire)) {
        PoolSnapshot snap{};
        if (readPoolSnapshot(exeBase, &snap) &&
            snap.valid &&
            snap.poolCap >= kStockPoolCap &&
            snap.poolCap < g_plan.poolCap &&
            snap.poolAlloc == snap.poolCap &&
            snap.poolFree == snap.poolAlloc &&
            snap.activeCount == 0u) {
            if (callSetMaxStreamsLocked(exeBase, snap.service, snap.sched, g_plan.poolCap)) {
                g_poolRaisedFrom.store(snap.poolCap, std::memory_order_relaxed);
                g_poolRaisedTo.store(g_plan.poolCap, std::memory_order_relaxed);
                g_poolBoostDone.store(true, std::memory_order_release);
            }
        }
    }

    uint32_t origTimeUs = 0u;
    uint32_t origMaxIter = 0u;
    if (!readBudgetPair(budgetPair, &origTimeUs, &origMaxIter)) {
        forward(owner, budgetPair);
        return;
    }

    const uint32_t wantLoad = g_maxWantLoad.load(std::memory_order_relaxed);
    uint32_t pending = 0u;
    if (wantLoad <= kBoostWantLoadThreshold) {
        pending = countPendingCapped(exeBase, kBoostPendingThreshold + 1u);
    }

    PumpInput in{};
    in.origTimeUs = origTimeUs;
    in.origMaxIter = origMaxIter;
    in.wantLoad = wantLoad;
    in.pending = pending;
    in.overBudgetEvicting = g_anyEvicting.load(std::memory_order_relaxed);
    in.flatProfile = g_flatProfile.load(std::memory_order_relaxed);
    in.frameMsEma = loadFrameMsEma();

    const PumpBudget chosen = selectPumpBudget(g_plan, in);
    if (!chosen.boosted) {
        if (chosen.evictBraked && !g_burstLock.test_and_set(std::memory_order_acquire)) {
            g_burst.evictBrakes += 1u;
            g_burst.lastBoostTickMs = GetTickCount64();
            g_burstLock.clear(std::memory_order_release);
        }
        forward(owner, budgetPair);
        return;
    }

    uint32_t callPair[2] = {chosen.timeUs, chosen.maxIter};
    const int64_t t0 = edvr::qpcNow();
    forward(owner, callPair);
    const int64_t t1 = edvr::qpcNow();

    const double elapsedUs = (t1 > t0) ? (static_cast<double>(t1 - t0) * qpcUsPerTick()) : 0.0;
    if (!g_burstLock.test_and_set(std::memory_order_acquire)) {
        g_burst.boostedCalls += 1u;
        if (chosen.tightThrottled) g_burst.tightThrottles += 1u;
        g_burst.elapsedUsSum += elapsedUs;
        if (elapsedUs > g_burst.elapsedUsMax) g_burst.elapsedUsMax = elapsedUs;
        g_burst.lastBoostTickMs = GetTickCount64();
        g_burst.lastFlat = in.flatProfile;
        g_burstLock.clear(std::memory_order_release);
    }
}

bool armSlot(HookSlot& slot, uintptr_t target, const uint8_t expected[kPrologueBytes],
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

void flushBurstReportIfReady(uint64_t nowMs, bool force) noexcept {
    BurstStats snap{};
    bool emit = false;
    if (!g_burstLock.test_and_set(std::memory_order_acquire)) {
        const bool hasActivity = (g_burst.boostedCalls > 0u || g_burst.evictBrakes > 0u);
        if (hasActivity && (force || (nowMs >= g_burst.lastBoostTickMs && (nowMs - g_burst.lastBoostTickMs) >= kBurstQuietMs))) {
            snap = g_burst;
            g_burst.reset();
            emit = true;
        }
        g_burstLock.clear(std::memory_order_release);
    }
    if (!emit) return;

    const double avgMs = snap.boostedCalls > 0u
        ? (snap.elapsedUsSum / static_cast<double>(snap.boostedCalls)) / 1000.0
        : 0.0;
    const double maxMs = snap.elapsedUsMax / 1000.0;
    Log::get().note(
        "stream_boost: load burst finished (boostedCalls=%llu avg=%.2fms max=%.2fms tight=%llu evictBrake=%llu profile=%s)",
        static_cast<unsigned long long>(snap.boostedCalls),
        avgMs,
        maxMs,
        static_cast<unsigned long long>(snap.tightThrottles),
        static_cast<unsigned long long>(snap.evictBrakes),
        snap.lastFlat ? "flat" : "vr");
}

}  // namespace

void streamBoostInstall() noexcept {
    bool expected = false;
    if (!g_attempted.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return;

    if (!gameBuildKnown()) {
        Log::get().note("stream_boost: standing down (%s)", "unknown build");
        return;
    }

    const HMODULE exe = GetModuleHandleW(nullptr);
    if (!exe) {
        Log::get().note("stream_boost: standing down (%s)", "no main module");
        return;
    }
    const uintptr_t base = reinterpret_cast<uintptr_t>(exe);
    g_exeBase.store(base, std::memory_order_release);

    StorageProbeDetails storageDetails{};
    SystemProfile sys{};
    sys.storage = probeExeStorage(&storageDetails);
    sys.underWine = detectWine();
    sys.logicalCpus = detectLogicalCpus();
    sys.totalRamBytes = detectTotalRamBytes();

    const StreamBoostPlan plan = computeStreamBoostPlan(sys);
    g_plan = plan;
    const double ramGiB = static_cast<double>(sys.totalRamBytes) / static_cast<double>(kGiB);

    if (!plan.active) {
        Log::get().note(
            "stream_boost: standing down (reason=%s storage=%s seekPenalty=%d bus=%u wine=%d cpus=%u ram=%.1fGiB)",
            plan.standDownReason ? plan.standDownReason : "inactive",
            storageClassName(sys.storage),
            storageDetails.hasSeekPenalty ? (storageDetails.incursSeekPenalty ? 1 : 0) : -1,
            storageDetails.hasBusType ? storageDetails.busType : 0u,
            sys.underWine ? 1 : 0,
            sys.logicalCpus,
            ramGiB);
        return;
    }

    // 1. Raise StreamLoad per-tick concurrency cap (30 -> plan.streamLoadCap) with 16-byte pin.
    bool capPatchedNow = false;
    if (plan.streamLoadCap > kStockStreamLoadCap) {
        uint8_t capBytes[kPrologueBytes] = {};
        if (readBytesGuarded(base + kStreamLoadCapPinRva, capBytes, kPrologueBytes) &&
            std::memcmp(capBytes, kStreamLoadCapPin, kPrologueBytes) == 0) {
            const uintptr_t immAddr = base + kStreamLoadCapPinRva + kStreamLoadCapImmOffset;
            if (patchSingleByte(immAddr, kStockStreamLoadCap, plan.streamLoadCap)) {
                g_capPatchAddr.store(immAddr, std::memory_order_release);
                g_capPatchedValue.store(plan.streamLoadCap, std::memory_order_release);
                g_capPatched.store(true, std::memory_order_release);
                capPatchedNow = true;
            }
        }
    }

    // 2. Verify SetMaxConcurrentStreams pin if plan raises poolCap above stock 16.
    bool poolPinnedNow = false;
    if (plan.poolCap > kStockPoolCap) {
        uint8_t setMaxBytes[kSetMaxStreamsPinBytes] = {};
        if (readBytesGuarded(base + kFresSetMaxStreamsRva, setMaxBytes, kSetMaxStreamsPinBytes) &&
            std::memcmp(setMaxBytes, kSetMaxStreamsPin, kSetMaxStreamsPinBytes) == 0) {
            g_setMaxStreamsPinned.store(true, std::memory_order_release);
            poolPinnedNow = true;
        }
    }

    // 3. Install lightweight StreamLoad signal hook and fRes completion pump hook.
    const bool streamOk = armSlot(g_streamSlot, base + kStreamLoadRva, kStreamLoadPrologue,
                                  reinterpret_cast<void*>(&streamLoadDetour),
                                  "stream_boost_load", "stream_boost(load)");
    const bool fresOk = armSlot(g_fresSlot, base + kFresPumpRva, kFresPumpPrologue,
                                reinterpret_cast<void*>(&fresPumpDetour),
                                "stream_boost_pump", "stream_boost(pump)");

    Log::get().note(
        "stream_boost: active (storage=%s seekPenalty=%d bus=%u wine=%d cpus=%u ram=%.1fGiB -> streamCap=%u/%u poolCap=%u/%u flatPump=%uus/%u vrPump=%uus/%u hooks=%d/%d)",
        storageClassName(sys.storage),
        storageDetails.hasSeekPenalty ? (storageDetails.incursSeekPenalty ? 1 : 0) : -1,
        storageDetails.hasBusType ? storageDetails.busType : 0u,
        sys.underWine ? 1 : 0,
        sys.logicalCpus,
        ramGiB,
        capPatchedNow ? static_cast<unsigned>(plan.streamLoadCap) : static_cast<unsigned>(kStockStreamLoadCap),
        static_cast<unsigned>(plan.streamLoadCap),
        poolPinnedNow ? plan.poolCap : kStockPoolCap,
        plan.poolCap,
        plan.flatPumpTimeUs,
        plan.flatPumpMaxIter,
        plan.vrPumpTimeUs,
        plan.vrPumpMaxIter,
        streamOk ? 1 : 0,
        fresOk ? 1 : 0);
}

void streamBoostNoteFrame(bool flatProfile) noexcept {
    g_flatProfile.store(flatProfile, std::memory_order_relaxed);
    const int64_t now = edvr::qpcNow();
    if (g_lastPresentQpc > 0 && now > g_lastPresentQpc) {
        const double ms = (static_cast<double>(now - g_lastPresentQpc) * qpcUsPerTick()) / 1000.0;
        const double next = updateFrameMsEma(loadFrameMsEma(), ms);
        storeFrameMsEma(next);
    }
    g_lastPresentQpc = now;
}

void streamBoostPoll() noexcept {
    if (!g_streamSlot.installed.load(std::memory_order_acquire) &&
        !g_fresSlot.installed.load(std::memory_order_acquire)) {
        return;
    }
    if (g_poolBoostDone.load(std::memory_order_acquire) &&
        !g_poolBoostLogged.exchange(true, std::memory_order_acq_rel)) {
        Log::get().note("stream_boost: fResStreamPool raised %llu -> %llu",
                        static_cast<unsigned long long>(g_poolRaisedFrom.load(std::memory_order_relaxed)),
                        static_cast<unsigned long long>(g_poolRaisedTo.load(std::memory_order_relaxed)));
    }
    flushBurstReportIfReady(GetTickCount64(), false);
}

void streamBoostShutdown() noexcept {
    g_streamSlot.gate.store(0, std::memory_order_release);
    g_fresSlot.gate.store(0, std::memory_order_release);

    if (g_capPatched.exchange(false, std::memory_order_acq_rel)) {
        const uintptr_t addr = g_capPatchAddr.load(std::memory_order_acquire);
        const uint8_t patchedVal = g_capPatchedValue.load(std::memory_order_acquire);
        if (addr >= kMinValidPtr) {
            patchSingleByte(addr, patchedVal, kStockStreamLoadCap);
        }
    }

    flushBurstReportIfReady(GetTickCount64(), true);

    if (g_streamSlot.installed.exchange(false, std::memory_order_acq_rel)) {
        g_streamSlot.hook.uninstall();
    }
    if (g_fresSlot.installed.exchange(false, std::memory_order_acq_rel)) {
        g_fresSlot.hook.uninstall();
    }
}

}  // namespace edvr
