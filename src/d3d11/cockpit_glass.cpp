// visual.cockpit_glass: cockpit_glass.h says what and why; the decisions are cockpit_glass_core.h's, the dirt texture is cockpit_glass_streaks.h's.
#include "cockpit_glass.h"

#include "binding_shadow.h"            // bindingShaderHash: the game's own shader hashes, which the swap and the overlay never disturb
#include "cockpit_glass_core.h"
#include "cockpit_glass_streaks.h"
#include "shader_swap.h"
#include "temporal_shader_bytecode.h"   // kCockpitGlass*PsBytecode
#include "vscreen.h"                    // the raw calls: past the binding shadow's hooks, so the shadow keeps describing the game's bindings

#include "../common/config.h"
#include "../common/guard.h"
#include "../common/log.h"

#include <windows.h>

#include <string>
#include <thread>
#include <vector>

namespace edvr {

namespace detail {
std::atomic<bool> g_cockpitGlassLegacy{false};
}  // namespace detail

namespace {

using namespace cockpit_glass;

constexpr ULONGLONG kWindowMs = 30000;
// The streak texture lives at t12 for every variant (cockpit_glass_shader.h).
constexpr UINT kStreakSlot = 12;

bool g_configRead = false;
uint32_t g_configGeneration = 0;
bool g_on = false;
int g_dirtPercent = kDirtPercentDefault;   // visual.cockpit_glass_dirt, read with the look at the frame boundary
float g_lightsLevel = 0.0f;                // visual.cockpit_glass_lights, read with the look (lightsLevelOf)
std::string g_unrecognisedSaid;   // the last unrecognised value the log has named, so a typo is said once and not every config change

// Everything made on one device is kept as raw pointers on purpose, never released at exit (a static destructor that releases a D3D object during shutdown is the kind of
// thing the exit-crash arcs were about). They are released only when the device they were made on is replaced.
void* g_device = nullptr;
ID3D11PixelShader* g_ps = nullptr;                    // the intact replacement
ID3D11PixelShader* g_overlayPs[2] = {nullptr, nullptr};   // ice, condensation
ID3D11BlendState* g_overlayBlend = nullptr;           // colour ONE + ONE, RGB written, alpha untouched
ID3D11ShaderResourceView* g_streakSrv = nullptr;
ID3D11ShaderResourceView* g_emptyStreakSrv = nullptr;   // 1x1 zero fallback at t12 while the 512x512 worker runs or fails, so an explicit game clear to null is distinguishable (#188)
ID3D11Buffer* g_levelCb = nullptr;                    // the block at b13, two registers (32 bytes): the dirt level, then the layer-to-game pixel map (cockpit_glass_core.h LevelBlock)
LevelBlock g_levelUploaded = {-1.0f, 0.0f, {0, 0}, {1, 1, 0, 0}};  // what g_levelCb holds; a different block is uploaded at the next bind or layer bracket
LevelBlock g_levelWanted = {0.0f, 0.0f, {0, 0}, {1, 1, 0, 0}};     // the dirt level and local lights now, and the map the layer set for the canopy draw it brackets (identity otherwise)
bool g_mapSet = false;                               // the layer's map is in force (set by cockpitGlassSetLayerMap, cleared by cockpitGlassClearLayerMap or any scene bind)
bool g_levelRefused = false;
Replacement g_state = Replacement::NotTried;
bool g_overlayRefused[2] = {false, false};
bool g_blendRefused = false;
bool g_streakRefused = false;
bool g_emptyStreakRefused = false;
bool g_streakLogged = false;
bool g_overlayDsRefused = false;
bool g_overlayStrictDepthLogged = false;

struct OverlayDsEntry {
    D3D11_DEPTH_STENCIL_DESC desc{};
    ID3D11DepthStencilState* state = nullptr;
};
constexpr size_t kOverlayDsCacheMax = 8;
std::vector<OverlayDsEntry> g_overlayDsCache;

// The streak texture is generated off the render thread (about 50 ms on the build machine): started when the key turns on, picked up when ready.
// The intact swap uses a zero texture while the worker runs. If either required input cannot be created, the intact canopy uses its stock shader.
// The job is leaked, so a thread that outlives everything finds it.
struct StreakJob {
    std::atomic<int> state{0};   // 0 not started, 1 running, 2 ready, 3 failed
    std::vector<std::vector<uint8_t>> mips;
    double ms = 0;
    double mean = 0;   // of the stored value (1 = the legacy max)
};
StreakJob* g_job = nullptr;

ULONGLONG g_windowStart = 0;
uint32_t g_binds = 0;      // canopy binds handed the replacement, this window
uint32_t g_seen = 0;       // canopy binds the game made, this window
uint32_t g_overlays = 0;   // ice or condensation draws drawn over with the legacy dirt, this window
uint32_t g_t12Rebinds = 0, g_b13Rebinds = 0;   // canopy draws that found another binding in the swap's slot and put the swap's back, this window (cockpitGlassBeforeDraw)
bool g_t12WriterSaid = false;                  // the first such t12 is described once a session

// What was at t12 and b13 before the intact replacement's bind put the streaks and the dirt level there; given back at the next pixel-shader bind of another shader, or at the
// frame boundary, ONLY when the slot still holds what EDVR bound there (g_t12Ours / g_b13Ours, issue #188).
ID3D11ShaderResourceView* g_t12Prev = nullptr;
ID3D11Buffer* g_b13Prev = nullptr;
ID3D11ShaderResourceView* g_t12Ours = nullptr;   // non-owning identity of what EDVR bound at t12 (g_streakSrv, g_emptyStreakSrv, or nullptr)
ID3D11Buffer* g_b13Ours = nullptr;               // non-owning identity of what EDVR bound at b13 (g_levelCb or nullptr)
bool g_t12GameWrote = false;                     // game called PSSetShaderResources covering t12 while g_t12Bound
bool g_b13GameWrote = false;                     // game called PSSetConstantBuffers covering b13 while g_t12Bound
bool g_t12Bound = false;

// The overlay's own save of the game's state across the one reissued draw.
ID3D11PixelShader* g_savedPs = nullptr;
ID3D11BlendState* g_savedBlend = nullptr;
float g_savedFactor[4] = {1, 1, 1, 1};
UINT g_savedMask = 0xFFFFFFFFu;
ID3D11ShaderResourceView* g_savedT12 = nullptr;
ID3D11Buffer* g_savedB13 = nullptr;
ID3D11DepthStencilState* g_savedDs = nullptr;
UINT g_savedStencilRef = 0;
bool g_savedDsValid = false;

template <class T>
void releaseGuarded(T*& p, const char* site) {
    T* old = p;
    p = nullptr;
    if (old) guarded(site, [&] { old->Release(); });
}

// The device a context belongs to, as a plain pointer for comparison (the reference taken is released at once).
void* deviceOf(ID3D11DeviceContext* ctx) {
    ID3D11Device* dev = nullptr;
    if (!guarded("cockpitGlass.device", [&] { ctx->GetDevice(&dev); }) || !dev) return nullptr;
    void* key = dev;
    guarded("cockpitGlass.device.release", [&] { dev->Release(); });
    return key;
}

void startStreaks() {
    if (g_job) return;
    g_job = new StreakJob;
    g_job->state.store(1);
    StreakJob* job = g_job;
    try {
        std::thread([job] {
            try {
                SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
                LARGE_INTEGER f, a, b;
                QueryPerformanceFrequency(&f);
                QueryPerformanceCounter(&a);
                std::vector<uint8_t> base(static_cast<size_t>(glass_streaks::kSize) * glass_streaks::kSize);
                glass_streaks::generate(base.data());
                glass_streaks::buildMips(base.data(), glass_streaks::kSize, job->mips);
                QueryPerformanceCounter(&b);
                double sum = 0;
                for (uint8_t v : base) sum += v;
                job->mean = sum / static_cast<double>(base.size()) / 255.0;
                job->ms = static_cast<double>(b.QuadPart - a.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);
                job->state.store(2);
            } catch (...) {
                job->state.store(3);
            }
        }).detach();
    } catch (...) {
        job->state.store(3);
    }
}

// The streak texture's view for this device, made from the finished job the first time it is wanted. Null until the worker is done, and for good if it failed.
ID3D11ShaderResourceView* streakSrv(ID3D11DeviceContext* ctx) {
    if (g_streakSrv) return g_streakSrv;
    if (g_streakRefused) return nullptr;
    if (!g_job) startStreaks();
    const int state = g_job->state.load();
    if (state == 3) {
        g_streakRefused = true;
        Log::get().note("cockpit glass: the dirt streak texture could not be generated; the legacy glass draws without dirt");
        return nullptr;
    }
    if (state != 2) return nullptr;
    if (!g_streakLogged) {
        g_streakLogged = true;
        Log::get().note("cockpit glass: dirt streak texture %dx%d generated in %.1f ms on a worker thread (stored mean %.4f of max, the legacy dirt texture's 0.024 / 0.09 = %.4f)",
                        glass_streaks::kSize, glass_streaks::kSize, g_job->ms, g_job->mean, static_cast<double>(glass_streaks::kTargetMean));
    }
    ID3D11Device* dev = nullptr;
    bool ok = false;
    ID3D11Texture2D* tex = nullptr;
    ID3D11ShaderResourceView* view = nullptr;
    guarded("cockpitGlass.streaks.create", [&] {
        ctx->GetDevice(&dev);
        if (!dev) return;
        D3D11_TEXTURE2D_DESC td{};
        td.Width = td.Height = glass_streaks::kSize;
        td.MipLevels = static_cast<UINT>(g_job->mips.size());
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        std::vector<D3D11_SUBRESOURCE_DATA> init(g_job->mips.size());
        int size = glass_streaks::kSize;
        for (size_t m = 0; m < g_job->mips.size(); ++m, size /= 2) {
            init[m].pSysMem = g_job->mips[m].data();
            init[m].SysMemPitch = static_cast<UINT>(size);
        }
        if (SUCCEEDED(dev->CreateTexture2D(&td, init.data(), &tex)) && tex) ok = SUCCEEDED(dev->CreateShaderResourceView(tex, nullptr, &view)) && view;
    });
    if (dev) guarded("cockpitGlass.streaks.device.release", [&] { dev->Release(); });
    releaseGuarded(tex, "cockpitGlass.streaks.tex.release");   // the view keeps the texture alive
    if (!ok) {
        releaseGuarded(view, "cockpitGlass.streaks.view.release");
        g_streakRefused = true;
        Log::get().note("cockpit glass: the dirt streak texture was refused by the device; the legacy glass draws without dirt");
        return nullptr;
    }
    g_streakSrv = view;
    Log::get().note("cockpit glass: dirt streak texture created (R8_UNORM %dx%d, %zu mips) and bound at t12", glass_streaks::kSize, glass_streaks::kSize, g_job->mips.size());
    return g_streakSrv;
}

// Synchronous 1x1 zero-texel R8_UNORM fallback view bound at t12 while the 512x512 worker runs
// (or if streak generation fails) so EDVR's t12 binding is a distinct non-null pointer (#188).
ID3D11ShaderResourceView* emptyStreakSrv(ID3D11DeviceContext* ctx) {
    if (g_emptyStreakSrv) return g_emptyStreakSrv;
    if (g_emptyStreakRefused) return nullptr;
    ID3D11Device* dev = nullptr;
    ID3D11Texture2D* tex = nullptr;
    ID3D11ShaderResourceView* view = nullptr;
    bool ok = false;
    guarded("cockpitGlass.emptyStreak.create", [&] {
        ctx->GetDevice(&dev);
        if (!dev) return;
        D3D11_TEXTURE2D_DESC td{};
        td.Width = td.Height = 1;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        const uint8_t zero = 0;
        const D3D11_SUBRESOURCE_DATA init{&zero, 1, 1};
        if (SUCCEEDED(dev->CreateTexture2D(&td, &init, &tex)) && tex)
            ok = SUCCEEDED(dev->CreateShaderResourceView(tex, nullptr, &view)) && view;
    });
    if (dev) guarded("cockpitGlass.emptyStreak.device.release", [&] { dev->Release(); });
    releaseGuarded(tex, "cockpitGlass.emptyStreak.tex.release");
    if (!ok) {
        releaseGuarded(view, "cockpitGlass.emptyStreak.view.release");
        g_emptyStreakRefused = true;
        return nullptr;
    }
    g_emptyStreakSrv = view;
    return g_emptyStreakSrv;
}

ID3D11ShaderResourceView* streakSrvOrEmpty(ID3D11DeviceContext* ctx) {
    if (ID3D11ShaderResourceView* s = streakSrv(ctx)) return s;
    return emptyStreakSrv(ctx);
}

void dropDeviceObjects() {
    g_t12Ours = nullptr;
    g_b13Ours = nullptr;
    g_t12GameWrote = false;
    g_b13GameWrote = false;
    releaseGuarded(g_ps, "cockpitGlass.ps.release");
    for (auto& p : g_overlayPs) releaseGuarded(p, "cockpitGlass.overlay.release");
    releaseGuarded(g_overlayBlend, "cockpitGlass.blend.release");
    for (auto& e : g_overlayDsCache) releaseGuarded(e.state, "cockpitGlass.ds.release");
    g_overlayDsCache.clear();
    releaseGuarded(g_streakSrv, "cockpitGlass.streaks.release");
    releaseGuarded(g_emptyStreakSrv, "cockpitGlass.emptyStreak.release");
    releaseGuarded(g_levelCb, "cockpitGlass.level.release");
    releaseGuarded(g_t12Prev, "cockpitGlass.t12.release");
    releaseGuarded(g_b13Prev, "cockpitGlass.b13.release");
    g_t12Bound = false;
    g_state = Replacement::NotTried;
    g_overlayRefused[0] = g_overlayRefused[1] = false;
    g_blendRefused = false;
    g_streakRefused = false;
    g_emptyStreakRefused = false;
    g_streakLogged = false;
    g_levelRefused = false;
    g_overlayDsRefused = false;
    g_overlayStrictDepthLogged = false;
    g_levelUploaded = LevelBlock{-1.0f, 0.0f, {0, 0}, {1, 1, 0, 0}};
    g_mapSet = false;
}

// Issue #188(a): a switch back to legacy retries every refused resource (not just the intact shader)
// and restarts a failed streak job without disturbing a running or finished worker.
void resetRefusalsForSwitchOn() {
    if (g_state == Replacement::Refused) g_state = Replacement::NotTried;
    g_levelRefused = false;
    g_overlayRefused[0] = g_overlayRefused[1] = false;
    g_blendRefused = false;
    g_streakRefused = false;
    g_emptyStreakRefused = false;
    g_overlayDsRefused = false;
    g_overlayStrictDepthLogged = false;
    if (g_job && g_job->state.load() == 3) {
        g_job = nullptr;   // failed job's worker has already exited (or never launched)
        g_streakLogged = false;
    }
}

// The device the objects were made on is this context's, or they are made again.
void* ensureDevice(ID3D11DeviceContext* ctx) {
    void* dev = deviceOf(ctx);
    if (!dev) return nullptr;
    if (g_device && g_device != dev) {
        Log::get().note("cockpit glass: the device changed; the replacement shaders and the streak texture are made again");
        dropDeviceObjects();
    }
    g_device = dev;
    return dev;
}

// The intact replacement for this context's device, made the first time it is wanted. Null means draw stock; a refusal stands until the key is turned off and on.
ID3D11PixelShader* replacementFor(ID3D11DeviceContext* ctx) {
    if (g_state == Replacement::Refused) return nullptr;
    if (!ensureDevice(ctx)) return nullptr;
    if (g_ps) return g_ps;
    g_ps = shaderSwapCreatePs(ctx, kCockpitGlassLegacyPsBytecode, sizeof(kCockpitGlassLegacyPsBytecode), "cockpit_glass_legacy_ps", "cockpit glass");
    if (!g_ps) {
        g_state = Replacement::Refused;
        Log::get().note("cockpit glass: replacement pixel shader refused (the build's bytecode did not create); the canopy is drawn stock");
        return nullptr;
    }
    g_state = Replacement::Ready;
    Log::get().note("cockpit glass: replacement pixel shader ready (%zu bytes of build bytecode); it takes the place of the game's intact quality-3 "
                    "canopy shader %016llX, linked with the canopy vertex shader %016llX",
                    sizeof(kCockpitGlassLegacyPsBytecode), static_cast<unsigned long long>(kReplacedPsHash),
                    static_cast<unsigned long long>(kCanopyVsHash));
    return g_ps;
}

// Brings the block up to date with g_levelWanted: an upload only on a change (the dirt level moved, or the layer set or cleared its map).
void uploadLevel(ID3D11DeviceContext* ctx) {
    if (!g_levelCb || !levelBlockDiffers(g_levelWanted, g_levelUploaded)) return;
    const LevelBlock v = g_levelWanted;
    guarded("cockpitGlass.level.update", [&] { vScreenUpdateSubresourceRaw(ctx, g_levelCb, 0, nullptr, &v, 0, 0); });
    g_levelUploaded = v;
}

// The dirt level's block for this device, made the first time it is wanted and brought up to date with visual.cockpit_glass_dirt when the player has moved it (an upload
// only on a change). Null when the device refused it; callers keep stock glass in that case. This is the block for a SCENE bind (the swap,
// the overlay): the pixel map is the identity, whatever a layer bracket left, since a scene draw's SV_Position is the game's own.
ID3D11Buffer* levelBuffer(ID3D11DeviceContext* ctx) {
    g_levelWanted.dirt = dirtLevelOf(g_dirtPercent);
    g_levelWanted.lights = g_lightsLevel;
    identityLevelMap(g_levelWanted);
    g_mapSet = false;
    if (g_levelCb) {
        uploadLevel(ctx);
        return g_levelCb;
    }
    if (g_levelRefused) return nullptr;
    ID3D11Device* dev = nullptr;
    const LevelBlock v = g_levelWanted;
    guarded("cockpitGlass.level.create", [&] {
        ctx->GetDevice(&dev);
        if (!dev) return;
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof v;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        const D3D11_SUBRESOURCE_DATA init{&v, 0, 0};
        dev->CreateBuffer(&bd, &init, &g_levelCb);
    });
    if (dev) guarded("cockpitGlass.level.device.release", [&] { dev->Release(); });
    if (!g_levelCb) {
        g_levelRefused = true;
        Log::get().note("cockpit glass: the dirt level block was refused by the device; falling back to stock canopy glass");
        return nullptr;
    }
    g_levelUploaded = v;
    return g_levelCb;
}

// The shaders' two inputs of EDVR's own: the streak texture at t12 and the dirt level at b13.
// Callers supply non-null EDVR objects (falling back to stock canopy glass if device creation failed)
// so both slots always carry distinguishable non-null identities and restoreGlassInputs() never leaves
// a game slot cleared after an allocation failure (#188).
void bindGlassInputs(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* srv, ID3D11Buffer* level) {
    ID3D11ShaderResourceView* curSrv = nullptr;
    ID3D11Buffer* curCb = nullptr;
    guarded("cockpitGlass.inputs.bind", [&] {
        if (!g_t12Bound) {
            ctx->PSGetShaderResources(kStreakSlot, 1, &g_t12Prev);
            ctx->PSGetConstantBuffers(kLevelSlot, 1, &g_b13Prev);
            g_t12Bound = true;
        } else {
            // If the game rebound t12/b13 while the intact canopy PS stayed bound and then rebound
            // the same PS again, refresh the saved previous bindings instead of overwriting them.
            ctx->PSGetShaderResources(kStreakSlot, 1, &curSrv);
            ctx->PSGetConstantBuffers(kLevelSlot, 1, &curCb);
            if (!shouldRestoreSlot(curSrv, g_t12Ours, g_t12GameWrote)) {
                ID3D11ShaderResourceView* oldPrev = g_t12Prev;
                g_t12Prev = curSrv;
                curSrv = nullptr;
                if (oldPrev) oldPrev->Release();
            }
            if (!shouldRestoreSlot(curCb, g_b13Ours, g_b13GameWrote)) {
                ID3D11Buffer* oldPrev = g_b13Prev;
                g_b13Prev = curCb;
                curCb = nullptr;
                if (oldPrev) oldPrev->Release();
            }
        }
        vScreenPSSetShaderResourcesRaw(ctx, kStreakSlot, 1, &srv);
        vScreenPSSetConstantBuffersRaw(ctx, kLevelSlot, 1, &level);
        g_t12Ours = srv;
        g_b13Ours = level;
        g_t12GameWrote = false;
        g_b13GameWrote = false;
    });
    if (curSrv) guarded("cockpitGlass.bind.cursrv.release", [&] { curSrv->Release(); });
    if (curCb) guarded("cockpitGlass.bind.curcb.release", [&] { curCb->Release(); });
}

void restoreGlassInputs(ID3D11DeviceContext* ctx) {
    if (!g_t12Bound) return;
    g_t12Bound = false;
    ID3D11ShaderResourceView* prev = g_t12Prev;
    ID3D11Buffer* prevB13 = g_b13Prev;
    ID3D11ShaderResourceView* oursSrv = g_t12Ours;
    ID3D11Buffer* oursCb = g_b13Ours;
    const bool t12Wrote = g_t12GameWrote;
    const bool b13Wrote = g_b13GameWrote;
    g_t12Prev = nullptr;
    g_b13Prev = nullptr;
    g_t12Ours = nullptr;
    g_b13Ours = nullptr;
    g_t12GameWrote = false;
    g_b13GameWrote = false;
    ID3D11ShaderResourceView* curSrv = nullptr;
    ID3D11Buffer* curCb = nullptr;
    guarded("cockpitGlass.inputs.restore", [&] {
        // Issue #188(b): restore each slot only if it still holds EDVR's non-null binding and the game did not write it.
        ctx->PSGetShaderResources(kStreakSlot, 1, &curSrv);
        ctx->PSGetConstantBuffers(kLevelSlot, 1, &curCb);
        if (shouldRestoreSlot(curSrv, oursSrv, t12Wrote)) vScreenPSSetShaderResourcesRaw(ctx, kStreakSlot, 1, &prev);
        if (shouldRestoreSlot(curCb, oursCb, b13Wrote)) vScreenPSSetConstantBuffersRaw(ctx, kLevelSlot, 1, &prevB13);
    });
    if (curSrv) guarded("cockpitGlass.restore.cursrv.release", [&] { curSrv->Release(); });
    if (curCb) guarded("cockpitGlass.restore.curcb.release", [&] { curCb->Release(); });
    if (prev) guarded("cockpitGlass.streaks.prev.release", [&] { prev->Release(); });
    if (prevB13) guarded("cockpitGlass.b13.prev.release", [&] { prevB13->Release(); });
}

static_assert(static_cast<uint32_t>(D3D11_COMPARISON_NEVER) == 1u &&
                  static_cast<uint32_t>(D3D11_COMPARISON_LESS) == 2u &&
                  static_cast<uint32_t>(D3D11_COMPARISON_LESS_EQUAL) == 4u &&
                  static_cast<uint32_t>(D3D11_COMPARISON_GREATER) == 5u &&
                  static_cast<uint32_t>(D3D11_COMPARISON_NOT_EQUAL) == 6u &&
                  static_cast<uint32_t>(D3D11_COMPARISON_GREATER_EQUAL) == 7u &&
                  static_cast<uint32_t>(D3D11_COMPARISON_ALWAYS) == 8u &&
                  static_cast<uint32_t>(D3D11_STENCIL_OP_KEEP) == 1u,
              "deriveOverlayDepthRule relies on D3D11_COMPARISON_FUNC / D3D11_STENCIL_OP ABI values");

bool depthStencilDescEqual(const D3D11_DEPTH_STENCIL_DESC& a, const D3D11_DEPTH_STENCIL_DESC& b) {
    return a.DepthEnable == b.DepthEnable && a.DepthWriteMask == b.DepthWriteMask && a.DepthFunc == b.DepthFunc &&
           a.StencilEnable == b.StencilEnable && a.StencilReadMask == b.StencilReadMask && a.StencilWriteMask == b.StencilWriteMask &&
           a.FrontFace.StencilFailOp == b.FrontFace.StencilFailOp && a.FrontFace.StencilDepthFailOp == b.FrontFace.StencilDepthFailOp &&
           a.FrontFace.StencilPassOp == b.FrontFace.StencilPassOp && a.FrontFace.StencilFunc == b.FrontFace.StencilFunc &&
           a.BackFace.StencilFailOp == b.BackFace.StencilFailOp && a.BackFace.StencilDepthFailOp == b.BackFace.StencilDepthFailOp &&
           a.BackFace.StencilPassOp == b.BackFace.StencilPassOp && a.BackFace.StencilFunc == b.BackFace.StencilFunc;
}

ID3D11DepthStencilState* overlayDepthStateFor(ID3D11DeviceContext* ctx, const D3D11_DEPTH_STENCIL_DESC& wanted) {
    for (const OverlayDsEntry& e : g_overlayDsCache) {
        if (depthStencilDescEqual(e.desc, wanted)) return e.state;
    }
    if (g_overlayDsRefused) return nullptr;
    ID3D11Device* dev = nullptr;
    ID3D11DepthStencilState* made = nullptr;
    guarded("cockpitGlass.ds.create", [&] {
        ctx->GetDevice(&dev);
        if (dev) dev->CreateDepthStencilState(&wanted, &made);
    });
    if (dev) guarded("cockpitGlass.ds.device.release", [&] { dev->Release(); });
    if (!made) {
        g_overlayDsRefused = true;
        Log::get().note("cockpit glass: the overlay depth-stencil state was refused; ice and condensation are drawn as the game draws them");
        return nullptr;
    }
    if (g_overlayDsCache.size() >= kOverlayDsCacheMax) {
        releaseGuarded(g_overlayDsCache.front().state, "cockpitGlass.ds.evict");
        g_overlayDsCache.erase(g_overlayDsCache.begin());
    }
    g_overlayDsCache.push_back(OverlayDsEntry{wanted, made});
    return made;
}

ID3D11PixelShader* overlayShaderFor(ID3D11DeviceContext* ctx, Overlay which) {
    const int i = static_cast<int>(which);
    if (g_overlayPs[i]) return g_overlayPs[i];
    if (g_overlayRefused[i]) return nullptr;
    const bool ice = which == Overlay::Ice;
    const void* code = ice ? static_cast<const void*>(kCockpitGlassIceOverlayPsBytecode) : static_cast<const void*>(kCockpitGlassCondensationOverlayPsBytecode);
    const size_t bytes = ice ? sizeof(kCockpitGlassIceOverlayPsBytecode) : sizeof(kCockpitGlassCondensationOverlayPsBytecode);
    g_overlayPs[i] = shaderSwapCreatePs(ctx, code, bytes, ice ? "cockpit_glass_ice_overlay_ps" : "cockpit_glass_condensation_overlay_ps", "cockpit glass");
    if (!g_overlayPs[i]) {
        g_overlayRefused[i] = true;
        Log::get().note("cockpit glass: the %s overlay pixel shader was refused; the %s is drawn as the game draws it, without the legacy dirt", ice ? "ice" : "condensation",
                        ice ? "ice" : "condensation");
        return nullptr;
    }
    Log::get().note("cockpit glass: %s overlay pixel shader ready (%zu bytes); it is drawn over the game's %s canopy draw, quality 3 (%016llX), with colour ONE + ONE",
                    ice ? "ice" : "condensation", bytes, ice ? "ice" : "condensation",
                    static_cast<unsigned long long>(ice ? kIcePsQ3Hash : kCondensationPsQ3Hash));
    return g_overlayPs[i];
}

ID3D11BlendState* overlayBlend(ID3D11DeviceContext* ctx) {
    if (g_overlayBlend) return g_overlayBlend;
    if (g_blendRefused) return nullptr;
    ID3D11Device* dev = nullptr;
    guarded("cockpitGlass.blend.create", [&] {
        ctx->GetDevice(&dev);
        if (!dev) return;
        D3D11_BLEND_DESC bd{};
        D3D11_RENDER_TARGET_BLEND_DESC& rt = bd.RenderTarget[0];
        rt.BlendEnable = TRUE;
        rt.SrcBlend = D3D11_BLEND_ONE;
        rt.DestBlend = D3D11_BLEND_ONE;
        rt.BlendOp = D3D11_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D11_BLEND_ONE;
        rt.DestBlendAlpha = D3D11_BLEND_ZERO;
        rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;   // alpha untouched
        dev->CreateBlendState(&bd, &g_overlayBlend);
    });
    if (dev) guarded("cockpitGlass.blend.device.release", [&] { dev->Release(); });
    if (!g_overlayBlend) {
        g_blendRefused = true;
        Log::get().note("cockpit glass: the overlay blend state was refused; ice and condensation are drawn as the game draws them");
    }
    return g_overlayBlend;
}

void resetWindow(ULONGLONG now) {
    g_windowStart = now;
    g_binds = 0;
    g_seen = 0;
    g_overlays = 0;
    g_t12Rebinds = 0;
    g_b13Rebinds = 0;
}

void logWindow(ULONGLONG now) {
    char line[240];
    formatWindow(line, sizeof line, (now - g_windowStart) / 1000.0, g_binds, g_seen, g_overlays, replacementName(g_state));
    Log::get().note("%s", line);
    if (g_t12Rebinds || g_b13Rebinds)
        Log::get().note("cockpit glass: canopy draws that found another binding and put the swap's back, this window: t12 %u, b13 %u", g_t12Rebinds, g_b13Rebinds);
}

}  // namespace

ID3D11PixelShader* cockpitGlassPsFor(ID3D11DeviceContext* ctx, ID3D11PixelShader* ps, uint64_t hash) {
    if (hash != kReplacedPsHash) {   // every other bind: this compare, the streaks' and the level's hand-back if they are still bound, and out
        if (g_t12Bound) restoreGlassInputs(ctx);
        return ps;
    }
    ++g_seen;
    ID3D11PixelShader* replacement = replacementFor(ctx);
    if (!swapWanted(Look::Legacy, replacement != nullptr, hash)) {
        if (g_t12Bound) restoreGlassInputs(ctx);
        return ps;
    }
    ID3D11ShaderResourceView* srv = streakSrvOrEmpty(ctx);
    ID3D11Buffer* level = levelBuffer(ctx);
    if (!srv || !level) {
        if (g_t12Bound) restoreGlassInputs(ctx);
        return ps;
    }
    ++g_binds;
    bindGlassInputs(ctx, srv, level);
    return replacement;
}

uint64_t cockpitGlassReplacedHash(ID3D11PixelShader* actual) { return actual && actual == g_ps ? kReplacedPsHash : 0; }

bool cockpitGlassSwapActive() { return g_t12Bound; }

bool cockpitGlassSetLayerMap(ID3D11DeviceContext* ctx, float scaleX, float scaleY, float biasX, float biasY) {
    if (!g_levelCb || !g_t12Bound) return false;   // no swap in, or no block: the draw has nothing of ours to map
    g_levelWanted.map[0] = scaleX;
    g_levelWanted.map[1] = scaleY;
    g_levelWanted.map[2] = biasX;
    g_levelWanted.map[3] = biasY;
    g_mapSet = true;
    uploadLevel(ctx);
    return true;
}

void cockpitGlassClearLayerMap(ID3D11DeviceContext* ctx) {
    if (!g_mapSet) return;   // every draw but a taken canopy's: this and out
    g_mapSet = false;
    identityLevelMap(g_levelWanted);
    uploadLevel(ctx);
}

void cockpitGlassNotePsSrvWrite(UINT start, UINT n) {
    if (g_t12Bound && start <= kStreakSlot && kStreakSlot - start < n) g_t12GameWrote = true;
}

void cockpitGlassNotePsCbWrite(UINT start, UINT n) {
    if (g_t12Bound && start <= kLevelSlot && kLevelSlot - start < n) g_b13GameWrote = true;
}

void cockpitGlassBeforeDraw(ID3D11DeviceContext* ctx) {
    if (!g_t12Bound) return;   // every draw but the canopy's: this and out
    ID3D11ShaderResourceView* wantSrv = streakSrvOrEmpty(ctx);
    guarded("cockpitGlass.inputs.check", [&] {
        ID3D11ShaderResourceView* srv = nullptr;
        ID3D11Buffer* cb = nullptr;
        ctx->PSGetShaderResources(kStreakSlot, 1, &srv);
        ctx->PSGetConstantBuffers(kLevelSlot, 1, &cb);
        const bool t12GameChanged = g_t12GameWrote || (srv != g_t12Ours);
        if (t12GameChanged) {
            ++g_t12Rebinds;
            if (!g_t12WriterSaid) {
                g_t12WriterSaid = true;
                D3D11_SHADER_RESOURCE_VIEW_DESC d{};
                if (srv) srv->GetDesc(&d);
                Log::get().note("cockpit glass: at a canopy draw t12 was %s (view dimension %d, format %d), written after the swap's bind; the streaks are bound again at every such draw and that view is what t12 gets back",
                                srv ? "another view" : "empty", static_cast<int>(d.ViewDimension), static_cast<int>(d.Format));
            }
            // The binding that is there now is what the next pixel shader's hand-back must restore: the one saved at the swap's bind is stale. The reference moves to g_t12Prev.
            if (g_t12Prev) g_t12Prev->Release();
            g_t12Prev = srv;
            srv = nullptr;
            vScreenPSSetShaderResourcesRaw(ctx, kStreakSlot, 1, &wantSrv);
            g_t12Ours = wantSrv;
            g_t12GameWrote = false;
        } else if (srv != wantSrv) {
            // The 512x512 streak worker finished while the empty fallback was bound: upgrade t12 without disturbing g_t12Prev.
            vScreenPSSetShaderResourcesRaw(ctx, kStreakSlot, 1, &wantSrv);
            g_t12Ours = wantSrv;
        }
        const bool b13GameChanged = g_b13GameWrote || (cb != g_b13Ours);
        if (b13GameChanged) {
            ++g_b13Rebinds;
            if (g_b13Prev) g_b13Prev->Release();
            g_b13Prev = cb;
            cb = nullptr;
            vScreenPSSetConstantBuffersRaw(ctx, kLevelSlot, 1, &g_levelCb);
            g_b13Ours = g_levelCb;
            g_b13GameWrote = false;
        }
        if (srv) srv->Release();
        if (cb) cb->Release();
    });
}

struct BoundDepthAccess {
    bool hasDsv = false;
    bool hasStencil = false;
    bool readOnlyDepth = false;
    bool readOnlyStencil = false;
};

bool readBoundDepthStencilDesc(ID3D11DeviceContext* ctx, ID3D11DepthStencilState** curDsOut,
                               UINT* curRefOut, D3D11_DEPTH_STENCIL_DESC* descOut,
                               BoundDepthAccess* accessOut) {
    *curDsOut = nullptr;
    *curRefOut = 0;
    *descOut = D3D11_DEPTH_STENCIL_DESC{};
    *accessOut = BoundDepthAccess{};
    return guarded("cockpitGlass.overlay.ds.get", [&] {
        ID3D11DepthStencilView* dsv = nullptr;
        ctx->OMGetRenderTargets(0, nullptr, &dsv);
        if (dsv) {
            D3D11_DEPTH_STENCIL_VIEW_DESC vd{};
            dsv->GetDesc(&vd);
            accessOut->hasDsv = true;
            accessOut->hasStencil = vd.Format == DXGI_FORMAT_D24_UNORM_S8_UINT ||
                                    vd.Format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT ||
                                    vd.Format == DXGI_FORMAT_UNKNOWN;
            accessOut->readOnlyDepth = (vd.Flags & D3D11_DSV_READ_ONLY_DEPTH) != 0;
            accessOut->readOnlyStencil = (vd.Flags & D3D11_DSV_READ_ONLY_STENCIL) != 0;
            dsv->Release();
        }
        ctx->OMGetDepthStencilState(curDsOut, curRefOut);
        if (*curDsOut) {
            (*curDsOut)->GetDesc(descOut);
        } else {   // D3D11 default depth-stencil state
            descOut->DepthEnable = TRUE;
            descOut->DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
            descOut->DepthFunc = D3D11_COMPARISON_LESS;
            descOut->StencilEnable = FALSE;
            descOut->StencilReadMask = D3D11_DEFAULT_STENCIL_READ_MASK;
            descOut->StencilWriteMask = D3D11_DEFAULT_STENCIL_WRITE_MASK;
            descOut->FrontFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP,
                                  D3D11_COMPARISON_ALWAYS};
            descOut->BackFace = descOut->FrontFace;
        }
    });
}

