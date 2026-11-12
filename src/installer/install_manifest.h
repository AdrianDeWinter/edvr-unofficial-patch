// The schema-2 manifest: what a release carries and where each file goes
// (docs\design-manifest-schema2-2026-11-12.md).
//
// One parser for the installer and the launcher. It reads an already parsed JSON
// object; the signature, the size cap and the ASCII check belong to
// update_manifest.cpp, which calls it. The rules here are the ones of
// tools\sign_manifest.py's validate_manifest, rule for rule: tools\launcher_test
// feeds both the same fixtures and fails when they disagree.
#pragma once

#include <string>
#include <vector>

#include "update_json.h"

namespace edvr::installer {

// The one schema this build reads. tools\sign_manifest.py's MANIFEST_SCHEMA is
// this number; its self-test reads this line as text and compares.
inline constexpr int kManifestSchema = 2;

// The two roots a file's target path is under.
enum class TargetRoot { Game, Openvr };

// How a file already at the target is treated.
enum class ExistingMode { Replace, Owned, Chain, Runtime, Settings };

// The profiles an install covers, as bits: a `when.profile` list is their union.
inline constexpr unsigned kProfileVr = 1;
inline constexpr unsigned kProfileFlat = 2;
inline constexpr unsigned kProfileAll = kProfileVr | kProfileFlat;

struct ManifestWhen {
    unsigned profiles = 0;       // 0: no profile condition; otherwise kProfileVr and/or kProfileFlat
    bool adapterNvidia = false;  // the survey must have found an NVIDIA adapter
    bool present = false;        // the manifest carried a `when` object at all
};

struct ManifestTarget {
    TargetRoot root = TargetRoot::Game;
    std::string path;  // relative to the root, '/' separated, already checked by isTargetPath
};

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
    std::string id;     // [a-z0-9_]{1,32}, unique, stable across releases
    std::string asset;  // the ManifestAsset.name of the zip that holds it
    std::string path;   // the file's path inside its zip and inside an unpacked payload folder
    std::string sha256;
    unsigned long long size = 0;
    ManifestTarget target;
    ManifestWhen when;
    ExistingMode existing = ExistingMode::Replace;
};

struct Manifest {
    std::string version;
    std::string build;
    std::string tag;
    long long issued = 0;
    bool hasLauncher = false;
    ManifestLauncher launcher;
    std::vector<ManifestAsset> assets;
    std::vector<ManifestFile> files;
};

enum class ManifestParse {
    Ok,
    Malformed,    // `error` says which rule broke
    SchemaTooNew  // `schema` is above kManifestSchema; nothing else was looked at
};

// Reads `schema` on its own first, before any shape check: above kManifestSchema
// is SchemaTooNew (with the number in `*schemaSeen`, which may be null); below it,
// missing, or not an integer is Malformed. Then the whole schema-2 shape and every
// cross-file rule. On anything but Ok `out` is untouched and `error` (may be null)
// is a sentence; a rule about one file names its id.
ManifestParse parseManifest(const JsonValue& root, Manifest* out, std::string* error,
                            long long* schemaSeen = nullptr);

// The path rule for a target path (docs, "Schema 2"): refuses a backslash, a colon
// (drives and alternate streams), a control or non-ASCII character, an empty, "."
// or ".." segment, a segment ending in a dot or a space, a character Windows bars
// from a name (< > " | ? *), and a reserved device name (CON, PRN, AUX, NUL,
// COM0-9, LPT0-9, any case, with or without an extension) as a segment's stem.
// On false `why` (may be null) says which.
bool isTargetPath(const std::string& path, std::string* why);

// Whether a `when` holds for an install covering `installedProfiles` (kProfile*
// bits) on a machine where `nvidiaAdapter` says whether the survey found one.
bool whenHolds(const ManifestWhen& when, unsigned installedProfiles, bool nvidiaAdapter);

// Whether two `when` conditions can both hold on some install. A missing profile
// condition holds for every profile; two profile lists must share a value. The
// adapter never excludes: a machine may have an NVIDIA adapter.
bool whensCanOverlap(const ManifestWhen& a, const ManifestWhen& b);

const char* targetRootName(TargetRoot root);
const char* existingName(ExistingMode mode);

}  // namespace edvr::installer
