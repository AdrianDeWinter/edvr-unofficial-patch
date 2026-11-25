// Multi-platform VR headset and OpenXR runtime detection for the launcher's
// Play workspace readout, Play VR pre-flight check, and Save logs snapshot.
//
// ED-UP's VR bridge (openvr_api.dll) targets the system's active OpenXR
// runtime (or the local edup_openxr.ini runtime= setting parsed with EDVR's
// own native_local_config.h). Detection inspects five layers:
//   1. Effective OpenXR runtime (edup_openxr.ini via native_local_config.h,
//      Khronos registry ActiveRuntime, XR_RUNTIME_JSON matching Elite's rules)
//   2. Running VR runtime services and wireless bridges (SteamVR, Meta Link,
//      Virtual Desktop, ALVR, Pimax Play, WMR, Varjo, Vive, WiVRn, Monado)
//   3. Active OpenVR routing check (detects OpenComposite openvr_api.dll)
//   4. USB PnP headset hardware (known VR headset vendor/product IDs)
//   5. Optional out-of-process live OpenXR query spawned in a short-lived child
//      process using ONLY a cryptographically verified openxr_loader.dll,
//      killed on timeout or shutdown, never run while elevated or playing,
//      and stopped for the session after one timeout
//
// Classification, runtime selection, probe scheduling, preflight evaluation,
// path redaction, and status resolution are pure functions unit-tested in
// tools/launcher_test/launcher_test.cpp.
#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "../openxr/native_local_config.h"
#include "launcher_core.h"

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
    Custom,
};

enum class EffectiveRuntimeSource {
    None,
    Registry,
    EnvVar,
    LocalExplicit,
    LocalConfigInvalid,
};

enum class ChildXrEnvAction {
    Inherit,
    ClearXrRuntimeJson,
    SetXrRuntimeJson,
};

struct EffectiveRuntimeSelection {
    EffectiveRuntimeSource source = EffectiveRuntimeSource::None;
    std::wstring activeRuntimePath;
    bool activeRuntimeFromEnv = false;
    bool localConfigInvalid = false;
    bool envDisagreesWithLocal = false;
    ChildXrEnvAction childEnvAction = ChildXrEnvAction::Inherit;
    std::wstring childEnvRuntimePath;
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
    EffectiveRuntimeSource runtimeSource = EffectiveRuntimeSource::None;
    std::wstring activeRuntimePath;
    std::wstring linuxHostRuntimePath;        // host Linux active_runtime.json path (informational / wineopenxr hint)
    std::wstring linuxHostRuntimeName;        // parsed runtime.name from host Linux active_runtime.json, if any
    std::wstring linuxHostRuntimeHint;        // host Linux active_runtime.json text hint behind Proton wineopenxr
    bool activeRuntimeFromEnv = false;
    bool localConfigInvalid = false;
    bool envDisagreesWithLocal = false;
    bool openVrShimIsOpenComposite = false;  // selected install's active openvr_api.dll is OpenComposite
    OpenXrManifestInfo activeManifest;
    std::vector<std::wstring> availableRuntimePaths;
    std::vector<std::string> runningProcesses;   // lowercase exe/comm/ipc names
    std::vector<std::wstring> openVrDrivers;     // external_drivers paths from openvrpaths.vrpath
    UsbHeadsetMatch usbHeadset;
    OpenXrLiveProbe liveProbe;
};

struct VrHeadsetReport {
    VrStatusKind status = VrStatusKind::Checking;
    EffectiveRuntimeSource runtimeSource = EffectiveRuntimeSource::None;
    VrPlatform activePlatform = VrPlatform::Unknown;
    VrPlatform runningBridge = VrPlatform::Unknown;
    bool runtimeMismatch = false;
    bool openCompositeInstalled = false;
    bool localConfigInvalid = false;
    bool envDisagreesWithLocal = false;
    bool headsetFromUsb = false;
    bool activeRuntimeMissingFile = false;
    bool activeLibraryMissingFile = false;
    bool hasLiveResult = false;
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
    std::wstring linuxHostRuntimeName;  // host Linux OpenXR runtime detected via XDG when Wine has no active runtime
    std::wstring linuxHostRuntimePath;  // redacted when written to vr-environment.txt
    std::vector<std::wstring> availableRuntimes;
    std::vector<std::string> runningVrServices;  // matched VR services only (never arbitrary user processes)
};

// Pre-flight confirmation warnings shown before launching Elite in VR.
// Only genuine missing-runtime / missing-file / invalid local config conditions warn;
// runtime mismatches, OpenComposite, and live probe errors are informational in the card.
enum class VrPreflightWarning {
    None,
    NoRuntime,
    MissingRuntimeFile,
    MissingRuntimeLibrary,
    InvalidLocalConfig,
};

enum class VrProbeTrigger {
    PlayTabOpened,
    InstallSelected,
    ManualRefresh,
    PeriodicTimer,
};

struct VrProbeDecision {
    bool shouldRun = false;
    bool allowLiveProbe = false;
};

