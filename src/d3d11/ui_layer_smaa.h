#pragma once

// The HUD layer's edge smoothing (fix.hud_antialiasing; the flat profile since 2026-10-24, VR too since 2026-11-03: docs/design-flat-ui-quality-2026-10-05.md,
// "2026-10-24: edge smoothing for the HUD layer", and docs/ui-layer-2026-09-23.md, "2026-11-03"): SMAA 1x over the layer's finished 8-bit content, so the composite that downsamples it reads smoothed edges. The shaders are
// ui_layer_smaa_shader.h's edge entry and the map's weights and blend (map_smaa_shader.h), all built offline (tools/temporal_shader_build) with SMAA.hlsl
// as upstream wrote it; the lookup textures are map_smaa.cpp's. Nothing here logs: ui_layer.cpp reads the state and the failure this returns and says it.
//
// One UiLayerSmaa belongs to one eye's layer. uiLayerSmaaEnsure makes what is missing at the layer's size (the targets are re-made when the size moves), and a
// creation that fails at a size is remembered for that size: every later call there answers kStoodDown without trying again. uiLayerSmaaRun is the three
// dispatches. Everything a pass touches on the compute stage is put back by the caller except the two samplers, which this puts back itself.

#include "map_smaa.h"
#include "temporal_shader_bytecode.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>

namespace edvr {

// What a creation that failed was doing, and the HRESULT it came back with.
struct UiLayerSmaaFail {
    const char* what = nullptr;
    HRESULT hr = S_OK;
};

enum class UiLayerSmaaState : uint8_t {
    kReady,      // usable; nothing was made this call
    kMade,       // usable; the targets were made (or re-made at a new size) this call
    kFailed,     // not usable; this call's creation failed (UiLayerSmaaFail says what)
    kStoodDown,  // not usable; a creation already failed at this size and was said
};

struct UiLayerSmaa {
    template <class T>
    using Ptr = Microsoft::WRL::ComPtr<T>;
    // Per device: the passes, the two samplers and SMAA's immutable lookup textures.
    Ptr<ID3D11ComputeShader> edgesCs, weightsCs, blendCs;
    Ptr<ID3D11ComputeShader> defaultEdgesCs, fullWeightsCs, fullBlendCs;
    Ptr<ID3D11SamplerState> linear, point;
    Ptr<ID3D11Texture2D> area, search;
    Ptr<ID3D11ShaderResourceView> areaSrv, searchSrv;
    // Per layer size: the metrics (1/w, 1/h, w, h), the edges (R8G8), the weights and the blended output (R8G8B8A8),
    // plus the 32x8 tile-culling buffers (indirect args, per-tile edge/dirty flags, active tile lists, dedup bitmask).
    Ptr<ID3D11Buffer> metrics;
    Ptr<ID3D11Texture2D> edges, weights, out;
    Ptr<ID3D11UnorderedAccessView> edgesUav, weightsUav, outUav;
    Ptr<ID3D11ShaderResourceView> edgesSrv, weightsSrv, outSrv;
    Ptr<ID3D11Buffer> indirectArgs, tileEdge, tileDirtyOut, weightTiles, blendTiles, blendMask;
    Ptr<ID3D11UnorderedAccessView> indirectArgsUav, tileEdgeUav, tileDirtyOutUav, weightTilesUav, blendTilesUav, blendMaskUav;
    Ptr<ID3D11ShaderResourceView> tileEdgeSrv, tileDirtyOutSrv, weightTilesSrv, blendTilesSrv;
    bool targetsCleared = false;
    uint32_t w = 0, h = 0;               // the size the targets stand at; 0: none
    uint32_t failedW = 0, failedH = 0;   // the size a creation failed at; 0: none

