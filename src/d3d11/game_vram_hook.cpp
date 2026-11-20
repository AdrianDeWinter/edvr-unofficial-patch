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
// advanced.game_vram = auto learns its reserve per setup. The safe starting
// reserve is not the same on every machine: it differs by OS (Windows or
// Proton), upscaler, card and driver. So each setup's learned reserve is kept
// in %LOCALAPPDATA%\EDVR\game_vram.txt, keyed by vendor, device, driver,
// layer, profile and AA mode. Only DXGI's own figures are read to learn it;
// no game address is used. Learning stops when the AA mode or the profile
// changes in a session, because Elite sized its budget once, for the mode and
// the profile it started in.
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
// The reserve EDVR leaves free this session: the learned one, or the safe default. Fixed for the session.
uint32_t g_reserveMb = 0;
// Under Wine, the reported budget was the whole card at install (gamevram::budgetIsWholeCard): auto ignores it for the
// session, sizes from 95% of the card with the safe default reserve, and learns nothing. Set once, before the hook exists.
bool g_budgetIgnored = false;

constexpr uint32_t kLoggedCalls = 32;
constexpr uint32_t kLearnEverySamples = 60;   // one learning pass per minute of samples, or at the session's first crossing
constexpr size_t kMaxStoreBytes = 1u << 20;   // the store is a few hundred bytes; anything bigger is not ours

// What auto learns this session, and what it has already written.
struct AutoLearn {
    bool active = false;           // installed in auto with a store to write
    bool stopped = false;          // the AA mode or the profile changed: nothing more is learned this session
    bool haveGpu = false;          // install read a real adapter desc; the device is checked against it
    bool deviceNoted = false;      // gameVramNoteDevice has run; samples wait so they are not filed under the wrong key
    uint32_t vendorId = 0;
    uint32_t deviceId = 0;
    std::string key;
    std::string aaAtInstall;       // requestedTemporalMode() when the budget was sized
    RuntimeProfile profileAtInstall = RuntimeProfile::Invalid;   // the profile the key was made for
    const char* profileNameAtInstall = "";                       // its runtimeProfileName() (a static string)
    std::wstring storePath;        // empty when LOCALAPPDATA is unset or the store is unreadable: nothing is learned or saved
    uint32_t lastPassSamples = 0;
    uint32_t lastCrossings = 0;
    uint32_t lastSavedReserve = 0; // the last pass handed to the writer; 0 = none yet
    const char* lastSavedWhy = ""; // its reason (a static string)
};
AutoLearn g_learn;

// SAVING OFF THE RENDER THREAD. A save reads the store, merges this session's record and replaces the file atomically,
// with a flush: usually a millisecond, but tens on a slow disk, under a virus scanner or through Wine's file layer, and
// the first crossing of the budget comes when the frame is already struggling. So the render thread only decides what to
// save and hands it to a thread-pool callback. One slot: a newer result replaces one not yet written, and one callback
// at a time drains it, so two saves never read and write the store at once. If the pool will not take the work, the
// save runs inline, as it did before. key and storePath are fixed at install, so the callback reads g_learn safely.
struct SaveJob {
    gamevram::LearnResult learned;
    uint32_t startedReserveMb = 0; // the reserve this session started from (mergeLearned)
    std::string line;              // the learning line, formatted on the render thread, logged once the save landed
};
SRWLOCK g_saveLock = SRWLOCK_INIT;
SaveJob* g_savePending = nullptr;  // under g_saveLock
bool g_saveBusy = false;           // under g_saveLock: a drain is queued or running
std::atomic<bool> g_saveFailureLogged{false};
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

// Read LOCAL budget from the adapter. 0 on any failure, which compute() reads as unknown.
uint64_t osLocalBudget(IDXGIAdapter* self) {
    IDXGIAdapter3* a3 = nullptr;
    if (FAILED(self->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&a3))) || !a3) {
        return 0;
    }
    const uint64_t budget = vramReadSegment(a3, DXGI_MEMORY_SEGMENT_GROUP_LOCAL).budget;
    a3->Release();
    return budget;
}

