#pragma once
// The concourse cull's VR bake (TEMPORARY, docs\concourse-cull-2026-11-05.md, "Flight 51 results" and after): its pure half. The flat bake moves flat's one camera to the room's
// centre (concourse_camera_test.cpp); in VR the eye cameras are built by 0x283FA20 from the base the eye stub sees (native_preview_camera.cpp), so the bake writes that base.
// Nothing here touches D3D, the game or a lock: tools\optimized_station_test checks every helper.
//
// THE CONVENTIONS, as read in the code (build 334925 unless said):
// - Elite's rows. A pose is four rows of four floats (previewcam::Pose: v[r * 4 + c]); rows 0 to 2 are the basis, row 3 the translation, and lane 3 of each row is not Elite's.
//   A point goes p' = p * M (row vectors). previewcam::compose(local, base) is Elite's multiply, local first (native_preview_camera_core.h; 0x283FA20 composes its eye
//   matrix with the base exactly so, analysis\decomp\decomp_283FA20_334925.txt: eye row r = M row r times the base's 3x3, plus the base's row 3 for r = 3).
// - The eye cameras. 0x283FA20 builds eye_i = M_i * B, with B its pose argument (the stub's pose: 0x2845730's stack copy of W+0x3330) and M_i Elite's own eye matrix, which
//   0x4E2640 makes as E_i * H (scratchpad dis_4E2640.txt: GetEyeToHeadTransform at vfn+0x20, then the head, then each row of E times H's rows plus H's row 3 for row 3).
// - The camera. 0x287B330 takes forward = normalize(row 2), right = normalize(row 1 x row 2), up = forward x right, position = row 3 (dis_287B330.txt), and the view looks
//   along +forward (flat's camera, the same object: "Flight 24: the row at the room's up looked up"). So a frame's rows are right, up, forward with right = up x forward,
//   which is the cyclic order of a proper rotation (row 0 x row 1 = row 2).
// - The head. The runtime hands Elite OpenVR's 3x4 (column vectors, x right, y up, z back). Elite converts the head to S*H*S, S = diag(1, 1, -1), and takes the eye-to-head raw,
//   so EDVR hands it S*E*S (openvr_system.cpp GetEyeToHeadTransform; docs\canted-projection.md 2026-10-09: 0x4E3690 emulated equals S*H*S, the product is E @ H). In Elite's rows
//   either becomes G(X) = (S X S) transposed: G(X)[i][j] = s_i s_j X[j][i], row 3 = (t_x, t_y, -t_z). G(X * E) = G(E) * G(X), so eye_i = G(E_i) * G(H) * B.
// - The aim. For a face with rows D (right, up, forward, the centre), B = G(H)^-1 * D gives eye_i = G(E_i) * D while Elite's head is the H read: each eye sits at the centre
//   less its own eye-to-head offset, looking along the face. Elite multiplies in a head a frame newer, so the frame's own rows (cb1 270..279) are what the capture uses.
#include <cmath>
#include <cstdint>

#include "native_preview_camera_core.h"   // previewcam::Pose and compose: Elite's row multiply

namespace edvr {
namespace concourse_vr {

using previewcam::Pose;

constexpr uint32_t kFaces = 5;                  // the flat bake's five directions (concourse_cull_core.h kBakeDirs)
constexpr float kFaceSlack = 1.002f;            // a texel on a face's edge is taken by both faces' frames: no seam (the flat test's 0.2 percent)
constexpr float kAtCentreM = 0.5f;              // a frame whose eye row 275 is farther than this from the cube's origin is not a bake frame
constexpr float kFaceMinDot = 0.82f;            // a frame looking more than about 35 degrees off every wanted face writes nothing (cos 35)
constexpr float kWorldOffsetM = 0.5f;           // the world check: render less store past this is taken as an offset
constexpr float kCoveredFrac = 0.99f;           // a face whose frames covered this share of its texels is done
constexpr uint32_t kAimsPerFace = 3;            // the first aim and two more, then a face that is still short is let go
constexpr uint32_t kVrBakeDwell = 3;            // frames held on a new aim: the room's upper structure flickers on some frames (Flight 53, H62), and the cube keeps the union of a face's frames
constexpr uint32_t kReadbackTicks = 7;          // an aim's frame, its capture and the read-back of its counts take about this many ticks
constexpr float kPitchMaxRad = 0.35f;           // about 20 degrees: a frustum that needs more is not centred, it is logged

inline Pose identityPose() noexcept {
    Pose p;
    p.v[0] = p.v[5] = p.v[10] = p.v[15] = 1.0f;
    return p;
}
// a * b in Elite's rows (a first): previewcam::compose with the identity's lanes.
inline Pose mul(const Pose& a, const Pose& b) noexcept { return previewcam::compose(a, b, identityPose()); }

// G(X): an OpenVR 3x4 (row-major m[i * 4 + j], the translation in m[3], m[7], m[11]) as Elite composes it. See the header.
inline Pose gameFromOpenVr(const float m[12]) noexcept {
    const float s[3] = {1.0f, 1.0f, -1.0f};
    Pose p = identityPose();
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) p.v[i * 4 + j] = s[i] * s[j] * m[j * 4 + i];
    for (int j = 0; j < 3; ++j) p.v[12 + j] = s[j] * m[j * 4 + 3];
    return p;
}

// The inverse of a rigid pose in Elite's rows: p = (p' - t) * R^T.
inline Pose rigidInverse(const Pose& a) noexcept {
    Pose p = identityPose();
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) p.v[i * 4 + j] = a.v[j * 4 + i];
    for (int j = 0; j < 3; ++j) p.v[12 + j] = -(a.v[12] * p.v[j] + a.v[13] * p.v[4 + j] + a.v[14] * p.v[8 + j]);
    return p;
}