bool cockpitGlassOverlayBegin(ID3D11DeviceContext* ctx, bool baseDrawIssued) {
    if (!baseDrawIssued) return false;
    const Overlay which = overlayFor(bindingShaderHash(BindSlot::Ps));   // the game's own pixel shader: the shadow records it, never the replacement
    if (which == Overlay::None) return false;
    if (!ensureDevice(ctx)) return false;
    ID3D11PixelShader* ps = overlayShaderFor(ctx, which);
    ID3D11BlendState* blend = ps ? overlayBlend(ctx) : nullptr;
    if (!ps || !blend) return false;
    // Issue #188(d): inspect the game's bound DSV and depth-stencil state before touching any slot.
    // Skip depth-writing strict inequalities (LESS/GREATER/NOT_EQUAL) and stencil states whose
    // writes mutate bits tested by the stencil comparison, where exact per-pixel coverage cannot
    // be preserved on replay; when no DSV is bound or the DSV is read-only, the original draw
    // cannot modify depth/stencil so the overlay keeps the game's depth/stencil test verbatim.
    ID3D11DepthStencilState* curDs = nullptr;
    UINT curStencilRef = 0;
    D3D11_DEPTH_STENCIL_DESC dsDesc{};
    BoundDepthAccess dsvAccess{};
    const bool dsReadOk = readBoundDepthStencilDesc(ctx, &curDs, &curStencilRef, &dsDesc, &dsvAccess);
    if (!dsReadOk) {
        releaseGuarded(curDs, "cockpitGlass.overlay.ds.release");
        return false;
    }
    const StencilFaceRule frontRule{
        static_cast<uint32_t>(dsDesc.FrontFace.StencilFailOp),
        static_cast<uint32_t>(dsDesc.FrontFace.StencilDepthFailOp),
        static_cast<uint32_t>(dsDesc.FrontFace.StencilPassOp),
        static_cast<uint32_t>(dsDesc.FrontFace.StencilFunc),
    };
    const StencilFaceRule backRule{
        static_cast<uint32_t>(dsDesc.BackFace.StencilFailOp),
        static_cast<uint32_t>(dsDesc.BackFace.StencilDepthFailOp),
        static_cast<uint32_t>(dsDesc.BackFace.StencilPassOp),
        static_cast<uint32_t>(dsDesc.BackFace.StencilFunc),
    };
    const OverlayDepthRule dsRule = deriveOverlayDepthRuleForDsv(
        dsvAccess.hasDsv, dsvAccess.readOnlyDepth, dsvAccess.readOnlyStencil,
        dsDesc.DepthEnable != FALSE, dsDesc.DepthWriteMask != D3D11_DEPTH_WRITE_MASK_ZERO,
        static_cast<uint32_t>(dsDesc.DepthFunc), dsDesc.StencilEnable != FALSE, dsDesc.StencilWriteMask,
        dsvAccess.hasStencil, dsDesc.StencilReadMask, frontRule, backRule);
    if (dsRule.skipOverlay) {
        releaseGuarded(curDs, "cockpitGlass.overlay.ds.release");
        if (!g_overlayStrictDepthLogged) {
            g_overlayStrictDepthLogged = true;
            Log::get().note("cockpit glass: skipping ice/condensation overlay for mutating depth/stencil state "
                            "(DepthFunc=%u DepthWrite=%u StencilEnable=%u StencilRead=0x%02X StencilWrite=0x%02X "
                            "FrontFunc=%u BackFunc=%u; exact per-pixel coverage cannot be preserved on replay)",
                            static_cast<unsigned>(dsDesc.DepthFunc),
                            dsDesc.DepthWriteMask != D3D11_DEPTH_WRITE_MASK_ZERO ? 1u : 0u,
                            dsDesc.StencilEnable != FALSE ? 1u : 0u,
                            static_cast<unsigned>(dsDesc.StencilReadMask),
                            static_cast<unsigned>(dsDesc.StencilWriteMask),
                            static_cast<unsigned>(dsDesc.FrontFace.StencilFunc),
                            static_cast<unsigned>(dsDesc.BackFace.StencilFunc));
        }
        return false;
    }
    ID3D11Buffer* level = levelBuffer(ctx);
    if (!level) {
        releaseGuarded(curDs, "cockpitGlass.overlay.ds.release");
        return false;
    }
    ID3D11DepthStencilState* overlayDs = nullptr;
    if (dsRule.needsClone) {
        dsDesc.DepthFunc = static_cast<D3D11_COMPARISON_FUNC>(dsRule.depthFunc);
        if (dsRule.zeroDepthWrite) dsDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        if (dsRule.zeroStencilWrite) dsDesc.StencilWriteMask = 0;
        overlayDs = overlayDepthStateFor(ctx, dsDesc);
        if (!overlayDs) {
            releaseGuarded(curDs, "cockpitGlass.overlay.ds.release");
            return false;
        }
    } else {
        releaseGuarded(curDs, "cockpitGlass.overlay.ds.release");
    }
    ID3D11ShaderResourceView* srv = streakSrv(ctx);
    bool saved = false;
    const bool ran = guarded("cockpitGlass.overlay.begin", [&] {
        ctx->PSGetShader(&g_savedPs, nullptr, nullptr);
        ctx->OMGetBlendState(&g_savedBlend, g_savedFactor, &g_savedMask);
        ctx->PSGetShaderResources(kStreakSlot, 1, &g_savedT12);
        ctx->PSGetConstantBuffers(kLevelSlot, 1, &g_savedB13);
        if (dsRule.needsClone) {
            g_savedDs = curDs;
            curDs = nullptr;
            g_savedStencilRef = curStencilRef;
            g_savedDsValid = true;
        }
        saved = true;
        const float one[4] = {1, 1, 1, 1};
        vScreenPSSetShaderRaw(ctx, ps, nullptr, 0);
        vScreenOMSetBlendStateRaw(ctx, blend, one, 0xFFFFFFFFu);
        if (dsRule.needsClone) vScreenOMSetDepthStencilStateRaw(ctx, overlayDs, curStencilRef);
        vScreenPSSetShaderResourcesRaw(ctx, kStreakSlot, 1, &srv);
        vScreenPSSetConstantBuffersRaw(ctx, kLevelSlot, 1, &level);
    });
    if (curDs) releaseGuarded(curDs, "cockpitGlass.overlay.ds.release");
    if (!ran) {
        if (saved) cockpitGlassOverlayEnd(ctx);
        return false;
    }
    ++g_overlays;
    return true;
}

