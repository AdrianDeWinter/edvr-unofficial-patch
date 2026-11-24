// Parallel projection left on for a canted Pimax headset, read from the eye orientations the runtime locates (docs/canted-projection.md,
// "PP left on").
//
// WHY. Pimax headsets of the P2 family (Vision 8K X, 8K Plus, 8K, 5K Super, 5K Plus, 5K XR, Artisan) angle their panels outward by about ten
// degrees. Pimax Play's "Enable Parallel Projection" makes the RUNTIME hide that: it reports two parallel views instead, and a much larger
// recommended eye (8268x3948 instead of 5016x3160 on an 8K X at FOV Wide). EDVR renders canted displays natively since the S*E*S handedness
// fix (src/openxr/canted_display.h), so the toggle buys nothing and costs up to about 30% of the GPU time (measured on an 8K X through PimaxXR,
// FSR, same scenes: 15.9 -> 11.0 ms, 13.7 -> 10.0 ms, 10.8 -> 9.6 ms with it off). EDVR cannot undo it: under parallel projection the runtime
// does not expose the native cant or the vertical FOV, and the only overrides are machine-global. So the most it can do is say so.
//
// WHAT IS READ. Three things, none of them Pimax Play's settings: the system name the runtime reports (a canted model or not), the two located
// eye orientations (the angle between them: 20 degrees apart with the toggle off on an 8K X, 0 with it on), and the runtime name (PimaxXR
// latches the toggle from Pimax Play and is proven to honour it; Pimax Play's own runtime is not known to, and may apply parallel projection to
// every canted headset whatever the toggle says). The views must be real: classifyViews says Unknown for anything that is not a finite unit
// orientation, decide() turns Unknown into NotApplicable, and the callers latch only on a known pair, so a frame that has not located yet
// never counts as "canted" or as "parallel".
//
// WHERE IT SHOWS. The openxr half logs it (formatLog) from the first valid locate, once a session. The graphics half reads the same pieces
// through channels that already cross (the eye poses the menu client publishes, the runtime and system names the render-settings query
// carries), asks the same decide(), and for Notice only raises the settings menu's own notice: a toast and the Status page's hint, the way
// vr_supersample_notice.h and terrain_checkerboard_notice.h do. A runtime that does not honour the toggle gets the log line only: telling the
// user to turn off a setting that may do nothing would send them hunting.
//
// Pure and dependency-free: tools\openxr_native_test drives every function here, and both halves call the very same ones.
#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace edvr {
namespace ppn {

// Two eyes whose orientations differ by less than this are parallel. A canted P2 panel pair is 20 degrees apart.
constexpr double kParallelUnderDegrees = 0.5;
// How far from unit a quaternion's squared length (and a rotation's orthonormality) may be before it is not an orientation.
constexpr double kUnitTolerance = 1.0e-3;
// The lengths of the two short forms: a toast is one line of the toast card, the hint two lines of the Status page's hint box.
constexpr size_t kToastMax = 55;
constexpr size_t kHintMax = 78;

namespace detail {
constexpr double kRadiansToDegrees = 57.295779513082320876798154814105;

// The name as a comparison sees it: lower-case letters and digits only, '+' read as "plus" (so "5K+" is "5kplus"), every other byte (space,
// punctuation, anything non-ASCII) dropped. Truncates to size - 1 characters.
inline size_t normalise(const char* name, char* out, size_t size) {
    size_t n = 0;
    if (!out || !size) return 0;
    for (const char* p = name; p && *p; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        if (c >= 'A' && c <= 'Z') {
            if (n + 1 >= size) break;
            out[n++] = static_cast<char>(c - 'A' + 'a');
        } else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            if (n + 1 >= size) break;
            out[n++] = static_cast<char>(c);
        } else if (c == '+') {
            if (n + 5 > size) break;
            out[n++] = 'p'; out[n++] = 'l'; out[n++] = 'u'; out[n++] = 's';
        }
    }
    out[n] = 0;
    return n;
}
inline bool finite3x4(const float* m) {
    for (int i = 0; i < 12; ++i) if (!std::isfinite(m[i])) return false;
    return true;
}
}  // namespace detail

