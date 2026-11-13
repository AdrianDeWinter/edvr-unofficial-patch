#include "payload.h"

#include <windows.h>

#include <map>
#include <mutex>

#include "probe.h"
#include "update_json.h"

#ifndef EDVR_VERSION_STRING
#define EDVR_VERSION_STRING "unknown"
#endif

namespace edvr::installer {
namespace {

bool findBlob(int id, const void** data, size_t* size) {
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!res) return false;
    const DWORD bytes = SizeofResource(nullptr, res);
    HGLOBAL handle = LoadResource(nullptr, res);
    if (!handle || bytes == 0) return false;
    const void* p = LockResource(handle);
    if (!p) return false;
    *data = p;
    *size = bytes;
    return true;
}

// One payload file, mapped read-only.
struct Mapped {
    HANDLE file = INVALID_HANDLE_VALUE;
    HANDLE mapping = nullptr;
    const void* view = nullptr;
    size_t size = 0;
    // The hash usePayloadFiles checked this file against, or empty when the caller gave
    // none. buildInfo reuses it instead of hashing the same bytes again.
    std::string sha;
};

void unmap(Mapped* m) {
    if (m->view) UnmapViewOfFile(m->view);
    if (m->mapping) CloseHandle(m->mapping);
    if (m->file != INVALID_HANDLE_VALUE) CloseHandle(m->file);
    *m = Mapped();
}

bool mapFile(const std::wstring& path, Mapped* out) {
    Mapped m;
    m.file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
    if (m.file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(m.file, &size) || size.QuadPart <= 0 || size.QuadPart > (1ll << 30)) {
        unmap(&m);
        return false;
    }
    m.mapping = CreateFileMappingW(m.file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!m.mapping) {
        unmap(&m);
        return false;
    }
    m.view = MapViewOfFile(m.mapping, FILE_MAP_READ, 0, 0, 0);
    if (!m.view) {
        unmap(&m);
        return false;
    }
    m.size = static_cast<size_t>(size.QuadPart);
    *out = m;
    return true;
}

// The payload while it is outside the executable. Replaced mappings are retired,
// not released: payloadItem hands out raw pointers into them, and applyPlan may
// still hold one. A launcher switches payload a handful of times at most.
std::mutex g_mutex;
bool g_external = false;
Manifest g_manifest;                  // the external payload's manifest
std::string g_version;
std::map<std::string, Mapped> g_files;  // by manifest file id
std::vector<Mapped> g_retired;
PayloadInfo g_info;
bool g_infoValid = false;

// The manifest the executable carries as RCDATA 100: empty when it carries none (the launcher, a test rig) or when
// the resource is not a manifest this build reads, either of which reads as "no payload".
bool g_embeddedRead = false;
Manifest g_embedded;

const Manifest& embeddedManifest() {
    if (g_embeddedRead) return g_embedded;
    g_embeddedRead = true;
    const void* data = nullptr;
    size_t size = 0;
    if (!findBlob(IDR_EDVR_MANIFEST, &data, &size)) return g_embedded;
    JsonValue root;
    std::string error;
    Manifest parsed;
    if (parseJson(std::string(static_cast<const char*>(data), size), &root, &error) &&
        parseManifest(root, &parsed, &error) == ManifestParse::Ok)
        g_embedded = std::move(parsed);
    return g_embedded;
}

const Manifest& activeManifest() { return g_external ? g_manifest : embeddedManifest(); }

// The bytes of file `index` of the active manifest, without the lock, for code that already holds it.
bool lookupIndex(size_t index, const void** data, size_t* size) {
    const Manifest& manifest = activeManifest();
    if (index >= manifest.files.size()) return false;
    if (g_external) {
        const auto found = g_files.find(manifest.files[index].id);
        if (found == g_files.end()) return false;
        *data = found->second.view;
        *size = found->second.size;
        return true;
    }
    return findBlob(IDR_EDVR_FILE_BASE + static_cast<int>(index), data, size);
}

bool lookup(const std::string& id, const void** data, size_t* size) {
    const Manifest& manifest = activeManifest();
    for (size_t i = 0; i < manifest.files.size(); ++i) {
        if (manifest.files[i].id == id) return lookupIndex(i, data, size);
    }
    return false;
}

// The SHA-256 of what lookupIndex() returned. A file outside the executable that
// usePayloadFiles already verified against the caller's hash keeps that hash, so the
// launcher reads each of its ~200 MB of files once, not once more per payloadInfo.
std::string shaOfIndex(size_t index, const void* data, size_t size) {
    if (g_external) {
        const auto found = g_files.find(activeManifest().files[index].id);
        if (found != g_files.end() && !found->second.sha.empty()) return found->second.sha;
    }
    return sha256Bytes(data, size);
}

PayloadInfo buildInfo() {
    PayloadInfo p;
    p.version = g_external ? g_version : std::string(EDVR_VERSION_STRING);
    const Manifest& manifest = activeManifest();
    for (size_t i = 0; i < manifest.files.size(); ++i) {
        const ManifestFile& file = manifest.files[i];
        const void* data = nullptr;
        size_t size = 0;
        if (!lookupIndex(i, &data, &size)) continue;
        PayloadFileEntry entry;
        entry.file = file;
        entry.data = data;
        entry.size = size;
        entry.sha = shaOfIndex(i, data, size);
        // The executable's own bytes are the ones its manifest was written from; a file that is not is not carried.
        if (!g_external && entry.sha != file.sha256) continue;
        // The three native images are checked for what they are. The loader is found by its id: nothing in its manifest entry
        // says it is the Khronos loader, and the check is of that file's own exports.
        if (file.existing == ExistingMode::Chain)
            entry.valid = validateNativePayloadBytes(data, size, NativeImageKind::Graphics);
        else if (file.existing == ExistingMode::Runtime)
            entry.valid = validateNativePayloadBytes(data, size, NativeImageKind::Runtime);
        else if (file.id == "openxr_loader")
            entry.valid = validateNativePayloadBytes(data, size, NativeImageKind::Loader);
        p.files.push_back(std::move(entry));
    }
    return p;
}

void retireFiles() {
    for (auto& entry : g_files) g_retired.push_back(entry.second);
    g_files.clear();
}

}  // namespace

