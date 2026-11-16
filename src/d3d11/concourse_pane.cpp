// The concourse's glass panes: see concourse_pane.h. TEMPORARY (docs\concourse-cull-2026-11-05.md): removed when the arc closes.
#include "concourse_pane.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "../common/config.h"
#include "../common/hotkey.h"
#include "../common/log.h"
#include "../common/runtime_profile.h"
#include "binding_shadow.h"
#include "concourse_cull_core.h"
#include "concourse_glass_layout.h"
#include "concourse_vr_bake_core.h"   // TEMPORARY (the VR bake): the capture shader's inverse and its limits
#include "depth_probe.h"              // TEMPORARY (the VR bake): each eye's scene depth, and a draw's eye by its depth view
#include "exposure_fix.h"
#include "flat_compute_readback.h"
#include "journal_watch.h"
#include "shader_swap.h"
#include "temporal_shader_bytecode.h"   // kConcourse*Bytecode: the bake, backdrop and pane shaders, compiled at build time (tools/temporal_shader_build, concourse_pane_shaders.h)
#include "tonemap_admit.h"            // TEMPORARY (the VR bake): the game's tonemap draw, recognised structurally

namespace edvr {
namespace detail {
std::atomic<bool> g_concoursePaneArmed{false};
std::atomic<uint32_t> g_concourseVrBakeHooks{0};
}  // namespace detail

namespace {

namespace cc = edvr::concourse;

enum Mode : uint32_t { kModeMirror = 0, kModeMirrorSoft = 1, kModeTint = 2, kModeOff = 3, kModeLit = 4, kModeBright = 5, kModeBrightest = 6, kModeSteady = 7, kModeBackdrop = 8 };

// The lit modes' glow pass, as a factor on the white, rough material: how much of the room's own light the pane gives back on top of its mirror. That pass is the
// glass shader, so it carries the sun and its shadow map, and they move as the station turns.
float glowOf(uint32_t m) noexcept { return m == kModeLit ? 1.0f : m == kModeBright ? 2.0f : m == kModeBrightest ? 4.0f : 0.0f; }
// The steady mode's glow pass: a flat HDR colour added over the mirror, with no light or shadow in it, so it does not change as the station turns. Its level is by
// window type (diag.concourse_glow: sky, bay, other); a pixel shader scales it by how much the pane faces the anchor's up axis.
constexpr float kSteadyColour[3] = {0.95f, 0.97f, 0.92f};   // a pale white, a little to the green of the station's light

const char* modeText(uint32_t m) noexcept {
    switch (m) {
        case kModeMirrorSoft: return "mirror-soft";
        case kModeTint: return "tint";
        case kModeOff: return "off";
        case kModeLit: return "mirror-lit";
        case kModeBright: return "mirror-bright";
        case kModeBrightest: return "mirror-brightest";
        case kModeSteady: return "mirror-steady";
        case kModeBackdrop: return "mirror-backdrop";
        default: return "mirror";
    }
}

bool parseMode(const std::string& t, uint32_t* out) noexcept {
    if (t == "mirror") *out = kModeMirror;
    else if (t == "mirror-soft") *out = kModeMirrorSoft;
    else if (t == "tint") *out = kModeTint;
    else if (t == "off") *out = kModeOff;
    else if (t == "mirror-lit") *out = kModeLit;
    else if (t == "mirror-bright") *out = kModeBright;
    else if (t == "mirror-brightest") *out = kModeBrightest;
    else if (t == "mirror-steady") *out = kModeSteady;
    else if (t == "mirror-backdrop") *out = kModeBackdrop;
    else return false;
    return true;
}

// Bits of ConcoursePaneScope::flags_.
constexpr uint32_t kFlagBlend = 1u << 0;
constexpr uint32_t kFlagPs = 1u << 1;
constexpr uint32_t kFlagSrv = 1u << 2;
constexpr uint32_t kFlagDepth = 1u << 3;
constexpr uint32_t kFlagCb = 1u << 4;
constexpr uint32_t kFlagQuery = 1u << 5;
constexpr uint32_t kFlagCapQuery = 1u << 6;
constexpr uint32_t kFlagTestDrawn = 1u << 7;    // the side test's first query (the game's glass draw) is open
constexpr uint32_t kFlagTestMirror = 1u << 8;   // the side test's second query (the mirror draw) is open
constexpr uint32_t kFlagProbePass = 1u << 11;   // mirror-backdrop: the probe pass's state is bound and its query is open (endProbePass puts it back)
constexpr uint32_t kFlagCapPass = 1u << 10;     // mirror-backdrop: the capture pass's state is bound (endCapturePass puts it back)
constexpr uint32_t kFlagSampler = 1u << 9;      // mirror-backdrop: the sampler slot holds EDVR's sampler (the game's is in prevSampler_)

// ---- The list, the mode and the feed (the draw threads read them; the tick and the feed write them) --------------------------------------------
std::atomic<uint32_t> g_mode{kModeSteady};
cc::PaneTriple g_list[cc::kPaneListCap] = {{cc::kPaneFamilyVs, cc::kPaneFamilyPs, 0}};   // entry 0 is the built-in glass family; diag.concourse_panes adds
std::atomic<uint32_t> g_listN{1};
// The glass's other pixel shaders: the game builds one per Material Quality level (and for Spot Shadows below High), all with the glass's layout
// (concourse_glass_layout.h). Learned as each is created, from any thread, so it is static storage and survives the diag key's reload of g_list.
constexpr uint32_t kLearnedCap = 32;
std::atomic<uint64_t> g_learnedPs[kLearnedCap];
std::atomic<uint32_t> g_learnedN{0};
std::atomic<bool> g_learnedFullSaid{false};
bool learnedPs(uint64_t ps) noexcept {
    const uint32_t n = std::min<uint32_t>(g_learnedN.load(std::memory_order_acquire), kLearnedCap);
    for (uint32_t i = 0; i < n; ++i)
        if (g_learnedPs[i].load(std::memory_order_relaxed) == ps) return true;
    return false;
}
std::atomic<bool> g_feedMeasure{false}, g_feedTreat{false}, g_feedWithhold{false}, g_feedSealed{false}, g_feedEffects{false};
std::atomic<bool> g_withholdSealedNow{false};   // the hangar is sealed: only the families marked sealedToo are not issued
std::atomic<float> g_bayOpacity{1.0f};
std::atomic<bool> g_treatNow{false};      // treat fed, a mode other than off, and the replacement states made
std::atomic<bool> g_withholdNow{false};   // withhold fed (the cull is on, the plain mirror phase): the mirror is marked; the mirror itself needs a mode other than off
std::atomic<bool> g_effectsNow{false};    // the outside draws (smoke, trails, lasers, the planet) are not issued, whatever the mode: the plain phase, or the backdrop capture's lift (feed.effects)
std::atomic<bool> g_washOffNow{false};    // TEMPORARY: the windows show the bake: the families marked washToo are not issued, the cull lifted or not
std::atomic<bool> g_shareOn{false};       // measure fed and the query pool made
std::atomic<ID3D11DeviceContext*> g_immediate{nullptr};   // not held: the device's immediate context, the one the binding shadow follows
std::atomic<bool> g_capturing{false};     // a capture frame is open
std::atomic<uint64_t> g_treated{0};
std::atomic<uint64_t> g_withheld{0};
// One counter per withheld family (the pixel shader list, then the vertex shader list), read and cleared every 30 s.
constexpr size_t kWithheldFamilies = cc::kWithheldPsCount + cc::kWithheldVsCount;
std::atomic<uint64_t> g_withheldFam[kWithheldFamilies] = {};
std::atomic<uint64_t> g_farLimited{0};   // glows limited by the mark (farLimitedPs), read and cleared with the families
std::atomic<bool> g_surveyRequested{false};   // an F3 press, for the cull's names survey
uint64_t g_withheldLogMs = 0;
std::atomic<uint32_t> g_frameNow{0};
std::atomic<uint32_t> g_hashEpoch{1};

// The replacement states, made by the tick for the game's device.
std::atomic<ID3D11BlendState*> g_bsFactor{nullptr};   // src = BLEND_FACTOR, dest = ZERO (tint)
std::atomic<ID3D11BlendState*> g_bsMirror{nullptr};   // src = ONE, dest = ZERO (a fully opaque pane)
std::atomic<ID3D11BlendState*> g_bsGlow{nullptr};     // src = BLEND_FACTOR, dest = ONE (a lit mode's glow, over the mirror)
std::atomic<ID3D11BlendState*> g_bsFade{nullptr};     // src = BLEND_FACTOR, dest = INV_BLEND_FACTOR (a pane below full opacity)
std::atomic<ID3D11PixelShader*> g_psWhite{nullptr};   // tint: the blend factor multiplies this, so it is the pane's colour
std::atomic<ID3D11ShaderResourceView*> g_mirrorSrv[3] = {};   // t8, t9, t10
std::atomic<ID3D11ShaderResourceView*> g_glowT10{nullptr};    // t10 of the glow pass; its t8 and t9 are the mirror's
std::atomic<ID3D11Buffer*> g_cbMirror{nullptr};       // mirror-soft: the pixel shader's constant buffer 2
std::atomic<bool> g_statesReady{false};
bool g_psWhiteTried = false;   // the tick's: one compile per device
// The steady glow's own pixel shader and its constant buffer (b2: up.xyz, then the sky ratio in the second row's x), the levels, and the anchor's up axis.
std::atomic<ID3D11PixelShader*> g_psSky{nullptr};
std::atomic<ID3D11Buffer*> g_cbGlow{nullptr};      // b2 of the steady pass for every window but the bay: up, (sky, angled, other), colour
std::atomic<ID3D11Buffer*> g_cbGlowBay{nullptr};   // the same for the bay window: no up, its one level
std::atomic<bool> g_steadyFallbackSaid{false};
bool g_psSkyTried = false;
std::atomic<float> g_glowSky{cc::kGlowSkyDefault}, g_glowAngled{cc::kGlowAngledDefault}, g_glowOther{cc::kGlowOtherDefault}, g_glowBay{cc::kGlowBayDefault};   // diag.concourse_glow
std::string g_glowAppliedText;
bool g_glowTextSeen = false;
std::atomic<float> g_upFed[3] = {};
std::atomic<bool> g_upFedValid{false};
float g_glowCbSent[24] = {};     // the tick's: what the buffers hold (rows: up, levels, colour, then the backdrop's distance mode and radius)
float g_glowCbBaySent[24] = {};
bool g_glowCbSentValid = false;
bool g_glowLogged = false;
float g_glowLoggedLv[4] = {};
std::atomic<uint32_t> g_bayN{cc::kBayIndexCount};   // this room's bay window (the feed's, from its anchor)
std::atomic<bool> g_realSkyNow{false};                // this room's windows show the real sky (the feed's): skySide families are issued with the cull on
// While the bake captures (concoursePaneBakeBegin to its finish or cancel): the people's draws are not issued (cc::bakePeopleVs), and how many were left out, for the finish line.
std::atomic<bool> g_bakePeopleOff{false};
std::atomic<uint64_t> g_bakePeopleLeft{0};
std::atomic<float> g_radiusFed{cc::kBackdropRadiusDefaultM};   // this room's sphere, unless the key says

// The mark pass's own objects (made by the tick; the draw threads read the atomics).
std::atomic<ID3D11VertexShader*> g_vsMark{nullptr};      // the fullscreen triangle at the beyond depth
std::atomic<ID3D11RasterizerState*> g_rsMark{nullptr};   // solid, no cull, depth clip, no scissor
std::atomic<ID3D11BlendState*> g_bsMark{nullptr};        // no colour write
std::atomic<ID3D11DepthStencilState*> g_dsMark{nullptr};   // depth test greater-equal, no depth write, stencil replace 0x40
std::atomic<bool> g_markReady{false};
std::atomic<float> g_beyondM{cc::kMirrorBeyondDefaultM};   // diag.concourse_mirror_beyond
float g_vsMarkM = 0.0f;       // the metres the compiled vertex shader holds (the tick's)
bool g_vsMarkTried = false;
bool g_markStatesTried = false;
std::string g_beyondAppliedText;
bool g_beyondTextSeen = false;
constexpr UINT kMarkRef = 0x40;
// The bake has been painted on this pixel this frame (DsVariant::kMarkedOnce). No draw after the frame's first glass reads this bit: in Flight 36's census they read nothing (83 draws, writing 0x04 and
// 0x15) or 0x80 (one draw), which is why it is not 0x80. The mark pass clears it, so whatever the game left in it earlier does not count.
constexpr UINT kBakedBit = 0x02;
std::atomic<uint64_t> g_markPasses{0}, g_markRefused{0}, g_testsRead{0};
// The frame the plain mirror phase last began. For kMarkSettleFrames after it, a pane known to be exterior stays whole: the frame in flight may still hold the
// depth of the structure the cull has just taken away, and the mark would open holes in the pane for that frame.
constexpr uint32_t kMarkSettleFrames = 8;
std::atomic<uint32_t> g_plainFrom{0};

// ---- The backdrop (PROTOTYPE on the concourse arc, docs\concourse-cull-2026-11-05.md "The window plan") ----------------------------------------------------
// Two cubes of view directions from a fixed origin O, filled by capture passes on a key press from frames with the cull off: colour (R16G16B16A16_FLOAT, alpha 1 where
// written, 0 elsewhere) and the distance from O (R16_FLOAT). `back` textures are written by the capture pass (UAVs); `front` textures have a full mip chain, are copied from
// the back ones by the tick after the capture frame and sampled by the display pass, so no resource is bound both ways. O is the camera's row of constant buffer 1 at the
// first capture of a visit, kept in a small buffer. Everything is made on the first capture, per device.
constexpr UINT kBdUavFirst = 6;      // u6 (distance) and u7 (colour) of the capture shader; the bound render target count must not exceed 6
constexpr UINT kBdUavCount = 2;
constexpr UINT kBdSceneSlot = 8;     // t8 t9 t10: the capture shader's scene colour, scene depth and origin; the display shader's colour cube, distance cube and origin
constexpr UINT kBdSrvCount = 3;
constexpr UINT kBdSamplerSlot = 7;   // s7 of the display shader
constexpr UINT kBdCb1Row = 275;      // the camera's row of the glass shaders' constant buffer 1 (INFERRED: the vertex shader subtracts cb1[275], the pixel shader rotates v2 without it)
std::atomic<bool> g_bdArmed{false};                 // a capture frame is open: pane draws copy the scene and issue capture passes
std::atomic<uint32_t> g_bdSceneFrame{0xFFFFFFFFu};  // the frame whose first pane draw tried the scene copy
std::atomic<uint32_t> g_bdSceneOkFrame{0xFFFFFFFFu};   // the frame whose scene, depth and origin copies were all taken
std::atomic<uint32_t> g_bdPaneDraws{0};             // capture passes issued since the arm
std::atomic<bool> g_bdSceneCopied{false};           // a scene copy was taken since the arm
std::atomic<void*> g_bdSceneSource{nullptr};        // the render target resource the copy came from (identity only; a capture pass needs the same one bound)
std::atomic<ID3D11Texture2D*> g_bdScene{nullptr}, g_bdDepth{nullptr};
std::atomic<ID3D11ShaderResourceView*> g_bdSceneSrv{nullptr}, g_bdDepthSrv{nullptr};
std::atomic<ID3D11Texture2D*> g_bdBack{nullptr}, g_bdFront{nullptr}, g_bdDistBack{nullptr}, g_bdDistFront{nullptr};
std::atomic<ID3D11UnorderedAccessView*> g_bdBackUav{nullptr}, g_bdDistBackUav{nullptr};
std::atomic<ID3D11ShaderResourceView*> g_bdFrontSrv{nullptr}, g_bdDistFrontSrv{nullptr};
std::atomic<ID3D11Buffer*> g_bdOrigin{nullptr};     // 16 bytes: the camera at the first capture of this visit (Buffer<float4> originBuf)
std::atomic<ID3D11ShaderResourceView*> g_bdOriginSrv{nullptr};
std::atomic<bool> g_bdOriginSet{false};             // g_bdOrigin holds this visit's origin
std::atomic<bool> g_bdOriginSetNow{false};          // the origin was set during the current capture (the capture line says set or kept)
std::atomic<uint32_t> g_bdCaptures{0};              // captures that went into this cube
std::atomic<bool> g_bdResetPending{false};          // a new visit (treat rose): the tick empties the cube and forgets the origin
std::atomic<bool> g_bakeBaked{false};               // TEMPORARY: the cube is the bake's: a new visit does not empty it (treat rises just after the bake, on arrival)
std::atomic<uint32_t> g_bdParallax{cc::kParallaxSphere};   // the distance mode (hotkey.concourse_backdrop_parallax); the sphere by Sean's eye on Flight 30, against the march and none
std::atomic<float> g_bdRadius{cc::kBackdropRadiusDefaultM};   // diag.concourse_backdrop_radius
std::atomic<ID3D11PixelShader*> g_psBdShow{nullptr}, g_psBdCapture{nullptr};
std::atomic<ID3D11SamplerState*> g_bdSampler{nullptr};
std::atomic<bool> g_bdCubeValid{false};             // front holds a capture
std::atomic<bool> g_bdFallbackSaid{false};
std::atomic<bool> g_bdSceneRefusedSaid{false};
std::atomic<bool> g_bdPassRefusedSaid{false};
std::mutex g_bdMutex;                               // the scene and depth textures' (re)making and the status text below
uint32_t g_bdSceneW = 0, g_bdSceneH = 0;
DXGI_FORMAT g_bdSceneFmt = DXGI_FORMAT_UNKNOWN;
uint32_t g_bdDepthW = 0, g_bdDepthH = 0;
DXGI_FORMAT g_bdDepthFmt = DXGI_FORMAT_UNKNOWN;
char g_bdSceneFmtText[28] = {};
char g_bdRefusal[200] = {};
std::atomic<uint8_t> g_bdPhase{0};                  // ConcourseBackdropPhase
std::atomic<bool> g_bdArmRequested{false};
uint32_t g_bdArmFrame = 0;                          // the tick's
uint32_t g_bdFramesWaited = 0;
// The glass look (diag.concourse_glass: r, g, b, haze, blur, fresnel) and its text.
std::atomic<float> g_glass[6] = {{cc::kGlassDefault[0]}, {cc::kGlassDefault[1]}, {cc::kGlassDefault[2]}, {cc::kGlassDefault[3]}, {cc::kGlassDefault[4]}, {cc::kGlassDefault[5]}};
std::string g_glassAppliedText;
bool g_glassTextSeen = false;
// Automatic capture: the probe pass (a colourless pass whose occlusion count is the pixels the cube cannot show), what it measured, and the sticky enable a hotkey sets.
std::atomic<bool> g_bdAutoOn{false};                // a hotkey (F4 to backdrop, or F6) turned automatic capture on; it runs only while the mode is mirror-backdrop
std::atomic<bool> g_bdToast{true};                  // the capture in progress was F6's: its result is a toast (an automatic capture says nothing on screen)
std::atomic<uint64_t> g_probeIssued{0};             // probe passes issued (the draw threads)
std::atomic<uint64_t> g_probeRead{0};               // probe results read (the tick)
std::atomic<ID3D11PixelShader*> g_psBdProbe{nullptr};
struct MissEntry {
    uint32_t frame = 0;
    double missing = 0.0;
};
constexpr size_t kMissEntries = 32;
constexpr uint32_t kMissWindowFrames = 8;
MissEntry g_miss[kMissEntries];                     // the tick's: measured missing samples by the frame they were drawn in
size_t g_missNext = 0;
double g_missPixels = 0.0;                          // the viewport area of the latest measurement
std::atomic<float> g_missShare{0.0f};               // the tick's result: the share of the viewport missing over the last kMissWindowFrames frames
Hotkey* g_keyParallax = nullptr;
std::string g_keyParallaxText;
Hotkey* g_keyLightTest = nullptr;                // TEMPORARY: hotkey.concourse_light_test
std::string g_keyLightTestText;
uint32_t g_lightTestStep = 0;
std::atomic<uint64_t> g_lightTestPs{0};          // the pixel shader left out; 0 is none
std::atomic<uint64_t> g_lightTestHits{0};        // draws left out since the last press: 0 says the step matched nothing
std::string g_radiusAppliedText;
bool g_radiusTextSeen = false;// The toast, one slot: the newest text wins (hotkey changes of the mode, the capture's result). The tick's thread writes it and menuTick takes it.
std::mutex g_toastMutex;
char g_toastText[64] = {};
bool g_toastDue = false;
void queueToast(const char* text) noexcept {
    std::lock_guard<std::mutex> lock(g_toastMutex);
    std::snprintf(g_toastText, sizeof(g_toastText), "%s", text);
    g_toastDue = true;
}
void releaseBackdrop() noexcept;
void releaseBackdropShaders() noexcept;
std::atomic<ID3D11Device*> g_bdShaderDevice{nullptr};   // the device the three backdrop pixel shaders were made on: compared only, never held (they outlive releaseBackdrop, like g_csBake)

constexpr UINT kPaneSrvFirst = 8;
constexpr UINT kPaneSrvCount = 3;
constexpr UINT kPaneCbSlot = 2;
constexpr UINT kPaneCbBytes = 48;

// ---- The capture ------------------------------------------------------------------------------------------------------------------------------
constexpr size_t kPool = 1024;
struct CapDraw {
    uint64_t vs = 0, ps = 0;
    uint32_t n = 0;
    uint32_t vpW = 0, vpH = 0;
    uint32_t src = 0, dst = 0;
    int depthWrite = 0;
    uint32_t stencil = 0;
    bool arrived = false;
    uint64_t samples = 0;
};
ID3D11Query* g_pool[kPool] = {};
CapDraw g_cap[kPool];
size_t g_capN = 0;           // under g_capMutex while capturing; the tick's after
uint64_t g_capDropped = 0;   // qualifying draws past the pool
std::mutex g_capMutex;       // taken only while a capture is open
enum class CapState : uint8_t { kIdle, kOpen, kReading };
CapState g_capState = CapState::kIdle;
uint32_t g_capFrame = 0;
bool g_poolMade = false;
bool g_capPressPending = false;

// ---- The share measurement: an occlusion query around each family draw on the immediate context -------------------------------------------------
constexpr size_t kShareSlots = 48;
constexpr size_t kBins = 8;
constexpr uint32_t kBinMaxN = 8;
struct ShareSlot {
    ID3D11Query* q = nullptr;
    uint8_t state = 0;   // 0 free, 1 open (between Begin and End), 2 ended and not yet read
    uint32_t frame = 0;
    uint32_t n = 0;
    double pixels = 0.0;
    cc::PaneKey key;                                   // the pane, and its side as the table had it when the query opened (the draw hook's, never read under this mutex)
    cc::PaneSide side = cc::PaneSide::kUnknown;
};
struct Bin {
    bool used = false;
    uint32_t frame = 0;
    uint32_t outstanding = 0;
    uint32_t count = 0;
    uint32_t n[kBinMaxN] = {};
    float share[kBinMaxN] = {};
    uint32_t sampleCount = 0;                          // one sample per pane draw: the total of the outside glass reads these
    cc::GlassSample samples[kShareSlots];
};
std::mutex g_shareMutex;
ShareSlot g_slots[kShareSlots];
Bin g_bins[kBins];
bool g_slotsMade = false;
ConcoursePaneShares g_latest;
uint64_t g_latestMs = 0;
uint32_t g_latestFrame = 0;
uint32_t g_seqCounter = 0;
uint64_t g_shareDropped = 0;
uint32_t g_sampleDropped = 0;   // samples that did not fit a bin's table, since the last stats call
double g_mainPixels = 0.0;      // the largest viewport of the visit, decaying (cc::mainViewPixelsNext); under g_shareMutex

// ---- The one-shot material log ---------------------------------------------------------------------------------------------------------------
constexpr size_t kMatMax = 4;
struct MatPending {
    ID3D11Buffer* staging = nullptr;
    uint32_t dueFrame = 0;
    uint32_t tries = 0;
    char text[760] = {};
};
std::mutex g_matMutex;
uint32_t g_matSeenN[kMatMax] = {};
size_t g_matSeen = 0;
MatPending g_mat[kMatMax];

// ---- The tint colours: one per distinct index count, in the order seen -------------------------------------------------------------------------------
std::mutex g_tintMutex;
uint32_t g_tintN[cc::kPaneColourCount] = {};
size_t g_tintCount = 0;

// ---- The side tests and the table of each pane's side ----------------------------------------------------------------------------------------------
// A test is two occlusion queries around two draws of one pane: the game's glass, then the mirror (marked). The ring holds 16 tests; the tick reads what arrived.
constexpr size_t kTestSlots = 16;
struct TestSlot {
    ID3D11Query* qDrawn = nullptr;
    ID3D11Query* qMirror = nullptr;
    ID3D11Query* qProbe = nullptr;   // the backdrop's probe pass (between the game's draw and the display pass); used only on a test frame in mirror-backdrop with a cube
    uint8_t probe = 0;               // 0 no probe in this test, 1 its query is open, 2 closed (read with the other two)
    uint8_t state = 0;   // 0 free, 1 the game's draw open, 2 closed and waiting for the mirror, 3 the mirror open, 4 both closed, not yet read
    uint32_t frame = 0;
    cc::PaneKey key;
    double pixels = 0.0;   // the viewport's area at the draw (the share's denominator)
};
std::mutex g_sideMutex;   // the table and the ring: the draw thread and the tick both touch them
TestSlot g_tests[kTestSlots];
cc::PaneSides g_sides;
bool g_testsMade = false;
std::atomic<bool> g_testsOn{false};
uint32_t g_sideLogExterior = 0, g_sideLogOther = 0;   // lines said this session, each capped
constexpr uint32_t kSideLogCap = 96;

// The mark pass runs at most once per (frame, depth view, viewport): the draw thread's (the immediate context only).
struct MarkKey {
    uint32_t frame = 0;
    ID3D11DepthStencilView* dsv = nullptr;   // identity only, never dereferenced
    float vp[4] = {};
    bool valid = false;
};
MarkKey g_markKey;
bool g_markLogged = false;
bool g_markNoStencilLogged = false;

// ---- The derived depth states: the game's, with depth writes on ----------------------------------------------------------------------------------
// Two variants per original state: `plain` (depth write on, stencil off) for an opaque pane drawn everywhere, and `marked` (depth write on, stencil read mask 0x40,
// equal to the reference, no stencil write) for an opaque pane drawn where the mark pass set bit 0x40.
struct DsEntry {
    ID3D11DepthStencilState* orig = nullptr;   // held, so the pointer cannot be reused for another state
    ID3D11DepthStencilState* plain = nullptr;
    ID3D11DepthStencilState* marked = nullptr;
    ID3D11DepthStencilState* markedOnce = nullptr;
    ID3D11DepthStencilState* farOut = nullptr;
    ID3D11DepthStencilState* probe = nullptr;
    ID3D11DepthStencilState* markedProbe = nullptr;
};
// markedOnce: `marked`, and a pixel takes the bake once. The bake writes its picture's depth, and two panes on one pixel compute that depth along the same ray, so their depth test is a coin
// toss that differs by the rounding of each: the later pane either repaints the bake over its own glass or leaves its glass on the bake, pixel by pixel and frame by frame (Flight 36, where one
// glass wall stands inside the room in front of the bay). Now the first pane paints it and sets kBakedBit, and a later pane there keeps the game's glass, on top of the bake, as with the cull off.
enum class DsVariant { kPlain, kMarked, kMarkedOnce, kFarOut, kProbe, kMarkedProbe };
std::mutex g_dsMutex;
DsEntry g_ds[16];
size_t g_dsN = 0;

// The tick's state.
ID3D11Device* g_device = nullptr;
bool g_social = false;
uint64_t g_nextPollMs = 0;
Hotkey* g_keyCapture = nullptr;
Hotkey* g_keyMode = nullptr;
std::string g_keyCaptureText, g_keyModeText;
bool g_keysBound = false;
std::string g_modeAppliedText, g_panesAppliedText;
bool g_modeTextSeen = false, g_panesTextSeen = false;
uint64_t g_lastTickMs = 0;

// ---- Shader hashes, cached by pointer per thread (cleared about once a second) ----------------------------------------------------------------
struct HashCache {
    void* key[64] = {};
    uint64_t hash[64] = {};
    uint32_t epoch = 0;
};
thread_local HashCache t_hashCache;

uint64_t cachedHash(void* shader) noexcept {
    HashCache& c = t_hashCache;
    const uint32_t epoch = g_hashEpoch.load(std::memory_order_relaxed);
    if (c.epoch != epoch) {
        std::memset(c.key, 0, sizeof(c.key));
        c.epoch = epoch;
    }
    const size_t i = (reinterpret_cast<uintptr_t>(shader) >> 4) & 63u;
    if (c.key[i] == shader) return c.hash[i];
    const uint64_t h = lookupShaderHash(shader);
    c.key[i] = shader;
    c.hash[i] = h;
    return h;
}

uint64_t pixelShaderHash(ID3D11DeviceContext* ctx) noexcept {
    ID3D11PixelShader* p = nullptr;
    ctx->PSGetShader(&p, nullptr, nullptr);
    if (!p) return 0;
    const uint64_t h = cachedHash(p);
    p->Release();
    return h;
}

uint64_t vertexShaderHash(ID3D11DeviceContext* ctx) noexcept {
    ID3D11VertexShader* v = nullptr;
    ctx->VSGetShader(&v, nullptr, nullptr);
    if (!v) return 0;
    const uint64_t h = cachedHash(v);
    v->Release();
    return h;
}

void shaderHashes(ID3D11DeviceContext* ctx, uint64_t* vs, uint64_t* ps) noexcept {
    ID3D11VertexShader* v = nullptr;
    ID3D11PixelShader* p = nullptr;
    ctx->VSGetShader(&v, nullptr, nullptr);
    ctx->PSGetShader(&p, nullptr, nullptr);
    *vs = v ? lookupShaderHash(v) : 0;
    *ps = p ? lookupShaderHash(p) : 0;
    if (v) v->Release();
    if (p) p->Release();
}

void releaseStates() noexcept {
    // The draw threads may hold the pointers for one more draw, and the old device owns the objects: not released (a device change is rare).
    releaseBackdrop();   // the exception: the cube is about 230 MB
    releaseBackdropShaders();   // the device is gone or going: the shaders belong to it
    g_statesReady.store(false, std::memory_order_release);
    g_bsFactor.store(nullptr, std::memory_order_release);
    g_bsMirror.store(nullptr, std::memory_order_release);
    g_bsFade.store(nullptr, std::memory_order_release);
    g_bsGlow.store(nullptr, std::memory_order_release);
    g_glowT10.store(nullptr, std::memory_order_release);
    g_psWhite.store(nullptr, std::memory_order_release);
    g_psSky.store(nullptr, std::memory_order_release);
    g_cbGlow.store(nullptr, std::memory_order_release);
    g_cbGlowBay.store(nullptr, std::memory_order_release);
    g_psSkyTried = false;
    g_glowCbSentValid = false;
    for (auto& s : g_mirrorSrv) s.store(nullptr, std::memory_order_release);
    g_cbMirror.store(nullptr, std::memory_order_release);
    g_psWhiteTried = false;
    g_markReady.store(false, std::memory_order_release);
    g_vsMark.store(nullptr, std::memory_order_release);
    g_rsMark.store(nullptr, std::memory_order_release);
    g_bsMark.store(nullptr, std::memory_order_release);
    g_dsMark.store(nullptr, std::memory_order_release);
    g_vsMarkTried = false;
    g_vsMarkM = 0.0f;
    g_markStatesTried = false;
    {
        std::lock_guard<std::mutex> lock(g_dsMutex);
        for (auto& e : g_ds) e = DsEntry{};
        g_dsN = 0;
    }
    {
        std::lock_guard<std::mutex> lock(g_sideMutex);
        g_testsOn.store(false, std::memory_order_release);
        for (auto& t : g_tests) t = TestSlot{};
        g_testsMade = false;
        g_sides.reset();
        g_markKey = MarkKey{};
    }
    {
        std::lock_guard<std::mutex> lock(g_shareMutex);
        g_shareOn.store(false, std::memory_order_release);
        for (auto& s : g_slots) s = ShareSlot{};
        for (auto& b : g_bins) b = Bin{};
        g_slotsMade = false;
        g_latest = ConcoursePaneShares{};
    }
    {
        std::lock_guard<std::mutex> lock(g_matMutex);
        for (auto& m : g_mat) m = MatPending{};
    }
}

// A failed compile leaves tint multiplying the game's own colour, and the log says so.
void makeWhitePs(ID3D11Device* device) noexcept {
    if (g_psWhiteTried) return;
    g_psWhiteTried = true;
    ID3D11DeviceContext* imm = nullptr;
    device->GetImmediateContext(&imm);
    if (!imm) return;
    ID3D11PixelShader* ps = shaderSwapCreatePs(imm, kConcourseWhiteBytecode, sizeof(kConcourseWhiteBytecode), "concourse_pane_white", "concourse pane");
    imm->Release();
    g_psWhite.store(ps, std::memory_order_release);
    if (!ps) Log::get().note("concourse pane: the white pixel shader did not compile; tint multiplies the game's own colour, so a dark pane stays dark");
}

// The steady pane's pixel shader: it replaces the glass shader for the whole pane (one flat opaque draw, so the glass shader's reflection of a horizon cannot show).
// Its inputs are the first two outputs of the glass vertex shader (matched by name and order). The level is told by how much the pane faces the anchor's up axis:
// overhead (sky), angled, other. A zero `up` gives `other`. A failed compile is logged once and the steady mode falls back to the plain mirror.
// The pane's shader texts (the steady pixel shader's, the backdrop's, the bake's) are in concourse_pane_shaders.h and compiled at build time (kConcourse*Bytecode).
void makeSkyPs(ID3D11Device* device) noexcept {
    if (g_psSkyTried) return;
    g_psSkyTried = true;
    ID3D11DeviceContext* imm = nullptr;
    device->GetImmediateContext(&imm);
    if (!imm) return;
    ID3D11PixelShader* ps = shaderSwapCreatePs(imm, kConcourseSkyBytecode, sizeof(kConcourseSkyBytecode), "concourse_pane_sky", "concourse pane");
    imm->Release();
    g_psSky.store(ps, std::memory_order_release);
    if (!ps) Log::get().note("concourse pane: the steady pixel shader did not compile; mirror-steady draws the plain mirror");
}

bool makeGlowCb(ID3D11Device* device) noexcept {
    float init[24] = {0, 0, 0, 0, 1, 1, 1, 0, kSteadyColour[0], kSteadyColour[1], kSteadyColour[2], 0, static_cast<float>(cc::kParallaxSphere), cc::kBackdropRadiusDefaultM, 0, 0,
                      cc::kGlassDefault[0], cc::kGlassDefault[1], cc::kGlassDefault[2], cc::kGlassDefault[3], cc::kGlassDefault[4], cc::kGlassDefault[5], 0, 0};
    ID3D11Buffer* made[2] = {};
    for (int i = 0; i < 2; ++i) {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof(init);
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA sd{};
        sd.pSysMem = init;
        const HRESULT h = device->CreateBuffer(&bd, &sd, &made[i]);
        if (FAILED(h) || !made[i]) {
            Log::get().note("concourse pane: the steady constant buffers could not be made (hr %08X); mirror-steady draws the plain mirror", static_cast<unsigned>(h));
            if (made[0]) made[0]->Release();
            return false;
        }
    }
    std::memcpy(g_glowCbSent, init, sizeof(init));
    std::memcpy(g_glowCbBaySent, init, sizeof(init));
    g_glowCbSentValid = true;
    g_cbGlowBay.store(made[1], std::memory_order_release);
    g_cbGlow.store(made[0], std::memory_order_release);
    return true;
}

bool makeBlendStates(ID3D11Device* device) noexcept {
    D3D11_BLEND_DESC d{};
    d.AlphaToCoverageEnable = FALSE;
    d.IndependentBlendEnable = FALSE;
    D3D11_RENDER_TARGET_BLEND_DESC& rt = d.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.BlendOp = D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D11_BLEND_ONE;
    rt.DestBlendAlpha = D3D11_BLEND_ZERO;
    rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = 0x7;   // red, green and blue: the alpha channel is left alone
    ID3D11BlendState* factor = nullptr;
    ID3D11BlendState* mirror = nullptr;
    ID3D11BlendState* fade = nullptr;
    ID3D11BlendState* glow = nullptr;
    rt.SrcBlend = D3D11_BLEND_BLEND_FACTOR;
    rt.DestBlend = D3D11_BLEND_ZERO;
    const HRESULT h1 = device->CreateBlendState(&d, &factor);
    rt.SrcBlend = D3D11_BLEND_ONE;
    const HRESULT h2 = device->CreateBlendState(&d, &mirror);
    rt.SrcBlend = D3D11_BLEND_BLEND_FACTOR;
    rt.DestBlend = D3D11_BLEND_INV_BLEND_FACTOR;
    const HRESULT h3 = device->CreateBlendState(&d, &fade);
    rt.DestBlend = D3D11_BLEND_ONE;
    const HRESULT h4 = device->CreateBlendState(&d, &glow);
    if (FAILED(h1) || FAILED(h2) || FAILED(h3) || FAILED(h4) || !factor || !mirror || !fade || !glow) {
        Log::get().note("concourse pane: the replacement blend states could not be made (hr %08X, %08X, %08X); no pane is treated", static_cast<unsigned>(h1),
                        static_cast<unsigned>(h2), static_cast<unsigned>(h3));
        if (factor) factor->Release();
        if (mirror) mirror->Release();
        if (fade) fade->Release();
        if (glow) glow->Release();
        return false;
    }
    g_bsMirror.store(mirror, std::memory_order_release);
    g_bsFactor.store(factor, std::memory_order_release);
    g_bsFade.store(fade, std::memory_order_release);
    g_bsGlow.store(glow, std::memory_order_release);
    return true;
}

bool makeMirrorTextures(ID3D11Device* device) noexcept {
    static uint8_t t8[cc::kPaneTexSize * cc::kPaneTexSize * 2];
    static uint8_t t9[cc::kPaneTexSize * cc::kPaneTexSize * 2];
    static uint8_t t10[cc::kPaneTexSize * cc::kPaneTexSize * 4];
    static uint8_t glow[cc::kPaneTexSize * cc::kPaneTexSize * 4];
    cc::fillPaneT8(t8);
    cc::fillPaneT9(t9);
    cc::fillPaneT10(t10);
    cc::fillPaneGlowT10(glow);
    struct Spec {
        DXGI_FORMAT format;
        const uint8_t* data;
        UINT bytes;
    };
    const Spec specs[4] = {{DXGI_FORMAT_R8G8_UNORM, t8, 2}, {DXGI_FORMAT_R8G8_UNORM, t9, 2}, {DXGI_FORMAT_R8G8B8A8_UNORM, t10, 4}, {DXGI_FORMAT_R8G8B8A8_UNORM, glow, 4}};
    ID3D11ShaderResourceView* made[4] = {};
    bool ok = true;
    for (int i = 0; i < 4 && ok; ++i) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = cc::kPaneTexSize;
        td.Height = cc::kPaneTexSize;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = specs[i].format;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA sd{};
        sd.pSysMem = specs[i].data;
        sd.SysMemPitch = cc::kPaneTexSize * specs[i].bytes;
        ID3D11Texture2D* tex = nullptr;
        if (FAILED(device->CreateTexture2D(&td, &sd, &tex)) || !tex) {
            ok = false;
            break;
        }
        D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
        vd.Format = specs[i].format;
        vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        vd.Texture2DArray.MostDetailedMip = 0;
        vd.Texture2DArray.MipLevels = 1;
        vd.Texture2DArray.FirstArraySlice = 0;
        vd.Texture2DArray.ArraySize = 1;
        const HRESULT h = device->CreateShaderResourceView(tex, &vd, &made[i]);
        tex->Release();
        if (FAILED(h) || !made[i]) ok = false;
    }
    if (!ok) {
        for (auto* v : made)
            if (v) v->Release();
        Log::get().note("concourse pane: the mirror textures could not be made; no pane is treated");
        return false;
    }
    g_glowT10.store(made[3], std::memory_order_release);
    for (int i = 0; i < 3; ++i) g_mirrorSrv[i].store(made[i], std::memory_order_release);
    return true;
}

bool makeMirrorCb(ID3D11Device* device) noexcept {
    static const float kRows[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1.8f, 0};   // row 2: F0 scale 1, roughness scale 1.8 (0.30 * 1.8 = 0.54)
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = kPaneCbBytes;
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA sd{};
    sd.pSysMem = kRows;
    ID3D11Buffer* b = nullptr;
    const HRESULT h = device->CreateBuffer(&bd, &sd, &b);
    if (FAILED(h) || !b) {
        Log::get().note("concourse pane: the mirror constant buffer could not be made (hr %08X); mirror-soft is not available", static_cast<unsigned>(h));
        return false;
    }
    g_cbMirror.store(b, std::memory_order_release);
    return true;
}

// The mark pass's vertex shader: one fullscreen triangle at the depth of the beyond distance (reversed Z: 0.025 / metres). Recompiled when the key changes; the
// old shader is not released (a draw thread may hold it for one more draw, and a change of the key is a developer's act).
void ensureMarkVs(ID3D11Device* device) noexcept {
    const float want = g_beyondM.load(std::memory_order_relaxed);
    if (g_vsMarkTried && g_vsMarkM == want) return;
    g_vsMarkTried = true;
    g_vsMarkM = want;
    ID3D11DeviceContext* imm = nullptr;
    device->GetImmediateContext(&imm);
    if (!imm) return;
    const float z = cc::markDepthForDistance(want);
    char hlsl[400] = {};
    const int len = std::snprintf(hlsl, sizeof(hlsl),
                                  "float4 main(uint id : SV_VertexID) : SV_Position { float2 p = float2((id << 1) & 2, id & 2); return float4(p.x * 2 - 1, 1 - p.y * 2, %.9g, 1); }\n",
                                  static_cast<double>(z));
    ID3D11VertexShader* vs = len > 0 ? shaderSwapCompileVs(imm, hlsl, static_cast<size_t>(len), "main", "concourse_pane_mark", nullptr, "concourse pane") : nullptr;
    imm->Release();
    if (!vs) {
        Log::get().note("concourse pane: the mark vertex shader did not compile (beyond %.0f m); the mirror cannot be limited to the far pixels, so it does not draw", static_cast<double>(want));
        g_vsMark.store(nullptr, std::memory_order_release);
        return;
    }
    g_vsMark.store(vs, std::memory_order_release);
}

bool makeMarkStates(ID3D11Device* device) noexcept {
    if (g_markStatesTried) return g_rsMark.load(std::memory_order_acquire) != nullptr;
    g_markStatesTried = true;
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    rd.ScissorEnable = FALSE;
    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].BlendEnable = FALSE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_ZERO;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = 0;
    D3D11_DEPTH_STENCIL_DESC dd{};
    dd.DepthEnable = TRUE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
    dd.StencilEnable = TRUE;
    dd.StencilReadMask = 0;
    dd.StencilWriteMask = static_cast<UINT8>(kMarkRef | kBakedBit);   // a marked pixel starts this frame with the bake not yet painted on it
    for (D3D11_DEPTH_STENCILOP_DESC* f : {&dd.FrontFace, &dd.BackFace}) {
        f->StencilFunc = D3D11_COMPARISON_ALWAYS;
        f->StencilFailOp = D3D11_STENCIL_OP_KEEP;
        f->StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
        f->StencilPassOp = D3D11_STENCIL_OP_REPLACE;
    }
    ID3D11RasterizerState* rs = nullptr;
    ID3D11BlendState* bs = nullptr;
    ID3D11DepthStencilState* ds = nullptr;
    const HRESULT h1 = device->CreateRasterizerState(&rd, &rs);
    const HRESULT h2 = device->CreateBlendState(&bd, &bs);
    const HRESULT h3 = device->CreateDepthStencilState(&dd, &ds);
    if (FAILED(h1) || FAILED(h2) || FAILED(h3) || !rs || !bs || !ds) {
        Log::get().note("concourse pane: the mark pass's states could not be made (hr %08X, %08X, %08X); the mirror does not draw", static_cast<unsigned>(h1),
                        static_cast<unsigned>(h2), static_cast<unsigned>(h3));
        if (rs) rs->Release();
        if (bs) bs->Release();
        if (ds) ds->Release();
        return false;
    }
    g_bsMark.store(bs, std::memory_order_release);
    g_dsMark.store(ds, std::memory_order_release);
    g_rsMark.store(rs, std::memory_order_release);
    return true;
}