// ---- which headset ---------------------------------------------------------------------------------------------------------
// A canted Pimax model by the name the runtime reports for the system (XrSystemProperties.systemName; "Pimax Vision 8K X" through PimaxXR).
// Case, spaces and punctuation do not matter. True only for a name with "pimax" in it and one of the P2 family's model names; the Crystal
// line, the 12K, Dream Air, the Portal and every other maker's headset are false.
inline bool cantedPimaxModel(const char* systemName) {
    char n[160];
    detail::normalise(systemName, n, sizeof(n));
    if (!std::strstr(n, "pimax")) return false;
    static constexpr const char* kModels[] = {
        "8kx", "8kplus", "5ksuper", "5kplus", "5kxr", "artisan",
        "pimax8k", "pimaxvision8k",   // the plain 8K, which has no suffix to look for
    };
    for (const char* model : kModels)
        if (std::strstr(n, model)) return true;
    return false;
}

// Whether the runtime is one that is proven to apply Pimax Play's "Enable Parallel Projection" toggle: PimaxXR ("PimaxXR (Unofficial)").
// Pimax Play's own runtime ("Pimax OpenXR") and SteamVR are not known to, so they are false.
inline bool honoursToggle(const char* runtimeName) {
    char n[160];
    detail::normalise(runtimeName, n, sizeof(n));
    return std::strstr(n, "pimaxxr") != nullptr;
}

// ---- what the views say ----------------------------------------------------------------------------------------------------
// The angle between two eye orientations, degrees, from unit quaternions (x, y, z, w); -1 when either is not finite or not of unit length
// (squared length within kUnitTolerance of 1). q and -q are the same orientation. The relative rotation conj(a) * b has angle
// 2 * atan2(|vector part|, |w|), which stays exact for small angles where acos does not.
inline double quatAngleDegrees(double ax, double ay, double az, double aw, double bx, double by, double bz, double bw) {
    const double v[8] = {ax, ay, az, aw, bx, by, bz, bw};
    for (double c : v) if (!std::isfinite(c)) return -1.0;
    if (std::fabs(ax*ax + ay*ay + az*az + aw*aw - 1.0) > kUnitTolerance ||
        std::fabs(bx*bx + by*by + bz*bz + bw*bw - 1.0) > kUnitTolerance) return -1.0;
    const double qx = aw*bx - bw*ax - (ay*bz - az*by);
    const double qy = aw*by - bw*ay - (az*bx - ax*bz);
    const double qz = aw*bz - bw*az - (ax*by - ay*bx);
    const double qw = aw*bw + ax*bx + ay*by + az*bz;
    return 2.0 * std::atan2(std::sqrt(qx*qx + qy*qy + qz*qz), std::fabs(qw)) * detail::kRadiansToDegrees;
}
// The same angle from two row-major 3x4 rigid transforms (the rotation is the left 3x3 of each: the shape the menu client publishes the eye
// poses in); -1 when either has a non-finite entry or a rotation that is not orthonormal with determinant +1.
inline double rotationAngleDegrees(const float* a, const float* b) {
    if (!a || !b || !detail::finite3x4(a) || !detail::finite3x4(b)) return -1.0;
    for (const float* m : {a, b}) {
        for (int p = 0; p < 3; ++p) for (int q = p; q < 3; ++q) {
            double dot = 0;
            for (int c = 0; c < 3; ++c) dot += double(m[p*4 + c]) * m[q*4 + c];
            if (std::fabs(dot - (p == q ? 1.0 : 0.0)) > kUnitTolerance) return -1.0;
        }
        const double det = double(m[0]) * (double(m[5])*m[10] - double(m[6])*m[9]) - double(m[1]) * (double(m[4])*m[10] - double(m[6])*m[8]) +
                           double(m[2]) * (double(m[4])*m[9] - double(m[5])*m[8]);
        if (std::fabs(det - 1.0) > 2.0 * kUnitTolerance) return -1.0;
    }
    double r[3][3];   // a's rotation transposed times b's: the rotation from eye a to eye b
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) {
        r[i][j] = 0;
        for (int k = 0; k < 3; ++k) r[i][j] += double(a[k*4 + i]) * b[k*4 + j];
    }
    const double sx = r[2][1] - r[1][2], sy = r[0][2] - r[2][0], sz = r[1][0] - r[0][1];   // 2 sin(angle) as a vector
    const double cosine2 = r[0][0] + r[1][1] + r[2][2] - 1.0;                              // 2 cos(angle)
    return std::atan2(std::sqrt(sx*sx + sy*sy + sz*sz), cosine2) * detail::kRadiansToDegrees;
}

