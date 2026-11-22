// Multi-platform VR headset and OpenXR runtime detection for the launcher's
// Play workspace readout, Play VR pre-flight check, and Save logs snapshot.
//
// ED-UP's VR bridge (openvr_api.dll) targets the system's active OpenXR
// runtime (or the local edup_openxr.ini runtime= setting), so detection
// inspects five non-intrusive layers:
//   1. Effective OpenXR runtime (edup_openxr.ini runtime=system|path,
//      Khronos registry ActiveRuntime, XR_RUNTIME_JSON when applicable)
//   2. Running VR runtime services and wireless bridges (SteamVR, Meta Link,
//      Virtual Desktop, ALVR, Pimax Play, WMR, Varjo, Vive, WiVRn, Monado)
//   3. Active OpenVR routing check (detects OpenComposite openvr_api.dll)
//   4. USB PnP headset hardware (known VR headset vendor/product IDs)
//   5. Bounded, SEH-guarded live OpenXR query using ONLY a cryptographically
//      verified openxr_loader.dll when the active runtime service is already
//      running (never auto-launching SteamVR or loading unverified DLLs)
//
// Classification, preflight evaluation, path redaction, and status resolution
// are pure functions so every platform route and edge case is unit-tested.
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

enum class LocalOpenXrRuntimeMode {
    NotConfigured,  // no edup_openxr.ini / edvr_openxr.ini
    System,         // runtime=system (clears inherited XR_RUNTIME_JSON in Elite)
    ExplicitPath,   // runtime=<absolute path>
};

struct LocalOpenXrRuntimeConfig {
    LocalOpenXrRuntimeMode mode = LocalOpenXrRuntimeMode::NotConfigured;
    std::wstring explicitPath;
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
    bool timedOut = false;
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
    bool openVrShimIsOpenComposite = false;  // selected install's active openvr_api.dll is OpenComposite
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
    bool openCompositeWarning = false;
    bool headsetFromUsb = false;
    bool activeRuntimeMissingFile = false;
    bool activeLibraryMissingFile = false;
    bool liveProbeTimedOut = false;
    bool underWine = false;
    bool activeRuntimeFromEnv = false;
    int openXrResult = 0;

    std::wstring headsetName;          // e.g. "Meta Quest 3", "Valve Index", empty when none
    std::wstring runtimeDisplayName;   // e.g. "Virtual Desktop (VDXR)", "SteamVR", empty when none
    std::wstring runtimeVersion;       // e.g. "2.8.4" when live OpenXR reported one
    std::wstring transportName;        // e.g. "Virtual Desktop", "ALVR", "Steam Link"
    std::wstring mismatchRuntimeName;  // other running runtime when runtimeMismatch is true
    std::wstring activeRuntimePath;
    std::wstring activeLibraryPath;
    std::vector<std::wstring> availableRuntimes;
    std::vector<std::string> runningVrServices;  // matched VR services only (never arbitrary user processes)
};

enum class VrPreflightWarning {
    None,
    NoRuntime,
    MissingRuntimeFile,
    RuntimeMismatch,
    OpenComposite,
    RuntimeError,
};

struct VrProbeOptions {
    std::wstring gameDir;
    // Controlled, cryptographically verified openxr_loader.dll path and its
    // expected lowercase hex SHA-256 from the signed release manifest. Live
    // OpenXR probing only loads a DLL when both are non-empty and match on disk.
    std::wstring verifiedLoaderPath;
    std::string verifiedLoaderSha256;
    bool allowLiveProbe = true;
    unsigned int liveProbeTimeoutMs = 2500;
};

// Pure helpers (unit-tested directly in launcher_test).
VrPlatform classifyVrPlatform(const std::wstring& pathOrName, const std::wstring& libraryPath = L"");
std::wstring vrPlatformDisplayName(VrPlatform platform, const std::wstring& customName = L"");
LocalOpenXrRuntimeConfig parseLocalOpenXrRuntimeConfig(const std::string& iniUtf8);
OpenXrManifestInfo parseOpenXrRuntimeJson(const std::string& jsonUtf8, const std::wstring& jsonPath);
std::vector<std::wstring> parseOpenVrExternalDrivers(const std::string& vrpathUtf8);
UsbHeadsetMatch identifyUsbVrHeadset(const std::wstring& instanceId, const std::wstring& description);
bool isPlatformServiceRunning(VrPlatform platform, const std::vector<std::string>& lowerProcesses);
bool shouldRunLiveOpenXrProbe(VrPlatform activePlatform, const std::vector<std::string>& lowerProcesses,
                              bool underWine, bool activeFromEnv);
VrHeadsetReport resolveVrHeadsetStatus(const VrProbeInputs& in);
VrPreflightWarning evaluateVrLaunchPreflight(const VrHeadsetReport& report);
std::wstring redactUserPath(const std::wstring& path);
std::string formatVrEnvironmentSnapshot(const VrHeadsetReport& report);

// System probe. With allowLiveProbe=false, completes quickly using registry,
// local install config, process snapshot, and USB scan without loading any DLL.
VrHeadsetReport probeVrHeadset(const VrProbeOptions& options);

}  // namespace edvr::installer
