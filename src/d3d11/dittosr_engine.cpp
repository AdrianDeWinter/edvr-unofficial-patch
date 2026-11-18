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

#include <cstdlib>
#include <cstring>
#include <string>

namespace edvr {
namespace {

struct Tex {
    ID3D11Texture2D* tex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    ID3D11UnorderedAccessView* uav = nullptr;
    void release() {
        if (uav) uav->Release();
        if (srv) srv->Release();
        if (tex) tex->Release();
        *this = {};
    }
};

// A view of a caller texture, kept while that texture is the one this slot is given. Creating and releasing five views
// on every evaluation was a render-thread cost in VR (three slots). The texture pointer is the key: a new texture
// drops the old view.
struct HeldSrv {
    ID3D11Resource* resource = nullptr;
    ID3D11ShaderResourceView* view = nullptr;
    void release() {
        if (view) view->Release();
        resource = nullptr;
        view = nullptr;
    }
    bool take(ID3D11Device* dev, ID3D11Resource* want) {
        if (resource == want && (view || !want)) return true;
        release();
        if (!want) return true;
        if (FAILED(dev->CreateShaderResourceView(want, nullptr, &view)) || !view) {
            view = nullptr;
            return false;
        }
        resource = want;
        return true;
    }
};

struct HeldUav {
    ID3D11Resource* resource = nullptr;
    ID3D11UnorderedAccessView* view = nullptr;
    void release() {
        if (view) view->Release();
        resource = nullptr;
        view = nullptr;
    }
    bool take(ID3D11Device* dev, ID3D11Resource* want) {
        if (resource == want && (view || !want)) return true;
        release();
        if (!want) return true;
        if (FAILED(dev->CreateUnorderedAccessView(want, nullptr, &view)) || !view) {
            view = nullptr;
            return false;
        }
        resource = want;
        return true;
    }
};

struct Slot {
    uint32_t rw = 0, rh = 0, ow = 0, oh = 0;
    bool live = false;
    bool told = false;    // the first successful evaluation of this slot has been logged
    bool failed = false;  // this size already failed to allocate; do not try again until the size changes
    uint32_t failRw = 0, failRh = 0, failOw = 0, failOh = 0;
    int cur = 0;
    Tex mda[2], hist[2], aux[2], stab[2], env[2], expo[2];
    Tex change;
    HeldSrv colour, depth, motion, reactive;
    HeldUav out;
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
bool g_refusalNoted = false;

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
    for (Slot& s : g_slot) {
        for (int i = 0; i < 2; ++i) {
            s.mda[i].release();
            s.hist[i].release();
            s.aux[i].release();
            s.stab[i].release();
            s.env[i].release();
            s.expo[i].release();
        }
        s.change.release();
        s.colour.release();
        s.depth.release();
        s.motion.release();
        s.reactive.release();
        s.out.release();
        s = {};
    }
    for (Clock& c : g_clock) c.release();
    g_clockCur = 0;
    g_openClock = -1;
    g_refusalNoted = false;
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
    t.release();
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
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &t.tex)) || !t.tex) return false;
    if (FAILED(dev->CreateShaderResourceView(t.tex, nullptr, &t.srv)) || !t.srv) return false;
    if (FAILED(dev->CreateUnorderedAccessView(t.tex, nullptr, &t.uav)) || !t.uav) return false;
    return true;
}

void dropSlotTextures(Slot& s) {
    for (int i = 0; i < 2; ++i) {
        s.mda[i].release();
        s.hist[i].release();
        s.aux[i].release();
        s.stab[i].release();
        s.env[i].release();
        s.expo[i].release();
    }
    s.change.release();
    s.live = false;
}

