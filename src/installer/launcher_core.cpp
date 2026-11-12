#include "launcher_core.h"

#include <windows.h>
#include <shellapi.h>

#include <vector>

#include "install_manifest.h"
#include "mirror.h"
#include "state.h"
#include "update_http.h"
#include "update_json.h"
#include "update_manifest.h"
#include "update_util.h"

namespace edvr::installer {
namespace {

bool lastWrite(const std::wstring& path, FILETIME* out) {
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return false;
    *out = data.ftLastWriteTime;
    return true;
}

}  // namespace

std::wstring LauncherRoots::launcherDir() const { return joinPath(dataRoot, L"launcher"); }
std::wstring LauncherRoots::cacheDir() const { return joinPath(dataRoot, L"payload"); }
std::wstring LauncherRoots::exePath() const { return joinPath(launcherDir(), kLauncherExeName); }

LauncherRoots launcherRoots(const std::wstring& dataRootArg) {
    LauncherRoots roots;
    if (dataRootArg.empty()) {
        roots.dataRoot = defaultMirrorRoot();
        roots.canonical = true;
    } else {
        roots.dataRoot = dataRootArg;
        roots.canonical = false;
    }
    return roots;
}

bool listPayloadFolder(const std::wstring& dir, std::vector<PayloadFile>* files, std::string* version,
                       std::string* error) {
    auto fail = [&](const std::string& text) {
        if (error) *error = text;
        return false;
    };
    if (!dirExists(dir)) return fail("the --payload folder does not exist");
    const std::wstring manifestPath = joinPath(dir, widenUtf8(kManifestAssetName));
    std::string bytes, readError;
    if (!readFileBytes(manifestPath, &bytes, kMaxManifestBytes, &readError))
        return fail(std::string("that folder has no ") + kManifestAssetName + " (" + readError +
                    "); point --payload at an unpacked ed-up-<version>.zip with its schema-2 manifest beside the files");
    JsonValue root;
    std::string parseError;
    if (!parseJson(bytes, &root, &parseError)) return fail(std::string(kManifestAssetName) + " is not valid JSON: " + parseError);
    Manifest manifest;
    long long schemaSeen = 0;
    const ManifestParse parsed = parseManifest(root, &manifest, &parseError, &schemaSeen);
    if (parsed == ManifestParse::SchemaTooNew) return fail(kSchemaTooNewMessage);
    if (parsed != ManifestParse::Ok) return fail(std::string(kManifestAssetName) + " is refused: " + parseError);

    // The manifest is unsigned here: the folder is the user's own, named with --payload. A file the
    // folder does not hold is skipped, as a folder unpacked from the core zip alone lacks the DLSS and
    // XeSS ones; usePayloadFiles then simply has no such item.
    std::vector<PayloadFile> found;
    for (const ManifestFile& file : manifest.files) {
        std::wstring relative = widenUtf8(file.path);
        for (wchar_t& c : relative) {
            if (c == L'/') c = L'\\';
        }
        const std::wstring path = joinPath(dir, relative);
        if (!fileExists(path)) continue;
        PayloadFile f;
        f.item = stage1PayloadItem(file.id);
        if (f.item.empty()) return fail("this launcher cannot place the file " + file.id + " yet");
        f.path = path;
        found.push_back(std::move(f));
    }
    *files = std::move(found);
    *version = manifest.build;
    return true;
}

PayloadLoad loadLauncherPayload(const LauncherRoots& roots, const std::wstring& payloadDir, const char* const* keys) {
    PayloadLoad load;
    std::string error;
    if (!payloadDir.empty()) {
        std::vector<PayloadFile> files;
        if (!listPayloadFolder(payloadDir, &files, &load.version, &error)) {
            load.note = error;
            return load;
        }
        load.kind = PayloadKind::Folder;
        if (!usePayloadFiles(files, load.version, &error)) {
            load.note = error;
            return load;
        }
        load.ok = true;
        return load;
    }

    LauncherState state;
    if (!state.load(joinPath(roots.launcherDir(), L"launcher.ini"), &error)) {
        load.note = error;
        return load;
    }
    if (state.currentManifest().empty()) {
        // The normal first run: no update has been adopted, so there is nothing to install yet.
        load.firstRun = true;
        load.note = "no update has been adopted yet";
        return load;
    }

    OfflineClient offline;
    UpdaterSettings settings;
    settings.launcherDir = roots.launcherDir();
    settings.cacheDir = roots.cacheDir();
    settings.keys = keys;
    Updater updater(offline, settings);
    Manifest manifest;
    std::vector<PayloadFile> files;
    if (!updater.loadCurrentManifest(&manifest, &files, &error)) {
        load.note = error;
        return load;
    }
    load.kind = PayloadKind::Manifest;
    load.manifestPath = joinPath(joinPath(roots.launcherDir(), L"manifests"), widenUtf8(state.currentManifest())) + L".json";
    load.version = manifest.build;
    if (!usePayloadFiles(files, manifest.build, &error)) {
        load.note = error;
        return load;
    }
    load.ok = true;
    return load;
}

bool payloadPresent(const PayloadInfo& info) { return info.haveD3d11 || info.haveOpenvr || info.haveIni; }

bool isDeveloperInstall(const std::wstring& gameDir) {
    FILETIME record{};
    const bool haveRecord = lastWrite(statePath(gameDir), &record);
    for (const wchar_t* pattern : {L"edvr_native_receipt.json*", L"edvr_flat_receipt.json*"}) {
        WIN32_FIND_DATAW find;
        HANDLE h = FindFirstFileW(joinPath(gameDir, pattern).c_str(), &find);
        if (h == INVALID_HANDLE_VALUE) continue;
        bool newer = false;
        do {
            if (find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            if (!haveRecord || CompareFileTime(&find.ftLastWriteTime, &record) > 0) newer = true;
        } while (!newer && FindNextFileW(h, &find));
        FindClose(h);
        if (newer) return true;
    }
    return false;
}

std::wstring fileVersionOf(const std::wstring& path) {
    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &handle);
    if (size == 0) return std::wstring();
    std::vector<unsigned char> buffer(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, buffer.data())) return std::wstring();
    struct LangCp {
        WORD lang, cp;
    };
    LangCp* langs = nullptr;
    UINT langBytes = 0;
    if (!VerQueryValueW(buffer.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&langs), &langBytes) ||
        langBytes < sizeof(LangCp))
        return std::wstring();
    for (UINT i = 0; i < langBytes / sizeof(LangCp); ++i) {
        wchar_t sub[96];
        _snwprintf_s(sub, _TRUNCATE, L"\\StringFileInfo\\%04x%04x\\FileVersion", langs[i].lang, langs[i].cp);
        wchar_t* value = nullptr;
        UINT length = 0;
        if (VerQueryValueW(buffer.data(), sub, reinterpret_cast<void**>(&value), &length) && value && length > 0)
            return std::wstring(value, wcsnlen(value, length));
    }
    return std::wstring();
}