inline float det3(const Pose& a) noexcept {
    const float* r0 = &a.v[0];
    const float* r1 = &a.v[4];
    const float* r2 = &a.v[8];
    return r0[0] * (r1[1] * r2[2] - r1[2] * r2[1]) - r0[1] * (r1[0] * r2[2] - r1[2] * r2[0]) + r0[2] * (r1[0] * r2[1] - r1[1] * r2[0]);
}

inline void cross3(const float a[3], const float b[3], float out[3]) noexcept {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}
inline float dot3(const float a[3], const float b[3]) noexcept { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
inline bool normalize3(float v[3]) noexcept {
    const float l = std::sqrt(dot3(v, v));
    if (!(l > 1e-6f) || !std::isfinite(l)) return false;
    for (int k = 0; k < 3; ++k) v[k] /= l;
    return true;
}

// The eye stub's fence (native_preview_camera.cpp bakeEye): a call is the base Elite is using, and so may be written, when its own pose (read before any write) is within
// kBaseMatchM of the expected base in translation and each of its three axes (rows 0 to 2, normalised) has a dot above kBaseMatchDot with the expected one. A pose whose
// interpreted lanes are not finite, or whose axes cannot be normalised, never matches. Only the 12 lanes the game interprets are read (each row's x, y, z, as
// previewcam::finite does): Flight 54's poses failed every match on the fourth lanes, which the game leaves unset, so nothing was written. Pure: the stub reads the pose, this decides.
constexpr float kBaseMatchM = 0.25f;
constexpr float kBaseMatchDot = 0.995f;
inline bool baseMatches(const float have[16], const float expect[16]) noexcept {
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 3; ++c)
            if (!std::isfinite(have[r * 4 + c]) || !std::isfinite(expect[r * 4 + c])) return false;
    const float d[3] = {have[12] - expect[12], have[13] - expect[13], have[14] - expect[14]};
    if (!(dot3(d, d) <= kBaseMatchM * kBaseMatchM)) return false;
    for (int r = 0; r < 3; ++r) {
        float a[3] = {have[r * 4], have[r * 4 + 1], have[r * 4 + 2]};
        float b[3] = {expect[r * 4], expect[r * 4 + 1], expect[r * 4 + 2]};
        if (!normalize3(a) || !normalize3(b)) return false;
        if (!(dot3(a, b) > kBaseMatchDot)) return false;
    }
    return true;
}

// A face's rows: right, up, forward, unit and orthogonal, right = up x forward (Elite's order, see the header).
struct Face {
    float r[3] = {}, u[3] = {}, f[3] = {};
};

// The flat bake's direction d along the room's axes (optimized_station.cpp bakeForward): 0 +a2, 1 -a2, 2 +a0, 3 -a0, 4 +a1 (up); its up is a1, and a2 for direction 4
// (the flat aim's own choice). The axes are made orthonormal here (forward first, then up); false when they cannot be.
inline bool faceOf(const float axes[3][3], uint32_t d, Face* out) noexcept {
    if (!out || d >= kFaces) return false;
    const float* a = axes[d < 2 ? 2 : d < 4 ? 0 : 1];
    const float sign = d == 1 || d == 3 ? -1.0f : 1.0f;
    const float* up = axes[d == 4 ? 2 : 1];
    Face f;
    for (int k = 0; k < 3; ++k) {
        f.f[k] = a[k] * sign;
        f.u[k] = up[k];
    }
    if (!normalize3(f.f)) return false;
    const float along = dot3(f.u, f.f);
    for (int k = 0; k < 3; ++k) f.u[k] -= along * f.f[k];
    if (!normalize3(f.u)) return false;
    cross3(f.u, f.f, f.r);
    *out = f;
    return true;
}

