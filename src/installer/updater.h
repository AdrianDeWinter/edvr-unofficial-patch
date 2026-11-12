// Finding, fetching and adopting an update. Logic only: no window, and every
// request goes through an HttpClient, so tools\launcher_test can script GitHub.
//
// The rules (docs\design-launcher-2026-11-07.md, "Updates"):
//   * Nothing here ever blocks a launch. A transport failure, a 403, a 429 or a
//     5xx is Skipped, with a sentence for the log, and the next launch tries again.
//   * A release is offered only if it carries a signed manifest that verifies
//     under a key built into the exe. A release that fails any check is Refused,
//     is not offered, and does not fall back to an older release.
//   * Digests, not tags, decide "up to date": a release re-cut under the same
//     tag has a different manifest digest.
//   * `issued` only goes up: a verified manifest older than the highest one
//     already accepted is ignored, so a hijacked account cannot roll users back.
//   * Nothing is downloaded but the manifest and its signature until the caller
//     asks for downloadUpdate, and nothing about the install changes until
//     commitManifest.
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "launcher_version.h"
#include "payload.h"
#include "update_cache.h"
#include "update_http.h"
#include "update_manifest.h"

namespace edvr::installer {

// The one place the project's address lives.
inline constexpr const char* kGitHubApiBase =
    "https://api.github.com/repos/characterecho-sean/ed-unofficial-patch";
inline constexpr const char* kManifestAssetName = "ed-up-manifest.json";
inline constexpr const char* kSignatureAssetName = "ed-up-manifest.json.sig";

// What the window and --check say when a release's manifest is of a newer schema than this
// launcher reads. The detail ("manifest schema N, launcher reads 2") goes in UpdateCheck::detail.
inline constexpr const char* kSchemaTooNewMessage =
    "This update needs a newer ED-UP launcher. Download the latest launcher from the releases page.";

enum class UpdateOutcome {
    UpToDate,   // nothing newer; `message` may say why
    Available,  // a verified, newer manifest is in UpdateCheck
    Refused,    // a release was found and failed a check: not offered
    Skipped,    // the check could not be made; try again next launch
};

struct ReleaseAsset {
    std::string name;
    std::string digest;  // lower-case hex of the API's "sha256:<hex>", or empty when absent or odd
    std::string url;     // browser_download_url
    unsigned long long size = 0;
};

struct UpdateCheck {
    UpdateOutcome outcome = UpdateOutcome::Skipped;
    std::string message;  // one sentence, for the window's quiet line and the log
    std::string detail;   // for the log only: why a Refused outcome was refused, when message is a fixed line
    std::string tag;
    Manifest manifest;
    std::string manifestBytes;
    std::string signatureText;
    std::string manifestSha256;
    std::vector<ReleaseAsset> assets;
    bool launcherNeedsUpdate = false;  // the manifest's launcher differs from ours and is not older
    bool signatureVerified = false;
};

struct UpdaterSettings {
    std::wstring launcherDir;  // launcher.ini, manifests\ and the API reply cache
    std::wstring cacheDir;     // the payload cache
    std::string channel;       // "stable" or "rc"; empty reads launcher.ini
    std::string apiBase = kGitHubApiBase;
    const char* const* keys = nullptr;  // null means kUpdatePublicKeys
    std::string launcherVersion = EDUP_LAUNCHER_VERSION;
    std::string launcherSha256;  // of the running exe; empty when unknown
};

using DownloadProgress = std::function<void(const std::string& assetName, unsigned long long received,
                                            unsigned long long total)>;

class Updater {
public:
    Updater(HttpClient& http, const UpdaterSettings& settings) : http_(http), settings_(settings) {}

    // Asks GitHub and decides. Writes only the API reply cache in the launcher folder.
    UpdateCheck check();

    // Fills the payload cache for `check.manifest`, fetching only assets that hold a
    // file the cache lacks. Stops at `cancel`.
    bool downloadUpdate(const UpdateCheck& check, const std::atomic<bool>* cancel, const DownloadProgress& progress,
                        std::string* error);

    // Adopts a checked manifest whose files are all in the cache: saves it as
    // manifests\<sha>.json and .sig, makes it current, raises highest_issued, keeps
    // the newest three manifests and prunes the cache to what they list.
    bool commitManifest(const UpdateCheck& check, std::string* error);

    // Re-verifies the current manifest from disk and lists its files in the cache
    // as payload files, ready for usePayloadFiles. Works offline. The manifest's
    // signature is checked here; each cached file is only checked for existing and
    // having its listed size, because the PayloadFile carries its hash and
    // usePayloadFiles, which reads the whole file anyway, is where it is compared. A
    // caller that lists files and never calls usePayloadFiles has verified nothing.
    bool loadCurrentManifest(Manifest* manifest, std::vector<PayloadFile>* files, std::string* error);

    // The same, for one stored manifest by path (<launcher>\manifests\<sha>.json, with
    // its .sig beside it) instead of the one launcher.ini names. The file's name must
    // be the SHA-256 of its bytes. This is what the elevated copy uses: it is handed a
    // path, trusts nothing about it, and checks the signature itself.
    bool loadManifestFile(const std::wstring& jsonPath, Manifest* manifest, std::vector<PayloadFile>* files,
                          std::string* error);

private:
    std::wstring statePath() const;
    const char* const* keys() const;

    HttpClient& http_;
    UpdaterSettings settings_;
};

}  // namespace edvr::installer