// Unknown is the answer to every doubt (an angle of -1 or NaN): the views were not a pair of real orientations, which says nothing.
enum class Views : uint8_t { Unknown = 0, Parallel = 1, Canted = 2 };
inline Views viewsFromAngle(double degrees) {
    if (!(degrees >= 0.0)) return Views::Unknown;
    return degrees < kParallelUnderDegrees ? Views::Parallel : Views::Canted;
}
// Any type with x, y, z, w members: XrQuaternionf from xrLocateViews, with no OpenXR header in this one.
template <class Quaternion>
inline double viewAngleDegrees(const Quaternion& left, const Quaternion& right) {
    return quatAngleDegrees(left.x, left.y, left.z, left.w, right.x, right.y, right.z, right.w);
}
template <class Quaternion>
inline Views classifyViews(const Quaternion& left, const Quaternion& right) { return viewsFromAngle(viewAngleDegrees(left, right)); }
inline Views classifyRotations(const float* leftPose, const float* rightPose) { return viewsFromAngle(rotationAngleDegrees(leftPose, rightPose)); }
// True when the angle between the two orientations is under kParallelUnderDegrees; false for canted views and for invalid input alike.
template <class Quaternion>
inline bool parallelViews(const Quaternion& left, const Quaternion& right) { return classifyViews(left, right) == Views::Parallel; }

// ---- the verdict -----------------------------------------------------------------------------------------------------------
// NotApplicable: not a canted Pimax model, or the views are not known yet. ProjectionOff: a canted model whose views are canted, the state
// this feature wants. Notice: parallel projection is on, and the runtime applies the toggle: say so to the user. LogOnly: parallel projection
// is on, and whether this runtime applies the toggle is unverified: the log says so and the user is not sent to a setting that may do nothing.
enum class Verdict : uint8_t { NotApplicable = 0, ProjectionOff = 1, Notice = 2, LogOnly = 3 };
inline Verdict decide(const char* systemName, const char* runtimeName, Views views) {
    if (views == Views::Unknown || !cantedPimaxModel(systemName)) return Verdict::NotApplicable;
    if (views == Views::Canted) return Verdict::ProjectionOff;
    return honoursToggle(runtimeName) ? Verdict::Notice : Verdict::LogOnly;
}