// The head pitched up by `rad` about the face's right axis: forward tips toward up. The face itself (what is captured) is not changed; only where the head looks.
inline Face pitchFace(const Face& in, float rad) noexcept {
    const float c = std::cos(rad), s = std::sin(rad);
    Face o = in;
    for (int k = 0; k < 3; ++k) {
        o.f[k] = in.f[k] * c + in.u[k] * s;
        o.u[k] = in.u[k] * c - in.f[k] * s;
    }
    return o;
}

// D: the head's rows for a face at `centre`. `handed` is +1 for Elite's own handedness (a proper rotation, row 0 = row 1 x row 2) and -1 if its own base was read with a
// negative determinant (the base the stub saw), so the cant of each eye turns the way Elite turns it.
inline Pose faceRows(const Face& face, const float centre[3], float handed) noexcept {
    Pose d = identityPose();
    for (int k = 0; k < 3; ++k) {
        d.v[k] = face.r[k] * handed;
        d.v[4 + k] = face.u[k];
        d.v[8 + k] = face.f[k];
        d.v[12 + k] = centre[k];
    }
    return d;
}

// The base to write: B = G(H)^-1 * D (see the header). Elite then builds eye_i = G(E_i) * G(H') * B, which is G(E_i) * D while its head H' is the H read.
inline Pose aimBase(const Pose& headGame, const Pose& rows) noexcept { return mul(rigidInverse(headGame), rows); }

// Where eye i should be (row 275) in a frame aimed with rows D: G(E_i) * D's translation. eyeToHead is the OpenVR 3x4 (the unconjugated one native_temporal.cpp holds: the
// conjugation leaves the translation's x and y and negates z, exactly what G does).
inline void eyeAt(const float eyeToHead[12], const Pose& rows, float out[3]) noexcept {
    const Pose e = mul(gameFromOpenVr(eyeToHead), rows);
    for (int k = 0; k < 3; ++k) out[k] = e.v[12 + k];
}

// The pitch that centres a face vertically in the two eyes' frusta (union): frusta[e] = {left, right, down, up} tangents as the host gave them (left and down negative). Each
// face needs |tan| <= 1 both ways; a frustum taller below than above wants the head pitched up by half the difference of the two angles. Clamped to kPitchMaxRad.
inline float pitchForFrusta(const float frusta[2][4], const bool have[2]) noexcept {
    float up = -1e9f, down = 1e9f;
    bool any = false;
    for (int e = 0; e < 2; ++e) {
        if (!have[e]) continue;
        if (!std::isfinite(frusta[e][2]) || !std::isfinite(frusta[e][3]) || !(frusta[e][3] > frusta[e][2])) continue;
        any = true;
        if (frusta[e][3] > up) up = frusta[e][3];
        if (frusta[e][2] < down) down = frusta[e][2];
    }
    if (!any) return 0.0f;
    float p = 0.5f * (std::atan(-down) - std::atan(up));
    if (p > kPitchMaxRad) p = kPitchMaxRad;
    if (p < -kPitchMaxRad) p = -kPitchMaxRad;
    return p;
}

// How well the two eyes' frusta (union) cover a face with the head pitched by `pitch`: the least margin, in tangent units, of points along the face's edge; positive is covered.
// eyeToHead[e] is the eye's OpenVR 3x4 relative to the head (its rotation is the true cant). The face is the head's unpitched forward: in the head's game rows a face point
// (a, b, 1) is (a, b cos - sin, b sin + cos), in OpenVR's head axes (x, y, -z), then into the eye by the transpose of its rotation, then x / depth and y / depth.
inline float faceSpare(const float frusta[2][4], const float eyeToHead[2][12], const bool have[2], float pitch, int perEdge = 16) noexcept {
    const float c = std::cos(pitch), s = std::sin(pitch);
    float worst = 1e9f;
    for (int edge = 0; edge < 4; ++edge) {
        for (int i = 0; i <= perEdge; ++i) {
            const float t = -1.0f + 2.0f * static_cast<float>(i) / static_cast<float>(perEdge);
            const float a = edge == 0 ? t : edge == 1 ? t : edge == 2 ? -1.0f : 1.0f;
            const float b = edge == 0 ? 1.0f : edge == 1 ? -1.0f : t;
            const float q[3] = {a, b * c - s, b * s + c};
            const float p[3] = {q[0], q[1], -q[2]};
            float best = -1e9f;
            for (int e = 0; e < 2; ++e) {
                if (!have[e]) continue;
                const float* m = eyeToHead[e];
                float pe[3];
                for (int j = 0; j < 3; ++j) pe[j] = m[0 * 4 + j] * p[0] + m[1 * 4 + j] * p[1] + m[2 * 4 + j] * p[2];
                const float depth = -pe[2];
                if (!(depth > 1e-6f)) continue;
                const float tx = pe[0] / depth, ty = pe[1] / depth;
                float margin = tx - frusta[e][0];
                if (frusta[e][1] - tx < margin) margin = frusta[e][1] - tx;
                if (ty - frusta[e][2] < margin) margin = ty - frusta[e][2];
                if (frusta[e][3] - ty < margin) margin = frusta[e][3] - ty;
                if (margin > best) best = margin;
            }
            if (best < worst) worst = best;
        }
    }
    return worst;
}

