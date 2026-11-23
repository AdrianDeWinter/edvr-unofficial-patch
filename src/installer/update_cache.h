// The payload cache: verified files kept by their SHA-256.
//
// A file lives at <root>\<sha256> and nowhere else, and the name is the
// validated hash, never anything the network or a zip supplied. That makes
// repair work offline and a rollback one switch of manifest. Files are written
// to a temporary name, flushed and then moved into place, so a crash or a
// failure leaves either a whole file or nothing; a file that is there is
// trusted only after hasVerified has hashed it again.
#pragma once

#include <functional>
#include <set>
#include <string>
#include <vector>

#include "update_manifest.h"

namespace edvr::installer {

// Fetches one manifest asset (a zip, or the launcher) into `destination`, a file
// the cache named. The fetcher knows where the asset lives; the cache does not.
using AssetFetcher = std::function<bool(const ManifestAsset& asset, const std::wstring& destination,
                                        std::string* error)>;

class PayloadCache {
public:
    explicit PayloadCache(const std::wstring& root) : root_(root) {}

    const std::wstring& root() const { return root_; }

    // <root>\<sha256>, or empty when `sha256` is not 64 lower-case hex characters.
    std::wstring pathFor(const std::string& sha256) const;

    // True when the cached file exists, has `size` bytes and hashes to `sha256`.
    bool hasVerified(const std::string& sha256, unsigned long long size) const;

    // True when the cached file exists and has `size` bytes. The hash is not read: a caller
    // that goes on to read the whole file anyway (usePayloadFiles) checks it there, once.
    bool hasSized(const std::string& sha256, unsigned long long size) const;

    // True when every required file of the manifest is cached and verified. When `includeOptIn`
    // is false, opt-in files (those with a non-empty `when.option`) are skipped. Hashes listed
    // in `preVerified` (returned by an immediately preceding ensureFiles call in
    // the same run) are checked by size instead of re-reading and re-hashing.
    bool hasAll(const Manifest& manifest, const std::set<std::string>* preVerified = nullptr,
                bool includeOptIn = true) const;

    // Makes every required file of the manifest present, fetching only the assets that
    // hold a file the cache lacks. When `includeOptIn` is false, opt-in files (and any
    // split asset that only holds opt-in files) are neither downloaded nor cached. Each
    // fetched asset is checked for size and hash BEFORE its zip is opened, each file after
    // it is extracted, and nothing is moved into place unverified. Temporary files left by an
    // earlier run are removed first. On failure no partial file remains; files
    // that were completed stay, being whole and verified. `fetchedAssets` (may
    // be null) receives the names of the assets that were fetched. `verifiedOut`
    // (may be null) receives the sha256 hashes verified during this call.
    bool ensureFiles(const Manifest& manifest, const AssetFetcher& fetcher, std::string* error,
                     std::vector<std::string>* fetchedAssets = nullptr,
                     std::set<std::string>* verifiedOut = nullptr,
                     bool includeOptIn = true,
                     std::string* optInWarningOut = nullptr);

    // Removes every cached file no manifest in `keep` lists, and any leftover
    // temporary file. Files whose names are not hashes are left alone.
    void prune(const std::vector<Manifest>& keep);

    // A temporary name inside the cache for a download of the launcher or an
    // asset; removed by the next ensureFiles or prune.
    std::wstring temporaryPath(const std::string& tag) const;

private:
    void removeTemporaries() const;

    std::wstring root_;
};

}  // namespace edvr::installer
