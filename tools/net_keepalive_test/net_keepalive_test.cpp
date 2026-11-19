// The loading-connection reuse table (src/d3d11/net_keepalive_core.h).
// Fake init and cleanup: no game, no network. Proves a pump thread keeps one
// handle, a handle idle more than 50 s is replaced, a full table evicts only
// dead threads, a foreign caller still runs the real cleanup, and the real
// init and cleanup are not called under the table's lock.
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "../../src/d3d11/net_keepalive_core.h"

using namespace edvr;

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "pass" : "FAIL", what);
    ++g_checks;
    if (!ok) ++g_failures;
}

struct Fakes {
    std::uint64_t now = 1000;
    int inits = 0;
    int cleanups = 0;
    bool initNull = false;
    std::vector<void*> cleaned;
    std::vector<std::uint32_t> dead;
    std::uintptr_t next = 0x10000;
};
Fakes g_fake;

void* fInit() {
    ++g_fake.inits;
    if (g_fake.initNull) return nullptr;
    g_fake.next += 0x40;
    return reinterpret_cast<void*>(g_fake.next);
}
int fCleanup(void* multi) {
    ++g_fake.cleanups;
    g_fake.cleaned.push_back(multi);
    return 0x5A;
}
std::uint64_t fNow() { return g_fake.now; }
bool fAlive(std::uint32_t tid) {
    for (std::uint32_t d : g_fake.dead)
        if (d == tid) return false;
    return true;
}
KeepEnv fakeEnv() {
    KeepEnv env;
    env.init = &fInit;
    env.cleanup = &fCleanup;
    env.nowMs = &fNow;
    env.isAlive = &fAlive;
    return env;
}
bool cleanedContains(void* multi) {
    for (void* c : g_fake.cleaned)
        if (c == multi) return true;
    return false;
}

void oneThreadOneHandle() {
    std::printf("-- one thread, one handle\n");
    g_fake = Fakes();
    NetPool pool(fakeEnv());
    void* a = pool.acquire(100);
    check(a != nullptr && g_fake.inits == 1 && pool.holds(100, a) && pool.entries() == 1, "the first acquire makes one handle and keeps it");
    check(pool.release(100, a), "the pump's cleanup is kept");
    g_fake.now += 20000;
    void* b = pool.acquire(100);
    check(b == a && g_fake.inits == 1 && g_fake.cleanups == 0, "the next acquire returns the same handle");
    pool.release(100, b);
    bool same = true;
    for (int i = 0; i < 50; ++i) {
        g_fake.now += 100;
        void* c = pool.acquire(100);
        pool.release(100, c);
        same = same && c == a;
    }
    KeepCounts counts = pool.counts();
    check(same && g_fake.inits == 1 && counts.acquires == 52 && counts.hits == 51 && counts.kept == 52 && counts.refreshed == 0 && counts.pumps == 1,
          "fifty more transfers still use the one handle");
    check(pool.acquire(100) == a, "a hit again, left busy");
    const int cleanups = g_fake.cleanups;
    const int inits = g_fake.inits;
    void* fresh = pool.acquire(100);
    check(fresh != nullptr && fresh != a && g_fake.inits == inits + 1 && g_fake.cleanups == cleanups && !cleanedContains(a) && pool.holds(100, fresh) &&
              pool.counts().abandoned == 1,
          "an acquire with no release abandons the old handle and does not clean it up");
}

void distinctThreads() {
    std::printf("-- distinct threads\n");
    g_fake = Fakes();
    NetPool pool(fakeEnv());
    void* h[4];
    for (int i = 0; i < 4; ++i) h[i] = pool.acquire(200 + static_cast<std::uint32_t>(i));
    bool distinct = h[0] && h[1] && h[2] && h[3];
    for (int i = 0; i < 4; ++i)
        for (int k = i + 1; k < 4; ++k) distinct = distinct && h[i] != h[k];
    check(distinct && g_fake.inits == 4 && pool.entries() == 4, "four pump threads get four handles");
    bool stable = true;
    for (int i = 0; i < 4; ++i) {
        pool.release(200 + static_cast<std::uint32_t>(i), h[i]);
        stable = stable && pool.acquire(200 + static_cast<std::uint32_t>(i)) == h[i];
    }
    check(stable && g_fake.inits == 4, "each thread keeps its own");
}

