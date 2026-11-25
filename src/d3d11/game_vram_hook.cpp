// Elite sizes its texture and mesh streaming from the card's DedicatedVideoMemory
// x 0.85, read through IDXGIAdapter::GetDesc, and never asks Windows for the
// per-process budget. EDVR's own share sits on top of that, so the streaming
// budget can outgrow what Windows allows (docs/memory-growth-2026-10-22.md).
// This hook answers only the game's own GetDesc calls with the size that
// advanced.game_vram asks for.
//
// The vtable patch is shared by every adapter object in the process: the
// upscalers (NGX, XeSS, FSR), the driver and EDVR itself must keep seeing the
// real size, so the hook acts only when its return address is inside the game
// executable's image.
//
// advanced.game_vram = auto learns two figures per setup (game_vram_learn.h):
// the reserve, EDVR's, the anti-aliasing engine's and the driver's share, and
// the flight budget, what the OS lowers the process's budget to in flight.
// Neither is the same on every machine, so each setup's are kept in
// %LOCALAPPDATA%\EDUP\game_vram.txt, keyed by vendor, device, driver, layer,
// profile and AA engine. Only DXGI's own figures are read to learn them; no
// game address is used. Learning stops when the AA engine or the profile
// changes in a session, because Elite sized its budget once, for the engine
// and the profile it started in.
//
// Nothing is installed in a profile that forwards only (passthrough, or an
// invalid install), nor in a process that is not the game.
//
// The key follows the adapter the device reports; a different GPU's samples
// are not written under the adapter install saw.

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_4.h>
#include <intrin.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#include "game_vram_hook.h"
#include "../common/adapter_memory.h"
#include "../common/game_vram.h"
#include "../common/game_vram_learn.h"
#include "../common/config.h"
#include "../common/game_build.h"
#include "../common/iniedit.h"
#include "../common/log.h"
#include "../common/runtime_profile.h"
#include "../common/vram_query.h"
#include "../common/vtable_hook.h"