bool payloadItem(const std::string& id, const void** data, size_t* size) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return lookup(id, data, size);
}

const PayloadInfo& payloadInfo() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_infoValid) {
        g_info = buildInfo();
        g_infoValid = true;
    }
    return g_info;
}

bool usePayloadFiles(const Manifest& manifest, const std::vector<PayloadFile>& files, std::string* error) {
    auto refuse = [&](const std::string& text) {
        if (error) *error = text;
        return false;
    };
    std::map<std::string, Mapped> fresh;
    auto releaseFresh = [&]() {
        for (auto& entry : fresh) unmap(&entry.second);
        fresh.clear();
    };
    for (const PayloadFile& file : files) {
        bool listed = false;
        for (const ManifestFile& f : manifest.files) listed |= f.id == file.id;
        if (!listed) {
            releaseFresh();
            return refuse("the payload names an unknown file: " + file.id);
        }
        if (fresh.find(file.id) != fresh.end()) {
            releaseFresh();
            return refuse("the payload lists the file twice: " + file.id);
        }
        Mapped mapped;
        if (!mapFile(file.path, &mapped)) {
            releaseFresh();
            return refuse("the payload file for " + file.id + " cannot be opened");
        }
        if (!file.sha256.empty() && sha256Bytes(mapped.view, mapped.size) != file.sha256) {
            unmap(&mapped);
            releaseFresh();
            return refuse("the payload file for " + file.id + " does not match its hash");
        }
        mapped.sha = file.sha256;  // empty when unchecked; non-empty only after the comparison above
        fresh[file.id] = mapped;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    retireFiles();
    g_files = std::move(fresh);
    g_external = true;
    g_manifest = manifest;
    g_version = manifest.build;
    g_infoValid = false;
    return true;
}

void resetPayloadFiles() {
    std::lock_guard<std::mutex> lock(g_mutex);
    retireFiles();
    g_external = false;
    g_manifest = Manifest();
    g_version.clear();
    g_infoValid = false;
}

}  // namespace edvr::installer
