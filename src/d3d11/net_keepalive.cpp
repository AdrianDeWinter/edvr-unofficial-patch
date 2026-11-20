// Build 334925 only (hook_signatures.py AUDIT_EXACT_ONLY). The two functions and
// the pump's calls to them are not in the signature table yet, so an unknown
// build stands down instead of guessing. The bytes below were read from
// EliteDangerous64 of that build.
//
// curl_multi_init at 0x48D9930 begins `mov edx, 0x61` (BA 61 00 00 00). The
// pump calls it at 0x5C41A4 (`call` then `mov rdx, [r12+68h]`); the return
// address is 0x5C41A9, and the rel32 lands on 0x48D9930.
// curl_multi_cleanup at 0x48D9640 begins `push rdi; sub rsp, 20h`. The pump
// calls it at 0x5C44B7, after `mov rcx, r13` at 0x5C44B4; the return address
// is 0x5C44BC, and the rel32 lands on 0x48D9640.
//
// Both hooks are published before either one keeps a handle. Until then each
// wrapper calls the original, so a pump that runs during install still pairs
// its own init with its own cleanup.
//
// The five-byte jump cannot reach this DLL. The flight on 2026-11-19 stood
// down with "00007FF6DB629930 is more than two gigabytes from the replacement".
// Each site gets a page reserved beside the exe, CodeHook jumps there, and
// that page tail-jumps to the wrapper. Do not pass the wrapper to CodeHook.
#include "net_keepalive.h"

#include "net_keepalive_core.h"

#include <windows.h>

#include <intrin.h>

#include <atomic>
#include <cstdint>
#include <cstring>

#include "../common/code_hook.h"
#include "../common/game_build.h"
#include "../common/log.h"