    // The size-bound targets go (the layer was released or re-made); what failed at a size stays remembered.
    void releaseTargets() {
        metrics.Reset();
        edges.Reset();
        weights.Reset();
        out.Reset();
        edgesUav.Reset();
        weightsUav.Reset();
        outUav.Reset();
        edgesSrv.Reset();
        weightsSrv.Reset();
        outSrv.Reset();
        indirectArgs.Reset();
        tileEdge.Reset();
        tileDirtyOut.Reset();
        weightTiles.Reset();
        blendTiles.Reset();
        blendMask.Reset();
        indirectArgsUav.Reset();
        tileEdgeUav.Reset();
        tileDirtyOutUav.Reset();
        weightTilesUav.Reset();
        blendTilesUav.Reset();
        blendMaskUav.Reset();
        tileEdgeSrv.Reset();
        tileDirtyOutSrv.Reset();
        weightTilesSrv.Reset();
        blendTilesSrv.Reset();
        targetsCleared = false;
        w = h = 0;
    }
    // Every child of the device goes, and the failure with them: a new device may make what the last could not.
    void releaseAll() {
        releaseTargets();
        edgesCs.Reset();
        weightsCs.Reset();
        blendCs.Reset();
        defaultEdgesCs.Reset();
        fullWeightsCs.Reset();
        fullBlendCs.Reset();
        linear.Reset();
        point.Reset();
        area.Reset();
        search.Reset();
        areaSrv.Reset();
        searchSrv.Reset();
        failedW = failedH = 0;
    }
    bool ready() const { return outSrv && indirectArgsUav && w && h; }
};

namespace uilayersmaa_detail {

inline bool formatStores(ID3D11Device* dev, DXGI_FORMAT format, HRESULT* hr) {
    D3D11_FEATURE_DATA_FORMAT_SUPPORT2 support{};
    support.InFormat = format;
    *hr = dev->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2, &support, sizeof(support));
    if (FAILED(*hr)) return false;
    if (!(support.OutFormatSupport2 & D3D11_FORMAT_SUPPORT2_UAV_TYPED_STORE)) {
        *hr = E_FAIL;
        return false;
    }
    return true;
}

// A texture of the layer's size with an unordered-access and a shader view, over the format's own typed views.
inline bool makeTarget(ID3D11Device* dev, uint32_t w, uint32_t h, DXGI_FORMAT format, UiLayerSmaa::Ptr<ID3D11Texture2D>& tex,
                       UiLayerSmaa::Ptr<ID3D11UnorderedAccessView>& uav, UiLayerSmaa::Ptr<ID3D11ShaderResourceView>& srv, HRESULT* hr) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = td.ArraySize = 1;
    td.Format = format;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
    *hr = dev->CreateTexture2D(&td, nullptr, tex.ReleaseAndGetAddressOf());
    if (FAILED(*hr)) return false;
    *hr = dev->CreateUnorderedAccessView(tex.Get(), nullptr, uav.ReleaseAndGetAddressOf());
    if (FAILED(*hr)) return false;
    *hr = dev->CreateShaderResourceView(tex.Get(), nullptr, srv.ReleaseAndGetAddressOf());
    return SUCCEEDED(*hr);
}

// One of SMAA's two immutable lookup textures (AreaTex R8G8, SearchTex R8), made into locals and published only whole.
inline bool makeLookup(ID3D11Device* dev, const MapSmaaTable& table, DXGI_FORMAT format, UiLayerSmaa::Ptr<ID3D11Texture2D>& tex,
                       UiLayerSmaa::Ptr<ID3D11ShaderResourceView>& srv, HRESULT* hr) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = table.width;
    td.Height = table.height;
    td.MipLevels = td.ArraySize = 1;
    td.Format = format;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init{};
    init.pSysMem = table.bytes;
    init.SysMemPitch = table.pitch;
    UiLayerSmaa::Ptr<ID3D11Texture2D> made;
    UiLayerSmaa::Ptr<ID3D11ShaderResourceView> view;
    *hr = dev->CreateTexture2D(&td, &init, made.GetAddressOf());
    if (FAILED(*hr)) return false;
    *hr = dev->CreateShaderResourceView(made.Get(), nullptr, view.GetAddressOf());
    if (FAILED(*hr)) return false;
    tex = made;
    srv = view;
    return true;
}

inline bool makeStructuredUint(ID3D11Device* dev, uint32_t count, bool wantSrv, UiLayerSmaa::Ptr<ID3D11Buffer>& buf,
                               UiLayerSmaa::Ptr<ID3D11UnorderedAccessView>& uav, UiLayerSmaa::Ptr<ID3D11ShaderResourceView>* srv, HRESULT* hr) {
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = count * 4u;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS | (wantSrv ? D3D11_BIND_SHADER_RESOURCE : 0u);
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    bd.StructureByteStride = 4u;
    *hr = dev->CreateBuffer(&bd, nullptr, buf.ReleaseAndGetAddressOf());
    if (FAILED(*hr)) return false;
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.Format = DXGI_FORMAT_UNKNOWN;
    ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    ud.Buffer.NumElements = count;
    *hr = dev->CreateUnorderedAccessView(buf.Get(), &ud, uav.ReleaseAndGetAddressOf());
    if (FAILED(*hr)) return false;
    if (wantSrv && srv) {
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = DXGI_FORMAT_UNKNOWN;
        sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        sd.Buffer.NumElements = count;
        *hr = dev->CreateShaderResourceView(buf.Get(), &sd, srv->ReleaseAndGetAddressOf());
        if (FAILED(*hr)) return false;
    }
    return true;
}

