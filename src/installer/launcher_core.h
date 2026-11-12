// What the launcher needs around the installer's engine and the update core, with no window
// and no command line in it: where its files live, how a payload is loaded, and the checks
// that stand between the launcher and a game folder somebody else is working in.
//
// Every root is a parameter. The launcher passes %LOCALAPPDATA%\EDVR, or the folder
// --data-root names; tools\launcher_test passes a scratch folder.
#pragma once

#include <string>
#include <vector>

#include "detect.h"
#include "launcher_state.h"
#include "payload.h"
#include "updater.h"

namespace edvr::installer {

// Where the launcher keeps its own things. <dataRoot>\launcher holds the exe, launcher.ini, the
// stored manifests and the API reply cache; <dataRoot>\payload is the file cache; the settings
// mirror folders (mirror.h) sit directly in <dataRoot>, as they always have.
struct LauncherRoots {
    std::wstring dataRoot;
    // False when --data-root named the folder. Such a run copies nowhere, registers nothing,
    // never replaces itself and takes no mutex: it exists so the window and the update path
    // can be run against a real release without touching the user's real folders.
    bool canonical = true;

    std::wstring launcherDir() const;
    std::wstring cacheDir() const;
    std::wstring exePath() const;  // <launcherDir>\ed-up-launcher.exe
};

// `dataRootArg` empty: %LOCALAPPDATA%\EDVR, canonical. Otherwise that folder, not canonical.
LauncherRoots launcherRoots(const std::wstring& dataRootArg);

// The file name of the launcher exe, which a release asset and the canonical path share.
inline constexpr const wchar_t* kLauncherExeName = L"ed-up-launcher.exe";

// A client that never reaches the network, for reading a stored manifest. Updater wants one
// even for loadCurrentManifest, which makes no request.
class OfflineClient : public HttpClient {
public:
    HttpResponse get(const HttpRequest&) override {
        HttpResponse r;
        r.error = "offline";
        return r;
    }
};

enum class PayloadKind { None, Folder, Manifest };

struct PayloadLoad {
    PayloadKind kind = PayloadKind::None;
    bool ok = false;           // the process's payload is now these files
    bool firstRun = false;     // no manifest adopted yet: nothing to load, and nothing wrong
    std::string version;       // the build stamp the install record will carry
    std::string note;          // what to do, or what went wrong, in a sentence for the window
    std::wstring manifestPath; // the stored manifest, when kind is Manifest
};

// The files of an unpacked ed-up-<ver>.zip, listed by the schema-2 manifest <dir>\ed-up-manifest.json
// (unsigned is fine here) and taken by each file's `path` under the folder. The version is the
// manifest's build. Nothing here checks a hash: the folder is the user's, named with --payload,
// and a signed manifest does not cover it.
bool listPayloadFolder(const std::wstring& dir, std::vector<PayloadFile>* files, std::string* version,
                       std::string* error);

// Loads the payload the launcher will install from: the --payload folder when one is given,
// else the manifest launcher.ini names (re-verified, its files hashed once by
// usePayloadFiles), else none. With none, nothing is switched: payloadItem finds nothing in a
// launcher build, which is what "no payload" reads as (payloadPresent). `keys` null means the
// release keys. The first failure of a folder or manifest is the `note`.
PayloadLoad loadLauncherPayload(const LauncherRoots& roots, const std::wstring& payloadDir, const char* const* keys);

// True when the payload carries the files an install needs. A launcher built without
// resources, with nothing loaded, says false.
bool payloadPresent(const PayloadInfo& info);

// install_edvr.py puts branch builds in the same folders. A folder whose
// edvr_native_receipt.json or edvr_flat_receipt.json (or a backup-named sibling of either) was
// written after the install record, or that has a receipt and no record at all, belongs to
// whoever is developing here: a hash-based repair would overwrite a build about to be flown.
// Only the confirmation warns; nothing is ever done to such a folder on its own.
bool isDeveloperInstall(const std::wstring& gameDir);

// The VERSIONINFO FileVersion of a file, or empty.
std::wstring fileVersionOf(const std::wstring& path);

// One argument as CommandLineToArgvW will read it back.
std::wstring quoteArg(const std::wstring& arg);

// One launcher window per user. The mutex is held by the process that owns the window.
inline constexpr const wchar_t* kInstanceMutexName = L"Local\\ED-UP-Launcher";
inline constexpr const wchar_t* kLauncherWindowClass = L"EdupLauncherWindow";

// True when this process now holds the mutex. With `waitMs` it keeps trying that long, for a
// launcher that has just started its replacement and is about to exit.
bool takeInstanceMutex(unsigned waitMs);
// Lets go of it, before another copy is started.
void releaseInstanceMutex();
// Restores and raises the window of the launcher that holds the mutex.
void bringRunningLauncherForward();

// This process's own arguments, quoted and ready to hand to another copy; --updated is added when
// the copy is the replacement a self-update started.
std::wstring restartArguments(bool addUpdated);
// Starts `exe` with `arguments`, unelevated, and does not wait.
bool startLauncherProcess(const std::wstring& exe, const std::wstring& arguments);

}  // namespace edvr::installer
