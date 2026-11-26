// F175 (native on foot VR): the view's roll and pitch against the headset's, by look mode and item. MEASURE ONLY: nothing the user sees changes, no key. Header only, no game or D3D11 state,
// so native_frame_test runs the very functions the pin hook and the frame thread use. Journal: the arc doc, F175.
//
// The report (a tester, a combat zone, fix.native_on_foot_look_to_aim on): "a really unpleasant tilting motion to your view when looking horizontally"; they turned it off quickly. No log.
// Under look to aim the game's aim follows the head (F109's follower through the head aim's map) and the runtime takes what the game applied back out of the reported head (the seat yaw,
// then the ADS pitch overlay about the seat's right axis for that yaw); the game composes its camera as the aim turned by the reported head (F61). By design the view is the headset's, with
// no roll added. Nothing in the log measured that. Four readings of the report, and what separates them in this line:
//   1. EDVR's cancel adds roll (a composition or timing fault): the roll error grows with the aim's pitch times the head's yaw lead over the aim, near 0 looking level. The slope of the roll
//      error against (aim pitch x the head's yaw lead over the aim), radians against radians, is near +-1; the line prints it with its pair count and the spread of its input.
//   2. The game's own camera rolls (weapon sway, the body catching up with the aim, the combat stance): the roll error is there with the head still, and in mouse look too (both modes print).
//   3. The tilt is not a roll: the roll error stays near 0 and the view's pitch against the head's nods (look to aim) or the camera's position swings (the pitch error prints apart).
//   4. F61's calibration rig was on (fix.native_on_foot_rot_calibrate): the line says whether it is.
//
// What is measured, per pin call (once a game frame):
//  - the view's roll: the roll about its forward of the camera the eyes were RENDERED with, against the character's up. The camera is the two eyes' scene rows of the game frame (F126's
//    pairing, pinCycloUpdate: their mean forward and up, in world axes, taken to the model space of the 3P body by the AMC's inverse): the rotation actually rendered, not the aim.
//  - the headset's roll in natural LOCAL (np.local), about its forward against LOCAL's up, by the same formula.
//  - roll error = the view's roll less the headset's.
//  - pitch error = the view's elevation (its forward against the character's up) less the expected one: under look to aim the headset's elevation in LOCAL; under mouse look the headset's
//    plus the aim's pitch (the FP root's elevation, the one F137 uses).
//  - the headset's angular speed and its horizontal (yaw) part, from consecutive LOCAL orientations of poses that differ.
//  - the aim's pitch and the reported head's yaw and pitch lead over the aim (F109's figures: the render head in the seated space, adsHeadAngles).
// Sign convention: roll is positive when the camera's RIGHT side is above its left (the right-hand side up: the horizon turns clockwise in the picture); the model's axes are x right, y up,
// z forward (right = up x forward), LOCAL's are OpenXR's x right, y up, -z forward (right = the rotation's first column, up its second).
//
// The cost: a few dot products a frame, no allocation, no hook, no GPU work.
#pragma once

