// TEMPORARY (docs\concourse-cull-2026-11-05.md): the concourse camera test, the look test. Flight 22 showed that moving flat's main camera where it is made (after 0x2874290, on 0x281D190's
// stack, before any copy is taken) moves the whole picture. This puts it at the room's centre, turning through six directions (concourse_camera_test.cpp labels each offset, build 334925).
// 0x1EA9C0 is read only, for view 0. Flat only (the caller refuses the VR profile), installed only when hotkey.concourse_camera_test is bound; the relay gate is shut while the mode is 0,
// so unbound costs nothing.
// Remove with the concourse arc.
#pragma once

#include <cstdint>

namespace edvr {

struct ConcourseCameraTestSnap {
    static constexpr int kCalls = 2;   // 0x1EA9C0, 0x2874290
    uint32_t hookMask = 0;             // bit i: hook i is installed (the order above)
    uint64_t calls[kCalls] = {};
    uint64_t written = 0, refused = 0, otherSite = 0, noView0 = 0, multiView = 0, faults = 0;
    uint32_t rvCount = 0, mainCount = 0, flags = 0, slotFlags = 0, slotIndex = 0;
    bool slotKnown = false;
    float rvPos[3] = {}, slot[3] = {}, camBefore[3] = {}, camAfter[3] = {}, fwdBefore[3] = {}, fwdAfter[3] = {};
};

bool concourseCameraTestInstall(const char** why) noexcept;             // true when at least one hook is in; else false, and why (one sentence)
// mode 0 off, 1 log only, 2 the look, 3 the bake; acting: the camera may be written now
void concourseCameraTestLook(const float centre[3], const float up[3], uint32_t dir) noexcept;   // the room's centre and up axis, and the direction 0 to 5
// `busy` (the bake): a bake is armed or running. The hooks' gate is shut otherwise, so a bake costs nothing between arrivals.
void concourseCameraTestControl(uint32_t mode, bool acting, bool busy) noexcept;
void concourseCameraTestAim(const float centre[3], const float forward[3], const float up[3], float fovRad, float nearM) noexcept;   // the bake's frame: unit axes, vertical field of view in radians, near plane in metres
bool concourseCameraTestHooked() noexcept;
bool concourseCameraTestCanMove() noexcept;   // the producer hook (0x2874290) is in, so the look and the bake can move flat's camera
void concourseCameraTestSnapshot(ConcourseCameraTestSnap* out) noexcept;

// TEMPORARY (the VR bake): the near value at RVA 0x5E85B8C without the flat hooks. Resolve checks the build and the 0.025 read there (false with why). Raise reads the value
// (into `before`) and writes `nearM`, flat's ctNearRaise; Restore puts back the value the raise replaced (flat's ctNearRestore; the 0.025 is only the address check at resolve) and says whether it had been raised.
bool concourseNearValueResolve(const char** why) noexcept;
bool concourseNearValueRaise(float nearM, float* before) noexcept;
bool concourseNearValueRestore() noexcept;
float concourseNearValueRestoredTo() noexcept;

}  // namespace edvr