HRESULT STDMETHODCALLTYPE hookedGetDesc(IDXGIAdapter* self, DXGI_ADAPTER_DESC* desc) {
    // Taken first, before anything else can move the stack.
    const uintptr_t caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
    const HRESULT hr = reinterpret_cast<GetDescFn>(g_realGetDesc)(self, desc);
    const ExeImage& exe = exeImage();
    const uintptr_t callerRva = caller - exe.base;
    if (FAILED(hr) || !desc || callerRva >= exe.size) return hr;

    gamevram::Inputs in;
    in.realDedicatedBytes = static_cast<uint64_t>(desc->DedicatedVideoMemory);
    in.osBudgetBytes = (g_setting.mode == gamevram::Mode::Auto && !g_budgetIgnored) ? osLocalBudget(self) : 0;
    in.reserveMb = g_reserveMb;

    const gamevram::Result r = gamevram::compute(g_setting, in);
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
        char line[256];
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

// %LOCALAPPDATA%\EDVR\game_vram.txt, with its folder made. False when LOCALAPPDATA is unset.
bool storePath(std::wstring& path) {
    wchar_t local[1024] = {};
    const DWORD got = GetEnvironmentVariableW(L"LOCALAPPDATA", local, 1024);
    if (got == 0 || got >= 1024) return false;
    const std::wstring folder = std::wstring(local) + L"\\EDVR";
    CreateDirectoryW(folder.c_str(), nullptr);   // an existing folder is fine
    path = folder + L"\\game_vram.txt";
    return true;
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
        if (gamevram::classifyStoreOpen(error) == gamevram::StoreRead::Missing) return gamevram::StoreRead::Missing;
    }
    return gamevram::StoreRead::Error;
}