// The face test, the C++ twin of the capture shader's (concourse_pane.cpp, the VR bake's shader): a direction lies on the face's own square, its edges widened by `slack`.
inline bool onFace(const float d[3], const Face& f, float slack) noexcept {
    const float df = dot3(d, f.f);
    if (!(df > 0.0f)) return false;
    return std::fabs(dot3(d, f.r)) <= slack * df && std::fabs(dot3(d, f.u)) <= slack * df;
}

// Which wanted face a frame shows, from where it looks (the shader's twin): the nearest by forward, if within kFaceMinDot; -1 when none.
inline int faceOfView(const float forward[3], const Face faces[kFaces], uint32_t mask, float minDot) noexcept {
    float fw[3] = {forward[0], forward[1], forward[2]};
    if (!normalize3(fw)) return -1;
    int best = -1;
    float bestDot = minDot;
    for (uint32_t f = 0; f < kFaces; ++f) {
        if ((mask & (1u << f)) == 0) continue;
        const float k = dot3(fw, faces[f].f);
        if (k > bestDot) {
            bestDot = k;
            best = static_cast<int>(f);
        }
    }
    return best;
}

// The world check: render less store. True when it is past kWorldOffsetM (the bake then aims at the centre plus it, and the cube's origin moves with it).
inline bool worldOffset(const float store[3], const float render[3], float out[3], float* dist) noexcept {
    for (int k = 0; k < 3; ++k) out[k] = render[k] - store[k];
    const float d = std::sqrt(dot3(out, out));
    if (dist) *dist = d;
    return std::isfinite(d) && d > kWorldOffsetM;
}

