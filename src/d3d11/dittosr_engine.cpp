// DittoSR 0.5.0 on the game's Direct3D 11 device. The pass order, the bindings
// and the constant formulas are Vancete/DittoSR v0.5.0 (src/dittosr.cpp
// DittoSR::dispatch and src/constants.h). Shader model 5 has no DXC select(),
// and the history is RGBA16F: both are named in third_party/dittosr/README.md.
#include "dittosr_engine.h"

#include "dittosr_math.h"
#include "dittosr_shader_bytecode.h"
#include "dlaa.h"

#include "../common/config.h"
#include "../common/log.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <cstring>
#include <utility>

namespace edvr {
namespace {

using Microsoft::WRL::ComPtr;

struct Tex {
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11UnorderedAccessView> uav;
};

struct SlotSize {
    uint32_t rw = 0, rh = 0, ow = 0, oh = 0;
    bool operator==(const SlotSize& other) const {
        return rw == other.rw && rh == other.rh && ow == other.ow && oh == other.oh;
    }
};

struct History {
    Tex mda[2], hist[2], aux[2], stab[2], env[2], expo[2];
    Tex change;
};

struct Slot {
    SlotSize size;  // last attempted size, including a failed allocation
    bool live = false;
    bool evaluationNoted = false;
    bool refusalNoted = false;
    int cur = 0;
    History history;
};

struct Clock {
    ID3D11Query* disjoint = nullptr;
    ID3D11Query* t0 = nullptr;
    ID3D11Query* t1 = nullptr;
    bool pending = false;
    void release() {
        if (t1) t1->Release();
        if (t0) t0->Release();
        if (disjoint) disjoint->Release();
        *this = {};
    }
};

// The four live tuning keys, parsed once and read again at most every 500 ms, so a menu change lands within about a second
// and the per-eye evaluation does not look up and parse them every frame. Render thread, like the rest of this file's state.
struct DittoTuning {
    float sharpness = 0.6f;
    float frameWeight = 2.0f;
    int sharpenMode = 0;
    float antiShimmer = -1.0f;
    ULONGLONG readAtMs = 0;
    bool read = false;
};

const DittoTuning& dittoTuning() {
    static DittoTuning cached;
    const ULONGLONG now = GetTickCount64();
    if (!cached.read || now - cached.readAtMs >= 500) {
        Config& cfg = Config::get();
        cached.sharpness = cfg.getFloat("fix.dittosr_sharpness", 0.6f);
        cached.frameWeight = cfg.getFloat("fix.dittosr_frame_weight", 2.0f);
        cached.sharpenMode = dittoSharpenModeFromText(cfg.getString("fix.dittosr_sharpen", "standard").c_str());
        cached.antiShimmer = dittoAntiShimmerFromText(cfg.getString("fix.dittosr_anti_shimmer", "auto").c_str());
        cached.readAtMs = now;
        cached.read = true;
    }
    return cached;
}

ID3D11Device* g_dev = nullptr;
ID3D11ComputeShader* g_convert = nullptr;
ID3D11ComputeShader* g_motion = nullptr;
ID3D11ComputeShader* g_upscale = nullptr;
ID3D11ComputeShader* g_sharpen = nullptr;
ID3D11Buffer* g_cb = nullptr;
ID3D11SamplerState* g_samp = nullptr;
Slot g_slot[kUpscalerSlots];
Clock g_clock[2];
int g_clockCur = 0;
int g_openClock = -1;
bool g_shadersTried = false;
const char* g_shaderWhy = "DittoSR's compute shaders have not been created";
uint32_t g_evals = 0, g_resets = 0, g_measured = 0;
double g_msSum = 0, g_msMax = 0;
bool g_compiledNoted = false;
bool g_deviceRefusalNoted = false;
bool g_invalidSlotNoted = false;

void releaseShaders() {
    if (g_sharpen) g_sharpen->Release();
    if (g_upscale) g_upscale->Release();
    if (g_motion) g_motion->Release();
    if (g_convert) g_convert->Release();
    if (g_cb) g_cb->Release();
    if (g_samp) g_samp->Release();
    g_sharpen = g_upscale = g_motion = g_convert = nullptr;
    g_cb = nullptr;
    g_samp = nullptr;
    g_shadersTried = false;
}

void releaseSlots() {
    for (Slot& s : g_slot) s = {};
    for (Clock& c : g_clock) c.release();
    g_clockCur = 0;
    g_openClock = -1;
    g_invalidSlotNoted = false;
}

bool createOne(ID3D11Device* dev, const unsigned char* bytes, size_t size, ID3D11ComputeShader** out,
               const char** why) {
    const HRESULT created = dev->CreateComputeShader(bytes, size, nullptr, out);
    if (FAILED(created) || !*out) {
        if (why) *why = "DittoSR's compute shader could not be created on this device";
        return false;
    }
    return true;
}

bool ensureShaders(ID3D11Device* dev, const char** why) {
    if (!dev) {
        if (why) *why = "DittoSR has no device";
        return false;
    }
    if (g_dev && g_dev != dev) {
        dittoShutdown();
    }
    if (g_convert && g_motion && g_upscale && g_sharpen && g_cb && g_samp && g_dev == dev) return true;
    if (g_shadersTried && g_dev == dev) {
        if (why) *why = g_shaderWhy;
        return false;
    }
    g_shadersTried = true;
    g_dev = dev;
    const bool ok = createOne(dev, kDittoConvertDxbc, sizeof kDittoConvertDxbc, &g_convert, why) &&
                    createOne(dev, kDittoMotionDxbc, sizeof kDittoMotionDxbc, &g_motion, why) &&
                    createOne(dev, kDittoUpscaleDxbc, sizeof kDittoUpscaleDxbc, &g_upscale, why) &&
                    createOne(dev, kDittoSharpenDxbc, sizeof kDittoSharpenDxbc, &g_sharpen, why);
    if (!ok) {
        g_shaderWhy = (why && *why) ? *why : "DittoSR's compute shader could not be created on this device";
        releaseShaders();
        g_shadersTried = true;
        g_dev = dev;
        return false;
    }
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(DittoCb);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(dev->CreateBuffer(&bd, nullptr, &g_cb)) || !g_cb || FAILED(dev->CreateSamplerState(&sd, &g_samp)) || !g_samp) {
        g_shaderWhy = "DittoSR could not create its constant buffer or sampler";
        if (why) *why = g_shaderWhy;
        releaseShaders();
        g_shadersTried = true;
        g_dev = dev;
        return false;
    }
    if (!g_compiledNoted) {
        g_compiledNoted = true;
        Log::get().note("DittoSR: %s compute passes created (convert, motion, upscale, sharpen).", dittoVersionLabel());
    }
    return true;
}

bool makeTex(ID3D11Device* dev, Tex& t, uint32_t w, uint32_t h, DXGI_FORMAT fmt) {
    if (!w || !h) return false;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, t.tex.GetAddressOf())) || !t.tex) return false;
    if (FAILED(dev->CreateShaderResourceView(t.tex.Get(), nullptr, t.srv.GetAddressOf())) || !t.srv) return false;
    if (FAILED(dev->CreateUnorderedAccessView(t.tex.Get(), nullptr, t.uav.GetAddressOf())) || !t.uav) return false;
    return true;
}