namespace edvr {
namespace {

using GetDescFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIAdapter*, DXGI_ADAPTER_DESC*);

// The original GetDesc entry, written by VTableHook::replace().
void* g_realGetDesc = nullptr;
gamevram::Setting g_setting;
std::atomic<uint32_t> g_calls{0};
// The reserve EDVR leaves free this session: the learned one, or the engine's start. Fixed for the session.
uint32_t g_reserveMb = 0;
// The flight budget this session started from; 0 = none. Fixed for the session, and 0 when the budget is not one to learn from.
uint32_t g_flightMb = 0;
// What other processes held of the card when auto was prepared (game_vram.h, startBudgetMb). Fixed for the session.
bool g_othersKnown = false;
uint64_t g_othersBytes = 0;
// The start budget, in bytes, behind the game's first answer about the adapter auto is keyed to: what learning measures a
// fall against. 0 until that call. Written by the game's thread, read by the render thread.
std::atomic<uint64_t> g_startBudgetBytes{0};
// Under Wine, the reported budget was the whole card at install (gamevram::budgetIsWholeCard): auto ignores it for the
// session, sizes from 95% of the card with the engine's start, and learns nothing. Set once, before the hook exists.
bool g_budgetIgnored = false;
// What other programs hold, read on a pool thread from the first prepareAuto on and kept for the session: a device on
// another card takes its row from the same reading. Null under Wine, or when the pool refused the work.
AdapterUsageRead* g_othersRead = nullptr;
bool g_othersStarted = false;
// finishAuto has run for the adapter prepareAuto keyed. Until then the hook waits for it; prepareAuto clears it.
std::atomic<bool> g_autoReady{false};
SRWLOCK g_readyLock = SRWLOCK_INIT;

constexpr uint32_t kLoggedCalls = 32;
constexpr uint32_t kLearnEverySamples = 60;   // one learning pass per minute of samples, and one at the session's first spill
constexpr DWORD kOthersWaitMs = 10000;        // what other programs hold is waited for until 10 s after the read began; it took 1.3-4.4 s
constexpr size_t kMaxStoreBytes = 1u << 20;   // the store is a few hundred bytes; anything bigger is not ours

// What auto learns this session, and what it has already written.
struct AutoLearn {
    bool active = false;           // installed in auto with a store to write
    bool stopped = false;          // the AA mode or the profile changed: nothing more is learned this session
    bool haveGpu = false;          // install read a real adapter desc; the device is checked against it
    bool deviceNoted = false;      // gameVramNoteDevice has run; samples wait so they are not filed under the wrong key
    uint32_t vendorId = 0;
    uint32_t deviceId = 0;
    LUID luid{};                   // the card itself: two of the same kind share the ids, and what other programs hold differs
    uint64_t realBytes = 0;        // its DedicatedVideoMemory
    bool wine = false;             // under Wine: there is no reading of what other programs hold
    bool adopt = false;            // the store was read: finishAuto takes this setup's record from it
    std::vector<gamevram::Record> store;   // ...as read by prepareAuto
    std::string key;
    gamevram::AaSlot aaAtInstall = gamevram::AaSlot::Off;   // the engine when the budget was sized
    RuntimeProfile profileAtInstall = RuntimeProfile::Invalid;   // the profile the key was made for
    const char* profileNameAtInstall = "";                       // its runtimeProfileName() (a static string)
    gamevram::ReserveBounds bounds{};                            // the limits for that profile and engine
    gamevram::StartFacts facts;                                  // the budget when the key was made, for an old record
    std::wstring storePath;        // empty when LOCALAPPDATA is unset or the store is unreadable: nothing is learned or saved
    uint32_t lastPassSamples = 0;
    bool spillPassed = false;      // a pass has run for a spill
    bool haveHanded = false;       // a result was handed to the writer
    gamevram::LearnResult handed;  // ...the last one
};
AutoLearn g_learn;

// SAVING OFF THE RENDER THREAD. A save reads the store, merges this session's record and replaces the file atomically,
// with a flush: usually a millisecond, but tens on a slow disk, under a virus scanner or through Wine's file layer, and
// the first spill over the budget comes when the frame is already struggling. So the render thread only decides what to
// save and hands it to a thread-pool callback. One slot: a newer result replaces one not yet written, and one callback
// at a time drains it, so two saves never read and write the store at once. If the pool will not take the work, the
// save runs inline, as it did before. key and storePath are fixed before the first save (at install, or when the device's
// adapter re-keys auto, which a sample waits for), so the callback reads g_learn safely.
struct SaveJob {
    gamevram::LearnResult learned;
    uint32_t startedReserveMb = 0; // the reserve and flight budget this session started from (mergeLearned)
    uint32_t startedFlightMb = 0;
    std::string line;              // the learning line, formatted on the render thread, logged once the save landed
};
SRWLOCK g_saveLock = SRWLOCK_INIT;
SaveJob* g_savePending = nullptr;  // under g_saveLock
bool g_saveBusy = false;           // under g_saveLock: a drain is queued or running
std::atomic<bool> g_saveFailureLogged{false};
// What this session's saves merge onto. Used by runSave only, which never runs twice at once.
gamevram::SaveBasis g_saveBasis;
// Set by a save that did not land, read by the render thread: the next pass writes again even if its result is the
// one already handed over, so a transient failure (a scanner holding the file past the retries) is not final.
std::atomic<bool> g_saveFailed{false};

// Base and size of the game executable's image, read once.
struct ExeImage {
    uintptr_t base = 0;
    uintptr_t size = 0;   // 0 when the header is unreadable: no address is then in the exe
};

const ExeImage& exeImage() {
    static const ExeImage image = [] {
        ExeImage e;
        uint32_t size = 0;
        if (gameBuildIdentity(nullptr, &size)) {
            e.base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
            e.size = size;
        }
        return e;
    }();
    return image;
}

// The adapter's LOCAL segment for this process: valid is false, and the figures 0, on any failure.
VramSegment localSegment(IDXGIAdapter* self) {
    VramSegment segment;
    IDXGIAdapter3* a3 = nullptr;
    if (SUCCEEDED(self->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&a3))) && a3) {
        segment = vramReadSegment(a3, DXGI_MEMORY_SEGMENT_GROUP_LOCAL);
        a3->Release();
    }
    return segment;
}

// 0 on any failure, which compute() reads as unknown.
uint64_t osLocalBudget(IDXGIAdapter* self) { return localSegment(self).budget; }

void ensureAutoReady();

