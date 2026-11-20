#pragma once
namespace edvr {
// Present thread: arm the pinned camera wrappers, publish the native on-foot
// gate and drain bounded default diagnostics. No configuration key of its own.
void nativePreviewCameraBoundary(bool wanted,bool active);
void nativePreviewCameraStop() noexcept;
// F232, present thread, once a frame: while one of F209's watched preview screens reads open (screenOpen) the producer hooks keep a roster snapshot of the last child they refused, and once a second
// each role's newest snapshot is logged with the per-reason counts (at most 40 lines in any ten seconds). No effect on the camera.
void nativePreviewCameraDiagTick(bool screenOpen);
// TEMPORARY (the concourse cull's VR proving build, hotkey.concourse_vr_camera_probe; docs\concourse-cull-2026-11-05.md): a second client of the eye stub on 0x283FA20.
// Arm: for `frames` consecutive present-thread frames after `frame` (the press's own), the first call of the stub in each frame raises the head pose's translation by
// `offset` (metres). Installs the stub on demand when the ship preview has not. False with `why` when it cannot (not installed, already running, a bad count).
bool nativeCameraProbeArm(const float offset[3],unsigned frames,unsigned frame,const char** why);
// The present thread, once a frame while the probe runs: logs the window that ended, opens the next.
void nativeCameraProbeTick(unsigned frame);
bool nativeCameraProbeRunning() noexcept;
// For the tone draw's lines: 0 outside the probe, 1 the press's own frame (before), 2 a frame that writes, 3 the frame after. Wrote: whether that frame's first call was written.
unsigned nativeCameraProbePhase(unsigned frame) noexcept;
bool nativeCameraProbeWrote(unsigned frame) noexcept;

// TEMPORARY (the concourse cull's VR bake, optimized_station.cpp; docs\concourse-cull-2026-11-05.md): a third client of the eye stub. The stub's pose is the base B Elite's eye
// cameras are built on (concourse_vr_bake_core.h). Available installs the stub on demand (false with why). Observe arms the client without writing: each window records the
// first call's pose as Elite left it. Aim publishes the 16 floats of a base (Elite's rows, four of four; lane 3 is not written) and writes it into every call of each window
// that carries Elite's base (a stack address, not the identity) until StopWriting. Tick, once a frame on the present thread: the window that ended.
struct NativeCameraBakeWindow {
    static constexpr unsigned kCalls = 6;
    bool armed = false;
    unsigned calls = 0, stackCalls = 0, wroteMask = 0, stackMask = 0, torn = 0, foreign = 0;   // foreign: calls refused, their own pose is not the base the aim expects
    bool firstSeen = false, firstStack = false, firstReadable = false;
    float firstBefore[16] = {};      // the window's first call, as Elite left it (its base)
    float callT[kCalls][3] = {};     // each of the first calls' translation before
};
bool nativeCameraBakeAvailable(const char** why);
void nativeCameraBakeObserve(bool on) noexcept;
// expected: the base Elite is using (the latest window's first call before); a call is written only if its own pose matches it. Null writes nothing. Each Aim is a lease for one
// window: nativeCameraBakeTick ends it, so the caller aims again every tick it wants written.
void nativeCameraBakeAim(const float pose[16], const float expected[16]) noexcept;
void nativeCameraBakeStopWriting() noexcept;
bool nativeCameraBakeWriting() noexcept;
void nativeCameraBakeTick(NativeCameraBakeWindow* out) noexcept;
}
