#pragma once
// The concourse's glass panes (docs\concourse-cull-2026-11-05.md, "The panes"). TEMPORARY. While the concourse cull is wanted (key or override on, on foot, in a
// station's social space, the anchor seen), the station's glass family is treated, and the outside is not drawn while the cull is on: the plume and its twin, which
// paint over the panes, and the smoke, ship lasers, engine trails, atmosphere and planet (concourse_cull_core.h, kWithheldPs and kWithheldVs), whatever the mode. The glass family is named by its vertex shader (concourse_cull_core.h, kPaneFamilyVs) and a pixel shader: kPaneFamilyPs, or any pixel
// shader whose layout is the glass's (concourse_glass_layout.h; the game builds one per Material Quality level, learned as each is created by concoursePaneNoteShader), any index count;
// diag.concourse_panes adds entries. The game's own glass draw is always issued, unchanged; the treatment is a SECOND draw of
// the same arguments, right after it, that paints the pane over. The modes (diag.concourse_pane_mode, F4):
//   mirror       the second draw is the game's shader with three stand-in textures (t8, t9, t10), opaque (RGB only), writing depth
//   mirror-soft  mirror, and the pixel shader's constant buffer 2 replaced by EDVR's own (roughness scaled to 0.54)
//   tint         the second draw is a white pixel shader and a blend-factor colour per index count, opaque, so each window type can be told apart by eye
//   off          the game's own glass, unchanged
//   mirror-backdrop  mirror-steady's one opaque pass, with a pixel shader that samples a cube of view directions captured on a key press (PROTOTYPE, the end of this header)
// While the cull is on (the plain phase), the second draw is limited to the pixels whose scene behind the pane is farther than diag.concourse_mirror_beyond
// metres: a mark pass (one fullscreen triangle at that depth, writing stencil bit 0x40 where the scene is farther) runs once a frame before the first mirror
// draw, and the mirror draw tests that bit. Glass with a wall behind it (a terminal screen) is left as the game drew it. Each pane's side, learned by two
// occlusion queries (the game's glass samples, the mirror's samples), decides when the cull is lifted (the windows clearing together).
// Family draws are also measured: an occlusion query around the game's draw gives the share of the viewport it covers, whatever the mode.
//
// The draw thunks of vscreen.cpp (hookedDraw and hookedDrawInstanced for what is withheld; hookedDrawIndexed and hookedDrawIndexedInstanced for that and the
// mirror; the flat branch and the VR path of each) build one ConcoursePaneScope around the
// real draw call, skip the real call when withheld() says so, and issue the second draw when nextMirrorPass() says so. With nothing armed the scope is one
// relaxed atomic load, and nextMirrorPass() is a check of one member that is false for every draw that is not the family's.
#include <d3d11_1.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace edvr {

namespace detail {
extern std::atomic<bool> g_concoursePaneArmed;   // a capture frame is open, or panes are treated, measured or withheld (written by the tick and the feed)
}

// The game's own entry points, past EDVR's thunks (so the binding shadow and the issue epoch do not move; the state is put back before anything else looks).
// The input layout, topology and rasteriser state setters are not hooked at all, so the context's own methods are used for those.
using ConcoursePaneSetBlendFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11BlendState*, const FLOAT[4], UINT);
using ConcoursePaneSetPsFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11PixelShader*, ID3D11ClassInstance* const*, UINT);
using ConcoursePaneSetVsFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11VertexShader*, ID3D11ClassInstance* const*, UINT);
using ConcoursePaneSetSrvFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11ShaderResourceView* const*);
using ConcoursePaneSetDepthFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11DepthStencilState*, UINT);
using ConcoursePaneDrawFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
struct ConcoursePaneCalls {
    ConcoursePaneSetBlendFn setBlend;
    ConcoursePaneSetPsFn setPs;
    ConcoursePaneSetSrvFn setSrv;
    ConcoursePaneSetDepthFn setDepth;
    ConcoursePaneSetVsFn setVs;
    ConcoursePaneDrawFn draw;
};