HRESULT STDMETHODCALLTYPE hookedGetDesc(IDXGIAdapter* self, DXGI_ADAPTER_DESC* desc) {
    // Taken first, before anything else can move the stack.
    const uintptr_t caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
    const HRESULT hr = reinterpret_cast<GetDescFn>(g_realGetDesc)(self, desc);
    const ExeImage& exe = exeImage();
    const uintptr_t callerRva = caller - exe.base;
    if (FAILED(hr) || !desc || callerRva >= exe.size) return hr;
    if (g_setting.mode == gamevram::Mode::Auto) ensureAutoReady();

    gamevram::Inputs in;
    in.realDedicatedBytes = static_cast<uint64_t>(desc->DedicatedVideoMemory);
    in.osBudgetBytes = (g_setting.mode == gamevram::Mode::Auto && !g_budgetIgnored) ? osLocalBudget(self) : 0;
    in.reserveMb = g_reserveMb;
    // Elite also asks about other adapters. What other programs hold, and what the budget falls to, are the keyed one's.
    const bool keyed = gamevram::sameAdapterIdentity(g_learn.vendorId, g_learn.deviceId, desc->VendorId, desc->DeviceId);
    if (keyed) {
        in.othersKnown = g_othersKnown;
        in.othersBytes = g_othersBytes;
        in.flightBudgetMb = g_flightMb;
    }

    const gamevram::Result r = gamevram::compute(g_setting, in);
    if (keyed && r.startBytes != 0) {   // the first answer about it
        uint64_t none = 0;
        g_startBudgetBytes.compare_exchange_strong(none, r.startBytes, std::memory_order_acq_rel);
    }
    if (r.change) {
        desc->DedicatedVideoMemory = static_cast<SIZE_T>(r.reportedBytes);
        // Below 4 GB Elite does not use dedicated memory alone (build 334925): under 1 GB the larger of
        // dedicated and shared, and from 1 to 4 GB the average when shared is larger. Shared has to name
        // the same size or a discrete GPU's shared heap hides the smaller card. Dedicated system memory
        // and the adapter LUID stay as the driver reported them.
        const uint64_t shared = gamevram::sharedForReported(r.reportedBytes,
                                                            static_cast<uint64_t>(desc->SharedSystemMemory));
        if (shared != static_cast<uint64_t>(desc->SharedSystemMemory))
            desc->SharedSystemMemory = static_cast<SIZE_T>(shared);
    }

    const uint32_t n = ++g_calls;
    if (n <= kLoggedCalls) {
        char line[320];
        gamevram::formatCallLine(line, sizeof line, n, static_cast<uint64_t>(callerRva), g_setting, in, r);
        Log::get().note("%s", line);
    } else if (n == kLoggedCalls + 1) {
        Log::get().note("game vram: further calls are counted, not logged.");
    }
    return hr;
}

// Wine answers wine_get_version from ntdll; Windows does not export it.
bool underWine() {
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    return ntdll != nullptr && GetProcAddress(ntdll, "wine_get_version") != nullptr;
}

// %LOCALAPPDATA%\EDUP\game_vram.txt, with its folder made (and legacy %LOCALAPPDATA%\EDVR fallback
// on read). False when LOCALAPPDATA is unset.
bool storePath(std::wstring& path) {
    wchar_t local[1024] = {};
    const DWORD got = GetEnvironmentVariableW(L"LOCALAPPDATA", local, 1024);
    if (got == 0 || got >= 1024) return false;
    const std::wstring folder = std::wstring(local) + L"\\EDUP";
    CreateDirectoryW(folder.c_str(), nullptr);   // an existing folder is fine
    path = folder + L"\\game_vram.txt";
    return true;
}

std::wstring legacyStorePath() {
    wchar_t local[1024] = {};
    const DWORD got = GetEnvironmentVariableW(L"LOCALAPPDATA", local, 1024);
    if (got == 0 || got >= 1024) return std::wstring();
    return std::wstring(local) + L"\\EDVR\\game_vram.txt";
}


// The store's records into `records`. Missing (normal before the first save) is no records. Error is a file that is there but
// could not be read -- held by another program, or too big to be ours -- and must not be written over: a save from an empty read
// would replace every other setup's record with this one.
gamevram::StoreRead loadStore(const std::wstring& path, std::vector<gamevram::Record>& records) {
    records.clear();
    std::string text;
    if (readFileBytes(path, &text, kMaxStoreBytes)) {
        records = gamevram::parseStore(text);
        return gamevram::StoreRead::Ok;
    }
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        const DWORD error = GetLastError();
        if (gamevram::classifyStoreOpen(error) == gamevram::StoreRead::Missing) {
            const std::wstring legacy = legacyStorePath();
            if (!legacy.empty() && _wcsicmp(legacy.c_str(), path.c_str()) != 0 &&
                readFileBytes(legacy, &text, kMaxStoreBytes)) {
                records = gamevram::parseStore(text);
                return gamevram::StoreRead::Ok;
            }
            return gamevram::StoreRead::Missing;
        }
    }
    return gamevram::StoreRead::Error;
}

