// The files this installer carries, embedded in its own executable, or the files a launcher
// reads from its cache.
//
// One .exe and nothing else: a zip that has to be extracted first is where
// most manual installs go wrong (Explorer will happily run a program from
// INSIDE a zip, in a temporary folder, with none of its siblings) and it is
// the step people skip. The payload is linked in as RCDATA by build.bat, which
// generates the .rc (tools\gen_installer_rc.py) from package_native.py's one table of files.
//
// A payload is the parsed schema-2 manifest (install_manifest.h) plus the bytes of each of its
// files: the same shape whether the executable carries it or a launcher's cache does
// (docs\design-manifest-schema2-2026-11-12.md, "One engine"). The planner reads PayloadInfo and
// the applier asks payloadItem for a file's bytes by its manifest id.
#pragma once

#include <string>
#include <vector>

#include "install_manifest.h"
#include "plan.h"

namespace edvr::installer {

// Resource ids, matched by the generated payload.rc: the manifest, then one RCDATA per file of its
// `files` list at IDR_EDVR_FILE_BASE + its index. (Resource 120, the launcher's licence, is not a payload file.)
#define IDR_EDVR_MANIFEST 100
#define IDR_EDVR_FILE_BASE 101

// The bytes of payload file `id` (a manifest file id), for applyPlan's provider.
bool payloadItem(const std::string& id, const void** data, size_t* size);

// The manifest's files that have bytes, with their hashes and validity. Computed on first use, and again
// after usePayloadFiles or resetPayloadFiles: the reference stays valid, but what it holds changes, so a
// caller keeps a copy rather than the reference across such a call.
const PayloadInfo& payloadInfo();

// One file of a payload that lives outside the executable: the manifest id, where the file is, and its SHA-256
// when the caller has one to check it against.
struct PayloadFile {
    std::string id;
    std::wstring path;
    std::string sha256;  // lower-case hex, or empty to skip the check
};

// Switches payloadItem and payloadInfo from the embedded resources to `manifest` and these files,
// for the rest of the process. Each file is mapped read-only and kept mapped, so a
// pointer payloadItem returned stays valid even after a later call replaces the
// payload; a file whose hash does not match is refused, and so is an id the manifest
// does not list or a repeated one. Nothing changes unless every file is accepted. A manifest file with
// no PayloadFile has no bytes: payloadItem finds nothing for it, as for a folder unpacked from the core zip alone.
// A file given a hash is hashed here, once: payloadInfo reuses that value rather than
// reading the same bytes again, and Updater::loadCurrentManifest leaves the hashing to
// this call, so a launcher reads its payload once at start-up, not three times.
// The payload's version is the manifest's build.
// Call it from the thread that owns the payload; it does not synchronise with a
// reader running at the same time.
bool usePayloadFiles(const Manifest& manifest, const std::vector<PayloadFile>& files, std::string* error);

// Back to the embedded resources, as if usePayloadFiles had never been called.
void resetPayloadFiles();

}  // namespace edvr::installer