class ConcoursePaneScope {
public:
    // kind: the draw call that built it, for the VR bake's draw diff: D Draw, I DrawIndexed, N DrawInstanced, X DrawIndexedInstanced, Y DrawInstancedIndirect, Z
    // DrawIndexedInstancedIndirect, A DrawAuto. The indirect kinds pass 0 for the counts (they are on the GPU), and never ask for a mirror pass.
    ConcoursePaneScope(ID3D11DeviceContext* ctx, UINT indexCount, UINT startIndex, INT baseVertex, char kind, const ConcoursePaneCalls& calls) noexcept {
        if (detail::g_concoursePaneArmed.load(std::memory_order_relaxed)) begin(ctx, indexCount, startIndex, baseVertex, kind, calls);
    }
    ~ConcoursePaneScope() {
        if (flags_ != 0) end();
    }
    ConcoursePaneScope(const ConcoursePaneScope&) = delete;
    ConcoursePaneScope& operator=(const ConcoursePaneScope&) = delete;

    // True for a draw that is not issued (an outside draw while the cull is on): the site skips only the real draw call.
    bool withheld() const noexcept { return withheld_; }
    // Called right after the game's own draw was issued: true when the site is to issue the same draw again (the mirror), with the replacement state bound. The
    // state is put back when the scope ends. False for every draw that is not a family draw.
    // Asked after the game's own draw and after each pass: true means issue the same draw again. First the mirror, then, in the lit modes, its glow.
    bool nextMirrorPass() noexcept { return family_ && nextMirrorPassSlow(); }

private:
    void begin(ID3D11DeviceContext* ctx, UINT indexCount, UINT startIndex, INT baseVertex, char kind, const ConcoursePaneCalls& calls) noexcept;
    void beginBody(ID3D11DeviceContext* ctx, UINT indexCount, UINT startIndex, INT baseVertex, const ConcoursePaneCalls& calls) noexcept;
    bool nextMirrorPassSlow() noexcept;
    void bindPsCb(ID3D11Buffer* own) noexcept;   // constant buffer 2 of the pixel stage, the previous one kept for end()
    bool beginMirrorSlow() noexcept;
    void closeGameQueries() noexcept;   // the share query and the side test's first query end with the game's own draw, before any pass of ours
    bool beginCapturePass() noexcept;   // PROTOTYPE (backdrop): binds the capture pass; false leaves the context as it was
    void endCapturePass() noexcept;     // puts the capture pass's state back (the UAV, the pixel shader, t8, the blend and depth states)
    bool beginProbePass() noexcept;     // PROTOTYPE (automatic capture): the colourless probe pass with its own occlusion query; false leaves the context as it was
    void endProbePass() noexcept;       // ends the probe's query and puts its state back (all but the constant buffer, which the display pass keeps and end() restores)
    void end() noexcept;