bool ensureSlot(ID3D11Device* dev, ID3D11DeviceContext* ctx, Slot& s, const char* route, SlotSize size,
                bool& reset, const char** why) {
    static constexpr char historyFailure[] = "DittoSR could not create its history textures on this device";
    if (s.size == size) {
        // A failed allocation stays latched at its size, as in fsr3_engine.cpp.
        if (!s.live && why) *why = historyFailure;
        return s.live;
    }
    const SlotSize previous = s.size;
    s.history = {};
    s.live = false;
    s.size = size;
    const auto [rw, rh, ow, oh] = size;
    // Publish the new set only when every allocation succeeds.
    History next;
    const bool made = makeTex(dev, next.mda[0], rw, rh, DXGI_FORMAT_R16G16B16A16_FLOAT) &&
                      makeTex(dev, next.mda[1], rw, rh, DXGI_FORMAT_R16G16B16A16_FLOAT) &&
                      makeTex(dev, next.hist[0], ow, oh, DXGI_FORMAT_R16G16B16A16_FLOAT) &&
                      makeTex(dev, next.hist[1], ow, oh, DXGI_FORMAT_R16G16B16A16_FLOAT) &&
                      makeTex(dev, next.aux[0], rw, rh, DXGI_FORMAT_R8G8B8A8_UNORM) &&
                      makeTex(dev, next.aux[1], rw, rh, DXGI_FORMAT_R8G8B8A8_UNORM) &&
                      makeTex(dev, next.stab[0], ow, oh, DXGI_FORMAT_R16G16_FLOAT) &&
                      makeTex(dev, next.stab[1], ow, oh, DXGI_FORMAT_R16G16_FLOAT) &&
                      makeTex(dev, next.env[0], rw, rh, DXGI_FORMAT_R16G16_FLOAT) &&
                      makeTex(dev, next.env[1], rw, rh, DXGI_FORMAT_R16G16_FLOAT) &&
                      makeTex(dev, next.expo[0], 1, 1, DXGI_FORMAT_R32_FLOAT) &&
                      makeTex(dev, next.expo[1], 1, 1, DXGI_FORMAT_R32_FLOAT) &&
                      makeTex(dev, next.change, 1, 1, DXGI_FORMAT_R32_FLOAT);
    if (!made) {
        s.refusalNoted = true;
        Log::get().note("DittoSR: %s refused %ux%u -> %ux%u (history allocation failed).", route, rw, rh, ow, oh);
        if (why) *why = historyFailure;
        return false;
    }
    const float zero[4] = {};
    ctx->ClearUnorderedAccessViewFloat(next.expo[0].uav.Get(), zero);
    ctx->ClearUnorderedAccessViewFloat(next.expo[1].uav.Get(), zero);
    s.history = std::move(next);
    s.cur = 0;
    s.live = true;
    reset = true;
    if (previous.rw != 0) {
        Log::get().note(
            "DittoSR: %s history ready after size change (%ux%u -> %ux%u becomes %ux%u -> %ux%u).",
            route, previous.rw, previous.rh, previous.ow, previous.oh, rw, rh, ow, oh);
    }
    return true;
}