bool ensureSlot(ID3D11Device* dev, ID3D11DeviceContext* ctx, unsigned slot, Slot& s, uint32_t rw, uint32_t rh,
                uint32_t ow, uint32_t oh, const char** why) {
    if (s.live && s.rw == rw && s.rh == rh && s.ow == ow && s.oh == oh) return true;
    // This size already failed. Trying again would free and reallocate the partial set on every frame, on the
    // render thread, which is the rig that is already short of memory (the same latch as fsr3_engine.cpp).
    if (s.failed && s.failRw == rw && s.failRh == rh && s.failOw == ow && s.failOh == oh) {
        if (why) *why = "DittoSR could not create its history textures on this device";
        return false;
    }
    const bool remake = s.live;
    const uint32_t wasRw = s.rw, wasRh = s.rh, wasOw = s.ow, wasOh = s.oh;
    if (remake) {
        Log::get().note(
            "DittoSR: %s history is remade (%ux%u -> %ux%u becomes %ux%u -> %ux%u). Its history starts again.",
            upscalerSlotLabel(static_cast<int>(slot)), wasRw, wasRh, wasOw, wasOh, rw, rh, ow, oh);
    }
    dropSlotTextures(s);
    s.failed = false;
    const bool made = makeTex(dev, s.mda[0], rw, rh, DXGI_FORMAT_R16G16B16A16_FLOAT) &&
                      makeTex(dev, s.mda[1], rw, rh, DXGI_FORMAT_R16G16B16A16_FLOAT) &&
                      makeTex(dev, s.hist[0], ow, oh, DXGI_FORMAT_R16G16B16A16_FLOAT) &&
                      makeTex(dev, s.hist[1], ow, oh, DXGI_FORMAT_R16G16B16A16_FLOAT) &&
                      makeTex(dev, s.aux[0], rw, rh, DXGI_FORMAT_R8G8B8A8_UNORM) &&
                      makeTex(dev, s.aux[1], rw, rh, DXGI_FORMAT_R8G8B8A8_UNORM) &&
                      makeTex(dev, s.stab[0], ow, oh, DXGI_FORMAT_R16G16_FLOAT) &&
                      makeTex(dev, s.stab[1], ow, oh, DXGI_FORMAT_R16G16_FLOAT) &&
                      makeTex(dev, s.env[0], rw, rh, DXGI_FORMAT_R16G16_FLOAT) &&
                      makeTex(dev, s.env[1], rw, rh, DXGI_FORMAT_R16G16_FLOAT) &&
                      makeTex(dev, s.expo[0], 1, 1, DXGI_FORMAT_R32_FLOAT) &&
                      makeTex(dev, s.expo[1], 1, 1, DXGI_FORMAT_R32_FLOAT) &&
                      makeTex(dev, s.change, 1, 1, DXGI_FORMAT_R32_FLOAT);
    if (!made) {
        dropSlotTextures(s);
        s.failed = true;
        s.failRw = rw;
        s.failRh = rh;
        s.failOw = ow;
        s.failOh = oh;
        if (why) *why = "DittoSR could not create its history textures on this device";
        return false;
    }
    const float zero[4] = {};
    ctx->ClearUnorderedAccessViewFloat(s.expo[0].uav, zero);
    ctx->ClearUnorderedAccessViewFloat(s.expo[1].uav, zero);
    s.rw = rw;
    s.rh = rh;
    s.ow = ow;
    s.oh = oh;
    s.cur = 0;
    s.live = true;
    return true;
}

bool hasBind(ID3D11Texture2D* tex, UINT flag) {
    if (!tex) return false;
    D3D11_TEXTURE2D_DESC d{};
    tex->GetDesc(&d);
    return (d.BindFlags & flag) == flag;
}

