#pragma once

namespace edvr {

// SMAA 1x over the HUD layer's finished 8-bit content, in the flat profile, before the composite downsamples it (fix.hud_antialiasing;
// docs/design-flat-ui-quality-2026-10-05.md, "2026-10-24: edge smoothing for the HUD layer"). The layer is composited after the game's anti-aliasing, so
// the game's own SMAA or DLSS never sees the HUD; this runs the same SMAA on the layer image instead.
//
// The three passes are the map's (map_smaa_shader.h): the same prologue, the same SMAA chunks (SMAA.hlsl, unmodified, SMAA_PRESET_HIGH), and for
// passes 2 and 3 the map's own entries (smaaWeights, smaaBlend), which are plain SMAA with no colour transform. Only the edge test differs, and it is
// the one entry here. The layer holds premultiplied light in rgb and TRANSMITTANCE in alpha, cleared to (0, 0, 0, 1) and composited as
// out = L.rgb + F.rgb * L.a: a dark opaque backing is rgb 0 and a 0 against the clear's rgb 0 and a 1, so only alpha shows that edge. SMAA's own colour
// edge test looks at rgb alone; this one takes the colour distance over all four channels. Everything else in it -- the threshold, the steps, the local
// contrast adaptation -- is SMAAColorEdgeDetectionPS line for line (SMAA.hlsl), so SMAA.hlsl itself stays as upstream wrote it.
//
// The blend writes all four channels with no transform: a linear mix of premultiplied rgb and transmittance is what the composite's own box filter
// does to them, so it is exact for this composite. The layer is read with Load (the point sampler's clamp becomes an explicit clamp).
//
// Tile-culled indirect dispatch (32x8 tiles): the HUD typically covers only 5-15% of the screen, so pass 1 scans all 32x8 tiles, skips edge writes on
// persistently empty tiles, and appends only active edge tiles to the pass-2 indirect list and active content/edge/neighbor/previously-dirty tiles to the
// pass-3 indirect list. Pass 2 (weights) and pass 3 (blend) run via DispatchIndirect over only those active tiles, producing bit-identical output to
// full-screen SMAA while cutting memory traffic ~4x at 5120x1440.
inline constexpr char kUiLayerSmaaEdgesHlsl[] = R"HLSL(
Texture2D UiSmaaLayer : register(t0);
StructuredBuffer<uint> UiSmaaPrevOut : register(t1);
RWTexture2D<float2> UiSmaaEdges : register(u0);
RWStructuredBuffer<uint> UiSmaaTileEdge : register(u1);
RWByteAddressBuffer UiSmaaArgs : register(u2);
RWStructuredBuffer<uint> UiSmaaWeightTiles : register(u3);
RWStructuredBuffer<uint> UiSmaaBlendTiles : register(u4);
RWStructuredBuffer<uint> UiSmaaBlendMask : register(u5);

groupshared uint g_smaaFlags;
groupshared uint g_smaaPrevEdge;

float4 uiSmaaTexel(int2 p) {
    return UiSmaaLayer.Load(int3(clamp(p, int2(0, 0), int2(smaaMetrics.zw) - 1), 0));
}

float uiSmaaDelta(float4 a, float4 b) {
    const float4 t = abs(a - b);
    return max(max(t.r, t.g), max(t.b, t.a));
}

float2 uiSmaaEdgeTest(int2 p, float4 C) {
    const float2 threshold = float2(SMAA_THRESHOLD, SMAA_THRESHOLD);
    float4 delta;
    delta.x = uiSmaaDelta(C, uiSmaaTexel(p + int2(-1, 0)));
    delta.y = uiSmaaDelta(C, uiSmaaTexel(p + int2(0, -1)));
    float2 edges = step(threshold, delta.xy);
    if (dot(edges, float2(1.0, 1.0)) == 0.0)
        return float2(0.0, 0.0);
    delta.z = uiSmaaDelta(C, uiSmaaTexel(p + int2(1, 0)));
    delta.w = uiSmaaDelta(C, uiSmaaTexel(p + int2(0, 1)));
    float2 maxDelta = max(delta.xy, delta.zw);
    delta.z = uiSmaaDelta(C, uiSmaaTexel(p + int2(-2, 0)));
    delta.w = uiSmaaDelta(C, uiSmaaTexel(p + int2(0, -2)));
    maxDelta = max(maxDelta.xy, delta.zw);
    const float finalDelta = max(maxDelta.x, maxDelta.y);
    edges.xy *= step(finalDelta, SMAA_LOCAL_CONTRAST_ADAPTATION_FACTOR * delta.xy);
    return edges;
}

void uiSmaaMarkBlendTile(uint idx) {
    const uint bit = 1u << (idx & 31u);
    uint prev = 0u;
    InterlockedOr(UiSmaaBlendMask[idx >> 5], bit, prev);
    if ((prev & bit) == 0u) {
        uint slot = 0u;
        UiSmaaArgs.InterlockedAdd(12u, 1u, slot);
        UiSmaaBlendTiles[slot] = idx;
    }
}

[numthreads(32, 8, 1)]
void uiSmaaEdges(uint3 id : SV_DispatchThreadID, uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID) {
    const uint W = uint(smaaMetrics.z);
    const uint H = uint(smaaMetrics.w);
    const uint tilesX = (W + 31u) >> 5;
    const uint tileIdx = gid.y * tilesX + gid.x;
    if (gtid.x == 0u && gtid.y == 0u) {
        g_smaaFlags = 0u;
        g_smaaPrevEdge = UiSmaaTileEdge[tileIdx];
    }
    GroupMemoryBarrierWithGroupSync();

    float2 e = float2(0.0, 0.0);
    const bool inBounds = id.x < W && id.y < H;
    if (inBounds) {
        const float4 C = uiSmaaTexel(int2(id.xy));
        e = uiSmaaEdgeTest(int2(id.xy), C);
        uint f = 0u;
        if (e.x > 0.0 || e.y > 0.0) {
            f |= 1u;
            if (gtid.x == 0u) f |= 4u;
            if (gtid.y == 0u) f |= 8u;
        }
        if (any(C != float4(0.0, 0.0, 0.0, 1.0))) f |= 2u;
        if (f != 0u) InterlockedOr(g_smaaFlags, f);
    }
    GroupMemoryBarrierWithGroupSync();

    const uint flags = g_smaaFlags;
    if (inBounds && ((flags & 1u) != 0u || g_smaaPrevEdge != 0u)) {
        UiSmaaEdges[id.xy] = e;
    }
    if (gtid.x == 0u && gtid.y == 0u) {
        const uint hasEdge = flags & 1u;
        if (hasEdge != g_smaaPrevEdge) UiSmaaTileEdge[tileIdx] = hasEdge;
        if (hasEdge != 0u) {
            uint slot = 0u;
            UiSmaaArgs.InterlockedAdd(0u, 1u, slot);
            UiSmaaWeightTiles[slot] = tileIdx;
        }
        if ((flags & 3u) != 0u || UiSmaaPrevOut[tileIdx] != 0u) uiSmaaMarkBlendTile(tileIdx);
        if ((flags & 4u) != 0u && gid.x > 0u) uiSmaaMarkBlendTile(tileIdx - 1u);
        if ((flags & 8u) != 0u && gid.y > 0u) uiSmaaMarkBlendTile(tileIdx - tilesX);
    }
}
)HLSL";