// What other processes hold of the adapter's own memory: the whole card's figure less this process's, from one sample taken
// when auto was prepared, so nearly all of it is theirs. Waits for the reading until kOthersWaitMs after it began. Unknown
// leaves the OS budget to stand alone.
void readOthers() {
    const auto unknown = [](const char* why, double ms) {
        Log::get().note("game vram: what other programs hold of the card could not be read (%s, %.0f ms); the OS budget stands alone.",
                        why, ms);
    };
    if (!g_othersRead) {
        unknown("no thread to read it on", 0.0);
        return;
    }
    DWORD waited = 0;
    if (!adapterUsageFinished(g_othersRead, kOthersWaitMs, &waited)) {
        unknown("the counters did not answer in time", static_cast<double>(kOthersWaitMs));
        return;
    }
    if (!g_othersRead->known) {
        unknown(g_othersRead->why, g_othersRead->ms);
        return;
    }
    AdapterUsageRow row;
    if (!findAdapterRow(g_othersRead->rows, static_cast<uint32_t>(g_learn.luid.HighPart),
                        static_cast<uint32_t>(g_learn.luid.LowPart), &row)) {
        unknown("no counter for this card", g_othersRead->ms);
        return;
    }
    g_othersKnown = true;
    g_othersBytes = row.allBytes > row.ownBytes ? row.allBytes - row.ownBytes : 0;
    Log::get().note("game vram: other programs hold %llu MB of the card's %llu MB (all %llu MB, this process %llu MB; read in %.0f ms, "
                    "Elite's first question waited %lu ms for it).",
                    static_cast<unsigned long long>(g_othersBytes >> 20), static_cast<unsigned long long>(g_learn.realBytes >> 20),
                    static_cast<unsigned long long>(row.allBytes >> 20), static_cast<unsigned long long>(row.ownBytes >> 20),
                    g_othersRead->ms, static_cast<unsigned long>(waited));
}

// Auto's key, limits and store for this setup, and the start of the reading of what other programs hold; finishAuto takes
// the record once that reading is in. Sets g_learn.storePath only when there is a store to learn into. Runs again from
// gameVramNoteDevice when the device's adapter is not the one install keyed, so it starts from a clean slate each time.
void prepareAuto(IDXGIAdapter* obj, gamevram::AaSlot aa) {
    g_autoReady.store(false, std::memory_order_release);
    g_learn.active = false;
    g_learn.adopt = false;
    g_learn.store.clear();
    g_budgetIgnored = false;
    g_flightMb = 0;
    g_startBudgetBytes.store(0, std::memory_order_release);
    g_learn.storePath.clear();
    DXGI_ADAPTER_DESC desc{};
    g_learn.haveGpu = SUCCEEDED(obj->GetDesc(&desc));
    if (!g_learn.haveGpu) desc = {};   // a failed read must not key off a partial desc
    g_learn.vendorId = desc.VendorId;
    g_learn.deviceId = desc.DeviceId;
    g_learn.luid = desc.AdapterLuid;
    g_learn.realBytes = static_cast<uint64_t>(desc.DedicatedVideoMemory);
    LARGE_INTEGER driver{};
    if (FAILED(obj->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driver))) driver.QuadPart = 0;

    const bool vr = runtimeVrProfile();
    const bool wine = underWine();
    g_learn.wine = wine;
    const gamevram::ReserveBounds& bounds = gamevram::reserveBounds(vr, aa);
    g_learn.key = gamevram::makeKey(desc.VendorId, desc.DeviceId, static_cast<uint64_t>(driver.QuadPart),
                                    wine, vr, aa);
    g_learn.aaAtInstall = aa;
    g_learn.bounds = bounds;
    g_learn.facts = {};
    g_othersKnown = false;
    g_othersBytes = 0;
    // Wine has no such counter. One reading serves every card, so a re-key does not start another.
    if (!g_othersStarted && !wine) {
        g_othersStarted = true;
        g_othersRead = adapterUsageStart();
    }
    const uint64_t osBudget = g_learn.haveGpu ? osLocalBudget(obj) : 0;
    g_learn.facts.osBudgetMb = static_cast<uint32_t>(osBudget >> 20);
    g_learn.profileAtInstall = runtimeProfileValue();
    g_learn.profileNameAtInstall = runtimeProfileName();
    g_reserveMb = bounds.startMb;   // unless the store has a record
    // The key names the profile and the engine.
    const auto noteStart = [&](const std::string& tail) {
        Log::get().note("game vram: auto for %s: reserve %u MB (limits %u to %u), the engine's start; %s", g_learn.key.c_str(),
                        static_cast<unsigned>(g_reserveMb), static_cast<unsigned>(bounds.minMb),
                        static_cast<unsigned>(bounds.maxMb), tail.c_str());
    };

    if (wine) {
        if (gamevram::budgetIsWholeCard(osBudget, static_cast<uint64_t>(desc.DedicatedVideoMemory))) {
            g_budgetIgnored = true;
            noteStart("the reported budget (" + std::to_string(osBudget >> 20) + " MB) is the whole card (" +
                      std::to_string(static_cast<uint64_t>(desc.DedicatedVideoMemory) >> 20) +
                      " MB), which under Wine means no real budget: sizing from 95% of the card, and nothing is learned or saved "
                      "this session.");
            return;
        }
    }

    std::wstring path;
    if (!storePath(path)) {
        noteStart("LOCALAPPDATA is unset, so nothing is learned or saved.");
        return;
    }

    if (loadStore(path, g_learn.store) == gamevram::StoreRead::Error) {
        noteStart(runtimeProfileUtf8(path) + " is there but could not be read, so nothing is learned or saved this session.");
        return;
    }
    g_learn.storePath = path;
    g_learn.adopt = true;
}