    // The three members the destructor and the inline accessors read, and so the only ones the constructor sets when nothing is armed (the unarmed draw, 13-17k a frame, pays
    // for these three stores and not for ~330 bytes of defaults). Every other member is set by initCold() as the first act of begin(), before anything reads it: begin() runs
    // whenever the scope is armed, and no member below is read unless begin() ran (end() only when flags_ != 0, nextMirrorPassSlow() only when family_, both set by begin()).
    uint32_t flags_ = 0;   // which of the things to put back or close are open
    bool family_ = false;   // the draw is a pane of the list
    bool withheld_ = false;
    // /GS: MSVC guards an array of a non-pointer type with more than two elements, and a struct of three or more such values, so a scope holding FLOAT[4] (or a struct of four floats)
    // made every draw thunk that builds one set up a security cookie and check it, armed or not (measured on a replica, 2026-11-08). The blend factors are therefore flat members
    // here, and the code that hands them to D3D copies them through a local array. The pointer arrays (the saved t8..t10) are not guarded.
    void initCold() noexcept;   // every member below, to the value it had as a default member initialiser
    ID3D11DeviceContext* ctx_;
    const ConcoursePaneCalls* calls_;
    // The plan begin() decides and beginMirrorSlow() carries out.
    bool planMirror_;
    bool planMark_;     // the mirror draw is limited to the marked pixels
    bool planOpaque_;   // false: the fade (blend factor opacity)
    bool planTint_;
    bool planSoft_;     // mirror-soft: EDVR's constant buffer 2
    float planGlow_;    // the lit modes: the glow pass's blend factor (0: no glow pass)
    bool planSteady_;   // the steady modes: the glow pass is a flat colour, with no light or shadow in it
    bool mirrorBound_;  // the mirror pass has been bound
    bool planBackdrop_; // mirror-backdrop: the steady pass with the cube-sampling pixel shader
    bool planCapture_;  // an armed backdrop capture: this exterior pane gets a capture pass before its display pass
    bool capDone_;      // the capture pass has been asked for
    bool capBound_;     // the capture pass's state is bound (endCapturePass puts it back)
    bool planProbe_;    // a test frame in mirror-backdrop with the mark: a probe pass runs before the display pass
    bool probeDone_;    // the probe pass has been asked for
    bool probeBound_;   // the probe pass's state is bound and its query is open (endProbePass closes and restores; the capture pass's save fields are used, the two never share a scope)
    ID3D11SamplerState* probePrevSampler_;
    ID3D11BlendState* capPrevBs_;
    FLOAT capPrevFactor0_, capPrevFactor1_, capPrevFactor2_, capPrevFactor3_;   // the blend factor as four members, not FLOAT[4]: see the note above
    UINT capPrevMask_;
    ID3D11PixelShader* capPrevPs_;
    ID3D11ShaderResourceView* capPrevSrv_[3];
    ID3D11DepthStencilState* capPrevDs_;
    UINT capPrevRef_;
    ID3D11SamplerState* prevSampler_;   // the display pass's sampler slot, the game's kept for end()
    FLOAT planFactor0_, planFactor1_, planFactor2_, planFactor3_;
    uint32_t keyN_, keyStart_;
    int32_t keyBase_;
    int query_;
    int capQuery_;
    int testSlot_;
    ID3D11BlendState* prevBs_;
    FLOAT prevFactor0_, prevFactor1_, prevFactor2_, prevFactor3_;
    UINT prevMask_;
    ID3D11PixelShader* prevPs_;
    ID3D11ShaderResourceView* prevSrv_[3];
    ID3D11DepthStencilState* prevDs_;
    UINT prevStencilRef_;
    ID3D11DeviceContext1* ctx1_;
    ID3D11Buffer* prevCb_;
    UINT prevCbFirst_, prevCbNum_;
};

// What the concourse tick tells the panes each frame.
struct ConcoursePaneFeed {
    bool measure = false;       // the gate holds: family draws are measured
    bool treat = false;         // the cull is wanted: family draws are treated by the mode
    bool withhold = false;      // the cull is on and the phase is the plain mirror phase: the outside draws are not issued, the mirror is marked
    bool effects = false;       // the outside effects are not issued although the phase is not the plain one (the backdrop capture's lift); withhold implies it
    bool sealed = false;        // the hangar is sealed: the families that can only be outside are not issued (concourse_cull_core.h, sealedToo)
    bool realSky = false;       // this room's windows show the real sky (cc::RoomLayout::realSky): the planet, its atmosphere and the sun are issued with the cull on (skySide)
    float bayOpacity = 1.0f;    // the opacity every outside pane has while the cull is lifted, 0 to 1
    float up[3] = {};           // the anchor's local up axis in world (the steady glow's sky test); zero when not valid
    bool upValid = false;
    uint32_t bayN = 0;          // this room's bay window's index count (cc::RoomLayout); 0 is the large station's
    float backdropRadiusM = 0.0f;   // the sphere this room's bake is shown on, unless diag.concourse_backdrop_radius says; 0 is the large station's
};
void concoursePaneSetFeed(const ConcoursePaneFeed& feed) noexcept;

// The latest completed frame's shares of the viewport: the total of the outside glass in the main view, and per index count (family draws only).
struct ConcoursePaneShares {
    bool valid = false;     // a reading from the last 250 ms
    uint32_t seq = 0;       // moves with each new frame read
    float exterior = -1.0f; // the total share of the main view that all the outside glass fills (cc::exteriorGlassShare, each pane weighted by its entry scale); 0 when none was drawn; negative when not valid
    uint32_t count = 0;
    uint32_t n[8] = {};
    float share[8] = {};    // 0 to 1
};
ConcoursePaneShares concoursePaneShares() noexcept;