namespace edvr {
namespace {

constexpr std::uint32_t kMultiInitRva = 0x48D9930u;
constexpr std::uint32_t kMultiCleanupRva = 0x48D9640u;
constexpr std::uint32_t kInitCallRva = 0x5C41A4u;
constexpr std::uint32_t kCleanupCallRva = 0x5C44B4u;
constexpr std::uint32_t kInitReturnRva = 0x5C41A9u;
constexpr std::uint32_t kCleanupReturnRva = 0x5C44BCu;

constexpr std::uint8_t kInitPrologue[] = {0xBA, 0x61, 0x00, 0x00, 0x00};
constexpr std::uint8_t kCleanupPrologue[] = {0x40, 0x57, 0x48, 0x83, 0xEC, 0x20};
constexpr std::uint8_t kInitCall[] = {0xE8, 0x87, 0x57, 0x31, 0x04, 0x49, 0x8B, 0x54, 0x24, 0x68};
constexpr std::uint8_t kCleanupCall[] = {0x49, 0x8B, 0xCD, 0xE8, 0x84, 0x51, 0x31, 0x04};

using InitFn = void* (*)();
using CleanupFn = int (*)(void*);

NetPool g_pool;
CodeHook g_initHook;
CodeHook g_cleanupHook;
std::uint8_t* g_initRelay = nullptr;
std::uint8_t* g_cleanupRelay = nullptr;
std::atomic<void*> g_origInit{nullptr};
std::atomic<void*> g_origCleanup{nullptr};
std::atomic<int> g_phase{0};  // 0: call the original; 1: keep handles
std::uintptr_t g_imageBase = 0;
std::atomic<bool> g_ran{false};

bool readBytes(const void* at, std::uint8_t* dst, std::size_t n) noexcept {
    __try {
        std::memcpy(dst, at, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool sameBytes(const std::uint8_t* got, const std::uint8_t* pin, std::size_t n) noexcept {
    for (std::size_t i = 0; i < n; ++i)
        if (got[i] != pin[i]) return false;
    return true;
}

std::uint64_t nowMs() { return GetTickCount64(); }

bool threadAlive(std::uint32_t tid) {
    HANDLE thread = OpenThread(SYNCHRONIZE, FALSE, tid);
    if (!thread) return false;
    const DWORD wait = WaitForSingleObject(thread, 0);
    CloseHandle(thread);
    return wait == WAIT_TIMEOUT;
}

std::uint32_t currentTid() { return GetCurrentThreadId(); }

KeepFns keepFns() {
    KeepFns env;
    env.init = reinterpret_cast<InitFn>(g_origInit.load(std::memory_order_acquire));
    env.cleanup = reinterpret_cast<CleanupFn>(g_origCleanup.load(std::memory_order_acquire));
    env.threadId = &currentTid;
    env.pool = &g_pool;
    env.initReturnRva = kInitReturnRva;
    env.cleanupReturnRva = kCleanupReturnRva;
    return env;
}

__declspec(noinline) void* multiInitWrap() {
    InitFn orig = reinterpret_cast<InitFn>(g_origInit.load(std::memory_order_acquire));
    if (g_phase.load(std::memory_order_acquire) != 1 || orig == nullptr) return orig ? orig() : nullptr;
    const auto rva = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(_ReturnAddress()) - g_imageBase);
    return keepInit(keepFns(), rva);
}

__declspec(noinline) int multiCleanupWrap(void* multi) {
    CleanupFn orig = reinterpret_cast<CleanupFn>(g_origCleanup.load(std::memory_order_acquire));
    if (g_phase.load(std::memory_order_acquire) != 1 || orig == nullptr) return orig ? orig(multi) : 0;
    const auto rva = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(_ReturnAddress()) - g_imageBase);
    return keepCleanup(keepFns(), rva, multi);
}

bool prepareInit(void* trampoline, void*) noexcept {
    g_origInit.store(trampoline, std::memory_order_release);
    return true;
}

bool prepareCleanup(void* trampoline, void*) noexcept {
    g_origCleanup.store(trampoline, std::memory_order_release);
    return true;
}

DWORD WINAPI reportThread(void*) {
    const std::uint64_t start = GetTickCount64();
    std::uint64_t last = 0;
    for (;;) {
        Sleep(static_cast<DWORD>(kKeepReportPeriodMs));
        const std::uint64_t now = GetTickCount64();
        const std::uint64_t elapsed = now >= start ? now - start : kKeepReportDeadlineMs;
        const KeepCounts counts = g_pool.counts();
        const std::uint64_t activity = keepActivity(counts);
        const KeepReportTick tick = keepReportTick(elapsed, kKeepReportDeadlineMs, activity, last);
        if (tick.emit) {
            char line[400];
            if (tick.done) formatKeepFinal(counts, static_cast<unsigned>(kKeepReportDeadlineMs / 1000), line, sizeof(line));
            else formatKeepCounts(counts, line, sizeof(line));
            Log::get().note("%s", line);
        }
        if (tick.done) return 0;
        last = activity;
    }
}

void standDown(const char* why) {
    Log::get().note("connection reuse: standing down: %s; nothing is hooked", why);
}

// A page within rel32 of `target`. The same search the other engine hooks
// carry (vertex_resync_hook.cpp's allocateRelay): copied rather than shared,
// so none of those files is touched.
std::uint8_t* allocateRelay(std::uintptr_t target) noexcept {
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    const std::uintptr_t granularity = info.dwAllocationGranularity;
    const std::uintptr_t floor = reinterpret_cast<std::uintptr_t>(info.lpMinimumApplicationAddress);
    const std::uintptr_t ceiling = reinterpret_cast<std::uintptr_t>(info.lpMaximumApplicationAddress);
    const std::uintptr_t distance = static_cast<std::uintptr_t>(INT32_MAX) - 0x10000u;
    std::uintptr_t at = target > distance ? target - distance : floor;
    if (at < floor) at = floor;
    const std::uintptr_t limit = target > ceiling - distance ? ceiling : target + distance;
    while (at < limit) {
        MEMORY_BASIC_INFORMATION region{};
        if (!VirtualQuery(reinterpret_cast<void*>(at), &region, sizeof(region))) break;
        const std::uintptr_t start = reinterpret_cast<std::uintptr_t>(region.BaseAddress);
        if (region.RegionSize > UINTPTR_MAX - start) break;
        const std::uintptr_t end = start + region.RegionSize;
        if (region.State == MEM_FREE) {
            std::uintptr_t candidate = at > start ? at : start;
            if (candidate > UINTPTR_MAX - (granularity - 1)) break;
            candidate = (candidate + granularity - 1) & ~(granularity - 1);
            if (candidate < limit && candidate < end && end - candidate >= 4096) {
                auto* page = static_cast<std::uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(candidate), 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
                if (page) return page;
            }
        }
        if (end <= at) break;
        at = end;
    }
    return nullptr;
}

// Null when the page is executable and holds the tail jump. On failure the
// page is released: nothing can have entered it, because the exe is not
// patched yet. A page whose hook did publish is kept even if a later site
// stands down; a pump may be inside it.
const char* placeRelay(std::uint8_t*& relay, std::uintptr_t site, void* wrap) noexcept {
    relay = allocateRelay(site);
    if (!relay) return "no page could be reserved within two gigabytes of the function";
    writeKeepRelay(relay, wrap);
    DWORD oldProtect = 0;
    if (!VirtualProtect(relay, 4096, PAGE_EXECUTE_READ, &oldProtect) ||
        !FlushInstructionCache(GetCurrentProcess(), relay, kKeepRelayBytes)) {
        VirtualFree(relay, 0, MEM_RELEASE);
        relay = nullptr;
        return "the relay page could not be made executable";
    }
    return nullptr;
}

}  // namespace

void netKeepaliveInstall() noexcept {
    bool expected = false;
    if (!g_ran.compare_exchange_strong(expected, true)) return;

    if (!gameBuildKnown()) {
        standDown("this process is not build 334925");
        return;
    }
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (!base) {
        standDown("the executable's base is unknown");
        return;
    }
    std::uint8_t initCall[sizeof(kInitCall)] = {};
    std::uint8_t cleanupCall[sizeof(kCleanupCall)] = {};
    std::uint8_t initPrologue[sizeof(kInitPrologue)] = {};
    std::uint8_t cleanupPrologue[sizeof(kCleanupPrologue)] = {};
    const std::uint8_t* image = reinterpret_cast<const std::uint8_t*>(base);
    const bool calls = readBytes(image + kInitCallRva, initCall, sizeof(initCall)) &&
                       readBytes(image + kCleanupCallRva, cleanupCall, sizeof(cleanupCall));
    const bool prologues = readBytes(image + kMultiInitRva, initPrologue, sizeof(initPrologue)) &&
                           readBytes(image + kMultiCleanupRva, cleanupPrologue, sizeof(cleanupPrologue));
    if (!calls || !prologues) {
        standDown("the pump's curl calls could not be read");
        return;
    }
    const bool callPins = sameBytes(initCall, kInitCall, sizeof(kInitCall)) && sameBytes(cleanupCall, kCleanupCall, sizeof(kCleanupCall));
    const bool prologuePins = sameBytes(initPrologue, kInitPrologue, sizeof(kInitPrologue)) &&
                              sameBytes(cleanupPrologue, kCleanupPrologue, sizeof(kCleanupPrologue));
    if (!keepArming(true, true, callPins, prologuePins)) {
        standDown(callPins ? "curl_multi_init or curl_multi_cleanup does not begin with build 334925's bytes"
                           : "the pump's call to curl_multi_init or curl_multi_cleanup is not build 334925's bytes");
        return;
    }

    g_imageBase = base;
    KeepEnv env;
    env.nowMs = &nowMs;
    env.isAlive = &threadAlive;
    const auto initSite = base + kMultiInitRva;
    const auto cleanupSite = base + kMultiCleanupRva;
    if (const char* why = placeRelay(g_initRelay, initSite, reinterpret_cast<void*>(&multiInitWrap))) {
        standDown(why);
        return;
    }
    if (!g_initHook.install(reinterpret_cast<void*>(initSite), g_initRelay, nullptr, "connection-reuse-init", &prepareInit, nullptr)) {
        VirtualFree(g_initRelay, 0, MEM_RELEASE);
        g_initRelay = nullptr;
        standDown(g_initHook.refusal()[0] ? g_initHook.refusal() : "curl_multi_init could not be hooked");
        return;
    }
    if (const char* why = placeRelay(g_cleanupRelay, cleanupSite, reinterpret_cast<void*>(&multiCleanupWrap))) {
        g_initHook.uninstall();
        standDown(why);
        return;
    }
    if (!g_cleanupHook.install(reinterpret_cast<void*>(cleanupSite), g_cleanupRelay, nullptr, "connection-reuse-cleanup", &prepareCleanup, nullptr)) {
        VirtualFree(g_cleanupRelay, 0, MEM_RELEASE);
        g_cleanupRelay = nullptr;
        g_initHook.uninstall();
        standDown(g_cleanupHook.refusal()[0] ? g_cleanupHook.refusal() : "curl_multi_cleanup could not be hooked");
        return;
    }
    env.init = reinterpret_cast<InitFn>(g_origInit.load(std::memory_order_acquire));
    env.cleanup = reinterpret_cast<CleanupFn>(g_origCleanup.load(std::memory_order_acquire));
    if (env.init == nullptr || env.cleanup == nullptr) {
        g_cleanupHook.uninstall();
        g_initHook.uninstall();
        standDown("a hook installed without its trampoline");
        return;
    }
    g_pool.setEnv(env);
    g_phase.store(1, std::memory_order_release);
    Log::get().note("connection reuse: armed on build 334925: relays at %p and %p, beside the exe; each HTTP pump keeps one libcurl multi "
                    "handle, so later loading requests reuse its TLS connection; a handle idle more than 50 s is replaced; counts for 600 s",
                    static_cast<void*>(g_initRelay), static_cast<void*>(g_cleanupRelay));
    HANDLE thread = CreateThread(nullptr, 0, &reportThread, nullptr, 0, nullptr);
    if (thread) CloseHandle(thread);
    else Log::get().note("connection reuse: the counts thread did not start (error %lu); reuse is still on", GetLastError());
}

}  // namespace edvr
