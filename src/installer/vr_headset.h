// Multi-platform VR headset and OpenXR runtime detection for the launcher's
// Play workspace readout.
//
// ED-UP's VR bridge (openvr_api.dll) targets the system's active OpenXR
// runtime, so detection inspects five non-intrusive layers:
//   1. Active and available OpenXR runtimes (XR_RUNTIME_JSON and Khronos registry)
//   2. Running VR runtime services and wireless bridges (SteamVR, Meta Link,
//      Virtual Desktop, ALVR, Pimax Play, WMR, Varjo, Vive, WiVRn, Monado)
//   3. USB PnP headset hardware (known VR headset vendor/product IDs)
//   4. Live OpenXR headset query (ONLY when the active runtime's service is
//      already running, so opening the launcher never auto-boots SteamVR)
//   5. SteamVR / OpenVR driver config (openvrpaths.vrpath) for bridge hints
//
// Classification and status resolution are pure functions over VrProbeInputs
// so every platform route and state transition is unit-tested directly.
#pragma once

#include <string>
#include <vector>

namespace edvr::installer {

enum class VrStatusKind {
    Checking,
    Ready,
    Standby,
    RuntimeInactive,
    NotConfigured,
    Error,
};

enum class VrPlatform {
    Unknown,
    SteamVr,
    MetaLink,
    VirtualDesktopVdxr,
    SteamLink,
    Alvr,
    Pimax,
    WindowsMixedReality,
    Varjo,
    Vive,
    Monado,
    Wivrn,
    OpenComposite,
    Custom,
};

struct OpenXrManifestInfo {
    bool valid = false;
    std::wstring jsonPath;
    std::wstring runtimeName;
    std::wstring libraryPath;
    bool jsonExists = false;
    bool libraryExists = false;
};

struct UsbHeadsetMatch {
    std::wstring name;
    VrPlatform preferredPlatform = VrPlatform::Unknown;
};

struct OpenXrLiveProbe {
    bool attempted = false;
    bool loaderFound = false;
    bool instanceCreated = false;
    bool headsetPresent = false;
    int resultCode = 0;  // 0 == XR_SUCCESS
    std::wstring runtimeName;
    std::wstring runtimeVersion;
    std::wstring systemName;
};

struct VrProbeInputs {
    bool underWine = false;
    std::wstring activeRuntimePath;
    bool activeRuntimeFromEnv = false;
    OpenXrManifestInfo activeManifest;
    std::vector<std::wstring> availableRuntimePaths;
    std::vector<std::string> runningProcesses;   // lowercase exe/comm names
    std::vector<std::wstring> openVrDrivers;     // external_drivers paths from openvrpaths.vrpath
    bool steamVrInstalled = false;
    UsbHeadsetMatch usbHeadset;
    OpenXrLiveProbe liveProbe;
};

struct VrHeadsetReport {
    VrStatusKind status = VrStatusKind::Checking;
    VrPlatform activePlatform = VrPlatform::Unknown;
    VrPlatform runningBridge = VrPlatform::Unknown;
    VrPlatform mismatchPlatform = VrPlatform::Unknown;
    bool runtimeMismatch = false;
    bool headsetFromUsb = false;
    bool activeRuntimeMissingFile = false;
    bool activeLibraryMissingFile = false;
    int openXrResult = 0;

    std::wstring headsetName;          // e.g. "Meta Quest 3", "Valve Index", empty when none
    std::wstring runtimeDisplayName;   // e.g. "Virtual Desktop (VDXR)", "SteamVR", empty when none
    std::wstring runtimeVersion;       // e.g. "2.8.4" when live OpenXR reported one
    std::wstring transportName;        // e.g. "Virtual Desktop", "ALVR", "Steam Link", "USB"
    std::wstring mismatchRuntimeName;  // other running runtime when runtimeMismatch is true
};

// Pure helpers (unit-tested directly in launcher_test).
VrPlatform classifyVrPlatform(const std::wstring& pathOrName, const std::wstring& libraryPath = L"");
std::wstring vrPlatformDisplayName(VrPlatform platform, const std::wstring& customName = L"");
OpenXrManifestInfo parseOpenXrRuntimeJson(const std::string& jsonUtf8, const std::wstring& jsonPath);
std::vector<std::wstring> parseOpenVrExternalDrivers(const std::string& vrpathUtf8);
UsbHeadsetMatch identifyUsbVrHeadset(const std::wstring& instanceId, const std::wstring& description);
bool isPlatformServiceRunning(VrPlatform platform, const std::vector<std::string>& lowerProcesses);
bool shouldRunLiveOpenXrProbe(VrPlatform activePlatform, const std::vector<std::string>& lowerProcesses,
                              bool underWine, bool activeFromEnv);
VrHeadsetReport resolveVrHeadsetStatus(const VrProbeInputs& in);

// Live system probe (runs safely on a worker thread; never auto-starts SteamVR).
// `gameDir` and `payloadDir` are optional search hints for openxr_loader.dll.
VrHeadsetReport probeVrHeadset(const std::wstring& gameDir, const std::wstring& payloadDir);

}  // namespace edvr::installer
