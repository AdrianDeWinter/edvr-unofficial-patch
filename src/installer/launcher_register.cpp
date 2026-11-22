#include "launcher_register.h"

#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>

#include "detect.h"
#include "launcher_core.h"
#include "update_util.h"

#pragma comment(lib, "uuid.lib")

namespace edvr::installer {
namespace {

constexpr const wchar_t* kAppName = L"ED-UP Launcher";
constexpr const wchar_t* kPublisher = L"ED-UP (unofficial)";

// Deletes everything under `dir`, then `dir`, but leaves `keep` (a file path) alone. A link
// to a folder is unlinked, never followed. False when something was left behind.
bool removeTree(const std::wstring& dir, const std::wstring& keep) {
    bool clean = true;
    WIN32_FIND_DATAW find;
    HANDLE h = FindFirstFileW(joinPath(dir, L"*").c_str(), &find);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = find.cFileName;
            if (name == L"." || name == L"..") continue;
            const std::wstring path = joinPath(dir, name);
            if (find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (find.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                    if (!RemoveDirectoryW(path.c_str())) clean = false;
                } else if (!removeTree(path, keep)) {
                    clean = false;
                }
                continue;
            }
            if (samePath(path, keep)) {
                clean = false;
                continue;
            }
            SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
            if (!DeleteFileW(path.c_str())) clean = false;
        } while (FindNextFileW(h, &find));
        FindClose(h);
    }
    if (!RemoveDirectoryW(dir.c_str()) && GetFileAttributesW(dir.c_str()) != INVALID_FILE_ATTRIBUTES) clean = false;
    return clean;
}

bool setString(HKEY key, const wchar_t* name, const std::wstring& value) {
    return RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                          static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}

bool setDword(HKEY key, const wchar_t* name, DWORD value) {
    return RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value)) == ERROR_SUCCESS;
}

bool writeShortcut(const std::wstring& linkPath, const std::wstring& exe, const std::wstring& workDir) {
    IShellLinkW* link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) return false;
    link->SetPath(exe.c_str());
    link->SetWorkingDirectory(workDir.c_str());
    link->SetDescription(kAppName);
    link->SetIconLocation(exe.c_str(), 0);
    IPersistFile* file = nullptr;
    bool ok = false;
    if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&file)))) {
        ok = SUCCEEDED(file->Save(linkPath.c_str(), TRUE));
        file->Release();
    }
    link->Release();
    return ok;
}

// True when the shortcut at `linkPath` starts the launcher that lives in `launcherDir`.
bool shortcutRunsLauncherIn(const std::wstring& linkPath, const std::wstring& launcherDir) {
    IShellLinkW* link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) return false;
    bool ours = false;
    IPersistFile* file = nullptr;
    if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&file)))) {
        if (SUCCEEDED(file->Load(linkPath.c_str(), STGM_READ))) {
            wchar_t target[MAX_PATH]{};
            if (SUCCEEDED(link->GetPath(target, MAX_PATH, nullptr, SLGP_RAWPATH))) {
                const std::wstring path = target;
                ours = _wcsicmp(leafOf(path).c_str(), kLauncherExeName) == 0 &&
                       _wcsicmp(joinPath(launcherDir, leafOf(path)).c_str(), path.c_str()) == 0;
            }
        }
        file->Release();
    }
    link->Release();
    return ours;
}

}  // namespace

std::wstring defaultStartMenuDir() {
    PWSTR path = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Programs, 0, nullptr, &path)) && path) {
        dir = path;
        CoTaskMemFree(path);
    }
    return dir;
}

