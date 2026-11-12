// The release manifest: its schema, its signature and its version rules.
//
// The manifest is ASCII JSON, signed as its exact bytes with P-256 ECDSA over
// the SHA-256 of the file (docs\design-launcher-2026-11-07.md, "The manifest"
// and "Signing and the key"). loadVerifiedManifest checks the signature FIRST
// and parses only bytes that passed, so nothing an attacker controls reaches
// the parser. The schema rules here are the ones of tools\sign_manifest.py's
// validate_manifest, rule for rule: tools\launcher_test feeds both the same
// fixtures and fails when they disagree.
#pragma once

#include <string>
#include <vector>

namespace edvr::installer {

struct ManifestLauncher {
    std::string version;
    std::string asset;
    std::string sha256;
    unsigned long long size = 0;
};

struct ManifestAsset {
    std::string name;
    std::string sha256;
    unsigned long long size = 0;
};

struct ManifestFile {
    std::string item;   // a name payloadItem() knows, one of kManifestItems
    std::string path;   // the file's path inside its zip and inside an unpacked payload folder
    std::string asset;  // the ManifestAsset.name of the zip that holds it
    std::string sha256;
    unsigned long long size = 0;
};

struct Manifest {
    std::string version;
    std::string build;
    std::string tag;
    long long issued = 0;
    std::string minLauncher;
    bool hasLauncher = false;
    ManifestLauncher launcher;
    std::vector<ManifestAsset> assets;
    std::vector<ManifestFile> files;
};

// The item names a manifest may use; the same list as ITEMS in sign_manifest.py.
inline constexpr const char* kManifestItems[] = {
    "native_graphics", "native_runtime", "ini", "flat_ini", "profile", "ngx",
    "xess", "xess_fg", "xess_ll", "xess_license", "xess_third_party", "openxr_loader", "openxr_license"};

inline constexpr size_t kMaxManifestBytes = 1u << 20;

// `keys` is a nullptr-terminated list of P-256 public keys, each 128 hex
// characters (x then y); production passes kUpdatePublicKeys from
// update_keys.h. `sigText` is exactly 128 lower-case hex characters, then at
// most one line ending. The signature must verify under any listed key; a key
// that is not a valid point is skipped. On failure `error` (may be null) says
// which check failed and `out` is untouched.
bool loadVerifiedManifest(const std::string& bytes, const std::string& sigText,
                          const char* const* keys, Manifest* out, std::string* error);

// Numeric major.minor.patch comparison: negative when a < b, zero when equal,
// positive when a > b. A leading "v" is ignored, a missing component is 0, and
// a prerelease ("1.2.0-rc.7") sorts below its release and above every earlier
// version.
int compareVersions(const std::string& a, const std::string& b);

}  // namespace edvr::installer