struct VrProbeOptions {
    std::wstring gameDir;
    // Optional --data-root passed to the launcher; empty means the canonical %LOCALAPPDATA%\EDVR root.
    // The out-of-process probe helper resolves openxr_loader.dll itself from the signed manifest under
    // this root and verifies it against the release signing keys before loading it.
    std::wstring dataRoot;
    bool hasSignedLoader = false;
    bool allowLiveProbe = false;
    bool isElevated = false;
    unsigned int liveProbeTimeoutMs = 2500;
    // Optional manual-reset event signaled on shutdown to terminate any in-flight child probe immediately.
    HANDLE cancelEvent = nullptr;
};

// Pure helpers (unit-tested directly in launcher_test).
VrPlatform classifyVrPlatform(const std::wstring& pathOrName, const std::wstring& libraryPath = L"");
std::wstring vrPlatformDisplayName(VrPlatform platform, const std::wstring& customName = L"");
bool isWineOpenXrBridgePath(const std::wstring& pathOrName, const std::wstring& libraryPath = L"");
bool isPosixSharedLibraryPath(const std::wstring& path);
std::vector<std::wstring> linuxOpenXrManifestCandidates(const std::wstring& xdgConfigHome,
                                                        const std::wstring& homeDir,
                                                        const std::wstring& xdgConfigDirs);
std::vector<std::wstring> linuxCompositorSocketPaths(const std::wstring& xdgRuntimeDir, VrPlatform platform);
EffectiveRuntimeSelection resolveEffectiveOpenXrRuntime(edvr::openxr::LocalConfigResult localResult,
                                                        const edvr::openxr::LocalConfig& localConfig,
                                                        const std::wstring& envRuntime,
                                                        const std::wstring& regRuntime, bool underWine);
VrProbeDecision decideVrProbe(VrProbeTrigger trigger, bool onPlayTab, bool minimized, bool playing,
                              bool gameRunning, bool elevated, bool liveProbeTimedOut);
OpenXrManifestInfo parseOpenXrRuntimeJson(const std::string& jsonUtf8, const std::wstring& jsonPath);
std::vector<std::wstring> parseOpenVrExternalDrivers(const std::string& vrpathUtf8);
UsbHeadsetMatch identifyUsbVrHeadset(const std::wstring& instanceId, const std::wstring& description);
bool isEliteRunning(const std::vector<std::string>& lowerProcesses);
bool isPlatformServiceRunning(VrPlatform platform, const std::vector<std::string>& lowerProcesses);
bool shouldRunLiveOpenXrProbe(VrPlatform activePlatform, const std::vector<std::string>& lowerProcesses,
                              bool underWine, bool activeFromEnv);
VrHeadsetReport resolveVrHeadsetStatus(const VrProbeInputs& in);
VrHeadsetReport mergeStaticRefreshWithLive(const VrHeadsetReport& freshStatic, const VrHeadsetReport& previous);
VrPreflightWarning evaluateVrLaunchPreflight(const VrHeadsetReport& report);
std::wstring redactUserPath(const std::wstring& path);
std::string formatVrEnvironmentSnapshot(const VrHeadsetReport& report);

// Signed loader lookup and out-of-process probe runner / child entrypoint.
// Resolves openxr_loader.dll from the signed manifest under `roots` (checking `<gameDir>\Openvr\win64`
// first, then the payload cache) and verifies its SHA-256 and 64-bit OpenXR export.
// `keys` nullptr uses the release signing keys (kUpdatePublicKeys).
bool resolveSignedOpenXrLoader(const LauncherRoots& roots, const std::wstring& gameDir,
                               const char* const* keys, std::wstring* pathOut, std::string* shaOut);
// `parseOpenXrProbeOutput` parses the key=value stdout produced by `runOpenXrProbeHelper`.
OpenXrLiveProbe parseOpenXrProbeOutput(const std::string& stdoutUtf8);
// Builds a sorted Windows Unicode environment block (double-NUL terminated) for the probe child.
std::vector<wchar_t> buildChildEnvironment(ChildXrEnvAction action, const std::wstring& runtimePath,
                                           bool* okOut = nullptr);
OpenXrLiveProbe runOpenXrProbeSubprocess(const std::wstring& exePath, const std::wstring& commandLine,
                                         ChildXrEnvAction envAction, const std::wstring& envRuntimePath,
                                         unsigned int timeoutMs, HANDLE cancelEvent = nullptr);
// Runs inside the short-lived `--probe-openxr` child process; resolves and verifies openxr_loader.dll
// from the signed manifest under `roots`, prints key=value lines to stdout, and returns 0.
// `elevated` is the caller's isElevatedForPlay(): an explicitly elevated launcher loads nothing.
int runOpenXrProbeHelper(const LauncherRoots& roots, const std::wstring& gameDir, bool elevated,
                         const char* const* keys = nullptr);

// System probe. With allowLiveProbe=false, completes using registry, EDVR's
// native_local_config.h parser, process snapshot, and USB scan without spawning
// a child process or loading any OpenXR DLL.
VrHeadsetReport probeVrHeadset(const VrProbeOptions& options);

}  // namespace edvr::installer