// Auto's starting reserve for this setup: the learned one when the store has one, else the safe default.
// Sets g_learn.storePath only when there is a store to learn into.
void prepareAuto(IDXGIAdapter* obj, const std::string& aaMode) {
    DXGI_ADAPTER_DESC desc{};
    g_learn.haveGpu = SUCCEEDED(obj->GetDesc(&desc));
    if (!g_learn.haveGpu) desc = {};   // a failed read must not key off a partial desc
    g_learn.vendorId = desc.VendorId;
    g_learn.deviceId = desc.DeviceId;
    LARGE_INTEGER driver{};
    if (FAILED(obj->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driver))) driver.QuadPart = 0;

    const bool vr = runtimeVrProfile();
    const bool wine = underWine();
    const gamevram::AaClass aa = gamevram::aaClassOf(aaMode);
    g_learn.key = gamevram::makeKey(desc.VendorId, desc.DeviceId, static_cast<uint64_t>(driver.QuadPart),
                                    wine, vr, aaMode);
    g_learn.aaAtInstall = aaMode;
    g_learn.profileAtInstall = runtimeProfileValue();
    g_learn.profileNameAtInstall = runtimeProfileName();
    const char* profile = vr ? "vr" : "flat";
    const char* aaName = gamevram::aaClassName(aa);

    if (wine) {
        const uint64_t budget = osLocalBudget(obj);
        if (gamevram::budgetIsWholeCard(budget, static_cast<uint64_t>(desc.DedicatedVideoMemory))) {
            g_budgetIgnored = true;
            g_reserveMb = gamevram::defaultReserveMb(vr, aa);
            Log::get().note("game vram: auto for %s: the reported budget (%llu MB) is the whole card (%llu MB), which under Wine "
                            "means no real budget; sizing from 95%% of the card with reserve %u MB, the safe default for %s with "
                            "%s, and nothing is learned or saved this session.",
                            g_learn.key.c_str(), static_cast<unsigned long long>(budget >> 20),
                            static_cast<unsigned long long>(static_cast<uint64_t>(desc.DedicatedVideoMemory) >> 20),
                            static_cast<unsigned>(g_reserveMb), profile, aaName);
            return;
        }
    }

    std::wstring path;
    if (!storePath(path)) {
        g_reserveMb = gamevram::defaultReserveMb(vr, aa);
        Log::get().note("game vram: auto for %s: reserve %u MB, the safe default for %s with %s; LOCALAPPDATA is unset, so nothing is learned or saved.",
                        g_learn.key.c_str(), static_cast<unsigned>(g_reserveMb), profile, aaName);
        return;
    }

    std::vector<gamevram::Record> store;
    if (loadStore(path, store) == gamevram::StoreRead::Error) {
        g_reserveMb = gamevram::defaultReserveMb(vr, aa);
        Log::get().note("game vram: auto for %s: reserve %u MB, the safe default for %s with %s; %s is there but could not be read, so "
                        "nothing is learned or saved this session.",
                        g_learn.key.c_str(), static_cast<unsigned>(g_reserveMb), profile, aaName, runtimeProfileUtf8(path).c_str());
        return;
    }
    g_learn.storePath = path;

    const gamevram::Record* found = gamevram::findRecord(store, g_learn.key);
    if (found) {
        g_reserveMb = found->reserveMb;
        if (found->tightened == 0 && found->relaxed == 0) {
            Log::get().note("game vram: auto for %s: reserve %u MB, kept unchanged by %u earlier session(s).",
                            g_learn.key.c_str(), static_cast<unsigned>(g_reserveMb), static_cast<unsigned>(found->sessions));
        } else {
            Log::get().note("game vram: auto for %s: reserve %u MB, learned over %u earlier session(s) (raised %u, lowered %u times).",
                            g_learn.key.c_str(), static_cast<unsigned>(g_reserveMb), static_cast<unsigned>(found->sessions),
                            static_cast<unsigned>(found->tightened), static_cast<unsigned>(found->relaxed));
        }
    } else {
        g_reserveMb = gamevram::defaultReserveMb(vr, aa);
        Log::get().note("game vram: auto for %s: reserve %u MB, the safe default for %s with %s (nothing learned yet).",
                        g_learn.key.c_str(), static_cast<unsigned>(g_reserveMb), profile, aaName);
    }
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
    const gamevram::Record rec = gamevram::mergeLearned(g_learn.key, job.startedReserveMb, job.learned,
                                                        gamevram::findRecord(store, g_learn.key));
    gamevram::upsertRecord(store, rec);
    std::wstring error;
    if (!writeFileAtomic(g_learn.storePath, gamevram::serializeStore(store), &error)) {
        saveFailed("error " + runtimeProfileUtf8(error));
        return;
    }
    Log::get().note("%s", job.line.c_str());
    if (rec.reserveMb != job.learned.reserveMb) {
        Log::get().note("game vram: stored reserve %u MB for %s, kept from another session that raised it.",
                        static_cast<unsigned>(rec.reserveMb), g_learn.key.c_str());
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

    const std::string temporal = Config::get().requestedTemporalMode();

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
    g_learn.active = !g_learn.storePath.empty();

    char owner[MAX_PATH] = {};
    const char* ownerName = vtableOwnerModuleName(g_realGetDesc, owner, sizeof owner);
    char setting[32];
    gamevram::settingText(g_setting, setting, sizeof setting);
    Log::get().note("game vram: installed, setting=%s, adapter=%s, GetDesc slot 8 -> %s; the game's own calls get the changed size, every other caller the real one.",
                    setting, adapterSource, ownerName);
    releaseOwned();
}