#include "native_on_foot_core.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace edvr {
namespace nofoot {
namespace f175 {

constexpr double kStillDegS = 10.0;     // the head is still under this angular speed
constexpr double kTurningDegS = 30.0;   // the head is turning horizontally over this yaw rate
constexpr double kOverDeg = 2.0;        // the share of frames with |roll error| over this
constexpr double kFlagRmsDeg = 1.5;     // the health rule: a mode's roll error rms over this ...
constexpr uint32_t kFlagFrames = 2 * 60;  // ... across at least this many frames (2 s at 60 frames a second) ...
constexpr uint32_t kFlagSeconds = 2;    // ... and this many seconds with frames
constexpr uint32_t kDurationFromSecs = 0xFFFFFFFFu;  // sentinel: formatGroup derives duration from integer modeActiveSecs when caller omits measured ms (#199)
constexpr double kMinRateDtS = 0.001, kMaxRateDtS = 0.25;   // two LOCAL poses make a rate when they are this far apart in time at the most and at the least
constexpr double kRateFreshS = 0.25;    // a rate is used for this long after it was last worked out (the pose may not change every game frame)
constexpr double kPi = 3.14159265358979323846;

inline double deg(double rad) noexcept { return rad * 180.0 / kPi; }
inline double wrapPi(double a) noexcept {
    while (a > kPi) a -= 2.0 * kPi;
    while (a < -kPi) a += 2.0 * kPi;
    return a;
}

// ---- the geometry -----------------------------------------------------------------------------------------------------------------------------------------------------

inline bool unit3(const double v[3], double out[3]) noexcept {
    const double l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (!std::isfinite(l) || !(l > 1e-9)) return false;
    out[0] = v[0] / l;
    out[1] = v[1] / l;
    out[2] = v[2] / l;
    return true;
}
inline double dot3(const double a[3], const double b[3]) noexcept { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// The roll of a camera in the model's axes (x right, y up, z forward): its right is up x forward; roll = atan2(dot(right, worldUp), dot(up, worldUp)), positive when the right side is up.
// The camera's up need not be perpendicular to its forward (the mean of two eyes' rows): the right is taken from their cross product and the roll is the angle about the forward.
// False when either vector is degenerate or they are parallel.
inline bool viewRoll(const double fwd[3], const double camUp[3], const double worldUp[3], double* rollRad) noexcept {
    double f[3], u[3], w[3], r[3];
    if (!unit3(fwd, f) || !unit3(camUp, u) || !unit3(worldUp, w)) return false;
    r[0] = u[1] * f[2] - u[2] * f[1];
    r[1] = u[2] * f[0] - u[0] * f[2];
    r[2] = u[0] * f[1] - u[1] * f[0];
    double rr[3];
    if (!unit3(r, rr)) return false;
    // the camera's up made perpendicular to its forward, so that (right, up, forward) is the camera's frame
    double p[3] = {f[1] * rr[2] - f[2] * rr[1], f[2] * rr[0] - f[0] * rr[2], f[0] * rr[1] - f[1] * rr[0]};   // forward x right = up in these axes
    double pu[3];
    if (!unit3(p, pu)) return false;
    *rollRad = std::atan2(dot3(rr, w), dot3(pu, w));
    return std::isfinite(*rollRad);
}
// The elevation of a forward against the world's up, nose up positive (radians).
inline bool viewElevation(const double fwd[3], const double worldUp[3], double* elevRad) noexcept {
    double f[3], w[3];
    if (!unit3(fwd, f) || !unit3(worldUp, w)) return false;
    *elevRad = std::asin(std::max(-1.0, std::min(1.0, dot3(f, w))));
    return std::isfinite(*elevRad);
}
// The headset's roll in natural LOCAL from the 3x4 row-major pose (OpenXR axes): right is the rotation's first column, up the second, against LOCAL's up (0, 1, 0).
inline bool localRoll(const float m[12], double* rollRad) noexcept {
    if (!m || !std::isfinite(m[4]) || !std::isfinite(m[5])) return false;
    const double r = static_cast<double>(m[4]), u = static_cast<double>(m[5]);
    if (!(std::sqrt(r * r + u * u) > 1e-6)) return false;
    *rollRad = std::atan2(r, u);
    return true;
}
// The headset's elevation in natural LOCAL, nose up positive (the forward is minus the third column: pinNaturalLocalPitch's asin(-m[6])).
inline bool localElevation(const float m[12], double* elevRad) noexcept { return pinNaturalLocalPitch(m, elevRad); }

// The angular speed (radians a second) between two LOCAL orientations dt seconds apart, and its part about the vertical (LOCAL's y). R = now x prev^T, the turn between the poses in
// LOCAL's axes: its angle from the trace, its axis from the antisymmetric part; the yaw part is angle x axis.y / dt. False for a dt outside (kMinRateDtS, kMaxRateDtS] or a pose that is not a number.
inline bool angularRate(const float prev[12], const float now[12], double dt, double* speed, double* yawRate) noexcept {
    if (!(dt >= kMinRateDtS) || !(dt <= kMaxRateDtS)) return false;
    double P[3][3], N[3][3];
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) {
            P[r][c] = static_cast<double>(prev[4 * r + c]);
            N[r][c] = static_cast<double>(now[4 * r + c]);
            if (!std::isfinite(P[r][c]) || !std::isfinite(N[r][c])) return false;
        }
    double R[3][3];
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) R[r][c] = N[r][0] * P[c][0] + N[r][1] * P[c][1] + N[r][2] * P[c][2];
    const double c = 0.5 * (R[0][0] + R[1][1] + R[2][2] - 1.0);                                              // cos(angle)
    const double v[3] = {0.5 * (R[2][1] - R[1][2]), 0.5 * (R[0][2] - R[2][0]), 0.5 * (R[1][0] - R[0][1])};   // sin(angle) x axis
    const double s = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);                                     // sin(angle)
    const double angle = std::atan2(s, c);                                                                    // well conditioned for the small turns of one frame (acos of a cosine near 1 is not)
    *speed = angle / dt;
    *yawRate = s > 1e-12 ? angle * (v[1] / s) / dt : 0.0;   // the signed turn about LOCAL's up (positive: counter-clockwise seen from above, the head turning left)
    return std::isfinite(*speed) && std::isfinite(*yawRate);
}