// ---- the graphics half's latch ---------------------------------------------------------------------------------------------------
// The graphics half decides once per menu session and never again inside it. `session` is the identity the native menu gives its session
// (nativeMenuSessionId): it changes at every acquire and at every close, so a new session, and a close that leaves none, start the decision
// again. Nothing from the last session carries over: neither a checked verdict that would suppress the next session's notice, nor a notice
// that outlives the session it was raised in.
struct SessionLatch {
    uint64_t session = 0;   // the identity the flags belong to; 0 before any session
    bool checked = false;   // decided for `session`
    bool notice = false;    // that decision is Notice: the toast was raised for it, and the Status page shows the hint
};
// One tick. `eyes` is the held pair of 3x4 eye poses once the names are known too, and nullptr when there is nothing to decide on yet (no
// pose, or no names). Returns true on the one tick a session's verdict is Notice, so the caller raises the toast then: a session says it once,
// and a close followed by a publish of the same verdict says it again, because that is a new session.
inline bool latchTick(SessionLatch& latch, uint64_t session, const float (*eyes)[12], const char* systemName, const char* runtimeName) {
    if (session != latch.session) {
        latch = SessionLatch{};
        latch.session = session;
    }
    if (latch.checked || !eyes) return false;
    const Views views = classifyRotations(eyes[0], eyes[1]);
    if (views == Views::Unknown) return false;   // not a pair of orientations yet: the check waits
    latch.checked = true;
    if (decide(systemName, runtimeName, views) != Verdict::Notice) return false;
    latch.notice = true;
    return true;
}

// ---- the words ---------------------------------------------------------------------------------------------------------------
// The openxr log's line, once a session: one line (the trace line buffer is 4096 bytes). `width` x `height` is the runtime's recommended
// eye size, `angleDegrees` the angle between the located eyes. NotApplicable writes an empty string and returns 0.
inline int formatLog(char* out, size_t size, Verdict verdict, const char* systemName, const char* runtimeName, uint32_t width, uint32_t height,
                     double angleDegrees) {
    const char* system = systemName && *systemName ? systemName : "Pimax headset";
    const char* runtime = runtimeName && *runtimeName ? runtimeName : "the runtime";
    switch (verdict) {
        case Verdict::Notice:
            return std::snprintf(out, size,
                "canted eyes: parallel projection is on: this %s is canted, but %s reports parallel views (%ux%u per eye). ED-UP renders "
                "canted displays without it; turning \"Enable Parallel Projection\" off in Pimax Play and restarting the game cuts GPU "
                "time by up to about 30%%.",
                system, runtime, width, height);
        case Verdict::LogOnly:
            return std::snprintf(out, size,
                "canted eyes: parallel projection is on: this %s is canted, but %s reports parallel views (%ux%u per eye). ED-UP renders "
                "canted displays without it; whether this runtime honours Pimax Play's toggle is unverified.",
                system, runtime, width, height);
        case Verdict::ProjectionOff:
            return std::snprintf(out, size,
                "canted eyes: parallel projection is off: this %s is canted and %s reports canted views, %.2f degrees apart (%ux%u per "
                "eye); ED-UP renders them natively.",
                system, runtime, angleDegrees, width, height);
        case Verdict::NotApplicable:
            break;
    }
    if (out && size) out[0] = 0;
    return 0;
}
// The headset toast: one short line (kToastMax characters at most) that says what is on and where to turn it off.
inline int formatToast(char* out, size_t size) {
    return std::snprintf(out, size, "Parallel projection is on: turn it off in Pimax Play");
}
// The Status page's hint, at most kHintMax characters: it replaces the hint the page has and adds no line (vr_supersample_notice.h says why a
// line or a note would take the menu bitmap past its 2048 px guard on exactly the Pimax headsets this is for).
inline int formatStatusHint(char* out, size_t size) {
    return std::snprintf(out, size, "Parallel projection is on: turn it off in Pimax Play, then restart Elite.");
}

// ---- the Status page's one hint slot ---------------------------------------------------------------------------------------
// The slot is shared with the supersampling and the checkerboard hints, which alternate between themselves every `turnMs`
// (tcn::pickStatusHint). This one shows alone when neither applies; when one or both do, it takes every third turn and leaves the other two
// theirs, so none of the three is ever hidden.
inline bool statusHintTurn(bool otherNoticeApplies, uint64_t nowMs, uint64_t turnMs) {
    if (!otherNoticeApplies) return true;
    return turnMs != 0 && (nowMs / turnMs) % 3u == 2u;
}

}  // namespace ppn
}  // namespace edvr