// The rest of auto's start, at Elite's first question about an adapter: what other programs hold, the start budget, and
// this setup's record taken from the store prepareAuto read. Before that question the reading overlaps the device's
// creation and Elite's own loading.
void finishAuto() {
    g_othersKnown = false;
    g_othersBytes = 0;
    if (g_learn.haveGpu && !g_learn.wine) readOthers();
    // The facts are needed under Wine too: an old record is taken over by them.
    g_learn.facts.startMb = static_cast<uint32_t>(gamevram::startBudgetMb(
        g_learn.facts.osBudgetMb, g_learn.realBytes >> 20, g_othersKnown, g_othersBytes >> 20));
    if (!g_learn.adopt) return;
    const gamevram::ReserveBounds& bounds = g_learn.bounds;
    std::vector<gamevram::Record>& store = g_learn.store;
    const auto noteStart = [&](const std::string& tail) {
        Log::get().note("game vram: auto for %s: reserve %u MB (limits %u to %u), the engine's start; %s", g_learn.key.c_str(),
                        static_cast<unsigned>(g_reserveMb), static_cast<unsigned>(bounds.minMb),
                        static_cast<unsigned>(bounds.maxMb), tail.c_str());
    };

    gamevram::Record found;
    if (gamevram::adoptRecord(store, g_learn.key, bounds, g_learn.facts, found)) {
        g_reserveMb = found.reserveMb;
        const gamevram::Record* stored = gamevram::findRecord(store, g_learn.key);
        // A record without one (an old one, or an engine that never met the fall) starts like a new engine does.
        g_flightMb = found.flightBudgetMb != 0 ? found.flightBudgetMb : gamevram::seedFlightBudgetMb(store, g_learn.key);
        Log::get().note("game vram: auto for %s: reserve %u MB (limits %u to %u), flight budget %u MB (0: no fall seen), from %u earlier session(s) "
                        "(reserve raised %u, lowered %u times).",
                        g_learn.key.c_str(), static_cast<unsigned>(g_reserveMb), static_cast<unsigned>(bounds.minMb),
                        static_cast<unsigned>(bounds.maxMb), static_cast<unsigned>(g_flightMb),
                        static_cast<unsigned>(found.sessions), static_cast<unsigned>(found.tightened),
                        static_cast<unsigned>(found.relaxed));
        if (stored->old) {
            Log::get().note("game vram: that is an older version's record: its reserve of %u MB was taken over as %u MB, and the "
                            "flight budget is learned apart from it from now on.",
                            static_cast<unsigned>(stored->reserveMb), static_cast<unsigned>(g_reserveMb));
        }
    } else {
        g_flightMb = gamevram::seedFlightBudgetMb(store, g_learn.key);
        noteStart("flight budget " + std::to_string(g_flightMb) + " MB" +
                  (g_flightMb != 0 ? ", from another engine on this machine" : " (no fall seen)") + " (nothing learned yet).");
    }
    store.clear();
    g_learn.active = true;
}

// finishAuto once for each adapter prepareAuto keys, whichever game thread asks first.
void ensureAutoReady() {
    if (g_autoReady.load(std::memory_order_acquire)) return;
    AcquireSRWLockExclusive(&g_readyLock);
    if (!g_autoReady.load(std::memory_order_relaxed)) {
        finishAuto();
        g_autoReady.store(true, std::memory_order_release);
    }
    ReleaseSRWLockExclusive(&g_readyLock);
}

// A save that did not land: the next pass hands it over again, and the first failure of the session is said once.
void saveFailed(const std::string& why) {
    g_saveFailed.store(true, std::memory_order_release);
    if (!g_saveFailureLogged.exchange(true)) {
        Log::get().note("game vram: could not save what auto learned to %s (%s).", runtimeProfileUtf8(g_learn.storePath).c_str(),
                        why.c_str());
    }
}

