// The launcher replacing its own exe.
//
// Only the canonical copy does this (<LocalAppData>\EDVR\launcher\ed-up-launcher.exe). The new
// exe is downloaded beside it as <exe>.new and checked against the SIGNED manifest's size and
// SHA-256, so a changed download is never run. A running exe can be renamed but not
// overwritten or deleted, so the swap is two renames: the running file becomes <exe>.old, then
// the new file takes its name. Either rename failing puts everything back. The .old file is
// deleted by the next start.
#pragma once

#include <atomic>
#include <functional>
#include <string>

#include "updater.h"

namespace edvr::installer {

inline std::wstring newLauncherPath(const std::wstring& exe) { return exe + L".new"; }
inline std::wstring oldLauncherPath(const std::wstring& exe) { return exe + L".old"; }

// Downloads the launcher the manifest lists, from the release asset of that name, to
// <exe>.new, then checks its size and SHA-256 against the manifest. On any failure no .new
// file is left.
bool downloadLauncher(HttpClient& http, const UpdateCheck& check, const std::wstring& exePath,
                      const std::atomic<bool>* cancel, const DownloadProgress& progress, std::string* error);

// How a file is moved over another name; the rig substitutes one that fails on a chosen call.
using FileMover = std::function<bool(const std::wstring& from, const std::wstring& to)>;
bool moveReplacing(const std::wstring& from, const std::wstring& to);

// exe -> exe.old, then exe.new -> exe. On failure of either step the original is back at
// `exePath` and `error` says which step failed (and, if the restore failed too, says that).
bool swapInNewLauncher(const std::wstring& exePath, const FileMover& move, std::string* error);

// Deletes <exe>.old and <exe>.new, ignoring files that are not there or are in use.
void removeLauncherLeftovers(const std::wstring& exePath);

}  // namespace edvr::installer
