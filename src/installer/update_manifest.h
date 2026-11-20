// The release manifest: its schema, its signature and its version rules.
//
// The manifest is ASCII JSON, signed as its exact bytes with P-256 ECDSA over
// the SHA-256 of the file (docs\design-launcher-2026-11-07.md, "The manifest"
// and "Signing and the key"). loadVerifiedManifest checks the signature FIRST
// and parses only bytes that passed, so nothing an attacker controls reaches
// the parser. The schema itself, shared with the installer, is
// install_manifest.h (docs\design-manifest-schema2-2026-11-12.md); its rules are
// the ones of tools\sign_manifest.py's validate_manifest, rule for rule:
// tools\launcher_test feeds both the same fixtures and fails when they disagree.
#pragma once

#include <string>
#include <vector>

#include "install_manifest.h"

namespace edvr::installer {

inline constexpr size_t kMaxManifestBytes = 1u << 20;

// `keys` is a nullptr-terminated list of P-256 public keys, each 128 hex
// characters (x then y); production passes kUpdatePublicKeys from
// update_keys.h. `sigText` is exactly 128 lower-case hex characters, then at
// most one line ending. The signature must verify under any listed key; a key
// that is not a valid point is skipped. On failure `error` (may be null) says
// which check failed and `out` is untouched. A manifest of a schema newer than
// kManifestSchema is refused with `failure->schemaTooNew` set and the number in
// `failure->schema` (`failure` may be null); `error` then reads "manifest schema
// N, launcher reads 2".
struct ManifestFailure {
    bool schemaTooNew = false;
    long long schema = 0;
};
bool loadVerifiedManifest(const std::string& bytes, const std::string& sigText,
                          const char* const* keys, Manifest* out, std::string* error,
                          ManifestFailure* failure = nullptr);

// Numeric major.minor.patch comparison: negative when a < b, zero when equal,
// positive when a > b. A leading "v" is ignored, a missing component is 0, and
// a prerelease ("1.2.0-rc.7") sorts below its release and above every earlier
// version.
int compareVersions(const std::string& a, const std::string& b);

}  // namespace edvr::installer
