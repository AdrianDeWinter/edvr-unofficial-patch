// F233 (native on-foot VR): the weapon preview screens' head-side producer. Constants only, header only, no Windows and no game: native_preview_camera.cpp places the hook,
// native_preview_camera_core.h takes the roster entry as a head.
//
// WHY (reverse engineering of build 334925, inbox 20261109-132023-weapon-head-producer, "RE 6" in docs\vr-livery-model-placement-2026-11-09.md). The weapons purchase and the loadout weapon preview
// have the Location child's roster [preview, HeadlookCamera]. HeadlookCamera (interface vtable 0x518C350, object vtable 0x518C2E0 at entry-0x20) is HMDCamera's sibling: same base class and layout
// (primary +0x50, local +0xD0, back-pointer +0x120, enabled byte entry-4, weight 0x8BEA20 and state 0x5294E0 at interface +0x20/+0x28). Its producer is 0x1074650, reached by a thunk with rcx the
// object (HMDCamera's is 0x1074140). It sets primary = R x base + d from the look input (R at +0x250/+0x260/+0x270, d at +0x280), so with idle look primary == base, and it hands no base on.
//
// The addresses are exact-build only: the producer is hooked after the build identity is known (the pin below is checked first), and the two vtables are taken from the same identity.
#pragma once

#include <cstddef>
#include <cstdint>

namespace edvr {
namespace previewcam {
namespace headlook {

constexpr uint32_t kProducerRva = 0x1074650, kIfaceRva = 0x518C350, kObjectRva = 0x518C2E0;
constexpr size_t kProducerPinBytes = 15;
inline constexpr uint8_t kProducerPin[kProducerPinBytes] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x60};
constexpr uint32_t kOffRotation0 = 0x250, kOffRotation1 = 0x260, kOffRotation2 = 0x270, kOffDisplacement = 0x280;   // the look rotation's rows and the displacement

}  // namespace headlook
}  // namespace previewcam
}  // namespace edvr
