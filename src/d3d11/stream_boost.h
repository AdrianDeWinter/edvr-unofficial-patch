#pragma once
// System-adaptive asset streaming boost for Elite Dangerous (build 334925).
//
// Lifts the three interacting streaming bottlenecks according to host storage,
// CPU cores, system RAM, active profile (flat vs VR), live frame-time headroom,
// and live VRAM eviction state. Stands down automatically on spinning HDDs,
// unknown game builds, or byte-pin mismatches, and restores patched bytes on
// shutdown.

#include <cstdint>

namespace edvr {

void streamBoostInstall() noexcept;
void streamBoostNoteFrame(bool flatProfile) noexcept;
void streamBoostPoll() noexcept;
void streamBoostShutdown() noexcept;

}  // namespace edvr
