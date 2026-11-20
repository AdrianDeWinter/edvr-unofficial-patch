// The loading-connection reuse decision, with no Windows calls and no detours.
//
// Elite's HTTP pump threads (four of them) create a libcurl multi handle, do
// one transfer, and destroy the handle. The multi handle owns the connection
// cache, so that pattern opens a new TCP connection and runs a full TLS
// handshake for every file of a load. Keeping one multi handle per pump thread
// leaves the cache in place: the same requests go out in the same order, and
// a later request on that thread reuses the connection. On build 334925 an
// early Continue dropped from about 64 s to about 10 s once this was in place.
//
// A handle that has been idle for more than 50 s is replaced. libcurl only
// notices a connection the server has already closed; one a NAT dropped
// silently would otherwise sit until the game's own 30 s timeout.
//
// The table is fixed at 8 entries. Real init, cleanup, the clock and the
// "is this thread still alive" check are injected, and none of them runs while
// the table's lock is held. There is no cleanup at process exit: the pumps
// live for the process, and freeing a handle while one of them is inside a
// transfer would be a use after free.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <cstdio>

namespace edvr {

constexpr std::size_t kKeepSlots = 8;
constexpr std::uint64_t kKeepIdleRefreshMs = 50000;
constexpr std::uint64_t kKeepReportPeriodMs = 30000;
constexpr std::uint64_t kKeepReportDeadlineMs = 600000;

enum class KeepGate { Pass, Act };

// Act only when the return address is the pump's own call. Every other caller,
// including libcurl cleaning up one of its own handles, goes straight to the
// original. A return address below the image wraps to a huge value and matches
// nothing.
inline KeepGate keepGate(std::uint64_t returnRva, std::uint32_t expected) {
    return returnRva == expected ? KeepGate::Act : KeepGate::Pass;
}

// Both sites and both call-site pins, or nothing. Hooking init alone would let
// the real cleanup free a handle the table still hands out.
inline bool keepArming(bool initSite, bool cleanupSite, bool initCallPin, bool cleanupCallPin) {
    return initSite && cleanupSite && initCallPin && cleanupCallPin;
}

// A five-byte E9 at `site` reaches `landing` only when the displacement from
// the instruction's end fits in a signed 32-bit immediate. EliteDangerous64.exe
// and this DLL are routinely farther apart than that.
inline bool keepFiveByteReaches(std::uintptr_t site, std::uintptr_t landing) {
    const std::int64_t displacement = static_cast<std::int64_t>(landing) - (static_cast<std::int64_t>(site) + 5);
    return displacement <= 0x7FFFFFFFLL && displacement >= -0x80000000LL;
}

// Fourteen bytes: `jmp qword ptr [rip+0]` and the absolute address. A tail jump
// pushes nothing and touches no register, so the wrapper's return address is
// still the pump's call and curl's argument registers arrive unchanged. A call
// would name the relay as the caller, and the pump check would never act.
constexpr std::size_t kKeepRelayBytes = 14;
inline void writeKeepRelay(std::uint8_t* code, const void* replacement) {
    code[0] = 0xFF;
    code[1] = 0x25;
    code[2] = code[3] = code[4] = code[5] = 0;
    const auto address = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(replacement));
    std::memcpy(code + 6, &address, sizeof(address));
}
inline bool keepRelayIsTailJump(const std::uint8_t* code, const void* replacement) {
    if (code[0] != 0xFF || code[1] != 0x25) return false;
    if (code[2] != 0 || code[3] != 0 || code[4] != 0 || code[5] != 0) return false;
    std::uint64_t address = 0;
    std::memcpy(&address, code + 6, sizeof(address));
    return address == static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(replacement));
}

struct KeepEnv {
    void* (*init)() = nullptr;
    int (*cleanup)(void* multi) = nullptr;
    std::uint64_t (*nowMs)() = nullptr;
    bool (*isAlive)(std::uint32_t tid) = nullptr;
};

struct KeepCounts {
    std::uint64_t pumps;  // entries in the table now
    std::uint64_t acquires, hits, refreshed, kept, passthrough, foreignCleanups, evictions, abandoned;
};

inline std::uint64_t keepActivity(const KeepCounts& c) {
    return c.acquires + c.hits + c.refreshed + c.kept + c.passthrough + c.foreignCleanups + c.evictions + c.abandoned;
}

// pumps= is a snapshot, so it is not part of the activity sum: a line is due
// when a counter grew, and the final line is due at the deadline either way.
struct KeepReportTick { bool emit; bool done; };
inline KeepReportTick keepReportTick(std::uint64_t elapsedMs, std::uint64_t deadlineMs, std::uint64_t activity, std::uint64_t lastActivity) {
    if (elapsedMs >= deadlineMs) return {true, true};
    if (activity != lastActivity) return {true, false};
    return {false, false};
}