// One save: another game instance may have written the store since install, so read it, merge this session onto the
// record just read (its counters, and its reserve when that instance tightened past ours), and write it back atomically.
// Runs on a pool thread (or inline when the pool refused the work).
void runSave(const SaveJob& job) {
    std::vector<gamevram::Record> store;
    if (loadStore(g_learn.storePath, store) == gamevram::StoreRead::Error) {
        saveFailed("it is there but could not be read, so it was left as it is");
        return;
    }
    // Adopted, as at install: an older version's reserve over the limit is not another session that raised it.
    gamevram::Record found;
    const bool any = gamevram::adoptRecord(store, g_learn.key, g_learn.bounds, g_learn.facts, found);
    const gamevram::Record* basis = g_saveBasis.basisFor(any ? &found : nullptr);
    const gamevram::Record rec =
        gamevram::mergeLearned(g_learn.key, job.startedReserveMb, job.startedFlightMb, job.learned, basis);
    gamevram::upsertRecord(store, rec);
    std::wstring error;
    if (!writeFileAtomic(g_learn.storePath, gamevram::serializeStore(store), &error)) {
        saveFailed("error " + runtimeProfileUtf8(error));
        return;
    }
    g_saveBasis.landed(rec);
    Log::get().note("%s", job.line.c_str());
    if (rec.reserveMb != job.learned.reserveMb || rec.flightBudgetMb != job.learned.flightBudgetMb) {
        Log::get().note("game vram: stored reserve %u MB and flight budget %u MB for %s, kept from another session that learned a tighter one.",
                        static_cast<unsigned>(rec.reserveMb), static_cast<unsigned>(rec.flightBudgetMb), g_learn.key.c_str());
    }
}

// Write whatever is pending until the slot is empty, then let the next submit start a new drain.
void drainSaves() {
    for (;;) {
        AcquireSRWLockExclusive(&g_saveLock);
        SaveJob* job = g_savePending;
        g_savePending = nullptr;
        if (!job) g_saveBusy = false;
        ReleaseSRWLockExclusive(&g_saveLock);
        if (!job) return;
        runSave(*job);
        delete job;
    }
}

VOID CALLBACK saveCallback(PTP_CALLBACK_INSTANCE, PVOID) { drainSaves(); }

// Takes ownership of job. Replaces a pending job not yet written; starts a drain unless one is queued or running.
void submitSave(SaveJob* job) {
    AcquireSRWLockExclusive(&g_saveLock);
    delete g_savePending;
    g_savePending = job;
    const bool start = !g_saveBusy;
    g_saveBusy = true;
    ReleaseSRWLockExclusive(&g_saveLock);
    if (start && !TrySubmitThreadpoolCallback(saveCallback, nullptr, nullptr)) drainSaves();
}

}  // namespace

