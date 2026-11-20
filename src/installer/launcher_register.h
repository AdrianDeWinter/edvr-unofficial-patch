// Putting the launcher on the user's machine, and taking it off: a Start menu shortcut and
// the entry in Windows' list of installed programs.
//
// Both live under the current user: the shortcut in their Start menu folder, the entry
// under HKCU. Nothing needs administrator rights. The roots are parameters so that
// tools\launcher_test can run the whole thing against a scratch folder and a registry
// subkey of its own.
#pragma once

#include <string>
#include <vector>

namespace edvr::installer {

struct RegisterTargets {
    std::wstring exePath;      // the launcher the shortcut and the uninstall entry point at
    std::wstring launcherDir;  // removed by unregister, except the exe that is running
    std::wstring payloadDir;   // the file cache, removed by unregister
    std::wstring startMenuDir; // the folder that gets ED-UP Launcher.lnk
    std::wstring registryKey;  // under HKCU
    std::string  version;      // DisplayVersion
};

// The current user's Start menu Programs folder.
std::wstring defaultStartMenuDir();

// HKCU\Software\Microsoft\Windows\CurrentVersion\Uninstall\ED-UP
std::wstring defaultUninstallKey();

inline constexpr const wchar_t* kShortcutName = L"ED-UP Launcher.lnk";
// The name earlier builds gave the shortcut. Register and unregister remove it only while it
// still starts the launcher, so a shortcut of the user's own with that name is left alone.
inline constexpr const wchar_t* kLegacyShortcutName = L"ED-UP.lnk";

// Writes the shortcut and the registry entry (DisplayName, DisplayVersion, Publisher,
// InstallLocation, UninstallString "<exe>" --unregister, NoModify, NoRepair), replacing
// earlier ones. COM must be initialised on the calling thread.
bool registerLauncher(const RegisterTargets& targets, std::string* error);

struct UnregisterResult {
    std::vector<std::string> notes;  // what was removed and what was left, for the report
    bool exeRemains = false;         // `runningExe` is inside launcherDir and could not be deleted
};

// Removes the shortcut, the registry entry, the payload cache and the launcher folder.
// `runningExe` (may be empty) is the exe doing this: a running image cannot be deleted, so
// it stays and `exeRemains` says so. The launcher folder and the cache folder are removed
// only if they are named "launcher" and "payload": a wrong path in a caller must not become
// a recursive delete of something else. The mod in game folders and the settings mirror are
// never touched, and the notes say so.
bool unregisterLauncher(const RegisterTargets& targets, const std::wstring& runningExe, UnregisterResult* result,
                        std::string* error);

// Starts a hidden command that waits a few seconds, then deletes `exePath` and removes its
// folder. For the last step of --unregister, once this process has exited.
bool scheduleSelfRemoval(const std::wstring& exePath, const std::wstring& folder);

}  // namespace edvr::installer
