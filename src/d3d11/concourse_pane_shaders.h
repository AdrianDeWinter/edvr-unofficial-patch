// The concourse cull's runtime shaders as text, built from the cull core's own constants and HLSL pieces, so a number the CPU side uses (the cube size, the
// distances) is the number the shader has. Every text here was compiled at runtime on the render thread (concourse_pane.cpp: D3DCompile with flags 0, no macros);
// since the perf audit 2022 they are compiled at build time instead (tools\temporal_shader_build, kConcourse*Bytecode in temporal_shader_bytecode.h) and the
// proxy creates them from the bytes (shaderSwapCreateCs / shaderSwapCreatePs). The text, the entry point ("main"), the profile (cs_5_0 / ps_5_0) and the flags (0) are
// the ones the runtime compile used; this header is the former assembly code moved unchanged. The mark pass's vertex shader (ensureMarkVs) is not here: it carries
// the user's beyond distance as a literal, so it stays a runtime compile.
#pragma once

#include <cstdio>
#include <string>

#include "concourse_cull_core.h"
#include "concourse_vr_bake_core.h"

namespace edvr::concourse_shader {

namespace cc = edvr::concourse;
namespace cv = edvr::concourse_vr;

// A float as the shaders spell it: nine significant digits, so the text round-trips the constant exactly.
static std::string bdFloatText(double v) {
    char b[40];
    std::snprintf(b, sizeof(b), "%.9g", v);
    return b;
}

// The steady colour of a pane from its lighting normal and b2 (up, the levels, the colour): shared by the steady shader and the backdrop's shaders, so they are the same text.
inline constexpr char kSkyCommon[] =
    "cbuffer GlowCb : register(b2) { float4 up; float4 lv; float4 colour; float4 par; float4 glassA; float4 glassB; };\n"
    "float3 fallbackColour(float3 n) {\n"
    "    float ul = length(up.xyz);\n"
    "    float3 nn = n * rsqrt(max(dot(n, n), 1e-12));\n"
    "    float d = ul > 0.5 ? abs(dot(nn, up.xyz / ul)) : 0.0;\n"
    "    float lvl = lerp(lv.z, lv.y, smoothstep(0.35, 0.6, d));\n"
    "    lvl = lerp(lvl, lv.x, smoothstep(0.92, 0.98, d));\n"
    "    return colour.rgb * lvl;\n"
    "}\n";

// The white pixel shader (concourse_pane_white): tint multiplies it.
inline std::string whiteHlsl() { return "float4 main() : SV_Target { return float4(1.0, 1.0, 1.0, 1.0); }\n"; }

// The steady pane's pixel shader (concourse_pane_sky).
inline std::string skyHlsl() {
    return std::string(kSkyCommon) +
        "struct PsIn { nointerpolation uint face : __USER_VERTEX_FACEINVARIANT0; float3 n : __USER_VERTEX_M_LIGHTINGNORMAL0; };\n"
        "float4 main(PsIn i) : SV_Target {\n"
        "    return float4(fallbackColour(i.n), 1.0);\n"
        "}\n";
}

// The flat bake's compute shader (concourse_bake): one cube texel a thread, from the frame's own camera rows.
inline std::string bakeHlsl() {
    return std::string(cc::cubeDirHlsl()) +
            "cbuffer C : register(b0) { row_major float4x4 m; row_major float4x4 inv; float4 rel; float4 size; };\n"
            "Texture2D<float4> scene : register(t0);\n"
            "Texture2D<float> sceneDepth : register(t1);\n"
            "SamplerState smp : register(s0);\n"
            "RWTexture2DArray<float> distCube : register(u0);\n"
            "RWTexture2DArray<float4> cube : register(u1);\n"
            "[numthreads(32, 32, 1)]\n"
            "void main(uint3 id : SV_DispatchThreadID) {\n"
            "    float3 d = normalize(cubeDir(id.z, (float2(id.xy) + 0.5) / " + std::to_string(cc::kBackdropCubeSize) + ".0));\n"
            "    float4 c = mul(m, float4(d, 0.0));\n"
            "    if (c.w <= 1e-4) return;\n"
            "    float2 ndc = c.xy / c.w;\n"
            "    if (abs(ndc.x) >= 1.0 || abs(ndc.y) >= 1.0) return;\n"
            // Only this frame's own 90 degrees across, not all of its 121. The near plane clips a box around the centre only if each direction is taken from the frame looking along it: an
            // overlap would let a frame that clips a point overwrite the one that kept it, and draw a seam along the frame's edge.
            "    if (abs(ndc.x) * size.x / size.y > 1.002) return;\n"
            "    float2 uv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);\n"
            "    float z = sceneDepth.Load(int3(uv * size.xy, 0));\n"
            "    float t = size.w;\n"
            "    if (z > 0.0) { float4 b = mul(inv, float4(ndc, z, 1.0)); t = min(length(b.xyz / b.w), size.w); }\n"
            "    float rl = length(rel.xyz + d * t);\n"
            "    if (!(rl > size.z)) return;\n"
            "    cube[id] = float4(clamp(scene.SampleLevel(smp, uv, 0.0).rgb, 0.0, 60000.0), 1.0);\n"
            "    distCube[id] = clamp(rl, 1.0, size.w);\n"
            "}\n";
}

// The VR bake's compute shader (concourse_bake_vr).
inline std::string vrBakeHlsl() {
    const std::string n = std::to_string(cc::kBackdropCubeSize) + ".0";
    return std::string(cc::cubeDirHlsl()) + cv::inverse4Hlsl() +
        "cbuffer C : register(b0) { float4 faceR[5]; float4 faceU[5]; float4 faceF[5]; float4 origin; float4 size; float4 lim; uint4 ctl; };\n"
        "Texture2D<float4> scene : register(t0);\n"
        "Texture2D<float> sceneDepth : register(t1);\n"
        "Buffer<float4> rowsA : register(t2);\n"
        "Buffer<float4> rowsB : register(t3);\n"
        "SamplerState smp : register(s0);\n"
        "RWTexture2DArray<float> distCube : register(u0);\n"
        "RWTexture2DArray<float4> cube : register(u1);\n"
        "RWByteAddressBuffer counts : register(u2);\n"
        "groupshared float gInv[16];\n"
        "groupshared int gFace;\n"
        "groupshared uint gStatus;\n"
        "groupshared uint gCount[15];\n"
        "bool inClip(float3 d, float4 a, float4 b, float4 c, out float2 ndc) {\n"
        "    float4 h = d.x * a + d.y * b + d.z * c;\n"
        "    ndc = float2(2.0, 2.0);\n"
        "    if (h.w <= 1e-4) return false;\n"
        "    ndc = h.xy / h.w;\n"
        "    return abs(ndc.x) < 1.0 && abs(ndc.y) < 1.0;\n"
        "}\n"
        "[numthreads(32, 32, 1)]\n"
        "void main(uint3 id : SV_DispatchThreadID, uint3 gid : SV_GroupID, uint gi : SV_GroupIndex) {\n"
        "    float4 r0 = rowsA[0], r1 = rowsA[1], r2 = rowsA[2], r3 = rowsA[3];\n"
        "    float3 off = rowsA[5].xyz - origin.xyz;\n"
        "    if (gi == 0) {\n"
        "        [unroll] for (int c0 = 0; c0 < 15; ++c0) gCount[c0] = 0u;\n"
        "        float3 fw = float3(r0.w, r1.w, r2.w);\n"
        "        float fl = length(fw);\n"
        "        int best = -1;\n"
        "        float bestDot = lim.z;\n"
        "        if (fl > 1e-6) {\n"
        "            fw /= fl;\n"
        "            [unroll] for (int f = 0; f < 5; ++f) {\n"
        "                float k = dot(fw, faceF[f].xyz);\n"
        "                if ((ctl.x & (1u << (uint)f)) != 0u && k > bestDot) { bestDot = k; best = f; }\n"
        "            }\n"
        "        }\n"
        "        uint st = 1u;\n"
        "        if (!(dot(off, off) < lim.x * lim.x)) st = 2u;\n"
        "        else if (best < 0) st = 3u;\n"
        "        float m[16];\n"
        "        m[0] = r0.x; m[1] = r1.x; m[2] = r2.x; m[3] = r3.x;\n"
        "        m[4] = r0.y; m[5] = r1.y; m[6] = r2.y; m[7] = r3.y;\n"
        "        m[8] = r0.z; m[9] = r1.z; m[10] = r2.z; m[11] = r3.z;\n"
        "        m[12] = r0.w; m[13] = r1.w; m[14] = r2.w; m[15] = r3.w;\n"
        "        float inv[16];\n"
        "        [unroll] for (int z0 = 0; z0 < 16; ++z0) inv[z0] = 0.0;\n"
        "        if (st == 1u && !invert4(m, inv)) st = 4u;\n"
        "        [unroll] for (int k2 = 0; k2 < 16; ++k2) gInv[k2] = inv[k2];\n"
        "        gFace = best;\n"
        "        gStatus = st;\n"
        "    }\n"
        "    GroupMemoryBarrierWithGroupSync();\n"
        "    if (gStatus == 1u) {\n"
        "        int f = gFace;\n"
        "        float3 d = normalize(cubeDir(id.z, (float2(id.xy) + 0.5) / " + n + "));\n"
        "        float dF = dot(d, faceF[f].xyz);\n"
        "        if (dF > 0.0 && abs(dot(d, faceR[f].xyz)) <= lim.y * dF && abs(dot(d, faceU[f].xyz)) <= lim.y * dF) {\n"
        "            uint o;\n"
        "            InterlockedAdd(gCount[0], 1u, o);\n"
        "            float2 ndc, ndcB;\n"
        "            bool inA = inClip(d, r0, r1, r2, ndc);\n"
        "            bool inB = false;\n"
        "            if (ctl.y != 0u) inB = inClip(d, rowsB[0], rowsB[1], rowsB[2], ndcB);\n"
        "            if (inA || inB) InterlockedAdd(gCount[3], 1u, o);\n"
        "            if (inA) {\n"
        "                InterlockedAdd(gCount[1], 1u, o);\n"
        "                float2 uv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);\n"
        "                float z = sceneDepth.Load(int3(uv * size.xy, 0));\n"
        "                float t = size.w;\n"
        "                if (z <= 0.0) InterlockedAdd(gCount[5], 1u, o);\n"
        "                if (z > 0.0) {\n"
        "                    float4 v = float4(ndc, z, 1.0);\n"
        "                    float4 b;\n"
        "                    b.x = gInv[0] * v.x + gInv[1] * v.y + gInv[2] * v.z + gInv[3] * v.w;\n"
        "                    b.y = gInv[4] * v.x + gInv[5] * v.y + gInv[6] * v.z + gInv[7] * v.w;\n"
        "                    b.z = gInv[8] * v.x + gInv[9] * v.y + gInv[10] * v.z + gInv[11] * v.w;\n"
        "                    b.w = gInv[12] * v.x + gInv[13] * v.y + gInv[14] * v.z + gInv[15] * v.w;\n"
        "                    if (abs(b.w) > 1e-20) { float l = length(b.xyz / b.w); if (l < t) t = l; }\n"
        "                }\n"
        "                float rl = length(off + d * t);\n"
        "                if (z > 0.0) {\n"
        "                    uint bd = t < 50.0 ? 0u : t < 200.0 ? 1u : t < 1000.0 ? 2u : t < 4000.0 ? 3u : 4u;\n"
        "                    InterlockedAdd(gCount[10u + bd], 1u, o);\n"
        "                }\n"
        "                if (rl > size.z) {\n"
        "                    InterlockedAdd(gCount[4], 1u, o);\n"
        "                    if (ctl.w != 0u) {\n"
        "                        cube[id] = float4(clamp(scene.SampleLevel(smp, uv, 0.0).rgb, 0.0, 60000.0), 1.0);\n"
        "                        distCube[id] = clamp(rl, 1.0, size.w);\n"
        "                        InterlockedAdd(gCount[2], 1u, o);\n"
        "                    }\n"
        "                } else {\n"
        "                    uint cb = t < 5.0 ? 0u : t < 20.0 ? 1u : t < 50.0 ? 2u : 3u;\n"
        "                    InterlockedAdd(gCount[6u + cb], 1u, o);\n"
        "                }\n"
        "            }\n"
        "        }\n"
        "    }\n"
        "    GroupMemoryBarrierWithGroupSync();\n"
        "    if (gi == 0) {\n"
        "        uint base = ctl.z * 128u;\n"
        "        uint o2;\n"
        "        if (gStatus == 1u) {\n"
        "            [unroll] for (uint ci = 0u; ci < 15u; ++ci) {\n"
        "                uint wi = ci < 4u ? ci : ci + 2u;\n"
        "                if (gCount[ci] != 0u) counts.InterlockedAdd(base + wi * 4u, gCount[ci], o2);\n"
        "            }\n"
        "        }\n"
        "        if (gid.x == 0u && gid.y == 0u && gid.z == 0u) {\n"
        "            counts.Store(base + 16u, (uint)(gFace + 1));\n"
        "            counts.Store(base + 20u, gStatus);\n"
        "        }\n"
        "    }\n"
        "}\n";
}

// The backdrop's display and probe pixel shaders share everything up to main.
inline std::string backdropCb1() { return "cbuffer Cb1 : register(b1) { float4 cb1[327]; };\n"; }

inline std::string backdropHead() {
    const std::string cb1 = backdropCb1();
    return std::string(kSkyCommon) + cb1 +
        "struct PsIn { nointerpolation uint face : __USER_VERTEX_FACEINVARIANT0; float3 n : __USER_VERTEX_M_LIGHTINGNORMAL0; float3 wp : __USER_VERTEX_M_LIGHTINGPOSITION0; };\n"
        "TextureCube<float4> backdrop : register(t8);\n"
        "TextureCube<float> distMap : register(t9);\n"
        "Buffer<float4> originBuf : register(t10);\n"
        "SamplerState backdropSmp : register(s7);\n"
        // The composite of lods 0 1 2 3 5 7 (up to lastLod), each plus the glass's blur, front to back over premultiplied colour.
        "float4 lookup(float3 s, float blur, float lastLod) {\n"
        "    float4 acc = float4(0, 0, 0, 0);\n"
        "    static const float L[6] = {0.0, 1.0, 2.0, 3.0, 5.0, 7.0};\n"
        "    [unroll] for (int k = 0; k < 6; ++k) {\n"
        "        if (L[k] <= lastLod) {\n"
        "            float4 c = backdrop.SampleLevel(backdropSmp, s, L[k] + blur);\n"
        "            acc.rgb += (1.0 - acc.a) * c.rgb;\n"
        "            acc.a += (1.0 - acc.a) * c.a;\n"
        "        }\n"
        "    }\n"
        "    return acc;\n"
        "}\n"
        // `t` is how far along the view's ray the picture lies. In the depth mode it is a march along the ray against the distance cube: even steps in 1 / t, which are even steps of
        // parallax, until the ray passes behind what the cube holds, then a bisection. Flight 29's look-up repeated t = the distance at the last guess's direction: it does not settle at an
        // edge, so edges rippled and swam as the camera moved. A texel with no distance is not a surface.
        "float3 backdropS(PsIn i, out float3 dir, out float t) {\n"
        "    dir = i.wp * rsqrt(max(dot(i.wp, i.wp), 1e-12));\n"
        "    float3 off = cb1[275].xyz - originBuf[0].xyz;\n"
        "    float3 s = dir;\n"
        "    float mode = par.x;\n"
        "    float R = max(par.y, 1.0);\n"
        "    t = " + bdFloatText(cc::kBackdropFarM) + ";\n"
        "    if (mode > 0.5) {\n"
        "        float b = dot(off, dir);\n"
        "        float cq = dot(off, off) - R * R;\n"
        "        t = -b + sqrt(max(b * b - cq, 0.0));\n"
        "        if (mode > 1.5) {\n"
        "            const float uFar = 1.0 / " + bdFloatText(cc::kBackdropFarM) + ";\n"
        "            const float uNear = 1.0 / " + bdFloatText(cc::kBackdropMarchNearM) + ";\n"
        "            const float du = (uNear - uFar) / " + std::to_string(cc::kBackdropMarchSteps) + ".0;\n"
        "            float outside = uNear, inside = uFar;\n"
        "            bool hit = false;\n"
        "            [loop] for (int k = 1; k <= " + std::to_string(cc::kBackdropMarchSteps) + "; ++k) {\n"
        "                float u = uNear - du * k;\n"
        "                float3 rel = off + dir / u;\n"
        "                float r = distMap.SampleLevel(backdropSmp, rel, 1.0);\n"
        "                if (r >= 1.0 && dot(rel, rel) >= r * r) { inside = u; hit = true; break; }\n"
        "                outside = u;\n"
        "            }\n"
        "            t = 1.0 / uFar;\n"
        "            if (hit) {\n"
        "                [loop] for (int k = 0; k < " + std::to_string(cc::kBackdropMarchBisections) + "; ++k) {\n"
        "                    float u = 0.5 * (outside + inside);\n"
        "                    float3 rel = off + dir / u;\n"
        "                    float r = distMap.SampleLevel(backdropSmp, rel, 0.0);\n"
        "                    if (r >= 1.0 && dot(rel, rel) >= r * r) inside = u; else outside = u;\n"
        "                }\n"
        "                t = 2.0 / (outside + inside);\n"
        "            }\n"
        "        }\n"
        "        s = off + dir * t;\n"
        "    }\n"
        "    return s;\n"
        "}\n";
}

// Display (concourse_backdrop_show): glass after the composite.
inline std::string backdropShowHlsl() {
    return backdropHead() +
        "float4 main(PsIn i, out float pictureDepth : SV_Depth) : SV_Target {\n"
        "    float3 fb = fallbackColour(i.n);\n"
        "    float3 dir;\n"
        "    float t;\n"
        "    float3 s = backdropS(i, dir, t);\n"
        "    pictureDepth = cb1[273].z / max(t * dot(dir, float3(cb1[270].w, cb1[271].w, cb1[272].w)), 1.0);\n"
        "    float4 acc = lookup(s, glassB.x, 7.0);\n"
        // Flight 25: a hole took the window's level, and that is 4 on a skylight: white shapes where the bake left the room's things out. Colour over coverage takes the neighbours'
        // colour instead, and only a direction with nothing near it falls back. The sheen is the level's colour at no more than 1, so a bright window is not a washed-out one.
        "    float3 base = (acc.a > 0.02 ? acc.rgb / acc.a : fb) * glassA.rgb;\n"
        "    float3 sheen = fb / max(max(fb.r, max(fb.g, fb.b)), 1.0);\n"
        "    float f = glassB.y * pow(saturate(1.0 - abs(dot(i.n * rsqrt(max(dot(i.n, i.n), 1e-12)), dir))), 5.0);\n"
        "    return float4(lerp(base, sheen, saturate(glassA.w + f)), 1.0);\n"
        "}\n";
}

// Probe (concourse_backdrop_probe).
inline std::string backdropProbeHlsl() {
    return backdropHead() +
        "float4 main(PsIn i) : SV_Target {\n"
        "    float3 dir;\n"
        "    float t;\n"
        "    float3 s = backdropS(i, dir, t);\n"
        "    float4 acc = lookup(s, glassB.x, 3.0);\n"
        "    clip(0.5 - acc.a);\n"
        "    return float4(0, 0, 0, 0);\n"
    "}\n";
}

// Capture (concourse_backdrop_capture): an exterior pane's pixel.
inline std::string backdropCaptureHlsl() {
    const std::string cb1 = backdropCb1();
    return std::string(cc::cubeTexelHlsl()) + cb1 +
        "struct PsIn { nointerpolation uint face : __USER_VERTEX_FACEINVARIANT0; float3 n : __USER_VERTEX_M_LIGHTINGNORMAL0; float3 wp : __USER_VERTEX_M_LIGHTINGPOSITION0;\n"
        "              float3 tg : __USER_VERTEX_M_LIGHTINGTANGENT0; float2 uv : __USER_VERTEX_M_TEXCOORD0; float4 p : SV_POSITION; };\n"
        "Texture2D<float4> scene : register(t8);\n"
        "Texture2D<float> sceneDepth : register(t9);\n"
        "Buffer<float4> originBuf : register(t10);\n"
        "RWTexture2DArray<float> distCube : register(u6);\n"
        "RWTexture2DArray<float4> cube : register(u7);\n"
        "[earlydepthstencil]\n"
        "void main(PsIn i) {\n"
        "    float3 keep = (i.face == 0xDEADBEEFu) ? float3(i.tg.x, i.n.y + i.uv.x, i.n.z) : float3(0, 0, 0);\n"   // never true: keeps v3 and v4 in the signature
        "    float3 dir = i.wp + keep;\n"
        "    float dl = dot(dir, dir);\n"
        "    if (dl < 1e-12) return;\n"
        "    dir *= rsqrt(dl);\n"
        "    float z = sceneDepth.Load(int3(i.p.xy, 0));\n"
        "    float vd = z > 0.0 ? " + bdFloatText(cc::kBackdropNearZ) + " / z : " + bdFloatText(cc::kBackdropFarM) + ";\n"
        "    float t = min(vd / max(abs(dot(cb1[279].xyz, dir)), 1e-3), " + bdFloatText(cc::kBackdropFarM) + ");\n"
        "    if (t < " + bdFloatText(cc::kBackdropMinDistM) + ") return;\n"
        "    float3 rel = cb1[275].xyz + dir * t - originBuf[0].xyz;\n"
        "    float rl = length(rel);\n"
        "    if (!(rl > 1e-3)) return;\n"
        "    uint3 tx = cubeTexel(rel, " + std::to_string(cc::kBackdropCubeSize) + "u);\n"
        "    float3 c = clamp(scene.Load(int3(i.p.xy, 0)).rgb, 0.0, 60000.0);\n"
        "    cube[tx] = float4(c, 1.0);\n"
        "    distCube[tx] = clamp(rl, 1.0, " + bdFloatText(cc::kBackdropFarM) + ");\n"
        "}\n";
}

}  // namespace edvr::concourse_shader