void gameVramInstall(IDXGIAdapter* adapter) {
    static std::atomic<bool> done{false};
    if (done.exchange(true)) return;

    // advanced.game_vram is read by every profile (runtime_profile.h), passthrough and an invalid install included, and its
    // default is auto; those profiles forward only, so the answer stays the card's.
    if (!runtimeFeaturesAllowed()) {
        Log::get().note("game vram: not installed, the %s profile forwards only; Elite is told the card's real size.",
                        runtimeProfileName());
        return;
    }
    // The hook tells the main executable's calls from everyone else's; in another process (a tool or a test that loaded the
    // proxy) that executable is not Elite.
    if (!gameBuildKnown() && !gameBuildUnknown()) {
        Log::get().note("game vram: not installed, %s.", gameBuildText());
        return;
    }

    const std::string text = Config::get().getString("advanced.game_vram", "auto");
    g_setting = gamevram::parseSetting(text);
    if (!g_setting.recognised) {
        Log::get().note("game vram: advanced.game_vram = \"%s\" is not off, auto or a number of MB from 256; read as off.",
                        text.c_str());
    }
    if (g_setting.mode == gamevram::Mode::Off) {
        Log::get().note("game vram: off -- Elite is told the card's real size.");
        return;
    }

    const gamevram::AaSlot temporal = gamevram::aaSlotOf(Config::get().requestedTemporalMode());

    // The factory and any adapter we enumerate are ours to release. An adapter
    // the game passed in is not: we took no reference on it.
    IDXGIFactory1* factory = nullptr;
    IDXGIAdapter* obj = adapter;
    const bool enumerated = adapter == nullptr;
    const char* adapterSource = enumerated ? "enumerated" : "passed by the game";
    auto releaseOwned = [&] {
        if (enumerated && obj) obj->Release();
        if (factory) factory->Release();
        obj = nullptr;
        factory = nullptr;
    };

    const char* failed = nullptr;   // the step that failed
    if (enumerated) {
        // Resolved at run time, as xess_fg.cpp does: the proxy does not import dxgi.dll, and the game has loaded it by now.
        using CreateFactoryFn = HRESULT(WINAPI*)(REFIID, void**);
        HMODULE dxgi = GetModuleHandleW(L"dxgi.dll");
        if (!dxgi) dxgi = LoadLibraryW(L"dxgi.dll");
        const auto createFactory = dxgi ? reinterpret_cast<CreateFactoryFn>(GetProcAddress(dxgi, "CreateDXGIFactory1")) : nullptr;
        if (!createFactory || FAILED(createFactory(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) || !factory) {
            failed = "factory";
        } else if (FAILED(factory->EnumAdapters(0, &obj)) || !obj) {
            failed = "adapter";
        }
    }

    if (!failed) {
        // Before the hook exists, so this GetDesc is not the patched one and the reserve is known before the first answer.
        if (g_setting.mode == gamevram::Mode::Auto) prepareAuto(obj, temporal);

        // The first device creation is early enough: in every logged session the install line came before the game's
        // first GetDesc call (those come from its budget sizing during the main menu load).
        // Leaked on purpose: the patch lives as long as the process, and the
        // destructor would otherwise try to undo it at teardown.
        auto* hook = new VTableHook();
        if (!hook->attach(obj, 64)) failed = "attach";
        else if (!hook->replace(8, reinterpret_cast<void*>(&hookedGetDesc), &g_realGetDesc)) failed = "replace";
        else if (!hook->commit()) failed = "commit";
    }
    if (failed) {
        Log::get().note("game vram: not installed, the step that failed was %s.", failed);
        releaseOwned();
        return;
    }

    char owner[MAX_PATH] = {};
    const char* ownerName = vtableOwnerModuleName(g_realGetDesc, owner, sizeof owner);
    char setting[32];
    gamevram::settingText(g_setting, setting, sizeof setting);
    Log::get().note("game vram: installed, setting=%s, adapter=%s, GetDesc slot 8 -> %s; the game's own calls get the changed size, every other caller the real one.",
                    setting, adapterSource, ownerName);
    releaseOwned();
}

// The device exists only after D3D11CreateDevice returns. Elite's hardware create passes a null adapter, so install
// keyed EnumAdapters(0), which a per-app high-performance GPU is not. When the device's adapter is a different GPU, auto
// is sized and learned for that adapter instead: its own key and record (or the engine's start), set here, before Elite's
// first GetDesc, which comes from its budget sizing after create returns. The vtable patch is not touched again: every
// adapter shares it, and the hook reads the reserve on each call. A device adapter that cannot be read leaves learning
// stopped, so the session writes nothing under a key it cannot check.
void gameVramNoteDevice(ID3D11Device* device) {
    static std::atomic<bool> once{false};
    if (!device || once.exchange(true)) return;

    if (g_setting.mode == gamevram::Mode::Auto && g_realGetDesc) {
        IDXGIAdapter* adapter = nullptr;
        IDXGIDevice* dxgi = nullptr;
        if (SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi))) && dxgi) {
            if (FAILED(dxgi->GetAdapter(&adapter))) adapter = nullptr;
            dxgi->Release();
        }
        // Our own call: the hook answers it with the real desc, the caller is not the game exe.
        DXGI_ADAPTER_DESC desc{};
        if (!adapter || FAILED(adapter->GetDesc(&desc))) {
            // Learning starts only at Elite's first question, so a store to learn into is what says it would have.
            if (!g_learn.stopped && !g_learn.storePath.empty())
                Log::get().note("game vram: the device's adapter could not be read, so this session's samples are not saved.");
            g_learn.stopped = true;
        } else if (!g_learn.haveGpu ||
                   !gamevram::sameAdapterIdentity(g_learn.vendorId, g_learn.deviceId, desc.VendorId, desc.DeviceId) ||
                   desc.AdapterLuid.LowPart != g_learn.luid.LowPart || desc.AdapterLuid.HighPart != g_learn.luid.HighPart) {
            Log::get().note("game vram: the device's adapter is v=%04X d=%04X luid=%08lX, not v=%04X d=%04X luid=%08lX the reserve "
                            "was read for; auto is sized and learned for the device's adapter instead.",
                            static_cast<unsigned>(desc.VendorId & 0xFFFFu), static_cast<unsigned>(desc.DeviceId & 0xFFFFu),
                            static_cast<unsigned long>(desc.AdapterLuid.LowPart),
                            static_cast<unsigned>(g_learn.vendorId & 0xFFFFu), static_cast<unsigned>(g_learn.deviceId & 0xFFFFu),
                            static_cast<unsigned long>(g_learn.luid.LowPart));
            prepareAuto(adapter, g_learn.aaAtInstall);
        }
        if (adapter) adapter->Release();
    }
    g_learn.deviceNoted = true;
}