void nullInitNotCached() {
    std::printf("-- a failed init is not cached\n");
    g_fake = Fakes();
    NetPool pool(fakeEnv());
    g_fake.initNull = true;
    check(pool.acquire(300) == nullptr && g_fake.inits == 1 && pool.entries() == 0, "a null init is returned and no entry is kept");
    check(!pool.release(300, nullptr), "release of null is never kept");
    g_fake.initNull = false;
    void* m = pool.acquire(300);
    check(m != nullptr && g_fake.inits == 2 && pool.entries() == 1, "the next acquire tries init again");
    pool.release(300, m);
    g_fake.now += kKeepIdleRefreshMs + 1;
    g_fake.initNull = true;
    check(pool.acquire(300) == nullptr && cleanedContains(m) && pool.entries() == 0, "a refresh whose init fails drops the entry after cleaning up the old handle");
}

void idleRefresh() {
    std::printf("-- idle for more than 50 s replaces the handle\n");
    g_fake = Fakes();
    NetPool pool(fakeEnv());
    void* a = pool.acquire(400);
    pool.release(400, a);
    g_fake.now += kKeepIdleRefreshMs;
    check(pool.acquire(400) == a && g_fake.cleanups == 0 && g_fake.inits == 1, "idle exactly 50 s keeps the handle");
    pool.release(400, a);
    g_fake.now += kKeepIdleRefreshMs + 1;
    void* b = pool.acquire(400);
    check(b != nullptr && b != a && g_fake.cleanups == 1 && g_fake.cleaned.back() == a && g_fake.inits == 2 && pool.holds(400, b) && pool.counts().refreshed == 1,
          "idle 50.001 s cleans up the old handle once and makes a new one");
    check(!pool.release(400, a) && pool.release(400, b), "only the new handle is ours");
    pool.release(400, b);
    g_fake.now -= 5000;
    check(pool.acquire(400) == b, "a clock that reads earlier than the release is not a long idle");
}

void fill(NetPool& pool, std::uint32_t first, void** handles) {
    for (std::size_t i = 0; i < kKeepSlots; ++i) {
        handles[i] = pool.acquire(first + static_cast<std::uint32_t>(i));
        pool.release(first + static_cast<std::uint32_t>(i), handles[i]);
    }
}

void fullTableEvictsDead() {
    std::printf("-- a full table evicts dead threads only\n");
    g_fake = Fakes();
    NetPool pool(fakeEnv());
    void* h[kKeepSlots];
    fill(pool, 500, h);
    g_fake.dead = {501, 504, 507};
    void* fresh = pool.acquire(900);
    check(pool.holds(900, fresh) && g_fake.cleanups == 3 && cleanedContains(h[1]) && cleanedContains(h[4]) && cleanedContains(h[7]),
          "three dead threads are cleaned up and the new thread gets a slot");
    bool live = true;
    for (int i : {0, 2, 3, 5, 6}) live = live && pool.holds(500 + static_cast<std::uint32_t>(i), h[i]) && !cleanedContains(h[i]);
    check(live && pool.entries() == 6 && pool.counts().evictions == 3 && pool.counts().passthrough == 0, "the five live entries stay");
}

void fullTablePassesThrough() {
    std::printf("-- a full table of live threads does not cache\n");
    g_fake = Fakes();
    NetPool pool(fakeEnv());
    void* h[kKeepSlots];
    fill(pool, 600, h);
    const int inits = g_fake.inits;
    void* ninth = pool.acquire(990);
    check(ninth != nullptr && g_fake.inits == inits + 1 && g_fake.cleanups == 0 && !pool.holds(990, ninth) && pool.counts().passthrough == 1,
          "the ninth live thread gets the real handle, uncached");
    check(!pool.release(990, ninth), "its cleanup is not kept, so the caller runs the real one");
    bool intact = true;
    for (std::size_t i = 0; i < kKeepSlots; ++i) intact = intact && pool.holds(600 + static_cast<std::uint32_t>(i), h[i]);
    check(intact, "the eight entries are unchanged");
}

void releaseRefusesOthers() {
    std::printf("-- release keeps only the owner's handle\n");
    g_fake = Fakes();
    NetPool pool(fakeEnv());
    void* a = pool.acquire(800);
    void* b = pool.acquire(801);
    check(!pool.release(801, a) && !pool.release(800, b) && !pool.release(802, a) && !pool.release(800, reinterpret_cast<void*>(0x1234)) && !pool.release(800, nullptr),
          "another thread, an unknown handle and null are not kept");
    check(pool.holds(800, a) && pool.holds(801, b) && pool.release(800, a) && pool.release(801, b), "the owners still can");
}