bool makeTests(ID3D11Device* device) noexcept {
    std::lock_guard<std::mutex> lock(g_sideMutex);
    if (g_testsMade) return true;
    FlatComputeInternalScope internal;
    D3D11_QUERY_DESC qd{};
    qd.Query = D3D11_QUERY_OCCLUSION;
    for (size_t i = 0; i < kTestSlots; ++i) {
        if (FAILED(device->CreateQuery(&qd, &g_tests[i].qDrawn)) || !g_tests[i].qDrawn || FAILED(device->CreateQuery(&qd, &g_tests[i].qMirror)) || !g_tests[i].qMirror) {
            Log::get().note("concourse pane: the side test queries could not be made (test %u of %u); the panes' sides are not learned", static_cast<unsigned>(i),
                            static_cast<unsigned>(kTestSlots));
            for (size_t k = 0; k <= i; ++k) {
                if (g_tests[k].qDrawn) g_tests[k].qDrawn->Release();
                if (g_tests[k].qMirror) g_tests[k].qMirror->Release();
                g_tests[k] = TestSlot{};
            }
            return false;
        }
    }
    // The probe queries (automatic capture): a failure costs only the probes, said once; the tests go on.
    bool probesOk = true;
    for (size_t i = 0; i < kTestSlots && probesOk; ++i) probesOk = SUCCEEDED(device->CreateQuery(&qd, &g_tests[i].qProbe)) && g_tests[i].qProbe != nullptr;
    if (!probesOk) {
        Log::get().note("concourse backdrop: the probe queries could not be made; automatic capture cannot measure what is missing (F6 still captures)");
        for (auto& s : g_tests) {
            if (s.qProbe) s.qProbe->Release();
            s.qProbe = nullptr;
        }
    }
    g_testsMade = true;
    return true;
}

bool makeStates(ID3D11Device* device) noexcept {
    if (!device) return false;
    if (g_statesReady.load(std::memory_order_acquire)) return true;
    makeWhitePs(device);
    makeSkyPs(device);
    if (!g_cbGlow.load(std::memory_order_acquire)) makeGlowCb(device);
    makeMarkStates(device);
    ensureMarkVs(device);
    g_markReady.store(g_rsMark.load(std::memory_order_acquire) != nullptr && g_vsMark.load(std::memory_order_acquire) != nullptr, std::memory_order_release);
    if (!g_bsMirror.load(std::memory_order_acquire) && !makeBlendStates(device)) return false;
    if (!g_mirrorSrv[0].load(std::memory_order_acquire) && !makeMirrorTextures(device)) return false;
    if (!g_cbMirror.load(std::memory_order_acquire)) makeMirrorCb(device);   // mirror-soft only: a failure is logged and the plain mirror carries on
    g_statesReady.store(true, std::memory_order_release);
    return true;
}

bool makeSlots(ID3D11Device* device) noexcept {
    std::lock_guard<std::mutex> lock(g_shareMutex);
    if (g_slotsMade) return true;
    FlatComputeInternalScope internal;
    D3D11_QUERY_DESC qd{};
    qd.Query = D3D11_QUERY_OCCLUSION;
    for (size_t i = 0; i < kShareSlots; ++i) {
        if (FAILED(device->CreateQuery(&qd, &g_slots[i].q)) || !g_slots[i].q) {
            Log::get().note("concourse pane: the share query pool could not be made (query %u of %u); the shares are not measured", static_cast<unsigned>(i),
                            static_cast<unsigned>(kShareSlots));
            for (size_t k = 0; k < i; ++k) {
                if (g_slots[k].q) g_slots[k].q->Release();
                g_slots[k] = ShareSlot{};
            }
            return false;
        }
    }
    g_slotsMade = true;
    return true;
}

bool makePool(ID3D11Device* device) noexcept {
    if (g_poolMade) return true;
    FlatComputeInternalScope internal;
    D3D11_QUERY_DESC qd{};
    qd.Query = D3D11_QUERY_OCCLUSION;
    for (size_t i = 0; i < kPool; ++i) {
        if (FAILED(device->CreateQuery(&qd, &g_pool[i])) || !g_pool[i]) {
            Log::get().note("concourse pane capture: the query pool could not be made (query %u of %u)", static_cast<unsigned>(i), static_cast<unsigned>(kPool));
            for (size_t k = 0; k < i; ++k) {
                if (g_pool[k]) g_pool[k]->Release();
                g_pool[k] = nullptr;
            }
            return false;
        }
    }
    g_poolMade = true;
    return true;
}

// ---- The share bins (under g_shareMutex) -------------------------------------------------------------------------------------------------------------
Bin* binFor(uint32_t frame, bool create) noexcept {
    for (auto& b : g_bins)
        if (b.used && b.frame == frame) return &b;
    if (!create) return nullptr;
    Bin* pick = nullptr;
    for (auto& b : g_bins)
        if (!b.used) {
            pick = &b;
            break;
        }
    if (!pick) {
        pick = &g_bins[0];
        for (auto& b : g_bins)
            if (b.frame < pick->frame) pick = &b;
    }
    *pick = Bin{};
    pick->used = true;
    pick->frame = frame;
    return pick;
}

// Opens a query around a family draw (immediate context only). Returns the slot, or -1.
// `side` is what the side table said when the draw came (read by the caller before this call: this takes the shares' mutex, and the table has its own).
int openShareQuery(ID3D11DeviceContext* ctx, const cc::PaneKey& key, cc::PaneSide side) noexcept {
    D3D11_VIEWPORT vp{};
    UINT nvp = 1;
    ctx->RSGetViewports(&nvp, &vp);
    if (nvp < 1 || !(vp.Width > 0.0f) || !(vp.Height > 0.0f)) return -1;
    int idx = -1;
    {
        std::lock_guard<std::mutex> lock(g_shareMutex);
        if (!g_slotsMade) return -1;
        for (size_t i = 0; i < kShareSlots; ++i)
            if (g_slots[i].state == 0 && g_slots[i].q) {
                idx = static_cast<int>(i);
                break;
            }
        if (idx < 0) {
            ++g_shareDropped;
            return -1;
        }
        const uint32_t frame = g_frameNow.load(std::memory_order_relaxed);
        ShareSlot& s = g_slots[idx];
        s.state = 1;
        s.frame = frame;
        s.n = key.n;
        s.key = key;
        s.side = side;
        s.pixels = static_cast<double>(vp.Width) * static_cast<double>(vp.Height);
        Bin* b = binFor(frame, true);
        if (b) ++b->outstanding;
    }
    FlatComputeInternalScope internal;
    ctx->Begin(g_slots[idx].q);
    return idx;
}

void closeShareQuery(ID3D11DeviceContext* ctx, int idx) noexcept {
    {
        FlatComputeInternalScope internal;
        ctx->End(g_slots[idx].q);
    }
    std::lock_guard<std::mutex> lock(g_shareMutex);
    g_slots[idx].state = 2;
}