bool floatColour(ID3D11Texture2D* tex) {
    D3D11_TEXTURE2D_DESC d{};
    tex->GetDesc(&d);
    switch (d.Format) {
        case DXGI_FORMAT_R32G32B32A32_FLOAT:
        case DXGI_FORMAT_R32G32B32_FLOAT:
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R11G11B10_FLOAT:
        case DXGI_FORMAT_R9G9B9E5_SHAREDEXP: return true;
        default: return false;
    }
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

bool refuse(const char** why, const char* reason, ID3D11Device* dev = nullptr) {
    if (reason && why) *why = reason;
    const char* said = why && *why ? *why : reason;
    if (said && !g_refusalNoted) {
        g_refusalNoted = true;
        Log::get().note("DittoSR: refused (%s). The pass's own history runs instead.", said);
    }
    if (dev) dev->Release();
    return false;
}

}  // namespace

const char* dittoVersionLabel() { return "DittoSR 0.5.0"; }

bool dittoAvailable(ID3D11Device* dev, const char** why) { return ensureShaders(dev, why); }

void dittoReleaseFeatures() { releaseSlots(); }

void dittoShutdown() {
    releaseSlots();
    releaseShaders();
    g_dev = nullptr;
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

bool dittoEvaluate(ID3D11DeviceContext* ctx, unsigned slot, ID3D11Texture2D* colour, ID3D11Texture2D* depth,
                   ID3D11Texture2D* mv, ID3D11Texture2D* reactive, ID3D11Texture2D* out, uint32_t w, uint32_t h,
                   uint32_t outW, uint32_t outH, float jx, float jy, bool reset, const char** why, bool hdr) {
    if (why) *why = nullptr;
    if (!ctx || slot >= kUpscalerSlots || !w || !h || !outW || !outH)
        return refuse(why, "DittoSR was asked for a frame it cannot size");
    if (!colour || !depth || !mv || !out)
        return refuse(why, "DittoSR needs colour, depth, motion vectors and an output");
    if (!hasBind(colour, D3D11_BIND_SHADER_RESOURCE) || !hasBind(depth, D3D11_BIND_SHADER_RESOURCE) ||
        !hasBind(mv, D3D11_BIND_SHADER_RESOURCE) || (reactive && !hasBind(reactive, D3D11_BIND_SHADER_RESOURCE)))
        return refuse(why, "DittoSR's inputs need a shader-resource bind");
    if (!hasBind(out, D3D11_BIND_UNORDERED_ACCESS))
        return refuse(why, "DittoSR's output needs an unordered-access bind");
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return refuse(why, "DittoSR has no device");
    if (!ensureShaders(dev, why)) return refuse(why, nullptr, dev);
    Slot& s = g_slot[slot];
    const bool grew = !s.live || s.rw != w || s.rh != h || s.ow != outW || s.oh != outH;
    if (!ensureSlot(dev, ctx, slot, s, w, h, outW, outH, why)) return refuse(why, nullptr, dev);
    if (grew) reset = true;

    if (!s.colour.take(dev, colour) || !s.depth.take(dev, depth) || !s.motion.take(dev, mv) ||
        !s.reactive.take(dev, reactive) || !s.out.take(dev, out))
        return refuse(why, "DittoSR could not view the frame's textures", dev);
    ID3D11ShaderResourceView* colourSrv = s.colour.view;
    ID3D11ShaderResourceView* depthSrv = s.depth.view;
    ID3D11ShaderResourceView* mvSrv = s.motion.view;
    ID3D11ShaderResourceView* reactiveSrv = s.reactive.view;
    ID3D11UnorderedAccessView* outUav = s.out.view;

    DittoEval ev;
    ev.rw = w;
    ev.rh = h;
    ev.ow = outW;
    ev.oh = outH;
    ev.jitX = jx;
    ev.jitY = jy;
    ev.reset = reset;
    ev.hdr = hdr || floatColour(colour);
    ev.depthInverted = true;
    ev.hasReactive = reactiveSrv != nullptr;
    ev.autoExposure = ev.hdr;
    ev.sharpness = Config::get().getFloat("fix.dittosr_sharpness", 0.6f);
    ev.frameWeight = Config::get().getFloat("fix.dittosr_frame_weight", 2.0f);
    ev.sharpenMode = dittoSharpenModeFromText(Config::get().getString("fix.dittosr_sharpen", "cas").c_str());
    ev.antiShimmer = dittoAntiShimmerFromText(Config::get().getString("fix.dittosr_anti_shimmer", "auto").c_str());
    const DittoCb packed = dittoFillConstants(ev);

    SavedCs saved;
    saved.capture(ctx);
    harvest(ctx, g_clock[0]);
    harvest(ctx, g_clock[1]);
    stampBegin(dev, ctx);

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
        ID3D11ShaderResourceView* z[8] = {};
        ID3D11UnorderedAccessView* u[3] = {};

        // convert, plus the extra row that writes how much of the image changed
        z[0] = depthSrv;
        z[1] = mvSrv;
        z[2] = colourSrv;
        z[3] = s.hist[prev].srv;
        z[5] = ev.autoExposure ? s.expo[prev].srv : nullptr;
        u[0] = s.mda[cur].uav;
        u[1] = s.change.uav;
        u[2] = ev.autoExposure ? s.expo[cur].uav : nullptr;
        run(ctx, g_convert, z, u, (w + 7) / 8, (h + 7) / 8 + 1);

        // motion
        for (int i = 0; i < 8; ++i) z[i] = nullptr;
        for (int i = 0; i < 3; ++i) u[i] = nullptr;
        z[0] = s.aux[prev].srv;
        z[1] = s.mda[cur].srv;
        z[2] = colourSrv;
        z[3] = s.mda[prev].srv;
        z[4] = s.env[prev].srv;
        z[5] = ev.autoExposure ? s.expo[cur].srv : nullptr;
        u[0] = s.aux[cur].uav;
        u[1] = s.env[cur].uav;
        run(ctx, g_motion, z, u, (w + 7) / 8, (h + 7) / 8);

        // upscale. ANTI_SHIMMER writes a float2 stability texture.
        for (int i = 0; i < 8; ++i) z[i] = nullptr;
        for (int i = 0; i < 3; ++i) u[i] = nullptr;
        z[0] = s.hist[prev].srv;
        z[1] = s.mda[cur].srv;
        z[2] = colourSrv;
        z[3] = s.aux[cur].srv;
        z[4] = s.change.srv;
        z[5] = ev.autoExposure ? s.expo[cur].srv : nullptr;
        z[6] = reactiveSrv;
        z[7] = s.stab[prev].srv;
        u[0] = s.hist[cur].uav;
        u[1] = outUav;
        u[2] = s.stab[cur].uav;
        run(ctx, g_upscale, z, u, (outW + 15) / 16, (outH + 7) / 8);

        // sharpen, unless the upscale already wrote the output (fused, ratio <= 2.55, CAS)
        if (packed.fusedSharpen <= 0.0f) {
            for (int i = 0; i < 8; ++i) z[i] = nullptr;
            for (int i = 0; i < 3; ++i) u[i] = nullptr;
            z[0] = s.hist[cur].srv;
            z[1] = s.mda[cur].srv;
            z[5] = ev.autoExposure ? s.expo[cur].srv : nullptr;
            z[7] = s.stab[cur].srv;
            u[1] = outUav;
            run(ctx, g_sharpen, z, u, (outW + 7) / 8, (outH + 7) / 8);
        }
        ran = true;
    } else if (why) {
        *why = "DittoSR could not write its constants";
    }
    stampEnd(ctx);
    saved.restore(ctx);
    dev->Release();
    if (!ran) return refuse(why, why && *why ? *why : "DittoSR could not write its constants");
    if (!s.told) {
        s.told = true;
        Log::get().note("DittoSR: %s evaluated %ux%u -> %ux%u.", upscalerSlotLabel(static_cast<int>(slot)), w, h, outW, outH);
    }
    ++g_evals;
    if (reset) ++g_resets;
    return true;
}

}  // namespace edvr