// The aims: one tick a frame, each at the next wanted face not yet covered, in order; a face is aimed at again only once its last aim's counts should have come back
// (kReadbackTicks), at most kAimsPerFace times. With none to aim at, the last stays (its frames go on arriving). The bake is finished when every wanted face is covered or
// has used its aims and their counts are in.
class AimPlan {
public:
    void reset(uint32_t want) noexcept {
        want_ = want & ((1u << kFaces) - 1u);
        covered_ = 0;
        last_ = kFaces - 1;
        dwell_ = kVrBakeDwell;
        for (uint32_t f = 0; f < kFaces; ++f) {
            aims_[f] = 0;
            lastAim_[f] = 0;
            best_[f] = 0.0f;
        }
    }
    // The face this tick aims at, and whether it is a new aim (false: the face is held, or the last face stays). A new aim holds its face for kVrBakeDwell ticks, because
    // the room's upper structure flickers on some frames (Flight 53, H62) and every held frame writes, so the cube keeps the union; *dwell is the index of this tick in the hold (0..kVrBakeDwell-1), or kVrBakeDwell for a tick that only stays on the last face.
    // aims_ counts whole holds, and lastAim_ is the tick of the hold's last frame.
    uint32_t next(uint32_t tick, bool* fresh, uint32_t* dwell = nullptr) noexcept {
        if (dwell_ + 1 < kVrBakeDwell) {
            ++dwell_;
            lastAim_[last_] = tick;
            if (fresh) *fresh = false;
            if (dwell) *dwell = dwell_;
            return last_;
        }
        for (uint32_t k = 1; k <= kFaces; ++k) {
            const uint32_t f = (last_ + k) % kFaces;
            if (!eligible(f, tick)) continue;
            last_ = f;
            ++aims_[f];
            lastAim_[f] = tick;
            dwell_ = 0;
            if (fresh) *fresh = true;
            if (dwell) *dwell = 0;
            return f;
        }
        dwell_ = kVrBakeDwell;
        if (fresh) *fresh = false;
        if (dwell) *dwell = kVrBakeDwell;
        return last_;
    }
    void noteCoverage(uint32_t f, float frac) noexcept {
        if (f >= kFaces || !std::isfinite(frac)) return;
        if (frac > best_[f]) best_[f] = frac;
        if (frac >= kCoveredFrac) covered_ |= 1u << f;
    }
    bool exhausted(uint32_t f, uint32_t tick) const noexcept { return aims_[f] >= kAimsPerFace && tick - lastAim_[f] >= kReadbackTicks; }
    bool finished(uint32_t tick) const noexcept {
        for (uint32_t f = 0; f < kFaces; ++f) {
            if ((want_ & (1u << f)) == 0 || (covered_ & (1u << f)) != 0) continue;
            if (!exhausted(f, tick)) return false;
        }
        return true;
    }
    uint32_t want() const noexcept { return want_; }
    uint32_t covered() const noexcept { return covered_; }
    uint32_t aims(uint32_t f) const noexcept { return f < kFaces ? aims_[f] : 0; }
    float best(uint32_t f) const noexcept { return f < kFaces ? best_[f] : 0.0f; }

private:
    bool eligible(uint32_t f, uint32_t tick) const noexcept {
        if ((want_ & (1u << f)) == 0 || (covered_ & (1u << f)) != 0) return false;
        if (aims_[f] >= kAimsPerFace) return false;
        return aims_[f] == 0 || tick - lastAim_[f] >= kReadbackTicks;
    }
    uint32_t want_ = 0, covered_ = 0, last_ = kFaces - 1, dwell_ = kVrBakeDwell;
    uint32_t aims_[kFaces] = {}, lastAim_[kFaces] = {};
    float best_[kFaces] = {};
};

// The capture shader's 4x4 inverse (cofactors, each written once below and turned into both the C++ and the HLSL, so the two cannot differ): m is the clip matrix in column
// form (m[r * 4 + c]: clip_r = sum over c of m[r * 4 + c] * p_c), as the flat bake builds it from b1 270..273 (concourse_pane.cpp concoursePaneBakeFrame).
#define EDVR_CV_INV4(X)                                                                                                                                          \
    X(0, m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10])                  \
    X(4, -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10])                 \
    X(8, m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9])                    \
    X(12, -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9])                  \
    X(1, -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10])                 \
    X(5, m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10])                  \
    X(9, -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9])                   \
    X(13, m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9])                   \
    X(2, m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6])                      \
    X(6, -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6])                     \
    X(10, m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5])                     \
    X(14, -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5])                    \
    X(3, -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6])                       \
    X(7, m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6])                        \
    X(11, -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5])                        \
    X(15, m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5])
#define EDVR_CV_INV4_DET m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12]
#define EDVR_CV_INV4_CPP(i, e) inv[i] = (e);
#define EDVR_CV_INV4_TXT(i, e) "    inv[" #i "] = " #e ";\n"
#define EDVR_CV_STR2(x) #x
#define EDVR_CV_STR(x) EDVR_CV_STR2(x)

inline bool inverse4(const float m[16], float inv[16]) noexcept {
    EDVR_CV_INV4(EDVR_CV_INV4_CPP)
    const float det = EDVR_CV_INV4_DET;
    if (!(std::fabs(det) > 1e-30f) || !std::isfinite(det)) return false;
    const float k = 1.0f / det;
    for (int i = 0; i < 16; ++i) {
        inv[i] *= k;
        if (!std::isfinite(inv[i])) return false;
    }
    return true;
}
inline const char* inverse4Hlsl() noexcept {
    return "bool invert4(float m[16], out float inv[16]) {\n" EDVR_CV_INV4(EDVR_CV_INV4_TXT) "    float det = " EDVR_CV_STR(EDVR_CV_INV4_DET) ";\n"
           "    if (!(abs(det) > 1e-30)) return false;\n"
           "    float k = 1.0 / det;\n"
           "    [unroll] for (int i = 0; i < 16; ++i) inv[i] *= k;\n"
           "    return true;\n"
           "}\n";
}

// The clip matrix in column form from b1 rows 270..273 (rows[r] = row 270 + r): m[r * 4 + c] = rows[c][r], as the flat bake builds it.
inline void clipColumns(const float rows[4][4], float m[16]) noexcept {
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) m[r * 4 + c] = rows[c][r];
}

}  // namespace concourse_vr
}  // namespace edvr