// The frame tick, once a frame at the Present boundary (device_hook.cpp presentFrameBoundary): the keys, the capture's state machine, the mode and the list,
// the measurement's queries and the material read-back, the armed flag. `device` is the game's device; nowMs is GetTickCount64().
void concoursePaneTick(ID3D11Device* device, uint32_t frame, uint64_t nowMs) noexcept;

// Treated draws (mirror draws issued) and withheld draws since the last call (the cull's 5 s summary reports them, so "matched nothing" is visible).
uint64_t concoursePaneTakeTreated() noexcept;
uint64_t concoursePaneTakeWithheld() noexcept;
// An F3 press since the last call: the cull runs its names survey too, so the log names what is left in view.
bool concoursePaneTakeSurveyRequest() noexcept;
// The mirror's own counters: mark passes run and refused (no depth view, no stencil, a geometry stage bound, states missing) and side tests read since the last
// call; the side table as it stands now. All zero when the mirror never ran.
struct ConcoursePaneMirrorStats {
    uint64_t markPasses = 0, markRefused = 0, tests = 0;
    uint32_t known = 0, exterior = 0, dropped = 0;
    uint32_t shareSamplesDropped = 0;   // glass draws measured since the last call that did not fit a frame's sample table
};
ConcoursePaneMirrorStats concoursePaneTakeMirrorStats() noexcept;
// The mode ("mirror-backdrop", "mirror-steady", "mirror", "mirror-soft", "tint", "off") and the number of panes in the list, for the summary.
const char* concoursePaneModeName() noexcept;
uint32_t concoursePaneSlotCount() noexcept;
void concoursePaneNoteShader(uint64_t hash, const void* bytecode, size_t len) noexcept;   // a pixel shader was created: one of the glass's layout joins the glass family (any thread)
// Panes are being treated: the feed says treat, the mode is not off and the replacement states are made.
bool concoursePaneTreating() noexcept;

// ---- The backdrop capture (PROTOTYPE on the concourse arc, docs\concourse-cull-2026-11-05.md "The window plan") ----------------------------------------------
// The cull's tick lifts the cull for kBackdropLiftMs, then calls concoursePaneArmCapture(). In the next frame (up to kBackdropWaitFrames) the first pane draw copies the
// bound render target, and each exterior pane gets a capture pass that writes the scene behind it into the cube's back texture; the tick after copies it to the front
// texture the display pass samples. The status says what happened, so the cull's tick can log it once and end the lift.
enum class ConcourseBackdropPhase : uint8_t { kIdle, kArmed, kDone, kRefused, kEmpty };
struct ConcourseBackdropStatus {
    ConcourseBackdropPhase phase = ConcourseBackdropPhase::kIdle;
    bool sceneCopied = false;        // a frame's first pane draw copied the render target
    uint32_t sceneW = 0, sceneH = 0;
    char sceneFormat[28] = {};
    bool originSet = false;          // this capture set the cube's origin (false: the visit's earlier origin was kept)
    uint32_t captures = 0;           // captures that have gone into this cube, this one included once done
    uint32_t paneDraws = 0;          // capture passes issued
    uint32_t framesWaited = 0;
    char refusal[200] = {};          // kRefused: why
};
void concoursePaneSetBackdropMode(const char* why) noexcept;   // F6: the mode becomes mirror-backdrop (a toast says so)
void concoursePaneArmCapture(bool toast) noexcept;               // the capture is armed from the next tick; the status follows; `toast`: its result is said on screen (F6's, not an automatic one's)
ConcourseBackdropStatus concoursePaneBackdropStatus() noexcept;
// Automatic capture: the pane mode is mirror-backdrop and a hotkey (F4 to it, or F6) turned automatic capture on.
bool concoursePaneBackdropAuto() noexcept;
bool concoursePaneIsBackdropMode() noexcept;
// What the probes measured: the share of the viewport the cube cannot show through the exterior panes, over the last 8 frames (0 when nothing was measured), and the probe
// counters since the process began (issued by the draw threads, read by the tick), so a probe that never ran, or never returned, can be told from a share that is just low.
struct ConcourseBackdropMissing {
    float share = 0.0f;
    uint64_t probesIssued = 0, probesRead = 0;
    bool cubeValid = false;
};
ConcourseBackdropMissing concoursePaneBackdropMissing() noexcept;
void concoursePaneBackdropEnd() noexcept;                      // the cull's tick has read the result (or gives up): back to idle, and an open capture is closed
// A toast text, once per hotkey change of the mode and per capture result ("Windows: ..."); false when none is waiting. The tick hands it to menuShowNotice.
bool concoursePaneTakeToast(char* out, size_t size) noexcept;
void concoursePaneQueueToast(const char* text) noexcept;   // TEMPORARY: the camera test's key (optimized_station.cpp) shows its mode through the same toast