void cockpitGlassOverlayEnd(ID3D11DeviceContext* ctx) {
    guarded("cockpitGlass.overlay.end", [&] {
        vScreenPSSetShaderRaw(ctx, g_savedPs, nullptr, 0);
        vScreenOMSetBlendStateRaw(ctx, g_savedBlend, g_savedFactor, g_savedMask);
        if (g_savedDsValid) vScreenOMSetDepthStencilStateRaw(ctx, g_savedDs, g_savedStencilRef);
        vScreenPSSetShaderResourcesRaw(ctx, kStreakSlot, 1, &g_savedT12);
        vScreenPSSetConstantBuffersRaw(ctx, kLevelSlot, 1, &g_savedB13);
    });
    releaseGuarded(g_savedPs, "cockpitGlass.overlay.ps.release");
    releaseGuarded(g_savedBlend, "cockpitGlass.overlay.blend.release");
    releaseGuarded(g_savedDs, "cockpitGlass.overlay.ds.release");
    g_savedDsValid = false;
    g_savedStencilRef = 0;
    releaseGuarded(g_savedT12, "cockpitGlass.overlay.t12.release");
    releaseGuarded(g_savedB13, "cockpitGlass.overlay.b13.release");
}

void cockpitGlassFrameBoundary(ID3D11DeviceContext* ctx) {
    Config& cfg = Config::get();
    const uint32_t generation = cfg.generation();   // read before the value (config.h): a swap between costs one more read, never a stale one
    if (!g_configRead || generation != g_configGeneration) {
        g_configRead = true;
        g_configGeneration = generation;
        const std::string v = cfg.getString("visual.cockpit_glass", "odyssey");
        bool recognised = true;
        const bool on = parseLook(v.c_str(), &recognised) == Look::Legacy;
        if (!recognised && v != g_unrecognisedSaid) {
            g_unrecognisedSaid = v;
            Log::get().note("cockpit glass: visual.cockpit_glass = \"%s\" is not odyssey or legacy; it is read as odyssey (the game as it ships)", v.c_str());
        }
        // The dirt level (visual.cockpit_glass_dirt), read whatever the look is: it takes effect at the next bind, and is said when it moves while `legacy` is on.
        const int dirtPercent = cfg.getIntInRange("visual.cockpit_glass_dirt", kDirtPercentDefault, kDirtPercentMin, kDirtPercentMax);
        if (dirtPercent != g_dirtPercent) {
            g_dirtPercent = dirtPercent;
            if (on && g_on)
                Log::get().note("cockpit glass: dirt level %d%% (visual.cockpit_glass_dirt), from the next canopy bind", dirtPercent);
        }
        // Which local lights reach the dirt, said when it moves, whatever the look, with the value as read.
        const std::string lights = cfg.getString("visual.cockpit_glass_lights", "off");
        const float lightsLevel = lightsLevelOf(lights.c_str());
        if (lightsLevel != g_lightsLevel) {
            g_lightsLevel = lightsLevel;
            Log::get().note("cockpit glass: local lights on the dirt: %s (visual.cockpit_glass_lights = %s), from the next canopy bind",
                            lightsLevel > 0.5f ? "the station's spot lights" : "none", lights.c_str());
        }
        if (on != g_on) {
            const ULONGLONG now = GetTickCount64();
            if (on) {
                resetWindow(now);
                resetRefusalsForSwitchOn();   // a new try for every refused resource on a new switch-on
                startStreaks();
                Log::get().note("cockpit glass: legacy on (visual.cockpit_glass = legacy): the game's intact quality-3 canopy pixel shader %016llX is "
                                "replaced at bind time, and the ice and condensation draws get the legacy dirt drawn over them, at a dirt level of %d%% "
                                "(visual.cockpit_glass_dirt); a window line every 30 s "
                                "counts the binds, and binds=0 with canopy-seen above 0 means the replacement did not take",
                                static_cast<unsigned long long>(kReplacedPsHash), g_dirtPercent);
            } else {
                logWindow(now);
                if (ctx) restoreGlassInputs(ctx);
                Log::get().note("cockpit glass: legacy off (visual.cockpit_glass = %s): the canopy is drawn by the game's own shader from its next bind",
                                v.c_str());
            }
            g_on = on;
            detail::g_cockpitGlassLegacy.store(on, std::memory_order_relaxed);
        }
    }
    if (!g_on) return;
    if (ctx) restoreGlassInputs(ctx);   // the frame is over: whatever was at t12 and b13 is back
    const ULONGLONG now = GetTickCount64();
    if (windowDue(now, g_windowStart, kWindowMs)) {
        logWindow(now);
        resetWindow(now);
    }
}

}  // namespace edvr