// The rate between consecutive LOCAL poses of the hook's calls: a pose that differs from the last (its own time later) makes a new rate; calls between keep the last for kRateFreshS.
struct RateTracker {
    bool have = false;
    float prev[12] = {};
    double prevPoseS = 0.0;
    uint32_t prevGeneration = 0;
    bool rateValid = false;
    double speedDegS = 0.0, yawDegS = 0.0, rateS = 0.0;
    void reset() noexcept { *this = RateTracker{}; }
    // poseS: the pose's own time; nowS: this call's; generation: the LOCAL reference's (a recentre starts over). True when a rate is in force (this call's or a recent one).
    bool step(const float local[12], double poseS, double nowS, uint32_t generation, double* speedDeg, double* yawDeg) noexcept {
        if (!have || generation != prevGeneration || !(poseS >= prevPoseS)) {
            have = true;
            rateValid = false;
            std::memcpy(prev, local, sizeof(prev));
            prevPoseS = poseS;
            prevGeneration = generation;
        } else if (poseS > prevPoseS) {
            double sp = 0.0, yw = 0.0;
            if (angularRate(prev, local, poseS - prevPoseS, &sp, &yw)) {
                rateValid = true;
                speedDegS = deg(sp);
                yawDegS = deg(yw);
                rateS = nowS;
            } else {
                rateValid = false;
            }
            std::memcpy(prev, local, sizeof(prev));
            prevPoseS = poseS;
        }
        if (!rateValid || !(nowS - rateS <= kRateFreshS)) return false;
        *speedDeg = speedDegS;
        *yawDeg = yawDegS;
        return true;
    }
};

// ---- the window ---------------------------------------------------------------------------------------------------------------------------------------------------------

