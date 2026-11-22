#include "vr_headset.h"

#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cwctype>
#include <memory>
#include <thread>

#include "probe.h"
#include "update_json.h"

namespace edvr::installer {
namespace {

std::wstring toLowerW(std::wstring s) {
    for (wchar_t& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

std::string toLowerA(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

bool containsW(const std::wstring& haystack, const wchar_t* needle) {
    return haystack.find(needle) != std::wstring::npos;
}

std::wstring widenUtf8(const std::string& text) {
    if (text.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), &out[0], n);
    return out;
}

std::string narrowUtf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int n =
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), &out[0], n, nullptr, nullptr);
    return out;
}

bool isKnownVrServiceProcess(const std::string& lowerProc) {
    static const char* const kKnown[] = {
        "vrserver.exe", "vrmonitor.exe", "vrcompositor.exe", "vrserver", "vrmonitor",
        "ovrserver_x64.exe", "oculusclient.exe", "metaquestlink.exe",
        "virtualdesktop.streamer.exe", "virtualdesktop.service.exe",
        "alvr_dashboard.exe", "alvr_launcher.exe", "alvr_dashboard", "alvr_launcher",
        "pi_server.exe", "pimaxclient.exe", "pimaxplay.exe", "piservice.exe", "piservice_launcher.exe",
        "mixedrealityportal.exe", "holoshellapp.exe",
        "varjobase.exe", "varjoserver.exe",
        "viveconsole.exe", "vivehub.exe", "viveruntime.exe",
        "wivrn-server", "monado-service", "envision",
    };
    for (const char* name : kKnown) {
        if (lowerProc == name) return true;
    }
    return false;
}

std::wstring trimW(const std::wstring& s) {
    size_t a = 0;
    while (a < s.size() && std::iswspace(s[a])) ++a;
    size_t b = s.size();
    while (b > a && std::iswspace(s[b - 1])) --b;
    return s.substr(a, b - a);
}

std::string trimA(const std::string& s) {
    size_t a = 0;
    while (a < s.size() && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    size_t b = s.size();
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    return s.substr(a, b - a);
}

bool pathExistsW(const std::wstring& path) {
    if (path.empty()) return false;
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::wstring dirOfPath(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

std::wstring stemOfPath(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    std::wstring base = slash == std::wstring::npos ? path : path.substr(slash + 1);
    const size_t dot = base.find_last_of(L'.');
    if (dot != std::wstring::npos && dot > 0) base.resize(dot);
    return base;
}

std::wstring joinW(const std::wstring& base, const std::wstring& leaf) {
    if (base.empty()) return leaf;
    if (leaf.empty()) return base;
    if (base.back() == L'\\' || base.back() == L'/') return base + leaf;
    return base + L'\\' + leaf;
}

bool isAbsolutePathW(const std::wstring& p) {
    if (p.size() >= 2 && p[1] == L':') return true;
    if (!p.empty() && (p[0] == L'\\' || p[0] == L'/')) return true;
    return false;
}

std::wstring normalizeWinePosixPath(const std::wstring& path, bool underWine) {
    if (underWine && !path.empty() && path[0] == L'/') {
        std::wstring mapped = L"Z:";
        for (wchar_t c : path) mapped.push_back(c == L'/' ? L'\\' : c);
        return mapped;
    }
    return path;
}

bool readFileUtf8(const std::wstring& path, std::string* out) {
    if (!out || path.empty()) return false;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(file, &sz) || sz.QuadPart <= 0 || sz.QuadPart > 4LL * 1024 * 1024) {
        CloseHandle(file);
        return false;
    }
    std::string buf(static_cast<size_t>(sz.QuadPart), '\0');
    DWORD got = 0;
    const BOOL ok = ReadFile(file, &buf[0], static_cast<DWORD>(buf.size()), &got, nullptr);
    CloseHandle(file);
    if (!ok) return false;
    buf.resize(got);
    if (buf.size() >= 3 && static_cast<unsigned char>(buf[0]) == 0xEF &&
        static_cast<unsigned char>(buf[1]) == 0xBB && static_cast<unsigned char>(buf[2]) == 0xBF) {
        buf.erase(0, 3);
    }
    *out = std::move(buf);
    return true;
}

// Linux procfs entries such as /proc/<pid>/comm report 0 bytes to GetFileSizeEx
// even though ReadFile returns the process name. Read directly into a small
// bounded buffer without relying on file size.
bool readSmallProcText(const std::wstring& path, std::string* out) {
    if (!out || path.empty()) return false;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    char buf[256]{};
    DWORD got = 0;
    const BOOL ok = ReadFile(file, buf, static_cast<DWORD>(sizeof(buf) - 1), &got, nullptr);
    CloseHandle(file);
    if (!ok || got == 0) return false;
    out->assign(buf, got);
    return true;
}

bool detectUnderWine() {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    return ntdll && GetProcAddress(ntdll, "wine_get_version") != nullptr;
}

std::wstring readRegString(HKEY root, const wchar_t* subkey, const wchar_t* valueName) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subkey, 0, KEY_READ, &key) != ERROR_SUCCESS) return {};
    DWORD type = 0;
    DWORD bytes = 0;
    if (RegQueryValueExW(key, valueName, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS ||
        (type != REG_SZ && type != REG_EXPAND_SZ) || bytes == 0 || bytes > 32768) {
        RegCloseKey(key);
        return {};
    }
    std::wstring buf(bytes / sizeof(wchar_t) + 1, L'\0');
    if (RegQueryValueExW(key, valueName, nullptr, &type, reinterpret_cast<BYTE*>(&buf[0]), &bytes) != ERROR_SUCCESS) {
        RegCloseKey(key);
        return {};
    }
    RegCloseKey(key);
    while (!buf.empty() && buf.back() == L'\0') buf.pop_back();
    if (type == REG_EXPAND_SZ && !buf.empty()) {
        wchar_t expanded[1024]{};
        const DWORD n = ExpandEnvironmentStringsW(buf.c_str(), expanded, 1024);
        if (n > 0 && n < 1024) return trimW(std::wstring(expanded));
    }
    return trimW(buf);
}

std::vector<std::wstring> readAvailableOpenXrRuntimes() {
    std::vector<std::wstring> out;
    const HKEY roots[] = {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER};
    for (HKEY root : roots) {
        HKEY key = nullptr;
        if (RegOpenKeyExW(root, L"SOFTWARE\\Khronos\\OpenXR\\1\\AvailableRuntimes", 0, KEY_READ, &key) !=
            ERROR_SUCCESS) {
            continue;
        }
        for (DWORD idx = 0; idx < 64; ++idx) {
            wchar_t name[1024]{};
            DWORD nameLen = 1024;
            DWORD type = 0;
            DWORD disabled = 0;
            DWORD dataLen = sizeof(disabled);
            const LSTATUS st = RegEnumValueW(key, idx, name, &nameLen, nullptr, &type,
                                             reinterpret_cast<BYTE*>(&disabled), &dataLen);
            if (st == ERROR_NO_MORE_ITEMS) break;
            if (st != ERROR_SUCCESS) continue;
            if (type == REG_DWORD && disabled != 0) continue;  // Khronos spec: non-zero disables entry
            std::wstring path(name, nameLen);
            if (!path.empty() && std::find(out.begin(), out.end(), path) == out.end()) {
                out.push_back(std::move(path));
            }
        }
        RegCloseKey(key);
    }
    return out;
}

bool hasProcess(const std::vector<std::string>& lowerProcs, const char* name) {
    return std::find(lowerProcs.begin(), lowerProcs.end(), name) != lowerProcs.end();
}

std::vector<std::string> snapshotRunningProcesses(bool underWine) {
    std::vector<std::string> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        if (Process32FirstW(snap, &entry)) {
            do {
                std::string narrow;
                for (const wchar_t* p = entry.szExeFile; *p; ++p) {
                    const wchar_t ch = *p;
                    narrow.push_back(ch < 128 ? static_cast<char>(ch) : '?');
                }
                narrow = toLowerA(std::move(narrow));
                if (!narrow.empty() && std::find(out.begin(), out.end(), narrow) == out.end()) {
                    out.push_back(std::move(narrow));
                }
            } while (Process32NextW(snap, &entry));
        }
        CloseHandle(snap);
    }

