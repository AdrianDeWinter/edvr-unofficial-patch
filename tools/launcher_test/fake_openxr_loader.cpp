// fake_openxr_loader -- minimal 64-bit OpenXR loader stand-in for launcher_test.
//
// Exports xrGetInstanceProcAddr plus the five OpenXR 1.0 functions runOpenXrProbeHelper
// binds (src\installer\vr_headset.cpp). Controlled solely by EDUP_FAKE_OPENXR_* env vars
// and XR_RUNTIME_JSON so launcher_test can verify signed-manifest loading and every live
// probe status path without touching a real VR runtime.
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

using XrResult = int32_t;
using XrVersion = uint64_t;
using XrInstance = uint64_t;
using XrSystemId = uint64_t;
using XrStructureType = int32_t;
using XrFormFactor = int32_t;
using XrBool32 = uint32_t;

constexpr XrResult kXrSuccess = 0;
constexpr XrResult kXrErrorInstanceLost = -13;
constexpr XrResult kXrErrorFormFactorUnavailable = -35;
constexpr XrResult kXrErrorRuntimeUnavailable = -51;

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

std::string envUtf8(const wchar_t* name) {
    wchar_t buf[1024]{};
    const DWORD n = GetEnvironmentVariableW(name, buf, static_cast<DWORD>(sizeof(buf) / sizeof(buf[0])));
    if (n == 0 || n >= sizeof(buf) / sizeof(buf[0])) return {};
    const int need = WideCharToMultiByte(CP_UTF8, 0, buf, static_cast<int>(n), nullptr, 0, nullptr, nullptr);
    if (need <= 0) return {};
    std::string out(static_cast<size_t>(need), '\0');
    WideCharToMultiByte(CP_UTF8, 0, buf, static_cast<int>(n), &out[0], need, nullptr, nullptr);
    return out;
}

}  // namespace

extern "C" {

__declspec(dllexport) XrResult __stdcall xrGetInstanceProcAddr(XrInstance, const char*, void** function) {
    if (function) *function = nullptr;
    return kXrSuccess;
}

__declspec(dllexport) XrResult __stdcall xrCreateInstance(const void*, XrInstance* instance) {
    const std::string mode = envUtf8(L"EDUP_FAKE_OPENXR_MODE");
    if (mode == "instance_lost") return kXrErrorInstanceLost;
    if (mode == "runtime_unavailable") return kXrErrorRuntimeUnavailable;
    if (instance) *instance = 0x1001ULL;
    return kXrSuccess;
}

__declspec(dllexport) XrResult __stdcall xrDestroyInstance(XrInstance) { return kXrSuccess; }

__declspec(dllexport) XrResult __stdcall xrGetInstanceProperties(XrInstance, XrInstProps* properties) {
    if (!properties) return -1;
    std::string runtime = envUtf8(L"EDUP_FAKE_OPENXR_RUNTIME");
    if (runtime.empty()) runtime = "SteamVR/OpenXR";
    properties->runtimeVersion = (2ULL << 48) | (8ULL << 32) | 4ULL;
    std::snprintf(properties->runtimeName, sizeof(properties->runtimeName), "%s", runtime.c_str());
    return kXrSuccess;
}

__declspec(dllexport) XrResult __stdcall xrGetSystem(XrInstance, const XrSysGetInfo*, XrSystemId* systemId) {
    const std::string mode = envUtf8(L"EDUP_FAKE_OPENXR_MODE");
    if (mode == "standby") return kXrErrorFormFactorUnavailable;
    if (systemId) *systemId = 7ULL;
    return kXrSuccess;
}

__declspec(dllexport) XrResult __stdcall xrGetSystemProperties(XrInstance, XrSystemId systemId, XrSysProps* properties) {
    if (!properties) return -1;
    properties->systemId = systemId;
    properties->vendorId = 0x2833;
    std::string sysName = envUtf8(L"EDUP_FAKE_OPENXR_SYSTEM");
    if (sysName.empty()) sysName = envUtf8(L"XR_RUNTIME_JSON");
    if (sysName.empty()) sysName = "Oculus Quest 3";
    std::snprintf(properties->systemName, sizeof(properties->systemName), "%s", sysName.c_str());
    return kXrSuccess;
}

}  // extern "C"
