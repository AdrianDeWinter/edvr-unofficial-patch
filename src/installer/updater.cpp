#include "updater.h"

#include <windows.h>

#include <algorithm>

#include "../common/iniedit.h"
#include "launcher_state.h"
#include "probe.h"
#include "update_json.h"
#include "update_keys.h"
#include "update_util.h"

namespace edvr::installer {
namespace {

constexpr unsigned long long kMaxApiBytes = 8ull << 20;
constexpr unsigned long long kMaxSmallAssetBytes = 1ull << 20;
constexpr size_t kKeptManifests = 3;

UpdateCheck outcomeOnly(UpdateOutcome outcome, const std::string& message) {
    UpdateCheck c;
    c.outcome = outcome;
    c.message = message;
    return c;
}

bool isThrottleOrServerFailure(int status) { return status == 403 || status == 429 || status >= 500; }

std::string statusText(int status) { return "status " + std::to_string(status); }

// The reply cache: the ETag on the first line, the body after it.
bool readApiCache(const std::wstring& path, std::string* etag, std::string* body) {
    std::string text;
    if (!readFileBytes(path, &text, kMaxApiBytes + 1024, nullptr)) return false;
    const size_t newline = text.find('\n');
    if (newline == std::string::npos) return false;
    *etag = text.substr(0, newline);
    *body = text.substr(newline + 1);
    return !body->empty();
}

void writeApiCache(const std::wstring& path, const std::string& etag, const std::string& body) {
    // An ETag cannot hold a line break; one that somehow does is not stored.
    if (etag.find('\n') != std::string::npos || etag.find('\r') != std::string::npos) return;
    writeFileAtomic(path, etag + "\n" + body);
}

// The "sha256:<hex>" digest GitHub reports for an asset, as bare lower-case hex.
std::string digestOf(const JsonValue& asset) {
    const JsonValue* digest = asset.find("digest");
    if (!digest || !digest->isString()) return std::string();
    const std::string prefix = "sha256:";
    if (digest->text.compare(0, prefix.size(), prefix) != 0) return std::string();
    const std::string hex = digest->text.substr(prefix.size());
    return isLowerHex(hex, 64) ? hex : std::string();
}

bool readAssets(const JsonValue& release, std::vector<ReleaseAsset>* out) {
    const JsonValue* assets = release.find("assets");
    if (!assets || !assets->isArray()) return false;
    for (const JsonValue& asset : assets->items) {
        if (!asset.isObject()) continue;
        const JsonValue* name = asset.find("name");
        const JsonValue* url = asset.find("browser_download_url");
        if (!name || !name->isString() || !url || !url->isString()) continue;
        ReleaseAsset a;
        a.name = name->text;
        a.url = url->text;
        a.digest = digestOf(asset);
        const JsonValue* size = asset.find("size");
        if (size && size->isInt() && size->integer >= 0) a.size = static_cast<unsigned long long>(size->integer);
        out->push_back(std::move(a));
    }
    return true;
}

const ReleaseAsset* findAsset(const std::vector<ReleaseAsset>& assets, const std::string& name) {
    for (const ReleaseAsset& a : assets) {
        if (a.name == name) return &a;
    }
    return nullptr;
}

bool holdsSignedManifest(const std::vector<ReleaseAsset>& assets) {
    return findAsset(assets, kManifestAssetName) && findAsset(assets, kSignatureAssetName);
}

std::vector<std::pair<std::string, std::string>> githubHeaders(const std::string& etag) {
    std::vector<std::pair<std::string, std::string>> headers = {
        {"Accept", "application/vnd.github+json"}, {"X-GitHub-Api-Version", "2022-11-28"}};
    if (!etag.empty()) headers.push_back({"If-None-Match", etag});
    return headers;
}

bool writeFile(const std::wstring& path, const std::string& bytes) { return writeFileAtomic(path, bytes); }

// Stored manifests, newest first by file time.
struct StoredManifest {
    std::string sha;
    unsigned long long written = 0;
};

std::vector<StoredManifest> listStored(const std::wstring& dir) {
    std::vector<StoredManifest> found;
    WIN32_FIND_DATAW find;
    HANDLE h = FindFirstFileW(joinPath(dir, L"*.json").c_str(), &find);
    if (h == INVALID_HANDLE_VALUE) return found;
    do {
        if (find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const std::string name = narrowUtf8(find.cFileName);
        if (name.size() != 64 + 5 || name.compare(64, 5, ".json") != 0) continue;
        const std::string sha = name.substr(0, 64);
        if (!isLowerHex(sha, 64)) continue;
        StoredManifest s;
        s.sha = sha;
        s.written = (static_cast<unsigned long long>(find.ftLastWriteTime.dwHighDateTime) << 32) |
                    find.ftLastWriteTime.dwLowDateTime;
        found.push_back(std::move(s));
    } while (FindNextFileW(h, &find));
    FindClose(h);
    std::sort(found.begin(), found.end(),
              [](const StoredManifest& a, const StoredManifest& b) { return a.written > b.written; });
    return found;
}

}  // namespace

std::wstring Updater::statePath() const { return joinPath(settings_.launcherDir, L"launcher.ini"); }

const char* const* Updater::keys() const { return settings_.keys ? settings_.keys : kUpdatePublicKeys; }

UpdateCheck Updater::check() {
    LauncherState state;
    std::string stateError;
    if (!state.load(statePath(), &stateError))
        return outcomeOnly(UpdateOutcome::Skipped, "launcher.ini could not be read, so the update check was skipped");
    const std::string channel = settings_.channel.empty() ? state.channel() : (settings_.channel == "rc" ? "rc" : "stable");
    const bool rc = channel == "rc";

    const std::wstring cachePath = joinPath(settings_.launcherDir, rc ? L"api-rc.cache" : L"api-stable.cache");
    std::string cachedEtag, cachedBody;
    if (!readApiCache(cachePath, &cachedEtag, &cachedBody)) {
        cachedEtag.clear();
        cachedBody.clear();
    }

    HttpRequest request;
    request.url = settings_.apiBase + (rc ? "/releases?per_page=20" : "/releases/latest");
    request.headers = githubHeaders(cachedBody.empty() ? std::string() : cachedEtag);
    request.maxBytes = kMaxApiBytes;
    const HttpResponse reply = http_.get(request);
    if (!reply.transportOk)
        return outcomeOnly(UpdateOutcome::Skipped, "GitHub could not be reached (" + reply.error + "), so the update check was skipped");

    std::string body;
    if (reply.status == 304) {
        if (cachedBody.empty())
            return outcomeOnly(UpdateOutcome::Skipped, "GitHub said nothing changed but there is no stored reply, so the update check was skipped");
        body = cachedBody;
    } else if (reply.status == 200) {
        body = reply.body;
        createDirectories(settings_.launcherDir);
        writeApiCache(cachePath, reply.etag, body);
    } else if (isThrottleOrServerFailure(reply.status)) {
        return outcomeOnly(UpdateOutcome::Skipped, "GitHub answered with " + statusText(reply.status) + ", so the update check was skipped");
    } else {
        return outcomeOnly(UpdateOutcome::Skipped, "GitHub answered unexpectedly (" + statusText(reply.status) + "), so the update check was skipped");
    }

    JsonValue root;
    std::string parseError;
    if (!parseJson(body, &root, &parseError))
        return outcomeOnly(UpdateOutcome::Skipped, "GitHub's reply could not be read, so the update check was skipped");

    // Choose the release: stable is whatever /latest returned; rc is the first
    // release in list order that is not a draft and holds both manifest assets.
    const JsonValue* release = nullptr;
    std::vector<ReleaseAsset> assets;
    if (!rc) {
        if (!root.isObject()) return outcomeOnly(UpdateOutcome::Skipped, "GitHub's reply was not a release, so the update check was skipped");
        release = &root;
        if (!readAssets(*release, &assets) || !holdsSignedManifest(assets))
            return outcomeOnly(UpdateOutcome::UpToDate, "the latest release carries no signed update");
    } else {
        if (!root.isArray()) return outcomeOnly(UpdateOutcome::Skipped, "GitHub's reply was not a release list, so the update check was skipped");
        for (const JsonValue& candidate : root.items) {
            if (!candidate.isObject()) continue;
            const JsonValue* draft = candidate.find("draft");
            if (draft && draft->type == JsonValue::Type::Bool && draft->boolean) continue;
            std::vector<ReleaseAsset> candidateAssets;
            if (!readAssets(candidate, &candidateAssets) || !holdsSignedManifest(candidateAssets)) continue;
            release = &candidate;
            assets = std::move(candidateAssets);
            break;
        }
        if (!release) return outcomeOnly(UpdateOutcome::UpToDate, "no release carries a signed update");
    }
    const JsonValue* tagName = release->find("tag_name");
    if (!tagName || !tagName->isString() || tagName->text.empty())
        return outcomeOnly(UpdateOutcome::Skipped, "a release has no tag, so the update check was skipped");
    const std::string tag = tagName->text;
    const ReleaseAsset& manifestAsset = *findAsset(assets, kManifestAssetName);
    const ReleaseAsset& signatureAsset = *findAsset(assets, kSignatureAssetName);

    // Same manifest as the one already adopted: nothing to download.
    if (!manifestAsset.digest.empty() && manifestAsset.digest == state.currentManifest())
        return outcomeOnly(UpdateOutcome::UpToDate, "ED-UP is up to date");

    auto fetchSmall = [&](const ReleaseAsset& asset, std::string* bytes, UpdateCheck* failure) {
        HttpRequest req;
        req.url = asset.url;
        req.maxBytes = kMaxSmallAssetBytes;
        const HttpResponse r = http_.get(req);
        if (!r.transportOk) {
            *failure = outcomeOnly(UpdateOutcome::Skipped, asset.name + " could not be downloaded (" + r.error + "), so the update check was skipped");
            return false;
        }
        if (r.status != 200) {
            *failure = outcomeOnly(isThrottleOrServerFailure(r.status) ? UpdateOutcome::Skipped : UpdateOutcome::Refused,
                                   asset.name + " could not be downloaded (" + statusText(r.status) + ")");
            return false;
        }
        *bytes = r.body;
        return true;
    };

    UpdateCheck result;
    std::string manifestBytes, signatureText;
    if (!fetchSmall(manifestAsset, &manifestBytes, &result)) return result;
    if (!fetchSmall(signatureAsset, &signatureText, &result)) return result;
    // A file that differs from GitHub's own listing is not a verdict on the release. After an asset
    // is replaced, as a re-cut does, GitHub's download cache serves the old bytes for some minutes
    // while the API already lists the new ones (seen on the 2026-11-07 test release). Nothing is
    // offered, and the next check looks again; only the signature can refuse a release.
    if (!manifestAsset.digest.empty() && sha256Bytes(manifestBytes.data(), manifestBytes.size()) != manifestAsset.digest)
        return outcomeOnly(UpdateOutcome::Skipped, "GitHub is still serving an older copy of the manifest, so the update check was skipped");
    if (!signatureAsset.digest.empty() && sha256Bytes(signatureText.data(), signatureText.size()) != signatureAsset.digest)
        return outcomeOnly(UpdateOutcome::Skipped, "GitHub is still serving an older copy of the signature, so the update check was skipped");

    Manifest manifest;
    std::string why;
    ManifestFailure failure;
    if (!loadVerifiedManifest(manifestBytes, signatureText, keys(), &manifest, &why, &failure)) {
        if (failure.schemaTooNew) {
            // Refused before any download: this launcher cannot read that manifest at all.
            UpdateCheck refused = outcomeOnly(UpdateOutcome::Refused, kSchemaTooNewMessage);
            refused.tag = tag;
            refused.detail = why;  // "manifest schema N, launcher reads 2"
            return refused;
        }
        return outcomeOnly(UpdateOutcome::Refused, "the release " + tag + " was refused: " + why);
    }
    if (manifest.tag != tag)
        return outcomeOnly(UpdateOutcome::Refused, "the release " + tag + " was refused: its manifest is for another tag");
    // A re-offered release whose digest GitHub did not report is caught here.
    if (sha256Bytes(manifestBytes.data(), manifestBytes.size()) == state.currentManifest())
        return outcomeOnly(UpdateOutcome::UpToDate, "ED-UP is up to date");
    if (manifest.issued < state.highestIssued())
        return outcomeOnly(UpdateOutcome::UpToDate, "the release " + tag + " is older than one already accepted and was ignored");

    const std::string& ours = settings_.launcherVersion;

    result = UpdateCheck();
    result.outcome = UpdateOutcome::Available;
    result.message = "ED-UP " + manifest.version + " is available";
    result.tag = tag;
    result.manifestSha256 = sha256Bytes(manifestBytes.data(), manifestBytes.size());
    result.manifestBytes = std::move(manifestBytes);
    result.signatureText = std::move(signatureText);
    result.signatureVerified = true;
    result.launcherNeedsUpdate = manifest.hasLauncher && manifest.launcher.sha256 != settings_.launcherSha256 &&
                                 compareVersions(manifest.launcher.version, ours) >= 0;
    result.manifest = std::move(manifest);
    result.assets = std::move(assets);
    return result;
}

bool Updater::downloadUpdate(const UpdateCheck& check, const std::atomic<bool>* cancel,
                             const DownloadProgress& progress, std::string* error) {
    if (check.outcome != UpdateOutcome::Available) {
        if (error) *error = "there is no update to download";
        return false;
    }
    PayloadCache cache(settings_.cacheDir);
    const AssetFetcher fetcher = [&](const ManifestAsset& asset, const std::wstring& destination, std::string* why) {
        const ReleaseAsset* listed = findAsset(check.assets, asset.name);
        if (!listed) {
            *why = "the release does not list this asset";
            return false;
        }
        HttpRequest request;
        request.url = listed->url;
        request.toFile = destination;
        request.maxBytes = asset.size;
        request.cancel = cancel;
        if (progress) {
            const std::string name = asset.name;
            request.progress = [&progress, name](unsigned long long got, unsigned long long total) {
                progress(name, got, total);
            };
        }
        const HttpResponse r = http_.get(request);
        if (!r.transportOk) {
            *why = r.cancelled ? std::string("cancelled") : r.error;
            return false;
        }
        if (r.status != 200) {
            *why = "the download failed (" + statusText(r.status) + ")";
            return false;
        }
        return true;
    };
    return cache.ensureFiles(check.manifest, fetcher, error);
}

bool Updater::commitManifest(const UpdateCheck& check, std::string* error) {
    auto fail = [&](const char* text) {
        if (error) *error = text;
        return false;
    };
    if (check.outcome != UpdateOutcome::Available) return fail("there is no update to adopt");
    const std::string sha = sha256Bytes(check.manifestBytes.data(), check.manifestBytes.size());
    // The bytes are verified again so that only a manifest that verifies is ever stored.
    Manifest verified;
    if (!loadVerifiedManifest(check.manifestBytes, check.signatureText, keys(), &verified, error)) return false;

    PayloadCache cache(settings_.cacheDir);
    if (!cache.hasAll(verified)) return fail("the payload cache does not hold every file of this release yet");

    const std::wstring dir = joinPath(settings_.launcherDir, L"manifests");
    if (!createDirectories(dir)) return fail("the manifests folder cannot be created");
    const std::wstring base = joinPath(dir, widenUtf8(sha));
    if (!writeFile(base + L".sig", check.signatureText) || !writeFile(base + L".json", check.manifestBytes))
        return fail("the manifest could not be stored");

    LauncherState state;
    if (!state.load(statePath(), error)) return false;
    state.setCurrentManifest(sha);
    state.raiseHighestIssued(verified.issued);
    if (!state.save(error)) return false;

    // Keep the current manifest and the newest others, up to three; the files of
    // a kept manifest are what rolling back needs offline.
    std::vector<Manifest> keep;
    keep.push_back(verified);
    size_t kept = 1;
    for (const StoredManifest& stored : listStored(dir)) {
        if (stored.sha == sha) continue;
        const std::wstring storedBase = joinPath(dir, widenUtf8(stored.sha));
        bool keepIt = false;
        if (kept < kKeptManifests) {
            std::string manifestBytes, signatureText;
            Manifest other;
            if (readFileBytes(storedBase + L".json", &manifestBytes, kMaxManifestBytes, nullptr) &&
                readFileBytes(storedBase + L".sig", &signatureText, 1024, nullptr) &&
                sha256Bytes(manifestBytes.data(), manifestBytes.size()) == stored.sha &&
                loadVerifiedManifest(manifestBytes, signatureText, keys(), &other, nullptr)) {
                keep.push_back(std::move(other));
                keepIt = true;
                ++kept;
            }
        }
        if (!keepIt) {
            DeleteFileW((storedBase + L".json").c_str());
            DeleteFileW((storedBase + L".sig").c_str());
        }
    }
    cache.prune(keep);
    return true;
}

bool Updater::loadCurrentManifest(Manifest* manifest, std::vector<PayloadFile>* files, std::string* error) {
    auto fail = [&](const std::string& text) {
        if (error) *error = text;
        return false;
    };
    LauncherState state;
    if (!state.load(statePath(), error)) return false;
    const std::string sha = state.currentManifest();
    if (sha.empty()) return fail("no update has been adopted yet");
    return loadManifestFile(joinPath(joinPath(settings_.launcherDir, L"manifests"), widenUtf8(sha)) + L".json",
                            manifest, files, error);
}

bool Updater::loadManifestFile(const std::wstring& jsonPath, Manifest* manifest, std::vector<PayloadFile>* files,
                               std::string* error) {
    auto fail = [&](const std::string& text) {
        if (error) *error = text;
        return false;
    };
    const size_t slash = jsonPath.find_last_of(L'\\');
    const std::string leaf = narrowUtf8(slash == std::wstring::npos ? jsonPath : jsonPath.substr(slash + 1));
    if (leaf.size() != 64 + 5 || leaf.compare(64, 5, ".json") != 0 || !isLowerHex(leaf.substr(0, 64), 64))
        return fail("the manifest's file name is not its hash");
    const std::string sha = leaf.substr(0, 64);
    const std::wstring base = jsonPath.substr(0, jsonPath.size() - 5);
    std::string bytes, signatureText;
    if (!readFileBytes(base + L".json", &bytes, kMaxManifestBytes, nullptr) ||
        !readFileBytes(base + L".sig", &signatureText, 1024, nullptr))
        return fail("the stored manifest cannot be read");
    if (sha256Bytes(bytes.data(), bytes.size()) != sha) return fail("the stored manifest has changed since it was adopted");
    Manifest loaded;
    if (!loadVerifiedManifest(bytes, signatureText, keys(), &loaded, error)) return false;

    PayloadCache cache(settings_.cacheDir);
    std::vector<PayloadFile> list;
    for (const ManifestFile& f : loaded.files) {
        if (!cache.hasSized(f.sha256, f.size)) return fail("the payload cache lacks " + f.path + "; connect and update to repair it");
        PayloadFile p;
        p.item = stage1PayloadItem(f.id);
        if (p.item.empty()) return fail("this launcher cannot place the file " + f.id + " yet");
        p.path = cache.pathFor(f.sha256);
        p.sha256 = f.sha256;
        list.push_back(std::move(p));
    }
    *manifest = std::move(loaded);
    *files = std::move(list);
    return true;
}

}  // namespace edvr::installer
