// Pure shader-family declarations shared by the motion producer and its
// metadata tests. A declared pair is not proof that a runtime patch succeeded.
#pragma once
#include <cstdint>

namespace edvr {
namespace engine_velocity_family {
struct Family {
    uint64_t vs;
    const char* name;
    uint64_t ps[8];
    bool flatOnly = false;
    uint64_t flatPs = 0;
    bool vrOnly = false;   // keyed in the VR profiles alone (see the 2026-10-08 rows below)
    // F2 (dxbc_skin_clone.h): the vertex shader has the game's skinning chain and takes the second skin in VR; the pairs that also
    // export E to target 7 are kSkinPairs below. Never in the flat profile.
    bool skinned = false;
    // The register the second skin's E travels in, for a pair whose pixel shader already reads it: the vertex and pixel stages must agree on the
    // register (a pixel-side remap gives wrong E on WARP, skin_clone_test K4b), and the vertex shader's own next free one (one past its last
    // output) is a system value's input register in the hair's pixel shader. 0 = the vertex shader's next free register.
    uint32_t skinRegister = 0;
    // Keyed for the motion producer, but not a flat scene source. An NPC body draws the world camera's depth with its own rows, and counting
    // it as a source makes every concourse frame source-camera-or-depth-not-unique (Epic edvr_gfx_20261120_095243).
    bool flatSource = true;
};
constexpr Family kFamilies[] = {
    // 054658: both additional PSs draw certified moving station records;
    // the real corpus preserves their G-buffer/depth and exports exact MRT6.
    // Not E06E61AE, DCCCEDD6 or AD6196D0 (keyed and removed 2026-11-20): the census showed them on this vertex shader, but
    // each writes EDVR's o6 marker, so they are this family's generated substitutes (kSelfMarking's note), not game materials.
    {0xEB5234DB6ADB491Dull, "vs_EB5234DB6ADB491D", {0xCB9F297EFF264251ull, 0x9ABF60B4B51F2C1Full, 0x3434972DB5336AA4ull, 0xDC603C35BBE74B31ull, 0x63B1524A9F805A4Cull}},
    {0x5B4D8E894EEDA8B4ull, "vs_5B4D8E894EEDA8B4", {0x4375B72964F386CDull, 0, 0, 0}},
    {0xBBE58E40FE88EC80ull, "vs_BBE58E40FE88EC80", {0xDB3E8D20CF53FBC0ull, 0, 0, 0}},
    // PS91 needs a separate rasterizer-position input beside SV_IsFrontFace.
    // The captured pair is draw-qualified on WARP; VR remains unqualified.
    // 054658: 03B17 draws the station's moving rig; its survivors and discard
    // branch are qualified against the original on WARP.
    // 2026-11-08 (a flat report of DLSS dropping on a planet once the SRV left the view; Sean's Frontier 11:03 trace):
    // 60D69AB6 is a stock G-buffer material of this family (four targets, normal-mapped PBR), the only stock draw of the
    // refused frames, with the HDR world camera's exact bytes and its depth. Unkeyed, a view holding it and no keyed draw
    // was refused no-supported-motion-source-pair. Both profiles. Hash recomputed from the dumped bytecode (log.cpp
    // fnv1a64); passes the corpus harness whole on WARP. E364F563 (the reporter's) stays a candidate in the harness.
    // 2026-11-20 (Epic bar, docs/fsr-upscaler-design-2026-09-16.md journal "plants and lights named"): A6070F9D is this family's
    // alpha-cutout material (a discard), the station's plants. Unkeyed, it overdrew keyed surfaces with no slot of its own, so its
    // pixels were stale slots the depth check refused on alternate jitter phases and the raw frame showed: the plants shimmered.
    // Passes the corpus harness whole on WARP (survivors and discard). Both profiles. 430AF34F and 3AF0C44D, seen beside it in
    // the census, write EDVR's own o6 marker: they are this family's generated substitutes (kSelfMarking's note), not game variants.
    {0xDE545DC8EE4FBB87ull, "vs_DE545DC8EE4FBB87", {0xE46E3E4832B2FDB0ull, 0xCB429E043DBB2506ull, 0x03B17F89B31C4788ull, 0x60D69AB6C65C549Aull,
        0xA6070F9DD1CFB601ull, 0}, false, 0x91F8937EDA723663ull},
    // 2026-11-20 (Epic bar, docs/fsr-upscaler-design-2026-09-16.md journal): the light strips under the bar, 6-index quads into the
    // G-buffer with depth write on. The same instance-record inputs as DE54 (INSTANCEANDMODELDATAINDEX at v0) and a pixel shader
    // with the record id at v0, four targets and no discard. Unkeyed, the strips overdrew keyed surfaces with no slot of their own and
    // shimmered as the plants did. Both profiles.
    {0x2684F02B9B0BB0DEull, "vs_2684F02B9B0BB0DE", {0x2376A8D9AA874372ull, 0}},
    // 2026-11-16 (issue #230, a flat report with EDHM): CAD1F585 is EDHM's build of CF534B32 (it adds a t120 parameter strip). With EDHM
    // every draw of this vertex shader uses it, world objects and the held tool alike, so unkeyed it left an on-foot view with no ship in
    // sight without a supported draw, and the tool's glow pass was refused hdr-camera-changed on the copy route. Patched, reflected,
    // created and identical in o0..o3 and depth on WARP with a 1D dummy at t120. Both profiles.
    {0xAACFDCF2FB9AD809ull, "vs_AACFDCF2FB9AD809", {0xCF534B32F491561Aull, 0xCAD1F585EDDC5641ull, 0, 0}},
    // 2026-09-29, the Krait's main-menu F10 capture: 235567BE is the stock pixel
    // shader of the hull PLATING (5 draws, ~70k vertices). Unkeyed, its draws wrote
    // depth over pool records without writing a slot, so 22.5% of the frame had a
    // stale slot, engineBefore returned "reject", and finish() showed the raw
    // jittered colour there: the panel lines crawled. It is the family's third
    // keyed pixel shader, structurally its sibling 864F1F94 (four targets, no
    // discard, no depth output); the real-corpus harness passes it whole (patched,
    // reflected, created, o0..o3 and depth bit-identical on WARP). Flat-only
    // (flatPs): VR never keyed it and stays as it was. Left for later, not keyed:
    // BBE58E40/7311054A (an SV_Position input); the flat census names it when it
    // draws. (AACFDCF2/CAD1F585, EDHM's build, was keyed 2026-11-16, above.)
    {0x66DE2CADB1F4AE6Bull, "vs_66DE2CADB1F4AE6B", {0x864F1F949851B8DEull, 0xBBDE4E71FB78528Aull, 0, 0}, false, 0x235567BE2840B3EDull},
    {0x61AE8EB05FDC18DDull, "vs_61AE8EB05FDC18DD", {0xFC43E42710010343ull, 0x451A82D4DD1BA254ull, 0x4504BC268E109C31ull, 0}, false, 0, false, true},
    {0x436193B352A2897Eull, "vs_436193B352A2897E", {0x16940F576006BE65ull, 0x51EE1F922FD220B0ull, 0, 0}},
    {0x889A5279E68F0672ull, "vs_889A5279E68F0672", {0xB46E52A1E0B2F39Cull, 0xEBA95E15B0A66102ull, 0xD31DCAFA7C05CB47ull, 0}},
    // Epic flat cockpit shell: the real shader corpus proves the exported
    // t33 slot and byte-identical G-buffer/depth after the MRT6 patch.
    {0xBFE51414CC3024B4ull, "vs_BFE51414CC3024B4", {0xDB79AE788E049DFDull, 0, 0, 0}, true},
    // 2026-10-08, F10 (6f47ea53, Explorer Cam, a walking settlement NPC; the F10 entries of
    // docs/kinematic-motion-injection-2026-09-19.md). The NPC is a rigid body (17 base-0 records, read by 15 draws of
    // 8B58 and 2 of 7B0D, certified joined, one rigid body to 0.2-0.8 mm) plus a skinned body (20 base != 0 records
    // read by 7B0D, 114A and D99A draws, no previous pose by design). None of these pairs wrote MRT6, so 38.7% of the NPC's pixels
    // (D3) kept a background slot under them (kind 4, stale, refused by the world route) and the rigid body had no
    // motion. Keyed, the rigid records join (kind 1) and a skinned record lands in kind 3 (the camera term stands,
    // no refusal): the emit leaves base != 0 records native, so they carry no marker. Each hash was recomputed
    // (FNV-1a 64, log.cpp fnv1a64) from the dumped bytecode and each pair passed the corpus_identity harness on
    // WARP: patched, created, reflected, o0..o3 and depth bit-identical, MRT6 = 2 slot + 1.
    // 8B58 stays VR-only: it is weapon_motion's first-person family and the flat profile switch test's VR-only row, and the
    // 2026-11-20 concourse did not create it. 7B0D, 114A and D99A are keyed on flat too (Epic concourse edvr_gfx_20261120_082441).
    // 7B0D and 114A are also weapon vertex shaders; flat's foreground candidate ignores a draw that shares the world camera,
    // so an NPC body is not captured as the gun.
    {0x8B589D25B2A0ADDCull, "vs_8B589D25B2A0ADDC", {0x7268762D11A610F2ull, 0, 0, 0}, false, 0, true, true},
    {0x7B0DC42D383F694Cull, "vs_7B0DC42D383F694C", {0x0DF03E64DF9DBEF1ull, 0, 0, 0}, false, 0, false, true, 0, false},
    {0x114AF608F86D9ED8ull, "vs_114AF608F86D9ED8", {0xA17504A2627767F2ull, 0, 0, 0}, false, 0, false, true, 0, false},
    {0xD99AFDC250D19A3Full, "vs_D99AFDC250D19A3F", {0xE86271E464CCDC1Dull, 0, 0, 0}, false, 0, false, true, 0, false},
    // 2026-11-01, an NPC's hair (Frontier 15:43 flight, eye runs 154737-154858; docs/kinematic-motion-injection-2026-09-19.md, "NPC hair"). Its
    // pixels carried no slot and no E while the face beside them did, so a turning head moved the face and left the hair at the camera term
    // (about 5 px a frame of error). The hair is drawn by an alpha-tested depth prepass (vs 3186/ps 4EBA, depth only) and then this forward
    // pass: the skinning chain of D99A and its siblings, one colour output, blending off, depth write on. The pixel shader
    // reads SV_IsFrontFace at v7, the register the vertex shader's E would take, so E travels in v8 for this family (skinRegister).
    // Flat draws a different pixel shader (ED91F94E, Epic concourse 2026-11-20); that companion is flatPs. The VR shader stays 1AE6.
    {0xA6A39338C06E03A1ull, "vs_A6A39338C06E03A1", {0x1AE6AB9A94C0456Eull, 0, 0, 0}, false, 0xED91F94EC94FA5C2ull, false, true, 8, false},
    // 2026-11-05, the commander's suit, helmet, headset and collar on the Holo-Me screen (Frontier 12:48, eye run 125144765001, the ship stopped;
    // docs/vr-holo-me-head-flick-2026-11-05.md). No slot and no E, so these pixels took the head path while the face beside them had E: their motion
    // error follows the head's own motion (r 0.985), up to 1.75 input px a frame, and the helmet's thin lines shimmer. The same class as the hair above.
    // Keyed on flat as well (the same pixel shader, Epic concourse 2026-11-20).
    {0x01DA82F8D0EA0C99ull, "vs_01DA82F8D0EA0C99", {0x2C3ADCD2FCD47298ull, 0, 0, 0}, false, 0, false, true, 0, false},
    // 2026-11-05, an NPC's eyes (Frontier 14:37, eye run 143722980001, a bartender; docs/vr-npc-eye-motion-2026-11-05.md). No slot and no E, so these
    // pixels took the world path while the face round them had E: 2.8 to 7.7 native px a frame apart while the head moved. Drawn by a depth prepass
    // (vs F516/ps B40B, depth only) and then this forward pass, the shape of the hair above. Its 432- and 168-index meshes are not identified.
    // Still VR-only: the 2026-11-20 flat concourse created the vertex shader and neither keyed pixel shader (7AA0, nor the flat recipe's 6B8C).
    {0x33A5025C48FC8259ull, "vs_33A5025C48FC8259", {0x7AA0441EE0E88667ull, 0, 0, 0}, false, 0, true, true},
};
constexpr int kFamilyCount = static_cast<int>(sizeof(kFamilies) / sizeof(kFamilies[0]));
// F2: the five measured skinned pairs (the NPC's shading pairs, 2026-10-08 F10/F11): the pixel shader exports the vertex shader's E to
// target 7. A skinned family's other keyed pixel shaders (61AE with FC43 and 4504) keep their slot export and write nothing to target 7.
struct SkinPair { uint64_t vs, ps; };
constexpr SkinPair kSkinPairs[] = {
    {0xD99AFDC250D19A3Full, 0xE86271E464CCDC1Dull},
    {0x61AE8EB05FDC18DDull, 0x451A82D4DD1BA254ull},
    {0x114AF608F86D9ED8ull, 0xA17504A2627767F2ull},
    {0x7B0DC42D383F694Cull, 0x0DF03E64DF9DBEF1ull},
    {0x8B589D25B2A0ADDCull, 0x7268762D11A610F2ull},
    {0xA6A39338C06E03A1ull, 0x1AE6AB9A94C0456Eull},
    {0x01DA82F8D0EA0C99ull, 0x2C3ADCD2FCD47298ull},
    {0x33A5025C48FC8259ull, 0x7AA0441EE0E88667ull},
};
constexpr bool skinPair(uint64_t vs, uint64_t ps) noexcept {
    for (const SkinPair& p : kSkinPairs)
        if (p.vs == vs && p.ps == ps) return true;
    return false;
}
// Historical marker-bearing pairs retained for compatibility. PROVENANCE
// CORRECTION, 2026-09-28: every PS below is EDVR's generated substitution,
// not a native game shader. Re-running engineVelocityPatchPs on the captured
// originals reproduces these exact creation-byte hashes:
//   51EE1F922FD220B0 -> BCF75CEA37060EAE
//   D31DCAFA7C05CB47 -> 2F924695596C8195
//   DB3E8D20CF53FBC0 -> 25CC28229319DFA8
//   CF534B32F491561A -> C4835018A5128866
//   FC43E42710010343 -> C21E17F391CC04AF
// The device's creation hook dumps/registers EDVR shaders too. A census
// reading these hashes live while the PS hook sees zero binds is expected
// substitution, not evidence of a bypass. Do not key them as game variants.
// See docs/kinematic-motion-injection-2026-09-19.md, 2026-09-28 investigation.
struct SelfMarking {
    uint64_t vs, ps;
};
constexpr SelfMarking kSelfMarking[] = {
    {0x436193B352A2897Eull, 0xBCF75CEA37060EAEull},
    {0x889A5279E68F0672ull, 0x2F924695596C8195ull},
    // The 15:46 session's marker-bearing draws: these are also EDVR's
    // generated shaders, as the exact lineage above proves.
    {0xBBE58E40FE88EC80ull, 0x25CC28229319DFA8ull},
    {0xAACFDCF2FB9AD809ull, 0xC4835018A5128866ull},
    {0x61AE8EB05FDC18DDull, 0xC21E17F391CC04AFull},
};
constexpr bool selfMarkingPair(uint64_t vs, uint64_t ps) noexcept {
    for (const SelfMarking& p : kSelfMarking)
        if (p.vs == vs && p.ps == ps) return true;
    return false;
}
// Any keyed or self-marking pixel shader hash: the draw path's probe gate.
constexpr bool anyFamilyPs(uint64_t ps) noexcept {
    for (const Family& f : kFamilies)
        for (uint64_t h : f.ps)
            if (h == ps) return true;
    for (const SelfMarking& p : kSelfMarking)
        if (p.ps == ps) return true;
    return false;
}
// The PS half alone: the shader hook's census of their binds (the draw path
// never saw 095337's seam draws; the bind count separates "never bound through
// the hook" from "bound but never drawn through it" in one flight).
constexpr bool selfMarkingPs(uint64_t ps) noexcept {
    for (const SelfMarking& p : kSelfMarking)
        if (p.ps == ps) return true;
    return false;
}
inline int familyOfVs(uint64_t hash) noexcept {
    for (int i = 0; i < kFamilyCount; ++i) if (kFamilies[i].vs == hash) return i;
    return -1;
}
inline int familyForProfile(uint64_t hash, bool flat) noexcept {
    const int family = familyOfVs(hash);
    return family >= 0 && (!kFamilies[family].flatOnly || flat) && (!kFamilies[family].vrOnly || !flat) ? family : -1;
}
inline bool keyedPs(int family, uint64_t hash, bool flat = false) noexcept {
    if (family < 0 || family >= kFamilyCount || !hash) return false;
    if (flat && kFamilies[family].vrOnly) return false;
    for (uint64_t h : kFamilies[family].ps) if (h == hash) return true;
    return flat && kFamilies[family].flatPs == hash;
}
// A flat scene source. Motion-only families (flatSource false) stay keyed for the producer and are not one.
inline bool flatSceneSource(int family) noexcept {
    return family >= 0 && family < kFamilyCount && kFamilies[family].flatSource;
}
inline bool supportedPair(uint64_t vs, uint64_t ps) noexcept {
    // Flat recipe/metadata consumers include pairs qualified only for flat.
    const int family = familyOfVs(vs);
    return keyedPs(family, ps, true) && flatSceneSource(family);
}
}  // namespace engine_velocity_family
}  // namespace edvr