// The device exists only after D3D11CreateDevice returns. Elite's hardware create passes a null adapter, so install
// keyed EnumAdapters(0), which a per-app high-performance GPU is not. Samples stay off that key when the device's
// adapter is a different GPU. The GetDesc lie is not retargeted: it still answers the game exe, on the shared vtable.
void gameVramNoteDevice(ID3D11Device* device) {
    static std::atomic<bool> once{false};
    if (!device || once.exchange(true)) return;

    bool stop = false;
    uint32_t gotVendor = 0;
    uint32_t gotDevice = 0;
    if (g_setting.mode == gamevram::Mode::Auto && g_learn.haveGpu) {
        IDXGIDevice* dxgi = nullptr;
        if (SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi))) && dxgi) {
            IDXGIAdapter* adapter = nullptr;
            const HRESULT got = dxgi->GetAdapter(&adapter);
            dxgi->Release();
            if (SUCCEEDED(got) && adapter) {
                DXGI_ADAPTER_DESC desc{};
                const HRESULT described = adapter->GetDesc(&desc);
                adapter->Release();
                if (SUCCEEDED(described)) {
                    gotVendor = desc.VendorId;
                    gotDevice = desc.DeviceId;
                    stop = !gamevram::sameAdapterIdentity(g_learn.vendorId, g_learn.deviceId, gotVendor, gotDevice);
                }
            }
        }
    }
    if (stop) {
        g_learn.stopped = true;
        Log::get().note("game vram: the device's adapter is v=%04X d=%04X, not v=%04X d=%04X the reserve was read for, "
                        "so this session's samples are not saved under that key.",
                        static_cast<unsigned>(gotVendor & 0xFFFFu), static_cast<unsigned>(gotDevice & 0xFFFFu),
                        static_cast<unsigned>(g_learn.vendorId & 0xFFFFu),
                        static_cast<unsigned>(g_learn.deviceId & 0xFFFFu));
    }
    g_learn.deviceNoted = true;
}

// The render thread, once a second, from the graphics memory watch (vram_tick.cpp). Feeds the session's figures and,
// at each minute of samples or a new crossing of the budget, learns the reserve for the next session and saves it.
void gameVramObserve(uint64_t nowMs, uint64_t usageBytes, uint64_t budgetBytes) {
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

    const std::string aa = Config::get().requestedTemporalMode();
    if (aa != g_learn.aaAtInstall) {
        g_learn.stopped = true;
        // The new mode is not named: the F8 menu steps through the modes in between, so the first change seen is often
        // not the one the player is choosing.
        Log::get().note("game vram: the AA mode changed away from %s; Elite's budget was sized for %s, so this session teaches nothing more. "
                        "The next start sizes it for whichever mode is selected then.",
                        g_learn.aaAtInstall.empty() ? "off" : g_learn.aaAtInstall.c_str(),
                        g_learn.aaAtInstall.empty() ? "off" : g_learn.aaAtInstall.c_str());
        return;
    }
    // The device's adapter is known only after create returns. Until then a sample would teach the install key,
    // which on a high-performance GPU is not the device. (A store that could not be read at install left active false.)
    if (g_learn.haveGpu && !g_learn.deviceNoted) return;

    static gamevram::SessionWatch watch;
    watch.sample(nowMs, usageBytes, budgetBytes);
    const uint32_t samples = watch.samples();
    const uint32_t crossings = watch.crossings();
    // A pass runs at the first crossing of the budget, and then once a minute of samples.
    const bool firstCrossing = g_learn.lastCrossings == 0 && crossings > 0;
    const bool everyMinute = samples >= kLearnEverySamples && samples - g_learn.lastPassSamples >= kLearnEverySamples;
    if (!firstCrossing && !everyMinute) return;
    g_learn.lastPassSamples = samples;
    g_learn.lastCrossings = crossings;

    // Always from the reserve this session started with, so repeated passes never compound.
    const gamevram::LearnResult r = gamevram::learn(g_reserveMb, watch);
    // A save that failed is not "already written": forget it so this pass hands it over again.
    if (g_saveFailed.exchange(false, std::memory_order_acq_rel)) g_learn.lastSavedReserve = 0;
    if (g_learn.lastSavedReserve != 0 && r.reserveMb == g_learn.lastSavedReserve &&
        std::strcmp(g_learn.lastSavedWhy, r.why) == 0)
        return;
    // A session too short to judge has taught nothing: it must not write a record that later reads as "learned".
    if (r.outcome == gamevram::LearnResult::Outcome::TooShort) return;

    auto* job = new (std::nothrow) SaveJob;
    if (!job) return;
    job->learned = r;
    job->startedReserveMb = g_reserveMb;
    char line[512];
    gamevram::formatLearnLine(line, sizeof line, g_learn.key, g_reserveMb, watch, r);
    job->line = line;
    g_learn.lastSavedReserve = r.reserveMb;
    g_learn.lastSavedWhy = r.why;
    submitSave(job);
}

}  // namespace edvr