    // Under Wine/Proton, native Linux OpenXR servers live in host /proc/<pid>/comm.
    if (underWine) {
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW(L"Z:\\proc\\*", &fd);
        if (h != INVALID_HANDLE_VALUE) {
            int scanned = 0;
            do {
                if (++scanned > 4096) break;
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                bool digits = fd.cFileName[0] != L'\0';
                for (const wchar_t* p = fd.cFileName; *p; ++p) {
                    if (*p < L'0' || *p > L'9') {
                        digits = false;
                        break;
                    }
                }
                if (!digits) continue;
                std::string comm;
                if (!readSmallProcText(std::wstring(L"Z:\\proc\\") + fd.cFileName + L"\\comm", &comm)) continue;
                while (!comm.empty() && (comm.back() == '\n' || comm.back() == '\r')) comm.pop_back();
                comm = toLowerA(std::move(comm));
                if (isKnownVrServiceProcess(comm) && std::find(out.begin(), out.end(), comm) == out.end()) {
                    out.push_back(std::move(comm));
                }
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    return out;
}

// Dynamic SetupAPI scan for USB-connected VR headsets (zero link dependency on setupapi.lib).
UsbHeadsetMatch scanUsbVrHeadsets() {
    HMODULE mod = LoadLibraryExW(L"setupapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!mod) return {};

    struct DevInfoData {
        DWORD cbSize;
        GUID classGuid;
        DWORD devInst;
        ULONG_PTR reserved;
    };
    using FnGetClassDevsW = HANDLE(WINAPI*)(const GUID*, PCWSTR, HWND, DWORD);
    using FnEnumDeviceInfo = BOOL(WINAPI*)(HANDLE, DWORD, DevInfoData*);
    using FnGetDeviceInstanceIdW = BOOL(WINAPI*)(HANDLE, DevInfoData*, PWSTR, DWORD, PDWORD);
    using FnGetDeviceRegPropW = BOOL(WINAPI*)(HANDLE, DevInfoData*, DWORD, PDWORD, PBYTE, DWORD, PDWORD);
    using FnDestroyDeviceInfoList = BOOL(WINAPI*)(HANDLE);

    auto fnGetDevs = reinterpret_cast<FnGetClassDevsW>(GetProcAddress(mod, "SetupDiGetClassDevsW"));
    auto fnEnum = reinterpret_cast<FnEnumDeviceInfo>(GetProcAddress(mod, "SetupDiEnumDeviceInfo"));
    auto fnInstId = reinterpret_cast<FnGetDeviceInstanceIdW>(GetProcAddress(mod, "SetupDiGetDeviceInstanceIdW"));
    auto fnRegProp = reinterpret_cast<FnGetDeviceRegPropW>(GetProcAddress(mod, "SetupDiGetDeviceRegistryPropertyW"));
    auto fnDestroy = reinterpret_cast<FnDestroyDeviceInfoList>(GetProcAddress(mod, "SetupDiDestroyDeviceInfoList"));
    if (!fnGetDevs || !fnEnum || !fnInstId || !fnDestroy) {
        FreeLibrary(mod);
        return {};
    }

    constexpr DWORD kDigcfPresent = 0x00000002;
    constexpr DWORD kDigcfAllClasses = 0x00000004;
    constexpr DWORD kSpdrpDeviceDesc = 0x00000000;
    constexpr DWORD kSpdrpFriendlyName = 0x0000000C;

    UsbHeadsetMatch best;
    HANDLE devs = fnGetDevs(nullptr, L"USB", nullptr, kDigcfPresent | kDigcfAllClasses);
    if (devs != INVALID_HANDLE_VALUE) {
        DevInfoData dev{};
        dev.cbSize = sizeof(dev);
        for (DWORD idx = 0; fnEnum(devs, idx, &dev); ++idx) {
            wchar_t idBuf[512]{};
            if (!fnInstId(devs, &dev, idBuf, 512, nullptr)) continue;
            std::wstring desc;
            if (fnRegProp) {
                wchar_t propBuf[256]{};
                DWORD propType = 0;
                if (fnRegProp(devs, &dev, kSpdrpFriendlyName, &propType, reinterpret_cast<PBYTE>(propBuf),
                              sizeof(propBuf), nullptr) &&
                    propType == REG_SZ) {
                    desc = propBuf;
                } else if (fnRegProp(devs, &dev, kSpdrpDeviceDesc, &propType, reinterpret_cast<PBYTE>(propBuf),
                                     sizeof(propBuf), nullptr) &&
                           propType == REG_SZ) {
                    desc = propBuf;
                }
            }
            UsbHeadsetMatch hit = identifyUsbVrHeadset(idBuf, desc);
            if (!hit.name.empty()) {
                best = std::move(hit);
                break;
            }
        }
        fnDestroy(devs);
    }
    FreeLibrary(mod);
    return best;
}

// ---------------------------------------------------------------------------
// Minimal Khronos OpenXR 1.0 C ABI for headless headset/runtime probing.
// Keeps launcher compilation independent of third_party/openxr headers.
// ---------------------------------------------------------------------------
using XrResult = int32_t;
using XrVersion = uint64_t;
using XrInstance = uint64_t;
using XrSystemId = uint64_t;
using XrStructureType = int32_t;
using XrFormFactor = int32_t;
using XrBool32 = uint32_t;
using XrFlags64 = uint64_t;

constexpr XrStructureType kXrTypeInstanceCreateInfo = 3;
constexpr XrStructureType kXrTypeInstanceProperties = 32;
constexpr XrStructureType kXrTypeSystemGetInfo = 4;
constexpr XrStructureType kXrTypeSystemProperties = 5;
constexpr XrFormFactor kXrFormFactorHmd = 1;
constexpr XrVersion kXrApiVersion10 = (1ULL << 48);
constexpr int kXrErrorFormFactorUnavailable = -35;
constexpr int kXrErrorRuntimeUnavailable = -51;

struct XrAppInfo {
    char applicationName[128];
    uint32_t applicationVersion;
    char engineName[128];
    uint32_t engineVersion;
    XrVersion apiVersion;
};

struct XrInstCreateInfo {
    XrStructureType type;
    const void* next;
    XrFlags64 createFlags;
    XrAppInfo applicationInfo;
    uint32_t enabledApiLayerCount;
    const char* const* enabledApiLayerNames;
    uint32_t enabledExtensionCount;
    const char* const* enabledExtensionNames;
};

struct XrInstProps {
    XrStructureType type;
    void* next;
    XrVersion runtimeVersion;
    char runtimeName[128];
};

struct XrSysGetInfo {
    XrStructureType type;
    const void* next;
    XrFormFactor formFactor;
};

struct XrSysGraphicsProps {
    uint32_t maxSwapchainImageHeight;
    uint32_t maxSwapchainImageWidth;
    uint32_t maxLayerCount;
};

struct XrSysTrackingProps {
    XrBool32 orientationTracking;
    XrBool32 positionTracking;
};

struct XrSysProps {
    XrStructureType type;
    void* next;
    XrSystemId systemId;
    uint32_t vendorId;
    char systemName[256];
    XrSysGraphicsProps graphicsProperties;
    XrSysTrackingProps trackingProperties;
};

using PfnXrCreateInstance = XrResult(__stdcall*)(const XrInstCreateInfo*, XrInstance*);
using PfnXrDestroyInstance = XrResult(__stdcall*)(XrInstance);
using PfnXrGetInstanceProperties = XrResult(__stdcall*)(XrInstance, XrInstProps*);
using PfnXrGetSystem = XrResult(__stdcall*)(XrInstance, const XrSysGetInfo*, XrSystemId*);
using PfnXrGetSystemProperties = XrResult(__stdcall*)(XrInstance, XrSystemId, XrSysProps*);

struct RawOpenXrProbeOut {
    bool loaderLoaded = false;
    bool instanceCreated = false;
    bool headsetPresent = false;
    int resultCode = 0;
    char runtimeName[128]{};
    XrVersion runtimeVersion = 0;
    char systemName[256]{};
};

// Pure POD SEH-protected loader + OpenXR query so neither DLL initialization
// nor a misbehaving OpenXR runtime can crash the launcher.
bool invokeOpenXrProbeSeh(const wchar_t* verifiedLoaderPath, RawOpenXrProbeOut* out) {
    HMODULE loader = nullptr;
    __try {
        loader = LoadLibraryExW(verifiedLoaderPath, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!loader) return false;
        out->loaderLoaded = true;

        auto fnCreate = reinterpret_cast<PfnXrCreateInstance>(GetProcAddress(loader, "xrCreateInstance"));
        auto fnDestroy = reinterpret_cast<PfnXrDestroyInstance>(GetProcAddress(loader, "xrDestroyInstance"));
        auto fnInstProps =
            reinterpret_cast<PfnXrGetInstanceProperties>(GetProcAddress(loader, "xrGetInstanceProperties"));
        auto fnGetSys = reinterpret_cast<PfnXrGetSystem>(GetProcAddress(loader, "xrGetSystem"));
        auto fnSysProps = reinterpret_cast<PfnXrGetSystemProperties>(GetProcAddress(loader, "xrGetSystemProperties"));
        if (!fnCreate || !fnDestroy || !fnGetSys) {
            FreeLibrary(loader);
            return false;
        }

        XrInstCreateInfo ci{};
        ci.type = kXrTypeInstanceCreateInfo;
        std::snprintf(ci.applicationInfo.applicationName, sizeof(ci.applicationInfo.applicationName), "%s",
                      "ED-UP Launcher");
        ci.applicationInfo.applicationVersion = 1;
        std::snprintf(ci.applicationInfo.engineName, sizeof(ci.applicationInfo.engineName), "%s", "ED-UP");
        ci.applicationInfo.engineVersion = 1;
        ci.applicationInfo.apiVersion = kXrApiVersion10;

        XrInstance instance = 0;
        const XrResult cr = fnCreate(&ci, &instance);
        out->resultCode = static_cast<int>(cr);
        if (cr < 0 || instance == 0) {
            FreeLibrary(loader);
            return true;
        }
        out->instanceCreated = true;

        if (fnInstProps) {
            XrInstProps ip{};
            ip.type = kXrTypeInstanceProperties;
            if (fnInstProps(instance, &ip) >= 0) {
                ip.runtimeName[sizeof(ip.runtimeName) - 1] = '\0';
                std::snprintf(out->runtimeName, sizeof(out->runtimeName), "%s", ip.runtimeName);
                out->runtimeVersion = ip.runtimeVersion;
            }
        }

        XrSysGetInfo sgi{};
        sgi.type = kXrTypeSystemGetInfo;
        sgi.formFactor = kXrFormFactorHmd;
        XrSystemId sysId = 0;
        const XrResult sr = fnGetSys(instance, &sgi, &sysId);
        out->resultCode = static_cast<int>(sr);
        if (sr >= 0 && sysId != 0) {
            out->headsetPresent = true;
            if (fnSysProps) {
                XrSysProps sp{};
                sp.type = kXrTypeSystemProperties;
                if (fnSysProps(instance, sysId, &sp) >= 0) {
                    sp.systemName[sizeof(sp.systemName) - 1] = '\0';
                    std::snprintf(out->systemName, sizeof(out->systemName), "%s", sp.systemName);
                }
            }
        }

        fnDestroy(instance);
        FreeLibrary(loader);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (loader) FreeLibrary(loader);
        out->resultCode = -999;
        return false;
    }
}

std::wstring formatXrVersion(XrVersion v) {
    if (v == 0) return {};
    const uint32_t major = static_cast<uint32_t>((v >> 48) & 0xFFFFU);
    const uint32_t minor = static_cast<uint32_t>((v >> 32) & 0xFFFFU);
    const uint32_t patch = static_cast<uint32_t>(v & 0xFFFFFFFFU);
    wchar_t buf[64]{};
    std::swprintf(buf, 64, L"%u.%u.%u", major, minor, patch);
    return buf;
}

bool verifySignedLoaderOnDisk(const std::wstring& path, const std::string& expectedSha256) {
    if (path.empty() || expectedSha256.size() != 64 || !pathExistsW(path)) return false;
    const DllInfo info = probeDll(path);
    if (!info.is64 || !info.hasXrGetInstanceProcAddr) return false;
    return toLowerA(info.sha256) == toLowerA(expectedSha256);
}

// Runs the SEH-guarded OpenXR query behind a bounded timeout so an unresponsive
// runtime DLL can never wedge the UI, Save logs, or process shutdown.
OpenXrLiveProbe runLiveOpenXrProbeBounded(const std::wstring& verifiedLoaderPath,
                                          const std::string& verifiedLoaderSha256,
                                          unsigned int timeoutMs) {
    OpenXrLiveProbe result;
    if (!verifySignedLoaderOnDisk(verifiedLoaderPath, verifiedLoaderSha256)) return result;

    result.attempted = true;
    result.loaderFound = true;

    struct SharedJob {
        HANDLE doneEvent = nullptr;
        std::wstring path;
        RawOpenXrProbeOut raw{};
        bool ok = false;
        ~SharedJob() {
            if (doneEvent) CloseHandle(doneEvent);
        }
    };

    auto job = std::make_shared<SharedJob>();
    job->doneEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    job->path = verifiedLoaderPath;
    if (!job->doneEvent) {
        result.resultCode = -998;
        return result;
    }

    std::thread worker([job]() {
        job->ok = invokeOpenXrProbeSeh(job->path.c_str(), &job->raw);
        SetEvent(job->doneEvent);
    });

    const DWORD waitMs = timeoutMs > 0 ? static_cast<DWORD>(timeoutMs) : 2500u;
    const DWORD waitResult = WaitForSingleObject(job->doneEvent, waitMs);
    if (waitResult != WAIT_OBJECT_0) {
        // Runtime call stalled past deadline: detach so the caller returns cleanly.
        worker.detach();
        result.timedOut = true;
        result.resultCode = -997;
        return result;
    }

    if (worker.joinable()) worker.join();
    if (!job->ok) {
        result.loaderFound = job->raw.loaderLoaded;
        result.resultCode = job->raw.resultCode != 0 ? job->raw.resultCode : -999;
        return result;
    }
    result.instanceCreated = job->raw.instanceCreated;
    result.headsetPresent = job->raw.headsetPresent;
    result.resultCode = job->raw.resultCode;
    result.runtimeName = trimW(widenUtf8(job->raw.runtimeName));
    result.runtimeVersion = formatXrVersion(job->raw.runtimeVersion);
    result.systemName = trimW(widenUtf8(job->raw.systemName));
    return result;
}

std::wstring cleanHeadsetDisplayName(const std::wstring& raw) {
    std::wstring s = trimW(raw);
    if (s.empty()) return {};
    const std::wstring low = toLowerW(s);
    if (low == L"oculus quest 3" || low == L"quest 3") return L"Meta Quest 3";
    if (low == L"oculus quest 3s" || low == L"quest 3s") return L"Meta Quest 3S";
    if (low == L"oculus quest 2" || low == L"quest 2") return L"Meta Quest 2";
    if (low == L"oculus quest pro" || low == L"quest pro") return L"Meta Quest Pro";
    if (low == L"indexhmd" || low == L"index hmd") return L"Valve Index";
    return s;
}

bool parseHexUint(const std::wstring& text, size_t pos, uint32_t* out) {
    if (!out || pos + 4 > text.size()) return false;
    uint32_t val = 0;
    for (size_t i = 0; i < 4; ++i) {
        const wchar_t c = text[pos + i];
        uint32_t nibble = 0;
        if (c >= L'0' && c <= L'9') nibble = static_cast<uint32_t>(c - L'0');
        else if (c >= L'a' && c <= L'f') nibble = static_cast<uint32_t>(c - L'a' + 10);
        else if (c >= L'A' && c <= L'F') nibble = static_cast<uint32_t>(c - L'A' + 10);
        else return false;
        val = (val << 4) | nibble;
    }
    *out = val;
    return true;
}

VrPlatform findMismatchedRunningRuntime(VrPlatform active, const std::vector<std::string>& lowerProcs) {
    const VrPlatform candidates[] = {
        VrPlatform::SteamVr,
        VrPlatform::VirtualDesktopVdxr,
        VrPlatform::MetaLink,
        VrPlatform::Pimax,
        VrPlatform::WindowsMixedReality,
        VrPlatform::Varjo,
        VrPlatform::Vive,
        VrPlatform::Wivrn,
        VrPlatform::Monado,
    };
    for (VrPlatform p : candidates) {
        if (p == active) continue;
        if (active == VrPlatform::SteamVr && p == VrPlatform::VirtualDesktopVdxr) continue;
        if (isPlatformServiceRunning(p, lowerProcs)) return p;
    }
    return VrPlatform::Unknown;
}

VrPlatform detectStreamingBridge(VrPlatform active, const std::vector<std::string>& lowerProcs,
                                 const std::vector<std::wstring>& openVrDrivers, const std::wstring& systemName) {
    const std::wstring sysLow = toLowerW(systemName);
    if (containsW(sysLow, L"steam link") || containsW(sysLow, L"steamlink")) return VrPlatform::SteamLink;
    if (containsW(sysLow, L"virtual desktop") || containsW(sysLow, L"vdxr")) return VrPlatform::VirtualDesktopVdxr;
    if (containsW(sysLow, L"alvr")) return VrPlatform::Alvr;

    if (active == VrPlatform::SteamVr) {
        if (hasProcess(lowerProcs, "virtualdesktop.streamer.exe") ||
            hasProcess(lowerProcs, "virtualdesktop.service.exe")) {
            return VrPlatform::VirtualDesktopVdxr;
        }
        if (hasProcess(lowerProcs, "alvr_dashboard.exe") || hasProcess(lowerProcs, "alvr_launcher.exe") ||
            hasProcess(lowerProcs, "alvr_dashboard") || hasProcess(lowerProcs, "alvr_launcher")) {
            return VrPlatform::Alvr;
        }
        for (const std::wstring& drv : openVrDrivers) {
            const std::wstring low = toLowerW(drv);
            if (containsW(low, L"alvr")) return VrPlatform::Alvr;
        }
    }
    return VrPlatform::Unknown;
}

bool pathsMatchCaseInsensitive(const std::wstring& a, const std::wstring& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        wchar_t ca = static_cast<wchar_t>(std::towlower(a[i]));
        wchar_t cb = static_cast<wchar_t>(std::towlower(b[i]));
        if (ca == L'/') ca = L'\\';
        if (cb == L'/') cb = L'\\';
        if (ca != cb) return false;
    }
    return true;
}

LocalOpenXrRuntimeConfig readInstallLocalOpenXrConfig(const std::wstring& gameDir) {
    if (gameDir.empty()) return {};
    const std::wstring openvrDir = joinW(gameDir, L"Openvr\\win64");
    const std::wstring candidates[] = {
        joinW(openvrDir, L"edup_openxr.ini"),
        joinW(openvrDir, L"edvr_openxr.ini"),
    };
    for (const std::wstring& path : candidates) {
        std::string text;
        if (readFileUtf8(path, &text)) {
            LocalOpenXrRuntimeConfig cfg = parseLocalOpenXrRuntimeConfig(text);
            if (cfg.mode != LocalOpenXrRuntimeMode::NotConfigured) return cfg;
        }
    }
    return {};
}

bool detectInstallOpenCompositeActive(const std::wstring& gameDir) {
    if (gameDir.empty()) return false;
    const std::wstring activeDll = joinW(gameDir, L"Openvr\\win64\\openvr_api.dll");
    if (!pathExistsW(activeDll)) return false;
    const DllInfo info = probeDll(activeDll);
    if (info.kind == DllKind::Absent || info.kind == DllKind::Edvr) return false;
    return _wcsicmp(modNameOf(info).c_str(), L"OpenComposite") == 0;
}

}  // namespace

VrPlatform classifyVrPlatform(const std::wstring& pathOrName, const std::wstring& libraryPath) {
    const std::wstring combined = toLowerW(pathOrName + L" " + libraryPath);
    if (combined.find_first_not_of(L" \t\r\n") == std::wstring::npos) return VrPlatform::Unknown;

    if (containsW(combined, L"vdxr") || containsW(combined, L"virtualdesktop") ||
        containsW(combined, L"virtual desktop")) {
        return VrPlatform::VirtualDesktopVdxr;
    }
    if (containsW(combined, L"wivrn")) return VrPlatform::Wivrn;
    if (containsW(combined, L"monado")) return VrPlatform::Monado;
    if (containsW(combined, L"alvr")) return VrPlatform::Alvr;
    if (containsW(combined, L"pimax")) return VrPlatform::Pimax;
    if (containsW(combined, L"varjo")) return VrPlatform::Varjo;
    if (containsW(combined, L"mixedreality") || containsW(combined, L"mixed reality") ||
        containsW(combined, L"hololens") || containsW(combined, L"\\wmr\\")) {
        return VrPlatform::WindowsMixedReality;
    }
    if (containsW(combined, L"oculus") || containsW(combined, L"meta quest") ||
        containsW(combined, L"meta horizon") || containsW(combined, L"ovr_openxr")) {
        return VrPlatform::MetaLink;
    }
    if (containsW(combined, L"viveconsole") || containsW(combined, L"vivehub") ||
        containsW(combined, L"viveruntime") || containsW(combined, L"htc_rr") ||
        containsW(combined, L"vive openxr") || containsW(combined, L"vive_openxr")) {
        return VrPlatform::Vive;
    }
    if (containsW(combined, L"opencomposite") || containsW(combined, L"xrizer")) return VrPlatform::OpenComposite;
    if (containsW(combined, L"steamvr") || containsW(combined, L"steamxr") || containsW(combined, L"vrclient")) {
        return VrPlatform::SteamVr;
    }
    return VrPlatform::Custom;
}

std::wstring vrPlatformDisplayName(VrPlatform platform, const std::wstring& customName) {
    switch (platform) {
        case VrPlatform::SteamVr: return L"SteamVR";
        case VrPlatform::MetaLink: return L"Meta Horizon Link";
        case VrPlatform::VirtualDesktopVdxr: return L"Virtual Desktop (VDXR)";
        case VrPlatform::SteamLink: return L"Steam Link";
        case VrPlatform::Alvr: return L"ALVR";
        case VrPlatform::Pimax: return L"Pimax Play";
        case VrPlatform::WindowsMixedReality: return L"Windows Mixed Reality";
        case VrPlatform::Varjo: return L"Varjo Base";
        case VrPlatform::Vive: return L"HTC Vive OpenXR";
        case VrPlatform::Monado: return L"Monado";
        case VrPlatform::Wivrn: return L"WiVRn";
        case VrPlatform::OpenComposite: return L"OpenComposite";
        case VrPlatform::Custom: return customName.empty() ? L"OpenXR" : customName;
        case VrPlatform::Unknown:
        default:
            return customName;
    }
}

LocalOpenXrRuntimeConfig parseLocalOpenXrRuntimeConfig(const std::string& iniUtf8) {
    LocalOpenXrRuntimeConfig cfg;
    bool inOpenXr = false;
    size_t pos = 0;
    while (pos < iniUtf8.size()) {
        size_t end = iniUtf8.find('\n', pos);
        if (end == std::string::npos) end = iniUtf8.size();
        std::string line = trimA(iniUtf8.substr(pos, end - pos));
        pos = end < iniUtf8.size() ? end + 1 : iniUtf8.size();
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line.front() == '[' && line.back() == ']') {
            inOpenXr = (toLowerA(line) == "[openxr]");
            continue;
        }
        if (!inOpenXr) continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = toLowerA(trimA(line.substr(0, eq)));
        const std::string val = trimA(line.substr(eq + 1));
        if (key != "runtime" || val.empty()) continue;
        if (toLowerA(val) == "system") {
            cfg.mode = LocalOpenXrRuntimeMode::System;
            cfg.explicitPath.clear();
            return cfg;
        }
        const std::wstring wval = widenUtf8(val);
        if (isAbsolutePathW(wval)) {
            cfg.mode = LocalOpenXrRuntimeMode::ExplicitPath;
            cfg.explicitPath = wval;
            return cfg;
        }
    }
    return cfg;
}

OpenXrManifestInfo parseOpenXrRuntimeJson(const std::string& jsonUtf8, const std::wstring& jsonPath) {
    OpenXrManifestInfo info;
    info.jsonPath = jsonPath;
    JsonValue root;
    std::string err;
    if (!parseJson(jsonUtf8, &root, &err) || !root.isObject()) return info;
    const JsonValue* runtime = root.findNoCase("runtime");
    if (!runtime || !runtime->isObject()) return info;

    if (const JsonValue* name = runtime->findNoCase("name"); name && name->isString()) {
        info.runtimeName = trimW(widenUtf8(name->text));
    }
    if (const JsonValue* lib = runtime->findNoCase("library_path"); lib && lib->isString()) {
        std::wstring libPath = trimW(widenUtf8(lib->text));
        if (!libPath.empty() && !isAbsolutePathW(libPath)) {
            const std::wstring baseDir = dirOfPath(jsonPath);
            if (!baseDir.empty()) libPath = joinW(baseDir, libPath);
        }
        info.libraryPath = std::move(libPath);
    }
    info.valid = !info.libraryPath.empty() || !info.runtimeName.empty();
    return info;
}

std::vector<std::wstring> parseOpenVrExternalDrivers(const std::string& vrpathUtf8) {
    std::vector<std::wstring> out;
    JsonValue root;
    std::string err;
    if (!parseJson(vrpathUtf8, &root, &err) || !root.isObject()) return out;
    const JsonValue* drivers = root.findNoCase("external_drivers");
    if (!drivers || !drivers->isArray()) return out;
    for (const JsonValue& item : drivers->items) {
        if (item.isString() && !item.text.empty()) out.push_back(widenUtf8(item.text));
    }
    return out;
}

UsbHeadsetMatch identifyUsbVrHeadset(const std::wstring& instanceId, const std::wstring& description) {
    const std::wstring id = toLowerW(instanceId);
    const std::wstring desc = toLowerW(description);

    const size_t vidPos = id.find(L"vid_");
    const size_t pidPos = id.find(L"pid_");
    uint32_t vid = 0;
    uint32_t pid = 0;
    if (vidPos == std::wstring::npos || pidPos == std::wstring::npos) return {};
    if (!parseHexUint(id, vidPos + 4, &vid) || !parseHexUint(id, pidPos + 4, &pid)) return {};

    // Valve (0x28DE): match only Index HMD / camera / lighthouse HMD PIDs (never Steam Controller / Deck).
    if (vid == 0x28DE) {
        if (pid == 0x2300 || pid == 0x2400 || pid == 0x2613 ||
            (pid == 0x2000 && containsW(desc, L"index"))) {
            return {L"Valve Index", VrPlatform::SteamVr};
        }
        return {};
    }

    // Meta / Oculus (0x2833).
    if (vid == 0x2833) {
        if (pid == 0x0031) return {L"Oculus Rift", VrPlatform::MetaLink};
        if (pid == 0x0051 || pid == 0x0052) return {L"Oculus Rift S", VrPlatform::MetaLink};
        if (pid == 0x0183) return {L"Meta Quest", VrPlatform::MetaLink};
        if (pid == 0x0186) return {L"Meta Quest 2", VrPlatform::MetaLink};
        if (pid == 0x0187 || pid == 0x0188) return {L"Meta Quest Pro", VrPlatform::MetaLink};
        if (pid == 0x0182 || pid == 0x018C || pid == 0x0201 || pid == 0x0202) {
            if (containsW(desc, L"quest 3s")) return {L"Meta Quest 3S", VrPlatform::MetaLink};
            return {L"Meta Quest 3", VrPlatform::MetaLink};
        }
        if (containsW(desc, L"quest 3s")) return {L"Meta Quest 3S", VrPlatform::MetaLink};
        if (containsW(desc, L"quest 3")) return {L"Meta Quest 3", VrPlatform::MetaLink};
        if (containsW(desc, L"quest pro")) return {L"Meta Quest Pro", VrPlatform::MetaLink};
        if (containsW(desc, L"quest 2")) return {L"Meta Quest 2", VrPlatform::MetaLink};
        if (containsW(desc, L"rift s")) return {L"Oculus Rift S", VrPlatform::MetaLink};
        if (containsW(desc, L"rift")) return {L"Oculus Rift", VrPlatform::MetaLink};
        if (containsW(desc, L"quest") || containsW(desc, L"oculus")) {
            return {L"Meta Quest", VrPlatform::MetaLink};
        }
        return {};
    }

    // HTC Vive (0x0BB4).
    if (vid == 0x0BB4) {
        if (pid == 0x2C87) return {L"HTC Vive", VrPlatform::SteamVr};
        if (pid == 0x0309 || pid == 0x030A) return {L"HTC Vive Pro", VrPlatform::SteamVr};
        if (pid == 0x032B || pid == 0x0342) return {L"HTC Vive Pro 2", VrPlatform::Vive};
        if (pid == 0x0313) return {L"HTC Vive Cosmos", VrPlatform::Vive};
        if (pid == 0x034E || pid == 0x034F) return {L"HTC Vive XR Elite", VrPlatform::Vive};
        if (pid == 0x0356) return {L"HTC Vive Focus Vision", VrPlatform::Vive};
        return {};
    }

    if (vid == 0x35BD) return {L"Bigscreen Beyond", VrPlatform::SteamVr};

    if (vid == 0x34A4) {
        if (containsW(desc, L"crystal super")) return {L"Pimax Crystal Super", VrPlatform::Pimax};
        if (containsW(desc, L"crystal light")) return {L"Pimax Crystal Light", VrPlatform::Pimax};
        if (containsW(desc, L"crystal")) return {L"Pimax Crystal", VrPlatform::Pimax};
        if (containsW(desc, L"8kx")) return {L"Pimax 8KX", VrPlatform::Pimax};
        return {L"Pimax Headset", VrPlatform::Pimax};
    }

    if (vid == 0x054C && (pid == 0x0CDE || pid == 0x0E52)) {
        return {L"PlayStation VR2", VrPlatform::SteamVr};
    }

    if (vid == 0x03F0) {
        if (pid == 0x0580) return {L"HP Reverb G2", VrPlatform::WindowsMixedReality};
        if (pid == 0x0C6A) return {L"HP Reverb", VrPlatform::WindowsMixedReality};
        return {};
    }

    if (vid == 0x045E && pid == 0x0659) {
        if (containsW(desc, L"reverb g2")) return {L"HP Reverb G2", VrPlatform::WindowsMixedReality};
        if (containsW(desc, L"odyssey")) return {L"Samsung Odyssey", VrPlatform::WindowsMixedReality};
        return {L"Windows Mixed Reality Headset", VrPlatform::WindowsMixedReality};
    }

    if (vid == 0x2D40) {
        if (pid == 0x00B9) return {L"PICO 4 Ultra", VrPlatform::SteamVr};
        if (pid == 0x00B6 || pid == 0x00B7) return {L"PICO 4", VrPlatform::SteamVr};
        if (pid == 0x00B0) return {L"PICO Neo 3", VrPlatform::SteamVr};
        return {};
    }

    if (vid == 0x30C1) {
        if (containsW(desc, L"xr-4")) return {L"Varjo XR-4", VrPlatform::Varjo};
        if (containsW(desc, L"aero")) return {L"Varjo Aero", VrPlatform::Varjo};
        return {L"Varjo Headset", VrPlatform::Varjo};
    }

    if (vid == 0x3376) return {L"DPVR E4", VrPlatform::SteamVr};
    if (vid == 0x3582) return {L"Shiftall MeganeX", VrPlatform::SteamVr};
    return {};
}

bool isPlatformServiceRunning(VrPlatform platform, const std::vector<std::string>& lowerProcesses) {
    switch (platform) {
        case VrPlatform::SteamVr:
        case VrPlatform::SteamLink:
            return hasProcess(lowerProcesses, "vrserver.exe") || hasProcess(lowerProcesses, "vrmonitor.exe") ||
                   hasProcess(lowerProcesses, "vrcompositor.exe") || hasProcess(lowerProcesses, "vrserver") ||
                   hasProcess(lowerProcesses, "vrmonitor");
        case VrPlatform::MetaLink:
            return hasProcess(lowerProcesses, "ovrserver_x64.exe") || hasProcess(lowerProcesses, "oculusclient.exe") ||
                   hasProcess(lowerProcesses, "metaquestlink.exe");
        case VrPlatform::VirtualDesktopVdxr:
            return hasProcess(lowerProcesses, "virtualdesktop.streamer.exe") ||
                   hasProcess(lowerProcesses, "virtualdesktop.service.exe");
        case VrPlatform::Alvr:
            return hasProcess(lowerProcesses, "alvr_dashboard.exe") || hasProcess(lowerProcesses, "alvr_launcher.exe") ||
                   hasProcess(lowerProcesses, "alvr_dashboard") || hasProcess(lowerProcesses, "alvr_launcher");
        case VrPlatform::Pimax:
            return hasProcess(lowerProcesses, "pi_server.exe") || hasProcess(lowerProcesses, "pimaxclient.exe") ||
                   hasProcess(lowerProcesses, "pimaxplay.exe") || hasProcess(lowerProcesses, "piservice.exe") ||
                   hasProcess(lowerProcesses, "piservice_launcher.exe");
        case VrPlatform::WindowsMixedReality:
            return hasProcess(lowerProcesses, "mixedrealityportal.exe") || hasProcess(lowerProcesses, "holoshellapp.exe");
        case VrPlatform::Varjo:
            return hasProcess(lowerProcesses, "varjobase.exe") || hasProcess(lowerProcesses, "varjoserver.exe");
        case VrPlatform::Vive:
            return hasProcess(lowerProcesses, "viveconsole.exe") || hasProcess(lowerProcesses, "vivehub.exe") ||
                   hasProcess(lowerProcesses, "viveruntime.exe");
        case VrPlatform::Wivrn:
            return hasProcess(lowerProcesses, "wivrn-server");
        case VrPlatform::Monado:
            return hasProcess(lowerProcesses, "monado-service") || hasProcess(lowerProcesses, "envision");
        case VrPlatform::OpenComposite:
            return isPlatformServiceRunning(VrPlatform::SteamVr, lowerProcesses) ||
                   isPlatformServiceRunning(VrPlatform::MetaLink, lowerProcesses) ||
                   isPlatformServiceRunning(VrPlatform::VirtualDesktopVdxr, lowerProcesses) ||
                   isPlatformServiceRunning(VrPlatform::Wivrn, lowerProcesses) ||
                   isPlatformServiceRunning(VrPlatform::Monado, lowerProcesses);
        case VrPlatform::Custom:
        case VrPlatform::Unknown:
        default:
            return false;
    }
}

bool shouldRunLiveOpenXrProbe(VrPlatform activePlatform, const std::vector<std::string>& lowerProcesses,
                              bool underWine, bool activeFromEnv) {
    if (activePlatform == VrPlatform::Unknown) return false;
    if (isPlatformServiceRunning(activePlatform, lowerProcesses)) return true;
    if (underWine && activeFromEnv && activePlatform != VrPlatform::SteamVr) return true;
    return false;
}

VrHeadsetReport resolveVrHeadsetStatus(const VrProbeInputs& in) {
    VrHeadsetReport report;
    report.underWine = in.underWine;
    report.activeRuntimeFromEnv = in.activeRuntimeFromEnv;
    report.activeRuntimePath = in.activeRuntimePath;
    report.activeLibraryPath = in.activeManifest.libraryPath;
    report.availableRuntimes = in.availableRuntimePaths;
    report.openCompositeWarning = in.openVrShimIsOpenComposite;
    for (const std::string& proc : in.runningProcesses) {
        if (isKnownVrServiceProcess(proc) &&
            std::find(report.runningVrServices.begin(), report.runningVrServices.end(), proc) ==
                report.runningVrServices.end()) {
            report.runningVrServices.push_back(proc);
        }
    }

    // 1. Identify active OpenXR runtime.
    const bool hasActivePath = !in.activeRuntimePath.empty();
    if (!hasActivePath) {
        report.status = VrStatusKind::NotConfigured;
        report.activePlatform = VrPlatform::Unknown;
        if (!in.usbHeadset.name.empty()) {
            report.headsetName = in.usbHeadset.name;
            report.headsetFromUsb = true;
        }
        const VrPlatform running = findMismatchedRunningRuntime(VrPlatform::Unknown, in.runningProcesses);
        if (running != VrPlatform::Unknown) {
            report.mismatchPlatform = running;
            report.mismatchRuntimeName = vrPlatformDisplayName(running);
        }
        return report;
    }

    const std::wstring fallbackCustomName =
        !in.activeManifest.runtimeName.empty() ? in.activeManifest.runtimeName : stemOfPath(in.activeRuntimePath);
    report.activePlatform = classifyVrPlatform(
        in.activeRuntimePath + L" " + in.activeManifest.runtimeName, in.activeManifest.libraryPath);
    if (in.liveProbe.instanceCreated && !in.liveProbe.runtimeName.empty()) {
        const VrPlatform livePlat = classifyVrPlatform(in.liveProbe.runtimeName);
        if (livePlat != VrPlatform::Unknown && livePlat != VrPlatform::Custom) {
            report.activePlatform = livePlat;
        }
    }
    if (report.activePlatform == VrPlatform::OpenComposite) report.openCompositeWarning = true;
    report.runtimeDisplayName = vrPlatformDisplayName(report.activePlatform, fallbackCustomName);
    report.runtimeVersion = in.liveProbe.runtimeVersion;

    if (!in.activeManifest.jsonExists) {
        report.status = VrStatusKind::NotConfigured;
        report.activeRuntimeMissingFile = true;
        if (!in.usbHeadset.name.empty()) {
            report.headsetName = in.usbHeadset.name;
            report.headsetFromUsb = true;
        }
        return report;
    }
    if (in.activeManifest.valid && !in.activeManifest.libraryPath.empty() && !in.activeManifest.libraryExists) {
        report.status = VrStatusKind::Error;
        report.activeLibraryMissingFile = true;
        if (!in.usbHeadset.name.empty()) {
            report.headsetName = in.usbHeadset.name;
            report.headsetFromUsb = true;
        }
        return report;
    }

    // 2. Detect companion streaming bridges and runtime mismatches.
    const bool activeServiceUp = isPlatformServiceRunning(report.activePlatform, in.runningProcesses);
    report.runningBridge =
        detectStreamingBridge(report.activePlatform, in.runningProcesses, in.openVrDrivers, in.liveProbe.systemName);
    if (report.runningBridge != VrPlatform::Unknown && report.runningBridge != report.activePlatform) {
        report.transportName = vrPlatformDisplayName(report.runningBridge);
    }

    const VrPlatform otherRunning = findMismatchedRunningRuntime(report.activePlatform, in.runningProcesses);
    if (otherRunning != VrPlatform::Unknown && !activeServiceUp) {
        report.runtimeMismatch = true;
        report.mismatchPlatform = otherRunning;
        report.mismatchRuntimeName = vrPlatformDisplayName(otherRunning);
    }

    // 3. Resolve live status from OpenXR probe -> running service -> USB hardware.
    if (in.liveProbe.attempted && in.liveProbe.loaderFound) {
        report.openXrResult = in.liveProbe.resultCode;
        report.liveProbeTimedOut = in.liveProbe.timedOut;
        if (in.liveProbe.timedOut) {
            report.status = VrStatusKind::Error;
            if (!in.usbHeadset.name.empty()) {
                report.headsetName = in.usbHeadset.name;
                report.headsetFromUsb = true;
            }
            return report;
        }
        if (in.liveProbe.instanceCreated && in.liveProbe.headsetPresent) {
            report.status = VrStatusKind::Ready;
            report.headsetName = cleanHeadsetDisplayName(in.liveProbe.systemName);
            if (report.headsetName.empty() && !in.usbHeadset.name.empty()) {
                report.headsetName = in.usbHeadset.name;
                report.headsetFromUsb = true;
            }
            return report;
        }
        if (in.liveProbe.instanceCreated && !in.liveProbe.headsetPresent) {
            // Reserve Standby strictly for an unavailable HMD; preserve runtime failures as Error.
            if (in.liveProbe.resultCode == kXrErrorFormFactorUnavailable || in.liveProbe.resultCode == 0) {
                report.status = VrStatusKind::Standby;
            } else {
                report.status = VrStatusKind::Error;
            }
            if (!in.usbHeadset.name.empty()) {
                report.headsetName = in.usbHeadset.name;
                report.headsetFromUsb = true;
            }
            return report;
        }
        if (in.liveProbe.resultCode == kXrErrorRuntimeUnavailable ||
            in.liveProbe.resultCode == kXrErrorFormFactorUnavailable) {
            report.status = activeServiceUp ? VrStatusKind::Standby : VrStatusKind::RuntimeInactive;
        } else {
            report.status = VrStatusKind::Error;
        }
        if (!in.usbHeadset.name.empty()) {
            report.headsetName = in.usbHeadset.name;
            report.headsetFromUsb = true;
        }
        return report;
    }

    if (!in.usbHeadset.name.empty()) {
        report.headsetName = in.usbHeadset.name;
        report.headsetFromUsb = true;
    }
    report.status = activeServiceUp ? VrStatusKind::Standby : VrStatusKind::RuntimeInactive;
    return report;
}

VrPreflightWarning evaluateVrLaunchPreflight(const VrHeadsetReport& report) {
    if (report.status == VrStatusKind::Checking) return VrPreflightWarning::None;
    if (report.openCompositeWarning) return VrPreflightWarning::OpenComposite;
    if (report.status == VrStatusKind::NotConfigured) {
        return report.activeRuntimeMissingFile ? VrPreflightWarning::MissingRuntimeFile
                                               : VrPreflightWarning::NoRuntime;
    }
    if (report.status == VrStatusKind::Error) return VrPreflightWarning::RuntimeError;
    if (report.status == VrStatusKind::RuntimeInactive && report.runtimeMismatch) {
        return VrPreflightWarning::RuntimeMismatch;
    }
    return VrPreflightWarning::None;
}

std::wstring redactUserPath(const std::wstring& path) {
    if (path.empty()) return {};
    std::wstring out = path;

    auto replaceSegmentAfter = [](std::wstring& s, const wchar_t* marker) {
        const std::wstring mLower = toLowerW(marker);
        size_t start = 0;
        while (start < s.size()) {
            const std::wstring sLower = toLowerW(s);
            const size_t at = sLower.find(mLower, start);
            if (at == std::wstring::npos) break;
            const size_t segStart = at + mLower.size();
            if (segStart >= s.size()) break;
            size_t segEnd = s.find_first_of(L"\\/", segStart);
            if (segEnd == std::wstring::npos) segEnd = s.size();
            if (segEnd > segStart) {
                s.replace(segStart, segEnd - segStart, L"<user>");
                start = segStart + 6;
            } else {
                start = segStart + 1;
            }
        }
    };

    replaceSegmentAfter(out, L"\\users\\");
    replaceSegmentAfter(out, L"/users/");
    replaceSegmentAfter(out, L"/home/");
    replaceSegmentAfter(out, L"\\home\\");
    return out;
}

std::string formatVrEnvironmentSnapshot(const VrHeadsetReport& report) {
    auto statusName = [](VrStatusKind kind) -> const char* {
        switch (kind) {
            case VrStatusKind::Ready: return "ready";
            case VrStatusKind::Standby: return "standby";
            case VrStatusKind::RuntimeInactive: return "runtime_inactive";
            case VrStatusKind::NotConfigured: return "not_configured";
            case VrStatusKind::Error: return "error";
            case VrStatusKind::Checking:
            default: return "checking";
        }
    };
    auto preflightName = [](VrPreflightWarning w) -> const char* {
        switch (w) {
            case VrPreflightWarning::NoRuntime: return "no_runtime";
            case VrPreflightWarning::MissingRuntimeFile: return "missing_runtime_file";
            case VrPreflightWarning::RuntimeMismatch: return "runtime_mismatch";
            case VrPreflightWarning::OpenComposite: return "opencomposite_active";
            case VrPreflightWarning::RuntimeError: return "runtime_error";
            case VrPreflightWarning::None:
            default: return "none";
        }
    };

    std::string out;
    out += "ED-UP VR Environment Snapshot\r\n";
    out += "=============================\r\n";
    out += std::string("status: ") + statusName(report.status) + "\r\n";
    out += std::string("preflight_warning: ") + preflightName(evaluateVrLaunchPreflight(report)) + "\r\n";
    out += "runtime: " +
           (report.runtimeDisplayName.empty() ? std::string("(none)")
                                              : narrowUtf8(report.runtimeDisplayName) +
                                                    (report.runtimeVersion.empty()
                                                         ? std::string()
                                                         : " " + narrowUtf8(report.runtimeVersion))) +
           "\r\n";
    out += std::string("runtime_source: ") +
           (report.activeRuntimePath.empty() ? "(none)"
                                             : (report.activeRuntimeFromEnv ? "XR_RUNTIME_JSON" : "registry")) +
           "\r\n";
    if (!report.activeRuntimePath.empty()) {
        out += "active_runtime_json: " + narrowUtf8(redactUserPath(report.activeRuntimePath)) +
               (report.activeRuntimeMissingFile ? " [MISSING]" : "") + "\r\n";
    }
    if (!report.activeLibraryPath.empty()) {
        out += "active_runtime_library: " + narrowUtf8(redactUserPath(report.activeLibraryPath)) +
               (report.activeLibraryMissingFile ? " [MISSING]" : "") + "\r\n";
    }
    out += "headset: " +
           (report.headsetName.empty() ? std::string("(none)") : narrowUtf8(report.headsetName)) + "\r\n";
    out += std::string("headset_source: ") +
           (report.headsetName.empty() ? "(none)" : (report.headsetFromUsb ? "usb" : "openxr")) + "\r\n";
    if (!report.transportName.empty()) {
        out += "transport: " + narrowUtf8(report.transportName) + "\r\n";
    }
    if (report.liveProbeTimedOut) {
        out += "live_probe_timeout: yes\r\n";
    }
    if (report.openXrResult != 0) {
        out += "openxr_result: " + std::to_string(report.openXrResult) + "\r\n";
    }
    out += std::string("runtime_mismatch: ") +
           (report.runtimeMismatch ? ("yes (" + narrowUtf8(report.mismatchRuntimeName) + ")") : "no") + "\r\n";
    out += std::string("opencomposite_warning: ") + (report.openCompositeWarning ? "yes" : "no") + "\r\n";
    out += std::string("under_wine: ") + (report.underWine ? "yes" : "no") + "\r\n";

    out += "running_vr_services: ";
    if (report.runningVrServices.empty()) {
        out += "(none)\r\n";
    } else {
        for (size_t i = 0; i < report.runningVrServices.size(); ++i) {
            if (i > 0) out += ", ";
            out += report.runningVrServices[i];
        }
        out += "\r\n";
    }

    if (!report.availableRuntimes.empty()) {
        out += "available_openxr_runtimes:\r\n";
        for (const std::wstring& rt : report.availableRuntimes) {
            out += "  - " + narrowUtf8(redactUserPath(rt)) + "\r\n";
        }
    }
    return out;
}

VrHeadsetReport probeVrHeadset(const VrProbeOptions& options) {
    VrProbeInputs in;
    in.underWine = detectUnderWine();

    // Layer 1: Effective OpenXR runtime, respecting the selected install's
    // edup_openxr.ini runtime= setting (which mirrors native_local_config.h).
    const LocalOpenXrRuntimeConfig localCfg = readInstallLocalOpenXrConfig(options.gameDir);
    wchar_t envBuf[1024]{};
    const DWORD envLen = GetEnvironmentVariableW(L"XR_RUNTIME_JSON", envBuf, 1024);
    const std::wstring envRuntime = (envLen > 0 && envLen < 1024) ? trimW(std::wstring(envBuf, envLen)) : std::wstring();
    std::wstring regRuntime = readRegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\OpenXR\\1", L"ActiveRuntime");
    if (regRuntime.empty()) {
        regRuntime = readRegString(HKEY_CURRENT_USER, L"SOFTWARE\\Khronos\\OpenXR\\1", L"ActiveRuntime");
    }

    bool envConflictsWithEffective = false;
    if (localCfg.mode == LocalOpenXrRuntimeMode::System) {
        // Elite clears XR_RUNTIME_JSON when edup_openxr.ini sets runtime=system.
        if (!regRuntime.empty()) {
            in.activeRuntimePath = regRuntime;
            in.activeRuntimeFromEnv = false;
            if (!envRuntime.empty() && !pathsMatchCaseInsensitive(envRuntime, regRuntime)) {
                envConflictsWithEffective = true;
            }
        } else if (in.underWine && !envRuntime.empty()) {
            in.activeRuntimePath = envRuntime;
            in.activeRuntimeFromEnv = true;
        }
    } else if (localCfg.mode == LocalOpenXrRuntimeMode::ExplicitPath) {
        if (!envRuntime.empty()) {
            in.activeRuntimePath = envRuntime;
            in.activeRuntimeFromEnv = true;
        } else {
            in.activeRuntimePath = localCfg.explicitPath;
            in.activeRuntimeFromEnv = false;
        }
    } else {
        if (!envRuntime.empty()) {
            in.activeRuntimePath = envRuntime;
            in.activeRuntimeFromEnv = true;
        } else {
            in.activeRuntimePath = regRuntime;
            in.activeRuntimeFromEnv = false;
        }
    }
    in.availableRuntimePaths = readAvailableOpenXrRuntimes();

    if (!in.activeRuntimePath.empty()) {
        const std::wstring fsPath = normalizeWinePosixPath(in.activeRuntimePath, in.underWine);
        in.activeManifest.jsonPath = in.activeRuntimePath;
        in.activeManifest.jsonExists = pathExistsW(fsPath);
        if (in.activeManifest.jsonExists) {
            std::string jsonText;
            if (readFileUtf8(fsPath, &jsonText)) {
                in.activeManifest = parseOpenXrRuntimeJson(jsonText, fsPath);
                in.activeManifest.jsonExists = true;
                if (!in.activeManifest.libraryPath.empty()) {
                    const std::wstring libFs = normalizeWinePosixPath(in.activeManifest.libraryPath, in.underWine);
                    in.activeManifest.libraryExists = pathExistsW(libFs);
                }
            }
        }
    }

    // Layer 2: Running VR runtime & streaming bridge processes.
    in.runningProcesses = snapshotRunningProcesses(in.underWine);

    // Layer 3: Active OpenVR shim check for OpenComposite in the selected install.
    in.openVrShimIsOpenComposite = detectInstallOpenCompositeActive(options.gameDir);

    // Layer 4: USB PnP hardware detection (fast, zero auto-launch).
    in.usbHeadset = scanUsbVrHeadsets();

    // Layer 5: SteamVR openvrpaths.vrpath (external drivers like ALVR).
    wchar_t localAppData[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH) > 0) {
        const std::wstring vrpathFile = joinW(localAppData, L"openvr\\openvrpaths.vrpath");
        std::string vrpathText;
        if (readFileUtf8(vrpathFile, &vrpathText)) {
            in.steamVrInstalled = true;
            in.openVrDrivers = parseOpenVrExternalDrivers(vrpathText);
        }
    }

    // Layer 6: Bounded, verified live OpenXR query ONLY when safe:
    // - allowed by caller
    // - runtime manifest & library exist
    // - active runtime service is already running (never auto-boot SteamVR)
    // - inherited XR_RUNTIME_JSON does not conflict with Elite's effective runtime
    // - cryptographically verified openxr_loader.dll was provided
    const VrPlatform activePlatform =
        in.activeRuntimePath.empty()
            ? VrPlatform::Unknown
            : classifyVrPlatform(in.activeRuntimePath + L" " + in.activeManifest.runtimeName,
                                 in.activeManifest.libraryPath);
    if (options.allowLiveProbe && !envConflictsWithEffective && in.activeManifest.jsonExists &&
        (!in.activeManifest.valid || in.activeManifest.libraryExists) &&
        shouldRunLiveOpenXrProbe(activePlatform, in.runningProcesses, in.underWine, in.activeRuntimeFromEnv)) {
        in.liveProbe = runLiveOpenXrProbeBounded(options.verifiedLoaderPath, options.verifiedLoaderSha256,
                                                 options.liveProbeTimeoutMs);
    }

    return resolveVrHeadsetStatus(in);
}

}  // namespace edvr::installer
