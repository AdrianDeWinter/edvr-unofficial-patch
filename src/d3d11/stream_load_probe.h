#pragma once
// Read-only diagnostic probe on Cobra's StreamLoad (RVA 0x9E86A0 in build 334925).
// Samples per-category concurrency, memory budgets, eviction ticks, priority-list backlog,
// and in-flight texture upload bytes, reporting once every 5 seconds per active category.
// Zero writes to game memory; stands down cleanly if PE identity or prologue does not match.

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

// Build 334925 StreamLoad entry point and texture upload in-flight counter
// (hook_signatures.py AUDIT_EXACT_ONLY; installs only when gameBuildKnown()).
constexpr uint32_t kStreamLoadRva = 0x9E86A0u;
constexpr uint32_t kTexUploadInFlightRva = 0x5F2D1B8u;

constexpr size_t kPrologueBytes = 16;
// push rbp; push r12; push r13; push r14; push r15; sub rsp, 0x110
constexpr uint8_t kExpectedPrologue[kPrologueBytes] = {
    0x40, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56,
    0x41, 0x57, 0x48, 0x81, 0xEC, 0x10, 0x01, 0x00,
};

// StreamCategory and related read-only offsets (build 334925).
constexpr uintptr_t kMinValidPtr = 0x10000u;
constexpr size_t kOffCategoryId = 0x08u;
constexpr size_t kOffCpuBudgetBytes = 0x10u;
constexpr size_t kOffGpuBudgetBytes = 0x18u;
constexpr size_t kOffProviderCount = 0x50u;
constexpr size_t kOffProvidersPtr = 0x58u;
constexpr size_t kProviderStride = 16u;
constexpr size_t kOffProviderInFlight = 0x78u;
constexpr size_t kOffEntriesPtr = 0x90u;
constexpr size_t kEntryStride = 24u;
constexpr size_t kOffEntryGpuBytes = 0x0Cu;
constexpr size_t kOffEntryFlags = 0x14u;
constexpr size_t kOffPrioCount = 0xB8u;
constexpr size_t kOffPrioListPtr = 0xC0u;
constexpr size_t kPrioStride = 8u;
constexpr size_t kOffOverBudgetEvicting = 0xF0u;
constexpr size_t kOffExtMemPtr = 0xF8u;
constexpr size_t kOffExtCpuBytes = 0x10u;
constexpr size_t kOffExtGpuBytes = 0x18u;

constexpr uint32_t kFlagLoadableBit = 0x20000000u;
constexpr uint64_t kMaxProvidersScan = 512u;
constexpr uint64_t kMaxPrioScan = 8192u;
constexpr uint32_t kMaxEntryIndex = 0x1000000u;
constexpr uint64_t kConcurrencyCap = 30u;
constexpr uint64_t kReportIntervalMs = 5000u;
constexpr size_t kMaxCategories = 8;

// Relay layout matching the other engine hooks (44 bytes: gate check -> detour or trampoline).
constexpr size_t kRelayBytes = 44;
constexpr size_t kOriginalLiteral = 36;

using StreamLoadFn = void(__fastcall*)(uintptr_t category, uintptr_t jobSys);

struct CategoryHeader {
    uint16_t categoryId;
    uint8_t overBudgetEvicting;
    uint64_t cpuBudgetBytes;
    uint64_t gpuBudgetBytes;
    uint64_t extCpuBytes;
    uint64_t extGpuBytes;
    uint64_t providerCount;
    uint64_t inFlight;
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
    uint64_t cap30Ticks = 0;
    uint64_t evictTicks = 0;
    uint32_t maxInFlight = 0;
    uint32_t wantLoadMax = 0;
    uint32_t reqLoadMax = 0;
    uint32_t loadingMax = 0;
    uint32_t loadedLatest = 0;
    uint32_t unloadingLatest = 0;
    uint64_t loadedGpuBytes = 0;
    uint64_t texUploadMaxBytes = 0;
    bool cap30ScannedInWindow = false;

    void resetWindow() noexcept {
        calls = 0;
        cap30Ticks = 0;
        evictTicks = 0;
        maxInFlight = 0;
        wantLoadMax = 0;
        reqLoadMax = 0;
        loadingMax = 0;
        loadedLatest = 0;
        unloadingLatest = 0;
        loadedGpuBytes = 0;
        texUploadMaxBytes = 0;
        cap30ScannedInWindow = false;
    }
};

