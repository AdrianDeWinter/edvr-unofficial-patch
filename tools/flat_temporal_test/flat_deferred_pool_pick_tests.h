#pragma once
// Two or more SOURCE records on the deferred lighting pass's depth (flat_runtime_model.h, flatRuntimeDeferredDonor's pool rule). On foot
// with a tool held, at render >= output (the HDR route, where no first-person cohort is set aside), the tool's keyed material draws into
// the G-buffer under the first-person camera beside the world's, so the depth holds two records, one per camera. The pool rule took a
// single record only, so the pass had no donor and every jittered frame was refused deferred-inverse-unproven (a flat field report on
// v0.22.0-rc.7 and the maintainer's Frontier flight of 2026-11-10, both on traces). The resolver now hands the verifier every record's
// camera when each record stands in the pass's layout, and takes back the one CB2 belongs to (`select`), as it does for a two-camera
// witness entry. These tests judge the resolver, the real triple from the report, and the wiring in the verifier.
#include <cstdio>
#include <cstring>
#include <string>
#include "flat_deferred_second_pick_tests.h"
#include "flat_deferred_pool_pair_captures.h"
namespace deferred_pool_pick {
using namespace edvr;
using deferred_witness_test::Scene;
using laser_scene_tone_test::token;

inline unsigned run() {
    unsigned failures = 0;
    const auto expect = [&](bool ok, const char* name) { if (!ok) { ++failures; std::printf("FAIL: deferred pool pick %s\n", name); } };
    using Why = FlatDeferredDonorWhy;
    using Role = FlatDeferredDonorRole;

    // ---- the real triple -------------------------------------------------------------------------------------------------------------
    unsigned decoded = 0;
    for (const auto& cap : deferred_pool_pair::kCaptures) {
        unsigned char world[96], firstPerson[96], cb2[112];
        if (!(deferred_second_pick::hex(cap.world, world, 96) && deferred_second_pick::hex(cap.firstPerson, firstPerson, 96) &&
              deferred_second_pick::hex(cap.cb2, cb2, 112))) { expect(false, "capture decodes"); continue; }
        ++decoded;
        float px, py; std::memcpy(&px, &cap.phaseX, 4); std::memcpy(&py, &cap.phaseY, 4);
        FlatProjectionJitter jitter{};
        const bool ready = (px != 0 || py != 0) && flatProjectionJitter(px, py, cap.width, cap.height, jitter);
        char name[160];
        std::snprintf(name, sizeof(name), "frame %u: a nonzero phase, so the repairs are asked", cap.frame);
        expect(ready, name);
        std::snprintf(name, sizeof(name), "frame %u: the world's camera holds against CB2", cap.frame);
        expect(deferred_second_pick::holds(world, cb2, jitter, ready), name);
        unsigned char corrected[112];
        std::printf("  pool-pair frame %u: world relation=%s missing-phase=%u whole=%u; first-person relation=%s\n", cap.frame,
                    flatDeferredCameraMatchName(flatMatchDeferredCamera(world, cb2)),
                    flatRepairDeferredMissingPhase(world, cb2, jitter, corrected) ? 1u : 0u,
                    flatRepairDeferredWholeOmission(world, cb2, jitter, corrected) ? 1u : 0u,
                    flatDeferredCameraMatchName(flatMatchDeferredCamera(firstPerson, cb2)));
        std::snprintf(name, sizeof(name), "frame %u: the tool's first-person camera does not hold", cap.frame);
        expect(!deferred_second_pick::holds(firstPerson, cb2, jitter, ready), name);
        std::snprintf(name, sizeof(name), "frame %u: the two cameras are not the same bytes", cap.frame);
        expect(std::memcmp(world, firstPerson, 96) != 0, name);
    }
    expect(decoded == 1, "the field report's capture decodes");

    // ---- the resolver --------------------------------------------------------------------------------------------------------------
    {
        Scene s;   // the world's record, then the tool's: both into SRV0, two cameras
        s.see(s.pool(s.gbuf));
        s.see(s.pool(s.gbuf, .0675f));
        const auto lighting = s.lighting();
        const auto plain = s.donor(lighting);
        expect(s.p->sourcesUsed == 2 && !plain && plain.why == Why::PoolRule && plain.poolRecords == 2 && plain.poolSelectable &&
               plain.poolCameraCount == 2 && plain.poolCameras[0] == s.p->sources[0].camera && plain.poolCameras[1] == s.p->sources[1].camera &&
               !plain.selected, "two records in SRV0 are selectable and, unnamed, give no donor");
        const auto named = [&](const unsigned char* camera) { return flatRuntimeDeferredDonor(*s.p, lighting.key, s.p->sequence + 1, nullptr, camera); };
        const auto world = named(s.p->sources[0].camera);
        expect(world && world.role == Role::Pool && world.why == Why::None && world.selected && world.camera == s.p->sources[0].camera &&
               world.cameraHash == s.p->sources[0].key.cameraHash && world.b1 == s.p->sources[0].key.b1 &&
               world.first == s.p->sources[0].first && world.last == s.p->sources[0].last &&
               world.writeSeq == s.p->sources[0].key.writeSeq && world.lastWriteSeq == s.p->sources[0].lastWriteSeq,
               "naming the world's camera hands out the world's record");
        const auto tool = named(s.p->sources[1].camera);
        expect(tool && tool.selected && tool.camera == s.p->sources[1].camera && tool.first == s.p->sources[1].first,
               "naming the other camera hands out its record");
        unsigned char stranger[96]; std::memcpy(stranger, s.p->sources[0].camera, 96); stranger[84] ^= 1;
        expect(!named(stranger) && !named(stranger).selected, "bytes that name neither record select nothing");
        unsigned char zero[96] = {};
        expect(!named(zero), "an unverified draw's empty camera selects nothing");
        // the qualification asks the same way, with the verified draw's camera
        auto verified = lighting; std::memcpy(verified.camera, s.p->sources[0].camera, 96); verified.key.cameraHash = s.p->sources[0].key.cameraHash;
        const auto qd = flatRuntimeDeferredDonor(*s.p, verified.key, s.p->sequence + 1, nullptr, verified.camera);
        expect(qd && qd.selected && qd.cameraHash == verified.key.cameraHash && std::memcmp(qd.camera, verified.camera, 96) == 0,
               "the qualification's donor is the record the verified draw's camera names");
    }
    {
        Scene one;   // one record: the name is not needed and not used
        one.see(one.pool(one.gbuf));
        unsigned char other[96]; std::memcpy(other, one.p->sources[0].camera, 96); other[84] ^= 1;
        const auto plain = one.donor(one.lighting());
        const auto named = flatRuntimeDeferredDonor(*one.p, one.lighting().key, one.p->sequence + 1, nullptr, other);
        expect(plain && !plain.poolSelectable && !plain.selected && plain.poolCameraCount == 0, "one record is the donor, as before");
        expect(named && !named.selected && named.camera == plain.camera, "and a name does not change it");
    }
    {
        Scene e;   // the same bytes behind another b1: two records, one set of bytes, nothing to choose
        e.see(e.pool(e.gbuf));
        auto twin = e.pool(e.gbuf); twin.key.b1 = token(0x3001); e.see(twin);
        const auto d = e.donor(e.lighting());
        expect(e.p->sourcesUsed == 2 && d.poolSelectable && !flatRuntimeDeferredDonor(*e.p, e.lighting().key, e.p->sequence + 1, nullptr, e.p->sources[0].camera),
               "two records of equal bytes cannot be told apart by their bytes: no selection");
    }
    {
        Scene c;   // a record in another colour on the depth: not selectable (the existing rule)
        c.see(c.pool(c.gbuf)); c.see(c.pool(token(0x1100), .0675f));
        const auto d = c.donor(c.lighting());
        expect(!d && !d.poolSelectable && d.poolRecords == 2 &&
               !flatRuntimeDeferredDonor(*c.p, c.lighting().key, c.p->sequence + 1, nullptr, c.p->sources[0].camera),
               "a second record in another colour refuses, named or not");
        Scene st;   // a stale record among the two
        st.see(st.pool(st.gbuf)); auto stale = st.pool(st.gbuf, .0675f); stale.key.writeEpoch = deferred_witness_test::kFrame - 1; st.see(stale);
        expect(!st.donor(st.lighting()).poolSelectable &&
               !flatRuntimeDeferredDonor(*st.p, st.lighting().key, st.p->sequence + 1, nullptr, st.p->sources[0].camera),
               "a stale record among them refuses, named or not");
        Scene vp;   // a partial viewport among the two
        vp.see(vp.pool(vp.gbuf)); auto part = vp.pool(vp.gbuf, .0675f); part.key.viewport[2] = 960; vp.see(part);
        expect(!vp.donor(vp.lighting()).poolSelectable, "a record with a partial viewport refuses");
        Scene late;   // the pass at or before a record's last draw
        late.see(late.pool(late.gbuf)); late.see(late.pool(late.gbuf, .0675f));
        const auto at = flatRuntimeDeferredDonor(*late.p, late.lighting().key, 2, nullptr, late.p->sources[0].camera);
        expect(!at && !at.poolSelectable, "a pass that does not come after every record selects nothing");
        Scene self;   // the pass writing its own source
        self.see(self.pool(self.gbuf)); self.see(self.pool(self.gbuf, .0675f));
        auto own = self.lighting(); own.key.color = self.gbuf;
        expect(!flatRuntimeDeferredDonor(*self.p, own.key, self.p->sequence + 1, nullptr, self.p->sources[0].camera), "the pass writing its own source selects nothing");
    }
    {
        Scene three;   // three cameras, all in the layout: the named one
        three.see(three.pool(three.gbuf)); three.see(three.pool(three.gbuf, .0675f)); three.see(three.pool(three.gbuf, .09f));
        const auto d = three.donor(three.lighting());
        const auto mid = flatRuntimeDeferredDonor(*three.p, three.lighting().key, three.p->sequence + 1, nullptr, three.p->sources[1].camera);
        expect(d.poolSelectable && d.poolCameraCount == 3 && mid && mid.selected && mid.camera == three.p->sources[1].camera,
               "three records: the named one");
        Scene many;   // more records than the pick holds: none, and not selectable
        const float nears[] = {.025f, .0675f, .09f, .11f, .13f};
        for (float n : nears) many.see(many.pool(many.gbuf, n));
        const auto m = many.donor(many.lighting());
        expect(many.p->sourcesUsed == 5 && !m && !m.poolSelectable && m.poolRecords == 5 &&
               !flatRuntimeDeferredDonor(*many.p, many.lighting().key, many.p->sequence + 1, nullptr, many.p->sources[0].camera),
               "five records are more than the pick tests: no donor");
    }
    {
        // 2026-11-16, the HDR route with nothing keyed in the world (Epic, EDHM, native res): the tool's keyed draws came first under
        // the first-person camera and were the depth's ONLY source record, so the single-record rule handed the pass the tool's camera.
        Scene before;   // as it was: the tool a source record, the world's unkeyed writers then not noted
        auto tool = before.pool(before.gbuf, .0675f); before.see(tool);
        before.see(before.writer()); before.see(before.writer());
        const auto wrong = before.donor(before.lighting());
        expect(wrong && wrong.role == Role::Pool && wrong.camera == before.p->sources[0].camera && before.p->witnessesUsed == 0,
               "control: with the tool a source record, the lone record is the donor and it is the first-person camera");
        Scene now;      // the cohort flagged on this route: the tool is counted, not recorded, and the world's writers stand in
        auto cohort = now.pool(now.gbuf, .0675f); cohort.firstPersonCohort = true; now.see(cohort);
        const auto world = now.writer(); now.see(world); now.see(now.writer());
        const auto right = now.donor(now.lighting());
        expect(now.p->sourcesUsed == 0 && now.p->firstPersonDraws == 1 && flatRuntimeFirstPerson(*now.p, now.depth),
               "a flagged tool joins no source record and is counted as first person on the depth");
        expect(right && right.role == Role::Witness && right.cameraHash == world.key.cameraHash && right.poolRecords == 0,
               "the lighting pass's donor is then the world's witness camera, not the tool's");
    }
    {
        Scene w;   // records present: a valid witness entry does not stand in, as before
        w.see(w.writer()); w.see(w.pool(w.gbuf)); w.see(w.pool(w.gbuf, .0675f));
        const auto d = w.donor(w.lighting());
        expect(!d && d.why == Why::PoolRule && d.poolSelectable && d.role == Role::None, "with records on the depth the witness is not asked");
    }

    // ---- wiring ---------------------------------------------------------------------------------------------------------------------------
    std::ifstream in("src/d3d11/flat_runtime.cpp"); std::string source, line;
    while (std::getline(in, line)) source += line + '\n';
    const auto verifier = deferred_witness_test::between(source, "const FlatProjectionBindingPlan* verifyDeferredHdrCamera(", "void reportHdrProjectionCapture(");
    const auto picker = deferred_witness_test::between(verifier, "const auto donor=[&]()->FlatDeferredDonor {", "const auto match=donor && copied");
    const auto pool = deferred_witness_test::between(picker, "if(resolved.poolSelectable) {", "if(!qualifiedPass){pick=SecondCameraPick::Unqualified;return resolved;}");
    expect(!pool.empty() && picker.find("if(!resolved.pairSelectable && !resolved.poolSelectable)return resolved;") != std::string::npos &&
           picker.find("const bool qualifiedPass=actual && input && copied && !foreignWork.load(std::memory_order_acquire);") != std::string::npos,
           "the pool pick runs before the pair pick, on the same qualified pass");
    expect(pool.find("if(qualifiedPass)") != std::string::npos && pool.find("i<resolved.poolCameraCount") != std::string::npos &&
           pool.find("holds(resolved.poolCameras[i])") != std::string::npos &&
           pool.find("held==1?PoolCameraPick::One") != std::string::npos &&
           pool.find("if(poolPick!=PoolCameraPick::One)return resolved;") != std::string::npos &&
           pool.find("return flatRuntimeDeferredDonor(s.prefix,k,s.prefix.sequence+1,gbuffer,named);") != std::string::npos,
           "every record's camera is asked, and only exactly one holding names the donor back to the resolver");
    expect(deferred_witness_test::countOf(pool, "flatRuntimeDeferredDonor(") == 1 && pool.find("certified") == std::string::npos && pool.find("draw.") == std::string::npos,
           "the pool pick touches no certified upload and no draw");
    expect(deferred_witness_test::countOf(source, "pool-cameras=%u pool-pick=%s pool-picks=one:%llu,several:%llu,none:%llu,unqualified:%llu") == 1 &&
           verifier.find("resolved.poolCameraCount,poolCameraPickName(poolPick),") != std::string::npos,
           "the HDR camera line says how many records stood on the depth and which camera held");
    // The HDR route's selection marks a frame with a flagged cohort on H's depth mixed-camera, so the foreground contract runs.
    {
        std::ifstream hin("src/d3d11/flat_hdr_route.h"); std::string hdr, hl;
        while (std::getline(hin, hl)) hdr += hl + '\n';
        const auto at = deferred_witness_test::between(hdr, "inline FlatMonoFrame flatSelectHdrRouteAt(", "inline FlatMonoFrame flatSelectHdrRoute(");
        const size_t select = at.find("out = flatSelectHdrFrame(in, f.trigger.hdr, consumerSeq, gate);");
        const size_t mixed = at.find("if (out.selected() && flatRuntimeFirstPerson(p, target->writes.key.depth)) out.mixedCamera = true;");
        expect(select != std::string::npos && mixed != std::string::npos && select < mixed,
               "the HDR route's selection marks a frame with first-person draws on H's depth mixed-camera, after selecting");
    }
    std::printf("flat deferred pool pick: %u failure(s)\n", failures);
    return failures;
}
}
