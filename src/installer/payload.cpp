#include "payload.h"

#include <windows.h>

#include <map>
#include <mutex>

#include "probe.h"

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

int idFor(const std::string& item) {
    if (item == "d3d11") return IDR_EDVR_D3D11;
    if (item == "openvr") return IDR_EDVR_OPENVR;
    if (item == "native_graphics") return IDR_EDVR_D3D11;
    if (item == "native_runtime") return IDR_EDVR_OPENVR;
    if (item == "ini") return IDR_EDVR_INI;
    if (item == "ngx") return IDR_EDVR_NGX;
    if (item == "xess") return IDR_EDVR_XESS;
    if (item == "xess_license") return IDR_EDVR_XESS_LICENSE;
    if (item == "xess_third_party") return IDR_EDVR_XESS_THIRD_PARTY;
    if (item == "openxr_loader") return IDR_EDVR_OPENXR_LOADER;
    if (item == "openxr_license") return IDR_EDVR_OPENXR_LICENSE;
    if (item == "profile") return IDR_EDVR_PROFILE;
    if (item == "flat_ini") return IDR_EDVR_FLAT_INI;
    return 0;
}

// The name an item is kept under when the payload is outside the executable:
// the two historical aliases fold into the native names.
std::string canonicalItem(const std::string& item) {
    if (item == "d3d11") return "native_graphics";
    if (item == "openvr") return "native_runtime";
    return idFor(item) != 0 ? item : std::string();
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
std::string g_version;
std::map<std::string, Mapped> g_files;
std::vector<Mapped> g_retired;
PayloadInfo g_info;
bool g_infoValid = false;

void retireFiles() {
    for (auto& entry : g_files) g_retired.push_back(entry.second);
    g_files.clear();
}

// payloadItem without the lock, for code that already holds it.
bool lookup(const std::string& item, const void** data, size_t* size) {
    if (g_external) {
        const std::string name = canonicalItem(item);
        const auto found = g_files.find(name);
        if (name.empty() || found == g_files.end()) return false;
        *data = found->second.view;
        *size = found->second.size;
        return true;
    }
    const int id = idFor(item);
    if (id == 0) return false;
    return findBlob(id, data, size);
}

// The SHA-256 of what lookup() returned for `item`. A file outside the executable that
// usePayloadFiles already verified against the caller's hash keeps that hash, so the
// launcher reads each of its ~200 MB of files once, not once more per payloadInfo.
std::string shaOfItem(const std::string& item, const void* data, size_t size) {
    if (g_external) {
        const auto found = g_files.find(canonicalItem(item));
        if (found != g_files.end() && !found->second.sha.empty()) return found->second.sha;
    }
    return sha256Bytes(data, size);
}

PayloadInfo buildInfo() {
    PayloadInfo p;
    p.version = g_external ? g_version : std::string(EDVR_VERSION_STRING);

    const void* data = nullptr;
    size_t size = 0;
    if (lookup("d3d11", &data, &size)) {
        p.haveD3d11 = true;
        p.d3d11Sha = shaOfItem("d3d11", data, size);
    }
    if (lookup("openvr", &data, &size)) {
        p.haveOpenvr = true;
        p.openvrSha = shaOfItem("openvr", data, size);
    }
    if (lookup("ngx", &data, &size)) {
        p.haveNgx = true;
        p.ngxSha = shaOfItem("ngx", data, size);
    }
    if (lookup("xess", &data, &size)) {
        p.haveXess = true;
        p.xessSha = shaOfItem("xess", data, size);
    }
    if (lookup("xess_license", &data, &size)) p.xessLicenseSha = shaOfItem("xess_license", data, size);
    if (lookup("xess_third_party", &data, &size)) p.xessThirdPartySha = shaOfItem("xess_third_party", data, size);
    const bool graphicsPresent = lookup("native_graphics", &data, &size);
    const bool graphics = graphicsPresent && validateNativePayloadBytes(data, size, NativeImageKind::Graphics);
    p.nativeGraphicsValid = graphics;
    const bool runtimePresent = lookup("native_runtime", &data, &size);
    const bool runtime = runtimePresent && validateNativePayloadBytes(data, size, NativeImageKind::Runtime);
    const bool loaderPresent = lookup("openxr_loader", &data, &size);
    if (loaderPresent) { p.haveOpenxrLoader = true; p.openxrLoaderSha = shaOfItem("openxr_loader", data, size); }
    const bool loader = loaderPresent && validateNativePayloadBytes(data, size, NativeImageKind::Loader);
    const bool license = lookup("openxr_license", &data, &size);
    p.nativePairValid = graphics && runtime && loader && license;
    if (license) { p.haveOpenxrLicense = true; p.openxrLicenseSha = shaOfItem("openxr_license", data, size); }
    // Resource 103 is edvr.ini; resource 111 is edvr-flat.ini. Both are carried, and the
    // scope the install asks for decides which of them it writes.
    if (lookup("ini", &data, &size)) {
        p.haveIni = true;
        p.iniText.assign(static_cast<const char*>(data), size);
    }
    if (lookup("flat_ini", &data, &size)) {
        p.haveFlatIni = true;
        p.flatIniText.assign(static_cast<const char*>(data), size);
    }
    if (lookup("profile", &data, &size)) {
        p.descriptorText.assign(static_cast<const char*>(data), size);
        p.descriptorSha = shaOfItem("profile", data, size);
    }
    return p;
}

}  // namespace