void locksNotHeld() {
    std::printf("-- init and cleanup run outside the lock\n");
    struct Probe {
        static NetPool*& pool() { static NetPool* p = nullptr; return p; }
        static std::atomic<int>& blocked() { static std::atomic<int> n{0}; return n; }
        static void probe() {
            auto done = std::make_shared<std::atomic<bool>>(false);
            NetPool* p = pool();
            std::thread t([done, p] {
                p->entries();
                done->store(true);
            });
            for (int i = 0; i < 400 && !done->load(); ++i) Sleep(5);
            if (!done->load()) {
                blocked().fetch_add(1);
                t.detach();
            } else {
                t.join();
            }
        }
        static void* init() {
            probe();
            ++g_fake.inits;
            g_fake.next += 0x40;
            return reinterpret_cast<void*>(g_fake.next);
        }
        static int cleanup(void* multi) {
            probe();
            ++g_fake.cleanups;
            g_fake.cleaned.push_back(multi);
            return 0;
        }
    };
    g_fake = Fakes();
    KeepEnv env = fakeEnv();
    env.init = &Probe::init;
    env.cleanup = &Probe::cleanup;
    NetPool pool(env);
    Probe::pool() = &pool;
    Probe::blocked().store(0);
    void* a = pool.acquire(1000);
    pool.release(1000, a);
    g_fake.now += kKeepIdleRefreshMs + 1;
    pool.release(1000, pool.acquire(1000));
    for (std::size_t i = 0; i + 1 < kKeepSlots; ++i)
        pool.release(1100 + static_cast<std::uint32_t>(i), pool.acquire(1100 + static_cast<std::uint32_t>(i)));
    g_fake.dead = {1100};
    pool.acquire(1200);
    check(Probe::blocked().load() == 0 && g_fake.cleanups >= 2, "another thread enters the table during init and cleanup");
}

void fourThreads() {
    std::printf("-- four real threads\n");
    struct Mt {
        static std::atomic<std::uintptr_t>& next() { static std::atomic<std::uintptr_t> n{0x80000}; return n; }
        static std::atomic<int>& inits() { static std::atomic<int> n{0}; return n; }
        static void* init() { inits().fetch_add(1); return reinterpret_cast<void*>(next().fetch_add(0x40)); }
        static int cleanup(void*) { return 0; }
        static std::uint64_t now() { return 5000; }
        static bool alive(std::uint32_t) { return true; }
    };
    KeepEnv env;
    env.init = &Mt::init;
    env.cleanup = &Mt::cleanup;
    env.nowMs = &Mt::now;
    env.isAlive = &Mt::alive;
    NetPool pool(env);
    std::atomic<int> wrong{0};
    std::vector<std::thread> threads;
    for (std::uint32_t k = 0; k < 4; ++k) {
        threads.emplace_back([&, k] {
            void* mine = nullptr;
            for (int i = 0; i < 4000; ++i) {
                void* m = pool.acquire(2000 + k);
                if (mine == nullptr) mine = m;
                if (m != mine || !pool.release(2000 + k, m)) wrong.fetch_add(1);
            }
        });
    }
    for (std::thread& t : threads) t.join();
    check(wrong.load() == 0 && Mt::inits().load() == 4 && pool.counts().acquires == 16000 && pool.counts().hits == 16000 - 4,
          "four threads keep four handles across 16000 transfers");
}

constexpr std::uint32_t kRetInit = 0x5C41A9u;
constexpr std::uint32_t kRetCleanup = 0x5C44BCu;

struct Orig {
    int inits = 0;
    int cleanups = 0;
    void* last = nullptr;
};
Orig g_orig;
void* oInit() {
    ++g_orig.inits;
    return reinterpret_cast<void*>(0xAAAA0);
}
int oCleanup(void* multi) {
    ++g_orig.cleanups;
    g_orig.last = multi;
    return 0x33;
}
std::uint32_t oTid() { return 4242; }