inline void formatKeepCounts(const KeepCounts& c, char* out, std::size_t capacity) {
    std::snprintf(out, capacity,
                  "connection reuse: pumps=%llu acquires=%llu hits=%llu refreshed=%llu kept=%llu passthrough=%llu foreign_cleanups=%llu evictions=%llu abandoned=%llu",
                  static_cast<unsigned long long>(c.pumps), static_cast<unsigned long long>(c.acquires), static_cast<unsigned long long>(c.hits),
                  static_cast<unsigned long long>(c.refreshed), static_cast<unsigned long long>(c.kept), static_cast<unsigned long long>(c.passthrough),
                  static_cast<unsigned long long>(c.foreignCleanups), static_cast<unsigned long long>(c.evictions), static_cast<unsigned long long>(c.abandoned));
}

inline void formatKeepFinal(const KeepCounts& c, unsigned seconds, char* out, std::size_t capacity) {
    char counts[320];
    formatKeepCounts(c, counts, sizeof(counts));
    const char prefix[] = "connection reuse: ";
    const char* body = counts;
    if (std::strncmp(counts, prefix, sizeof(prefix) - 1) == 0) body = counts + (sizeof(prefix) - 1);
    std::snprintf(out, capacity, "connection reuse: final after %u s: %s; no further report this session", seconds, body);
}

struct KeepFns {
    void* (*init)() = nullptr;
    int (*cleanup)(void* multi) = nullptr;
    std::uint32_t (*threadId)() = nullptr;
    class NetPool* pool = nullptr;
    std::uint32_t initReturnRva = 0;
    std::uint32_t cleanupReturnRva = 0;
};

class NetPool {
public:
    NetPool() = default;
    explicit NetPool(const KeepEnv& env) : env_(env) {}
    void setEnv(const KeepEnv& env) { env_ = env; }

    // The multi handle for tid's next transfer. A handle idle for more than
    // 50 s is cleaned up and replaced. A full table drops entries whose threads
    // are gone and retries once; still full, the real init's result is returned
    // and not cached. A null from init is not cached.
    void* acquire(std::uint32_t tid);
    // True when multi is the handle this table holds for tid: the caller must
    // not run the real cleanup, and returns 0 (CURLM_OK). False: not ours.
    bool release(std::uint32_t tid, void* multi);
    // A cleanup that did not come from the pump. If the table holds multi, the
    // entry is dropped before the caller runs the real cleanup on it.
    void noteForeignCleanup(void* multi);

    KeepCounts counts();
    bool holds(std::uint32_t tid, void* multi);
    std::size_t entries();

private:
    struct Entry {
        bool used;
        std::uint32_t tid;
        void* multi;
        std::uint64_t releasedMs;
        bool busy;
    };
    int findLocked(std::uint32_t tid) const;
    int freeLocked() const;
    int reserveLocked(std::uint32_t tid, std::uint64_t now, void** hit, void** old);
    std::size_t evictDead();

    KeepEnv env_{};
    struct Counters {
        std::atomic<std::uint64_t> acquires{0}, hits{0}, refreshed{0}, kept{0}, passthrough{0}, foreignCleanups{0}, evictions{0}, abandoned{0};
    } counters_{};
    std::mutex mutex_;
    Entry entries_[kKeepSlots] = {};
};

inline void* keepInit(const KeepFns& env, std::uint64_t returnRva) {
    if (keepGate(returnRva, env.initReturnRva) == KeepGate::Pass) return env.init();
    return env.pool->acquire(env.threadId());
}

inline int keepCleanup(const KeepFns& env, std::uint64_t returnRva, void* multi) {
    if (keepGate(returnRva, env.cleanupReturnRva) == KeepGate::Pass) {
        env.pool->noteForeignCleanup(multi);
        return env.cleanup(multi);
    }
    if (env.pool->release(env.threadId(), multi)) return 0;
    return env.cleanup(multi);
}

inline int NetPool::findLocked(std::uint32_t tid) const {
    for (std::size_t i = 0; i < kKeepSlots; ++i)
        if (entries_[i].used && entries_[i].tid == tid) return static_cast<int>(i);
    return -1;
}

inline int NetPool::freeLocked() const {
    for (std::size_t i = 0; i < kKeepSlots; ++i)
        if (!entries_[i].used) return static_cast<int>(i);
    return -1;
}

inline std::size_t NetPool::evictDead() {
    std::uint32_t tids[kKeepSlots];
    void* handles[kKeepSlots];
    bool seen[kKeepSlots] = {};
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (std::size_t i = 0; i < kKeepSlots; ++i) {
            seen[i] = entries_[i].used && !entries_[i].busy;
            tids[i] = entries_[i].tid;
            handles[i] = entries_[i].multi;
        }
    }
    bool dead[kKeepSlots] = {};
    for (std::size_t i = 0; i < kKeepSlots; ++i)
        if (seen[i]) dead[i] = env_.isAlive == nullptr || !env_.isAlive(tids[i]);
    void* toClean[kKeepSlots];
    std::size_t clean = 0, evicted = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (std::size_t i = 0; i < kKeepSlots; ++i) {
            if (!dead[i] || !entries_[i].used || entries_[i].busy || entries_[i].tid != tids[i] || entries_[i].multi != handles[i]) continue;
            if (entries_[i].multi != nullptr) toClean[clean++] = entries_[i].multi;
            entries_[i] = Entry{};
            ++evicted;
        }
    }
    for (std::size_t i = 0; i < clean; ++i) env_.cleanup(toClean[i]);
    if (evicted != 0) counters_.evictions.fetch_add(evicted, std::memory_order_relaxed);
    return evicted;
}