inline constexpr char kUiLayerSmaaWeightsHlsl[] = R"HLSL(
Texture2D UiSmaaEdgesIn : register(t0);
Texture2D UiSmaaArea : register(t1);
Texture2D UiSmaaSearch : register(t2);
StructuredBuffer<uint> UiSmaaWeightTilesIn : register(t3);
RWTexture2D<float4> UiSmaaWeights : register(u0);

[numthreads(32, 8, 1)]
void uiSmaaWeights(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID) {
    const uint W = uint(smaaMetrics.z);
    const uint H = uint(smaaMetrics.w);
    const uint tilesX = (W + 31u) >> 5;
    const uint tileIdx = UiSmaaWeightTilesIn[gid.x];
    const uint tx = tileIdx % tilesX;
    const uint ty = tileIdx / tilesX;
    const uint2 p = uint2(tx * 32u + gtid.x, ty * 8u + gtid.y);
    if (p.x >= W || p.y >= H) return;
    const float2 texcoord = (float2(p) + 0.5) * smaaMetrics.xy;
    float2 pixcoord;
    float4 offset[3];
    SMAABlendingWeightCalculationVS(texcoord, pixcoord, offset);
    UiSmaaWeights[p] = SMAABlendingWeightCalculationPS(texcoord, pixcoord, offset, UiSmaaEdgesIn, UiSmaaArea, UiSmaaSearch,
                                                       float4(0.0, 0.0, 0.0, 0.0));
}
)HLSL";