alignas(8) inline std::atomic<uintptr_t> g_relayGate{0};
static_assert(decltype(g_relayGate)::is_always_lock_free, "The relay reads the aligned atomic cell directly.");

inline uint8_t* g_relay = nullptr;
inline std::atomic<uintptr_t> g_forward{0};
inline std::atomic<uintptr_t> g_exeBase{0};
inline std::atomic<bool> g_attempted{false};
inline std::atomic<bool> g_installed{false};
inline CodeHook g_hook;

inline std::atomic_flag g_bucketsLock = ATOMIC_FLAG_INIT;
inline CategoryBucket g_buckets[kMaxCategories]{};

inline bool g_pollStarted = false;
inline uint64_t g_windowStartMs = 0;

inline double bytesToMb(uint64_t bytes) noexcept {
    return static_cast<double>(bytes) / (1024.0 * 1024.0);
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
    // mov rax, &gate; cmp qword ptr [rax], 0; je original; jmp [callback]; original: jmp [trampoline].
    // StreamLoad is standard Win64 __fastcall(rcx, rdx) and does not read RAX or flags on entry.
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

inline bool prepareRelay(void* trampoline, void*) noexcept {
    const uintptr_t address = reinterpret_cast<uintptr_t>(trampoline);
    std::memcpy(g_relay + kOriginalLiteral, &address, 8);
    DWORD oldProtect = 0;
    if (!VirtualProtect(g_relay, 4096, PAGE_EXECUTE_READ, &oldProtect) ||
        !FlushInstructionCache(GetCurrentProcess(), g_relay, kRelayBytes)) {
        return false;
    }
    g_forward.store(address, std::memory_order_release);
    return true;
}

// --- SEH-isolated plain-POD readers (no C++ unwindable objects in scope) ---

__declspec(noinline) inline bool readPrologueBytes(uintptr_t addr, uint8_t out[kPrologueBytes]) noexcept {
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(addr), kPrologueBytes);
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

        uintptr_t providersPtr = 0;
        std::memcpy(&providersPtr, cat + kOffProvidersPtr, sizeof(providersPtr));
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

        uint64_t inFlightSum = 0;
        if (providersPtr >= kMinValidPtr && out->providerCount > 0) {
            const uint64_t provN = out->providerCount <= kMaxProvidersScan ? out->providerCount : kMaxProvidersScan;
            for (uint64_t i = 0; i < provN; ++i) {
                uintptr_t prov = 0;
                std::memcpy(&prov, reinterpret_cast<const uint8_t*>(providersPtr) + i * kProviderStride, sizeof(prov));
                if (prov >= kMinValidPtr) {
                    uint64_t provInFlight = 0;
                    std::memcpy(&provInFlight, reinterpret_cast<const uint8_t*>(prov) + kOffProviderInFlight, sizeof(provInFlight));
                    inFlightSum += provInFlight;
                }
            }
        }
        out->inFlight = inFlightSum;

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
            uint32_t gpuBytes = 0;
            uint32_t flags = 0;
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
    const auto forward = reinterpret_cast<StreamLoadFn>(g_forward.load(std::memory_order_acquire));
    if (!forward) return;
    forward(category, jobSys);

    if (g_relayGate.load(std::memory_order_acquire) == 0) return;

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
            const uint32_t inFlight32 = hdr.inFlight > 0xFFFFFFFFull
                ? 0xFFFFFFFFu
                : static_cast<uint32_t>(hdr.inFlight);
            if (inFlight32 > b->maxInFlight) b->maxInFlight = inFlight32;
            if (hdr.inFlight >= kConcurrencyCap) b->cap30Ticks += 1u;
            if (hdr.texUploadInFlightBytes > b->texUploadMaxBytes) {
                b->texUploadMaxBytes = hdr.texUploadInFlightBytes;
            }
            if ((b->calls % 16u) == 1u) {
                needScan = true;
                if (hdr.inFlight >= kConcurrencyCap) b->cap30ScannedInWindow = true;
            } else if (hdr.inFlight >= kConcurrencyCap && !b->cap30ScannedInWindow) {
                b->cap30ScannedInWindow = true;
                needScan = true;
            }
        }
        g_bucketsLock.clear(std::memory_order_release);
    }

    if (!needScan) return;

    PrioScan scan{};
    if (!readPrioScan(hdr.prioListPtr, hdr.prioCount, hdr.entriesPtr, &scan)) return;

    if (!g_bucketsLock.test_and_set(std::memory_order_acquire)) {
        if (CategoryBucket* b = findBucket(category)) {
            if (scan.wantLoad > b->wantLoadMax) b->wantLoadMax = scan.wantLoad;
            if (scan.reqLoad > b->reqLoadMax) b->reqLoadMax = scan.reqLoad;
            if (scan.loading > b->loadingMax) b->loadingMax = scan.loading;
            b->loadedLatest = scan.loaded;
            b->unloadingLatest = scan.unloading;
            b->loadedGpuBytes = scan.loadedGpuBytes;
        }
        g_bucketsLock.clear(std::memory_order_release);
    }
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

    const uintptr_t target = exeBase + d::kStreamLoadRva;
    uint8_t got[d::kPrologueBytes] = {};
    if (!d::readPrologueBytes(target, got)) {
        Log::get().note("stream load probe: inert (%s)", "prologue unreadable at exe+0x9E86A0");
        return;
    }
    if (std::memcmp(got, d::kExpectedPrologue, d::kPrologueBytes) != 0) {
        Log::get().note("stream load probe: inert (%s)", "prologue mismatch at exe+0x9E86A0");
        return;
    }

    uint8_t* relay = d::allocateRelay(target);
    if (!relay) {
        Log::get().note("stream load probe: inert (%s)", "no relay memory within 2 GB of target");
        return;
    }
    d::g_relay = relay;
    d::g_exeBase.store(exeBase, std::memory_order_release);
    d::buildRelay(relay, &d::g_relayGate, reinterpret_cast<void*>(&d::streamLoadDetour));

    if (!d::g_hook.install(
            reinterpret_cast<void*>(target),
            relay,
            nullptr,
            "stream-load-probe",
            &d::prepareRelay,
            nullptr)) {
        VirtualFree(relay, 0, MEM_RELEASE);
        d::g_relay = nullptr;
        const char* why = d::g_hook.refusal();
        Log::get().note("stream load probe: inert (%s)", (why && why[0]) ? why : "CodeHook refused target");
        return;
    }

    d::g_installed.store(true, std::memory_order_release);
    d::g_relayGate.store(1, std::memory_order_release);
    Log::get().note("stream load probe: armed at exe+0x9E86A0 (build 334925 verified, read-only)");
}