inline int NetPool::reserveLocked(std::uint32_t tid, std::uint64_t now, void** hit, void** old) {
    *hit = nullptr;
    *old = nullptr;
    const int own = findLocked(tid);
    if (own >= 0) {
        Entry& e = entries_[own];
        const bool idleTooLong = now > e.releasedMs && now - e.releasedMs > kKeepIdleRefreshMs;
        if (e.busy) {
            // An Acquire whose previous Acquire was never Released. The pump
            // always releases, so this is a thread id that was reused while its
            // old handle might still be in use. The old handle is abandoned:
            // leaked, never cleaned up.
            e.multi = nullptr;
            counters_.abandoned.fetch_add(1, std::memory_order_relaxed);
            return own;
        }
        e.busy = true;
        if (e.multi != nullptr && !idleTooLong) {
            *hit = e.multi;
            return own;
        }
        *old = e.multi;
        e.multi = nullptr;
        return own;
    }
    const int free = freeLocked();
    if (free >= 0) entries_[free] = Entry{true, tid, nullptr, now, true};
    return free;
}

inline void* NetPool::acquire(std::uint32_t tid) {
    counters_.acquires.fetch_add(1, std::memory_order_relaxed);
    const std::uint64_t now = env_.nowMs();
    void* hit = nullptr;
    void* old = nullptr;
    int slot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        slot = reserveLocked(tid, now, &hit, &old);
    }
    if (hit != nullptr) {
        counters_.hits.fetch_add(1, std::memory_order_relaxed);
        return hit;
    }
    if (slot < 0) {
        evictDead();
        std::lock_guard<std::mutex> lock(mutex_);
        slot = reserveLocked(tid, now, &hit, &old);
    }
    if (slot < 0) {
        counters_.passthrough.fetch_add(1, std::memory_order_relaxed);
        return env_.init();
    }
    if (old != nullptr) {
        env_.cleanup(old);
        counters_.refreshed.fetch_add(1, std::memory_order_relaxed);
    }
    void* made = env_.init();
    std::lock_guard<std::mutex> lock(mutex_);
    Entry& e = entries_[slot];
    if (made == nullptr) {
        e = Entry{};
    } else {
        e.multi = made;
        e.releasedMs = now;
    }
    return made;
}

inline bool NetPool::release(std::uint32_t tid, void* multi) {
    if (multi == nullptr) return false;
    const std::uint64_t now = env_.nowMs();
    std::lock_guard<std::mutex> lock(mutex_);
    const int own = findLocked(tid);
    if (own < 0 || entries_[own].multi != multi) return false;
    entries_[own].releasedMs = now;
    entries_[own].busy = false;
    counters_.kept.fetch_add(1, std::memory_order_relaxed);
    return true;
}

inline void NetPool::noteForeignCleanup(void* multi) {
    counters_.foreignCleanups.fetch_add(1, std::memory_order_relaxed);
    if (multi == nullptr) return;
    std::lock_guard<std::mutex> lock(mutex_);
    for (std::size_t i = 0; i < kKeepSlots; ++i)
        if (entries_[i].used && entries_[i].multi == multi) entries_[i] = Entry{};
}

inline KeepCounts NetPool::counts() {
    KeepCounts c{};
    c.acquires = counters_.acquires.load(std::memory_order_relaxed);
    c.hits = counters_.hits.load(std::memory_order_relaxed);
    c.refreshed = counters_.refreshed.load(std::memory_order_relaxed);
    c.kept = counters_.kept.load(std::memory_order_relaxed);
    c.passthrough = counters_.passthrough.load(std::memory_order_relaxed);
    c.foreignCleanups = counters_.foreignCleanups.load(std::memory_order_relaxed);
    c.evictions = counters_.evictions.load(std::memory_order_relaxed);
    c.abandoned = counters_.abandoned.load(std::memory_order_relaxed);
    c.pumps = entries();
    return c;
}

inline bool NetPool::holds(std::uint32_t tid, void* multi) {
    std::lock_guard<std::mutex> lock(mutex_);
    const int own = findLocked(tid);
    return own >= 0 && entries_[own].multi == multi && multi != nullptr;
}

inline std::size_t NetPool::entries() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::size_t n = 0;
    for (std::size_t i = 0; i < kKeepSlots; ++i)
        if (entries_[i].used) ++n;
    return n;
}

}  // namespace edvr
