#include "selfupdate.h"

#include <windows.h>

#include "probe.h"
#include "update_util.h"

namespace edvr::installer {
namespace {

bool setError(std::string* error, const std::string& text) {
    if (error) *error = text;
    return false;
}

bool fileExistsAt(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

}  // namespace

bool moveReplacing(const std::wstring& from, const std::wstring& to) {
    return MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

bool downloadLauncher(HttpClient& http, const UpdateCheck& check, const std::wstring& exePath,
                      const std::atomic<bool>* cancel, const DownloadProgress& progress, std::string* error) {
    if (!check.manifest.hasLauncher) return setError(error, "the release lists no launcher");
    const ManifestLauncher& want = check.manifest.launcher;
    // The URL comes from the release's asset list by the manifest's name. manifest.assets holds
    // only the zips, so the launcher is not looked up there.
    const ReleaseAsset* asset = nullptr;
    for (const ReleaseAsset& a : check.assets) {
        if (a.name == want.asset) asset = &a;
    }
    if (!asset) return setError(error, "the release has no asset named " + want.asset);

    const std::wstring target = newLauncherPath(exePath);
    DeleteFileW(target.c_str());
    HttpRequest request;
    request.url = asset->url;
    request.toFile = target;
    request.maxBytes = want.size;  // the signed size: a longer body is refused, not stored
    request.cancel = cancel;
    if (progress) {
        const std::string name = want.asset;
        request.progress = [progress, name](unsigned long long received, unsigned long long total) {
            progress(name, received, total);
        };
    }
    const HttpResponse response = http.get(request);
    auto discard = [&]() { DeleteFileW(target.c_str()); };
    if (response.cancelled) {
        discard();
        return setError(error, "cancelled");
    }
    if (!response.transportOk || response.status != 200) {
        discard();
        return setError(error, response.transportOk ? "the launcher download failed (status " + std::to_string(response.status) + ")"
                                                    : "the launcher download failed: " + response.error);
    }
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(target.c_str(), GetFileExInfoStandard, &data)) {
        discard();
        return setError(error, "the downloaded launcher is missing");
    }
    const unsigned long long size = (static_cast<unsigned long long>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    if (size != want.size) {
        discard();
        return setError(error, "the downloaded launcher is not the size the manifest lists");
    }
    if (sha256File(target) != want.sha256) {
        discard();
        return setError(error, "the downloaded launcher does not match the manifest's hash");
    }
    return true;
}

bool swapInNewLauncher(const std::wstring& exePath, const FileMover& move, std::string* error) {
    const std::wstring fresh = newLauncherPath(exePath);
    const std::wstring old = oldLauncherPath(exePath);
    if (!fileExistsAt(fresh)) return setError(error, "there is no downloaded launcher to switch to");
    DeleteFileW(old.c_str());  // an earlier run's leftover, if its process has gone

    if (!move(exePath, old)) return setError(error, "the running launcher could not be renamed");
    if (!move(fresh, exePath)) {
        // The new file did not take the name. Put the running one back so there is an exe at the path.
        if (!move(old, exePath)) {
            return setError(error,
                            "the new launcher could not be put in place, and the old one could not be put back; it is at " +
                                narrowUtf8(old));
        }
        return setError(error, "the new launcher could not be put in place; the old one is back");
    }
    return true;
}

void removeLauncherLeftovers(const std::wstring& exePath) {
    DeleteFileW(oldLauncherPath(exePath).c_str());
    DeleteFileW(newLauncherPath(exePath).c_str());
}

}  // namespace edvr::installer