inline constexpr char kUiLayerSmaaBlendHlsl[] = R"HLSL(
Texture2D UiSmaaColorIn : register(t0);
Texture2D UiSmaaWeightsIn : register(t1);
StructuredBuffer<uint> UiSmaaBlendTilesIn : register(t2);
StructuredBuffer<uint> UiSmaaTileEdgeIn : register(t3);
RWTexture2D<float4> UiSmaaBlended : register(u0);
RWStructuredBuffer<uint> UiSmaaDirtyOut : register(u1);

groupshared uint g_smaaNonClear;

[numthreads(32, 8, 1)]
void uiSmaaBlend(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID) {
    const uint W = uint(smaaMetrics.z);
    const uint H = uint(smaaMetrics.w);
    const uint tilesX = (W + 31u) >> 5;
    const uint tilesY = (H + 7u) >> 3;
    const uint tileIdx = UiSmaaBlendTilesIn[gid.x];
    const uint tx = tileIdx % tilesX;
    const uint ty = tileIdx / tilesX;
    if (gtid.x == 0u && gtid.y == 0u) g_smaaNonClear = 0u;
    GroupMemoryBarrierWithGroupSync();

    const uint2 p = uint2(tx * 32u + gtid.x, ty * 8u + gtid.y);
    if (p.x < W && p.y < H) {
        const uint eHere = UiSmaaTileEdgeIn[tileIdx];
        const uint eRight = (tx + 1u < tilesX) ? UiSmaaTileEdgeIn[tileIdx + 1u] : 0u;
        const uint eDown = (ty + 1u < tilesY) ? UiSmaaTileEdgeIn[tileIdx + tilesX] : 0u;
        float4 outColor;
        if ((eHere | eRight | eDown) == 0u) {
            outColor = UiSmaaColorIn.Load(int3(p, 0));
        } else {
            const int2 pR = int2(min(p.x + 1u, W - 1u), p.y);
            const uint hasR = ((uint(pR.x) >> 5) != tx) ? eRight : eHere;
            const int2 pD = int2(p.x, min(p.y + 1u, H - 1u));
            const uint hasD = ((uint(pD.y) >> 3) != ty) ? eDown : eHere;
            float4 a;
            a.x = hasR ? UiSmaaWeightsIn.Load(int3(pR, 0)).a : 0.0;
            a.y = hasD ? UiSmaaWeightsIn.Load(int3(pD, 0)).g : 0.0;
            a.wz = eHere ? UiSmaaWeightsIn.Load(int3(p, 0)).xz : float2(0.0, 0.0);
            SMAA_BRANCH
            if (dot(a, float4(1.0, 1.0, 1.0, 1.0)) < 1e-5) {
                outColor = UiSmaaColorIn.Load(int3(p, 0));
            } else {
                const float2 texcoord = (float2(p) + 0.5) * smaaMetrics.xy;
                const bool h = max(a.x, a.z) > max(a.y, a.w);
                float4 blendingOffset = float4(0.0, a.y, 0.0, a.w);
                float2 blendingWeight = a.yw;
                SMAAMovc(bool4(h, h, h, h), blendingOffset, float4(a.x, 0.0, a.z, 0.0));
                SMAAMovc(bool2(h, h), blendingWeight, a.xz);
                blendingWeight /= dot(blendingWeight, float2(1.0, 1.0));
                const float4 blendingCoord = mad(blendingOffset, float4(smaaMetrics.xy, -smaaMetrics.xy), texcoord.xyxy);
                outColor = blendingWeight.x * UiSmaaColorIn.SampleLevel(LinearSampler, blendingCoord.xy, 0);
                outColor += blendingWeight.y * UiSmaaColorIn.SampleLevel(LinearSampler, blendingCoord.zw, 0);
            }
        }
        UiSmaaBlended[p] = outColor;
        if (any(outColor != float4(0.0, 0.0, 0.0, 1.0))) InterlockedOr(g_smaaNonClear, 1u);
    }
    GroupMemoryBarrierWithGroupSync();

    if (gtid.x == 0u && gtid.y == 0u) UiSmaaDirtyOut[tileIdx] = g_smaaNonClear;
}
)HLSL";

}  // namespace edvr