void gateAndWrappers() {
    std::printf("-- the return-address gate\n");
    check(keepGate(kRetInit, kRetInit) == KeepGate::Act && keepGate(kRetCleanup, kRetCleanup) == KeepGate::Act, "the pump's two return addresses act");
    check(keepGate(kRetInit + 1, kRetInit) == KeepGate::Pass && keepGate(kRetCleanup, kRetInit) == KeepGate::Pass && keepGate(0, kRetInit) == KeepGate::Pass &&
              keepGate(~std::uint64_t{0}, kRetCleanup) == KeepGate::Pass,
          "any other return address passes through");
    int armed = 0;
    for (int i = 0; i < 16; ++i) {
        const bool on = keepArming(i & 1, i & 2, i & 4, i & 8);
        if (on) ++armed;
        if (i != 15) check(!on, "arming needs both sites and both pins");
    }
    check(armed == 1 && keepArming(true, true, true, true), "only all four together arm");

    g_fake = Fakes();
    g_orig = Orig();
    NetPool pool(fakeEnv());
    KeepFns env;
    env.init = &oInit;
    env.cleanup = &oCleanup;
    env.threadId = &oTid;
    env.pool = &pool;
    env.initReturnRva = kRetInit;
    env.cleanupReturnRva = kRetCleanup;
    void* kept = keepInit(env, kRetInit);
    check(kept != nullptr && g_orig.inits == 0 && g_fake.inits == 1 && pool.holds(4242, kept), "the pump's init takes the table's handle, not a new original call");
    check(keepCleanup(env, kRetCleanup, kept) == 0 && g_orig.cleanups == 0 && pool.holds(4242, kept), "the pump's cleanup of that handle returns 0 and does not free it");
    void* again = keepInit(env, kRetInit);
    check(again == kept && g_fake.inits == 1, "the next pump init reuses it");
    void* stray = oInit();
    check(keepCleanup(env, kRetCleanup, stray) == 0x33 && g_orig.last == stray, "the pump's cleanup of an uncached handle runs the real cleanup");
    const int foreignBefore = static_cast<int>(pool.counts().foreignCleanups);
    check(keepCleanup(env, 0x48DF33Bu, kept) == 0x33 && g_orig.last == kept && pool.counts().foreignCleanups == static_cast<std::uint64_t>(foreignBefore + 1) && !pool.holds(4242, kept),
          "a caller that is not the pump frees the handle and the table forgets it");
    check(keepInit(env, 0x1111u) == reinterpret_cast<void*>(0xAAAA0) && g_orig.inits == 2, "a caller that is not the pump gets the original init");
}

void reportLines() {
    std::printf("-- the counts line\n");
    KeepCounts c{};
    c.pumps = 4;
    c.acquires = 10;
    c.hits = 6;
    char line[400];
    formatKeepCounts(c, line, sizeof(line));
    check(std::strstr(line, "connection reuse: pumps=4 acquires=10 hits=6 refreshed=0 kept=0 passthrough=0 foreign_cleanups=0 evictions=0 abandoned=0") == line,
          "the counts line names every counter");
    formatKeepFinal(c, 600, line, sizeof(line));
    check(std::strstr(line, "connection reuse: final after 600 s: pumps=4 acquires=10 hits=6") == line && std::strstr(line, "no further report this session") != nullptr,
          "the final line is the counts plus the stop");
    check(!keepReportTick(0, kKeepReportDeadlineMs, 0, 0).emit && !keepReportTick(0, kKeepReportDeadlineMs, 0, 0).done, "nothing to say before the deadline");
    KeepReportTick changed = keepReportTick(30000, kKeepReportDeadlineMs, 3, 0);
    check(changed.emit && !changed.done, "a changed count is a line and the thread stays");
    KeepReportTick done = keepReportTick(kKeepReportDeadlineMs, kKeepReportDeadlineMs, 3, 3);
    check(done.emit && done.done, "the deadline writes one last line even when nothing changed");
    check(keepActivity(c) == 16, "activity is the counters that only grow, not the snapshot of pumps");
}

void relayReach() {
    std::printf("-- the relay beside the exe\n");
    // The address CodeHook reported on the 2026-11-19 flight, and a landing in another module far below it.
    constexpr std::uintptr_t kFlightInit = 0x7FF6DB629930ull;
    constexpr std::uintptr_t kFarDll = 0x7FF200001000ull;
    check(!keepFiveByteReaches(kFlightInit, kFarDll), "a five-byte jump from curl_multi_init cannot reach the DLL");
    check(keepFiveByteReaches(kFlightInit, kFlightInit - 0x10000u), "the same site can reach a relay one allocation below it");
    std::uint8_t stub[kKeepRelayBytes] = {};
    const void* wrap = reinterpret_cast<const void*>(kFarDll);
    writeKeepRelay(stub, wrap);
    check(keepRelayIsTailJump(stub, wrap), "the relay is an absolute tail jump to the wrapper");
    stub[0] = 0xE8;
    check(!keepRelayIsTailJump(stub, wrap), "a call is not the relay: it would hide the pump's return address");
}

}  // namespace

int main() {
    oneThreadOneHandle();
    distinctThreads();
    nullInitNotCached();
    idleRefresh();
    fullTableEvictsDead();
    fullTablePassesThrough();
    releaseRefusesOthers();
    locksNotHeld();
    fourThreads();
    gateAndWrappers();
    reportLines();
    relayReach();
    std::printf("net_keepalive_test: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