// ---- The bake (TEMPORARY, docs\concourse-cull-2026-11-05.md "Flight 24 results"): the cube filled at arrival from the room's centre, in the frames the camera is moved for ------
// Arm when a ride up is armed: the cubes and the shader are made and cleared in the tick, while the cabin is shut, and the back buffer is kept each Present. Begin when the anchor is first
// seen: the kept image is shown from then on, and each frame the resolve sees at the centre goes into the cube. Finish when the frames are taken: the cube is published and the pane
// mode becomes the captured view; the hold ends when the resolve sees the camera back. Cancel ends it with nothing published.
bool concoursePaneBakeDone() noexcept;                                      // the windows have a bake to show
bool concoursePaneBakeWouldBeSharper(uint64_t nowMs) noexcept;              // the render size has risen past the bake's and held steady
void concoursePaneBakeRelease() noexcept;                                   // leaving the station: the cube goes at the next tick, and the next arrival bakes afresh
void concoursePaneBakeArm(bool on) noexcept;
// false when the cube was not ready: the frames still move, nothing is captured or held. nearM is the frames' near plane (for the log); cutM is the room's cut: a point nearer the
// centre than it is not written. While it captures, the people's draws are not issued (cc::bakePeopleVs).
bool concoursePaneBakeBegin(const float centre[3], float nearM, float cutM) noexcept;
void concoursePaneBakeExpect(const float (*forward)[3], uint32_t count, uint32_t mask) noexcept;   // where each direction looks, and which are wanted (cc::kBakeDirs of them)
uint32_t concoursePaneBakeCaptured() noexcept;                              // the directions that have reached the resolve: all of the wanted ones are the end of the bake
void concoursePaneBakeFinish() noexcept;
void concoursePaneBakeCancel() noexcept;
// From the flat runtime, with what the resolve is about to read: the scene's colour and depth before the visor and the HUD, and b1[270..275]. One relaxed load when no bake runs.
void concoursePaneBakeFrame(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* color, ID3D11ShaderResourceView* depth, const float camera[6][4], uint32_t width, uint32_t height) noexcept;
// From Present, just before the real one: keeps the back buffer, or puts the kept image back. One relaxed load when no bake runs.
void concoursePaneBakePresent(IDXGISwapChain* swap) noexcept;