inline bool makeIndirectArgs(ID3D11Device* dev, UiLayerSmaa::Ptr<ID3D11Buffer>& buf, UiLayerSmaa::Ptr<ID3D11UnorderedAccessView>& uav, HRESULT* hr) {
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = 24u;  // two DispatchIndirect triplets: {weightGroups, 1, 1}, {blendGroups, 1, 1}
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    bd.MiscFlags = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS | D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    *hr = dev->CreateBuffer(&bd, nullptr, buf.ReleaseAndGetAddressOf());
    if (FAILED(*hr)) return false;
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.Format = DXGI_FORMAT_R32_TYPELESS;
    ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    ud.Buffer.NumElements = 6u;
    ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
    *hr = dev->CreateUnorderedAccessView(buf.Get(), &ud, uav.ReleaseAndGetAddressOf());
    return SUCCEEDED(*hr);
}

}  // namespace uilayersmaa_detail

// Makes what is missing for a layer of w x h. A failure is told once per size: the first call that fails fills `fail` and answers kFailed, the calls after it
// at that size answer kStoodDown without creating anything. A different size tries again.
inline UiLayerSmaaState uiLayerSmaaEnsure(UiLayerSmaa& s, ID3D11Device* dev, uint32_t w, uint32_t h, UiLayerSmaaFail* fail) {
    using namespace uilayersmaa_detail;
    if (s.ready() && s.w == w && s.h == h) return UiLayerSmaaState::kReady;
    if (s.failedW == w && s.failedH == h && w && h) return UiLayerSmaaState::kStoodDown;
    s.releaseTargets();
    HRESULT hr = S_OK;
    const auto stand = [&](const char* what) {
        s.releaseTargets();
        s.failedW = w;
        s.failedH = h;
        if (fail) {
            fail->what = what;
            fail->hr = hr;
        }
        return UiLayerSmaaState::kFailed;
    };
    if (!dev || !w || !h) {
        hr = E_INVALIDARG;
        return stand("no device or an empty layer");
    }
    // The edges and the weights are written through typed unordered-access views of their formats.
    if (!formatStores(dev, DXGI_FORMAT_R8G8_UNORM, &hr)) return stand("no typed unordered-access store for R8G8_UNORM on this GPU");
    if (!formatStores(dev, DXGI_FORMAT_R8G8B8A8_UNORM, &hr)) return stand("no typed unordered-access store for R8G8B8A8_UNORM on this GPU");
    if (!s.edgesCs) {
        if (FAILED(hr = dev->CreateComputeShader(kUiLayerSmaaEdgesBytecode, sizeof(kUiLayerSmaaEdgesBytecode), nullptr, s.edgesCs.GetAddressOf())))
            return stand("the edge shader could not be created");
        s.defaultEdgesCs = s.edgesCs;
    }
    if (!s.weightsCs &&
        FAILED(hr = dev->CreateComputeShader(kUiLayerSmaaWeightsBytecode, sizeof(kUiLayerSmaaWeightsBytecode), nullptr, s.weightsCs.GetAddressOf())))
        return stand("the weights shader could not be created");
    if (!s.blendCs && FAILED(hr = dev->CreateComputeShader(kUiLayerSmaaBlendBytecode, sizeof(kUiLayerSmaaBlendBytecode), nullptr, s.blendCs.GetAddressOf())))
        return stand("the blend shader could not be created");
    // SMAA.hlsl's LinearSampler (s0) and PointSampler (s1): clamped, mip point.
    if (!s.linear) {
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(hr = dev->CreateSamplerState(&sd, s.linear.GetAddressOf()))) return stand("the linear sampler could not be created");
    }
    if (!s.point) {
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(hr = dev->CreateSamplerState(&sd, s.point.GetAddressOf()))) return stand("the point sampler could not be created");
    }
    if (!s.areaSrv && !makeLookup(dev, mapSmaaAreaTable(), DXGI_FORMAT_R8G8_UNORM, s.area, s.areaSrv, &hr)) return stand("SMAA's area texture could not be created");
    if (!s.searchSrv && !makeLookup(dev, mapSmaaSearchTable(), DXGI_FORMAT_R8_UNORM, s.search, s.searchSrv, &hr))
        return stand("SMAA's search texture could not be created");
    // (1/width, 1/height, width, height): the cbuffer SMAA.hlsl's SMAA_RT_METRICS names. Immutable: it moves only with the size.
    {
        const float metrics[4] = {1.0f / static_cast<float>(w), 1.0f / static_cast<float>(h), static_cast<float>(w), static_cast<float>(h)};
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof(metrics);
        bd.Usage = D3D11_USAGE_IMMUTABLE;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA init{};
        init.pSysMem = metrics;
        if (FAILED(hr = dev->CreateBuffer(&bd, &init, s.metrics.GetAddressOf()))) return stand("the metrics constant buffer could not be created");
    }
    if (!makeTarget(dev, w, h, DXGI_FORMAT_R8G8_UNORM, s.edges, s.edgesUav, s.edgesSrv, &hr)) return stand("the edges texture (R8G8) could not be created");
    if (!makeTarget(dev, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, s.weights, s.weightsUav, s.weightsSrv, &hr))
        return stand("the weights texture (R8G8B8A8) could not be created");
    if (!makeTarget(dev, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, s.out, s.outUav, s.outSrv, &hr)) return stand("the smoothed output (R8G8B8A8) could not be created");
    const uint32_t tilesX = (w + 31u) / 32u, tilesY = (h + 7u) / 8u;
    const uint32_t numTiles = tilesX * tilesY;
    const uint32_t maskWords = (numTiles + 31u) / 32u;
    if (!makeIndirectArgs(dev, s.indirectArgs, s.indirectArgsUav, &hr)) return stand("the SMAA indirect args buffer could not be created");
    if (!makeStructuredUint(dev, numTiles, true, s.tileEdge, s.tileEdgeUav, &s.tileEdgeSrv, &hr))
        return stand("the SMAA tile-edge buffer could not be created");
    if (!makeStructuredUint(dev, numTiles, true, s.tileDirtyOut, s.tileDirtyOutUav, &s.tileDirtyOutSrv, &hr))
        return stand("the SMAA tile-dirty buffer could not be created");
    if (!makeStructuredUint(dev, numTiles, true, s.weightTiles, s.weightTilesUav, &s.weightTilesSrv, &hr))
        return stand("the SMAA weight-tile list could not be created");
    if (!makeStructuredUint(dev, numTiles, true, s.blendTiles, s.blendTilesUav, &s.blendTilesSrv, &hr))
        return stand("the SMAA blend-tile list could not be created");
    if (!makeStructuredUint(dev, maskWords, false, s.blendMask, s.blendMaskUav, nullptr, &hr))
        return stand("the SMAA blend-tile mask could not be created");
    s.targetsCleared = false;
    s.w = w;
    s.h = h;
    return UiLayerSmaaState::kMade;
}

