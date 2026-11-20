// The VR Galaxy Map's hover point and the two relays that carry it (docs/galaxy-map-vr-2026-11-03.md, Fix 3). Pure: no device, no game memory, no log.
//
// THE POINT. The game hovers a star by projecting the pointer through the same projector it uses for every star and comparing viewport x, y. The
// projector (K) treats its input as a world position. The arrow is drawn in the UI frame, about one unit from the camera; the stars are 53 to 120 map
// units away. So the hover is replaced by a point 100000 units out along the arrow's direction, taken in the map camera's axes: at that distance the
// projection depends on direction only, and the star under the arrow is the star selected, whatever the head does.
//
// THE RAY'S ORIGIN (docs/galaxy-map-vr-2026-11-03.md, "The ray from the head as the map opened"). The UI frame's origin is the tracking origin, the place
// the view was last recentred, not the head: in dump 141733701001 its two cameras sat at x -0.038 and +0.017, the head 1 cm to the left of it with half the
// 0.055 baseline either side. A direction taken from that origin is exact from there only. Over a panel the arrow at star depth then misses the button in
// each eye by that eye's offset from the line through the origin and the button, over the button's 1.04 units: someone sitting 2.75 cm to the right of
// where they recentred sees it on the button in the left eye and 3 degrees off in the right. So the direction is taken from a fixed point instead: the
// midpoint of the two eyes as the map opened (HeadLatch), held for that map. Being fixed, it keeps the stars still under a still arrow while the head
// moves, as the origin did; a live head would bring back the drift Fix 3 removed.
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace edvr {
namespace mappointer {

constexpr float kFarDistance = 100000.0f;
// The arrow's drawn position, measured in the UI frame: |P| = 1.03. Anything outside this band is not the arrow.
constexpr float kArrowMin = 0.1f;
constexpr float kArrowMax = 10.0f;

enum class Why : uint8_t { kOk, kBadPose, kBadArrow };

inline bool finite(float v) { return std::isfinite(v); }

// The map camera's rows (right, up, forward as the publisher reads them) and position, the arrow P and the ray's origin F in the UI frame (all zero
// before the latch: the tracking origin) and the game's fourth lane. out is the point to project instead: position + (F + 100000 * dir) taken on the
// rows, dir = (P - F) / |P - F|, out[3] = w.
inline Why farPoint(const float rows[3][3], const float position[3], const float arrow[3], const float from[3], float w, float out[4]) {
    for (int i = 0; i < 3; ++i) {
        if (!finite(position[i]) || !finite(from[i]) || !finite(arrow[i])) return finite(arrow[i]) ? Why::kBadPose : Why::kBadArrow;
        for (int j = 0; j < 3; ++j)
            if (!finite(rows[i][j])) return Why::kBadPose;
    }
    const float rel[3] = {arrow[0] - from[0], arrow[1] - from[1], arrow[2] - from[2]};
    const float length = std::sqrt(rel[0] * rel[0] + rel[1] * rel[1] + rel[2] * rel[2]);
    if (!(length > kArrowMin && length < kArrowMax)) return Why::kBadArrow;
    float p[3];
    for (int i = 0; i < 3; ++i) p[i] = from[i] + kFarDistance * (rel[i] / length);
    for (int k = 0; k < 3; ++k)
        out[k] = position[k] + (p[0] * rows[0][k] + p[1] * rows[1][k] + p[2] * rows[2][k]);
    out[3] = w;
    return Why::kOk;
}

// THE HELD POINTER, the System Map's click (docs/galaxy-map-vr-2026-11-03.md, "The System Map's click"). The game keeps the pointer's viewport x, y at the button's press and selects at its release only when the two are exactly equal, bit for bit. With a mouse at rest that holds in flat. In VR the arrow's
// position is smoothed through the head and differs every frame, so a click was never a click. So the answer is held: while the map's camera has not moved and the arrow's direction is within kHoldRadians of the direction it was taken at, the same answer is returned, and the mouse decides when it changes.
// Measured: the arrow's direction wanders by about 5e-6 radians over 19 frames of a near-still head (dump 050358167001) and by 1.7e-4 while the head yaws 8.7 degrees (dump 061416462001). The pick's own radius is at least 0.03 of the viewport's height, far above this.
constexpr float kHoldRadians = 5e-4f;
struct PointerHold {
    bool have = false;
    float dir[3] = {};
    uint64_t changes = 0;
    float out[4] = {};
    uint64_t ret = 0;
};
inline bool arrowDirection(const float arrow[3], float d[3]) {
    const float length = std::sqrt(arrow[0] * arrow[0] + arrow[1] * arrow[1] + arrow[2] * arrow[2]);
    if (!(length > kArrowMin && length < kArrowMax)) return false;
    for (int i = 0; i < 3; ++i) d[i] = arrow[i] / length;
    return true;
}
inline bool holdApplies(const PointerHold& h, const float arrow[3], uint64_t changes) {
    float d[3];
    if (!h.have || h.changes != changes || !arrowDirection(arrow, d)) return false;
    const float dx = d[0] - h.dir[0], dy = d[1] - h.dir[1], dz = d[2] - h.dir[2];
    return dx * dx + dy * dy + dz * dz < kHoldRadians * kHoldRadians;
}
inline void holdTake(PointerHold& h, const float arrow[3], uint64_t changes, const float out[4], uint64_t ret) {
    h.have = arrowDirection(arrow, h.dir);
    h.changes = changes;
    std::memcpy(h.out, out, sizeof(h.out));
    h.ret = ret;
}

// The arrow's direction from the ray's origin F (THE RAY'S ORIGIN above), for the System Map's body under the arrow. The same band as the far point.
inline bool arrowDirectionFrom(const float arrow[3], const float from[3], float d[3]) {
    const float rel[3] = {arrow[0] - from[0], arrow[1] - from[1], arrow[2] - from[2]};
    return arrowDirection(rel, d);
}

// THE HEAD AS THE MAP OPENED. The eyes are the UI frame's two cameras' origins (flat_stationary_camera_capture.h mapCameraEyes): the two most recent
// distinct ones, kept across maps, so at an open they can be the last map's until two more have been written (writes alternate the slots). A pair is two
// eyes only when they are kLatchMinApart to kLatchMaxApart apart: 0.055 measured, the IPD and the world scale (50 to 200 percent) moving it within that;
// two readings of one eye are millimetres apart. Once taken, the point is held until the map closes. A recentre moves the UI frame's origin to the head,
// so the midpoint then lands within millimetres of zero in one step: a reading within kRecentreNear of the origin that is more than kRecentreJump from
// either of the two readings before it (or from the point, at the first) drops the point and takes it again from the next two writes. Two readings back,
// because a reading taken between the two eyes' refreshes is half way. A head drifting onto the origin moves millimetres a frame and is not a recentre;
// one that sat within kRecentreJump of the origin when it recentred is not seen, and keeps a point that far off. Pure; the frame thread's.
constexpr float kLatchMinApart = 0.01f;
constexpr float kLatchMaxApart = 0.25f;
constexpr float kRecentreJump = 0.02f;
constexpr float kRecentreNear = 0.005f;
constexpr uint64_t kLatchFreshWrites = 2;
struct HeadLatch {
    bool open = false;
    bool have = false;          // at holds the point
    uint64_t sinceGen = 0;      // the eyes' generation the next point must be two writes past
    uint32_t history = 0;       // accepted readings in last (1) and prev (2) since the point was taken
    float last[3] = {};
    float prev[3] = {};
    float at[3] = {};           // the point; zero while none is held
    float apart = 0.0f;         // the eyes' distance when it was taken
    uint32_t taken = 0;         // points taken, ever
    uint32_t afterRecentre = 0; // recentres seen, each dropping the point
    uint64_t refused = 0;       // readings that were not two eyes
};
inline void latchClear(HeadLatch& h) {
    h.have = false;
    h.history = 0;
    for (float& v : h.at) v = 0.0f;
}
inline float latchDistance(const float a[3], const float b[3]) {
    const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return std::sqrt(x * x + y * y + z * z);
}
// The map opened: generation is the eyes' as it stands (0 when no pair has been seen).
inline void latchOpen(HeadLatch& h, uint64_t generation) {
    latchClear(h);
    h.open = true;
    h.sinceGen = generation;
}
inline void latchClose(HeadLatch& h) {
    latchClear(h);
    h.open = false;
}
// One frame's reading of the eyes (ok false when there is no pair). True when the point changed: taken, or dropped at a recentre.
inline bool latchOffer(HeadLatch& h, bool ok, const float eyes[2][3], uint64_t generation) {
    if (!h.open || !ok) return false;
    float mid[3], apart2 = 0.0f;
    for (int i = 0; i < 3; ++i) {
        if (!finite(eyes[0][i]) || !finite(eyes[1][i])) { ++h.refused; return false; }
        mid[i] = 0.5f * (eyes[0][i] + eyes[1][i]);
        const float d = eyes[1][i] - eyes[0][i];
        apart2 += d * d;
    }
    if (!(apart2 >= kLatchMinApart * kLatchMinApart && apart2 <= kLatchMaxApart * kLatchMaxApart)) { ++h.refused; return false; }
    if (h.have) {
        // The two readings before this one: last and prev, with the point's own reading standing in for those not made yet.
        const float zero[3] = {};
        const float step = (std::max)(latchDistance(mid, h.history >= 1 ? h.last : h.at), latchDistance(mid, h.history >= 2 ? h.prev : h.at));
        const bool nearOrigin = latchDistance(mid, zero) < kRecentreNear;
        std::memcpy(h.prev, h.last, sizeof(mid));
        std::memcpy(h.last, mid, sizeof(mid));
        if (h.history < 2) ++h.history;
        if (nearOrigin && step > kRecentreJump) {
            latchClear(h);
            h.sinceGen = generation;
            ++h.afterRecentre;
            return true;
        }
        return false;
    }
    if (generation < h.sinceGen + kLatchFreshWrites) return false;
    std::memcpy(h.at, mid, sizeof(mid));
    h.apart = std::sqrt(apart2);
    h.have = true;
    ++h.taken;
    return true;
}

// The live head: the midpoint of a pair that is two eyes (the latch's band). While the arrow is on a panel it is the game's own, drawn at the panel's depth from each eye,
// so the star that shows under it is the one along P - head: the hover takes that (docs/galaxy-map-vr-2026-11-03.md, "Flight, the latched ray"). False when the pair is not two eyes.
inline bool liveHead(const float eyes[2][3], float out[3]) {
    float apart2 = 0.0f;
    for (int i = 0; i < 3; ++i) {
        if (!finite(eyes[0][i]) || !finite(eyes[1][i])) return false;
        const float d = eyes[1][i] - eyes[0][i];
        apart2 += d * d;
    }
    if (!(apart2 >= kLatchMinApart * kLatchMinApart && apart2 <= kLatchMaxApart * kLatchMaxApart)) return false;
    for (int i = 0; i < 3; ++i) out[i] = 0.5f * (eyes[0][i] + eyes[1][i]);
    return true;
}

// The point as the other threads read it (the projector's detour runs on the game's update thread): one writer, readers behind a sequence count. A
// reader that cannot settle in eight tries takes zero, the tracking origin, for that one call.
class SharedPoint {
public:
    void store(const float p[3]) noexcept {
        uint32_t w[3];
        std::memcpy(w, p, sizeof(w));
        const uint32_t s = seq_.load(std::memory_order_relaxed);
        seq_.store(s + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        for (int i = 0; i < 3; ++i) words_[i].store(w[i], std::memory_order_relaxed);
        seq_.store(s + 2, std::memory_order_release);
    }
    void load(float out[3]) const noexcept {
        for (int tries = 0; tries < 8; ++tries) {
            const uint32_t s1 = seq_.load(std::memory_order_acquire);
            if (s1 & 1u) continue;
            uint32_t w[3];
            for (int i = 0; i < 3; ++i) w[i] = words_[i].load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (seq_.load(std::memory_order_relaxed) != s1) continue;
            std::memcpy(out, w, sizeof(w));
            return;
        }
        for (int i = 0; i < 3; ++i) out[i] = 0.0f;
    }
private:
    std::atomic<uint32_t> seq_{0};
    std::atomic<uint32_t> words_[3] = {};
};

// Whether the System Map's state is on: the pointer is armed, the build's System Map return was found, a map is open and it is not the Galaxy Map.
// Without the return the System Map stays as the game made it (no marker or reticule swap, no body table, no close-view smoothing).
inline bool systemMapOn(bool galaxyOpen, bool armed, uint64_t systemReturn, bool anyMapOpen) {
    return !galaxyOpen && armed && systemReturn != 0 && anyMapOpen;
}

// rel in the map's axes: a = rel . rows[0], b = rel . rows[1], c = rel . rows[2] (the rig checks the far point's direction with it).
inline void inMapAxes(const float rows[3][3], const float rel[3], float out[3]) {
    for (int i = 0; i < 3; ++i) out[i] = rel[0] * rows[i][0] + rel[1] * rows[i][1] + rel[2] * rows[i][2];
}

// ---- the generated code ------------------------------------------------------------------------------------------------------------------------------
// Both relays jump, never call, so the detour's _ReturnAddress() is the game's caller. Each ends in `jmp [rip+0]` over an 8-byte cell the CodeHook
// prepare callback fills with the trampoline.

// The gate-first relay: mov rax, gate; cmp qword [rax], 0; je trampoline; jmp [callback]; jmp [trampoline]. 44 bytes; the trampoline cell is at 36.
constexpr uint32_t kGateRelayBytes = 44;
constexpr uint32_t kGateRelayTrampolineAt = 36;
inline void buildGateRelay(uint8_t* code, const void* gate, const void* callback) {
    const uint8_t body[kGateRelayBytes] = {
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0x48, 0x83, 0x38, 0,
        0x74, 0x0E, 0xFF, 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0xFF, 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    std::memcpy(code, body, sizeof(body));
    const uint64_t g = reinterpret_cast<uintptr_t>(gate), c = reinterpret_cast<uintptr_t>(callback);
    std::memcpy(code + 2, &g, 8);
    std::memcpy(code + 22, &c, 8);
}

// The capture stub: mov rax, slot; mov [rax], rcx; jmp [trampoline]. rax is scratch at a function's entry; every argument register is untouched.
// 27 bytes; the trampoline cell is at 19.
constexpr uint32_t kCaptureStubBytes = 27;
constexpr uint32_t kCaptureStubTrampolineAt = 19;
inline void buildCaptureStub(uint8_t* code, const void* slot) {
    const uint8_t body[kCaptureStubBytes] = {
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0x48, 0x89, 0x08,
        0xFF, 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    std::memcpy(code, body, sizeof(body));
    const uint64_t s = reinterpret_cast<uintptr_t>(slot);
    std::memcpy(code + 2, &s, 8);
}

} // namespace mappointer
} // namespace edvr