// ---- The VR bake (TEMPORARY, docs\concourse-cull-2026-11-05.md "Flight 51 results"): the same cube, filled from VR's own frames --------------------------------------------
// The cull's tick (optimized_station.cpp) aims the eye cameras through the eye stub; this half copies each eye's camera rows (VS b1 270..279) on the GPU at its first
// recognised scene draw of a frame (SnapDraw, from vscreen.cpp's VR draw path), and at each eye's admitted tonemap draw (ToneDraw, the VR eye draw path, eye by the draw's
// order in the frame as the proving build found it) dispatches the VR variant of the bake's shader with that eye's H (PS t1), its scene depth (depth_probe.h) and its rows.
// The shader inverts the rows on the GPU, takes a texel only on the face the frame looks along (its own square) and inside the frame's clip, and counts per eye: the texels on
// the face, those in this eye's view, those written (past the room's cut) and those in either eye's view. The counts and the rows are read back a few frames later (the
// pane tick) for the cull's tick. The flat bake's shader, constants and callers are untouched; nothing here runs in the flat profile (the hooks are armed only by the VR tick).
namespace detail {
extern std::atomic<uint32_t> g_concourseVrBakeHooks;   // bit 0: the tone draw is handed over; bit 1: the scene draws are
}
inline bool concourseVrBakeToneArmed() noexcept { return (detail::g_concourseVrBakeHooks.load(std::memory_order_relaxed) & 1u) != 0; }
inline bool concourseVrBakeSnapArmed() noexcept { return (detail::g_concourseVrBakeHooks.load(std::memory_order_relaxed) & 2u) != 0; }
void concoursePaneVrBakeToneDraw(ID3D11DeviceContext* ctx, char kind, uint32_t count, uint32_t instances, uint32_t startInstance) noexcept;
void concoursePaneVrBakeSnapDraw(ID3D11DeviceContext* ctx) noexcept;
// The tone draw is looked for (no capture) while `on`; once one is admitted with an eye and its depth, Seen says so for the session, with what it was.
struct ConcourseVrToneSeen {
    uint32_t hdrW = 0, hdrH = 0, hdrFmt = 0, depthFmt = 0;
};
void concoursePaneVrBakeDiscover(bool on) noexcept;
bool concoursePaneVrToneSeen(ConcourseVrToneSeen* what) noexcept;
void concoursePaneVrBakeArm(bool on) noexcept;    // the cube and the VR shader are made and the cube cleared (the pane tick); no desktop hold
bool concoursePaneVrBakeBegin() noexcept;         // the rows are copied from the next frame (the world check); false (with a line) when the cube or the shader is not ready
// Capture from the next frame: the cube's origin (render world), each face's right, up and forward (faces[f][0..8]), the faces wanted, the frames' near plane (for the log)
// and the room's cut (nearer the origin is not written). The people's draws are left out from now (cc::bakePeopleVs), as in flat.
void concoursePaneVrBakeCapture(const float origin[3], const float faces[5][9], uint32_t mask, float nearM, float cutM) noexcept;
bool concoursePaneVrBakeEnd(bool publish, const char* line) noexcept;   // publish: the cube becomes the windows' view, as flat's finish; `line` is the tick's finish text; true when it published
// TEMPORARY (H58-H61 instrument, Flight 53): which face and dwell a frame was aimed at, and whether it writes the cube. A frame with no entry, or not a write frame, only counts.
void concoursePaneVrBakeAimFrame(uint32_t frame, int face, uint32_t dwell, bool write) noexcept;
struct ConcourseVrBakeRows {
    uint32_t frame = 0;
    int eye = -1;
    float rows[10][4] = {};   // b1 270..279
};
struct ConcourseVrBakeCount {
    uint32_t frame = 0;
    bool dispatched[2] = {}, both[2] = {};   // both: the other eye's rows were this frame's (the union count is good)
    int face[2] = {-1, -1};
    uint32_t status[2] = {}, onFace[2] = {}, inView[2] = {}, written[2] = {}, eitherView[2] = {};
    // TEMPORARY (H58-H61 instrument, Flight 53): over the texels on the face and in this eye's view.
    uint32_t pastCut[2] = {}, nothingDrawn[2] = {};
    uint32_t cutBy[2][4] = {};     // cut (not past the room's cut), by the eye's distance t: <5, 5-20, 20-50, >=50
    uint32_t drawnBy[2][5] = {};   // drawn (z > 0), by t: <50, 50-200, 200-1000, 1000-4000, >=4000
};
uint32_t concoursePaneVrBakeTakeRows(ConcourseVrBakeRows* out, uint32_t cap) noexcept;
uint32_t concoursePaneVrBakeTakeCounts(ConcourseVrBakeCount* out, uint32_t cap) noexcept;
struct ConcourseVrBakeStats {
    uint64_t toneDraws = 0, dispatches[2] = {}, noRows = 0, noDepth = 0, refused = 0, snaps[2] = {}, snapNoCb = 0, lost = 0;
    uint32_t firstOrder[4] = {};   // the last captured frame's order: eye 0's rows, eye 0's tone, eye 1's rows, eye 1's tone (a sequence number each; 0 not seen)
};
ConcourseVrBakeStats concoursePaneVrBakeStats() noexcept;

}  // namespace edvr