// The three passes on `layer` (an R8G8B8A8_UNORM view of the size uiLayerSmaaEnsure made the targets for): edges, then weights, then the blend into the output.
// Returns the output's view, valid until the next run, or null when there is nothing to run. The compute shader, constant buffer 0, resource slots 0-2 and
// unordered-access slot 0 are left as the last pass set them (slots 0-2 and unordered-access slot 0 null); the caller saves and restores those. The two
// samplers, resource slot 3 and unordered-access slots 1-5 are saved and put back here.
inline ID3D11ShaderResourceView* uiLayerSmaaRun(UiLayerSmaa& s, ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* layer) {
    if (!ctx || !layer || !s.ready() || !s.edgesCs || !s.weightsCs || !s.blendCs) return nullptr;
    ID3D11SamplerState* savedSamplers[2] = {};
    ctx->CSGetSamplers(0, 2, savedSamplers);
    ID3D11SamplerState* samplers[2] = {s.linear.Get(), s.point.Get()};
    ID3D11Buffer* metrics = s.metrics.Get();
    ctx->CSSetSamplers(0, 2, samplers);
    ctx->CSSetConstantBuffers(0, 1, &metrics);
    const bool customEdges = s.edgesCs.Get() != s.defaultEdgesCs.Get();
    if (customEdges) {
        // Fallback for tests that substitute a custom 8x8 edge shader (e.g. map colour-only control).
        if (!s.fullWeightsCs || !s.fullBlendCs) {
            UiLayerSmaa::Ptr<ID3D11Device> dev;
            ctx->GetDevice(dev.GetAddressOf());
            if (!dev) return nullptr;
            if (!s.fullWeightsCs)
                dev->CreateComputeShader(kFlatMapSmaaWeightsBytecode, sizeof(kFlatMapSmaaWeightsBytecode), nullptr, s.fullWeightsCs.GetAddressOf());
            if (!s.fullBlendCs)
                dev->CreateComputeShader(kFlatMapSmaaBlendBytecode, sizeof(kFlatMapSmaaBlendBytecode), nullptr, s.fullBlendCs.GetAddressOf());
        }
        if (s.fullWeightsCs && s.fullBlendCs) {
            const UINT groupsX = (s.w + 7u) / 8u, groupsY = (s.h + 7u) / 8u;
            ID3D11ShaderResourceView* const noSrv[3] = {};
            ID3D11UnorderedAccessView* noUav = nullptr;
            {
                ID3D11ShaderResourceView* in = layer;
                ID3D11UnorderedAccessView* target = s.edgesUav.Get();
                ctx->CSSetShader(s.edgesCs.Get(), nullptr, 0);
                ctx->CSSetShaderResources(0, 1, &in);
                ctx->CSSetUnorderedAccessViews(0, 1, &target, nullptr);
                ctx->Dispatch(groupsX, groupsY, 1);
                ctx->CSSetShaderResources(0, 3, noSrv);
                ctx->CSSetUnorderedAccessViews(0, 1, &noUav, nullptr);
            }
            {
                ID3D11ShaderResourceView* in[3] = {s.edgesSrv.Get(), s.areaSrv.Get(), s.searchSrv.Get()};
                ID3D11UnorderedAccessView* target = s.weightsUav.Get();
                ctx->CSSetShader(s.fullWeightsCs.Get(), nullptr, 0);
                ctx->CSSetShaderResources(0, 3, in);
                ctx->CSSetUnorderedAccessViews(0, 1, &target, nullptr);
                ctx->Dispatch(groupsX, groupsY, 1);
                ctx->CSSetShaderResources(0, 3, noSrv);
                ctx->CSSetUnorderedAccessViews(0, 1, &noUav, nullptr);
            }
            {
                ID3D11ShaderResourceView* in[2] = {layer, s.weightsSrv.Get()};
                ID3D11UnorderedAccessView* target = s.outUav.Get();
                ctx->CSSetShader(s.fullBlendCs.Get(), nullptr, 0);
                ctx->CSSetShaderResources(0, 2, in);
                ctx->CSSetUnorderedAccessViews(0, 1, &target, nullptr);
                ctx->Dispatch(groupsX, groupsY, 1);
                ctx->CSSetShaderResources(0, 3, noSrv);
                ctx->CSSetUnorderedAccessViews(0, 1, &noUav, nullptr);
            }
            s.targetsCleared = false;
            ctx->CSSetSamplers(0, 2, savedSamplers);
            for (ID3D11SamplerState* saved : savedSamplers)
                if (saved) saved->Release();
            return s.outSrv.Get();
        }
    }

    ID3D11ShaderResourceView* savedSrv3 = nullptr;
    ID3D11UnorderedAccessView* savedUavs[5] = {};
    ctx->CSGetShaderResources(3, 1, &savedSrv3);
    ctx->CSGetUnorderedAccessViews(1, 5, savedUavs);

    const UINT zeroU[4] = {0u, 0u, 0u, 0u};
    if (!s.targetsCleared) {
        const float clearEdges[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        const float clearOut[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        ctx->ClearUnorderedAccessViewFloat(s.edgesUav.Get(), clearEdges);
        ctx->ClearUnorderedAccessViewFloat(s.outUav.Get(), clearOut);
        ctx->ClearUnorderedAccessViewUint(s.tileEdgeUav.Get(), zeroU);
        ctx->ClearUnorderedAccessViewUint(s.tileDirtyOutUav.Get(), zeroU);
        s.targetsCleared = true;
    }
    const UINT initArgs[6] = {0u, 1u, 1u, 0u, 1u, 1u};
    ctx->UpdateSubresource(s.indirectArgs.Get(), 0, nullptr, initArgs, 0, 0);
    ctx->ClearUnorderedAccessViewUint(s.blendMaskUav.Get(), zeroU);

    const UINT tilesX = (s.w + 31u) / 32u, tilesY = (s.h + 7u) / 8u;
    ID3D11ShaderResourceView* const noSrv[4] = {};
    ID3D11UnorderedAccessView* const noUav[6] = {};
    ctx->CSSetShaderResources(0, 4, noSrv);
    ctx->CSSetUnorderedAccessViews(0, 6, noUav, nullptr);
    {
        // Pass 1: 32x8 four-channel edge test + active-tile classification.
        ID3D11ShaderResourceView* in[2] = {layer, s.tileDirtyOutSrv.Get()};
        ID3D11UnorderedAccessView* out[6] = {s.edgesUav.Get(), s.tileEdgeUav.Get(), s.indirectArgsUav.Get(),
                                             s.weightTilesUav.Get(), s.blendTilesUav.Get(), s.blendMaskUav.Get()};
        ctx->CSSetShader(s.edgesCs.Get(), nullptr, 0);
        ctx->CSSetShaderResources(0, 2, in);
        ctx->CSSetUnorderedAccessViews(0, 6, out, nullptr);
        ctx->Dispatch(tilesX, tilesY, 1);
        ctx->CSSetShaderResources(0, 4, noSrv);
        ctx->CSSetUnorderedAccessViews(0, 6, noUav, nullptr);
    }
    {
        // Pass 2: blending weights on active edge tiles only.
        ID3D11ShaderResourceView* in[4] = {s.edgesSrv.Get(), s.areaSrv.Get(), s.searchSrv.Get(), s.weightTilesSrv.Get()};
        ID3D11UnorderedAccessView* target = s.weightsUav.Get();
        ctx->CSSetShader(s.weightsCs.Get(), nullptr, 0);
        ctx->CSSetShaderResources(0, 4, in);
        ctx->CSSetUnorderedAccessViews(0, 1, &target, nullptr);
        ctx->DispatchIndirect(s.indirectArgs.Get(), 0);
        ctx->CSSetShaderResources(0, 4, noSrv);
        ctx->CSSetUnorderedAccessViews(0, 1, noUav, nullptr);
    }
    {
        // Pass 3: neighbourhood blend on active content / edge / neighbour / previously-dirty tiles only.
        ID3D11ShaderResourceView* in[4] = {layer, s.weightsSrv.Get(), s.blendTilesSrv.Get(), s.tileEdgeSrv.Get()};
        ID3D11UnorderedAccessView* out[2] = {s.outUav.Get(), s.tileDirtyOutUav.Get()};
        ctx->CSSetShader(s.blendCs.Get(), nullptr, 0);
        ctx->CSSetShaderResources(0, 4, in);
        ctx->CSSetUnorderedAccessViews(0, 2, out, nullptr);
        ctx->DispatchIndirect(s.indirectArgs.Get(), 12);
        ctx->CSSetShaderResources(0, 4, noSrv);
        ctx->CSSetUnorderedAccessViews(0, 2, noUav, nullptr);
    }
    ctx->CSSetShaderResources(3, 1, &savedSrv3);
    ctx->CSSetUnorderedAccessViews(1, 5, savedUavs, nullptr);
    if (savedSrv3) savedSrv3->Release();
    for (ID3D11UnorderedAccessView* u : savedUavs)
        if (u) u->Release();
    ctx->CSSetSamplers(0, 2, savedSamplers);
    for (ID3D11SamplerState* saved : savedSamplers)
        if (saved) saved->Release();
    return s.outSrv.Get();
}

// Single-pass premultiplied-alpha-aware FXAA 3.11 over the finished 8-bit UI layer (fix.hud_antialiasing = fxaa).
// One R8G8B8A8_UNORM output texture (4 BPP) and one linear-clamp sampler; mirrors UiLayerSmaa's lifecycle and stand-down contract.
struct UiLayerFxaa {
    template <class T>
    using Ptr = Microsoft::WRL::ComPtr<T>;
    Ptr<ID3D11ComputeShader> fxaaCs;
    Ptr<ID3D11SamplerState> linear;
    Ptr<ID3D11Buffer> metrics;
    Ptr<ID3D11Texture2D> out;
    Ptr<ID3D11UnorderedAccessView> outUav;
    Ptr<ID3D11ShaderResourceView> outSrv;
    uint32_t w = 0, h = 0;
    uint32_t failedW = 0, failedH = 0;

    void releaseTargets() {
        metrics.Reset();
        out.Reset();
        outUav.Reset();
        outSrv.Reset();
        w = h = 0;
    }
    void releaseAll() {
        releaseTargets();
        fxaaCs.Reset();
        linear.Reset();
        failedW = failedH = 0;
    }
    bool ready() const { return outSrv && outUav && metrics && fxaaCs && linear && w && h; }
};

inline UiLayerSmaaState uiLayerFxaaEnsure(UiLayerFxaa& s, ID3D11Device* dev, uint32_t w, uint32_t h, UiLayerSmaaFail* fail) {
    using namespace uilayersmaa_detail;
    if (s.ready() && s.w == w && s.h == h) return UiLayerSmaaState::kReady;
    if (s.failedW == w && s.failedH == h && w && h) return UiLayerSmaaState::kStoodDown;
    s.releaseTargets();
    HRESULT hr = S_OK;
    const auto stand = [&](const char* what) {
        s.releaseTargets();
        s.failedW = w;
        s.failedH = h;
        if (fail) {
            fail->what = what;
            fail->hr = hr;
        }
        return UiLayerSmaaState::kFailed;
    };
    if (!dev || !w || !h) {
        hr = E_INVALIDARG;
        return stand("no device or an empty layer");
    }
    if (!formatStores(dev, DXGI_FORMAT_R8G8B8A8_UNORM, &hr)) return stand("no typed unordered-access store for R8G8B8A8_UNORM on this GPU");
    if (!s.fxaaCs && FAILED(hr = dev->CreateComputeShader(kUiLayerFxaaBytecode, sizeof(kUiLayerFxaaBytecode), nullptr, s.fxaaCs.GetAddressOf())))
        return stand("the FXAA shader could not be created");
    if (!s.linear) {
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(hr = dev->CreateSamplerState(&sd, s.linear.GetAddressOf()))) return stand("the linear sampler could not be created");
    }
    {
        const float metrics[4] = {1.0f / static_cast<float>(w), 1.0f / static_cast<float>(h), static_cast<float>(w), static_cast<float>(h)};
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof(metrics);
        bd.Usage = D3D11_USAGE_IMMUTABLE;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA init{};
        init.pSysMem = metrics;
        if (FAILED(hr = dev->CreateBuffer(&bd, &init, s.metrics.GetAddressOf()))) return stand("the metrics constant buffer could not be created");
    }
    if (!makeTarget(dev, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, s.out, s.outUav, s.outSrv, &hr))
        return stand("the smoothed output (R8G8B8A8) could not be created");
    s.w = w;
    s.h = h;
    return UiLayerSmaaState::kMade;
}

inline ID3D11ShaderResourceView* uiLayerFxaaRun(UiLayerFxaa& s, ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* layer) {
    if (!ctx || !layer || !s.ready()) return nullptr;
    ID3D11SamplerState* savedSampler = nullptr;
    ctx->CSGetSamplers(0, 1, &savedSampler);
    ID3D11SamplerState* linear = s.linear.Get();
    ID3D11Buffer* metrics = s.metrics.Get();
    ID3D11ShaderResourceView* in = layer;
    ID3D11UnorderedAccessView* target = s.outUav.Get();
    ID3D11ShaderResourceView* noSrv = nullptr;
    ID3D11UnorderedAccessView* noUav = nullptr;

    ctx->CSSetSamplers(0, 1, &linear);
    ctx->CSSetConstantBuffers(0, 1, &metrics);
    ctx->CSSetShader(s.fxaaCs.Get(), nullptr, 0);
    ctx->CSSetShaderResources(0, 1, &in);
    ctx->CSSetUnorderedAccessViews(0, 1, &target, nullptr);
    ctx->Dispatch((s.w + 31u) / 32u, (s.h + 7u) / 8u, 1u);
    ctx->CSSetShaderResources(0, 1, &noSrv);
    ctx->CSSetUnorderedAccessViews(0, 1, &noUav, nullptr);

    ctx->CSSetSamplers(0, 1, &savedSampler);
    if (savedSampler) savedSampler->Release();
    return s.outSrv.Get();
}

}  // namespace edvr