void harvest(ID3D11DeviceContext* ctx, Clock& c) {
    if (!c.pending || !c.disjoint) return;
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
    UINT64 a = 0, b = 0;
    if (ctx->GetData(c.disjoint, &dj, sizeof dj, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) return;
    if (ctx->GetData(c.t0, &a, sizeof a, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) return;
    if (ctx->GetData(c.t1, &b, sizeof b, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) return;
    c.pending = false;
    if (dj.Disjoint || !dj.Frequency || b <= a) return;
    const double ms = static_cast<double>(b - a) * 1000.0 / static_cast<double>(dj.Frequency);
    if (ms < 0.0 || ms > 1000.0) return;
    g_msSum += ms;
    ++g_measured;
    if (ms > g_msMax) g_msMax = ms;
}

void stampBegin(ID3D11Device* dev, ID3D11DeviceContext* ctx) {
    g_openClock = -1;
    for (int n = 0; n < 2; ++n) {
        Clock& c = g_clock[g_clockCur];
        if (!c.pending) {
            if (!c.disjoint) {
                D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
                D3D11_QUERY_DESC qt{D3D11_QUERY_TIMESTAMP, 0};
                if (FAILED(dev->CreateQuery(&qd, &c.disjoint)) || FAILED(dev->CreateQuery(&qt, &c.t0)) ||
                    FAILED(dev->CreateQuery(&qt, &c.t1))) {
                    c.release();
                    return;
                }
            }
            ctx->Begin(c.disjoint);
            ctx->End(c.t0);
            c.pending = true;
            g_openClock = g_clockCur;
            g_clockCur ^= 1;
            return;
        }
        g_clockCur ^= 1;
    }
}

void stampEnd(ID3D11DeviceContext* ctx) {
    if (g_openClock < 0) return;
    Clock& c = g_clock[g_openClock];
    g_openClock = -1;
    if (!c.pending || !c.t1) return;
    ctx->End(c.t1);
    ctx->End(c.disjoint);
}

struct SavedCs {
    ID3D11ComputeShader* cs = nullptr;
    ID3D11Buffer* cb = nullptr;
    ID3D11SamplerState* samp = nullptr;
    ID3D11ShaderResourceView* srv[8] = {};
    ID3D11UnorderedAccessView* uav[3] = {};
    void capture(ID3D11DeviceContext* ctx) {
        ctx->CSGetShader(&cs, nullptr, nullptr);
        ctx->CSGetConstantBuffers(0, 1, &cb);
        ctx->CSGetSamplers(0, 1, &samp);
        ctx->CSGetShaderResources(0, 8, srv);
        ctx->CSGetUnorderedAccessViews(0, 3, uav);
    }
    void restore(ID3D11DeviceContext* ctx) {
        ctx->CSSetShader(cs, nullptr, 0);
        ctx->CSSetConstantBuffers(0, 1, &cb);
        ctx->CSSetSamplers(0, 1, &samp);
        ctx->CSSetShaderResources(0, 8, srv);
        ctx->CSSetUnorderedAccessViews(0, 3, uav, nullptr);
        if (cs) cs->Release();
        if (cb) cb->Release();
        if (samp) samp->Release();
        for (auto* p : srv)
            if (p) p->Release();
        for (auto* p : uav)
            if (p) p->Release();
    }
};

void run(ID3D11DeviceContext* ctx, ID3D11ComputeShader* cs, ID3D11ShaderResourceView* const srv[8],
         ID3D11UnorderedAccessView* const uav[3], UINT gx, UINT gy) {
    ID3D11UnorderedAccessView* none[3] = {};
    ctx->CSSetUnorderedAccessViews(0, 3, none, nullptr);
    ctx->CSSetShaderResources(0, 8, srv);
    ctx->CSSetUnorderedAccessViews(0, 3, uav, nullptr);
    ctx->CSSetShader(cs, nullptr, 0);
    ctx->Dispatch(gx, gy, 1);
}

bool refuse(bool& noted, const char* route, const char* reason, const char** why) {
    if (why) *why = reason;
    if (!noted) {
        noted = true;
        Log::get().note("DittoSR: %s refused (%s).", route, reason);
    }
    return false;
}

}  // namespace

const char* dittoVersionLabel() { return "DittoSR 0.5.0"; }

bool dittoAvailable(ID3D11Device* dev, const char** why) {
    const char* reason = nullptr;
    if (ensureShaders(dev, &reason)) {
        if (why) *why = nullptr;
        return true;
    }
    if (why) *why = reason;
    if (!g_deviceRefusalNoted) {
        g_deviceRefusalNoted = true;
        Log::get().note("DittoSR: unavailable (%s).", reason);
    }
    return false;
}

void dittoReleaseFeatures() { releaseSlots(); }

void dittoShutdown() {
    releaseSlots();
    releaseShaders();
    g_dev = nullptr;
    g_deviceRefusalNoted = false;
    g_shaderWhy = "DittoSR's compute shaders have not been created";
}

bool dittoTotals(uint32_t* evaluations, double* avgMs, double* maxMs, uint32_t* resets) {
    if (!g_measured) return false;
    if (evaluations) *evaluations = g_evals;
    if (avgMs) *avgMs = g_msSum / static_cast<double>(g_measured);
    if (maxMs) *maxMs = g_msMax;
    if (resets) *resets = g_resets;
    return true;
}

bool dittoEvaluate(ID3D11DeviceContext* ctx, unsigned slot, const char* route, const DittoFrameViews& views,
                   uint32_t w, uint32_t h, uint32_t outW, uint32_t outH, float jx, float jy,
                   bool reset, const char** why, bool hdr) {
    if (why) *why = nullptr;
    if (slot >= kUpscalerSlots)
        return refuse(g_invalidSlotNoted, route, "DittoSR was asked for an upscaler slot out of range", why);
    Slot& s = g_slot[slot];
    if (!ctx || !w || !h || !outW || !outH)
        return refuse(s.refusalNoted, route, "DittoSR was asked for a frame it cannot size", why);
    if (!views.colour || !views.depth || !views.motion || !views.output)
        return refuse(s.refusalNoted, route, "DittoSR needs views of colour, depth, motion vectors and output", why);
    ComPtr<ID3D11Device> dev;
    ctx->GetDevice(dev.GetAddressOf());
    if (!dittoAvailable(dev.Get(), why)) return false;
    if (!ensureSlot(dev.Get(), ctx, s, route, {w, h, outW, outH}, reset, why)) return false;

    DittoEval ev;
    ev.rw = w;
    ev.rh = h;
    ev.ow = outW;
    ev.oh = outH;
    ev.jitX = jx;
    ev.jitY = jy;
    ev.reset = reset;
    ev.hdr = hdr;
    ev.depthInverted = true;
    ev.hasReactive = views.reactive != nullptr;
    ev.autoExposure = ev.hdr;
    const DittoTuning& tuning = dittoTuning();
    ev.sharpness = tuning.sharpness;
    ev.frameWeight = tuning.frameWeight;
    ev.sharpenMode = tuning.sharpenMode;
    ev.antiShimmer = tuning.antiShimmer;
    const DittoCb packed = dittoFillConstants(ev);

    SavedCs saved;
    saved.capture(ctx);
    harvest(ctx, g_clock[0]);
    harvest(ctx, g_clock[1]);
    stampBegin(dev.Get(), ctx);

    D3D11_MAPPED_SUBRESOURCE map{};
    bool ran = false;
    if (SUCCEEDED(ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &map)) && map.pData) {
        std::memcpy(map.pData, &packed, sizeof packed);
        ctx->Unmap(g_cb, 0);
        ID3D11Buffer* cb = g_cb;
        ctx->CSSetConstantBuffers(0, 1, &cb);
        ctx->CSSetSamplers(0, 1, &g_samp);

        s.cur ^= 1;
        const int cur = s.cur;
        const int prev = cur ^ 1;
        const History& history = s.history;
        ID3D11ShaderResourceView* z[8] = {};
        ID3D11UnorderedAccessView* u[3] = {};

        // convert, plus the extra row that writes how much of the image changed
        z[0] = views.depth;
        z[1] = views.motion;
        z[2] = views.colour;
        z[3] = history.hist[prev].srv.Get();
        z[5] = ev.autoExposure ? history.expo[prev].srv.Get() : nullptr;
        u[0] = history.mda[cur].uav.Get();
        u[1] = history.change.uav.Get();
        u[2] = ev.autoExposure ? history.expo[cur].uav.Get() : nullptr;
        run(ctx, g_convert, z, u, (w + 7) / 8, (h + 7) / 8 + 1);

        // motion
        for (int i = 0; i < 8; ++i) z[i] = nullptr;
        for (int i = 0; i < 3; ++i) u[i] = nullptr;
        z[0] = history.aux[prev].srv.Get();
        z[1] = history.mda[cur].srv.Get();
        z[2] = views.colour;
        z[3] = history.mda[prev].srv.Get();
        z[4] = history.env[prev].srv.Get();
        z[5] = ev.autoExposure ? history.expo[cur].srv.Get() : nullptr;
        u[0] = history.aux[cur].uav.Get();
        u[1] = history.env[cur].uav.Get();
        run(ctx, g_motion, z, u, (w + 7) / 8, (h + 7) / 8);

        // upscale. ANTI_SHIMMER writes a float2 stability texture.
        for (int i = 0; i < 8; ++i) z[i] = nullptr;
        for (int i = 0; i < 3; ++i) u[i] = nullptr;
        z[0] = history.hist[prev].srv.Get();
        z[1] = history.mda[cur].srv.Get();
        z[2] = views.colour;
        z[3] = history.aux[cur].srv.Get();
        z[4] = history.change.srv.Get();
        z[5] = ev.autoExposure ? history.expo[cur].srv.Get() : nullptr;
        z[6] = views.reactive;
        z[7] = history.stab[prev].srv.Get();
        u[0] = history.hist[cur].uav.Get();
        u[1] = views.output;
        u[2] = history.stab[cur].uav.Get();
        run(ctx, g_upscale, z, u, (outW + 15) / 16, (outH + 7) / 8);

        // sharpen, unless the upscale already wrote the output (fused, ratio <= 2.55, CAS)
        if (packed.fusedSharpen <= 0.0f) {
            for (int i = 0; i < 8; ++i) z[i] = nullptr;
            for (int i = 0; i < 3; ++i) u[i] = nullptr;
            z[0] = history.hist[cur].srv.Get();
            z[1] = history.mda[cur].srv.Get();
            z[5] = ev.autoExposure ? history.expo[cur].srv.Get() : nullptr;
            z[7] = history.stab[cur].srv.Get();
            u[1] = views.output;
            run(ctx, g_sharpen, z, u, (outW + 7) / 8, (outH + 7) / 8);
        }
        ran = true;
    }
    stampEnd(ctx);
    saved.restore(ctx);
    if (!ran) return refuse(s.refusalNoted, route, "DittoSR could not write its constants", why);
    if (!s.evaluationNoted) {
        s.evaluationNoted = true;
        Log::get().note("DittoSR: %s evaluated %ux%u -> %ux%u.", route, w, h, outW, outH);
    }
    ++g_evals;
    if (reset) ++g_resets;
    return true;
}

}  // namespace edvr