// The tick: reads what arrived, closes the frames that are complete. `measuring` opens an (empty) bin for the frame just ended, so a frame with no family draw
// is a reading of zero rather than no reading.
void pollShares(ID3D11Device* device, uint32_t prevFrame, uint32_t frame, bool measuring, uint64_t nowMs) noexcept {
    ID3D11DeviceContext* imm = nullptr;
    device->GetImmediateContext(&imm);
    if (!imm) return;
    {
        std::lock_guard<std::mutex> lock(g_shareMutex);
        if (!g_slotsMade) {
            imm->Release();
            return;
        }
        if (measuring) binFor(prevFrame, true);
        FlatComputeInternalScope internal;
        for (auto& s : g_slots) {
            if (s.state != 2) continue;
            UINT64 samples = 0;
            if (imm->GetData(s.q, &samples, sizeof(samples), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
            Bin* b = binFor(s.frame, false);
            if (b) {
                const float share = s.pixels > 0.0 ? static_cast<float>(static_cast<double>(samples) / s.pixels) : 0.0f;
                uint32_t k = 0;
                while (k < b->count && b->n[k] != s.n) ++k;
                if (k == b->count && k < kBinMaxN) {
                    b->n[k] = s.n;
                    b->share[k] = share;
                    ++b->count;
                } else if (k < b->count && share > b->share[k]) {
                    b->share[k] = share;
                }
                if (b->sampleCount < kShareSlots) {
                    cc::GlassSample& g = b->samples[b->sampleCount++];
                    g.key = s.key;
                    g.share = share;
                    g.pixels = s.pixels;
                    g.side = s.side;
                } else {
                    ++g_sampleDropped;
                }
                if (b->outstanding > 0) --b->outstanding;
            }
            s.state = 0;
        }
        Bin* best = nullptr;
        for (auto& b : g_bins) {
            if (!b.used) continue;
            if (b.frame < frame && b.outstanding == 0) {
                if (!best || b.frame > best->frame) best = &b;
            } else if (frame - b.frame > 120u) {
                b.used = false;
            }
        }
        if (best && measuring && (!g_latest.valid || best->frame >= g_latestFrame)) {
            ConcoursePaneShares r;
            r.valid = true;
            r.seq = ++g_seqCounter;
            r.count = best->count;
            for (uint32_t k = 0; k < best->count; ++k) {
                r.n[k] = best->n[k];
                r.share[k] = best->share[k];
            }
            // The total of the outside glass in the main view (concourse_cull_core.h exteriorGlassShare), then the main view's size is remembered for the next frame.
            r.exterior = cc::exteriorGlassShare(best->samples, best->sampleCount, g_mainPixels, g_bayN.load(std::memory_order_relaxed));
            if (best->sampleCount > 0) {
                double frameMax = 0.0;
                for (uint32_t k = 0; k < best->sampleCount; ++k)
                    if (best->samples[k].pixels > frameMax) frameMax = best->samples[k].pixels;
                g_mainPixels = cc::mainViewPixelsNext(g_mainPixels, frameMax);
            }
            g_latest = r;
            g_latestMs = nowMs;
            g_latestFrame = best->frame;
        }
        if (best) {
            for (auto& b : g_bins)
                if (b.used && b.frame <= best->frame && b.outstanding == 0 && b.frame < frame) b.used = false;
        }
        if (!measuring) {
            g_latest = ConcoursePaneShares{};
            for (auto& b : g_bins) b = Bin{};
        }
    }
    imm->Release();
}

// ---- The derived depth state -------------------------------------------------------------------------------------------------------------------
// nullptr when the variant cannot be made (a full cache, or the device refused): the caller does not draw the mirror then.
// kProbe is the backdrop capture pass's: the game's depth test, no depth write, no stencil. kPlain and kMarked are the mirror's: depth write on, without and with the mark's test. kFarOut is the far-limited glows': the game's depth state as it is, and only
// the pixels the mark did not set.
ID3D11DepthStencilState* derivedDepth(ID3D11DeviceContext* ctx, ID3D11DepthStencilState* orig, DsVariant variant) noexcept {
    std::lock_guard<std::mutex> lock(g_dsMutex);
    size_t at = g_dsN;
    for (size_t i = 0; i < g_dsN; ++i)
        if (g_ds[i].orig == orig) {
            at = i;
            break;
        }
    if (at == g_dsN) {
        if (g_dsN >= sizeof(g_ds) / sizeof(g_ds[0])) return nullptr;
        orig->AddRef();
        g_ds[g_dsN] = DsEntry{};
        g_ds[g_dsN].orig = orig;
        ++g_dsN;
    }
    ID3D11DepthStencilState*& slot = variant == DsVariant::kFarOut ? g_ds[at].farOut : variant == DsVariant::kMarked ? g_ds[at].marked : variant == DsVariant::kMarkedOnce ? g_ds[at].markedOnce
                                   : variant == DsVariant::kProbe ? g_ds[at].probe : variant == DsVariant::kMarkedProbe ? g_ds[at].markedProbe : g_ds[at].plain;
    if (slot) return slot;
    D3D11_DEPTH_STENCIL_DESC d{};
    orig->GetDesc(&d);
    if (variant == DsVariant::kProbe || variant == DsVariant::kMarkedProbe) d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;   // (kMarkedProbe: the display's mark test, nothing written)   // the backdrop's capture pass: the game's depth test, nothing written
    else if (variant != DsVariant::kFarOut) d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    if (variant != DsVariant::kPlain && variant != DsVariant::kProbe) {
        d.StencilEnable = TRUE;
        d.StencilReadMask = static_cast<UINT8>(kMarkRef);
        d.StencilWriteMask = 0;
        for (D3D11_DEPTH_STENCILOP_DESC* f : {&d.FrontFace, &d.BackFace}) {
            f->StencilFunc = variant == DsVariant::kFarOut ? D3D11_COMPARISON_NOT_EQUAL : D3D11_COMPARISON_EQUAL;
            f->StencilFailOp = D3D11_STENCIL_OP_KEEP;
            f->StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
            f->StencilPassOp = D3D11_STENCIL_OP_KEEP;
        }
        if (variant == DsVariant::kMarkedOnce) {   // the reference is kMarkRef, so this passes on the mark with kBakedBit clear, and sets it
            d.StencilReadMask = static_cast<UINT8>(kMarkRef | kBakedBit);
            d.StencilWriteMask = static_cast<UINT8>(kBakedBit);
            for (D3D11_DEPTH_STENCILOP_DESC* f : {&d.FrontFace, &d.BackFace}) f->StencilPassOp = D3D11_STENCIL_OP_INVERT;
        }
    } else {
        d.StencilEnable = FALSE;
        d.StencilWriteMask = 0;
    }
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    ID3D11DepthStencilState* derived = nullptr;
    if (dev) {
        if (FAILED(dev->CreateDepthStencilState(&d, &derived))) derived = nullptr;
        dev->Release();
    }
    slot = derived;   // a refusal is remembered as null: the variant is not tried again
    return derived;
}

// ---- The tint colour of an index count ---------------------------------------------------------------------------------------------------------
size_t tintSlotOf(uint32_t n) noexcept {
    size_t slot = 0;
    bool fresh = false;
    {
        std::lock_guard<std::mutex> lock(g_tintMutex);
        size_t i = 0;
        while (i < g_tintCount && g_tintN[i] != n) ++i;
        if (i < g_tintCount) {
            slot = i;
        } else if (g_tintCount < cc::kPaneColourCount) {
            g_tintN[g_tintCount] = n;
            slot = g_tintCount++;
            fresh = true;
        } else {
            slot = cc::kPaneColourCount - 1;   // the table is full: the last colour repeats
        }
    }
    if (fresh) Log::get().note("concourse pane: n=%u colour %s", n, cc::kPaneColourNames[slot % cc::kPaneColourCount]);
    return slot;
}

// ---- The one-shot material log ---------------------------------------------------------------------------------------------------------------
const char* formatName(DXGI_FORMAT f) noexcept {
    switch (f) {
        case DXGI_FORMAT_R8G8_UNORM: return "R8G8_UNORM";
        case DXGI_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "R8G8B8A8_UNORM_SRGB";
        case DXGI_FORMAT_R8_UNORM: return "R8_UNORM";
        case DXGI_FORMAT_BC1_UNORM: return "BC1_UNORM";
        case DXGI_FORMAT_BC1_UNORM_SRGB: return "BC1_UNORM_SRGB";
        case DXGI_FORMAT_BC3_UNORM: return "BC3_UNORM";
        case DXGI_FORMAT_BC3_UNORM_SRGB: return "BC3_UNORM_SRGB";
        case DXGI_FORMAT_BC5_UNORM: return "BC5_UNORM";
        case DXGI_FORMAT_BC7_UNORM: return "BC7_UNORM";
        case DXGI_FORMAT_BC7_UNORM_SRGB: return "BC7_UNORM_SRGB";
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return "D32_FLOAT_S8X24_UINT";
        case DXGI_FORMAT_D24_UNORM_S8_UINT: return "D24_UNORM_S8_UINT";
        case DXGI_FORMAT_D32_FLOAT: return "D32_FLOAT";
        case DXGI_FORMAT_D16_UNORM: return "D16_UNORM";
        case DXGI_FORMAT_R32G8X24_TYPELESS: return "R32G8X24_TYPELESS";
        case DXGI_FORMAT_R24G8_TYPELESS: return "R24G8_TYPELESS";
        case DXGI_FORMAT_UNKNOWN: return "UNKNOWN";
        default: return nullptr;
    }
}

void describeSrv(ID3D11ShaderResourceView* v, UINT slot, char* out, size_t cap) noexcept {
    if (!v) {
        std::snprintf(out, cap, "t%u none", slot);
        return;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
    v->GetDesc(&vd);
    ID3D11Resource* r = nullptr;
    v->GetResource(&r);
    ID3D11Texture2D* t = nullptr;
    if (r) r->QueryInterface(IID_PPV_ARGS(&t));
    if (t) {
        D3D11_TEXTURE2D_DESC td{};
        t->GetDesc(&td);
        const char* fn = formatName(td.Format);
        char fmt[24] = {};
        if (fn) std::snprintf(fmt, sizeof(fmt), "%s", fn);
        else std::snprintf(fmt, sizeof(fmt), "format %u", static_cast<unsigned>(td.Format));
        std::snprintf(out, cap, "t%u %s %ux%u array=%u mips=%u view=%u", slot, fmt, td.Width, td.Height, td.ArraySize, td.MipLevels,
                      static_cast<unsigned>(vd.ViewDimension));
        t->Release();
    } else {
        std::snprintf(out, cap, "t%u not a Texture2D (view dimension %u)", slot, static_cast<unsigned>(vd.ViewDimension));
    }
    if (r) r->Release();
}

// The first sight of an index count of the family: the game's three textures at slots 8 to 10 and constant buffer 2, on the immediate context. The buffer is
// copied to a staging buffer (from its first-constant offset) and read a few frames later by the tick.
void materialOnce(ID3D11DeviceContext* ctx, uint32_t n) noexcept {
    {
        std::lock_guard<std::mutex> lock(g_matMutex);
        if (g_matSeen >= kMatMax) return;
        for (size_t i = 0; i < g_matSeen; ++i)
            if (g_matSeenN[i] == n) return;
        g_matSeenN[g_matSeen++] = n;
    }
    ID3D11ShaderResourceView* srv[3] = {};
    ctx->PSGetShaderResources(kPaneSrvFirst, kPaneSrvCount, srv);
    char d[3][160] = {};
    for (UINT i = 0; i < 3; ++i) describeSrv(srv[i], kPaneSrvFirst + i, d[i], sizeof(d[i]));
    for (auto* v : srv)
        if (v) v->Release();
    MatPending mp;
    ID3D11DeviceContext1* c1 = nullptr;
    ctx->QueryInterface(IID_PPV_ARGS(&c1));
    ID3D11Buffer* cb = nullptr;
    UINT first = 0, num = 0;
    if (c1) c1->PSGetConstantBuffers1(kPaneCbSlot, 1, &cb, &first, &num);
    else ctx->PSGetConstantBuffers(kPaneCbSlot, 1, &cb);
    if (c1) c1->Release();
    char tail[200] = {};
    ID3D11Buffer* staging = nullptr;
    if (!cb) {
        std::snprintf(tail, sizeof(tail), "cb2 not bound");
    } else {
        D3D11_BUFFER_DESC bd{};
        cb->GetDesc(&bd);
        const UINT off = first * 16u;
        if (off >= bd.ByteWidth) {
            std::snprintf(tail, sizeof(tail), "cb2[2] unreadable (first constant %u is past the buffer's %u bytes)", first, bd.ByteWidth);
        } else {
            const UINT len = std::min<UINT>(kPaneCbBytes, bd.ByteWidth - off);
            ID3D11Device* dev = nullptr;
            ctx->GetDevice(&dev);
            D3D11_BUFFER_DESC sd{};
            sd.ByteWidth = kPaneCbBytes;
            sd.Usage = D3D11_USAGE_STAGING;
            sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (dev && SUCCEEDED(dev->CreateBuffer(&sd, nullptr, &staging)) && staging) {
                D3D11_BOX box{};
                box.left = off;
                box.right = off + len;
                box.top = 0;
                box.bottom = 1;
                box.front = 0;
                box.back = 1;
                FlatComputeInternalScope internal;
                ctx->CopySubresourceRegion(staging, 0, 0, 0, 0, cb, 0, &box);
                std::snprintf(tail, sizeof(tail), "(cb2 buffer %u bytes, first constant %u, count %u, copied %u bytes)", bd.ByteWidth, first, num, len);
            } else {
                std::snprintf(tail, sizeof(tail), "cb2[2] unreadable (the staging buffer could not be made)");
            }
            if (dev) dev->Release();
        }
        cb->Release();
    }
    char head[620] = {};
    std::snprintf(head, sizeof(head), "concourse pane: n=%u material %s, %s, %s", n, d[0], d[1], d[2]);
    if (!staging) {
        Log::get().note("%s, %s", head, tail);
        return;
    }
    mp.staging = staging;
    mp.dueFrame = g_frameNow.load(std::memory_order_relaxed) + 2;
    // The text up to the values; the tick adds the floats.
    std::snprintf(mp.text, sizeof(mp.text), "%s, cb2[2] = %%g %%g %%g %%g %s", head, tail);
    std::lock_guard<std::mutex> lock(g_matMutex);
    for (auto& slot : g_mat)
        if (!slot.staging) {
            slot = mp;
            return;
        }
    staging->Release();
}

void readMaterials(ID3D11Device* device, uint32_t frame) noexcept {
    ID3D11DeviceContext* imm = nullptr;
    std::lock_guard<std::mutex> lock(g_matMutex);
    for (auto& m : g_mat) {
        if (!m.staging || frame < m.dueFrame) continue;
        if (!imm) {
            device->GetImmediateContext(&imm);
            if (!imm) return;
        }
        FlatComputeInternalScope internal;
        D3D11_MAPPED_SUBRESOURCE ms{};
        const HRESULT h = imm->Map(m.staging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &ms);
        if (SUCCEEDED(h)) {
            float f[12] = {};
            std::memcpy(f, ms.pData, sizeof(f));
            imm->Unmap(m.staging, 0);
            char line[900] = {};
            std::snprintf(line, sizeof(line), m.text, static_cast<double>(f[8]), static_cast<double>(f[9]), static_cast<double>(f[10]), static_cast<double>(f[11]));
            Log::get().note("%s", line);
            m.staging->Release();
            m = MatPending{};
        } else if (++m.tries > 120) {
            Log::get().note("concourse pane: a material read-back never completed (hr %08X)", static_cast<unsigned>(h));
            m.staging->Release();
            m = MatPending{};
        }
    }
    if (imm) imm->Release();
}

// ---- The capture's qualifying draw ---------------------------------------------------------------------------------------------------------------
// RT0 blending on, a colour target bound, a viewport at least 1000 wide. Opens a query around the draw.
int captureBegin(ID3D11DeviceContext* ctx, UINT indexCount) noexcept {
    ID3D11BlendState* bs = nullptr;
    FLOAT factor[4] = {};
    UINT mask = 0;
    ctx->OMGetBlendState(&bs, factor, &mask);
    if (!bs) return -1;
    D3D11_BLEND_DESC bd{};
    bs->GetDesc(&bd);
    bs->Release();
    if (!bd.RenderTarget[0].BlendEnable) return -1;
    ID3D11RenderTargetView* rtv = nullptr;
    ctx->OMGetRenderTargets(1, &rtv, nullptr);
    if (!rtv) return -1;
    rtv->Release();
    D3D11_VIEWPORT vp{};
    UINT nvp = 1;
    ctx->RSGetViewports(&nvp, &vp);
    if (nvp < 1 || !(vp.Width >= 1000.0f)) return -1;
    CapDraw d;
    d.n = indexCount;
    shaderHashes(ctx, &d.vs, &d.ps);
    d.vpW = static_cast<uint32_t>(vp.Width);
    d.vpH = static_cast<uint32_t>(vp.Height);
    d.src = static_cast<uint32_t>(bd.RenderTarget[0].SrcBlend);
    d.dst = static_cast<uint32_t>(bd.RenderTarget[0].DestBlend);
    ID3D11DepthStencilState* ds = nullptr;
    UINT ref = 0;
    ctx->OMGetDepthStencilState(&ds, &ref);
    if (ds) {
        D3D11_DEPTH_STENCIL_DESC dd{};
        ds->GetDesc(&dd);
        ds->Release();
        d.depthWrite = dd.DepthEnable && dd.DepthWriteMask != D3D11_DEPTH_WRITE_MASK_ZERO ? 1 : 0;
        d.stencil = dd.StencilEnable ? 1u : 0u;
    }
    int idx = -1;
    {
        std::lock_guard<std::mutex> lock(g_capMutex);
        if (g_capN < kPool && g_pool[g_capN]) {
            idx = static_cast<int>(g_capN++);
            g_cap[idx] = d;
        } else {
            ++g_capDropped;
        }
    }
    if (idx < 0) return -1;
    FlatComputeInternalScope internal;
    ctx->Begin(g_pool[idx]);
    return idx;
}

// ---- The side tests -----------------------------------------------------------------------------------------------------------------------------
int testOpen(ID3D11DeviceContext* ctx, const cc::PaneKey& key) noexcept {
    int idx = -1;
    D3D11_VIEWPORT vp{};
    UINT nvp = 1;
    ctx->RSGetViewports(&nvp, &vp);
    const double pixels = nvp >= 1 ? static_cast<double>(vp.Width) * static_cast<double>(vp.Height) : 0.0;
    {
        std::lock_guard<std::mutex> lock(g_sideMutex);
        if (!g_testsMade) return -1;
        for (size_t i = 0; i < kTestSlots; ++i)
            if (g_tests[i].state == 0 && g_tests[i].qDrawn) {
                idx = static_cast<int>(i);
                break;
            }
        if (idx < 0) return -1;
        g_tests[idx].state = 1;
        g_tests[idx].probe = 0;
        g_tests[idx].frame = g_frameNow.load(std::memory_order_relaxed);
        g_tests[idx].key = key;
        g_tests[idx].pixels = pixels;
    }
    FlatComputeInternalScope internal;
    ctx->Begin(g_tests[idx].qDrawn);
    return idx;
}

// End of the game's draw: the first query closes and the slot waits for the mirror.
void testCloseDrawn(ID3D11DeviceContext* ctx, int idx) noexcept {
    {
        FlatComputeInternalScope internal;
        ctx->End(g_tests[idx].qDrawn);
    }
    std::lock_guard<std::mutex> lock(g_sideMutex);
    g_tests[idx].state = 2;
}

void testOpenMirror(ID3D11DeviceContext* ctx, int idx) noexcept {
    {
        FlatComputeInternalScope internal;
        ctx->Begin(g_tests[idx].qMirror);
    }
    std::lock_guard<std::mutex> lock(g_sideMutex);
    g_tests[idx].state = 3;
}

void testCloseMirror(ID3D11DeviceContext* ctx, int idx) noexcept {
    {
        FlatComputeInternalScope internal;
        ctx->End(g_tests[idx].qMirror);
    }
    std::lock_guard<std::mutex> lock(g_sideMutex);
    g_tests[idx].state = 4;
}

// A test that did not get its mirror draw: the slot is free again (a query that ended and was not read may begin again).
void testAbandon(int idx) noexcept {
    std::lock_guard<std::mutex> lock(g_sideMutex);
    g_tests[idx].state = 0;
    g_tests[idx].probe = 0;
}

cc::PaneSide sideOf(const cc::PaneKey& key) noexcept {
    std::lock_guard<std::mutex> lock(g_sideMutex);
    return g_sides.side(key);
}

// The tick: reads the tests that arrived and feeds the table.
void pollTests(ID3D11Device* device, uint32_t frame) noexcept {
    ID3D11DeviceContext* imm = nullptr;
    device->GetImmediateContext(&imm);
    if (!imm) return;
    {
        std::lock_guard<std::mutex> lock(g_sideMutex);
        if (g_testsMade) {
            FlatComputeInternalScope internal;
            for (auto& t : g_tests) {
                if (t.state != 4) {
                    if (t.state != 0 && static_cast<uint32_t>(frame - t.frame) > 600u) {
                        t.state = 0;   // stuck open (a draw that never reached its end)
                        t.probe = 0;
                    }
                    continue;
                }
                UINT64 drawn = 0, mirrored = 0, probed = 0;
                const bool hadProbe = t.probe == 2;
                const bool arrived = imm->GetData(t.qDrawn, &drawn, sizeof(drawn), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
                                     imm->GetData(t.qMirror, &mirrored, sizeof(mirrored), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
                                     (!hadProbe || imm->GetData(t.qProbe, &probed, sizeof(probed), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK);
                if (!arrived) {
                    if (static_cast<uint32_t>(frame - t.frame) > 600u) {   // a query that never returns: the slot is freed (the probe counters then show issued without read)
                        t.state = 0;
                        t.probe = 0;
                    }
                    continue;
                }
                const cc::PaneKey key = t.key;
                const double pixels = t.pixels;
                const uint32_t drawFrame = t.frame;
                t.state = 0;
                t.probe = 0;
                g_testsRead.fetch_add(1, std::memory_order_relaxed);
                if (hadProbe) g_probeRead.fetch_add(1, std::memory_order_relaxed);
                const cc::PaneSides::Change c = g_sides.note(key, drawn, mirrored);
                // The backdrop's missing share: what a probe counted (pixels the cube cannot show), or, with no cube yet, the whole of what the pane would draw (the mirror count).
                if (g_mode.load(std::memory_order_relaxed) == kModeBackdrop && pixels > 0.0 && g_sides.side(key) == cc::PaneSide::kExterior) {
                    double miss = -1.0;
                    if (hadProbe) miss = static_cast<double>(probed);
                    else if (!g_bdCubeValid.load(std::memory_order_relaxed)) miss = static_cast<double>(mirrored);
                    if (miss >= 0.0) {
                        g_miss[g_missNext].frame = drawFrame;
                        g_miss[g_missNext].missing = miss;
                        g_missNext = (g_missNext + 1) % kMissEntries;
                        g_missPixels = pixels;
                    }
                }
                if (c == cc::PaneSides::Change::kNone) continue;
                const bool ext = g_sides.side(key) == cc::PaneSide::kExterior;
                uint32_t& cap = (c == cc::PaneSides::Change::kFirst && ext) ? g_sideLogExterior : g_sideLogOther;
                if (cap >= kSideLogCap) continue;
                ++cap;
                const char* word = c == cc::PaneSides::Change::kChanged ? (ext ? "changed side to exterior" : "changed side to interior") : (ext ? "exterior" : "interior");
                Log::get().note("concourse pane: %s n=%u start=%u base=%d (drawn %llu, mirrored %llu)", word, key.n, key.startIndex, key.baseVertex,
                                static_cast<unsigned long long>(drawn), static_cast<unsigned long long>(mirrored));
            }
        }
    }
    imm->Release();
}

// ---- The mark pass -----------------------------------------------------------------------------------------------------------------------------
// ---- The backdrop's resources and the scene copy (PROTOTYPE, docs\concourse-cull-2026-11-05.md) ------------------------------------------------------------
template <class T>
void releaseAtomic(std::atomic<T*>& a) noexcept {
    T* p = a.exchange(nullptr, std::memory_order_acq_rel);   // the draw threads may hold the pointer for one more draw
    if (p) p->Release();
}

void releaseBackdrop() noexcept {
    g_bakeBaked.store(false, std::memory_order_release);
    g_bdArmed.store(false, std::memory_order_release);
    g_bdCubeValid.store(false, std::memory_order_release);
    g_bdOriginSet.store(false, std::memory_order_release);
    g_bdCaptures.store(0, std::memory_order_release);
    g_bdSceneSource.store(nullptr, std::memory_order_release);
    g_bdSceneFrame.store(0xFFFFFFFFu, std::memory_order_relaxed);
    g_bdSceneOkFrame.store(0xFFFFFFFFu, std::memory_order_relaxed);
    releaseAtomic(g_bdSceneSrv);
    releaseAtomic(g_bdScene);
    releaseAtomic(g_bdDepthSrv);
    releaseAtomic(g_bdDepth);
    releaseAtomic(g_bdOriginSrv);
    releaseAtomic(g_bdOrigin);
    releaseAtomic(g_bdBackUav);
    releaseAtomic(g_bdDistBackUav);
    releaseAtomic(g_bdFrontSrv);
    releaseAtomic(g_bdDistFrontSrv);
    releaseAtomic(g_bdBack);
    releaseAtomic(g_bdFront);
    releaseAtomic(g_bdDistBack);
    releaseAtomic(g_bdDistFront);
    // The three backdrop pixel shaders are not released here: they are made per device, like g_csBake, and kept across the cube's release so a concourse entry does not
    // create them again (releaseBackdropShaders, from releaseStates, and makeBackdropShaders on a device change).
    releaseAtomic(g_bdSampler);
    std::lock_guard<std::mutex> lock(g_bdMutex);
    g_bdSceneW = g_bdSceneH = 0;
    g_bdSceneFmt = DXGI_FORMAT_UNKNOWN;
    g_bdDepthW = g_bdDepthH = 0;
    g_bdDepthFmt = DXGI_FORMAT_UNKNOWN;
    g_bdSceneFmtText[0] = '\0';
}

void releaseBackdropShaders() noexcept {
    releaseAtomic(g_psBdShow);
    releaseAtomic(g_psBdCapture);
    releaseAtomic(g_psBdProbe);
    g_bdShaderDevice.store(nullptr, std::memory_order_release);
}
const char* bdFormatName(DXGI_FORMAT f, char* fb, size_t cap) noexcept {
    switch (f) {
        case DXGI_FORMAT_R16G16B16A16_FLOAT: return "R16G16B16A16_FLOAT";
        case DXGI_FORMAT_R32G32B32A32_FLOAT: return "R32G32B32A32_FLOAT";
        case DXGI_FORMAT_R11G11B10_FLOAT: return "R11G11B10_FLOAT";
        case DXGI_FORMAT_R10G10B10A2_UNORM: return "R10G10B10A2_UNORM";
        case DXGI_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
        case DXGI_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return "R16G16B16A16_TYPELESS";
        default: break;
    }
    const char* n = formatName(f);
    if (n) return n;
    std::snprintf(fb, cap, "format %u", static_cast<unsigned>(f));
    return fb;
}

bool formatIsTypeless(DXGI_FORMAT f) noexcept {
    switch (f) {
        case DXGI_FORMAT_R32G32B32A32_TYPELESS: case DXGI_FORMAT_R32G32B32_TYPELESS: case DXGI_FORMAT_R16G16B16A16_TYPELESS: case DXGI_FORMAT_R32G32_TYPELESS:
        case DXGI_FORMAT_R32G8X24_TYPELESS: case DXGI_FORMAT_R10G10B10A2_TYPELESS: case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R16G16_TYPELESS:
        case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_R24G8_TYPELESS: case DXGI_FORMAT_R8G8_TYPELESS: case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_R8_TYPELESS:
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8X8_TYPELESS:
            return true;
        default: return false;
    }
}

bool bdRefuse(const char* why) noexcept {
    {
        std::lock_guard<std::mutex> lock(g_bdMutex);
        std::snprintf(g_bdRefusal, sizeof(g_bdRefusal), "%s", why);
    }
    Log::get().note("concourse backdrop: capture refused: %s", why);
    return false;
}

UINT bdMipCount() noexcept {
    UINT n = 1;
    for (UINT s = cc::kBackdropCubeSize; s > 1; s >>= 1) ++n;
    return n;
}

// The back textures' mip 0 goes into the front ones slice by slice (their mip counts differ, so not CopyResource), then the front mips are built. Unwritten texels are zero
// in every channel and written ones have alpha 1, so each mip is premultiplied by coverage (its alpha), in the colour cube and in the distance cube alike.
void backdropPublishFront(ID3D11DeviceContext* imm) noexcept {
    ID3D11Texture2D* back = g_bdBack.load(std::memory_order_acquire);
    ID3D11Texture2D* front = g_bdFront.load(std::memory_order_acquire);
    ID3D11Texture2D* dback = g_bdDistBack.load(std::memory_order_acquire);
    ID3D11Texture2D* dfront = g_bdDistFront.load(std::memory_order_acquire);
    ID3D11ShaderResourceView* srv = g_bdFrontSrv.load(std::memory_order_acquire);
    ID3D11ShaderResourceView* dsrv = g_bdDistFrontSrv.load(std::memory_order_acquire);
    if (!imm || !back || !front || !dback || !dfront || !srv || !dsrv) return;
    FlatComputeInternalScope internal;
    const UINT mips = bdMipCount();
    for (UINT slice = 0; slice < 6; ++slice) {
        imm->CopySubresourceRegion(front, D3D11CalcSubresource(0, slice, mips), 0, 0, 0, back, D3D11CalcSubresource(0, slice, 1), nullptr);
        imm->CopySubresourceRegion(dfront, D3D11CalcSubresource(0, slice, mips), 0, 0, 0, dback, D3D11CalcSubresource(0, slice, 1), nullptr);
    }
    imm->GenerateMips(srv);
    imm->GenerateMips(dsrv);
}

// A new visit (treat rose): the back cubes are emptied, the front ones follow, the origin is forgotten.
void backdropReset(ID3D11Device* device) noexcept {
    const bool had = g_bdCubeValid.load(std::memory_order_relaxed) || g_bdOriginSet.load(std::memory_order_relaxed);
    ID3D11UnorderedAccessView* u1 = g_bdBackUav.load(std::memory_order_acquire);
    ID3D11UnorderedAccessView* u2 = g_bdDistBackUav.load(std::memory_order_acquire);
    if (had && device && u1 && u2) {
        ID3D11DeviceContext* imm = nullptr;
        device->GetImmediateContext(&imm);
        if (imm) {
            {
                FlatComputeInternalScope internal;
                const FLOAT zero[4] = {};
                imm->ClearUnorderedAccessViewFloat(u1, zero);
                imm->ClearUnorderedAccessViewFloat(u2, zero);
            }
            backdropPublishFront(imm);
            imm->Release();
        }
    }
    g_bdCubeValid.store(false, std::memory_order_release);
    g_bdOriginSet.store(false, std::memory_order_release);
    const uint32_t n = g_bdCaptures.exchange(0, std::memory_order_acq_rel);
    if (had) Log::get().note("concourse backdrop: the cube is emptied and the origin forgotten (a new visit; it held %u capture%s)", n, n == 1 ? "" : "s");
}

// The capture shader (an exterior pane's pixel) and the display shader. Both declare the glass vertex shader's outputs in order (v0 face, v1 lighting normal, v2 lighting
// position = the world position relative to the camera, INFERRED); the capture shader also declares what lies before SV_POSITION in the game's own pixel shader (v3 tangent,
// v4 texcoord). Both read cb1[275].xyz as the camera in the game's world frame (the glass constant buffer, bound at b1 by the game and left alone) and an origin O from a
// 16-byte buffer (t10).
//   Capture: dir = normalize(v2); the scene's depth z gives the view depth 0.025 / z (reversed infinite Z), along the ray t = depth / |dot(cb1[279].xyz, dir)| (cb1[279] is the
//   view Z axis, INFERRED); the hit H = C + dir * t; rel = H - O. The colour goes to the colour cube's texel of rel (alpha 1) and |rel| to the distance cube's. A hit nearer than
//   5 m is not backdrop and is not written.
//   Display, per pixel: the constant-buffer rows and 1 buffer load; None: 6 cube samples (lods 0 1 2 3 5 7). Sphere: the same, with the direction moved. Depth: a march of the distance
//   cube, cc::kBackdropMarchSteps at lod 1 and cc::kBackdropMarchBisections at lod 0 at most, and the final 6. Mips fill the holes the splat leaves: the lookup composites lod 0 over the
//   premultiplied coarser lods front to back.
// Made once per device and kept across releaseBackdrop (the cube's release when the station is left behind): a concourse entry used to compile all three here, 150-220 ms of
// the render thread together with the bake shader. They are precompiled (kConcourse*Bytecode, texts in concourse_pane_shaders.h) and created from bytes; a different device
// makes them again. One log line each from shaderSwapCreatePs, on the first entry only.
bool makeBackdropShaders(ID3D11Device* device) noexcept {
    if (g_bdShaderDevice.load(std::memory_order_acquire) != device) {
        releaseBackdropShaders();
        g_bdShaderDevice.store(device, std::memory_order_release);
    }
    if (g_psBdShow.load(std::memory_order_acquire) && g_psBdCapture.load(std::memory_order_acquire) && g_psBdProbe.load(std::memory_order_acquire)) return true;
    ID3D11DeviceContext* imm = nullptr;
    device->GetImmediateContext(&imm);
    if (!imm) return false;
    ID3D11PixelShader* psShow = g_psBdShow.load(std::memory_order_acquire);
    ID3D11PixelShader* psCap = g_psBdCapture.load(std::memory_order_acquire);
    ID3D11PixelShader* psProbe = g_psBdProbe.load(std::memory_order_acquire);
    ID3D11PixelShader* madeShow = nullptr;
    ID3D11PixelShader* madeCap = nullptr;
    ID3D11PixelShader* madeProbe = nullptr;
    if (!psShow) psShow = madeShow = shaderSwapCreatePs(imm, kConcourseBackdropShowBytecode, sizeof(kConcourseBackdropShowBytecode), "concourse_backdrop_show", "concourse backdrop");
    if (!psCap) psCap = madeCap = shaderSwapCreatePs(imm, kConcourseBackdropCaptureBytecode, sizeof(kConcourseBackdropCaptureBytecode), "concourse_backdrop_capture", "concourse backdrop");
    if (!psProbe) psProbe = madeProbe = shaderSwapCreatePs(imm, kConcourseBackdropProbeBytecode, sizeof(kConcourseBackdropProbeBytecode), "concourse_backdrop_probe", "concourse backdrop");
    imm->Release();
    if (!psShow || !psCap || !psProbe) {
        if (madeShow) madeShow->Release();
        if (madeCap) madeCap->Release();
        if (madeProbe) madeProbe->Release();
        return bdRefuse(!psShow ? "the display pixel shader did not compile" : !psCap ? "the capture pixel shader did not compile" : "the probe pixel shader did not compile");
    }
    g_psBdShow.store(psShow, std::memory_order_release);
    g_psBdCapture.store(psCap, std::memory_order_release);
    g_psBdProbe.store(psProbe, std::memory_order_release);
    return true;
}

bool bdFormatOk(ID3D11Device* device, DXGI_FORMAT fmt, const char* name) noexcept {
    UINT sup = 0;
    const UINT need = D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_TEXTURECUBE | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE | D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW |
                      D3D11_FORMAT_SUPPORT_RENDER_TARGET | D3D11_FORMAT_SUPPORT_MIP_AUTOGEN;
    char why[160];
    if (FAILED(device->CheckFormatSupport(fmt, &sup)) || (sup & need) != need) {
        std::snprintf(why, sizeof(why), "%s cannot be a sampled, mip-generated cube and a typed UAV on this device", name);
        return bdRefuse(why);
    }
    D3D11_FEATURE_DATA_FORMAT_SUPPORT2 f2{};
    f2.InFormat = fmt;
    if (FAILED(device->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2, &f2, sizeof(f2))) || (f2.OutFormatSupport2 & D3D11_FORMAT_SUPPORT2_UAV_TYPED_STORE) == 0) {
        std::snprintf(why, sizeof(why), "%s has no typed UAV store on this device", name);
        return bdRefuse(why);
    }
    return true;
}

// The cube pairs, the views, the origin buffer, the sampler and both shaders; made once per device (releaseBackdrop undoes it). A refusal is one log line and leaves nothing
// half-made.
bool makeBackdropResources(ID3D11Device* device) noexcept {
    if (g_bdBack.load() && g_bdFront.load() && g_bdDistBack.load() && g_bdDistFront.load() && g_bdBackUav.load() && g_bdDistBackUav.load() && g_bdFrontSrv.load() &&
        g_bdDistFrontSrv.load() && g_bdOrigin.load() && g_bdOriginSrv.load() && g_psBdCapture.load() && g_psBdShow.load() && g_psBdProbe.load() && g_bdSampler.load())
        return true;
    releaseBackdrop();
    const DXGI_FORMAT cfmt = DXGI_FORMAT_R16G16B16A16_FLOAT, dfmt = DXGI_FORMAT_R16_FLOAT;
    if (!bdFormatOk(device, cfmt, "R16G16B16A16_FLOAT") || !bdFormatOk(device, dfmt, "R16_FLOAT")) return false;
    const UINT size = cc::kBackdropCubeSize;
    const UINT mips = bdMipCount();
    // No initial data: the back pair used to be uploaded as 12 subresources of zeros (a 33.5 MB vector, about 240 MB of copies, on every entry). They are cleared on the GPU below instead,
    // before the first read of either (backdropPublishFront reads the back pair; the front pair, never given data, is wholly written by it).
    ID3D11Texture2D* tex[4] = {};   // colour back, colour front, distance back, distance front
    HRESULT h[4] = {};
    for (int k = 0; k < 4; ++k) {
        const bool isBack = (k % 2) == 0;
        D3D11_TEXTURE2D_DESC td{};
        td.Width = td.Height = size;
        td.MipLevels = isBack ? 1u : mips;
        td.ArraySize = 6;
        td.Format = k < 2 ? cfmt : dfmt;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = isBack ? D3D11_BIND_UNORDERED_ACCESS : (D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
        td.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE | (isBack ? 0u : D3D11_RESOURCE_MISC_GENERATE_MIPS);
        h[k] = device->CreateTexture2D(&td, nullptr, &tex[k]);
    }
    ID3D11UnorderedAccessView* uav[2] = {};
    ID3D11ShaderResourceView* srv[2] = {};
    HRESULT hv[4] = {E_FAIL, E_FAIL, E_FAIL, E_FAIL};
    if (tex[0] && tex[1] && tex[2] && tex[3]) {
        for (int k = 0; k < 2; ++k) {
            D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
            ud.Format = k == 0 ? cfmt : dfmt;
            ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
            ud.Texture2DArray.MipSlice = 0;
            ud.Texture2DArray.FirstArraySlice = 0;
            ud.Texture2DArray.ArraySize = 6;
            hv[k] = device->CreateUnorderedAccessView(tex[k * 2], &ud, &uav[k]);
            D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = k == 0 ? cfmt : dfmt;
            sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
            sd.TextureCube.MostDetailedMip = 0;
            sd.TextureCube.MipLevels = static_cast<UINT>(-1);
            hv[2 + k] = device->CreateShaderResourceView(tex[k * 2 + 1], &sd, &srv[k]);
        }
    }
    // The origin: 16 bytes, a copy destination for the camera's row of the game's constant buffer.
    const float zero4[4] = {};
    D3D11_BUFFER_DESC ob{};
    ob.ByteWidth = 16;
    ob.Usage = D3D11_USAGE_DEFAULT;
    ob.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA oi{};
    oi.pSysMem = zero4;
    ID3D11Buffer* origin = nullptr;
    const HRESULT ho = device->CreateBuffer(&ob, &oi, &origin);
    ID3D11ShaderResourceView* originSrv = nullptr;
    HRESULT hos = E_FAIL;
    if (origin) {
        D3D11_SHADER_RESOURCE_VIEW_DESC od{};
        od.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        od.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        od.Buffer.FirstElement = 0;
        od.Buffer.NumElements = 1;
        hos = device->CreateShaderResourceView(origin, &od, &originSrv);
    }
    D3D11_SAMPLER_DESC smd{};
    smd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    smd.AddressU = smd.AddressV = smd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    smd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    smd.MaxLOD = D3D11_FLOAT32_MAX;
    ID3D11SamplerState* smp = nullptr;
    const HRESULT h5 = device->CreateSamplerState(&smd, &smp);
    bool good = smp && origin && originSrv && uav[0] && uav[1] && srv[0] && srv[1];
    for (int k = 0; k < 4; ++k) good = good && tex[k] != nullptr && SUCCEEDED(h[k]);
    if (!good) {
        char why[220];
        std::snprintf(why, sizeof(why), "the cubes could not be made (textures hr %08X %08X %08X %08X, views %08X %08X %08X %08X, origin %08X %08X, sampler %08X)", static_cast<unsigned>(h[0]),
                      static_cast<unsigned>(h[1]), static_cast<unsigned>(h[2]), static_cast<unsigned>(h[3]), static_cast<unsigned>(hv[0]), static_cast<unsigned>(hv[1]),
                      static_cast<unsigned>(hv[2]), static_cast<unsigned>(hv[3]), static_cast<unsigned>(ho), static_cast<unsigned>(hos), static_cast<unsigned>(h5));
        for (auto* p : tex) if (p) p->Release();
        for (auto* p : uav) if (p) p->Release();
        for (auto* p : srv) if (p) p->Release();
        if (origin) origin->Release();
        if (originSrv) originSrv->Release();
        if (smp) smp->Release();
        return bdRefuse(why);
    }
    g_bdBack.store(tex[0], std::memory_order_release);
    g_bdFront.store(tex[1], std::memory_order_release);
    g_bdDistBack.store(tex[2], std::memory_order_release);
    g_bdDistFront.store(tex[3], std::memory_order_release);
    g_bdBackUav.store(uav[0], std::memory_order_release);
    g_bdDistBackUav.store(uav[1], std::memory_order_release);
    g_bdFrontSrv.store(srv[0], std::memory_order_release);
    g_bdDistFrontSrv.store(srv[1], std::memory_order_release);
    g_bdOrigin.store(origin, std::memory_order_release);
    g_bdOriginSrv.store(originSrv, std::memory_order_release);
    g_bdSampler.store(smp, std::memory_order_release);
    if (!makeBackdropShaders(device)) {
        releaseBackdrop();
        return false;
    }
    ID3D11DeviceContext* imm = nullptr;
    device->GetImmediateContext(&imm);
    if (!imm) {
        releaseBackdrop();
        return bdRefuse("the cubes could not be cleared (no immediate context)");
    }
    {
        // Both back cubes (one mip, six faces: the UAVs cover all of each) start as zero everywhere (alpha 0: nothing captured), as the uploaded zeros were. Their first reader is
        // backdropPublishFront just below, in this call; no capture or bake pass can come earlier, as the cube is not published until this returns.
        FlatComputeInternalScope internal;
        const FLOAT zero[4] = {};
        imm->ClearUnorderedAccessViewFloat(uav[0], zero);
        imm->ClearUnorderedAccessViewFloat(uav[1], zero);
    }
    backdropPublishFront(imm);   // the front mips start as zero too: this copies the cleared back mip 0 into front mip 0 and builds the rest
    imm->Release();
    uint64_t texels = static_cast<uint64_t>(size) * size * 6u;
    const uint64_t mb = (texels * (8u + 2u) + texels * (8u + 2u) * 4u / 3u) >> 20;
    Log::get().note("concourse backdrop: cubes made: %u square x 6, colour R16G16B16A16_FLOAT and distance R16_FLOAT, %u mips on the front pair, about %u MB", static_cast<unsigned>(size),
                    static_cast<unsigned>(mips), static_cast<unsigned>(mb));
    return true;
}

// The depth view's texture can be read through a view of the same texture only in a typeless-compatible format.
DXGI_FORMAT bdDepthReadFormat(DXGI_FORMAT f) noexcept {
    switch (f) {
        case DXGI_FORMAT_R32G8X24_TYPELESS: case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        case DXGI_FORMAT_R24G8_TYPELESS: case DXGI_FORMAT_D24_UNORM_S8_UINT: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_D32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
        case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_D16_UNORM: return DXGI_FORMAT_R16_UNORM;
        default: return DXGI_FORMAT_UNKNOWN;
    }
}

// The first pane draw of an armed frame, before the game's glass draws (begin() runs before the real draw): the bound render target and the bound depth are copied to
// textures of the same size and format (a typeless colour format resolves through the view's own), which the capture shader reads, and, at the first capture of a visit, the
// camera's row of constant buffer 1 is copied into the origin buffer. Remade when the render target's or depth's size or format changes. On any refusal (each has its own
// line, once per capture) nothing is captured this frame: the capture passes need g_bdSceneOkFrame to be this frame, which is set only when all three copies were taken.
void backdropSceneCopy(ID3D11DeviceContext* ctx) noexcept {
    const uint32_t frame = g_frameNow.load(std::memory_order_relaxed);
    if (g_bdSceneFrame.exchange(frame, std::memory_order_acq_rel) == frame) return;   // once a frame
    const auto refuse = [&](const char* what, const char* why) noexcept {
        if (!g_bdSceneRefusedSaid.exchange(true, std::memory_order_relaxed)) Log::get().note("concourse backdrop: %s refused: %s", what, why);
    };
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11DepthStencilView* dsv = nullptr;
    ctx->OMGetRenderTargets(1, &rtv, &dsv);
    ID3D11Resource* res = nullptr;
    ID3D11Resource* dres = nullptr;
    D3D11_RENDER_TARGET_VIEW_DESC rd{};
    if (rtv) {
        rtv->GetDesc(&rd);
        rtv->GetResource(&res);
        rtv->Release();
    }
    if (dsv) {
        dsv->GetResource(&dres);
        dsv->Release();
    }
    ID3D11Texture2D* src = nullptr;
    ID3D11Texture2D* dsrc = nullptr;
    if (res) res->QueryInterface(IID_PPV_ARGS(&src));
    if (dres) dres->QueryInterface(IID_PPV_ARGS(&dsrc));
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    bool ok = false;
    do {
        if (!rtv) { refuse("scene copy", "no render target is bound at the first pane draw"); break; }
        if (!src) { refuse("scene copy", "the render target is not a 2D texture"); break; }
        D3D11_TEXTURE2D_DESC sd{};
        src->GetDesc(&sd);
        const DXGI_FORMAT fmt = rd.Format != DXGI_FORMAT_UNKNOWN ? rd.Format : sd.Format;
        if (formatIsTypeless(fmt) || sd.SampleDesc.Count > 1 || sd.ArraySize != 1 || rd.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D || rd.Texture2D.MipSlice != 0) {
            char why[160];
            std::snprintf(why, sizeof(why), "the render target is not a single, single-sample, whole 2D texture of a typed format (format %u, %ux%u, %u samples, array %u)", static_cast<unsigned>(fmt),
                          sd.Width, sd.Height, sd.SampleDesc.Count, sd.ArraySize);
            refuse("scene copy", why);
            break;
        }
        if (!dsrc) { refuse("depth copy", "no depth view is bound at the first pane draw"); break; }
        D3D11_TEXTURE2D_DESC dd{};
        dsrc->GetDesc(&dd);
        const DXGI_FORMAT dread = bdDepthReadFormat(dd.Format);
        if (dread == DXGI_FORMAT_UNKNOWN || dd.SampleDesc.Count > 1 || dd.ArraySize != 1 || dd.MipLevels != 1) {
            char why[160];
            std::snprintf(why, sizeof(why), "the depth texture cannot be read as one (format %u, %ux%u, %u samples, array %u, %u mips)", static_cast<unsigned>(dd.Format), dd.Width, dd.Height,
                          dd.SampleDesc.Count, dd.ArraySize, dd.MipLevels);
            refuse("depth copy", why);
            break;
        }
        if (!dev) { refuse("scene copy", "no device"); break; }
        std::lock_guard<std::mutex> lock(g_bdMutex);
        ID3D11Texture2D* scene = g_bdScene.load(std::memory_order_acquire);
        if (!scene || g_bdSceneW != sd.Width || g_bdSceneH != sd.Height || g_bdSceneFmt != fmt) {
            ID3D11Texture2D* made = nullptr;
            ID3D11ShaderResourceView* madeSrv = nullptr;
            D3D11_TEXTURE2D_DESC td{};
            td.Width = sd.Width;
            td.Height = sd.Height;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = fmt;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            if (SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &made)) && made) {
                D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
                vd.Format = fmt;
                vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                vd.Texture2D.MipLevels = 1;
                if (FAILED(dev->CreateShaderResourceView(made, &vd, &madeSrv)) || !madeSrv) {
                    made->Release();
                    made = nullptr;
                }
            }
            if (!made) { refuse("scene copy", "the scene texture could not be made"); break; }
            ID3D11Texture2D* oldTex = g_bdScene.exchange(made, std::memory_order_acq_rel);
            ID3D11ShaderResourceView* oldSrv = g_bdSceneSrv.exchange(madeSrv, std::memory_order_acq_rel);
            if (oldSrv) oldSrv->Release();
            if (oldTex) oldTex->Release();
            scene = made;
            g_bdSceneW = sd.Width;
            g_bdSceneH = sd.Height;
            g_bdSceneFmt = fmt;
            char fb[24] = {};
            std::snprintf(g_bdSceneFmtText, sizeof(g_bdSceneFmtText), "%s", bdFormatName(fmt, fb, sizeof(fb)));
        }
        ID3D11Texture2D* depth = g_bdDepth.load(std::memory_order_acquire);
        if (!depth || g_bdDepthW != dd.Width || g_bdDepthH != dd.Height || g_bdDepthFmt != dd.Format) {
            ID3D11Texture2D* made = nullptr;
            ID3D11ShaderResourceView* madeSrv = nullptr;
            D3D11_TEXTURE2D_DESC td{};
            td.Width = dd.Width;
            td.Height = dd.Height;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = dd.Format;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            if (SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &made)) && made) {
                D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
                vd.Format = dread;
                vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                vd.Texture2D.MipLevels = 1;
                if (FAILED(dev->CreateShaderResourceView(made, &vd, &madeSrv)) || !madeSrv) {
                    made->Release();
                    made = nullptr;
                }
            }
            if (!made) { refuse("depth copy", "the depth copy texture or its view could not be made"); break; }
            ID3D11Texture2D* oldTex = g_bdDepth.exchange(made, std::memory_order_acq_rel);
            ID3D11ShaderResourceView* oldSrv = g_bdDepthSrv.exchange(madeSrv, std::memory_order_acq_rel);
            if (oldSrv) oldSrv->Release();
            if (oldTex) oldTex->Release();
            depth = made;
            g_bdDepthW = dd.Width;
            g_bdDepthH = dd.Height;
            g_bdDepthFmt = dd.Format;
        }
        // The origin, once per visit: the camera's row of the glass's constant buffer 1, copied on the GPU (no read of the game's memory).
        bool originNow = false;
        if (!g_bdOriginSet.load(std::memory_order_acquire)) {
            ID3D11DeviceContext1* c1 = nullptr;
            ctx->QueryInterface(IID_PPV_ARGS(&c1));
            ID3D11Buffer* cb = nullptr;
            UINT first = 0, num = 0;
            if (c1) c1->PSGetConstantBuffers1(1, 1, &cb, &first, &num);
            char why[200] = {};
            ID3D11Buffer* origin = g_bdOrigin.load(std::memory_order_acquire);
            if (!c1) std::snprintf(why, sizeof(why), "the context has no ID3D11DeviceContext1, so the b1 window cannot be read");
            else if (!cb) std::snprintf(why, sizeof(why), "no constant buffer is bound at b1");
            else if (!origin) std::snprintf(why, sizeof(why), "the origin buffer is missing");
            else {
                D3D11_BUFFER_DESC bd{};
                cb->GetDesc(&bd);
                const UINT byteOff = (first + static_cast<UINT>(kBdCb1Row)) * 16u;
                if ((num != 0 && kBdCb1Row >= num) || byteOff + 16u > bd.ByteWidth)
                    std::snprintf(why, sizeof(why), "constant buffer 1 (%u bytes, window start %u, %u constants) has no row %u", bd.ByteWidth, first, num, static_cast<unsigned>(kBdCb1Row));
                else {
                    D3D11_BOX box{byteOff, 0, 0, byteOff + 16u, 1, 1};
                    FlatComputeInternalScope internal;
                    ctx->CopySubresourceRegion(origin, 0, 0, 0, 0, cb, 0, &box);
                    originNow = true;
                }
            }
            if (cb) cb->Release();
            if (c1) c1->Release();
            if (!originNow) { refuse("origin copy", why); break; }
        }
        {
            FlatComputeInternalScope internal;
            ctx->CopySubresourceRegion(scene, 0, 0, 0, 0, src, 0, nullptr);
            ctx->CopyResource(depth, dsrc);
        }
        if (originNow) {
            g_bdOriginSet.store(true, std::memory_order_release);
            g_bdOriginSetNow.store(true, std::memory_order_relaxed);
        }
        g_bdSceneSource.store(res, std::memory_order_release);   // identity only
        g_bdSceneOkFrame.store(frame, std::memory_order_release);
        g_bdSceneCopied.store(true, std::memory_order_release);
        ok = true;
    } while (false);
    (void)ok;
    if (dev) dev->Release();
    if (src) src->Release();
    if (dsrc) dsrc->Release();
    if (res) res->Release();
    if (dres) dres->Release();
}
bool formatHasStencil(DXGI_FORMAT f) noexcept {
    return f == DXGI_FORMAT_D24_UNORM_S8_UINT || f == DXGI_FORMAT_D32_FLOAT_S8X24_UINT || f == DXGI_FORMAT_R24G8_TYPELESS || f == DXGI_FORMAT_R32G8X24_TYPELESS;
}

bool markRefuse() noexcept {
    g_markRefused.fetch_add(1, std::memory_order_relaxed);
    return false;
}

// Runs at most once per (frame, depth view, viewport): one fullscreen triangle, no colour, at the depth of the beyond distance; where the scene's stored depth is at
// or below it (the scene is farther) stencil bit 0x40 is set. Everything touched is put back before returning, through the game's own entry points (past EDVR's
// thunks: the binding shadow, the shader substitution and the issue epoch do not move) or the context's own methods where no thunk exists (input layout, topology
// and rasteriser state are not hooked), and no draw thunk is re-entered (the draw goes through the real Draw).
bool ensureMarked(ID3D11DeviceContext* ctx, const ConcoursePaneCalls& calls) noexcept {
    if (!g_markReady.load(std::memory_order_acquire) || !calls.draw || !calls.setVs || !calls.setPs || !calls.setBlend || !calls.setDepth) return markRefuse();
    ID3D11VertexShader* vsMark = g_vsMark.load(std::memory_order_acquire);
    ID3D11RasterizerState* rsMark = g_rsMark.load(std::memory_order_acquire);
    ID3D11BlendState* bsMark = g_bsMark.load(std::memory_order_acquire);
    ID3D11DepthStencilState* dsMark = g_dsMark.load(std::memory_order_acquire);
    if (!vsMark || !rsMark || !bsMark || !dsMark) return markRefuse();
    ID3D11DepthStencilView* dsv = nullptr;
    ctx->OMGetRenderTargets(0, nullptr, &dsv);
    if (!dsv) return markRefuse();
    D3D11_VIEWPORT vp{};
    UINT nvp = 1;
    ctx->RSGetViewports(&nvp, &vp);
    const uint32_t frame = g_frameNow.load(std::memory_order_relaxed);
    if (g_markKey.valid && g_markKey.frame == frame && g_markKey.dsv == dsv && g_markKey.vp[0] == vp.TopLeftX && g_markKey.vp[1] == vp.TopLeftY &&
        g_markKey.vp[2] == vp.Width && g_markKey.vp[3] == vp.Height) {
        dsv->Release();
        return true;
    }
    D3D11_DEPTH_STENCIL_VIEW_DESC dd{};
    dsv->GetDesc(&dd);
    DXGI_FORMAT fmt = dd.Format;
    if (fmt == DXGI_FORMAT_UNKNOWN) {   // a view made without a description: the resource's own format
        ID3D11Resource* res = nullptr;
        dsv->GetResource(&res);
        ID3D11Texture2D* tex = nullptr;
        if (res) res->QueryInterface(IID_PPV_ARGS(&tex));
        if (tex) {
            D3D11_TEXTURE2D_DESC td{};
            tex->GetDesc(&td);
            fmt = td.Format;
            tex->Release();
        }
        if (res) res->Release();
    }
    if (!formatHasStencil(fmt) || (dd.Flags & D3D11_DSV_READ_ONLY_STENCIL) != 0 || nvp < 1 || !(vp.Width > 0.0f) || !(vp.Height > 0.0f)) {
        if (!g_markNoStencilLogged) {
            g_markNoStencilLogged = true;
            const char* fn = formatName(fmt);
            char fb[24] = {};
            if (!fn) std::snprintf(fb, sizeof(fb), "format %u", static_cast<unsigned>(fmt));
            Log::get().note("concourse pane: the mirror cannot work: the depth view (%s, flags %u, %u viewports, %.0fx%.0f) has no writable stencil; the game's glass is left alone",
                            fn ? fn : fb, static_cast<unsigned>(dd.Flags), static_cast<unsigned>(nvp), static_cast<double>(vp.Width), static_cast<double>(vp.Height));
        }
        dsv->Release();
        return markRefuse();
    }
    ID3D11GeometryShader* gs = nullptr;
    ID3D11HullShader* hs = nullptr;
    ID3D11DomainShader* dom = nullptr;
    ctx->GSGetShader(&gs, nullptr, nullptr);
    ctx->HSGetShader(&hs, nullptr, nullptr);
    ctx->DSGetShader(&dom, nullptr, nullptr);
    const bool staged = gs || hs || dom;
    if (gs) gs->Release();
    if (hs) hs->Release();
    if (dom) dom->Release();
    if (staged) {   // a geometry or tessellation stage would take the triangle: not run (counted)
        dsv->Release();
        return markRefuse();
    }
    // Save.
    ID3D11InputLayout* prevLayout = nullptr;
    D3D11_PRIMITIVE_TOPOLOGY prevTopo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ID3D11VertexShader* prevVs = nullptr;
    ID3D11PixelShader* prevPs = nullptr;
    ID3D11RasterizerState* prevRs = nullptr;
    ID3D11BlendState* prevBs = nullptr;
    FLOAT prevFactor[4] = {};
    UINT prevMask = 0;
    ID3D11DepthStencilState* prevDs = nullptr;
    UINT prevRef = 0;
    ctx->IAGetInputLayout(&prevLayout);
    ctx->IAGetPrimitiveTopology(&prevTopo);
    ctx->VSGetShader(&prevVs, nullptr, nullptr);
    ctx->PSGetShader(&prevPs, nullptr, nullptr);
    ctx->RSGetState(&prevRs);
    ctx->OMGetBlendState(&prevBs, prevFactor, &prevMask);
    ctx->OMGetDepthStencilState(&prevDs, &prevRef);
    // Draw.
    const FLOAT zeros[4] = {};
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    calls.setVs(ctx, vsMark, nullptr, 0);
    calls.setPs(ctx, nullptr, nullptr, 0);
    ctx->RSSetState(rsMark);
    calls.setBlend(ctx, bsMark, zeros, 0xFFFFFFFFu);
    calls.setDepth(ctx, dsMark, kMarkRef);
    calls.draw(ctx, 3, 0);
    // Restore.
    calls.setDepth(ctx, prevDs, prevRef);
    calls.setBlend(ctx, prevBs, prevFactor, prevMask);
    ctx->RSSetState(prevRs);
    calls.setPs(ctx, prevPs, nullptr, 0);
    calls.setVs(ctx, prevVs, nullptr, 0);
    ctx->IASetPrimitiveTopology(prevTopo);
    ctx->IASetInputLayout(prevLayout);
    if (prevDs) prevDs->Release();
    if (prevBs) prevBs->Release();
    if (prevRs) prevRs->Release();
    if (prevPs) prevPs->Release();
    if (prevVs) prevVs->Release();
    if (prevLayout) prevLayout->Release();
    g_markKey.valid = true;
    g_markKey.frame = frame;
    g_markKey.dsv = dsv;   // identity only
    g_markKey.vp[0] = vp.TopLeftX;
    g_markKey.vp[1] = vp.TopLeftY;
    g_markKey.vp[2] = vp.Width;
    g_markKey.vp[3] = vp.Height;
    dsv->Release();
    g_markPasses.fetch_add(1, std::memory_order_relaxed);
    if (!g_markLogged) {
        g_markLogged = true;
        const float beyond = g_vsMarkM;
        const char* fn = formatName(fmt);
        char fb[24] = {};
        if (!fn) std::snprintf(fb, sizeof(fb), "format %u", static_cast<unsigned>(fmt));
        Log::get().note("concourse pane: mark pass: dsv format %s, viewport %ux%u, beyond %.0f m (z %.6g)", fn ? fn : fb, static_cast<unsigned>(vp.Width),
                        static_cast<unsigned>(vp.Height), static_cast<double>(beyond), static_cast<double>(cc::markDepthForDistance(beyond)));
    }
    return true;
}

// ---- TEMPORARY (H62 instrument, Flight 54): the VR bake's draw diff ---------------------------------------------------------------------------------------------------------
// During the VR bake's aim frames every draw the scope sees that is not withheld and has a depth-stencil view bound is keyed (vertex shader, pixel shader, index count, start index,
// base vertex, and the draw's kind: D Draw, I DrawIndexed, N DrawInstanced, X DrawIndexedInstanced, Y DrawInstancedIndirect, Z DrawIndexedInstancedIndirect, A DrawAuto) and kept per
// frame. When a face's frames are over (the next face's first aim has begun, or the bake's end) the keys whose count is not the same on all of the face's frames are logged,
// largest index count first: the draws behind the band of the room's upper structure that Flight 53 saw on some frames and not others, with the camera unchanged.
// The draw thread only appends a key to its frame's buffer under g_diffMutex. The present thread's tick (vrDrawDiffTick) takes a finished face's group out under the mutex and
// sorts and logs it outside; the bake's end takes whatever is left (vrDrawDiffFlush). For an indirect draw the counts live on the GPU: its index count and start are 0 and its
// kind says so.
std::atomic<bool> g_vrDiffOn{false};
uint64_t vrAimEntry(uint32_t frame) noexcept;   // the aim ring's packed entry for the frame (0 when none), defined with the ring
constexpr uint32_t kDiffFrames = 3;             // the frames of a face compared (kVrBakeDwell)
constexpr uint32_t kDiffKeyCap = 16384;         // draws kept a frame; the rest are counted as dropped
constexpr uint32_t kDiffLines = 30;
constexpr size_t kDiffGroupCap = 64;            // groups waiting for the tick (a bake aims 15 times at most); the rest are dropped
struct DiffKey {
    uint64_t vs, ps;
    uint32_t idx, start;
    int32_t base;
    char kind;
};
bool diffLess(const DiffKey& a, const DiffKey& b) noexcept {
    if (a.vs != b.vs) return a.vs < b.vs;
    if (a.ps != b.ps) return a.ps < b.ps;
    if (a.idx != b.idx) return a.idx < b.idx;
    if (a.start != b.start) return a.start < b.start;
    if (a.base != b.base) return a.base < b.base;
    return a.kind < b.kind;
}
bool diffSame(const DiffKey& a, const DiffKey& b) noexcept { return a.vs == b.vs && a.ps == b.ps && a.idx == b.idx && a.start == b.start && a.base == b.base && a.kind == b.kind; }
struct DiffFrame {
    uint32_t frame = 0, draws = 0, dropped = 0;
    std::vector<DiffKey> keys;
};
struct DiffGroup {
    uint32_t face = 0, n = 0;
    DiffFrame fr[kDiffFrames];
};
std::mutex g_diffMutex;   // the draw threads and the tick both touch the groups
std::vector<DiffGroup> g_diffGroups;   // oldest first; the last is the one the draws append to. Under g_diffMutex.
uint32_t g_diffGroupsDropped = 0;      // draws of a group that found the list full (under g_diffMutex)

// Counts of a key in a sorted list.
uint32_t diffCount(const std::vector<DiffKey>& sorted, const DiffKey& k) noexcept {
    const auto r = std::equal_range(sorted.begin(), sorted.end(), k, diffLess);
    return static_cast<uint32_t>(r.second - r.first);
}

// Sorts and logs one finished group. No lock is held: the group is the caller's own.
void vrDrawDiffLog(DiffGroup& g) noexcept {
    std::vector<DiffKey> all;
    uint32_t distinct[kDiffFrames] = {}, dropped = 0;
    for (uint32_t f = 0; f < kDiffFrames; ++f) {
        std::sort(g.fr[f].keys.begin(), g.fr[f].keys.end(), diffLess);
        dropped += g.fr[f].dropped;
        for (size_t i = 0; i < g.fr[f].keys.size(); ++i) {
            if (i == 0 || !diffSame(g.fr[f].keys[i], g.fr[f].keys[i - 1])) {
                ++distinct[f];
                all.push_back(g.fr[f].keys[i]);
            }
        }
    }
    std::sort(all.begin(), all.end(), diffLess);
    all.erase(std::unique(all.begin(), all.end(), diffSame), all.end());
    struct Diff {
        DiffKey k;
        uint32_t c[kDiffFrames];
    };
    std::vector<Diff> diffs;
    for (const DiffKey& k : all) {
        Diff d{};
        d.k = k;
        bool differ = false;
        for (uint32_t f = 0; f < kDiffFrames; ++f) {
            d.c[f] = diffCount(g.fr[f].keys, k);
            if (d.c[f] != d.c[0]) differ = true;
        }
        if (differ) diffs.push_back(d);
    }
    std::stable_sort(diffs.begin(), diffs.end(), [](const Diff& a, const Diff& b) { return a.k.idx > b.k.idx; });
    Log::get().note("concourse bake: vr draw diff: face %u, frames %u %u %u: draws %u %u %u, distinct keys %u %u %u, keys that differ %u (dropped %u)", g.face, g.fr[0].frame, g.fr[1].frame,
                    g.fr[2].frame, g.fr[0].draws, g.fr[1].draws, g.fr[2].draws, distinct[0], distinct[1], distinct[2], static_cast<unsigned>(diffs.size()), dropped);
    for (size_t i = 0; i < diffs.size() && i < kDiffLines; ++i)
        Log::get().note("concourse bake: vr draw diff: face %u: vs %016llX ps %016llX indices %u start %u base vertex %d kind %c%s: counts %u %u %u", g.face,
                        static_cast<unsigned long long>(diffs[i].k.vs), static_cast<unsigned long long>(diffs[i].k.ps), diffs[i].k.idx, diffs[i].k.start, diffs[i].k.base,
                        diffs[i].k.kind, diffs[i].k.kind == 'Y' || diffs[i].k.kind == 'Z' || diffs[i].k.kind == 'A' ? " (indirect: the counts are on the GPU)" : "", diffs[i].c[0],
                        diffs[i].c[1], diffs[i].c[2]);
}
// The bake's end: every group still waiting is logged, then the list is empty. Present thread.
void vrDrawDiffFlush() noexcept {
    std::vector<DiffGroup> take;
    uint32_t lost = 0;
    {
        std::lock_guard<std::mutex> lock(g_diffMutex);
        take.swap(g_diffGroups);
        lost = g_diffGroupsDropped;
        g_diffGroupsDropped = 0;
    }
    for (DiffGroup& g : take) vrDrawDiffLog(g);
    if (lost) Log::get().note("concourse bake: vr draw diff: %u draw(s) found %u groups waiting and were not kept", lost, static_cast<unsigned>(kDiffGroupCap));
}
// Once a tick during the bake: one finished group, if there is one, taken out under the mutex and logged outside it. Finished: not the group the draws append to, and its last
// frame is two ticks or more behind (a late draw of an earlier frame has landed by then).
void vrDrawDiffTick(uint32_t frame) noexcept {
    DiffGroup take;
    bool have = false;
    {
        std::lock_guard<std::mutex> lock(g_diffMutex);
        if (g_diffGroups.size() >= 2) {
            DiffGroup& g = g_diffGroups.front();
            const uint32_t last = g.n > 0 ? g.fr[g.n - 1].frame : 0;
            if (frame - last < 0x80000000u && frame - last >= 2) {
                take = std::move(g);
                g_diffGroups.erase(g_diffGroups.begin());
                have = true;
            }
        }
    }
    if (have) vrDrawDiffLog(take);
}

// One draw the scope saw and did not withhold. Cheap out of the VR bake (one flag); in it, only an aim frame's draws are looked at. Appends, nothing else.
void vrDrawDiffNote(ID3D11DeviceContext* ctx, UINT indexCount, UINT startIndex, INT baseVertex, char kind) noexcept {
    const uint32_t frame = g_frameNow.load(std::memory_order_relaxed);
    const uint64_t e = vrAimEntry(frame);
    if ((e >> 63) == 0 || static_cast<uint32_t>((e >> 32) & 0x7FFFFFFFu) != (frame & 0x7FFFFFFFu) || (e & 0x100u) == 0) return;   // not an aim frame that writes
    const uint32_t face = static_cast<uint32_t>(e & 0xF), dwell = static_cast<uint32_t>((e >> 4) & 0xF);
    ID3D11DepthStencilView* dsv = nullptr;
    ctx->OMGetRenderTargets(0, nullptr, &dsv);
    if (!dsv) return;
    dsv->Release();
    const bool followed = ctx == g_immediate.load(std::memory_order_relaxed);
    DiffKey k;
    k.vs = followed ? bindingShaderHash(BindSlot::Vs) : vertexShaderHash(ctx);
    k.ps = followed ? bindingShaderHash(BindSlot::Ps) : pixelShaderHash(ctx);
    k.idx = indexCount;
    k.start = startIndex;
    k.base = baseVertex;
    k.kind = kind;
    std::lock_guard<std::mutex> lock(g_diffMutex);
    // A new group when the face changes, or an aim of the same face starts again (dwell 0 on a frame the group has not got).
    if (g_diffGroups.empty() || g_diffGroups.back().face != face || (dwell == 0 && g_diffGroups.back().n > 0 && g_diffGroups.back().fr[g_diffGroups.back().n - 1].frame != frame)) {
        if (g_diffGroups.size() >= kDiffGroupCap) {
            ++g_diffGroupsDropped;
            return;
        }
        g_diffGroups.emplace_back();
        g_diffGroups.back().face = face;
    }
    DiffGroup& g = g_diffGroups.back();
    uint32_t slot = kDiffFrames;
    for (uint32_t i = 0; i < g.n; ++i)
        if (g.fr[i].frame == frame) slot = i;
    if (slot == kDiffFrames) {
        if (g.n >= kDiffFrames) return;
        slot = g.n++;
        g.fr[slot].frame = frame;
    }
    DiffFrame& f = g.fr[slot];
    ++f.draws;
    if (f.keys.size() < kDiffKeyCap) f.keys.push_back(k);
    else ++f.dropped;
}

}  // namespace

// begin() changes no state. It runs the capture path, the withheld check, the family match, the one-shot material read and the share query, then only DECIDES the
// plan (a mirror draw or none, marked or not, opaque or fading) and opens the side test's first query when one is due.
// The members the constructor leaves alone when nothing is armed (concourse_pane.h), each to the value it was given as a default member initialiser.
void ConcoursePaneScope::initCold() noexcept {
    ctx_ = nullptr;
    calls_ = nullptr;
    planMirror_ = false;
    planMark_ = false;
    planOpaque_ = true;
    planTint_ = false;
    planSoft_ = false;
    planGlow_ = 0.0f;
    planSteady_ = false;
    mirrorBound_ = false;
    planBackdrop_ = false;
    planCapture_ = false;
    capDone_ = false;
    capBound_ = false;
    planProbe_ = false;
    probeDone_ = false;
    probeBound_ = false;
    probePrevSampler_ = nullptr;
    capPrevBs_ = nullptr;
    capPrevFactor0_ = capPrevFactor1_ = capPrevFactor2_ = capPrevFactor3_ = 0.0f;
    capPrevMask_ = 0;
    capPrevPs_ = nullptr;
    for (auto*& s : capPrevSrv_) s = nullptr;
    capPrevDs_ = nullptr;
    capPrevRef_ = 0;
    prevSampler_ = nullptr;
    planFactor0_ = planFactor1_ = planFactor2_ = planFactor3_ = 1.0f;
    keyN_ = 0;
    keyStart_ = 0;
    keyBase_ = 0;
    query_ = -1;
    capQuery_ = -1;
    testSlot_ = -1;
    prevBs_ = nullptr;
    prevFactor0_ = prevFactor1_ = prevFactor2_ = prevFactor3_ = 0.0f;
    prevMask_ = 0;
    prevPs_ = nullptr;
    for (auto*& s : prevSrv_) s = nullptr;
    prevDs_ = nullptr;
    prevStencilRef_ = 0;
    ctx1_ = nullptr;
    prevCb_ = nullptr;
    prevCbFirst_ = 0;
    prevCbNum_ = 0;
}

void ConcoursePaneScope::begin(ID3D11DeviceContext* ctx, UINT indexCount, UINT startIndex, INT baseVertex, char kind, const ConcoursePaneCalls& calls) noexcept {
    initCold();   // first: nothing reads a cold member before this
    beginBody(ctx, indexCount, startIndex, baseVertex, calls);
    // TEMPORARY (H62 instrument, Flight 53): the VR bake's draw diff sees every draw of the scene pass, after the decisions above (a draw withheld here is not drawn).
    if (ctx && !withheld_ && g_vrDiffOn.load(std::memory_order_relaxed)) vrDrawDiffNote(ctx, indexCount, startIndex, baseVertex, kind);
}

void ConcoursePaneScope::beginBody(ID3D11DeviceContext* ctx, UINT indexCount, UINT startIndex, INT baseVertex, const ConcoursePaneCalls& calls) noexcept {
    if (!ctx) return;
    if (g_capturing.load(std::memory_order_acquire)) {
        const int q = captureBegin(ctx, indexCount);
        if (q >= 0) {
            ctx_ = ctx;
            capQuery_ = q;
            flags_ = kFlagCapQuery;
        }
        return;
    }
    // Every draw of the frame passes here while the gate holds. On the immediate context (the one the binding shadow follows) the shadow answers with no COM
    // call: 0 is no pixel shader (a depth-only draw), and the draw has none to name. Any other context asks the context for its pixel shader.
    const bool followed = ctx == g_immediate.load(std::memory_order_relaxed);
    // The bake's frames: the people (an NPC, the commander's arms and suit) are not issued, so what is behind them is captured. By vertex shader, on any context.
    if (g_bakePeopleOff.load(std::memory_order_relaxed) && cc::bakePeopleVs(followed ? bindingShaderHash(BindSlot::Vs) : vertexShaderHash(ctx))) {
        withheld_ = true;
        g_bakePeopleLeft.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const uint64_t ps = followed ? bindingShaderHash(BindSlot::Ps) : pixelShaderHash(ctx);
    const bool cullOn = g_effectsNow.load(std::memory_order_relaxed);   // the outside is not issued: the plain phase, or the backdrop capture's lift
    const bool sealedOn = g_withholdSealedNow.load(std::memory_order_relaxed);
    const bool washOff = g_washOffNow.load(std::memory_order_relaxed);
    if (cullOn || sealedOn || washOff) {
        // TEMPORARY (hotkey.concourse_light_test): one of the room's lights left out, to name what blows out the lift bank.
        const uint64_t lightTest = g_lightTestPs.load(std::memory_order_relaxed);
        if (lightTest != 0 && cc::lightTestLeavesOut(lightTest, ps)) {
            withheld_ = true;
            g_lightTestHits.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        // The outside (smoke, trails, ship lasers, the planet) is not issued while the cull is on. By pixel shader on any context; by vertex shader too on the
        // immediate one, because a terrain patch has no pixel shader. Checked before `if (!ps) return`.
        int fam = cc::withheldPsIndex(ps);
        bool sealedToo = fam >= 0 && cc::kWithheldPs[fam].sealedToo;
        bool washToo = fam >= 0 && cc::kWithheldPs[fam].washToo;
        bool skySide = fam >= 0 && cc::kWithheldPs[fam].skySide;
        if (fam < 0 && followed) {
            const int v = cc::withheldVsIndex(bindingShaderHash(BindSlot::Vs));
            if (v >= 0) {
                fam = static_cast<int>(cc::kWithheldPsCount) + v;
                sealedToo = cc::kWithheldVs[v].sealedToo;
                washToo = cc::kWithheldVs[v].washToo;
                skySide = cc::kWithheldVs[v].skySide;
            }
        }
        // A room with the real sky in its windows (a carrier) draws the planet, its atmosphere and the sun with the cull on; a sealed hangar still does not.
        const bool skyKept = skySide && g_realSkyNow.load(std::memory_order_relaxed);
        if (fam >= 0 && ((cullOn && !skyKept) || (sealedOn && sealedToo) || (washOff && washToo))) {
            withheld_ = true;
            g_withheld.fetch_add(1, std::memory_order_relaxed);
            g_withheldFam[fam].fetch_add(1, std::memory_order_relaxed);
            return;
        }
        // The glows that may also be the room's: their far pixels are dropped by the mark (concourse_cull_core.h, farLimitedPs). Immediate context only, as the mark is.
        if (!cullOn && !washOff) return;   // sealed: nothing below is for a sealed hangar
        if (followed && g_withholdNow.load(std::memory_order_relaxed) && cc::farLimitedPs(ps)) {   // the plain phase only: the capture's lift draws the room's far glows
            if (ensureMarked(ctx, calls)) {
                UINT ref = 0;
                ctx->OMGetDepthStencilState(&prevDs_, &ref);
                ID3D11DepthStencilState* derived = prevDs_ ? derivedDepth(ctx, prevDs_, DsVariant::kFarOut) : nullptr;
                if (derived) {
                    ctx_ = ctx;
                    calls_ = &calls;
                    prevStencilRef_ = ref;
                    calls.setDepth(ctx, derived, kMarkRef);
                    flags_ |= kFlagDepth;
                    g_farLimited.fetch_add(1, std::memory_order_relaxed);
                } else if (prevDs_) {
                    prevDs_->Release();
                    prevDs_ = nullptr;
                }
            }
            return;
        }
    }
    if (!ps) return;
    const uint32_t listN = std::min<uint32_t>(g_listN.load(std::memory_order_acquire), static_cast<uint32_t>(cc::kPaneListCap));
    bool listed = false;
    for (uint32_t i = 0; i < listN; ++i)
        if (g_list[i].ps == ps) {
            listed = true;
            break;
        }
    const bool learned = !listed && learnedPs(ps);   // a pixel shader of the glass's layout, learned at creation: the built-in family's, with any index count
    if (!listed && !learned) return;   // the pixel shader first: the vertex shader's hash is read only for a draw that could be a pane
    const uint64_t vs = vertexShaderHash(ctx);
    const bool learnedGlass = learned && vs == cc::kPaneFamilyVs;
    if (!learnedGlass && cc::paneTripleIndex(g_list, listN, vs, ps, indexCount) < 0) return;
    ctx_ = ctx;
    calls_ = &calls;
    family_ = true;
    keyN_ = indexCount;
    keyStart_ = startIndex;
    keyBase_ = baseVertex;
    // PROTOTYPE (backdrop): in an armed frame the first pane draw copies the render target now, before the game's glass or any pass of ours has drawn into it, and
    // an exterior pane is planned a capture pass (its context's own: a deferred context records it).
    if (g_bdArmed.load(std::memory_order_acquire)) {
        backdropSceneCopy(ctx);
        if (sideOf(cc::PaneKey{indexCount, startIndex, baseVertex}) == cc::PaneSide::kExterior) planCapture_ = true;
    }
    const bool immediate = ctx->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE;
    if (immediate) {
        materialOnce(ctx, indexCount);
        if (g_shareOn.load(std::memory_order_acquire)) {
            const cc::PaneKey shareKey{indexCount, startIndex, baseVertex};
            query_ = openShareQuery(ctx, shareKey, sideOf(shareKey));   // the side is read before the shares' mutex is taken
            if (query_ >= 0) flags_ |= kFlagQuery;
        }
    }
    const uint32_t mode = g_mode.load(std::memory_order_relaxed);
    if (!immediate || !g_treatNow.load(std::memory_order_acquire) || mode == kModeOff) return;   // measured only, and the game's glass alone
    const bool tint = mode == kModeTint;
    const bool plain = g_withholdNow.load(std::memory_order_relaxed);   // the cull is on and the phase is the plain mirror phase
    bool mark = false;
    bool opaque = true;
    float opacity = 1.0f;
    if (plain) {
        mark = true;   // every pane mirrors where the scene behind it is far; the mark pass says where
        if (g_frameNow.load(std::memory_order_relaxed) - g_plainFrom.load(std::memory_order_relaxed) < kMarkSettleFrames &&
            sideOf(cc::PaneKey{indexCount, startIndex, baseVertex}) == cc::PaneSide::kExterior)
            mark = false;
    } else {
        // The cull is lifted (lifting, fading, clear): only a pane known to be exterior is still mirrored, over its whole area, and every such pane fades together.
        const cc::PaneKey key{indexCount, startIndex, baseVertex};
        if (sideOf(key) != cc::PaneSide::kExterior) return;
        opacity = std::min(1.0f, std::max(0.0f, g_bayOpacity.load(std::memory_order_relaxed)));
        if (opacity <= 0.001f) return;   // the outside glass is clear: the game's own glass, alone
        if (opacity < 0.999f) opaque = false;
    }
    if (tint) opaque = true;
    planMirror_ = true;
    planMark_ = mark;
    planOpaque_ = opaque;
    planTint_ = tint;
    planSoft_ = mode == kModeMirrorSoft;
    planSteady_ = mode == kModeSteady || mode == kModeBackdrop;
    planBackdrop_ = mode == kModeBackdrop;
    if (tint) {
        planGlow_ = 0.0f;
    } else if (planSteady_) {
        planGlow_ = 0.0f;   // the steady mode is one flat pass (beginMirrorSlow), with no glow pass after it
    } else {
        planGlow_ = glowOf(mode) * opacity;
    }
    if (tint) {
        const float* c = cc::kPaneColours[tintSlotOf(indexCount) % cc::kPaneColourCount];
        planFactor0_ = c[0] * cc::kPaneTintIntensity;
        planFactor1_ = c[1] * cc::kPaneTintIntensity;
        planFactor2_ = c[2] * cc::kPaneTintIntensity;
        planFactor3_ = 1.0f;
    } else if (!opaque) {
        planFactor0_ = planFactor1_ = planFactor2_ = planFactor3_ = opacity;
    }
    // The side test (plain phase, one frame in eight per pane): the first query wraps the game's draw and closes in beginMirror().
    if (plain && mark && g_testsOn.load(std::memory_order_acquire) && g_markReady.load(std::memory_order_acquire) &&
        cc::paneTestThisFrame(cc::PaneKey{indexCount, startIndex, baseVertex}, g_frameNow.load(std::memory_order_relaxed))) {
        testSlot_ = testOpen(ctx, cc::PaneKey{indexCount, startIndex, baseVertex});
        if (testSlot_ >= 0) {
            flags_ |= kFlagTestDrawn;
            if (planBackdrop_ && !g_bdArmed.load(std::memory_order_acquire)) planProbe_ = true;   // never on a capture frame
        }
    }
}

// Called right after the game's own draw. Closes the share query (so the bay's coverage is the game's draw alone) and the test's first query, then, if the plan
// says so and the mark pass could run, binds the mirror's state and returns true. Every false return leaves the context as the game had it.
bool ConcoursePaneScope::nextMirrorPassSlow() noexcept {
    if (capBound_) endCapturePass();   // the capture pass has drawn: its state goes back before anything else is bound
    if (probeBound_) endProbePass();   // so has the probe pass: its query ends and its state goes back (the constant buffer stays, for the display pass)
    if (planCapture_ && !capDone_) {
        capDone_ = true;
        closeGameQueries();   // the capture draw must not count in the game's draw's occlusion queries
        if (beginCapturePass()) return true;
    }
    if (planProbe_ && !probeDone_) {
        probeDone_ = true;
        closeGameQueries();   // the probe must not count in the game's draw's query
        if (beginProbePass()) return true;
    }
    if (!mirrorBound_) {
        if (!beginMirrorSlow()) {
            family_ = false;
            return false;
        }
        mirrorBound_ = true;
        return true;
    }
    // The mirror has drawn. Its count for the side test ends here, whether or not a glow follows; the glow is the last pass.
    family_ = false;
    if (testSlot_ >= 0 && (flags_ & kFlagTestMirror) != 0) {
        testCloseMirror(ctx_, testSlot_);
        flags_ &= ~kFlagTestMirror;
        testSlot_ = -1;
    }
    if (!(planGlow_ > 0.0f) || (flags_ & kFlagSrv) == 0 || (flags_ & kFlagBlend) == 0) return false;
    ID3D11BlendState* bs = g_bsGlow.load(std::memory_order_acquire);
    if (!bs) return false;
    ID3D11ShaderResourceView* mine[3] = {g_mirrorSrv[0].load(std::memory_order_acquire), g_mirrorSrv[1].load(std::memory_order_acquire),
                                         g_glowT10.load(std::memory_order_acquire)};
    if (!mine[0] || !mine[1] || !mine[2]) return false;
    calls_->setSrv(ctx_, kPaneSrvFirst, kPaneSrvCount, mine);
    const FLOAT factor[4] = {planGlow_, planGlow_, planGlow_, planGlow_};
    calls_->setBlend(ctx_, bs, factor, 0xFFFFFFFFu);
    return true;
}

// Constant buffer 2 of the pixel stage becomes `own`; the game's is kept for end() (once: a second call only changes the buffer).
void ConcoursePaneScope::bindPsCb(ID3D11Buffer* own) noexcept {
    if ((flags_ & kFlagCb) != 0) {
        if (ctx1_) ctx1_->PSSetConstantBuffers1(kPaneCbSlot, 1, &own, nullptr, nullptr);
        else ctx_->PSSetConstantBuffers(kPaneCbSlot, 1, &own);
        return;
    }
    ctx_->QueryInterface(IID_PPV_ARGS(&ctx1_));
    if (ctx1_) {
        // The *ConstantBuffers1 methods are not hooked, so this passes no thunk, and the saved offset window is put back exactly.
        ctx1_->PSGetConstantBuffers1(kPaneCbSlot, 1, &prevCb_, &prevCbFirst_, &prevCbNum_);
        ctx1_->PSSetConstantBuffers1(kPaneCbSlot, 1, &own, nullptr, nullptr);
    } else {
        ctx_->PSGetConstantBuffers(kPaneCbSlot, 1, &prevCb_);
        ctx_->PSSetConstantBuffers(kPaneCbSlot, 1, &own);
    }
    flags_ |= kFlagCb;
}

void ConcoursePaneScope::closeGameQueries() noexcept {
    if ((flags_ & kFlagQuery) != 0) {
        if (query_ >= 0) closeShareQuery(ctx_, query_);
        flags_ &= ~kFlagQuery;
    }
    if ((flags_ & kFlagTestDrawn) != 0) {
        testCloseDrawn(ctx_, testSlot_);
        flags_ &= ~kFlagTestDrawn;
    }
}

// PROTOTYPE (backdrop). The capture pass: the game's glass geometry again, no colour written, the capture pixel shader, the scene copy at t8, the game's depth test with no
// write, and the cube's back texture at u7. Refused (nothing bound, one line once) when the scene was not copied this frame, the bound render target is not the one copied, the
// render target count does not leave slot 7 free, or there is no depth view. The saved state comes back in endCapturePass (called by the next nextMirrorPass, or end()).
bool ConcoursePaneScope::beginCapturePass() noexcept {
    const auto refuse = [&](const char* why) noexcept {
        if (!g_bdPassRefusedSaid.exchange(true, std::memory_order_relaxed)) Log::get().note("concourse backdrop: a capture pass was refused: %s", why);
        return false;
    };
    ID3D11PixelShader* ps = g_psBdCapture.load(std::memory_order_acquire);
    ID3D11ShaderResourceView* sceneSrv = g_bdSceneSrv.load(std::memory_order_acquire);
    ID3D11ShaderResourceView* depthSrv = g_bdDepthSrv.load(std::memory_order_acquire);
    ID3D11ShaderResourceView* originSrv = g_bdOriginSrv.load(std::memory_order_acquire);
    ID3D11UnorderedAccessView* uavs[kBdUavCount] = {g_bdDistBackUav.load(std::memory_order_acquire), g_bdBackUav.load(std::memory_order_acquire)};   // u6 distance, u7 colour
    ID3D11BlendState* noColour = g_bsMark.load(std::memory_order_acquire);
    if (!ps || !sceneSrv || !depthSrv || !originSrv || !uavs[0] || !uavs[1] || !noColour || !calls_ || !calls_->setBlend || !calls_->setPs || !calls_->setSrv || !calls_->setDepth) return refuse("a shader, view or state is missing");
    if (g_bdSceneOkFrame.load(std::memory_order_acquire) != g_frameNow.load(std::memory_order_relaxed)) return refuse("the scene was not copied in this frame");
    ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    ctx_->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv);
    int bound = 0;   // the render target count the runtime sees: the highest bound slot, plus one
    for (int i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
        if (rtvs[i]) bound = i + 1;
    bool sameTarget = false;
    if (rtvs[0]) {
        ID3D11Resource* r = nullptr;
        rtvs[0]->GetResource(&r);
        sameTarget = r != nullptr && r == g_bdSceneSource.load(std::memory_order_acquire);
        if (r) r->Release();
    }
    for (auto*& v : rtvs)
        if (v) {
            v->Release();
            v = nullptr;
        }
    const bool haveDsv = dsv != nullptr;
    if (dsv) dsv->Release();
    if (!sameTarget) return refuse("the render target is not the one the scene was copied from");
    if (bound > static_cast<int>(kBdUavFirst)) return refuse("a render target is bound at slot 6 or above, so u6 and u7 are not free");
    if (!haveDsv) return refuse("no depth view is bound");
    UINT ref = 0;
    ctx_->OMGetDepthStencilState(&capPrevDs_, &ref);
    ID3D11DepthStencilState* derived = capPrevDs_ ? derivedDepth(ctx_, capPrevDs_, DsVariant::kProbe) : nullptr;
    if (!derived) {
        if (capPrevDs_) capPrevDs_->Release();
        capPrevDs_ = nullptr;
        return refuse("the depth state could not be derived");
    }
    capPrevRef_ = ref;
    {   // the factor is kept in four members (concourse_pane.h)
        FLOAT f[4] = {};
        ctx_->OMGetBlendState(&capPrevBs_, f, &capPrevMask_);
        capPrevFactor0_ = f[0];
        capPrevFactor1_ = f[1];
        capPrevFactor2_ = f[2];
        capPrevFactor3_ = f[3];
    }
    ctx_->PSGetShader(&capPrevPs_, nullptr, nullptr);
    ctx_->PSGetShaderResources(kBdSceneSlot, kBdSrvCount, capPrevSrv_);
    const FLOAT zeros[4] = {};
    calls_->setBlend(ctx_, noColour, zeros, 0xFFFFFFFFu);
    calls_->setPs(ctx_, ps, nullptr, 0);
    ID3D11ShaderResourceView* mine[kBdSrvCount] = {sceneSrv, depthSrv, originSrv};
    calls_->setSrv(ctx_, kBdSceneSlot, kBdSrvCount, mine);
    calls_->setDepth(ctx_, derived, ref);
    {
        // OMSetRenderTargetsAndUnorderedAccessViews is not hooked by EDVR (nothing in src hooks it): this goes to the context directly.
        FlatComputeInternalScope internal;
        ctx_->OMSetRenderTargetsAndUnorderedAccessViews(D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL, nullptr, nullptr, kBdUavFirst, kBdUavCount, uavs, nullptr);
    }
    capBound_ = true;
    flags_ |= kFlagCapPass;
    g_bdPaneDraws.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// PROTOTYPE (automatic capture). The probe pass: the game's glass geometry again, no colour, the probe pixel shader (the display shader's direction and look-up, `clip` where
// the composite covers at least a half), the cube bound as the display pass binds it, the game's depth test with the display's mark stencil and no depth write, and its own
// occlusion query, whose count is the pixels the cube cannot show. Only on a test frame, only with a cube, never on a capture frame.
bool ConcoursePaneScope::beginProbePass() noexcept {
    if (testSlot_ < 0 || !planMark_ || !calls_ || !calls_->setBlend || !calls_->setPs || !calls_->setSrv || !calls_->setDepth) return false;
    if (g_bdArmed.load(std::memory_order_acquire) || !g_bdCubeValid.load(std::memory_order_acquire)) return false;
    ID3D11Query* q = g_tests[testSlot_].qProbe;
    ID3D11PixelShader* ps = g_psBdProbe.load(std::memory_order_acquire);
    ID3D11ShaderResourceView* mine[kBdSrvCount] = {g_bdFrontSrv.load(std::memory_order_acquire), g_bdDistFrontSrv.load(std::memory_order_acquire), g_bdOriginSrv.load(std::memory_order_acquire)};
    ID3D11SamplerState* smp = g_bdSampler.load(std::memory_order_acquire);
    ID3D11BlendState* noColour = g_bsMark.load(std::memory_order_acquire);
    ID3D11Buffer* cb = (keyN_ == g_bayN.load(std::memory_order_relaxed) ? g_cbGlowBay : g_cbGlow).load(std::memory_order_acquire);
    if (!q || !ps || !mine[0] || !mine[1] || !mine[2] || !smp || !noColour || !cb) return false;
    if (!ensureMarked(ctx_, *calls_)) return false;   // runs and restores its own state
    UINT ref = 0;
    ctx_->OMGetDepthStencilState(&capPrevDs_, &ref);
    ID3D11DepthStencilState* derived = capPrevDs_ ? derivedDepth(ctx_, capPrevDs_, DsVariant::kMarkedProbe) : nullptr;
    if (!derived) {
        if (capPrevDs_) capPrevDs_->Release();
        capPrevDs_ = nullptr;
        return false;
    }
    capPrevRef_ = ref;
    {   // the factor is kept in four members (concourse_pane.h)
        FLOAT f[4] = {};
        ctx_->OMGetBlendState(&capPrevBs_, f, &capPrevMask_);
        capPrevFactor0_ = f[0];
        capPrevFactor1_ = f[1];
        capPrevFactor2_ = f[2];
        capPrevFactor3_ = f[3];
    }
    ctx_->PSGetShader(&capPrevPs_, nullptr, nullptr);
    ctx_->PSGetShaderResources(kBdSceneSlot, kBdSrvCount, capPrevSrv_);
    ctx_->PSGetSamplers(kBdSamplerSlot, 1, &probePrevSampler_);
    const FLOAT zeros[4] = {};
    calls_->setBlend(ctx_, noColour, zeros, 0xFFFFFFFFu);
    calls_->setPs(ctx_, ps, nullptr, 0);
    calls_->setSrv(ctx_, kBdSceneSlot, kBdSrvCount, mine);
    ctx_->PSSetSamplers(kBdSamplerSlot, 1, &smp);
    calls_->setDepth(ctx_, derived, kMarkRef);
    bindPsCb(cb);   // the game's constant buffer 2 is saved once (kFlagCb); the display pass changes the buffer again and end() puts the game's back
    {
        FlatComputeInternalScope internal;
        ctx_->Begin(q);
    }
    {
        std::lock_guard<std::mutex> lock(g_sideMutex);
        g_tests[testSlot_].probe = 1;
    }
    probeBound_ = true;
    flags_ |= kFlagProbePass;
    g_probeIssued.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void ConcoursePaneScope::endProbePass() noexcept {
    if (!probeBound_) return;
    probeBound_ = false;
    flags_ &= ~kFlagProbePass;
    if (testSlot_ >= 0) {
        {
            FlatComputeInternalScope internal;
            ctx_->End(g_tests[testSlot_].qProbe);
        }
        std::lock_guard<std::mutex> lock(g_sideMutex);
        g_tests[testSlot_].probe = 2;
    }
    ctx_->PSSetSamplers(kBdSamplerSlot, 1, &probePrevSampler_);
    if (probePrevSampler_) probePrevSampler_->Release();
    probePrevSampler_ = nullptr;
    calls_->setDepth(ctx_, capPrevDs_, capPrevRef_);
    if (capPrevDs_) capPrevDs_->Release();
    capPrevDs_ = nullptr;
    calls_->setSrv(ctx_, kBdSceneSlot, kBdSrvCount, capPrevSrv_);
    for (auto*& v : capPrevSrv_) {
        if (v) v->Release();
        v = nullptr;
    }
    calls_->setPs(ctx_, capPrevPs_, nullptr, 0);
    if (capPrevPs_) capPrevPs_->Release();
    capPrevPs_ = nullptr;
    {
        const FLOAT f[4] = {capPrevFactor0_, capPrevFactor1_, capPrevFactor2_, capPrevFactor3_};
        calls_->setBlend(ctx_, capPrevBs_, f, capPrevMask_);
    }
    if (capPrevBs_) capPrevBs_->Release();
    capPrevBs_ = nullptr;
}

void ConcoursePaneScope::endCapturePass() noexcept {
    if (!capBound_) return;
    capBound_ = false;
    flags_ &= ~kFlagCapPass;
    {
        FlatComputeInternalScope internal;
        ID3D11UnorderedAccessView* none[kBdUavCount] = {};
        ctx_->OMSetRenderTargetsAndUnorderedAccessViews(D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL, nullptr, nullptr, kBdUavFirst, kBdUavCount, none, nullptr);
    }
    calls_->setDepth(ctx_, capPrevDs_, capPrevRef_);
    if (capPrevDs_) capPrevDs_->Release();
    capPrevDs_ = nullptr;
    calls_->setSrv(ctx_, kBdSceneSlot, kBdSrvCount, capPrevSrv_);
    for (auto*& v : capPrevSrv_) {
        if (v) v->Release();
        v = nullptr;
    }
    calls_->setPs(ctx_, capPrevPs_, nullptr, 0);
    if (capPrevPs_) capPrevPs_->Release();
    capPrevPs_ = nullptr;
    {
        const FLOAT f[4] = {capPrevFactor0_, capPrevFactor1_, capPrevFactor2_, capPrevFactor3_};
        calls_->setBlend(ctx_, capPrevBs_, f, capPrevMask_);
    }
    if (capPrevBs_) capPrevBs_->Release();
    capPrevBs_ = nullptr;
}

bool ConcoursePaneScope::beginMirrorSlow() noexcept {
    closeGameQueries();
    const auto no = [&]() noexcept {
        if (testSlot_ >= 0) {
            testAbandon(testSlot_);
            testSlot_ = -1;
        }
        return false;
    };
    if (!planMirror_) return no();
    if (planMark_ && !ensureMarked(ctx_, *calls_)) return no();   // runs and restores its own state
    ID3D11BlendState* repl = planTint_ ? g_bsFactor.load(std::memory_order_acquire)
                                       : planOpaque_ ? g_bsMirror.load(std::memory_order_acquire) : g_bsFade.load(std::memory_order_acquire);
    if (!repl) return no();
    // The steady mode is one flat pass with EDVR's pixel shader and no textures. If that shader or the pane's buffer is missing, the pane is the plain mirror
    // instead (the textures), said once: a pane is never left transparent with the outside culled.
    ID3D11PixelShader* skyPs = nullptr;
    ID3D11Buffer* steadyCb = nullptr;
    ID3D11ShaderResourceView* cubeSrvs[kBdSrvCount] = {};   // mirror-backdrop: the colour cube, the distance cube and the origin, with the sampler (null: not this pass)
    ID3D11SamplerState* cubeSmp = nullptr;
    if (planSteady_ && !planTint_) {
        skyPs = g_psSky.load(std::memory_order_acquire);
        steadyCb = (keyN_ == g_bayN.load(std::memory_order_relaxed) ? g_cbGlowBay : g_cbGlow).load(std::memory_order_acquire);
        if (planBackdrop_) {
            ID3D11PixelShader* show = g_psBdShow.load(std::memory_order_acquire);
            ID3D11ShaderResourceView* srv = g_bdFrontSrv.load(std::memory_order_acquire);
            ID3D11ShaderResourceView* dsrv = g_bdDistFrontSrv.load(std::memory_order_acquire);
            ID3D11ShaderResourceView* osrv = g_bdOriginSrv.load(std::memory_order_acquire);
            ID3D11SamplerState* smp = g_bdSampler.load(std::memory_order_acquire);
            if (show && srv && dsrv && osrv && smp) {
                skyPs = show;
                cubeSrvs[0] = srv;
                cubeSrvs[1] = dsrv;
                cubeSrvs[2] = osrv;
                cubeSmp = smp;
            } else if (!g_bdFallbackSaid.exchange(true, std::memory_order_relaxed)) {
                Log::get().note("concourse backdrop: mirror-backdrop has no %s yet (the cube is made by the first capture, F6); the panes are drawn as mirror-steady",
                                !show ? "display shader" : !srv || !dsrv || !osrv ? "cube" : "sampler");
            }
        }
        if (!skyPs || !steadyCb) {
            skyPs = nullptr;
            steadyCb = nullptr;
            if (!g_steadyFallbackSaid.exchange(true, std::memory_order_relaxed))
                Log::get().note("concourse pane: mirror-steady has no pixel shader or buffer (see the line before); the panes are drawn as the plain mirror");
        }
    }
    const bool steadyNow = skyPs != nullptr;
    ID3D11ShaderResourceView* mine[3] = {};
    if (!planTint_ && !steadyNow) {
        for (int i = 0; i < 3; ++i) mine[i] = g_mirrorSrv[i].load(std::memory_order_acquire);
        if (!mine[0] || !mine[1] || !mine[2]) return no();
    }
    ID3D11PixelShader* white = planTint_ ? g_psWhite.load(std::memory_order_acquire) : nullptr;
    ID3D11Buffer* ownCb = planSoft_ && !planTint_ ? g_cbMirror.load(std::memory_order_acquire) : nullptr;
    // The depth state first: an opaque pane needs a derived one, and without it nothing is bound.
    ID3D11DepthStencilState* derived = nullptr;
    UINT stencilRef = 0;
    if (planOpaque_) {
        UINT ref = 0;
        ctx_->OMGetDepthStencilState(&prevDs_, &ref);
        derived = prevDs_ ? derivedDepth(ctx_, prevDs_, planMark_ ? (planBackdrop_ ? DsVariant::kMarkedOnce : DsVariant::kMarked) : DsVariant::kPlain) : nullptr;
        if (!derived) {
            if (prevDs_) prevDs_->Release();
            prevDs_ = nullptr;
            return no();
        }
        prevStencilRef_ = ref;
        stencilRef = planMark_ ? kMarkRef : ref;
    }
    // Bind.
    {
        FLOAT f[4] = {};
        ctx_->OMGetBlendState(&prevBs_, f, &prevMask_);
        prevFactor0_ = f[0];
        prevFactor1_ = f[1];
        prevFactor2_ = f[2];
        prevFactor3_ = f[3];
    }
    {
        const FLOAT f[4] = {planFactor0_, planFactor1_, planFactor2_, planFactor3_};
        calls_->setBlend(ctx_, repl, f, 0xFFFFFFFFu);
    }
    flags_ |= kFlagBlend;
    if (planTint_) {
        if (white) {
            ctx_->PSGetShader(&prevPs_, nullptr, nullptr);
            calls_->setPs(ctx_, white, nullptr, 0);
            flags_ |= kFlagPs;
        }
    } else if (steadyNow) {
        ctx_->PSGetShader(&prevPs_, nullptr, nullptr);
        calls_->setPs(ctx_, skyPs, nullptr, 0);
        flags_ |= kFlagPs;   // put back by end(), with the buffer below (kFlagCb); no textures are swapped (the backdrop: t8 and s7 below)
        if (cubeSrvs[0]) {
            ctx_->PSGetShaderResources(kPaneSrvFirst, kPaneSrvCount, prevSrv_);
            calls_->setSrv(ctx_, kBdSceneSlot, kBdSrvCount, cubeSrvs);
            flags_ |= kFlagSrv;
            ctx_->PSGetSamplers(kBdSamplerSlot, 1, &prevSampler_);
            ctx_->PSSetSamplers(kBdSamplerSlot, 1, &cubeSmp);
            flags_ |= kFlagSampler;
        }
    } else {
        ctx_->PSGetShaderResources(kPaneSrvFirst, kPaneSrvCount, prevSrv_);
        calls_->setSrv(ctx_, kPaneSrvFirst, kPaneSrvCount, mine);
        flags_ |= kFlagSrv;
    }
    if (derived) {
        calls_->setDepth(ctx_, derived, stencilRef);
        flags_ |= kFlagDepth;
    }
    if (steadyNow) bindPsCb(steadyCb);
    else if (ownCb) bindPsCb(ownCb);
    if (testSlot_ >= 0) {
        testOpenMirror(ctx_, testSlot_);
        flags_ |= kFlagTestMirror;
    }
    g_treated.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void ConcoursePaneScope::end() noexcept {
    if ((flags_ & kFlagProbePass) != 0) endProbePass();   // before the test slot is touched below: it closes the probe's query
    if ((flags_ & kFlagCapPass) != 0) endCapturePass();   // a capture pass the site never asked past (the loop always does; this is the way out of any other exit)
    if ((flags_ & kFlagSampler) != 0) {
        ctx_->PSSetSamplers(kBdSamplerSlot, 1, &prevSampler_);
        if (prevSampler_) prevSampler_->Release();
        prevSampler_ = nullptr;
    }
    if (testSlot_ >= 0) {
        if ((flags_ & kFlagTestMirror) != 0) {
            testCloseMirror(ctx_, testSlot_);
        } else {
            if ((flags_ & kFlagTestDrawn) != 0) {   // the game's draw was never followed by beginMirror(): close its query and drop the test
                FlatComputeInternalScope internal;
                ctx_->End(g_tests[testSlot_].qDrawn);
            }
            testAbandon(testSlot_);
        }
        testSlot_ = -1;
        flags_ &= ~(kFlagTestMirror | kFlagTestDrawn);
    }
    if ((flags_ & kFlagCb) != 0) {
        if (ctx1_) {
            if (prevCb_) ctx1_->PSSetConstantBuffers1(kPaneCbSlot, 1, &prevCb_, &prevCbFirst_, &prevCbNum_);
            else ctx1_->PSSetConstantBuffers1(kPaneCbSlot, 1, &prevCb_, nullptr, nullptr);
            ctx1_->Release();
            ctx1_ = nullptr;
        } else {
            ctx_->PSSetConstantBuffers(kPaneCbSlot, 1, &prevCb_);
        }
        if (prevCb_) prevCb_->Release();
        prevCb_ = nullptr;
    }
    if ((flags_ & kFlagDepth) != 0) {
        calls_->setDepth(ctx_, prevDs_, prevStencilRef_);
        if (prevDs_) prevDs_->Release();
        prevDs_ = nullptr;
    }
    if ((flags_ & kFlagSrv) != 0) {
        calls_->setSrv(ctx_, kPaneSrvFirst, kPaneSrvCount, prevSrv_);
        for (auto*& v : prevSrv_) {
            if (v) v->Release();
            v = nullptr;
        }
    }
    if ((flags_ & kFlagPs) != 0) {
        calls_->setPs(ctx_, prevPs_, nullptr, 0);
        if (prevPs_) prevPs_->Release();
        prevPs_ = nullptr;
    }
    if ((flags_ & kFlagBlend) != 0) {
        const FLOAT f[4] = {prevFactor0_, prevFactor1_, prevFactor2_, prevFactor3_};
        calls_->setBlend(ctx_, prevBs_, f, prevMask_);
        if (prevBs_) prevBs_->Release();
        prevBs_ = nullptr;
    }
    if ((flags_ & kFlagQuery) != 0 && query_ >= 0) closeShareQuery(ctx_, query_);
    if ((flags_ & kFlagCapQuery) != 0 && capQuery_ >= 0) {
        FlatComputeInternalScope internal;
        ctx_->End(g_pool[capQuery_]);
    }
    flags_ = 0;
}

namespace {

// ---- The capture's report ---------------------------------------------------------------------------------------------------------------------
struct Group {
    uint64_t vs = 0, ps = 0;
    uint32_t n = 0;
    uint32_t vpW = 0, vpH = 0;
    uint32_t src = 0, dst = 0;
    int depthWrite = 0;
    uint32_t stencil = 0;
    uint32_t draws = 0;
    uint64_t samples = 0;
};

// The toast names the mode in plain words (F4 and F6 only; the ini's own setting at start-up says nothing).
void modeToast(uint32_t m) noexcept {
    switch (m) {
        case kModeBackdrop: queueToast(g_bdCubeValid.load(std::memory_order_relaxed) ? "Windows: captured view" : "Windows: captured view, none yet"); break;
        case kModeSteady: queueToast("Windows: flat light"); break;
        case kModeLit: case kModeBright: case kModeBrightest: queueToast("Windows: bright mirror"); break;
        case kModeMirror: case kModeMirrorSoft: queueToast("Windows: mirror"); break;
        case kModeTint: queueToast("Windows: tint"); break;
        default: queueToast("Windows: plain glass"); break;
    }
}

void setMode(uint32_t m, const char* why, bool toast = false) noexcept {
    const uint32_t before = g_mode.exchange(m, std::memory_order_relaxed);
    if (before != m) Log::get().note("concourse pane: mode %s (%s)", modeText(m), why);
    if (toast && m == kModeBackdrop) g_bdAutoOn.store(true, std::memory_order_relaxed);   // a hotkey chose the backdrop: automatic capture is on (the ini's own setting does not turn it on)
    if (toast && before != m) modeToast(m);
}

// The ranking only: the capture never changes the list or the mode.
void reportCapture() noexcept {
    static Group groups[kPool];
    size_t gn = 0;
    uint32_t arrived = 0;
    for (size_t i = 0; i < g_capN; ++i) {
        const CapDraw& d = g_cap[i];
        if (!d.arrived) continue;
        ++arrived;
        size_t k = 0;
        while (k < gn && !(groups[k].vs == d.vs && groups[k].ps == d.ps && groups[k].n == d.n)) ++k;
        if (k == gn) {
            groups[gn] = Group{};
            groups[gn].vs = d.vs;
            groups[gn].ps = d.ps;
            groups[gn].n = d.n;
            groups[gn].vpW = d.vpW;
            groups[gn].vpH = d.vpH;
            groups[gn].src = d.src;
            groups[gn].dst = d.dst;
            groups[gn].depthWrite = d.depthWrite;
            groups[gn].stencil = d.stencil;
            ++gn;
        }
        groups[k].samples += d.samples;
        ++groups[k].draws;
    }
    Log::get().note("concourse pane capture: %u blended draws wrapped, %u results arrived, %u pending, %llu qualifying draws over the pool cap of %u; %u triples",
                    static_cast<unsigned>(g_capN), arrived, static_cast<unsigned>(g_capN) - arrived, static_cast<unsigned long long>(g_capDropped),
                    static_cast<unsigned>(kPool), static_cast<unsigned>(gn));
    std::sort(groups, groups + gn, [](const Group& a, const Group& b) { return a.samples > b.samples; });
    const size_t top = gn < 24 ? gn : 24;
    for (size_t i = 0; i < top; ++i) {
        const Group& g = groups[i];
        const double pixels = static_cast<double>(g.vpW) * static_cast<double>(g.vpH);
        Log::get().note("concourse pane capture: #%d samples %llu (%.1f%% of %ux%u) vs=%016llX ps=%016llX n=%u draws=%u blend=%u,%u depth_write=%d stencil=%u",
                        static_cast<int>(i + 1), static_cast<unsigned long long>(g.samples), pixels > 0.0 ? 100.0 * static_cast<double>(g.samples) / pixels : 0.0,
                        g.vpW, g.vpH, static_cast<unsigned long long>(g.vs), static_cast<unsigned long long>(g.ps), g.n, g.draws, g.src, g.dst, g.depthWrite,
                        g.stencil);
    }
    if (gn == 0) Log::get().note("concourse pane capture: no blended draw covered a pixel (or no result arrived)");
}

void pollResults(ID3D11Device* device) noexcept {
    ID3D11DeviceContext* imm = nullptr;
    device->GetImmediateContext(&imm);
    if (!imm) return;
    FlatComputeInternalScope internal;
    for (size_t i = 0; i < g_capN; ++i) {
        CapDraw& d = g_cap[i];
        if (d.arrived) continue;
        UINT64 samples = 0;
        if (imm->GetData(g_pool[i], &samples, sizeof(samples), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK) {
            d.arrived = true;
            d.samples = samples;
        }
    }
    imm->Release();
}

bool allArrived() noexcept {
    for (size_t i = 0; i < g_capN; ++i)
        if (!g_cap[i].arrived) return false;
    return true;
}

// ---- The tick's pieces ------------------------------------------------------------------------------------------------------------------------
void bindKey(Hotkey*& key, std::string& applied, const std::string& text, const char* dotted, const char* what) {
    if (!key) key = new Hotkey();
    if (g_keysBound && text == applied) return;
    const bool first = !g_keysBound;
    applied = text;
    key->setBinding(text.c_str());
    if (key->bound()) Log::get().note("concourse pane: %s bound (%s = \"%s\")", what, dotted, text.c_str());
    else if (!text.empty()) Log::get().note("concourse pane: %s is set (\"%s\") but bound nothing, so it cannot be used this session", dotted, text.c_str());
    else if (!first) Log::get().note("concourse pane: %s cleared", dotted);
}

void pollConfig(uint64_t nowMs) {
    if (nowMs < g_nextPollMs) return;
    g_nextPollMs = nowMs + 1000;
    g_hashEpoch.fetch_add(1, std::memory_order_relaxed);
    bindKey(g_keyCapture, g_keyCaptureText, Config::get().getString("hotkey.concourse_pane_capture", ""), "hotkey.concourse_pane_capture", "capture key");
    bindKey(g_keyMode, g_keyModeText, Config::get().getString("hotkey.concourse_pane_mode", ""), "hotkey.concourse_pane_mode", "mode key");
    bindKey(g_keyParallax, g_keyParallaxText, Config::get().getString("hotkey.concourse_backdrop_parallax", ""), "hotkey.concourse_backdrop_parallax", "backdrop distance key");
    bindKey(g_keyLightTest, g_keyLightTestText, Config::get().getString("hotkey.concourse_light_test", ""), "hotkey.concourse_light_test", "light test key");
    g_keysBound = true;
    const std::string modeT = Config::get().getString("diag.concourse_pane_mode", "mirror-steady");
    if (!g_modeTextSeen || modeT != g_modeAppliedText) {
        g_modeTextSeen = true;
        g_modeAppliedText = modeT;
        uint32_t m = kModeSteady;
        if (parseMode(modeT, &m)) setMode(m, "diag.concourse_pane_mode");
        else Log::get().note("concourse pane: diag.concourse_pane_mode = \"%s\" is not mirror-backdrop, mirror-steady, mirror-brightest, mirror-lit, mirror-bright, mirror-soft, mirror, tint or off; the mode is unchanged", modeT.c_str());
    }
    const std::string glowT = Config::get().getString("diag.concourse_glow", "");
    if (!g_glowTextSeen || glowT != g_glowAppliedText) {
        g_glowTextSeen = true;
        g_glowAppliedText = glowT;
        float lv[4] = {};
        if (glowT.empty()) {
            g_glowSky.store(cc::kGlowSkyDefault, std::memory_order_relaxed);   // unset: the defaults
            g_glowAngled.store(cc::kGlowAngledDefault, std::memory_order_relaxed);
            g_glowOther.store(cc::kGlowOtherDefault, std::memory_order_relaxed);
            g_glowBay.store(cc::kGlowBayDefault, std::memory_order_relaxed);
        } else if (cc::parseGlowLevels(glowT.c_str(), lv)) {
            g_glowSky.store(lv[0], std::memory_order_relaxed);
            g_glowAngled.store(lv[1], std::memory_order_relaxed);
            g_glowOther.store(lv[2], std::memory_order_relaxed);
            g_glowBay.store(lv[3], std::memory_order_relaxed);
        } else {
            Log::get().note("concourse pane: diag.concourse_glow = \"%s\" is not four numbers from 0 to %.0f (sky,angled,other,bay); the levels stay sky %g angled %g other %g bay %g",
                            glowT.c_str(), static_cast<double>(cc::kGlowMax), static_cast<double>(g_glowSky.load(std::memory_order_relaxed)),
                            static_cast<double>(g_glowAngled.load(std::memory_order_relaxed)), static_cast<double>(g_glowOther.load(std::memory_order_relaxed)),
                            static_cast<double>(g_glowBay.load(std::memory_order_relaxed)));
        }
    }
    const std::string beyondT = Config::get().getString("diag.concourse_mirror_beyond", "");
    if (!g_beyondTextSeen || beyondT != g_beyondAppliedText) {
        const bool first = !g_beyondTextSeen;
        g_beyondTextSeen = true;
        g_beyondAppliedText = beyondT;
        if (beyondT.empty()) {
            g_beyondM.store(cc::kMirrorBeyondDefaultM, std::memory_order_relaxed);   // unset: the default
        } else {
            char* end = nullptr;
            const double v = std::strtod(beyondT.c_str(), &end);
            while (end && (*end == ' ' || *end == '\t')) ++end;
            if (end && end != beyondT.c_str() && *end == '\0' && v >= static_cast<double>(cc::kMirrorBeyondMinM) && v <= static_cast<double>(cc::kMirrorBeyondMaxM)) {
                g_beyondM.store(static_cast<float>(v), std::memory_order_relaxed);
                if (!first || v != static_cast<double>(cc::kMirrorBeyondDefaultM))
                    Log::get().note("concourse pane: diag.concourse_mirror_beyond is now %.1f m", v);
            } else {
                Log::get().note("concourse pane: diag.concourse_mirror_beyond = \"%s\" is not a number from %.0f to %.0f; the value stays %.1f m", beyondT.c_str(),
                                static_cast<double>(cc::kMirrorBeyondMinM), static_cast<double>(cc::kMirrorBeyondMaxM),
                                static_cast<double>(g_beyondM.load(std::memory_order_relaxed)));
            }
        }
    }
    const std::string radiusT = Config::get().getString("diag.concourse_backdrop_radius", "");
    const float radiusFed = g_radiusFed.load(std::memory_order_relaxed);
    static float s_radiusFedApplied = cc::kBackdropRadiusDefaultM;
    if (!g_radiusTextSeen || radiusT != g_radiusAppliedText || radiusFed != s_radiusFedApplied) {
        const bool first = !g_radiusTextSeen;
        g_radiusTextSeen = true;
        g_radiusAppliedText = radiusT;
        s_radiusFedApplied = radiusFed;
        float want = radiusFed;
        bool bad = false;
        if (!radiusT.empty()) {
            char* end = nullptr;
            const double v = std::strtod(radiusT.c_str(), &end);
            while (end && (*end == ' ' || *end == '\t')) ++end;
            if (end && end != radiusT.c_str() && *end == '\0' && v >= static_cast<double>(cc::kBackdropRadiusMinM) && v <= static_cast<double>(cc::kBackdropRadiusMaxM)) want = static_cast<float>(v);
            else bad = true;
        }
        if (bad) {
            Log::get().note("concourse backdrop: diag.concourse_backdrop_radius = \"%s\" is not a number from %.0f to %.0f; the radius stays %.0f m", radiusT.c_str(),
                            static_cast<double>(cc::kBackdropRadiusMinM), static_cast<double>(cc::kBackdropRadiusMaxM), static_cast<double>(g_bdRadius.load(std::memory_order_relaxed)));
        } else {
            g_bdRadius.store(want, std::memory_order_relaxed);
            if (!first || want != cc::kBackdropRadiusDefaultM)
                Log::get().note("concourse backdrop: distance mode %s, radius %.0f m", cc::parallaxName(g_bdParallax.load(std::memory_order_relaxed)), static_cast<double>(want));
        }
    }
    const std::string glassT = Config::get().getString("diag.concourse_glass", "");
    if (!g_glassTextSeen || glassT != g_glassAppliedText) {
        const bool first = !g_glassTextSeen;
        g_glassTextSeen = true;
        g_glassAppliedText = glassT;
        float v[6] = {};
        if (glassT.empty()) {
            for (int k = 0; k < 6; ++k) g_glass[k].store(cc::kGlassDefault[k], std::memory_order_relaxed);   // unset: the defaults
            if (!first) Log::get().note("concourse backdrop: glass back to the defaults: tint %g %g %g haze %g blur %g fresnel %g", static_cast<double>(cc::kGlassDefault[0]),
                                        static_cast<double>(cc::kGlassDefault[1]), static_cast<double>(cc::kGlassDefault[2]), static_cast<double>(cc::kGlassDefault[3]),
                                        static_cast<double>(cc::kGlassDefault[4]), static_cast<double>(cc::kGlassDefault[5]));
        } else if (cc::parseGlass(glassT.c_str(), v)) {
            for (int k = 0; k < 6; ++k) g_glass[k].store(v[k], std::memory_order_relaxed);
            Log::get().note("concourse backdrop: glass tint %g %g %g haze %g blur %g fresnel %g", static_cast<double>(v[0]), static_cast<double>(v[1]), static_cast<double>(v[2]),
                            static_cast<double>(v[3]), static_cast<double>(v[4]), static_cast<double>(v[5]));
        } else {
            Log::get().note("concourse backdrop: diag.concourse_glass = \"%s\" is not six numbers (r,g,b 0 to 2, haze 0 to 1, blur 0 to 8, fresnel 0 to 2); the glass stays tint %g %g %g haze %g blur %g fresnel %g",
                            glassT.c_str(), static_cast<double>(g_glass[0].load()), static_cast<double>(g_glass[1].load()), static_cast<double>(g_glass[2].load()),
                            static_cast<double>(g_glass[3].load()), static_cast<double>(g_glass[4].load()), static_cast<double>(g_glass[5].load()));
        }
    }
    const std::string panesT = Config::get().getString("diag.concourse_panes", "");
    if (!g_panesTextSeen || panesT != g_panesAppliedText) {
        const bool first = !g_panesTextSeen;
        g_panesTextSeen = true;
        g_panesAppliedText = panesT;
        if (!(first && panesT.empty())) {
            cc::PaneTriple parsed[cc::kPaneListCap];
            size_t bad = 0;
            const size_t n = cc::parsePaneList(panesT.c_str(), parsed, cc::kPaneListCap - 1, &bad);
            g_listN.store(1, std::memory_order_release);   // the built-in family alone while the added entries are written
            size_t total = 1;
            for (size_t i = 0; i < n; ++i)
                if (!cc::paneTripleHas(g_list, total, parsed[i])) g_list[total++] = parsed[i];
            g_listN.store(static_cast<uint32_t>(total), std::memory_order_release);
            Log::get().note("concourse pane: diag.concourse_panes: %u pane(s) added to the built-in family, %u entr%s refused (an entry is VS/PS or VS/PS/N: two 16-digit hex hashes, and an index count)",
                            static_cast<unsigned>(total - 1), static_cast<unsigned>(bad), bad == 1 ? "y" : "ies");
            for (size_t i = 1; i < total; ++i)
                Log::get().note("concourse pane: added vs=%016llX ps=%016llX n=%u (0 is any)", static_cast<unsigned long long>(g_list[i].vs),
                                static_cast<unsigned long long>(g_list[i].ps), g_list[i].n);
        }
    }
}

void updateArmed() noexcept {
    const uint32_t mode = g_mode.load(std::memory_order_relaxed);
    const bool treat = g_feedTreat.load(std::memory_order_relaxed) && mode != kModeOff && g_statesReady.load(std::memory_order_acquire);
    const bool withhold = g_feedWithhold.load(std::memory_order_relaxed);   // whatever the mode: F4 at off shows what the cull leaves
    const bool effects = withhold || g_feedEffects.load(std::memory_order_relaxed);   // the outside is not issued: the plain phase, or the backdrop capture's lift
    const bool shares = g_feedMeasure.load(std::memory_order_relaxed) && g_shareOn.load(std::memory_order_relaxed);
    g_treatNow.store(treat, std::memory_order_release);
    const bool sealed = g_feedSealed.load(std::memory_order_relaxed);
    g_withholdNow.store(withhold, std::memory_order_release);
    g_effectsNow.store(effects, std::memory_order_release);
    g_washOffNow.store(treat && mode == kModeBackdrop && g_bakeBaked.load(std::memory_order_acquire), std::memory_order_release);
    g_withholdSealedNow.store(sealed, std::memory_order_release);
    detail::g_concoursePaneArmed.store(treat || effects || sealed || shares || g_capturing.load(std::memory_order_relaxed) || g_bdArmed.load(std::memory_order_relaxed) ||
                                           g_bakePeopleOff.load(std::memory_order_relaxed),
                                       std::memory_order_release);
}

}  // namespace

uint64_t concoursePaneTakeTreated() noexcept { return g_treated.exchange(0, std::memory_order_relaxed); }
bool concoursePaneTakeSurveyRequest() noexcept { return g_surveyRequested.exchange(false, std::memory_order_relaxed); }
namespace {
// The glow constant buffer follows the levels and the anchor's up axis: written at the tick, only when a value changed. The one-shot line says the levels at first use
// (the steady mode treating panes) and again when they change.
void updateGlowCb(ID3D11Device* device) noexcept {
    ID3D11Buffer* cb = g_cbGlow.load(std::memory_order_acquire);
    ID3D11Buffer* cbBay = g_cbGlowBay.load(std::memory_order_acquire);
    if (!cb || !cbBay) return;
    const float sky = g_glowSky.load(std::memory_order_relaxed), angled = g_glowAngled.load(std::memory_order_relaxed), other = g_glowOther.load(std::memory_order_relaxed),
                bay = g_glowBay.load(std::memory_order_relaxed);
    float data[24] = {};
    const bool upOk = g_upFedValid.load(std::memory_order_relaxed);
    for (int k = 0; k < 3; ++k) data[k] = upOk ? g_upFed[k].load(std::memory_order_relaxed) : 0.0f;
    data[4] = sky;
    data[5] = angled;
    data[6] = other;
    for (int k = 0; k < 3; ++k) data[8 + k] = kSteadyColour[k];
    float dataBay[24] = {};
    dataBay[4] = dataBay[5] = dataBay[6] = bay;
    for (int k = 0; k < 3; ++k) dataBay[8 + k] = kSteadyColour[k];
    // The backdrop's row 3: the distance mode (cc::kParallax*) and the sphere's radius. Both buffers carry it (the bay window is backdrop too).
    data[12] = dataBay[12] = static_cast<float>(g_bdParallax.load(std::memory_order_relaxed));
    data[13] = dataBay[13] = g_bdRadius.load(std::memory_order_relaxed);
    // Rows 4 and 5: the glass look (r, g, b, haze; blur, fresnel).
    for (int k = 0; k < 6; ++k) data[16 + k] = dataBay[16 + k] = g_glass[k].load(std::memory_order_relaxed);
    const bool changed = !g_glowCbSentValid || std::memcmp(data, g_glowCbSent, sizeof(data)) != 0;
    const bool changedBay = !g_glowCbSentValid || std::memcmp(dataBay, g_glowCbBaySent, sizeof(dataBay)) != 0;
    if (changed || changedBay) {
        ID3D11DeviceContext* imm = nullptr;
        device->GetImmediateContext(&imm);
        if (imm) {
            FlatComputeInternalScope internal;
            if (changed) imm->UpdateSubresource(cb, 0, nullptr, data, 0, 0);
            if (changedBay) imm->UpdateSubresource(cbBay, 0, nullptr, dataBay, 0, 0);
            imm->Release();
            std::memcpy(g_glowCbSent, data, sizeof(data));
            std::memcpy(g_glowCbBaySent, dataBay, sizeof(dataBay));
            g_glowCbSentValid = true;
        }
    }
    if ((g_mode.load(std::memory_order_relaxed) == kModeSteady || g_mode.load(std::memory_order_relaxed) == kModeBackdrop) && g_treatNow.load(std::memory_order_relaxed) &&
        (!g_glowLogged || g_glowLoggedLv[0] != sky || g_glowLoggedLv[1] != angled || g_glowLoggedLv[2] != other || g_glowLoggedLv[3] != bay)) {
        g_glowLogged = true;
        g_glowLoggedLv[0] = sky;
        g_glowLoggedLv[1] = angled;
        g_glowLoggedLv[2] = other;
        g_glowLoggedLv[3] = bay;
        Log::get().note("concourse pane: glow levels sky %g angled %g other %g bay %g, up %.3f %.3f %.3f%s", static_cast<double>(sky), static_cast<double>(angled),
                        static_cast<double>(other), static_cast<double>(bay), static_cast<double>(data[0]), static_cast<double>(data[1]), static_cast<double>(data[2]),
                        upOk ? "" : " (not read: every window takes the other level)");
    }
}

// Every 30 s, while any family was withheld: one line naming the families that were (a family that never matched is absent).
void logWithheld(uint64_t nowMs) noexcept {
    if (nowMs - g_withheldLogMs < 30000) return;
    g_withheldLogMs = nowMs;
    char text[900] = {};
    size_t used = 0;
    bool any = false;
    for (size_t i = 0; i < kWithheldFamilies; ++i) {
        const uint64_t n = g_withheldFam[i].exchange(0, std::memory_order_relaxed);
        if (n == 0) continue;
        const char* name = i < cc::kWithheldPsCount ? cc::kWithheldPs[i].name : cc::kWithheldVs[i - cc::kWithheldPsCount].name;
        const int w = std::snprintf(text + used, sizeof(text) - used, "%s%s %llu", any ? ", " : "", name, static_cast<unsigned long long>(n));
        any = true;
        if (w > 0 && used + static_cast<size_t>(w) < sizeof(text)) used += static_cast<size_t>(w);
    }
    const uint64_t limited = g_farLimited.exchange(0, std::memory_order_relaxed);
    if (limited != 0) {
        const int w = std::snprintf(text + used, sizeof(text) - used, "%sfar glows limited %llu", any ? ", " : "", static_cast<unsigned long long>(limited));
        any = true;
        if (w > 0 && used + static_cast<size_t>(w) < sizeof(text)) used += static_cast<size_t>(w);
    }
    if (any) Log::get().note("concourse withheld 30s: %s", text);
}
}  // namespace

uint64_t concoursePaneTakeWithheld() noexcept { return g_withheld.exchange(0, std::memory_order_relaxed); }
ConcoursePaneMirrorStats concoursePaneTakeMirrorStats() noexcept {
    ConcoursePaneMirrorStats r;
    r.markPasses = g_markPasses.exchange(0, std::memory_order_relaxed);
    r.markRefused = g_markRefused.exchange(0, std::memory_order_relaxed);
    r.tests = g_testsRead.exchange(0, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(g_shareMutex);
        r.shareSamplesDropped = g_sampleDropped;
        g_sampleDropped = 0;
    }
    std::lock_guard<std::mutex> lock(g_sideMutex);
    r.known = g_sides.knownCount();
    r.exterior = g_sides.exteriorCount();
    r.dropped = g_sides.dropped();
    return r;
}
const char* concoursePaneModeName() noexcept { return modeText(g_mode.load(std::memory_order_relaxed)); }
void concoursePaneNoteShader(uint64_t hash, const void* bytecode, size_t len) noexcept {
    if (!cc::glassLayoutPs(bytecode, len)) return;
    static std::mutex writers;   // creation is rare and the draw path never takes this
    std::lock_guard<std::mutex> lock(writers);
    if (learnedPs(hash)) return;
    const uint32_t n = g_learnedN.load(std::memory_order_relaxed);
    if (n >= kLearnedCap) {
        if (!g_learnedFullSaid.exchange(true, std::memory_order_relaxed))
            Log::get().note("concourse pane: the learned glass shader set is full (%u); ps %016llX is not added", kLearnedCap, static_cast<unsigned long long>(hash));
        return;
    }
    g_learnedPs[n].store(hash, std::memory_order_relaxed);
    g_learnedN.store(n + 1, std::memory_order_release);
    Log::get().note("concourse pane: a new pixel shader has the glass's layout: ps %016llX joins the glass family (%u known)", static_cast<unsigned long long>(hash), n + 1);
}
uint32_t concoursePaneSlotCount() noexcept { return g_listN.load(std::memory_order_relaxed); }
bool concoursePaneTreating() noexcept { return g_treatNow.load(std::memory_order_relaxed); }

void concoursePaneSetFeed(const ConcoursePaneFeed& feed) noexcept {
    g_bayN.store(feed.bayN != 0 ? feed.bayN : cc::kBayIndexCount, std::memory_order_relaxed);
    g_radiusFed.store(feed.backdropRadiusM > 0.0f ? feed.backdropRadiusM : cc::kBackdropRadiusDefaultM, std::memory_order_relaxed);
    g_feedMeasure.store(feed.measure, std::memory_order_relaxed);
    const bool treatBefore = g_feedTreat.exchange(feed.treat, std::memory_order_relaxed);
    if (!treatBefore && feed.treat) {   // a new visit to a concourse: what was learned about the panes is forgotten
        std::lock_guard<std::mutex> lock(g_sideMutex);
        g_sides.reset();
        if (!g_bakeBaked.load(std::memory_order_acquire)) g_bdResetPending.store(true, std::memory_order_release);   // and the backdrop cube and its origin (the tick does it)
    }
    if (!treatBefore && feed.treat) {   // and the size of the main view (the side table's lock is let go before the shares' is taken)
        std::lock_guard<std::mutex> lock(g_shareMutex);
        g_mainPixels = 0.0;
    }
    if (!g_feedWithhold.exchange(feed.withhold, std::memory_order_relaxed) && feed.withhold) {
        g_plainFrom.store(g_frameNow.load(std::memory_order_relaxed), std::memory_order_relaxed);
    }
    g_feedEffects.store(feed.effects, std::memory_order_relaxed);
    g_feedSealed.store(feed.sealed, std::memory_order_relaxed);
    g_realSkyNow.store(feed.realSky, std::memory_order_relaxed);
    g_bayOpacity.store(feed.bayOpacity, std::memory_order_relaxed);
    for (int k = 0; k < 3; ++k) g_upFed[k].store(feed.upValid ? feed.up[k] : 0.0f, std::memory_order_relaxed);
    g_upFedValid.store(feed.upValid, std::memory_order_relaxed);
    updateArmed();
}

ConcoursePaneShares concoursePaneShares() noexcept {
    std::lock_guard<std::mutex> lock(g_shareMutex);
    ConcoursePaneShares r = g_latest;
    if (r.valid && GetTickCount64() - g_latestMs > 250) r = ConcoursePaneShares{};   // no family draw on screen makes no reading: the bay is out of view, soon
    return r;
}

namespace {
// The state machine, once a tick (the cull's tick runs first in the same Present: it arms, and reads the result a tick later). Idle -> (arm requested) -> Armed, whose
// pane draws, in the frames after this tick, copy the scene and capture; at the tick after a frame in which at least one pane was captured, the back texture is copied to
// the front one and the phase is Done. With no pane captured for kBackdropWaitFrames frames it is Empty. A resource refusal is Refused (its line is logged by the maker).
void backdropTick(ID3D11Device* device, uint32_t frame) noexcept {
    const auto phase = static_cast<ConcourseBackdropPhase>(g_bdPhase.load(std::memory_order_relaxed));
    // A new visit (treat rose): the cube is emptied and the origin forgotten, unless a capture is open (then at the next tick without one).
    if (g_bdResetPending.load(std::memory_order_acquire) && !g_bdArmed.load(std::memory_order_acquire) && !g_bdArmRequested.load(std::memory_order_acquire) &&
        phase != ConcourseBackdropPhase::kArmed) {
        g_bdResetPending.store(false, std::memory_order_release);
        backdropReset(device);
    }
    if (phase == ConcourseBackdropPhase::kIdle && g_bdArmRequested.exchange(false, std::memory_order_acq_rel)) {
        if (!device) {
            bdRefuse("no device");
            g_bdPhase.store(static_cast<uint8_t>(ConcourseBackdropPhase::kRefused), std::memory_order_relaxed);
            if (g_bdToast.load(std::memory_order_relaxed)) queueToast("Windows: capture refused");
        } else if (!makeBackdropResources(device) || !g_statesReady.load(std::memory_order_acquire)) {
            if (g_statesReady.load(std::memory_order_acquire) == false && g_bdBack.load() != nullptr) bdRefuse("the pane states are not made (the cull is not treating panes)");
            g_bdPhase.store(static_cast<uint8_t>(ConcourseBackdropPhase::kRefused), std::memory_order_relaxed);
            if (g_bdToast.load(std::memory_order_relaxed)) queueToast("Windows: capture refused");
        } else {
            g_bdPaneDraws.store(0, std::memory_order_relaxed);
            g_bdSceneCopied.store(false, std::memory_order_relaxed);
            g_bdOriginSetNow.store(false, std::memory_order_relaxed);
            g_bdSceneFrame.store(0xFFFFFFFFu, std::memory_order_relaxed);
            g_bdSceneOkFrame.store(0xFFFFFFFFu, std::memory_order_relaxed);
            g_bdSceneRefusedSaid.store(false, std::memory_order_relaxed);
            g_bdPassRefusedSaid.store(false, std::memory_order_relaxed);
            g_bdFramesWaited = 0;
            g_bdArmFrame = frame;
            g_bdPhase.store(static_cast<uint8_t>(ConcourseBackdropPhase::kArmed), std::memory_order_relaxed);
            g_bdArmed.store(true, std::memory_order_release);   // the draws after this tick are the capture frame
        }
        return;
    }
    if (phase != ConcourseBackdropPhase::kArmed || frame == g_bdArmFrame) return;
    g_bdFramesWaited = frame - g_bdArmFrame;
    if (g_bdPaneDraws.load(std::memory_order_relaxed) > 0) {
        g_bdArmed.store(false, std::memory_order_release);
        ID3D11DeviceContext* imm = nullptr;
        if (device) device->GetImmediateContext(&imm);
        if (g_bdBack.load(std::memory_order_acquire) && g_bdFront.load(std::memory_order_acquire) && imm) {
            backdropPublishFront(imm);   // back to front, mip 0 slice by slice, then the mips of both cubes
            g_bdCaptures.fetch_add(1, std::memory_order_relaxed);
            g_bdCubeValid.store(true, std::memory_order_release);
            g_bdPhase.store(static_cast<uint8_t>(ConcourseBackdropPhase::kDone), std::memory_order_relaxed);
            if (g_bdToast.load(std::memory_order_relaxed)) queueToast("Windows: view captured");
        } else {
            bdRefuse("the cube could not be copied (no immediate context or texture)");
            g_bdPhase.store(static_cast<uint8_t>(ConcourseBackdropPhase::kRefused), std::memory_order_relaxed);
            if (g_bdToast.load(std::memory_order_relaxed)) queueToast("Windows: capture refused");
        }
        if (imm) imm->Release();
    } else if (g_bdFramesWaited >= cc::kBackdropWaitFrames) {
        g_bdArmed.store(false, std::memory_order_release);
        g_bdPhase.store(static_cast<uint8_t>(ConcourseBackdropPhase::kEmpty), std::memory_order_relaxed);
        if (g_bdToast.load(std::memory_order_relaxed)) queueToast("Windows: nothing captured");
    }
}

// ---- The bake (TEMPORARY, concourse_pane.h) ------------------------------------------------------------------------------------------------------------------
// All of this runs on the render thread: the tick, the resolve's call and Present.
std::atomic<bool> g_bakeWatch{false};          // the resolve's call has work: a bake is between begin and the hold's end
std::atomic<bool> g_bakeCapturing{false};
std::atomic<bool> g_bakeWantPrepare{false};
std::atomic<bool> g_bakeHoldSave{false};       // each Present keeps the back buffer
std::atomic<bool> g_bakeHoldShow{false};       // each Present puts the kept image back
bool g_bakeArmedNow = false, g_bakeReady = false, g_bakeReleasing = false, g_bakeHeldValid = false, g_bakeAtCentre = false, g_bakeSaidRefused = false;
std::atomic<bool> g_bakeWantRelease{false};
std::atomic<uint32_t> g_bakeFrameHeight{0};    // the render height of the frame last resolved
uint32_t g_bakeCapHeight = 0;                  // the height the bake's frames had
uint32_t g_bakeHeightSeen = 0;                 // for the settling of a change of g_bakeFrameHeight
uint64_t g_bakeHeightMs = 0;
float g_bakeCentre[3] = {};
float g_bakeDirFwd[cc::kBakeDirs][3] = {};     // where each direction looks
uint32_t g_bakeWantMask = 0, g_bakeCapMask = 0;   // the directions wanted, and those captured: all of them is the end of the bake
float g_bakeNearM = cc::kBakeNearDefaultM;
uint32_t g_bakeCaps = 0, g_bakeSeenAtCentre = 0, g_bakeSeenAway = 0, g_bakeHoldPresents = 0, g_bakeReleaseTicks = 0, g_bakeHeldW = 0, g_bakeHeldH = 0;
uint64_t g_bakeHoldStartMs = 0;
uint32_t g_bakeHeldMismatches = 0;             // presents whose back buffer no longer matched the kept image (counted, the copy skipped)
bool g_bakeSaidHeldMismatch = false;
DXGI_FORMAT g_bakeHeldFmt = DXGI_FORMAT_UNKNOWN;
ID3D11Device* g_bakeDevice = nullptr;          // not held: compared only
ID3D11ComputeShader* g_csBake = nullptr;
ID3D11Buffer* g_cbBake = nullptr;
ID3D11Texture2D* g_bakeHeld = nullptr;
struct BakeConstants {
    float m[16];       // the position relative to the camera to clip: the frame's own camera rows
    float inv[16];     // clip over w back to that position
    float rel[4];      // the camera less the origin
    float size[4];     // width, height, the near limit, the far limit
};

// One cube texel a thread, not one pixel: where its direction falls in this frame, from the frame's own camera rows, and what is there. Every texel the frame covers gets a colour, so no
// holes are left for the mips to fill (Flight 25: a forward splat of 1080p across 90 degrees filled about one in four). The camera is at the origin in these frames, so a texel's direction
// from the camera is its direction from the origin. A point nearer than the near limit is the room's, and the texel keeps whatever another frame gave it.
bool bakeMakeCompute(ID3D11Device* device) noexcept {
    if (g_bakeDevice != device) {
        if (g_csBake) g_csBake->Release();
        if (g_cbBake) g_cbBake->Release();
        if (g_bakeHeld) g_bakeHeld->Release();
        g_csBake = nullptr;
        g_cbBake = nullptr;
        g_bakeHeld = nullptr;
        g_bakeHeldValid = false;
        g_bakeDevice = device;
    }
    if (!g_csBake) {
        // Precompiled (kConcourseBakeBytecode, text in concourse_pane_shaders.h): the compile used to run here on the render thread at every concourse entry.
        ID3D11DeviceContext* imm = nullptr;
        device->GetImmediateContext(&imm);
        if (!imm) return bdRefuse("the bake: the shader did not create");
        g_csBake = shaderSwapCreateCs(imm, kConcourseBakeBytecode, sizeof(kConcourseBakeBytecode), "concourse_bake", "concourse bake");
        imm->Release();
        if (!g_csBake) return bdRefuse("the bake: the shader did not create");
    }
    if (!g_cbBake) {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof(BakeConstants);
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(device->CreateBuffer(&bd, nullptr, &g_cbBake)) || !g_cbBake) return bdRefuse("the bake: the constant buffer could not be made");
    }
    return true;
}

// The hold's end. A frame with the real camera has been resolved, or it has waited long enough: the screen is never left frozen by a resolve that stopped.
void bakeEndHold(const char* why, uint64_t nowMs) noexcept {
    g_bakeHoldShow.store(false, std::memory_order_release);
    g_bakeWatch.store(false, std::memory_order_release);
    g_bakeReleasing = false;
    Log::get().note("concourse bake: the hold ends (%s): %u Presents over %llu ms; %u frames at the centre, %u captured", why, g_bakeHoldPresents,
                    static_cast<unsigned long long>(nowMs - g_bakeHoldStartMs), g_bakeSeenAtCentre, g_bakeCaps);
}

bool vrBakeHoldsCube() noexcept;   // TEMPORARY (the VR bake, below): armed or running, so the cube is not released (always false in the flat profile)

void bakeTick(ID3D11Device* device, uint64_t nowMs) noexcept {
    if (g_bakeWantRelease.exchange(false, std::memory_order_acq_rel) && !g_bakeArmedNow && !g_bakeWatch.load(std::memory_order_acquire) && !vrBakeHoldsCube() &&
        (g_bakeReady || g_bakeBaked.load(std::memory_order_acquire))) {
        releaseBackdrop();
        g_bakeReady = false;
        g_bakeCapHeight = 0;
        Log::get().note("concourse bake: the station is left behind: the cube is released");
    }
    if (g_bakeWantPrepare.exchange(false, std::memory_order_acq_rel)) {
        const uint64_t t0 = GetTickCount64();
        g_bakeReady = device && makeBackdropResources(device) && bakeMakeCompute(device);
        if (g_bakeReady) {
            ID3D11UnorderedAccessView* u1 = g_bdBackUav.load(std::memory_order_acquire);
            ID3D11UnorderedAccessView* u2 = g_bdDistBackUav.load(std::memory_order_acquire);
            ID3D11DeviceContext* imm = nullptr;
            device->GetImmediateContext(&imm);
            if (imm && u1 && u2) {
                FlatComputeInternalScope internal;
                const FLOAT zero[4] = {};
                imm->ClearUnorderedAccessViewFloat(u1, zero);
                imm->ClearUnorderedAccessViewFloat(u2, zero);
            } else {
                g_bakeReady = false;
            }
            if (imm) imm->Release();
        }
        Log::get().note("concourse bake: armed: the cube is %s after %llu ms", g_bakeReady ? "ready and empty" : "NOT READY", static_cast<unsigned long long>(GetTickCount64() - t0));
    }
    if (!g_bakeHoldShow.load(std::memory_order_acquire)) return;
    if (nowMs - g_bakeHoldStartMs > 1000) {
        bakeEndHold("a second has passed", nowMs);
    } else if (g_bakeReleasing) {
        if (!g_bakeAtCentre && g_bakeSeenAway > 0) bakeEndHold("the camera is back", nowMs);
        else if (++g_bakeReleaseTicks >= 12) bakeEndHold("12 ticks after the last frame, and the resolve did not see the camera back", nowMs);
    }
}
}  // namespace

bool concoursePaneBakeDone() noexcept { return g_bakeBaked.load(std::memory_order_acquire) && g_bdCubeValid.load(std::memory_order_acquire); }

bool concoursePaneBakeWouldBeSharper(uint64_t nowMs) noexcept {
    const uint32_t h = g_bakeFrameHeight.load(std::memory_order_relaxed);
    if (h != g_bakeHeightSeen) {
        g_bakeHeightSeen = h;
        g_bakeHeightMs = nowMs;
        return false;
    }
    return concoursePaneBakeDone() && cc::bakeWouldBeSharper(h, g_bakeCapHeight) && nowMs - g_bakeHeightMs >= cc::kBakeSharperSettleMs;
}

void concoursePaneBakeRelease() noexcept { g_bakeWantRelease.store(true, std::memory_order_release); }

void concoursePaneBakeArm(bool on) noexcept {
    if (on && !g_bakeArmedNow) {
        g_bakeWantPrepare.store(true, std::memory_order_release);
        g_bakeHeldValid = false;
        g_bakeHoldSave.store(true, std::memory_order_release);
    } else if (!on && g_bakeArmedNow) {
        g_bakeHoldSave.store(false, std::memory_order_release);
    }
    g_bakeArmedNow = on;
}

bool concoursePaneBakeBegin(const float centre[3], float nearM, float cutM) noexcept {
    g_bakeHoldSave.store(false, std::memory_order_release);
    if (!g_bakeReady || !g_bakeDevice) {
        Log::get().note("concourse bake: begin: the cube is not ready: nothing is captured or held");
        return false;
    }
    for (int k = 0; k < 3; ++k) g_bakeCentre[k] = centre[k];
    g_bakeNearM = cutM;   // the compute shader's near limit is the room's cut; the frames' own near plane is nearM
    g_bakePeopleLeft.store(0, std::memory_order_relaxed);
    g_bakePeopleOff.store(true, std::memory_order_release);
    detail::g_concoursePaneArmed.store(true, std::memory_order_release);   // the next draw, not the next tick: updateArmed keeps it while the flag holds
    g_bakeCaps = g_bakeSeenAtCentre = g_bakeSeenAway = g_bakeHoldPresents = g_bakeReleaseTicks = 0;
    g_bakeWantMask = g_bakeCapMask = 0;
    g_bakeAtCentre = false;
    g_bakeReleasing = false;
    g_bakeSaidRefused = false;
    g_bakeHeldMismatches = 0;
    g_bakeSaidHeldMismatch = false;
    g_bakeHoldStartMs = GetTickCount64();
    ID3D11Buffer* origin = g_bdOrigin.load(std::memory_order_acquire);
    ID3D11DeviceContext* imm = nullptr;
    g_bakeDevice->GetImmediateContext(&imm);
    if (imm && origin) {
        const float o[4] = {centre[0], centre[1], centre[2], 0.0f};
        imm->UpdateSubresource(origin, 0, nullptr, o, 0, 0);
        g_bdOriginSet.store(true, std::memory_order_release);
    }
    if (imm) imm->Release();
    g_bakeCapturing.store(true, std::memory_order_release);
    g_bakeWatch.store(true, std::memory_order_release);
    if (g_bakeHeldValid) g_bakeHoldShow.store(true, std::memory_order_release);
    Log::get().note("concourse bake: begin: centre %.2f %.2f %.2f, near %.0f m, the room's cut %.0f m (nearer is not written), the people left out; the kept image %s",
                    static_cast<double>(centre[0]), static_cast<double>(centre[1]), static_cast<double>(centre[2]), static_cast<double>(nearM), static_cast<double>(cutM),
                    g_bakeHeldValid ? "is held" : "IS NOT THERE: no hold");
    return true;
}

void concoursePaneBakeExpect(const float (*forward)[3], uint32_t count, uint32_t mask) noexcept {
    for (uint32_t d = 0; d < cc::kBakeDirs && d < count; ++d)
        for (int k = 0; k < 3; ++k) g_bakeDirFwd[d][k] = forward[d][k];
    g_bakeWantMask = mask;
    g_bakeCapMask = 0;
}

uint32_t concoursePaneBakeCaptured() noexcept { return g_bakeCapMask; }

void concoursePaneBakeFinish() noexcept {
    g_bakeCapturing.store(false, std::memory_order_release);
    const bool peopleWere = g_bakePeopleOff.exchange(false, std::memory_order_acq_rel);
    if (!g_bakeWatch.load(std::memory_order_acquire)) return;
    const uint64_t now = GetTickCount64();
    ID3D11DeviceContext* imm = nullptr;
    if (g_bakeDevice) g_bakeDevice->GetImmediateContext(&imm);
    if (g_bakeCaps > 0 && imm) {
        backdropPublishFront(imm);
        g_bdCaptures.store(1, std::memory_order_release);
        g_bdCubeValid.store(true, std::memory_order_release);
        g_bakeBaked.store(true, std::memory_order_release);
        g_bdAutoOn.store(false, std::memory_order_relaxed);
        setMode(kModeBackdrop, "the bake", false);
    }
    if (imm) imm->Release();
    Log::get().note("concourse bake: finished after %llu ms: %u frames captured, of %u seen at the centre; %s; the people's draws left out over its frames: %llu%s",
                    static_cast<unsigned long long>(now - g_bakeHoldStartMs), g_bakeCaps, g_bakeSeenAtCentre,
                    g_bakeCaps > 0 ? "the windows show it now" : "NOTHING CAPTURED, the cube is as it was (is an anti-aliasing mode on? the capture sits in its path)",
                    static_cast<unsigned long long>(g_bakePeopleLeft.load(std::memory_order_relaxed)), peopleWere ? "" : " (the people were not being left out: no begin)");
    g_bakeReleaseTicks = 0;
    g_bakeSeenAway = 0;
    g_bakeReleasing = true;
    if (!g_bakeHoldShow.load(std::memory_order_acquire)) g_bakeWatch.store(false, std::memory_order_release);
}

void concoursePaneBakeCancel() noexcept {
    g_bakeHoldSave.store(false, std::memory_order_release);
    g_bakeCapturing.store(false, std::memory_order_release);
    g_bakePeopleOff.store(false, std::memory_order_release);
    g_bakeArmedNow = false;
    if (g_bakeHoldShow.load(std::memory_order_acquire)) {
        g_bakeReleaseTicks = 0;
        g_bakeSeenAway = 0;
        g_bakeReleasing = true;
    } else {
        g_bakeWatch.store(false, std::memory_order_release);
    }
    Log::get().note("concourse bake: cancelled: nothing is published");
}

void concoursePaneBakeFrame(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* color, ID3D11ShaderResourceView* depth, const float camera[6][4], uint32_t width, uint32_t height) noexcept {
    g_bakeFrameHeight.store(height, std::memory_order_relaxed);
    if (!g_bakeWatch.load(std::memory_order_acquire)) return;
    const float dx = camera[5][0] - g_bakeCentre[0], dy = camera[5][1] - g_bakeCentre[1], dz = camera[5][2] - g_bakeCentre[2];
    const bool atCentre = dx * dx + dy * dy + dz * dz < 4.0f;
    // A frame back at the player with the near plane still raised would show the room clipped away, so the hold does not end on one (the game's own is 0.025 m).
    g_bakeAtCentre = atCentre || camera[3][2] > 1.0f;
    if (!atCentre) {
        if (!g_bakeAtCentre) ++g_bakeSeenAway;
        return;
    }
    ++g_bakeSeenAtCentre;
    if (!g_bakeCapturing.load(std::memory_order_acquire)) return;
    // Clip w is the position along the view's forward axis, so these are where this frame looks.
    const float fw[3] = {camera[0][3], camera[1][3], camera[2][3]};
    // Only a direction wanted and not yet taken: the frames aimed again while a capture was on its way go on arriving.
    uint32_t dirBit = 0;
    for (uint32_t d = 0; d < cc::kBakeDirs; ++d) {
        if ((g_bakeWantMask & (1u << d)) != 0 && fw[0] * g_bakeDirFwd[d][0] + fw[1] * g_bakeDirFwd[d][1] + fw[2] * g_bakeDirFwd[d][2] > 0.999f) {
            dirBit = 1u << d;
            break;
        }
    }
    if (dirBit == 0 || (g_bakeCapMask & dirBit) != 0) return;
    ID3D11UnorderedAccessView* uav[2] = {g_bdDistBackUav.load(std::memory_order_acquire), g_bdBackUav.load(std::memory_order_acquire)};
    ID3D11SamplerState* smp = g_bdSampler.load(std::memory_order_acquire);
    if (!ctx || !color || !depth || !g_csBake || !g_cbBake || !uav[0] || !uav[1] || !smp || width == 0 || height == 0) {
        if (!g_bakeSaidRefused) {
            g_bakeSaidRefused = true;
            Log::get().note("concourse bake: a frame refused: colour %d depth %d shader %d cubes %d sampler %d, %ux%u", color ? 1 : 0, depth ? 1 : 0, g_csBake ? 1 : 0, uav[0] && uav[1] ? 1 : 0, smp ? 1 : 0, width, height);
        }
        return;
    }
    // b1[270..273] are the clip columns over the position relative to the camera (flat_projection_recipes.h), so the matrix's column c, row r is camera[c][r].
    double m[16], inv[16];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) m[r * 4 + c] = camera[c][r];
    if (!cc::invert4(m, inv)) {
        if (!g_bakeSaidRefused) {
            g_bakeSaidRefused = true;
            Log::get().note("concourse bake: a frame refused: its camera rows do not invert");
        }
        return;
    }
    BakeConstants k{};
    for (int i = 0; i < 16; ++i) {
        k.m[i] = static_cast<float>(m[i]);
        k.inv[i] = static_cast<float>(inv[i]);
    }
    k.rel[0] = dx;
    k.rel[1] = dy;
    k.rel[2] = dz;
    k.size[0] = static_cast<float>(width);
    k.size[1] = static_cast<float>(height);
    k.size[2] = g_bakeNearM;
    k.size[3] = cc::kBackdropFarM;
    {
        FlatComputeInternalScope internal;
        ID3D11ComputeShader* oldCs = nullptr;
        ID3D11ShaderResourceView* oldSrv[2] = {};
        ID3D11UnorderedAccessView* oldUav[2] = {};
        ID3D11Buffer* oldCb = nullptr;
        ID3D11SamplerState* oldSmp = nullptr;
        ctx->CSGetShader(&oldCs, nullptr, nullptr);
        ctx->CSGetShaderResources(0, 2, oldSrv);
        ctx->CSGetUnorderedAccessViews(0, 2, oldUav);
        ctx->CSGetConstantBuffers(0, 1, &oldCb);
        ctx->CSGetSamplers(0, 1, &oldSmp);
        ctx->UpdateSubresource(g_cbBake, 0, nullptr, &k, 0, 0);
        ID3D11ShaderResourceView* srv[2] = {color, depth};
        ctx->CSSetShader(g_csBake, nullptr, 0);
        ctx->CSSetConstantBuffers(0, 1, &g_cbBake);
        ctx->CSSetSamplers(0, 1, &smp);
        ctx->CSSetShaderResources(0, 2, srv);
        ctx->CSSetUnorderedAccessViews(0, 2, uav, nullptr);
        ctx->Dispatch(cc::kBackdropCubeSize / 32u, cc::kBackdropCubeSize / 32u, 6);
        ctx->CSSetShader(oldCs, nullptr, 0);
        ctx->CSSetConstantBuffers(0, 1, &oldCb);
        ctx->CSSetSamplers(0, 1, &oldSmp);
        ctx->CSSetShaderResources(0, 2, oldSrv);
        ctx->CSSetUnorderedAccessViews(0, 2, oldUav, nullptr);
        if (oldCs) oldCs->Release();
        if (oldCb) oldCb->Release();
        if (oldSmp) oldSmp->Release();
        for (auto* p : oldSrv) if (p) p->Release();
        for (auto* p : oldUav) if (p) p->Release();
    }
    g_bakeCapHeight = height;
    g_bakeCapMask |= dirBit;
    ++g_bakeCaps;
    if (g_bakeCaps <= 16) {
        const float sy = std::sqrt(camera[0][1] * camera[0][1] + camera[1][1] * camera[1][1] + camera[2][1] * camera[2][1]);
        // Clip z is the near plane over a position at 1 m: with reversed infinite depth, the fourth column's z (flat_projection_recipes.h), so this says whether the near write took.
        Log::get().note("concourse bake: capture %u: %ux%u, forward %.2f %.2f %.2f, vertical field of view %.1f degrees, near plane %.3f m", g_bakeCaps, width, height, static_cast<double>(camera[0][3]), static_cast<double>(camera[1][3]),
                        static_cast<double>(camera[2][3]), sy > 0.0f ? 2.0 * std::atan(1.0 / static_cast<double>(sy)) * 57.29578 : 0.0, static_cast<double>(camera[3][2]));
    }
}

void concoursePaneBakePresent(IDXGISwapChain* swap) noexcept {
    const bool save = g_bakeHoldSave.load(std::memory_order_acquire), show = g_bakeHoldShow.load(std::memory_order_acquire);
    if ((!save && !show) || !swap || !g_bakeDevice) return;
    ID3D11Texture2D* back = nullptr;
    if (FAILED(swap->GetBuffer(0, IID_PPV_ARGS(&back))) || !back) return;
    ID3D11DeviceContext* imm = nullptr;
    g_bakeDevice->GetImmediateContext(&imm);
    if (imm) {
        FlatComputeInternalScope internal;
        if (show) {
            ++g_bakeHoldPresents;
            if (g_bakeHeld && g_bakeHeldValid) {
                // CopyResource needs the two to match exactly; the swap chain may have been resized since the image was kept. The held image was made one sample (see below).
                D3D11_TEXTURE2D_DESC bd{};
                back->GetDesc(&bd);
                if (bd.Width == g_bakeHeldW && bd.Height == g_bakeHeldH && bd.Format == g_bakeHeldFmt && bd.SampleDesc.Count == 1) {
                    imm->CopyResource(back, g_bakeHeld);
                } else {
                    g_bakeHeldValid = false;
                    ++g_bakeHeldMismatches;
                    if (!g_bakeSaidHeldMismatch) {
                        g_bakeSaidHeldMismatch = true;
                        Log::get().note("concourse bake: the kept image was NOT shown: the back buffer is %ux%u format %u, %u sample(s), the kept image %ux%u format %u; it is dropped, so the game's "
                                        "own frames show (once a bake)",
                                        bd.Width, bd.Height, static_cast<unsigned>(bd.Format), bd.SampleDesc.Count, g_bakeHeldW, g_bakeHeldH, static_cast<unsigned>(g_bakeHeldFmt));
                    }
                }
            }
        } else {
            D3D11_TEXTURE2D_DESC bd{};
            back->GetDesc(&bd);
            if (!g_bakeHeld || g_bakeHeldW != bd.Width || g_bakeHeldH != bd.Height || g_bakeHeldFmt != bd.Format) {
                if (g_bakeHeld) g_bakeHeld->Release();
                g_bakeHeld = nullptr;
                g_bakeHeldValid = false;
                D3D11_TEXTURE2D_DESC td = bd;
                td.Usage = D3D11_USAGE_DEFAULT;
                td.BindFlags = 0;
                td.CPUAccessFlags = 0;
                td.MiscFlags = 0;
                td.MipLevels = 1;
                td.ArraySize = 1;
                if (bd.SampleDesc.Count == 1 && SUCCEEDED(g_bakeDevice->CreateTexture2D(&td, nullptr, &g_bakeHeld)) && g_bakeHeld) {
                    g_bakeHeldW = bd.Width;
                    g_bakeHeldH = bd.Height;
                    g_bakeHeldFmt = bd.Format;
                }
            }
            if (g_bakeHeld) {
                imm->CopyResource(g_bakeHeld, back);
                g_bakeHeldValid = true;
            }
        }
        imm->Release();
    }
    back->Release();
}

// ---- The VR bake (TEMPORARY, concourse_pane.h) ----------------------------------------------------------------------------------------------------------------------------
namespace {
namespace cv = edvr::concourse_vr;
constexpr uint32_t kVrSlots = 8;          // frames whose counts and rows are on their way back
constexpr UINT kVrRows = 10;              // b1 rows 270..279
constexpr UINT kVrRowFirst = 270;
constexpr uint32_t kVrOutCap = 32;
constexpr uint32_t kVrCountWords = 32;                    // TEMPORARY (H58-H61 instrument, Flight 53): words an eye's counts take (the shader's comment lists them)
constexpr uint32_t kVrCountBytes = 2u * kVrCountWords * 4u;
std::atomic<bool> g_vrDiscover{false}, g_vrToneSeen{false}, g_vrRunning{false}, g_vrCapturing{false}, g_vrWantPrepare{false};
bool g_vrArmedNow = false, g_vrReady = false;
ConcourseVrToneSeen g_vrToneWhat;
ID3D11Device* g_vrDevice = nullptr;       // not held: compared only
ID3D11ComputeShader* g_csVr = nullptr;
ID3D11Buffer* g_cbVr = nullptr;
ID3D11Buffer* g_vrRowsBuf[2] = {};
ID3D11ShaderResourceView* g_vrRowsSrv[2] = {};
ID3D11Buffer* g_vrRowsStage[2][kVrSlots] = {};
ID3D11Buffer* g_vrCount[kVrSlots] = {};
ID3D11UnorderedAccessView* g_vrCountUav[kVrSlots] = {};
ID3D11Buffer* g_vrCountStage[kVrSlots] = {};
ID3D11Texture2D* g_vrDepthTex[2] = {};    // the texture each cached view was made on (the view holds it)
ID3D11ShaderResourceView* g_vrDepthSrv[2] = {};
struct VrCountSlot {
    uint32_t frame = 0xFFFFFFFFu, copyFrame = 0;
    bool used = false, copied = false;
    bool dispatched[2] = {}, both[2] = {};
};
VrCountSlot g_vrSlot[kVrSlots];
struct VrRowSlot {
    uint32_t frame = 0xFFFFFFFFu;
    bool pending = false;
};
VrRowSlot g_vrRowSlot[2][kVrSlots];
uint32_t g_vrSnapFrame[2] = {0xFFFFFFFFu, 0xFFFFFFFFu};
uint32_t g_vrToneFrame = 0xFFFFFFFFu, g_vrToneNext = 0, g_vrSeq = 0;
uint32_t g_vrOrder[4] = {}, g_vrOrderFrame = 0xFFFFFFFFu, g_vrLastOrder[4] = {};
struct VrBakeConstants {
    float faceR[5][4], faceU[5][4], faceF[5][4];
    float origin[4];
    float size[4];     // width, height, the room's cut, the far limit
    float lim[4];      // the at-centre limit, the face slack, the least forward dot, 0
    uint32_t ctl[4];   // the faces wanted, the other eye's rows are this frame's, the eye, 0
};
VrBakeConstants g_vrK{};
std::atomic<uint64_t> g_vrToneDraws{0}, g_vrDisp[2], g_vrNoRows{0}, g_vrNoDepth{0}, g_vrRefused{0}, g_vrSnaps[2], g_vrSnapNoCb{0}, g_vrLost{0};
ConcourseVrBakeRows g_vrRowsOut[kVrOutCap];
uint32_t g_vrRowsOutN = 0;
ConcourseVrBakeCount g_vrCountOut[kVrOutCap];
uint32_t g_vrCountOutN = 0;
bool g_vrSaidRefused = false;
// TEMPORARY (H58-H61 instrument, Flight 53): the aim ring, keyed by frame. Packed: bit 63 valid, bits 32..62 the low 31 bits of the frame, 8 the write flag, 4..7 the dwell, 0..3 the face.
constexpr uint32_t kVrAimRing = 16;
std::atomic<uint64_t> g_vrAimRing[kVrAimRing];
bool vrAimWrites(uint32_t frame) noexcept {
    const uint64_t v = g_vrAimRing[frame % kVrAimRing].load(std::memory_order_acquire);
    return (v >> 63) != 0 && static_cast<uint32_t>((v >> 32) & 0x7FFFFFFFu) == (frame & 0x7FFFFFFFu) && (v & 0x100u) != 0;
}
uint64_t vrAimEntry(uint32_t frame) noexcept { return g_vrAimRing[frame % kVrAimRing].load(std::memory_order_acquire); }

template <class T>
void vrRelease(T*& p) noexcept {
    if (p) p->Release();
    p = nullptr;
}
void vrReleaseAll() noexcept {
    vrRelease(g_csVr);
    vrRelease(g_cbVr);
    for (int e = 0; e < 2; ++e) {
        vrRelease(g_vrRowsBuf[e]);
        vrRelease(g_vrRowsSrv[e]);
        vrRelease(g_vrDepthSrv[e]);
        g_vrDepthTex[e] = nullptr;
        for (uint32_t s = 0; s < kVrSlots; ++s) {
            vrRelease(g_vrRowsStage[e][s]);
            g_vrRowSlot[e][s] = VrRowSlot{};
        }
    }
    for (uint32_t s = 0; s < kVrSlots; ++s) {
        vrRelease(g_vrCount[s]);
        vrRelease(g_vrCountUav[s]);
        vrRelease(g_vrCountStage[s]);
        g_vrSlot[s] = VrCountSlot{};
    }
    g_vrReady = false;
}

// The VR variant of the bake's shader. One cube texel a thread, as flat's, but every matrix is the frame's own on the GPU: t2 holds this eye's b1 rows 270..279 copied at its
// first scene draw (t3 the other eye's), the clip matrix in column form is m[r * 4 + c] = row (270 + c) lane r, as the flat bake builds it, and its inverse is made per group
// (concourse_vr_bake_core.h, the cofactors' one text). A frame is taken only when its eye's row 275 is within lim.x of the origin (a frame the bake aimed); the face is the
// wanted one nearest where the frame looks (clip w's coefficients, as flat's forward), and a texel only on that face's own square, inside the eye's clip. Counts: per eye at
// ctl.z * 128 bytes (32 words; TEMPORARY, H58-H61 instrument, Flight 53): word 0 on the face, 1 in this eye's view, 2 written (past the room's cut, on a write frame only: ctl.w),
// 3 in either eye's view, 4 the face + 1, 5 the frame's status (1 taken, 2 not at the centre, 3 no face near where it looks, 4 the rows do not invert), 6 past the cut (what would
// be written), 7 nothing drawn (z == 0), 8..11 cut by the eye's distance t (<5, 5-20, 20-50, >=50), 12..16 drawn (z > 0) by t (<50, 50-200, 200-1000, 1000-4000, >=4000); the words
// 6..16 count the texels on the face and in this eye's view.
bool vrMakeShader(ID3D11Device* device) noexcept {
    // Precompiled (kConcourseVrBakeBytecode, text in concourse_pane_shaders.h): the compile used to cost most of the VR prepare's 62-94 ms on the render thread.
    ID3D11DeviceContext* imm = nullptr;
    device->GetImmediateContext(&imm);
    if (!imm) {
        Log::get().note("concourse bake: vr: the capture shader did not create (no immediate context)");
        return false;
    }
    g_csVr = shaderSwapCreateCs(imm, kConcourseVrBakeBytecode, sizeof(kConcourseVrBakeBytecode), "concourse_bake_vr", "concourse bake");
    imm->Release();
    if (!g_csVr) {
        Log::get().note("concourse bake: vr: the capture shader did not create");
        return false;
    }
    return true;
}

bool vrMake(ID3D11Device* device) noexcept {
    if (!device) return false;
    if (g_vrDevice != device) {
        vrReleaseAll();
        g_vrDevice = device;
    }
    if (g_vrReady) return true;
    if (!g_csVr && !vrMakeShader(device)) return false;
    bool good = true;
    if (!g_cbVr) {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof(VrBakeConstants);
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        good = good && SUCCEEDED(device->CreateBuffer(&bd, nullptr, &g_cbVr)) && g_cbVr;
    }
    for (int e = 0; e < 2 && good; ++e) {
        if (!g_vrRowsBuf[e]) {
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = kVrRows * 16u;
            bd.Usage = D3D11_USAGE_DEFAULT;
            bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            good = SUCCEEDED(device->CreateBuffer(&bd, nullptr, &g_vrRowsBuf[e])) && g_vrRowsBuf[e];
        }
        if (good && !g_vrRowsSrv[e]) {
            D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
            sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
            sd.Buffer.FirstElement = 0;
            sd.Buffer.NumElements = kVrRows;
            good = SUCCEEDED(device->CreateShaderResourceView(g_vrRowsBuf[e], &sd, &g_vrRowsSrv[e])) && g_vrRowsSrv[e];
        }
        for (uint32_t s = 0; s < kVrSlots && good; ++s) {
            if (g_vrRowsStage[e][s]) continue;
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = kVrRows * 16u;
            bd.Usage = D3D11_USAGE_STAGING;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            good = SUCCEEDED(device->CreateBuffer(&bd, nullptr, &g_vrRowsStage[e][s])) && g_vrRowsStage[e][s];
        }
    }
    for (uint32_t s = 0; s < kVrSlots && good; ++s) {
        if (!g_vrCount[s]) {
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = kVrCountBytes;
            bd.Usage = D3D11_USAGE_DEFAULT;
            bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
            bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
            good = SUCCEEDED(device->CreateBuffer(&bd, nullptr, &g_vrCount[s])) && g_vrCount[s];
        }
        if (good && !g_vrCountUav[s]) {
            D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
            ud.Format = DXGI_FORMAT_R32_TYPELESS;
            ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
            ud.Buffer.FirstElement = 0;
            ud.Buffer.NumElements = kVrCountBytes / 4u;
            ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
            good = SUCCEEDED(device->CreateUnorderedAccessView(g_vrCount[s], &ud, &g_vrCountUav[s])) && g_vrCountUav[s];
        }
        if (good && !g_vrCountStage[s]) {
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = kVrCountBytes;
            bd.Usage = D3D11_USAGE_STAGING;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            good = SUCCEEDED(device->CreateBuffer(&bd, nullptr, &g_vrCountStage[s])) && g_vrCountStage[s];
        }
    }
    if (!good) {
        Log::get().note("concourse bake: vr: the rows, counts or constant buffers could not be made");
        vrReleaseAll();
        return false;
    }
    g_vrReady = true;
    return true;
}

// A view of an eye's scene depth (the depth probe's texture), made once per texture. Null when it cannot be read as a shader resource.
ID3D11ShaderResourceView* vrDepthView(ID3D11Device* device, int eye, ID3D11Texture2D* tex) noexcept {
    if (!device || eye < 0 || eye > 1 || !tex) return nullptr;
    if (g_vrDepthTex[eye] == tex && g_vrDepthSrv[eye]) return g_vrDepthSrv[eye];
    vrRelease(g_vrDepthSrv[eye]);
    g_vrDepthTex[eye] = nullptr;
    D3D11_TEXTURE2D_DESC td{};
    tex->GetDesc(&td);
    const DXGI_FORMAT f = bdDepthReadFormat(td.Format);
    if (f == DXGI_FORMAT_UNKNOWN || (td.BindFlags & D3D11_BIND_SHADER_RESOURCE) == 0 || td.SampleDesc.Count != 1) return nullptr;
    D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
    vd.Format = f;
    vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    vd.Texture2D.MipLevels = 1;
    if (FAILED(device->CreateShaderResourceView(tex, &vd, &g_vrDepthSrv[eye])) || !g_vrDepthSrv[eye]) {
        g_vrDepthSrv[eye] = nullptr;
        return nullptr;
    }
    g_vrDepthTex[eye] = tex;
    return g_vrDepthSrv[eye];
}

bool vrBakeHoldsCube() noexcept { return g_vrArmedNow || g_vrRunning.load(std::memory_order_acquire); }

void vrSetHooks() noexcept {
    const uint32_t bits = (g_vrDiscover.load(std::memory_order_relaxed) || g_vrRunning.load(std::memory_order_relaxed) ? 1u : 0u) |
                          (g_vrRunning.load(std::memory_order_relaxed) ? 2u : 0u);
    detail::g_concourseVrBakeHooks.store(bits, std::memory_order_relaxed);
}

// The pane tick's part: the prepare, the counts copied once their frame has ended, and the copies mapped once they are in (no wait).
void vrBakeTick(ID3D11Device* device, uint32_t frame) noexcept {
    if (device != g_vrDevice && g_vrDevice) {   // another device: its buffers are not this one's
        vrReleaseAll();
        g_vrDevice = nullptr;
    }
    if (g_vrWantPrepare.exchange(false, std::memory_order_acq_rel)) {
        const uint64_t t0 = GetTickCount64();
        const bool ok = vrMake(device);
        Log::get().note("concourse bake: vr: armed: the VR capture shader and its buffers are %s after %llu ms", ok ? "ready" : "NOT READY", static_cast<unsigned long long>(GetTickCount64() - t0));
    }
    vrSetHooks();
    if (!g_vrReady || !device) return;
    ID3D11DeviceContext* imm = g_immediate.load(std::memory_order_relaxed);
    if (!imm) return;
    FlatComputeInternalScope internal;
    for (uint32_t s = 0; s < kVrSlots; ++s) {
        VrCountSlot& sl = g_vrSlot[s];
        if (!sl.used) continue;
        if (!sl.copied) {
            if (frame == sl.frame) continue;   // its draws are still to come
            imm->CopyResource(g_vrCountStage[s], g_vrCount[s]);
            sl.copied = true;
            sl.copyFrame = frame;
            continue;
        }
        if (frame == sl.copyFrame) continue;
        D3D11_MAPPED_SUBRESOURCE mp{};
        const HRESULT hr = imm->Map(g_vrCountStage[s], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mp);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) continue;
        if (SUCCEEDED(hr) && mp.pData) {
            uint32_t u[2 * kVrCountWords];
            std::memcpy(u, mp.pData, sizeof(u));
            imm->Unmap(g_vrCountStage[s], 0);
            if (g_vrCountOutN < kVrOutCap) {
                ConcourseVrBakeCount& c = g_vrCountOut[g_vrCountOutN++];
                c = ConcourseVrBakeCount{};
                c.frame = sl.frame;
                for (int e = 0; e < 2; ++e) {
                    c.dispatched[e] = sl.dispatched[e];
                    c.both[e] = sl.both[e];
                    const uint32_t* w = u + e * kVrCountWords;
                    c.onFace[e] = w[0];
                    c.inView[e] = w[1];
                    c.written[e] = w[2];
                    c.eitherView[e] = w[3];
                    c.face[e] = static_cast<int>(w[4]) - 1;
                    c.status[e] = w[5];
                    c.pastCut[e] = w[6];
                    c.nothingDrawn[e] = w[7];
                    for (int k = 0; k < 4; ++k) c.cutBy[e][k] = w[8 + k];
                    for (int k = 0; k < 5; ++k) c.drawnBy[e][k] = w[12 + k];
                }
            } else {
                g_vrLost.fetch_add(1, std::memory_order_relaxed);
            }
        }
        sl = VrCountSlot{};
    }
    for (int e = 0; e < 2; ++e)
        for (uint32_t s = 0; s < kVrSlots; ++s) {
            VrRowSlot& rs = g_vrRowSlot[e][s];
            if (!rs.pending || frame == rs.frame) continue;
            D3D11_MAPPED_SUBRESOURCE mp{};
            const HRESULT hr = imm->Map(g_vrRowsStage[e][s], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mp);
            if (hr == DXGI_ERROR_WAS_STILL_DRAWING) continue;
            if (SUCCEEDED(hr) && mp.pData) {
                if (g_vrRowsOutN < kVrOutCap) {
                    ConcourseVrBakeRows& r = g_vrRowsOut[g_vrRowsOutN++];
                    r.frame = rs.frame;
                    r.eye = e;
                    std::memcpy(r.rows, mp.pData, sizeof(r.rows));
                } else {
                    g_vrLost.fetch_add(1, std::memory_order_relaxed);
                }
                imm->Unmap(g_vrRowsStage[e][s], 0);
            }
            rs.pending = false;
        }
}
}  // namespace

void concoursePaneVrBakeSnapDraw(ID3D11DeviceContext* ctx) noexcept {
    if (!ctx || !g_vrReady || !g_vrRunning.load(std::memory_order_relaxed)) return;
    const uint32_t frame = g_frameNow.load(std::memory_order_relaxed);
    void* dsv = bindingGet(BindSlot::Dsv0);
    int eye = -1, target = -1;
    if (!dsv || !depthProbeCurrentSceneEyeOf(static_cast<ID3D11DepthStencilView*>(dsv), &eye, &target) || eye < 0 || eye > 1) return;
    if (g_vrSnapFrame[eye] == frame) return;
    g_vrSnapFrame[eye] = frame;   // one try a frame an eye, taken or not
    ID3D11Buffer* b1 = nullptr;
    UINT first = 0, num = 0;
    ID3D11DeviceContext1* ctx1 = nullptr;
    if (SUCCEEDED(ctx->QueryInterface(IID_PPV_ARGS(&ctx1))) && ctx1) {
        ctx1->VSGetConstantBuffers1(1, 1, &b1, &first, &num);
        ctx1->Release();
    } else {
        ctx->VSGetConstantBuffers(1, 1, &b1);
    }
    if (!b1) {
        g_vrSnapNoCb.fetch_add(1, std::memory_order_relaxed);
        g_vrSnapFrame[eye] = 0xFFFFFFFFu;   // another draw of this eye may have it
        return;
    }
    D3D11_BUFFER_DESC bd{};
    b1->GetDesc(&bd);
    const UINT offset = (first + kVrRowFirst) * 16u;
    if (bd.ByteWidth < offset + kVrRows * 16u) {
        b1->Release();
        g_vrSnapNoCb.fetch_add(1, std::memory_order_relaxed);
        g_vrSnapFrame[eye] = 0xFFFFFFFFu;
        return;
    }
    const D3D11_BOX box{offset, 0, 0, offset + kVrRows * 16u, 1, 1};
    const uint32_t s = frame % kVrSlots;
    {
        FlatComputeInternalScope internal;
        ctx->CopySubresourceRegion(g_vrRowsBuf[eye], 0, 0, 0, 0, b1, 0, &box);
        ctx->CopySubresourceRegion(g_vrRowsStage[eye][s], 0, 0, 0, 0, b1, 0, &box);
    }
    b1->Release();
    if (g_vrRowSlot[eye][s].pending) g_vrLost.fetch_add(1, std::memory_order_relaxed);
    g_vrRowSlot[eye][s].frame = frame;
    g_vrRowSlot[eye][s].pending = true;
    g_vrSnaps[eye].fetch_add(1, std::memory_order_relaxed);
    if (g_vrOrderFrame != frame) {
        g_vrOrderFrame = frame;
        for (uint32_t& o : g_vrOrder) o = 0;
    }
    g_vrOrder[eye * 2] = ++g_vrSeq;
}

void concoursePaneVrBakeToneDraw(ID3D11DeviceContext* ctx, char kind, uint32_t count, uint32_t instances, uint32_t startInstance) noexcept {
    if (!ctx || (kind != 'D' && kind != 'N') || count != 3 || instances != 1 || startInstance != 0) return;
    ToneAdmit ta;
    if (!tonemapAdmitStructure(ctx, bindingShaderHash(BindSlot::Ps), ta) || !tonemapAdmitFull(ta, bindingShaderHash(BindSlot::Vs)) || ta.hdrSlotRead < 0) return;
    const uint32_t frame = g_frameNow.load(std::memory_order_relaxed);
    if (frame != g_vrToneFrame) {
        g_vrToneFrame = frame;
        g_vrToneNext = 0;
    }
    // The eye by the draw's order in the frame, the first eye 0 (Flight 51: two admitted tonemap draws a frame, eye 0 then eye 1, the eye table never told).
    const int eye = g_vrToneNext < 2 ? static_cast<int>(g_vrToneNext) : -1;
    ++g_vrToneNext;
    if (eye < 0) return;
    ID3D11Texture2D* depth = nullptr;
    const bool depthOk = depthProbeSceneDepth(ta.hdrW, ta.hdrH, eye, &depth) && depth;
    if (!g_vrToneSeen.load(std::memory_order_relaxed) && depthOk) {
        D3D11_TEXTURE2D_DESC dd{};
        depth->GetDesc(&dd);
        g_vrToneWhat.hdrW = ta.hdrW;
        g_vrToneWhat.hdrH = ta.hdrH;
        g_vrToneWhat.hdrFmt = static_cast<uint32_t>(ta.hdrFmt);
        g_vrToneWhat.depthFmt = static_cast<uint32_t>(dd.Format);
        g_vrToneSeen.store(true, std::memory_order_release);
    }
    if (!g_vrCapturing.load(std::memory_order_acquire) || !g_vrReady) return;
    g_vrToneDraws.fetch_add(1, std::memory_order_relaxed);
    if (g_vrOrderFrame == frame) g_vrOrder[eye * 2 + 1] = ++g_vrSeq;
    if (g_vrSnapFrame[eye] != frame) {
        g_vrNoRows.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    ID3D11ShaderResourceView* depthSrv = depthOk ? vrDepthView(g_vrDevice, eye, depth) : nullptr;
    if (!depthSrv) {
        g_vrNoDepth.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    ID3D11ShaderResourceView* hdr = nullptr;
    ctx->PSGetShaderResources(static_cast<UINT>(ta.hdrSlotRead), 1, &hdr);
    ID3D11UnorderedAccessView* uavCube[2] = {g_bdDistBackUav.load(std::memory_order_acquire), g_bdBackUav.load(std::memory_order_acquire)};
    ID3D11SamplerState* smp = g_bdSampler.load(std::memory_order_acquire);
    if (!hdr || !uavCube[0] || !uavCube[1] || !smp || !g_csVr || !g_cbVr || ta.hdrW == 0 || ta.hdrH == 0) {
        if (!g_vrSaidRefused) {
            g_vrSaidRefused = true;
            Log::get().note("concourse bake: vr: a tone draw refused: H %d, cubes %d, sampler %d, shader %d, %ux%u", hdr ? 1 : 0, uavCube[0] && uavCube[1] ? 1 : 0, smp ? 1 : 0,
                            g_csVr ? 1 : 0, ta.hdrW, ta.hdrH);
        }
        if (hdr) hdr->Release();
        g_vrRefused.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const bool other = g_vrSnapFrame[1 - eye] == frame;
    const uint32_t s = frame % kVrSlots;
    VrCountSlot& sl = g_vrSlot[s];
    FlatComputeInternalScope internal;
    if (sl.frame != frame) {
        if (sl.used) g_vrLost.fetch_add(1, std::memory_order_relaxed);
        sl = VrCountSlot{};
        sl.frame = frame;
        sl.used = true;
        const UINT zero[4] = {};
        ctx->ClearUnorderedAccessViewUint(g_vrCountUav[s], zero);
        for (int k = 0; k < 4; ++k) g_vrLastOrder[k] = 0;
    }
    g_vrK.size[0] = static_cast<float>(ta.hdrW);
    g_vrK.size[1] = static_cast<float>(ta.hdrH);
    g_vrK.ctl[1] = other ? 1u : 0u;
    g_vrK.ctl[2] = static_cast<uint32_t>(eye);
    g_vrK.ctl[3] = vrAimWrites(frame) ? 1u : 0u;   // an aimed dwell frame writes the cube (the union of a face's frames); the rest only count (TEMPORARY counters, H58-H61)
    ID3D11ComputeShader* oldCs = nullptr;
    ID3D11ShaderResourceView* oldSrv[4] = {};
    ID3D11UnorderedAccessView* oldUav[3] = {};
    ID3D11Buffer* oldCb = nullptr;
    ID3D11SamplerState* oldSmp = nullptr;
    ctx->CSGetShader(&oldCs, nullptr, nullptr);
    ctx->CSGetShaderResources(0, 4, oldSrv);
    ctx->CSGetUnorderedAccessViews(0, 3, oldUav);
    ctx->CSGetConstantBuffers(0, 1, &oldCb);
    ctx->CSGetSamplers(0, 1, &oldSmp);
    ctx->UpdateSubresource(g_cbVr, 0, nullptr, &g_vrK, 0, 0);
    ID3D11ShaderResourceView* srv[4] = {hdr, depthSrv, g_vrRowsSrv[eye], g_vrRowsSrv[1 - eye]};
    ID3D11UnorderedAccessView* uav[3] = {uavCube[0], uavCube[1], g_vrCountUav[s]};
    ctx->CSSetShader(g_csVr, nullptr, 0);
    ctx->CSSetConstantBuffers(0, 1, &g_cbVr);
    ctx->CSSetSamplers(0, 1, &smp);
    ctx->CSSetShaderResources(0, 4, srv);
    ctx->CSSetUnorderedAccessViews(0, 3, uav, nullptr);
    ctx->Dispatch(cc::kBackdropCubeSize / 32u, cc::kBackdropCubeSize / 32u, 6);
    ctx->CSSetShader(oldCs, nullptr, 0);
    ctx->CSSetConstantBuffers(0, 1, &oldCb);
    ctx->CSSetSamplers(0, 1, &oldSmp);
    ctx->CSSetShaderResources(0, 4, oldSrv);
    ctx->CSSetUnorderedAccessViews(0, 3, oldUav, nullptr);
    if (oldCs) oldCs->Release();
    if (oldCb) oldCb->Release();
    if (oldSmp) oldSmp->Release();
    for (auto* p : oldSrv) if (p) p->Release();
    for (auto* p : oldUav) if (p) p->Release();
    hdr->Release();
    sl.dispatched[eye] = true;
    sl.both[eye] = other;
    g_vrDisp[eye].fetch_add(1, std::memory_order_relaxed);
    if (g_vrOrderFrame == frame)
        for (int k = 0; k < 4; ++k) g_vrLastOrder[k] = g_vrOrder[k];
}

void concoursePaneVrBakeDiscover(bool on) noexcept {
    g_vrDiscover.store(on && !g_vrToneSeen.load(std::memory_order_relaxed), std::memory_order_relaxed);
    vrSetHooks();
}

bool concoursePaneVrToneSeen(ConcourseVrToneSeen* what) noexcept {
    if (!g_vrToneSeen.load(std::memory_order_acquire)) return false;
    if (what) *what = g_vrToneWhat;
    return true;
}

void concoursePaneVrBakeArm(bool on) noexcept {
    if (on && !g_vrArmedNow) {
        g_bakeWantPrepare.store(true, std::memory_order_release);   // the cube, made and cleared by the bake's own prepare (bakeTick)
        g_vrWantPrepare.store(true, std::memory_order_release);
    }
    g_vrArmedNow = on;
}

bool concoursePaneVrBakeBegin() noexcept {
    if (!g_bakeReady || !g_vrReady || !g_bakeDevice) {
        Log::get().note("concourse bake: vr: begin: the cube is %s and the VR shader %s: nothing is captured", g_bakeReady ? "ready" : "NOT READY", g_vrReady ? "ready" : "NOT READY");
        return false;
    }
    for (uint32_t& f : g_vrSnapFrame) f = 0xFFFFFFFFu;
    for (int e = 0; e < 2; ++e) {
        g_vrDisp[e].store(0, std::memory_order_relaxed);
        g_vrSnaps[e].store(0, std::memory_order_relaxed);
    }
    g_vrToneDraws.store(0, std::memory_order_relaxed);
    g_vrNoRows.store(0, std::memory_order_relaxed);
    g_vrNoDepth.store(0, std::memory_order_relaxed);
    g_vrRefused.store(0, std::memory_order_relaxed);
    g_vrSnapNoCb.store(0, std::memory_order_relaxed);
    g_vrLost.store(0, std::memory_order_relaxed);
    g_vrRowsOutN = g_vrCountOutN = 0;
    for (auto& a : g_vrAimRing) a.store(0, std::memory_order_relaxed);
    for (uint32_t& o : g_vrLastOrder) o = 0;
    g_vrSaidRefused = false;
    g_vrCapturing.store(false, std::memory_order_release);
    g_vrDiffOn.store(false, std::memory_order_release);
    g_vrRunning.store(true, std::memory_order_release);
    vrSetHooks();
    return true;
}

void concoursePaneVrBakeCapture(const float origin[3], const float faces[5][9], uint32_t mask, float nearM, float cutM) noexcept {
    for (int f = 0; f < 5; ++f)
        for (int k = 0; k < 3; ++k) {
            g_vrK.faceR[f][k] = faces[f][k];
            g_vrK.faceU[f][k] = faces[f][3 + k];
            g_vrK.faceF[f][k] = faces[f][6 + k];
        }
    for (int f = 0; f < 5; ++f) g_vrK.faceR[f][3] = g_vrK.faceU[f][3] = g_vrK.faceF[f][3] = 0.0f;
    for (int k = 0; k < 3; ++k) g_vrK.origin[k] = origin[k];
    g_vrK.origin[3] = 0.0f;
    g_vrK.size[2] = cutM;
    g_vrK.size[3] = cc::kBackdropFarM;
    g_vrK.lim[0] = cv::kAtCentreM;
    g_vrK.lim[1] = cv::kFaceSlack;
    g_vrK.lim[2] = cv::kFaceMinDot;
    g_vrK.lim[3] = 0.0f;
    g_vrK.ctl[0] = mask;
    g_vrK.ctl[3] = 0;   // set per dispatch from the aim ring
    g_bakePeopleLeft.store(0, std::memory_order_relaxed);
    g_bakePeopleOff.store(true, std::memory_order_release);
    detail::g_concoursePaneArmed.store(true, std::memory_order_release);   // the next draw, not the next tick: updateArmed keeps it while the people's flag holds
    g_bakeHoldStartMs = GetTickCount64();                                  // the people's 2 s backstop counts from here
    ID3D11Buffer* originBuf = g_bdOrigin.load(std::memory_order_acquire);
    ID3D11DeviceContext* imm = nullptr;
    if (g_bakeDevice) g_bakeDevice->GetImmediateContext(&imm);
    if (imm && originBuf) {
        FlatComputeInternalScope internal;
        const float o[4] = {origin[0], origin[1], origin[2], 0.0f};
        imm->UpdateSubresource(originBuf, 0, nullptr, o, 0, 0);
        g_bdOriginSet.store(true, std::memory_order_release);
    }
    if (imm) imm->Release();
    g_vrCapturing.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(g_diffMutex);
        g_diffGroups.clear();
        g_diffGroupsDropped = 0;
    }
    g_vrDiffOn.store(true, std::memory_order_release);   // TEMPORARY (H62 instrument, Flight 54)
    Log::get().note("concourse bake: vr: capture: origin %.2f %.2f %.2f, faces 0x%X, near %.0f m, the room's cut %.0f m (nearer is not written), the people left out; a frame is taken "
                    "when its eye is within %.1f m of the origin and looks within %.0f degrees of a face",
                    static_cast<double>(origin[0]), static_cast<double>(origin[1]), static_cast<double>(origin[2]), mask, static_cast<double>(nearM), static_cast<double>(cutM),
                    static_cast<double>(cv::kAtCentreM), std::acos(static_cast<double>(cv::kFaceMinDot)) * 57.29578);
}

void concoursePaneVrBakeAimFrame(uint32_t frame, int face, uint32_t dwell, bool write) noexcept {
    const uint64_t v = (1ull << 63) | (static_cast<uint64_t>(frame & 0x7FFFFFFFu) << 32) | (write ? 0x100ull : 0ull) | (static_cast<uint64_t>(dwell & 0xFu) << 4) |
                       static_cast<uint64_t>(face & 0xF);
    g_vrAimRing[frame % kVrAimRing].store(v, std::memory_order_release);
}

bool concoursePaneVrBakeEnd(bool publish, const char* line) noexcept {
    const bool wasCapturing = g_vrCapturing.exchange(false, std::memory_order_acq_rel);
    g_vrDiffOn.store(false, std::memory_order_release);   // TEMPORARY (H62 instrument, Flight 54): the last face's diff is logged now
    vrDrawDiffFlush();
    const bool peopleWere = wasCapturing && g_bakePeopleOff.exchange(false, std::memory_order_acq_rel);
    const bool wasRunning = g_vrRunning.exchange(false, std::memory_order_acq_rel);
    vrSetHooks();
    if (!wasRunning) return false;
    bool published = false;
    if (publish && wasCapturing) {
        ID3D11DeviceContext* imm = nullptr;
        if (g_bakeDevice) g_bakeDevice->GetImmediateContext(&imm);
        if (imm) {
            backdropPublishFront(imm);
            g_bdCaptures.store(1, std::memory_order_release);
            g_bdCubeValid.store(true, std::memory_order_release);
            g_bakeBaked.store(true, std::memory_order_release);
            g_bdAutoOn.store(false, std::memory_order_relaxed);
            setMode(kModeBackdrop, "the VR bake", false);
            published = true;
            imm->Release();
        }
    }
    Log::get().note("concourse bake: %s; %s; the people's draws left out over its frames: %llu%s", line ? line : "vr: ended",
                    published ? "the windows show it now" : publish ? "NOT PUBLISHED (no immediate context, or it never captured)" : "nothing is published",
                    static_cast<unsigned long long>(g_bakePeopleLeft.load(std::memory_order_relaxed)), peopleWere || !wasCapturing ? "" : " (the people were not being left out)");
    return published;
}

uint32_t concoursePaneVrBakeTakeRows(ConcourseVrBakeRows* out, uint32_t cap) noexcept {
    const uint32_t n = g_vrRowsOutN < cap ? g_vrRowsOutN : cap;
    for (uint32_t i = 0; i < n; ++i) out[i] = g_vrRowsOut[i];
    g_vrRowsOutN = 0;
    return n;
}
uint32_t concoursePaneVrBakeTakeCounts(ConcourseVrBakeCount* out, uint32_t cap) noexcept {
    const uint32_t n = g_vrCountOutN < cap ? g_vrCountOutN : cap;
    for (uint32_t i = 0; i < n; ++i) out[i] = g_vrCountOut[i];
    g_vrCountOutN = 0;
    return n;
}
ConcourseVrBakeStats concoursePaneVrBakeStats() noexcept {
    ConcourseVrBakeStats s;
    s.toneDraws = g_vrToneDraws.load(std::memory_order_relaxed);
    for (int e = 0; e < 2; ++e) {
        s.dispatches[e] = g_vrDisp[e].load(std::memory_order_relaxed);
        s.snaps[e] = g_vrSnaps[e].load(std::memory_order_relaxed);
    }
    s.noRows = g_vrNoRows.load(std::memory_order_relaxed);
    s.noDepth = g_vrNoDepth.load(std::memory_order_relaxed);
    s.refused = g_vrRefused.load(std::memory_order_relaxed);
    s.snapNoCb = g_vrSnapNoCb.load(std::memory_order_relaxed);
    s.lost = g_vrLost.load(std::memory_order_relaxed);
    for (int k = 0; k < 4; ++k) s.firstOrder[k] = g_vrLastOrder[k];
    return s;
}

void concoursePaneTick(ID3D11Device* device, uint32_t frame, uint64_t nowMs) noexcept {
    g_lastTickMs = nowMs;
    logWithheld(nowMs);
    if (device != g_device) {
        releaseStates();
        g_poolMade = false;   // the old queries belong to the old device and are left alone
        for (size_t i = 0; i < kPool; ++i) g_pool[i] = nullptr;
        g_device = device;
        ID3D11DeviceContext* imm = nullptr;
        if (device) device->GetImmediateContext(&imm);
        g_immediate.store(imm, std::memory_order_relaxed);
        if (imm) imm->Release();
    }
    if (device) updateGlowCb(device);   // after the device check: the buffer belongs to g_device
    const uint32_t prevFrame = g_frameNow.exchange(frame, std::memory_order_relaxed);
    pollConfig(nowMs);
    const JournalRawStatus raw = journalRawStatus();
    const bool known = journalOnFootKnown() || (runtimeFlatProfile() && raw.gameplay);
    g_social = cc::concourseSocialOnFoot(journalStatusFlags2(), known, raw.gameplay);
    if (g_keyMode && g_keyMode->pressed()) {
        const uint32_t cur = g_mode.load(std::memory_order_relaxed);
        // mirror-lit, mirror-bright and mirror-soft are set by the ini only: the press steps mirror-backdrop, mirror-steady, mirror-brightest, mirror, tint, off.
        const uint32_t next = cur == kModeBackdrop ? kModeSteady : cur == kModeSteady ? kModeBrightest : cur == kModeBrightest ? kModeMirror : cur == kModeMirror ? kModeTint
                              : cur == kModeTint ? kModeOff : kModeBackdrop;
        setMode(next, g_social ? "hotkey.concourse_pane_mode" : "hotkey.concourse_pane_mode; it applies on foot in social space", true);
    }
    if (g_keyParallax && g_keyParallax->pressed()) {
        const uint32_t next = cc::parallaxNext(g_bdParallax.load(std::memory_order_relaxed));
        g_bdParallax.store(next, std::memory_order_relaxed);
        Log::get().note("concourse backdrop: distance mode %s, radius %.0f m (hotkey.concourse_backdrop_parallax)", cc::parallaxName(next), static_cast<double>(g_bdRadius.load(std::memory_order_relaxed)));
        queueToast(next == cc::kParallaxNone ? "Windows: distance off" : next == cc::kParallaxSphere ? "Windows: distance by sphere" : "Windows: distance by depth");
    }
    if (g_keyLightTest && g_keyLightTest->pressed()) {
        const unsigned long long hits = static_cast<unsigned long long>(g_lightTestHits.exchange(0, std::memory_order_relaxed));
        g_lightTestStep = cc::lightTestNext(g_lightTestStep);
        const uint64_t ps = cc::lightTestShaderAt(g_lightTestStep);
        g_lightTestPs.store(ps, std::memory_order_relaxed);
        char text[64];
        if (ps == cc::kLightTestAllLights) std::snprintf(text, sizeof(text), "Light test %u of %u: every light off", g_lightTestStep, cc::kLightTestSteps - 1);
        else if (ps != 0) std::snprintf(text, sizeof(text), "Light test %u of %u: %08X off", g_lightTestStep, cc::kLightTestSteps - 1, static_cast<unsigned>(ps >> 32));
        else std::snprintf(text, sizeof(text), "Light test: all on");
        Log::get().note("concourse light test: %s (the step before left out %llu draws; 0 says it matched nothing)", text, hits);
        queueToast(text);
    }
    if (g_keyCapture && g_keyCapture->pressed()) {
        g_surveyRequested.store(true, std::memory_order_relaxed);
        if (!g_social) Log::get().note("concourse pane capture: refused: not on foot in social space (Status.json Flags2 bits 0 and 14, gameplay seen)");
        else if (!device) Log::get().note("concourse pane capture: refused: no device");
        else if (g_capState != CapState::kIdle) Log::get().note("concourse pane capture: refused: a capture is still open");
        else g_capPressPending = true;
    }
    // The capture arms one full frame: the open frame is the one between this tick and the next.
    if (g_capState == CapState::kIdle && g_capPressPending) {
        g_capPressPending = false;
        if (makePool(device)) {
            {
                std::lock_guard<std::mutex> lock(g_capMutex);
                g_capN = 0;
                g_capDropped = 0;
            }
            g_capState = CapState::kOpen;
            g_capFrame = frame;
            g_capturing.store(true, std::memory_order_release);
            Log::get().note("concourse pane capture: armed at frame %u: the next whole frame's blended draws (RT0 blending, a colour target, viewport at least 1000 wide) are measured", frame);
        }
    } else if (g_capState == CapState::kOpen && frame != g_capFrame) {
        g_capturing.store(false, std::memory_order_release);
        g_capState = CapState::kReading;
        g_capFrame = frame;
    } else if (g_capState == CapState::kReading) {
        pollResults(device);
        const bool done = allArrived();
        if (done || frame - g_capFrame >= 60) {
            if (!done) Log::get().note("concourse pane capture: %u frames passed with results still pending; reporting what arrived", frame - g_capFrame);
            reportCapture();
            g_capState = CapState::kIdle;
        }
    }
    if (device) {
        const bool wantStates = g_feedTreat.load(std::memory_order_relaxed);   // mode off too: the far-limited glows use the mark whatever the mode
        if (wantStates) {
            makeStates(device);
            if (g_statesReady.load(std::memory_order_acquire)) {
                ensureMarkVs(device);   // a change of diag.concourse_mirror_beyond compiles the vertex shader again
                g_markReady.store(g_rsMark.load(std::memory_order_acquire) != nullptr && g_vsMark.load(std::memory_order_acquire) != nullptr,
                                  std::memory_order_release);
                if (makeTests(device)) g_testsOn.store(true, std::memory_order_release);
            }
        }
        pollTests(device, frame);
        {   // the missing share over the last kMissWindowFrames frames (the tick's thread owns the entries)
            double sum = 0.0;
            if (g_mode.load(std::memory_order_relaxed) == kModeBackdrop && g_missPixels > 0.0)
                for (const MissEntry& e : g_miss)
                    if (e.missing > 0.0 && frame - e.frame < kMissWindowFrames) sum += e.missing;
            g_missShare.store(g_missPixels > 0.0 ? static_cast<float>(sum / g_missPixels) : 0.0f, std::memory_order_relaxed);
        }
        const bool measuring = g_feedMeasure.load(std::memory_order_relaxed);
        if (measuring && makeSlots(device)) g_shareOn.store(true, std::memory_order_release);
        if (!measuring) g_shareOn.store(false, std::memory_order_release);
        pollShares(device, prevFrame, frame, measuring, nowMs);
        readMaterials(device, frame);
    }
    backdropTick(device, frame);
    bakeTick(device, nowMs);
    vrBakeTick(device, frame);   // TEMPORARY (the VR bake): its prepare and read-backs; nothing while it was never armed
    if (g_vrDiffOn.load(std::memory_order_acquire)) vrDrawDiffTick(frame);   // TEMPORARY (H62 instrument): a finished face's draw diff, sorted and logged here, not in the draw hook
    // A backstop: a bake takes about 110 ms, and every end of one clears the people's flag. A bake ended some other way (its mode stepped by the key mid-bake) would leave the
    // people out for good, so past 2 s they come back, and the log says so.
    if (g_bakePeopleOff.load(std::memory_order_acquire) && nowMs - g_bakeHoldStartMs > 2000 && g_bakePeopleOff.exchange(false, std::memory_order_acq_rel))
        Log::get().note("concourse bake: the people's draws are issued again: the bake did not end within 2 s of its begin (%llu left out)",
                        static_cast<unsigned long long>(g_bakePeopleLeft.load(std::memory_order_relaxed)));
    updateArmed();
}

// ---- The backdrop capture's tick and its API (PROTOTYPE) -------------------------------------------------------------------------------------------------
void concoursePaneSetBackdropMode(const char* why) noexcept { setMode(kModeBackdrop, why, true); }

void concoursePaneArmCapture(bool toast) noexcept {
    g_bdToast.store(toast, std::memory_order_relaxed);
    g_bdPhase.store(static_cast<uint8_t>(ConcourseBackdropPhase::kIdle), std::memory_order_relaxed);
    g_bdArmRequested.store(true, std::memory_order_release);
}

ConcourseBackdropStatus concoursePaneBackdropStatus() noexcept {
    ConcourseBackdropStatus s;
    s.phase = static_cast<ConcourseBackdropPhase>(g_bdPhase.load(std::memory_order_relaxed));
    s.sceneCopied = g_bdSceneCopied.load(std::memory_order_relaxed);
    s.paneDraws = g_bdPaneDraws.load(std::memory_order_relaxed);
    s.originSet = g_bdOriginSetNow.load(std::memory_order_relaxed);
    s.captures = g_bdCaptures.load(std::memory_order_relaxed);
    s.framesWaited = g_bdFramesWaited;
    std::lock_guard<std::mutex> lock(g_bdMutex);
    s.sceneW = g_bdSceneW;
    s.sceneH = g_bdSceneH;
    std::snprintf(s.sceneFormat, sizeof(s.sceneFormat), "%s", g_bdSceneFmtText);
    std::snprintf(s.refusal, sizeof(s.refusal), "%s", g_bdRefusal);
    return s;
}

void concoursePaneBackdropEnd() noexcept {
    g_bdArmRequested.store(false, std::memory_order_relaxed);
    g_bdArmed.store(false, std::memory_order_release);
    g_bdPhase.store(static_cast<uint8_t>(ConcourseBackdropPhase::kIdle), std::memory_order_relaxed);
    updateArmed();
}

bool concoursePaneBackdropAuto() noexcept { return g_bdAutoOn.load(std::memory_order_relaxed) && g_mode.load(std::memory_order_relaxed) == kModeBackdrop; }
bool concoursePaneIsBackdropMode() noexcept { return g_mode.load(std::memory_order_relaxed) == kModeBackdrop; }
ConcourseBackdropMissing concoursePaneBackdropMissing() noexcept {
    ConcourseBackdropMissing m;
    m.share = g_missShare.load(std::memory_order_relaxed);
    m.probesIssued = g_probeIssued.load(std::memory_order_relaxed);
    m.probesRead = g_probeRead.load(std::memory_order_relaxed);
    m.cubeValid = g_bdCubeValid.load(std::memory_order_relaxed);
    return m;
}

void concoursePaneQueueToast(const char* text) noexcept {
    if (text) queueToast(text);
}

bool concoursePaneTakeToast(char* out, size_t size) noexcept {
    if (!out || size == 0) return false;
    std::lock_guard<std::mutex> lock(g_toastMutex);
    if (!g_toastDue) return false;
    g_toastDue = false;
    std::snprintf(out, size, "%s", g_toastText);
    return true;
}

}  // namespace edvr