// The render thread, once a second, from the graphics memory watch (vram_tick.cpp). Feeds the session's figures and,
// at each minute of samples or a spill over the budget, learns the reserve for the next session and saves it.
void gameVramObserve(uint64_t nowMs, uint64_t usageBytes, uint64_t budgetBytes) {
    // Elite has not asked yet, so it was not sized: nothing to measure against. Read first: finishAuto's writes to g_learn
    // come before this budget is published.
    const uint64_t startBudget = g_startBudgetBytes.load(std::memory_order_acquire);
    if (startBudget == 0) return;
    if (!g_learn.active || g_learn.stopped) return;

    // A switch between flat and VR (or to passthrough) changes EDVR's own share by gigabytes, and the key names the profile
    // Elite's budget was sized under: what follows would be filed under the wrong setup.
    if (runtimeProfileValue() != g_learn.profileAtInstall) {
        g_learn.stopped = true;
        Log::get().note("game vram: the profile changed from %s to %s; Elite's budget was sized under %s, so this session teaches "
                        "nothing more.",
                        g_learn.profileNameAtInstall, runtimeProfileName(), g_learn.profileNameAtInstall);
        return;
    }

    if (gamevram::aaSlotOf(Config::get().requestedTemporalMode()) != g_learn.aaAtInstall) {
        g_learn.stopped = true;
        // The new mode is not named: the F8 menu steps through the modes in between, so the first change seen is often
        // not the one the player is choosing.
        const char* was = gamevram::aaSlotWord(g_learn.aaAtInstall);
        Log::get().note("game vram: the AA mode changed away from %s; Elite's budget was sized for %s, so this session teaches nothing more. "
                        "The next start sizes it for whichever mode is selected then.", was, was);
        return;
    }
    // The device's adapter is known only after create returns. Until then a sample would teach the install key,
    // which on a high-performance GPU is not the device. (A store that could not be read at install left active false.)
    if (!g_learn.deviceNoted) return;

    static gamevram::SessionWatch watch;
    const uint64_t plannedMb = gamevram::plannedBudgetMb(startBudget >> 20, g_flightMb);
    watch.sample(nowMs, usageBytes, budgetBytes, plannedMb << 20);
    const uint32_t samples = watch.samples();
    // Said once, so that a log without a result tells a short session from a watch that never ran.
    static bool noted = false;
    if (!noted && samples != 0) {
        noted = true;
        Log::get().note("game vram: watching %s: Elite was sized from %llu MB of a %llu MB start budget. The first result comes after "
                        "five minutes, or when the process has been over the budget for two.",
                        g_learn.key.c_str(), static_cast<unsigned long long>(plannedMb),
                        static_cast<unsigned long long>(startBudget >> 20));
    }
    // A pass runs at the first spill, and then once a minute of samples.
    const bool firstSpill = !g_learn.spillPassed && watch.spills() > 0;
    const bool everyMinute = samples >= kLearnEverySamples && samples - g_learn.lastPassSamples >= kLearnEverySamples;
    if (!firstSpill && !everyMinute) return;
    g_learn.lastPassSamples = samples;
    g_learn.spillPassed = watch.spills() > 0;

    // Always from the reserve and flight budget this session started with, so repeated passes never compound.
    const gamevram::LearnResult r =
        gamevram::learn(g_reserveMb, g_flightMb, static_cast<uint32_t>(startBudget >> 20),
                        g_learn.bounds, watch);
    // A save that failed is not "already written": forget it so this pass hands it over again.
    if (g_saveFailed.exchange(false, std::memory_order_acq_rel)) g_learn.haveHanded = false;
    if (g_learn.haveHanded && r.reserveMb == g_learn.handed.reserveMb && r.flightBudgetMb == g_learn.handed.flightBudgetMb &&
        std::strcmp(g_learn.handed.why, r.why) == 0)
        return;
    // A session too short to judge has taught nothing: it must not write a record that later reads as "learned".
    if (r.outcome == gamevram::LearnResult::Outcome::TooShort) return;

    auto* job = new (std::nothrow) SaveJob;
    if (!job) return;
    job->learned = r;
    job->startedReserveMb = g_reserveMb;
    job->startedFlightMb = g_flightMb;
    char line[768];
    gamevram::formatLearnLine(line, sizeof line, g_learn.key, g_reserveMb, g_flightMb, watch, r);
    job->line = line;
    g_learn.handed = r;
    g_learn.haveHanded = true;
    submitSave(job);
}

}  // namespace edvr