// One frame's figures. rollOk / pitchOk: the roll error / pitch error are numbers; rateOk: the head's rate is in force; leadOk: the aim's pitch and the reported head's yaw lead are numbers.
struct Sample {
    int mode = 0;   // 0 mouse look, 1 look to aim
    int item = 0;   // 0 unarmed, 1 armed
    bool f61On = false;   // F61 calibration rig state that produced these eye rows (#199)
    bool rollOk = false, pitchOk = false, rateOk = false, leadOk = false;
    double rollErrRad = 0.0, pitchErrRad = 0.0;
    double speedDegS = 0.0, yawDegS = 0.0;
    double aimPitchRad = 0.0, yawLeadRad = 0.0, pitchLeadRad = 0.0;
};
struct Stat {
    uint32_t n = 0;
    double sum = 0.0, sumSq = 0.0, maxAbs = 0.0;
    void add(double v) noexcept {
        ++n;
        sum += v;
        sumSq += v * v;
        maxAbs = std::max(maxAbs, std::fabs(v));
    }
    void merge(const Stat& o) noexcept {
        n += o.n;
        sum += o.sum;
        sumSq += o.sumSq;
        maxAbs = std::max(maxAbs, o.maxAbs);
    }
    double mean() const noexcept { return n ? sum / static_cast<double>(n) : 0.0; }
    double rms() const noexcept { return n ? std::sqrt(sumSq / static_cast<double>(n)) : 0.0; }
};
struct Group {
    uint32_t frames = 0, over = 0;                // frames; frames with |roll error| over kOverDeg
    uint32_t activeSecs = 0;                      // distinct slices in the window that had eligible frames (F125/F175)
    uint32_t activeMs = 0;                        // measured active duration in ms across those slices (#199)
    Stat roll, still, turning, pitch;             // degrees: the roll error, split by the head still / turning horizontally, and the pitch error
    uint32_t slopeN = 0;                          // the pairs of the slope
    double sxx = 0.0, sxy = 0.0;                  // radians: x = aim pitch x yaw lead, y = the roll error
    Stat speed, aim, yawLead, pitchLead;          // degrees (deg/s for the speed)
    void add(const Sample& s) noexcept {
        ++frames;
        if (s.rollOk) {
            const double e = deg(s.rollErrRad);
            roll.add(e);
            if (std::fabs(e) > kOverDeg) ++over;
            if (s.rateOk) {
                if (s.speedDegS < kStillDegS) still.add(e);
                if (std::fabs(s.yawDegS) > kTurningDegS) turning.add(e);
            }
            if (s.leadOk) {
                const double x = s.aimPitchRad * s.yawLeadRad;
                ++slopeN;
                sxx += x * x;
                sxy += x * s.rollErrRad;
            }
        }
        if (s.pitchOk) pitch.add(deg(s.pitchErrRad));
        if (s.rateOk) speed.add(s.speedDegS);
        if (s.leadOk) {
            aim.add(deg(s.aimPitchRad));
            yawLead.add(deg(s.yawLeadRad));
            pitchLead.add(deg(s.pitchLeadRad));
        }
    }
    void merge(const Group& o) noexcept {
        frames += o.frames;
        over += o.over;
        activeSecs += o.activeSecs;
        activeMs += o.activeMs;
        roll.merge(o.roll);
        still.merge(o.still);
        turning.merge(o.turning);
        pitch.merge(o.pitch);
        slopeN += o.slopeN;
        sxx += o.sxx;
        sxy += o.sxy;
        speed.merge(o.speed);
        aim.merge(o.aim);
        yawLead.merge(o.yawLead);
        pitchLead.merge(o.pitchLead);
    }
    // The least-squares slope through the origin of the roll error against aim pitch x yaw lead (radians against radians); 0 without a spread in x. The x spread is printed beside it.
    double slope() const noexcept { return sxx > 1e-12 ? sxy / sxx : 0.0; }
    double xRms() const noexcept { return slopeN ? std::sqrt(sxx / static_cast<double>(slopeN)) : 0.0; }
};
struct Window {
    Group g[2][2];                         // [mode][item] with F61 rig off
    uint32_t modeActiveSecs[2] = {0, 0};   // distinct slices in the window where either item in [mode] had eligible frames with F61 off (F125/F175)
    uint32_t modeActiveMs[2] = {0, 0};     // measured active duration (ms) for [mode] with F61 off (#199)
    Group gF61On[2][2];                    // [mode][item] with F61 rig ON (kept strictly separate from F61 off across toggles, #199)
    uint32_t modeActiveSecsF61On[2] = {0, 0};
    uint32_t modeActiveMsF61On[2] = {0, 0};
    void add(const Sample& s) noexcept {
        if ((s.mode != 0 && s.mode != 1) || (s.item != 0 && s.item != 1)) return;
        (s.f61On ? gF61On : g)[s.mode][s.item].add(s);
    }
    uint32_t totalFramesFor(bool f61On) const noexcept {
        const Group (*side)[2] = f61On ? gF61On : g;
        uint32_t total = 0;
        for (int m = 0; m < 2; ++m)
            for (int i = 0; i < 2; ++i) total += side[m][i].frames;
        return total;
    }
    // Fold one hook snapshot covering `sliceMs` milliseconds into a multi-second summary window,
    // counting each [mode][item] and each [mode] union that had eligible frames separately from
    // the reporting interval and strictly separated by per-sample F61 state (#199).
    static void mergeSide(Group dst[2][2], uint32_t dstModeSecs[2], uint32_t dstModeMs[2],
                          const Group src[2][2], const uint32_t srcModeSecs[2],
                          const uint32_t srcModeMs[2], uint32_t sliceMs) noexcept {
        for (int m = 0; m < 2; ++m) {
            bool modeAny = false;
            for (int i = 0; i < 2; ++i)
                if (src[m][i].frames) {
                    dst[m][i].merge(src[m][i]);
                    dst[m][i].activeSecs += src[m][i].activeSecs ? 0u : 1u;
                    dst[m][i].activeMs += src[m][i].activeSecs ? 0u : sliceMs;
                    modeAny = true;
                }
            if (modeAny) {
                dstModeSecs[m] += srcModeSecs[m] ? srcModeSecs[m] : 1u;
                dstModeMs[m] += srcModeSecs[m] ? srcModeMs[m] : sliceMs;
            }
        }
    }
    void mergeSlice(const Window& sec, uint32_t sliceMs) noexcept {
        // When a single raw snapshot holds samples from both F61 states (only possible across
        // an F61 toggle edge), split sliceMs proportionally by frame count so an in-flight
        // straggler never inflates the other side's measured duration past sliceMs (#199).
        const uint32_t offFrames = sec.totalFramesFor(false);
        const uint32_t onFrames = sec.totalFramesFor(true);
        uint32_t offMs = sliceMs;
        uint32_t onMs = sliceMs;
        if (offFrames > 0 && onFrames > 0) {
            offMs = static_cast<uint32_t>((static_cast<uint64_t>(sliceMs) * offFrames) / (offFrames + onFrames));
            onMs = sliceMs - offMs;
        }
        mergeSide(g, modeActiveSecs, modeActiveMs, sec.g, sec.modeActiveSecs, sec.modeActiveMs, offMs);
        mergeSide(gF61On, modeActiveSecsF61On, modeActiveMsF61On, sec.gF61On, sec.modeActiveSecsF61On,
                  sec.modeActiveMsF61On, onMs);
    }
    void mergeSecond(const Window& sec) noexcept { mergeSlice(sec, 1000u); }
    bool anyFor(bool f61On) const noexcept {
        const Group (*side)[2] = f61On ? gF61On : g;
        for (int m = 0; m < 2; ++m)
            for (int i = 0; i < 2; ++i)
                if (side[m][i].frames) return true;
        return false;
    }
    uint32_t maxActiveFor(bool f61On) const noexcept {
        const Group (*side)[2] = f61On ? gF61On : g;
        const uint32_t* mSecs = f61On ? modeActiveSecsF61On : modeActiveSecs;
        uint32_t maxA = 0;
        for (int m = 0; m < 2; ++m) {
            if (mSecs[m] > maxA) maxA = mSecs[m];
            for (int i = 0; i < 2; ++i)
                if (side[m][i].frames) {
                    const uint32_t a = side[m][i].activeSecs ? side[m][i].activeSecs : 1u;
                    if (a > maxA) maxA = a;
                }
        }
        return maxA;
    }
    bool any() const noexcept { return anyFor(false) || anyFor(true); }
    void clear() noexcept { *this = Window{}; }
};