std::wstring defaultUninstallKey() { return L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\ED-UP"; }

bool registerLauncher(const RegisterTargets& t, std::string* error) {
    auto fail = [&](const std::string& text) {
        if (error) *error = text;
        return false;
    };
    if (t.exePath.empty() || t.registryKey.empty()) return fail("nothing to register");
    if (!t.startMenuDir.empty()) {
        if (!createDirectories(t.startMenuDir)) return fail("the Start menu folder cannot be created");
        if (!writeShortcut(joinPath(t.startMenuDir, kShortcutName), t.exePath, t.launcherDir))
            return fail("the Start menu shortcut cannot be written");
        // The shortcut from an earlier build is replaced by the one just written, not left beside it.
        const std::wstring legacy = joinPath(t.startMenuDir, kLegacyShortcutName);
        if (fileExists(legacy) && shortcutRunsLauncherIn(legacy, t.launcherDir)) DeleteFileW(legacy.c_str());
    }
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, t.registryKey.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key,
                        nullptr) != ERROR_SUCCESS)
        return fail("the entry in the list of installed programs cannot be written");
    const bool ok = setString(key, L"DisplayName", kAppName) &&
                    setString(key, L"DisplayVersion", widenUtf8(t.version)) &&
                    setString(key, L"Publisher", kPublisher) &&
                    setString(key, L"InstallLocation", t.launcherDir) &&
                    setString(key, L"DisplayIcon", t.exePath) &&
                    setString(key, L"UninstallString", L"\"" + t.exePath + L"\" --unregister") &&
                    setDword(key, L"NoModify", 1) && setDword(key, L"NoRepair", 1);
    RegCloseKey(key);
    return ok ? true : fail("the entry in the list of installed programs is incomplete");
}

bool unregisterLauncher(const RegisterTargets& t, const std::wstring& runningExe, UnregisterResult* result,
                        std::string* error) {
    UnregisterResult out;
    auto fail = [&](const std::string& text) {
        if (error) *error = text;
        return false;
    };
    // The two folders are deleted recursively, so they are checked by name first.
    if (!t.launcherDir.empty() && _wcsicmp(leafOf(t.launcherDir).c_str(), L"launcher") != 0)
        return fail("refusing to remove a folder that is not named launcher");
    if (!t.payloadDir.empty() && _wcsicmp(leafOf(t.payloadDir).c_str(), L"payload") != 0)
        return fail("refusing to remove a folder that is not named payload");

    if (!t.startMenuDir.empty()) {
        const std::wstring link = joinPath(t.startMenuDir, kShortcutName);
        const std::wstring legacy = joinPath(t.startMenuDir, kLegacyShortcutName);
        const bool legacyIsOurs = fileExists(legacy) && shortcutRunsLauncherIn(legacy, t.launcherDir);
        if (fileExists(link) || legacyIsOurs) {
            bool removed = true;
            if (fileExists(link) && !DeleteFileW(link.c_str())) removed = false;
            if (legacyIsOurs && !DeleteFileW(legacy.c_str())) removed = false;
            out.notes.push_back(removed ? "Removed the Start menu shortcut." : "Could not remove the Start menu shortcut.");
        }
    }
    if (!t.registryKey.empty()) {
        const LONG r = RegDeleteKeyW(HKEY_CURRENT_USER, t.registryKey.c_str());
        if (r == ERROR_SUCCESS) out.notes.push_back("Removed ED-UP from the list of installed programs.");
        else if (r != ERROR_FILE_NOT_FOUND) out.notes.push_back("Could not remove ED-UP from the list of installed programs.");
    }
    if (!t.payloadDir.empty() && dirExists(t.payloadDir)) {
        out.notes.push_back(removeTree(t.payloadDir, std::wstring()) ? "Removed the downloaded files."
                                                                      : "Could not remove all of the downloaded files.");
    }
    if (!t.launcherDir.empty() && dirExists(t.launcherDir)) {
        const bool clean = removeTree(t.launcherDir, runningExe);
        if (clean) {
            out.notes.push_back("Removed the launcher folder.");
        } else if (samePath(joinPath(t.launcherDir, leafOf(runningExe)), runningExe) && fileExists(runningExe)) {
            out.exeRemains = true;
            out.notes.push_back("The launcher's own file is deleted when this window closes.");
        } else {
            out.notes.push_back("Could not remove all of the launcher folder.");
        }
    }
    out.notes.push_back(
        "Left alone: ED-UP in your game folders (use Uninstall in the launcher for that, first) and the copy of your "
        "settings kept beside the launcher folder.");
    *result = std::move(out);
    return true;
}

bool scheduleSelfRemoval(const std::wstring& exePath, const std::wstring& folder) {
    wchar_t system[MAX_PATH]{};
    if (GetSystemDirectoryW(system, MAX_PATH) == 0) return false;
    const std::wstring cmd = joinPath(system, L"cmd.exe");
    std::wstring line = L"\"" + cmd + L"\" /c ping -n 4 127.0.0.1 >nul & del /f /q \"" + exePath +
                        L"\" & rmdir \"" + folder + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, &line[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr,
                        system, &si, &pi))
        return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

}  // namespace edvr::installer
