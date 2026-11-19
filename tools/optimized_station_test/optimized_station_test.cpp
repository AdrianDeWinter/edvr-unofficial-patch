// Test rig for the optimized station's pure half (src/d3d11/optimized_station_core.h): the seal trigger (the state machine that seals, releases
// and re-seals the station), the release rules (setting off, undocked, the camera leaving the bay, the pad moving against the hangar
// shell, a hangar change, launch), the pad-local frame's round trip and the release's threshold, the parse of the per-size floors, the own-ship
// prefix and sphere, the bay's volume, and the unreadable rule. No DLL and no game.
#include "../../src/d3d11/optimized_station_core.h"
#include "../../src/d3d11/concourse_glass_layout.h"   // the glass pixel shader recognised by its layout (pure; the container checksum needs DxilHash.cpp)
#include "../../src/d3d11/concourse_cull_core.h"   // the concourse cull: allow-list, gate, judge, rig verdict, pane triples (pure)
#include "../../src/d3d11/concourse_vr_bake_core.h"   // the concourse cull's VR bake: the aim, the conversions, the pitch, the face test (pure)
#include "../../src/d3d11/avatar_hooks_core.h"
#include "../../src/d3d11/journal_watch.h"   // journalSeatedFromFlags (a constexpr; the rig does not link the watcher)
#include "../../src/d3d11/station_gather_core.h"
#include "../../src/d3d11/station_lockfree_core.h"
#include "../../src/d3d11/station_pass_core.h"
#include "../../src/d3d11/station_seal_trace.h"
#include "../../src/d3d11/station_draw_mask.h"   // d3d11.h types only: the rig links nothing of D3D
#include "../../src/common/flat_profile_keys.h"
#include "../../src/d3d11/menu_flat_rows.h"
#include "../../src/d3d11/view_source.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <random>
#include <thread>
#include <vector>   // the build-334925 PE identity constants (store hook gate)

#include <cmath>
#include <limits>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace se = edvr::sealed;

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s\n", what);
    }
}

// The seal's inputs for a docked station whose own pad the game says is in the hangar (r1 1, fresh), with the camera in the bay.
se::SealInputs inHangar(uint64_t now) {
    se::SealInputs in;
    in.setting = true;
    in.docked = true;
    in.aboard = true;   // seated in the ship on the pad (Status.json); the on-foot cases clear it
    in.ownKnown = true;
    in.r1Fresh = true;
    in.inHangar = true;
    in.pickOk = true;
    in.cameraInBox = true;
    in.pad = 0x1000;
    in.nowMs = now;
    return in;
}

// Runs the trigger from t0 to t1 every 100 ms with the same inputs; returns the first engage request's time, or 0 when none came.
uint64_t firstEngage(se::SealState* st, se::SealInputs in, uint64_t t0, uint64_t t1) {
    se::ReleaseWhy why = se::ReleaseWhy::kNone;
    for (uint64_t t = t0; t <= t1; t += 100) {
        in.nowMs = t;
        if (se::sealStep(in, st, &why) == se::SealAction::kRequestEngage) return t;
    }
    return 0;
}

// The seal trace's emitter for the rig: every line, in order.
void captureLine(void* ctx, const char* line) {
    static_cast<std::vector<std::string>*>(ctx)->push_back(line);
}

// A docked, held tick with the own pad present in an informative pass and the held cell refreshed: quiet by every trigger. The rig changes one field.
se::TickRecord heldTick(uint32_t frame, uint64_t nowMs) {
    se::TickRecord r;
    r.frame = frame;
    r.nowMs = nowMs;
    r.docked = true;
    r.held = true;
    r.verdict = se::TraceVerdict::kInformative;
    r.ownSet = true;
    r.ownInPass = true;
    r.refresh = se::HeldRefresh::kRefreshed;
    r.camValid = true;
    return r;
}

// ---- The concourse cull's VR bake (src\d3d11\concourse_vr_bake_core.h): the conventions, each pinned against the game's own multiply (previewcam::compose) -----------------
namespace cvb = edvr::concourse_vr;
// An OpenVR 3x4 (row-major, column vectors) from a rotation and a translation.
void ovrPose(const double R[3][3], const double t[3], float m[12]) {
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) m[i * 4 + j] = static_cast<float>(R[i][j]);
        m[i * 4 + 3] = static_cast<float>(t[i]);
    }
}
void ovrRotY(double deg, double R[3][3]) {
    const double a = deg * 3.14159265358979 / 180.0, c = std::cos(a), s = std::sin(a);
    const double r[3][3] = {{c, 0, s}, {0, 1, 0}, {-s, 0, c}};
    std::memcpy(R, r, sizeof(r));
}
void ovrRotX(double deg, double R[3][3]) {
    const double a = deg * 3.14159265358979 / 180.0, c = std::cos(a), s = std::sin(a);
    const double r[3][3] = {{1, 0, 0}, {0, c, -s}, {0, s, c}};
    std::memcpy(R, r, sizeof(r));
}
void mat3Mul(const double A[3][3], const double B[3][3], double out[3][3]) {
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            out[i][j] = 0.0;
            for (int k = 0; k < 3; ++k) out[i][j] += A[i][k] * B[k][j];
        }
}
// X * E in OpenVR's column vectors: the eye's pose in the seated space.
void ovrCompose(const float X[12], const float E[12], float out[12]) {
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) out[i * 4 + j] = X[i * 4 + 0] * E[0 * 4 + j] + X[i * 4 + 1] * E[1 * 4 + j] + X[i * 4 + 2] * E[2 * 4 + j];
        out[i * 4 + 3] = X[i * 4 + 0] * E[3] + X[i * 4 + 1] * E[7] + X[i * 4 + 2] * E[11] + X[i * 4 + 3];
    }
}
bool poseNear(const cvb::Pose& a, const cvb::Pose& b, float tol) {
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 3; ++c)
            if (!(std::fabs(a.v[r * 4 + c] - b.v[r * 4 + c]) <= tol)) return false;
    return true;
}
bool vecNear(const float* a, const float* b, float tol) { return std::fabs(a[0] - b[0]) <= tol && std::fabs(a[1] - b[1]) <= tol && std::fabs(a[2] - b[2]) <= tol; }
// The camera 0x287B330 builds from an eye matrix: forward = normalize(row 2), right = normalize(row 1 x row 2), up = forward x right, position = row 3.
void gameCamera(const cvb::Pose& eye, float right[3], float up[3], float forward[3], float pos[3]) {
    for (int k = 0; k < 3; ++k) {
        forward[k] = eye.v[8 + k];
        pos[k] = eye.v[12 + k];
    }
    cvb::normalize3(forward);
    const float r1[3] = {eye.v[4], eye.v[5], eye.v[6]}, r2[3] = {eye.v[8], eye.v[9], eye.v[10]};
    cvb::cross3(r1, r2, right);
    cvb::normalize3(right);
    cvb::cross3(forward, right, up);
}

void testVrBake() {
    using cvb::Pose;
    // A head yawed 30 degrees and pitched 10, at a seated place; a canted left eye (8 degrees out) and its offset.
    double Ry[3][3], Rx[3][3], Rh[3][3], Re[3][3];
    ovrRotY(30.0, Ry);
    ovrRotX(10.0, Rx);
    mat3Mul(Ry, Rx, Rh);
    ovrRotY(8.0, Re);
    const double th[3] = {0.21, 1.62, -0.33}, te[3] = {-0.032, 0.0, 0.011};
    float X[12], E[12], XE[12];
    ovrPose(Rh, th, X);
    ovrPose(Re, te, E);
    ovrCompose(X, E, XE);
    const Pose hg = cvb::gameFromOpenVr(X), eg = cvb::gameFromOpenVr(E);
    // G is S*X*S in rows (docs\canted-projection.md: 0x4E3690 converts the head to S*H*S): the head's forward, OpenVR's -z, z-negated, is row 2.
    {
        const float fwdOvr[3] = {-static_cast<float>(Rh[0][2]), -static_cast<float>(Rh[1][2]), -static_cast<float>(Rh[2][2])};
        const float fwdGame[3] = {fwdOvr[0], fwdOvr[1], -fwdOvr[2]};
        check(vecNear(&hg.v[8], fwdGame, 1e-5f), "vr bake: G puts the head's forward (OpenVR -z, z negated) in row 2");
        const float tGame[3] = {static_cast<float>(th[0]), static_cast<float>(th[1]), static_cast<float>(-th[2])};
        check(vecNear(&hg.v[12], tGame, 1e-6f), "vr bake: G's row 3 is the translation with z negated");
        check(std::fabs(cvb::det3(hg) - 1.0f) < 1e-5f, "vr bake: G keeps a rotation proper");
    }
    // G(X * E) = G(E) * G(X): the eye matrix 0x4E2640 makes (E times H in rows) is G of the eye's seated pose.
    check(poseNear(cvb::mul(eg, hg), cvb::gameFromOpenVr(XE), 1e-5f), "vr bake: G(E) * G(H) in Elite's rows is G of the eye's pose X * E");
    check(poseNear(cvb::mul(hg, cvb::rigidInverse(hg)), cvb::identityPose(), 1e-5f), "vr bake: the rigid inverse undoes the head");
    // The axes of a room, not along the world's: a yaw of 40 degrees and a small tilt, right-handed (a0 x a1 = a2).
    double Ra[3][3], Rt[3][3], Rr[3][3];
    ovrRotY(40.0, Ra);
    ovrRotX(3.0, Rt);
    mat3Mul(Ra, Rt, Rr);
    float axes[3][3];
    for (int a = 0; a < 3; ++a)
        for (int k = 0; k < 3; ++k) axes[a][k] = static_cast<float>(Rr[k][a]);
    const float centre[3] = {1203.5f, -88.25f, 4410.0f};
    cvb::Face faces[cvb::kFaces];
    bool allFaces = true, properAll = true, mirrorAll = true;
    for (uint32_t d = 0; d < cvb::kFaces; ++d) {
        allFaces = allFaces && cvb::faceOf(axes, d, &faces[d]);
        properAll = properAll && std::fabs(cvb::det3(cvb::faceRows(faces[d], centre, 1.0f)) - 1.0f) < 1e-5f;
        mirrorAll = mirrorAll && std::fabs(cvb::det3(cvb::faceRows(faces[d], centre, -1.0f)) + 1.0f) < 1e-5f;
    }
    check(allFaces, "vr bake: the five faces are made from the room's axes");
    check(properAll && mirrorAll, "vr bake: a face's rows are a proper rotation, and a mirrored base flips it");
    check(vecNear(faces[0].f, axes[2], 1e-6f) && vecNear(faces[2].f, axes[0], 1e-6f) && vecNear(faces[4].f, axes[1], 1e-6f) && vecNear(faces[4].u, axes[2], 1e-6f),
          "vr bake: the faces look as flat's bakeForward does (0 +a2, 2 +a0, 4 up with a2 as its up)");
    {
        const float back[3] = {-axes[2][0], -axes[2][1], -axes[2][2]};
        check(vecNear(faces[1].f, back, 1e-6f) && vecNear(faces[1].u, axes[1], 1e-6f), "vr bake: face 1 looks along -a2 with the room's up");
        float rx[3];
        cvb::cross3(faces[0].u, faces[0].f, rx);
        check(vecNear(faces[0].r, rx, 1e-6f), "vr bake: right = up x forward, as 0x287B330 takes right = row 1 x row 2");
    }
    {
        float skew[3][3];
        std::memcpy(skew, axes, sizeof(skew));
        for (int k = 0; k < 3; ++k) skew[1][k] += 0.2f * axes[2][k];   // an up that leans toward forward
        cvb::Face f0;
        check(cvb::faceOf(skew, 0, &f0) && std::fabs(cvb::dot3(f0.u, f0.f)) < 1e-6f && std::fabs(cvb::dot3(f0.r, f0.f)) < 1e-6f && std::fabs(cvb::dot3(f0.u, f0.u) - 1.0f) < 1e-5f,
              "vr bake: skewed axes are made orthonormal, forward first");
        const float zero[3][3] = {};
        check(!cvb::faceOf(zero, 0, &f0) && !cvb::faceOf(axes, 5, &f0), "vr bake: no face from zero axes or past the fifth");
    }
    // The aim: B = H^-1 * D, and Elite's multiply of the head with it gives D back (native_preview_camera_core.h compose, the game's own).
    for (uint32_t d = 0; d < cvb::kFaces; ++d) {
        const cvb::Face pf = cvb::pitchFace(faces[d], 0.096f);
        const Pose rows = cvb::faceRows(pf, centre, 1.0f);
        const Pose b = cvb::aimBase(hg, rows);
        const Pose back = edvr::previewcam::compose(hg, b, cvb::identityPose());
        char what[96];
        std::snprintf(what, sizeof(what), "vr bake: face %u: B = H^-1 * D, and compose(H, B) is D", d);
        check(poseNear(back, rows, 5e-3f), what);   // translations of thousands of metres: float rounding
    }
    // The whole chain as Elite runs it: eye = (G(E) * G(H)) * B, and 0x287B330's camera from it. A parallel eye looks exactly along the face, from the centre less its offset.
    {
        const float ipdOnly[12] = {1, 0, 0, -0.032f, 0, 1, 0, 0, 0, 0, 1, 0};
        const Pose b = cvb::aimBase(hg, cvb::faceRows(faces[2], centre, 1.0f));
        const Pose eye = cvb::mul(cvb::mul(cvb::gameFromOpenVr(ipdOnly), hg), b);
        float r[3], u[3], f[3], p[3], pred[3];
        gameCamera(eye, r, u, f, p);
        cvb::eyeAt(ipdOnly, cvb::faceRows(faces[2], centre, 1.0f), pred);
        check(vecNear(f, faces[2].f, 1e-5f) && vecNear(u, faces[2].u, 1e-5f) && vecNear(r, faces[2].r, 1e-5f), "vr bake: a parallel eye's camera looks along the face, its up the face's up");
        check(vecNear(p, pred, 5e-3f), "vr bake: and sits where eyeAt predicts");
        float expect[3];
        for (int k = 0; k < 3; ++k) expect[k] = centre[k] - 0.032f * faces[2].r[k];
        check(vecNear(p, expect, 5e-3f), "vr bake: which is the centre less half the eyes' separation along the face's right");
    }
    // The eye stub's write fence (native_preview_camera.cpp bakeEye, the branch review 2026-11-08): a call is written only if its own pose is the base Elite is using.
    {
        const Pose base = cvb::aimBase(hg, cvb::faceRows(faces[2], centre, 1.0f));   // any proper pose with a translation will do as Elite's base
        Pose same = base, moved = base, turned = base, ident = cvb::identityPose(), nan = base;
        check(cvb::baseMatches(base.v, base.v), "vr bake: the fence takes the base Elite is using");
        same.v[12] += 0.2f;   // inside the 0.25 m
        check(cvb::baseMatches(same.v, base.v), "vr bake: and the same base a step later (0.2 m)");
        moved.v[13] += 0.3f;
        check(!cvb::baseMatches(moved.v, base.v), "vr bake: but not one 0.3 m away");
        // a rotation of about 6 degrees about up: the forward axis's dot is cos 6 = 0.9945, under 0.995
        const float c6 = std::cos(6.0f * 3.14159265f / 180.0f), s6 = std::sin(6.0f * 3.14159265f / 180.0f);
        for (int r = 0; r < 3; ++r) {
            const float x = base.v[r * 4], z = base.v[r * 4 + 2];
            turned.v[r * 4] = c6 * x + s6 * z;
            turned.v[r * 4 + 2] = -s6 * x + c6 * z;
        }
        check(!cvb::baseMatches(turned.v, base.v), "vr bake: nor one turned 6 degrees");
        const float c3 = std::cos(3.0f * 3.14159265f / 180.0f), s3 = std::sin(3.0f * 3.14159265f / 180.0f);
        Pose slight = base;
        for (int r = 0; r < 3; ++r) {
            const float x = base.v[r * 4], z = base.v[r * 4 + 2];
            slight.v[r * 4] = c3 * x + s3 * z;
            slight.v[r * 4 + 2] = -s3 * x + c3 * z;
        }
        check(cvb::baseMatches(slight.v, base.v), "vr bake: but one turned 3 degrees is still it");
        Pose distant = cvb::identityPose();
        distant.v[12] = 5.0f;
        check(!cvb::baseMatches(ident.v, distant.v) && cvb::baseMatches(ident.v, ident.v), "vr bake: the heap calls' identity at the origin is not a base 5 m away, and is itself");
        nan.v[5] = std::nanf("");
        check(!cvb::baseMatches(nan.v, base.v) && !cvb::baseMatches(base.v, nan.v), "vr bake: a pose with a NaN never matches, either way round");
        Pose zeroAxes = base;
        for (int k = 0; k < 4; ++k) zeroAxes.v[k] = 0.0f;
        check(!cvb::baseMatches(zeroAxes.v, base.v), "vr bake: a pose whose axis cannot be normalised never matches");
        // Flight 54: the game leaves each row's fourth lane unset, and a fence that read it refused every call.
        Pose padded = base, paddedLater = same;
        for (int r = 0; r < 4; ++r) {
            padded.v[r * 4 + 3] = std::nanf("");
            paddedLater.v[r * 4 + 3] = std::numeric_limits<float>::infinity();
        }
        check(cvb::baseMatches(padded.v, base.v) && cvb::baseMatches(base.v, padded.v) && cvb::baseMatches(paddedLater.v, padded.v),
              "vr bake: the fourth lanes, which the game does not set, are not read (Flight 54)");
    }
    // A canted eye: the camera's forward is the face's turned by the eye's own rotation, read through S (the true eye in OpenVR's head axes, z negated, over the face's rows).
    {
        const Pose rows = cvb::faceRows(faces[0], centre, 1.0f);
        const Pose eye = cvb::mul(cvb::mul(eg, hg), cvb::aimBase(hg, rows));
        float r[3], u[3], f[3], p[3];
        gameCamera(eye, r, u, f, p);
        const double qo[3] = {-Re[0][2], -Re[1][2], -Re[2][2]};   // the eye's forward in the head's OpenVR axes
        const float q[3] = {static_cast<float>(qo[0]), static_cast<float>(qo[1]), static_cast<float>(-qo[2])};
        float expect[3];
        for (int k = 0; k < 3; ++k) expect[k] = q[0] * faces[0].r[k] + q[1] * faces[0].u[k] + q[2] * faces[0].f[k];
        check(vecNear(f, expect, 1e-5f), "vr bake: a canted eye's forward is its own cant, in the face's frame");
        check(std::fabs(cvb::dot3(f, faces[0].f) - static_cast<float>(std::cos(8.0 * 3.14159265358979 / 180.0))) < 1e-5f, "vr bake: 8 degrees off the face, as the cant");
        float pred[3];
        cvb::eyeAt(E, rows, pred);
        check(vecNear(p, pred, 5e-3f), "vr bake: and its place is eyeAt's");
    }
    // The pitch that centres a face in the frusta (docs\concourse-cull-2026-11-05.md "Flight 51 results"): a Quest 3 up 42, down 53, pitched up about 5.5; the Pimax Crystal Super
    // at plus and minus 46.8, level.
    {
        const auto t = [](double deg) { return static_cast<float>(std::tan(deg * 3.14159265358979 / 180.0)); };
        const float quest[2][4] = {{-t(54.0), t(42.0), -t(53.0), t(42.0)}, {-t(42.0), t(54.0), -t(53.0), t(42.0)}};
        const float pimax[2][4] = {{-t(51.4), t(41.2), -t(46.8), t(46.8)}, {-t(41.2), t(51.4), -t(46.8), t(46.8)}};
        const float narrow[2][4] = {{-t(51.4), t(41.2), -t(44.0), t(44.0)}, {-t(41.2), t(51.4), -t(44.0), t(44.0)}};
        const bool both[2] = {true, true}, none[2] = {false, false};
        const float parallel[2][12] = {{1, 0, 0, -0.032f, 0, 1, 0, 0, 0, 0, 1, 0}, {1, 0, 0, 0.032f, 0, 1, 0, 0, 0, 0, 1, 0}};
        const float pq = cvb::pitchForFrusta(quest, both), pp = cvb::pitchForFrusta(pimax, both);
        check(std::fabs(pq * 57.29578f - 5.5f) < 0.05f, "vr bake: a Quest 3's frusta (up 42, down 53) pitch the head up 5.5 degrees");
        check(std::fabs(pp) < 1e-6f, "vr bake: the Pimax's (46.8 both ways) leave it level");
        check(cvb::pitchForFrusta(quest, none) == 0.0f, "vr bake: no frustum read: no pitch");
        check(cvb::faceSpare(quest, parallel, both, 0.0f) < 0.0f, "vr bake: level, the Quest 3 misses the face's top (42 under 45)");
        const float sq = cvb::faceSpare(quest, parallel, both, pq);
        check(sq > 0.05f && sq < 0.09f, "vr bake: pitched, it covers the face with about 2.5 degrees spare (0.076 in tangent)");
        const float sp = cvb::faceSpare(pimax, parallel, both, 0.0f);
        check(sp > 0.05f && sp < 0.08f, "vr bake: the Pimax's two eyes cover the face, about 1.5 degrees spare");
        const bool one[2] = {true, false};
        check(cvb::faceSpare(pimax, parallel, one, 0.0f) < 0.0f, "vr bake: one Pimax eye alone does not (its inner side is 41.2)");
        check(cvb::faceSpare(narrow, parallel, both, 0.0f) < 0.0f, "vr bake: frusta under 45 vertically do not");
    }
    // The face test (the capture shader's twin) and the faces' tiling: with the down face added, every direction lies on exactly one face's square.
    {
        const float* f = faces[2].f;
        const float* r = faces[2].r;
        const float* u = faces[2].u;
        const auto dirOf = [&](float a, float b, float out[3]) {
            for (int k = 0; k < 3; ++k) out[k] = f[k] + a * r[k] + b * u[k];
            cvb::normalize3(out);
        };
        float d[3];
        dirOf(0.0f, 0.0f, d);
        check(cvb::onFace(d, faces[2], cvb::kFaceSlack), "vr bake: the face's centre is on it");
        dirOf(0.96f, -0.96f, d);
        check(cvb::onFace(d, faces[2], cvb::kFaceSlack), "vr bake: a corner just inside is on it");
        dirOf(1.001f, 0.0f, d);
        check(cvb::onFace(d, faces[2], cvb::kFaceSlack) && !cvb::onFace(d, faces[2], 1.0f), "vr bake: an edge 0.1 percent past is taken with the slack only");
        dirOf(1.05f, 0.0f, d);
        check(!cvb::onFace(d, faces[2], cvb::kFaceSlack), "vr bake: 5 percent past the edge is not");
        const float behind[3] = {-f[0], -f[1], -f[2]};
        check(!cvb::onFace(behind, faces[2], cvb::kFaceSlack), "vr bake: behind is not");
        cvb::Face all[6];
        for (int k = 0; k < 5; ++k) all[k] = faces[k];
        for (int k = 0; k < 3; ++k) {
            all[5].f[k] = -axes[1][k];
            all[5].u[k] = axes[2][k];
        }
        cvb::cross3(all[5].u, all[5].f, all[5].r);
        uint32_t samples = 0, once = 0;
        for (uint32_t face = 0; face < 6; ++face)
            for (int a = 0; a < 9; ++a)
                for (int b = 0; b < 9; ++b) {
                    float dir[3];
                    edvr::concourse::cubeDirOf(face, 0.053f + 0.1f * static_cast<float>(a), 0.047f + 0.1f * static_cast<float>(b), dir);
                    cvb::normalize3(dir);
                    int n = 0;
                    for (const cvb::Face& fc : all) n += cvb::onFace(dir, fc, 1.0f) ? 1 : 0;
                    ++samples;
                    once += n == 1 ? 1u : 0u;
                }
        check(samples == once, "vr bake: the five faces and the floor tile the cube's directions, each on exactly one square");
    }
    // Which face a frame shows: the nearest wanted one within about 35 degrees.
    {
        float fw[3];
        for (int k = 0; k < 3; ++k) fw[k] = faces[2].f[k] * 0.98f + faces[2].u[k] * 0.17f;   // about 10 degrees up
        check(cvb::faceOfView(fw, faces, 0x1Fu, cvb::kFaceMinDot) == 2, "vr bake: a frame looking 10 degrees above face 2 shows face 2");
        check(cvb::faceOfView(fw, faces, 0x1Bu, cvb::kFaceMinDot) == -1, "vr bake: not when face 2 is not wanted");
        float diag[3];
        for (int k = 0; k < 3; ++k) diag[k] = faces[2].f[k] + faces[0].f[k];   // 45 degrees between two faces
        check(cvb::faceOfView(diag, faces, 0x1Fu, cvb::kFaceMinDot) == -1, "vr bake: half way between two faces shows neither");
        const float zero[3] = {};
        check(cvb::faceOfView(zero, faces, 0x1Fu, cvb::kFaceMinDot) == -1, "vr bake: no forward, no face");
    }
    // The world check.
    {
        const float store[3] = {100.0f, 20.0f, -5.0f}, near1[3] = {100.3f, 20.1f, -5.2f}, far1[3] = {101.363f, -9.0f, 12.0f};
        float off[3], dist = 0.0f;
        check(!cvb::worldOffset(store, near1, off, &dist) && dist < 0.5f, "vr bake: the world check: within half a metre is one world");
        check(cvb::worldOffset(store, far1, off, &dist) && std::fabs(off[1] + 29.0f) < 1e-4f && dist > 30.0f, "vr bake: tens of metres is an offset, render less store");
    }
    // The aims: each face held for kVrBakeDwell ticks in order (every held frame writes: the cube keeps the union), a short one again once its counts are due, three times at most, then let go.
    {
        cvb::AimPlan p;
        p.reset(0x1Fu);
        check(cvb::kVrBakeDwell == 3, "vr bake: the dwell is three frames");
        bool fresh = false, orderOk = true, dwellOk = true, freshOk = true, aimsOk = true;
        uint32_t dw = 99;
        for (uint32_t t = 1; t <= 15; ++t) {
            const uint32_t f = p.next(t, &fresh, &dw);
            orderOk = orderOk && f == (t - 1) / 3;
            dwellOk = dwellOk && dw == (t - 1) % 3;
            freshOk = freshOk && fresh == (dw == 0);
            aimsOk = aimsOk && p.aims(f) == 1;
        }
        check(orderOk && dwellOk && freshOk && aimsOk, "vr bake: five faces give fifteen ticks in face order, dwell 0, 1, 2 each, fresh only on dwell 0, one aim a face");
        for (uint32_t f = 0; f < 5; ++f) p.noteCoverage(f, f == 2 ? 0.9f : 1.0f);
        check(p.covered() == 0x1Bu && !p.finished(15), "vr bake: a face at 90 percent is short, and the bake goes on");
        check(p.next(16, &fresh, &dw) == 2 && fresh && dw == 0 && p.aims(2) == 2, "vr bake: then it is aimed at again, from its last frame's tick");
        check(p.next(17, &fresh, &dw) == 2 && !fresh && dw == 1 && p.next(18, &fresh, &dw) == 2 && !fresh && dw == 2 && p.aims(2) == 2, "vr bake: held for its dwell, still one aim");
        check(p.next(24, &fresh, &dw) == 2 && !fresh && dw == cvb::kVrBakeDwell, "vr bake: a short face waits for its counts' ticks (the last stays, counts only)");
        check(p.next(25, &fresh, &dw) == 2 && fresh && dw == 0 && p.aims(2) == 3, "vr bake: and once more");
        check(p.next(26, &fresh, &dw) == 2 && p.next(27, &fresh, &dw) == 2 && dw == 2, "vr bake: held again");
        check(p.next(40, &fresh, &dw) == 2 && !fresh && p.aims(2) == 3, "vr bake: not a fourth time");
        check(!p.finished(33) && p.finished(34), "vr bake: the bake ends once its last aim's last frame's counts are due");
        check(std::fabs(p.best(2) - 0.9f) < 1e-6f, "vr bake: the best coverage is kept for the finish line");
        p.reset(0x10u);
        check(p.next(1, &fresh, &dw) == 4 && fresh && dw == 0 && p.next(2, &fresh, &dw) == 4 && !fresh && dw == 1 && p.next(3, &fresh, &dw) == 4 && dw == 2 &&
                  p.next(4, &fresh, &dw) == 4 && !fresh && dw == cvb::kVrBakeDwell,
              "vr bake: one face wanted: it alone");
        p.noteCoverage(4, 0.995f);
        check(p.finished(5) && p.covered() == 0x10u, "vr bake: covered, finished at once");
        p.reset(0x1Fu);
        p.noteCoverage(9, 1.0f);
        check(p.covered() == 0u, "vr bake: a face out of range is ignored");
    }
    // The capture shader's inverse: one text for the C++ and the HLSL; against the flat bake's own inverse, on a reversed infinite camera.
    {
        const double sx = 0.5625, sy = 1.0, nearP = 5.0;
        const double right[3] = {0.5, 0.866025404, 0.0}, up[3] = {0.866025404, -0.5, 0.0}, fwd[3] = {0.0, 0.0, -1.0};
        float rows[4][4] = {};
        for (int i = 0; i < 3; ++i) {   // the flat test's camera (m[0 + i], m[4 + i], m[12 + i] and m[8 + 3]) written as the b1 rows that make it: rows[c][r] = m[r * 4 + c]
            rows[i][0] = static_cast<float>(sx * right[i]);
            rows[i][1] = static_cast<float>(sy * up[i]);
            rows[i][3] = static_cast<float>(fwd[i]);
        }
        rows[3][2] = static_cast<float>(nearP);
        float m[16], inv[16];
        cvb::clipColumns(rows, m);
        double md[16], invd[16];
        for (int i = 0; i < 16; ++i) md[i] = m[i];
        check(m[0 * 4 + 1] == rows[1][0] && m[3 * 4 + 2] == rows[2][3] && m[2 * 4 + 3] == rows[3][2], "vr bake: the clip matrix's columns are the rows, as the flat bake builds it");
        check(cvb::inverse4(m, inv) && edvr::concourse::invert4(md, invd), "vr bake: the cofactor inverse and the flat bake's both invert the camera");
        bool same = true;
        for (int i = 0; i < 16; ++i) same = same && std::fabs(inv[i] - static_cast<float>(invd[i])) < 1e-4f * (1.0f + std::fabs(static_cast<float>(invd[i])));
        check(same, "vr bake: and agree");
        const float flatM[16] = {1, 0, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 1};
        check(!cvb::inverse4(flatM, inv), "vr bake: a singular matrix is refused");
        const char* h = cvb::inverse4Hlsl();
        int lines = 0;
        for (const char* q = std::strstr(h, "    inv["); q; q = std::strstr(q + 1, "    inv[")) ++lines;
        check(std::strstr(h, "bool invert4(float m[16], out float inv[16])") != nullptr && lines == 16, "vr bake: the HLSL twin has the function and its sixteen cofactors");
        check(std::strstr(h, "inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14]") != nullptr && std::strstr(h, "float det = m[0] * inv[0] + m[1] * inv[4]") != nullptr,
              "vr bake: written from the same text as the C++");
    }
}

// ---- The glass pixel shader's layout (concourse_glass_layout.h): synthetic programs, no game bytecode ----------------------------------------------
namespace glt {
struct Spec {
    std::vector<std::pair<uint32_t, uint32_t>> cbs = {{1, 327}, {2, 3}};              // slot, size
    std::vector<std::pair<uint32_t, uint32_t>> inputs = {{0, 0x1}, {1, 0x7}, {2, 0x7}, {3, 0x7}, {4, 0x3}};   // register, mask
    std::vector<std::pair<uint32_t, uint32_t>> outputs = {{0, 0xF}};                  // register, mask
    uint32_t outputType = 2;
    bool sgvInput = false;
    bool sgvOutput = false;
    uint32_t programType = 0x50;
    bool customData = false;
};
void tok(std::vector<uint32_t>& t, uint32_t opcode, std::initializer_list<uint32_t> body) {
    t.push_back(opcode | (static_cast<uint32_t>(body.size() + 1) << 24));
    t.insert(t.end(), body.begin(), body.end());
}
std::vector<unsigned char> build(const Spec& sp) {
    std::vector<uint32_t> t = {sp.programType, 0};
    if (sp.customData) {   // an immediate constant buffer: opcode 53, class 3, then the length word, then four values
        t.push_back(53u | (3u << 11));
        t.push_back(6);
        for (int i = 0; i < 4; ++i) t.push_back(0);
    }
    for (const auto& cb : sp.cbs) tok(t, 89, {0x00208882u, cb.first, cb.second});
    for (const auto& in : sp.inputs) tok(t, 98 | (2u << 11), {0x00101002u | (in.second << 4), in.first});
    if (sp.sgvInput) tok(t, 99, {0x00101002u | (1u << 4), 7, 1});   // dcl_input_ps_sgv, ignored
    for (const auto& out : sp.outputs) tok(t, 101, {(sp.outputType << 12) | 0x00100002u | (out.second << 4), out.first});
    if (sp.sgvOutput) tok(t, 102, {0x00102002u | (1u << 4), 0, 1});
    tok(t, 104, {1});   // dcl_temps 1
    t.push_back(62u | (1u << 24));   // ret
    t[1] = static_cast<uint32_t>(t.size());
    edvr::dxbc_container::Chunk c;
    c.tag = 0x58454853u;
    c.bytes.resize(t.size() * 4);
    std::memcpy(c.bytes.data(), t.data(), c.bytes.size());
    auto out = edvr::dxbc_container::makeContainer({c});
    return std::vector<unsigned char>(out.begin(), out.end());
}
bool ok(const Spec& sp) {
    const auto b = build(sp);
    return edvr::concourse::glassLayoutPs(b.data(), b.size());
}
void testAll() {
    using edvr::concourse::glassLayoutPs;
    Spec base;
    check(ok(base), "glass layout: the exact layout passes");
    { Spec s = base; s.customData = true; check(ok(s), "glass layout: custom data among the declarations is skipped"); }
    { Spec s = base; s.sgvInput = true; check(ok(s), "glass layout: a dcl_input_ps_sgv is not counted"); }
    { Spec s = base; s.cbs[0].second = 100; check(ok(s), "glass layout: constant buffer 1 of another size passes"); }
    { Spec s = base; s.cbs[1].second = 2; check(!ok(s), "glass layout: constant buffer 2 of size 2 (the logo) fails"); }
    { Spec s = base; s.cbs.push_back({0, 4}); check(!ok(s), "glass layout: a third constant buffer fails"); }
    { Spec s = base; s.cbs.pop_back(); check(!ok(s), "glass layout: a missing constant buffer 2 fails"); }
    { Spec s = base; s.cbs[0].first = 0; check(!ok(s), "glass layout: constant buffer 0 in place of 1 fails"); }
    { Spec s = base; s.inputs[2].second = 0xF; check(!ok(s), "glass layout: v2.xyzw fails"); }
    { Spec s = base; s.inputs.push_back({5, 0x3}); check(!ok(s), "glass layout: a sixth input fails"); }
    { Spec s = base; s.inputs.pop_back(); check(!ok(s), "glass layout: a missing input fails"); }
    { Spec s = base; s.inputs[4].second = 0x7; check(!ok(s), "glass layout: v4.xyz fails"); }
    { Spec s = base; s.outputs.push_back({1, 0xF}); check(!ok(s), "glass layout: a second output fails"); }
    { Spec s = base; s.outputs[0] = {1, 0xF}; check(!ok(s), "glass layout: o1 in place of o0 fails"); }
    { Spec s = base; s.outputs[0].second = 0x7; check(!ok(s), "glass layout: o0.xyz fails"); }
    { Spec s = base; s.outputType = 12; s.outputs[0].second = 0x1; check(!ok(s), "glass layout: an oDepth output fails"); }
    { Spec s = base; s.sgvOutput = true; check(!ok(s), "glass layout: a dcl_output_sgv fails"); }
    { Spec s = base; s.programType = 0x00010050u; check(!ok(s), "glass layout: a vs_5_0 program fails"); }
    const auto good = build(base);
    check(!glassLayoutPs(nullptr, 0), "glass layout: no bytes fail");
    check(!glassLayoutPs(good.data(), 0), "glass layout: an empty buffer fails");
    check(!glassLayoutPs(good.data(), 7), "glass layout: seven bytes fail");
    bool anyTruncatedPassed = false;
    for (size_t n = 1; n < good.size(); ++n)
        if (glassLayoutPs(good.data(), n)) anyTruncatedPassed = true;
    check(!anyTruncatedPassed, "glass layout: every truncation of a good container fails");
    std::vector<unsigned char> junk(256);
    for (size_t i = 0; i < junk.size(); ++i) junk[i] = static_cast<unsigned char>(i * 37 + 11);
    check(!glassLayoutPs(junk.data(), junk.size()), "glass layout: garbage bytes fail");
    std::vector<unsigned char> sized = good;
    const uint32_t wrongSize = static_cast<uint32_t>(sized.size() + 4);
    std::memcpy(sized.data() + 24, &wrongSize, 4);
    check(!glassLayoutPs(sized.data(), sized.size()), "glass layout: a container whose size field is not its length fails");
    std::vector<unsigned char> digest = good;
    digest[4] ^= 0x55;   // the checksum is not read: D3D has accepted the bytecode before the check runs
    check(glassLayoutPs(digest.data(), digest.size()), "glass layout: the container's checksum is not what decides");
}
// Local mode (--scan <dir> ...): every ps_*.dxbc in the folders is run through glassLayoutPs; the matches and the count of distinct pixel shaders are printed.
int scan(int argc, char** argv) {
    namespace fs = std::filesystem;
    std::vector<std::string> matches, seen;
    for (int i = 2; i < argc; ++i) {
        std::error_code ec;
        for (fs::directory_iterator it(argv[i], ec), end; !ec && it != end; it.increment(ec)) {
            const std::string name = it->path().filename().string();
            if (name.size() < 8 || name.compare(0, 3, "ps_") != 0 || it->path().extension() != ".dxbc") continue;
            const std::string hash = name.substr(3, name.size() - 3 - 5);
            if (std::find(seen.begin(), seen.end(), hash) != seen.end()) continue;
            seen.push_back(hash);
            FILE* f = std::fopen(it->path().string().c_str(), "rb");
            if (!f) continue;
            std::vector<unsigned char> bytes;
            unsigned char buf[4096];
            size_t got;
            while ((got = std::fread(buf, 1, sizeof(buf), f)) > 0) bytes.insert(bytes.end(), buf, buf + got);
            std::fclose(f);
            if (edvr::concourse::glassLayoutPs(bytes.data(), bytes.size())) matches.push_back(hash);
        }
        if (ec) std::printf("scan: %s: %s\n", argv[i], ec.message().c_str());
    }
    std::sort(matches.begin(), matches.end());
    for (const auto& m : matches) std::printf("match ps_%s\n", m.c_str());
    std::printf("scan: %zu distinct pixel shaders checked, %zu match\n", seen.size(), matches.size());
    return 0;
}
}  // namespace glt

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--scan") == 0) return glt::scan(argc, argv);
    if (argc > 1 && std::strcmp(argv[1], "--self-test") != 0) {
        std::printf("usage: optimized_station_test --self-test\n");
        return 2;
    }

    // ---- Built v14.23 (2026-10-13): the seal is the game's pad state (r1 1 on the own pad). The geometric trigger has no tests any more ---------------
    {
        // Load-in: the game says the own pad is in the hangar (r1 1, fresh) and the camera is in the bay: the seal comes on the first eligible frame.
        se::SealState st;
        check(firstEngage(&st, inHangar(0), 100, 10000) == 100, "v14.23 load-in: the own pad in the hangar by the game (r1 1) seals on the first eligible frame (100 ms)");
    }
    {
        // The surface of an outpost: r0 0 and r1 0. The pad never seals (no fallback), and the sun stays on.
        se::SealState st;
        se::SealInputs in = inHangar(0);
        in.inHangar = false;
        check(firstEngage(&st, in, 0, 60000) == 0, "v14.23 surface: r0 0 and r1 0 (outside the hangar) never seals, with no fallback");
        se::SunState sun;
        se::SunInputs si;
        si.setting = true;
        si.docked = true;
        si.ownKnown = true;
        si.r0Fresh = true;
        si.r0 = 0;
        si.r1Fresh = true;
        si.inHangar = false;
        si.relOk = true;
        si.rel = -53.5f;
        const char* why = "";
        check(!se::sunStep(si, &sun, &why) && !sun.off, "v14.23 surface: the sun stays on at the outpost surface (r0 0)");
    }
    {
        // A port on a planet: its pad descends like a station's (r0 3, then r0 1 and r1 1 at a CraterOutpost, Flight 34), and the sun stays on for all of it, the concourse's bay window included.
        namespace js = edvr::journal_station;
        se::SunInputs si;
        si.setting = true;
        si.docked = true;
        si.ownKnown = true;
        si.r0Fresh = true;
        si.r0 = 3;
        si.r1Fresh = true;
        si.inHangar = true;
        si.relOk = true;
        si.rel = -6.0f;
        const char* why = "";
        se::SunState orbital;
        check(se::sunStep(si, &orbital, &why) && orbital.off, "surface port: the same pad at an orbital station turns the sun off");
        si.surfacePort = true;
        se::SunState surface;
        check(!se::sunStep(si, &surface, &why) && !surface.off, "surface port: the pad descending does not turn the sun off");
        si.r0 = 1;
        check(!se::sunStep(si, &surface, &why) && !surface.off, "surface port: nor the pad locked in");
        // What the journal says may arrive after the pad has moved: then it comes back on, once, with a reason.
        se::SunState late;
        late.off = true;
        check(se::sunStep(si, &late, &why) && !late.off && why[0] != '\0', "surface port: a sun already off comes back on and says why");
        check(!se::sunStep(si, &late, &why) && !late.off, "surface port: and says it once");

        const uint32_t kOrbitalFlags = 0x00000005u, kOrbitalFlags2 = 0x00014009u;   // on foot, an Orbis, Flight 34
        const uint32_t kSurfaceFlags = 0x00200005u, kSurfaceFlags2 = 0x00014011u;   // on foot, a CraterOutpost, Flight 34
        for (js::Kind k : {js::Kind::kCraterPort, js::Kind::kCraterOutpost, js::Kind::kSurfaceStation, js::Kind::kOnFootSettlement})
            check(js::atSurfacePort(k, 0, 0), "surface port: the type alone says so");
        for (js::Kind k : {js::Kind::kCoriolis, js::Kind::kOrbis, js::Kind::kOcellus, js::Kind::kOutpost, js::Kind::kAsteroidBase, js::Kind::kFleetCarrier, js::Kind::kMegaShip})
            check(!js::atSurfacePort(k, kSurfaceFlags, kSurfaceFlags2), "surface port: a type in space wins over a latitude");
        check(js::atSurfacePort(js::Kind::kUnknown, kSurfaceFlags, kSurfaceFlags2), "surface port: a Disembark on a planet names no station, and Status says it");
        check(js::atSurfacePort(js::Kind::kUnknown, 0x0121000Du, 0), "surface port: docked in the ship, a latitude says it");
        check(js::atSurfacePort(js::Kind::kUnknown, 0, kSurfaceFlags2), "surface port: on foot on a planet says it");
        check(!js::atSurfacePort(js::Kind::kUnknown, kOrbitalFlags, kOrbitalFlags2), "surface port: an orbital station with no type is not one");
        check(!js::atSurfacePort(js::Kind::kUnknown, 0, 0), "surface port: nothing known is not one");
        check(!js::atSurfacePort(js::Kind::kOther, kOrbitalFlags, kOrbitalFlags2) && js::atSurfacePort(js::Kind::kOther, kSurfaceFlags, kSurfaceFlags2), "surface port: a type not listed goes by Status");
        // A fleet carrier, on foot: neither in a station nor on a planet (Flights 39, 41, 42: the concourse 0x00014001, the hangar 0x00016001).
        const uint32_t kCarrierFlags = 0x00000005u, kCarrierFlags2 = 0x00014001u, kCarrierHangarFlags2 = 0x00016001u;
        check(js::atCarrier(js::Kind::kFleetCarrier, 0, 0), "carrier: the type alone says so");
        check(js::atCarrier(js::Kind::kUnknown, kCarrierFlags, kCarrierFlags2) && js::atCarrier(js::Kind::kUnknown, kCarrierFlags, kCarrierHangarFlags2),
              "carrier: a load on foot names none, and Status says it, in the concourse and the hangar");
        check(!js::atCarrier(js::Kind::kUnknown, kOrbitalFlags, kOrbitalFlags2) && !js::atCarrier(js::Kind::kUnknown, kOrbitalFlags, 0x00016009u), "carrier: an orbital station with no type is not one");
        check(!js::atCarrier(js::Kind::kUnknown, kSurfaceFlags, kSurfaceFlags2), "carrier: nor a port on a planet");
        check(!js::atCarrier(js::Kind::kUnknown, 0, kCarrierFlags2) && !js::atCarrier(js::Kind::kUnknown, kCarrierFlags, 0) && !js::atCarrier(js::Kind::kUnknown, 0, 0),
              "carrier: not docked, not on foot, or nothing known is not one");
        for (js::Kind k : {js::Kind::kOrbis, js::Kind::kCoriolis, js::Kind::kOutpost, js::Kind::kCraterOutpost, js::Kind::kMegaShip, js::Kind::kOther})
            check(!js::atCarrier(k, kCarrierFlags, kCarrierFlags2), "carrier: another type says it is not one, whatever Status says");
    }
    {
        // A descent: r0 3 (the descent has started) turns the sun off; the pad is not yet in the bay, so no seal.
        se::SunState sun;
        se::SunInputs si;
        si.setting = true;
        si.docked = true;
        si.ownKnown = true;
        si.r0Fresh = true;
        si.r0 = 3;
        si.r1Fresh = true;
        si.inHangar = false;
        si.relOk = true;
        si.rel = -20.0f;
        const char* why = "";
        check(se::sunStep(si, &sun, &why) && sun.off && std::strcmp(why, "pad moving in or locked in (r0)") == 0,
              "v14.23 descent: r0 3 turns the sun off");
        se::SealState st;
        se::SealInputs in = inHangar(0);
        in.inHangar = false;
        check(firstEngage(&st, in, 0, 10000) == 0, "v14.23 descent: r1 0 during the descent: no seal");
    }
    {
        // The lock: r1 turns 1 (the game says the pad is in the hangar): the seal comes at once; r0 1 turns the sun off.
        se::SealState st;
        check(firstEngage(&st, inHangar(0), 4000, 10000) == 4000, "v14.23 lock: r1 1 on the own pad seals at once (the first eligible frame)");
        se::SunState sun;
        se::SunInputs si;
        si.setting = true;
        si.docked = true;
        si.ownKnown = true;
        si.r0Fresh = true;
        si.r0 = 1;
        si.r1Fresh = true;
        si.inHangar = true;
        const char* why = "";
        check(se::sunStep(si, &sun, &why) && sun.off, "v14.23 lock: r0 1 (moving in) turns the sun off");
    }
    {
        // Toggle off and on while r1 is 1: the bay re-seals at once (no edge is needed for a setting release).
        se::SealState st;
        se::ReleaseWhy w = se::ReleaseWhy::kNone;
        se::SealInputs in = inHangar(100);
        check(se::sealStep(in, &st, &w) == se::SealAction::kRequestEngage, "v14.23 toggle: the seal in the hangar");
        in.held = true;
        in.setting = false;
        check(se::sealStep(in, &st, &w) == se::SealAction::kRelease && w == se::ReleaseWhy::kSettingOff, "v14.23 toggle: the setting off releases");
        in.held = false;
        in.setting = true;
        in.nowMs = 200;
        check(se::sealStep(in, &st, &w) == se::SealAction::kRequestEngage, "v14.23 toggle: the setting on again, with r1 1, re-seals at once");
    }
    {
        // A mechanism release (the launch) while r1 stays 1: no re-seal for 2 s of r1 1; r1 0 then 1 (the fresh edge) re-seals.
        se::SealState st;
        se::ReleaseWhy w = se::ReleaseWhy::kNone;
        se::SealInputs in = inHangar(0);
        check(se::sealStep(in, &st, &w) == se::SealAction::kRequestEngage, "v14.23 mechanism: the setup seal");
        in.held = true;
        in.mechanismMoved = true;
        in.nowMs = 100;
        check(se::sealStep(in, &st, &w) == se::SealAction::kRelease && w == se::ReleaseWhy::kMechanism, "v14.23 mechanism: the bay mechanism moving releases");
        in.held = false;
        in.mechanismMoved = false;
        uint64_t early = 0;
        for (uint64_t t = 200; t <= 2500; t += 100) {
            in.nowMs = t;
            if (se::sealStep(in, &st, &w) == se::SealAction::kRequestEngage && early == 0) early = t;
        }
        check(early == 0, "v14.23 mechanism: with r1 still 1 after the launch release, nothing re-seals (2 s)");
        in.inHangar = false;
        in.nowMs = 2600;
        check(se::sealStep(in, &st, &w) == se::SealAction::kNone, "v14.23 mechanism: r1 0 is the first half of the fresh edge (no seal)");
        in.inHangar = true;
        in.nowMs = 2700;
        check(se::sealStep(in, &st, &w) == se::SealAction::kRequestEngage, "v14.23 mechanism: r1 back to 1 after the 0 re-seals");
    }
    {
        // An r1 1 to 0 edge (the pad leaves the bay) releases the seal.
        se::SealState st;
        se::ReleaseWhy w = se::ReleaseWhy::kNone;
        se::SealInputs in = inHangar(0);
        se::sealStep(in, &st, &w);
        in.held = true;
        in.inHangar = false;
        check(se::sealStep(in, &st, &w) == se::SealAction::kRelease && w == se::ReleaseWhy::kInHangarLeft, "v14.23 edge: r1 1 to 0 releases the seal");
    }
    {
        // The camera leaves and re-enters the bay: the seal re-takes without any in-hangar edge (r1 stayed 1).
        se::SealState st;
        se::ReleaseWhy w = se::ReleaseWhy::kNone;
        se::SealInputs in = inHangar(0);
        se::sealStep(in, &st, &w);
        in.held = true;
        in.cameraOutside = true;
        bool left = false;
        for (uint64_t t = 100; t <= 1000; t += 100) {
            in.nowMs = t;
            if (se::sealStep(in, &st, &w) == se::SealAction::kRelease && w == se::ReleaseWhy::kCameraLeft) left = true;
        }
        check(left, "v14.23 camera: the camera outside the box for kCameraFrames releases the seal");
        in.held = false;
        in.cameraOutside = false;
        in.cameraInBox = false;
        in.nowMs = 1100;
        se::sealStep(in, &st, &w);
        uint64_t back = 0;
        for (uint64_t t = 1200; t <= 4000; t += 100) {
            in.cameraInBox = true;
            in.nowMs = t;
            if (se::sealStep(in, &st, &w) == se::SealAction::kRequestEngage && back == 0) back = t;
        }
        check(back != 0, "v14.23 camera: the camera back in the box re-seals with r1 still 1 (no edge needed)");
    }
    {
        // A request made for one pad is never admitted for another; the trigger asks again for the new pad.
        se::SealState st;
        se::ReleaseWhy w = se::ReleaseWhy::kNone;
        se::SealInputs a = inHangar(0);
        a.pad = 0x1000;
        se::sealStep(a, &st, &w);
        se::EngageRequest reqA{true, 0x1000, 0x5000};
        se::SealInputs b = inHangar(100);
        b.pad = 0x2000;
        const bool askedB = se::sealStep(b, &st, &w) == se::SealAction::kRequestEngage;
        se::engageTrigger(&reqA, st);
        const se::EngageRequest reqB{true, 0x2000, 0x5000};
        check(askedB && !se::engageAdmitted(reqA, st, 0x2000, 0x5000, true) && se::engageAdmitted(reqB, st, 0x2000, 0x5000, true),
              "v14.23 engage: a request bound to pad A is never admitted for pad B; the new request for B is");
    }
    {
        // The own pad is unknown (no r0 on this dock): no seal, and the waiting line says so.
        se::SealState st;
        se::SealInputs in = inHangar(0);
        in.ownKnown = false;
        check(firstEngage(&st, in, 0, 10000) == 0, "v14.23 unknown: no own pad from the game (no r0 this dock): no seal");
        se::GameView w;
        w.dockedKnown = true;
        w.docked = true;
        w.cameraKnown = true;
        w.cameraIn = true;
        w.cellBuilt = true;
        char line[480] = {};
        se::gameWaitingLine(w, line, sizeof(line));
        check(std::strstr(line, "game pad state unknown") != nullptr, "v14.23 unknown: the waiting line says game pad state unknown");
        se::GameView k = w;
        k.ownKnown = true;
        k.own = 0xABC40;
        k.r0 = 1;
        k.r1 = 1;
        k.r0Fresh = true;
        k.r1Fresh = true;
        se::gameWaitingLine(k, line, sizeof(line));
        check(std::strstr(line, "own pad @0abc40, r0=1 r1=1, in hangar=yes") != nullptr, "v14.23 waiting line: the own pad's key and codes, and in hangar yes");
    }
    {
        // The own pad: the object r0 was last called on this dock; a reset at undock or a hangar change; an r0 from before the reset names nothing.
        se::GameTrack tr;
        check(tr.observe(false, 0, 0) == se::GameTrackEvent::kNone && !tr.have, "v14.23 own: no r0 yet: no own pad");
        check(tr.observe(true, 0xA000, 100) == se::GameTrackEvent::kFirst && tr.own == 0xA000, "v14.23 own: the first r0 this dock names the own pad");
        check(tr.observe(true, 0xA000, 200) == se::GameTrackEvent::kNone, "v14.23 own: the same object again: no event");
        check(tr.observe(true, 0xB000, 300) == se::GameTrackEvent::kSwitched && tr.own == 0xB000,
              "v14.23 own: r0 on another object switches the own pad (one event)");
        tr.reset(400);
        check(!tr.have, "v14.23 own: undock or a hangar change resets the own pad");
        check(tr.observe(true, 0xB000, 350) == se::GameTrackEvent::kNone && !tr.have,
              "v14.23 own: an r0 call from before the reset names no own pad");
        check(tr.observe(true, 0xB000, 450) == se::GameTrackEvent::kFirst, "v14.23 own: an r0 call after the reset names the own pad");
    }
    {
        // The sun after a lock: r0 1 is off; the pad in the bay (r1 1) holds it; out and raised turns it on; undock turns it on.
        se::SunState sun;
        se::SunInputs si;
        si.setting = true;
        si.docked = true;
        si.ownKnown = true;
        si.r0Fresh = true;
        si.r0 = 1;
        si.r1Fresh = true;
        si.inHangar = true;
        si.relOk = true;
        si.rel = -45.0f;
        const char* why = "";
        check(se::sunStep(si, &sun, &why) && sun.off, "v14.23 sun: r0 1 turns the skip on");
        si.r0 = 0;
        check(!se::sunStep(si, &sun, &why) && sun.off, "v14.23 sun: the pad still in the bay (r1 1) holds the skip after r0 stops");
        si.inHangar = false;
        si.rel = 5.0f;
        check(se::sunStep(si, &sun, &why) && !sun.off && std::strcmp(why, "pad raised (launch)") == 0,
              "v14.23 sun: the pad out of the bay (r1 0, r0 not 1 or 3) and raised turns the sun back on");
        si.r0 = 1;
        si.inHangar = false;
        si.rel = -45.0f;
        check(se::sunStep(si, &sun, &why) && sun.off, "v14.23 sun: moving again turns it off");
        si.docked = false;
        check(se::sunStep(si, &sun, &why) && !sun.off && std::strcmp(why, "undocked") == 0, "v14.23 sun: undock turns it on");
    }
    // The pure helpers that are not about the seal trigger (kept): the latch's identity, the bounded probe, the rig's epoch rule, the view's centre.
    {
        se::LatchIdentity id;
        id.bind(0xB0, 0x5000);
        check(!id.step(false, 0, true, 0x5000), "v14.20 latch: a sample that misses the pad keeps the identity: no change");
        check(id.step(true, 0xA0, true, 0x5000) && id.pad == 0xA0, "v14.20 latch: pad B replaced by pad A is a genuine change: rebuild");
        se::LatchSet ls;
        ls.n = 2;
        ls.latched = true;
        ls.recs[0].movedRun = 2;
        ls.recs[1].movedRun = 3;
        ls.reset();
        check(ls.movedIndex() == -1 && ls.recs[1].movedRun == 0, "v14.20 latch: a reset clears the records' motion references");
        const uint8_t outside = static_cast<uint8_t>(se::RigVerdict::kOutside);
        check(se::rigMayVeto(3, 3, true, outside) && !se::rigMayVeto(3, 4, true, outside) && !se::rigMayVeto(4, 4, false, outside),
              "v14.20 rig: an outside bound from the current epoch may veto; an old epoch or an unbounded slot forwards");
        check(se::viewCentreReady(true) && !se::viewCentreReady(false),
              "v14.20 view: the origin is the centre; an eye pose alone is not");
        se::SealState s2;
        check(s2.reseal == 0 && !s2.requested, "v14.23: a fresh trigger holds no re-seal and requests nothing");
    }
    {
        // The bounded probe: at the bound, a full run is exhausted (no walk of the table); a stale slot is free.
        struct Slot {
            uintptr_t key = 0;
            uint32_t gen = 0;
        };
        constexpr size_t kCap = 64;
        Slot table[kCap] = {};
        uint32_t cur = 1;
        auto matchesKey = [&](size_t i, uintptr_t key) { return table[i].gen == cur && table[i].key == key; };
        auto staleSlot = [&](size_t i) { return table[i].gen != cur; };
        for (uintptr_t k = 1; k <= se::kTableProbeMax; ++k) {
            const se::ProbeResult p = se::boundedProbe(0, kCap, se::kTableProbeMax, [&](size_t i) { return matchesKey(i, k); }, staleSlot);
            if (p.empty) {
                table[p.index].key = k;
                table[p.index].gen = cur;
            }
        }
        const se::ProbeResult miss = se::boundedProbe(0, kCap, se::kTableProbeMax, [&](size_t i) { return matchesKey(i, 99); }, staleSlot);
        check(miss.exhausted && miss.probes == se::kTableProbeMax, "v14.20 table: at capacity a missing key is a miss after the bound (32 probes)");
        cur = 2;
        const se::ProbeResult fresh = se::boundedProbe(0, kCap, se::kTableProbeMax, [&](size_t i) { return matchesKey(i, 99); }, staleSlot);
        check(fresh.empty && fresh.probes == 1, "v14.20 table: a slot of an earlier epoch is free");
    }
    {
        // The store hook's install decision (issue 124 pilot): the PE header readable, the store's address (the constant on build 334925, the
        // signature match on an unknown build, 0 when none was found), the prologue read there, and the pin compare (game_build.h).
        const uint8_t pin[16] = {0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0xAC, 0x24, 0x68, 0xFE, 0xFF};
        check(std::memcmp(pin, se::kStorePrologue, 16) == 0, "v14.20 store hook: the pin is build 334925's sixteen bytes");
        check(se::storeInstallDecision(true, 0x4314C60u, pin, true) == se::StoreInstall::kInstall,
              "v14.20 store hook: build 334925's store with its pin installs the detour");
        check(se::storeInstallDecision(true, 0x4314D00u, pin, true) == se::StoreInstall::kInstall,
              "issue 124 store hook: an unknown build's store found by signature, its pin matching, installs the detour");
        check(se::storeInstallDecision(true, 0, pin, true) == se::StoreInstall::kNotFound,
              "issue 124 store hook: an unknown build whose store was not found by signature is refused");
        check(se::storeInstallDecision(false, 0x4314C60u, pin, true) == se::StoreInstall::kUnreadable &&
              se::storeInstallDecision(true, 0x4314C60u, nullptr, false) == se::StoreInstall::kUnreadable,
              "v14.20 store hook: an unreadable PE header or prologue is refused");
        check(se::storeInstallDecision(true, 0x4314C60u, pin, false) == se::StoreInstall::kWrongPrologue,
              "v14.20 store hook: a prologue that does not match the pin is refused");
    }
    {
        // The shell frame (kept): the pad 3 m along x and 4 m along y from the shell, 45 m below it; forward +y against +x: yaw +90 deg about up.
        const float shellO[3] = {0.0f, 0.0f, 0.0f};
        const float upZ[3] = {0.0f, 0.0f, 1.0f};
        const float shFwdX[3] = {1.0f, 0.0f, 0.0f};
        const float padP[3] = {3.0f, 4.0f, -45.0f};
        const float padFwdY[3] = {0.0f, 1.0f, 0.0f};
        const se::ShellFrame sf = se::shellFrameOf(padP, shellO, upZ, shFwdX, padFwdY);
        check(sf.ok && std::fabs(sf.z + 45.0f) < 1e-4f && std::fabs(sf.lateral - 5.0f) < 1e-4f && sf.yawOk && std::fabs(sf.yawDeg - 90.0f) < 1e-3f,
              "v14.17 shell frame: z is the rel reading, lateral is the distance from the shaft, and yaw is +90 deg");
    }

    // ---- Built v14: the own pad's near-tie and its stickiness (fix 1); the shell named once, never a gate, ramp or skirt (fix 2) ----------------
    {
        se::PadObs pa{};
        pa.addr = 0x1000;
        pa.centre[0] = -20.0f;
        pa.radius = 30.0f;
        pa.size = se::PadSize::kMedium;
        pa.up[2] = -1.0f;
        pa.upOk = true;
        std::snprintf(pa.name, sizeof(pa.name), "%s", "LandingPadMediumA");
        se::PadObs pb{};
        pb.addr = 0x2000;
        pb.centre[0] = 20.0f;
        pb.radius = 30.0f;
        pb.size = se::PadSize::kMedium;
        pb.up[2] = 1.0f;
        pb.upOk = true;
        std::snprintf(pb.name, sizeof(pb.name), "%s", "LandingPadMediumB");
        const float radii[3] = {80.0f, 120.0f, 145.0f};
        const float camMid[3] = {0.0f, 0.0f, 0.0f};    // 20 m from each: an exact tie
        const float camB[3] = {10.0f, 0.0f, 0.0f};     // B 10 m, A 30 m: not a tie
        const float camNearB[3] = {4.0f, 0.0f, 0.0f};  // B 16 m, A 24 m: B nearer by 8 m, not a tie
        se::PadObs ordAB[2] = {pa, pb};
        se::PadObs ordBA[2] = {pb, pa};
        se::OwnCell c1, c2, c3;
        se::pickOwnPad(ordAB, 2, camMid, radii, &c1);
        se::pickOwnPad(ordBA, 2, camMid, radii, &c2);
        check(c1.has && c1.pad == 0x1000 && c2.has && c2.pad == 0x1000, "fix 1: a tie within 5 m goes to the platform named A, whatever the record order");
        se::pickOwnPad(ordAB, 2, camB, radii, &c3);
        check(c3.has && c3.pad == 0x1000, "v14.8: the B platform is not picked while the A platform of its size is within 60 m (the nearer B loses)");

        se::OwnPadSticky st;
        se::OwnCell s;
        se::pickOwnPadSticky(ordAB, 2, camMid, radii, &st, &s);
        check(s.has && s.pad == 0x1000, "fix 1: the first pick, from the tie, is A");
        se::pickOwnPadSticky(ordBA, 2, camNearB, radii, &st, &s);
        check(s.has && s.pad == 0x1000, "fix 1: sticky: A stays the own pad while its record is seen, though B is now nearer");
        se::PadObs onlyB[1] = {pb};
        bool anyPick = false;
        for (int f = 1; f <= 29; ++f) {
            se::pickOwnPadSticky(onlyB, 1, camNearB, radii, &st, &s);
            if (s.has) anyPick = true;
        }
        check(!anyPick, "fix 1: sticky: A unseen for 29 frames: no other pad is picked yet");
        se::pickOwnPadSticky(onlyB, 1, camNearB, radii, &st, &s);
        check(s.has && s.pad == 0x2000, "fix 1: sticky: A unseen for kHeldLostFrames (30): B takes over");

        se::ExplicitObs ex[4] = {};
        ex[0].addr = 1;
        ex[0].centre[0] = 2.0f;
        std::snprintf(ex[0].name, sizeof(ex[0].name), "%s", "MediumHangerSkirt");
        ex[1].addr = 2;
        ex[1].centre[0] = 3.0f;
        std::snprintf(ex[1].name, sizeof(ex[1].name), "%s", "HangarGate_Ramp1_1");
        ex[2].addr = 3;
        ex[2].centre[0] = 40.0f;
        std::snprintf(ex[2].name, sizeof(ex[2].name), "%s", "HangerMedium");
        ex[3].addr = 4;
        ex[3].centre[0] = 60.0f;
        std::snprintf(ex[3].name, sizeof(ex[3].name), "%s", "HangerLarge");
        const float padC[3] = {0.0f, 0.0f, 0.0f};
        check(!se::shellNameUsable("MediumHangerSkirt") && !se::shellNameUsable("HangarGate_Ramp1_1") && se::shellNameUsable("HangerMedium"),
              "fix 2: a skirt, a gate and a ramp are never the shell; a hanger is");
        check(se::pickShell(ex, 4, padC, se::PadSize::kMedium, 150.0f) == 2, "fix 2: the medium pad's HangerMedium, though the skirt and the gate are nearer");
        check(se::pickShell(ex, 4, padC, se::PadSize::kSmall, 150.0f) == 2, "fix 2: a small pad with no HangerSmall takes the nearest other sized hanger");
        check(se::pickShell(ex, 2, padC, se::PadSize::kMedium, 150.0f) == 2, "fix 2: with only the skirt and the gate there is no shell (n)");
    }


    // ---- Built v14.8: the lift platform B, the cell validity, the ship part count, the still clock across a source change ----------------
    {
        se::PadObs pa{};
        pa.addr = 0x1000;
        pa.centre[0] = -20.0f;
        pa.radius = 30.0f;
        pa.size = se::PadSize::kMedium;
        std::snprintf(pa.name, sizeof(pa.name), "%s", "LandingPadMediumA");
        se::PadObs pb{};
        pb.addr = 0x2000;
        pb.centre[0] = 20.0f;
        pb.radius = 30.0f;
        pb.size = se::PadSize::kMedium;
        std::snprintf(pb.name, sizeof(pb.name), "%s", "LandingPadMediumB");
        const float radii[3] = {80.0f, 120.0f, 145.0f};
        const float camB[3] = {10.0f, 0.0f, 0.0f};   // B 10 m, A 30 m
        se::PadObs ordAB[2] = {pa, pb};
        se::OwnCell c;
        se::pickOwnPad(ordAB, 2, camB, radii, &c);
        check(c.has && c.pad == 0x1000, "v14.8: the nearer B platform loses to the A platform of its size within 60 m");
        se::PadObs farA = pa;
        farA.centre[0] = -80.0f;   // A 90 m from the camera
        se::PadObs ordFar[2] = {farA, pb};
        se::pickOwnPad(ordFar, 2, camB, radii, &c);
        check(c.has && c.pad == 0x2000, "v14.8: beyond 60 m the A platform does not count: B is picked");
        se::OwnPadSticky st;
        se::OwnCell s;
        se::PadObs onlyB[1] = {pb};
        se::pickOwnPadSticky(onlyB, 1, camB, radii, &st, &s);
        check(s.has && s.pad == 0x2000, "v14.8 sticky: B picked while no A is in the store");
        se::pickOwnPadSticky(ordAB, 2, camB, radii, &st, &s);
        check(s.has && s.pad == 0x1000, "v14.8 sticky: a held B gives way to the A platform within 60 m at once");

        se::OwnCell none;
        check(se::cellInvalidWhy(none) != nullptr, "v14.8 cell: no pad, no seal");
        se::OwnCell noTarget;
        noTarget.has = true;
        check(se::cellInvalidWhy(noTarget) != nullptr, "v14.8 cell: a pad with no shaft target does not seal");
        se::OwnCell zero = noTarget;
        zero.capsule = true;
        zero.box = true;   // the zero box the flight sealed with
        check(se::cellInvalidWhy(zero) != nullptr, "v14.8 cell: a zero box never seals (the flight's 0 x 0 x 0)");
        se::OwnCell ok = zero;
        ok.boxHalf[0] = ok.boxHalf[1] = ok.boxHalf[2] = 12.0f;
        check(se::cellInvalidWhy(ok) == nullptr, "v14.8 cell: a box with every half-extent at least 10 m is a valid cell");
        se::OwnCell thin = ok;
        thin.boxHalf[1] = 5.0f;
        check(se::cellInvalidWhy(thin) != nullptr, "v14.8 cell: a box under 10 m on any axis does not seal");
    }
    {
        // The ship's part count: a real ship has many records near the camera; a boarding volume or a GUI record does not.
        check(se::shipPrefixNamesShip("LakonMiner_", 10), "v14.8 ship: a prefix with ten records near the camera names a ship");
        check(!se::shipPrefixNamesShip("LakonMiner_", 9), "v14.8 ship: nine records are not enough");
        check(!se::shipPrefixNamesShip("ShipBoardingVolume_", 50), "v14.8 ship: a BoardingVolume prefix never names a ship");
        check(!se::shipPrefixNamesShip("GUI_", 50) && !se::shipPrefixNamesShip("GUI_Panel", 50), "v14.8 ship: a GUI_ prefix never names a ship");
        check(!se::shipPrefixNamesShip("", 50) && !se::shipPrefixNamesShip(nullptr, 50), "v14.8 ship: no prefix, no ship");
    }

    // ---- Built v14.10: the bay settles (the watch's motion, the continuous movers, the fallback, the frame shift) --------------------------------
    {
        // The watch's motion test: a frame shift of the record and the shell together is not motion; a rotation over 0.5 deg is; so is a 3 cm move.
        const float a0[3] = {1.0f, 0.0f, 0.0f};
        const float a2[3] = {0.0f, 0.0f, 1.0f};
        const float prev[3] = {5.0f, 6.0f, -45.0f};
        check(!se::poseMoved(prev, prev, a0, a0, a2, a2), "v14.10 watch: a record that does not move is not moved");
        const float v[3] = {123.0f, -40.0f, 900.0f};
        const float shell[3] = {0.0f, 0.0f, 0.0f};
        const float recC[3] = {5.0f, 6.0f, -45.0f};
        const float shellS[3] = {shell[0] + v[0], shell[1] + v[1], shell[2] + v[2]};
        const float recS[3] = {recC[0] + v[0], recC[1] + v[1], recC[2] + v[2]};
        const float rel0[3] = {recC[0] - shell[0], recC[1] - shell[1], recC[2] - shell[2]};
        const float rel1[3] = {recS[0] - shellS[0], recS[1] - shellS[1], recS[2] - shellS[2]};
        check(!se::poseMoved(rel0, rel1, a0, a0, a2, a2), "v14.10 watch: a frame shift of the record and the shell together is not motion");
        const float tilt1[3] = {std::cos(1.0f * 0.0174533f), std::sin(1.0f * 0.0174533f), 0.0f};
        const float tilt03[3] = {std::cos(0.3f * 0.0174533f), std::sin(0.3f * 0.0174533f), 0.0f};
        check(se::poseMoved(prev, prev, a0, tilt1, a2, a2), "v14.10 watch: a 1 deg rotation of an axis is motion");
        check(!se::poseMoved(prev, prev, a0, tilt03, a2, a2), "v14.10 watch: a 0.3 deg rotation is not motion");
        const float near3[3] = {prev[0] + 0.02f, prev[1], prev[2]};
        const float far3[3] = {prev[0] + 0.05f, prev[1], prev[2]};
        check(!se::poseMoved(prev, near3, a0, a0, a2, a2) && se::poseMoved(prev, far3, a0, a0, a2, a2),
              "v14.10 watch: a 2 cm centre move is not motion; a 5 cm move is");
    }

    // ---- Built v14.12: the latch (selection, cap, the by-address still test), the lift sequence, the held pad absence ----------------------
    {
        // Selection: the own pad (A, medium, at the origin); the other medium pad 40 m lateral; a small pad 10 m lateral (no); a medium pad 70 m lateral
        // (no); a Lift within the span (yes); a Lift beyond the span (no); a BlastShieldBase (yes); a HangarGate_Ramp (yes); a container (no); a guide
        // marker pad (no).
        se::LatchCand cands[12] = {};
        auto put = [](se::LatchCand& cd, uintptr_t addr, const char* nm, float x, float y, float z) {
            cd.addr = addr;
            std::snprintf(cd.name, sizeof(cd.name), "%s", nm);
            cd.centre[0] = x;
            cd.centre[1] = y;
            cd.centre[2] = z;
        };
        put(cands[0], 0x2, "LandingPadMediumB", 0.0f, 40.0f, -20.0f);
        put(cands[1], 0x3, "LandingPadSmallA", 10.0f, 0.0f, 0.0f);
        put(cands[2], 0x4, "LandingPadMediumB", 70.0f, 0.0f, 0.0f);
        put(cands[3], 0x5, "Lift_1", 20.0f, 0.0f, -30.0f);
        put(cands[4], 0x6, "Lift_2", 20.0f, 0.0f, 100.0f);
        put(cands[5], 0x7, "BlastShieldBase", 0.0f, 30.0f, -10.0f);
        put(cands[6], 0x8, "HangarGate_Ramp1_0", 50.0f, 0.0f, -5.0f);
        put(cands[7], 0x9, "Container_1", 5.0f, 0.0f, -5.0f);
        put(cands[8], 0xA, "LandingPadMediumGuideMarkers", 5.0f, 0.0f, 0.0f);
        const float origin[3] = {0.0f, 0.0f, 0.0f};
        const float up[3] = {0.0f, 0.0f, 1.0f};
        se::LatchRec out[se::kLatchCap];
        const size_t k = se::selectMechanism(cands, 9, 0x1, "LandingPadMediumA", se::PadSize::kMedium, origin, up, -90.0f, 40.0f, out);
        bool hasB = false, hasLift1 = false, hasShield = false, hasRamp = false, hasLift2 = false, hasSmall = false;
        for (size_t i = 0; i < k; ++i) {
            hasB = hasB || std::strcmp(out[i].name, "LandingPadMediumB") == 0 && out[i].addr == 0x2;
            hasLift1 = hasLift1 || std::strcmp(out[i].name, "Lift_1") == 0;
            hasShield = hasShield || std::strcmp(out[i].name, "BlastShieldBase") == 0;
            hasRamp = hasRamp || std::strcmp(out[i].name, "HangarGate_Ramp1_0") == 0;
            hasLift2 = hasLift2 || std::strcmp(out[i].name, "Lift_2") == 0;
            hasSmall = hasSmall || std::strcmp(out[i].name, "LandingPadSmallA") == 0;
        }
        check(k == 5 && out[0].addr == 0x1 && hasB && hasLift1 && hasShield && hasRamp && !hasLift2 && !hasSmall,
              "v14.12 latch: the own pad, the medium B 40 m lateral, Lift_1, BlastShieldBase, HangarGate_Ramp within the span; not the small pad, the far pad, the lift beyond the span, the container or the guide markers");
        se::LatchCand many[40] = {};
        for (int i = 0; i < 30; ++i) {
            char nm[48] = {};
            std::snprintf(nm, sizeof(nm), "Lift_%d", i);
            put(many[i], static_cast<uintptr_t>(0x100 + i), nm, 5.0f, 0.0f, 0.0f);
        }
        const size_t kc = se::selectMechanism(many, 30, 0x1, "LandingPadMediumA", se::PadSize::kMedium, origin, up, -90.0f, 40.0f, out);
        check(kc == se::kLatchCap, "v14.12 latch: at most kLatchCap (16) records: the own pad and fifteen others");
    }
    {
        // By address: a still record is not moved; 5 cm is; a missing frame re-bases without counting; a frame shift of the record and the shell
        // together is not motion (the relative centre is the same).
        const float a0[3] = {1.0f, 0.0f, 0.0f};
        const float a2[3] = {0.0f, 0.0f, 1.0f};
        const float r0[3] = {1.0f, 2.0f, -45.0f};
        const float r1[3] = {1.0f, 2.05f, -45.0f};
        se::LatchRec lr;
        lr.step(10, r0, a0, a2, 1, 1, 1000);
        check(!lr.step(11, r0, a0, a2, 1, 1, 1100) && lr.lastMovedMs == 0, "v14.12 by address: a still record is not moved");
        check(lr.step(12, r1, a0, a2, 1, 1, 1200) && lr.lastMovedMs == 1200, "v14.12 by address: a 5 cm move is a move");
        check(!lr.step(14, r0, a0, a2, 1, 1, 1400) && lr.lastMovedMs == 1200, "v14.12 by address: a missing frame re-bases without counting");
        const float v[3] = {123.0f, -40.0f, 900.0f};
        const float shell[3] = {0.0f, 0.0f, 0.0f};
        const float rec[3] = {5.0f, 6.0f, -45.0f};
        const float relA[3] = {rec[0] - shell[0], rec[1] - shell[1], rec[2] - shell[2]};
        const float relB[3] = {rec[0] + v[0] - (shell[0] + v[0]), rec[1] + v[1] - (shell[1] + v[1]), rec[2] + v[2] - (shell[2] + v[2])};
        se::LatchRec fs;
        fs.step(1, relA, a0, a2, 1, 1, 100);
        check(!fs.step(2, relB, a0, a2, 1, 1, 200) && fs.lastMovedMs == 0, "v14.12 by address: a frame shift of the record and the shell together is not motion");
    }

    // ---- Built v14.16: a release by motion waits the lowered pad and the stillness; the mechanism set (no TurboLift, doors or decals; the cap) ----
    {
        // The mechanism set: no TurboLift*, doors or decals; Lift and Lift_* yes, LiftDoor no; the cap of 16 with the refusals logged.
        check(se::latchClass("TurboLiftDoorLeft") < 0 && se::latchClass("TurboLift_Blocker") < 0 && se::latchClass("DecalB") < 0 &&
                  se::latchClass("LiftDoor") < 0 && se::latchClass("Lift") == 2 && se::latchClass("Lift_3") == 2 && se::latchClass("BlastShieldBase") == 2 &&
                  se::latchClass("HangarGate_Ramp1_0") == 3 && se::latchClass("LandingPadMediumB") == 1,
              "v14.16 set: TurboLift, doors and decals are never mechanism records; Lift and Lift_* are; the pads are the first class");
        se::LatchCand many[40] = {};
        for (int i = 0; i < 30; ++i) {
            char nm[48] = {};
            std::snprintf(nm, sizeof(nm), "Lift_%d", i);
            many[i].addr = static_cast<uintptr_t>(0x300 + i);
            std::snprintf(many[i].name, sizeof(many[i].name), "%s", nm);
            many[i].centre[0] = 5.0f;
        }
        const float origin[3] = {0.0f, 0.0f, 0.0f};
        const float up[3] = {0.0f, 0.0f, 1.0f};
        se::LatchRec out[se::kLatchCap];
        se::LatchReject rej[10];
        size_t rejN = 0;
        const size_t kc = se::selectMechanism(many, 30, 0x1, "LandingPadMediumA", se::PadSize::kMedium, origin, up, -85.0f, 300.0f, out, rej, 10, &rejN);
        check(kc == se::kLatchCap && rejN == 10 && std::strstr(rej[0].why, "cap") != nullptr,
              "v14.16 set: at the cap (16) the rest are refused with 'over the latch cap' (the first ten logged)");
        se::LatchCand door[2] = {};
        std::snprintf(door[0].name, sizeof(door[0].name), "%s", "TurboLiftDoorLeft");
        door[0].addr = 0x401;
        std::snprintf(door[1].name, sizeof(door[1].name), "%s", "DecalB");
        door[1].addr = 0x402;
        size_t rejN2 = 0;
        const size_t kd = se::selectMechanism(door, 2, 0x1, "LandingPadMediumA", se::PadSize::kMedium, origin, up, -85.0f, 300.0f, out, rej, 10, &rejN2);
        check(kd == 1 && rejN2 == 2, "v14.16 set: the elevator doors and decals are refused, the set is the own pad only");
    }

    // ---- Built v14.15: the load-in latch, the reject log, the bay's span, and the own pad's 3D move while sealed ----------------------------
    {
        // The span: a BlastShield 268 m along the shaft with no shaft target built (capLen 0) is kept; one 70 m lateral is refused with its reason.
        se::LatchCand cands[3] = {};
        std::snprintf(cands[0].name, sizeof(cands[0].name), "%s", "BlastShield_A");
        cands[0].addr = 0x21;
        cands[0].centre[0] = 41.0f;
        cands[0].centre[2] = 268.0f;
        std::snprintf(cands[1].name, sizeof(cands[1].name), "%s", "BlastShieldBase_B");
        cands[1].addr = 0x22;
        cands[1].centre[0] = 70.0f;
        cands[1].centre[2] = 100.0f;
        const float origin[3] = {0.0f, 0.0f, 0.0f};
        const float up[3] = {0.0f, 0.0f, 1.0f};
        se::LatchRec out[se::kLatchCap];
        se::LatchReject rej[10];
        size_t rejN = 0;
        const float span = std::max(0.0f + 10.0f, se::kLatchSpanM);
        const size_t k = se::selectMechanism(cands, 1, 0x1, "LandingPadMediumA", se::PadSize::kMedium, origin, up, -85.0f, span, out, rej, 10, &rejN);
        check(k == 2 && rejN == 0 && std::strcmp(out[1].name, "BlastShield_A") == 0,
              "v14.15: a BlastShield 41 m lateral and 268 m along (no target built yet) is latched");
        size_t rejN2 = 0;
        se::selectMechanism(cands + 1, 1, 0x1, "LandingPadMediumA", se::PadSize::kMedium, origin, up, -85.0f, span, out, rej, 10, &rejN2);
        check(rejN2 == 1 && std::strstr(rej[0].why, "lateral") != nullptr && std::strcmp(rej[0].name, "BlastShieldBase_B") == 0,
              "v14.15: a BlastShield 70 m lateral is refused, with its reason logged");
    }


    // ---- Built v14.13: the sun inside the hangar (its own state), the hangar watch, the no-hangar pad, the names --------------------------
    {
        // A docked pad with no sized shell: the note is due once; the give-up after kNoShellGiveUpFrames; a shell resets the count; undock starts over.
        using HE = se::HangarWatchEvent;
        se::HangarWatch hw;
        int notes = 0;
        for (uint32_t k = 0; k < se::kHangarNoteFrames + 20; ++k) notes += hw.step(false, true, false, false) == HE::kNote ? 1 : 0;
        check(notes == 1 && hw.logged && !hw.giveUp, "v14.13 hangar: the no-hangar note is due once per dock, at 60 frames");
        int giveUps = 0;
        for (uint32_t k = 0; k < se::kNoShellGiveUpFrames; ++k) giveUps += hw.step(false, true, false, false) == HE::kGiveUp ? 1 : 0;
        check(hw.giveUp && giveUps == 1, "v14.13 hangar: after 300 frames without a shell the gather of the shell parts stops (one give-up event)");
        se::HangarWatch found;
        for (uint32_t k = 0; k < 100; ++k) found.step(false, true, false, false);
        found.step(true, true, false, false);
        check(found.missing == 0 && !found.giveUp, "v14.13 hangar: a shell found resets the count (no give-up)");
        found.step(false, false, false, false);
        check(!found.logged && found.missing == 0, "v14.13 hangar: undock starts the dock over (the note is due again)");
    }
    {
        // Built 2026-10-23 (log 20261023_093833, MacLeod Market, large pad, flown in): the pad sat on the surface, HangerLarge out of reach, for the
        // 300 frames before the descent, and the search never resumed. The game putting the pad in the hangar (r1 0 to 1) starts it over.
        using HE = se::HangarWatchEvent;
        se::HangarWatch hw;
        for (uint32_t k = 0; k < se::kNoShellGiveUpFrames + 50; ++k) hw.step(false, true, false, false);
        check(hw.giveUp && hw.logged, "2026-10-23 hangar: a large pad on the surface gives up the search");
        const HE ev = hw.step(false, true, false, true);
        check(ev == HE::kRearm && !hw.giveUp && !hw.logged && hw.missing == 0, "2026-10-23 hangar: the pad entering the hangar re-arms the search");
        check(hw.step(false, true, false, true) == HE::kNone && hw.missing == 1, "2026-10-23 hangar: r1 held at 1 is no second re-arm");
        int notes = 0;
        for (uint32_t k = 0; k < se::kHangarNoteFrames; ++k) notes += hw.step(false, true, false, true) == HE::kNote ? 1 : 0;
        check(notes == 1, "2026-10-23 hangar: still no shell in the hangar: the names are noted again, from the lowered pad");
        se::HangarWatch spawn;   // a session that starts docked in the hangar (the 09:55 log): r1 is 1 from the first docked frame
        check(spawn.step(false, true, false, true) == HE::kNone && spawn.missing == 1, "2026-10-23 hangar: docked already in the hangar is no re-arm");
        se::HangarWatch ok;      // a medium pad whose shell is found on the surface: entering the hangar changes nothing
        for (uint32_t k = 0; k < 10; ++k) ok.step(true, true, false, false);
        check(ok.step(true, true, false, true) == HE::kNone && !ok.giveUp, "2026-10-23 hangar: a shell already found is no re-arm");
    }
    {
        // Names: the hangar-like filter (hangars, pads, lifts, doors and bays), not a plain structure.
        check(se::hangarLikeName("HangerSmall") && se::hangarLikeName("LandingPadSmallA") && se::hangarLikeName("Elevator_1") &&
                  se::hangarLikeName("BayDoor_2") && !se::hangarLikeName("Container_1"),
              "v14.13 names: hangar, pad, elevator and bay names are noted; a container is not");
        // A part with no size word is not the shell (outposts and carriers have none of the sized kind).
        se::ExplicitObs ex[1] = {};
        ex[0].addr = 1;
        std::snprintf(ex[0].name, sizeof(ex[0].name), "%s", "HangarXYZ");
        const float centre[3] = {0.0f, 0.0f, 0.0f};
        check(se::pickShell(ex, 1, centre, se::PadSize::kMedium, 150.0f) == 1,
              "v14.13 shell: a hangar part with no size word is not the reference");
    }


    // ---- Built v14.6: a ship change (the own ship\'s type prefix) resets the trigger; a flicker is not a change -----------------------------
    {
        se::ShipWatch w;
        check(!w.step("Anaconda"), "v14.6: the first reading sets the own ship's prefix: no change");
        bool any = false;
        for (int k = 0; k < 20; ++k) any = any || w.step("Anaconda");
        check(!any, "v14.6: the same prefix every tick is no change");
        any = false;
        for (int k = 0; k < 9; ++k) any = any || w.step("Cobra");
        check(!any, "v14.6: a different prefix for nine readings is not yet a ship change");
        check(!w.step("Anaconda") && std::strcmp(w.prefix, "Anaconda") == 0, "v14.6: a flicker back to the old prefix resets the count");
        any = false;
        for (int k = 0; k < 9; ++k) any = any || w.step("Asp");
        check(!any, "v14.6: nine readings of the new prefix: still not a change");
        check(w.step("Asp") && std::strcmp(w.prefix, "Asp") == 0, "v14.6: the tenth reading of the new prefix is the ship change (the prefix is now Asp)");
        check(!w.step(nullptr) && !w.step(""), "v14.6: a missing reading is no change");
    }


    // ---- Built v14.4: the flicker fix (the reference, the hysteresis, the sequence after a motion release, the safety net) ----------------------
    {
        // (1) A static pad, its reference read from one source. A later read of the same pad from another source is a different centre (a constant
        // offset in the two computations): it is not compared, and it never releases. The same source, unchanged, never releases.
        se::ShellMotion sm;
        const float up[3] = {0.0f, 0.0f, 1.0f};
        const float shell[3] = {0.0f, 0.0f, 0.0f};
        const float padAt[3] = {0.0f, 0.0f, 10.0f};
        const float padOff[3] = {0.0f, 0.0f, 7.6f};   // the other source reads 2.4 m lower: the observed -2.4 m constant
        check(!sm.sample(padAt, 1, shell, 2, up) && sm.captured, "v14.4: the reference is captured at the first sample");
        bool released = false;
        for (int k = 0; k < 200; ++k) released = released || sm.sample(padAt, 1, shell, 2, up);
        check(!released && sm.delta == 0.0f, "v14.4: a static pad read from the same source never releases (200 samples)");
        released = false;
        for (int k = 0; k < 200; ++k) released = released || sm.sample(padOff, 0, shell, 2, up);
        check(!released, "v14.4: a static pad read from another source (a constant -2.4 m computation difference) never releases");
    }
    {
        // (2) Hysteresis: a pad 3 m off its reference releases only on the tenth consecutive sample; a single sample never releases.
        se::ShellMotion sm;
        const float up[3] = {0.0f, 0.0f, 1.0f};
        const float shell[3] = {0.0f, 0.0f, 0.0f};
        const float padAt[3] = {0.0f, 0.0f, 10.0f};
        const float padMoved[3] = {0.0f, 0.0f, 13.0f};
        sm.sample(padAt, 1, shell, 2, up);
        bool early = false;
        for (int k = 0; k < 9; ++k) early = early || sm.sample(padMoved, 1, shell, 2, up);
        check(!early, "v14.4: 3 m off the reference for nine samples does not release");
        check(sm.sample(padMoved, 1, shell, 2, up), "v14.4: 3 m off the reference for ten consecutive samples releases");
        se::ShellMotion one;
        one.sample(padAt, 1, shell, 2, up);
        check(!one.sample(padMoved, 1, shell, 2, up) && !one.sample(padAt, 1, shell, 2, up), "v14.4: a single sample off the reference never releases");
    }
    {
        // (4) The safety net: more than six seals in a minute trips it; spaced seals never do; a reset (a hangar change or a setting toggle) clears it.
        se::SealRateGuard g;
        bool any = false;
        for (uint64_t i = 0; i < 6; ++i) any = any || g.note(i * 1000);
        check(!any && !g.tripped, "v14.4 safety net: six seals in a minute do not trip it");
        check(g.note(6000) && g.tripped, "v14.4 safety net: the seventh seal in a minute trips it");
        se::SealRateGuard h;
        bool spaced = false;
        for (uint64_t i = 0; i < 12; ++i) spaced = spaced || h.note(i * 70000);
        check(!spaced, "v14.4 safety net: seals spaced over a minute apart never trip it");
        g = se::SealRateGuard{};
        check(!g.tripped && !g.note(7000), "v14.4 safety net: a reset clears it");
    }

    // ---- The rules the seal relies on -------------------------------------------------------------------------------------------------------------
    check(se::kPadMoveReleaseM == 2.0f && se::kCameraLeaveM == 2.0f && se::kBayStillMs == 2000,
          "the constants: the pad-move release 2 m, the camera 2 m, the latch movers window 2 s");
    {
        float v = -1.0f;
        check(se::parseBelowM("", se::kBelowDefaultsM[0], &v) && v == 85.0f && se::kBelowDefaultsM[0] == 85.0f,
              "the small floor defaults to 85 m (the lift doors at -72 m)");
        check(se::belowForSize(se::PadSize::kMedium, se::kBelowDefaultsM, false, 0.0f) == 120.0f &&
                  se::belowForSize(se::PadSize::kLarge, se::kBelowDefaultsM, false, 0.0f) == 145.0f,
              "the medium floor is 120 m and the large 145 m (the measured doors plus margin)");
        check(se::parseBelowM("45", 85.0f, &v) && v == 45.0f && !se::parseBelowM("sixty", 85.0f, &v) && !se::parseBelowM("201", 85.0f, &v),
              "a floor parses as metres in [0, 200]; text and 201 are refused");
    }
    {
        const float rawOk[3] = {10.0f, 2.0f, -3.0f}, rawZero[3] = {0.0f, 0.0f, 0.0f};
        check(se::rawCentreUsable(rawOk, 5.0f) && !se::rawCentreUsable(rawZero, 5.0f) && !se::rawCentreUsable(rawOk, 0.0f),
              "an unreadable centre (the origin) is not usable: such a record is kept, never judged");
    }
    check(se::ownPadCandidate(50.0f, 60.0f) && se::ownPadCandidate(39.0f, 0.0f) && !se::ownPadCandidate(41.0f, 0.0f),
          "a pad is an own pad candidate when the camera is inside its sphere or its centre is within 40 m");
    check(!se::heldCellDrops(29, 29, se::kHeldLostFrames) && se::heldCellDrops(30, 0, se::kHeldLostFrames) &&
              se::heldCellDrops(0, 30, se::kHeldLostFrames),
          "the held cell drops at 30 consecutive frames of absence or of another own pad");
    {
        char stripped[48] = {};
        check(se::strippedPrefix("CobraMkV_", stripped, sizeof(stripped)) && std::strcmp(stripped, "CobraMkV") == 0 &&
                  !se::strippedPrefix("CobraMkV", stripped, sizeof(stripped)),
              "the own record's name is the prefix without its underscore");
        check(se::ownRecordMatch("CobraMkV", "CobraMkV") && !se::ownRecordMatch("CobraMkV_Door", "CobraMkV"),
              "the own record matches its exact name only");
        const float oc[3] = {0.0f, 0.0f, 0.0f}, in1[3] = {34.0f, 0.0f, 0.0f}, in2[3] = {36.0f, 0.0f, 0.0f};
        check(se::ownSphereKeeps(in1, oc, 30.0f, 5.0f) && !se::ownSphereKeeps(in2, oc, 30.0f, 5.0f),
              "a record within the own record's live sphere plus 5 m is kept; 36 m from a 30 m sphere is not");
    }
    {
        const float o[3] = {0.0f, 0.0f, 0.0f}, u[3] = {0.0f, 0.0f, 1.0f};
        const float p1[3] = {0.0f, 0.0f, 50.0f}, p2[3] = {61.0f, 0.0f, 50.0f}, p3[3] = {60.0f, 0.0f, 50.0f};
        const float p4[3] = {0.0f, 0.0f, -90.0f}, p5[3] = {0.0f, 0.0f, -80.0f};
        check(se::inBayVolume(p1, o, u, 100.0f, 85.0f) && !se::inBayVolume(p2, o, u, 100.0f, 85.0f) && se::inBayVolume(p3, o, u, 100.0f, 85.0f),
              "the bay: on the shaft is in; 61 m lateral is out; 60 m lateral is in");
        check(!se::inBayVolume(p4, o, u, 100.0f, 85.0f) && se::inBayVolume(p5, o, u, 100.0f, 85.0f),
              "the bay: 90 m below the pad is out of an 85 m floor; 80 m below is in");
        check(se::bayPartName("Hangar_Small") && se::bayPartName("TurboLiftDoor") && se::bayPartName("LandingPad_Large") &&
                  !se::bayPartName("CobraMkV_") && !se::bayPartName("SocialSpace"),
              "the bay keeps the hangar's part names (Hangar, LandingPad, lift doors, ...) and not a ship's or the concourse's");
    }

    // ---- The pad-local frame and the release by the shell
    {
        // Built v13.7 (a): the pad frame. A rotated pad at (10, 2, 3): a point taken to the pad's frame and back is itself; the cell placed back
        // after a frame shift moves by exactly the shift; a pad turned about up turns the box's directions with it, and a point at a fixed place on
        // the pad keeps that place on the pad.
        const float th = 0.5235988f;   // 30 degrees
        const float axesA[3][3] = {{std::cos(th), std::sin(th), 0.0f}, {-std::sin(th), std::cos(th), 0.0f}, {0.0f, 0.0f, 1.0f}};
        const float padC[3] = {10.0f, 2.0f, 3.0f};
        const float p0[3] = {14.0f, -5.0f, 20.0f};
        float loc0[3] = {}, back0[3] = {};
        se::pointToLocal(p0, padC, axesA, loc0);
        se::pointToWorld(loc0, padC, axesA, back0);
        check(std::fabs(back0[0] - p0[0]) < 1e-3f && std::fabs(back0[1] - p0[1]) < 1e-3f && std::fabs(back0[2] - p0[2]) < 1e-3f,
              "a point taken to the rotated pad's frame and back is the same point");
        check(std::fabs(loc0[2] - 17.0f) < 1e-3f, "a point 17 m above the pad's centre is 17 m along the pad's up axis");

        // A cell with a shaft, a box, frozen at engage; then the pad's live frame is shifted by T (the record frame moves).
        se::OwnCell cellW;
        cellW.has = true;
        cellW.centre[0] = padC[0]; cellW.centre[1] = padC[1]; cellW.centre[2] = padC[2];
        cellW.capA[0] = padC[0]; cellW.capA[1] = padC[1]; cellW.capA[2] = padC[2];
        cellW.capB[0] = 12.0f; cellW.capB[1] = 40.0f; cellW.capB[2] = 90.0f;
        cellW.capsule = true;
        cellW.box = true;
        cellW.boxCentre[0] = 9.0f; cellW.boxCentre[1] = 3.0f; cellW.boxCentre[2] = 60.0f;
        cellW.boxU[0] = std::cos(th); cellW.boxU[1] = std::sin(th);
        cellW.boxV[0] = -std::sin(th); cellW.boxV[1] = std::cos(th);
        cellW.boxW[2] = 1.0f;
        cellW.boxHalf[0] = 33.0f; cellW.boxHalf[1] = 14.0f; cellW.boxHalf[2] = 60.0f;
        se::OwnCell cellL;
        se::localizeCell(cellW, padC, axesA, &cellL);
        const float T[3] = {-5.18f, -2.99f, 34.0f};   // the v13.6 flight's shift
        const float padS[3] = {padC[0] + T[0], padC[1] + T[1], padC[2] + T[2]};
        se::OwnCell cellS = cellW;
        se::worldizeCell(cellL, padS, axesA, &cellS);
        check(std::fabs(cellS.centre[0] - padS[0]) < 1e-3f && std::fabs(cellS.centre[2] - padS[2]) < 1e-3f,
              "the pad-frame cell's centre follows the pad's live centre after a frame shift");
        check(std::fabs(cellS.capB[0] - (cellW.capB[0] + T[0])) < 1e-3f && std::fabs(cellS.capB[1] - (cellW.capB[1] + T[1])) < 1e-3f &&
                  std::fabs(cellS.capB[2] - (cellW.capB[2] + T[2])) < 1e-3f,
              "the shaft target moves with the frame shift, by the same offset as the pad");
        check(std::fabs(cellS.boxCentre[2] - (cellW.boxCentre[2] + T[2])) < 1e-3f && std::fabs(cellS.boxU[0] - cellW.boxU[0]) < 1e-5f &&
                  std::fabs(cellS.boxHalf[2] - 60.0f) < 1e-6f,
              "the box moves by the shift; its axes and half-extents do not change under a pure shift");

        // A pad turned 90 degrees about up: the box's length axis (world +x at 30 degrees, which is the pad's own axis 0) turns with the pad to
        // world +y; the shaft target keeps its height along the pad's up axis.
        const float axesR[3][3] = {{0.0f, 1.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}};
        se::OwnCell cellT = cellW;
        se::worldizeCell(cellL, padC, axesR, &cellT);
        check(std::fabs(cellT.boxU[0]) < 1e-3f && std::fabs(cellT.boxU[1] - 1.0f) < 1e-3f,
              "a pad turned 90 degrees turns the box's length axis with it (to world +y)");
        const float rotCap[3] = {cellT.capB[0] - padC[0], cellT.capB[1] - padC[1], cellT.capB[2] - padC[2]};
        check(std::fabs(rotCap[2] - cellL.capB[2]) < 1e-3f,
              "the shaft target keeps its height along the pad's up axis when the pad turns about up");

        // Built v13.7 (b): the release by the pad's own motion along the shaft, against the hangar shell. A shift of the whole frame (pad and shell
        // alike) changes nothing; the pad rising 6 m relative to the shell releases; rising 4 m or 5 m does not (the limit is more than 5 m).
        const float up[3] = {0.0f, 0.0f, 1.0f};
        const float shellE[3] = {0.0f, 0.0f, 50.0f}, padE[3] = {0.0f, 0.0f, 10.0f};
        const float relE = se::shellRelative(padE, shellE, up);
        const float shellS[3] = {T[0], T[1], 50.0f + T[2]}, padSh[3] = {T[0], T[1], 10.0f + T[2]};
        check(!se::padMovedAway(relE, se::shellRelative(padSh, shellS, up)),
              "a frame shift that moves the pad and the shell alike does not release the cell");
        const float padUp6[3] = {0.0f, 0.0f, 13.0f}, padUp4[3] = {0.0f, 0.0f, 11.5f}, padUp5[3] = {0.0f, 0.0f, 12.0f};
        check(se::padMovedAway(relE, se::shellRelative(padUp6, shellE, up)) && !se::padMovedAway(relE, se::shellRelative(padUp4, shellE, up)) &&
                  !se::padMovedAway(relE, se::shellRelative(padUp5, shellE, up)),
              "the pad rising 3 m against the shell releases the cell (the limit is 2 m); 1.5 m and exactly 2 m do not");
        check(!se::padMovedAway(NAN, 0.0f), "an unreadable reference never releases");
    }


    // ---- Restored (test-only, 2026-10-13): retained behaviour whose tests the v14.23 prune removed, rewritten against the current APIs --------------
    {
        // v14.9, the absent-pad hold: a pad absent for a few frames keeps the trigger's request; a longer absence clears it.
        se::SealState st;
        se::ReleaseWhy w = se::ReleaseWhy::kNone;
        check(se::sealStep(inHangar(0), &st, &w) == se::SealAction::kRequestEngage, "v14.9 hold: the setup request");
        se::SealInputs held = inHangar(100);
        held.pickOk = false;
        held.padHeld = true;
        check(se::sealStep(held, &st, &w) == se::SealAction::kNone && st.requested,
              "v14.9 hold: a pad absent for fewer than kHeldLostFrames keeps the request (no release, no new request)");
        held.nowMs = 200;
        check(se::sealStep(held, &st, &w) == se::SealAction::kNone && st.requested, "v14.9 hold: the same short absence again keeps it");
        se::SealInputs lost = inHangar(300);
        lost.pickOk = false;
        lost.padHeld = false;
        check(se::sealStep(lost, &st, &w) == se::SealAction::kNone && !st.requested, "v14.9 hold: a pad absent past the hold clears the request");
        check(!se::heldCellDrops(29, 0, se::kHeldLostFrames) && se::heldCellDrops(30, 0, se::kHeldLostFrames),
              "v14.9 hold: the held cell drops at kHeldLostFrames (30) absent frames, and not before");
    }
    {
        // The hangar change releases a held seal, with its own reason (the ship swap and the held pad gone both come here).
        se::SealState st;
        se::ReleaseWhy w = se::ReleaseWhy::kNone;
        se::SealInputs in = inHangar(0);
        se::sealStep(in, &st, &w);
        in.held = true;
        in.hangarChanged = true;
        in.nowMs = 100;
        check(se::sealStep(in, &st, &w) == se::SealAction::kRelease && w == se::ReleaseWhy::kHangarChanged,
              "hangar change: a held seal is released by the hangar change");
    }
    {
        // The pad moved against the shell (ShellMotion) releases a held seal; the next seal needs the r1 0 then 1 edge.
        se::SealState st;
        se::ReleaseWhy w = se::ReleaseWhy::kNone;
        se::SealInputs in = inHangar(0);
        se::sealStep(in, &st, &w);
        in.held = true;
        in.padMoved = true;
        in.nowMs = 100;
        check(se::sealStep(in, &st, &w) == se::SealAction::kRelease && w == se::ReleaseWhy::kPadMoved, "pad moved: a held seal is released by the pad moving");
        in.held = false;
        in.padMoved = false;
        uint64_t early = 0;
        for (uint64_t t = 200; t <= 2000; t += 100) {
            in.nowMs = t;
            if (se::sealStep(in, &st, &w) == se::SealAction::kRequestEngage && early == 0) early = t;
        }
        check(early == 0, "pad moved: with r1 still 1, the pad's move does not re-seal until the r1 edge (2 s)");
    }
    {
        // The mechanism latch (v14.14-16): a latched record that moves on two compared frames in a row is the launch; one move is not; a source change
        // re-bases without counting; the own pad is index 0 and counts the same way.
        se::LatchSet ls;
        ls.n = 1;
        ls.latched = true;
        const float r0[3] = {0.0f, 0.0f, 0.0f};
        const float r1[3] = {0.0f, 0.0f, 0.5f};
        const float r2[3] = {0.0f, 0.0f, 1.0f};
        const float a0[3] = {1.0f, 0.0f, 0.0f};
        const float a2[3] = {0.0f, 0.0f, 1.0f};
        ls.recs[0].step(1, r0, a0, a2, 1, 2, 100);
        ls.recs[0].step(2, r1, a0, a2, 1, 2, 200);
        check(ls.movedIndex() == -1, "latch: one compared move is not a release");
        ls.recs[0].step(3, r2, a0, a2, 1, 2, 300);
        check(ls.movedIndex() == 0, "latch: a record that moves on two compared frames in a row is the launch (the own pad is index 0)");
        ls.recs[0].step(4, r2, a0, a2, 1, 2, 400);
        check(ls.movedIndex() == -1, "latch: a still frame ends the run");
        ls.recs[0].step(5, r0, a0, a2, 3, 2, 500);   // another centre source: a re-base, never a move
        check(ls.movedIndex() == -1 && ls.recs[0].movedRun == 0, "latch: a change of centre source re-bases without counting as a move");
        // The identity (review 2): a shell replacement is a genuine change too.
        se::LatchIdentity id;
        id.bind(0xA0, 0x5000);
        check(id.step(true, 0xA0, true, 0x6000) && id.shell == 0x6000, "latch identity: a shell replacement is a change (rebuild the latch)");
    }
    {
        // The camera (v14.7): out for kCameraFrames frames releases the seal (the 10th frame); back inside it re-seals after kCameraFrames
        // frames inside the bay, with no in-hangar edge (r1 stayed 1).
        se::SealState st;
        se::ReleaseWhy w = se::ReleaseWhy::kNone;
        se::SealInputs in = inHangar(0);
        se::sealStep(in, &st, &w);
        in.held = true;
        in.cameraOutside = true;
        int outTicks = 0;
        for (int k = 1; k <= 12 && outTicks == 0; ++k) {
            in.nowMs = 100 * static_cast<uint64_t>(k);
            if (se::sealStep(in, &st, &w) == se::SealAction::kRelease && w == se::ReleaseWhy::kCameraLeft) outTicks = k;
        }
        check(outTicks == 10, "camera: out of the box for kCameraFrames (10) frames releases the seal, on the 10th frame");
        in.held = false;
        in.cameraOutside = false;
        in.cameraInBox = false;
        in.nowMs = 2000;
        se::sealStep(in, &st, &w);
        int inTicks = 0;
        for (int k = 1; k <= 12 && inTicks == 0; ++k) {
            in.cameraInBox = true;
            in.nowMs = 2000 + 100 * static_cast<uint64_t>(k);
            if (se::sealStep(in, &st, &w) == se::SealAction::kRequestEngage) inTicks = k;
        }
        check(inTicks == 10, "camera: back inside the bay the re-seal waits kCameraFrames (10) frames, and needs no in-hangar edge");
    }
    {
        // Review 1: a request whose pad or shell changed before its box was valid is never admitted to freeze a cell.
        se::SealState st;
        st.requested = true;
        st.camInRun = se::kCameraFrames;
        const se::EngageRequest r{true, 0x1000, 0x5000};
        check(!se::engageAdmitted(r, st, 0x1000, 0x5000, false), "review 1: an invalid box at the request does not freeze a cell");
        check(!se::engageAdmitted(r, st, 0x1000, 0x6000, true), "review 1: the shell replaced before the box was valid: no freeze");
        check(!se::engageAdmitted(r, st, 0x2000, 0x5000, true), "review 1: the pad replaced before the box was valid: no freeze");
        check(se::engageAdmitted(r, st, 0x1000, 0x5000, true), "review 1: control: the same pad and shell with a valid box freeze the cell");
    }
    {
        // Store mode (v14.1): the store observer does nothing with the setting off or undocked and unsealed; gathers while docked and unsealed; judges
        // while sealed. The promote is due when the frame stamp has moved on.
        check(se::storeMode(false, false, true) == se::StoreMode::kNone && se::storeMode(true, false, false) == se::StoreMode::kNone,
              "v14.1 store: setting off, or undocked and unsealed, the store observer does nothing");
        check(se::storeMode(true, false, true) == se::StoreMode::kGather, "v14.1 store: docked and unsealed, the observer gathers (no judgement)");
        check(se::storeMode(true, true, false) == se::StoreMode::kJudge && se::storeMode(true, true, true) == se::StoreMode::kJudge,
              "v14.1 store: sealed, the observer judges, docked or not");
        check(se::framePromoteDue(3, 4) && !se::framePromoteDue(4, 4),
              "v14.2 promote: the frame's cell is promoted once per frame stamp (due when the stamp moved on)");
    }
    {
        // v14.1: a pad that was not gathered (no pick) never requests the seal, however docked; the pad in the hangar with a pick does.
        se::SealState st;
        se::SealInputs noPick = inHangar(0);
        noPick.pickOk = false;
        check(firstEngage(&st, noPick, 0, 20000) == 0, "v14.1: no pad gathered (pickOk false): the seal is never requested");
    }

    // ---- Release blocker for v0.20.0 (2026-10-14): the seal is for the commander seated in the ship; on foot it never holds -------------------------
    {
        // Aboard, docked, the pad in the hangar: seals; the seat leaving (Disembark: on foot in the hangar) releases at once, with its own reason.
        se::SealState st;
        se::SealInputs in = inHangar(0);
        check(firstEngage(&st, in, 100, 1000) == 100, "on foot: aboard in the hangar seals");
        in.held = true;
        se::ReleaseWhy why = se::ReleaseWhy::kNone;
        check(se::sealStep(in, &st, &why) == se::SealAction::kNone && why == se::ReleaseWhy::kNone, "on foot: aboard and held keeps the seal");
        in.aboard = false;   // on foot in the hangar (Flags2 OnFoot | OnFootInHangar): the seat is gone
        check(se::sealStep(in, &st, &why) == se::SealAction::kRelease && why == se::ReleaseWhy::kOnFoot, "on foot: the seat leaving releases the seal, reason kOnFoot");
        // The release wins over the camera rule: the lift ride up (the camera still inside the tall box) keeps it released, and nothing re-seals on foot.
        in.held = false;
        st = se::SealState{};   // as after a release: no request in flight
        for (int i = 0; i < 400; ++i) {
            in.nowMs = 1000 + 100u * static_cast<uint64_t>(i);
            check(se::sealStep(in, &st, &why) == se::SealAction::kNone, "on foot: the lift ride and the concourse, the camera in the tall box, never re-seal");
        }
        // Back aboard (boarding again on the pad, the hangar unchanged): the seal returns on the normal conditions.
        in.aboard = true;
        check(firstEngage(&st, in, 200000, 202000) != 0, "on foot: boarding again seals again");
    }
    {
        // The lift door and the concourse: on foot at the concourse level the pad is no longer in the hangar for the game (r1 0) or the camera has left the box; neither matters,
        // and the seat alone keeps the seal off. A held seal released by the seat is not re-armed by the r1 edge either way.
        se::SealState st;
        se::SealInputs in = inHangar(0);
        in.held = true;
        in.aboard = false;
        in.cameraInBox = false;
        in.cameraOutside = true;
        se::ReleaseWhy why = se::ReleaseWhy::kNone;
        check(se::sealStep(in, &st, &why) == se::SealAction::kRelease && why == se::ReleaseWhy::kOnFoot, "on foot in the concourse: released for the seat before the camera rule is consulted");
        check(st.reseal == 0, "on foot: the release asks for no r1 edge, only the seat returning");
        // Undocked still reads as undocked, ahead of the seat: the existing reason is kept.
        se::SealState st2;
        se::SealInputs und = inHangar(0);
        und.held = true;
        und.docked = false;
        und.aboard = false;
        check(se::sealStep(und, &st2, &why) == se::SealAction::kRelease && why == se::ReleaseWhy::kUndocked, "an undock keeps its own release reason ahead of the seat");
    }
    {
        // Flags missing or stale: the commander is not known to be aboard, so nothing seals and a held seal drops. The station feeds aboard = seatedKnown && seated.
        se::SealState st;
        se::SealInputs in = inHangar(0);
        in.aboard = false;   // Status.json unreadable, a menu (Flags 0), or the watcher off
        check(firstEngage(&st, in, 0, 60000) == 0, "flags unknown: a docked station in the hangar never seals");
        in.held = true;
        se::ReleaseWhy why = se::ReleaseWhy::kNone;
        check(se::sealStep(in, &st, &why) == se::SealAction::kRelease && why == se::ReleaseWhy::kOnFoot, "flags unknown: a held seal is released");
        // the gate the station applies
        auto aboardOf = [](bool known, bool seated) { return known && seated; };
        check(!aboardOf(false, true) && !aboardOf(true, false) && aboardOf(true, true), "aboard needs the seat known AND seated (a stale 'seated' with the sample unknown is not aboard)");
        check(edvr::journalSeatedFromFlags(1u << 24, 0) && edvr::journalSeatedFromFlags(0, 1u << 1) && !edvr::journalSeatedFromFlags(0, se::kFlags2OnFoot | se::kFlags2OnFootInHangar) &&
                  !edvr::journalSeatedFromFlags(1u, se::kFlags2OnFoot | se::kFlags2OnFootSocialSpace),
              "the seat from the words: the ship's bit or a taxi seat is aboard; on foot in the hangar or in social space, docked bit alone, is not");
    }
    {
        char text[400];
        se::describeStatusFlags(text, sizeof(text), true, 0x00000001u, 0x00002001u);
        check(std::strstr(text, "Flags 0x00000001 Flags2 0x00002001") && std::strstr(text, "on foot 1, in hangar 1, social space 0") && std::strstr(text, "docked 1, in main ship 0"),
              "the on-foot release line names both words and the bits (docked, ship, on foot, in hangar, social space)");
        se::describeStatusFlags(text, sizeof(text), false, 0, 0);
        check(std::strcmp(text, "Status Flags unknown") == 0, "an unknown sample says so");
        se::describeStatusFlags(text, sizeof(text), true, 0x01000001u, 0x0u);
        check(std::strstr(text, "in main ship 1") && std::strstr(text, "on foot 0"), "aboard in the main ship reads as such");
    }

    // ---- Built v14.24 (2026-10-13, the review of the 013c6122 flight) -----------------------------------------------------------------------------
    {
        // Item 1: a change of rig A's association forgets A's bounds and verdict only; rig B keeps its bound and its verdict (no whole-epoch bump).
        bool aCur = true, aLast = true, bCur = true, bLast = true;
        uint8_t aV = 2, bV = 2;
        uint32_t aRun = 3, bRun = 3;
        se::rigForgetBounds(&aCur, &aLast, &aV, &aRun);
        check(!aCur && !aLast && aV == 0 && aRun == 0, "v14.24 rig: a change of rig A's collection forgets rig A's bounds and verdict");
        check(bCur && bLast && bV == 2 && bRun == 3 && se::rigMayVeto(4, 4, bCur, bV),
              "v14.24 rig: rig B keeps its bound and its outside verdict, and may still veto (no whole-epoch bump)");
        check(se::collAssociationIs(0x500, 0x500) && !se::collAssociationIs(0x600, 0x500) && !se::collAssociationIs(0, 0),
              "v14.24 rig: a table entry names its rig only while the rig still holds that collection");
    }
    {
        // Release review 2026-10-14 finding 4: a rig's collection changed, so the rig's first record's name goes with its bounds (the own-ship prefix exemption reads the name).
        struct RigModel {
            bool cur = false, last = false, named = false;
            uint8_t verdict = 0;
            uint32_t run = 0;
            char name[48] = {};
            uintptr_t coll = 0;
        };
        auto note = [](RigModel& r, const char* recordName) {   // what applyRigNote does for one record: a bound, and the name while the slot has none
            r.cur = true;
            se::rigRecordName(&r.named, r.name, sizeof(r.name), recordName);
        };
        auto assoc = [](RigModel& r, uintptr_t coll) {   // what applyRigAssoc does
            if (coll == r.coll) return;
            se::rigReassociate(&r.cur, &r.last, &r.verdict, &r.run, &r.named, r.name, sizeof(r.name));
            r.coll = coll;
        };
        auto exempt = [](const RigModel& r, const char* prefix) { return se::ownShipExempt(false, 0.0f, r.name, prefix); };
        // the name is recorded once, while the slot has none
        RigModel a;
        note(a, "Cobra_Hull_01");
        note(a, "Rock_02");
        check(a.named && std::strcmp(a.name, "Cobra_Hull_01") == 0 && exempt(a, "Cobra_"), "release review 2026-10-14 rig name: the first record names the rig, a later record does not rename it");
        assoc(a, 0x500);
        a.verdict = 2;
        a.last = true;
        // own-ship prefix -> not: the old name must not exempt the new object
        assoc(a, 0x600);
        check(!a.named && a.name[0] == '\0' && !a.cur && !a.last && a.verdict == 0 && a.run == 0, "release review 2026-10-14 rig name: a change of collection forgets the name with the bounds and the verdict");
        check(!exempt(a, "Cobra_"), "release review 2026-10-14 rig name: with no name yet the rig is not exempt by prefix");
        note(a, "Rock_02");
        check(a.named && std::strcmp(a.name, "Rock_02") == 0 && !exempt(a, "Cobra_"), "release review 2026-10-14 rig name: prefix to non-prefix, fresh bounds: the new object is classed by its own name (not exempt)");
        // not -> own-ship prefix: the new object must be protected by its own name
        RigModel b;
        note(b, "Rock_02");
        assoc(b, 0x500);
        note(b, "Rock_02");
        check(!exempt(b, "Cobra_"), "release review 2026-10-14 rig name: a non-prefix rig is not exempt");
        assoc(b, 0x700);
        note(b, "Cobra_Hull_01");
        check(exempt(b, "Cobra_") && std::strcmp(b.name, "Cobra_Hull_01") == 0, "release review 2026-10-14 rig name: non-prefix to prefix, fresh bounds: the new object is exempt by its own name");
        // the same collection again is not a change: the name stays
        assoc(b, 0x700);
        check(b.named && exempt(b, "Cobra_"), "release review 2026-10-14 rig name: the same collection again keeps the name");
        // a record with no readable name, and a zero-size name buffer, are handled
        RigModel c;
        note(c, nullptr);
        check(c.named && c.name[0] == '\0', "release review 2026-10-14 rig name: a null record name records an empty name");
        bool named = false;
        char tiny[1] = {'x'};
        check(!se::rigRecordName(&named, tiny, 0, "abc") && !named && tiny[0] == 'x', "release review 2026-10-14 rig name: a zero-size buffer records nothing");
    }
    {
        // Item 3: the cost summary's percentiles, and the change-only gate.
        double v[100];
        for (int i = 0; i < 100; ++i) v[i] = static_cast<double>(100 - i);   // 100 down to 1
        double p50 = 0.0, p95 = 0.0, mx = 0.0;
        se::samplePercentiles(v, 100, &p50, &p95, &mx);
        check(p50 == 50.0 && p95 == 95.0 && mx == 100.0, "v14.24 cost: p50, p95 and max of 1..100 are 50, 95 and 100");
        se::samplePercentiles(v, 0, &p50, &p95, &mx);
        check(p50 == 0.0 && p95 == 0.0 && mx == 0.0, "v14.24 cost: no samples read as zeros");
        se::ChangeGate g;
        check(g.changed(7) && !g.changed(7) && g.changed(8) && !g.changed(8), "v14.24 log: a change-only gate writes a key once per change");
        check(se::textKey("pad") == se::textKey("pad") && se::textKey("pad") != se::textKey("pad "),
              "v14.24 log: a text key is stable for the same text and differs for another");
        se::SealState st;
        check(!st.mechanism && st.poseRun == 0, "v14.24: a fresh trigger holds no mechanism release");
    }
    {
        // Item 2: the pose of a latched record against its pose at the seal.
        const float rel0[3] = {0.0f, 0.0f, 0.0f};
        const float relOff[3] = {0.0f, 0.0f, 0.3f};
        const float a0[3] = {1.0f, 0.0f, 0.0f};
        const float a2[3] = {0.0f, 0.0f, 1.0f};
        const float c2 = std::cos(2.0f * 3.14159265f / 180.0f), s2 = std::sin(2.0f * 3.14159265f / 180.0f);
        const float a0Turn[3] = {c2, s2, 0.0f};
        float dM = -1.0f, dDeg = -1.0f;
        se::latchPoseDelta(rel0, rel0, a0, a0, a2, a2, &dM, &dDeg);
        check(dM == 0.0f && dDeg == 0.0f && se::latchPoseWithin(dM, dDeg), "v14.24 pose: a record at its sealed pose is within tolerance");
        se::latchPoseDelta(relOff, rel0, a0, a0, a2, a2, &dM, &dDeg);
        check(std::fabs(dM - 0.3f) < 1e-5f && !se::latchPoseWithin(dM, dDeg), "v14.24 pose: a 0.3 m offset is outside the 0.25 m tolerance");
        se::latchPoseDelta(rel0, rel0, a0Turn, a0, a2, a2, &dM, &dDeg);
        check(std::fabs(dDeg - 2.0f) < 1e-3f && !se::latchPoseWithin(dM, dDeg), "v14.24 pose: a 2 degree turn is outside the 1 degree tolerance");
    }
    {
        // Item 2, the re-seal on the poses: a Lift that returns to its sealed pose re-seals after kPoseBackFrames frames with r1 1.
        se::SealState st;
        se::ReleaseWhy w = se::ReleaseWhy::kNone;
        se::SealInputs in = inHangar(0);
        check(se::sealStep(in, &st, &w) == se::SealAction::kRequestEngage, "v14.24 pose: the setup seal");
        in.held = true;
        in.mechanismMoved = true;
        in.poseBack = false;
        in.nowMs = 100;
        check(se::sealStep(in, &st, &w) == se::SealAction::kRelease && w == se::ReleaseWhy::kMechanism, "v14.24 pose: the Lift moving releases the seal");
        in.held = false;
        in.mechanismMoved = false;
        in.poseBack = true;
        int frames = 0;
        for (int k = 1; k <= 60 && frames == 0; ++k) {
            in.nowMs = 100 + 100 * static_cast<uint64_t>(k);
            if (se::sealStep(in, &st, &w) == se::SealAction::kRequestEngage) frames = k;
        }
        check(frames == static_cast<int>(se::kPoseBackFrames),
              "v14.24 pose: a Lift that returns to its sealed pose re-seals after 30 frames (r1 1, camera in)");
    }
    {
        // A Lift that stays away does not re-seal, however long r1 stays 1.
        se::SealState st;
        se::ReleaseWhy w = se::ReleaseWhy::kNone;
        se::SealInputs in = inHangar(0);
        se::sealStep(in, &st, &w);
        in.held = true;
        in.mechanismMoved = true;
        in.nowMs = 100;
        se::sealStep(in, &st, &w);
        in.held = false;
        in.mechanismMoved = false;
        in.poseBack = false;
        uint64_t early = 0;
        for (uint64_t t = 200; t <= 10000; t += 100) {
            in.nowMs = t;
            if (se::sealStep(in, &st, &w) == se::SealAction::kRequestEngage && early == 0) early = t;
        }
        check(early == 0, "v14.24 pose: a Lift that stays away does not re-seal (100 s with r1 1, the pose never back)");
    }
    {
        // r1 1 to 0 while away keeps the edge rule: the pose does not count while the pad is out; the next seal is the r1 0 to 1 edge.
        se::SealState st;
        se::ReleaseWhy w = se::ReleaseWhy::kNone;
        se::SealInputs in = inHangar(0);
        se::sealStep(in, &st, &w);
        in.held = true;
        in.mechanismMoved = true;
        in.nowMs = 100;
        se::sealStep(in, &st, &w);
        in.held = false;
        in.mechanismMoved = false;
        in.poseBack = false;
        in.nowMs = 200;
        check(se::sealStep(in, &st, &w) == se::SealAction::kNone, "v14.24 edge: with the pose away and r1 1, no re-seal");
        in.inHangar = false;
        in.poseBack = true;
        in.nowMs = 300;
        check(se::sealStep(in, &st, &w) == se::SealAction::kNone && st.reseal == 2 && !st.mechanism,
              "v14.24 edge: r1 1 to 0 moves the re-seal to the edge rule and drops the pose count");
        in.inHangar = true;
        in.poseBack = false;
        in.nowMs = 400;
        check(se::sealStep(in, &st, &w) == se::SealAction::kRequestEngage, "v14.24 edge: r1 back to 1 is the edge: it re-seals at once");
    }
    {
        // The pose rule belongs to the mechanism release only: a pad-moved release does not re-seal on poses.
        se::SealState st;
        se::ReleaseWhy w = se::ReleaseWhy::kNone;
        se::SealInputs in = inHangar(0);
        se::sealStep(in, &st, &w);
        in.held = true;
        in.padMoved = true;
        in.nowMs = 100;
        se::sealStep(in, &st, &w);
        in.held = false;
        in.padMoved = false;
        in.poseBack = true;
        uint64_t early = 0;
        for (uint64_t t = 200; t <= 4000; t += 100) {
            in.nowMs = t;
            if (se::sealStep(in, &st, &w) == se::SealAction::kRequestEngage && early == 0) early = t;
        }
        check(early == 0, "v14.24 pose: a pad-moved release does not re-seal on the poses (it waits for the r1 edge)");
    }
    {
        // Item 3: the waiting reason carries no per-frame counter: two frames of an absent pad give the same text.
        se::OwnPadSticky sticky;
        sticky.pad = 0x1000;
        const float camera[3] = {0.0f, 0.0f, 0.0f};
        const float radii[3] = {80.0f, 120.0f, 145.0f};
        se::OwnCell first, second;
        se::pickOwnPadSticky(nullptr, 0, camera, radii, &sticky, &first);
        se::pickOwnPadSticky(nullptr, 0, camera, radii, &sticky, &second);
        check(std::strcmp(first.reason, second.reason) == 0 && std::strstr(first.reason, "held for up to 30 frames") != nullptr,
              "v14.24 log: the held-pad reason is the same on every frame (no counter, no line per frame)");
    }

    // ---- Built v14.25 (2026-10-13): the lock-free pieces, driven from several threads ---------------------------------------------------------------
    {
        // The snapshot ring, one thread: the value pinned is the one published.
        struct Snap {
            uint64_t seq = 0;
            uint64_t a = 0;
            uint64_t b = 0;
            uint64_t check = 0;
        };
        se::SnapshotRing<Snap, 4> ring;
        Snap& w = ring.beginWrite();
        w.seq = 1;
        w.a = 3;
        w.b = 5;
        w.check = 9;
        ring.publish();
        size_t idx = 0;
        const Snap* p = ring.pin(&idx);
        check(p->seq == 1 && p->check == 9, "v14.25 ring: the value pinned is the value published");
        ring.unpin(idx);
    }
    {
        // Eight readers and one publisher. A pinned value is never torn (check = seq + a + b), a reader's sequence never goes back, and the last
        // publish is the one read at the end (no lost epoch).
        struct Snap {
            uint64_t seq = 0;
            uint64_t a = 0;
            uint64_t b = 0;
            uint64_t check = 0;
        };
        se::SnapshotRing<Snap, 4> ring;
        std::atomic<bool> stop{false};
        std::atomic<uint64_t> torn{0};
        std::atomic<uint64_t> backwards{0};
        std::atomic<uint64_t> reads{0};
        std::vector<std::thread> readers;
        for (int t = 0; t < 8; ++t) {
            readers.emplace_back([&] {
                uint64_t last = 0;
                while (!stop.load(std::memory_order_relaxed)) {
                    size_t idx2 = 0;
                    const Snap* q = ring.pin(&idx2);
                    if (q->check != q->seq + q->a + q->b) torn.fetch_add(1);
                    if (q->seq < last) backwards.fetch_add(1);
                    last = q->seq;
                    ring.unpin(idx2);
                    reads.fetch_add(1);
                }
            });
        }
        const uint64_t kPublishes = 20000;
        while (reads.load() == 0) std::this_thread::yield();   // the readers are running before the first publish
        for (uint64_t k = 1; k <= kPublishes; ++k) {
            Snap& wr = ring.beginWrite();
            wr.seq = k;
            wr.a = 3 * k;
            wr.b = 5 * k;
            wr.check = k + 3 * k + 5 * k;
            ring.publish();
        }
        stop.store(true);
        for (auto& th : readers) th.join();
        check(torn.load() == 0, "v14.25 ring, threads: no torn snapshot across 8 readers and 20000 publishes");
        check(backwards.load() == 0, "v14.25 ring, threads: no reader sees the epoch go backwards");
        size_t idx3 = 0;
        const Snap* last = ring.pin(&idx3);
        check(last->seq == kPublishes && ring.publishes() == kPublishes, "v14.25 ring, threads: the last publish is the one read (no lost epoch)");
        ring.unpin(idx3);
        check(reads.load() > 0, "v14.25 ring, threads: the readers did read");
    }
    {
        // The seqlock table: a single-threaded reference, then eight writers inserting the same records concurrently into a fresh table. Every
        // record must read back its exact payload in both; readers running during the writes never see a torn payload.
        constexpr size_t kCap = 1024;
        static se::SeqCell ref[kCap];
        static se::SeqCell par[kCap];
        const size_t kRecs = 160;
        auto homeOf = [&](uintptr_t rec) { return static_cast<size_t>((static_cast<uint64_t>(rec) >> 4) * 0x9E3779B97F4A7C15ull >> 54) & (kCap - 1); };
        auto valueOf = [](uintptr_t rec, float out[4]) {
            out[0] = static_cast<float>(rec % 997);
            out[1] = static_cast<float>(rec % 31);
            out[2] = static_cast<float>(rec % 17);
            out[3] = static_cast<float>(rec % 11) + 1.0f;
        };
        for (size_t k = 0; k < kRecs; ++k) {
            const uintptr_t rec = 0x7000 + 16 * static_cast<uintptr_t>(k);
            float v[4];
            valueOf(rec, v);
            se::seqTableInsert(ref, kCap, homeOf(rec), rec, 1, v, nullptr);
        }
        size_t refFound = 0;
        for (size_t k = 0; k < kRecs; ++k) {
            const uintptr_t rec = 0x7000 + 16 * static_cast<uintptr_t>(k);
            float got[4] = {};
            float want[4];
            valueOf(rec, want);
            if (se::seqTableFind(ref, kCap, homeOf(rec), rec, 1, got, nullptr) && std::memcmp(got, want, sizeof(want)) == 0) ++refFound;
        }
        check(refFound == kRecs, "v14.25 table, reference: every record reads back its payload (single thread)");
        std::atomic<uint64_t> badHits{0};
        std::vector<std::thread> writers;
        for (int t = 0; t < 8; ++t) {
            writers.emplace_back([&, t] {
                for (size_t k = 0; k < kRecs; ++k) {
                    const size_t j = (k * 7 + static_cast<size_t>(t) * 13) % kRecs;   // each thread inserts all records, in its own order
                    const uintptr_t rec = 0x7000 + 16 * static_cast<uintptr_t>(j);
                    float v[4];
                    valueOf(rec, v);
                    se::seqTableInsert(par, kCap, homeOf(rec), rec, 1, v, nullptr);
                }
            });
        }
        std::thread checker([&] {
            for (int round = 0; round < 200; ++round) {
                for (size_t k = 0; k < kRecs; ++k) {
                    const uintptr_t rec = 0x7000 + 16 * static_cast<uintptr_t>(k);
                    float got[4] = {};
                    float want[4];
                    valueOf(rec, want);
                    if (se::seqTableFind(par, kCap, homeOf(rec), rec, 1, got, nullptr) && std::memcmp(got, want, sizeof(want)) != 0) badHits.fetch_add(1);
                }
            }
        });
        for (auto& th : writers) th.join();
        checker.join();
        size_t parFound = 0;
        for (size_t k = 0; k < kRecs; ++k) {
            const uintptr_t rec = 0x7000 + 16 * static_cast<uintptr_t>(k);
            float got[4] = {};
            float want[4];
            valueOf(rec, want);
            if (se::seqTableFind(par, kCap, homeOf(rec), rec, 1, got, nullptr) && std::memcmp(got, want, sizeof(want)) == 0) ++parFound;
        }
        check(badHits.load() == 0, "v14.25 table, threads: a reader during concurrent inserts never sees a torn payload");
        check(parFound == refFound && parFound == kRecs,
              "v14.25 table, threads: eight writers inserting the same records give the same decisions as the single-thread reference");
        float got[4] = {};
        check(!se::seqTableFind(par, kCap, homeOf(0x7000), 0x7000, 2, got, nullptr), "v14.25 table: a record of another generation is not found");
    }
    // ---- Built v14.26 (2026-10-13): the gather buffers and the rig publication, from several threads --------------------------------------------
    {
        // The gather buffers, one thread: a full slot keeps its first entries and counts the rest; the entries come back in the order written, once.
        struct Item {
            uint32_t thread = 0;
            uint32_t seq = 0;
            uint32_t stamp = 0;
        };
        std::atomic<uint32_t> stamp{0};
        se::PassGather<Item, 2, 8> gb(stamp);
        {
            se::PassGather<Item, 2, 8>::Writer w(gb);
            w.call(0x1000);
            for (uint32_t k = 0; k < 10; ++k) {
                Item it;
                it.seq = k;
                w.push(it);
            }
        }
        stamp.fetch_add(1);
        std::vector<uint32_t> got;
        const se::PassDrain d = gb.drain([&](const Item& it) { got.push_back(it.seq); });
        check(got.size() == 8 && d.lost == 2, "v14.26 gather, one thread: a full slot keeps its first entries and counts the rest as lost");
        bool inOrder = true;
        for (size_t k = 0; k < got.size(); ++k) inOrder = inOrder && got[k] == k;
        check(inOrder, "v14.26 gather, one thread: the entries come back in the order written");
        check(d.complete && d.passes == 1 && d.calls == 1 && d.ctx == 0x1000, "v14.27 gather: the drain reports its pass, its callback count and the context");
        got.clear();
        stamp.fetch_add(1);
        const se::PassDrain again = gb.drain([&](const Item& it) { got.push_back(it.seq); });
        check(got.empty() && again.lost == 0 && again.calls == 0, "v14.26 gather, one thread: a drained entry is not delivered twice, and the loss is counted once");
    }
    {
        // Built 2026-10-23: two unfinished drains in a row put the tick more than the ring behind, and the drain after them skips the oldest passes. Their
        // entries are never read, so they are lost and must be counted, even when no writer comes back to that buffer to overwrite them: an uncounted loss
        // is an informative pass that silently misses records, and the station's fail-safe (a loss keeps every record) never fires.
        struct Item {
            uint32_t thread = 0;
            uint32_t seq = 0;
            uint32_t stamp = 0;
        };
        std::atomic<uint32_t> stamp{0};
        se::PassGather<Item, 2, 8> gb(stamp);
        stamp.fetch_add(1);   // pass 1
        {
            se::PassGather<Item, 2, 8>::Writer w(gb);
            w.call(0x3000);
            for (uint32_t k = 0; k < 5; ++k) {
                Item it;
                it.seq = k;
                w.push(it);
            }
        }
        stamp.fetch_add(4);   // the tick falls behind: passes 1..4 closed, none drained, and no writer returns
        uint64_t delivered = 0;
        uint64_t lost = gb.drain([&](const Item&) { ++delivered; }).lost;
        for (int k = 0; k < 4; ++k) {
            stamp.fetch_add(1);
            lost += gb.drain([&](const Item&) { ++delivered; }).lost;
        }
        check(delivered == 0 && lost == 5, "gather, skipped passes: the entries of a pass the tick skipped are counted lost though no writer overwrites them");
    }
    {
        // Eight writers and a draining tick: every entry is delivered once or counted as lost, and each writer's entries keep their order.
        struct Item {
            uint32_t thread = 0;
            uint32_t seq = 0;
            uint32_t stamp = 0;
        };
        std::atomic<uint32_t> stamp{0};
        auto gbHeap = std::make_unique<se::PassGather<Item, 16, 512>>(stamp);   // 300 KB: not on the stack
        se::PassGather<Item, 16, 512>& gb = *gbHeap;
        std::atomic<int> done{0};
        const uint32_t kPer = 4000;
        std::vector<std::thread> writers;
        for (uint32_t t = 0; t < 8; ++t) {
            writers.emplace_back([&, t] {
                for (uint32_t k = 0; k < kPer; ++k) {
                    Item it;
                    it.thread = t;
                    it.seq = k;
                    se::PassGather<Item, 16, 512>::Writer w(gb);   // one callback, one entry
                    w.call(0x2000 + t);
                    w.push(it);
                }
                done.fetch_add(1);
            });
        }
        std::vector<uint32_t> nextSeq(8, 0);
        uint64_t delivered = 0;
        uint64_t lostTotal = 0;
        bool ordered = true;
        auto visit = [&](const Item& it) {
            ++delivered;
            if (it.seq < nextSeq[it.thread]) ordered = false;
            nextSeq[it.thread] = it.seq + 1;
        };
        uint64_t incomplete = 0;
        while (done.load() < 8) {
            stamp.fetch_add(1);
            const se::PassDrain d = gb.drain(visit);
            lostTotal += d.lost;
            if (!d.complete) ++incomplete;
            std::this_thread::yield();
        }
        for (auto& th : writers) th.join();
        for (int k = 0; k < 4; ++k) {
            stamp.fetch_add(1);
            lostTotal += gb.drain(visit).lost;
        }
        (void)incomplete;
        check(delivered + lostTotal == 8u * kPer, "v14.26 gather, threads: every entry is delivered once or counted as lost");
        check(ordered, "v14.26 gather, threads: each writer's entries come back in the order written");
        check(delivered > 0, "v14.26 gather, threads: the tick received entries while the writers ran");
    }
    {
        // The rig publication, one thread: an outside rig is vetoed thirty calls in a row and let through once (the refresh), a mixed verdict resets the
        // run, and a rig the latest publication does not hold forwards.
        auto pubHeap = std::make_unique<se::RigPubTable>();   // 400 KB: not on the stack
        se::RigPubTable& pub = *pubHeap;
        pub.begin();
        const uintptr_t rig = 0x5000;
        const uintptr_t kColl = 0x123400000ull;
        size_t idx = 0;
        check(pub.insert(rig, 7, se::kRigVerdictOutside, kColl, &idx), "v14.26 rig publication: an outside rig is inserted");
        uint8_t v = 0;
        {
            uintptr_t cs = 0;
            size_t is = 0;
            check(!pub.lookup(rig, 7, &v, &cs, &is), "v14.27 rig publication: a staged rig is not visible until the publication is committed");
        }
        pub.commit();
        uintptr_t c = 0;
        size_t i2 = 0;
        check(pub.lookup(rig, 7, &v, &c, &i2) && v == se::kRigVerdictOutside && c == kColl, "v14.26 rig publication: the verdict and collection read back");
        int vetoes = 0;
        bool pattern = true;
        for (int call = 1; call <= 61; ++call) {
            const se::RigOutcome o = pub.decide(idx, se::kRigVerdictOutside, 1);
            if (call == 31) pattern = pattern && o == se::RigOutcome::kRefresh;
            else pattern = pattern && o == se::RigOutcome::kVetoed;
            if (o == se::RigOutcome::kVetoed) ++vetoes;
        }
        check(pattern && vetoes == 60, "v14.26 rig publication: an outside rig is vetoed thirty calls in a row, then let through once");
        const se::RigOutcome m = pub.decide(idx, se::kRigVerdictMixed, 1);
        const se::RigOutcome after = pub.decide(idx, se::kRigVerdictOutside, 1);
        check(m == se::RigOutcome::kMixed && after == se::RigOutcome::kVetoed, "v14.26 rig publication: a mixed verdict resets the run");
        pub.begin();
        pub.commit();
        check(!pub.lookup(rig, 7, &v, &c, &i2), "v14.26 rig publication: a rig the latest publication does not hold is not found");
    }
    {
        // Eight readers decide and look up while the tick republishes two verdicts, each with its own collection. No reader sees a verdict paired with
        // the other collection, and the last publication is the one read at the end.
        auto pubHeap = std::make_unique<se::RigPubTable>();   // 400 KB: not on the stack
        se::RigPubTable& pub = *pubHeap;
        const uintptr_t kRig = 0x9000;
        const uintptr_t kCollOut = 0xAAAA00000ull;
        const uintptr_t kCollUnk = 0xBBBB00000ull;
        pub.begin();
        size_t i0 = 0;
        pub.insert(kRig, 9, se::kRigVerdictOutside, kCollOut, &i0);
        pub.commit();
        std::atomic<bool> stop{false};
        std::atomic<uint64_t> torn{0};
        std::atomic<uint64_t> reads{0};
        std::atomic<uint64_t> gaps{0};
        std::vector<std::thread> readers;
        for (int r = 0; r < 8; ++r) {
            readers.emplace_back([&] {
                while (!stop.load(std::memory_order_relaxed)) {
                    uint8_t v = 0;
                    uintptr_t c = 0;
                    size_t idx = 0;
                    if (pub.lookup(kRig, 9, &v, &c, &idx)) {
                        const bool ok = (v == se::kRigVerdictOutside && c == kCollOut) || (v == se::kRigVerdictUnknown && c == kCollUnk);
                        if (!ok) torn.fetch_add(1);
                        (void)pub.decide(idx, v, 1);
                        reads.fetch_add(1);
                    } else {
                        gaps.fetch_add(1);   // the rig is in every publication: a miss is a window between two publications
                    }
                }
            });
        }
        while (reads.load() == 0) std::this_thread::yield();   // the readers run before the first republish (the same wait as the ring test)
        for (uint32_t k = 0; k < 20000; ++k) {
            pub.begin();
            size_t idx = 0;
            const bool outside = (k & 1u) == 0;
            pub.insert(kRig, 9, outside ? se::kRigVerdictOutside : se::kRigVerdictUnknown, outside ? kCollOut : kCollUnk, &idx);
            pub.commit();
        }
        stop.store(true);
        for (auto& th : readers) th.join();
        uint8_t v = 0;
        uintptr_t c = 0;
        size_t idx = 0;
        check(torn.load() == 0, "v14.26 rig publication, threads: no reader pairs a verdict with the other publication's collection");
        check(pub.lookup(kRig, 9, &v, &c, &idx) && v == se::kRigVerdictUnknown && c == kCollUnk,
              "v14.26 rig publication, threads: the last publication is the one read at the end");
        check(reads.load() > 0, "v14.26 rig publication, threads: the readers did read");
        (void)gaps;   // a reader preempted across two republications may miss once (it forwards); the property is tested deterministically below
    }
    {
        // Staging a publication leaves the committed one whole: every rig of the old publication is found until the commit, and the run of vetoes carries.
        auto pubHeap = std::make_unique<se::RigPubTable>();   // 400 KB: not on the stack
        se::RigPubTable& pub = *pubHeap;
        pub.begin();
        size_t idx[3] = {};
        for (uintptr_t k = 0; k < 3; ++k) pub.insert(0x100 + 16 * k, 3 + k, se::kRigVerdictOutside, 0xC000 + k, &idx[k], 7);
        pub.commit();
        for (int call = 0; call < 10; ++call) (void)pub.decide(idx[0], se::kRigVerdictOutside, 5);
        pub.begin();
        size_t ni = 0;
        pub.insert(0x100, 3, se::kRigVerdictOutside, 0xC000, &ni, 7, pub.runAt(idx[0]));   // staged, not committed
        bool allFound = true;
        for (uintptr_t k = 0; k < 3; ++k) {
            uint8_t v = 0;
            uintptr_t c = 0;
            size_t i = 0;
            uint32_t ep = 0;
            allFound = allFound && pub.lookup(0x100 + 16 * k, 3 + k, &v, &c, &i, &ep) && v == se::kRigVerdictOutside && c == 0xC000 + k && ep == 7;
        }
        check(allFound, "v14.27 rig publication: while the next publication is staged, every rig of the committed one is still found, with its epoch");
        pub.commit();
        int refresh = 0;
        int calls = 0;
        for (; calls < 21; ++calls) {
            if (pub.decide(ni, se::kRigVerdictOutside, 6) == se::RigOutcome::kRefresh) ++refresh;
        }
        check(refresh == 1, "v14.27 rig publication: the run of vetoes carries into the next publication (the refresh comes on the 31st call, not the 31st after each tick)");
        uint8_t v = 0;
        uintptr_t c = 0;
        size_t i = 0;
        check(!pub.lookup(0x110, 4, &v, &c, &i), "v14.27 rig publication: a rig left out of the new publication is gone after the commit");
    }

    // ---- Built v14.27 (2026-10-13): the pass rules. Callbacks of a frame land before, during and after its tick; an empty or unfinished pass is no evidence --
    {
        se::PassGate g;
        check(g.step(false, 0, 0) == se::PassVerdict::kDeferred, "v14.27 pass gate: a drain with a writer still inside is deferred");
        check(g.step(true, 0, 0) == se::PassVerdict::kEmpty, "v14.27 pass gate: no pass closed is empty");
        check(g.step(true, 1, 0) == se::PassVerdict::kEmpty, "v14.27 pass gate: a closed pass with no store callback is empty");
        check(g.step(true, 1, 1000) == se::PassVerdict::kInformative && g.run == 1, "v14.27 pass gate: a complete pass with callbacks is informative");
        check(g.step(true, 1, 400) == se::PassVerdict::kInformative && g.thin == 1, "v14.27 pass gate: a pass under half the usual calls is informative and counted thin");
        g.step(true, 0, 0);
        check(g.run == 0 && g.empty == 3 && g.deferred == 1 && g.informative == 2, "v14.27 pass gate: the window counts each verdict and an empty pass ends the run");
    }
    {
        struct P {
            uintptr_t addr = 0;
            int v = 0;
        };
        se::SeenWindow<P, 4> w;
        P a;
        a.addr = 0xA;
        a.v = 1;
        w.put(a);
        a.v = 2;
        w.put(a);
        check(w.n == 1 && w.items[0].v == 2, "v14.27 seen window: one entry per address, the newest reading");
        w.endPass();
        w.endPass();
        check(w.has(0xA), "v14.27 seen window: a record is kept through two passes that do not see it");
        w.endPass();
        check(!w.has(0xA), "v14.27 seen window: a record unseen for the whole window is gone");
        for (uintptr_t k = 1; k <= 6; ++k) {
            P p;
            p.addr = k;
            w.put(p);
        }
        check(w.n == 4, "v14.27 seen window: a full window replaces its oldest entry");
        se::CamHold ch;
        check(!ch.resolved(false) && ch.resolved(true) && ch.resolved(false), "v14.27 camera hold: a failure before any camera is unknown; one after a good one is held");
        bool held = true;
        for (uint32_t k = 1; k < se::kCamHoldTicks; ++k) held = held && ch.resolved(false);   // the first failure was counted above
        check(held && !ch.resolved(false), "v14.27 camera hold: the last camera is held for kCamHoldTicks failures, then it is unknown");
        se::StableGate sg;
        bool early = false;
        bool fired = false;
        sg.step(true, 3);
        for (int k = 1; k <= 5; ++k) {
            const bool f = sg.step(false, 3);
            if (k < 3) early = early || f;
            if (k == 3) fired = f;
        }
        check(!early && fired, "v14.27 stable gate: a change counts once, after it has held for the required ticks");
    }
    {
        // A writer still inside when the tick closes the pass: nothing is read, then both passes come back in order once it has left.
        struct E {
            uint32_t seq = 0;
            uint32_t stamp = 0;
        };
        std::atomic<uint32_t> stamp{1};
        se::PassGather<E, 4, 16> gb(stamp);
        std::vector<std::pair<uint32_t, uint32_t>> got;   // (stamp, seq)
        auto visit = [&](const E& e) { got.push_back({e.stamp, e.seq}); };
        auto w = std::make_unique<se::PassGather<E, 4, 16>::Writer>(gb);
        w->call(0x77);
        E e;
        e.seq = 1;
        w->push(e);
        stamp.fetch_add(1);
        const se::PassDrain open = gb.drain(visit, 2);
        check(!open.complete && got.empty() && open.passes == 0, "v14.27 gather: a pass with a writer still inside is not read");
        {
            se::PassGather<E, 4, 16>::Writer w2(gb);   // a callback of the next frame, on the same slot
            w2.call(0x77);
            E e2;
            e2.seq = 2;
            w2.push(e2);
        }
        w.reset();
        stamp.fetch_add(1);
        const se::PassDrain done = gb.drain(visit, 2);
        check(done.complete && done.passes == 2 && done.calls == 2 && got.size() == 2 && got[0].second == 1 && got[1].second == 2 && got[0].first == 1 && got[1].first == 2,
              "v14.27 gather: the unfinished pass is read with the next, oldest first, each entry naming its own pass");
        // A pass the tick never read is overwritten by the writer three stamps later and counted as lost, not misread as a newer pass.
        for (int k = 0; k < 3; ++k) {
            {
                se::PassGather<E, 4, 16>::Writer wl(gb);
                wl.call(0);
                E x;
                x.seq = 10 + k;
                wl.push(x);
            }
            stamp.fetch_add(1);
        }
        got.clear();
        {
            se::PassGather<E, 4, 16>::Writer wl(gb);
            wl.call(0);
        }
        const se::PassDrain lap = gb.drain(visit, 2);
        check(lap.lost >= 1 || got.size() <= 2, "v14.27 gather: a pass overwritten before it was read is counted lost");
        bool namesOwn = true;
        for (const auto& g : got) namesOwn = namesOwn && g.first == 3 + (g.second - 10);
        check(namesOwn, "v14.27 gather: no entry is delivered under a pass it was not written in");
    }
    {
        // Scripted: the own pad's callback lands on the far side of the tick every other frame (the pad is in pass N, then in no pass, then in two). The window
        // keeps the pad, the camera stays known, and a latch sample never spans an empty pass.
        struct E {
            uint32_t kind = 0;
            uint32_t stamp = 0;
        };
        std::atomic<uint32_t> stamp{1};
        se::PassGather<E, 4, 64> gb(stamp);
        struct P {
            uintptr_t addr = 0;
        };
        se::SeenWindow<P, 4> pads;
        se::PassGate gate;
        se::CamHold cam;
        int absent = 0;
        int oldRuleAbsent = 0;
        int camUnknown = 0;
        bool firstSeen = false;
        for (int f = 0; f < 200; ++f) {
            // frame f: 30 callbacks; the pad's callback is before the tick on even frames and after it on odd frames, so passes hold 2 or 0 pads
            const bool padAfter = (f & 1) != 0;
            auto cb = [&](bool pad) {
                se::PassGather<E, 4, 64>::Writer w(gb);
                w.call(0x55);
                E e;
                e.kind = pad ? 1 : 2;
                w.push(e);
            };
            for (int k = 0; k < 15; ++k) cb(false);
            if (!padAfter) cb(true);
            stamp.fetch_add(1);   // the tick of frame f
            bool padInPass = false;
            const se::PassDrain d = gb.drain([&](const E& e) {
                if (e.kind == 1) {
                    P p;
                    p.addr = 0xAD;
                    pads.put(p);
                    padInPass = true;
                }
            });
            const bool inf = gate.step(d.complete, d.passes, d.calls) == se::PassVerdict::kInformative;
            if (inf) {
                cam.resolved(d.ctx != 0);
                if (firstSeen && !pads.has(0xAD)) ++absent;
                if (firstSeen && !padInPass) ++oldRuleAbsent;
                if (padInPass) firstSeen = true;
                pads.endPass();
            }
            if (firstSeen && !cam.valid) ++camUnknown;
            if (padAfter) cb(true);
            for (int k = 0; k < 15; ++k) cb(false);
        }
        check(oldRuleAbsent > 50, "v14.27 scripted: the one-pass rule of v14.26 would have read the pad as absent on the passes the pad call missed");
        check(absent == 0, "v14.27 scripted: the pad is never absent when its call lands on the other side of the tick");
        check(camUnknown == 0, "v14.27 scripted: the camera is never unknown across those passes");
    }
    {
        // Scripted: the latch compares a record only between consecutive informative ticks. A pad that moves between two samples with an empty tick between
        // them is re-based, never counted as moving; the same motion on consecutive ticks is counted.
        se::LatchRec held;
        se::LatchRec live;
        const float ax0[3] = {1, 0, 0};
        const float ax2[3] = {0, 0, 1};
        uint32_t frame = 10;
        for (int k = 0; k < 8; ++k) {
            const float rel[3] = {5.0f * static_cast<float>(k), 0, 0};
            frame += 2;   // an empty tick between every two samples: the sample is skipped, so the stamps are two apart
            held.step(frame, rel, ax0, ax2, 1, 1, 1000);
        }
        check(held.movedRun == 0, "v14.27 latch: samples with an empty tick between them are re-based, never counted as motion");
        frame = 10;
        for (int k = 0; k < 8; ++k) {
            const float rel[3] = {5.0f * static_cast<float>(k), 0, 0};
            live.step(++frame, rel, ax0, ax2, 1, 1, 1000);
        }
        check(live.movedRun >= 2, "v14.27 latch: the same motion on consecutive informative ticks is counted");
    }
    {
        // Random interleavings: 1000 frames, callbacks spread over a burst, the tick landing anywhere in or around the burst, some callbacks long enough to
        // straddle a tick. A steady pad must give a continuous seal: no absence, no unknown camera, no motion release, and the rig publication must stay.
        struct E {
            uint32_t kind = 0;
            uint32_t stamp = 0;
        };
        using G = se::PassGather<E, 4, 128>;
        struct Ev {
            double t;
            int type;   // 0 begin, 1 end, 2 tick
            int id;
            bool pad;
        };
        int totalAbsent = 0, totalCamUnknown = 0, totalMotion = 0, totalSeal = 0, totalInformative = 0, totalPassNoPad = 0, totalRigGaps = 0, totalDeferred = 0;
        int totalOldRule = 0;
        for (uint32_t seed = 1; seed <= 6; ++seed) {
            std::mt19937 rng(seed);
            std::uniform_real_distribution<double> u01(0.0, 1.0);
            std::vector<Ev> evs;
            int id = 0;
            const int kFrames = 1000;
            for (int f = 0; f < kFrames; ++f) {
                const double base = 10.0 * f;
                const double start = base + 6.0 * u01(rng);
                const int n = 24;
                const int padAt = static_cast<int>(u01(rng) * n) % n;
                for (int k = 0; k < n; ++k) {
                    const double t = start + 4.0 * k / n;
                    const double dur = u01(rng) < 0.02 ? 3.0 : 0.01;   // some callbacks straddle a tick
                    evs.push_back({t, 0, id, k == padAt});
                    evs.push_back({t + dur, 1, id, false});
                    ++id;
                }
                evs.push_back({base + 10.0 + (u01(rng) * 2.0 - 1.0) * 6.0, 2, -1, false});   // the tick: anywhere around the burst's end and next start
            }
            std::stable_sort(evs.begin(), evs.end(), [](const Ev& a, const Ev& b) { return a.t != b.t ? a.t < b.t : a.type < b.type; });
            std::atomic<uint32_t> stamp{1};
            G gb(stamp);
            std::vector<std::unique_ptr<G::Writer>> open(static_cast<size_t>(id));
            struct P {
                uintptr_t addr = 0;
            };
            se::SeenWindow<P, 4> pads;
            se::PassGate gate;
            se::CamHold cam;
            se::LatchRec rec;
            auto pubHeap = std::make_unique<se::RigPubTable>();   // 400 KB: not on the stack
        se::RigPubTable& pub = *pubHeap;
            pub.begin();
            pub.insert(0x500, 11, se::kRigVerdictOutside, 0xC0, nullptr, 3);
            pub.commit();
            bool firstSeen = false;
            bool padInPass = false;
            uint32_t sealedFrames = 0;
            bool sealed = false;
            const float ax0[3] = {1, 0, 0};
            const float ax2[3] = {0, 0, 1};
            for (const Ev& ev : evs) {
                if (ev.type == 0) {
                    open[static_cast<size_t>(ev.id)] = std::make_unique<G::Writer>(gb);
                    G::Writer& w = *open[static_cast<size_t>(ev.id)];
                    w.call(0x99);
                    E e;
                    e.kind = ev.pad ? 1 : 2;
                    w.push(e);
                } else if (ev.type == 1) {
                    open[static_cast<size_t>(ev.id)].reset();
                } else {
                    stamp.fetch_add(1);
                    padInPass = false;
                    const se::PassDrain d = gb.drain(
                        [&](const E& e) {
                            if (e.kind == 1) {
                                P p;
                                p.addr = 0xAD;
                                pads.put(p);
                                padInPass = true;
                            }
                        },
                        1);
                    const se::PassVerdict v = gate.step(d.complete, d.passes, d.calls);
                    if (v == se::PassVerdict::kDeferred) ++totalDeferred;
                    if (v == se::PassVerdict::kInformative) {
                        ++totalInformative;
                        cam.resolved(d.ctx != 0);
                        if (firstSeen && !padInPass) ++totalOldRule;
                        if (firstSeen && !padInPass) ++totalPassNoPad;
                        if (firstSeen && !pads.has(0xAD)) ++totalAbsent;
                        if (padInPass) firstSeen = true;
                        pads.endPass();
                        const float rel[3] = {3.0f, 4.0f, -48.0f};   // a steady pad against its shell
                        rec.step(stamp.load(), rel, ax0, ax2, 1, 1, 1000);
                        if (rec.movedRun >= 2) ++totalMotion;
                        if (!sealed && gate.run >= 10) sealed = true;   // the camera-in-box rule: ten informative passes in a row (and it holds across holds below)
                        pub.begin();
                        pub.insert(0x500, 11, se::kRigVerdictOutside, 0xC0, nullptr, 3);
                        pub.commit();
                    }
                    if (firstSeen && !cam.valid) ++totalCamUnknown;
                    if (sealed) ++sealedFrames;
                    uint8_t vv = 0;
                    uintptr_t cc = 0;
                    size_t ii = 0;
                    if (firstSeen && !pub.lookup(0x500, 11, &vv, &cc, &ii)) ++totalRigGaps;
                }
            }
            totalSeal += static_cast<int>(sealedFrames);
        }
        check(totalInformative > 4000, "v14.27 random interleavings: most ticks read an informative pass");
        check(totalDeferred > 0, "v14.27 random interleavings: callbacks that straddle a tick did defer some passes (the cases are exercised)");
        check(totalOldRule > 200, "v14.27 random interleavings: the v14.26 one-pass rule would have lost the pad on many passes (the cases are exercised)");
        check(totalAbsent == 0, "v14.27 random interleavings: the own pad is never absent from the window, whatever the interleaving");
        check(totalCamUnknown == 0, "v14.27 random interleavings: the camera is never unknown");
        check(totalMotion == 0, "v14.27 random interleavings: a steady pad never reads as moving");
        check(totalRigGaps == 0, "v14.27 random interleavings: the rig publication stays across empty and unfinished passes");
        check(totalSeal > 0, "v14.27 random interleavings: the seal's pass condition is met");
    }
    // ---- Built v14.28: the latch measures in the hangar shell's frame; an origin rebase is not motion (flight 2d4ba6db: 50.85 m on every record) --------
    {
        struct Frame {   // the game's world frame: a rotation about z and a translation, applied to everything in it
            float c = 1.0f, s = 0.0f, t[3] = {0, 0, 0};
            void pos(const float p[3], float o[3]) const { o[0] = c * p[0] - s * p[1] + t[0]; o[1] = s * p[0] + c * p[1] + t[1]; o[2] = p[2] + t[2]; }
            void dir(const float d[3], float o[3]) const { o[0] = c * d[0] - s * d[1]; o[1] = s * d[0] + c * d[1]; o[2] = d[2]; }
        };
        struct Rec {
            float p[3];
            float a0[3];
            float a2[3];
        };
        // the shell with identity axes; a pad (index 0), a Lift, a gate ramp, in the shell's own frame
        const float shellP[3] = {100, 200, 0};
        const float sAx0[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
        const Rec base[3] = {{{110, 200, -48}, {1, 0, 0}, {0, 0, 1}}, {{112, 203, -20}, {1, 0, 0}, {0, 0, 1}}, {{95, 210, -10}, {0, 1, 0}, {0, 0, 1}}};
        // Reads the three records and the shell in a world frame, as latchRead does (the shell read in the same call), into three LatchRecs.
        auto sample = [&](const Frame& fr, const Rec in[3], uint32_t frame, se::LatchRec out[3]) {
            float sp[3];
            fr.pos(shellP, sp);
            float sAx[3][3];
            for (int k = 0; k < 3; ++k) fr.dir(sAx0[k], sAx[k]);
            for (int i = 0; i < 3; ++i) {
                float p[3], a0[3], a2[3], rel[3], l0[3], l2[3];
                fr.pos(in[i].p, p);
                fr.dir(in[i].a0, a0);
                fr.dir(in[i].a2, a2);
                se::shellLocalSample(p, a0, a2, sp, sAx, rel, l0, l2);
                out[i].step(frame, rel, l0, l2, 1, 1, 1000);
            }
        };
        auto movedAny = [](const se::LatchRec r[3]) { return r[0].movedRun >= 2 || r[1].movedRun >= 2 || r[2].movedRun >= 2; };
        {
            se::LatchRec lr[3];
            Frame fr;
            Rec cur[3] = {base[0], base[1], base[2]};
            bool moved = false;
            for (uint32_t f = 1; f <= 60; ++f) {
                if (f == 20) fr.t[0] = 50.85f;   // the origin rebase of the flight: one vector for everything
                if (f == 35) {                   // and a turn of the whole frame
                    fr.c = 0.0f;
                    fr.s = 1.0f;
                    fr.t[1] = -300.0f;
                }
                sample(fr, cur, f, lr);
                moved = moved || movedAny(lr);
            }
            check(!moved, "v14.28 latch: a common 50.85 m translation and a common 90 degree turn of the whole frame give no motion");
        }
        {
            // the v14.27 reading: the shell taken from before the shift (stale), the records live: the same rebase reads as motion
            se::LatchRec lr[3];
            Frame after;
            after.t[0] = 50.85f;
            bool moved = false;
            for (uint32_t f = 1; f <= 30; ++f) {
                const Frame before;
                const Frame& live = f >= 10 ? after : before;
                for (int i = 0; i < 3; ++i) {
                    float p[3], a0[3], a2[3];
                    live.pos(base[i].p, p);
                    live.dir(base[i].a0, a0);
                    live.dir(base[i].a2, a2);
                    const float rel[3] = {p[0] - shellP[0], p[1] - shellP[1], p[2] - shellP[2]};
                    lr[i].step(f, rel, a0, a2, 1, 1, 1000);
                }
                moved = moved || movedAny(lr);
            }
            check(moved || lr[0].haveMotion, "v14.28 latch: the world-space reading with a stale shell does read the rebase as motion (the bug is reproduced)");
        }
        {
            se::LatchRec lr[3];
            Frame fr;
            Rec cur[3] = {base[0], base[1], base[2]};
            bool moved = false;
            for (uint32_t f = 1; f <= 40; ++f) {
                if (f == 20) fr.t[0] = 50.85f;
                if (f >= 25 && f <= 30) cur[1].p[2] += 0.5f;   // the Lift travels 0.5 m a frame for six frames: 3 m against the shell
                sample(fr, cur, f, lr);
                if (lr[1].movedRun >= 2) moved = true;
            }
            check(moved, "v14.28 latch: a Lift that moves 3 m against the shell releases, with a rebase in the same window");
        }
        {
            se::LatchRec lr[3];
            Frame fr;
            Rec cur[3] = {base[0], base[1], base[2]};
            bool moved = false;
            for (uint32_t f = 1; f <= 40; ++f) {
                if (f >= 20) {   // the ramp turns 89 degrees in place and shifts 0.65 m over seven frames
                    const float u = f < 27 ? static_cast<float>(f - 19) / 7.0f : 1.0f;
                    const float ang = u * 89.0f * 0.0174532925f;
                    cur[2].a0[0] = std::sin(ang);
                    cur[2].a0[1] = std::cos(ang);
                    cur[2].a0[2] = 0.0f;
                    cur[2].p[0] = base[2].p[0] + 0.65f * u;
                }
                sample(fr, cur, f, lr);
                if (lr[2].movedRun >= 2) moved = true;
            }
            check(moved, "v14.28 latch: a gate ramp that turns 89 degrees in place (0.65 m) still releases");
        }
        {
            // The pose-back rule across a rebase: the poses at the seal, the Lift moves away, the origin shifts and turns, the Lift comes back.
            se::LatchRec seal[3];
            Frame f0;
            Rec cur[3] = {base[0], base[1], base[2]};
            sample(f0, cur, 1, seal);
            Frame fr;
            cur[1].p[2] += 3.0f;   // away
            se::LatchRec lr[3];
            sample(fr, cur, 2, lr);
            float dM = 0.0f, dDeg = 0.0f;
            se::latchPoseDelta(lr[1].rel, seal[1].rel, lr[1].ax0, seal[1].ax0, lr[1].ax2, seal[1].ax2, &dM, &dDeg);
            check(!se::latchPoseWithin(dM, dDeg), "v14.28 pose-back: a Lift 3 m from its sealed pose is away");
            fr.t[0] = 50.85f;   // the rebase while it is away
            fr.c = std::cos(0.5f);
            fr.s = std::sin(0.5f);
            cur[1].p[2] = base[1].p[2];   // and it returns
            sample(fr, cur, 3, lr);
            bool allBack = true;
            for (int i = 0; i < 3; ++i) {
                se::latchPoseDelta(lr[i].rel, seal[i].rel, lr[i].ax0, seal[i].ax0, lr[i].ax2, seal[i].ax2, &dM, &dDeg);
                allBack = allBack && se::latchPoseWithin(dM, dDeg);
            }
            check(allBack, "v14.28 pose-back: a Lift that moved and returned is back at its sealed pose though the origin shifted and turned between");
        }
        {
            const float sd[3] = {50.85f, 0, 0};
            const float same[3][3] = {{50.85f, 0, 0}, {50.9f, 0.05f, 0}, {50.8f, 0, 0.04f}};
            check(se::commonFrameShift(same, 3, sd, 1.0f, 0.1f), "v14.28 backstop: the shell and every record moving by one vector is a rebase");
            const float lift[3][3] = {{50.85f, 0, 0}, {50.85f, 0, 2.0f}, {50.85f, 0, 0}};
            check(!se::commonFrameShift(lift, 3, sd, 1.0f, 0.1f), "v14.28 backstop: one record moving differently is not a rebase");
            const float still[3] = {0.02f, 0, 0};
            const float tiny[3][3] = {{0.02f, 0, 0}, {0.02f, 0, 0}, {0.02f, 0, 0}};
            check(!se::commonFrameShift(tiny, 3, still, 1.0f, 0.1f) && !se::commonFrameShift(tiny, 0, sd, 1.0f, 0.1f),
                  "v14.28 backstop: a small common move, or no records, is not a rebase");
        }
    }

    // ---- Built v14.29: the node cache with flags, the sampled gathers, the per-thread pin ---------------------------------------------------------------
    {
        const uintptr_t node = 0x7ff612345678ull & ~uintptr_t(7);
        const uint32_t key = se::classKey(7);
        const uint64_t v = se::packSlot(node, se::NameClass::kExplicit, se::kNodeMechanism | se::kNodeShipPrefix, key);
        se::NameClass cls = se::NameClass::kNone;
        uint32_t flags = 0;
        check(se::unpackSlot(v, node, key, &cls, &flags) && cls == se::NameClass::kExplicit && flags == (se::kNodeMechanism | se::kNodeShipPrefix),
              "v14.29 node cache: a slot returns the node's class and flags under its key");
        check(!se::unpackSlot(v, node + 8, key, &cls, &flags), "v14.29 node cache: another node in the same slot is a miss (node reuse)");
        check(!se::unpackSlot(v, node, se::classKey(8), &cls, &flags), "v14.29 node cache: a lifecycle generation (an epoch bump) reads every name again");
        check(!se::unpackSlot(0, node, key, &cls, &flags), "v14.29 node cache: an empty slot is a miss");
        const uint64_t v2 = se::packSlot(node, se::NameClass::kOther, 0, se::classKey(8));
        check(se::unpackSlot(v2, node, se::classKey(8), &cls, &flags) && cls == se::NameClass::kOther && flags == 0,
              "v14.29 node cache: after a bump the node's new name is cached with its own flags");
        check(se::nodeFlagsOf("Lift_01") == (se::kNodeMechanism | se::kNodeHangarLike | se::kNodeShipPrefix), "v14.29 node flags: a Lift is a mechanism and a hangar name");
        check(se::nodeFlagsOf("CobraMkV_Hull") == se::kNodeShipPrefix, "v14.29 node flags: a ship part has the prefix flag only");
        check(se::nodeFlagsOf("Rock") == 0 && se::nodeFlagsOf("") == 0, "v14.29 node flags: a plain or empty name has none");
        check((se::nodeFlagsOf("HangarGate_Ramp1_0") & se::kNodeMechanism) != 0, "v14.29 node flags: a hangar gate is a mechanism record");
        // which records an unsealed pass reads the centre of
        using NC = se::NameClass;
        check(!se::nodeWantsCentre(NC::kOther, 0, false, false, false, false), "v14.29 gather: a plain record is not read on a sampling-free pass");
        check(se::nodeWantsCentre(NC::kPad, 0, false, false, false, false), "v14.29 gather: a pad is always read");
        check(se::nodeWantsCentre(NC::kExplicit, 0, false, false, false, false) && !se::nodeWantsCentre(NC::kExplicit, 0, true, false, false, false),
              "v14.29 gather: an explicit part is read until the hangar watch gives up");
        check(se::nodeWantsCentre(NC::kOther, se::kNodeMechanism, false, true, false, false) && !se::nodeWantsCentre(NC::kOther, se::kNodeMechanism, false, false, false, false),
              "v14.29 gather: a mechanism record is read only while a latch is pending");
        check(se::nodeWantsCentre(NC::kOther, se::kNodeHangarLike, false, false, true, false) && !se::nodeWantsCentre(NC::kOther, se::kNodeHangarLike, false, false, false, false),
              "v14.29 gather: a hangar-like record is read only while the names are wanted");
        check(se::nodeWantsCentre(NC::kOther, 0, false, false, false, true), "v14.29 gather: a full pass reads every record");
        check(se::fullGatherPass(8, false, false) && se::fullGatherPass(16, false, false) && !se::fullGatherPass(9, false, false) && se::fullGatherPass(9, true, false),
              "v14.29 gather: every eighth stamp is a full pass unsealed, and every stamp while an engage is pending");
    }
    {
        // The sampled gathers over many ticks: the pad is seen at every informative tick (it is read on every pass), the own candidate and the tally
        // arrive on the full passes only, and a ship swap is seen after ten samples (about eighty ticks unsealed, ten sealed). Most records are never read.
        struct Rec {
            se::NameClass cls;
            uint32_t flags;
            bool inShip;   // within the ship's tally radius and containing the camera for the candidate
            bool pad;
            bool cand;
        };
        struct E {
            uint32_t kind = 0;   // 1 pad, 2 candidate, 3 tally
            uint32_t stamp = 0;
            int ship = 0;
        };
        using NC = se::NameClass;
        auto run = [&](bool gatherAll, int shipAfter, int ticks, int* seen, uint64_t* centreReads, uint64_t* calls, int* padSeen, int* candArrivals) {
            std::atomic<uint32_t> stamp{1};
            auto gbHeap = std::make_unique<se::PassGather<E, 4, 2048>>(stamp);
            se::PassGather<E, 4, 2048>& gb = *gbHeap;
            se::ShipWatch watch;
            *seen = -1;
            *centreReads = *calls = 0;
            *padSeen = *candArrivals = 0;
            std::vector<Rec> recs;
            recs.push_back({NC::kPad, 0, false, true, false});
            for (int i = 0; i < 40; ++i) recs.push_back({NC::kOther, se::kNodeShipPrefix, true, false, i == 0});   // a ship's parts; the first holds the camera
            for (int i = 0; i < 200; ++i) recs.push_back({NC::kOther, 0, false, false, false});                   // scenery
            for (int t = 0; t < ticks; ++t) {
                const uint32_t st = stamp.load();
                const int ship = (shipAfter >= 0 && t >= shipAfter) ? 2 : 1;
                const bool full = se::fullGatherPass(st, gatherAll, false);
                {
                    se::PassGather<E, 4, 2048>::Writer w(gb);
                    for (const Rec& r : recs) {
                        w.call(0x1);
                        ++*calls;
                        if (!se::nodeWantsCentre(r.cls, r.flags, false, false, false, full)) continue;
                        ++*centreReads;
                        E e;
                        if (r.pad) {
                            e.kind = 1;
                            w.push(e);
                        }
                        if (full && r.cand) {
                            e.kind = 2;
                            e.ship = ship;
                            w.push(e);
                        }
                        if (full && r.inShip && (r.flags & se::kNodeShipPrefix) != 0) {
                            e.kind = 3;
                            e.ship = ship;
                            w.push(e);
                        }
                    }
                }
                stamp.fetch_add(1);
                int candShip = 0;
                int tally = 0;
                bool pad = false;
                gb.drain([&](const E& e) {
                    if (e.kind == 1) pad = true;
                    if (e.kind == 2) candShip = e.ship;
                    if (e.kind == 3) ++tally;
                });
                if (pad) ++*padSeen;
                if (candShip != 0) {
                    ++*candArrivals;
                    char pfx[32];
                    std::snprintf(pfx, sizeof(pfx), "Ship%d_", candShip);
                    if (se::shipPrefixNamesShip(pfx, static_cast<uint32_t>(tally)) && watch.step(pfx) && *seen < 0) *seen = t;
                }
            }
        };
        int seen = -1, padSeen = 0, cands = 0;
        uint64_t reads = 0, calls = 0;
        run(false, 100, 400, &seen, &reads, &calls, &padSeen, &cands);
        check(padSeen >= 395, "v14.29 sampled gathers: the pad is read on every pass, so it is seen at every tick");
        check(cands >= 400 / 8 - 2 && cands <= 400 / 8 + 2, "v14.29 sampled gathers: the own candidate arrives on the full passes only (one tick in eight)");
        check(seen > 100 && seen <= 100 + 10 * 8 + 16, "v14.29 sampled gathers: an unsealed ship swap is seen after ten samples, about eighty ticks");
        check(reads * 100 < calls * 20, "v14.29 sampled gathers: the unsealed gather reads the centre of under a fifth of the records");
        run(true, 100, 400, &seen, &reads, &calls, &padSeen, &cands);
        check(cands >= 399 && seen > 100 && seen <= 112, "v14.29 sampled gathers: sealed or with an engage pending, every pass samples and the swap is seen in ten ticks");
    }
    {
        // The per-thread pin: a slot a thread holds is not rewritten, whatever the publisher does; a later pin moves to the newest.
        struct V {
            uint64_t a = 0, b = 0, c = 0, d = 0;
        };
        auto ringHeap = std::make_unique<se::SnapshotRing<V, 8, 4>>();
        se::SnapshotRing<V, 8, 4>& ring = *ringHeap;
        auto publish = [&](uint64_t n) {
            V& v = ring.beginWrite();
            v.a = v.b = v.c = v.d = n;
            ring.publish();
        };
        publish(1);
        size_t counted = 7;
        const V* held = ring.pinThread(&counted);
        const uint64_t first = held->a;
        for (uint64_t n = 2; n < 200; ++n) publish(n);
        check(counted == 0 && held->a == first && held->b == first && held->d == first,
              "v14.29 per-thread pin: the slot a thread holds is never rewritten while the publisher goes round the ring");
        size_t c2 = 7;
        const V* newest = ring.pinThread(&c2);
        check(newest->a == 199, "v14.29 per-thread pin: the next pin on the thread moves to the newest slot");
        // threads: more readers than hazard cells (4), so some pin by the shared count; none sees a torn value, the publisher never stalls
        std::atomic<bool> stop{false};
        std::atomic<uint64_t> torn{0}, reads{0};
        std::vector<std::thread> readers;
        for (int r = 0; r < 10; ++r) {
            readers.emplace_back([&] {
                while (!stop.load(std::memory_order_relaxed)) {
                    size_t cnt = 0;
                    const V* p = ring.pinThread(&cnt);
                    const uint64_t a = p->a;
                    if (p->b != a || p->c != a || p->d != a) torn.fetch_add(1);
                    reads.fetch_add(1);
                    if (cnt != 0) ring.unpin(cnt - 1);
                }
            });
        }
        uint64_t n = 300;
        for (; n < 30300; ++n) publish(n);
        while (reads.load() < 1000) publish(n++);   // under load the readers may start late: keep publishing until they have read
        stop.store(true);
        for (auto& th : readers) th.join();
        check(torn.load() == 0 && reads.load() > 0, "v14.29 per-thread pin, threads: ten readers (four hazard cells, six counted pins) never see a torn slot");
    }

    // ---- Built v14.31: the node cache (a probed table, name beside the word), the sealed sampling, the sun shadow skip's gate --------------------------------
    {
        se::NodeCache<64> cache;
        const uintptr_t n1 = 0x7ff612345678ull & ~uintptr_t(7);
        se::NameClass cls = se::NameClass::kNone;
        uint32_t flags = 0;
        char name[64] = {};
        check(!cache.find(n1, 1, &cls, &flags, name), "v14.31 node cache: an empty cache misses");
        check(cache.put(n1, 1, se::NameClass::kExplicit, se::kNodeMechanism, "Lift_01") && cache.find(n1, 1, &cls, &flags, name) &&
                  cls == se::NameClass::kExplicit && flags == se::kNodeMechanism && std::strcmp(name, "Lift_01") == 0,
              "v14.31 node cache: a node comes back with its class, flags and name");
        check(!cache.find(n1 + 8, 1, &cls, &flags, name), "v14.31 node cache: another node is a miss");
        check(!cache.find(n1, 2, &cls, &flags, name), "v14.31 node cache: a lifecycle generation bump starts every node afresh");
        check(cache.put(n1, 2, se::NameClass::kOther, 0, "Rock") && cache.find(n1, 2, &cls, &flags, name) && std::strcmp(name, "Rock") == 0 && flags == 0,
              "v14.31 node cache: after a bump the node's new name is cached; the old generation's slot is reused");
        check(cache.find(n1, 2, &cls, &flags, nullptr), "v14.31 node cache: a lookup without a name works");
        // a long name is cut at 63 characters and terminated
        char longName[200];
        std::memset(longName, 'x', sizeof(longName));
        longName[199] = '\0';
        check(cache.put(n1 + 16, 2, se::NameClass::kOther, 0, longName) && cache.find(n1 + 16, 2, &cls, &flags, name) && std::strlen(name) == 63,
              "v14.31 node cache: a long name is cut to 63 characters");
        // the table is bounded: far more live nodes than slots, every put either lands or reports a full window, and no hit returns another node's name
        size_t landed = 0, refused = 0;
        for (uintptr_t k = 0; k < 400; ++k) {
            const uintptr_t node = 0x10000 + 8 * k;
            char nm[32];
            std::snprintf(nm, sizeof(nm), "N%llu", static_cast<unsigned long long>(k));
            if (cache.put(node, 5, se::NameClass::kOther, k & 7u, nm)) ++landed;
            else ++refused;
        }
        bool consistent = true;
        size_t hits = 0;
        for (uintptr_t k = 0; k < 400; ++k) {
            const uintptr_t node = 0x10000 + 8 * k;
            char nm[32];
            std::snprintf(nm, sizeof(nm), "N%llu", static_cast<unsigned long long>(k));
            if (cache.find(node, 5, &cls, &flags, name)) {
                ++hits;
                consistent = consistent && std::strcmp(name, nm) == 0 && flags == (k & 7u);
            }
        }
        check(landed <= 64 && refused >= 336 && hits == landed && consistent, "v14.31 node cache: the probe bound keeps the table bounded and a hit is always the node's own");
    }
    {
        // threads: writers and readers on a small cache; a hit must carry the name its node implies, never a torn one
        auto cacheHeap = std::make_unique<se::NodeCache<256>>();
        se::NodeCache<256>& cache = *cacheHeap;
        std::atomic<bool> stop{false};
        std::atomic<uint64_t> bad{0}, hits{0};
        auto nameOf = [](uintptr_t node, char* out) { std::snprintf(out, 64, "Node_%llu_abcdefghijklmnopqrstuvwxyz", static_cast<unsigned long long>(node >> 3)); };
        std::vector<std::thread> threads;
        for (int t = 0; t < 8; ++t) {
            threads.emplace_back([&, t] {
                uint64_t x = 88172645463325252ull + static_cast<uint64_t>(t);
                while (!stop.load(std::memory_order_relaxed)) {
                    x ^= x << 13;
                    x ^= x >> 7;
                    x ^= x << 17;
                    const uintptr_t node = 0x20000 + 8 * (x % 600);
                    char want[64];
                    nameOf(node, want);
                    se::NameClass cls = se::NameClass::kNone;
                    uint32_t flags = 0;
                    char got[64];
                    if (cache.find(node, 3, &cls, &flags, got)) {
                        hits.fetch_add(1);
                        if (std::strcmp(got, want) != 0) bad.fetch_add(1);
                    } else {
                        cache.put(node, 3, se::NameClass::kOther, 0, want);
                    }
                }
            });
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        stop.store(true);
        for (auto& th : threads) th.join();
        check(bad.load() == 0 && hits.load() > 0, "v14.31 node cache, threads: eight threads inserting and reading never see a torn or foreign name");
    }
    {
        // Release review 2026-10-14 finding 2: two writers rewrite ONE node's entry in place (the same node, class, flags and generation, so the published word is identical
        // every time) while readers copy its name. The name is a seqlock payload of atomic words; a hit must be exactly one of the two names, never a mix of both.
        auto cacheHeap = std::make_unique<se::NodeCache<64>>();
        se::NodeCache<64>& cache = *cacheHeap;
        const uintptr_t node = 0x30008;
        char nameA[64], nameB[64];
        std::memset(nameA, 'A', 63);
        std::memset(nameB, 'B', 63);
        nameA[63] = nameB[63] = '\0';
        check(cache.put(node, 3, se::NameClass::kOther, 0, nameA), "node cache seqlock: the entry is created");
        std::atomic<bool> stop{false};
        std::atomic<uint64_t> torn{0}, hits{0}, misses{0}, puts{0};
        std::vector<std::thread> threads;
        for (int t = 0; t < 2; ++t) {
            threads.emplace_back([&, t] {
                const char* mine = t == 0 ? nameA : nameB;
                while (!stop.load(std::memory_order_relaxed)) {
                    if (cache.put(node, 3, se::NameClass::kOther, 0, mine)) puts.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&] {
                while (!stop.load(std::memory_order_relaxed)) {
                    se::NameClass cls = se::NameClass::kNone;
                    uint32_t flags = 0;
                    char got[64];
                    std::memset(got, 0, sizeof(got));
                    if (cache.find(node, 3, &cls, &flags, got)) {
                        hits.fetch_add(1, std::memory_order_relaxed);
                        const char c = got[0];
                        bool whole = (c == 'A' || c == 'B') && got[63] == '\0';
                        for (int k = 0; whole && k < 63; ++k) whole = got[k] == c;
                        if (!whole) torn.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        misses.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        stop.store(true);
        for (auto& th : threads) th.join();
        check(torn.load() == 0 && hits.load() > 0 && puts.load() > 0, "node cache seqlock, threads: two writers rewriting one node and four readers never see a name that mixes the two");
        // after the writers stop the entry is whole and readable
        se::NameClass cls = se::NameClass::kNone;
        uint32_t flags = 0;
        char got[64];
        check(cache.find(node, 3, &cls, &flags, got) && (std::strcmp(got, nameA) == 0 || std::strcmp(got, nameB) == 0), "node cache seqlock: the entry is one whole name once the writers stop");
    }
    {
        // Built v14.32: a node freed and reallocated within a generation (a streamed asset) is caught by the name-text validator on the hit path.
        uint8_t padText[16] = {'L', 'a', 'n', 'd', 'i', 'n', 'g', 'P', 'a', 'd', 'S', 'm', 'a', 'l', 'l', 'A'};
        uint8_t liftText[16] = {'L', 'i', 'f', 't', '_', '0', '1', 0, 0, 0, 0, 0, 0, 0, 0, 0};
        const uint64_t vPad = se::nodeValidator(0x7f0000001000ull, padText);
        const uint64_t vPadSamePtrOtherText = se::nodeValidator(0x7f0000001000ull, liftText);
        const uint64_t vPadOtherPtr = se::nodeValidator(0x7f0000002000ull, padText);
        const uint8_t zero[16] = {};
        check(vPad != 0 && vPad != vPadSamePtrOtherText && vPad != vPadOtherPtr && se::nodeValidator(0, zero) != 0,
              "v14.32 node validator: it changes with the text pointer and with the first sixteen bytes, and is never zero");
        check(vPad == se::nodeValidator(0x7f0000001000ull, padText), "v14.32 node validator: the same pointer and bytes give the same validator");
        se::NodeCache<64> cache;
        const uintptr_t node = 0x7ff600001230ull & ~uintptr_t(7);
        se::NameClass cls = se::NameClass::kNone;
        uint32_t flags = 0;
        char name[64] = {};
        bool stale = false;
        check(cache.put(node, 4, se::NameClass::kPad, 0, "LandingPadSmallA", vPad), "v14.32 node reuse: a pad node is cached with its validator");
        check(cache.find(node, 4, &cls, &flags, name, vPad, &stale) && !stale && cls == se::NameClass::kPad && std::strcmp(name, "LandingPadSmallA") == 0,
              "v14.32 node reuse: the same name (the same validator) is a hit");
        stale = false;
        check(!cache.find(node, 4, &cls, &flags, name, vPadSamePtrOtherText, &stale) && stale,
              "v14.32 node reuse: the same node pointer, another name, the same generation is refused and reported stale");
        check(cache.put(node, 4, se::NameClass::kExplicit, se::kNodeMechanism, "Lift_01", vPadSamePtrOtherText), "v14.32 node reuse: the node is classified afresh and replaced in place");
        stale = false;
        check(cache.find(node, 4, &cls, &flags, name, vPadSamePtrOtherText, &stale) && !stale && cls == se::NameClass::kExplicit && flags == se::kNodeMechanism &&
                  std::strcmp(name, "Lift_01") == 0,
              "v14.32 node reuse: the new name is the hit");
        check(!cache.find(node, 4, &cls, &flags, name, vPad, &stale), "v14.32 node reuse: the old name no longer hits");
        check(cache.used(4) == 1, "v14.32 node reuse: the entry was replaced in place, not added beside the stale one");
        // an unreadable node (validator 0) stays a hit while it stays unreadable
        check(cache.put(node + 8, 4, se::NameClass::kUnnamed, 0, nullptr, 0) && cache.find(node + 8, 4, &cls, &flags, name, 0, &stale) && cls == se::NameClass::kUnnamed,
              "v14.32 node reuse: an unreadable node (validator 0) is a hit while it stays unreadable");
        check(!cache.find(node + 8, 4, &cls, &flags, name, vPad, &stale), "v14.32 node reuse: and a node that becomes readable is re-read");
    }
    {
        // The sealed sampling: the pass of stamp S is a full one every 4th stamp sealed, every 8th unsealed, every stamp with an engage pending.
        check(se::fullGatherPass(4, false, true) && se::fullGatherPass(8, false, true) && !se::fullGatherPass(5, false, true) && !se::fullGatherPass(6, false, true),
              "v14.31 sampled gathers: sealed, every fourth stamp is a full pass");
        check(se::fullGatherPass(8, false, false) && !se::fullGatherPass(4, false, false), "v14.31 sampled gathers: unsealed, every eighth");
        check(se::fullGatherPass(5, true, true) && se::fullGatherPass(5, true, false), "v14.31 sampled gathers: an engage pending gathers every pass");
        // ShipWatch over sampled readings: ten readings of a new prefix make the change, so about 40 ticks sealed and about 80 unsealed
        auto swap = [](bool sealed) {
            se::ShipWatch w;
            int seen = -1;
            for (uint32_t t = 0; t < 400; ++t) {
                if (!se::fullGatherPass(t, false, sealed)) continue;   // no reading on a pass that did not gather the candidate
                const char* pfx = t < 100 ? "Cobra_" : "Anaconda_";
                if (w.step(pfx) && seen < 0) seen = static_cast<int>(t);
            }
            return seen;
        };
        const int sealedSeen = swap(true);
        const int unsealedSeen = swap(false);
        check(sealedSeen >= 100 + 36 && sealedSeen <= 100 + 44, "v14.31 sampled gathers: a sealed ship swap is seen about forty ticks after it (ten readings, one in four)");
        check(unsealedSeen >= 100 + 72 && unsealedSeen <= 100 + 88, "v14.31 sampled gathers: an unsealed ship swap is seen about eighty ticks after it");
    }

    // ---- Built v14.33: the mechanism release arms after the bay has been still for 60 informative passes (flight 1718577b: a port whose gate ramps were
    // still closing when the game said r1 1) ---------------------------------------------------------------------------------------------------------
    {
        const float ax0[3] = {1, 0, 0};
        const float ax2[3] = {0, 0, 1};
        se::LatchRec rec;
        se::MechArm arm;
        se::SealState st;
        se::ReleaseWhy w = se::ReleaseWhy::kNone;
        se::sealStep(inHangar(0), &st, &w);   // the seal is held from here on
        float z = 0.0f;
        uint32_t frame = 100;
        int releasedDuring = 0;
        int armedAt = -1;
        auto tick = [&](float moveZ, int index, bool r1) {
            z += moveZ;
            const float rel[3] = {3.0f, 4.0f, z};
            rec.step(++frame, rel, ax0, ax2, 1, 1, 1000 + frame);
            const bool allStill = rec.stamp == frame && rec.movedRun == 0;
            if (arm.step(allStill) && armedAt < 0) armedAt = index;
            se::SealInputs in = inHangar(1000 + frame);
            in.held = true;
            in.inHangar = r1;
            in.mechanismMoved = arm.armed && rec.movedRun >= 2;
            return se::sealStep(in, &st, &w);
        };
        // the gates close for 2 s (180 passes) after r1 1: moving every pass; nothing releases and nothing arms
        for (int i = 0; i < 180; ++i) {
            if (tick(0.5f, i, true) == se::SealAction::kRelease) ++releasedDuring;
        }
        check(releasedDuring == 0 && !arm.armed, "v14.33 arm: gates still closing for 2 s after r1 1: no release, not armed");
        // then still: it arms on the 60th still pass
        for (int i = 0; i < 59; ++i) tick(0.0f, 180 + i, true);
        check(!arm.armed, "v14.33 arm: 59 still passes do not arm");
        tick(0.0f, 239, true);
        check(arm.armed && armedAt == 239, "v14.33 arm: the 60th still pass arms (about 1 s)");
        for (int i = 0; i < 100; ++i) {
            if (tick(0.0f, 240 + i, true) == se::SealAction::kRelease) ++releasedDuring;
        }
        check(releasedDuring == 0, "v14.33 arm: armed and still, nothing releases");
        // a launch after arming releases, on the second moving pass
        bool released = false;
        for (int i = 0; i < 4 && !released; ++i) released = tick(0.5f, 340 + i, true) == se::SealAction::kRelease && w == se::ReleaseWhy::kMechanism;
        check(released, "v14.33 arm: a launch after arming releases by the mechanism");
    }
    {
        // an r1 1 to 0 before the arm still releases: the arm gates only the mechanism
        const float ax0[3] = {1, 0, 0};
        const float ax2[3] = {0, 0, 1};
        se::LatchRec rec;
        se::MechArm arm;
        se::SealState st;
        se::ReleaseWhy w = se::ReleaseWhy::kNone;
        se::sealStep(inHangar(0), &st, &w);
        float z = 0.0f;
        for (uint32_t f = 1; f <= 20; ++f) {
            z += 0.5f;
            const float rel[3] = {3.0f, 4.0f, z};
            rec.step(f, rel, ax0, ax2, 1, 1, 1000 + f);
            arm.step(rec.stamp == f && rec.movedRun == 0);
        }
        se::SealInputs in = inHangar(2000);
        in.held = true;
        in.inHangar = false;
        in.mechanismMoved = arm.armed && rec.movedRun >= 2;
        check(!arm.armed && !in.mechanismMoved && se::sealStep(in, &st, &w) == se::SealAction::kRelease && w == se::ReleaseWhy::kInHangarLeft,
              "v14.33 arm: an r1 1 to 0 before the arm still releases");
        // a record not read this pass (or a hole) restarts the count; a reset starts it afresh
        se::MechArm a2;
        for (int i = 0; i < 40; ++i) a2.step(true);
        a2.step(false);
        for (int i = 0; i < 59; ++i) a2.step(true);
        check(!a2.armed, "v14.33 arm: a pass with a record read late or moving restarts the count");
        a2.step(true);
        check(a2.armed, "v14.33 arm: and sixty clean passes after it arm");
        a2.reset();
        check(!a2.armed && a2.run == 0, "v14.33 arm: a new seal resets the arm");
    }

    // ---- Built v14.34: the optimized station in the flat profile -----------------------------------------------------------------------------------------
    {
        check(edvr::flatProfileReadsKey("fix.optimized_stations"), "v14.34 flat: fix.optimized_stations is on the flat key list (unlisted, getString answers off and the station never starts)");
        check(!edvr::flatProfileReadsKey("fix.optimized_station_x") && !edvr::flatProfileReadsKey("fix.supercruise_dust") && !edvr::flatProfileReadsKey("fix.fss_mode_latch"),
              "v14.34 flat: only that key was added (a near name and other fixes stay off the list)");
        check(edvr::flatPageHasRow("fix", "optimized_stations"), "v14.34 flat: the F8 flat page has the Optimized station row");
        check(edvr::flatProfileReadsKey("fix.station_window_distance") && edvr::flatPageHasRow("fix", "station_window_distance"), "flat: the Window 3D distance row is on the F8 page and its key is on the flat key list");
        const edvr::FlatPageRow* refused = edvr::flatPageFirstRefused([](const char* k) { return edvr::flatProfileReadsKey(k); });
        check(refused == nullptr, "v14.34 flat: every row of the flat page, the new one included, passes the flat key list");
        // the flat camera source: no temporal-pass rows, so the position is the render context's view 0 origin
        edvr::view::Resolved flat;
        flat.originOk = true;
        flat.origin[0] = 120.0f;
        flat.origin[1] = -40.0f;
        flat.origin[2] = 7.5f;
        flat.poseOk = false;
        float c[3] = {};
        bool fromRows = true;
        check(edvr::view::sphereCentre(flat, c, &fromRows) && !fromRows && c[0] == 120.0f && c[1] == -40.0f && c[2] == 7.5f,
              "v14.34 flat camera: with no pose rows the camera is the view 0 origin");
        edvr::view::Resolved none;
        check(!edvr::view::sphereCentre(none, c), "v14.34 flat camera: with neither rows nor origin there is no camera (the station keeps its camera unknown)");
        edvr::view::Resolved both = flat;
        both.poseOk = true;
        both.pose.pos[0] = 1.0f;
        both.pose.pos[1] = 2.0f;
        both.pose.pos[2] = 3.0f;
        check(edvr::view::sphereCentre(both, c, &fromRows) && !fromRows && c[0] == 120.0f, "v14.34 flat camera: rows present as well, the origin stays the source (as in VR)");
        // the camera rule on that position: inside a small bay box, outside it
        se::OwnCell cell;
        cell.has = true;
        cell.box = true;
        cell.boxHalf[0] = 76.0f;
        cell.boxHalf[1] = 37.5f;
        cell.boxHalf[2] = 129.0f;
        for (int k = 0; k < 3; ++k) {
            cell.boxCentre[k] = flat.origin[k];
        }
        cell.boxU[0] = 1.0f;
        cell.boxV[1] = 1.0f;
        cell.boxW[2] = 1.0f;
        float x[3] = {};
        se::boxCoordsOf(cell, c, x);
        check(std::fabs(x[0]) <= cell.boxHalf[0] && std::fabs(x[1]) <= cell.boxHalf[1] && std::fabs(x[2]) <= cell.boxHalf[2],
              "v14.34 flat camera: the view 0 origin at the box centre is inside the box");
        const float away[3] = {flat.origin[0] + 300.0f, flat.origin[1], flat.origin[2]};
        se::boxCoordsOf(cell, away, x);
        check(!(std::fabs(x[0]) <= cell.boxHalf[0] + se::kCameraLeaveM), "v14.34 flat camera: 300 m along the box is outside it (the camera rule releases)");
    }

    // ---- Built 2026-10-15: the transition hold. An origin shift (a boarding or disembark) releases the seal or holds the re-seal; a Lift release holds its pose-back
    // re-seal. The hold gates the request only: the camera and pose runs go on, and the r1 edge re-seal is not held (the v14.24 edge check above) -----------------
    {
        // (a) a held seal, the origin shifts while Status still reads aboard: released for the transition, and the hold starts
        se::SealState st;
        se::ReleaseWhy w = se::ReleaseWhy::kNone;
        se::SealInputs in = inHangar(0);
        check(se::sealStep(in, &st, &w) == se::SealAction::kRequestEngage, "transition: the setup seal");
        in.held = true;
        in.originShifted = true;
        in.nowMs = 100;
        check(se::sealStep(in, &st, &w) == se::SealAction::kRelease && w == se::ReleaseWhy::kOriginShift,
              "transition (a): a held seal released by the origin shift while the seat still reads aboard");
        check(st.holdUntilMs == 100 + se::kTransitionHoldMs && !st.requested && st.reseal == 0 && !st.mechanism,
              "transition (a): the release starts the hold (now + kTransitionHoldMs) and asks for no edge");
    }
    {
        // (a2) the guard: a held seal whose own pad's record is missing from kPadMissingPasses passes in a row is released, held like an origin shift, no edge needed
        se::SealState st;
        se::ReleaseWhy w = se::ReleaseWhy::kNone;
        se::SealInputs in = inHangar(0);
        check(se::sealStep(in, &st, &w) == se::SealAction::kRequestEngage, "pad missing: the setup seal");
        in.held = true;
        in.nowMs = 100;
        check(se::sealStep(in, &st, &w) == se::SealAction::kNone, "pad missing: a held seal with the pad present stays");
        in.padMissing = true;
        in.nowMs = 200;
        check(se::sealStep(in, &st, &w) == se::SealAction::kRelease && w == se::ReleaseWhy::kPadMissing, "pad missing: a held seal is released with kPadMissing");
        check(st.holdUntilMs == 200 + se::kTransitionHoldMs && st.shiftHold && st.reseal == 0 && !st.requested && !st.mechanism,
              "pad missing: the release sets the hold like an origin shift, asks for no edge (reseal stays 0) and withdraws the request");
        in.held = false;
        in.padMissing = false;
        in.nowMs = 300;
        check(se::sealStep(in, &st, &w) == se::SealAction::kNone && st.holdBlocked, "pad missing: a request right after the release waits for the hold");
        check(firstEngage(&st, in, 400, 4000) == 2700, "pad missing: the seal comes back at the hold's end (2700 ms), with no fresh edge");
        // earlier reasons win: an origin shift on the same tick is the origin shift; the guard wins over a hangar change
        se::SealState st2;
        se::SealInputs in2 = inHangar(0);
        se::sealStep(in2, &st2, &w);
        in2.held = true;
        in2.padMissing = true;
        in2.originShifted = true;
        in2.nowMs = 100;
        check(se::sealStep(in2, &st2, &w) == se::SealAction::kRelease && w == se::ReleaseWhy::kOriginShift, "pad missing: an origin shift on the same tick is released as the origin shift");
        se::SealState st3;
        se::SealInputs in3 = inHangar(0);
        se::sealStep(in3, &st3, &w);
        in3.held = true;
        in3.padMissing = true;
        in3.hangarChanged = true;
        in3.nowMs = 100;
        check(se::sealStep(in3, &st3, &w) == se::SealAction::kRelease && w == se::ReleaseWhy::kPadMissing, "pad missing: tested before a hangar change");
        // not held: the flag does nothing
        se::SealState st4;
        se::SealInputs in4 = inHangar(0);
        in4.padMissing = true;
        check(se::sealStep(in4, &st4, &w) == se::SealAction::kRequestEngage && w == se::ReleaseWhy::kNone, "pad missing: the flag does nothing while not sealed");
    }
    {
        // padMissingRun: not held 0; an uninformative pass leaves the run; the pad in the pass 0; else one more
        check(se::padMissingRun(2, true, false, false) == 0, "padMissingRun: not held gives 0");
        check(se::padMissingRun(2, false, false, true) == 2 && se::padMissingRun(2, false, true, true) == 2, "padMissingRun: an uninformative pass leaves the run as it was");
        check(se::padMissingRun(2, true, true, true) == 0, "padMissingRun: the pad in an informative pass resets the run");
        check(se::padMissingRun(2, true, false, true) == 3 && se::padMissingRun(0, true, false, true) == 1, "padMissingRun: an informative pass without the pad adds one");
        uint32_t run = 0;
        bool released = false;
        for (int i = 0; i < 3; ++i) {
            run = se::padMissingRun(run, true, false, true);
            released = run >= se::kPadMissingPasses;
        }
        check(released && se::kPadMissingPasses == 3, "padMissingRun: three informative passes without the pad reach the guard's limit");
    }
    {
        // (b) a Lift release, then the poses come back within the hold: the pose count runs, the request waits for the hold's end
        se::SealState st;
        se::ReleaseWhy w = se::ReleaseWhy::kNone;
        se::SealInputs in = inHangar(0);
        se::sealStep(in, &st, &w);
        in.held = true;
        in.mechanismMoved = true;
        in.poseBack = false;
        in.nowMs = 100;
        se::sealStep(in, &st, &w);
        in.held = false;
        in.mechanismMoved = false;
        in.poseBack = true;
        uint64_t first = 0;
        bool blockedInside = false;
        for (uint64_t t = 110; t <= 3000 && first == 0; t += 10) {   // 30 frames of poses back inside the first 400 ms
            in.nowMs = t;
            if (se::sealStep(in, &st, &w) == se::SealAction::kRequestEngage) first = t;
            else if (st.holdBlocked) blockedInside = true;
        }
        check(blockedInside && first == 2600, "transition (b): a Lift back at its pose within the hold re-seals at the hold's end (2600 ms), not at 30 frames");
        check(!st.holdBlocked && st.reseal == 0 && st.requested, "transition (b): the request that passes the hold clears the blocked flag");
    }
    {
        // (c) an origin shift while docked and aboard with nothing held: no request for kTransitionHoldMs, then the request
        se::SealState st;
        se::ReleaseWhy w = se::ReleaseWhy::kNone;
        se::SealInputs in = inHangar(100);
        in.originShifted = true;
        check(se::sealStep(in, &st, &w) == se::SealAction::kNone && st.holdBlocked && st.holdUntilMs == 100 + se::kTransitionHoldMs,
              "transition (c): an origin shift while docked and aboard sets the hold and asks for no seal");
        in.originShifted = false;
        check(firstEngage(&st, in, 200, 4000) == 2600, "transition (c): the seal comes back at the hold's end (2600 ms), not sooner");
    }
    {
        // (d) during the hold the seat leaves and comes back: the on-foot tick changes nothing else, and no request comes inside the hold
        se::SealState st;
        se::ReleaseWhy w = se::ReleaseWhy::kNone;
        se::SealInputs in = inHangar(100);
        in.originShifted = true;
        se::sealStep(in, &st, &w);
        in.originShifted = false;
        in.aboard = false;
        in.nowMs = 200;
        const se::SealState before = st;
        check(se::sealStep(in, &st, &w) == se::SealAction::kNone, "transition (d): on foot inside the hold: no request");
        check(st.holdUntilMs == before.holdUntilMs && st.requested == before.requested && st.reseal == before.reseal && st.mechanism == before.mechanism &&
                  st.poseRun == before.poseRun && st.camInRun == before.camInRun && st.camOutRun == before.camOutRun && st.padAddr == before.padAddr,
              "transition (d): the on-foot tick inside the hold changes no other state");
        in.aboard = true;
        in.nowMs = 300;
        check(se::sealStep(in, &st, &w) == se::SealAction::kNone && st.holdBlocked, "transition (d): back aboard inside the hold is still held back");
        check(firstEngage(&st, in, 400, 4000) == 2600, "transition (d): the request comes at the hold's end");
    }
    {
        // (e) an undock clears the hold, so the next dock seals at once
        se::SealState st;
        se::ReleaseWhy w = se::ReleaseWhy::kNone;
        se::SealInputs in = inHangar(100);
        in.originShifted = true;
        se::sealStep(in, &st, &w);
        in.originShifted = false;
        in.docked = false;
        in.nowMs = 200;
        check(se::sealStep(in, &st, &w) == se::SealAction::kNone && st.holdUntilMs == 0, "transition (e): an undock clears the hold");
        in.docked = true;
        in.nowMs = 300;
        check(se::sealStep(in, &st, &w) == se::SealAction::kRequestEngage, "transition (e): the next dock seals at once (300 ms, inside the old hold's span)");
    }
    {
        // (f) a setting toggle resets the hold with the rest of the state
        se::SealState st;
        se::ReleaseWhy w = se::ReleaseWhy::kNone;
        se::SealInputs in = inHangar(100);
        in.originShifted = true;
        se::sealStep(in, &st, &w);
        in.originShifted = false;
        in.setting = false;
        in.nowMs = 200;
        check(se::sealStep(in, &st, &w) == se::SealAction::kNone && st.holdUntilMs == 0 && !st.holdBlocked, "transition (f): the setting off resets the hold");
        in.setting = true;
        in.nowMs = 300;
        check(se::sealStep(in, &st, &w) == se::SealAction::kRequestEngage, "transition (f): the setting on again seals at once");
    }
    {
        // (g) no shift and no release: the seal is as before, requests at once, and the hold never starts
        se::SealState st;
        se::ReleaseWhy w = se::ReleaseWhy::kNone;
        se::SealInputs in = inHangar(0);
        check(se::sealStep(in, &st, &w) == se::SealAction::kRequestEngage && st.holdUntilMs == 0 && !st.holdBlocked,
              "transition (g): no shift and no release: the seal requests at once and no hold starts");
    }
    {
        // (h) the r1 out-then-in edge after a Lift release is not held: it re-seals at once, inside the hold (the v14.24 edge check pins the same)
        se::SealState st;
        se::ReleaseWhy w = se::ReleaseWhy::kNone;
        se::SealInputs in = inHangar(0);
        se::sealStep(in, &st, &w);
        in.held = true;
        in.mechanismMoved = true;
        in.nowMs = 100;
        se::sealStep(in, &st, &w);
        in.held = false;
        in.mechanismMoved = false;
        in.inHangar = false;
        in.nowMs = 200;
        se::sealStep(in, &st, &w);
        in.inHangar = true;
        in.nowMs = 300;
        check(se::sealStep(in, &st, &w) == se::SealAction::kRequestEngage, "transition (h): the r1 out-then-in edge after a Lift release re-seals at once, inside the hold");
    }
    {
        // (i) review follow-up of main 4bd06f12, F2 (scratch repro repro_transition.cpp): a Lift release, r1 out, THEN an origin shift while unsealed, then r1 back in.
        // The edge exemption belongs to the Lift hold; the later origin-shift hold (a boarding or a disembark whose Status.json seat still reads aboard) must still apply.
        se::SealState st;
        se::ReleaseWhy w = se::ReleaseWhy::kNone;
        se::SealInputs in = inHangar(0);
        se::sealStep(in, &st, &w);
        in.held = true;
        in.mechanismMoved = true;
        in.nowMs = 100;
        check(se::sealStep(in, &st, &w) == se::SealAction::kRelease && w == se::ReleaseWhy::kMechanism, "transition (i): the Lift release");
        in.held = false;
        in.mechanismMoved = false;
        in.inHangar = false;
        in.nowMs = 200;
        se::sealStep(in, &st, &w);
        check(st.reseal == 2, "transition (i): r1 out leaves the edge pending");
        in.originShifted = true;
        in.nowMs = 210;
        se::sealStep(in, &st, &w);
        check(st.holdUntilMs == 210 + se::kTransitionHoldMs, "transition (i): the origin shift starts a new hold");
        in.originShifted = false;
        in.inHangar = true;
        in.nowMs = 300;
        check(se::sealStep(in, &st, &w) == se::SealAction::kNone && st.holdBlocked && !st.requested,
              "transition (i): r1 back in inside the origin-shift hold does NOT re-seal on the old edge");
        in.nowMs = 210 + se::kTransitionHoldMs - 1;
        check(se::sealStep(in, &st, &w) == se::SealAction::kNone, "transition (i): still held a millisecond before the hold ends");
        in.nowMs = 210 + se::kTransitionHoldMs;
        check(se::sealStep(in, &st, &w) == se::SealAction::kRequestEngage, "transition (i): the seal is requested when the origin-shift hold ends");
    }


    // ---- Built 2026-10-15: the anomaly-armed seal trace (station_seal_trace.h): the ring, the four triggers, the dump's shape, the cap and spacing, the bound ----
    {
        // the ring holds docked ticks only, oldest first across the wrap; a quiet docked stream that is not held writes nothing
        se::SealTrace tr;
        std::vector<std::string> out;
        for (uint32_t i = 0; i < 10; ++i) {
            se::TickRecord r;
            r.frame = 900 + i;
            r.nowMs = 100ull * i;
            r.docked = false;
            tr.step(r, captureLine, &out);
        }
        for (uint32_t i = 0; i < 70; ++i) {
            se::TickRecord r;
            r.frame = i;
            r.nowMs = 1000 + 100ull * i;
            r.docked = true;
            tr.step(r, captureLine, &out);
        }
        check(tr.count == se::kTraceRing && tr.at(0).frame == 6 && tr.at(63).frame == 69 && tr.at(30).frame == 36,
              "trace ring: docked ticks only, the last 64 kept, oldest first across the wrap");
        check(out.empty(), "trace: a quiet docked stream that is not held writes nothing");
    }
    {
        // T1: a held cell kept stale. Quiet ticks (refreshed or unmoved) before it write nothing.
        se::SealTrace tr;
        std::vector<std::string> out;
        for (uint32_t i = 0; i < 300; ++i) {
            se::TickRecord r = heldTick(i, 100ull * i);
            if (i % 2 == 1) r.refresh = se::HeldRefresh::kNoChange;
            tr.step(r, captureLine, &out);
        }
        check(out.empty(), "trace: 300 quiet held ticks (refreshed and unmoved) write nothing");
        se::TickRecord bad = heldTick(300, 30000);
        bad.refresh = se::HeldRefresh::kKeptStale;
        tr.step(bad, captureLine, &out);
        check(!out.empty() && out[0].find("trigger T1 at frame 300") != std::string::npos, "trace T1: a held cell kept stale (its pose unreadable) fires");
    }
    {
        // T2: the centre moved more than 2 m from the previous tick's; a logged origin shift does not excuse it. 0.5 m a tick does not.
        se::SealTrace tr;
        std::vector<std::string> out;
        for (uint32_t i = 0; i < 20; ++i) {
            se::TickRecord r = heldTick(i, 100ull * i);
            r.centre[0] = 0.5f * static_cast<float>(i);
            tr.step(r, captureLine, &out);
        }
        check(out.empty(), "trace T2: a centre moving 0.5 m a tick does not fire");
        se::TickRecord jump = heldTick(20, 2000);
        jump.centre[0] = 0.5f * 19.0f + 2.5f;
        jump.originShift = true;
        tr.step(jump, captureLine, &out);
        check(!out.empty() && out[0].find("trigger T2 at frame 20") != std::string::npos,
              "trace T2: a centre moved 2.5 m from the previous tick fires, with the origin shift logged or not");
    }
    {
        // No T3 (removed, log 165224): a cull and rig-veto spike mid-seal, after a steady stream of 40 held ticks, writes no dump
        se::SealTrace tr;
        std::vector<std::string> out;
        uint64_t culled = 0;
        uint64_t rig = 0;
        for (uint32_t i = 0; i < 40; ++i) {
            se::TickRecord r = heldTick(i, 100ull * i);
            culled += 2;
            rig += 1;
            r.culled = culled;
            r.rigVetoes = rig;
            tr.step(r, captureLine, &out);
        }
        se::TickRecord spike = heldTick(40, 4000);
        culled += 500;
        rig += 300;
        spike.culled = culled;
        spike.rigVetoes = rig;
        tr.step(spike, captureLine, &out);
        check(out.empty(), "trace (T3 removed): a cull and rig-veto spike mid-seal writes no dump");
    }
    {
        // No T3 (removed, log 165224): a cull and rig-veto spike two ticks into a seal, with no held history, writes no dump
        se::SealTrace tr;
        std::vector<std::string> out;
        tr.step(heldTick(0, 0), captureLine, &out);
        se::TickRecord b = heldTick(1, 22);
        b.culled = 500;
        b.rigVetoes = 300;
        tr.step(b, captureLine, &out);
        se::TickRecord c = heldTick(2, 45);
        c.culled = 1000;
        c.rigVetoes = 600;
        tr.step(c, captureLine, &out);
        check(out.empty() && tr.count == 3 && tr.at(0).sealedNow, "trace (T3 removed): a cull and rig-veto spike at seal onset writes no dump");
    }
    {
        // T4: the own pad missing from three informative passes in a row; an empty pass is no evidence; the pad back in a pass starts the run again
        se::SealTrace tr;
        std::vector<std::string> out;
        se::TickRecord r = heldTick(0, 0);
        r.ownInPass = false;
        tr.step(r, captureLine, &out);
        r.frame = 1;
        r.nowMs = 100;
        tr.step(r, captureLine, &out);
        r.frame = 2;
        r.nowMs = 200;
        r.verdict = se::TraceVerdict::kEmpty;
        tr.step(r, captureLine, &out);
        check(out.empty(), "trace T4: two absent informative passes and an empty one: nothing yet");
        r.frame = 3;
        r.nowMs = 300;
        r.verdict = se::TraceVerdict::kInformative;
        tr.step(r, captureLine, &out);
        check(!out.empty() && out[0].find("trigger T4 at frame 3") != std::string::npos, "trace T4: the own pad absent from three informative passes in a row fires");
    }
    {
        se::SealTrace tr;
        std::vector<std::string> out;
        const bool pattern[] = {false, false, true, false, false};   // ownInPass per pass: the pad back in the third resets the run
        for (uint32_t i = 0; i < 5; ++i) {
            se::TickRecord r = heldTick(i, 100ull * i);
            r.ownInPass = pattern[i];
            tr.step(r, captureLine, &out);
        }
        check(out.empty(), "trace T4: the pad back in a pass starts the run again (two absent, one present, two absent: quiet)");
    }
    {
        // the dump: 64 records before the trigger (its own among them), the header, 64 after, the end line: 130 lines
        se::SealTrace tr;
        std::vector<std::string> out;
        for (uint32_t i = 0; i < 100; ++i) tr.step(heldTick(i, 100ull * i), captureLine, &out);
        se::TickRecord bad = heldTick(100, 10000);
        bad.refresh = se::HeldRefresh::kKeptStale;
        tr.step(bad, captureLine, &out);
        for (uint32_t i = 101; i <= 170; ++i) tr.step(heldTick(i, 100ull * i), captureLine, &out);
        check(out.size() == 130, "trace dump: the header, 64 records before the trigger, 64 after and the end line (130 lines)");
        check(out.size() == 130 && out[0].rfind("optimized station trace (dump 1 of 4, trigger T1 at frame 100)", 0) == 0,
              "trace dump: the header names the dump, the trigger and its frame");
        check(out.size() == 130 && out[1].rfind("f=37 ", 0) == 0 && out[64].rfind("f=100 ", 0) == 0 && out[65].rfind("f=101 ", 0) == 0 &&
                  out[128].rfind("f=164 ", 0) == 0,
              "trace dump: the first pre line is frame 37, the trigger's own record is the last pre line, the post lines follow it");
        check(out.size() == 130 && out[129].rfind("optimized station trace end (dump 1 of 4: 64 records before the trigger, 64 after)", 0) == 0,
              "trace dump: the end line closes the dump after the 64th record after the trigger");
    }
    {
        // the cap and the spacing: a trigger on every tick at 100 ms for 200 s writes four dumps, at 0 s, 30 s, 60 s and 90 s; the fourth says it is the last
        se::SealTrace tr;
        std::vector<std::string> out;
        for (uint32_t i = 0; i <= 2000; ++i) {
            se::TickRecord r = heldTick(i, 100ull * i);
            r.refresh = se::HeldRefresh::kKeptStale;
            tr.step(r, captureLine, &out);
        }
        std::vector<std::string> headers;
        for (const std::string& l : out)
            if (l.rfind("optimized station trace (dump ", 0) == 0) headers.push_back(l);
        check(headers.size() == 4, "trace cap: four dumps a session, however long the trigger holds");
        check(headers.size() == 4 && headers[1].find("at frame 300") != std::string::npos && headers[2].find("at frame 600") != std::string::npos &&
                  headers[3].find("at frame 900") != std::string::npos,
              "trace spacing: the dumps follow their triggers at least 30 s apart (frames 300, 600, 900 at 100 ms)");
        check(headers.size() == 4 && headers[3].find("dump 4 of 4") != std::string::npos && headers[3].find("the last dump this session") != std::string::npos &&
                  headers[2].find("last") == std::string::npos,
              "trace cap: the fourth header says it is the last, the earlier ones do not");
    }
    {
        // the line bound: every integer at 20 digits and the floats at their limits stays under 300 characters, with the event words all set
        se::TickRecord r;
        r.frame = 0xFFFFFFFFu;
        r.nowMs = ~0ull;
        r.docked = true;
        r.held = true;
        r.verdict = se::TraceVerdict::kUnfinished;
        r.thin = true;
        r.passCalls = ~0ull;
        r.ownSet = true;
        r.ownInPass = true;
        r.ownUnseen = 0xFFFFFFFFu;
        r.refresh = se::HeldRefresh::kKeptStale;
        r.centre[0] = 1e9f;
        r.centre[1] = -1e9f;
        r.centre[2] = static_cast<float>(std::nan(""));
        r.frameShiftM = 3.4e38f;
        r.camValid = true;
        r.cam[0] = -3.4e38f;
        r.cameraOutside = true;
        r.rigVetoes = ~0ull;
        r.producerVetoes = ~0ull;
        r.culled = ~0ull;
        r.released = true;
        r.why = se::ReleaseWhy::kMechanism;
        r.originShift = true;
        r.holdStarted = true;
        r.ms = ~0ull;
        r.sealedNow = true;
        r.shiftM = 1e9f;
        r.camJumpM = -1e9f;
        r.rigDelta = ~0ull;
        r.producerDelta = ~0ull;
        r.cullDelta = ~0ull;
        r.trigger = se::TraceTrigger::kOwnAbsent;
        for (uint64_t& d : r.padD) d = ~0ull;   // built 2026-11-03: the pad's stages and the pad's rig, at their limits
        r.rig.known = true;
        r.rig.publication = 3;
        r.rig.verdict = 255;
        r.rig.run = 0xFFFFFFFFu;
        r.rig.bounded = true;
        r.rig.distM = 1e9f;
        r.rig.radiusM = -1e9f;
        char line[448] = {};
        const int n = se::formatTickLine(r, line, sizeof(line));
        check(n > 0 && n < 440 && std::strlen(line) < 440, "trace line: under 440 characters with every integer at 20 digits and the floats at their limits");
        check(std::string(line).rfind("f=4294967295 t=18446744073709551615 h=1 v=U th=1 pc=18446744073709551615 op=1 un=4294967295 rf=k ", 0) == 0 &&
                  std::strstr(line, "hc=99999.0,-99999.0,nan hs=99999.00 fs=99999.00 cv=1 cj=-99999.00 co=1 ") != nullptr &&
                  std::strstr(line, "ev=seal+rel:mechanism+origin+hold+T4") != nullptr,
              "trace line: the keys in order, the floats clamped to 99999, the event words joined");
        check(std::strstr(line, "cu=18446744073709551615 pa=99999/99999/99999/99999 pp=99999/99999 pr=99999/99999/99999/99999/99999 "
                                "pg=1/3/255/4294967295/99999.0/-99999.0 ev=seal+") != nullptr,
              "trace line: the pad's stages follow the cull count, each delta capped at 99999, the pad's rig after them, the events last");
    }
    {
        // Built 2026-11-03: the pad's stages as deltas. The window totals rise; the trace takes the rise, and a window reset starts them again at zero.
        se::SealTrace tr;
        std::vector<std::string> out;
        se::TickRecord a = heldTick(0, 0);
        a.pad[se::kPadStore] = 2;
        a.pad[se::kPadProdVetoed] = 7;
        tr.step(a, captureLine, &out);
        se::TickRecord b = heldTick(1, 16);
        b.pad[se::kPadStore] = 3;
        b.pad[se::kPadProdVetoed] = 7;
        b.pad[se::kPadRigVetoed] = 1;
        b.pad[se::kPadRigStale] = 2;
        b.pad[se::kPadRigMoved] = 1;
        tr.step(b, captureLine, &out);
        check(tr.at(0).padD[se::kPadStore] == 2 && tr.at(1).padD[se::kPadStore] == 1 && tr.at(1).padD[se::kPadProdVetoed] == 0 &&
                  tr.at(1).padD[se::kPadRigVetoed] == 1,
              "pad stages: the trace takes the rise of each window total, not the total");
        char line[448] = {};
        se::formatTickLine(tr.at(1), line, sizeof(line));
        check(std::strstr(line, " pa=1/0/0/0 pp=0/0 pr=1/0/0/0/3 pg=0/1/255/0/0.0/0.0 ev=") != nullptr,
              "pad stages: the row reads store/early/unread/gathered, producer vetoed/kept, rig vetoed/refresh/mixed/unknown/not-published-stale-or-moved, and an unlearned rig");
        tr.windowReset();
        se::TickRecord c = heldTick(2, 32);
        c.pad[se::kPadStore] = 1;
        tr.step(c, captureLine, &out);
        check(tr.at(2).padD[se::kPadStore] == 1, "pad stages: after a window reset the totals start again at zero, so the first delta is the total");
        se::TickRecord d = heldTick(3, 48);
        d.rig.known = true;
        d.rig.bounded = false;
        d.rig.distM = 50.0f;
        d.rig.radiusM = 9.0f;
        d.rig.verdict = 2;
        d.rig.run = 17;
        d.rig.publication = 0;
        se::formatTickLine(d, line, sizeof(line));
        check(std::strstr(line, " pg=1/0/2/17/0.0/0.0 ev=") != nullptr, "pad stages: a rig with no bound prints no distance or radius, the rest of its state is there");
        d.rig.bounded = true;
        se::formatTickLine(d, line, sizeof(line));
        check(std::strstr(line, " pg=1/0/2/17/50.0/9.0 ev=") != nullptr, "pad stages: a bounded rig prints its distance to the cell and its radius");
    }
    {
        // the time since the seal: from the seal's rising edge
        se::SealTrace tr;
        std::vector<std::string> out;
        tr.step(heldTick(0, 1000), captureLine, &out);
        tr.step(heldTick(1, 1500), captureLine, &out);
        check(tr.count == 2 && tr.at(0).sealedNow && !tr.at(1).sealedNow && tr.at(1).ms == 500,
              "trace: the seal is marked on its rising edge and the time since the seal counts from it");
    }

    // ---- Built 2026-10-21: sphereOverlapsBox, the reach rule's own test. A record whose centre the box culls still draws into the bay when its sphere reaches
    // the box ----------------------------------------------------------------------------------------------------------------------------------------------
    {
        const float half[3] = {160.0f, 55.0f, 127.0f};
        const float inside[3] = {10.0f, -20.0f, 100.0f};
        check(se::sphereOverlapsBox(inside, 0.5f, half), "box overlap: a centre inside the box overlaps with any radius");
        const float above[3] = {0.0f, 0.0f, 132.0f};   // 5 m past the top
        check(!se::sphereOverlapsBox(above, 4.9f, half) && se::sphereOverlapsBox(above, 5.0f, half),
              "box overlap: a centre 5 m past a face overlaps from a radius of 5 m (the sphere touches the box)");
        const float corner[3] = {163.0f, 59.0f, 0.0f};   // 3 and 4 m past two faces: 5 m from the edge
        check(!se::sphereOverlapsBox(corner, 4.9f, half) && se::sphereOverlapsBox(corner, 5.0f, half),
              "box overlap: past two faces the distance is to the edge, not to either face (3, 4 -> 5 m)");
        const float below[3] = {-165.0f, 0.0f, 0.0f};
        check(se::sphereOverlapsBox(below, 6.0f, half) && !se::sphereOverlapsBox(below, 4.0f, half), "box overlap: the negative side is judged the same way");
        const float distant[3] = {1000.0f, 0.0f, 0.0f};
        check(!se::sphereOverlapsBox(distant, 800.0f, half) && se::sphereOverlapsBox(distant, 900.0f, half), "box overlap: a big far sphere (a deck or hull mesh) overlaps by its radius");
        check(!se::sphereOverlapsBox(inside, std::nanf(""), half) && !se::sphereOverlapsBox(inside, INFINITY, half) && !se::sphereOverlapsBox(inside, -1.0f, half),
              "box overlap: a non-finite or negative radius never overlaps");
        // The chain the rule runs: world centre -> box coordinates -> overlap, in a box turned on its side and far from the origin.
        se::OwnCell cell;
        cell.has = true;
        cell.box = true;
        cell.boxCentre[0] = 1000.0f;
        cell.boxCentre[1] = 2000.0f;
        cell.boxCentre[2] = 3000.0f;
        cell.boxU[1] = 1.0f;
        cell.boxV[0] = -1.0f;
        cell.boxW[2] = 1.0f;
        cell.boxHalf[0] = 160.0f;
        cell.boxHalf[1] = 55.0f;
        cell.boxHalf[2] = 127.0f;
        const float world[3] = {970.0f, 2170.0f, 3000.0f};   // U 170, V 30, W 0: 10 m past the length face
        float o[3] = {};
        se::boxCoords(world, cell, o);
        check(o[0] == 170.0f && o[1] == 30.0f && o[2] == 0.0f, "box overlap chain: a world centre reads in box coordinates (U 170, 10 m past the length face)");
        check(!se::sphereOverlapsBox(o, 9.9f, cell.boxHalf) && se::sphereOverlapsBox(o, 10.0f, cell.boxHalf), "box overlap chain: the overlap follows the box's own axes");
    }
    // ---- Built 2026-10-21: the reach rule. In box mode a record whose centre lies outside the box is kept (why reach) when its sphere overlaps the box,
    // whether or not the sphere contains the camera (the carrier's DoorA1 and DoorA2 do). The box is the large carrier bay of the flight log (half-extents U 223, V 74, W 163.9), axis-aligned at the origin, so a box
    // coordinate is a world coordinate; the door panels are the log's DoorA2 (centre -8.3,-88.6,-9.0, radius 72.5 m, outside the width face by 14.6 m) ----------
    {
        se::OwnCell box;
        box.has = true;
        box.box = true;
        box.boxU[0] = 1.0f;
        box.boxV[1] = 1.0f;
        box.boxW[2] = 1.0f;
        box.boxHalf[0] = 223.0f;
        box.boxHalf[1] = 74.0f;
        box.boxHalf[2] = 163.9f;
        const float camera[3] = {0.0f, 0.0f, -100.0f};   // the commander in the cockpit, on the pad
        const float door[3] = {-8.3f, -88.6f, -9.0f};
        const uintptr_t none[1] = {0};
        const float R = 80.0f;
        se::NamedKeep v = se::keepNamedCell(door, 72.5f, camera, R, box, none, 0, 0x1000);
        check(v.keep && v.why == se::KeepWhy::kReach && !v.containing,
              "reach: a door whose centre is 14.6 m outside the width face and whose sphere (72.5 m) reaches in is kept, by reach");
        const float cameraNearDoor[3] = {-8.3f, -60.0f, -9.0f};   // inside the box and inside the door's sphere (28.6 m from its centre)
        v = se::keepNamedCell(door, 72.5f, cameraNearDoor, R, box, none, 0, 0x1000);
        check(v.keep && v.why == se::KeepWhy::kReach && v.containing,
              "reach: the same record is kept, by reach, when its sphere contains the camera (the carrier's DoorA1 and DoorA2 at the pad)");
        const float deck[3] = {0.0f, -400.0f, 0.0f};   // a hull or deck mesh: centre missing outside, a radius that overlaps the box and holds the camera
        v = se::keepNamedCell(deck, 450.0f, camera, R, box, none, 0, 0x2000);
        check(v.keep && v.why == se::KeepWhy::kReach && v.containing, "reach: a big record that overlaps the box and contains the camera is kept, by reach");
        v = se::keepNamedCell(deck, 340.0f, camera, R, box, none, 0, 0x2000);   // reaches 14 m into the box, 412 m from the camera
        check(v.keep && v.why == se::KeepWhy::kReach && !v.containing,
              "reach: a big record that overlaps the box and does not contain the camera is kept (the rule is the sphere's, not its size)");
        const float missing[3] = {0.0f, -200.0f, 0.0f};   // outside the width face by 126 m
        v = se::keepNamedCell(missing, 72.5f, camera, R, box, none, 0, 0x3000);
        check(!v.keep && v.why == se::KeepWhy::kNone, "reach: a record whose sphere misses the box is culled");
        const float touch[3] = {0.0f, -146.5f, 0.0f};   // 72.5 m past the width face: the sphere just touches it
        check(se::keepNamedCell(touch, 72.5f, camera, R, box, none, 0, 0x3000).why == se::KeepWhy::kReach &&
                  !se::keepNamedCell(touch, 72.4f, camera, R, box, none, 0, 0x3000).keep,
              "reach: a sphere that just touches a face is kept, one that stops 0.1 m short is culled");
        const float corner[3] = {223.0f + 3.0f, 74.0f + 4.0f, 0.0f};   // 5 m from the edge, past two faces
        check(se::keepNamedCell(corner, 5.0f, camera, R, box, none, 0, 0x4000).why == se::KeepWhy::kReach &&
                  !se::keepNamedCell(corner, 4.9f, camera, R, box, none, 0, 0x4000).keep,
              "reach: past two faces the distance is to the edge");
        const float inside[3] = {10.0f, 20.0f, 30.0f};
        v = se::keepNamedCell(inside, 5.0f, camera, R, box, none, 0, 0x5000);
        check(v.keep && v.why == se::KeepWhy::kCell, "reach: a centre inside the box is kept by the cell, as before");
        const float insideBig[3] = {10.0f, 20.0f, -90.0f};   // centre in the box and a sphere that holds the camera: the centre rule keeps it
        v = se::keepNamedCell(insideBig, 500.0f, camera, R, box, none, 0, 0x5000);
        check(v.keep && v.why == se::KeepWhy::kCell && v.containing, "reach: a centre inside the box is kept even when the sphere contains the camera, as before");
        const uintptr_t explicitSet[1] = {0x6000};
        v = se::keepNamedCell(missing, 72.5f, camera, R, box, explicitSet, 1, 0x6000);
        check(v.keep && v.why == se::KeepWhy::kExplicit, "reach: an explicit part's nearest instance is kept, as before");
        v = se::keepNamedCell(door, 72.5f, camera, R, box, explicitSet, 1, 0x6000);
        check(v.keep && v.why == se::KeepWhy::kExplicit, "reach: an explicit part that also reaches into the box keeps its explicit label (the reach count is the new keeps only)");
        check(!se::keepNamedCell(missing, 72.5f, cameraNearDoor, R, box, explicitSet, 1, 0x7000).keep, "reach: another address is not the explicit part");
        check(!se::keepNamedCell(door, std::nanf(""), camera, R, box, none, 0, 0x1000).keep && !se::keepNamedCell(door, INFINITY, camera, R, box, none, 0, 0x1000).keep,
              "reach: a non-finite radius is never kept by reach");
        // A box turned and moved: the rule follows the box's own axes. U along world y, V along world -x, W along world z, centre (1000, 2000, 3000).
        se::OwnCell turned = box;
        turned.boxU[0] = 0.0f;
        turned.boxU[1] = 1.0f;
        turned.boxV[0] = -1.0f;
        turned.boxV[1] = 0.0f;
        turned.boxCentre[0] = 1000.0f;
        turned.boxCentre[1] = 2000.0f;
        turned.boxCentre[2] = 3000.0f;
        const float turnedDoor[3] = {1000.0f + 88.6f, 2000.0f - 8.3f, 3000.0f - 9.0f};   // box coordinates -8.3, -88.6, -9.0
        const float turnedCamera[3] = {1000.0f, 2000.0f, 3000.0f - 100.0f};
        v = se::keepNamedCell(turnedDoor, 72.5f, turnedCamera, R, turned, none, 0, 0x1000);
        check(v.keep && v.why == se::KeepWhy::kReach, "reach: in a turned box the door's sphere is judged against the box's own axes");
        const float wrongSide[3] = {1000.0f - 88.6f, 2000.0f - 8.3f, 3000.0f - 9.0f};   // 88.6 m the other way across the width: reaches in as well (the box is symmetric)
        check(se::keepNamedCell(wrongSide, 72.5f, turnedCamera, R, turned, none, 0, 0x1000).why == se::KeepWhy::kReach,
              "reach: the missing side of the width is judged the same way");
        const float alongLength[3] = {1000.0f - 8.3f, 2000.0f + 223.0f + 80.0f, 3000.0f};   // 80 m past the length face
        check(!se::keepNamedCell(alongLength, 72.5f, turnedCamera, R, turned, none, 0, 0x1000).keep, "reach: a sphere 80 m past the length face does not reach (72.5 m)");
        // The other cells judge the centre alone: a sphere that overlaps a capsule or a pad sphere is not kept for it.
        se::OwnCell sphereCell;
        sphereCell.has = true;
        sphereCell.radius = 50.0f;
        const float nearSphere[3] = {60.0f, 0.0f, 0.0f};
        v = se::keepNamedCell(nearSphere, 30.0f, camera, R, sphereCell, none, 0, 0x8000);
        check(!v.keep && v.why == se::KeepWhy::kNone, "reach: a pad-sphere cell keeps by the centre alone (a sphere that overlaps it is culled)");
        v = se::keepNamedCell(inside, 5.0f, camera, R, sphereCell, none, 0, 0x8000);
        check(v.keep && v.why == se::KeepWhy::kCell, "reach: a pad-sphere cell keeps a centre inside it by the cell");
        se::OwnCell capsuleCell;
        capsuleCell.has = true;
        capsuleCell.capsule = true;
        capsuleCell.radius = 20.0f;
        capsuleCell.capB[2] = 100.0f;
        const float nearCapsule[3] = {30.0f, 0.0f, 50.0f};
        v = se::keepNamedCell(nearCapsule, 30.0f, camera, R, capsuleCell, none, 0, 0x9000);
        check(!v.keep && v.why == se::KeepWhy::kNone, "reach: a capsule cell keeps by the centre alone (a sphere that overlaps it is culled)");
        // No cell: the sphere rule for the frame, unchanged.
        se::OwnCell noCell;
        v = se::keepNamedCell(door, 72.5f, camera, R, noCell, none, 0, 0x1000);
        check(v.keep && v.why == se::KeepWhy::kSphere, "reach: with no cell the sphere rule decides (this door is 127 m from the camera, inside 72.5 + 80 m)");
        v = se::keepNamedCell(missing, 72.5f, camera, R, noCell, none, 0, 0x3000);
        check(!v.keep && v.why == se::KeepWhy::kNone, "reach: with no cell a record beyond the camera sphere is culled (224 m from the camera)");
    }

    // ---- Built 2026-10-21: the flat sun draw issued with every write masked (station_draw_mask.h; the states' descriptions: Mask itself needs a device) ------
    {
        namespace sm = edvr::stationmask;
        const D3D11_BLEND_DESC b = sm::blendDesc();
        bool allOff = true, noBlend = true;
        for (UINT i = 0; i < sm::kTargets; ++i) {
            allOff = allOff && b.RenderTarget[i].RenderTargetWriteMask == 0;
            noBlend = noBlend && !b.RenderTarget[i].BlendEnable;
        }
        check(sm::kTargets == 8 && allOff && noBlend && b.IndependentBlendEnable && !b.AlphaToCoverageEnable,
              "sun mask: the blend state writes no colour to any of the eight targets, each by its own entry, with no blending or alpha-to-coverage");
        D3D11_DEPTH_STENCIL_DESC bound = {};
        bound.DepthEnable = TRUE;
        bound.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        bound.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
        bound.StencilEnable = TRUE;
        bound.StencilReadMask = 0x0F;
        bound.StencilWriteMask = 0xF0;
        bound.FrontFace = {D3D11_STENCIL_OP_REPLACE, D3D11_STENCIL_OP_INCR, D3D11_STENCIL_OP_ZERO, D3D11_COMPARISON_EQUAL};
        bound.BackFace = {D3D11_STENCIL_OP_DECR, D3D11_STENCIL_OP_INVERT, D3D11_STENCIL_OP_KEEP, D3D11_COMPARISON_NOT_EQUAL};
        const D3D11_DEPTH_STENCIL_DESC masked = sm::maskedDepthStencilDesc(bound);
        D3D11_DEPTH_STENCIL_DESC restored = masked;   // put back the two writes the mask turns off: nothing else may differ from the bound state
        restored.DepthWriteMask = bound.DepthWriteMask;
        restored.StencilWriteMask = bound.StencilWriteMask;
        check(masked.DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ZERO && masked.StencilWriteMask == 0 && sm::sameDepthStencilDesc(restored, bound) &&
                  !sm::sameDepthStencilDesc(masked, bound),
              "sun mask: the depth-stencil state is the bound one with the depth write and the stencil write mask off and nothing else changed");
        check(masked.DepthEnable && masked.StencilEnable && masked.DepthFunc == bound.DepthFunc && masked.StencilReadMask == 0x0F,
              "sun mask: the depth test and the stencil test still run (only the writes are off)");
        check(sm::sameDepthStencilDesc(sm::maskedDepthStencilDesc(masked), masked), "sun mask: masking a masked state changes nothing (the cache key is stable)");
        const D3D11_DEPTH_STENCIL_DESC dflt = sm::defaultDepthStencilDesc();
        check(dflt.DepthEnable && dflt.DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ALL && dflt.DepthFunc == D3D11_COMPARISON_LESS && !dflt.StencilEnable &&
                  dflt.StencilReadMask == 0xFF && dflt.StencilWriteMask == 0xFF && dflt.FrontFace.StencilFunc == D3D11_COMPARISON_ALWAYS &&
                  dflt.FrontFace.StencilPassOp == D3D11_STENCIL_OP_KEEP,
              "sun mask: a context with no depth-stencil state bound (null) is D3D's default state: depth on, writes on, LESS, stencil off");
        const D3D11_DEPTH_STENCIL_DESC maskedDefault = sm::maskedDepthStencilDesc(dflt);
        check(maskedDefault.DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ZERO && maskedDefault.StencilWriteMask == 0 && maskedDefault.DepthEnable,
              "sun mask: the default state is masked the same way");
        // The comparison ignores the structure's padding (a desc read back from a state does not define it).
        static_assert(offsetof(D3D11_DEPTH_STENCIL_DESC, FrontFace) == offsetof(D3D11_DEPTH_STENCIL_DESC, StencilWriteMask) + 3, "two padding bytes before FrontFace");
        D3D11_DEPTH_STENCIL_DESC a, c;
        a = bound;
        c = bound;
        std::memset(reinterpret_cast<unsigned char*>(&a) + offsetof(D3D11_DEPTH_STENCIL_DESC, StencilWriteMask) + 1, 0xAA, 2);   // the two bytes before FrontFace
        std::memset(reinterpret_cast<unsigned char*>(&c) + offsetof(D3D11_DEPTH_STENCIL_DESC, StencilWriteMask) + 1, 0x55, 2);
        check(sm::sameDepthStencilDesc(a, c), "sun mask: two descs that differ only in padding compare the same");
        D3D11_DEPTH_STENCIL_DESC d2 = bound;
        d2.BackFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
        check(!sm::sameDepthStencilDesc(d2, bound), "sun mask: a difference in a back-face stencil operation compares different");
        // Mask holds nothing until a draw asks.
        sm::Mask idle;
        check(!idle.active() && !idle.begin(nullptr, nullptr, nullptr), "sun mask: without a context nothing is bound and begin() declines");
        idle.end(nullptr);
        check(!idle.active(), "sun mask: end() without a begin() does nothing");
    }


    // ---- The concourse cull (docs\concourse-cull-2026-11-05.md, src\d3d11\concourse_cull_core.h) ----------------------------------------------------------------
    {
        namespace cc = edvr::concourse;
        char msg[200];
        const char* const allowed[] = {"Standard_SocialArea", "SA_Station_01_Bar", "PS_Sittable_Sofa_01", "FUI_Python", "PROP_Tablet",
                                       "STAT_HoloBanner_VertA", "GUI_Interactive_Terminal_Standing_Gen_01", "TP_M_Exp01_HelmetLightRig", "HangarLobby",
                                       "TurboLift_Door_A"};
        for (const char* n : allowed) {
            std::snprintf(msg, sizeof(msg), "concourse allow-list: %s is the room", n);
            check(cc::concourseAllowName(n, nullptr), msg);
        }
        const char* const refused[] = {"WheelSection00", "Spoke03", "RingRoad07", "HabitatRingA", "DockingHub", "HangerMedium", "LandingPadSmallA",
                                       "Anaconda", "CobraMkV_Rear_Arm_Upper", "Exhaust", "Barrel", "PlantStack0", "PlantStack4"};
        for (const char* n : refused) {
            std::snprintf(msg, sizeof(msg), "concourse allow-list: %s is not the room", n);
            check(!cc::concourseAllowName(n, nullptr), msg);
        }
        check(!cc::concourseAllowName("sa_station", nullptr) && !cc::concourseAllowName("standard_socialarea", nullptr) && !cc::concourseAllowName("fui_python", nullptr),
              "concourse allow-list: the prefix match is exact case");
        check(!cc::concourseAllowName("", nullptr) && !cc::concourseAllowName(nullptr, nullptr), "concourse allow-list: an empty or null name is not allowed");
        check(!cc::concourseAllowName("XSA_Bar", nullptr), "concourse allow-list: a prefix matches at the start of the name only");
        // A surface port's room and what stands outside it, with the names two surveys read at a CraterOutpost (Flight 35): the station's list parts them, and nothing it would cull was within 126 m.
        for (const char* n : {"SA_PlanetPortHub_Compact", "SA_PlanetPort_Exterior", "SA_PlanetPort_Compact_Hitcheck", "SA_PP_Fed_JeromeArcher_Logo_OnGlass_01", "PS_Sittable_Sofa_Seat_01",
                              "PS_STAT_HoloScreen_Horiz_Apex01", "FUI_Anaconda", "PROP_Tablet", "GUI_Interactive_Terminal_Standing_Gen_01", "STAT_HoloBanner_VertA", "TurboLiftDoorLeft",
                              "TurboLift_Blocker", "Perturbator", "Root"})
            check(cc::concourseAllowName(n, nullptr), "concourse surface port: the room's own name is on the list");
        for (const char* n : {"PB_Gen_BuildingSectionB", "PB_LampPost", "PB_Gen_ScienceStation", "PP_SmallDock", "PP_LargeDock", "HangerLarge", "LargeHangerSkirt", "LandingPadLargeA",
                              "LandingPadSmall_GuideMarkers", "BakedSkirtBuildings", "CobraMkV_Hull", "Type9", "LargeHardpointBunkerE", "SkimmerDrone", "PlanetPortF"})
            check(!cc::concourseAllowName(n, nullptr), "concourse surface port: what stands outside is not on the list");
        check(cc::concourseAnchorName("Standard_SocialArea") && cc::concourseAnchorName("SA_PlanetPortHub_Compact"), "concourse anchors: the station's room and the surface port's");
        check(!cc::concourseAnchorName("SA_StationHub_Compact") && !cc::concourseAnchorName("SA_PlanetPort_Exterior") && !cc::concourseAnchorName(nullptr) && !cc::concourseAnchorName(""),
              "concourse anchors: nothing else");
        check(&cc::roomLayoutOf("Standard_SocialArea") == &cc::kRoomStation && cc::kRoomStation.bayN == cc::kBayIndexCount && cc::kRoomStation.backdropRadiusM == cc::kBackdropRadiusDefaultM,
              "concourse layouts: a station's room is what it was");
        check(&cc::roomLayoutOf("SA_PlanetPortHub_Compact") == &cc::kRoomPort && cc::kRoomPort.bayN == 12u && cc::kRoomPort.backdropRadiusM >= cc::kBackdropRadiusMinM && cc::kRoomPort.backdropRadiusM <= cc::kBackdropRadiusMaxM,
              "concourse layouts: a surface port's bay is n=12, on a sphere the key's range allows");
        check(&cc::roomLayoutOf(nullptr) == &cc::kRoomStation && &cc::roomLayoutOf("") == &cc::kRoomStation, "concourse layouts: no name is a station's");
        // An orbital outpost's room, from two surveys: its own hub, the bay n=30, the station's list parting room from outside with nothing culled within 165 m.
        check(cc::concourseAnchorName("SA_OutpostHub_Compact") && &cc::roomLayoutOf("SA_OutpostHub_Compact") == &cc::kRoomOutpost && cc::kRoomOutpost.bayN == 30u,
              "concourse layouts: an orbital outpost's anchor and its bay");
        check(cc::kRoomOutpost.roomReachM > 33.3f && !cc::anchorMeansRoomHere(cc::kRoomOutpost, true, true, 431.0f) && cc::anchorMeansRoomHere(cc::kRoomOutpost, true, true, 15.6f),
              "concourse layouts: an outpost's reach takes the bay and not the hangar lobby");
        check(!cc::concourseAnchorName("SA_Outpost_Exterior") && !cc::concourseAnchorName("SA_Outpost_01_Bar") && !cc::concourseAnchorName("Outpost"), "concourse anchors: an outpost's other big names are not");
        for (const char* n : {"SA_OutpostHub_Compact", "SA_Outpost_01_Bar", "SA_Outpost_01_Store", "SA_Outpost_Exterior", "SA_Outpost_Compact_Hitcheck", "SA_PP_All_EdmundMahon_Logo_01",
                              "PS_STAT_HoloScreen_BarScreen_1", "PS_Sittable_BarStool_01", "PS_FanBroken1", "TP_Crate"})
            check(cc::concourseAllowName(n, nullptr), "concourse outpost: the room's own name is on the list");
        for (const char* n : {"Outpost", "BlastShieldBase", "BlastShield_A", "LandingPadSmallA", "HangerSmall", "HangerMedium", "SmallHangerSkirt", "WeaponsPlatform", "AdvertDroneA_Advert04", "Spheres", "Tube"})
            check(!cc::concourseAllowName(n, nullptr), "concourse outpost: what stands outside is not on the list");
        check(cc::clearEntryScale(30, 30) == 1.0f && cc::clearEntryScale(30, 36) == 0.3f && cc::clearEntryScale(30, 12) == 0.3f && cc::clearEntryScale(36, 30) == 1.0f,
              "concourse clear: n=30 is the Orbis shop window, entering early, except where it is the bay");
        // A fleet carrier's concourse, from five surveys: its anchor, the bar's n=30, the bridge inside the reach, and the carrier's own names kept.
        check(cc::concourseAnchorName("SA_Carrier_01") && &cc::roomLayoutOf("SA_Carrier_01") == &cc::kRoomCarrier && cc::kRoomCarrier.bayN == 30u,
              "concourse layouts: a fleet carrier's anchor and its bar window");
        check(!cc::concourseAnchorName("SA_Carrier_01_Bridge") && !cc::concourseAnchorName("SA_Carrier_01_Bar"), "concourse anchors: the carrier's bridge and bar are not");
        check(cc::anchorMeansRoomHere(cc::kRoomCarrier, true, true, 53.7f) && !cc::anchorMeansRoomHere(cc::kRoomCarrier, true, true, 1331.0f), "concourse layouts: a carrier's reach takes the bridge");
        // The bridge is the carrier's live room (Flight 41): the camera inside its record's sphere (r=24.8, 12.3 m at the window), not between the rooms (27.4 m).
        check(cc::kRoomCarrier.liveRoomName != nullptr && std::strcmp(cc::kRoomCarrier.liveRoomName, "SA_Carrier_01_Bridge") == 0 && cc::concourseAllowName(cc::kRoomCarrier.liveRoomName, nullptr),
              "concourse layouts: a carrier's live room is its bridge, a name the store sees as allowed");
        check(cc::insideLiveRoom(cc::kRoomCarrier, true, true, 12.3f, 24.8f) && !cc::insideLiveRoom(cc::kRoomCarrier, true, true, 27.4f, 24.8f), "concourse layouts: at the bridge window, and not between");
        check(!cc::insideLiveRoom(cc::kRoomCarrier, false, true, 12.3f, 24.8f) && !cc::insideLiveRoom(cc::kRoomCarrier, true, false, 12.3f, 24.8f) && !cc::insideLiveRoom(cc::kRoomCarrier, true, true, 12.3f, 0.0f),
              "concourse layouts: not known is not inside");
        check(!cc::insideLiveRoom(cc::kRoomStation, true, true, 1.0f, 24.8f) && !cc::insideLiveRoom(cc::kRoomPort, true, true, 1.0f, 24.8f) && !cc::insideLiveRoom(cc::kRoomOutpost, true, true, 1.0f, 24.8f),
              "concourse layouts: no other room has one");
        {
            // Flight 42: one tick with the bridge's record unread, 6 m inside. The latch holds through it, and lets go when the record stays unread.
            cc::LiveRoomLatch live;
            check(!live.step(cc::kRoomCarrier, true, false, 0.0f, 0.0f, 1000), "live room latch: unread with nothing before is not inside");
            check(live.step(cc::kRoomCarrier, true, true, 6.0f, 24.8f, 2000), "live room latch: read inside is inside");
            check(live.step(cc::kRoomCarrier, true, false, 0.0f, 0.0f, 2016), "live room latch: one tick unread keeps it");
            check(live.step(cc::kRoomCarrier, false, true, 6.0f, 24.8f, 2000 + cc::kLiveRoomHoldMs), "live room latch: and the camera unknown, up to the hold");
            check(!live.step(cc::kRoomCarrier, true, false, 0.0f, 0.0f, 2001 + cc::kLiveRoomHoldMs), "live room latch: past it, not inside");
            check(!live.step(cc::kRoomCarrier, true, false, 0.0f, 0.0f, 2010 + cc::kLiveRoomHoldMs), "live room latch: and it stays out");
            check(live.step(cc::kRoomCarrier, true, true, 12.3f, 24.8f, 4000) && !live.step(cc::kRoomCarrier, true, true, 27.4f, 24.8f, 4016), "live room latch: a read outside says at once");
            check(!live.step(cc::kRoomCarrier, true, false, 0.0f, 0.0f, 4032), "live room latch: and holds nothing after it");
            check(live.step(cc::kRoomCarrier, true, true, 12.3f, 24.8f, 5000) && !live.step(cc::kRoomStation, true, true, 1.0f, 24.8f, 5016), "live room latch: a room with none is never inside");
            live.step(cc::kRoomCarrier, true, true, 12.3f, 24.8f, 6000);
            live.reset();
            check(!live.step(cc::kRoomCarrier, true, false, 0.0f, 0.0f, 6016), "live room latch: reset forgets it");
        }
        check(cc::kBakeAfterJumpMs >= cc::kBakeRoomSettleMs, "carrier jump: the bake waits at least as long as a room settling");
        {
            // The rooms ceiling (TEMPORARY): Flight 43's carrier, its doors at a2 -11 and +19. The elevator bay's survey stood at -25.4, the central space's at +4.5, the bridge from +21.
            cc::RoomSlab s;
            check(cc::kRoomCarrier.slabCuts == 2u && cc::kRoomCarrier.slabCut[0] == -11.0f && cc::kRoomCarrier.slabCut[1] == 19.0f, "rooms: a carrier's doors from the survey");
            check(cc::kRoomStation.slabCuts == 0u && cc::kRoomPort.slabCuts == 0u && cc::kRoomOutpost.slabCuts == 0u && !cc::roomSlabOf(cc::kRoomStation, 0.0f, &s),
                  "rooms: no other layout has doors, and there is no slab there");
            check(cc::roomSlabOf(cc::kRoomCarrier, -25.4f, &s) && s.lo <= -cc::kRoomSlabOpen && s.hi == -11.0f, "rooms: the elevator bay runs to its door");
            check(cc::roomSlabOutside(s, 0.0f, 10.0f) && cc::roomSlabOutside(s, 45.8f, 25.0f), "rooms: from there the central space's service and the bridge are another room");
            check(!cc::roomSlabOutside(s, 0.0f, 33.1f) && !cc::roomSlabOutside(s, -26.0f, 2.0f), "rooms: the anchor's sphere reaches it, and the lift is in it");
            check(cc::roomSlabOf(cc::kRoomCarrier, 4.5f, &s) && s.lo == -11.0f && s.hi == 19.0f, "rooms: the central space lies between the doors");
            check(cc::roomSlabOutside(s, -26.0f, 2.0f) && cc::roomSlabOutside(s, 45.8f, 25.0f) && !cc::roomSlabOutside(s, 18.0f, 2.0f) && !cc::roomSlabOutside(s, -12.0f, 1.5f),
                  "rooms: from there the lift and the bridge are not, and what reaches across a door is kept");
            check(cc::roomSlabOf(cc::kRoomCarrier, 21.2f, &s) && s.lo == 19.0f && s.hi >= cc::kRoomSlabOpen, "rooms: the bridge runs from its door");
            check(cc::roomSlabOutside(s, -26.0f, 2.0f) && cc::roomSlabOutside(s, 4.5f, 10.0f) && !cc::roomSlabOutside(s, 45.8f, 25.0f), "rooms: from the bridge the rest is another room");
            check(cc::roomSlabOf(cc::kRoomCarrier, -11.0f, &s) && s.lo == -11.0f && s.hi == 19.0f, "rooms: on a door's plane is the room it opens into");
            check(!cc::roomSlabOutside(s, std::nanf(""), 1.0f) && !cc::roomSlabOutside(s, 100.0f, -1.0f) && !cc::roomSlabOf(cc::kRoomCarrier, std::nanf(""), &s), "rooms: nothing unreadable is culled");
            cc::RoomSlab a, b;
            check(cc::roomSlabOf(cc::kRoomCarrier, 0.0f, &a) && cc::roomSlabOf(cc::kRoomCarrier, 10.0f, &b) && cc::sameRoomSlab(a, b) && cc::roomSlabOf(cc::kRoomCarrier, 20.0f, &b) &&
                      !cc::sameRoomSlab(a, b), "rooms: the slab changes only across a door");
        }
        {
            // The door rule: only a blocking door bounds what the camera can see into.
            cc::RoomSlab s;
            const bool both[2] = {true, true}, bayDoor[2] = {true, false}, bridgeDoor[2] = {false, true}, none[2] = {false, false};
            check(cc::roomSeenSlab(cc::kRoomCarrier, -17.3f, both, &s) && s.lo <= -cc::kRoomSlabOpen && s.hi == -11.0f, "doors: from the elevator bay, its shut door bounds it");
            check(cc::roomSeenSlab(cc::kRoomCarrier, -17.3f, bridgeDoor, &s) && s.lo <= -cc::kRoomSlabOpen && s.hi == 19.0f, "doors: its door open, the bridge's shut, the central space is seen");
            check(cc::roomSeenSlab(cc::kRoomCarrier, 4.5f, both, &s) && s.lo == -11.0f && s.hi == 19.0f, "doors: the central space between two shut doors");
            check(cc::roomSeenSlab(cc::kRoomCarrier, 4.5f, bayDoor, &s) && s.lo == -11.0f && s.hi >= cc::kRoomSlabOpen, "doors: the bridge's door open, the bridge is seen");
            check(cc::roomSeenSlab(cc::kRoomCarrier, 21.5f, both, &s) && s.lo == 19.0f && s.hi >= cc::kRoomSlabOpen, "doors: from the bridge, its shut door bounds it");
            check(cc::roomSeenSlab(cc::kRoomCarrier, 4.5f, none, &s) && cc::roomSlabWhole(s), "doors: no door shut, nothing is behind one");
            check(!cc::roomSeenSlab(cc::kRoomStation, 4.5f, both, &s) && !cc::roomSeenSlab(cc::kRoomCarrier, std::nanf(""), both, &s), "doors: no doors known, or no place, says nothing");
            // A door blocks only shut, and only with the camera past kDoorNearM from its plane.
            check(cc::doorBlocks(cc::DoorState::kShut, -17.3f, -11.0f) && !cc::doorBlocks(cc::DoorState::kShut, -14.7f, -11.0f) && !cc::doorBlocks(cc::DoorState::kShut, 21.5f, 19.0f),
                  "doors: a shut door blocks from 6 m, not from 3.7 or 2.5");
            check(!cc::doorBlocks(cc::DoorState::kOpen, -17.3f, -11.0f) && !cc::doorBlocks(cc::DoorState::kUnknown, -17.3f, -11.0f), "doors: an open door, or one not known, never blocks");
            check(cc::doorCutOf(cc::kRoomCarrier, -11.0f) == 0 && cc::doorCutOf(cc::kRoomCarrier, 19.1f) == 1 && cc::doorCutOf(cc::kRoomCarrier, 4.5f) == -1 && cc::doorCutOf(cc::kRoomStation, 0.0f) == -1,
                  "doors: a leaf belongs to the door it lies at");
            // The leaves: shut 0.8 to 1.0 m apart, open 4.4 (Flight 43).
            cc::DoorWatch dw;
            check(dw.step(false, 0.0f, 0) == cc::DoorState::kUnknown, "door watch: unseen is not known");
            check(dw.step(true, 0.94f, 100) == cc::DoorState::kUnknown && dw.rest() == 0.94f, "door watch: together, first seen, is its rest but not yet shut");
            check(dw.step(true, 0.94f, 107) == cc::DoorState::kUnknown && dw.step(true, 0.94f, 107 + cc::kDoorStillMs - 1) == cc::DoorState::kUnknown &&
                      dw.step(true, 0.94f, 107 + cc::kDoorStillMs) == cc::DoorState::kShut,
                  "door watch: shut once still at rest for the hold");
            check(dw.step(true, 0.95f, 300) == cc::DoorState::kShut, "door watch: a centimetre past rest is still shut");
            check(dw.step(true, 0.97f, 307) == cc::DoorState::kOpen, "door watch: the first step past rest by 2 cm is open, whoever opened it");
            check(dw.step(true, 4.4f, 1000) == cc::DoorState::kOpen && dw.step(true, 0.95f, 1500) == cc::DoorState::kOpen, "door watch: open, and closing back to rest stays open");
            check(dw.step(true, 0.94f, 1507) == cc::DoorState::kOpen && dw.step(true, 0.94f, 1514) == cc::DoorState::kOpen && dw.step(true, 0.94f, 1514 + cc::kDoorStillMs - 1) == cc::DoorState::kOpen &&
                      dw.step(true, 0.94f, 1514 + cc::kDoorStillMs) == cc::DoorState::kShut,
                  "door watch: shut again only after the leaves are still at rest for the hold");
            check(dw.step(false, 0.0f, 2000) == cc::DoorState::kUnknown && dw.rest() == 0.94f, "door watch: a leaf lost is not known, and the rest stays");
            check(dw.step(true, 1.2f, 2016) == cc::DoorState::kOpen, "door watch: seen again apart from its rest is open");
            check(dw.step(true, std::nanf(""), 2032) == cc::DoorState::kUnknown, "door watch: an unreadable gap is not known");
            dw.reset();
            check(dw.state() == cc::DoorState::kUnknown && dw.rest() == 0.0f, "door watch: reset forgets it and its rest");
            check(dw.step(true, 1.2f, 3000) == cc::DoorState::kOpen, "door watch: first seen half apart, no rest known, is not shut");
            dw.reset();
            check(dw.step(true, 1.25f, 4000) == cc::DoorState::kOpen && dw.step(true, 1.05f, 4007) == cc::DoorState::kOpen && dw.step(true, 0.9f, 4014) == cc::DoorState::kOpen &&
                      dw.step(true, 0.9f, 4021) == cc::DoorState::kOpen && dw.step(true, 0.9f, 4021 + cc::kDoorStillMs) == cc::DoorState::kShut && dw.rest() == 0.9f,
                  "door watch: first seen closing, the rest follows it down and it is shut once still");
            dw.reset();
            check(dw.step(true, 0.9f, 5000) == cc::DoorState::kUnknown && dw.step(true, 0.908f, 5007) == cc::DoorState::kUnknown && dw.step(true, 0.916f, 5014) == cc::DoorState::kUnknown,
                  "door watch: leaves still moving are never shut");
            check(cc::doorRuleNext(cc::DoorRuleMode::kRecords) == cc::DoorRuleMode::kRigs && cc::doorRuleNext(cc::DoorRuleMode::kRigs) == cc::DoorRuleMode::kOff &&
                      cc::doorRuleNext(cc::DoorRuleMode::kOff) == cc::DoorRuleMode::kRecords, "doors: the key steps records, rigs, off");
            check(std::strcmp(cc::kDoorLeafLeft, "Narrow_Door_Left") == 0 && cc::concourseAllowName(cc::kDoorLeafLeft, nullptr) && cc::concourseAllowName(cc::kDoorLeafRight, nullptr),
                  "doors: the leaves' names are ones the store sees as allowed");
            // The real sky (2026-11-08): a carrier's windows show it; the large stations, ports and outposts keep their bake.
            check(cc::kRoomCarrier.realSky && !cc::kRoomStation.realSky && !cc::kRoomPort.realSky && !cc::kRoomOutpost.realSky, "real sky: a carrier's room only");
            auto skySideOf = [](const char* name, int want) {
                for (size_t i = 0; i < cc::kWithheldPsCount; ++i)
                    if (std::strcmp(cc::kWithheldPs[i].name, name) == 0) return static_cast<int>(cc::kWithheldPs[i].skySide) == want;
                for (size_t i = 0; i < cc::kWithheldVsCount; ++i)
                    if (std::strcmp(cc::kWithheldVs[i].name, name) == 0) return static_cast<int>(cc::kWithheldVs[i].skySide) == want;
                return false;
            };
            bool skyOk = true;
            for (const char* n : {"atmosphere", "atmosphere quad A", "atmosphere quad B", "planet surface", "body surface", "planetary pipeline", "planetary 2", "planetary 3",
                                  "planet surface vs", "terrain patch", "body sphere", "beside the bodies", "sun", "sun arcs"})
                skyOk = skyOk && skySideOf(n, 1);
            for (const char* n : {"plume", "plume twin", "trail ribbon", "beams and bolts", "multicannon rounds", "shield strike", "smoke", "flares", "scatter", "particles A", "ship ribbon",
                                  "ship engine glow", "light cones"})
                skyOk = skyOk && skySideOf(n, 0);
            check(skyOk, "real sky: the planet, its atmosphere and the sun are the sky's; ships, weapons fire and the room's washes are not");
        }
        for (const char* n : {"SA_Carrier_01", "SA_Carrier_01_Bar", "SA_Carrier_01_Bridge", "SA_Carrier_01_Bar_WindowShutters0", "SA_Carrier_DoorA0", "PS_FUI_STAT_BigScreen_Carrier_01",
                              "FUI_Panel_4x2_Carrier_010", "RandomDirt_NarrowRightA0", "Narrow_Door_Left_Tech", "Arm_Base0"})
            check(cc::concourseAllowName(n, nullptr), "concourse carrier: the room's own name is on the list");
        for (const char* n : {"MS_EngineBlock07", "MS_Command_Tower4", "MS_CoreHub1", "MS_CarrierDockB_Shell", "HangerLarge", "LandingPadLargeA", "CargoLayout08_03", "POI_Wreck_Independant_Fighter"})
            check(!cc::concourseAllowName(n, nullptr), "concourse carrier: what stands outside is not on the list");
        // The anchor means the room: a station's is gone from the store once the lift cuts away, a port's is seen from the hangar too (503 m, Flight 36).
        check(cc::anchorMeansRoomHere(cc::kRoomStation, false, false, 0.0f) && cc::anchorMeansRoomHere(cc::kRoomStation, true, true, 1090.0f), "concourse layouts: at a station the sighting is enough, as it was");
        check(cc::anchorMeansRoomHere(cc::kRoomPort, true, true, 3.5f) && cc::anchorMeansRoomHere(cc::kRoomPort, true, true, 22.9f), "concourse layouts: at a port, in the middle of the room and at the bay");
        check(cc::kRoomPort.roomReachM > 42.1f && !cc::anchorMeansRoomHere(cc::kRoomPort, true, true, 503.0f), "concourse layouts: at a port, the reach is past the hub's radius and short of the hangar");
        check(!cc::anchorMeansRoomHere(cc::kRoomPort, false, true, 3.5f) && !cc::anchorMeansRoomHere(cc::kRoomPort, true, false, 3.5f), "concourse layouts: at a port, no camera or no anchor read is not the room");
        check(!cc::anchorMeansRoomHere(cc::kRoomPort, true, true, std::numeric_limits<float>::quiet_NaN()), "concourse layouts: at a port, a distance that is not a number is not the room");
        check(cc::judgeKeeps(cc::concourseJudge(true, true, true, 3.5f, 42.1f, true)), "concourse surface port: its anchor is kept");
        check(cc::judgeKeeps(cc::concourseJudge(true, true, true, 64.9f, 102.2f, false)), "concourse surface port: its exterior shell is kept");
        check(!cc::judgeKeeps(cc::concourseJudge(true, false, true, 126.0f, 9.3f, false)), "concourse surface port: the nearest lamp post outside is culled, past the close distance");
        check(cc::concourseAllowName("Spoke03", "Spoke,Ring") && cc::concourseAllowName("RingRoad07", "Spoke,Ring"), "concourse extra prefixes: Spoke and Ring are kept");
        check(cc::concourseAllowName("Spoke03", " Wheel , Spoke ") && cc::concourseAllowName("WheelSection00", " Wheel , Spoke "),
              "concourse extra prefixes: spaces around an entry are ignored");
        check(!cc::concourseAllowName("Anaconda", "Spoke,Ring") && !cc::concourseAllowName("HabitatRingA", "Spoke,Ring"),
              "concourse extra prefixes: another name, and a prefix found inside a name, stay culled");
        check(!cc::concourseAllowName("Anaconda", "") && !cc::concourseAllowName("Anaconda", ",,  ,") && !cc::concourseAllowName("Anaconda", nullptr),
              "concourse extra prefixes: an empty list or empty entries match nothing");
        check(cc::concourseAllowName("SA_Station_01_Bar", "Spoke") && !cc::concourseAllowName("spoke03", "Spoke"), "concourse extra prefixes: the built-in list stays, and the extras are exact case");

        // The gate: Status.json Flags2 0x00014009 is on foot, in station, social space.
        check(cc::concourseSocialOnFoot(0x00014009u, true, true), "concourse gate: Flags2 0x00014009 with gameplay is on");
        check(!cc::concourseSocialOnFoot(0x00014009u, true, false), "concourse gate: without gameplay it is off (a leftover on-foot bit must not cull the main menu's hangar)");
        check(!cc::concourseSocialOnFoot(0x00014009u, false, true), "concourse gate: with the on-foot word unknown it is off");
        check(!cc::concourseSocialOnFoot(0x00002009u, true, true), "concourse gate: the hangar bits (bit 13) without bit 14 are off");
        check(!cc::concourseSocialOnFoot(0u, true, true), "concourse gate: Flags2 0 is off");
        check(!cc::concourseSocialOnFoot(0x00004000u, true, true) && !cc::concourseSocialOnFoot(0x00000001u, true, true), "concourse gate: each of bit 0 and bit 14 alone is off");

        // The judgement.
        check(cc::concourseJudge(true, false, true, 5.0f, 300.0f, false) == cc::Judge::kCull, "concourse judge: the ring (WheelSection00, 5 m away, radius 300 m) is culled");
        check(cc::concourseJudge(true, false, true, 1.0f, 1.0f, false) == cc::Judge::kKeepSmallClose, "concourse judge: Barrel at 1 m with radius 1 m is kept (small and close)");
        check(cc::concourseJudge(true, true, true, 150.0f, 3.0f, false) == cc::Judge::kKeepName, "concourse judge: an allowed name 150 m away is kept");
        check(cc::concourseJudge(true, true, true, 500.0f, 3.0f, false) == cc::Judge::kCull, "concourse judge: an allowed name 500 m away (radius 3 m) is culled, it is past the far limit");
        check(cc::concourseJudge(true, true, true, 203.0f, 3.0f, false) == cc::Judge::kKeepName && cc::concourseJudge(true, true, true, 203.01f, 3.0f, false) == cc::Judge::kCull,
              "concourse judge: the far rule is distance minus radius over 200 m, the edge itself is kept");
        check(cc::concourseJudge(true, true, true, 400.0f, 90.0f, false) == cc::Judge::kCull && cc::concourseJudge(true, true, true, 280.0f, 90.0f, false) == cc::Judge::kKeepName,
              "concourse judge: a big far structure (PlantStack, 400 m, radius 90 m) is culled; the same size at 280 m is kept");
        check(cc::concourseJudge(true, true, true, 500.0f, 3.0f, true) == cc::Judge::kKeepName && cc::concourseJudge(true, true, true, 1190.0f, 50.0f, true) == cc::Judge::kKeepName,
              "concourse judge: the anchor and the hub (far-exempt) are kept at any distance");
        check(cc::concourseJudge(true, true, false, 900.0f, 3.0f, false) == cc::Judge::kKeepName && cc::concourseJudge(true, true, true, 900.0f, 0.0f, false) == cc::Judge::kKeepName &&
                  cc::concourseJudge(true, true, true, std::nanf(""), 3.0f, false) == cc::Judge::kKeepName,
              "concourse judge: the far rule needs a readable sphere");
        check(cc::kAllowedFarM == 200.0f, "concourse judge: the far limit is 200 m");
        check(cc::concourseJudge(false, false, true, 5.0f, 300.0f, false) == cc::Judge::kKeepUnreadableName, "concourse judge: an unreadable name is kept");
        check(cc::concourseJudge(false, false, true, 500.0f, 30.0f, false) == cc::Judge::kCull, "concourse judge: an unreadable name wholly past the far limit is culled");
        check(cc::concourseJudge(false, false, true, 205.0f, 30.0f, false) == cc::Judge::kKeepUnreadableName, "concourse judge: an unreadable name reaching inside the far limit is kept");
        check(cc::concourseJudge(false, false, false, 500.0f, 30.0f, false) == cc::Judge::kKeepUnreadableName, "concourse judge: an unreadable name with no readable sphere is kept");
        check(cc::concourseJudge(true, false, false, 0.0f, 0.0f, false) == cc::Judge::kKeepUnreadableSphere, "concourse judge: an unreadable sphere is kept");
        check(cc::concourseJudge(true, false, true, 300.0f, 10.0f, false) == cc::Judge::kCull, "concourse judge: Spoke03 at 300 m with radius 10 m is culled");
        check(cc::concourseJudge(true, false, true, 40.0f, 25.0f, false) == cc::Judge::kKeepSmallClose && cc::concourseJudge(true, false, true, 40.01f, 25.0f, false) == cc::Judge::kCull &&
                  cc::concourseJudge(true, false, true, 40.0f, 25.01f, false) == cc::Judge::kCull,
              "concourse judge: small and close is radius at most 25 m and distance at most 40 m, both edges inclusive");
        check(cc::concourseJudge(true, false, true, std::nanf(""), 10.0f, false) == cc::Judge::kKeepUnreadableSphere &&
                  cc::concourseJudge(true, false, true, 10.0f, std::nanf(""), false) == cc::Judge::kKeepUnreadableSphere &&
                  cc::concourseJudge(true, false, true, 10.0f, 0.0f, false) == cc::Judge::kKeepUnreadableSphere,
              "concourse judge: a sphere that is not finite, or has no radius, is unreadable and kept");
        check(cc::concourseJudge(true, true, false, 0.0f, 0.0f, false) == cc::Judge::kKeepName && cc::concourseJudge(false, true, true, 5.0f, 300.0f, false) == cc::Judge::kKeepUnreadableName,
              "concourse judge: the name rules come first (an allowed name with no sphere is kept for its name; an unreadable name wins over an allowed flag)");
        check(cc::judgeKeeps(cc::Judge::kKeepName) && cc::judgeKeeps(cc::Judge::kKeepUnreadableName) && cc::judgeKeeps(cc::Judge::kKeepUnreadableSphere) &&
                  cc::judgeKeeps(cc::Judge::kKeepSmallClose) && !cc::judgeKeeps(cc::Judge::kCull),
              "concourse judge: only a cull is not a keep");

        // The rig verdict: vetoed only if a record was judged and none was kept.
        check(!cc::concourseRigVetoed(0, 0), "concourse rig: a rig with nothing judged forwards");
        check(cc::concourseRigVetoed(5, 0), "concourse rig: five records judged and none kept is vetoed");
        check(!cc::concourseRigVetoed(5, 1) && !cc::concourseRigVetoed(5, 5), "concourse rig: one kept record, or all kept, forwards");
        cc::RigTally t;
        check(!cc::concourseRigVetoed(t), "concourse rig: an empty tally forwards");
        t.note(false);
        t.note(false);
        check(t.judged == 2 && t.kept == 0 && cc::concourseRigVetoed(t), "concourse rig: two culled records veto the rig");
        t.note(true);
        check(t.judged == 3 && t.kept == 1 && !cc::concourseRigVetoed(t), "concourse rig: a kept record among them lifts the veto");

        // The node cache carries the two flags beside the older three, and the key keeps its meaning.
        {
            const uintptr_t node = 0x7FF612340ull * 8;
            const uint32_t key = se::classKey(300);
            const uint32_t all = se::kNodeMechanism | se::kNodeHangarLike | se::kNodeShipPrefix | se::kNodeConcourseAllow | se::kNodeConcourseAnchor;
            const uint64_t v = se::packSlot(node, se::NameClass::kOther, all, key);
            se::NameClass cls = se::NameClass::kNone;
            uint32_t flags = 0;
            check(se::unpackSlot(v, node, key, &cls, &flags) && cls == se::NameClass::kOther && flags == all, "concourse node cache: all five flags and the class survive a pack and unpack");
            check(!se::unpackSlot(v, node, se::classKey(301), &cls, &flags), "concourse node cache: another generation is still a miss");
            check(se::kNodeConcourseAllow != se::kNodeMechanism && se::kNodeConcourseAllow != se::kNodeHangarLike && se::kNodeConcourseAllow != se::kNodeShipPrefix &&
                      se::kNodeConcourseAnchor != se::kNodeConcourseAllow && (se::kNodeConcourseAnchor & se::kSlotFlagMask) != 0,
                  "concourse node cache: the two flags are free bits inside the slot's flag field");
        }
        // The store's mode: the concourse mode never outranks the setting or the seal, and ranks above plain gathering.
        check(se::storeMode(false, false, false, true) == se::StoreMode::kNone, "concourse store mode: the setting off is none, whatever the concourse says");
        check(se::storeMode(true, true, false, true) == se::StoreMode::kJudge, "concourse store mode: sealed is still the seal's judge");
        check(se::storeMode(true, false, false, true) == se::StoreMode::kConcourse && se::storeMode(true, false, true, true) == se::StoreMode::kConcourse,
              "concourse store mode: wanted and unsealed is the concourse mode, docked or not");
        check(se::storeMode(true, false, true) == se::StoreMode::kGather && se::storeMode(true, false, false) == se::StoreMode::kNone && se::storeMode(true, true, true) == se::StoreMode::kJudge,
              "concourse store mode: with the concourse not wanted the old modes are unchanged");

        // The pane triples.
        const cc::PaneTriple list[3] = {{0x1111111111111111ull, 0x2222222222222222ull, 12}, {0xAAAAAAAAAAAAAAAAull, 0xBBBBBBBBBBBBBBBBull, 336}, {0xAAAAAAAAAAAAAAAAull, 0xBBBBBBBBBBBBBBBBull, 804}};
        check(cc::paneTripleMatch(list, 3, 0xAAAAAAAAAAAAAAAAull, 0xBBBBBBBBBBBBBBBBull, 804) && cc::paneTripleMatch(list, 3, 0x1111111111111111ull, 0x2222222222222222ull, 12),
              "concourse pane: a listed triple matches");
        check(!cc::paneTripleMatch(list, 3, 0xAAAAAAAAAAAAAAAAull, 0xBBBBBBBBBBBBBBBBull, 337) && !cc::paneTripleMatch(list, 3, 0xAAAAAAAAAAAAAAAAull, 0xBBBBBBBBBBBBBBBCull, 804) &&
                  !cc::paneTripleMatch(list, 3, 0xAAAAAAAAAAAAAAABull, 0xBBBBBBBBBBBBBBBBull, 804),
              "concourse pane: a different index count, pixel shader or vertex shader does not match");
        check(!cc::paneTripleMatch(list, 0, 0x1111111111111111ull, 0x2222222222222222ull, 12) && !cc::paneTripleMatch(list, 1, 0xAAAAAAAAAAAAAAAAull, 0xBBBBBBBBBBBBBBBBull, 336),
              "concourse pane: an empty list, and entries past the count, match nothing");
        check(cc::paneTripleIndex(list, 3, 0xAAAAAAAAAAAAAAAAull, 0xBBBBBBBBBBBBBBBBull, 804) == 2 && cc::paneTripleIndex(list, 3, 1, 2, 3) == -1, "concourse pane: the slot is the triple's place in the list");

        cc::PaneTriple parsed[cc::kPaneListCap];
        size_t bad = 99;
        size_t n = cc::parsePaneList("0123456789ABCDEF/FEDCBA9876543210/12,aaaaaaaaaaaaaaaa/bbbbbbbbbbbbbbbb/6", parsed, cc::kPaneListCap, &bad);
        check(n == 2 && bad == 0 && parsed[0].vs == 0x0123456789ABCDEFull && parsed[0].ps == 0xFEDCBA9876543210ull && parsed[0].n == 12 && parsed[1].vs == 0xAAAAAAAAAAAAAAAAull &&
                  parsed[1].ps == 0xBBBBBBBBBBBBBBBBull && parsed[1].n == 6,
              "concourse pane list: two good entries parse, hashes as 16 hex digits in either case");
        n = cc::parsePaneList(" 0123456789ABCDEF/FEDCBA9876543210/12 , nonsense,0123456789ABCDEF/FEDCBA9876543210/0,123/456/7,0123456789ABCDEF/FEDCBA9876543210/12x,"
                              "0123456789ABCDEG/FEDCBA9876543210/5,0123456789ABCDEF/FEDCBA9876543210/4294967296,,AAAAAAAAAAAAAAAA/BBBBBBBBBBBBBBBB/336 ,",
                              parsed, cc::kPaneListCap, &bad);
        check(n == 2 && bad == 6 && parsed[0].n == 12 && parsed[1].n == 336, "concourse pane list: a bad entry (not a triple, a zero or oversize count, a short hash, a non-hex digit, trailing text) is counted and skipped, and the good ones around it stay");
        n = cc::parsePaneList("0123456789ABCDEF/FEDCBA9876543210/12,0123456789abcdef/fedcba9876543210/12", parsed, cc::kPaneListCap, &bad);
        check(n == 1 && bad == 0, "concourse pane list: a duplicate entry is dropped");
        n = cc::parsePaneList("", parsed, cc::kPaneListCap, &bad);
        check(n == 0 && bad == 0, "concourse pane list: an empty string is an empty list");
        n = cc::parsePaneList(nullptr, parsed, cc::kPaneListCap, &bad);
        check(n == 0 && bad == 0, "concourse pane list: a null string is an empty list");
        {
            std::string many;
            for (int i = 0; i < 20; ++i) {
                char e[64];
                std::snprintf(e, sizeof(e), "%s%016X/%016X/%d", i ? "," : "", 0x100 + i, 0x200 + i, 7 + i);
                many += e;
            }
            n = cc::parsePaneList(many.c_str(), parsed, cc::kPaneListCap, &bad);
            check(n == cc::kPaneListCap && parsed[15].n == 22 && bad == 0, "concourse pane list: the list stops at its cap of 16 entries");
        }
        check(cc::kPaneColourCount == 8 && std::strcmp(cc::kPaneColourNames[0], "red") == 0 && std::strcmp(cc::kPaneColourNames[7], "white") == 0 &&
                  cc::kPaneColours[0][0] == 1.0f && cc::kPaneColours[0][1] == 0.0f && cc::kPaneColours[4][0] == 1.0f && cc::kPaneColours[4][2] == 1.0f,
              "concourse pane: eight fixed colours per slot, red first, white last, magenta fifth");
        check(cc::concourseAllowName("TurboLift", nullptr) && cc::concourseAllowName("TurboLift_Shaft_02", nullptr) && !cc::concourseAllowName("turbolift", nullptr) &&
                  !cc::concourseAllowName("XTurboLift", nullptr),
              "concourse allow-list: the TurboLift prefix is pinned (the doors popped in at 40 m without it), exact case, at the start of the name");

        // The family: an entry with n = 0 matches any index count, one with a count matches only it.
        {
            const cc::PaneTriple fam[2] = {{cc::kPaneFamilyVs, cc::kPaneFamilyPs, 0}, {0x1111111111111111ull, 0x2222222222222222ull, 36}};
            check(cc::paneTripleMatch(fam, 2, cc::kPaneFamilyVs, cc::kPaneFamilyPs, 36) && cc::paneTripleMatch(fam, 2, cc::kPaneFamilyVs, cc::kPaneFamilyPs, 192) &&
                      cc::paneTripleMatch(fam, 2, cc::kPaneFamilyVs, cc::kPaneFamilyPs, 864) && cc::paneTripleMatch(fam, 2, cc::kPaneFamilyVs, cc::kPaneFamilyPs, 1),
                  "concourse pane: an entry with n = 0 matches the family at any index count");
            check(cc::paneTripleMatch(fam, 2, 0x1111111111111111ull, 0x2222222222222222ull, 36) && !cc::paneTripleMatch(fam, 2, 0x1111111111111111ull, 0x2222222222222222ull, 37),
                  "concourse pane: an entry with a count still matches only that count");
            check(cc::paneTripleIndex(fam, 2, cc::kPaneFamilyVs, cc::kPaneFamilyPs, 192) == 0 && cc::paneTripleIndex(fam, 2, cc::kPaneFamilyVs, 0x2222222222222222ull, 36) == -1,
                  "concourse pane: the family's slot is its place in the list, and a vertex shader with another pixel shader is not it");
            check(cc::kPaneFamilyVs == 0x98397963AAEC45D3ull && cc::kPaneFamilyPs == 0x66B08F89E4926C01ull && cc::kBayIndexCount == 36 && cc::kPlumePs == 0x3B0B38CD96F53BC1ull &&
                      cc::kTwinPs == 0x34A1475B2C790DFAull,
                  "concourse pane: the family, the bay count and the withheld pixel shaders are flight 1's");
            cc::PaneTriple pe;
            check(cc::parsePaneEntry("98397963AAEC45D3/66B08F89E4926C01", 33, &pe) && pe.vs == cc::kPaneFamilyVs && pe.ps == cc::kPaneFamilyPs && pe.n == 0,
                  "concourse pane list: VS/PS is accepted and means any index count");
            const char* withCount = "98397963AAEC45D3/66B08F89E4926C01/36";
            check(cc::parsePaneEntry(withCount, std::strlen(withCount), &pe) && pe.n == 36, "concourse pane list: VS/PS/N still reads its count");
            const char* trailing = "98397963AAEC45D3/66B08F89E4926C01/";
            check(!cc::parsePaneEntry(trailing, std::strlen(trailing), &pe) && !cc::parsePaneEntry("98397963AAEC45D3/66B08F89E4926C0", 32, &pe) &&
                      !cc::parsePaneEntry("98397963AAEC45D3/66B08F89E4926C01x", 34, &pe),
                  "concourse pane list: a trailing slash, a short hash or a trailing letter after VS/PS is refused");
            cc::PaneTriple two[cc::kPaneListCap];
            size_t bad2 = 9;
            const size_t n2 = cc::parsePaneList("98397963AAEC45D3/66B08F89E4926C01,98397963AAEC45D3/66B08F89E4926C01/36,98397963AAEC45D3/66B08F89E4926C01", two, cc::kPaneListCap, &bad2);
            check(n2 == 2 && bad2 == 0 && two[0].n == 0 && two[1].n == 36, "concourse pane list: VS/PS and VS/PS/36 are different entries, a repeat of one is dropped");
        }

        // The mirror textures: uniform, and the values the game's glass shader decodes.
        {
            check(cc::kPaneTexSize == 4, "concourse mirror textures: 4 texels square");
            static uint8_t t8[cc::kPaneTexSize * cc::kPaneTexSize * 2];
            static uint8_t t9[cc::kPaneTexSize * cc::kPaneTexSize * 2];
            static uint8_t t10[cc::kPaneTexSize * cc::kPaneTexSize * 4];
            cc::fillPaneT8(t8);
            cc::fillPaneT9(t9);
            cc::fillPaneT10(t10);
            bool metal = true;
            for (size_t i = 0; i < sizeof(t8); ++i) metal = metal && t8[i] == 255;
            check(metal, "concourse mirror textures: t8 is 255 in both channels (metal 255)");
            bool flat = true;
            for (size_t i = 0; i < sizeof(t9); ++i) flat = flat && t9[i] == 127;
            check(flat, "concourse mirror textures: t9 is 127, 127 everywhere (flat, no seam, no noise)");
            const double nx = (127 / 255.0) * 2.007874 - 0.999985;
            check(std::fabs(nx) < 0.002, "concourse mirror textures: 127 decodes to a normal xy of about zero");
            bool uniform = true;
            for (uint32_t i = 1; i < cc::kPaneTexSize * cc::kPaneTexSize; ++i)
                for (int c = 0; c < 4; ++c) uniform = uniform && t10[4 * i + c] == t10[c];
            check(uniform, "concourse mirror textures: t10 is one colour and one roughness (smooth)");
            check(t10[0] == 184 && t10[1] == 191 && t10[2] == 199 && t10[3] == 77, "concourse mirror textures: t10 is (184, 191, 199) with roughness byte 77 (0.30)");
        }

        // The mark depth: infinite reversed Z, near plane 0.025 m.
        {
            check(std::fabs(cc::markDepthForDistance(100.0f) - 0.00025f) < 1e-9f && std::fabs(cc::markDepthForDistance(25.0f) - 0.001f) < 1e-9f,
                  "concourse mark: the depth for 100 m is 0.00025 and for 25 m 0.001 (0.025 / metres)");
            check(cc::markDepthForDistance(0.1f) == 0.025f && cc::markDepthForDistance(-5.0f) == 0.025f && cc::markDepthForDistance(1.0f) == 0.025f &&
                      cc::markDepthForDistance(std::nanf("")) == 0.025f,
                  "concourse mark: metres under 1 (or not a number) read as 1");
            check(cc::kMirrorBeyondDefaultM == 100.0f && cc::markDepthForDistance(cc::kMirrorBeyondDefaultM) < cc::markDepthForDistance(50.0f),
                  "concourse mark: the default is 100 m, and farther means a smaller depth");
        }

        // Each pane side.
        {
            cc::PaneSides sides;
            const cc::PaneKey a{12, 0, 0}, b{12, 36, 5}, c{36, 0, -7};
            check(sides.side(a) == cc::PaneSide::kUnknown && sides.knownCount() == 0 && sides.exteriorCount() == 0 && sides.dropped() == 0, "concourse pane sides: a new table knows nothing");
            sides.note(a, 63, 63);
            check(sides.side(a) == cc::PaneSide::kUnknown && sides.knownCount() == 0, "concourse pane sides: fewer than 64 drawn samples change nothing");
            sides.note(a, 64, 38);   // 38 / 64 = 0.594
            check(sides.side(a) == cc::PaneSide::kInterior && sides.exteriorCount() == 0, "concourse pane sides: 38 of 64 is under the 0.6 ratio: interior");
            sides.note(a, 64, 39);   // 39 / 64 = 0.609
            check(sides.side(a) == cc::PaneSide::kExterior && sides.exteriorCount() == 1, "concourse pane sides: 39 of 64 reaches the ratio: exterior, and the latest reading wins");
            sides.note(a, 100, 60);
            check(sides.side(a) == cc::PaneSide::kExterior, "concourse pane sides: exactly 0.6 is exterior");
            sides.note(a, 1000, 10);
            check(sides.side(a) == cc::PaneSide::kInterior && sides.exteriorCount() == 0 && sides.knownCount() == 1, "concourse pane sides: a later low reading turns it back to interior");
            sides.note(a, 10, 10);
            check(sides.side(a) == cc::PaneSide::kInterior, "concourse pane sides: a reading under the minimum leaves the side as it was");
            sides.note(b, 200, 200);
            sides.note(c, 200, 0);
            check(sides.side(b) == cc::PaneSide::kExterior && sides.side(c) == cc::PaneSide::kInterior && sides.side(cc::PaneKey{12, 36, 6}) == cc::PaneSide::kUnknown &&
                      sides.knownCount() == 3 && sides.exteriorCount() == 1,
                  "concourse pane sides: the key is the count, the first index and the base vertex together");
            const cc::PaneKey e{99, 1, 1};
            const auto c1 = sides.note(e, 200, 200);
            const auto c2 = sides.note(e, 200, 200);
            const auto c3 = sides.note(e, 200, 0);
            check(c1 == cc::PaneSides::Change::kFirst && c2 == cc::PaneSides::Change::kNone && c3 == cc::PaneSides::Change::kChanged,
                  "concourse pane sides: note says first sight, no change and changed side");
            sides.reset();
            check(sides.side(b) == cc::PaneSide::kUnknown && sides.knownCount() == 0 && sides.exteriorCount() == 0 && sides.dropped() == 0, "concourse pane sides: reset forgets everything");
            for (uint32_t i = 0; i < cc::kPaneSideCap; ++i) sides.note(cc::PaneKey{i + 1, 0, 0}, 100, 100);
            check(sides.knownCount() == cc::kPaneSideCap && sides.exteriorCount() == cc::kPaneSideCap && sides.dropped() == 0, "concourse pane sides: 256 keys fit");
            sides.note(cc::PaneKey{9999, 0, 0}, 100, 100);
            sides.note(cc::PaneKey{9999, 0, 0}, 100, 100);
            check(sides.side(cc::PaneKey{9999, 0, 0}) == cc::PaneSide::kUnknown && sides.dropped() == 2 && sides.knownCount() == cc::kPaneSideCap,
                  "concourse pane sides: a full table reports unknown for a new key and counts the drops");
            sides.note(cc::PaneKey{1, 0, 0}, 100, 0);
            check(sides.side(cc::PaneKey{1, 0, 0}) == cc::PaneSide::kInterior && sides.exteriorCount() == cc::kPaneSideCap - 1, "concourse pane sides: a full table still updates a known key");
        }

        // Which frame a pane is tested: one in eight per key.
        {
            bool once = true;
            bool spread = false;
            const cc::PaneKey keys[] = {{12, 0, 0}, {36, 0, 0}, {12, 36, 5}, {192, 7, -3}, {864, 100, 0}};
            for (const cc::PaneKey& k : keys)
                for (uint32_t start = 0; start < 40; ++start) {
                    uint32_t hits = 0;
                    for (uint32_t f = start; f < start + 8; ++f) hits += cc::paneTestThisFrame(k, f) ? 1u : 0u;
                    if (hits != 1) once = false;
                }
            for (uint32_t f = 0; f < 8; ++f)
                if (cc::paneTestThisFrame(keys[0], f) != cc::paneTestThisFrame(keys[1], f) || cc::paneTestThisFrame(keys[0], f) != cc::paneTestThisFrame(keys[2], f)) spread = true;
            check(once, "concourse pane test frame: a key is tested exactly once in any 8 consecutive frames");
            check(spread, "concourse pane test frame: the keys are spread over the 8 frames, not all on one");
        }

        // The clearing machine: it follows the total share of the view the outside glass fills, not the camera.
        {
            using CP = cc::ClearPhase;
            cc::ClearInput in;
            in.gate = true;
            in.enterShare = 0.20f;
            uint32_t seq = 0;
            auto stepAt = [&](cc::ClearMachine& m, uint64_t now, float share) {
                in.nowMs = now;
                in.windowShare = share;
                in.shareSeq = ++seq;
                return m.step(in);
            };
            // Drives a fresh machine to the start of Clear at the returned time (share 0.30 throughout).
            auto toClear = [&](cc::ClearMachine& m, uint64_t t0) {
                stepAt(m, t0, 0.30f);
                stepAt(m, t0 + 16, 0.30f);                                         // lifting
                stepAt(m, t0 + 16 + 150, 0.30f);                                   // fading out
                const cc::ClearOutput o = stepAt(m, t0 + 16 + 150 + 600, 0.30f);   // clear
                return o.phase == CP::kClear ? t0 + 16 + 150 + 600 : uint64_t(0);
            };
            cc::ClearMachine m;
            cc::ClearOutput o = stepAt(m, 1000, 0.05f);
            check(o.cullOn && o.bayOpacity == 1.0f && o.phase == CP::kMirror, "concourse clear: below the share, the mirror phase holds, the cull on, opacity 1");
            o = stepAt(m, 1016, 0.30f);
            check(o.phase == CP::kMirror && o.cullOn, "concourse clear: one reading over the share is not enough");
            in.shareSeq = seq;   // the same reading seen again on the next tick
            o = m.step(in);
            check(o.phase == CP::kMirror, "concourse clear: the same reading seen on a second tick is one reading, not two");
            stepAt(m, 1032, 0.10f);
            o = stepAt(m, 1048, 0.30f);
            check(o.phase == CP::kMirror, "concourse clear: a reading under the share between two resets the count");
            o = stepAt(m, 1064, 0.30f);
            check(o.phase == CP::kLifting && !o.cullOn && o.bayOpacity == 1.0f, "concourse clear: two readings in a row, no camera needed: lifting, the cull off, the glass still opaque");
            const uint64_t liftAt = 1064;
            o = stepAt(m, liftAt + 149, 0.30f);
            check(o.phase == CP::kLifting && o.bayOpacity == 1.0f && !o.cullOn, "concourse clear: lifting lasts 150 ms with the glass opaque");
            o = stepAt(m, liftAt + 150, 0.30f);
            check(o.phase == CP::kFadingOut && !o.cullOn && o.bayOpacity == 1.0f, "concourse clear: after 150 ms the glass begins to fade out");
            const uint64_t fadeAt = liftAt + 150;
            o = stepAt(m, fadeAt + 300, 0.30f);
            check(o.phase == CP::kFadingOut && std::fabs(o.bayOpacity - 0.5f) < 0.001f, "concourse clear: opacity falls linearly, half at 300 ms");
            o = stepAt(m, fadeAt + 600, 0.30f);
            check(o.phase == CP::kClear && o.bayOpacity == 0.0f && !o.cullOn, "concourse clear: at 600 ms the glass is clear");
            // Hysteresis: between the exit share (8%) and the enter share (20%) it stays clear, however long.
            bool stays = true;
            for (uint64_t t = fadeAt + 700; t < fadeAt + 3000; t += 100) stays = stays && stepAt(m, t, 0.12f).phase == CP::kClear;
            check(stays, "concourse clear: between the exit share and the enter share it stays clear (hysteresis)");
            // A brief dip under the exit share (under 250 ms) stays clear, and coming back over it cancels the hold.
            const uint64_t d0 = fadeAt + 4000;
            stepAt(m, d0, 0.05f);
            o = stepAt(m, d0 + 249, 0.05f);
            check(o.phase == CP::kClear, "concourse clear: a dip under 250 ms stays clear");
            stepAt(m, d0 + 260, 0.12f);
            stepAt(m, d0 + 270, 0.05f);
            o = stepAt(m, d0 + 270 + 249, 0.05f);
            check(o.phase == CP::kClear, "concourse clear: a reading back over the exit share restarts the hold");
            // A sustained dip with the bay still on screen: it fades back in, the cull still off.
            const uint64_t inAt = d0 + 270 + 250;
            o = stepAt(m, inAt, 0.05f);
            check(o.phase == CP::kFadingIn && !o.cullOn && o.bayOpacity == 0.0f, "concourse clear: a dip held for 250 ms with the bay still visible fades the glass back in, the cull still off");
            o = stepAt(m, inAt + 300, 0.05f);
            check(o.phase == CP::kFadingIn && std::fabs(o.bayOpacity - 0.5f) < 0.001f && !o.cullOn, "concourse clear: opacity rises linearly, half at 300 ms");
            // Coming back during the fade turns it round from the current opacity.
            o = stepAt(m, inAt + 360, 0.35f);
            check(o.phase == CP::kFadingOut && std::fabs(o.bayOpacity - 0.6f) < 0.001f, "concourse clear: the share over the enter share while fading in turns it round from the current opacity");
            o = stepAt(m, inAt + 360 + 120, 0.35f);
            check(o.phase == CP::kFadingOut && std::fabs(o.bayOpacity - 0.4f) < 0.001f, "concourse clear: fading out again at the same rate (0.2 in 120 ms)");
            o = stepAt(m, inAt + 360 + 600, 0.35f);
            check(o.phase == CP::kClear && o.bayOpacity == 0.0f, "concourse clear: it reaches clear again");
            // The fade in runs to the end: the mirror phase returns with the cull on.
            const uint64_t e0 = inAt + 2000;
            stepAt(m, e0, 0.05f);
            o = stepAt(m, e0 + 250, 0.05f);
            check(o.phase == CP::kFadingIn, "concourse clear: (setup) fading in again");
            o = stepAt(m, e0 + 250 + 599, 0.05f);
            check(o.phase == CP::kFadingIn && o.bayOpacity < 1.0f, "concourse clear: not yet opaque at 599 ms");
            o = stepAt(m, e0 + 250 + 600, 0.05f);
            check(o.phase == CP::kMirror && o.cullOn && o.bayOpacity == 1.0f, "concourse clear: at opacity 1 the mirror phase returns with the cull on");
            // The bay out of view during the fade in: straight to the mirror phase.
            {
                cc::ClearMachine f;
                uint64_t t = toClear(f, 20000);
                check(t != 0, "concourse clear: (setup) clear");
                stepAt(f, t + 100, 0.05f);
                o = stepAt(f, t + 350, 0.05f);
                check(o.phase == CP::kFadingIn, "concourse clear: (setup) fading in with the bay visible");
                o = stepAt(f, t + 400, 0.005f);
                check(o.phase == CP::kMirror && o.cullOn && o.bayOpacity == 1.0f, "concourse clear: the bay going out of view during the fade in jumps to the mirror phase");
            }
            // A sustained dip with the bay out of view: straight to the mirror phase, no fade.
            {
                cc::ClearMachine f;
                uint64_t t = toClear(f, 30000);
                stepAt(f, t + 100, 0.0f);
                o = stepAt(f, t + 349, 0.0f);
                check(o.phase == CP::kClear, "concourse clear: (setup) still clear at 249 ms");
                o = stepAt(f, t + 350, 0.0f);
                check(o.phase == CP::kMirror && o.cullOn && o.bayOpacity == 1.0f, "concourse clear: a dip held for 250 ms with the bay out of view goes straight to the mirror phase, the cull on");
                o = stepAt(f, t + 400, 0.30f);
                check(o.phase == CP::kMirror, "concourse clear: (setup) hits were cleared on the way back, one reading is not enough");
            }
            // A negative share (no reading, no family draw on screen) counts as below.
            {
                cc::ClearMachine f;
                uint64_t t = toClear(f, 40000);
                stepAt(f, t + 100, -1.0f);
                o = stepAt(f, t + 350, -1.0f);
                check(o.phase == CP::kMirror && o.cullOn, "concourse clear: a negative share counts as below, and as out of view");
            }
            // Below during lifting returns to the mirror phase at once (the opacity is still 1), seen or not.
            {
                cc::ClearMachine f;
                stepAt(f, 50000, 0.30f);
                o = stepAt(f, 50016, 0.30f);
                check(o.phase == CP::kLifting, "concourse clear: (setup) lifting");
                stepAt(f, 50020, 0.05f);
                o = stepAt(f, 50270, 0.05f);
                check(o.phase == CP::kMirror && o.cullOn && o.bayOpacity == 1.0f, "concourse clear: below the exit share for 250 ms during lifting returns to the mirror phase, the bay still visible");
            }
            // Below during the fade out (bay still on screen): fades back in from the current opacity.
            {
                cc::ClearMachine f;
                stepAt(f, 60000, 0.30f);
                stepAt(f, 60016, 0.30f);
                stepAt(f, 60016 + 150, 0.30f);
                stepAt(f, 60016 + 150 + 200, 0.30f);   // opacity about 0.67
                stepAt(f, 60016 + 150 + 200 + 10, 0.05f);
                o = stepAt(f, 60016 + 150 + 200 + 260, 0.05f);
                check(o.phase == CP::kFadingIn && o.bayOpacity > 0.3f && o.bayOpacity < 0.7f && !o.cullOn, "concourse clear: below for 250 ms during the fade out fades back in from where it had got to");
            }
            // The gate dropping resets.
            cc::ClearMachine g;
            stepAt(g, 70000, 0.9f);
            o = stepAt(g, 70016, 0.9f);
            check(o.phase == CP::kLifting, "concourse clear: (setup) lifting again");
            in.gate = false;
            o = stepAt(g, 70032, 0.9f);
            check(o.phase == CP::kMirror && o.cullOn && o.bayOpacity == 1.0f && g.phase() == CP::kMirror, "concourse clear: no gate resets to the mirror phase, opacity 1");
            in.gate = true;
            // enterShare 0 never clears, and releases a clear one.
            cc::ClearMachine z;
            in.enterShare = 0.0f;
            bool never = true;
            for (uint32_t i = 0; i < 10; ++i) never = never && stepAt(z, 5000 + i * 16, 1.0f).phase == CP::kMirror;
            check(never, "concourse clear: diag.concourse_clear_share 0 never clears");
            in.enterShare = 0.20f;
            cc::ClearMachine rel;
            const uint64_t rt = toClear(rel, 80000);
            in.enterShare = 0.0f;
            stepAt(rel, rt + 100, -1.0f);
            o = stepAt(rel, rt + 350, -1.0f);
            check(o.phase == CP::kMirror && o.cullOn, "concourse clear: enter share 0 releases a clear machine");
            in.enterShare = 0.20f;
            // No reading (negative) is not a hit.
            cc::ClearMachine q;
            bool noRead = true;
            for (uint32_t i = 0; i < 10; ++i) noRead = noRead && stepAt(q, 9000 + i * 16, -1.0f).phase == CP::kMirror;
            check(noRead, "concourse clear: no reading never starts it");
            // Entering at the threshold itself.
            cc::ClearMachine th;
            stepAt(th, 90000, 0.20f);
            o = stepAt(th, 90016, 0.20f);
            check(o.phase == CP::kLifting, "concourse clear: two readings at exactly the enter share start it");
            check(std::strcmp(cc::clearPhaseName(CP::kFadingOut), "fading-out") == 0 && std::strcmp(cc::clearPhaseName(CP::kMirror), "mirror") == 0 &&
                      cc::kClearLiftMs == 150 && cc::kClearFadeMs == 600 && cc::kClearReleaseMs == 250 && cc::kClearExitRatio == 0.4f && cc::kClearUnseenShare == 0.01f,
                  "concourse clear: the phase names and the constants (150 ms, 600 ms, 250 ms, 0.4, 0.01)");
        }

        // A change of room: the camera cut. The constant sits between the boarding shift (16.6 m) and the lift (about 1090 m).
        {
            check(cc::kRoomCutM > 16.6f && cc::kRoomCutM < 1090.0f && cc::kRoomCutM == 100.0f, "concourse room cut: the limit is 100 m, between the 16.6 m boarding shift and the 1090 m lift");
            check(cc::roomCut(1090.0f) && cc::roomCut(100.01f) && !cc::roomCut(100.0f) && !cc::roomCut(16.6f) && !cc::roomCut(0.0f) && !cc::roomCut(-5.0f),
                  "concourse room cut: a step over 100 m is a cut; the boarding shift and a still camera are not");
            check(!cc::roomCut(std::nanf("")) && !cc::roomCut(std::numeric_limits<float>::infinity()), "concourse room cut: a step that is not finite is not a cut");
        }

        // The walk survey's zone (log only).
        {
            using WZ = cc::WalkZone;
            struct Row { bool seated, anchor, cell, camIn; WZ want; };
            const Row rows[] = {
                {true, false, true, true, WZ::kSeated},   {true, true, true, true, WZ::kSeated},   {true, false, false, false, WZ::kSeated},
                {false, true, true, true, WZ::kConcourse}, {false, true, false, false, WZ::kConcourse},
                {false, false, false, false, WZ::kNoCell}, {false, false, false, true, WZ::kNoCell},
                {false, false, true, true, WZ::kBay},      {false, false, true, false, WZ::kPastBox},
            };
            bool ok = true;
            for (const Row& r : rows) ok = ok && cc::walkZone(r.seated, r.anchor, r.cell, r.camIn) == r.want;
            check(ok, "concourse walk zone: seated, then concourse (anchor fresh), then no cell, then bay or past the box");
            check(std::strcmp(cc::walkZoneName(WZ::kSeated), "seated") == 0 && std::strcmp(cc::walkZoneName(WZ::kConcourse), "concourse") == 0 &&
                      std::strcmp(cc::walkZoneName(WZ::kBay), "bay") == 0 && std::strcmp(cc::walkZoneName(WZ::kPastBox), "past the box") == 0 &&
                      std::strcmp(cc::walkZoneName(WZ::kNoCell), "no cell") == 0,
                  "concourse walk zone: the names");
        }

        // The lift: the flag survives the slot beside the others, the judgement while the player is in a shut cabin, and the gate on the flight's own sequence.
        {
            const uintptr_t node = 0x7FF612340ull * 8;
            const uint32_t key = se::classKey(300);
            const uint32_t all = se::kNodeMechanism | se::kNodeHangarLike | se::kNodeShipPrefix | se::kNodeConcourseAllow | se::kNodeConcourseAnchor | se::kNodeConcourseFarExempt | se::kNodeLiftPart;
            const uint64_t v = se::packSlot(node, se::NameClass::kOther, all, key);
            se::NameClass cls = se::NameClass::kNone;
            uint32_t flags = 0;
            check(se::unpackSlot(v, node, key, &cls, &flags) && cls == se::NameClass::kOther && flags == all && (se::kNodeLiftPart & se::kSlotFlagMask) == se::kNodeLiftPart,
                  "lift: all seven flags and the class survive a pack and unpack");
            check(!se::unpackSlot(v, node, se::classKey(301), &cls, &flags) && se::unpackSlot(se::packSlot(node, se::NameClass::kOther, 0, key), node, key, &cls, &flags) && flags == 0,
                  "lift: another generation is a miss, and a node with no flags reads none");

            check(cc::concourseJudge(true, true, true, 300.0f, 3.0f, false, true) == cc::Judge::kCull, "lift judge: an allowed name 300 m away is culled");
            check(cc::concourseJudge(true, true, true, 60.0f, 3.0f, false, true) == cc::Judge::kCull && cc::concourseJudge(true, false, true, 30.0f, 3.0f, false, true) == cc::Judge::kCull,
                  "lift judge: an allowed name or a small prop 30 m or more away is culled (the room's usual keeps do not apply)");
            check(cc::concourseJudge(true, true, true, 10.3f, 2.4f, false, true) == cc::Judge::kKeepSmallClose && cc::concourseJudge(true, true, true, 0.9f, 2.4f, false, true) == cc::Judge::kKeepSmallClose,
                  "lift judge: a small record within 8 m of the surface is kept (the cabin, 2.4 m radius, 0.9 m away)");
            check(cc::concourseJudge(true, true, true, 10.5f, 2.4f, false, true) == cc::Judge::kCull, "lift judge: a surface just over 8 m is culled");
            check(cc::concourseJudge(true, true, true, 8.0f, 30.0f, false, true) == cc::Judge::kCull, "lift judge: a large record (radius over 25 m) is culled even when the camera is inside it");
            check(cc::concourseJudge(true, true, true, 5000.0f, 900.0f, true, true) == cc::Judge::kCull && cc::concourseJudge(true, true, true, 40.0f, 3.0f, true, true) == cc::Judge::kCull &&
                      cc::concourseJudge(true, true, true, 3.0f, 3.0f, true, true) == cc::Judge::kKeepSmallClose && cc::concourseJudge(true, true, true, 5000.0f, 900.0f, true, false) == cc::Judge::kKeepName,
                  "lift judge: the far exemption (the anchor, the hub, extra keep names) does not apply: a far one is culled, a near small one kept; without the lift it is still kept"); 
            check(cc::extraKeepName("WheelSection03", "WheelSection, Spoke") && cc::extraKeepName("SpokeA", "WheelSection, Spoke") && !cc::extraKeepName("TurboLiftDoor", "WheelSection, Spoke") &&
                      !cc::extraKeepName("SA_Bar", "WheelSection") && !cc::extraKeepName("WheelSection03", "") && !cc::extraKeepName("WheelSection03", nullptr) && !cc::extraKeepName("", "WheelSection") &&
                      !cc::extraKeepName(nullptr, "WheelSection") && !cc::extraKeepName("WheelSection03", " , ,") && cc::extraKeepName("WheelSection03", ",,  WheelSection "),
                  "extra keep: only the extra list matches, the built-in allow-list does not (SA_ and TurboLift are not extra), empty entries match nothing");
            check(cc::concourseAllowName("SA_Bar", "WheelSection") && cc::concourseAllowName("WheelSection03", "WheelSection") && !cc::concourseAllowName("WheelSection03", ""),
                  "extra keep: concourseAllowName is still the allow-list or an extra prefix");
            check(cc::concourseJudge(false, false, false, 0.0f, 0.0f, false, true) == cc::Judge::kKeepUnreadableSphere && cc::concourseJudge(true, true, true, 20.0f, 0.0f, false, true) == cc::Judge::kKeepUnreadableSphere,
                  "lift judge: an unreadable sphere is kept, whatever the name");
            check(cc::concourseJudge(false, false, true, 250.0f, 3.0f, false, true) == cc::Judge::kCull && cc::concourseJudge(true, true, true, 300.0f, 3.0f, false, false) == cc::Judge::kCull &&
                      cc::concourseJudge(true, true, true, 150.0f, 3.0f, false, false) == cc::Judge::kKeepName,
                  "lift judge: an unreadable name is culled far away; the default (no lift) is unchanged");

            // The gate. A reading: cabin distance, then the two gaps (doors known).
            using cc::LiftGate;
            using cc::LiftReading;
            const auto rd = [](float dist, float g1, float g2) {
                LiftReading r;
                r.cabin = true;
                r.cabinDistM = dist;
                r.doors = true;
                r.gap1 = g1;
                r.gap2 = g2;
                return r;
            };
            const LiftReading none{};
            LiftGate g;
            uint64_t t = 1000;
            const auto tick = [&](const LiftReading& r, bool cut) { t += 33; return g.step(r, cut, t); };
            // The flight: outside, shut, 3.24 m (no arm); the doors open while outside; inside, open; inside, shut (six ticks arm it); the ride; the cut at 0.98 m, gap 1.50; the
            // destination's doors, still shut, then opening.
            bool early = false;
            for (int i = 0; i < 20; ++i) early = early || tick(rd(3.24f, 1.49f, 1.37f), false);
            for (int i = 0; i < 10; ++i) early = early || tick(rd(3.5f, 2.9f, 2.65f), false);
            check(!early && !g.on(), "lift gate: standing outside, shut or open, never arms");
            for (int i = 0; i < 10; ++i) early = early || tick(rd(0.85f, 2.9f, 2.65f), false);
            check(!early, "lift gate: inside with the doors open does not arm");
            bool on5 = false;
            for (int i = 0; i < 5; ++i) on5 = tick(rd(0.85f, 1.49f, 1.37f), false);
            check(!on5, "lift gate: five shut readings inside are not enough");
            check(tick(rd(0.85f, 1.49f, 1.37f), false) && g.on(), "lift gate: the sixth shut reading inside, after seeing the doors open, arms it");
            bool stays = true;
            for (int i = 0; i < 100; ++i) stays = tick(rd(0.98f, 1.50f, 1.40f), false) && stays;
            check(stays, "lift gate: stays on through the ride");
            check(!tick(rd(0.98f, 1.50f, 1.40f), true) && !g.on() && g.lastEnd() == cc::LiftEnd::kCut, "lift gate: the camera cut ends it, with the doors still reading shut");
            bool again = false;
            for (int i = 0; i < 12; ++i) again = again || tick(rd(0.98f, 1.50f, 1.40f), false);
            check(!again, "lift gate: the destination's shut doors cannot arm it after the cut");
            for (int i = 0; i < 5; ++i) again = again || tick(rd(0.98f, 2.2f, 2.0f), false);
            check(!again && !g.on(), "lift gate: the destination's doors opening does not arm it");
            for (int i = 0; i < 5; ++i) again = again || tick(rd(2.32f, 2.9f, 2.65f), false);
            check(!again, "lift gate: leaving by the door stays off");

            // Standing just outside a shut door at 2.32 m, never on.
            LiftGate o;
            bool ever = false;
            for (int i = 0; i < 100; ++i) ever = ever || o.step(rd(2.32f, 1.49f, 1.37f), false, 5000 + 33u * i);
            check(!ever, "lift gate: standing outside a shut door at 2.32 m is never on");
            // A door that reopens with no cut: off at once, and it takes a fresh open and shut to arm again.
            LiftGate r;
            uint64_t u = 0;
            const auto rs = [&](const LiftReading& x) { u += 33; return r.step(x, false, u); };
            for (int i = 0; i < 3; ++i) rs(rd(0.9f, 2.9f, 2.65f));
            for (int i = 0; i < 6; ++i) rs(rd(0.9f, 1.49f, 1.37f));
            check(r.on(), "lift gate: armed for the reopening test");
            check(!rs(rd(0.9f, 1.8f, 1.37f)) && r.lastEnd() == cc::LiftEnd::kDoorsMoved, "lift gate: one pair opening switches it off at once, for the doors");
            bool re = false;
            for (int i = 0; i < 8; ++i) re = re || rs(rd(0.9f, 1.49f, 1.37f));
            check(re, "lift gate: shut again after the doors were seen open arms it again");
            check(!rs(rd(0.9f, 1.49f, 1.45f)) && r.lastEnd() == cc::LiftEnd::kDoorsMoved, "lift gate: the second pair alone, over 1.42 m, is open");
            // Doors unknown holds; no cabin or too far ends it.
            for (int i = 0; i < 3; ++i) rs(rd(0.9f, 2.9f, 2.65f));
            for (int i = 0; i < 6; ++i) rs(rd(0.9f, 1.49f, 1.37f));
            LiftReading blind = rd(0.9f, 0, 0);
            blind.doors = false;
            const bool held1 = rs(blind);
            const bool held2 = rs(blind);
            check(held1 && held2, "lift gate: doors not read this tick hold the state");
            check(!rs(rd(1.31f, 1.49f, 1.37f)) && r.lastEnd() == cc::LiftEnd::kLeftCabin, "lift gate: the cabin farther than 1.3 m ends it");
            for (int i = 0; i < 3; ++i) rs(rd(0.9f, 2.9f, 2.65f));
            for (int i = 0; i < 6; ++i) rs(rd(0.9f, 1.49f, 1.37f));
            check(r.on() && !rs(none) && r.lastEnd() == cc::LiftEnd::kLeftCabin, "lift gate: no cabin record ends it");
            // The timeout.
            LiftGate to;
            uint64_t w = 0;
            for (int i = 0; i < 3; ++i) to.step(rd(0.9f, 2.9f, 2.65f), false, w += 33);
            for (int i = 0; i < 6; ++i) to.step(rd(0.9f, 1.49f, 1.37f), false, w += 33);
            const uint64_t armedAt = to.onSinceMs();
            check(to.on() && to.step(rd(0.9f, 1.49f, 1.37f), false, armedAt + cc::kLiftMaxOnMs), "lift gate: on at exactly the limit");
            check(!to.step(rd(0.9f, 1.49f, 1.37f), false, armedAt + cc::kLiftMaxOnMs + 1) && to.lastEnd() == cc::LiftEnd::kTimedOut, "lift gate: over the limit it is off, for the timeout");
            bool rearm = false;
            for (int i = 0; i < 10; ++i) rearm = rearm || to.step(rd(0.9f, 1.49f, 1.37f), false, armedAt + cc::kLiftMaxOnMs + 100 + 33u * i);
            check(!rearm, "lift gate: after a timeout the doors must be seen open again before it re-arms");
            to.stop();
            check(!to.on(), "lift gate: stop is off");

            // The camera test's timer (TEMPORARY): a ride up arms it, the anchor's first sighting fires it, and it ends by time or the mode, whatever the doors and the cut do.
            {
                using cc::CamTestEnd;
                using cc::CamTestTimer;
                const uint32_t look = cc::kCamTestModeLook;
                CamTestEnd e = CamTestEnd::kNone;
                CamTestTimer c;
                check(!c.step(cc::kCamTestModeOff, true, false, 1000, &e) && !c.step(cc::kCamTestModeLog, true, false, 1033, &e) && !c.armed(), "camera test: modes off and log only never arm it");
                check(!c.step(look, false, false, 1066, &e) && !c.armed(), "camera test: no gate: not armed");
                check(!c.step(look, true, true, 1100, &e) && !c.armed(), "camera test: the gate on with the room already seen is a ride down: not armed");
                check(!c.step(look, true, false, 2000, &e) && c.armed() && !c.active() && c.armedMs() == 2000, "camera test: the gate on with no room seen is a ride up: armed");
                check(!c.step(look, false, false, 7000, &e) && c.armed(), "camera test: the gate letting go before arrival keeps it armed");
                check(c.step(look, false, true, 7800, &e) && c.active() && c.startMs() == 7800 && !c.armed() && e == CamTestEnd::kNone, "camera test: the anchor's first sighting fires it");
                check(c.step(look, false, false, 7800 + cc::kCamTestMs - 1, &e) && e == CamTestEnd::kNone, "camera test: open just under the limit, with the anchor gone (the look culls it, and the cut forgets it)");
                check(!c.step(look, false, true, 7800 + cc::kCamTestMs, &e) && e == CamTestEnd::kTime, "camera test: closes at exactly the limit, for the time");
                check(!c.step(look, true, true, 7800 + cc::kCamTestMs + 100, &e) && !c.armed(), "camera test: a ride down after it does not arm it");
                check(!c.step(look, true, false, 20000, &e) && c.armed(), "camera test: the next ride up arms it again");
                check(!c.step(look, false, false, 20000 + cc::kCamTestArmMs + 1, &e) && !c.armed() && !c.active(), "camera test: no sighting in time: it lets go");
                c.step(look, true, false, 40000, &e);
                c.step(look, false, true, 41000, &e);
                check(c.active() && !c.step(cc::kCamTestModeOff, false, true, 41100, &e) && e == CamTestEnd::kModeOff, "camera test: the mode turned off closes it");
                c.step(look, true, false, 50000, &e);
                check(c.armed() && !c.step(cc::kCamTestModeLog, false, false, 50100, &e) && !c.armed(), "camera test: the mode moved on: disarmed");
                c.step(look, true, false, 60000, &e);
                c.step(look, false, true, 60100, &e);
                check(c.stop() && !c.active() && !c.armed(), "camera test: stop closes an open window and says so");
                check(c.stop() == false, "camera test: stop when closed is a no-op");
                check(cc::camTestNextMode(0) == 1 && cc::camTestNextMode(1) == 2 && cc::camTestNextMode(2) == 3 && cc::camTestNextMode(3) == 0, "camera test: the key steps off, log only, look, bake, off");
                {
                    int wash = 0;
                    bool scatter = false, cones = false;
                    for (size_t i = 0; i < cc::kWithheldPsCount; ++i)
                        if (cc::kWithheldPs[i].washToo) {
                            ++wash;
                            scatter = scatter || std::strcmp(cc::kWithheldPs[i].name, "scatter") == 0;
                            cones = cones || std::strcmp(cc::kWithheldPs[i].name, "light cones") == 0;
                        }
                    check(wash == 2 && scatter && cones, "the bake: the scatter and the light cones alone stay off while the windows show the bake");
                }
                {
                    CamTestTimer b;
                    b.step(cc::kCamTestModeBake, true, false, 70000, &e);
                    check(b.armed() && b.step(cc::kCamTestModeBake, false, true, 70100, &e) && b.active(), "the bake: a ride up arms it and the first sighting opens it, like the look");
                    check(std::strcmp(cc::camTestEndName(CamTestEnd::kDone), "the bake took its frames") == 0, "the bake: its end has its own name");
                    check(cc::bakeDirCount(cc::kBakeDirsAll) == 5 && cc::bakeDirCount(0x11u) == 2, "the bake: the directions it takes are counted");
                    check(cc::bakeTicks(cc::kBakeDirsAll) == 15 + cc::kBakeTailTicks && cc::bakeTicks(0x10u) == 3 + cc::kBakeTailTicks, "the bake: its limit follows the directions");
                    bool order = true;
                    uint32_t aimed = cc::kBakeDirs - 1;
                    for (uint32_t d = 0; d < cc::kBakeDirs; ++d) {
                        aimed = cc::bakeNextDir(cc::kBakeDirsAll, 0, aimed);
                        order = order && aimed == d;
                    }
                    check(order, "the bake: each direction in turn, with none captured");
                    check(cc::bakeNextDir(cc::kBakeDirsAll, 0, 4) == 0, "the bake: it goes round");
                    check(cc::bakeNextDir(cc::kBakeDirsAll, 0x1Cu, 4) == 0 && cc::bakeNextDir(cc::kBakeDirsAll, 0x1Cu, 0) == 1 && cc::bakeNextDir(cc::kBakeDirsAll, 0x1Cu, 1) == 0, "the bake: only those not yet captured are asked for again");
                    check(cc::bakeNextDir(cc::kBakeDirsAll, cc::kBakeDirsAll, 3) == 3, "the bake: with every direction captured it stays");
                    // Flight 28: the first frames with the near plane raised never reach the resolve, and what is written reaches it some frames later. Every direction still arrives, inside the limit.
                    {
                        const auto simulate = [](uint32_t lost, uint32_t latency, uint32_t mask, uint32_t* ticksOut) {
                            uint32_t queue[32] = {}, captured = 0, last = cc::kBakeDirs - 1, tick = 0;
                            for (; tick < cc::bakeTicks(mask) && (captured & mask) != mask; ++tick) {
                                last = cc::bakeNextDir(mask, captured, last);
                                queue[tick] = last;
                                if (tick >= latency && tick - latency >= lost) captured |= 1u << queue[tick - latency];
                            }
                            *ticksOut = tick;
                            return (captured & mask) == mask;
                        };
                        uint32_t ticks = 0;
                        check(simulate(2, 5, cc::kBakeDirsAll, &ticks) && ticks <= 13, "the bake: two lost frames at 5 frames' latency still take every direction, in about 12 frames");
                        check(simulate(0, 5, cc::kBakeDirsAll, &ticks) && ticks <= 11, "the bake: with none lost it is no longer than the frames in flight");
                        check(simulate(4, 8, cc::kBakeDirsAll, &ticks), "the bake: and it holds with more lost and a longer pipeline");
                        check(simulate(2, 5, 0x10u, &ticks), "the bake: and with one direction");
                        check(!simulate(40, 5, cc::kBakeDirsAll, &ticks), "the bake: when frames never arrive, the limit ends it uncaptured");
                    }
                    bool round = true;
                    for (uint32_t f = 0; f < 6; ++f)
                        for (int a = 0; a < 5; ++a)
                            for (int b2 = 0; b2 < 5; ++b2) {
                                const float u = 0.1f + 0.2f * static_cast<float>(a), v = 0.1f + 0.2f * static_cast<float>(b2);
                                float dir[3] = {};
                                cc::cubeDirOf(f, u, v, dir);
                                cc::CubeTexel tx;
                                round = round && cc::cubeFaceOf(dir[0], dir[1], dir[2], &tx) && tx.face == f && std::fabs(tx.u - u) < 1e-5f && std::fabs(tx.v - v) < 1e-5f;
                            }
                    check(round, "the bake: a place on a face, to its direction and back");
                    check(std::strstr(cc::cubeDirHlsl(), "float3 cubeDir(uint face, float2 uv)") != nullptr, "the bake: the direction's HLSL twin is there");
                    check(cc::bakeNextDir(0x14u, 0, 4) == 2 && cc::bakeNextDir(0x14u, 0, 2) == 4 && cc::bakeNextDir(0x14u, 0x10u, 2) == 2, "the bake: directions left out are skipped");
                    check(cc::bakeParseDirs("11111") == cc::kBakeDirsAll && cc::bakeParseDirs("00001") == 0x10u && cc::bakeParseDirs("10100") == 0x5u, "the bake: the directions' key reads left to right");
                    check(cc::bakeParseDirs("") == cc::kBakeDirsAll && cc::bakeParseDirs("00000") == cc::kBakeDirsAll && cc::bakeParseDirs("1x111") == cc::kBakeDirsAll && cc::bakeParseDirs(nullptr) == cc::kBakeDirsAll,
                          "the bake: a key it cannot use takes all of them");
                    // A camera like the game's: reversed infinite depth, turned, relative to the camera. A point goes to clip through the columns, and the inverse brings it back.
                    const double sx = 0.5625, sy = 1.0, nearP = 0.025;
                    const double right[3] = {0.5, 0.866025404, 0.0}, up[3] = {0.866025404, -0.5, 0.0}, fwd[3] = {0.0, 0.0, -1.0};
                    double m[16] = {};
                    for (int i = 0; i < 3; ++i) {
                        m[0 + i] = sx * right[i];
                        m[4 + i] = sy * up[i];
                        m[12 + i] = fwd[i];
                    }
                    m[8 + 3] = nearP;
                    double inv[16] = {};
                    check(cc::invert4(m, inv), "the bake: a reversed infinite camera inverts");
                    const double pt[4] = {12.0, -40.0, -90.0, 1.0};
                    double clip[4] = {}, back[4] = {};
                    for (int r = 0; r < 4; ++r)
                        for (int k = 0; k < 4; ++k) clip[r] += m[r * 4 + k] * pt[k];
                    const double ndc[4] = {clip[0] / clip[3], clip[1] / clip[3], clip[2] / clip[3], 1.0};
                    for (int r = 0; r < 4; ++r)
                        for (int k = 0; k < 4; ++k) back[r] += inv[r * 4 + k] * ndc[k];
                    check(std::fabs(back[0] / back[3] - pt[0]) < 1e-6 && std::fabs(back[1] / back[3] - pt[1]) < 1e-6 && std::fabs(back[2] / back[3] - pt[2]) < 1e-6, "the bake: the inverse brings a pixel and its depth back to the point");
                    const double flat[16] = {1, 0, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 1};
                    check(!cc::invert4(flat, inv), "the bake: a singular matrix is refused");
                }
                check(cc::camTestLookDir(0) == 0 && cc::camTestLookDir(cc::kCamTestDirMs - 1) == 0 && cc::camTestLookDir(cc::kCamTestDirMs) == 1 && cc::camTestLookDir(cc::kCamTestMs - 1) == cc::kCamTestDirs - 1 &&
                          cc::camTestLookDir(cc::kCamTestMs * 10) == cc::kCamTestDirs - 1, "camera test: the direction steps, and stays on the last past the end");
                {
                    // nameReadable, allowed, sphereReadable, distance, radius, farExempt, lift, look
                    using cc::Judge;
                    check(cc::concourseJudge(true, true, true, 5.0f, 2.0f, false, false, true) == Judge::kCull, "the look: an allowed name close by is culled");
                    check(cc::concourseJudge(true, true, true, 5.0f, 130.0f, true, false, true) == Judge::kCull, "the look: the anchor and the hub are culled too");
                    check(cc::concourseJudge(true, false, true, 400.0f, 300.0f, false, false, true) == Judge::kKeepName, "the look: a name outside the list is kept");
                    check(cc::concourseJudge(true, false, true, 5.0f, 2.0f, false, false, true) == Judge::kCull, "the look: a small one close by is in the way, and culled");
                    check(cc::concourseJudge(true, false, true, 100.0f, 50.0f, false, false, true) == Judge::kCull, "the look: and one that reaches within the limit");
                    check(cc::concourseJudge(true, false, true, 200.0f, 50.0f, false, false, true) == Judge::kKeepName, "the look: past the limit it is kept");
                    check(cc::concourseJudge(true, false, true, 5.0f, 300.0f, false, false, true) == Judge::kKeepName, "the look: a large one around the camera is kept, it may be the station");
                    check(cc::concourseJudge(true, false, false, 0.0f, 0.0f, false, false, true) == Judge::kKeepName, "the look: a name with no sphere is kept");
                    check(cc::concourseJudge(false, false, true, 50.0f, 10.0f, false, false, true) == Judge::kCull, "the look: a small unnamed record in the room is culled");
                    check(cc::concourseJudge(false, false, true, 900.0f, 10.0f, false, false, true) == Judge::kKeepUnreadableName, "the look: a small unnamed record past the room is kept");
                    check(cc::concourseJudge(false, false, true, 50.0f, 400.0f, false, false, true) == Judge::kKeepUnreadableName, "the look: a large unnamed record is kept, it may be the station");
                    check(cc::concourseJudge(false, false, false, 0.0f, 0.0f, false, false, true) == Judge::kKeepUnreadableName, "the look: an unnamed record with no sphere is kept");
                    check(cc::concourseJudge(true, true, true, 5.0f, 2.0f, false, true, true) == Judge::kCull, "the look: it wins over the lift's rule");
                }
                {
                    CamTestTimer past;
                    check(!past.step(cc::kCamTestModes, true, false, 20000, &e) && !past.active() && !past.armed(), "camera test: a mode past the last does not arm it");
                    check(std::strcmp(cc::camTestModeName(2), "look from the room's centre") == 0 &&
                              std::strcmp(cc::camTestModeName(1), "log only") == 0 && std::strcmp(cc::camTestModeName(0), "off") == 0,
                          "camera test: the toast names the mode");
                }
                {
                    // A bake with no ride: a load straight into the room, or a render size that has risen.
                    CamTestTimer n;
                    n.armNow(1000);
                    check(n.armed() && !n.active(), "no ride: arming does not fire");
                    check(!n.step(cc::kCamTestModeBake, false, true, 1000 + cc::kBakeNoRideArmMs - 1, &e) && n.armed(), "no ride: it waits for the hold to keep a frame, with the room in view");
                    check(n.step(cc::kCamTestModeBake, false, true, 1000 + cc::kBakeNoRideArmMs, &e) && n.active() && !n.armed(), "no ride: then it fires");
                    n.armNow(1200);
                    check(n.active() && !n.armed(), "no ride: a running window is not armed over");
                    CamTestTimer gone;
                    gone.armNow(0);
                    check(!gone.step(cc::kCamTestModeBake, false, false, cc::kCamTestArmMs + 1, &e) && !gone.armed(), "no ride: the room gone for the arming limit lets it go");
                    CamTestTimer ride;
                    ride.armNow(0);
                    ride.stop();
                    ride.step(cc::kCamTestModeBake, true, false, 5000, &e);
                    check(ride.armed() && ride.step(cc::kCamTestModeBake, false, true, 5000, &e), "no ride: a ride up after it still fires at the first sighting, with no wait");

                    using cc::BakeNoRide;
                    check(cc::bakeNoRide(false, cc::kBakeRoomSettleMs - 1, false, 0) == BakeNoRide::kNone && cc::bakeNoRide(false, cc::kBakeRoomSettleMs, false, 0) == BakeNoRide::kFirst,
                          "no ride: no bake, and the room in view for the settling time, starts one");
                    check(cc::bakeNoRide(true, 100000, false, 0) == BakeNoRide::kNone && cc::bakeNoRide(true, 0, true, 0) == BakeNoRide::kSharper, "no ride: with a bake, only a sharper one starts another");
                    check(cc::bakeNoRide(false, 100000, false, cc::kBakeNoRideTries) == BakeNoRide::kNone && cc::bakeNoRide(true, 0, true, cc::kBakeNoRideTries) == BakeNoRide::kNone,
                          "no ride: a bake that keeps failing stops being tried");
                    check(cc::bakeWouldBeSharper(2160, 1080) && !cc::bakeWouldBeSharper(1080, 2160) && !cc::bakeWouldBeSharper(1080, 1080) && cc::bakeWouldBeSharper(1080, 0),
                          "sharper: only a taller frame than the bake's");
                    check(!cc::bakeWouldBeSharper(4320, cc::kBackdropCubeSize) && !cc::bakeWouldBeSharper(4320, 2160) && cc::bakeWouldBeSharper(4320, 1080), "sharper: not past the cube's own size");

                    const auto near_ = [](float a, float b) { return std::fabs(a - b) < 1e-4f; };
                    check(near_(cc::windowClearPercent(20.0f, cc::kWindowDistanceDefault), 20.0f), "window distance: 100 is the shipped share");
                    check(near_(cc::windowClearPercent(20.0f, 200), 5.0f) && near_(cc::windowClearPercent(20.0f, 50), 80.0f), "window distance: the share goes with the inverse square of the distance");
                    check(cc::windowClearPercent(20.0f, 0) == 0.0f, "window distance: 0 never clears");
                    check(cc::windowClearPercent(20.0f, 10) == 100.0f, "window distance: the share stops at the whole view");
                }
                {
                    check(cc::concourseAllowName("Root", nullptr) && cc::concourseAllowName("Perturbator", nullptr) && !cc::concourseAllowName("Floor07", nullptr), "the keep list: the bar's tables are on it, and the station's floors are not");
                    const uint64_t cone = 0x2519C9050946D545ull, sun = 0x7CECABDE34FFBE9Eull, lastLight = 0x37649F54AD53319Cull;
                    check(cc::lightTestShaderAt(0) == 0 && cc::lightTestShaderAt(1) == cone && cc::lightTestShaderAt(cc::kLightTestSteps - 1) == cc::kLightTestAllLights && cc::lightTestShaderAt(cc::kLightTestSteps) == 0, "the light test: step 0 leaves nothing out, and each step after it one");
                    check(cc::lightTestLeavesOut(cone, cone) && !cc::lightTestLeavesOut(cone, sun) && !cc::lightTestLeavesOut(0, 0), "the light test: one step leaves out its own shader and nothing else, and step 0 nothing");
                    check(cc::lightTestLeavesOut(cc::kLightTestAllLights, sun) && cc::lightTestLeavesOut(cc::kLightTestAllLights, lastLight) && !cc::lightTestLeavesOut(cc::kLightTestAllLights, cone), "the light test: the last step leaves out every light and only those");
                    check(cc::lightTestNext(cc::kLightTestSteps - 1) == 0 && cc::lightTestNext(0) == 1, "the light test: it goes round, back to all on");
                }
                {
                    // The station's type from the journal (journal_station.h), in the shape of real lines.
                    namespace js = edvr::journal_station;
                    using K = js::Kind;
                    js::Latch l;
                    K last = K::kUnknown;
                    int changes = 0;
                    const auto feed = [&](js::Latch& to, const char* text) {
                        to.feed(text, static_cast<uint32_t>(std::strlen(text)), [&](K k) {
                            last = k;
                            ++changes;
                        });
                    };
                    const char* dockedLoad = "{ \"timestamp\":\"2026-11-07T15:14:57Z\", \"event\":\"Location\", \"DistFromStarLS\":3797.2, \"Docked\":true, \"StationName\":\"Test Market\", \"StationType\":\"Orbis\", \"MarketID\":1, \"BodyType\":\"Station\" }\r\n";
                    const char* disembark = "{ \"timestamp\":\"t\", \"event\":\"Disembark\", \"SRV\":false, \"Taxi\":false, \"Multicrew\":false, \"ID\":1, \"StarSystem\":\"S\", \"Body\":\"Test Market\", \"BodyID\":4, \"OnStation\":true, \"OnPlanet\":false, \"StationName\":\"Test Market\", \"StationType\":\"Orbis\", \"MarketID\":1 }\r\n";
                    const char* onFootLoad = "{ \"timestamp\":\"t\", \"event\":\"Location\", \"DistFromStarLS\":3797.2, \"Docked\":false, \"OnFoot\":true, \"StarSystem\":\"S\", \"Body\":\"Test Market\", \"BodyID\":4, \"BodyType\":\"Station\" }\r\n";
                    const char* onFootElsewhere = "{ \"timestamp\":\"t\", \"event\":\"Location\", \"Docked\":false, \"OnFoot\":true, \"StarSystem\":\"S\", \"Body\":\"Another Market\", \"BodyID\":4, \"BodyType\":\"Station\" }\r\n";
                    check(l.kind() == K::kUnknown, "station type: unknown before the journal names one");
                    feed(l, dockedLoad);
                    check(l.kind() == K::kOrbis && last == K::kOrbis && changes == 1 && l.takeSavedChanged() && !l.takeSavedChanged(), "station type: a docked load names it and asks once to be saved");
                    feed(l, disembark);
                    check(l.kind() == K::kOrbis && changes == 1 && !l.takeSavedChanged(), "station type: the same station again changes nothing");
                    feed(l, "{ \"timestamp\":\"t\", \"event\":\"Music\", \"MusicTrack\":\"MainMenu\" }\r\n");
                    check(l.kind() == K::kOrbis, "station type: other events leave it");
                    feed(l, "{ \"timestamp\":\"t\", \"event\":\"Undocked\", \"StationName\":\"Test Market\", \"StationType\":\"Orbis\" }\r\n");
                    check(l.kind() == K::kUnknown && !l.takeSavedChanged(), "station type: an undock forgets where we are and keeps what was saved");
                    feed(l, onFootLoad);
                    check(l.kind() == K::kOrbis, "station type: a load on foot matches its Body against the saved station");
                    feed(l, onFootElsewhere);
                    check(l.kind() == K::kUnknown, "station type: a load on foot somewhere else is unknown");
                    feed(l, "{ \"timestamp\":\"t\", \"event\":\"Location\", \"Docked\":false, \"OnFoot\":true, \"Body\":\"LP 1 a\", \"BodyID\":4, \"BodyType\":\"Planet\" }\r\n");
                    check(l.kind() == K::kUnknown, "station type: on foot on a planet is unknown");
                    {
                        js::Latch rest;
                        feed(rest, "{ \"event\":\"ApproachSettlement\", \"Name\":\"Chamberlain's Rest\", \"Name_Localised\":\"Industrial\", \"BodyName\":\"HIP 97950 ABC 2 k\" }\n");
                        check(rest.kind() == K::kCraterPort, "station type: Chamberlain's Rest, with no StationType, is a CraterPort");
                        feed(rest, "{ \"event\":\"Location\", \"Docked\":false, \"OnFoot\":true, \"Body\":\"HIP 97950 ABC 2 k\", \"BodyType\":\"Planet\" }\n");
                        check(rest.kind() == K::kCraterPort, "station type: the on-foot location after it keeps the CraterPort");
                        feed(rest, "{ \"event\":\"ApproachSettlement\", \"Name\":\"Other Rest\" }\n");
                        feed(rest, "{ \"event\":\"Location\", \"Docked\":false, \"OnFoot\":true, \"Body\":\"LP 1 a\", \"BodyType\":\"Planet\" }\n");
                        check(rest.kind() == K::kUnknown, "station type: another settlement does not keep it");
                    }
                    {
                        // A port on a planet: real lines from a CraterOutpost, Flight 37. Only Docked and Undocked name it.
                        const char* portDocked = "{ \"timestamp\":\"t\", \"event\":\"Docked\", \"StationName\":\"Test Installation\", \"StationType\":\"CraterOutpost\", \"MarketID\":2 }\r\n";
                        const char* portDisembark = "{ \"timestamp\":\"t\", \"event\":\"Disembark\", \"SRV\":false, \"Taxi\":false, \"Multicrew\":false, \"ID\":1, \"StarSystem\":\"S\", \"Body\":\"LP 1 a\", \"BodyID\":4, \"OnStation\":false, \"OnPlanet\":true }\r\n";
                        const char* portEmbark = "{ \"timestamp\":\"t\", \"event\":\"Embark\", \"SRV\":false, \"Taxi\":false, \"Multicrew\":false, \"ID\":1, \"StarSystem\":\"S\", \"Body\":\"LP 1 a\", \"BodyID\":4, \"OnStation\":false, \"OnPlanet\":true }\r\n";
                        const char* portUndocked = "{ \"timestamp\":\"t\", \"event\":\"Undocked\", \"StationName\":\"Test Installation\", \"StationType\":\"CraterOutpost\", \"MarketID\":2 }\r\n";
                        const char* planetFoot = "{ \"timestamp\":\"t\", \"event\":\"Location\", \"Docked\":false, \"OnFoot\":true, \"Body\":\"LP 1 a\", \"BodyID\":4, \"BodyType\":\"Planet\" }\r\n";
                        js::Latch p;
                        feed(p, portDocked);
                        check(p.kind() == K::kCraterOutpost && p.takeSavedChanged(), "station type: a port on a planet is named by Docked, and saved");
                        feed(p, portDisembark);
                        check(p.kind() == K::kCraterOutpost && !p.takeSavedChanged(), "station type: a Disembark there names no station, and the type holds");
                        feed(p, portEmbark);
                        check(p.kind() == K::kCraterOutpost, "station type: so does an Embark");
                        feed(p, portUndocked);
                        check(p.kind() == K::kUnknown, "station type: until Undocked");
                        feed(p, portDisembark);
                        check(p.kind() == K::kUnknown, "station type: then a Disembark on a planet is open ground");
                        feed(p, planetFoot);
                        check(p.kind() == K::kCraterOutpost, "station type: a load on foot on a planet takes a saved port on a planet");
                        js::Latch next;
                        next.restore(p.saved());
                        feed(next, planetFoot);
                        check(next.kind() == K::kCraterOutpost, "station type: in the next session too");
                        feed(next, disembark);
                        check(next.kind() == K::kOrbis, "station type: a station that names itself comes first");
                        feed(next, portDisembark);
                        check(next.kind() == K::kUnknown, "station type: a Disembark on a planet does not hold a station's type");
                        feed(next, planetFoot);
                        check(next.kind() == K::kUnknown, "station type: nor does a load on foot take one");
                        js::Latch settlement;
                        feed(settlement, "{ \"event\":\"Docked\", \"StationName\":\"X\", \"StationType\":\"OnFootSettlement\" }\n");
                        feed(settlement, portDisembark);
                        check(settlement.kind() == K::kOnFootSettlement, "station type: any port on a planet holds");
                        // A fleet carrier: its Disembark names neither it nor a planet.
                        const char* carrierDisembark = "{ \"timestamp\":\"t\", \"event\":\"Disembark\", \"SRV\":false, \"Taxi\":false, \"Multicrew\":false, \"ID\":1, \"StarSystem\":\"S\", \"Body\":\"S\", \"BodyID\":0, \"OnStation\":false, \"OnPlanet\":false }\r\n";
                        js::Latch carrier;
                        feed(carrier, "{ \"event\":\"Docked\", \"StationName\":\"Q1H-2DZ\", \"StationType\":\"FleetCarrier\" }\n");
                        feed(carrier, carrierDisembark);
                        check(carrier.kind() == K::kFleetCarrier, "station type: a carrier's Disembark holds its type");
                        feed(carrier, "{ \"event\":\"Undocked\", \"StationName\":\"Q1H-2DZ\", \"StationType\":\"FleetCarrier\" }\n");
                        feed(carrier, carrierDisembark);
                        check(carrier.kind() == K::kUnknown, "station type: and after Undocked such a Disembark is nowhere");
                        js::Latch orbis;
                        feed(orbis, dockedLoad);
                        feed(orbis, carrierDisembark);
                        check(orbis.kind() == K::kUnknown, "station type: it holds only a carrier's");
                        // A carrier near a planet: its Disembark says "OnPlanet":true (Flight 41's journal, at 38 Lyncis 5).
                        js::Latch nearPlanet;
                        feed(nearPlanet, "{ \"event\":\"Docked\", \"StationName\":\"Q1H-2DZ\", \"StationType\":\"FleetCarrier\" }\n");
                        feed(nearPlanet, "{ \"timestamp\":\"t\", \"event\":\"Disembark\", \"SRV\":false, \"Taxi\":false, \"Multicrew\":false, \"ID\":136, \"StarSystem\":\"S\", \"Body\":\"S 5\", \"BodyID\":26, \"OnStation\":false, \"OnPlanet\":true }\r\n");
                        check(nearPlanet.kind() == K::kFleetCarrier, "station type: a carrier near a planet holds through its Disembark too");
                        feed(nearPlanet, "{ \"timestamp\":\"t\", \"event\":\"Embark\", \"SRV\":false, \"Taxi\":false, \"Multicrew\":false, \"ID\":136, \"StarSystem\":\"S\", \"Body\":\"S 5\", \"BodyID\":26, \"OnStation\":false, \"OnPlanet\":true }\r\n");
                        check(nearPlanet.kind() == K::kFleetCarrier, "station type: and its Embark");
                        // A jump aboard: counted, and the type stays, whether or not the event names the carrier.
                        check(nearPlanet.jumps() == 0u, "carrier jump: none before one");
                        feed(nearPlanet, "{ \"timestamp\":\"t\", \"event\":\"CarrierJump\", \"Docked\":true, \"StationName\":\"Q1H-2DZ\", \"StationType\":\"FleetCarrier\", \"MarketID\":3706882816, \"StarSystem\":\"T\", \"Body\":\"T A\", \"BodyType\":\"Star\" }\r\n");
                        check(nearPlanet.jumps() == 1u && nearPlanet.kind() == K::kFleetCarrier, "carrier jump: one that names the carrier is counted, and it is still a carrier");
                        feed(nearPlanet, "{ \"timestamp\":\"t\", \"event\":\"CarrierJump\", \"Docked\":false, \"OnFoot\":true, \"StarSystem\":\"U\", \"Body\":\"U 1\", \"BodyType\":\"Planet\" }\r\n");
                        check(nearPlanet.jumps() == 2u && nearPlanet.kind() == K::kFleetCarrier, "carrier jump: one that names none is counted, and the type stays");
                        nearPlanet.newFile();
                        check(nearPlanet.jumps() == 2u, "carrier jump: another file of the session keeps the count");
                        js::Latch jumpIn;
                        feed(jumpIn, "{ \"timestamp\":\"t\", \"event\":\"CarrierJump\", \"Docked\":true, \"StationName\":\"Q1H-2DZ\", \"StationType\":\"FleetCarrier\" }\r\n");
                        check(jumpIn.kind() == K::kFleetCarrier && jumpIn.saved().nameHash == js::hashName("Q1H-2DZ", 7), "carrier jump: one that names the carrier says where the commander is, and saves it");
                    }

                    js::Latch fresh;
                    uint64_t askedFor = 0;
                    check(!fresh.takeUnmatched(&askedFor), "station type: nothing to look for before a load on foot");
                    feed(fresh, onFootLoad);
                    check(fresh.kind() == K::kUnknown, "station type: a load on foot with nothing saved is unknown");
                    check(fresh.takeUnmatched(&askedFor) && askedFor == js::hashName("Test Market", 11) && !fresh.takeUnmatched(&askedFor), "station type: and it asks once, by the hash of its Body, to look in the older journals");
                    {
                        const std::string older = std::string(disembark) + "{ \"timestamp\":\"t\", \"event\":\"Music\", \"MusicTrack\":\"MainMenu\" }\r\n";
                        js::Saved inOlder;
                        check(js::lastStationIn(older.c_str(), static_cast<uint32_t>(older.size()), &inOlder) && inOlder.nameHash == askedFor && inOlder.kind == K::kOrbis, "station type: an older journal names it");
                        fresh.adopt(inOlder, [&](K k) { last = k; });
                        check(fresh.kind() == K::kOrbis && last == K::kOrbis && fresh.takeSavedChanged(), "station type: what it says is taken, and saved");
                        const std::string none = "{ \"timestamp\":\"t\", \"event\":\"Fileheader\" }\r\n" + std::string(onFootLoad);
                        check(!js::lastStationIn(none.c_str(), static_cast<uint32_t>(none.size()), nullptr), "station type: a journal that names no station says nothing");
                    }
                    {
                        js::Latch withMatch;
                        feed(withMatch, disembark);
                        feed(withMatch, onFootLoad);
                        check(!withMatch.takeUnmatched(&askedFor), "station type: a load on foot that matches looks no further");
                    }
                    js::Latch restored;
                    restored.restore(l.saved());
                    feed(restored, onFootLoad);
                    check(restored.kind() == K::kOrbis, "station type: what was saved works in the next session");

                    js::Latch split;
                    const char* whole = disembark;
                    for (size_t i = 0, n = std::strlen(whole); i < n; ++i) split.feed(whole + i, 1, [](K) {});
                    check(split.kind() == K::kOrbis, "station type: a line read a byte at a time is the same");
                    js::Latch part;
                    feed(part, disembark);
                    part.feed("{ \"event\":\"Undo", 15, [](K) {});
                    part.newFile();
                    check(part.kind() == K::kOrbis, "station type: another file of the session changes nothing");
                    part.feed("cked\" }\n", 8, [](K) {});
                    check(part.kind() == K::kOrbis, "station type: and a line cut by it is not read as a whole line");

                    js::Latch type;
                    feed(type, "{ \"event\":\"Docked\", \"StationName\":\"A\", \"StationType\":\"Bernal\" }\n");
                    check(type.kind() == K::kOcellus, "station type: Bernal is an Ocellus");
                    feed(type, "{ \"event\":\"Docked\", \"StationName\":\"Stronghold Carrier\", \"StationType\":\"SurfaceStation\" }\n");
                    check(type.kind() == K::kSurfaceStation, "station type: a Stronghold Carrier is a SurfaceStation");
                    feed(type, "{ \"event\":\"Docked\", \"StationName\":\"Q1H-2DZ\", \"StationType\":\"FleetCarrier\" }\n");
                    check(type.kind() == K::kFleetCarrier, "station type: a fleet carrier has its own");
                    feed(type, "{ \"event\":\"Docked\", \"StationName\":\"B\", \"StationType\":\"SomethingNew\" }\n");
                    check(type.kind() == K::kOther, "station type: a type not on the list is another type");

                    using T = K;
                    check(cc::bakePlanFor(T::kOrbis).run && cc::bakePlanFor(T::kCoriolis).run && cc::bakePlanFor(T::kOcellus).run && cc::bakePlanFor(T::kOrbis).dirs == cc::kBakeDirsAll,
                          "bake plan: the large stations run, with every direction");
                    check(cc::bakePlanFor(T::kCraterOutpost).run && cc::bakePlanFor(T::kCraterOutpost).dirs == cc::kBakeDirsAll, "bake plan: so does a CraterOutpost, flown");
                    check(cc::bakePlanFor(T::kOutpost).run && cc::bakePlanFor(T::kOutpost).dirs == cc::kBakeDirsAll, "bake plan: and an orbital outpost, flown");
                    check(cc::bakePlanFor(T::kFleetCarrier).run && cc::bakePlanFor(T::kFleetCarrier).dirs == cc::kBakeDirsAll, "bake plan: and a fleet carrier, flown");
                    check(cc::bakePlanFor(T::kCraterPort).run && cc::bakePlanFor(T::kCraterPort).dirs == cc::kBakeDirsAll, "bake plan: and a large surface port, flown");
                    check(cc::bakePlanFor(T::kAsteroidBase).run && cc::bakePlanFor(T::kAsteroidBase).dirs == cc::kBakeDirsAll, "bake plan: and an asteroid base, flown");
                    check(cc::bakePlanFor(T::kDodec).run && cc::bakePlanFor(T::kDodec).dirs == cc::kBakeDirsAll, "bake plan: and a dodec, flown");
                    check(edvr::journal_station::kindOf("Dodec", 5) == T::kDodec && std::strcmp(edvr::journal_station::kindName(T::kDodec), "Dodec") == 0 &&
                              edvr::journal_station::kindOf("Dodecahedron", 12) == T::kOther,
                          "journal: \"Dodec\" is a dodec, and a longer name is not");
                    check(cc::runsWithoutBake(T::kFleetCarrier) && !cc::runsWithoutBake(T::kOrbis) && !cc::runsWithoutBake(T::kCoriolis) && !cc::runsWithoutBake(T::kOcellus) &&
                              !cc::runsWithoutBake(T::kCraterOutpost) && !cc::runsWithoutBake(T::kOutpost) && !cc::runsWithoutBake(T::kCraterPort) && !cc::runsWithoutBake(T::kAsteroidBase) &&
                              !cc::runsWithoutBake(T::kUnknown) && !cc::runsWithoutBake(T::kOther),
                          "runs without a bake: a fleet carrier only");
                    // H57: the frames at 5 m, so the sky, stars and sunlight are drawn; the room's cut at 50 m, as the near plane was.
                    check(cc::kBakeNearDefaultM == 5.0f && cc::kBakeRoomCutM == 50.0f && cc::kBakeNearMinM <= cc::kBakeNearDefaultM, "bake: frames at 5 m, the room cut at 50 m");
                    bool people = cc::bakePeopleVs(cc::kBakeHairPrepassVs);
                    for (uint64_t vs : {0x8B589D25B2A0ADDCull, 0x7B0DC42D383F694Cull, 0x114AF608F86D9ED8ull, 0xD99AFDC250D19A3Full, 0x61AE8EB05FDC18DDull, 0xA6A39338C06E03A1ull,
                                        0x01DA82F8D0EA0C99ull})
                        people = people && cc::bakePeopleVs(vs);
                    check(people, "bake: the people's families are left out (the NPC's body and skin, the hair and its prepass, the suit)");
                    check(!cc::bakePeopleVs(0) && !cc::bakePeopleVs(0xEB5234DB6ADB491Dull) && !cc::bakePeopleVs(0x66DE2CADB1F4AE6Bull) && !cc::bakePeopleVs(0x98397963AAEC45D3ull) &&
                              !cc::bakePeopleVs(0x7E38A6AA1269C901ull) && !cc::bakePeopleVs(0x8BD7C37ABCEE7E45ull),
                          "bake: the station's moving records, a hull, the glass and the sun are not people");
                    check(!cc::bakePlanFor(T::kUnknown).run &&
                              !cc::bakePlanFor(T::kSurfaceStation).run && !cc::bakePlanFor(T::kMegaShip).run &&
                              !cc::bakePlanFor(T::kOnFootSettlement).run && !cc::bakePlanFor(T::kOther).run,
                          "bake plan: everything else, and not knowing, stays the game's own");

                    char text[128] = {};
                    js::Saved s;
                    s.nameHash = js::hashName("Test Market", 11);
                    s.kind = K::kCoriolis;
                    js::formatSaved(s, text, sizeof(text));
                    js::Saved back;
                    check(js::parseSaved(text, &back) && back.nameHash == s.nameHash && back.kind == K::kCoriolis, "saved station: the line reads back");
                    check(std::strstr(text, "Test Market") == nullptr, "saved station: the name is not in it");
                    js::Saved none;
                    check(!js::parseSaved("", &none) && !js::parseSaved("edvr-station 1 zz Orbis\n", &none) && !js::parseSaved("edvr-station 1 0123456789abcdef Nonsense\n", &none) &&
                              !js::parseSaved("edvr-station 2 0123456789abcdef Orbis\n", &none) && !js::parseSaved(nullptr, &none) && none.nameHash == 0,
                          "saved station: anything else reads as nothing saved");
                }
            }
        }

        // The backdrop cube's face mapping (PROTOTYPE): D3D TextureCube addressing, faces +X -X +Y -Y +Z -Z.
        {
            using CT = cc::CubeTexel;
            const auto at = [](float x, float y, float z, uint32_t face, float u, float v) {
                CT c;
                return cc::cubeFaceOf(x, y, z, &c) && c.face == face && std::fabs(c.u - u) < 1e-6f && std::fabs(c.v - v) < 1e-6f;
            };
            check(at(1, 0, 0, 0, 0.5f, 0.5f) && at(-1, 0, 0, 1, 0.5f, 0.5f) && at(0, 1, 0, 2, 0.5f, 0.5f) && at(0, -1, 0, 3, 0.5f, 0.5f) && at(0, 0, 1, 4, 0.5f, 0.5f) && at(0, 0, -1, 5, 0.5f, 0.5f),
                  "backdrop cube: each axis is the centre of its face, +X -X +Y -Y +Z -Z in order");
            // Off-centre points on each face: sc and tc per the D3D table, u = 0.5 (sc / ma + 1), v = 0.5 (tc / ma + 1).
            check(at(2, 1, 1, 0, 0.5f * (-0.5f + 1), 0.5f * (-0.5f + 1)) && at(-2, 1, 1, 1, 0.5f * (0.5f + 1), 0.5f * (-0.5f + 1)),
                  "backdrop cube: the X faces take sc = -z / +z and tc = -y");
            check(at(1, 2, 1, 2, 0.5f * (0.5f + 1), 0.5f * (0.5f + 1)) && at(1, -2, 1, 3, 0.5f * (0.5f + 1), 0.5f * (-0.5f + 1)),
                  "backdrop cube: the Y faces take sc = +x and tc = +z / -z");
            check(at(1, 1, 2, 4, 0.5f * (0.5f + 1), 0.5f * (-0.5f + 1)) && at(1, 1, -2, 5, 0.5f * (-0.5f + 1), 0.5f * (-0.5f + 1)),
                  "backdrop cube: the Z faces take sc = +x / -x and tc = -y");
            check(at(1, 1, 1, 0, 0.0f, 0.0f) && at(1, -1, -1, 0, 1.0f, 1.0f) && at(-3, 3, 3, 1, 1.0f, 0.0f),
                  "backdrop cube: a corner direction is a tie won by x, then y, and lands on the face's corner");
            check(at(10, 0, 0, 0, 0.5f, 0.5f) && at(0.0f, 0.0f, -0.001f, 5, 0.5f, 0.5f), "backdrop cube: the length of the direction does not matter");
            CT c;
            check(!cc::cubeFaceOf(0, 0, 0, &c) && !cc::cubeFaceOf(NAN, 0, 1, &c) && !cc::cubeFaceOf(INFINITY, 0, 1, &c), "backdrop cube: a zero or non-finite direction has no face");
            uint32_t tx = 0, ty = 0;
            cc::cubeTexelOf(CT{0, 1.0f, 1.0f}, 1536, &tx, &ty);
            const bool clampHigh = tx == 1535 && ty == 1535;
            cc::cubeTexelOf(CT{0, 0.0f, 0.0f}, 1536, &tx, &ty);
            const bool lowEdge = tx == 0 && ty == 0;
            cc::cubeTexelOf(CT{0, 0.5f, 0.25f}, 1536, &tx, &ty);
            check(clampHigh && lowEdge && tx == 768 && ty == 384, "backdrop cube: u or v of 1.0 clamps to the last texel, 0.0 is the first, the middle is half the size");
            check(std::strstr(cc::cubeTexelHlsl(), "uint3 cubeTexel(float3 d, uint size)") != nullptr && cc::kBackdropCubeSize == 2048 && cc::kBackdropLiftMs == 200 && cc::kBackdropWaitFrames == 3,
                  "backdrop cube: the HLSL twin is present, the cube is 1536 square, the lift 200 ms and the wait 3 frames");
            // The distance modes: the sphere solve, and the cycle.
            {
                const float zero[3] = {0, 0, 0}, dir[3] = {0.0f, 3.0f, 4.0f};
                float s[3] = {};
                const bool okCentre = cc::backdropSphereHit(zero, dir, 600.0f, s) && std::fabs(s[0]) < 1e-3f && std::fabs(s[1] - 360.0f) < 1e-2f && std::fabs(s[2] - 480.0f) < 1e-2f;
                const float off[3] = {100.0f, -50.0f, 20.0f}, d2[3] = {1.0f, 2.0f, -2.0f};
                float p[3] = {};
                const bool hit = cc::backdropSphereHit(off, d2, 600.0f, p);
                const float len = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
                // the point lies on the sphere and along the ray: (p - off) is a positive multiple of d2
                const float k = (p[0] - off[0]) / 1.0f;
                const bool along = k > 0.0f && std::fabs((p[1] - off[1]) - 2.0f * k) < 1e-2f && std::fabs((p[2] - off[2]) + 2.0f * k) < 1e-2f;
                check(okCentre && hit && std::fabs(len - 600.0f) < 0.05f && along, "backdrop distance: the camera at the centre looks up dir * R; off centre it lands on the sphere, along its ray");
                const float farOff[3] = {1000.0f, 0.0f, 0.0f}, dn[3] = {0.0f, 1.0f, 0.0f};
                float m[3] = {};
                check(cc::backdropSphereHit(farOff, dn, 600.0f, m) && std::fabs(m[0] - 1000.0f) < 1e-3f && std::fabs(m[1]) < 1e-3f && !cc::backdropSphereHit(zero, zero, 600.0f, m),
                      "backdrop distance: a ray that misses the sphere takes its nearest point, a zero direction has none");
                check(cc::parallaxNext(cc::kParallaxNone) == cc::kParallaxSphere && cc::parallaxNext(cc::kParallaxSphere) == cc::kParallaxDepth && cc::parallaxNext(cc::kParallaxDepth) == cc::kParallaxNone &&
                          std::strcmp(cc::parallaxName(cc::kParallaxDepth), "depth") == 0 && cc::kBackdropRadiusDefaultM == 600.0f,
                      "backdrop distance: the key steps none, sphere, depth and round; the radius defaults to 600 m");
            }            // The automatic capture's trigger, as a table.
            {
                using In = cc::BackdropAutoInput;
                const auto ready = [](float share, uint64_t now) {
                    In in;
                    in.nowMs = now;
                    in.share = share;
                    in.autoOn = in.modeBackdrop = in.cullPartOn = in.plainMirror = true;
                    return in;
                };
                bool ok = true;
                {
                    cc::BackdropAutoGate g;
                    ok = ok && g.step(ready(0.0019f, 10000)).fire == false && g.step(ready(0.002f, 10000)).fire == true;   // below and at the threshold
                }
                {
                    cc::BackdropAutoGate g;
                    In in = ready(0.01f, 10000);
                    in.autoOn = false;
                    ok = ok && !g.step(in).fire;
                    in = ready(0.01f, 10000); in.modeBackdrop = false; ok = ok && !g.step(in).fire;
                    in = ready(0.01f, 10000); in.cullPartOn = false; ok = ok && !g.step(in).fire;
                    in = ready(0.01f, 10000); in.plainMirror = false; ok = ok && !g.step(in).fire;
                    in = ready(0.01f, 10000); in.liftOn = true; ok = ok && !g.step(in).fire;
                    in = ready(0.01f, 10000); in.captureRunning = true; ok = ok && !g.step(in).fire;
                    ok = ok && g.step(ready(0.01f, 10000)).fire;   // none of them: fires
                }
                check(ok, "backdrop auto: the share must reach 0.2%, and each blocking condition (auto off, not backdrop, cull part off, not plain mirror, lift, capture running) stops it");
                cc::BackdropAutoGate g;
                bool gapOk = g.step(ready(0.01f, 10000)).fire;   // first capture
                g.captureEnded(10300);
                gapOk = gapOk && !g.step(ready(0.01f, 11799)).fire && g.step(ready(0.001f, 11799)).fire == false;   // inside the gap; and a share below the threshold never fires
                check(gapOk && g.gapMs() == 1500, "backdrop auto: no capture within 1500 ms of the last one's end");
                // The share did not fall: the gap doubles each time, up to 30 s, and the escalation is said once.
                cc::BackdropAutoGate e;
                uint64_t now = 10000;
                bool fired = e.step(ready(0.01f, now)).fire;
                e.captureEnded(now + 300);
                now += 300 + 1500;
                cc::BackdropAutoDecision d = e.step(ready(0.01f, now));   // judged: not fallen, gap 3000, still inside it
                bool said = d.escalated && d.shareAtStart == 0.01f && d.gapMs == 3000 && !d.fire;
                now += 1500;
                d = e.step(ready(0.01f, now));
                fired = fired && d.fire && !d.escalated;
                e.captureEnded(now + 300);
                now += 300 + 3000;
                d = e.step(ready(0.01f, now));   // doubled again to 6000, and not said again
                bool again = !d.escalated && d.gapMs == 6000 && !d.fire;
                uint64_t last = 0;
                for (int i = 0; i < 12; ++i) {
                    now += 40000;
                    d = e.step(ready(0.01f, now));
                    if (d.fire) { e.captureEnded(now + 300); now += 300; }
                    last = d.gapMs;
                }
                check(fired && said && again && last == cc::kBackdropMaxGapMs, "backdrop auto: a capture that does not lower the share doubles the gap (said once), up to 30 s");
                // The share falls: the gap starts over, below the threshold or by a tenth.
                cc::BackdropAutoGate r;
                now = 10000;
                r.step(ready(0.01f, now));
                r.captureEnded(now + 300);
                now += 300 + 1500;
                d = r.step(ready(0.005f, now));   // 0.005 < 0.9 * 0.01: fallen, the gap stays 1500 and it fires
                bool fell = d.fire && d.gapMs == 1500 && !d.escalated;
                r.captureEnded(now + 300);
                now += 300 + 1500;
                d = r.step(ready(0.01f, now));    // up again, not lower than 0.9 * 0.005: the gap doubles
                bool up = d.gapMs == 3000;
                now += 100;
                r.step(ready(0.0f, now));         // nothing missing: reset
                bool back = r.gapMs() == 1500;
                r.reset();
                check(fell && up && back && r.gapMs() == 1500, "backdrop auto: a falling share, or none missing, resets the gap; reset() starts over");
            }
            // The glass parser.
            {
                float g[6] = {9, 9, 9, 9, 9, 9};
                char def[64];
                std::snprintf(def, sizeof(def), "%g,%g,%g,%g,%g,%g", static_cast<double>(cc::kGlassDefault[0]), static_cast<double>(cc::kGlassDefault[1]), static_cast<double>(cc::kGlassDefault[2]),
                              static_cast<double>(cc::kGlassDefault[3]), static_cast<double>(cc::kGlassDefault[4]), static_cast<double>(cc::kGlassDefault[5]));
                const bool okDef = cc::parseGlass(def, g) && g[0] == 0.92f && g[3] == 0.03f && g[4] == 0.25f && g[5] == 0.25f;
                float h[6] = {7, 7, 7, 7, 7, 7};
                const bool okSp = cc::parseGlass(" 1 , 1 ,1, 0 , 0 , 0 ", h) && h[0] == 1.0f && h[3] == 0.0f && h[5] == 0.0f;
                float k[6] = {5, 5, 5, 5, 5, 5};
                const bool bad = !cc::parseGlass("1,1,1,0,0", k) && !cc::parseGlass("1,1,1,0,0,0,0", k) && !cc::parseGlass("1,1,1,2,0,0", k) && !cc::parseGlass("3,1,1,0,0,0", k) &&
                                 !cc::parseGlass("1,1,1,0,9,0", k) && !cc::parseGlass("-1,1,1,0,0,0", k) && !cc::parseGlass("a,1,1,0,0,0", k) && !cc::parseGlass("1,1,1,0,0,0x", k) &&
                                 !cc::parseGlass("", k) && !cc::parseGlass(nullptr, k) && k[0] == 5.0f;
                check(okDef && okSp && bad, "backdrop glass: six numbers parse (spaces allowed), a wrong count, range, letter or trailing text is refused and leaves the values");
            }            using J = cc::Judge;
            check(cc::concourseJudge(true, true, true, 600.0f, 300.0f, false, false) == J::kCull && cc::concourseJudge(true, true, true, 600.0f, 300.0f, false, true) == J::kCull &&
                      cc::concourseJudge(true, true, true, 3.0f, 3.0f, false, true) == J::kKeepSmallClose && cc::concourseJudge(true, true, true, 150.0f, 3.0f, false, false) == J::kKeepName,
                  "concourse judge: with the size test gone, a big far record is culled as before and the lift rule is unchanged");
        }
        // The lift survey's name test and moved test (TEMPORARY, log only).
        {
            check(cc::liftPartName("TurboLiftDoorLeft") && cc::liftPartName("TurboLiftDoorLeft2") && cc::liftPartName("TurboLiftDoorRight") && cc::liftPartName("TurboLiftDoorRight2") &&
                      cc::liftPartName("TurboLiftDoor") && cc::liftPartName("TurboLift_Blocker") && cc::liftPartName("SA_TurboLiftBones") && cc::liftPartName("saturboliftx"),
                  "lift survey: the seven door and cabin names match, in any case");
            check(!cc::liftPartName("Lift") && !cc::liftPartName("LandingPadSmallA") && !cc::liftPartName("") && !cc::liftPartName(nullptr) && !cc::liftPartName("Turbo"),
                  "lift survey: Lift, a pad, an empty name, null and a partial prefix do not match");
            const float a[3] = {1, 2, 3}, b[3] = {1, 2.01f, 3}, c[3] = {1, 2.05f, 3}, nan3[3] = {NAN, 0, 0};
            check(!cc::liftMoved(b, a, 0.02f) && cc::liftMoved(c, a, 0.02f) && !cc::liftMoved(nan3, a, 0.02f) && cc::liftMoved(a, nan3, 0.02f), "lift survey: moved is over 0.02 m, a non-finite current is not a move, a non-finite last is");
        }

        // The glow levels by window type (diag.concourse_glow = sky,angled,other,bay).
        {
            float v[4] = {-1, -1, -1, -1};
            check(cc::parseGlowLevels("4,0.5,1,0.5", v) && v[0] == 4.0f && v[1] == 0.5f && v[2] == 1.0f && v[3] == 0.5f, "concourse glow: the default text parses to sky 4, angled 0.5, other 1, bay 0.5");
            check(cc::kGlowSkyDefault == 0.5f && cc::kGlowAngledDefault == 0.5f && cc::kGlowOtherDefault == 1.0f && cc::kGlowBayDefault == 0.5f, "concourse glow: the defaults are 0.5, 0.5, 1, 0.5");
            check(cc::parseGlowLevels(" 0 , 16 ,  2.5 , 1 ", v) && v[0] == 0.0f && v[1] == 16.0f && v[2] == 2.5f && v[3] == 1.0f, "concourse glow: spaces are allowed, 0 and 16 are the limits");
            float keep[4] = {7, 8, 9, 10};
            check(!cc::parseGlowLevels("", keep) && !cc::parseGlowLevels(nullptr, keep) && !cc::parseGlowLevels("1,2,3", keep) && !cc::parseGlowLevels("4,0.25,1", keep) &&
                      !cc::parseGlowLevels("1,2,3,4,5", keep) && !cc::parseGlowLevels("1,2,3,x", keep) && !cc::parseGlowLevels("1;2;3;4", keep) && !cc::parseGlowLevels("1,2,3,4 5", keep) &&
                      !cc::parseGlowLevels(",,,", keep),
                  "concourse glow: the old three-number form, the wrong count, a letter or a stray character is refused");
            check(!cc::parseGlowLevels("-0.1,1,1,1", keep) && !cc::parseGlowLevels("1,16.01,1,1", keep) && !cc::parseGlowLevels("1,1,1,100", keep) && !cc::parseGlowLevels("nan,1,1,1", keep) &&
                      !cc::parseGlowLevels("inf,1,1,1", keep),
                  "concourse glow: a value under 0 or over 16, or not finite, is refused");
            check(keep[0] == 7.0f && keep[1] == 8.0f && keep[2] == 9.0f && keep[3] == 10.0f, "concourse glow: a refused text leaves the output as it was");
        }

        // The entry share of the shop window.
        {
            check(cc::clearEntryScale(30) == 0.3f && cc::clearEntryScale(36) == 1.0f && cc::clearEntryScale(12) == 1.0f && cc::clearEntryScale(0) == 1.0f,
                  "concourse clear: only n=30 (the shop window) enters at 0.3 of the share");
            check(std::fabs(0.2f * cc::clearEntryScale(30) - 0.06f) < 1e-6f && std::fabs(0.2f * cc::clearEntryScale(30) * cc::kClearExitRatio - 0.024f) < 1e-6f,
                  "concourse clear: the 20% share becomes 6% in and 2.4% out for the shop window");
            check(0.073f >= 0.2f * cc::clearEntryScale(30) && 0.03f < 0.2f * cc::clearEntryScale(30), "concourse clear: the 7.3% seen at the counter enters, the under-3% seen elsewhere does not");
        }

        // The total of the outside glass in the main view: a bar's three meshes, two eyes, an offscreen pass, sides and the weights.
        {
            constexpr double kMain = 2560.0 * 1440.0;
            auto smp = [](uint32_t n, uint32_t start, float share, double pixels, cc::PaneSide side) {
                cc::GlassSample s;
                s.key = cc::PaneKey{n, start, 0};
                s.share = share;
                s.pixels = pixels;
                s.side = side;
                return s;
            };
            const cc::PaneSide ext = cc::PaneSide::kExterior;
            const auto close2 = [](float a, float b) { return std::fabs(a - b) < 1e-5f; };
            {
                const cc::GlassSample bar[3] = {smp(24, 0, 0.022f, kMain, ext), smp(30, 0, 0.015f, kMain, ext), smp(36, 0, 0.014f, kMain, ext)};
                check(close2(cc::exteriorGlassShare(bar, 3, kMain, 36), 0.022f + 0.015f / 0.3f + 0.014f),
                      "exterior glass: a bar's three meshes sum, the shop window's n=30 weighted by its entry scale (0.086)");
            }
            {
                const cc::GlassSample eyes[2] = {smp(36, 100, 0.10f, kMain, ext), smp(36, 100, 0.12f, kMain, ext)};
                check(close2(cc::exteriorGlassShare(eyes, 2, kMain, 36), 0.12f), "exterior glass: the same pane in two eyes counts once, at the larger share");
                const cc::GlassSample two[2] = {smp(36, 100, 0.10f, kMain, ext), smp(36, 200, 0.12f, kMain, ext)};
                check(close2(cc::exteriorGlassShare(two, 2, kMain, 36), 0.22f), "exterior glass: two panes of one index count with different first indices both count");
            }
            {
                const cc::GlassSample off[2] = {smp(30, 0, 0.414f, 256.0 * 256.0, ext), smp(36, 0, 0.02f, kMain, ext)};
                check(close2(cc::exteriorGlassShare(off, 2, kMain, 36), 0.02f), "exterior glass: a 256x256 viewport's 41.4% is not the screen and is left out");
                const cc::GlassSample alone[1] = {smp(30, 0, 0.414f, 256.0 * 256.0, ext)};
                check(close2(cc::exteriorGlassShare(alone, 1, kMain, 36), 0.0f), "exterior glass: with the main view remembered, an offscreen-only frame totals zero");
                const cc::GlassSample both[2] = {smp(30, 0, 0.414f, 256.0 * 256.0, ext), smp(30, 0, 0.015f, kMain, ext)};
                check(close2(cc::exteriorGlassShare(both, 2, 0.0, 36), 0.05f), "exterior glass: with no main view remembered yet, the frame's own largest viewport stands for it");
            }
            {
                const cc::GlassSample in1[1] = {smp(36, 0, 0.3f, kMain, cc::PaneSide::kInterior)};
                check(close2(cc::exteriorGlassShare(in1, 1, kMain, 36), 0.0f), "exterior glass: an interior pane never counts, not even the bay's index count");
            }
            {
                const cc::GlassSample unk[2] = {smp(36, 0, 0.05f, kMain, cc::PaneSide::kUnknown), smp(24, 0, 0.04f, kMain, cc::PaneSide::kUnknown)};
                check(close2(cc::exteriorGlassShare(unk, 2, kMain, 36), 0.05f), "exterior glass: an unknown side counts only when it is the bay's index count");
            }
            {
                const cc::GlassSample outpost[1] = {smp(30, 0, 0.1f, kMain, ext)};
                check(close2(cc::exteriorGlassShare(outpost, 1, kMain, 30), 0.1f), "exterior glass: where the bay is n=30 (an outpost) it has weight 1");
            }
            {
                double m = 0.0;
                m = cc::mainViewPixelsNext(m, 1000000.0);
                check(m == 1000000.0, "main view pixels: a larger frame is taken at once");
                m = cc::mainViewPixelsNext(m, 65536.0);
                check(std::fabs(m - 999000.0) < 1e-6, "main view pixels: a smaller frame decays it by 0.999");
                m = cc::mainViewPixelsNext(m, 65536.0);
                check(std::fabs(m - 998001.0) < 1e-6, "main view pixels: and again per call");
            }
            {
                const cc::GlassSample big[3] = {smp(36, 0, 0.6f, kMain, ext), smp(24, 0, 0.5f, kMain, ext), smp(30, 0, 0.4f, kMain, ext)};
                check(cc::exteriorGlassShare(big, 3, kMain, 36) == 1.0f, "exterior glass: the total clamps at 1");
            }
        }

        // The withheld outside draws: named by shader, found by a bloom and a scan.
        {
            static_assert(cc::kWithheldPs[0].hash == cc::kPlumePs && cc::kWithheldPs[1].hash == cc::kTwinPs, "the plume pair leads the list");
            check(cc::kWithheldPsCount == 22 && cc::kWithheldVsCount == 10, "concourse withheld: 22 pixel shaders and 10 vertex shaders");
            check(!cc::withheldHasDuplicate(cc::kWithheldPs, cc::kWithheldPsCount) && !cc::withheldHasDuplicate(cc::kWithheldVs, cc::kWithheldVsCount),
                  "concourse withheld: no list holds a hash twice");
            const cc::WithheldFamily dup[] = {{1, "a"}, {2, "b"}, {1, "c"}};
            check(cc::withheldHasDuplicate(dup, 3), "concourse withheld: the duplicate check finds a duplicate");
            bool psFound = true, vsFound = true, names = true;
            for (size_t i = 0; i < cc::kWithheldPsCount; ++i) {
                psFound = psFound && cc::withheldPsIndex(cc::kWithheldPs[i].hash) == static_cast<int>(i);
                names = names && cc::kWithheldPs[i].name && *cc::kWithheldPs[i].name;
            }
            for (size_t i = 0; i < cc::kWithheldVsCount; ++i) {
                vsFound = vsFound && cc::withheldVsIndex(cc::kWithheldVs[i].hash) == static_cast<int>(i);
                names = names && cc::kWithheldVs[i].name && *cc::kWithheldVs[i].name;
            }
            check(psFound && vsFound && names, "concourse withheld: every listed hash is found at its own index, and has a name");
            // Near misses: one bit off, same low six bits, and the hashes that must not be withheld.
            bool missed = true;
            for (size_t i = 0; i < cc::kWithheldPsCount; ++i) {
                const uint64_t h = cc::kWithheldPs[i].hash;
                missed = missed && cc::withheldPsIndex(h ^ 0x100ull) == -1 && cc::withheldPsIndex(h ^ 1ull) == -1 && cc::withheldPsIndex(h + (1ull << 40)) == -1;
            }
            for (size_t i = 0; i < cc::kWithheldVsCount; ++i) {
                const uint64_t h = cc::kWithheldVs[i].hash;
                missed = missed && cc::withheldVsIndex(h ^ 0x100ull) == -1 && cc::withheldVsIndex(h ^ 1ull) == -1 && cc::withheldVsIndex(h + (1ull << 40)) == -1;
            }
            check(missed, "concourse withheld: near-miss hashes are not found");
            check(cc::withheldPsIndex(0) == -1 && cc::withheldVsIndex(0) == -1, "concourse withheld: no shader (hash 0) is not found");
            check(cc::withheldPsIndex(cc::kPaneFamilyPs) == -1 && cc::withheldVsIndex(cc::kPaneFamilyVs) == -1, "concourse withheld: the glass family is not withheld");
            check(!cc::farLimitedPs(0x2519C9050946D545ull) && cc::farLimitedPs(0x92FF8499ED345759ull), "concourse far-limited: the glow quads, and no longer the cones");
            {
                const int coneAt = cc::withheldPsIndex(0x2519C9050946D545ull);
                check(coneAt >= 0 && cc::kWithheldPs[coneAt].washToo && !cc::kWithheldPs[coneAt].sealedToo && std::strcmp(cc::kWithheldPs[coneAt].name, "light cones") == 0,
                      "concourse withheld: the light cones go with the cull and stay off while the windows show the bake, and a sealed hangar keeps its own");
            }
            const auto sealedPs = [](uint64_t h) { const int i = cc::withheldPsIndex(h); return i >= 0 && cc::kWithheldPs[i].sealedToo; };
            const auto sealedVs = [](uint64_t h) { const int i = cc::withheldVsIndex(h); return i >= 0 && cc::kWithheldVs[i].sealedToo; };
            check(sealedPs(0xA75C1DB6562B8CA7ull) && sealedPs(0x3B0B38CD96F53BC1ull) && sealedPs(0x43E5E6EB67AC751Bull) && sealedPs(0xDD371C57C9093BB8ull) && sealedVs(0x26FC402B1274EE7Bull),
                  "concourse sealed: lasers, plumes, the planet and the ship ribbon also go in a sealed hangar");
            check(!sealedPs(0xD56F859BE4781431ull) && !sealedPs(0x41152F82C6E8BE1Full) && !sealedPs(0x2BAE3742FEB916D9ull) && !sealedPs(0x3789CA2062E196FBull) && !sealedPs(0xE54F2A902E5631F6ull) &&
                      !sealedPs(0x89B662E266E5D73Eull),
                  "concourse sealed: flares, the scatter, the particles and the engine glow stay in a sealed hangar");
            uint8_t glowT10[cc::kPaneTexSize * cc::kPaneTexSize * 4] = {};
            cc::fillPaneGlowT10(glowT10);
            check(glowT10[0] == 255 && glowT10[3] == 255 && glowT10[sizeof(glowT10) - 1] == 255, "concourse glow: a white, fully rough material");
            check(!cc::farLimitedPs(0) && !cc::farLimitedPs(cc::kPaneFamilyPs) && !cc::farLimitedPs(0x06332CA168B6DA63ull), "concourse far-limited: nothing else (no shader, the glass, the stars)");
            check(cc::withheldPsIndex(0x92FF8499ED345759ull) == -1, "concourse far-limited: not also withheld, or the room's would go");
            const uint64_t keep[] = {0x88DCF1164C640EC3ull, 0x494506A6ull, 0x1C506222ull, 0x359BF8FFull, 0xF8FA801Full, 0x68DDDEF0ull, 0xF7A6E916ull, 0xE904D334ull};
            bool kept = true;
            for (uint64_t h : keep) kept = kept && cc::withheldPsIndex(h) == -1 && cc::withheldVsIndex(h) == -1;
            check(kept, "concourse withheld: the player's laser, the room's glows, the sky, the stars and the dust are not withheld");
            check(cc::withheldPsIndex(0x43E5E6EB67AC751Bull) == 13 && cc::withheldVsIndex(0xACE405F428C17EF6ull) == 5, "concourse withheld: the planet surface (ps) and the terrain patch (vs) are named");
        }

        // The largest reading wins: one mesh drawn at two places must not flip its side from frame to frame.
        {
            cc::PaneSides sd;
            const cc::PaneKey x{12, 12078150, 0};
            sd.note(x, 133000, 88000);   // 66% mirrored at the large instance
            check(sd.side(x) == cc::PaneSide::kExterior, "concourse pane sides: the large instance reads exterior");
            const auto c1 = sd.note(x, 17000, 5100);   // 30% at the small instance
            check(c1 == cc::PaneSides::Change::kNone && sd.side(x) == cc::PaneSide::kExterior && sd.exteriorCount() == 1,
                  "concourse pane sides: the small interior instance does not flip a large exterior one");
            bool steady = true;
            for (int i = 0; i < 20; ++i) {
                sd.note(x, 17000, 5100);
                sd.note(x, 133000, 88000);
                steady = steady && sd.side(x) == cc::PaneSide::kExterior;
            }
            check(steady, "concourse pane sides: alternating small and large readings never flip it");
            const auto c2 = sd.note(x, 66500, 5000);   // exactly half of the largest: counts
            check(c2 == cc::PaneSides::Change::kChanged && sd.side(x) == cc::PaneSide::kInterior, "concourse pane sides: a reading at half the largest can still change the side");
            sd.note(x, 140000, 130000);
            check(sd.side(x) == cc::PaneSide::kExterior, "concourse pane sides: a larger reading changes it, and becomes the new largest");
            sd.note(x, 69000, 100);
            check(sd.side(x) == cc::PaneSide::kExterior, "concourse pane sides: the largest only grows (69000 is under half of 140000)");
            sd.reset();
            sd.note(x, 17000, 5100);
            check(sd.side(x) == cc::PaneSide::kInterior, "concourse pane sides: reset forgets the largest reading");
        }
    }
    testVrBake();
    glt::testAll();

    std::printf("optimized_station_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