namespace {
HANDLE g_instanceMutex = nullptr;
}  // namespace

bool takeInstanceMutex(unsigned waitMs) {
    for (unsigned waited = 0;; waited += 100) {
        HANDLE h = CreateMutexW(nullptr, FALSE, kInstanceMutexName);
        if (h && GetLastError() != ERROR_ALREADY_EXISTS) {
            g_instanceMutex = h;
            return true;
        }
        if (h) CloseHandle(h);
        if (waited >= waitMs) return false;
        Sleep(100);
    }
}

void releaseInstanceMutex() {
    if (g_instanceMutex) CloseHandle(g_instanceMutex);
    g_instanceMutex = nullptr;
}

void bringRunningLauncherForward() {
    HWND window = FindWindowW(kLauncherWindowClass, nullptr);
    if (!window) return;
    if (IsIconic(window)) ShowWindow(window, SW_RESTORE);
    SetForegroundWindow(window);
}

std::wstring restartArguments(bool addUpdated) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring out;
    for (int i = 1; argv && i < argc; ++i) {
        if (wcscmp(argv[i], L"--updated") == 0) continue;
        out += quoteArg(argv[i]) + L" ";
    }
    if (argv) LocalFree(argv);
    if (addUpdated) out += L"--updated";
    return out;
}

bool startLauncherProcess(const std::wstring& exe, const std::wstring& arguments) {
    std::wstring line = quoteArg(exe) + L" " + arguments;
    const size_t slash = exe.find_last_of(L"\\/");
    const std::wstring dir = slash == std::wstring::npos ? std::wstring() : exe.substr(0, slash);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), &line[0], nullptr, nullptr, FALSE, 0, nullptr, dir.empty() ? nullptr : dir.c_str(), &si, &pi))
        return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

std::wstring quoteArg(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos) return arg;
    std::wstring out = L"\"";
    size_t backslashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        if (c == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out += L'"';
        } else {
            out.append(backslashes, L'\\');
            out += c;
        }
        backslashes = 0;
    }
    out.append(backslashes * 2, L'\\');  // trailing backslashes must not escape the closing quote
    out += L'"';
    return out;
}

}  // namespace edvr::installer