// TEMPORARY, removed in stage 2: schema-2 file id -> the item name idFor knows.
std::string stage1PayloadItem(const std::string& fileId) {
    static const struct { const char* id; const char* item; } kBridge[] = {
        {"graphics", "native_graphics"},   {"runtime", "native_runtime"},
        {"openxr_loader", "openxr_loader"}, {"openxr_license", "openxr_license"},
        {"settings_vr", "ini"},            {"settings_flat", "flat_ini"},
        {"dlss", "ngx"},                   {"xess", "xess"},
        {"xess_license", "xess_license"},  {"xess_third_party", "xess_third_party"},
    };
    for (const auto& entry : kBridge) {
        if (fileId == entry.id) return entry.item;
    }
    return std::string();
}

bool payloadItem(const std::string& item, const void** data, size_t* size) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return lookup(item, data, size);
}

const PayloadInfo& payloadInfo() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_infoValid) {
        g_info = buildInfo();
        g_infoValid = true;
    }
    return g_info;
}

bool usePayloadFiles(const std::vector<PayloadFile>& files, const std::string& version, std::string* error) {
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
        const std::string name = canonicalItem(file.item);
        if (name.empty()) {
            releaseFresh();
            return refuse("the payload names an unknown item:" + file.item);
        }
        if (fresh.find(name) != fresh.end()) {
            releaseFresh();
            return refuse("the payload lists the item twice: " + file.item);
        }
        Mapped mapped;
        if (!mapFile(file.path, &mapped)) {
            releaseFresh();
            return refuse("the payload file for " + file.item + " cannot be opened");
        }
        if (!file.sha256.empty() && sha256Bytes(mapped.view, mapped.size) != file.sha256) {
            unmap(&mapped);
            releaseFresh();
            return refuse("the payload file for " + file.item + " does not match its hash");
        }
        mapped.sha = file.sha256;  // empty when unchecked; non-empty only after the comparison above
        fresh[name] = mapped;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    retireFiles();
    g_files = std::move(fresh);
    g_external = true;
    g_version = version;
    g_infoValid = false;
    return true;
}

void resetPayloadFiles() {
    std::lock_guard<std::mutex> lock(g_mutex);
    retireFiles();
    g_external = false;
    g_version.clear();
    g_infoValid = false;
}

}  // namespace edvr::installer