inline const char* modeName(int mode) noexcept { return mode ? "look to aim" : "mouse look"; }
inline const char* itemName(int item) noexcept { return item ? "armed" : "unarmed"; }

// One group's line for a summary window: the frames; the roll error's mean, rms and largest and the share over 2 deg; the same by the head still and turning; the pitch error against what is
// expected (the headset's elevation under look to aim, plus the aim's pitch under mouse look); the slope with its pairs and the spread of its input; the head's speed, the aim's pitch and
// the reported head's lead over the aim (yaw, pitch); F61's rig; and the seconds with eligible frames in the window (for this item and across the look mode, plus the flush sequence id). The log's lines end at 1200 bytes (1166 of them the message); this one runs to about 750.
inline int formatGroup(char* buf, size_t n, uint32_t seconds, int mode, int item, const Group& g, bool f61On,
                       uint32_t modeActiveSecs = 0, uint32_t flushId = 0,
                       uint32_t modeActiveMs = kDurationFromSecs) noexcept {
    const uint32_t rawActive = g.activeSecs ? g.activeSecs : (g.frames ? seconds : 0u);
    const uint32_t active = seconds ? std::min(rawActive, seconds) : rawActive;
    const uint32_t rawMode = modeActiveSecs ? std::max(modeActiveSecs, rawActive) : rawActive;
    const uint32_t modeActive = seconds ? std::min(rawMode, seconds) : rawMode;
    const double activeDurS = (modeActiveMs != kDurationFromSecs)
                                  ? (static_cast<double>(modeActiveMs) * 1e-3)
                                  : static_cast<double>(modeActive);
    return std::snprintf(buf, n,
                         "native on foot view roll (F175) %u s: mode %s, item %s, F61 rig %s; %u frames (roll: about the view's forward against the character's up, + the right side up; error = the view's "
                         "less the headset's in LOCAL); roll error mean %+.2f rms %.2f max %.2f deg, over 2 deg in %u frames; head still (under 10 deg/s) %u frames mean %+.2f rms %.2f max %.2f; "
                         "head turning (yaw over 30 deg/s) %u frames mean %+.2f rms %.2f max %.2f; pitch error (the view's elevation less %s) %u frames mean %+.2f rms %.2f max %.2f deg; "
                         "slope of the roll error against aim pitch x the head's yaw lead %+.3f over %u frames (x rms %.4f rad); head speed mean %.1f deg/s; aim pitch mean %+.1f deg; "
                         "reported head lead over the aim yaw %+.1f pitch %+.1f deg; frames in %u of %u s (mode %u of %u s, %.2f s active, flush #%u)",
                         seconds, modeName(mode), itemName(item), f61On ? "ON" : "off", g.frames, g.roll.mean(), g.roll.rms(), g.roll.maxAbs, g.over, g.still.n, g.still.mean(), g.still.rms(),
                         g.still.maxAbs, g.turning.n, g.turning.mean(), g.turning.rms(), g.turning.maxAbs,
                         mode ? "the headset's in LOCAL" : "the headset's in LOCAL plus the aim's pitch", g.pitch.n, g.pitch.mean(), g.pitch.rms(), g.pitch.maxAbs, g.slope(), g.slopeN, g.xRms(),
                         g.speed.mean(), g.aim.mean(), g.yawLead.mean(), g.pitchLead.mean(), active, seconds, modeActive, seconds, activeDurS, flushId);
}

}  // namespace f175
}  // namespace nofoot
}  // namespace edvr