inline void streamLoadProbePoll(uint64_t nowMs = 0) {
    namespace d = stream_load_probe_detail;
    if (!d::g_installed.load(std::memory_order_acquire)) return;
    if (nowMs == 0) nowMs = GetTickCount64();
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
                        "cpuBudget=%.1fMB extCpu=%.1fMB inFlightMax=%u cap30Ticks=%llu evictTicks=%llu prios=%u "
                        "wantLoad=%u req=%u loading=%u loaded=%u texUpload=%.2fMB",
            static_cast<unsigned>(b.categoryId),
            static_cast<unsigned long long>(b.calls),
            d::bytesToMb(b.gpuBudgetBytes),
            d::bytesToMb(b.extGpuBytes),
            d::bytesToMb(b.loadedGpuBytes),
            d::bytesToMb(b.cpuBudgetBytes),
            d::bytesToMb(b.extCpuBytes),
            static_cast<unsigned>(b.maxInFlight),
            static_cast<unsigned long long>(b.cap30Ticks),
            static_cast<unsigned long long>(b.evictTicks),
            prios,
            static_cast<unsigned>(b.wantLoadMax),
            static_cast<unsigned>(b.reqLoadMax),
            static_cast<unsigned>(b.loadingMax),
            static_cast<unsigned>(b.loadedLatest),
            d::bytesToMb(b.texUploadMaxBytes));
    }
}

inline void streamLoadProbeShutdown() {
    namespace d = stream_load_probe_detail;
    d::g_relayGate.store(0, std::memory_order_release);
    d::g_installed.store(false, std::memory_order_release);
    d::g_hook.uninstall();
    if (d::g_relay) {
        VirtualFree(d::g_relay, 0, MEM_RELEASE);
        d::g_relay = nullptr;
    }
    d::g_forward.store(0, std::memory_order_release);
}

}  // namespace edvr
