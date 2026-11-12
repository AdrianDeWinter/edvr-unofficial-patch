// A camera-term pixel whose previous position is off the screen, in the flat prep on WARP (XeSS Frame Generation, debug.w bit 64).
//
// The camera moves three render pixels between the previous frame and this one (the fixture camera at depth .01: .9375 of translation, the same
// move flat_steady_depth_gpu_tests.h's "camera moved three pixels" case uses), so the two right-hand columns of the 16 x 16 render were, last
// frame, to the right of the screen. Such a pixel is refused (reject 1, kClassRange) either way. Without FlatMonoResolveFrame::keepOffscreenMotion
// its motion is 0, as it always was; with it the motion is the camera term, (prev - rawUv) * size, which points off the screen, while the
// rejection stays 1. A pixel well inside the screen is the same in both. The same scenario against the prep with the bit's test removed must
// FAIL (a scenario that cannot fail proves nothing).
#pragma once
#include "flat_steady_depth_gpu_tests.h"

namespace offscreengpu {
using steadygpu::Rig;
using steadygpu::Seen;
using steadygpu::W;

inline float mx(const Seen& s, UINT x, UINT y) { return s.motion.size() > 2 * (size_t(y) * W + x) + 1 ? s.motion[2 * (size_t(y) * W + x)] : 9999.f; }
inline float my(const Seen& s, UINT x, UINT y) { return s.motion.size() > 2 * (size_t(y) * W + x) + 1 ? s.motion[2 * (size_t(y) * W + x) + 1] : 9999.f; }
inline bool refused(const Seen& s, UINT x, UINT y) { return size_t(y) * W + x < s.mask.size() && s.mask[size_t(y) * W + x] != 0; }

inline void scenario(Rig& r) {
    using namespace edvr;
    flatMonoResolveReset();
    r.start(FlatMonoResolveMode::Dlss);
    r.run("off-screen motion: the reset frame that starts the run");
    r.f.reset = false;
    const UINT edgeX = 14, insideX = 4, y = 8;
    // The camera term on the CPU: the fixture moves a pixel 3.2 texels per unit of translation at depth .01 (current to previous, render pixels).
    const float termX = .9375f * 3.2f;

    for (int keep = 0; keep < 2; ++keep) {
        r.fillZ(.01f); r.noSlots(); r.setRecord(0); r.phases(0, 0, 0, 0);
        r.f.keepOffscreenMotion = keep != 0;
        r.f.camera[5][0] = .9375f;
        const Seen s = r.run(keep ? "off-screen motion: the bit set, a camera that moved three pixels" : "off-screen motion: the bit clear, a camera that moved three pixels");
        r.f.camera[5][0] = 0; r.f.keepOffscreenMotion = false;
        const char* tag = keep ? "off-screen motion (bit set): " : "off-screen motion (bit clear): ";
        auto say = [&](const char* text) { static char buf[320]; std::snprintf(buf, sizeof(buf), "%s%s", tag, text); return static_cast<const char*>(buf); };

        // The pixel well inside the screen, the same in both: the camera term, kept.
        check(!refused(s, insideX, y), say("a pixel well inside the screen is not refused"));
        check(std::fabs(mx(s, insideX, y) - termX) < .05f && std::fabs(my(s, insideX, y)) < .05f,
              say("a pixel well inside the screen has the camera term (about +3, 0 render pixels)"));

        // The strip: its previous position is to the right of the screen.
        const float prevX = (float(edgeX) + .5f + termX) / float(W);
        check(prevX > 1.f, "off-screen motion: the fixture's strip pixel really has its previous position off the screen (test setup)");
        check(refused(s, edgeX, y) && refused(s, W - 1, y), say("the strip's history is refused (reject 1), with or without the bit"));
        if (!keep) {
            check(mx(s, edgeX, y) == 0 && my(s, edgeX, y) == 0, say("the strip pixel's motion is exactly 0"));
        } else {
            const float gotX = mx(s, edgeX, y), gotY = my(s, edgeX, y);
            check(std::fabs(gotX - termX) < .05f && std::fabs(gotY) < .05f, say("the strip pixel's motion is the camera term (about +3, 0 render pixels)"));
            check((float(edgeX) + .5f + gotX) / float(W) > 1.f, say("the strip pixel's motion points off the screen (its previous position is outside [0,1])"));
        }
    }

    // A stale-slot pixel (kind 3, steady detail on) in the strip: its camera term stands only once last frame's depth confirms it, which an
    // off-screen previous position cannot, so with the bit set it keeps motion 0. Its plain neighbour in the strip keeps the camera term.
    r.fillZ(.01f); r.noSlots(); r.setRecord(0); r.phases(0, 0, 0, 0);
    r.put(edgeX, y, 1, .5f);
    r.f.steadyDetail = true; r.f.keepOffscreenMotion = true; r.f.camera[5][0] = .9375f;
    const Seen stale = r.run("off-screen motion: the bit set, a stale-slot pixel in the strip");
    r.f.camera[5][0] = 0; r.f.keepOffscreenMotion = false; r.f.steadyDetail = false; r.noSlots();
    check(refused(stale, edgeX, y), "off-screen motion (stale slot): the stale strip pixel is refused");
    check(mx(stale, edgeX, y) == 0 && my(stale, edgeX, y) == 0,
          "off-screen motion (stale slot): a stale-slot pixel in the strip keeps motion 0 with the bit set (its vector is unconfirmed)");
    check(std::fabs(mx(stale, W - 1, y) - termX) < .05f && std::fabs(my(stale, W - 1, y)) < .05f,
          "off-screen motion (stale slot): the plain strip pixel beside it still keeps the camera term");
}

inline void mutationCheck(Rig& r) {
    // The shader with the bit's test removed (motion zeroed for every refused pixel, as before) must fail the scenario.
    const std::string shipped = edvr::kFlatMonoShaderSource;
    bool once = false;
    const std::string hlsl = fpgpu::replaceOnce(shipped, "(debug.w&64)!=0 && offscreenOnly", "false && offscreenOnly", &once);
    check(once, "off-screen motion mutation: the bit's test is in the shader source exactly once (a moved anchor tests nothing)");
    std::vector<unsigned char> bytes;
    if (!once || !fpgpu::compilePrep(hlsl, bytes)) { check(false, "off-screen motion mutation: the mutated prep compiles"); return; }
    edvr::flatMonoResolveTestPrepBytecode(bytes.data(), bytes.size());
    edvr::flatMonoResolveReset();
    int failed = 0; mutationFailures = &failed; mutationFirst.clear();
    scenario(r);
    mutationFailures = nullptr;
    std::printf("flat mono resolve: off-screen motion mutation (the bit ignored): %d checks fail; first: %s\n", failed, mutationFirst.c_str());
    check(failed > 0, "off-screen motion mutation: a prep that ignores the bit is caught by the scenario");
    edvr::flatMonoResolveTestPrepBytecode(nullptr, 0);
    edvr::flatMonoResolveReset();

    // The shader without the stale-slot and vehicle-volume exclusion (every sane off-screen pixel kept) must fail it too.
    bool guardOnce = false;
    const std::string unguarded = fpgpu::replaceOnce(shipped, "sane && kind!=3 && !(kind==0 && vehicleState!=0)", "sane", &guardOnce);
    check(guardOnce, "off-screen motion mutation: the exclusion is in the shader source exactly once");
    std::vector<unsigned char> guardBytes;
    if (!guardOnce || !fpgpu::compilePrep(unguarded, guardBytes)) { check(false, "off-screen motion mutation: the unguarded prep compiles"); return; }
    edvr::flatMonoResolveTestPrepBytecode(guardBytes.data(), guardBytes.size());
    edvr::flatMonoResolveReset();
    failed = 0; mutationFailures = &failed; mutationFirst.clear();
    scenario(r);
    mutationFailures = nullptr;
    std::printf("flat mono resolve: off-screen motion mutation (no stale-slot exclusion): %d checks fail; first: %s\n", failed, mutationFirst.c_str());
    check(failed > 0, "off-screen motion mutation: a prep that keeps a stale-slot pixel's unconfirmed vector is caught by the scenario");
    edvr::flatMonoResolveTestPrepBytecode(nullptr, 0);
    edvr::flatMonoResolveReset();
}
}  // namespace offscreengpu

inline void offscreenMotionGpuTests(ID3D11Device* device, ID3D11DeviceContext* context) {
    offscreengpu::Rig rig(device, context);
    offscreengpu::scenario(rig);
    offscreengpu::mutationCheck(rig);
    edvr::flatMonoResolveReset();
}
