#include "update_cache.h"

#include <windows.h>

#include <set>

#include "probe.h"
#include "update_util.h"
#include "update_zip.h"

namespace edvr::installer {
namespace {

bool fileSizeOf(const std::wstring& path, unsigned long long* size) {
    WIN32_FILE_ATTRIBUTE_DATA data = {};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return false;
    if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return false;
    *size = (static_cast<unsigned long long>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    return true;
}

bool endsWith(const std::wstring& text, const wchar_t* suffix) {
    const std::wstring s(suffix);
    return text.size() >= s.size() && text.compare(text.size() - s.size(), s.size(), s) == 0;
}

bool setError(std::string* error, const std::string& text) {
    if (error) *error = text;
    return false;
}

// Writes `bytes` to `temporary`, flushes, and moves it over `target`.
bool writeThenMove(const std::wstring& temporary, const std::wstring& target, const std::vector<unsigned char>& bytes) {
    HANDLE f = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    size_t done = 0;
    bool ok = true;
    while (done < bytes.size()) {
        const size_t left = bytes.size() - done;
        const DWORD want = static_cast<DWORD>(left > (1u << 20) ? (1u << 20) : left);
        DWORD written = 0;
        if (!WriteFile(f, bytes.data() + done, want, &written, nullptr) || written != want) {
            ok = false;
            break;
        }
        done += written;
    }
    if (ok) ok = FlushFileBuffers(f) != 0;
    CloseHandle(f);
    if (ok) ok = MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    if (!ok) DeleteFileW(temporary.c_str());
    return ok;
}

}  // namespace

std::wstring PayloadCache::pathFor(const std::string& sha256) const {
    if (!isLowerHex(sha256, 64)) return std::wstring();
    return joinPath(root_, widenUtf8(sha256));
}

std::wstring PayloadCache::temporaryPath(const std::string& tag) const {
    return joinPath(root_, widenUtf8(tag) + L".tmp");
}

bool PayloadCache::hasVerified(const std::string& sha256, unsigned long long size) const {
    const std::wstring path = pathFor(sha256);
    if (path.empty()) return false;
    unsigned long long actual = 0;
    if (!fileSizeOf(path, &actual) || actual != size) return false;
    return sha256File(path) == sha256;
}

bool PayloadCache::hasSized(const std::string& sha256, unsigned long long size) const {
    const std::wstring path = pathFor(sha256);
    if (path.empty()) return false;
    unsigned long long actual = 0;
    return fileSizeOf(path, &actual) && actual == size;
}

bool PayloadCache::hasAll(const Manifest& manifest) const {
    for (const ManifestFile& f : manifest.files) {
        if (!hasVerified(f.sha256, f.size)) return false;
    }
    return true;
}

void PayloadCache::removeTemporaries() const {
    WIN32_FIND_DATAW find;
    HANDLE h = FindFirstFileW(joinPath(root_, L"*.tmp").c_str(), &find);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) DeleteFileW(joinPath(root_, find.cFileName).c_str());
    } while (FindNextFileW(h, &find));
    FindClose(h);
}

bool PayloadCache::ensureFiles(const Manifest& manifest, const AssetFetcher& fetcher, std::string* error,
                               std::vector<std::string>* fetchedAssets) {
    if (!createDirectories(root_)) return setError(error, "the payload folder cannot be created");
    removeTemporaries();
    if (fetchedAssets) fetchedAssets->clear();

    int index = 0;
    for (const ManifestAsset& asset : manifest.assets) {
        ++index;
        std::vector<const ManifestFile*> missing;
        for (const ManifestFile& f : manifest.files) {
            if (f.asset == asset.name && !hasVerified(f.sha256, f.size)) missing.push_back(&f);
        }
        if (missing.empty()) continue;

        const std::wstring zipPath = temporaryPath("asset-" + std::to_string(index));
        auto discard = [&]() { DeleteFileW(zipPath.c_str()); };
        std::string fetchError;
        if (!fetcher(asset, zipPath, &fetchError)) {
            discard();
            return setError(error, asset.name + ": " + (fetchError.empty() ? std::string("the download failed") : fetchError));
        }
        if (fetchedAssets) fetchedAssets->push_back(asset.name);

        // The asset is checked whole, before anything reads its structure.
        unsigned long long size = 0;
        if (!fileSizeOf(zipPath, &size) || size != asset.size) {
            discard();
            return setError(error, asset.name + ": the download is not the size the manifest lists");
        }
        if (sha256File(zipPath) != asset.sha256) {
            discard();
            return setError(error, asset.name + ": the download does not match the manifest's hash");
        }

        std::string problem;
        {
            ZipReader zip;
            std::string zipError;
            if (!zip.openFile(zipPath, &zipError)) {
                problem = asset.name + ": " + zipError;
            } else {
                for (const ManifestFile* f : missing) {
                    if (hasVerified(f->sha256, f->size)) continue;  // an earlier entry had the same bytes
                    std::vector<unsigned char> bytes;
                    if (!zip.extract(f->path, f->size, &bytes, &zipError)) {
                        problem = asset.name + ": " + f->path + ": " + zipError;
                        break;
                    }
                    if (sha256Bytes(bytes.data(), bytes.size()) != f->sha256) {
                        problem = asset.name + ": " + f->path + " does not match the manifest's hash";
                        break;
                    }
                    const std::wstring target = pathFor(f->sha256);
                    if (!writeThenMove(temporaryPath("file-" + f->sha256.substr(0, 16)), target, bytes)) {
                        problem = asset.name + ": " + f->path + " could not be written to the cache";
                        break;
                    }
                }
            }
        }
        discard();
        if (!problem.empty()) return setError(error, problem);
    }
    return true;
}

void PayloadCache::prune(const std::vector<Manifest>& keep) {
    std::set<std::wstring> wanted;
    for (const Manifest& m : keep) {
        for (const ManifestFile& f : m.files) wanted.insert(widenUtf8(f.sha256));
    }
    WIN32_FIND_DATAW find;
    HANDLE h = FindFirstFileW(joinPath(root_, L"*").c_str(), &find);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const std::wstring name = find.cFileName;
        bool remove = endsWith(name, L".tmp");
        if (!remove && isLowerHex(narrowUtf8(name), 64)) remove = wanted.find(name) == wanted.end();
        if (remove) DeleteFileW(joinPath(root_, name).c_str());
    } while (FindNextFileW(h, &find));
    FindClose(h);
}

}  // namespace edvr::installer
