// The optimized station (fix.optimized_stations): inside a station, skip rendering what cannot be seen. While the ship sits in a sealed hangar
// (docked, the pad lowered and still, the hatch closed), everything outside the hangar is skipped, including the sun's light that leaks into it.
// The rules and their geometry are in optimized_station_core.h. The seal, the release and the status lines are here. The engine hooks it uses:
// store_hook.cpp (the store's record judgement, path A), kinematic_eval_hook.cpp (the producer-B and rig vetoes), vscreen.cpp (the sun pair).
#include "optimized_station.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "../common/config.h"
#include "../common/frame_flag.h"      // TEMPORARY (the concourse VR probe): requestSubmitHold
#include "../common/native_frame.h"    // nativeFrameHeldLatches
#include "../common/hotkey.h"
#include "../common/log.h"
#include "../common/runtime_profile.h"
#include "concourse_cull_core.h"
#include "concourse_pane.h"
#include "concourse_vr_bake_core.h"   // TEMPORARY (the concourse cull's VR bake): the aim's pure half
#include "journal_watch.h"
#include "kinematic_eval_hook.h"
#include "concourse_camera_test.h"   // TEMPORARY (docs\concourse-cull-2026-11-05.md): the camera test
#include "optimized_station_core.h"
#include "station_gather_core.h"
#include "station_lockfree_core.h"
#include "station_pass_core.h"
#include "station_seal_trace.h"
#include "pad_state_probe.h"
#include "store_hook.h"
#include "view_source.h"
#include "binding_shadow.h"        // TEMPORARY (the concourse VR probe): the tone draw's bound shaders and targets
#include "depth_probe.h"
#include "engine_velocity.h"
#include "native_preview_camera.h"
#include "temporal_pass.h"
#include "tonemap_admit.h"
#include "ui_depth.h"
#include "vr_camera_census.h"         // TEMPORARY (the VR bake): nativeTemporalHeadPose and nativeTemporalEyeGeometry, the head and the frusta the game is given

namespace edvr {
namespace detail {
std::atomic<bool> g_sunOff{false};   // the sun skip is on; optimized_station.h reads it inline
std::atomic<bool> g_vrProbeToneArmed{false};   // TEMPORARY (the concourse VR probe): the camera probe's tone-draw lines are wanted; optimized_station.h reads it inline
}  // namespace detail
namespace {

// ---- The frame's state (the store's observations, the own pad, the held cell) --------------------------------------------------------------
std::recursive_mutex g_sealedMutex;        // the lock: the view, the cell, the pads and explicit parts, the rig tables (never held across the game)
std::atomic<uint32_t> g_frameStamp{0};     // advanced at every frame tick: the observer resolves the view once per frame
std::atomic<bool> g_cameraKnown{false};
std::atomic<uint64_t> g_kept{0}, g_keptUnknown{0}, g_culled{0}, g_faults{0};
std::atomic<uint64_t> g_worldSrc[5] = {};               // the named rule's world centre by source (worldOfRecord)
// Built v14.27: the faults by reason (the window's counts; the summary names each). g_winFaults is their sum.
enum FaultReason : size_t { kFaultBucket, kFaultCullRead, kFaultCullRestored, kFaultCullTorn, kFaultTickView, kFaultReasons };
std::atomic<uint64_t> g_winFaultBy[kFaultReasons] = {};
std::atomic<uint64_t> g_winFaults{0};                  // reads or writes that failed in the window
std::atomic<uint64_t> g_winNoCamera{0};                // callbacks that found no camera yet (not faults: the first pass after the gate opens)
void faultNote(FaultReason r) noexcept {
    g_faults.fetch_add(1, std::memory_order_relaxed);
    g_winFaults.fetch_add(1, std::memory_order_relaxed);
    g_winFaultBy[r].fetch_add(1, std::memory_order_relaxed);
}
float g_margin = sealed::kMarginDefaultM;  // the box's margin (fixed; the trial's tuning ladder is removed)
// The sealed rule's sphere radius while no cell is held (the trial's starting radius; the named cell does not use it).
constexpr float kFallbackSphereM = 80.0f;
// Forward: the held cell is localized and the shell picked by setHeldFrame (below), called from the engage.
void setHeldFrame(const sealed::OwnCell& cell, uint32_t frame);
struct RigTally {   // built v14.26: atomic (the callbacks count their vetoes)
    std::atomic<uint64_t> seen{0}, vetoed{0}, mixed{0}, unknown{0}, refresh{0};
    std::atomic<uint64_t> assocSync{0}, assocJob{0}, assocNone{0}, matched{0}, unevaluated{0};
    // Built v14.27: what a rig veto's lookup found in the publication (the window's counts; the cost line names each).
    std::atomic<uint64_t> hitVetoed{0}, hitKept{0}, hitMoved{0}, miss{0}, stale{0}, asked{0};
};
RigTally g_rigTally;
// Built 2026-11-03 (log only): where the sealed pad leaves the pass (station_seal_trace.h PadStage). The 11-03 flight showed the pad's record in the store's pass once
// in 31 frames after a boarding seal. These count the events of the sealed pad's record (the producer veto, the store callback) and of the rig the tick last saw it in
// (the rig veto). Direct atomic adds: only the pad's record and its rig match, a few calls a frame, so no thread-local staging is needed and the trace rows are exact.
std::atomic<uint64_t> g_padStage[sealed::kPadStages];
std::atomic<uintptr_t> g_ownRigA{0};   // the rig the tick last saw the sealed pad's record gathered in (0: not learned yet); the rig veto compares with it
std::atomic<uint64_t> g_padStageSinceSeal[sealed::kPadStages];   // the same events, from the latest seal (reset when a seal is taken): the guard's line
inline void padStageAdd(sealed::PadStage k) noexcept {
    g_padStage[k].fetch_add(1, std::memory_order_relaxed);
    g_padStageSinceSeal[k].fetch_add(1, std::memory_order_relaxed);
}
uintptr_t g_ownRig = 0;                // tick only: the rig the sealed pad's record was last gathered in (applyRigNote); g_ownRigA publishes it to the rig veto
uintptr_t g_ownRigPad = 0;             // the sticky pad g_ownRig was learned for (a different pad clears it)
sealed::OwnRigTrace g_ownRigTrace;     // the tick's view of that rig, written once a tick; the trace row and the 30 s line read it
// Built v14.29: the per-call counters of the callbacks are the thread's own, flushed to the shared atomics once per pass the thread works in (the first
// call it makes in a new pass), so no call writes a cache line another thread touches. The summary reads the shared totals (at most one pass per thread
// behind).
struct TlsCounters {
    uint32_t stamp = 0;   // the pass these counts belong to
    uint64_t callsStore = 0, callsProducer = 0, callsRig = 0;
    uint64_t kept = 0, keptUnknown = 0, keptReach = 0, culled = 0, noCamera = 0;
    uint64_t worldSrc[5] = {};
    uint64_t producerVetoes = 0, rigVetoes = 0;
    uint64_t rigSeen = 0, rigVetoed = 0, rigMixed = 0, rigUnknown = 0, rigRefresh = 0;
    uint64_t rigHitVetoed = 0, rigHitKept = 0, rigHitMoved = 0, rigMiss = 0, rigStale = 0, rigAsked = 0;
    uint32_t sampleStore = 0, sampleProducer = 0, sampleRig = 0;   // the 1-in-32 sampling counters (the thread's own)
    uint64_t concRigSeen = 0, concRigVetoed = 0, concProdVetoed = 0, concProdKept = 0, concStoreCleared = 0;   // built 2026-11-05: the concourse cull's counts
    uint64_t concKept[4] = {};
    uint64_t concRoomProd = 0, concRoomStore = 0;   // the door rule: records the cull's own rule kept and the door rule culled
};
thread_local TlsCounters tlsTc;
void tcFlush(TlsCounters& t) noexcept;
inline TlsCounters& tcFor(uint32_t stamp) noexcept {
    TlsCounters& t = tlsTc;
    if (t.stamp != stamp) {
        tcFlush(t);
        t.stamp = stamp;
    }
    return t;
}
sealed::OwnCell g_heldCell;               // the sealed cell, in the world frame of the frame that reads it (refreshed each frame)
bool g_heldValid = false;                  // sealed
std::atomic<bool> g_sealedFlag{false};     // mirror of g_heldValid for the draw path and the hooks (atomic: read on the render thread)
bool g_engagePending = false;
uintptr_t g_engagePad = 0;     // built v14.20 (review 1): the pad a pending engage was requested for
uintptr_t g_engageShell = 0;   // built v14.20 (review 1): the shell it was requested against (0: none)
uint32_t g_engageTries = 0;
// Built v14.5: the pad's height above the hangar shell, read live each tick (frame-independent). The shell is picked once per pad.
bool g_relOk = false;
sealed::ShipWatch g_shipWatch;   // the own ship's type prefix, read every tick: a change is a ship change (built v14.6)
float g_relM = 0.0f;
bool g_preShellValid = false;
uintptr_t g_preShellAddr = 0;
uintptr_t g_preShellPad = 0;
char g_preShellName[48] = {};
char g_ownPrefix[32] = {};
char g_ownCandidateName[48] = {};
// Built v14.8: the records within kShipRecordsM of the camera by type prefix, this frame (the ship's part count).
struct PrefixTally {
    char p[32] = {};
    uint32_t n = 0;
};
constexpr size_t kPrefixSlots = 24;
PrefixTally g_prefixTally[kPrefixSlots];
size_t g_prefixTallyN = 0;
float g_ownCandidateDist = -1.0f;
// Built v14.27: the pads and explicit parts are windows over the informative passes (station_pass_core.h): a record stays for kSeenKeep passes after the
// pass that saw it, so a pass that carries only part of a frame's callbacks cannot make the own pad absent. g_pads and g_padCount keep their old names.
sealed::SeenWindow<sealed::PadObs, sealed::kPadSlots> g_padWin;
sealed::PadObs (&g_pads)[sealed::kPadSlots] = g_padWin.items;
size_t& g_padCount = g_padWin.n;
sealed::OwnPadSticky g_ownSticky;   // Built v14 (fix 1): the own pad, kept while its record is seen (reset when it is replaced)
sealed::SeenWindow<sealed::ExplicitObs, sealed::kExplicitSlots> g_explicitWin;
sealed::ExplicitObs (&g_explicits)[sealed::kExplicitSlots] = g_explicitWin.items;
size_t& g_explicitCount = g_explicitWin.n;
float g_frameCam[3] = {};
bool g_frameCamValid = false;
sealed::CamHold g_camHold;                  // built v14.27: the camera is held through a failed resolution
// Built v14.27: the pass rules. g_passInformative is this tick's verdict on the drained passes: complete and at least one store callback in them.
// Everything that reads the passes (the promote, the latch samples, the shell motion sample, the rig publication) runs only on an informative tick.
sealed::PassGate g_passGate;
bool g_passInformative = false;
uint64_t g_passCalls = 0;                   // callbacks the last drained passes held
uint64_t g_winPadSeen = 0, g_winPadMissed = 0, g_winPadAbsentWindow = 0;   // informative passes with / without the sticky pad's record, and without it in the window
uint64_t g_winCamHold = 0, g_winCamJump = 0, g_winCtxChange = 0;           // camera resolutions covered by the hold, jumps over kCamJumpM, context changes
uintptr_t g_lastPassCtx = 0;
uintptr_t g_passCtx = 0;                    // the context the last informative pass's busiest thread saw (the camera is resolved from it)
bool g_engagePublished = false;             // built v14.31: the last published view had an engage pending (every pass gathers the near list, the candidate, the tally)
uint32_t g_recordsLast = 0;                 // store callbacks the last drained passes held (the waiting line's evidence)
bool g_stickyPadInPass = false;
uint32_t g_padMissingRun = 0;   // the guard's run (padMissingRun); the tick's, under the state lock
// Built 2026-10-15: the seal trace's pass fields, set by drainGather (the tick): the verdict the drained passes earned, and whether it was thin.
sealed::TraceVerdict g_passVerdictLast = sealed::TraceVerdict::kEmpty;
bool g_passThinLast = false;
constexpr float kCamJumpM = 2.0f;
sealed::OwnCell g_pickCell;                // the frame's pick before the seal (what the seal rule reads)
sealed::OwnCell g_cellPrev;                // the cell the observer judges against this frame
uintptr_t g_explicitPrev[sealed::kExplicitNodes] = {};
size_t g_explicitPrevCount = 0;
uint32_t g_cellStamp = 0;
uint32_t g_heldAbsent = 0, g_otherPicked = 0;
uint32_t g_viewStamp = 0;
bool g_viewReady = false;
uint32_t g_viewTries = 0;
view::Resolved g_view;
// The pad-local frame and the shell (the release by the pad's motion along the shaft).
sealed::OwnCell g_heldLocal;
bool g_heldLocalValid = false;
float g_engageCentre[3] = {};
float g_engageAxes[3][3] = {};
uintptr_t g_shellAddr = 0;
char g_shellName[48] = {};
bool g_shellValid = false;
sealed::ShellMotion g_shellMotion;        // the pad against its shell: the reference is captured live at the first frame held (built v14.4)
uint32_t g_shellSampleStamp = 0xFFFFFFFFu;// the frame stamp of the last motion sample (the tick and the observer both refresh; one sample a frame)
sealed::SealRateGuard g_sealGuard;        // the flicker safety net: seals in a minute (built v14.4)
float g_frameShiftM = 0.0f;
float g_shellDeltaM = 0.0f;
bool g_releaseMotion = false;
// The own record (the own ship's live sphere) and the near list the engage's own-record pick reads.
struct NearRec {
    uintptr_t addr = 0;
    char name[48] = {};
    float centre[3] = {};
    float radius = 0.0f;
    float dist = 0.0f;
};
constexpr float kOwnNearM = 40.0f;
constexpr float kOwnSphereMarginM = 5.0f;
constexpr size_t kNearSlots = 24;
NearRec g_nearCur[kNearSlots], g_nearPrev[kNearSlots];
size_t g_nearCurN = 0, g_nearPrevN = 0;
uintptr_t g_ownRecAddr = 0;
char g_ownRecName[48] = {};
bool g_ownLiveValid = false;
float g_ownLiveCentre[3] = {};
float g_ownLiveRadius = 0.0f;


// The below distance for the held pad's size (the constants in the core; diag keys are not in the product).
float belowFor(sealed::PadSize size) noexcept {
    return sealed::belowForSize(size, sealed::kBelowDefaultsM, false, 0.0f);
}

bool cameraOf(const view::Resolved& v, float out[3]) noexcept { return view::sphereCentre(v, out); }

// The render context's layout (the same offsets as shadow_gate_core.h): views at +0x40, 0x6A0 bytes each; the view count at +0x1A940.
constexpr uintptr_t kCtxCount = 0x1A940;

constexpr uintptr_t kRecCentre = 0x240;     // float3: the world sphere centre

constexpr uintptr_t kRecMask = 0x208;       // u64: the view mask the store wrote

constexpr uintptr_t kRecAdmitted = 0x234;   // u8: the admitted flag the store wrote

constexpr uintptr_t kRecRadius = 0x280;     // float: the sphere radius

constexpr uintptr_t kRecChildren = 0x2A8;   // pointer: the child list, an array of 8-byte record pointers

constexpr uintptr_t kRecChildCount = 0x2B0; // u64: the child count

constexpr uintptr_t kBucketBase = 0x28;     // the bucket's base record

constexpr uintptr_t kRecName = 0x18;        // pointer: the record's node (node+0 is the text pointer)

constexpr size_t kNodeCacheSlots = 16384;    // built v14.31: the node cache (class, flags and name per node, probed within the table's bound)


constexpr uint32_t kViewTries = 64;

// Built v8.4: the centre measurements of the window (under the lock). rec+0x240 exactly zero; the pose unreadable (the fallback); and the distance
// between rec+0x240 and the pose centre for the records where both are readable and rec+0x240 is not zero (reserved at a toggle-on; a write past
// the reserve is not kept, so the hot path never allocates).
constexpr size_t kCentreDistCap = 8192;

uint64_t g_prodCalls = 0, g_prodVetoed = 0, g_prodKept = 0, g_prodStyle = 0, g_prodUnread = 0;


// The class cache (lock-free): a slot per node, a word per slot (sealed_trial_core.h packs it). Written by any thread that runs the store.
sealed::NodeCache<kNodeCacheSlots> g_nodeCache;   // built v14.31: one entry per node and lifecycle generation (a node's name does not change while it lives)
std::atomic<uint64_t> g_nodeNameReads{0}, g_nodeCacheFull{0}, g_nodeReuseCaught{0};
std::atomic<uint64_t> g_winSunDrawsSkipped{0};   // built v14.34: sun-pair draws skipped in the window (the summary line)   // the window's name reads and uncached reads (the cost line)
// Built 2026-10-21: the flat draw path's report of how it carried out those skips (optimizedStationNoteSunDraw); both stay 0 in the VR profile, which never issues the draw.
std::atomic<uint64_t> g_winSunNotIssued{0}, g_winSunMasked{0};
std::atomic<bool> g_flatProfile{false};   // set at install: the 30 s line splits the skips by path only where there is more than one
std::atomic<uint32_t> g_classGen{0};   // moves at every lifecycle point (bumpRigEpoch): every node's class and name are read afresh

// ---- Built 2026-11-05: the concourse cull (docs\concourse-cull-2026-11-05.md). On by itself in flat with the setting (diag.concourse_cull = auto), with the windows' bake; the diag keys and hotkeys beside it are TEMPORARY. ----------
// On foot in a station's social space the records that are not the room are not drawn. The judgement is by the record's name (the node cache's allow flag),
// at the same three levels as the hangar seal: the rig veto, the producer-B veto and the store's clear. The rules are concourse_cull_core.h's.
namespace cc = edvr::concourse;
constexpr uint64_t kConcAnchorFreshMs = 5000;   // the anchor record was seen by the store within this long
// diag.concourse_keep: the extra allow prefixes, in two buffers so the lock-free classifier never reads one being written (the tick flips them at most once a second).
struct ConcourseExtra {
    char text[2][192] = {};
    std::atomic<uint32_t> cur{0};
    void set(const char* s) noexcept {
        const uint32_t n = 1u - cur.load(std::memory_order_relaxed);
        std::snprintf(text[n], sizeof(text[n]), "%s", s ? s : "");
        cur.store(n, std::memory_order_release);
    }
    const char* get() const noexcept { return text[cur.load(std::memory_order_acquire)]; }
};
ConcourseExtra g_concExtra;
std::atomic<bool> g_concWanted{false};        // armed, on foot in social space and not sealed: the store gate is open (it sights the anchor, and judges while active)
std::atomic<bool> g_concActive{false};        // the cull runs: wanted, the key or its override on, and the anchor seen within kConcAnchorFreshMs
std::atomic<bool> g_camLookOn{false};         // TEMPORARY (the camera test): the look is on: the judgement is the keep list inside out (cc::concourseJudge); published in the snapshot as `look`, part of g_concActive
int g_camLookGraceTicks = 0;                  // TEMPORARY: ticks after the look in which a big camera step is its own way back, not another room
bool g_camLookSurvey = false;                 // TEMPORARY: the names survey was taken in the look: its report lists what is near the room's centre and still kept
std::atomic<bool> g_liftGateOn{false};       // the tick's: the lift gate is on, the player is in a shut lift cabin (published in the snapshot as `lift`; part of g_concActive)
// The door rule (cc::roomSeenSlab): the tick's verdict, published in the snapshot as `room`. Written by the tick and read by the publication, both under the state lock.
struct RoomPub {
    bool room = false;    // something is behind a shut door: records wholly outside `slab` are culled
    bool only = false;    // the live room: that is the only rule
    bool rigs = false;    // TEMPORARY (cc::DoorRuleMode::kRigs): a record the rule culls counts as culled in its rig's tally
    float origin[3] = {};
    float axis[3] = {};
    cc::RoomSlab slab;
};
RoomPub g_roomPub;
// The doors' leaves (cc::kDoorLeafLeft, kDoorLeafRight), sighted by the store while the tick wants them (concourseSight) and read by the tick. A slot whose record is not a leaf's any more
// is let go there.
constexpr size_t kDoorLeafSlots = 8;
std::atomic<uintptr_t> g_doorLeaf[kDoorLeafSlots] = {};
std::atomic<bool> g_doorLeafWanted{false};
std::atomic<bool> g_concSurveyArmed{false};   // the one-shot names are being gathered: the rig and producer vetoes forward so the store judges and names every record
std::atomic<bool> g_concNamesOnly{false};     // TEMPORARY: an F3 with the cull off is gathering names: the store only reads each record (concourseNamesOnlyRecord)
bool g_roomCutPending = false;   // the camera stepped over kRoomCutM this tick (resolveTickView); the concourse tick takes it, under the state lock
float g_roomCutM = 0.0f;
// TEMPORARY (the walk survey, below): the tick's. g_walkPassArmed is published to the callbacks in the snapshot (walkOn) with a copy of the pick cell.
bool g_walkPassArmed = false;
sealed::OwnCell g_walkCellCopy;
std::atomic<uint64_t> g_concAnchorMs{0};      // GetTickCount64() when the store last saw the anchor record
std::atomic<uintptr_t> g_concAnchorAddr{0};   // the anchor record, latched by address (re-read with a name check at the tick)
std::atomic<uintptr_t> g_concHubAddr{0};      // SA_StationHub_Compact, the same way
std::atomic<bool> g_concHubWanted{false};     // the tick wants the hub's address: the store compares the names of the allowed records until it has it
std::atomic<uintptr_t> g_concLiveAddr{0};     // the room's live room (cc::RoomLayout::liveRoomName), the same way
std::atomic<const char*> g_concLiveWanted{nullptr};   // its name while the tick wants its address, else null
// What the classifier adds to a node's flags: the allow-list (with the extra prefixes) and the anchor name.
uint32_t concourseNodeFlagsOf(const char* name) noexcept {
    uint32_t f = 0;
    if (cc::concourseAllowName(name, g_concExtra.get())) f |= sealed::kNodeConcourseAllow;
    if (cc::concourseAnchorName(name)) f |= sealed::kNodeConcourseAnchor | sealed::kNodeConcourseFarExempt;
    if (std::strcmp(name, cc::kHubName) == 0) f |= sealed::kNodeConcourseFarExempt;
    if (cc::extraKeepName(name, g_concExtra.get())) f |= sealed::kNodeConcourseFarExempt;   // diag.concourse_keep: the extra names are kept however far (an empty list matches nothing)
    if (cc::liftPartName(name)) f |= sealed::kNodeLiftPart;
    return f;
}
struct ConcourseWin {   // the 5 s summary's window (the callbacks' counters flush here, tcFlush)
    std::atomic<uint64_t> rigSeen{0}, rigVetoed{0}, prodVetoed{0}, prodKept{0}, storeCleared{0};
    std::atomic<uint64_t> tot[6] = {};    // the same counts, never reset: rig seen, rig vetoed, producer vetoed, producer kept, small-close kept, store cleared (the lift's summary takes differences)
    std::atomic<uint64_t> kept[4] = {};   // name, small-close, unreadable-name, unreadable-sphere (the producer's keeps)
    std::atomic<uint64_t> roomProd{0}, roomStore{0};   // the door rule's: taken by its own 5 s line
};
ConcourseWin g_concWin;

bool readMem(void* dst, uintptr_t src, size_t n) noexcept {
    if (!src) return false;
    __try {
        std::memcpy(dst, reinterpret_cast<const void*>(src), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool writeMem(uintptr_t dst, const void* src, size_t n) noexcept {
    if (!dst) return false;
    __try {
        std::memcpy(reinterpret_cast<void*>(dst), src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

template <typename T>
bool readVal(T* dst, uintptr_t src) noexcept { return readMem(dst, src, sizeof(T)); }

// --- Built v8.4: a record's centre. The builder's centre is computed from the pose it reads, *(rec+0x290): q = pose+0x10, t = pose+0x20, c =
// pose+0x80 (sealed_trial_core.h builderCentre; decomp_42B4420.txt:196-237). rec+0x240 is the fallback when the pose cannot be read (counted).
constexpr uintptr_t kRecPose = 0x290;     // pointer: the builder's rcx, the pose context

constexpr uintptr_t kPoseQuat = 0x10;     // float4: the quaternion x, y, z, w

constexpr uintptr_t kPoseT = 0x20;        // float4: the translation

constexpr uintptr_t kPoseCentre = 0x80;   // float4: the local centre c

struct CentreRead {
    sealed::CentreSource source = sealed::CentreSource::kNone;
    float world[3] = {};      // the centre the rule uses
    float pose[3] = {};       // the builder's centre (valid when poseOk)
    bool poseOk = false;
    float raw[3] = {};        // rec+0x240 (valid when rawOk)
    bool rawOk = false;
    bool rawZero = false;     // rec+0x240 is exactly the origin
    float dist = -1.0f;       // |raw - pose| when both are readable and raw is not zero; else -1
};

// The builder's centre from the pose (SEH-guarded reads only). False when the pose pointer or any of its fields cannot be read.
bool poseCentreOf(uintptr_t rec, float out[3]) noexcept {
    uintptr_t pose = 0;
    if (!readVal(&pose, rec + kRecPose) || !pose) return false;
    float q[4] = {}, t[4] = {}, c[4] = {}, w[4] = {};
    if (!readMem(q, pose + kPoseQuat, sizeof(q)) || !readMem(t, pose + kPoseT, sizeof(t)) ||
        !readMem(c, pose + kPoseCentre, sizeof(c)))
        return false;
    sealed::builderCentre(q, t, c, w);
    out[0] = w[0];
    out[1] = w[1];
    out[2] = w[2];
    return sealed::finite3(out);
}

CentreRead recordCentreRead(uintptr_t rec) noexcept {
    CentreRead r;
    r.poseOk = poseCentreOf(rec, r.pose);
    r.rawOk = readMem(r.raw, rec + kRecCentre, sizeof(r.raw)) && sealed::finite3(r.raw);
    r.rawZero = r.rawOk && sealed::isZeroCentre(r.raw);
    r.source = sealed::chooseCentre(r.poseOk, r.pose, r.rawOk, r.raw, r.world);
    if (r.poseOk && r.rawOk && !r.rawZero) r.dist = sealed::distance(r.pose, r.raw);
    return r;
}

// The store-style world sphere (decomp_4312040.txt lines 212-262, sealed_trial_core.h storeWorldSphere). False when any read fails or the result
// is not finite. Reads only.
bool storeSphereOf(uintptr_t rec, float centre[3], float* radius) noexcept {
    constexpr uintptr_t kNodeOff = 0x18;       // rec+0x18: the node (census's name pointer)
    constexpr uintptr_t kBlockOff = 0x08;      // node+0x08: the bounds block
    constexpr uintptr_t kPoseOff = 0x190;      // rec+0x190: the pose block A
    constexpr uintptr_t kMatrixOff = 0x50;     // A+0x50: the matrix B
    float L[3] = {}, A[15] = {}, B[16] = {};
    float r = 0.0f;
    uintptr_t node = 0, block = 0, pose = 0, matrix = 0;
    bool ok = readVal(&node, rec + kNodeOff) && node && readVal(&block, node + kBlockOff) && block &&
              readMem(L, block + 0x20, sizeof(L)) && readVal(&r, block + 0x2C) && readVal(&pose, rec + kPoseOff) && pose &&
              readMem(A, pose, sizeof(A)) && readVal(&matrix, pose + kMatrixOff) && matrix && readMem(B, matrix, sizeof(B));
    ok = ok && std::isfinite(r) && r > 0.0f && sealed::storeWorldSphere(L, A, B, centre);
    if (ok) *radius = r;
    return ok;
}

// Built v11 (second addendum): a record's local axes in the world. The builder's quaternion (the pose at rec+0x290: q at +0x10, the extent at
// +0x90) when the record has one (path A); else the store-style pose (rec+0x190 and its +0x50 matrix; decomp_4312040.txt). src: 1 builder,
// 2 store-style, 4 neither (false). extOk: the builder extent was read (only the builder pose gives one).
bool poseOrientOf(uintptr_t rec, float axes[3][3], float ext[3], bool* extOk, uint8_t* src) noexcept {
    *extOk = false;
    *src = 4;
    uintptr_t pose = 0;
    float q[4] = {};
    if (readVal(&pose, rec + kRecPose) && pose && readMem(q, pose + 0x10, sizeof(q)) && std::isfinite(q[0]) && std::isfinite(q[1]) &&
        std::isfinite(q[2]) && std::isfinite(q[3])) {
        sealed::quatAxes(q, axes);
        *src = 1;
        if (readMem(ext, pose + 0x90, 3 * sizeof(float)) && std::isfinite(ext[0]) && std::isfinite(ext[1]) && std::isfinite(ext[2])) *extOk = true;
        return true;
    }
    uintptr_t pa = 0, matrix = 0;
    float A[15] = {}, B[16] = {};
    if (readVal(&pa, rec + 0x190) && pa && readMem(A, pa, sizeof(A)) && readVal(&matrix, pa + 0x50) && matrix && readMem(B, matrix, sizeof(B))) {
        sealed::storeAxes(A, B, axes);
        *src = 2;
        return true;
    }
    return false;
}

// The producer B cache: per record, the centre and radius B's veto decision used, in a generation (the frame). Built v14.25: lock-free. Each cell is a
// seqlock (station_lockfree_core.h): an insert claims a cell by compare-and-swap and retries on a contended cell, and a lookup copies the cell and
// checks it. When the bounded probe finds no cell the record runs uncached (counted). The generation advances on the tick, under the lock.
constexpr size_t kProdSlots = 8192;
sealed::SeqCell g_prodCells[kProdSlots];
std::atomic<uint32_t> g_prodGenA{1};   // the generation the cells are valid for (0 is never valid)
uint32_t g_prodStampSeen = 0;          // the tick's: the stamp the generation last moved for

// The producer table's probe statistics, atomic (the producer path writes them without the lock).
struct ProdStats {
    std::atomic<uint32_t> maxProbes{0};
    std::atomic<uint64_t> full{0};
};
ProdStats g_prodStats;

void noteProdProbe(uint32_t probes) noexcept {
    uint32_t m = g_prodStats.maxProbes.load(std::memory_order_relaxed);
    while (probes > m && !g_prodStats.maxProbes.compare_exchange_weak(m, probes, std::memory_order_relaxed)) {
    }
}

// Built v14.20 (review 5): the occupancy and the longest probe of each locked table this window (the 30 s summary reports them and resets the maximum),
// and the lookups or inserts that reached the probe bound (a full table is a miss, never a walk of every slot).
struct TableStats {
    uint32_t used = 0;
    uint32_t maxProbes = 0;
    uint64_t full = 0;
};
TableStats g_collStats;
TableStats g_rigStats;

void noteProbe(TableStats& t, const sealed::ProbeResult& p) noexcept {
    if (p.probes > t.maxProbes) t.maxProbes = p.probes;
    if (p.exhausted) ++t.full;
}

// The tick's: moves the generation when the frame moved on (under the lock).
void prodSync() noexcept {
    const uint32_t s = g_frameStamp.load(std::memory_order_relaxed);
    if (s != g_prodStampSeen) {
        g_prodStampSeen = s;
        g_prodGenA.fetch_add(1, std::memory_order_relaxed);
    }
}

size_t prodIndex(uintptr_t rec) noexcept {
    return static_cast<size_t>((static_cast<uint64_t>(rec) >> 4) * 0x9E3779B97F4A7C15ull >> 51) & (kProdSlots - 1);
}

// Lock-free insert for the producer path (any thread), in the generation the snapshot carries.
void prodPutGen(uintptr_t rec, const float c[3], float r, uint32_t gen) noexcept {
    const float v[4] = {c[0], c[1], c[2], r};
    uint32_t probes = 0;
    const bool ok = sealed::seqTableInsert(g_prodCells, kProdSlots, prodIndex(rec), rec, gen, v, &probes);
    noteProdProbe(probes);
    if (!ok) g_prodStats.full.fetch_add(1, std::memory_order_relaxed);   // uncached: a later lookup misses it, as any record the producer never offered
}

// Lookup in the current generation (the locked callers: the world read).
bool prodGet(uintptr_t rec, float c[3], float* r) noexcept {
    float v[4] = {};
    uint32_t probes = 0;
    const bool ok = sealed::seqTableFind(g_prodCells, kProdSlots, prodIndex(rec), rec, g_prodGenA.load(std::memory_order_relaxed), v, &probes);
    noteProdProbe(probes);
    if (!ok) return false;
    c[0] = v[0];
    c[1] = v[1];
    c[2] = v[2];
    *r = v[3];
    return true;
}

// The view the per-record path reads (built v14.25): published by the tick, and by the first store callback of a frame, under the lock. Read by
// pinning the ring; never under the lock. The frame's first callbacks may read the previous frame's view: at most one frame old.
struct PadSnapshot {
    uint32_t frameStamp = 0;
    uint32_t prodGen = 0;
    bool held = false;
    bool camValid = false;
    float cam[3] = {};
    sealed::OwnCell cell;
    size_t explicitCount = 0;
    uintptr_t explicitPrev[sealed::kExplicitNodes] = {};
    char ownPrefix[32] = {};
    bool giveUp = false;          // the hangar watch gave up: the explicit parts are not gathered
    bool latchPending = false;    // a latch is pending: the mechanism candidates are gathered
    bool namesPending = false;    // the hangar names are wanted: the names near the pad are gathered
    float padC[3] = {};           // the pad's centre the names are measured from
    bool ownLiveValid = false;
    float ownLiveCentre[3] = {};
    float ownLiveRadius = 0.0f;
    bool lost = false;            // gathered entries were lost before this publication: the callbacks keep every record
    bool engagePending = false;   // built v14.31: an engage is pending: every pass gathers the near list, the own candidate and the tally
    bool concourse = false;       // built 2026-11-05: the concourse cull is active (the producer veto judges by name)
    bool look = false;            // TEMPORARY (the camera test): the room off and the rest on (cc::concourseJudge)
    bool lift = false;            // built 2026-11-06: the player is in a shut lift cabin: the judgement keeps only what is near (cc::concourseJudge)
    bool room = false;            // the door rule: one of the room's own records wholly behind a shut door is culled, whatever the rule above says (cc::roomSeenSlab)
    bool roomOnly = false;        // and that is the only rule (the live room, where the cull stands down)
    bool roomRigs = false;        // TEMPORARY (cc::DoorRuleMode::kRigs): such a record counts as culled in its rig's tally
    float roomOrigin[3] = {};     // the anchor's centre
    float roomAxis[3] = {};       // its a2
    float roomLo = 0.0f, roomHi = 0.0f;
    bool walkOn = false;         // TEMPORARY (the walk survey): this publication's passes report what the seal would cull against walkCell; nothing else reads either
    sealed::OwnCell walkCell;
};
sealed::SnapshotRing<PadSnapshot, 40, 32> g_snapRing;

// The published view for one callback (built v14.26: every callback reads the view this way). Built v14.29: the pin is the thread's own hazard cell, held
// until the thread's next call, so a batch of calls on one published view touches no cache line another thread writes; a thread past the last cell
// pins by the shared count and releases it here.
class SnapshotPin {
public:
    SnapshotPin() noexcept : p_(g_snapRing.pinThread(&counted_)) {}
    ~SnapshotPin() {
        if (counted_ != 0) g_snapRing.unpin(counted_ - 1);
    }
    SnapshotPin(const SnapshotPin&) = delete;
    SnapshotPin& operator=(const SnapshotPin&) = delete;
    const PadSnapshot& get() const noexcept { return *p_; }

private:
    size_t counted_ = 0;
    const PadSnapshot* p_;
};

// Built v14.26: what a callback gathers for the tick. Each callback appends to its own thread's slot (sealed::GatherBuffers); the tick drains the entries of
// the previous pass in the order they were written, and applies them to its own tables.
enum class GKind : uint8_t { kNone, kPad, kExplicit, kNear, kOwnCand, kTally, kLatch, kHangar, kRigNote, kRigAssoc, kCost };
constexpr uint8_t kFlagJob = 1;   // kRigNote: the store's bucket path (the rig comes from the bucket's collection, resolved on the tick)
constexpr uint8_t kFlagJudged = 2;   // kRigNote, built 2026-11-05: the concourse cull judged this record
constexpr uint8_t kFlagKeep = 4;     // kRigNote with kFlagJudged: the judgement kept it
struct GatherEntry {
    GKind kind = GKind::kNone;
    uint8_t flags = 0;            // kPad: the PadSize; kRigNote: kFlagJob; kCost: 0 store, 1 producer, 2 rig
    uint32_t stamp = 0;
    uintptr_t record = 0;
    uintptr_t aux = 0;            // kExplicit: the node; kRigNote: the rig (synchronous path) or the bucket's owner (job path); kRigAssoc: the rig
    uintptr_t bucket = 0;         // kRigNote: the store's bucket
    uintptr_t coll = 0;           // kRigAssoc: the collection the association moved to
    float centre[3] = {};
    float radius = 0.0f;
    float dist = 0.0f;
    float ms = 0.0f;              // kCost: the sampled call's time
    char name[64] = {};
};
constexpr size_t kGatherSlots = 16;
constexpr size_t kGatherCap = 1024;
sealed::PassGather<GatherEntry, kGatherSlots, kGatherCap> g_gather(g_frameStamp);
using GatherWriter = sealed::PassGather<GatherEntry, kGatherSlots, kGatherCap>::Writer;
bool g_lossPublished = false;                // the tick: entries were lost in the pass it last drained (the next publication keeps every record)
uint64_t g_winLost = 0;                      // entries lost in the window (the cost line)
std::atomic<uint64_t> g_lockOnCallback{0};   // state-lock acquisitions made by a callback (must stay 0; the cost line reports it)
thread_local bool tlsOnCallback = false;

// Marks a callback's extent for the lock counter above.
class CallbackScope {
public:
    CallbackScope() noexcept { tlsOnCallback = true; }
    ~CallbackScope() { tlsOnCallback = false; }
    CallbackScope(const CallbackScope&) = delete;
    CallbackScope& operator=(const CallbackScope&) = delete;
};

// A gathered entry with a centre and a name: the callbacks' common shape.
GatherEntry gatherEntry(GKind kind, uintptr_t record, const float c[3], float r, uint32_t stamp, const char* name) noexcept {
    GatherEntry e;
    e.kind = kind;
    e.stamp = stamp;
    e.record = record;
    e.centre[0] = c[0];
    e.centre[1] = c[1];
    e.centre[2] = c[2];
    e.radius = r;
    if (name != nullptr) std::snprintf(e.name, sizeof(e.name), "%s", name);
    return e;
}

// A callback's cost sample, pushed to the tick (no lock). 0 store, 1 producer, 2 rig.
void costPush(uint8_t which, double ms) noexcept {
    GatherEntry e;
    e.kind = GKind::kCost;
    e.flags = which;
    e.ms = static_cast<float>(ms);
    GatherWriter w(g_gather);
    w.push(e);
}

// Built v14.31: the node's class, flags and name come from the shared node cache: the name is read from game memory once per node per lifecycle
// generation (a node's name does not change while the node lives; undock, hangar change, ship change and a setting toggle start every node afresh).
struct NodeInfo {
    sealed::NameClass cls = sealed::NameClass::kUnnamed;
    uint32_t flags = 0;
    uint64_t validator = 0;   // built v14.32: the node's name-text validator, taken on the lookup (0: the text was unreadable)
};

// Built v14.32: the validator of a node's name: the text pointer (node+0) and the first 16 bytes of the text, two guarded reads. A node reallocated within
// a generation holds another name, so another validator, and a cache hit under the old one is refused.
uint64_t nodeValidatorOf(uintptr_t node) noexcept {
    uintptr_t text = 0;
    uint8_t first[16] = {};
    if (!readVal(&text, node) || !text || !readMem(first, text, sizeof(first))) return 0;
    return sealed::nodeValidator(static_cast<uint64_t>(text), first);
}

// Reads a node's name from game memory (guarded) and classifies it; name (64 bytes) is filled when the name is readable.
NodeInfo classifyNode(uintptr_t node, char* name) noexcept {
    NodeInfo info;
    if (name != nullptr) name[0] = '\0';
    uintptr_t text = 0;
    uint8_t bytes[sealed::kNameBytes] = {};
    g_nodeNameReads.fetch_add(1, std::memory_order_relaxed);
    if (!readVal(&text, node) || !text || !readMem(bytes, text, sizeof(bytes))) return info;
    std::string parsed;
    const bool ok = sealed::textName(bytes, sizeof(bytes), false, &parsed) || sealed::textName(bytes, sizeof(bytes), true, &parsed);
    info.cls = sealed::classifyName(ok, parsed.c_str());
    info.flags = ok ? (sealed::nodeFlagsOf(parsed.c_str()) | concourseNodeFlagsOf(parsed.c_str())) : 0u;
    if (ok && name != nullptr) std::snprintf(name, sealed::NodeCache<kNodeCacheSlots>::kNameBytes, "%s", parsed.c_str());
    return info;
}

// The node's info (and name when `name` is given), cached. A node that is not 8-aligned, or a full probe window, is read uncached. A hit is trusted only
// when the node's name-text validator still equals the entry's (one guarded read of the text pointer and 16 bytes); a reused node is classified afresh.
// knownValidator: the validator the caller already took for this node in this call (nullptr: take it here).
NodeInfo nodeInfo(uintptr_t node, char* name, const uint64_t* knownValidator = nullptr) noexcept {
    NodeInfo info;
    const uint32_t gen = g_classGen.load(std::memory_order_relaxed);
    const bool cacheable = sealed::slotFits(node);
    const uint64_t validator = knownValidator != nullptr ? *knownValidator : nodeValidatorOf(node);
    info.validator = validator;
    bool stale = false;
    if (cacheable) {
        NodeInfo hit;
        if (g_nodeCache.find(node, gen, &hit.cls, &hit.flags, name, validator, &stale)) {
            hit.validator = validator;
            return hit;
        }
    }
    if (stale) g_nodeReuseCaught.fetch_add(1, std::memory_order_relaxed);
    char local[sealed::NodeCache<kNodeCacheSlots>::kNameBytes];
    char* const buf = name != nullptr ? name : local;
    const NodeInfo read = classifyNode(node, buf);
    info.cls = read.cls;
    info.flags = read.flags;
    if (cacheable) {
        if (!g_nodeCache.put(node, gen, info.cls, info.flags, buf, validator)) g_nodeCacheFull.fetch_add(1, std::memory_order_relaxed);
    }
    return info;
}

// A node's name; false when it has none or it is unreadable.
bool cachedNameOfNode(uintptr_t node, char* out, size_t cap, const uint64_t* knownValidator = nullptr) noexcept {
    if (!out || cap == 0) return false;
    out[0] = '\0';
    if (!node) return false;
    char name[sealed::NodeCache<kNodeCacheSlots>::kNameBytes];
    const NodeInfo info = nodeInfo(node, name, knownValidator);
    if (info.cls == sealed::NameClass::kUnnamed || name[0] == '\0') return false;
    std::snprintf(out, cap, "%s", name);
    return true;
}

// The record's name: the one name reader on every path (built v14.25; lock-free since v14.26; the shared node cache since v14.31).
bool cachedName(uintptr_t record, char* out, size_t cap) noexcept {
    if (!out || cap == 0) return false;
    out[0] = '\0';
    uintptr_t node = 0;
    if (!record || !readVal(&node, record + kRecName) || !node) return false;
    return cachedNameOfNode(node, out, cap);
}

// The producer-B decision from the published view (built v14.25; was producerDecide under the lock). Caches the record's sphere in the snapshot's
// generation. True skips the record: outside the cell, and not the own ship.
bool producerDecideSnap(uintptr_t rec, const PadSnapshot& s) noexcept {
    float centre[3] = {};
    float radius = 0.0f;
    const bool ok = storeSphereOf(rec, centre, &radius);
    if (ok) prodPutGen(rec, centre, radius, s.prodGen);
    if (!ok || !s.camValid) return false;
    if (sealed::keepNamedCell(centre, radius, s.cam, kFallbackSphereM, s.cell, s.explicitPrev, s.explicitCount, rec).keep) return false;
    char nm[48] = {};
    if (s.ownPrefix[0] != '\0') cachedName(rec, nm, sizeof(nm));
    if (sealed::ownShipExempt(sealed::containsCamera(centre, radius, s.cam), radius, nm, s.ownPrefix)) return false;
    return true;
}

struct WorldRead {
    float world[3] = {};
    float radius = 0.0f;
    uint8_t source = 4;   // 0 producer B, 1 pose, 2 store-style, 3 raw fallback, 4 none
};

// The named rule's world centre and radius for a record (see the block comment above). Lock-free (built v14.26): any thread.
WorldRead worldOfRecord(uintptr_t rec) noexcept {
    WorldRead w;
    if (prodGet(rec, w.world, &w.radius)) {
        w.source = 0;
        return w;
    }
    float p[3] = {};
    if (poseCentreOf(rec, p)) {
        float r = 0.0f;
        if (readVal(&r, rec + kRecRadius) && std::isfinite(r) && r > 0.0f) {
            for (int k = 0; k < 3; ++k) w.world[k] = p[k];
            w.radius = r;
            w.source = 1;
        }
        return w;
    }
    if (storeSphereOf(rec, w.world, &w.radius)) {
        w.source = 2;
        return w;
    }
    float raw[3] = {};
    float r = 0.0f;
    // Built v13.3 (c): a zero raw centre is unreadable, not the origin: no sphere (the record is kept as unknown in named mode).
    if (readMem(raw, rec + kRecCentre, sizeof(raw)) && readVal(&r, rec + kRecRadius) && sealed::rawCentreUsable(raw, r)) {
        for (int k = 0; k < 3; ++k) w.world[k] = raw[k];
        w.radius = r;
        w.source = 3;
    }
    return w;
}

// The class of a node's name, read from game memory with no lock (the census's accept rules). The miss path of the class cache.
// The class and flags of a record's node: one guarded read of the node pointer and the cache lookup.
NodeInfo nodeInfoOf(uintptr_t record, uintptr_t* nodeOut) noexcept {
    *nodeOut = 0;
    uintptr_t node = 0;
    if (!record || !readVal(&node, record + kRecName) || !node) return NodeInfo{};
    *nodeOut = node;
    return nodeInfo(node, nullptr);
}

// Built v14.8: count one record of a type prefix (the records within kShipRecordsM of the camera).
void tallyShipPrefix(const char* name) noexcept {
    char pfx[32] = {};
    sealed::ownPrefixOf(name, pfx, sizeof(pfx));
    if (pfx[0] == '\0') return;
    for (size_t i = 0; i < g_prefixTallyN; ++i) {
        if (std::strcmp(g_prefixTally[i].p, pfx) == 0) {
            ++g_prefixTally[i].n;
            return;
        }
    }
    if (g_prefixTallyN < kPrefixSlots) {
        std::snprintf(g_prefixTally[g_prefixTallyN].p, sizeof(g_prefixTally[g_prefixTallyN].p), "%s", pfx);
        g_prefixTally[g_prefixTallyN].n = 1;
        ++g_prefixTallyN;
    }
}

uint32_t prefixTallyOf(const char* pfx) noexcept {
    for (size_t i = 0; i < g_prefixTallyN; ++i) {
        if (std::strcmp(g_prefixTally[i].p, pfx) == 0) return g_prefixTally[i].n;
    }
    return 0;
}

// Built v13 (fix 1): at each named engage, the own ship\'s type prefix from the candidate record (the one nearest the camera that contains it), logged.
void setOwnPrefixAtEngage(uint32_t frame) {
    (void)frame;
    sealed::ownPrefixOf(g_ownCandidateName, g_ownPrefix, sizeof(g_ownPrefix));
}

// Promotes the previous frame's pads into the cell for the frame now starting, and the explicit instances for that cell. Called under the lock
// at the frame's first record. The pads and explicit parts are the previous frame's; the camera is the previous frame's too.
// Built v13.2 (b): re-reads the own record by address (its centre and radius move with the pad). A record whose name no longer matches is dropped:
// the address was freed or reused, and the rule stands down until the next engage.
void readOwnLive() noexcept {
    g_ownLiveValid = false;
    if (!g_ownRecAddr) return;
    const WorldRead wr = worldOfRecord(g_ownRecAddr);
    char nm[48] = {};
    if (wr.source == 4 || !cachedName(g_ownRecAddr, nm, sizeof(nm)) || !sealed::ownRecordMatch(nm, g_ownRecName)) {
        g_ownRecAddr = 0;
        return;
    }
    if (!std::isfinite(wr.radius) || !(wr.radius > 0.0f)) return;
    std::memcpy(g_ownLiveCentre, wr.world, sizeof(g_ownLiveCentre));
    g_ownLiveRadius = wr.radius;
    g_ownLiveValid = true;
}

// Built v13.2 (b): at a named engage the own record is the one named as the stripped type prefix in the previous frame's near list: containing the
// camera preferred, then the nearest. Logged either way.
void pickOwnRecord(uint32_t frame) {
    (void)frame;
    g_ownRecAddr = 0;
    g_ownLiveValid = false;
    g_ownRecName[0] = '\0';
    char stripped[48] = {};
    if (!sealed::strippedPrefix(g_ownPrefix, stripped, sizeof(stripped))) {
        return;
    }
    int best = -1;
    bool bestIn = false;
    for (size_t i = 0; i < g_nearPrevN; ++i) {
        const NearRec& r = g_nearPrev[i];
        if (!sealed::ownRecordMatch(r.name, stripped)) continue;
        const bool in = r.dist <= r.radius;
        if (best < 0 || (in && !bestIn) || (in == bestIn && r.dist < g_nearPrev[best].dist)) {
            best = static_cast<int>(i);
            bestIn = in;
        }
    }
    if (best < 0) {
        return;
    }
    const NearRec& r = g_nearPrev[best];
    g_ownRecAddr = r.addr;
    std::snprintf(g_ownRecName, sizeof(g_ownRecName), "%s", r.name);
    readOwnLive();
}

// Built v13.7 (a, b): the held cell rebuilt in the held pad's CURRENT frame from its live pose (its centre by worldOfRecord, its axes by
// poseOrientOf), and the pad's height along the shaft against the shell. Called at the frame's promote (before the drop decisions) and at the frame
// tick (before the rig verdicts), so every judgement of a frame uses the cell as the frame's records are expressed. A pose that cannot be read
// this frame keeps the cell as it was. A shell that cannot be read stands the motion check down until the next engage.
// Built 2026-10-15: returns the call's result for the seal trace (station_seal_trace.h): not held, rebuilt and moved, rebuilt and unmoved, or kept as it was
// when the pose could not be read. The tick's own call is the one recorded; the same result is not used for anything else.
sealed::HeldRefresh refreshHeldWorld() {
    if (!g_heldValid || !g_heldLocalValid) return sealed::HeldRefresh::kNotHeld;
    const WorldRead pw = worldOfRecord(g_heldCell.pad);
    float axes[3][3] = {};
    float ext[3] = {};
    bool extOk = false;
    uint8_t src = 4;
    if (pw.source == 4 || !poseOrientOf(g_heldCell.pad, axes, ext, &extOk, &src)) return sealed::HeldRefresh::kKeptStale;
    sealed::OwnCell w = g_heldCell;
    sealed::worldizeCell(g_heldLocal, pw.world, axes, &w);
    const sealed::HeldRefresh res = sealed::distance(w.centre, g_heldCell.centre) == 0.0f ? sealed::HeldRefresh::kNoChange : sealed::HeldRefresh::kRefreshed;
    g_heldCell = w;
    g_cellPrev = w;
    g_frameShiftM = sealed::distance(pw.world, g_engageCentre);
    if (!g_shellValid) return res;
    // One sample a frame: the tick and the observer both refresh the held cell.
    const uint32_t shellStamp = g_frameStamp.load(std::memory_order_relaxed);
    if (g_shellSampleStamp == shellStamp) return res;
    if (!g_passInformative) return res;   // built v14.27: the motion sample is taken on an informative pass only (an empty or unfinished pass is no reading)
    g_shellSampleStamp = shellStamp;
    char nm[48] = {};
    const WorldRead sw = worldOfRecord(g_shellAddr);
    if (sw.source == 4 || !cachedName(g_shellAddr, nm, sizeof(nm)) || std::strcmp(nm, g_shellName) != 0) {
        // Built v14 (fix 2): the reference is not switched to another part; the pad's motion check stands down for this seal.
        g_shellValid = false;
        Log::get().note("optimized station: the reference shell %s is no longer at its record; the pad's motion check stands down (no other shell is taken)", g_shellName);
        return res;
    }
    const float up[3] = {g_engageAxes[2][0], g_engageAxes[2][1], g_engageAxes[2][2]};
    const bool first = !g_shellMotion.captured;
    const bool release = g_shellMotion.sample(pw.world, pw.source, sw.world, sw.source, up);
    if (first && g_shellMotion.captured) {
        Log::get().note("optimized station: motion reference read live at the first frame held: the pad sits %.2f m along the shaft above the shell %s "
                        "(centre sources pad %u, shell %u).", static_cast<double>(g_shellMotion.ref), g_shellName,
                        static_cast<unsigned>(pw.source), static_cast<unsigned>(sw.source));
    }
    g_shellDeltaM = g_shellMotion.delta;
    if (release) g_releaseMotion = true;
    return res;
}

// The record's view mask and admitted flag through the guarded reads and writes, for the fault-safe cull (sealed_trial_core.h).
struct RecordFields {
    uintptr_t rec;
    bool readMask(uint64_t* out) noexcept { return readVal(out, rec + kRecMask); }
    bool readAdmitted(uint8_t* out) noexcept { return readVal(out, rec + kRecAdmitted); }
    bool writeMask(uint64_t v) noexcept { return writeMem(rec + kRecMask, &v, sizeof(v)); }
    bool writeAdmitted(uint8_t v) noexcept { return writeMem(rec + kRecAdmitted, &v, sizeof(v)); }
};

constexpr size_t kRigSlots = 4096;

struct RigSlot {
    uintptr_t rig = 0;               // 0: an empty slot
    uint32_t epoch = 0;              // built v14.20 (review 4): the lifecycle epoch the slot was claimed in; an older one is free and forwards
    uint32_t curStamp = 0;           // the frame stamp the current box was accumulated in
    bool curValid = false;
    float curMn[3] = {}, curMx[3] = {};
    bool lastValid = false;          // the box of the last frame that had records (the one before curStamp)
    float lastMn[3] = {}, lastMx[3] = {};
    bool named = false;
    char name[48] = {};              // the name of the rig's first record (for the own-ship prefix)
    uintptr_t coll = 0;              // the rig's collection (rig+0x348), as last read at a decision
    uint8_t verdict = 0;             // kVerdictUnknown / kVerdictMixed / kVerdictOutside, refreshed once per frame (refreshRigVerdicts)
    cc::RigTally curTally, lastTally;   // built 2026-11-05: the concourse cull's judgements of the rig's records in the current and the last frame that had any
};

constexpr uint8_t kVerdictUnknown = 0, kVerdictMixed = 1, kVerdictOutside = 2;

RigSlot g_rigSlots[kRigSlots];
// The rig verdicts the tick publishes for the callbacks (built v14.26): the rig veto reads it, with no lock.
sealed::RigPubTable g_rigPub;

// The collection table (Built v12.1): collection -> rig, for the job path's association. Open addressing, written at a decision when a rig's
// collection is read, read per bucket by the store's observer. Under the trial's lock.
constexpr size_t kCollSlots = 1024;

// Built v14.20 (review 4): every association carries the lifecycle epoch it was made in. An association from an earlier epoch is free: a lookup misses
// it and an insert reuses it. The epoch is bumped at undock, hangar change, ship change, setting toggle and an association change (bumpRigEpoch).
uint32_t g_rigEpoch = 1;
std::atomic<uint32_t> g_rigEpochA{1};   // built v14.27: the epoch the rig veto compares a published cell's epoch to (a cell of an older epoch forwards)
uint64_t g_rigEpochs = 0;          // the lifecycle bumps this window (the summary's count)
uint64_t g_winRigPublications = 0, g_winRigPublished = 0;   // built v14.27: publications committed this window, and the rigs they held

struct CollSlot {
    uintptr_t coll = 0;
    uintptr_t rig = 0;
    uint32_t epoch = 0;
};

CollSlot g_collSlots[kCollSlots];

size_t collIndex(uintptr_t coll) noexcept {
    return static_cast<size_t>((static_cast<uint64_t>(coll) >> 4) * 0x9E3779B97F4A7C15ull >> 54) & (kCollSlots - 1);
}

void collInsert(uintptr_t coll, uintptr_t rig) noexcept {
    const sealed::ProbeResult p = sealed::boundedProbe(
        collIndex(coll), kCollSlots, sealed::kTableProbeMax,
        [coll](size_t i) { const CollSlot& s = g_collSlots[i]; return s.epoch == g_rigEpoch && s.coll == coll; },
        [](size_t i) { return g_collSlots[i].epoch != g_rigEpoch; });
    noteProbe(g_collStats, p);
    if (p.exhausted) return;   // not associated: the record forwards its rig's own rules, the same as a rig with no association
    CollSlot& s = g_collSlots[p.index];
    if (p.empty) ++g_collStats.used;
    s.coll = coll;
    s.rig = rig;
    s.epoch = g_rigEpoch;
}

uintptr_t collFind(uintptr_t coll) noexcept {
    const sealed::ProbeResult p = sealed::boundedProbe(
        collIndex(coll), kCollSlots, sealed::kTableProbeMax,
        [coll](size_t i) { const CollSlot& s = g_collSlots[i]; return s.epoch == g_rigEpoch && s.coll == coll; },
        [](size_t i) { return g_collSlots[i].epoch != g_rigEpoch; });
    noteProbe(g_collStats, p);
    return p.found ? g_collSlots[p.index].rig : 0;
}

// The job path's one-entry cache per frame: the last store bucket and the rig it resolved to (a bucket's records come in a run).
uintptr_t g_rigBucketCache = 0, g_rigBucketRig = 0;
uintptr_t g_rigBucketColl = 0;   // built v14.24: the collection the cached bucket names (the job path's association check)


uint64_t g_rigNamesLogged = 0;

size_t rigIndex(uintptr_t rig) noexcept {
    return static_cast<size_t>((static_cast<uint64_t>(rig) >> 4) * 0x9E3779B97F4A7C15ull >> 52) & (kRigSlots - 1);
}

// Built v14.20 (review 4): claims a slot for a rig in the current epoch: its bounds, its name and its verdict start empty, so the first evaluation
// forwards until a fresh bound exists.
RigSlot* rigClaim(RigSlot& s, uintptr_t rig) noexcept {
    s = RigSlot{};
    s.rig = rig;
    s.epoch = g_rigEpoch;
    ++g_rigStats.used;
    return &s;
}

// Built v14.20 (review 5): a bounded probe. A slot of an earlier epoch is free; a full bound returns null (the rig forwards: its records are not
// bounded, the same as a rig the table never saw).
RigSlot* rigFind(uintptr_t rig, bool insert) noexcept {
    const sealed::ProbeResult p = sealed::boundedProbe(
        rigIndex(rig), kRigSlots, sealed::kTableProbeMax,
        [rig](size_t i) { return g_rigSlots[i].rig == rig; },
        [](size_t i) { const RigSlot& s = g_rigSlots[i]; return s.rig == 0 || s.epoch != g_rigEpoch; });
    noteProbe(g_rigStats, p);
    if (p.found) {
        RigSlot& s = g_rigSlots[p.index];
        return s.epoch == g_rigEpoch ? &s : rigClaim(s, rig);
    }
    if (p.empty && insert) return rigClaim(g_rigSlots[p.index], rig);
    return nullptr;
}

// Built v12.1 (flight fix d), v14.26: the per-frame refresh of every rig's verdict, on the frame tick, from its bound: the box of its records' spheres in
// the last frame that had records. The verdicts are published for the callbacks: the rig veto reads the publication, with no lock. A rig with no bound,
// the own ship's rig, and every rig after a loss of gathered entries are unknown, and forward.
void refreshRigVerdicts() noexcept {
    g_rigPub.begin();
    // No cell held, no verdict is published (the veto answers only while sealed), except the concourse cull's (built 2026-11-05): a rig is outside when a record
    // of it was judged in its last pass and none was kept.
    const bool concourse = !g_heldValid && g_concActive.load(std::memory_order_acquire);
    if (!g_heldValid && !concourse) {
        g_rigPub.commit();
        return;
    }
    const bool allMode = false;
    uint32_t published = 0;
    for (RigSlot& s : g_rigSlots) {
        if (s.rig == 0 || s.epoch != g_rigEpoch) continue;
        if (concourse) {
            const cc::RigTally& t = s.curTally.judged > 0 ? s.curTally : s.lastTally;
            s.verdict = cc::concourseRigVetoed(t) ? kVerdictOutside : kVerdictUnknown;
            const uint8_t ccode = g_lossPublished ? kVerdictUnknown : s.verdict;
            uint32_t cCarried = 0;
            uint8_t cov = 0;
            uintptr_t coc = 0;
            size_t coidx = 0;
            if (g_rigPub.lookup(s.rig, rigIndex(s.rig), &cov, &coc, &coidx)) cCarried = g_rigPub.runAt(coidx);
            size_t cidx = 0;
            if (g_rigPub.insert(s.rig, rigIndex(s.rig), ccode, s.coll, &cidx, g_rigEpoch, cCarried)) ++published;
            continue;
        }
        const float* mn = nullptr;
        const float* mx = nullptr;
        if (s.curValid) {
            mn = s.curMn;
            mx = s.curMx;
        } else if (s.lastValid) {
            mn = s.lastMn;
            mx = s.lastMx;
        }
        const bool bounded = mn != nullptr;
        float c[3] = {0.0f, 0.0f, 0.0f};
        float r = 0.0f;
        if (bounded) {
            float d2 = 0.0f;
            for (int k = 0; k < 3; ++k) {
                c[k] = 0.5f * (mn[k] + mx[k]);
                const float h = 0.5f * (mx[k] - mn[k]);
                d2 += h * h;
            }
            r = std::sqrt(d2);
        }
        const sealed::RigVerdict v = sealed::rigVerdict(bounded, c, r, g_cellPrev.has, g_cellPrev, allMode, 0);
        // Built v13 (fix 1): the own ship's rig is not vetoed in sphere or named mode: its bound holds the camera with a small radius, or its first
        // record's name has the own-ship prefix. (All mode vetoes it.)
        bool ownRig = false;
        if (!allMode && bounded && g_frameCamValid) ownRig = sealed::containsCamera(c, r, g_frameCam) && r < sealed::kOwnShipRadiusM;
        if (!allMode && !ownRig && g_ownPrefix[0] != '\0') ownRig = sealed::ownShipExempt(false, 0.0f, s.name, g_ownPrefix);
        s.verdict = ownRig ? kVerdictUnknown : sealed::verdictCode(v);   // 2 outside, 1 mixed, 0 unknown
        const uint8_t code = g_lossPublished ? kVerdictUnknown : s.verdict;
        // The run of vetoes the rig has in the committed publication moves into this one (the refresh every kRigRefreshN calls needs it across ticks).
        uint32_t carried = 0;
        uint8_t ov = 0;
        uintptr_t oc = 0;
        size_t oidx = 0;
        if (g_rigPub.lookup(s.rig, rigIndex(s.rig), &ov, &oc, &oidx)) carried = g_rigPub.runAt(oidx);
        size_t idx = 0;
        if (g_rigPub.insert(s.rig, rigIndex(s.rig), code, s.coll, &idx, g_rigEpoch, carried)) ++published;
    }
    g_rigPub.commit();
    ++g_winRigPublications;
    g_winRigPublished += published;
}

// Built 2026-11-03 (log only): the tick's view of the sealed pad's rig, once a tick, for the trace row and the 30 s line. The rig is learned from the pad's
// own record (applyRigNote), which in a sealed pass arrives once in kRigRefreshN + 1 frames while the rig is vetoed; until then it reads "not known".
void updateOwnRigTrace() noexcept {
    if (g_ownRigPad != g_ownSticky.pad) {   // another pad (or none): the old rig says nothing about it
        g_ownRigPad = g_ownSticky.pad;
        g_ownRig = 0;
        g_ownRigA.store(0, std::memory_order_relaxed);
    }
    sealed::OwnRigTrace o;
    if (g_ownRig != 0) {
        o.known = true;
        uint8_t pv = 0;
        uintptr_t pc = 0;
        size_t idx = 0;
        uint32_t ep = 0;
        if (g_rigPub.lookup(g_ownRig, rigIndex(g_ownRig), &pv, &pc, &idx, &ep)) {
            o.verdict = pv;
            o.run = g_rigPub.runAt(idx);
            uintptr_t live = 0;
            o.publication = ep != g_rigEpochA.load(std::memory_order_acquire) ? 2 : (readVal(&live, g_ownRig + 0x348) && live != 0 && live != pc ? 3 : 0);
        }
        const RigSlot* s = rigFind(g_ownRig, false);
        const float* mn = nullptr;
        const float* mx = nullptr;
        if (s && s->curValid) {
            mn = s->curMn;
            mx = s->curMx;
        } else if (s && s->lastValid) {
            mn = s->lastMn;
            mx = s->lastMx;
        }
        if (mn) {
            float c[3] = {};
            float d2 = 0.0f;
            for (int k = 0; k < 3; ++k) {
                c[k] = 0.5f * (mn[k] + mx[k]);
                const float h = 0.5f * (mx[k] - mn[k]);
                d2 += h * h;
            }
            o.bounded = true;
            o.radiusM = std::sqrt(d2);
            o.distM = g_cellPrev.has ? sealed::distanceToCell(c, g_cellPrev) : 0.0f;
        }
    }
    g_ownRigTrace = o;
}

// ---- Built v14.24 (2026-10-13): the station's own costs, clocked with QPC and reported in the 30 s summary ---------------------------------------
// The store observer, the producer veto and the rig veto are timed one call in 32 (their p50, p95 and max are per call); the frame tick is timed every
// frame (per frame). The state lock's wait is timed on every acquisition it guards. The samples are the tick's (built v14.26: the callbacks' samples arrive as gathered entries).
int64_t costTicks() noexcept {
    LARGE_INTEGER q{};
    QueryPerformanceCounter(&q);
    return static_cast<int64_t>(q.QuadPart);
}
int64_t costFreq() noexcept {
    static const int64_t f = [] {
        LARGE_INTEGER q{};
        QueryPerformanceFrequency(&q);
        return static_cast<int64_t>(q.QuadPart);
    }();
    return f;
}
double costMs(int64_t a, int64_t b) noexcept { return static_cast<double>(b - a) * 1000.0 / static_cast<double>(costFreq()); }

constexpr size_t kCostSamples = 4096;
struct CostRing {
    double v[kCostSamples] = {};
    size_t n = 0;
    void add(double x) noexcept {
        v[n % kCostSamples] = x;
        ++n;
    }
    size_t used() const noexcept { return n < kCostSamples ? n : kCostSamples; }
    void clear() noexcept { n = 0; }
};
CostRing g_costStore;
CostRing g_costProducer;
CostRing g_costRig;
CostRing g_costFrame;
std::atomic<uint64_t> g_callsStore{0};      // built v14.25: every call (the per-frame counts)
std::atomic<uint64_t> g_callsProducer{0};
std::atomic<uint64_t> g_callsRig{0};
std::atomic<uint64_t> g_concCallsTotal[3] = {};   // the concourse summary's own monotonic totals: the 30 s hangar summary exchanges the three counters above to 0

// The mean of a ring's samples (tick only).
double ringMean(const CostRing& r) noexcept {
    const size_t n = r.used();
    if (n == 0) return 0.0;
    double sum = 0.0;
    for (size_t i = 0; i < n; ++i) sum += r.v[i];
    return sum / static_cast<double>(n);
}

// The producer cache's occupancy: the cells of the current generation (read under the lock, once per summary).
uint32_t prodOccupancy() noexcept {
    const uint32_t gen = g_prodGenA.load(std::memory_order_relaxed);
    uint32_t n = 0;
    for (size_t i = 0; i < kProdSlots; ++i) {
        uintptr_t r = 0;
        uint32_t g = 0;
        float tmp[4];
        if (sealed::seqCellRead(g_prodCells[i], &r, &g, tmp) && g == gen && r != 0) ++n;
    }
    return n;
}
double g_mutexWaitTotalMs = 0.0;
double g_mutexWaitMaxMs = 0.0;
uint64_t g_mutexBlocked = 0;          // acquisitions that waited more than 0.1 ms
uint64_t g_waitingLinesWritten = 0;   // waiting lines written this window (the change-only line)

// The tick's sample (built v14.26: the callbacks push theirs as entries, and the tick adds them here, with no lock).
void costAdd(CostRing& r, double ms) noexcept { r.add(ms); }
bool costSampleCall(uint32_t& calls) noexcept { return ((++calls) & 31u) == 0u; }   // built v14.29: the thread's own counter (no shared line)


// The state lock, with its wait timed. Built v14.26: taken by the tick (and the setting's toggle) only. An acquisition made by a callback is counted
// (the cost line reports the count, which must stay 0).
class TimedStateLock {
public:
    TimedStateLock() noexcept {
        if (tlsOnCallback) g_lockOnCallback.fetch_add(1, std::memory_order_relaxed);
        const int64_t t0 = costTicks();
        g_sealedMutex.lock();
        const double waitMs = costMs(t0, costTicks());
        g_mutexWaitTotalMs += waitMs;
        if (waitMs > g_mutexWaitMaxMs) g_mutexWaitMaxMs = waitMs;
        if (waitMs > 0.1) ++g_mutexBlocked;
    }
    ~TimedStateLock() { g_sealedMutex.unlock(); }
    TimedStateLock(const TimedStateLock&) = delete;
    TimedStateLock& operator=(const TimedStateLock&) = delete;
};

// Built v14.20 (review 4): a lifecycle point (undock, hangar change, ship change, setting toggle, a changed association) moves to a new epoch. Every rig
// slot, association and verdict of the old epoch is free; a rig forwards until it has a fresh bound. Caller holds g_sealedMutex.
void bumpRigEpoch() noexcept {
    ++g_rigEpoch;
    g_rigEpochA.store(g_rigEpoch, std::memory_order_release);
    g_classGen.fetch_add(1, std::memory_order_relaxed);   // built v14.29: every node's class and name are read afresh
    ++g_rigEpochs;
    g_rigStats.used = 0;
    g_collStats.used = 0;
    g_rigBucketCache = 0;
    g_rigBucketRig = 0;
}

// The rig early veto, lock-free (built v14.26). The verdict and the collection come from the tick's publication (RigPubTable). A rig the publication does
// not hold forwards, as a rig the tick never bounded did. A collection that moved since the publication forwards; the tick applies the move, one frame
// behind. The run of vetoes per rig is a compare-and-swap in the table.
bool rigVetoLockFree(uintptr_t rig) noexcept {
    if (!rig) return false;
    SnapshotPin pin;
    if (!pin.get().held && !pin.get().concourse) return false;
    const uint32_t stamp = g_frameStamp.load(std::memory_order_relaxed);
    TlsCounters& tc = tcFor(stamp);   // built v14.29: the thread's own counters
    ++tc.rigSeen;
    const bool padRig = rig == g_ownRigA.load(std::memory_order_relaxed);   // log only, 2026-11-03: the sealed pad's rig (rig is not 0 here)
    uint8_t verdict = kVerdictUnknown;
    uintptr_t published = 0;
    size_t idx = 0;
    uint32_t epoch = 0;
    uintptr_t coll = 0;
    const bool haveColl = readVal(&coll, rig + 0x348) && coll != 0;
    if (!g_rigPub.lookup(rig, rigIndex(rig), &verdict, &published, &idx, &epoch)) {
        // Built v14.27: a rig the publication does not hold asks the tick to register it (once per thread per frame). v14.26 only asked for rigs already
        // published, and the tick learns a rig no other way on the job path, so no rig was ever published and the veto never fired.
        ++tc.rigMiss;
        ++tc.rigUnknown;
        if (padRig) padStageAdd(sealed::kPadRigMiss);
        if (haveColl) {
            struct Asked {
                uintptr_t rig = 0;
                uint32_t stamp = 0;
            };
            thread_local Asked asked[256];
            Asked& a = asked[(rig >> 4) & 255];
            if (a.rig != rig || a.stamp != stamp) {
                a.rig = rig;
                a.stamp = stamp;
                GatherEntry e;
                e.kind = GKind::kRigAssoc;
                e.aux = rig;
                e.coll = coll;
                GatherWriter w(g_gather);
                if (w.push(e)) ++tc.rigAsked;
            }
        }
        return false;
    }
    if (epoch != g_rigEpochA.load(std::memory_order_acquire)) {
        ++tc.rigStale;   // published in an earlier lifecycle epoch: forwards until the tick republishes
        ++tc.rigUnknown;
        if (padRig) padStageAdd(sealed::kPadRigStale);
        return false;
    }
    if (haveColl && coll != published) {
        g_rigPub.touch(idx, stamp);
        ++tc.rigHitMoved;
        if (padRig) padStageAdd(sealed::kPadRigMoved);
        if (g_rigPub.askOnce(idx, stamp)) {
            GatherEntry e;
            e.kind = GKind::kRigAssoc;
            e.aux = rig;
            e.coll = coll;
            GatherWriter w(g_gather);
            w.push(e);
        }
        ++tc.rigUnknown;
        return false;
    }
    switch (g_rigPub.decide(idx, verdict, stamp)) {
        case sealed::RigOutcome::kVetoed:
            ++tc.rigVetoed;
            ++tc.rigHitVetoed;
            if (padRig) padStageAdd(sealed::kPadRigVetoed);
            return true;
        case sealed::RigOutcome::kRefresh:
            ++tc.rigRefresh;
            ++tc.rigHitKept;
            if (padRig) padStageAdd(sealed::kPadRigRefresh);
            return false;
        case sealed::RigOutcome::kMixed:
            ++tc.rigMixed;
            ++tc.rigHitKept;
            if (padRig) padStageAdd(sealed::kPadRigMixed);
            return false;
        default:
            ++tc.rigUnknown;
            ++tc.rigHitKept;
            if (padRig) padStageAdd(sealed::kPadRigUnknown);
            return false;
    }
}

// The post-store cull of one record, with its fault handling: true when it was culled. A failed read or write leaves the record as the store wrote
// it (kept, counted as a fault); a torn put-back is counted as a fault too.
bool cullCounted(uintptr_t record, bool containing) noexcept {
    (void)containing;
    RecordFields fields{record};
    sealed::CullField failed = sealed::CullField::kMaskRead;
    uint64_t value = 0;
    switch (sealed::cullRecordFaultSafe(fields, &failed, &value)) {
        case sealed::Cull::kCulled:
            return true;   // the caller counts it (the thread's own counter)
        case sealed::Cull::kUntouched:
            faultNote(kFaultCullRead);
            return false;
        case sealed::Cull::kRestored:
            faultNote(kFaultCullRestored);
            return false;
        case sealed::Cull::kTorn:
            faultNote(kFaultCullTorn);
            return false;
    }
    return false;
}
// ---- The product's state: the setting, the seal, the release, the window counts ----------------------------------------------------------------
std::atomic<bool> g_setting{false};        // fix.optimized_stations is on (read at install and every kSettingPollFrames frames)
bool g_vetoesAttached = false;             // the producer and rig vetoes are attached (under g_installMutex-free single writer: the render tick)
bool g_storeWanted = false;                // the store observer is wanted (the setting is on)
std::atomic<bool> g_dockedNow{false};     // Status.json Flags bit 0 (Docked), read at each tick while known (the store thread reads it)
bool g_dockedWasNow = false;               // built v14.20 (review 4): the docked flag at the last tick (an undock is a lifecycle point)
std::atomic<bool> g_dockedKnownNow{false};// Status.json has a Flags sample (the waiting line says unknown until then)
// Built v14.35 (a v0.20.0 blocker): the seat from the same sample (Flags bits 24-26, Flags2 bits 1-2), and the raw words for the release line and the change instrument.
bool g_aboardNow = false;                 // seated, and known
bool g_statusKnownNow = false;
uint32_t g_statusFlagsNow = 0, g_statusFlags2Now = 0;
bool g_flagLogSet = false;                // the change instrument: the last words logged
uint32_t g_flagLogFlags = 0, g_flagLogFlags2 = 0;
bool g_flagLogKnown = false;
uint32_t g_flagLogN = 0;
char g_onFootText[320] = "on foot";
sealed::GameView g_waitLast;              // the waiting line last written (built v14.23: the game's pad state)
bool g_waitLastSet = false;
sealed::GameView g_waitCand;              // built v14.27: the key this tick had, and how long it has held (the line is written on a stable change)
bool g_waitCandSet = false;
sealed::StableGate g_waitStable;
uint64_t g_waitLastMs = 0;                // when the waiting line was last written (at most every 10 s while docked)
sealed::SealState g_sealState;             // the seal trigger (optimized_station_core.h sealStep)
bool g_flagHangarChanged = false;          // promote saw the held pad go or another own pad nearest: the tick releases on it
// Built v14.13: the sun inside the hangar (its own state, the skip read by the draw), the hangar watch, the names near a pad with no sized shell.
sealed::SunState g_sun;
sealed::HangarWatch g_hangarWatch;
char g_hangarWhy[160] = "searching for a hangar shell";
struct NameNote {
    char name[48] = {};
    float dist = 0.0f;
    float radius = 0.0f;
};
constexpr size_t kNameLogCap = 32;   // built 2026-10-23: was 12 with repeats, which cut the 10-23 MacLeod Market list before any Hanger name
NameNote g_nameLog[kNameLogCap];
size_t g_nameLogN = 0;
bool g_namesPending = false;
float g_namesPadC[3] = {};
uintptr_t g_hangarPad = 0;
// Built v14.12: the bay's mechanism, latched once per descent (core LatchSet). Candidates are gathered only while a latch is pending.
sealed::LatchSet g_latch;
sealed::LatchIdentity g_latchId;   // built v14.20 (review 2): the pad and shell the latch was built for
// Built v14.24: each latched record's pose at the seal. A release by the mechanism (the Lift, a hatch) re-seals on these poses (all back within the
// tolerance for kPoseBackFrames frames, with r1 1), not only on the r1 edge. Taken when the seal is admitted; the latch is the same set while sealed.
struct SealPoseRec {
    uintptr_t addr = 0;
    char name[48] = {};
    float rel[3] = {};
    float ax0[3] = {};
    float ax2[3] = {};
};
SealPoseRec g_sealPose[sealed::kLatchCap];
size_t g_sealPoseN = 0;
sealed::ChangeGate g_poseAwayGate;   // the away line, written when its text changes
sealed::ChangeGate g_poseBackGate;   // the back line, written once per release
uint64_t g_poseLastLogMs = 0;        // the away line is written at most 2 Hz

void snapshotSealPose() noexcept {
    g_sealPoseN = 0;
    for (size_t i = 0; i < g_latch.n && i < sealed::kLatchCap; ++i) {
        const sealed::LatchRec& r = g_latch.recs[i];
        SealPoseRec& p = g_sealPose[g_sealPoseN++];
        p.addr = r.addr;
        std::snprintf(p.name, sizeof(p.name), "%s", r.name);
        std::memcpy(p.rel, r.rel, sizeof(p.rel));
        std::memcpy(p.ax0, r.ax0, sizeof(p.ax0));
        std::memcpy(p.ax2, r.ax2, sizeof(p.ax2));
    }
}

// Built v14.23: the own pad, from the game (the object the docking reader r0 was last called on this dock). Reset at undock and hangar change.
sealed::GameTrack g_track;
// The pad in the shell's frame this frame (the sealed line's lateral and yaw).
sealed::ShellFrame g_frameShell;
sealed::LatchCand g_latchCands[sealed::kLatchCandCap];
size_t g_latchCandN = 0;
bool g_latchPending = false;         // docked, unsealed, after a descent, not yet latched
bool g_bayWatching = false;          // docked, unsealed, after a descent: the latch is kept
bool g_bayNoting = false;            // watching, and the shell read this frame: the latched records are read
uint64_t g_watchNowMs = 0;
float g_shellNow[3] = {};
uint8_t g_shellNowSrc = 4;
bool g_shellNowValid = false;
bool g_mechanismMoved = false;             // built v14.14: a latched bay mechanism moved while sealed
sealed::MechArm g_mechArm;                 // built v14.33: the mechanism release arms after the latched records were still for 60 informative passes
bool g_flagPadMoved = false;               // promote saw the held pad move along the shaft against the shell: the tick releases on it
char g_releaseWhy[160] = {};               // the release text for the flags above (the pad move or the hangar change, with its names)
// Built 2026-10-15: the transition hold. latchRead sets g_originShiftPending when the common frame shifted (this tick, before the seal step, which takes
// the flag once); g_originShiftM is its distance, for the release text. g_holdCause names what set the hold, for the one line the hold writes.
bool g_originShiftPending = false;
float g_originShiftM = 0.0f;
const char* g_holdCause = "origin shift";
bool g_holdLineOpen = false;               // the hold's line is written for this hold (cleared when a request is no longer held back)
sealed::SealTrace g_sealTrace;             // built 2026-10-15: the anomaly-armed seal trace (station_seal_trace.h); the tick feeds it one record a frame
uint32_t g_settingPoll = 0;                // frames since the setting was last read
uint64_t g_summaryStartMs = 0;             // the 30 s summary's window
uint64_t g_winFrames = 0;                  // ticks in the window
uint64_t g_winSealedFrames = 0;            // ticks in the window while sealed
uint64_t g_winDockedFrames = 0;            // built v14.31: ticks in the window while docked (the 30 s lines are written only for a window that was docked or sealed)
std::atomic<uint64_t> g_winRigVetoes{0};               // rig vetoes (before the forward) in the window
std::atomic<uint64_t> g_winProducerVetoes{0};          // records the producer-B veto skipped in the window
std::atomic<uint64_t> g_winCulled{0};                  // records culled after the store in the window
std::atomic<uint64_t> g_winKeptUnknown{0};             // records kept because their centre could not be read, in the window
std::atomic<uint64_t> g_winKeptReach{0};               // records the post-store judgement kept because their sphere reaches into the box (centre outside), in the window


void tcFlush(TlsCounters& t) noexcept {
    g_callsStore.fetch_add(t.callsStore, std::memory_order_relaxed);
    g_callsProducer.fetch_add(t.callsProducer, std::memory_order_relaxed);
    g_callsRig.fetch_add(t.callsRig, std::memory_order_relaxed);
    g_concCallsTotal[0].fetch_add(t.callsStore, std::memory_order_relaxed);
    g_concCallsTotal[1].fetch_add(t.callsProducer, std::memory_order_relaxed);
    g_concCallsTotal[2].fetch_add(t.callsRig, std::memory_order_relaxed);
    g_kept.fetch_add(t.kept, std::memory_order_relaxed);
    g_keptUnknown.fetch_add(t.keptUnknown, std::memory_order_relaxed);
    g_winKeptUnknown.fetch_add(t.keptUnknown, std::memory_order_relaxed);
    g_winKeptReach.fetch_add(t.keptReach, std::memory_order_relaxed);
    g_culled.fetch_add(t.culled, std::memory_order_relaxed);
    g_winCulled.fetch_add(t.culled, std::memory_order_relaxed);
    g_winNoCamera.fetch_add(t.noCamera, std::memory_order_relaxed);
    for (int k = 0; k < 5; ++k) g_worldSrc[k].fetch_add(t.worldSrc[k], std::memory_order_relaxed);
    g_winProducerVetoes.fetch_add(t.producerVetoes, std::memory_order_relaxed);
    g_winRigVetoes.fetch_add(t.rigVetoes, std::memory_order_relaxed);
    g_rigTally.seen.fetch_add(t.rigSeen, std::memory_order_relaxed);
    g_rigTally.vetoed.fetch_add(t.rigVetoed, std::memory_order_relaxed);
    g_rigTally.mixed.fetch_add(t.rigMixed, std::memory_order_relaxed);
    g_rigTally.unknown.fetch_add(t.rigUnknown, std::memory_order_relaxed);
    g_rigTally.refresh.fetch_add(t.rigRefresh, std::memory_order_relaxed);
    g_rigTally.hitVetoed.fetch_add(t.rigHitVetoed, std::memory_order_relaxed);
    g_rigTally.hitKept.fetch_add(t.rigHitKept, std::memory_order_relaxed);
    g_rigTally.hitMoved.fetch_add(t.rigHitMoved, std::memory_order_relaxed);
    g_rigTally.miss.fetch_add(t.rigMiss, std::memory_order_relaxed);
    g_rigTally.stale.fetch_add(t.rigStale, std::memory_order_relaxed);
    g_rigTally.asked.fetch_add(t.rigAsked, std::memory_order_relaxed);
    g_concWin.rigSeen.fetch_add(t.concRigSeen, std::memory_order_relaxed);
    g_concWin.rigVetoed.fetch_add(t.concRigVetoed, std::memory_order_relaxed);
    g_concWin.prodVetoed.fetch_add(t.concProdVetoed, std::memory_order_relaxed);
    g_concWin.prodKept.fetch_add(t.concProdKept, std::memory_order_relaxed);
    g_concWin.storeCleared.fetch_add(t.concStoreCleared, std::memory_order_relaxed);
    g_concWin.tot[0].fetch_add(t.concRigSeen, std::memory_order_relaxed);
    g_concWin.tot[1].fetch_add(t.concRigVetoed, std::memory_order_relaxed);
    g_concWin.tot[2].fetch_add(t.concProdVetoed, std::memory_order_relaxed);
    g_concWin.tot[3].fetch_add(t.concProdKept, std::memory_order_relaxed);
    g_concWin.tot[4].fetch_add(t.concKept[1], std::memory_order_relaxed);
    g_concWin.tot[5].fetch_add(t.concStoreCleared, std::memory_order_relaxed);
    for (int k = 0; k < 4; ++k) g_concWin.kept[k].fetch_add(t.concKept[k], std::memory_order_relaxed);
    g_concWin.roomProd.fetch_add(t.concRoomProd, std::memory_order_relaxed);
    g_concWin.roomStore.fetch_add(t.concRoomStore, std::memory_order_relaxed);
    const uint32_t keepStamp = t.stamp;
    const uint32_t s0 = t.sampleStore, s1 = t.sampleProducer, s2 = t.sampleRig;
    t = TlsCounters{};
    t.stamp = keepStamp;
    t.sampleStore = s0;
    t.sampleProducer = s1;
    t.sampleRig = s2;
}
uint32_t g_sealCount = 0;                  // seals since the setting was turned on (for the status lines)

// The sun pass the station skips while sealed (the pair the trial measured: VS 7E38A6AA1269C901 with PS 7CECABDE34FFBE9E).
constexpr uint64_t kSunVs = 0x7E38A6AA1269C901ull;
constexpr uint64_t kSunPs = 0x7CECABDE34FFBE9Eull;

// The pad must be at its lowest seen height, and still to within kSealStillM, for kSealDelayMs before the seal (the hatch closes in this time:
// a guess to be measured in flight; the maintainer will tell us the hatch close time).
// The camera leaves the held box by more than this and the seal is released (riding the elevator down, an external camera flying out).
constexpr uint32_t kSettingPollFrames = 30;
constexpr uint64_t kSummaryMs = 30000;
constexpr uint32_t kEngageTries = 90;
constexpr float kLoweredToleranceM = 15.0f;
constexpr uint32_t kRigRefreshFramesMax = 30;   // (the rig verdict refresh cadence is sealed::kRigRefreshN; kept here for the docs)

const char* sizeWord(sealed::PadSize s) noexcept {
    switch (s) {
        case sealed::PadSize::kSmall: return "small";
        case sealed::PadSize::kMedium: return "medium";
        case sealed::PadSize::kLarge: return "large";
        default: return "unknown-size";
    }
}

// Releases the seal: the held cell, its pad-local frame, the own record and the shell go; the vetoes stop from this frame (they test the
// atomic mirror). One status line says why. The next seal needs the whole seal condition again (still for kSealDelayMs at the lowest height).
// The release's words for the status line, by the rule that released.
const char* releaseText(sealed::ReleaseWhy w) noexcept {
    switch (w) {
        case sealed::ReleaseWhy::kSettingOff: return "the setting is off";
        case sealed::ReleaseWhy::kUndocked: return "undocked";
        case sealed::ReleaseWhy::kCameraLeft: return "the camera left the bay by more than 2 m (riding out of the hangar, or an external camera)";
        case sealed::ReleaseWhy::kPadMoved: return g_releaseWhy;
        case sealed::ReleaseWhy::kHangarChanged: return g_releaseWhy;
        case sealed::ReleaseWhy::kLaunch: return "launch (the pad's state object flipped)";
        case sealed::ReleaseWhy::kMechanism: return g_releaseWhy;
        case sealed::ReleaseWhy::kInHangarLeft: return "the own pad left the hangar (the game's r1 went from 1 to 0)";
        case sealed::ReleaseWhy::kPadMissing: return "the own pad's record was missing from the store's passes (the guard); no re-seal for the transition hold";
        case sealed::ReleaseWhy::kOnFoot: {   // the words at the moment of the release, with the bits that matter
            char t[300];
            sealed::describeStatusFlags(t, sizeof(t), g_statusKnownNow, g_statusFlagsNow, g_statusFlags2Now);
            std::snprintf(g_onFootText, sizeof(g_onFootText), "on foot, not seated in the ship (%s); the seal is for the cockpit only", t);
            return g_onFootText;
        }
        case sealed::ReleaseWhy::kOriginShift:   // built 2026-10-15: the transition hold; the text carries the shift and the hold's length
            std::snprintf(g_releaseWhy, sizeof(g_releaseWhy), "the coordinate origin shifted by %.1f m: the commander is boarding or disembarking "
                          "(a transition); no re-seal for %.1f s", static_cast<double>(g_originShiftM),
                          static_cast<double>(sealed::kTransitionHoldMs) / 1000.0);
            return g_releaseWhy;
        default: return "released";
    }
}

void releaseSeal(const char* why, uint32_t frame) noexcept {
    // Built v14.20 (review 3): the release is a state change: it runs under the state lock, serialized with the store callbacks.
    std::lock_guard<std::recursive_mutex> lock(g_sealedMutex);
    if (!g_heldValid) return;
    padStateEvent("release");   // built v14.22: the pad-state probe writes its line at the next tick
    Log::get().note("optimized station: released (frame %u): %s. Everything is drawn as without the station until the seal conditions hold again.",
                    frame, why);
    g_heldValid = false;
    g_heldCell = sealed::OwnCell{};
    g_heldLocalValid = false;
    g_shellValid = false;
    g_releaseMotion = false;
    g_ownRecAddr = 0;
    g_ownLiveValid = false;
    g_heldAbsent = 0;
    g_otherPicked = 0;
    g_engagePending = false;
    sealed::clearTrigger(&g_sealState);   // the pad's motion memory stays: a release by motion makes the next seal wait its delay (built v14.4)
    g_mechArm.reset();
    g_sealedFlag.store(false, std::memory_order_release);
    // the latch is kept across a release (built v14.16): undock, a hangar change and a ship change clear it
}

// Built v14.12: the latch is kept while docked, unsealed, after a descent; it is cleared on a hangar change, a ship change, an undock or a seal.
bool bayWatchKeep() noexcept {
    return g_setting.load(std::memory_order_acquire) &&   // kept through the seal too (built v14.14)
           g_dockedNow.load(std::memory_order_acquire);   // from docking, not only after a descent: a load-in seal latches too (built v14.15)
}

// Built v14.12, v14.26: a gathered record that may be a mechanism record (its name from the cache), within 400 m of the camera. Only while a latch is
// pending (the view says so). The tick applies it.
void latchCandNote(GatherWriter& w, uintptr_t record, uintptr_t node, const float* centre, const PadSnapshot& s, uint32_t flags) noexcept {
    if (!s.latchPending || (flags & sealed::kNodeMechanism) == 0) return;
    if (s.camValid && sealed::distance(centre, s.cam) > 400.0f) return;   // a bay's span reaches ~270 m along the shaft (built v14.15)
    char nm[64] = {};
    if (!cachedNameOfNode(node, nm, sizeof(nm)) || !sealed::mechanismName(nm)) return;
    w.push(gatherEntry(GKind::kLatch, record, centre, 0.0f, 0, nm));
}

// Built v14.28: the latch backstop's last samples (latchRead).
float g_latchLastWorld[sealed::kLatchCap][3] = {};
float g_latchLastShell[3] = {};
uint32_t g_latchLastFrame = 0;
size_t g_latchLastN = 0;
uint64_t g_winOriginShifts = 0;   // the rebases the backstop saw this window

// Built v14.12: latch the bay's mechanism once: the own pad's shaft (its up axis), the pads of its size and the mechanism records near the shaft.
void latchNow(uint64_t now) noexcept {
    float axes[3][3] = {};
    float ext[3] = {};
    bool extOk = false;
    uint8_t src = 4;
    if (!poseOrientOf(g_pickCell.pad, axes, ext, &extOk, &src)) return;
    const float up[3] = {axes[2][0], axes[2][1], axes[2][2]};
    static sealed::LatchCand cands[sealed::kPadSlots + sealed::kLatchCandCap];
    size_t n = 0;
    for (size_t i = 0; i < g_padCount && n < sealed::kPadSlots; ++i) {
        cands[n].addr = g_pads[i].addr;
        std::snprintf(cands[n].name, sizeof(cands[n].name), "%s", g_pads[i].name);
        for (int k = 0; k < 3; ++k) cands[n].centre[k] = g_pads[i].centre[k];
        ++n;
    }
    for (size_t i = 0; i < g_latchCandN; ++i) cands[n++] = g_latchCands[i];
    const float alongMin = -belowFor(g_pickCell.size);
    // The span: the target's distance when the shaft target is built, and never less than the bay's span (built v14.15).
    const float alongMax = g_pickCell.capsule ? std::max(g_pickCell.capLen + 10.0f, sealed::kLatchSpanM) : sealed::kLatchSpanM;
    sealed::LatchReject rej[10];
    size_t rejN = 0;
    const size_t k = sealed::selectMechanism(cands, n, g_pickCell.pad, g_pickCell.padName, g_pickCell.size, g_pickCell.centre, up, alongMin, alongMax,
                                             g_latch.recs, rej, 10, &rejN);
    g_latch.n = k;
    g_latchLastN = 0;   // built v14.28: the backstop's last world positions belong to the old set
    g_latch.latched = true;
    g_latchId.bind(g_pickCell.pad, g_preShellValid ? g_preShellAddr : 0);   // built v14.20 (review 2)
    g_latch.lastMotionMs = 0;
    char names[200] = {};
    size_t used = 0;
    for (size_t i = 0; i < k; ++i) {
        const int w = std::snprintf(names + used, sizeof(names) - used, "%s%s", i > 0 ? ", " : "", g_latch.recs[i].name);
        if (w < 0 || used + static_cast<size_t>(w) >= sizeof(names)) break;
        used += static_cast<size_t>(w);
    }
    Log::get().note("optimized station: watching the bay's mechanism (%u records): %s", static_cast<unsigned>(k), names);
    for (size_t i = 0; i < rejN; ++i) Log::get().note("optimized station: latch refused %s: %s", rej[i].name, rej[i].why);
}

// Built v14.12: each latched record is read by address in the tick (its world centre and orientation), against the shell read this frame. A record
// unreadable this frame is skipped and re-based on the next reading.
// Built v14.28: every record is measured in the hangar SHELL's frame (its centre and its two axes along the shell's own axes), with the shell read in this
// same call. The game's origin shifts (on foot to cockpit) moved every record by one vector (50.85 m) and the latch, which compared world positions and
// read the shell from an earlier part of the tick, released the seal as a launch (flight 2d4ba6db). The shell unreadable: no sample, the references hold.
// Backstop: when the shell and every latched record moved by the same vector since the last sample, it is a rebase: the references are re-based.
void latchRead(uint64_t now) noexcept {
    const uint32_t frame = g_frameStamp.load(std::memory_order_relaxed);
    const uintptr_t shellAddr = g_shellValid ? g_shellAddr : (g_preShellValid ? g_preShellAddr : 0);
    if (shellAddr == 0) return;
    const WorldRead sw = worldOfRecord(shellAddr);
    float sAx[3][3] = {};
    float sExt[3] = {};
    bool sExtOk = false;
    uint8_t sSrc = 4;
    if (sw.source == 4 || !poseOrientOf(shellAddr, sAx, sExt, &sExtOk, &sSrc)) return;   // the shell is unreadable in this pass: no sample
    struct Samp {
        bool ok = false;
        float world[3] = {};
        float rel[3] = {};
        float l0[3] = {};
        float l2[3] = {};
        uint8_t src = 4;
    };
    Samp samp[sealed::kLatchCap];
    bool allOk = true;
    for (size_t i = 0; i < g_latch.n && i < sealed::kLatchCap; ++i) {
        const WorldRead pw = worldOfRecord(g_latch.recs[i].addr);
        float axes[3][3] = {};
        float ext[3] = {};
        bool extOk = false;
        uint8_t src = 4;
        if (pw.source == 4 || !poseOrientOf(g_latch.recs[i].addr, axes, ext, &extOk, &src)) {
            allOk = false;
            continue;
        }
        Samp& s = samp[i];
        s.ok = true;
        s.src = pw.source;
        std::memcpy(s.world, pw.world, sizeof(s.world));
        sealed::shellLocalSample(pw.world, axes[0], axes[2], sw.world, sAx, s.rel, s.l0, s.l2);
    }
    bool shifted = false;
    if (allOk && g_latchLastFrame + 1 == frame && g_latchLastN == g_latch.n && g_latch.n > 0) {
        float deltas[sealed::kLatchCap][3];
        const float sd[3] = {sw.world[0] - g_latchLastShell[0], sw.world[1] - g_latchLastShell[1], sw.world[2] - g_latchLastShell[2]};
        for (size_t i = 0; i < g_latch.n; ++i) {
            for (int k = 0; k < 3; ++k) deltas[i][k] = samp[i].world[k] - g_latchLastWorld[i][k];
        }
        shifted = sealed::commonFrameShift(deltas, g_latch.n, sd, 1.0f, 0.1f);
        if (shifted) {
            ++g_winOriginShifts;
            const float shiftM = std::sqrt(sd[0] * sd[0] + sd[1] * sd[1] + sd[2] * sd[2]);
            g_originShiftM = shiftM;             // built 2026-10-15: the transition hold (the seal step takes the flag this tick, after this read)
            g_originShiftPending = true;
            // Built 2026-11-06: a rig's bound is a world-frame box, not re-based by a shift and not refreshed while unsealed; one left from before the shift reads as
            // wholly outside the cell and is vetoed. A new epoch starts every claim empty, and a rig with no bound forwards. (The tick holds g_sealedMutex.)
            bumpRigEpoch();
            Log::get().note("optimized station: the coordinate origin shifted by %.2f m (the shell and all %u latched records moved together); the bay's "
                            "motion references are re-based and this is not motion.",
                            static_cast<double>(shiftM), static_cast<unsigned>(g_latch.n));
        }
    }
    g_latchLastN = allOk ? g_latch.n : 0;
    g_latchLastFrame = frame;
    for (size_t i = 0; i < g_latch.n && i < sealed::kLatchCap; ++i) std::memcpy(g_latchLastWorld[i], samp[i].world, sizeof(g_latchLastWorld[i]));
    std::memcpy(g_latchLastShell, sw.world, sizeof(g_latchLastShell));
    for (size_t i = 0; i < g_latch.n && i < sealed::kLatchCap; ++i) {
        sealed::LatchRec& r = g_latch.recs[i];
        if (!samp[i].ok) continue;
        if (shifted) {
            r.stamp = 0;   // the next step re-bases without counting
            r.movedRun = 0;
        }
        r.step(frame, samp[i].rel, samp[i].l0, samp[i].l2, samp[i].src, sw.source, now);
    }
    g_latch.recomputeMotion();
}

// The seal's account for the status line (built v14.23): the game's pad state said the own pad was in the hangar (r1 1). The codes are the last
// read at the seal (the seal line prints them with the own pad's key).
const char* engageHow() noexcept {
    static char how[160];
    std::snprintf(how, sizeof(how), "sealed on the game's pad state: own pad r1 1 (in the hangar)");
    return how;
}

// Built v14.13, v14.26: a gathered record that may name the hangar, near the pad, while the names are wanted (the view says so). The tick applies it.
void hangarNameNote(GatherWriter& w, uintptr_t record, uintptr_t node, const float* centre, float radius, const PadSnapshot& s, uint32_t flags) noexcept {
    if (!s.namesPending || (flags & sealed::kNodeHangarLike) == 0) return;
    const float d = sealed::distance(centre, s.padC);
    if (d > 150.0f) return;
    char nm[64] = {};
    if (!cachedNameOfNode(node, nm, sizeof(nm)) || !sealed::hangarLikeName(nm)) return;
    GatherEntry e = gatherEntry(GKind::kHangar, record, centre, radius, 0, nm);
    e.dist = d;
    w.push(e);
}

// Built v14.13: the once-per-dock note with no sized shell: the hangar-like and pad records near the pad (the first carrier dock tells us its
// names), or, when there are none, that there is no hangar here.
void hangarNote() noexcept {
    if (g_nameLogN == 0) {
        std::snprintf(g_hangarWhy, sizeof(g_hangarWhy), "%s", "no hangar here (outpost or open pad): nothing to do");
        Log::get().note("optimized station: no hangar here (outpost or open pad): nothing to do");
        return;
    }
    std::snprintf(g_hangarWhy, sizeof(g_hangarWhy), "%s", "no sized hangar shell matched near the pad (names logged)");
    // Built 2026-10-23: nearest first, every name, over as many lines as it takes (a line holds 1200 bytes; the old single 400-byte line cut off).
    NameNote sorted[kNameLogCap];
    const size_t n = g_nameLogN < kNameLogCap ? g_nameLogN : kNameLogCap;
    for (size_t i = 0; i < n; ++i) sorted[i] = g_nameLog[i];
    std::sort(sorted, sorted + n, [](const NameNote& a, const NameNote& b) { return a.dist < b.dist; });
    size_t i = 0;
    for (int part = 1; i < n; ++part) {
        char text[900] = {};
        size_t used = 0;
        const size_t first = i;
        for (; i < n; ++i) {
            const int w = std::snprintf(text + used, sizeof(text) - used, "%s%s (%.1f m, r %.1f m)", i > first ? "; " : "", sorted[i].name,
                                        static_cast<double>(sorted[i].dist), static_cast<double>(sorted[i].radius));
            if (w < 0 || used + static_cast<size_t>(w) >= sizeof(text)) {
                text[used] = '\0';
                break;
            }
            used += static_cast<size_t>(w);
        }
        if (i == first) break;   // cannot happen (a name is under 48 bytes); never loop
        Log::get().note("optimized station: no sized hangar shell within 150 m of the pad; the hangar-like and pad records within 150 m, nearest "
                        "first (%u names, part %d): %s",
                        static_cast<unsigned>(n), part, text);
    }
}

// Built v14.17: a log value with two decimals, or "n/a" when it is unreadable.
void numOrNa(char* out, size_t cap, bool ok, float v) noexcept {
    if (ok) std::snprintf(out, cap, "%.2f", static_cast<double>(v));
    else std::snprintf(out, cap, "n/a");
}

// The seal's status line, once per seal (called from the engage, after the cell is held and localized).
void sealStatus(const sealed::OwnCell& cell, uint32_t frame) noexcept {
    ++g_sealCount;
    padStateEvent("seal");   // built v14.22: the pad-state probe writes its line at the next tick
    char lat[24] = {};
    char yaw[24] = {};
    numOrNa(lat, sizeof(lat), g_frameShell.ok, g_frameShell.lateral);   // the last frame's shell frame (built v14.17)
    numOrNa(yaw, sizeof(yaw), g_frameShell.yawOk, g_frameShell.yawDeg);
    Log::get().note("optimized station: sealed in %s hangar (%s, frame %u, seal %u): pad %s, box %.0f x %.0f x %.0f m (length, width, height), "
                    "floor %.0f m under the pad, shell %s, lateral %s m, yaw %s deg.", sizeWord(cell.size), engageHow(),
                    frame, g_sealCount, cell.padName,
                    static_cast<double>(cell.boxHalf[0] * 2.0f), static_cast<double>(cell.boxHalf[1] * 2.0f),
                    static_cast<double>(cell.boxHalf[2] * 2.0f), static_cast<double>(belowFor(cell.size)),
                    g_shellValid ? g_shellName : "none (the pad's motion is not checked)", lat, yaw);
}

// The 30 s summary: what the station did in the window. A user's log shows the station's work here.
void summaryLine(uint64_t now) noexcept {
    // Built v14.31: the lines are written for a window in which the commander was docked or sealed (a flight in space writes none); the window resets either way.
    const bool quiet = g_winDockedFrames == 0 && g_winSealedFrames == 0;
    // Built 2026-10-21: read and reset here, outside the note, so a window that writes no line starts the next one at 0. In the flat profile the skips are split by
    // how the draw path carried them out (a path that never ran reads 0; skipped above 0 with both at 0 means the report was never reached).
    char sunText[96];
    {
        const unsigned long long skipped = g_winSunDrawsSkipped.exchange(0, std::memory_order_relaxed);
        const unsigned long long notIssued = g_winSunNotIssued.exchange(0, std::memory_order_relaxed);
        const unsigned long long masked = g_winSunMasked.exchange(0, std::memory_order_relaxed);
        if (g_flatProfile.load(std::memory_order_relaxed))
            std::snprintf(sunText, sizeof(sunText), "%llu (not issued %llu, masked %llu)", skipped, notIssued, masked);
        else
            std::snprintf(sunText, sizeof(sunText), "%llu", skipped);
    }
#define SUMMARY_NOTE(...) \
    do {                  \
        if (!quiet) Log::get().note(__VA_ARGS__); \
    } while (0)
    SUMMARY_NOTE("optimized station: last %llu s: sealed for %llu of %llu frames; rigs vetoed early %llu, records vetoed early %llu, "
                    "records culled after the store %llu, records kept by reach %llu, records kept unread %llu, faults %llu (unreadable bucket %llu, cull read %llu, cull restored "
                    "%llu, cull torn %llu, tick view %llu; callbacks before the first camera %llu, not faults); sun draws skipped %s.",
                    static_cast<unsigned long long>((now - g_summaryStartMs) / 1000), static_cast<unsigned long long>(g_winSealedFrames),
                    static_cast<unsigned long long>(g_winFrames), static_cast<unsigned long long>(g_winRigVetoes),
                    static_cast<unsigned long long>(g_winProducerVetoes), static_cast<unsigned long long>(g_winCulled),
                    static_cast<unsigned long long>(g_winKeptReach.load()), static_cast<unsigned long long>(g_winKeptUnknown), static_cast<unsigned long long>(g_winFaults.load()),
                    static_cast<unsigned long long>(g_winFaultBy[kFaultBucket].load()), static_cast<unsigned long long>(g_winFaultBy[kFaultCullRead].load()),
                    static_cast<unsigned long long>(g_winFaultBy[kFaultCullRestored].load()),
                    static_cast<unsigned long long>(g_winFaultBy[kFaultCullTorn].load()),
                    static_cast<unsigned long long>(g_winFaultBy[kFaultTickView].load()), static_cast<unsigned long long>(g_winNoCamera.load()),
                    sunText);
    // Built v14.27: the passes the tick read, and what it did with them; the rig veto's lookups. A dead veto or a flapping pad shows here.
    {
        const sealed::PassGate& g = g_passGate;
        const double avg = g.informative > 0 ? static_cast<double>(g.callsSum) / static_cast<double>(g.informative) : 0.0;
        SUMMARY_NOTE("optimized station: last %llu s passes: %llu informative, %llu empty, %llu unfinished (a writer was still inside), %llu thin (under "
                        "half the usual calls); callbacks per informative pass min/avg/max %llu/%.0f/%llu; the own pad's record in %llu informative passes, "
                        "missing from %llu, missing from the whole window %llu; camera held through %llu failed resolutions, %llu jumps over %.0f m, %llu "
                        "context changes; rig publications %llu (%.0f rigs each); rig requests sent %llu; origin shifts re-based by the latch %llu.",
                        static_cast<unsigned long long>((now - g_summaryStartMs) / 1000), static_cast<unsigned long long>(g.informative),
                        static_cast<unsigned long long>(g.empty), static_cast<unsigned long long>(g.deferred), static_cast<unsigned long long>(g.thin),
                        static_cast<unsigned long long>(g.informative > 0 ? g.callsMin : 0), avg, static_cast<unsigned long long>(g.callsMax),
                        static_cast<unsigned long long>(g_winPadSeen), static_cast<unsigned long long>(g_winPadMissed),
                        static_cast<unsigned long long>(g_winPadAbsentWindow), static_cast<unsigned long long>(g_winCamHold),
                        static_cast<unsigned long long>(g_winCamJump), static_cast<double>(kCamJumpM), static_cast<unsigned long long>(g_winCtxChange),
                        static_cast<unsigned long long>(g_winRigPublications),
                        g_winRigPublications > 0 ? static_cast<double>(g_winRigPublished) / static_cast<double>(g_winRigPublications) : 0.0,
                        static_cast<unsigned long long>(g_rigTally.asked.load()), static_cast<unsigned long long>(g_winOriginShifts));
        g_winOriginShifts = 0;
        g_passGate.clearWindow();
        g_winPadSeen = g_winPadMissed = g_winPadAbsentWindow = g_winCamHold = g_winCamJump = g_winCtxChange = 0;
        g_winRigPublications = g_winRigPublished = 0;
    }
    // Built 2026-11-03 (log only): where the sealed pad leaves the pass in this window (station_seal_trace.h PadStage), read and reset here like the other window totals.
    {
        unsigned long long p[sealed::kPadStages];
        for (size_t k = 0; k < sealed::kPadStages; ++k) p[k] = g_padStage[k].exchange(0, std::memory_order_relaxed);
        static const char* const kPub[] = {"current", "not published", "earlier epoch", "collection moved"};
        const sealed::OwnRigTrace& o = g_ownRigTrace;
        const char* verdictText = o.verdict == 0 ? "unknown" : o.verdict == 1 ? "mixed" : o.verdict == 2 ? "outside" : "none";
        SUMMARY_NOTE("optimized station: last %llu s sealed pad: store callbacks %llu (left early %llu, pose unread %llu, gathered %llu); producer veto vetoed %llu, "
                        "kept %llu; its rig %s: rig veto vetoed %llu, refreshed %llu, mixed %llu, unknown %llu, not published %llu, earlier epoch %llu, "
                        "collection moved %llu; the tick's view: publication %s, verdict %s, run %u, bound %.1f m from the cell, radius %.1f m; "
                        "sticky pad is the sealed pad: %s.",
                        static_cast<unsigned long long>((now - g_summaryStartMs) / 1000), p[sealed::kPadStore], p[sealed::kPadEarly], p[sealed::kPadUnread],
                        p[sealed::kPadGathered], p[sealed::kPadProdVetoed], p[sealed::kPadProdKept], o.known ? "known" : "not learned yet", p[sealed::kPadRigVetoed],
                        p[sealed::kPadRigRefresh], p[sealed::kPadRigMixed], p[sealed::kPadRigUnknown], p[sealed::kPadRigMiss], p[sealed::kPadRigStale],
                        p[sealed::kPadRigMoved], o.known ? kPub[o.publication < 4 ? o.publication : 1] : "-", o.known ? verdictText : "-", static_cast<unsigned>(o.run),
                        o.bounded ? static_cast<double>(o.distM) : 0.0, o.bounded ? static_cast<double>(o.radiusM) : 0.0,
                        (g_heldValid && g_ownSticky.pad != 0 && g_ownSticky.pad == g_heldCell.pad) ? "yes" : (g_heldValid ? "NO" : "not sealed"));
    }
    // Built v14.20 (review 5), v14.31: the tables' occupancy now (written only when a probe bound was hit), the longest probe and the bounded (full) count this window, and the rig epoch count.
    if (g_prodStats.full.load(std::memory_order_relaxed) + g_rigStats.full + g_collStats.full > 0)
    SUMMARY_NOTE("optimized station: last %llu s tables: producer %u of %u used, longest probe %u, bounded %llu; rigs %u of %u, longest %u, "
                    "bounded %llu; collections %u of %u, longest %u, bounded %llu; rig epochs %llu (epoch %u).",
                    static_cast<unsigned long long>((now - g_summaryStartMs) / 1000), static_cast<unsigned>(prodOccupancy()),
                    static_cast<unsigned>(kProdSlots), static_cast<unsigned>(g_prodStats.maxProbes),
                    static_cast<unsigned long long>(g_prodStats.full), static_cast<unsigned>(g_rigStats.used),
                    static_cast<unsigned>(kRigSlots), static_cast<unsigned>(g_rigStats.maxProbes), static_cast<unsigned long long>(g_rigStats.full),
                    static_cast<unsigned>(g_collStats.used), static_cast<unsigned>(kCollSlots), static_cast<unsigned>(g_collStats.maxProbes),
                    static_cast<unsigned long long>(g_collStats.full), static_cast<unsigned long long>(g_rigEpochs),
                    static_cast<unsigned>(g_rigEpoch));
    // Built v14.25: the station's own costs over the window. Per call (one in 32 sampled): p50, p95 and max. Per frame: the calls each callback made, and
    // the estimated ms per frame (the sampled mean times the calls, over the frames). The frame tick per frame. The state lock's wait. Change-only waits.
    // Built v14.26: the callbacks' samples are drained by the tick; the entries lost in the window; the state-lock acquisitions a callback made (must be 0).
    {
        double s50 = 0.0, s95 = 0.0, smax = 0.0, p50 = 0.0, p95 = 0.0, pmax = 0.0, r50 = 0.0, r95 = 0.0, rmax = 0.0, f50 = 0.0, f95 = 0.0, fmax = 0.0;
        double meanStore = 0.0, meanProd = 0.0, meanRig = 0.0;
        size_t nStore = 0, nProd = 0, nRig = 0, nFrame = 0;
        uint64_t blocked = 0, waitingLines = 0;
        double waitTotal = 0.0, waitMax = 0.0;
        const uint64_t frames = g_winFrames;
        const uint64_t callsStore = g_callsStore.exchange(0, std::memory_order_relaxed);
        const uint64_t callsProd = g_callsProducer.exchange(0, std::memory_order_relaxed);
        const uint64_t callsRig = g_callsRig.exchange(0, std::memory_order_relaxed);
        nStore = g_costStore.used();
        meanStore = ringMean(g_costStore);
        sealed::samplePercentiles(g_costStore.v, nStore, &s50, &s95, &smax);
        nProd = g_costProducer.used();
        meanProd = ringMean(g_costProducer);
        sealed::samplePercentiles(g_costProducer.v, nProd, &p50, &p95, &pmax);
        nRig = g_costRig.used();
        meanRig = ringMean(g_costRig);
        sealed::samplePercentiles(g_costRig.v, nRig, &r50, &r95, &rmax);
        nFrame = g_costFrame.used();
        sealed::samplePercentiles(g_costFrame.v, nFrame, &f50, &f95, &fmax);
        blocked = g_mutexBlocked;
        waitTotal = g_mutexWaitTotalMs;
        waitMax = g_mutexWaitMaxMs;
        waitingLines = g_waitingLinesWritten;
        const uint64_t lost = g_winLost;
        const uint64_t onCallback = g_lockOnCallback.load(std::memory_order_relaxed);
        g_costStore.clear();
        g_costProducer.clear();
        g_costRig.clear();
        g_costFrame.clear();
        g_mutexBlocked = 0;
        g_mutexWaitTotalMs = 0.0;
        g_mutexWaitMaxMs = 0.0;
        g_waitingLinesWritten = 0;
        g_winLost = 0;
        g_lockOnCallback.store(0, std::memory_order_relaxed);
        const double fr = frames > 0 ? static_cast<double>(frames) : 1.0;
        SUMMARY_NOTE("optimized station: cost last %llu s: store observer %llu calls (%.1f per frame, ~%.4f ms per frame; p50/p95/max %.4f/%.4f/%.4f "
                        "ms per call, %llu sampled), producer veto %llu calls (%.1f per frame, ~%.4f ms per frame; %.4f/%.4f/%.4f ms per call, %llu "
                        "sampled), rig veto %llu calls (%.1f per frame, ~%.4f ms per frame; %.4f/%.4f/%.4f ms per call, %llu sampled), frame tick "
                        "%.3f/%.3f/%.3f ms per frame (%llu frames), mutex wait total %.3f ms max %.3f ms (blocked over 0.1 ms: %llu), waiting lines "
                        "written %llu; gathered entries lost %llu; state lock taken from a callback %llu; rig veto lookups: hit vetoed %llu, hit kept %llu, "
                        "collection moved %llu, miss %llu, stale epoch %llu (rig requests sent %llu); node names read from the game %llu, uncached node reads %llu, node reuse caught %llu.",
                        static_cast<unsigned long long>((now - g_summaryStartMs) / 1000), static_cast<unsigned long long>(callsStore),
                        static_cast<double>(callsStore) / fr, meanStore * static_cast<double>(callsStore) / fr, s50, s95, smax,
                        static_cast<unsigned long long>(nStore), static_cast<unsigned long long>(callsProd), static_cast<double>(callsProd) / fr,
                        meanProd * static_cast<double>(callsProd) / fr, p50, p95, pmax, static_cast<unsigned long long>(nProd),
                        static_cast<unsigned long long>(callsRig), static_cast<double>(callsRig) / fr, meanRig * static_cast<double>(callsRig) / fr,
                        r50, r95, rmax, static_cast<unsigned long long>(nRig), f50, f95, fmax, static_cast<unsigned long long>(nFrame), waitTotal,
                        waitMax, static_cast<unsigned long long>(blocked), static_cast<unsigned long long>(waitingLines),
                        static_cast<unsigned long long>(lost), static_cast<unsigned long long>(onCallback),
                        static_cast<unsigned long long>(g_rigTally.hitVetoed.load()), static_cast<unsigned long long>(g_rigTally.hitKept.load()),
                        static_cast<unsigned long long>(g_rigTally.hitMoved.load()), static_cast<unsigned long long>(g_rigTally.miss.load()),
                        static_cast<unsigned long long>(g_rigTally.stale.load()), static_cast<unsigned long long>(g_rigTally.asked.load()),
                        static_cast<unsigned long long>(g_nodeNameReads.exchange(0, std::memory_order_relaxed)),
                        static_cast<unsigned long long>(g_nodeCacheFull.exchange(0, std::memory_order_relaxed)),
                        static_cast<unsigned long long>(g_nodeReuseCaught.exchange(0, std::memory_order_relaxed)));
        g_rigTally.hitVetoed = 0;
        g_rigTally.hitKept = 0;
        g_rigTally.hitMoved = 0;
        g_rigTally.miss = 0;
        g_rigTally.stale = 0;
        g_rigTally.asked = 0;
    }

    g_prodStats.maxProbes.store(0, std::memory_order_relaxed);
    g_rigStats.maxProbes = g_collStats.maxProbes = 0;
    g_prodStats.full.store(0, std::memory_order_relaxed);
    g_rigStats.full = g_collStats.full = 0;
    g_rigEpochs = 0;
    g_summaryStartMs = now;
    g_winFrames = g_winSealedFrames = g_winDockedFrames = g_winRigVetoes = g_winProducerVetoes = 0;
    g_winCulled = g_winKeptUnknown = g_winKeptReach = g_winFaults = 0;
    g_winNoCamera = 0;
    for (auto& f : g_winFaultBy) f = 0;
}
#undef SUMMARY_NOTE

// The producer-B and rig vetoes: attached while the setting is on, and they answer only while sealed.
// Built 2026-11-05: the concourse cull's producer-B veto. Judges the record by name and sphere against the published camera: true skips it. Lock-free, as the
// sealed decision is. During the names survey it forwards every record (the store judges them and names them).
void concourseCountKept(TlsCounters& tc, cc::Judge j) noexcept {
    switch (j) {
        case cc::Judge::kKeepName: ++tc.concKept[0]; break;
        case cc::Judge::kKeepSmallClose: ++tc.concKept[1]; break;
        case cc::Judge::kKeepUnreadableName: ++tc.concKept[2]; break;
        case cc::Judge::kKeepUnreadableSphere: ++tc.concKept[3]; break;
        default: break;
    }
}

// The door rule: whether the record is one of the room's own (an allowed name, not the anchor) and its sphere lies wholly behind a shut door, by the published stretch. What is outside the
// room (the carrier's hull, what the bridge's windows show) is not the rule's: the cull's own rule has it, where it runs. The lift and the look keep their own rules.
bool roomSlabCulls(const PadSnapshot& s, const NodeInfo& ni, bool sphere, const float c[3], float r) noexcept {
    if (!s.room || !sphere || s.lift || s.look) return false;
    if ((ni.flags & sealed::kNodeConcourseAllow) == 0 || (ni.flags & sealed::kNodeConcourseFarExempt) != 0) return false;
    const float along = (c[0] - s.roomOrigin[0]) * s.roomAxis[0] + (c[1] - s.roomOrigin[1]) * s.roomAxis[1] + (c[2] - s.roomOrigin[2]) * s.roomAxis[2];
    cc::RoomSlab slab;
    slab.lo = s.roomLo;
    slab.hi = s.roomHi;
    return cc::roomSlabOutside(slab, along, r);
}
// The judgement of one record: the cull's own rule, or keep where the slab is the only rule (the live room), then the slab. `slabbed` says the slab culled what the rule kept.
cc::Judge concourseJudgeHere(const PadSnapshot& s, bool nameReadable, const NodeInfo& ni, bool sphere, const float c[3], float d, float r, bool* slabbed) noexcept {
    *slabbed = false;
    cc::Judge j = s.roomOnly && !s.lift && !s.look ? cc::Judge::kKeepName
                                                   : cc::concourseJudge(nameReadable, (ni.flags & sealed::kNodeConcourseAllow) != 0, sphere, d, r,
                                                                        (ni.flags & sealed::kNodeConcourseFarExempt) != 0, s.lift, s.look);
    if (cc::judgeKeeps(j) && roomSlabCulls(s, ni, sphere, c, r)) {
        j = cc::Judge::kCull;
        *slabbed = true;
    }
    return j;
}

// ---- TEMPORARY (H62 instrument, Flight 56): the VR bake's record diff ------------------------------------------------------------------------------------------------------
// Observe only. During the VR bake's aim frames that write (the frames the draw diff compares) every record the concourse judge sees whose sphere comes within 60 m of the room's centre,
// or whose sphere is unreadable, is noted with its name, sphere, verdict and the reason (the rule that decided it). Both judges note: the store's (afterStoreConcourse, path S) and the
// producer's (concourseProducerVeto, path P). The key is the name plus the sphere's centre rounded to 0.5 m plus the path: the record's address is shown beside each frame's verdict ("rec"),
// so a reader sees whether it is stable, but is not the key (a store record may be rewritten in another slot from frame to frame).
// Frames: the window of a frame opens in the aim tick (vrBakeAimTick, present thread) and closes at the next one, whatever thread judged in between; the number printed is the aim's frame,
// the one the capture and aim lines carry, with g_frameStamp (the judge's own counter, advanced at the top of every tick) read as the window opened beside it. A judge that runs a frame late
// lands in the next window. The judges only append under g_recMutex (when g_vrRecOn); the present thread's ticks sort and log, off the lock.
std::atomic<bool> g_vrRecOn{false};
// The room's centre in the store's world (the bake's origin), set as each window opens. The judges' own distance is from the store's camera, which in VR is the real head, not the
// centre (the world check: "the store's camera" is where the commander stands), so the band 20 to 50 m from the centre could be past 60 m from it; nearness is measured from here.
std::atomic<float> g_recCentre[3] = {};
constexpr float kRecNearM = 60.0f, kRecKeptNearM = 50.0f;
constexpr size_t kRecKeyCap = 4096, kRecDiffLines = 40, kRecKeptLines = 40;
struct RecEntry {
    float r = 0.0f, nearest = 0.0f;   // nearest: the sphere's surface from the centre, negative when the sphere is unreadable
    uint32_t kept = 0, culled = 0;
    const char* keptWhy = nullptr;
    const char* cullWhy = nullptr;
    uintptr_t rec = 0;                // the first record of the key this frame
};
struct RecWin {
    uint32_t aimFrame = 0, stamp = 0, face = 0, dwell = 0, total = 0, dropped = 0;
    std::unordered_map<std::string, RecEntry> m;
};
struct RecKeptNear {
    float r = 0.0f, nearest = 0.0f;
    uint32_t frames = 0;
    const char* why[2] = {nullptr, nullptr};
};
struct RecState {
    std::mutex m;
    bool open = false;
    RecWin cur;                                         // under m
    std::vector<RecWin> group;                          // present thread only: the face's finished windows
    std::unordered_map<std::string, RecKeptNear> kept;  // present thread only: the bake's records kept near
    uint32_t frames = 0;                                // present thread only: windows finished this bake
};
RecState& recState() {
    static RecState* s = new RecState;   // never destroyed: a callback may still be inside it at unload
    return *s;
}

const char* recReason(const PadSnapshot& s, cc::Judge j, bool slabbed, const NodeInfo& ni, bool nameReadable) noexcept {
    const bool allowed = (ni.flags & sealed::kNodeConcourseAllow) != 0;
    if (slabbed) return "door slab (the rule kept it)";
    if (s.roomOnly && !s.lift && !s.look) return "room only";
    if (s.look) {
        switch (j) {
            case cc::Judge::kKeepName: return "look: outside the room, kept";
            case cc::Judge::kKeepUnreadableName: return "look: unreadable-name outside, kept";
            default: return allowed ? "look: the room's name, culled" : "look: small and in reach, culled";
        }
    }
    switch (j) {
        case cc::Judge::kKeepName: return "name rule: allowed name";
        case cc::Judge::kKeepUnreadableName: return "unreadable-name";
        case cc::Judge::kKeepUnreadableSphere: return "unreadable-sphere";
        case cc::Judge::kKeepSmallClose: return "small-close";
        default: break;
    }
    if (s.lift) return "lift: not small and near";
    if (!nameReadable) return "unreadable name past 200 m";
    return allowed ? "allowed name past 200 m" : "name not allowed, not small-close";
}

// The judge's side: one record. Cheap when off (the callers test g_vrRecOn first).
void recNote(const PadSnapshot& s, uintptr_t record, uintptr_t node, const NodeInfo& ni, bool nameReadable, bool sphere, const float c[3], float d, float r, cc::Judge j,
             bool slabbed, char path) noexcept {
    try {
        const bool ok = sphere && std::isfinite(d) && std::isfinite(r) && std::isfinite(c[0]) && std::isfinite(c[1]) && std::isfinite(c[2]);
        // From the room's centre (g_recCentre), not the judge's d (from the store's camera, the real head in VR).
        float dc = 0.0f;
        if (ok) {
            const float dx = c[0] - g_recCentre[0].load(std::memory_order_relaxed), dy = c[1] - g_recCentre[1].load(std::memory_order_relaxed),
                        dz = c[2] - g_recCentre[2].load(std::memory_order_relaxed);
            dc = std::sqrt(dx * dx + dy * dy + dz * dz);
        }
        const float nearest = ok ? (dc - r > 0.0f ? dc - r : 0.0f) : -1.0f;
        if (ok && !(nearest <= kRecNearM)) return;
        char nm[64] = {};
        if (!(nameReadable && cachedNameOfNode(node, nm, sizeof(nm), &ni.validator))) std::snprintf(nm, sizeof(nm), "<unreadable>");
        char key[160];
        if (ok)
            std::snprintf(key, sizeof(key), "%s@(%.1f %.1f %.1f)[%c]", nm, std::floor(static_cast<double>(c[0]) * 2.0 + 0.5) / 2.0, std::floor(static_cast<double>(c[1]) * 2.0 + 0.5) / 2.0,
                          std::floor(static_cast<double>(c[2]) * 2.0 + 0.5) / 2.0, path);
        else
            std::snprintf(key, sizeof(key), "%s@(no sphere)[%c]", nm, path);
        const char* why = recReason(s, j, slabbed, ni, nameReadable);
        RecState& st = recState();
        std::lock_guard<std::mutex> lock(st.m);
        if (!st.open) return;
        ++st.cur.total;
        auto it = st.cur.m.find(key);
        if (it == st.cur.m.end()) {
            if (st.cur.m.size() >= kRecKeyCap) {
                ++st.cur.dropped;
                return;
            }
            it = st.cur.m.emplace(key, RecEntry()).first;
            it->second.r = ok ? r : 0.0f;
            it->second.nearest = nearest;
            it->second.rec = record;
        }
        RecEntry& e = it->second;
        if (cc::judgeKeeps(j)) {
            ++e.kept;
            e.keptWhy = why;
        } else {
            ++e.culled;
            e.cullWhy = why;
        }
    } catch (...) {
    }
}

// The present thread's side. A frame's text for one key: absent, or what the judge said of its records.
std::string recStateText(const RecEntry* e, bool withRec) {
    if (!e) return "absent";
    char b[200];
    std::string t;
    if (e->kept) {
        if (e->kept > 1) std::snprintf(b, sizeof(b), "kept x%u (%s)", e->kept, e->keptWhy ? e->keptWhy : "?");
        else std::snprintf(b, sizeof(b), "kept (%s)", e->keptWhy ? e->keptWhy : "?");
        t += b;
    }
    if (e->culled) {
        if (!t.empty()) t += ", ";
        if (e->culled > 1) std::snprintf(b, sizeof(b), "culled x%u (%s)", e->culled, e->cullWhy ? e->cullWhy : "?");
        else std::snprintf(b, sizeof(b), "culled (%s)", e->cullWhy ? e->cullWhy : "?");
        t += b;
    }
    if (withRec) {
        std::snprintf(b, sizeof(b), " rec %llX", static_cast<unsigned long long>(e->rec));
        t += b;
    }
    return t;
}

void recLogGroup(std::vector<RecWin>& g) noexcept {
    try {
        if (g.empty()) return;
        const size_t n = g.size();
        std::vector<std::string> keys;
        for (const RecWin& w : g)
            for (const auto& kv : w.m) keys.push_back(kv.first);
        std::sort(keys.begin(), keys.end());
        keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
        uint32_t within[3] = {}, keptN[3] = {}, dropped = 0, frames[3] = {}, stamps[3] = {};
        for (size_t f = 0; f < n && f < 3; ++f) {
            frames[f] = g[f].aimFrame;
            stamps[f] = g[f].stamp;
            dropped += g[f].dropped;
            for (const auto& kv : g[f].m) {
                within[f] += kv.second.kept + kv.second.culled;
                keptN[f] += kv.second.kept;
            }
        }
        struct Diff {
            std::string key;
            float r, nearest;
        };
        std::vector<Diff> diffs;
        for (const std::string& k : keys) {
            std::string first;
            bool differ = false;
            float r = 0.0f, nearest = 1.0e9f;
            for (size_t f = 0; f < n; ++f) {
                auto it = g[f].m.find(k);
                const std::string t = recStateText(it == g[f].m.end() ? nullptr : &it->second, false);
                if (f == 0) first = t;
                else if (t != first) differ = true;
                if (it != g[f].m.end()) {
                    if (r == 0.0f) r = it->second.r;
                    if (it->second.nearest < nearest) nearest = it->second.nearest;
                }
            }
            if (differ) diffs.push_back(Diff{k, r, nearest});
        }
        std::stable_sort(diffs.begin(), diffs.end(), [](const Diff& a, const Diff& b) { return a.nearest < b.nearest; });
        Log::get().note("concourse bake: vr record diff: face %u, frames %u %u %u (the judge's numbering): records within 60 m %u %u %u, kept %u %u %u, keys that differ %u (dropped %u) "
                        "[the frames are the aim's; the judge's stamps %u %u %u]",
                        g[0].face, frames[0], frames[1], frames[2], within[0], within[1], within[2], keptN[0], keptN[1], keptN[2], static_cast<unsigned>(diffs.size()), dropped,
                        stamps[0], stamps[1], stamps[2]);
        for (size_t i = 0; i < diffs.size() && i < kRecDiffLines; ++i) {
            std::string line;
            for (size_t f = 0; f < n; ++f) {
                auto it = g[f].m.find(diffs[i].key);
                char b[40];
                std::snprintf(b, sizeof(b), "%sframe %u ", f ? " | " : "", g[f].aimFrame);
                line += b;
                line += recStateText(it == g[f].m.end() ? nullptr : &it->second, true);
            }
            Log::get().note("concourse bake: vr record diff: face %u: %s r %.1f nearest %.1f m: %s", g[0].face, diffs[i].key.c_str(), static_cast<double>(diffs[i].r),
                            static_cast<double>(diffs[i].nearest), line.c_str());
        }
    } catch (...) {
    }
}

// A window's records into the bake's kept-near table and the face's group (logged when the next face begins).
void recAccept(RecWin&& w) noexcept {
    try {
        RecState& st = recState();
        ++st.frames;
        for (const auto& kv : w.m) {
            if (kv.second.kept == 0 || !(kv.second.nearest <= kRecKeptNearM)) continue;   // an unreadable sphere (nearest -1) is kept in the table
            RecKeptNear& k = st.kept[kv.first];
            ++k.frames;
            k.r = kv.second.r;
            k.nearest = kv.second.nearest;
            const char* why = kv.second.keptWhy;
            if (why != k.why[0] && why != k.why[1]) k.why[k.why[0] ? 1 : 0] = why;
        }
        if (!st.group.empty() && (st.group[0].face != w.face || w.dwell == 0 || st.group.size() >= 3)) {
            recLogGroup(st.group);
            st.group.clear();
        }
        st.group.push_back(std::move(w));
    } catch (...) {
    }
}

// The open window ends: taken out under the lock, handled outside it. Present thread.
void recClose() noexcept {
    try {
        RecState& st = recState();
        RecWin w;
        bool had = false;
        {
            std::lock_guard<std::mutex> lock(st.m);
            if (st.open) {
                g_vrRecOn.store(false, std::memory_order_relaxed);
                st.open = false;
                w = std::move(st.cur);
                st.cur = RecWin();
                had = true;
            }
        }
        if (had) recAccept(std::move(w));
    } catch (...) {
    }
}

// An aim that writes opens its window; the judge numbering is g_frameStamp as it stands now.
void recOpen(uint32_t aimFrame, uint32_t face, uint32_t dwell, const float centre[3]) noexcept {
    try {
        for (int k = 0; k < 3; ++k) g_recCentre[k].store(centre[k], std::memory_order_relaxed);
        RecState& st = recState();
        std::lock_guard<std::mutex> lock(st.m);
        st.cur = RecWin();
        st.cur.aimFrame = aimFrame;
        st.cur.stamp = g_frameStamp.load(std::memory_order_relaxed);
        st.cur.face = face;
        st.cur.dwell = dwell;
        st.open = true;
        g_vrRecOn.store(true, std::memory_order_relaxed);
    } catch (...) {
    }
}

// The bake's end (and its start, to clear): the last window, the last face, then once the records kept near the centre on any aim frame.
void recFlush(bool log) noexcept {
    try {
        recClose();
        RecState& st = recState();
        if (log) {
            recLogGroup(st.group);
            struct Row {
                std::string key;
                RecKeptNear k;
            };
            std::vector<Row> rows;
            for (const auto& kv : st.kept) rows.push_back(Row{kv.first, kv.second});
            std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.k.nearest < b.k.nearest; });
            for (size_t i = 0; i < rows.size() && i < kRecKeptLines; ++i) {
                char why[160];
                std::snprintf(why, sizeof(why), "%s%s%s", rows[i].k.why[0] ? rows[i].k.why[0] : "?", rows[i].k.why[1] ? ", " : "", rows[i].k.why[1] ? rows[i].k.why[1] : "");
                Log::get().note("concourse bake: vr record kept near: %s r %.1f nearest %.1f m: kept on %u of %u frames (%s)", rows[i].key.c_str(), static_cast<double>(rows[i].k.r),
                                static_cast<double>(rows[i].k.nearest), rows[i].k.frames, st.frames, why);
            }
            if (rows.size() > kRecKeptLines)
                Log::get().note("concourse bake: vr record kept near: %u more not listed", static_cast<unsigned>(rows.size() - kRecKeptLines));
        }
        st.group.clear();
        st.kept.clear();
        st.frames = 0;
    } catch (...) {
    }
}

bool concourseProducerVeto(uintptr_t rec) noexcept {
    SnapshotPin pin;
    const PadSnapshot& s = pin.get();
    if (!s.concourse || !s.camValid || s.lost) return false;
    TlsCounters& tc = tcFor(g_frameStamp.load(std::memory_order_relaxed));
    uintptr_t node = 0;
    const NodeInfo ni = nodeInfoOf(rec, &node);
    float centre[3] = {};
    float radius = 0.0f;
    const bool sphere = storeSphereOf(rec, centre, &radius);
    if (sphere) prodPutGen(rec, centre, radius, s.prodGen);
    const bool nameReadable = ni.cls != sealed::NameClass::kUnnamed && ni.cls != sealed::NameClass::kNone;
    const float d = sphere ? sealed::distance(centre, s.cam) : 0.0f;
    bool slabbed = false;
    const cc::Judge j = concourseJudgeHere(s, nameReadable, ni, sphere, centre, d, radius, &slabbed);
    if (slabbed) ++tc.concRoomProd;
    if (g_vrRecOn.load(std::memory_order_relaxed)) recNote(s, rec, node, ni, nameReadable, sphere, centre, d, radius, j, slabbed, 'P');   // TEMPORARY (H62 instrument, Flight 56)
    if (cc::judgeKeeps(j)) {
        ++tc.concProdKept;
        concourseCountKept(tc, j);
        return false;
    }
    ++tc.concProdVetoed;
    return true;
}

bool producerVetoBody(uintptr_t rec, uintptr_t owner) noexcept {
    (void)owner;
    if (!g_sealedFlag.load(std::memory_order_acquire)) {
        if (!g_concActive.load(std::memory_order_acquire) || g_concSurveyArmed.load(std::memory_order_relaxed)) return false;
        return concourseProducerVeto(rec);
    }
    // Built v14.25: no lock. The decision reads the published view (pinned), and the producer cache is lock-free.
    SnapshotPin pin;   // built v14.29: the thread's own hazard cell
    const PadSnapshot& s = pin.get();
    const bool veto = s.held && producerDecideSnap(rec, s);
    if (veto) ++tlsTc.producerVetoes;
    if (s.held && rec != 0 && rec == s.cell.pad) padStageAdd(veto ? sealed::kPadProdVetoed : sealed::kPadProdKept);   // log only, 2026-11-03
    return veto;
}

// The producer-B veto, timed one call in 32 (built v14.24; the sample is gathered since v14.26).
bool producerVetoHook(uintptr_t rec, uintptr_t owner) noexcept {
    CallbackScope scope;
    TlsCounters& tc = tcFor(g_frameStamp.load(std::memory_order_relaxed));
    ++tc.callsProducer;
    const bool sample = costSampleCall(tc.sampleProducer);
    const int64_t t0 = sample ? costTicks() : 0;
    const bool veto = producerVetoBody(rec, owner);
    if (sample) costPush(1, costMs(t0, costTicks()));
    return veto;
}

bool rigVetoBody(uintptr_t rig) noexcept {
    if (!g_sealedFlag.load(std::memory_order_acquire)) {
        // Built 2026-11-05: the concourse cull's rig veto, from the same publication; forwards while the names survey runs.
        if (!g_concActive.load(std::memory_order_acquire) || g_concSurveyArmed.load(std::memory_order_relaxed)) return false;
        TlsCounters& tc = tcFor(g_frameStamp.load(std::memory_order_relaxed));
        ++tc.concRigSeen;
        const bool cveto = rigVetoLockFree(rig);
        if (cveto) ++tc.concRigVetoed;
        return cveto;
    }
    const bool veto = rigVetoLockFree(rig);
    if (veto) ++tlsTc.rigVetoes;
    return veto;
}

// The rig early veto, timed one call in 32 (built v14.24; the sample is gathered since v14.26).
bool rigVetoHook(uintptr_t rig, uintptr_t ctx) noexcept {
    (void)ctx;
    CallbackScope scope;
    TlsCounters& tc = tcFor(g_frameStamp.load(std::memory_order_relaxed));
    ++tc.callsRig;
    const bool sample = costSampleCall(tc.sampleRig);
    const int64_t t0 = sample ? costTicks() : 0;
    const bool veto = rigVetoBody(rig);
    if (sample) costPush(2, costMs(t0, costTicks()));
    return veto;
}

// Attaches or detaches the vetoes and the store observer to the setting. Attach is refused (logged once) when a hook stands down.
// Built v14.20 (review 3): the toggle's hook operations run OUTSIDE the state lock (a detach may wait for callbacks, and a callback needs the lock).
// Off: the callbacks are detached and the gate closed first, so no new callback starts; then the state changes under the lock, which waits for a
// callback already inside. On: the state resets under the lock, then the hooks attach. Either way the rig and collection epoch moves on.
void applySetting(bool on, uint32_t frame) noexcept {
    if (on == g_vetoesAttached) return;
    if (!on) {
        kinematicEvalSetDirectVeto(nullptr);
        kinematicEvalSetRigVeto(nullptr);
        kinematicEvalDirectVetoDetach();
        kinematicEvalRigVetoDetach();
        g_storeWanted = false;
        storeHookRecompute();   // closes the gate before the state changes
    }
    {
        std::lock_guard<std::recursive_mutex> lock(g_sealedMutex);
        g_sealState = sealed::SealState{};   // a setting toggle starts the trigger afresh; the own pad (GameTrack) is kept (built v14.23)
        g_preShellValid = false;
        g_sealGuard = sealed::SealRateGuard{};
        bumpRigEpoch();                      // built v14.20 (review 4): the rig and collection tables start afresh
        if (!on && g_sun.off) {   // setting off: the sun comes back on (built v14.13)
            g_sun.off = false;
            detail::g_sunOff.store(false, std::memory_order_release);
            Log::get().note("optimized station: sun light inside the hangar back on (setting off; Status docked=%s)",
                            g_dockedNow.load(std::memory_order_acquire) ? "yes" : "no");
        }
        if (!on) releaseSeal("setting off", frame);
    }
    if (on) {
        const char* direct = kinematicEvalDirectVetoAttach();
        const char* rig = kinematicEvalRigVetoAttach();
        kinematicEvalSetDirectVeto(&producerVetoHook);
        kinematicEvalSetRigVeto(&rigVetoHook);
        g_storeWanted = true;
        storeHookRecompute();
        Log::get().note("optimized station: on (frame %u). Producer veto: %s; rig veto: %s; store observer: %s.", frame, direct, rig,
                        storeHookStatus());
    } else {
        Log::get().note("optimized station: off (frame %u).", frame);
    }
    padStateSetOn(on);   // built v14.22: the pad-state probe (temporary, log only) installs with the setting
    g_vetoesAttached = on;
}

// The oriented box for the pad at the freeze: the pad's own orientation and extent (its builder pose), the wiki footprint doubled along its length
// (the hangar is the pad doubled along one axis), the margin, and the floor. Without a pose or an extent there is no box (the capsule is the cell).
void buildBoxFor(sealed::OwnCell& cell, const float target[3]) {
    cell.box = false;
    if (!cell.has || cell.size == sealed::PadSize::kNone) return;
    float axes[3][3] = {};
    float ext[3] = {};
    bool extOk = false;
    uint8_t src = 4;
    if (!poseOrientOf(cell.pad, axes, ext, &extOk, &src) || !extOk) return;
    float u[3] = {target[0] - cell.centre[0], target[1] - cell.centre[1], target[2] - cell.centre[2]};
    const float ul = std::sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
    if (!(ul >= 1.0f)) return;
    for (float& v : u) v /= ul;
    int li = -1, wi = -1, ui = -1;
    if (!sealed::footprintAxes(axes, u, ext, &li, &wi, &ui)) return;
    const float* wiki = sealed::kPadWikiM[sealed::sizeIndex(cell.size)];
    const float halfLen = wiki[0] * 0.5f * 2.0f;
    const float halfWid = wiki[1] * 0.5f;
    sealed::buildBox(cell.centre, target, axes[li], halfLen, halfWid, g_margin, belowFor(cell.size), &cell);
}

// The promote: the frame's first record. The pad pick, the pad's heights, the capsule and box, the engage, the hangar-change drops.
// ---- Built v14.26: the tick applies the gathered entries of the previous pass, in the order the callbacks wrote them ------------------------------

// The rig note of one judged record: the rig, its bounds, and its name the first time. The job path's rig is the bucket's collection, resolved here with
// the bucket's one cache entry (a bucket's records come in a run).
void applyRigNote(const GatherEntry& e) {
    uintptr_t rig = e.aux;
    const bool jobPath = (e.flags & kFlagJob) != 0;
    if (!jobPath) {
        ++g_rigTally.assocSync;
    } else {
        if (e.bucket != 0 && e.bucket == g_rigBucketCache) {
            rig = g_rigBucketRig;
        } else {
            const uintptr_t owner = e.aux;
            rig = 0;
            if (e.bucket != 0 && owner > 0x300) rig = collFind(owner - 0x300);
            g_rigBucketColl = owner > 0x300 ? owner - 0x300 : 0;   // built v14.24: the collection this bucket names
            g_rigBucketCache = e.bucket;
            g_rigBucketRig = rig;
        }
        if (rig) ++g_rigTally.assocJob;
        else ++g_rigTally.assocNone;
    }
    if (!rig) return;
    RigSlot* s = rigFind(rig, true);
    if (!s) return;
    // Built v14.24: a job-path association is current only while the rig still holds that collection.
    if (jobPath && !sealed::collAssociationIs(s->coll, g_rigBucketColl)) return;
    // Log only, 2026-11-03: the rig the sealed pad's record was gathered in. The rig veto compares with g_ownRigA to count its outcomes for this rig.
    if (e.record != 0 && e.record == g_ownSticky.pad) {
        g_ownRig = rig;
        g_ownRigPad = g_ownSticky.pad;
        g_ownRigA.store(rig, std::memory_order_relaxed);
    }
    size_t pidx = 0;
    uint8_t pv = 0;
    uintptr_t pc = 0;
    if (g_rigPub.lookup(rig, rigIndex(rig), &pv, &pc, &pidx) && g_rigPub.evalStampAt(pidx) == e.stamp) ++g_rigTally.matched;
    else ++g_rigTally.unevaluated;
    if (s->curStamp != e.stamp) {
        if (s->curValid) {
            for (int k = 0; k < 3; ++k) {
                s->lastMn[k] = s->curMn[k];
                s->lastMx[k] = s->curMx[k];
            }
            s->lastValid = true;
        }
        if (s->curTally.judged > 0) s->lastTally = s->curTally;   // built 2026-11-05: the concourse judgements rotate with the bounds
        s->curStamp = e.stamp;
        s->curValid = false;
        s->curTally = cc::RigTally{};
    }
    if ((e.flags & kFlagJudged) != 0) s->curTally.note((e.flags & kFlagKeep) != 0);
    if (e.radius > 0.0f && std::isfinite(e.radius)) {   // a concourse judgement of a record with no readable sphere carries none (the hangar's notes always do)
        for (int k = 0; k < 3; ++k) {
            const float lo = e.centre[k] - e.radius, hi = e.centre[k] + e.radius;
            if (!s->curValid) {
                s->curMn[k] = lo;
                s->curMx[k] = hi;
            } else {
                if (lo < s->curMn[k]) s->curMn[k] = lo;
                if (hi > s->curMx[k]) s->curMx[k] = hi;
            }
        }
        s->curValid = true;
    }
    if (!s->named) {
        char name[64] = {};
        if (!cachedName(e.record, name, sizeof(name))) std::snprintf(name, sizeof(name), "(none)");
        sealed::rigRecordName(&s->named, s->name, sizeof(s->name), name);   // Built v13: the own-ship prefix test reads it
    }
}

// A rig's collection moved (its veto found the new one): its bounds and its first record's name are forgotten (the name decides the own-ship exemption, so it goes with the
// bounds it was read beside; release review 2026-10-14 finding 4), the association is recorded, and its run of vetoes starts afresh.
void applyRigAssoc(uintptr_t rig, uintptr_t coll) {
    RigSlot* s = rigFind(rig, true);
    if (!s || coll == s->coll) return;
    uint32_t run = 0;
    sealed::rigReassociate(&s->curValid, &s->lastValid, &s->verdict, &run, &s->named, s->name, sizeof(s->name));
    s->coll = coll;
    collInsert(coll, rig);
    size_t idx = 0;
    uint8_t v = 0;
    uintptr_t c = 0;
    if (g_rigPub.lookup(rig, rigIndex(rig), &v, &c, &idx)) g_rigPub.resetRun(idx);
}

void applyEntry(const GatherEntry& e) {
    switch (e.kind) {
        case GKind::kPad: {
            sealed::PadObs p;
            p.addr = e.record;
            for (int k = 0; k < 3; ++k) p.centre[k] = e.centre[k];
            p.radius = e.radius;
            p.size = static_cast<sealed::PadSize>(e.flags);
            std::snprintf(p.name, sizeof(p.name), "%s", e.name);
            if (p.size != sealed::PadSize::kNone) g_padWin.put(p);
            if (e.record == g_ownSticky.pad) g_stickyPadInPass = true;
            break;
        }
        case GKind::kExplicit: {
            sealed::ExplicitObs x;
            x.addr = e.record;
            x.node = e.aux;
            for (int k = 0; k < 3; ++k) x.centre[k] = e.centre[k];
            std::snprintf(x.name, sizeof(x.name), "%s", e.name);
            g_explicitWin.put(x);
            break;
        }
        case GKind::kNear:
            if (g_nearCurN < kNearSlots) {
                NearRec& nr = g_nearCur[g_nearCurN++];
                nr.addr = e.record;
                std::snprintf(nr.name, sizeof(nr.name), "%s", e.name);
                for (int k = 0; k < 3; ++k) nr.centre[k] = e.centre[k];
                nr.radius = e.radius;
                nr.dist = e.dist;
            }
            break;
        case GKind::kOwnCand:
            if (g_ownCandidateDist < 0.0f || e.dist < g_ownCandidateDist) {
                std::snprintf(g_ownCandidateName, sizeof(g_ownCandidateName), "%s", e.name);
                g_ownCandidateDist = e.dist;
            }
            break;
        case GKind::kTally:
            tallyShipPrefix(e.name);
            break;
        case GKind::kLatch:
            if (g_latchCandN < sealed::kLatchCandCap) {
                sealed::LatchCand& cand = g_latchCands[g_latchCandN++];
                cand.addr = e.record;
                std::snprintf(cand.name, sizeof(cand.name), "%s", e.name);
                for (int k = 0; k < 3; ++k) cand.centre[k] = e.centre[k];
            }
            break;
        case GKind::kHangar: {
            // Built 2026-10-23: one entry per name (its nearest instance), so repeats of one record across passes cannot fill the list.
            size_t i = 0;
            while (i < g_nameLogN && std::strcmp(g_nameLog[i].name, e.name) != 0) ++i;
            if (i == g_nameLogN) {
                if (g_nameLogN >= kNameLogCap) break;
                ++g_nameLogN;
            } else if (g_nameLog[i].dist <= e.dist) {
                break;
            }
            NameNote& nn = g_nameLog[i];
            std::snprintf(nn.name, sizeof(nn.name), "%s", e.name);
            nn.dist = e.dist;
            nn.radius = e.radius;
            break;
        }
        case GKind::kRigNote:
            applyRigNote(e);
            break;
        case GKind::kRigAssoc:
            applyRigAssoc(e.aux, e.coll);
            break;
        case GKind::kCost:
            if (e.flags == 0) costAdd(g_costStore, static_cast<double>(e.ms));
            else if (e.flags == 1) costAdd(g_costProducer, static_cast<double>(e.ms));
            else costAdd(g_costRig, static_cast<double>(e.ms));
            break;
        case GKind::kNone:
            break;
    }
}

// Drains the gathered entries (the tick only, under the lock). A loss in the drained pass is kept for the next publication.
void drainGather() {
    g_rigBucketCache = 0;
    g_rigBucketRig = 0;
    g_stickyPadInPass = false;
    const sealed::PassDrain d = g_gather.drain([](const GatherEntry& e) { applyEntry(e); });
    g_winLost += d.lost;
    // Built v14.27: the verdict on the drained passes. Only a complete pass that held a store callback is evidence; an empty or unfinished one leaves the
    // pad, the camera, the latch samples and the rig publication as they were.
    const uint64_t thinBefore = g_passGate.thin;
    const sealed::PassVerdict v = g_passGate.step(d.complete, d.passes, d.calls);
    g_passInformative = v == sealed::PassVerdict::kInformative;
    // Built 2026-10-15: the seal trace's pass fields (the verdict, and whether the pass counted as thin: the same count the 30 s line keeps).
    g_passThinLast = g_passGate.thin != thinBefore;
    g_passVerdictLast = v == sealed::PassVerdict::kInformative ? sealed::TraceVerdict::kInformative
                        : v == sealed::PassVerdict::kEmpty   ? sealed::TraceVerdict::kEmpty
                                                             : sealed::TraceVerdict::kUnfinished;
    g_passCalls = d.calls;
    g_recordsLast = static_cast<uint32_t>(d.calls > 0xFFFFFFFFull ? 0xFFFFFFFFull : d.calls);
    // A loss keeps every record in the next publication; it ends with the first informative pass that lost nothing.
    if (d.lost != 0) g_lossPublished = true;
    else if (g_passInformative) g_lossPublished = false;
    if (g_passInformative) {
        if (d.ctx != 0 && g_lastPassCtx != 0 && d.ctx != g_lastPassCtx) ++g_winCtxChange;
        if (d.ctx != 0) g_lastPassCtx = d.ctx;
        g_passCtx = d.ctx;
    } else {
        g_passCtx = 0;
    }
}

void promoteFrame(uint32_t stamp, uint32_t frame) {
    sealed::OwnCell next;
    g_ownCandidateDist = -1.0f;
    g_prefixTallyN = 0;   // the tally is this frame's records (built v14.8)
    // Built v14.29: a pass that did not sample the near list leaves the last sampled one standing (the engage reads it).
    if (g_nearCurN > 0 || g_engagePublished) {
        std::memcpy(g_nearPrev, g_nearCur, sizeof(g_nearPrev));
        g_nearPrevN = g_nearCurN;
    }
    g_nearCurN = 0;
    readOwnLive();
    refreshHeldWorld();
    if (g_frameCamValid) {
        const float radii[3] = {sealed::kCellDefaultM[0], sealed::kCellDefaultM[1], sealed::kCellDefaultM[2]};
        for (size_t i = 0; i < g_padCount; ++i) {
            float axes[3][3] = {};
            float ext[3] = {};
            bool extOk = false;
            uint8_t src = 4;
            g_pads[i].upOk = poseOrientOf(g_pads[i].addr, axes, ext, &extOk, &src);
            if (g_pads[i].upOk) {
                for (int k = 0; k < 3; ++k) g_pads[i].up[k] = axes[2][k];
            }
        }
        sealed::pickOwnPadSticky(g_pads, g_padCount, g_frameCam, radii, &g_ownSticky, &next);
        if (next.has) {
            float e2[3] = {};
            float axes[3][3] = {};
            float ext[3] = {};
            bool extOk = false;
            uint8_t src = 4;
            const bool haveAxes = poseOrientOf(next.pad, axes, ext, &extOk, &src);
            if (haveAxes) {
                for (int k = 0; k < 3; ++k) e2[k] = axes[2][k];
            }
            float q[3] = {};
            char qn[48] = {};
            float perp = 0.0f, along = 0.0f;
            bool picked = false;
            if (haveAxes) {
                picked = sealed::pickTargetOnLine(g_explicits, g_explicitCount, next.centre, e2, 0, q, qn, sizeof(qn), &perp, &along) ||
                         sealed::pickTargetOnLine(g_explicits, g_explicitCount, next.centre, e2, 1, q, qn, sizeof(qn), &perp, &along);
            }
            if (!picked) picked = sealed::pickCapsuleTarget(g_explicits, g_explicitCount, next.centre, q, qn, sizeof(qn));
            if (picked && sealed::buildCapsule(next.centre, q, sealed::kCapsuleExtendM, next.capA, next.capB, &next.capLen)) {
                next.capsule = true;
                std::snprintf(next.capTarget, sizeof(next.capTarget), "%s", qn);
                buildBoxFor(next, q);
            }
        }
    } else {
        std::snprintf(next.reason, sizeof(next.reason), "no camera for the previous frame");
    }
    g_pickCell = next;
    // The held pad: absent from the store for kHeldLostFrames frames, or another own pad nearest for that long, is a hangar change.
    if (g_heldValid) {
        bool heldSeen = false;
        const sealed::PadObs* best = nullptr;
        float bestD = 0.0f;
        for (size_t i = 0; i < g_padCount; ++i) {
            const sealed::PadObs& p = g_pads[i];
            if (p.addr == g_heldCell.pad) heldSeen = true;
            if (!g_frameCamValid) continue;
            const float d = sealed::distance(p.centre, g_frameCam);
            if (!sealed::ownPadCandidate(d, p.radius)) continue;
            if (!best || d < bestD) {
                best = &p;
                bestD = d;
            }
        }
        g_heldAbsent = heldSeen ? 0 : g_heldAbsent + 1;
        const bool otherPick = next.has && next.pad != g_heldCell.pad;   // the sticky pick: another pad only after the held one is unseen
        g_otherPicked = otherPick ? g_otherPicked + 1 : 0;
        if (sealed::heldCellDrops(g_heldAbsent, g_otherPicked, sealed::kHeldLostFrames)) {
            char why[160];
            if (g_heldAbsent >= sealed::kHeldLostFrames)
                std::snprintf(why, sizeof(why), "the hangar changed: the held pad %s is no longer in the store's records", g_heldCell.padName);
            else
                std::snprintf(why, sizeof(why), "the hangar changed: the nearest own pad is now %s", best ? best->name : "none");
            g_flagHangarChanged = true;
            std::snprintf(g_releaseWhy, sizeof(g_releaseWhy), "%s", why);
        }
    }
    if (g_releaseMotion && g_heldValid) {
        char why[160];
        std::snprintf(why, sizeof(why), "the pad moved %.1f m along the shaft relative to the hangar shell %s (more than %.0f m)",
                      static_cast<double>(g_shellDeltaM), g_shellName, static_cast<double>(sealed::kPadMoveReleaseM));
        g_flagPadMoved = true;
        std::snprintf(g_releaseWhy, sizeof(g_releaseWhy), "%s", why);
    }
    // The engage: the cell is frozen from the pad when the seal rule asked for it. Held frames take the held cell.
    if (g_engagePending) {
        ++g_engageTries;
        // Built v14.20 (review 1): the freeze needs the request still in force for this pad and shell, a lowered pad in the bay, and a valid cell.
        const sealed::EngageRequest req{g_engagePending, g_engagePad, g_engageShell};
        const bool cellValid = next.has && next.radius > 0.0f && sealed::cellInvalidWhy(next) == nullptr;
        const bool admitted = sealed::engageAdmitted(req, g_sealState, next.pad, g_preShellValid ? g_preShellAddr : 0, cellValid);
        if (admitted) {
            setOwnPrefixAtEngage(frame);
            pickOwnRecord(frame);
            g_heldCell = next;
            g_heldValid = true;
            bumpRigEpoch();                  // built 2026-11-06: the rig bounds of the unsealed time (possibly from before an origin shift) are dropped; no stale bound can veto
            for (auto& c : g_padStageSinceSeal) c.store(0, std::memory_order_relaxed);
            g_padMissingRun = 0;
            snapshotSealPose();              // built v14.24: each latched record's pose at the seal (v14.33: taken again when the mechanism release arms)
            g_mechArm.reset();               // built v14.33: mechanism motion neither releases nor counts until 60 still passes
            setHeldFrame(g_heldCell, frame);
            g_engagePending = false;
            g_sealedFlag.store(true, std::memory_order_release);
            sealStatus(g_heldCell, frame);
        } else if (!g_sealState.requested || (next.has && next.pad != g_engagePad)) {
            // The request was withdrawn, or it belongs to another pad: cancelled, and never frozen later (built v14.20, review 1).
            g_engagePending = false;
            g_engageTries = 0;
        } else if (g_engageTries >= kEngageTries) {
            // Built v14.8: the cell is not valid (no box, a zero box, no target), so no seal. The seal is requested again (the pad may gain a target);
            // the note is written once per distinct reason.
            g_engagePending = false;
            g_sealState.requested = false;
            const char* why = sealed::cellInvalidWhy(next);
            if (why == nullptr) why = next.reason;
            static char lastWhy[120] = {};
            if (std::strcmp(lastWhy, why) != 0) {
                std::snprintf(lastWhy, sizeof(lastWhy), "%s", why);
                Log::get().note("optimized station: no seal: the cell is not valid (%s); the station keeps drawing everything, and the seal is tried again.", why);
            }
            g_engageTries = 0;
        }
        if (!g_heldValid) next = sealed::OwnCell{};
    } else if (g_heldValid) {
        next = g_heldCell;
    } else {
        next = sealed::OwnCell{};
    }
    if (next.has && next.capsule)
        g_explicitPrevCount = sealed::pickExplicitSegment(g_explicits, g_explicitCount, next.capA, next.capB, g_explicitPrev, sealed::kExplicitNodes);
    else
        g_explicitPrevCount = next.has ? sealed::pickExplicit(g_explicits, g_explicitCount, next.centre, g_explicitPrev, sealed::kExplicitNodes) : 0;
    g_cellPrev = next;
    g_padWin.endPass();        // built v14.27: the windows age by one informative pass; a record unseen for kSeenKeep passes is gone
    g_explicitWin.endPass();
    g_cellStamp = stamp;
}

// Localizes the held cell to the held pad's frame and picks the shell. No logs: the seal's status line is written by the caller.
void setHeldFrame(const sealed::OwnCell& cell, uint32_t frame) {
    (void)frame;
    g_heldLocalValid = false;
    g_shellValid = false;
    g_releaseMotion = false;
    g_shellMotion = sealed::ShellMotion{};
    g_shellSampleStamp = 0xFFFFFFFFu;
    float axes[3][3] = {};
    float ext[3] = {};
    bool extOk = false;
    uint8_t src = 4;
    if (!poseOrientOf(cell.pad, axes, ext, &extOk, &src)) return;
    std::memcpy(g_engageAxes, axes, sizeof(g_engageAxes));
    std::memcpy(g_engageCentre, cell.centre, sizeof(g_engageCentre));
    sealed::localizeCell(cell, cell.centre, axes, &g_heldLocal);
    g_heldLocalValid = true;
    const size_t bestI = sealed::pickShell(g_explicits, g_explicitCount, cell.centre, cell.size, 150.0f);   // chosen once, here
    if (bestI < g_explicitCount) {
        const sealed::ExplicitObs& x = g_explicits[bestI];
        g_shellAddr = x.addr;
        std::snprintf(g_shellName, sizeof(g_shellName), "%s", x.name);
        g_shellValid = true;
    }
}

// Publishes the view the callbacks read. The tick only (under g_sealedMutex): the callbacks pin the ring and never write it.
void publishSnapshot() noexcept {
    PadSnapshot& s = g_snapRing.beginWrite();
    s.frameStamp = g_frameStamp.load(std::memory_order_relaxed);
    s.prodGen = g_prodGenA.load(std::memory_order_relaxed);
    s.held = g_heldValid;
    s.camValid = g_frameCamValid;
    std::memcpy(s.cam, g_frameCam, sizeof(s.cam));
    s.cell = g_cellPrev;
    s.explicitCount = g_explicitPrevCount < sealed::kExplicitNodes ? g_explicitPrevCount : sealed::kExplicitNodes;
    std::memcpy(s.explicitPrev, g_explicitPrev, sizeof(s.explicitPrev));
    std::memcpy(s.ownPrefix, g_ownPrefix, sizeof(s.ownPrefix));
    s.giveUp = g_hangarWatch.giveUp;
    s.latchPending = g_latchPending;
    s.namesPending = g_namesPending;
    std::memcpy(s.padC, g_namesPadC, sizeof(s.padC));
    s.ownLiveValid = g_ownLiveValid;
    std::memcpy(s.ownLiveCentre, g_ownLiveCentre, sizeof(s.ownLiveCentre));
    s.ownLiveRadius = g_ownLiveRadius;
    s.lost = g_lossPublished;
    s.engagePending = g_engagePending;
    s.concourse = g_concActive.load(std::memory_order_acquire);
    s.lift = g_liftGateOn.load(std::memory_order_acquire);
    s.look = g_camLookOn.load(std::memory_order_acquire);
    s.room = g_roomPub.room;
    s.roomOnly = g_roomPub.room && g_roomPub.only;
    s.roomRigs = g_roomPub.room && g_roomPub.rigs;
    std::memcpy(s.roomOrigin, g_roomPub.origin, sizeof(s.roomOrigin));
    std::memcpy(s.roomAxis, g_roomPub.axis, sizeof(s.roomAxis));
    s.roomLo = g_roomPub.slab.lo;
    s.roomHi = g_roomPub.slab.hi;
    s.walkOn = g_walkPassArmed;
    if (s.walkOn) s.walkCell = g_walkCellCopy;
    g_engagePublished = g_engagePending;
    g_snapRing.publish();
}

// The tick's view of the camera: resolved from the context the last callback saw (built v14.26: one frame behind the callbacks that read it). Up to
// kViewTries resolves while the centre the consumer uses is not ready, as the callbacks did.
void resolveTickView() noexcept {
    // Built v14.27: only an informative pass resolves the camera, from the context its busiest thread's callbacks saw (not "the last callback's"). An
    // empty or unfinished pass holds the camera as it was.
    if (!g_passInformative) return;
    const uintptr_t ctx = g_passCtx;
    if (ctx == 0) return;   // the pass named no context: nothing to resolve from; the camera is held
    view::resolveView(ctx, &g_view);
    for (uint32_t t = 1; t < kViewTries && !sealed::viewCentreReady(g_view.originOk); ++t)
        view::resolveView(ctx, &g_view);
    float cam[3] = {};
    const bool ok = cameraOf(g_view, cam);
    if (ok) {
        if (g_frameCamValid && sealed::distance(cam, g_frameCam) > kCamJumpM) ++g_winCamJump;
        if (g_frameCamValid) {
            const float step = sealed::distance(cam, g_frameCam);
            const bool lookStep = g_camLookOn.load(std::memory_order_relaxed) || g_camLookGraceTicks > 0;   // TEMPORARY: the look's own jumps, there and back
            if (g_camLookGraceTicks > 0) --g_camLookGraceTicks;
            if (cc::roomCut(step) && !lookStep) {   // built 2026-11-06: another room (the lift): the concourse tick forgets the anchor
                g_roomCutPending = true;
                g_roomCutM = step;
            }
        }
        std::memcpy(g_frameCam, cam, sizeof(g_frameCam));
        g_cameraKnown.store(true, std::memory_order_relaxed);
    } else {
        faultNote(kFaultTickView);
        ++g_winCamHold;
    }
    g_frameCamValid = g_camHold.resolved(ok);   // a failed resolution is covered by the last good camera for kCamHoldTicks passes
}

// The gathered rig sphere of one judged record: the rig on the synchronous path (the evaluation's own stack), or the bucket's owner on the job path (the
// tick resolves its collection). A record without a sphere is not gathered.
// Built 2026-11-05: `judged` (kFlagJudged, with kFlagKeep for a kept record) is the concourse cull's: its note is gathered with no sphere too (the rig veto must
// count a kept record whose sphere is unreadable); the hangar's notes pass 0 and are gathered only with a sphere, as before.
void gatherRigNote(GatherWriter& w, uintptr_t bucket, uintptr_t record, const float c[3], float r, uint32_t stamp, uint8_t judged = 0) noexcept {
    if (judged == 0 && (!(r > 0.0f) || !std::isfinite(r))) return;
    GatherEntry e = gatherEntry(GKind::kRigNote, record, c, r, stamp, nullptr);
    e.bucket = bucket;
    e.flags = judged;
    const uintptr_t rig = kinematicEvalCurrentRig();
    if (rig != 0) {
        e.aux = rig;
    } else {
        e.flags = static_cast<uint8_t>(e.flags | kFlagJob);
        uintptr_t owner = 0;
        if (bucket != 0) readVal(&owner, bucket + 0x10);
        e.aux = owner;
    }
    w.push(e);
}

// The store observer's judgement, for one record the store has written. Built v14.26: no lock. The callback reads the published view, gathers the parts
// the tick needs into its thread's slot, and judges from the view alone. Order: the centre (unreadable: kept), the gathered parts (pads, explicit parts,
// the near list, the own candidate, the ship's tally, the latch and the hangar names), then (judge only) the named keep, the bay keep, the own ship, and
// otherwise culled. judge: the sealed judgement; false: gathering only.
// ---- TEMPORARY (the lift survey, docs\concourse-cull-2026-11-05.md; log only, removed when the arc closes) --------------------------------------------------
// Do the lift's door records move when the doors do? The store callbacks note every record whose name contains "TurboLift" (the doors, the blocker, the bones)
// with every position the record can give: c (worldOfRecord, source s), m (the pose's own translation, pose+0x20), k (the store-style centre) and w (the raw
// rec+0x240). The tick (liftSurveyTick) logs the ones near the camera. Nothing here reaches a veto or a keep decision.
constexpr size_t kLiftSlots = 128;
struct LiftPart {
    uintptr_t addr = 0;
    char name[32] = {};
    float c[3] = {}, m[3] = {}, k[3] = {}, w[3] = {};   // a position that could not be read is NaN
    float r = 0.0f;
    uint8_t src = 4;
    uint32_t stamp = 0;                                 // g_frameStamp when last seen
    float lastC[3] = {NAN, NAN, NAN}, lastM[3] = {NAN, NAN, NAN}, lastK[3] = {NAN, NAN, NAN}, lastW[3] = {NAN, NAN, NAN};   // the values last logged
};
struct LiftAgg {
    std::mutex mtx;
    LiftPart parts[kLiftSlots];
    size_t used = 0;
    uint64_t overflow = 0;
};
LiftAgg& liftAgg() {
    static LiftAgg* a = new LiftAgg;   // never destroyed: a callback may still be inside it at unload
    return *a;
}
std::atomic<bool> g_liftSurveyOn{false};   // the tick's: the survey is armed (the callbacks read it relaxed)

void liftSurveyNote(uintptr_t record, uintptr_t node, const NodeInfo& ni) noexcept {
    try {
        char nm[64] = {};
        if (!cachedNameOfNode(node, nm, sizeof(nm), &ni.validator) || !cc::liftPartName(nm)) return;
        const WorldRead wr = worldOfRecord(record);
        if (wr.source == 4 || !std::isfinite(wr.world[0]) || !std::isfinite(wr.world[1]) || !std::isfinite(wr.world[2]) || !std::isfinite(wr.radius)) return;
        float m[3] = {NAN, NAN, NAN}, k[3] = {NAN, NAN, NAN}, w[3] = {NAN, NAN, NAN};
        uintptr_t pose = 0;
        float t[4] = {};
        if (readVal(&pose, record + kRecPose) && pose && readMem(t, pose + kPoseT, sizeof(t)) && sealed::finite3(t)) {
            m[0] = t[0];
            m[1] = t[1];
            m[2] = t[2];
        }
        float kr = 0.0f;
        if (!storeSphereOf(record, k, &kr)) k[0] = k[1] = k[2] = NAN;
        float raw[3] = {};
        if (readMem(raw, record + kRecCentre, sizeof(raw)) && sealed::finite3(raw)) {
            w[0] = raw[0];
            w[1] = raw[1];
            w[2] = raw[2];
        }
        const uint32_t stamp = g_frameStamp.load(std::memory_order_relaxed);
        LiftAgg& a = liftAgg();
        std::lock_guard<std::mutex> lock(a.mtx);
        LiftPart* p = nullptr;
        for (size_t i = 0; i < a.used; ++i) {
            if (a.parts[i].addr == record) {
                p = &a.parts[i];
                break;
            }
        }
        if (!p) {
            if (a.used >= kLiftSlots) {
                ++a.overflow;
                return;
            }
            p = &a.parts[a.used++];
            *p = LiftPart();
            p->addr = record;
        }
        std::snprintf(p->name, sizeof(p->name), "%s", nm);
        for (int i = 0; i < 3; ++i) {
            p->c[i] = wr.world[i];
            p->m[i] = m[i];
            p->k[i] = k[i];
            p->w[i] = w[i];
        }
        p->r = wr.radius;
        p->src = wr.source;
        p->stamp = stamp;
    } catch (...) {
    }
}

// The anchor's sighting (and the hub's, while the tick wants it): whatever path the store callback takes, this is how the cull finds its room. Shared by the
// concourse path and the seal's gather path (a docked, on-foot pass with the cull not active), so the pad's gathers and the anchor's sighting both go on.
void concourseSight(uintptr_t record, uintptr_t node, const NodeInfo& ni) noexcept {
    if (g_liftSurveyOn.load(std::memory_order_relaxed) && (ni.flags & sealed::kNodeLiftPart) != 0) liftSurveyNote(record, node, ni);   // the lift gate's input and the survey's log
    if ((ni.flags & sealed::kNodeConcourseAnchor) != 0) {
        g_concAnchorAddr.store(record, std::memory_order_relaxed);
        g_concAnchorMs.store(GetTickCount64(), std::memory_order_relaxed);
    } else if ((ni.flags & sealed::kNodeConcourseAllow) != 0) {
        const bool hubWanted = g_concHubWanted.load(std::memory_order_relaxed);
        const char* liveWanted = g_concLiveWanted.load(std::memory_order_relaxed);
        const bool leafWanted = g_doorLeafWanted.load(std::memory_order_relaxed);
        if (!hubWanted && !liveWanted && !leafWanted) return;
        char hn[64] = {};
        if (!cachedNameOfNode(node, hn, sizeof(hn), &ni.validator)) return;
        if (hubWanted && std::strcmp(hn, cc::kHubName) == 0) g_concHubAddr.store(record, std::memory_order_relaxed);
        else if (liveWanted && std::strcmp(hn, liveWanted) == 0) g_concLiveAddr.store(record, std::memory_order_relaxed);
        else if (leafWanted && (std::strcmp(hn, cc::kDoorLeafLeft) == 0 || std::strcmp(hn, cc::kDoorLeafRight) == 0)) {
            // A door's leaf: into the first free slot unless it is already held. Two callbacks may both take a slot for one record; the tick reads both alike.
            for (std::atomic<uintptr_t>& slot : g_doorLeaf)
                if (slot.load(std::memory_order_relaxed) == record) return;
            for (std::atomic<uintptr_t>& slot : g_doorLeaf) {
                uintptr_t expect = 0;
                if (slot.compare_exchange_strong(expect, record, std::memory_order_relaxed)) return;
            }
        }
    }
}

// The sealed store's keep decision for one record against `cell`: the named-keep rule, the bay volume, the own ship's exemption and the live own sphere. The
// sealed judge passes the held cell; the walk survey (log only) passes the pick cell, so both use the one rule.
bool storeKeeps(const PadSnapshot& s, const sealed::OwnCell& cell, uintptr_t record, uintptr_t node, const NodeInfo& ni, const float* centre, float radius, const float* cam,
                bool containsCam, sealed::NamedKeep* out) noexcept {
    const sealed::NamedKeep v = sealed::keepNamedCell(centre, radius, cam, kFallbackSphereM, cell, s.explicitPrev, s.explicitCount, record);
    *out = v;
    bool keep = v.keep;
    // The bay: the own hangar's parts inside the bay volume, even where the box alone would cull them. The shaft is the cell's.
    if (!keep && cell.capsule) {
        float u[3] = {};
        if (sealed::unitBetween(cell.capA, cell.capB, u) && sealed::inBayVolume(centre, cell.capA, u, cell.capLen, belowFor(cell.size))) {
            char bn[64] = {};
            if (cachedNameOfNode(node, bn, sizeof(bn), &ni.validator) && sealed::bayPartName(bn)) keep = true;
        }
    }
    if (!keep) {
        char nm[64] = {};
        if (s.ownPrefix[0] != '\0') cachedNameOfNode(node, nm, sizeof(nm), &ni.validator);
        if (sealed::ownShipExempt(containsCam, radius, nm, s.ownPrefix)) keep = true;
    }
    if (!keep && s.ownLiveValid && sealed::ownSphereKeeps(centre, s.ownLiveCentre, s.ownLiveRadius, kOwnSphereMarginM)) keep = true;
    return keep;
}

// ---- TEMPORARY (the walk survey, docs\concourse-cull-2026-11-05.md; log only, removed when the arc closes) ----------------------------------------------------
// While the key is armed and the ship is docked: one line a second about where the commander is, and, every 5 s and at a zone change while unsealed with a pick
// cell and out of the concourse, a names report: what the seal's store judgement would cull against the pick cell, from one store pass. The cell reaches the
// callbacks as the snapshot's walkCell (never as `cell` or `held`, which the vetoes read), and only while walkOn is set.
struct WalkAgg {
    struct Name {
        uint32_t n = 0;
        float nearest = 0.0f;
        float radius = 0.0f;
    };
    std::mutex m;
    uint32_t total = 0, culled = 0;
    std::unordered_map<std::string, Name> names;   // would-cull names within kWalkNearM
};
WalkAgg& walkAgg() {
    static WalkAgg* a = new WalkAgg;   // never destroyed: a callback may still be inside it at unload
    return *a;
}
constexpr float kWalkNearM = 300.0f;

void walkSurveyRecord(const PadSnapshot& s, uintptr_t record, uintptr_t node, const NodeInfo& ni, const float* cam) noexcept {
    try {
        const WorldRead wr = worldOfRecord(record);
        const bool sphere = wr.source != 4 && std::isfinite(wr.radius) && wr.radius > 0.0f && std::isfinite(wr.world[0]) && std::isfinite(wr.world[1]) &&
                            std::isfinite(wr.world[2]);
        bool cull = false;
        float surface = 0.0f;
        if (sphere) {
            sealed::NamedKeep nk;
            cull = !storeKeeps(s, s.walkCell, record, node, ni, wr.world, wr.radius, cam, sealed::containsCamera(wr.world, wr.radius, cam), &nk);
            surface = std::max(0.0f, sealed::distance(wr.world, cam) - wr.radius);
        }
        char nm[64] = {};
        if (cull && surface <= kWalkNearM && !cachedNameOfNode(node, nm, sizeof(nm), &ni.validator)) std::snprintf(nm, sizeof(nm), "unnamed");
        WalkAgg& a = walkAgg();
        std::lock_guard<std::mutex> lock(a.m);
        ++a.total;
        if (!cull) return;
        ++a.culled;
        if (surface > kWalkNearM) return;
        WalkAgg::Name& e = a.names[nm];
        if (e.n == 0 || surface < e.nearest) e.nearest = surface;
        if (wr.radius > e.radius) e.radius = wr.radius;
        ++e.n;
    } catch (...) {
    }
}

void afterStoreJudge(uintptr_t bucket, uintptr_t record, bool judge) noexcept {
    // Built v14.27: the callback is one Writer. Its entries all go to the pass of the stamp it read on entry, and the pass is not read while it is inside.
    GatherWriter w(g_gather);
    w.call(0);
    TlsCounters& tc = tcFor(g_frameStamp.load(std::memory_order_relaxed));   // built v14.29: the thread's own counters
    SnapshotPin pin;
    const PadSnapshot& s = pin.get();
    if (judge && !s.held) return;
    // Log only, 2026-11-03: the sealed pad's own store callbacks, and how many leave before the pad is gathered (kPadStage* in station_seal_trace.h).
    const bool isPad = s.held && record != 0 && record == s.cell.pad;
    if (isPad) padStageAdd(sealed::kPadStore);
    uintptr_t ctx = 0;
    uint32_t count = 0;
    if (!bucket || !record || !readVal(&ctx, bucket) || !ctx || !readVal(&count, ctx + kCtxCount) || count == 0) {
        faultNote(kFaultBucket);
        if (isPad) padStageAdd(sealed::kPadEarly);
        return;
    }
    w.context(ctx);   // names the context for the pass (the call itself was counted above)
    if (!s.camValid) {
        // Not a fault: the first pass after the gate opens has no camera yet (v14.26 counted the whole pass, 1268 records, as faults here).
        ++tc.noCamera;
        if (isPad) padStageAdd(sealed::kPadEarly);
        return;
    }
    const float* cam = s.cam;
    const uint32_t stamp = w.stamp();
    const bool lost = s.lost || w.lostSinceDrain() != 0;   // a loss keeps every record (fail-safe)
    // Built v14.29: the node first. Its class and flags come from the node cache (the name is read once per node per kClassKeyFrames frames). An unsealed
    // pass that does not sample the near list, the own candidate and the tally reads the centre only of the nodes that can matter (pads, explicit parts,
    // and the mechanism and hangar names while they are wanted); every other record returns here.
    uintptr_t node = 0;
    const NodeInfo ni = nodeInfoOf(record, &node);
    // Built 2026-11-06: a docked, unsealed pass with the cull wanted but not active (on foot in the hangar) goes this way, not the concourse one: the anchor's
    // sighting goes on from here, so the cull can re-arm, and the pad's gathers below are as they are with the key off.
    if (!judge && g_concWanted.load(std::memory_order_relaxed)) concourseSight(record, node, ni);
    else if (!judge && g_liftSurveyOn.load(std::memory_order_relaxed) && (ni.flags & sealed::kNodeLiftPart) != 0) liftSurveyNote(record, node, ni);   // the lift gate's input on the hangar side
    if (!judge && s.walkOn) walkSurveyRecord(s, record, node, ni, cam);   // log only: one relaxed load off the survey's pass
    const bool full = sealed::fullGatherPass(stamp, s.engagePending, s.held);
    if (!judge && !sealed::nodeWantsCentre(ni.cls, ni.flags, s.giveUp, s.latchPending, s.namesPending, full)) {
        return;
    }
    const WorldRead wr = worldOfRecord(record);
    if (judge) {
        ++tc.worldSrc[wr.source];
        if (wr.source != 4) gatherRigNote(w, bucket, record, wr.world, wr.radius, stamp);
    }
    const float* centre = wr.world;
    const float radius = wr.radius;
    if (wr.source == 4 || !std::isfinite(radius) || !(radius > 0.0f) || !std::isfinite(centre[0]) || !std::isfinite(centre[1]) ||
        !std::isfinite(centre[2])) {
        if (judge) ++tc.keptUnknown;   // the centre cannot be read: kept, never judged
        if (isPad) padStageAdd(sealed::kPadUnread);
        return;
    }
    if (ni.cls == sealed::NameClass::kPad) {
        char name[64] = {};
        if (cachedNameOfNode(node, name, sizeof(name), &ni.validator)) {
            const sealed::PadSize size = sealed::padSizeOf(name);
            if (size != sealed::PadSize::kNone) {
                GatherEntry e = gatherEntry(GKind::kPad, record, centre, radius, stamp, name);
                e.flags = static_cast<uint8_t>(size);
                if (w.push(e) && isPad) padStageAdd(sealed::kPadGathered);
            }
        }
    } else if (ni.cls == sealed::NameClass::kExplicit && !s.giveUp) {   // built v14.13: stops after the give-up
        char xn[64] = {};
        cachedNameOfNode(node, xn, sizeof(xn), &ni.validator);   // no name: the explicit part is still gathered, with an empty name
        GatherEntry e = gatherEntry(GKind::kExplicit, record, centre, radius, stamp, xn);
        e.aux = node;
        w.push(e);
    }
    // The own ship: the own candidate (the nearest small containing sphere), the prefix rule, then the live own sphere. Built v14.29: the near list, the
    // candidate and the tally are gathered on a full pass only (every kSamplePass-th stamp, and every stamp while sealed or while an engage is pending).
    const bool containsCam = sealed::containsCamera(centre, radius, cam);
    const float dCam = sealed::distance(centre, cam);
    if (full) {
        if (dCam <= kOwnNearM) {
            char nn[64] = {};
            if (cachedNameOfNode(node, nn, sizeof(nn), &ni.validator)) {
                GatherEntry e = gatherEntry(GKind::kNear, record, centre, radius, stamp, nn);
                e.dist = dCam;
                w.push(e);
            }
        }
        if (containsCam && radius < sealed::kOwnShipRadiusM) {
            char cn[64] = {};
            if (cachedNameOfNode(node, cn, sizeof(cn), &ni.validator)) {
                GatherEntry e = gatherEntry(GKind::kOwnCand, record, centre, radius, stamp, cn);
                e.dist = dCam;
                w.push(e);
            }
        }
        // The ship's part count by prefix (built v14.8). A name with no prefix counts for nothing, so a node without the prefix flag is not read.
        if (dCam <= sealed::kShipRecordsM && (ni.flags & sealed::kNodeShipPrefix) != 0) {
            char tn[64] = {};
            if (cachedNameOfNode(node, tn, sizeof(tn), &ni.validator)) w.push(gatherEntry(GKind::kTally, record, centre, radius, stamp, tn));
        }
    }
    latchCandNote(w, record, node, centre, s, ni.flags);   // built v14.12: gathered only while a latch is pending
    hangarNameNote(w, record, node, centre, radius, s, ni.flags);   // built v14.13: only while the names are wanted
    if (!judge) return;   // gathering only: the judgement is sealed-only
    if (lost) {
        ++tc.kept;
        return;
    }
    sealed::NamedKeep v;
    const bool keep = storeKeeps(s, s.cell, record, node, ni, centre, radius, cam, containsCam, &v);
    const bool containing = v.containing;
    if (v.why == sealed::KeepWhy::kReach) ++tc.keptReach;   // built 2026-10-21: counted here only (the producer path judges the same record by the same rule)
    if (keep) {
        ++tc.kept;
    } else {
        if (cullCounted(record, containing)) ++tc.culled;
    }
}

// ---- Built 2026-11-05: the concourse cull's names survey and store judgement ---------------------------------------------------------------------
// The one-shot names: armed at each engage, gathered over 2 frames, before the first record is vetoed. A mutex is taken only while armed and never held across a
// call into the game. While it runs the rig and producer vetoes forward, so the store sees (and names) every record; the store's clear still applies.
struct SurveyAgg {
    uint32_t kept = 0, culled = 0, smallClose = 0, unreadSphere = 0;
    float radiusMax = 0.0f;
    float nearest = -1.0f;   // the smallest distance of a record with a readable sphere; negative when none
    float nearestAt[3] = {};   // that record from the camera, in world axes (the survey with the cull off asks for it)
    float cRadiusMax = 0.0f;   // the largest radius, and the smallest distance, among the culled records of the name
    float cNearest = -1.0f;
};
struct SurveyClose {
    char name[48] = {};
    float dist = 0.0f;
    float radius = 0.0f;
};
struct Survey {
    std::mutex m;
    std::unordered_map<std::string, SurveyAgg> names;
    std::vector<SurveyClose> close;
    std::vector<SurveyClose> unnamed;   // records with no readable name and a readable sphere; `name` says whether the judge kept them
};
Survey& survey() {
    static Survey* s = new Survey;   // never destroyed: a callback may still be inside it at unload
    return *s;
}
constexpr float kSurveyCloseM = 60.0f;
constexpr size_t kSurveyCloseCap = 8192;

void concourseSurveyUnnamed(cc::Judge j, float dist, float radius) noexcept {
    try {
        Survey& sv = survey();
        std::lock_guard<std::mutex> lock(sv.m);
        if (sv.unnamed.size() >= kSurveyCloseCap) return;
        SurveyClose c;
        std::snprintf(c.name, sizeof(c.name), "%s", cc::judgeKeeps(j) ? "kept" : "culled");
        c.dist = dist;
        c.radius = radius;
        sv.unnamed.push_back(c);
    } catch (...) {
    }
}

void concourseSurveyNote(const char* name, cc::Judge j, float dist, float radius, const float* at = nullptr) noexcept {
    try {
        Survey& sv = survey();
        std::lock_guard<std::mutex> lock(sv.m);
        SurveyAgg& a = sv.names[name];
        if (radius > a.radiusMax) a.radiusMax = radius;
        if (radius > 0.0f && std::isfinite(dist) && (a.nearest < 0.0f || dist < a.nearest)) {
            a.nearest = dist;
            if (at) std::memcpy(a.nearestAt, at, sizeof(a.nearestAt));
        }
        switch (j) {
            case cc::Judge::kKeepName: ++a.kept; break;
            case cc::Judge::kKeepSmallClose: ++a.smallClose; break;
            case cc::Judge::kKeepUnreadableSphere: ++a.unreadSphere; break;
            case cc::Judge::kCull:
                ++a.culled;
                if (radius > a.cRadiusMax) a.cRadiusMax = radius;
                if (radius > 0.0f && std::isfinite(dist) && (a.cNearest < 0.0f || dist < a.cNearest)) a.cNearest = dist;
                if (dist <= kSurveyCloseM && sv.close.size() < kSurveyCloseCap) {
                    SurveyClose c;
                    std::snprintf(c.name, sizeof(c.name), "%s", name);
                    c.dist = dist;
                    c.radius = radius;
                    sv.close.push_back(c);
                }
                break;
            default: break;
        }
    } catch (...) {
    }
}

// The store's judgement while the concourse cull is wanted (kConcourse). Whatever the state, a record named Standard_SocialArea is the anchor's sighting (and
// SA_StationHub_Compact the hub's, while the tick wants it). While active it judges the record: gathers the rig note with its keep bit, and clears a culled
// record (path A) with cullCounted. It returns before the seal's gathers.
void afterStoreConcourse(uintptr_t bucket, uintptr_t record) noexcept {
    GatherWriter w(g_gather);
    w.call(0);
    TlsCounters& tc = tcFor(g_frameStamp.load(std::memory_order_relaxed));
    SnapshotPin pin;
    const PadSnapshot& s = pin.get();
    uintptr_t ctx = 0;
    uint32_t count = 0;
    if (!bucket || !record || !readVal(&ctx, bucket) || !ctx || !readVal(&count, ctx + kCtxCount) || count == 0) {
        faultNote(kFaultBucket);
        return;
    }
    w.context(ctx);
    uintptr_t node = 0;
    const NodeInfo ni = nodeInfoOf(record, &node);
    concourseSight(record, node, ni);
    if (!s.concourse || !g_concActive.load(std::memory_order_acquire)) return;
    if (!s.camValid) {
        ++tc.noCamera;
        return;
    }
    if (s.lost || w.lostSinceDrain() != 0) {   // a loss keeps every record (fail-safe)
        ++tc.kept;
        return;
    }
    const uint32_t stamp = w.stamp();
    const WorldRead wr = worldOfRecord(record);
    const bool sphere = wr.source != 4 && std::isfinite(wr.radius) && wr.radius > 0.0f && std::isfinite(wr.world[0]) && std::isfinite(wr.world[1]) &&
                        std::isfinite(wr.world[2]);
    const bool nameReadable = ni.cls != sealed::NameClass::kUnnamed && ni.cls != sealed::NameClass::kNone;
    const float d = sphere ? sealed::distance(wr.world, s.cam) : 0.0f;
    bool slabbed = false;
    const cc::Judge j = concourseJudgeHere(s, nameReadable, ni, sphere, wr.world, d, sphere ? wr.radius : 0.0f, &slabbed);
    if (slabbed) ++tc.concRoomStore;
    if (g_vrRecOn.load(std::memory_order_relaxed)) recNote(s, record, node, ni, nameReadable, sphere, wr.world, d, sphere ? wr.radius : 0.0f, j, slabbed, 'S');   // TEMPORARY (H62 instrument, Flight 56)
    const bool keep = cc::judgeKeeps(j);
    // The door rule culls the record, but by default its rig's tally counts it kept, so the rule never vetoes a whole rig: a rig can draw what the store never judges (Flight 44's bridge
    // floor, INFERRED), and its other records here say nothing of where that is.
    const bool keepForRig = keep || (slabbed && !s.roomRigs);
    const float zero[3] = {0.0f, 0.0f, 0.0f};
    gatherRigNote(w, bucket, record, sphere ? wr.world : zero, sphere ? wr.radius : 0.0f, stamp,
                  static_cast<uint8_t>(kFlagJudged | (keepForRig ? kFlagKeep : 0)));
    if (g_concSurveyArmed.load(std::memory_order_relaxed) && nameReadable) {
        char nm[64] = {};
        if (cachedNameOfNode(node, nm, sizeof(nm), &ni.validator)) concourseSurveyNote(nm, j, d, sphere ? wr.radius : 0.0f);
    } else if (g_concSurveyArmed.load(std::memory_order_relaxed) && sphere) {
        concourseSurveyUnnamed(j, d, wr.radius);
    }
    if (keep) {
        ++tc.kept;
    } else if (cullCounted(record, false)) {
        ++tc.culled;
        ++tc.concStoreCleared;
    }
}

// TEMPORARY: one record of the survey an F3 asks for with the cull off. It reads and notes, and changes nothing: no gather, no rig note, no clear. The judge says what the keep list would do.
void concourseNamesOnlyRecord(uintptr_t record) noexcept {
    if (!record) return;
    SnapshotPin pin;
    const PadSnapshot& s = pin.get();
    uintptr_t node = 0;
    const NodeInfo ni = nodeInfoOf(record, &node);
    const WorldRead wr = worldOfRecord(record);
    const bool sphere = s.camValid && wr.source != 4 && std::isfinite(wr.radius) && wr.radius > 0.0f && std::isfinite(wr.world[0]) && std::isfinite(wr.world[1]) &&
                        std::isfinite(wr.world[2]);
    const bool nameReadable = ni.cls != sealed::NameClass::kUnnamed && ni.cls != sealed::NameClass::kNone;
    const float d = sphere ? sealed::distance(wr.world, s.cam) : 0.0f;
    const cc::Judge j = cc::concourseJudge(nameReadable, (ni.flags & sealed::kNodeConcourseAllow) != 0, sphere, d, sphere ? wr.radius : 0.0f,
                                           (ni.flags & sealed::kNodeConcourseFarExempt) != 0, false, false);
    if (nameReadable) {
        char nm[64] = {};
        const float at[3] = {wr.world[0] - s.cam[0], wr.world[1] - s.cam[1], wr.world[2] - s.cam[2]};
        if (cachedNameOfNode(node, nm, sizeof(nm), &ni.validator)) concourseSurveyNote(nm, j, d, sphere ? wr.radius : 0.0f, at);
    } else if (sphere) {
        concourseSurveyUnnamed(j, d, wr.radius);
    }
}

}  // namespace

// ---- The engine's entry points (declared in optimized_station.h) -------------------------------------------------------------------------

// Once, at install: reads fix.optimized_stations (on in VR, off in flat; the flat profile does not read it).
void optimizedStationInstall(bool flatProfile) {
    const std::string text = Config::get().getString("fix.optimized_stations", "on");
    const bool on = text != "off";
    g_setting.store(on, std::memory_order_release);
    g_summaryStartMs = GetTickCount64();
    g_flatProfile.store(flatProfile, std::memory_order_relaxed);
    if (on) applySetting(true, 0);
    Log::get().note("optimized station: fix.optimized_stations = %s (%s).", on ? "on" : "off",
                    flatProfile ? "flat profile" : "VR profile");
}

bool optimizedStationOn() noexcept { return g_setting.load(std::memory_order_acquire); }
bool optimizedStationGathers() noexcept {
    // Built 2026-11-05: also while the concourse cull is wanted (on foot in social space, the key on): the store gate opens so the anchor can be seen.
    return g_setting.load(std::memory_order_acquire) &&
           (g_dockedNow.load(std::memory_order_acquire) || g_concWanted.load(std::memory_order_acquire) || g_concNamesOnly.load(std::memory_order_relaxed));
}
bool optimizedStationSealed() noexcept { return g_sealedFlag.load(std::memory_order_acquire); }

// The draw path asks: while sealed, the sun pair is not drawn.
bool optimizedStationSkipsDraw(uint64_t vs, uint64_t ps) noexcept {
    if (!optimizedStationSunOff() || vs != kSunVs || ps != kSunPs) return false;   // the sun skip follows the pad, not the seal (built v14.13)
    g_winSunDrawsSkipped.fetch_add(1, std::memory_order_relaxed);   // built v14.34: the 30 s line says how many (a flat flight shows 0 if the flat hashes differ)
    return true;
}

void optimizedStationNoteSunDraw(SunDrawPath path) noexcept {
    (path == SunDrawPath::kMasked ? g_winSunMasked : g_winSunNotIssued).fetch_add(1, std::memory_order_relaxed);
}

// The store calls this after it has written a record (bucket = the collection, record = its record). While sealed it judges the record; while
// docked and unsealed it only gathers the frame's pads, explicit parts and camera for the seal trigger (built v14.1).
void afterStoreDispatch(uintptr_t bucket, uintptr_t record) noexcept {
    if (g_concNamesOnly.load(std::memory_order_relaxed)) concourseNamesOnlyRecord(record);
    // Built 2026-11-06: docked with the cull wanted but not active (on foot in the hangar) takes the seal's gather path (the pad's gathers; afterStoreJudge also
    // sights the anchor); the concourse path is for the cull active, or not docked (a taxi arrival).
    const bool docked = g_dockedNow.load(std::memory_order_acquire);
    const bool concourseRoute = g_concWanted.load(std::memory_order_acquire) && !(docked && !g_concActive.load(std::memory_order_acquire));
    const sealed::StoreMode mode = sealed::storeMode(g_setting.load(std::memory_order_acquire), g_sealedFlag.load(std::memory_order_acquire), docked, concourseRoute);
    if (mode == sealed::StoreMode::kNone) return;
    if (mode == sealed::StoreMode::kConcourse) {
        afterStoreConcourse(bucket, record);
        return;
    }
    afterStoreJudge(bucket, record, mode == sealed::StoreMode::kJudge);
}

// The store observer's entry (the store hook calls it per record), timed one call in 32 (built v14.24; the sample is gathered since v14.26).
void optimizedStationAfterStore(uintptr_t bucket, uintptr_t record) noexcept {
    CallbackScope scope;
    TlsCounters& tc = tcFor(g_frameStamp.load(std::memory_order_relaxed));
    ++tc.callsStore;
    const bool sample = costSampleCall(tc.sampleStore);
    const int64_t t0 = sample ? costTicks() : 0;
    afterStoreDispatch(bucket, record);
    if (sample) costPush(0, costMs(t0, costTicks()));
}

// The render thread's frame tick: the setting, the held cell in the pad's frame, the seal and release rules, the rig verdicts, the summary.
// The game's pad state for this tick (built v14.23): the own pad (the object r0 was last called on this dock, GameTrack), its latest r0 and r1 codes,
// and whether each was called fresh (kPadStateFreshMs). Set by gameStateTick, read by the sun, the trigger and the waiting line.
struct GameNow {
    bool ownKnown = false;
    uintptr_t own = 0;
    int r0 = -1;
    bool r0Fresh = false;
    int r1 = -1;
    bool r1Fresh = false;
};
GameNow g_game;

// The own pad from the game, once per tick (built v14.23). The own pad changes only when r0 is called on another object (logged once); it is reset at
// undock and hangar change (GameTrack). Reads the probe; takes no lock of the station.
void gameStateTick() noexcept {
    PadObjState latest;
    padStateLatestR0(&latest);
    const sealed::GameTrackEvent ev = g_track.observe(latest.have, latest.self, latest.r0Qpc);
    if (ev == sealed::GameTrackEvent::kFirst) {
        Log::get().note("optimized station: own pad is obj @%06llx (the game's docking reader r0 was called on it this dock)",
                        static_cast<unsigned long long>(static_cast<uint64_t>(g_track.own) & 0xFFFFFFull));
    } else if (ev == sealed::GameTrackEvent::kSwitched) {
        Log::get().note("optimized station: own pad switched to obj @%06llx (r0 was called on it; the previous own pad is no longer the one)",
                        static_cast<unsigned long long>(static_cast<uint64_t>(g_track.own) & 0xFFFFFFull));
    }
    g_game = GameNow{};
    if (!g_track.have) return;
    PadObjState own;
    padStateObject(g_track.own, &own);
    g_game.ownKnown = true;
    g_game.own = g_track.own;
    g_game.r0 = own.r0;
    g_game.r0Fresh = own.r0 >= 0 && sealed::gameFresh(padStateAgeMs(own.r0Qpc));
    g_game.r1 = own.r1;
    g_game.r1Fresh = own.r1 >= 0 && sealed::gameFresh(padStateAgeMs(own.r1Qpc));
}

// Built v14.24: the mechanism's re-seal on the poses. While the seal is released by the mechanism (the trigger's reseal 1 with the mechanism flag) every
// latched record is compared with its pose at the seal. Returns true when all of them are back this frame (the core counts the frames). Logs: the away
// line while the pad is out of the bay with r1 1 (change-only, at most 2 Hz, the largest four first); the back line once when all are back.
bool mechanismPoseTick(uint64_t now) noexcept {
    if (!(g_sealState.reseal == 1 && g_sealState.mechanism) || g_sealPoseN == 0 || g_latch.n != g_sealPoseN) {
        g_poseAwayGate.reset();
        g_poseBackGate.reset();
        return false;
    }
    const uint32_t frame = g_frameStamp.load(std::memory_order_relaxed);
    struct Away {
        char name[48];
        float dM;
        float dDeg;
    };
    Away away[sealed::kLatchCap];
    size_t nAway = 0;
    bool allBack = true;
    for (size_t i = 0; i < g_sealPoseN; ++i) {
        const sealed::LatchRec& r = g_latch.recs[i];
        const SealPoseRec& p = g_sealPose[i];
        if (r.addr != p.addr || r.stamp != frame) return false;   // a record not read this frame: no comparison, no pose
        float dM = 0.0f;
        float dDeg = 0.0f;
        sealed::latchPoseDelta(r.rel, p.rel, r.ax0, p.ax0, r.ax2, p.ax2, &dM, &dDeg);
        if (sealed::latchPoseWithin(dM, dDeg)) continue;
        allBack = false;
        Away& a = away[nAway++];
        std::snprintf(a.name, sizeof(a.name), "%s", p.name);
        a.dM = dM;
        a.dDeg = dDeg;
    }
    const bool r1Up = g_game.r1Fresh && g_game.r1 == sealed::kReaderInHangar;
    if (allBack) {
        g_poseAwayGate.reset();
        if (r1Up && g_poseBackGate.changed(1)) {
            Log::get().note("optimized station: mechanism back at its sealed pose: all %u latched records within %.2f m and %.0f deg; re-seal allowed "
                            "after %u frames with r1 1.",
                            static_cast<unsigned>(g_sealPoseN), static_cast<double>(sealed::kPoseTolM), static_cast<double>(sealed::kPoseTolDeg),
                            static_cast<unsigned>(sealed::kPoseBackFrames));
        }
        return true;
    }
    if (!r1Up) return false;
    // The largest first, at most four.
    std::sort(away, away + nAway, [](const Away& x, const Away& y) { return x.dM > y.dM; });
    char text[400] = {};
    size_t used = 0;
    const size_t shown = nAway < 4 ? nAway : 4;
    for (size_t i = 0; i < shown; ++i) {
        const int w = std::snprintf(text + used, sizeof(text) - used, "%s%s %.2f m %.1f deg", i > 0 ? "; " : "", away[i].name,
                                    static_cast<double>(away[i].dM), static_cast<double>(away[i].dDeg));
        if (w < 0 || used + static_cast<size_t>(w) >= sizeof(text)) break;
        used += static_cast<size_t>(w);
    }
    const uint64_t key = sealed::textKey(text);
    if (now >= g_poseLastLogMs + 500 && g_poseAwayGate.changed(key)) {
        g_poseLastLogMs = now;
        Log::get().note("optimized station: mechanism away from its sealed pose: %s (largest first, at most 4)", text);
    }
    return false;
}

// ---- Built 2026-11-05: the concourse cull's tick (docs\concourse-cull-2026-11-05.md). TEMPORARY. ------------------------------------------------------------
// The keys are polled about once a second (diag.concourse_cull, diag.concourse_keep, hotkey.concourse_cull); the hotkey is polled every tick. The state is
// the tick's alone; the callbacks see g_concWanted, g_concActive and g_concSurveyArmed (atomics) and the snapshot's `concourse` flag.
namespace {

enum class ConcOverride : uint8_t { kNone, kOn, kOff };
ConcOverride g_concOverride = ConcOverride::kNone;   // the hotkey's A/B override of the key; cleared when the key's text changes
bool g_concKeyOn = false;                            // diag.concourse_cull = on
bool g_concKeySeen = false;
uint64_t g_concNextPollMs = 0;
std::string g_concExtraApplied;
bool g_concExtraSeen = false;
Hotkey* g_concHotkey = nullptr;
// TEMPORARY (the concourse view test, docs\concourse-cull-2026-11-05.md; removed when the arc closes): its hotkey and mode. A press steps the mode 0 to 5 and wraps.
Hotkey* g_viewHotkey = nullptr;
std::string g_viewHotkeyText;
bool g_viewHotkeyBound = false;
// TEMPORARY (docs\concourse-cull-2026-11-05.md): the camera test. hotkey.concourse_camera_test steps g_camMode (cc::kCamTestMode*); the hooks are in
// concourse_camera_test.cpp (the write point is after 0x2874290) and install only when the key is bound, in the flat profile, on build 334925. The look acts in a window the tick opens (g_camTimer).
Hotkey* g_camHotkey = nullptr;
std::string g_camHotkeyText;
bool g_camHotkeyBound = false;
// The door rule (docs\concourse-cull-2026-11-05.md "Rooms behind bulkhead doors"): at a room whose layout has doors (cc::RoomLayout::slabCut), the room's own records behind a shut door
// are culled. In the live room, where the cull stands down, that is the only rule, so its windows stay the game's own. TEMPORARY: hotkey.concourse_room_test steps g_doorMode.
Hotkey* g_roomHotkey = nullptr;
std::string g_roomHotkeyText;
bool g_roomHotkeyBound = false;
// TEMPORARY (the VR proving build, docs\concourse-cull-2026-11-05.md "VR, next"): two probe hotkeys, VR only, log only: hotkey.concourse_vr_hold_probe (the frame hold) and
// hotkey.concourse_vr_camera_probe (the eye camera, and the tone draw's lines on its frames). Bound by concoursePoll like the room key; read by concourseVrProbeTick.
struct VrProbeKey {
    Hotkey* hk = nullptr;
    std::string text;
    bool bound = false;
};
VrProbeKey g_vrHoldKey, g_vrCameraKey;
cc::DoorRuleMode g_doorMode = cc::DoorRuleMode::kRecords;
bool g_doorsSetting = true;      // fix.skip_rooms_behind_doors, read in concoursePoll
bool g_doorsSettingSeen = false;
cc::DoorWatch g_doorWatch[2];
uint64_t g_roomSumMs = 0;
// The door watch's log: each door's last change (its frame and time, for the closing line's "open for"), the changes in the 5 s window, and the door block's own cost per tick
// in that window (QPC, to answer whether reading the leaves each frame costs anything worth a hook).
uint32_t g_doorChangeFrame[2] = {};
uint64_t g_doorChangeMs[2] = {};
uint32_t g_doorOpenings = 0, g_doorLogLines = 0;
constexpr uint32_t kDoorLogLinesMax = 400;   // a door that flaps (leaves that jitter past kDoorMoveM) is counted, not logged without end
int64_t g_doorCostTicks = 0, g_doorCostMax = 0;
uint32_t g_doorCostN = 0;
uint32_t g_camMode = 0;
bool g_camHooked = false;
bool g_camModeByKey = false;           // a press of the key chose the mode: the automatic bake leaves it alone
bool g_camAutoTried = false;           // the automatic install is tried once; one line says why not
uint64_t g_bakeRoomSinceMs = 0;        // on foot with the room in view since then; 0 is not in view
uint32_t g_bakeNoRideTries = 0;        // bakes started with no ride and not yet followed by a good one (cc::kBakeNoRideTries)
uint64_t g_bakeLeftMs = 0;             // neither on foot nor docked since then; 0 is in a station
uint64_t g_bakeAfterJumpMs = 0;        // no bake with no ride starts before this tick: a carrier's jump (cc::kBakeAfterJumpMs)
cc::CamTestTimer g_camTimer;
bool g_camLookValid = false;           // the room's centre and up were read when the window opened
bool g_camLookWasOn = false;
constexpr int kCamLookTickCount = 16;
int g_camLookTicks = 0;
uint64_t g_camLookTickMs = 0;
uint16_t g_camLookTickDt[kCamLookTickCount] = {};
float g_camLookCentre[3] = {};
float g_camLookAxes[3][3] = {};        // the anchor's, when the window opened: [1] is its up
uint32_t g_camBakeTick = 0;            // the bake: ticks since the window opened
uint32_t g_camBakeDir = 0;             // the bake: the direction last aimed at (cc::bakeNextDir)
uint32_t g_camBakeMask = cc::kBakeDirsAll;   // diag.concourse_bake_dirs, read when a ride up arms it
float g_camBakeNear = cc::kBakeNearDefaultM;   // diag.concourse_bake_near, the same
uint64_t g_camLogMs = 0;               // the last status line (mode 1: once a second; acting: every cc::kCamTestLogMs)
ConcourseCameraTestSnap g_camLast;     // the snapshot at that line, for the counts since
// PROTOTYPE (the backdrop capture, hotkey.concourse_view_test = F6; docs\concourse-cull-2026-11-05.md "The window plan"): Lifting (the cull is off for cc::kBackdropLiftMs, the
// outside effects still withheld), then Armed (the pane module is told to capture in the next frame, and its status is read each tick until it says what happened).
enum class BackdropPhase : uint8_t { kIdle, kLifting, kArmed };
BackdropPhase g_backdropPhase = BackdropPhase::kIdle;
uint64_t g_backdropStartMs = 0, g_backdropArmMs = 0;
// Automatic capture (mirror-backdrop, once a hotkey has turned it on): the pane module probes what the cube cannot show; this gate decides, from the share, when to capture.
cc::BackdropAutoGate g_backdropGate;
bool g_backdropAutoRun = false;                  // the capture in progress was started by the gate, not F6
uint32_t g_backdropAutoCount = 0, g_backdropManualCount = 0;   // captures started since this visit began
uint64_t g_backdropLiftedMs = 0;                 // the cull lifted for captures, in all, this visit
uint64_t g_backdropSummaryMs = 0;
uint64_t g_backdropProbeIssued0 = 0, g_backdropProbeRead0 = 0;   // the probe counters at the last 30 s line
std::string g_concHotkeyText;
bool g_concHotkeyBound = false;
bool g_concWasActive = false;
bool g_concWasArmed = false;
bool g_concWaitSet = false;
uint32_t g_concWaitKey = 0;
uint64_t g_concSumMs = 0;
uint64_t g_concSumFrames = 0;
uint64_t g_concSumCalls[3] = {};   // the store, producer and rig calls at the last summary
uint64_t g_concSurveyDueMs = 0;    // 0: none due
bool g_concSurveyOn = false;
bool g_concSurveyList = false;     // the running survey lists the culled names
uint32_t g_concSurveyStamp = 0;
bool g_concNamesOnlyOn = false;    // TEMPORARY: the tick's side of g_concNamesOnly
uint32_t g_concNamesOnlyStamp = 0;
const cc::RoomLayout* g_concLayout = &cc::kRoomStation;   // this room's, from the anchor's name (the tick's)
uintptr_t g_concLayoutAddr = 0;                           // the anchor record it was read from
cc::LiveRoomLatch g_concLiveLatch;                        // whether the camera is in the room's live room, through a tick with its record unread
uint64_t g_concTraceMs = 0;
uint32_t g_concTraceReason = 0;
uint64_t g_concTraceReasonMs = 0;
bool g_concSettingOffLogged = false;
cc::ClearMachine g_concClear;             // the bay glass's clearing on approach (concourse_cull_core.h)
cc::LiftGate g_liftGate;                  // the lift gate (concourse_cull_core.h), the tick's alone
bool g_liftWasOn = false;                 // the gate's state at the last tick
uint64_t g_liftStartMs = 0;               // the tick the gate came on
uint64_t g_liftEndMs = 0;                 // the tick it went off (the cull resuming just after is not a new arrival)
uint64_t g_liftTicks = 0;                 // the ticks from then, the first included, while it stayed on
uint64_t g_liftTot0[6] = {};              // g_concWin.tot when it came on
bool g_concWasCull = false;               // the cull's own part of `active` at the last tick (wantCull and the clearing machine), apart from the lift
bool g_concWasWant = false;               // wantCull at the last tick: the gate's rising edge starts the names survey, a re-engage from the latch does not
float g_concEnterShare = 0.0f;            // diag.concourse_clear_share as a fraction of the view; 0 never clears
bool g_concEnterSeen = false;
uint64_t g_concShareLogMs = 0;
uint32_t g_concShareSeq = 0;
ConcoursePaneShares g_concShareLast;

// diag.concourse_cull = auto runs only at a type of station whose layout has been flown (cc::bakePlanFor). The key at on, or the hotkey's override, is a test and goes anywhere.
bool g_concAuto = false;
journal_station::Kind g_concKind = journal_station::Kind::kUnknown;
bool g_concKindSeen = false;

// ---- The VR bake (TEMPORARY, docs\concourse-cull-2026-11-05.md "Flight 51 results"; the conventions, each read in the code, are in concourse_vr_bake_core.h) ------------------
// The same feature as flat's bake: a ride up, or the room in view with no bake, opens the window (g_camTimer), the cull waits for the bake, and the windows show it. The camera is
// the eye stub's (native_preview_camera.cpp): its base, written so that each eye sits at the room's centre looking along a face. The phases, one tick a frame:
//   measure  nothing is aimed or held. Each eye's b1 rows are copied at its first scene draw (concourse_pane.cpp) and, once read back, the eyes' row 275 is set against the
//            store's camera of the same frame (the world check): past kWorldOffsetM the aim and the cube's origin move by render less store. No rows in kVrMeasureMaxTicks: the
//            store's centre, said so.
//   aim      each tick the next face not yet covered (cvb::AimPlan), B = G(H)^-1 * D from the latest head (concourse_vr_bake_core.h), the near value raised, and the runtime asked
//            to withhold kVrHoldLag frames (each tick, so the hold runs until kVrHoldLag frames after the last aim). Each eye's tonemap draw captures with that eye's own rows on
//            the GPU; the counts that come back mark a face covered, or have it aimed at again, up to cvb::kAimsPerFace times. A face never filled ends the bake anyway, said so.
// Every line starts "concourse bake: vr". What a piece that never ran leaves: no "vr: the bake is available" line (the stub, or the tone draw never seen); a begin and no "vr rows"
// lines and the finish line's "rows copied 0 0" (no scene draw's rows copied); "vr stub" lines with 0 calls (the stub never ran) or written 0x0 (it ran and wrote nothing); the
// finish line's "tone draws 0" (no tonemap draw while capturing), or its frames "not at the centre" (the aim did not land); the hold's line "NONE taken"; the near's line "not
// available"; and no "vr world check" line before the aims (said instead as "could not be made").
namespace cvb = concourse_vr;
enum class VrBakePhase : uint8_t { kIdle, kMeasure, kAim };
constexpr uint32_t kVrMeasureMaxTicks = 10;   // no rows back by then: the world check is not made
// Aimed ticks in a row whose every tone draw lacked the frame's rows, past which the aims stop (the headset is held for each aim, so a bake that sees nothing only freezes it: Flight
// 53 held 969 ms for 66 rowless tone draws of 94). Six is two faces' dwell (kVrBakeDwell is 3): a lull that long is not the one or two late frames a working bake shows.
constexpr uint32_t kVrRowlessAimsMax = 6;
// The same for aimed windows in which the stub wrote nothing: the eyes stay where Elite put them, so no frame is at the centre and the hold only freezes the headset. Flight 54
// held three bakes of 656 to 1234 ms this way, every call refused by the fence, while the rows came back as usual. The same six.
constexpr uint32_t kVrUnwrittenAimsMax = kVrRowlessAimsMax;
constexpr uint32_t kVrBakeTicksMax = 96;     // the measure, three rounds of aims (each held kVrBakeDwell ticks) and their read-backs, with room to spare
constexpr uint32_t kVrHoldLag = 3;            // frames withheld after each aim's tick: the frames still in flight after the last
constexpr uint32_t kVrHoldFollowTicks = 8;    // the hold's line, this many ticks after the end
constexpr uint32_t kVrLogLines = 640;         // per bake: the stub, rows, capture and aim lines together (TEMPORARY, H58-H61 instrument, Flight 53: every capture line of every aim frame)
constexpr uint32_t kVrRing = 16;
struct VrBakeState {
    VrBakePhase phase = VrBakePhase::kIdle;
    bool stubTried = false, stubOk = false, nearTried = false, nearOk = false, toneSaid = false, availSaid = false;
    uint32_t tick = 0, startFrame = 0, aimTicks = 0, headMissing = 0, logLines = 0;
    uint64_t beginMs = 0;
    float centreStore[3] = {}, centre[3] = {}, offset[3] = {};
    bool worldChecked = false;
    cvb::Face faces[cvb::kFaces];
    uint32_t faceMask = 0;
    float pitch = 0.0f, handed = 1.0f;
    cvb::AimPlan plan;
    uint64_t heldBase = 0, written[2] = {}, taken = 0, status[5] = {};
    bool nearRaised = false;
    uint32_t camFrame[kVrRing] = {};
    float cam[kVrRing][3] = {};
    bool camValid[kVrRing] = {};
    uint32_t baseFrame[kVrRing] = {};
    float base[kVrRing][16] = {};
    bool baseValid[kVrRing] = {};
    bool baseStack[kVrRing] = {};          // the window's first call was at a stack address, so its pose is Elite's base (not a heap call's identity)
    uint32_t rowlessRun = 0;               // aimed ticks in a row whose tone draws all lacked the frame's rows (kVrRowlessAimsMax)
    uint32_t unwrittenRun = 0;             // aimed windows in a row in which the stub wrote no call (kVrUnwrittenAimsMax)
    bool stoppedUnwritten = false;         // the aims stopped on unwrittenRun, not rowlessRun (the stop line says which)
    uint64_t lastToneDraws = 0, lastNoRows = 0;
    uint32_t aimFrame[kVrRing] = {}, aimFace[kVrRing] = {}, aimDwell[kVrRing] = {};   // aimDwell: TEMPORARY (H58-H61 instrument, Flight 53)
    bool aimWrite[kVrRing] = {};                                                    // the frame writes the cube (the hold's last frame); the rest only count
    float aimPred[kVrRing][2][3] = {};
    bool aimValid[kVrRing] = {};
    ConcourseVrBakeRows rows[kVrRing][2];
    bool rowsValid[kVrRing][2] = {};
    bool holdFollow = false;
    uint32_t holdFollowFrame = 0, holdAims = 0;
    uint64_t holdBase = 0;
};
VrBakeState g_vr;

// VR's bake can run: the eye stub is in, and the game's tonemap draw has been seen with an eye and its depth.
bool vrBakeAvail() noexcept { return g_vr.stubOk && concoursePaneVrToneSeen(nullptr); }
bool vrLogTake() noexcept {
    if (g_vr.logLines >= kVrLogLines) return false;
    ++g_vr.logLines;
    return true;
}
void vrPoseText(const float* v, char* out, size_t n) {
    std::snprintf(out, n, "x(%.3f %.3f %.3f) y(%.3f %.3f %.3f) z(%.3f %.3f %.3f) t(%.3f %.3f %.3f)", static_cast<double>(v[0]), static_cast<double>(v[1]), static_cast<double>(v[2]),
                  static_cast<double>(v[4]), static_cast<double>(v[5]), static_cast<double>(v[6]), static_cast<double>(v[8]), static_cast<double>(v[9]), static_cast<double>(v[10]),
                  static_cast<double>(v[12]), static_cast<double>(v[13]), static_cast<double>(v[14]));
}
// Nothing is written into the stub any more, and the near value is put back.
void vrBakeStopAim() noexcept {
    nativeCameraBakeStopWriting();
    if (g_vr.nearRaised) {
        g_vr.nearRaised = false;
        const bool was = concourseNearValueRestore();
        Log::get().note("concourse bake: vr near: put back %.3f m%s", static_cast<double>(concourseNearValueRestoredTo()), was ? "" : " (it was no longer raised)");
    }
}
// The end, whichever way: the stub let go, the near put back, and the cube published when `publish` and something was written. The finish line names each face's coverage.
void vrBakeEnd(bool publish, const char* why, uint64_t now, uint32_t frame) {
    if (g_vr.phase == VrBakePhase::kIdle) return;
    const bool aimed = g_vr.phase == VrBakePhase::kAim;
    vrBakeStopAim();
    recFlush(true);   // TEMPORARY (H62 instrument, Flight 56): the last window, the last face and the kept-near table
    nativeCameraBakeObserve(false);
    const bool any =g_vr.written[0] + g_vr.written[1] > 0;
    char faces[260] = {};
    size_t at = 0;
    for (uint32_t f = 0; f < cvb::kFaces && at + 48 < sizeof(faces); ++f) {
        if ((g_vr.plan.want() & (1u << f)) == 0) continue;
        const bool covered = (g_vr.plan.covered() & (1u << f)) != 0;
        const float best = g_vr.plan.best(f);
        at += static_cast<size_t>(std::snprintf(faces + at, sizeof(faces) - at, "%s%u %.1f%% (%u aim%s)%s", at ? ", " : "", f, static_cast<double>(best) * 100.0, g_vr.plan.aims(f),
                                                g_vr.plan.aims(f) == 1 ? "" : "s", covered ? "" : best > 0.0f ? " SHORT" : " NEVER FILLED"));
    }
    const ConcourseVrBakeStats st = concoursePaneVrBakeStats();
    char line[1100];
    std::snprintf(line, sizeof(line),
                  "vr: finished after %llu ms (%s): faces covered 0x%X of 0x%X [%s]; frames taken %llu (dispatches eye 0 %llu, eye 1 %llu; tone draws %llu, without the frame's rows %llu, "
                  "without depth %llu, refused %llu; rows copied %llu %llu, no b1 %llu; read-backs lost %llu); frames not at the centre %llu, with no face in view %llu, with rows that "
                  "do not invert %llu; texels written eye 0 %llu, eye 1 %llu; the last frame's order: rows 0 #%u, tone 0 #%u, rows 1 #%u, tone 1 #%u; aims %u, head missing %u%s",
                  static_cast<unsigned long long>(now - g_vr.beginMs), why, g_vr.plan.covered(), g_vr.plan.want(), faces[0] ? faces : "no aims", static_cast<unsigned long long>(g_vr.taken),
                  static_cast<unsigned long long>(st.dispatches[0]), static_cast<unsigned long long>(st.dispatches[1]), static_cast<unsigned long long>(st.toneDraws),
                  static_cast<unsigned long long>(st.noRows), static_cast<unsigned long long>(st.noDepth), static_cast<unsigned long long>(st.refused),
                  static_cast<unsigned long long>(st.snaps[0]), static_cast<unsigned long long>(st.snaps[1]), static_cast<unsigned long long>(st.snapNoCb),
                  static_cast<unsigned long long>(st.lost), static_cast<unsigned long long>(g_vr.status[2]), static_cast<unsigned long long>(g_vr.status[3]),
                  static_cast<unsigned long long>(g_vr.status[4]), static_cast<unsigned long long>(g_vr.written[0]), static_cast<unsigned long long>(g_vr.written[1]), st.firstOrder[0],
                  st.firstOrder[1], st.firstOrder[2], st.firstOrder[3], g_vr.aimTicks, g_vr.headMissing,
                  !aimed ? "; it never aimed" : any ? "" : "; NOTHING CAPTURED, the cube is as it was");
    concoursePaneVrBakeEnd(publish && aimed && any, line);
    if (g_vr.aimTicks > 0) {
        g_vr.holdFollow = true;
        g_vr.holdFollowFrame = frame + kVrHoldFollowTicks;
        g_vr.holdAims = g_vr.aimTicks;
        g_vr.holdBase = g_vr.heldBase;
    }
    g_vr.phase = VrBakePhase::kIdle;
}
// fix.optimized_stations went off (or the profile is not VR): nothing of the VR bake goes on.
void vrBakeShutdown(uint64_t now) {
    concoursePaneVrBakeDiscover(false);
    concoursePaneVrBakeArm(false);
    if (g_vr.phase == VrBakePhase::kIdle) return;
    vrBakeEnd(false, "fix.optimized_stations is off", now, 0);
    g_vr.holdFollow = false;   // the setting tick has no frame to count from; the hold's line is not said
}

// In VR, by auto, a type on the list runs once VR's bake is available (vrBakeAvail), and the carrier (cc::runsWithoutBake) always: its windows take no bake. Until then the
// type stays the game's own, as before the VR bake. Flat: unchanged.
bool concourseStationOk() noexcept {
    if (!g_concAuto || g_concOverride != ConcOverride::kNone) return true;
    return cc::bakePlanFor(g_concKind).run && (runtimeFlatProfile() || cc::runsWithoutBake(g_concKind) || vrBakeAvail());
}
bool concourseEffective() noexcept {
    if (!concourseStationOk()) return false;
    return g_concOverride == ConcOverride::kNone ? g_concKeyOn : g_concOverride == ConcOverride::kOn;
}
bool concourseArmed() noexcept { return concourseStationOk() && (g_concKeyOn || g_concOverride != ConcOverride::kNone); }
const char* concourseKeyText() noexcept {
    switch (g_concOverride) {
        case ConcOverride::kOn: return "override-on";
        case ConcOverride::kOff: return "override-off";
        default: return g_concKeyOn ? "on" : "off";
    }
}

// TEMPORARY (docs\concourse-cull-2026-11-05.md): the camera test's hook, installed when it is wanted. One line says why not.
bool camTestInstall(const char* wher) {
    if (g_camHooked) return true;
    if (!runtimeFlatProfile()) {
        Log::get().note("concourse camera test: %s: not installed: flat only for now, and this is the VR profile", wher);
        return false;
    }
    const char* why = nullptr;
    if (concourseCameraTestInstall(&why)) {
        g_camHooked = true;
        Log::get().note("concourse camera test: %s: the camera hooks are installed (build 334925, TEMPORARY; the first status line says which)", wher);
        return true;
    }
    Log::get().note("concourse camera test: %s: not installed: %s", wher, why ? why : "no reason given");
    return false;
}

// One status line from the hook snapshot. The first part is the fixed form; the rest tells the cases apart. NEVER RAN: "not installed", or "NOT CALLED" for a hook that is in.
// RAN AND WROTE, NOTHING FOLLOWED: "written N", and "RV cam minus camera after" not near 0, or near 0 with flat camera origin steady. REACHED: that difference near 0 and, in the
// other lines, flat camera origin stepping about 30 m, and the view moves. In log only that difference near 0 says this producer's camera is view 0's, with nothing written.
void camTestLine(const char* tag, uint32_t mode) {
    ConcourseCameraTestSnap s;
    concourseCameraTestSnapshot(&s);
    static const char* const kNames[ConcourseCameraTestSnap::kCalls] = {"0x1EA9C0", "0x2874290"};
    char calls[120];
    size_t at = 0;
    calls[0] = 0;
    for (int i = 0; i < ConcourseCameraTestSnap::kCalls && at + 60 < sizeof(calls); ++i) {
        const unsigned long long d = s.calls[i] - g_camLast.calls[i];
        if (((s.hookMask >> i) & 1u) == 0) at += static_cast<size_t>(std::snprintf(calls + at, sizeof(calls) - at, "%s not installed; ", kNames[i]));
        else if (d == 0) at += static_cast<size_t>(std::snprintf(calls + at, sizeof(calls) - at, "%s NOT CALLED; ", kNames[i]));
        else at += static_cast<size_t>(std::snprintf(calls + at, sizeof(calls) - at, "%s %llu; ", kNames[i], d));
    }
    const unsigned long long dWritten = s.written - g_camLast.written;
    Log::get().note("concourse camera test: mode %u: camera before %.2f %.2f %.2f, after %.2f %.2f %.2f (forward row %.2f %.2f %.2f -> %.2f %.2f %.2f); RV cam %.2f %.2f %.2f; "
                    "view slot +0x540 %.2f %.2f %.2f after the fill; rv count %u (%s: calls this interval: %swritten %llu, refused %llu total, other call sites %llu total; RV cam minus camera after %.2f %.2f %.2f; "
                    "entries with flags&3 %u, view 0 flags 0x%X, slot %u flags 0x%X, slot %s, view 0 not found %llu total, several %llu total, faults %llu total)",
                    mode, static_cast<double>(s.camBefore[0]), static_cast<double>(s.camBefore[1]), static_cast<double>(s.camBefore[2]), static_cast<double>(s.camAfter[0]),
                    static_cast<double>(s.camAfter[1]), static_cast<double>(s.camAfter[2]), static_cast<double>(s.fwdBefore[0]), static_cast<double>(s.fwdBefore[1]),
                    static_cast<double>(s.fwdBefore[2]), static_cast<double>(s.fwdAfter[0]), static_cast<double>(s.fwdAfter[1]), static_cast<double>(s.fwdAfter[2]),
                    static_cast<double>(s.rvPos[0]), static_cast<double>(s.rvPos[1]), static_cast<double>(s.rvPos[2]), static_cast<double>(s.slot[0]), static_cast<double>(s.slot[1]),
                    static_cast<double>(s.slot[2]), s.rvCount, tag, calls, dWritten, static_cast<unsigned long long>(s.refused), static_cast<unsigned long long>(s.otherSite),
                    static_cast<double>(s.rvPos[0] - s.camAfter[0]), static_cast<double>(s.rvPos[1] - s.camAfter[1]), static_cast<double>(s.rvPos[2] - s.camAfter[2]), s.mainCount, s.flags,
                    s.slotIndex, s.slotFlags, s.slotKnown ? "read" : "NOT read", static_cast<unsigned long long>(s.noView0), static_cast<unsigned long long>(s.multiView),
                    static_cast<unsigned long long>(s.faults));
    g_camLast = s;
}

// A press of hotkey.concourse_camera_test: the next mode, said in the log and in a toast.
void camTestPress() {
    if (!camTestInstall("press")) {
        concoursePaneQueueToast("Camera test: not available");
        return;
    }
    g_camMode = cc::camTestNextMode(g_camMode);
    g_camModeByKey = true;
    Log::get().note("concourse camera test: mode %u (%s) by hotkey.concourse_camera_test", g_camMode, cc::camTestModeName(g_camMode));
    char text[64];
    std::snprintf(text, sizeof(text), "Camera test: %s", cc::camTestModeName(g_camMode));
    concoursePaneQueueToast(text);
}

// diag.concourse_cull = auto, the default: on with fix.optimized_stations, in flat, once the camera hooks are in. The windows need the bake and the bake needs the hooks, so with no
// hooks (another game build) there is no cull and the station stays the game's own. In VR it is on with the setting; the eye stub (the VR bake's camera) is installed once here,
// and concourseStationOk lets a type that needs a bake run only once the VR bake is available (the carrier needs none).
bool concourseAutoOn() {
    if (!g_setting.load(std::memory_order_acquire)) return false;
    if (!runtimeFlatProfile()) {
        if (!g_vr.stubTried) {
            g_vr.stubTried = true;
            const char* why = nullptr;
            g_vr.stubOk = nativeCameraBakeAvailable(&why);
            Log::get().note("concourse bake: vr: the eye stub %s%s", g_vr.stubOk ? "is installed: the VR bake can aim once the game's tonemap draw is seen" :
                            "is NOT available, so VR has no bake and a station that needs one stays the game's own: ", g_vr.stubOk ? "" : (why ? why : "no reason given"));
        }
        return true;
    }
    if (!g_camHooked && !g_camAutoTried) {
        g_camAutoTried = true;
        camTestInstall("automatic");
    }
    if (!g_camHooked) return false;
    if (!g_camModeByKey) g_camMode = cc::kCamTestModeBake;
    return true;
}

void concoursePoll(uint64_t now) {
    if (now < g_concNextPollMs) return;
    g_concNextPollMs = now + 1000;
    Config& cfg = Config::get();
    const std::string cullText = cfg.getString("diag.concourse_cull", "auto");
    g_concAuto = cullText == "auto";
    const bool on = g_concAuto ? concourseAutoOn() : cullText == "on";
    if (!g_concKeySeen || on != g_concKeyOn) {
        if (g_concKeySeen) {
            g_concOverride = ConcOverride::kNone;
            Log::get().note("concourse cull: diag.concourse_cull (%s) now means %s (the hotkey's override is cleared)", cullText.c_str(), on ? "on" : "off");
        } else {
            Log::get().note("concourse cull: diag.concourse_cull = %s: %s", cullText.c_str(), on ? "on" : "off");
        }
        g_concKeySeen = true;
        g_concKeyOn = on;
    }
    // How much of the view a window fills when it clears: the shipped share at fix.station_window_distance's 100, unless diag.concourse_clear_share names a number.
    float clearPct = cc::windowClearPercent(runtimeFlatProfile() ? 20.0f : 12.0f,
                                            cfg.getIntInRange("fix.station_window_distance", cc::kWindowDistanceDefault, cc::kWindowDistanceMin, cc::kWindowDistanceMax));
    {
        const std::string clearText = cfg.getString("diag.concourse_clear_share", "auto");
        char* end = nullptr;
        const float v = std::strtof(clearText.c_str(), &end);
        if (clearText != "auto" && end != clearText.c_str() && end && *end == '\0') clearPct = v;
    }
    if (!(clearPct > 0.0f)) clearPct = 0.0f;
    if (clearPct > 100.0f) clearPct = 100.0f;
    if (!g_concEnterSeen || clearPct / 100.0f != g_concEnterShare) {
        Log::get().note("concourse cull: a window clears at %.1f percent of the view (fix.station_window_distance, or diag.concourse_clear_share; 0 never clears)", static_cast<double>(clearPct));
        g_concEnterSeen = true;
        g_concEnterShare = clearPct / 100.0f;
    }
    // fix.skip_rooms_behind_doors: the door rule, its own switch under Optimized stations (anything but "off" is on). Off, the rule stands down whatever its hotkey says.
    {
        const bool doorsOn = cfg.getString("fix.skip_rooms_behind_doors", "on") != "off";
        if (!g_doorsSettingSeen || doorsOn != g_doorsSetting) {
            Log::get().note("concourse doors: fix.skip_rooms_behind_doors = %s", doorsOn ? "on" : "off");
            g_doorsSettingSeen = true;
            g_doorsSetting = doorsOn;
        }
    }
    const std::string extra = cfg.getString("diag.concourse_keep", "");
    if (!g_concExtraSeen || extra != g_concExtraApplied) {
        const bool first = !g_concExtraSeen;
        g_concExtraSeen = true;
        g_concExtraApplied = extra;
        g_concExtra.set(extra.c_str());
        g_classGen.fetch_add(1, std::memory_order_relaxed);   // every node's flags are read again with the new prefixes
        if (!(first && extra.empty()))
            Log::get().note("concourse cull: diag.concourse_keep = \"%s\" (extra name prefixes kept; every node's name is read again)", extra.c_str());
    }
    const std::string keyText = cfg.getString("hotkey.concourse_cull", "");
    if (!g_concHotkeyBound || keyText != g_concHotkeyText) {
        const bool first = !g_concHotkeyBound;
        if (!g_concHotkey) g_concHotkey = new Hotkey();
        g_concHotkeyBound = true;
        g_concHotkeyText = keyText;
        g_concHotkey->setBinding(keyText.c_str());
        if (g_concHotkey->bound()) Log::get().note("concourse cull: hotkey.concourse_cull = \"%s\" bound (a press turns the cull on or off, as an override of the key)", keyText.c_str());
        else if (!keyText.empty()) Log::get().note("concourse cull: hotkey.concourse_cull = \"%s\" bound nothing, so the override cannot be used this session", keyText.c_str());
        else if (!first) Log::get().note("concourse cull: hotkey.concourse_cull cleared");
    }
    const std::string viewKey = cfg.getString("hotkey.concourse_view_test", "");
    if (!g_viewHotkeyBound || viewKey != g_viewHotkeyText) {
        const bool first = !g_viewHotkeyBound;
        if (!g_viewHotkey) g_viewHotkey = new Hotkey();
        g_viewHotkeyBound = true;
        g_viewHotkeyText = viewKey;
        g_viewHotkey->setBinding(viewKey.c_str());
        if (g_viewHotkey->bound()) Log::get().note("concourse view test: hotkey.concourse_view_test = \"%s\" bound (TEMPORARY, a prototype: a press lifts the cull for %llu ms and captures the view through the windows, see concourse_pane.h)", viewKey.c_str(), static_cast<unsigned long long>(cc::kBackdropLiftMs));
        else if (!viewKey.empty()) Log::get().note("concourse view test: hotkey.concourse_view_test = \"%s\" bound nothing, so the capture cannot be used this session", viewKey.c_str());
        else if (!first) Log::get().note("concourse view test: hotkey.concourse_view_test cleared");
    }
    const std::string camKey = cfg.getString("hotkey.concourse_camera_test", "");
    if (!g_camHotkeyBound || camKey != g_camHotkeyText) {
        const bool first = !g_camHotkeyBound;
        if (!g_camHotkey) g_camHotkey = new Hotkey();
        g_camHotkeyBound = true;
        g_camHotkeyText = camKey;
        g_camHotkey->setBinding(camKey.c_str());
        if (g_camHotkey->bound()) {
            Log::get().note("concourse camera test: hotkey.concourse_camera_test = \"%s\" bound (TEMPORARY, flat only: each press steps off, log only, look, bake; on a ride up, from the anchor's first sighting, the look acts for %llu ms and the bake for a few frames)",
                            camKey.c_str(), static_cast<unsigned long long>(cc::kCamTestMs));
            camTestInstall("bind");
        } else {
            if (g_camModeByKey) {   // the mode was the key's: the automatic bake takes it back at the next poll
                g_camMode = 0;
                g_camModeByKey = false;
            }
            if (!camKey.empty()) Log::get().note("concourse camera test: hotkey.concourse_camera_test = \"%s\" bound nothing", camKey.c_str());
            else if (!first) Log::get().note("concourse camera test: hotkey.concourse_camera_test cleared (the mode is off; an installed hook stays, shut)");
        }
    }
    {   // TEMPORARY (the VR proving build): the two probe keys.
        const auto bindProbe = [](VrProbeKey& key, const char* name, const std::string& text, const char* what) {
            if (key.bound && text == key.text) return;
            const bool first = !key.bound;
            if (!key.hk) key.hk = new Hotkey();
            key.bound = true;
            key.text = text;
            key.hk->setBinding(text.c_str());
            if (key.hk->bound()) Log::get().note("concourse vr probe: %s = \"%s\" bound (TEMPORARY, VR only, log only: %s)", name, text.c_str(), what);
            else if (!text.empty()) Log::get().note("concourse vr probe: %s = \"%s\" bound nothing", name, text.c_str());
            else if (!first) Log::get().note("concourse vr probe: %s cleared", name);
        };
        bindProbe(g_vrHoldKey, "hotkey.concourse_vr_hold_probe", cfg.getString("hotkey.concourse_vr_hold_probe", ""), "each press asks the runtime to hold 10 frames");
        bindProbe(g_vrCameraKey, "hotkey.concourse_vr_camera_probe", cfg.getString("hotkey.concourse_vr_camera_probe", ""),
                  "each press raises the rendered head 30 m for 5 frames and logs the tone draws");
    }
    const std::string roomKey = cfg.getString("hotkey.concourse_room_test", "");
    if (!g_roomHotkeyBound || roomKey != g_roomHotkeyText) {
        const bool first = !g_roomHotkeyBound;
        if (!g_roomHotkey) g_roomHotkey = new Hotkey();
        g_roomHotkeyBound = true;
        g_roomHotkeyText = roomKey;
        g_roomHotkey->setBinding(roomKey.c_str());
        if (g_roomHotkey->bound()) {
            Log::get().note("concourse doors: hotkey.concourse_room_test = \"%s\" bound (TEMPORARY: each press steps the door rule: records, records and rigs, off)", roomKey.c_str());
        } else {
            if (!roomKey.empty()) Log::get().note("concourse doors: hotkey.concourse_room_test = \"%s\" bound nothing", roomKey.c_str());
            else if (!first) Log::get().note("concourse doors: hotkey.concourse_room_test cleared (the door rule stays %s)", cc::doorRuleName(g_doorMode));
        }
    }
}

bool g_concSurveyListPending = false;   // the next survey is the press's or F3's: it lists the culled names

// The capture begins: the cull is lifted for cc::kBackdropLiftMs, the outside effects stay withheld. `manual`: F6's (its result is a toast), else the gate's.
void concourseBackdropStart(uint64_t now, bool manual) {
    g_backdropAutoRun = !manual;
    if (manual) ++g_backdropManualCount;
    else ++g_backdropAutoCount;
    g_backdropPhase = BackdropPhase::kLifting;
    g_backdropStartMs = now;
    Log::get().note("concourse backdrop: capture requested (%s): the cull lifts for %llu ms, the outside effects stay withheld", manual ? "F6" : "automatic",
                    static_cast<unsigned long long>(cc::kBackdropLiftMs));
}

// A press of hotkey.concourse_view_test: the backdrop capture. Only while the cull's own part is on.
void concourseBackdropPress(bool cullPartOn, uint64_t now) {
    if (g_backdropPhase != BackdropPhase::kIdle) {
        Log::get().note("concourse backdrop: press ignored: a capture is already running");
        return;
    }
    if (!cullPartOn) {
        Log::get().note("concourse backdrop: press refused: the cull's own part is not on (no cull, the bay is clearing, or not in the concourse)");
        return;
    }
    concoursePaneSetBackdropMode("hotkey.concourse_view_test");   // also turns automatic capture on
    concourseBackdropStart(now, true);
}

// One tick of the capture. True while the cull is to stay lifted. The result is logged once, whichever way it ends, and what it says tells the ways apart: never armed
// ("no result"), armed with no pane drawn ("no pane draw reached the scene copy"), scene copied with no pane captured, refused (the pane module's own line says why), done.
bool concourseBackdropStep(uint64_t now, bool wantCull) {
    if (g_backdropPhase == BackdropPhase::kIdle) return false;
    const unsigned long long lifted = static_cast<unsigned long long>(now - g_backdropStartMs);
    const auto finish = [&]() {
        g_backdropLiftedMs += lifted;
        g_backdropGate.captureEnded(now);
        concoursePaneBackdropEnd();
        g_backdropPhase = BackdropPhase::kIdle;
        return false;
    };
    if (!wantCull) {
        Log::get().note("concourse backdrop: capture abandoned after %llu ms: the cull is no longer wanted (left the concourse, the hangar sealed, or the key is off)", lifted);
        return finish();
    }
    if (g_backdropPhase == BackdropPhase::kLifting) {
        if (now - g_backdropStartMs >= cc::kBackdropLiftMs) {
            concoursePaneArmCapture(!g_backdropAutoRun);
            g_backdropPhase = BackdropPhase::kArmed;
            g_backdropArmMs = now;
        }
        return true;
    }
    const ConcourseBackdropStatus st = concoursePaneBackdropStatus();
    switch (st.phase) {
        case ConcourseBackdropPhase::kDone:
            Log::get().note("concourse backdrop: capture: lifted %llu ms, scene %ux%u format %s, %u pane draws captured, origin %s, %u capture%s in this cube", lifted, st.sceneW, st.sceneH,
                            st.sceneFormat, st.paneDraws, st.originSet ? "set" : "kept", st.captures, st.captures == 1 ? "" : "s");
            return finish();
        case ConcourseBackdropPhase::kRefused:
            Log::get().note("concourse backdrop: capture failed after %llu ms lifted: refused (%s)", lifted, st.refusal[0] ? st.refusal : "no reason recorded");
            return finish();
        case ConcourseBackdropPhase::kEmpty:
            if (!st.sceneCopied)
                Log::get().note("concourse backdrop: capture empty after %llu ms lifted: no pane draw reached the scene copy in %u frames (no pane was drawn, or the copy was refused: see the scene copy line)",
                                lifted, st.framesWaited);
            else
                Log::get().note("concourse backdrop: capture empty after %llu ms lifted: scene %ux%u format %s was copied, but 0 pane draws were captured in %u frames (no exterior pane drawn, or every capture pass was refused: see the capture pass line)",
                                lifted, st.sceneW, st.sceneH, st.sceneFormat, st.framesWaited);
            return finish();
        default:
            break;
    }
    if (now - g_backdropArmMs > 1000) {
        Log::get().note("concourse backdrop: capture never finished: no result 1000 ms after the arm (the pane module's phase is %s); lifted %llu ms",
                        st.phase == ConcourseBackdropPhase::kArmed ? "armed" : "idle, so it never armed", lifted);
        return finish();
    }
    return true;
}
// The setting is off (fix.optimized_stations): nothing runs. One line says so when the concourse key is on.
void concourseSettingOffTick(uint64_t now) {
    concoursePoll(now);
    const bool wasActive = g_concWasActive;
    g_concWanted.store(false, std::memory_order_release);
    g_liftGateOn.store(false, std::memory_order_release);
    if (g_backdropPhase != BackdropPhase::kIdle) {
        concoursePaneBackdropEnd();
        g_backdropPhase = BackdropPhase::kIdle;
    }
    g_backdropGate.reset();
    g_liftGate.reset();
    g_roomPub = RoomPub{};
    g_doorLeafWanted.store(false, std::memory_order_relaxed);
    for (cc::DoorWatch& dw : g_doorWatch) dw.reset();
    if (g_camTimer.stop()) concoursePaneBakeCancel();
    concoursePaneBakeArm(false);
    concoursePaneBakeRelease();
    if (!runtimeFlatProfile()) vrBakeShutdown(now);   // TEMPORARY (the VR bake): the stub let go and the near put back
    g_bakeRoomSinceMs = 0;
    g_bakeNoRideTries = 0;
    g_camLookOn.store(false, std::memory_order_release);
    g_camLookWasOn = false;
    g_camLookValid = false;
    if (g_camHooked && g_camMode != 0) {
        g_camMode = 0;
        concourseCameraTestControl(0, false, false);
        Log::get().note("concourse camera test: fix.optimized_stations is off: the mode is off");
    }
    g_liftWasOn = false;
    g_concWasCull = false;
    g_concActive.store(false, std::memory_order_release);
    g_concSurveyArmed.store(false, std::memory_order_release);
    g_concSurveyOn = false;
    g_concNamesOnly.store(false, std::memory_order_release);
    g_concNamesOnlyOn = false;
    g_concWasActive = false;
    g_concWaitSet = false;
    g_concWasWant = false;
    g_concClear.reset();
    concoursePaneSetFeed(ConcoursePaneFeed{});
    if ((g_concKeyOn || wasActive) && !g_concSettingOffLogged) {
        g_concSettingOffLogged = true;
        Log::get().note("concourse cull: off (fix.optimized_stations is off; the cull is part of it)");
    } else if (!g_concKeyOn && !wasActive) {
        g_concSettingOffLogged = false;
    }
}

// The names survey's report (the tick, once, after its two frames).
void concourseSurveyReport(bool listCulled, bool everything = false) {
    struct Row {
        std::string name;
        uint32_t n = 0;
        float r = 0.0f;
        float nearest = -1.0f;
    };
    struct WholeRow {
        std::string name;
        SurveyAgg a;
    };
    std::vector<Row> kept, culled, smallClose, unreadSphere;
    std::vector<WholeRow> whole;
    std::vector<SurveyClose> close, unnamed;
    try {
        Survey& sv = survey();
        std::lock_guard<std::mutex> lock(sv.m);
        unnamed = sv.unnamed;
        sv.unnamed.clear();
        for (const auto& kv : sv.names) {
            const SurveyAgg& a = kv.second;
            if (everything) whole.push_back({kv.first, a});
            if (a.kept) kept.push_back({kv.first, a.kept, a.radiusMax, a.nearest});
            if (a.culled) culled.push_back({kv.first, a.culled, a.cRadiusMax, a.cNearest});
            if (a.smallClose) smallClose.push_back({kv.first, a.smallClose, a.radiusMax});
            if (a.unreadSphere) unreadSphere.push_back({kv.first, a.unreadSphere, a.radiusMax});
        }
        close = sv.close;
        sv.names.clear();
        sv.close.clear();
    } catch (...) {
        Log::get().note("concourse names: the survey could not be read (out of memory)");
        return;
    }
    auto byCount = [](const Row& a, const Row& b) { return a.n != b.n ? a.n > b.n : a.name < b.name; };
    std::sort(kept.begin(), kept.end(), byCount);
    std::sort(culled.begin(), culled.end(), byCount);
    std::sort(smallClose.begin(), smallClose.end(), byCount);
    std::sort(unreadSphere.begin(), unreadSphere.end(), byCount);
    std::sort(close.begin(), close.end(), [](const SurveyClose& a, const SurveyClose& b) { return a.dist < b.dist; });
    Log::get().note("concourse names: survey over 2 frames: %u kept names, %u culled names, %u small-close names, %u unreadable-sphere names, %u culled records within %.0f m",
                    static_cast<unsigned>(kept.size()), static_cast<unsigned>(culled.size()), static_cast<unsigned>(smallClose.size()),
                    static_cast<unsigned>(unreadSphere.size()), static_cast<unsigned>(close.size()), static_cast<double>(kSurveyCloseM));
    if (g_camLookSurvey) {   // TEMPORARY: what could still stand in the bake's way: every name kept within 200 m of the room's centre, with the centre as the distance
        Log::get().note("concourse names: the survey above was taken in the look or the bake: the camera is at the room's centre");
        uint32_t listed = 0;
        for (size_t i = 0; i < kept.size() && listed < 200; ++i) {
            if (kept[i].nearest - kept[i].r > 200.0f) continue;
            Log::get().note("concourse names: kept near the centre %s x%u r=%.1f nearest=%.1f", kept[i].name.c_str(), kept[i].n, static_cast<double>(kept[i].r), static_cast<double>(kept[i].nearest));
            ++listed;
        }
        g_camLookSurvey = false;
    }
    if (everything) {
        // TEMPORARY: F3 with the cull off, at a layout the cull has not run at: every name, the nearest first, and where its nearest record is. The positions are world axes from the camera,
        // so two presses in different places give the room's shape, the camera's own move between them, and which way each thing lies.
        Log::get().note("concourse names: that was F3 with the cull off, so nothing was culled: name, close and culled say what the keep list would do. Type %s, camera %.1f %.1f %.1f (known %d), %u names",
                        journal_station::kindName(journalStationKind()), static_cast<double>(g_frameCam[0]), static_cast<double>(g_frameCam[1]), static_cast<double>(g_frameCam[2]),
                        g_frameCamValid ? 1 : 0, static_cast<unsigned>(whole.size()));
        std::sort(whole.begin(), whole.end(), [](const WholeRow& a, const WholeRow& b) {
            const bool ar = a.a.nearest >= 0.0f, br = b.a.nearest >= 0.0f;
            if (ar != br) return ar;
            if (a.a.nearest != b.a.nearest) return a.a.nearest < b.a.nearest;
            return a.name < b.name;
        });
        for (size_t i = 0; i < whole.size() && i < 600; ++i) {
            const SurveyAgg& a = whole[i].a;
            Log::get().note("concourse names: all %s name=%u close=%u culled=%u r=%.1f nearest=%.1f at %.1f %.1f %.1f", whole[i].name.c_str(), a.kept, a.smallClose + a.unreadSphere, a.culled,
                            static_cast<double>(a.radiusMax), static_cast<double>(a.nearest), static_cast<double>(a.nearestAt[0]), static_cast<double>(a.nearestAt[1]),
                            static_cast<double>(a.nearestAt[2]));
        }
        if (whole.size() > 600) Log::get().note("concourse names: %u more names not listed", static_cast<unsigned>(whole.size() - 600));
        std::sort(unnamed.begin(), unnamed.end(), [](const SurveyClose& a, const SurveyClose& b) { return a.radius > b.radius; });
        Log::get().note("concourse names: %u records with no readable name and a readable sphere", static_cast<unsigned>(unnamed.size()));
        for (size_t i = 0; i < unnamed.size() && i < 24; ++i)
            Log::get().note("concourse names: unnamed %s r=%.1f dist=%.1f", unnamed[i].name, static_cast<double>(unnamed[i].radius), static_cast<double>(unnamed[i].dist));
        return;
    }
    for (size_t i = 0; i < kept.size() && i < 80; ++i)
        Log::get().note("concourse names: kept %s x%u r=%.1f nearest=%.1f", kept[i].name.c_str(), kept[i].n, static_cast<double>(kept[i].r),
                        static_cast<double>(kept[i].nearest));
    if (listCulled) {
        // A press of the view test or F3: every culled name, the largest first.
        std::sort(culled.begin(), culled.end(), [](const Row& a, const Row& b) { return a.r != b.r ? a.r > b.r : a.name < b.name; });
        for (size_t i = 0; i < culled.size() && i < 150; ++i)
            Log::get().note("concourse names: culled %s x%u r=%.1f nearest=%.1f", culled[i].name.c_str(), culled[i].n, static_cast<double>(culled[i].r), static_cast<double>(culled[i].nearest));
        if (culled.size() > 150) Log::get().note("concourse names: %u more culled names not listed", static_cast<unsigned>(culled.size() - 150));
    } else {
        for (size_t i = 0; i < culled.size() && i < 80; ++i)
            Log::get().note("concourse names: culled %s x%u r=%.1f", culled[i].name.c_str(), culled[i].n, static_cast<double>(culled[i].r));
    }
    for (size_t i = 0; i < close.size() && i < 40; ++i)
        Log::get().note("concourse names: culled close %s dist=%.1f r=%.1f", close[i].name, static_cast<double>(close[i].dist), static_cast<double>(close[i].radius));
    for (size_t i = 0; i < smallClose.size() && i < 40; ++i) Log::get().note("concourse names: small-close %s x%u", smallClose[i].name.c_str(), smallClose[i].n);
    for (size_t i = 0; i < unreadSphere.size() && i < 40; ++i)
        Log::get().note("concourse names: unreadable-sphere %s x%u", unreadSphere[i].name.c_str(), unreadSphere[i].n);
    // The records with no readable name: the largest, since a hull would be among them.
    std::sort(unnamed.begin(), unnamed.end(), [](const SurveyClose& a, const SurveyClose& b) { return a.radius > b.radius; });
    Log::get().note("concourse names: %u records with no readable name and a readable sphere", static_cast<unsigned>(unnamed.size()));
    for (size_t i = 0; i < unnamed.size() && i < 24; ++i)
        Log::get().note("concourse names: unnamed %s r=%.1f dist=%.1f", unnamed[i].name, static_cast<double>(unnamed[i].radius), static_cast<double>(unnamed[i].dist));
}

// H6: the camera in the anchor record's frame, once a second, while on foot in social space with the key armed (the cull on or off).
void concourseFrameTrace(uint64_t now) {
    if (now - g_concTraceMs < 1000) return;
    g_concTraceMs = now;
    // A line that says why there is no pose is written when the reason changes, and again every 10 s: never a fake zero.
    auto said = [&](uint32_t reason) {
        if (reason == g_concTraceReason && now - g_concTraceReasonMs < 10000) return false;
        g_concTraceReason = reason;
        g_concTraceReasonMs = now;
        return true;
    };
    if (!g_frameCamValid) {
        if (said(1)) Log::get().note("concourse frame: no line: the camera is not known this tick");
        return;
    }
    const uintptr_t anchor = g_concAnchorAddr.load(std::memory_order_relaxed);
    if (!anchor) {
        if (said(2)) Log::get().note("concourse frame: no line: no %s record latched yet (the store has not seen one)", cc::kAnchorName);
        return;
    }
    char nm[64] = {};
    if (!cachedName(anchor, nm, sizeof(nm)) || !cc::concourseAnchorName(nm)) {
        uintptr_t expect = anchor;
        g_concAnchorAddr.compare_exchange_strong(expect, 0, std::memory_order_relaxed);
        Log::get().note("concourse frame: the latched anchor @%llx no longer reads %s (it reads \"%s\"); the latch is dropped until the store sees it again",
                        static_cast<unsigned long long>(static_cast<uint64_t>(anchor) & 0xFFFFFFFFFFull), cc::kAnchorName, nm);
        return;
    }
    const WorldRead aw = worldOfRecord(anchor);
    float axes[3][3] = {};
    float ext[3] = {};
    bool extOk = false;
    uint8_t osrc = 4;
    if (aw.source == 4 || !poseOrientOf(anchor, axes, ext, &extOk, &osrc)) {
        if (said(3))
            Log::get().note("concourse frame: no line: the anchor's pose cannot be read (centre source %u, orientation source %u; 4 is none)", static_cast<unsigned>(aw.source),
                            static_cast<unsigned>(osrc));
        return;
    }
    float d[3] = {g_frameCam[0] - aw.world[0], g_frameCam[1] - aw.world[1], g_frameCam[2] - aw.world[2]};
    float local[3] = {};
    for (int k = 0; k < 3; ++k) local[k] = axes[k][0] * d[0] + axes[k][1] * d[1] + axes[k][2] * d[2];
    char hub[260] = {};
    const uintptr_t hubAddr = g_concHubAddr.load(std::memory_order_relaxed);
    if (!hubAddr) {
        std::snprintf(hub, sizeof(hub), "hub %s not latched yet", cc::kHubName);
    } else {
        char hn[64] = {};
        if (!cachedName(hubAddr, hn, sizeof(hn)) || std::strcmp(hn, cc::kHubName) != 0) {
            uintptr_t expect = hubAddr;
            g_concHubAddr.compare_exchange_strong(expect, 0, std::memory_order_relaxed);
            std::snprintf(hub, sizeof(hub), "hub %s: the latched record no longer reads that name; latch dropped", cc::kHubName);
        } else {
            const WorldRead hw = worldOfRecord(hubAddr);
            if (hw.source == 4) {
                std::snprintf(hub, sizeof(hub), "hub %s: the pose cannot be read", cc::kHubName);
            } else {
                const float hd[3] = {hw.world[0] - aw.world[0], hw.world[1] - aw.world[1], hw.world[2] - aw.world[2]};
                float hl[3] = {};
                for (int k = 0; k < 3; ++k) hl[k] = axes[k][0] * hd[0] + axes[k][1] * hd[1] + axes[k][2] * hd[2];
                std::snprintf(hub, sizeof(hub), "hub %s centre %.2f %.2f %.2f r %.1f (in the anchor's frame %.2f %.2f %.2f)", hn, static_cast<double>(hw.world[0]),
                              static_cast<double>(hw.world[1]), static_cast<double>(hw.world[2]), static_cast<double>(hw.radius), static_cast<double>(hl[0]),
                              static_cast<double>(hl[1]), static_cast<double>(hl[2]));
            }
        }
    }
    g_concTraceReason = 0;
    Log::get().note("concourse frame: cam_local %.2f %.2f %.2f anchor centre %.2f %.2f %.2f r %.1f axes (%.3f %.3f %.3f) (%.3f %.3f %.3f) (%.3f %.3f %.3f) src %u/%u, %s",
                    static_cast<double>(local[0]), static_cast<double>(local[1]), static_cast<double>(local[2]), static_cast<double>(aw.world[0]),
                    static_cast<double>(aw.world[1]), static_cast<double>(aw.world[2]), static_cast<double>(aw.radius), static_cast<double>(axes[0][0]),
                    static_cast<double>(axes[0][1]), static_cast<double>(axes[0][2]), static_cast<double>(axes[1][0]), static_cast<double>(axes[1][1]),
                    static_cast<double>(axes[1][2]), static_cast<double>(axes[2][0]), static_cast<double>(axes[2][1]), static_cast<double>(axes[2][2]),
                    static_cast<unsigned>(aw.source), static_cast<unsigned>(osrc), hub);
}

// The camera in the anchor record's frame (the frame trace's coordinates), for the log lines. False when it cannot be read now.
bool concourseCamLocal(float out[3]) {
    if (!g_frameCamValid) return false;
    const uintptr_t anchor = g_concAnchorAddr.load(std::memory_order_relaxed);
    if (!anchor) return false;
    char nm[64] = {};
    if (!cachedName(anchor, nm, sizeof(nm)) || !cc::concourseAnchorName(nm)) return false;
    const WorldRead aw = worldOfRecord(anchor);
    float axes[3][3] = {};
    float ext[3] = {};
    bool extOk = false;
    uint8_t osrc = 4;
    if (aw.source == 4 || !poseOrientOf(anchor, axes, ext, &extOk, &osrc)) return false;
    const float d[3] = {g_frameCam[0] - aw.world[0], g_frameCam[1] - aw.world[1], g_frameCam[2] - aw.world[2]};
    for (int k = 0; k < 3; ++k) out[k] = axes[k][0] * d[0] + axes[k][1] * d[1] + axes[k][2] * d[2];
    return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
}

// The anchor's local up axis in world (axes[1] of its pose), for the steady glow's sky test. False when it cannot be read now.
bool concourseAnchorUp(float out[3]) {
    const uintptr_t anchor = g_concAnchorAddr.load(std::memory_order_relaxed);
    if (!anchor) return false;
    char nm[64] = {};
    if (!cachedName(anchor, nm, sizeof(nm)) || !cc::concourseAnchorName(nm)) return false;
    const WorldRead aw = worldOfRecord(anchor);
    float axes[3][3] = {};
    float ext[3] = {};
    bool extOk = false;
    uint8_t osrc = 4;
    if (aw.source == 4 || !poseOrientOf(anchor, axes, ext, &extOk, &osrc)) return false;
    for (int k = 0; k < 3; ++k) out[k] = axes[1][k];
    const float len = std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
    if (!std::isfinite(len) || len < 0.5f) return false;
    for (int k = 0; k < 3; ++k) out[k] /= len;
    return true;
}

// TEMPORARY (the camera test): the anchor's centre (the frame the camera's position is in: `concourse frame` lines, checked by hand against its cam_local) and its three unit axes;
// axes[1] is its up. False when any cannot be read now.
bool concourseAnchorFrame(float centre[3], float axes[3][3]) {
    const uintptr_t anchor = g_concAnchorAddr.load(std::memory_order_relaxed);
    if (!anchor) return false;
    char nm[64] = {};
    if (!cachedName(anchor, nm, sizeof(nm)) || !cc::concourseAnchorName(nm)) return false;
    const WorldRead aw = worldOfRecord(anchor);
    float ext[3] = {};
    bool extOk = false;
    uint8_t osrc = 4;
    if (aw.source == 4 || !poseOrientOf(anchor, axes, ext, &extOk, &osrc)) return false;
    if (!std::isfinite(aw.world[0]) || !std::isfinite(aw.world[1]) || !std::isfinite(aw.world[2])) return false;
    for (int a = 0; a < 3; ++a) {
        const float len = std::sqrt(axes[a][0] * axes[a][0] + axes[a][1] * axes[a][1] + axes[a][2] * axes[a][2]);
        if (!std::isfinite(len) || len < 0.5f) return false;
        for (int k = 0; k < 3; ++k) axes[a][k] /= len;
    }
    for (int k = 0; k < 3; ++k) centre[k] = aw.world[k];
    return true;
}

// The clearing machine's step and its log lines, and the pane share line. Returns the machine's output; `sh` is this frame's reading of the panes.
cc::ClearOutput concourseClearStep(uint64_t now, bool wantCull, bool gateWanted, const ConcoursePaneShares& sh) {
    cc::ClearInput in;
    in.nowMs = now;
    in.gate = wantCull;
    in.windowShare = sh.valid ? sh.exterior : -1.0f;
    in.shareSeq = sh.seq;
    in.enterShare = g_concEnterShare;   // the shop window's 0.3 entry scale is inside the total (cc::exteriorGlassShare); the exit follows at 0.4 of the share
    const cc::ClearPhase before = g_concClear.phase();
    const cc::ClearOutput out = g_concClear.step(in);
    if (out.phase != before) {
        float local[3] = {};
        const bool camOk = concourseCamLocal(local);   // for the log line only: the machine follows the outside glass's share of the view, not the camera
        char pane[48] = {};
        if (sh.valid) std::snprintf(pane, sizeof(pane), "%.1f%%", 100.0 * static_cast<double>(sh.exterior));
        else std::snprintf(pane, sizeof(pane), "unread");
        char cam[96] = {};
        if (camOk) std::snprintf(cam, sizeof(cam), "cam_local %.2f %.2f %.2f", static_cast<double>(local[0]), static_cast<double>(local[1]), static_cast<double>(local[2]));
        else std::snprintf(cam, sizeof(cam), "cam_local unreadable");
        Log::get().note("concourse clear: %s (%sexterior glass %s of the view, weighted, %s)", cc::clearPhaseName(out.phase), wantCull ? "" : "gate lost, ", pane, cam);
    }
    // The shares line: at most once a second while the gate holds, when a share moved by over half a point or a window type came or went.
    if (gateWanted && sh.valid && sh.seq != g_concShareSeq && now - g_concShareLogMs >= 1000) {
        g_concShareSeq = sh.seq;
        bool changed = sh.count != g_concShareLast.count;
        for (uint32_t i = 0; i < sh.count && !changed; ++i) {
            uint32_t k = 0;
            while (k < g_concShareLast.count && g_concShareLast.n[k] != sh.n[i]) ++k;
            if (k == g_concShareLast.count || std::fabs(g_concShareLast.share[k] - sh.share[i]) > 0.005f) changed = true;
        }
        if (changed) {
            g_concShareLogMs = now;
            g_concShareLast = sh;
            char text[200] = {};
            size_t used = 0;
            for (uint32_t i = 0; i < sh.count; ++i) {
                const int w = std::snprintf(text + used, sizeof(text) - used, "%sn=%u %.1f%%", i ? " " : "", sh.n[i], 100.0 * static_cast<double>(sh.share[i]));
                if (w > 0 && used + static_cast<size_t>(w) < sizeof(text)) used += static_cast<size_t>(w);
            }
            if (sh.count == 0) std::snprintf(text, sizeof(text), "no family draw");
            float sl[3] = {};
            if (concourseCamLocal(sl))
                Log::get().note("concourse pane shares: exterior %.1f%% (weighted) %s cam_local %.2f %.2f %.2f", 100.0 * static_cast<double>(sh.exterior), text, static_cast<double>(sl[0]), static_cast<double>(sl[1]), static_cast<double>(sl[2]));
            else
                Log::get().note("concourse pane shares: exterior %.1f%% (weighted) %s", 100.0 * static_cast<double>(sh.exterior), text);
        }
    }
    return out;
}

// ---- TEMPORARY: the walk survey's tick (log only; nothing it reads or writes reaches a veto; removed when the arc closes) -----------------------------------
// Armed key and docked: "walk survey: on" once, then a line a second (900 a session), and the names report (see WalkAgg).
bool g_walkOn = false;
uint32_t g_walkLinesSession = 0, g_walkNamesSession = 0;
uint64_t g_walkLineMs = 0, g_walkNamesDueMs = 0;
uint32_t g_walkTicks = 0;
bool g_walkZoneSeen = false;
cc::WalkZone g_walkZone = cc::WalkZone::kNoCell;
cc::WalkZone g_walkArmZone = cc::WalkZone::kNoCell;
char g_walkArmCam[80] = {};
constexpr uint32_t kWalkLinesCap = 900, kWalkNamesCap = 300;

// The camera's place in the pick cell: the text of "cam in bay ...", and M, how far past the box (0 inside).
// The pick cell the walk survey reads is the last one with `has` true, kept for kWalkHoldMs: on foot the pick flickers from tick to tick.
sealed::OwnCell g_walkHeld;
bool g_walkHeldValid = false;
uint64_t g_walkHeldMs = 0;
uint64_t g_walkLastArmMs = 0;
constexpr uint64_t kWalkHoldMs = 10000;

void walkCamText(const sealed::OwnCell& cell, bool hasCell, char* out, size_t cap, float* pastM) noexcept {
    *pastM = 0.0f;
    if (!g_frameCamValid) {
        std::snprintf(out, cap, "cam not known");
    } else if (!hasCell) {
        std::snprintf(out, cap, "cam in bay no cell");
    } else if (cell.box) {
        float x[3] = {};
        sealed::boxCoordsOf(cell, g_frameCam, x);
        for (int k = 0; k < 3; ++k) *pastM = std::max(*pastM, std::fabs(x[k]) - cell.boxHalf[k]);
        std::snprintf(out, cap, "cam in bay %.1f %.1f %.1f of half %.1f %.1f %.1f m", static_cast<double>(x[0]), static_cast<double>(x[1]), static_cast<double>(x[2]),
                      static_cast<double>(cell.boxHalf[0]), static_cast<double>(cell.boxHalf[1]), static_cast<double>(cell.boxHalf[2]));
    } else {
        const float d = sealed::distance(cell.centre, g_frameCam);
        *pastM = std::max(0.0f, d - cell.radius);
        std::snprintf(out, cap, "cam in bay no box, %.1f m from the centre of radius %.1f m", static_cast<double>(d), static_cast<double>(cell.radius));
    }
    if (*pastM < 0.0f) *pastM = 0.0f;
}

void walkSurveyReport() noexcept {
    uint32_t total = 0, culled = 0;
    struct Row {
        std::string name;
        WalkAgg::Name e;
    };
    std::vector<Row> rows;
    try {
        WalkAgg& a = walkAgg();
        std::lock_guard<std::mutex> lock(a.m);
        total = a.total;
        culled = a.culled;
        for (const auto& kv : a.names) rows.push_back(Row{kv.first, kv.second});
        a.total = a.culled = 0;
        a.names.clear();
    } catch (...) {
        return;
    }
    std::sort(rows.begin(), rows.end(), [](const Row& x, const Row& y) { return x.e.nearest < y.e.nearest; });
    // Log::note holds 1200 characters a line, so the report runs on over as many lines as the names need.
    char text[1000] = {};
    int used = std::snprintf(text, sizeof(text), "walk survey names (zone %s, %s): would cull %u of %u records (%.0f%%), %u names within %.0f m", cc::walkZoneName(g_walkArmZone),
                             g_walkArmCam, culled, total, total ? 100.0 * static_cast<double>(culled) / static_cast<double>(total) : 0.0,
                             static_cast<unsigned>(rows.size()), static_cast<double>(kWalkNearM));
    const size_t shown = std::min<size_t>(rows.size(), 30);
    for (size_t i = 0; i < shown && used > 0; ++i) {
        char one[160] = {};
        const int w = std::snprintf(one, sizeof(one), "%s %s (%u records, nearest %.1f m, radius %.1f m)", i == 0 ? "; nearest would-cull:" : ";", rows[i].name.c_str(), rows[i].e.n,
                                    static_cast<double>(rows[i].e.nearest), static_cast<double>(rows[i].e.radius));
        if (w <= 0) break;
        if (static_cast<size_t>(used) + static_cast<size_t>(w) + 1 >= sizeof(text)) {
            Log::get().note("%s", text);
            used = std::snprintf(text, sizeof(text), "walk survey names (cont.)");
        }
        std::snprintf(text + used, sizeof(text) - static_cast<size_t>(used), "%s", one);
        used += std::min(w, static_cast<int>(sizeof(one)) - 1);
    }
    Log::get().note("%s", text);
}

// ---- TEMPORARY (the lift survey's report; log only, removed when the arc closes) ------------------------------------------------------------------------------
// At most every 33 ms: the lift parts seen in the last 60 frames and within 12 m (surface) of the camera, as one line when a position moved over 0.02 m since
// it was last logged, the set changed, or a second has passed. Parts unseen for 600 frames are dropped from the table.
constexpr uint32_t kLiftLinesCap = 3000, kLiftSeenFrames = 60, kLiftDropFrames = 600;
constexpr float kLiftReachM = 12.0f, kLiftMovedM = 0.02f, kLiftDoorReachM = 4.0f;   // the last: a door record counts as the cabin's within this of its centre
constexpr uint64_t kLiftPollMs = 33, kLiftRepeatMs = 1000;
bool g_liftOn = false;
uint64_t g_liftPollMsLast = 0, g_liftLineMs = 0;
uint32_t g_liftLines = 0;
bool g_liftCapNoted = false, g_liftOverflowNoted = false;
std::vector<uintptr_t> g_liftLastSet;

void liftSurveyLine(const char* text) noexcept {
    if (g_liftLines < kLiftLinesCap) {
        Log::get().note("%s", text);
    } else if (!g_liftCapNoted) {
        g_liftCapNoted = true;
        Log::get().note("lift survey: %u lines written; no more lines this session", kLiftLinesCap);
    }
    ++g_liftLines;
}

int liftPosText(char* out, size_t cap, const char* tag, const float p[3]) noexcept {
    if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2])) return 0;
    const int n = std::snprintf(out, cap, " %s %.2f %.2f %.2f", tag, static_cast<double>(p[0]), static_cast<double>(p[1]), static_cast<double>(p[2]));
    return n > 0 && static_cast<size_t>(n) < cap ? n : 0;
}

void liftSurveyTick(uint64_t now) noexcept {
    try {
        if (!g_liftOn) {
            g_liftOn = true;
            g_liftPollMsLast = 0;
            g_liftLineMs = 0;
            g_liftLastSet.clear();
            g_liftSurveyOn.store(true, std::memory_order_relaxed);
            if (g_liftLines < kLiftLinesCap) Log::get().note("lift survey: on");
        }
        if (!g_frameCamValid || g_liftLines > kLiftLinesCap || now - g_liftPollMsLast < kLiftPollMs) return;
        g_liftPollMsLast = now;
        const uint32_t stampNow = g_frameStamp.load(std::memory_order_relaxed);
        std::vector<LiftPart> seen;
        uint64_t overflow = 0;
        {
            LiftAgg& a = liftAgg();
            std::lock_guard<std::mutex> lock(a.mtx);
            overflow = a.overflow;
            for (size_t i = 0; i < a.used;) {
                const int32_t age = static_cast<int32_t>(stampNow - a.parts[i].stamp);   // negative: seen by a callback that read the next frame's stamp
                if (age > static_cast<int32_t>(kLiftDropFrames)) {
                    a.parts[i] = a.parts[--a.used];
                    continue;
                }
                const LiftPart& e = a.parts[i];
                if (age <= static_cast<int32_t>(kLiftSeenFrames)) {
                    const float surface = std::max(0.0f, sealed::distance(e.c, g_frameCam) - e.r);
                    if (surface <= kLiftReachM) seen.push_back(e);
                }
                ++i;
            }
        }
        if (overflow != 0 && !g_liftOverflowNoted) {
            g_liftOverflowNoted = true;
            Log::get().note("lift survey: the table of %u parts overflowed, %llu records not kept", static_cast<unsigned>(kLiftSlots), static_cast<unsigned long long>(overflow));
        }
        if (seen.empty()) {
            if (!g_liftLastSet.empty()) g_liftLastSet.clear();
            return;
        }
        std::sort(seen.begin(), seen.end(), [](const LiftPart& x, const LiftPart& y) {
            const int c = std::strcmp(x.name, y.name);
            return c != 0 ? c < 0 : x.addr < y.addr;
        });
        std::vector<uintptr_t> set;
        for (const LiftPart& e : seen) set.push_back(e.addr);
        bool emit = set != g_liftLastSet || g_liftLineMs == 0 || now - g_liftLineMs >= kLiftRepeatMs;
        for (const LiftPart& e : seen) {
            if (cc::liftMoved(e.c, e.lastC, kLiftMovedM) || cc::liftMoved(e.m, e.lastM, kLiftMovedM) || cc::liftMoved(e.k, e.lastK, kLiftMovedM) ||
                cc::liftMoved(e.w, e.lastW, kLiftMovedM))
                emit = true;
        }
        if (!emit) return;
        g_liftLastSet = set;
        g_liftLineMs = now;
        constexpr const char* kCont = "lift survey (cont.):";
        char text[1000] = {};
        int used = std::snprintf(text, sizeof(text), "lift survey: cam %.2f %.2f %.2f, %u parts:", static_cast<double>(g_frameCam[0]), static_cast<double>(g_frameCam[1]),
                                 static_cast<double>(g_frameCam[2]), static_cast<unsigned>(seen.size()));
        bool first = true;
        for (const LiftPart& e : seen) {
            char one[400] = {};
            int n = std::snprintf(one, sizeof(one), "%s %s a=%04X s=%u", first ? "" : ";", e.name, static_cast<unsigned>(e.addr & 0xFFFFu), static_cast<unsigned>(e.src));
            n += liftPosText(one + n, sizeof(one) - static_cast<size_t>(n), "c", e.c);
            n += std::snprintf(one + n, sizeof(one) - static_cast<size_t>(n), " r %.2f", static_cast<double>(e.r));
            n += liftPosText(one + n, sizeof(one) - static_cast<size_t>(n), "m", e.m);
            n += liftPosText(one + n, sizeof(one) - static_cast<size_t>(n), "k", e.k);
            n += liftPosText(one + n, sizeof(one) - static_cast<size_t>(n), "w", e.w);
            if (static_cast<size_t>(used) + static_cast<size_t>(n) + 1 >= sizeof(text)) {
                liftSurveyLine(text);
                used = std::snprintf(text, sizeof(text), "%s", kCont);
                if (!first) std::memmove(one, one + 1, std::strlen(one));   // drop the ';': the continuation's colon leads
            }
            std::snprintf(text + used, sizeof(text) - static_cast<size_t>(used), "%s", one);
            used += static_cast<int>(std::strlen(text + used));
            first = false;
        }
        liftSurveyLine(text);
        // The logged values: the next line compares to these.
        LiftAgg& a = liftAgg();
        std::lock_guard<std::mutex> lock(a.mtx);
        for (const LiftPart& e : seen) {
            for (size_t i = 0; i < a.used; ++i) {
                if (a.parts[i].addr != e.addr) continue;
                for (int k = 0; k < 3; ++k) {
                    a.parts[i].lastC[k] = e.c[k];
                    a.parts[i].lastM[k] = e.m[k];
                    a.parts[i].lastK[k] = e.k[k];
                    a.parts[i].lastW[k] = e.w[k];
                }
                break;
            }
        }
    } catch (...) {
    }
}

// ---- The lift gate's reading (the survey's table, `c`): the cabin record named TurboLift nearest the camera, and the four door records nearest it ------------------
cc::LiftReading liftReadNow() noexcept {
    cc::LiftReading r;
    try {
        const uint32_t stampNow = g_frameStamp.load(std::memory_order_relaxed);
        LiftAgg& a = liftAgg();
        std::lock_guard<std::mutex> lock(a.mtx);
        const LiftPart* cabin = nullptr;
        float best = 0.0f;
        for (size_t i = 0; i < a.used; ++i) {
            const LiftPart& e = a.parts[i];
            if (static_cast<int32_t>(stampNow - e.stamp) > static_cast<int32_t>(kLiftSeenFrames) || std::strcmp(e.name, "TurboLift") != 0) continue;
            const float d = sealed::distance(e.c, g_frameCam);
            if (!cabin || d < best) {
                cabin = &e;
                best = d;
            }
        }
        if (!cabin) return r;
        r.cabin = true;
        r.cabinDistM = best;
        static const char* const kDoors[4] = {"TurboLiftDoorLeft", "TurboLiftDoorRight", "TurboLiftDoorLeft2", "TurboLiftDoorRight2"};
        const LiftPart* door[4] = {};
        float db[4] = {};
        for (size_t i = 0; i < a.used; ++i) {
            const LiftPart& e = a.parts[i];
            if (static_cast<int32_t>(stampNow - e.stamp) > static_cast<int32_t>(kLiftSeenFrames)) continue;
            for (int k = 0; k < 4; ++k) {
                if (std::strcmp(e.name, kDoors[k]) != 0) continue;
                const float d = sealed::distance(e.c, cabin->c);
                if (d <= kLiftDoorReachM && (!door[k] || d < db[k])) {
                    door[k] = &e;
                    db[k] = d;
                }
            }
        }
        if (door[0] && door[1] && door[2] && door[3]) {
            r.doors = true;
            r.gap1 = sealed::distance(door[0]->c, door[1]->c);
            r.gap2 = sealed::distance(door[2]->c, door[3]->c);
        }
    } catch (...) {
    }
    return r;
}

void walkSurveyTick(uint64_t now, bool camIn) {
    // On foot only: the cull is on from launch now, and these surveys have no business while the commander sits in the ship.
    const bool gate = concourseArmed() && g_dockedNow.load(std::memory_order_acquire) && (g_statusFlags2Now & sealed::kFlags2OnFoot) != 0;
    if (!gate) {
        g_liftOn = false;
        g_liftSurveyOn.store(false, std::memory_order_relaxed);
        g_walkOn = false;
        g_walkPassArmed = false;
        g_walkZoneSeen = false;
        g_walkHeldValid = false;
        return;
    }
    liftSurveyTick(now);   // TEMPORARY, log only (the lift survey)
    // The held pick cell: a copy of the last pick with a cell, good for kWalkHoldMs, and the camera tested against it (the caller's camIn is the raw pick's).
    if (g_pickCell.has) {
        g_walkHeld = g_pickCell;
        g_walkHeldValid = true;
        g_walkHeldMs = now;
    } else if (g_walkHeldValid && now - g_walkHeldMs > kWalkHoldMs) {
        g_walkHeldValid = false;
    }
    const sealed::OwnCell& cell = g_walkHeld;
    const bool hasCell = g_walkHeldValid;
    camIn = false;
    if (g_frameCamValid && hasCell) {
        if (cell.box) {
            float x[3] = {};
            sealed::boxCoordsOf(cell, g_frameCam, x);
            camIn = std::fabs(x[0]) <= cell.boxHalf[0] && std::fabs(x[1]) <= cell.boxHalf[1] && std::fabs(x[2]) <= cell.boxHalf[2];
        } else {
            camIn = sealed::distance(cell.centre, g_frameCam) <= cell.radius;
        }
    }
    const uint64_t anchorMs = g_concAnchorMs.load(std::memory_order_relaxed);
    const bool anchorFresh = anchorMs != 0 && now - anchorMs <= kConcAnchorFreshMs;
    const cc::WalkZone zone = cc::walkZone(g_aboardNow, anchorFresh, hasCell, camIn);
    if (!g_walkOn) {
        g_walkOn = true;
        g_walkLineMs = now;
        g_walkNamesDueMs = now;
        g_walkTicks = 0;
        if (g_walkLinesSession < kWalkLinesCap) Log::get().note("walk survey: on");
    }
    ++g_walkTicks;
    // The pass armed on the last tick has been read by the store: the report, then the pass is off for this publication.
    if (g_walkPassArmed) {
        g_walkPassArmed = false;
        walkSurveyReport();
    }
    const bool zoneChanged = !g_walkZoneSeen || zone != g_walkZone;
    g_walkZoneSeen = true;
    g_walkZone = zone;
    if (now - g_walkLineMs >= 1000 && g_walkLinesSession <= kWalkLinesCap) {
        char camText[160] = {};
        float past = 0.0f;
        walkCamText(cell, hasCell, camText, sizeof(camText), &past);
        const double meanMs = g_walkTicks ? static_cast<double>(now - g_walkLineMs) / static_cast<double>(g_walkTicks) : 0.0;
        if (g_walkLinesSession < kWalkLinesCap) {
            Log::get().note("walk survey: zone %s, seated %d, on foot %d, in hangar %d, social %d, anchor age %lld ms, %s, past the box by %.1f m, pad %s, pad in pass %d, seal %s, "
                            "frame mean %.2f ms over %u frames",
                            cc::walkZoneName(zone), g_aboardNow ? 1 : 0, (g_statusFlags2Now & sealed::kFlags2OnFoot) != 0 ? 1 : 0,
                            (g_statusFlags2Now & sealed::kFlags2OnFootInHangar) != 0 ? 1 : 0, (g_statusFlags2Now & sealed::kFlags2OnFootSocialSpace) != 0 ? 1 : 0,
                            anchorMs != 0 ? static_cast<long long>(now - anchorMs) : -1ll, camText, static_cast<double>(past), hasCell ? cell.padName : "none",
                            g_stickyPadInPass ? 1 : 0, g_heldValid ? "held" : "not held", meanMs, static_cast<unsigned>(g_walkTicks));
        } else {
            Log::get().note("walk survey: %u lines written; no more lines this session", kWalkLinesCap);
        }
        ++g_walkLinesSession;
        g_walkLineMs = now;
        g_walkTicks = 0;
    }
    // The names report: unsealed, with a pick cell, out of the concourse; every 5 s and at a zone change. One store pass judges against the pick cell.
    // A zone change may arm a report only if a second has passed since the last one (a flicker of the zone cannot spend the session's reports).
    if (!g_heldValid && hasCell && g_frameCamValid && zone != cc::WalkZone::kConcourse && ((zoneChanged && now - g_walkLastArmMs >= 1000) || now >= g_walkNamesDueMs)) {
        g_walkLastArmMs = now;
        if (g_walkNamesSession < kWalkNamesCap) {
            try {
                WalkAgg& a = walkAgg();
                std::lock_guard<std::mutex> lock(a.m);
                a.total = a.culled = 0;
                a.names.clear();
            } catch (...) {
            }
            float past = 0.0f;
            walkCamText(cell, hasCell, g_walkArmCam, sizeof(g_walkArmCam), &past);
            g_walkArmZone = zone;
            g_walkCellCopy = cell;
            g_walkPassArmed = true;   // published with this tick's snapshot (publishSnapshot, later in the tick)
            if (++g_walkNamesSession == kWalkNamesCap) Log::get().note("walk survey names: %u reports written; no more this session", kWalkNamesCap);
        }
        g_walkNamesDueMs = now + 5000;
    }
}

// TEMPORARY (the VR proving build, docs\concourse-cull-2026-11-05.md "VR, next"): the two probes. VR only, log only. Every line starts "concourse vr probe:".
// What the log shows if the code never ran: no "bound" line at the poll (the key is empty), no "requested" line at a press (the press was not seen: the tick did not run, or fix.optimized_stations
// is off); a "requested" line with no later lines means the follow-ups never ran. A probe that ran says what it found in its last line.
std::atomic<uint32_t> g_vrProbeFrame{0};          // the tick's frame, for the tone draw's lines
std::atomic<uint32_t> g_vrProbeToneLines{0};
constexpr uint32_t kVrProbeToneCap = 40;
uint32_t g_vrHoldFrame = 0;                       // the frame of the hold press (0: none)
uint64_t g_vrHoldBase = 0;                        // nativeFrameHeldLatches() at the press
uint32_t g_vrHoldStep = 0;                        // follow-ups written
constexpr uint32_t kVrHoldFrames = 10;
constexpr uint32_t kVrCameraFrames = 5;
constexpr float kVrCameraRaiseM = 30.0f;
constexpr uint32_t kVrHoldFollow[] = {3, 12, 30};

void concourseVrProbeTick(uint32_t frame) {
    g_vrProbeFrame.store(frame, std::memory_order_relaxed);
    nativeCameraProbeTick(frame);
    if (detail::g_vrProbeToneArmed.load(std::memory_order_relaxed) && !nativeCameraProbeRunning()) {
        detail::g_vrProbeToneArmed.store(false, std::memory_order_release);
        Log::get().note("concourse vr probe: tone: %u line(s) written for the probe's frames (zero means no tonemap draw was admitted: the lines of the camera probe say whether the stub ran)",
                        g_vrProbeToneLines.load(std::memory_order_relaxed));
    }
    if (g_vrHoldFrame && g_vrHoldStep < sizeof(kVrHoldFollow) / sizeof(kVrHoldFollow[0]) && frame - g_vrHoldFrame >= kVrHoldFollow[g_vrHoldStep]) {
        const unsigned long long taken = static_cast<unsigned long long>(nativeFrameHeldLatches() - g_vrHoldBase);
        Log::get().note("concourse vr probe: hold: frame %u (%u after the request): the runtime's latch took %llu held frame(s) of the %u requested%s", frame, frame - g_vrHoldFrame, taken,
                        kVrHoldFrames, ++g_vrHoldStep == sizeof(kVrHoldFollow) / sizeof(kVrHoldFollow[0])
                            ? (taken ? "; last report (the runtime's withheld, replayed and empty counts are in the trace at the session's close)" : "; last report: NONE taken, so the request never reached the runtime's latch")
                            : "");
    }
    const bool holdPressed = g_vrHoldKey.hk && g_vrHoldKey.hk->pressed();
    const bool cameraPressed = g_vrCameraKey.hk && g_vrCameraKey.hk->pressed();
    if (holdPressed) {
        if (runtimeFlatProfile()) {
            Log::get().note("concourse vr probe: hold: not in VR (this is the flat profile); nothing done");
        } else {
            g_vrHoldFrame = frame;
            g_vrHoldStep = 0;
            g_vrHoldBase = nativeFrameHeldLatches();
            requestSubmitHold(kVrHoldFrames);
            Log::get().note("concourse vr probe: hold: requested %u frames at frame %u (the runtime's latch has taken %llu held frame(s) so far; follow-ups at +%u, +%u and +%u frames say how many it took now)",
                            kVrHoldFrames, frame, static_cast<unsigned long long>(g_vrHoldBase), kVrHoldFollow[0], kVrHoldFollow[1], kVrHoldFollow[2]);
        }
    }
    if (cameraPressed) {
        if (runtimeFlatProfile()) {
            Log::get().note("concourse vr probe: camera: not in VR (this is the flat profile); nothing done");
        } else if (nativeCameraProbeRunning()) {
            Log::get().note("concourse vr probe: camera: refused at frame %u: the last probe is still running", frame);
        } else {
            float centre[3] = {}, axes[3][3] = {};
            const bool anchored = concourseAnchorFrame(centre, axes);
            const float up[3] = {anchored ? axes[1][0] : 0.0f, anchored ? axes[1][1] : 1.0f, anchored ? axes[1][2] : 0.0f};
            const float offset[3] = {up[0] * kVrCameraRaiseM, up[1] * kVrCameraRaiseM, up[2] * kVrCameraRaiseM};
            const char* why = nullptr;
            if (nativeCameraProbeArm(offset, kVrCameraFrames, frame, &why)) {
                g_vrProbeToneLines.store(0, std::memory_order_relaxed);
                detail::g_vrProbeToneArmed.store(true, std::memory_order_release);
                Log::get().note("concourse vr probe: camera: requested at frame %u: the head pose raised %.0f m along %s up (%.3f %.3f %.3f) for the %u frames after it, then one more is watched; "
                                "the press's own frame is the one before",
                                frame, static_cast<double>(kVrCameraRaiseM), anchored ? "the concourse anchor's" : "world +Y (no anchor read)", static_cast<double>(up[0]),
                                static_cast<double>(up[1]), static_cast<double>(up[2]), kVrCameraFrames);
            } else {
                Log::get().note("concourse vr probe: camera: refused at frame %u: %s", frame, why ? why : "no reason given");
            }
        }
    }
}

// The camera each eye was drawn with, for the tone line: the temporal pass's per-eye rows (the eye's first recognised scene draw this frame; view->world [R | c], three float4
// rows: the position is the fourth column, the view's z axis the third), else the engine-velocity watch's one source camera (no eye). Text into `out`.
void vrProbeCameraText(int eye, char* out, size_t n) {
    float r[12] = {};
    const void* res = nullptr;
    uint64_t vs = 0;
    if (eye >= 0 && temporalPassEyeRows(eye, r, &res, &vs)) {
        std::snprintf(out, n, "temporal eye rows: pos (%.3f %.3f %.3f) view z axis (%.4f %.4f %.4f) view x axis (%.4f %.4f %.4f)", static_cast<double>(r[3]), static_cast<double>(r[7]),
                      static_cast<double>(r[11]), static_cast<double>(r[2]), static_cast<double>(r[6]), static_cast<double>(r[10]), static_cast<double>(r[0]), static_cast<double>(r[4]),
                      static_cast<double>(r[8]));
        return;
    }
    float rows[6][4] = {};
    uint32_t rf = 0, now = 0;
    if (engineVelocityLastSourceCameraRows(rows, &rf, &now)) {
        std::snprintf(out, n, "no per-eye rows (the temporal pass is not wanted, or the eye has no scene draw yet); engine-velocity source camera (one, not per eye; frame %u, now %u): "
                              "row 275 (%.3f %.3f %.3f), row 270 (%.4f %.4f %.4f)",
                      rf, now, static_cast<double>(rows[5][0]), static_cast<double>(rows[5][1]), static_cast<double>(rows[5][2]), static_cast<double>(rows[0][0]),
                      static_cast<double>(rows[0][1]), static_cast<double>(rows[0][2]));
        return;
    }
    std::snprintf(out, n, "no camera reader answered (no temporal rows for this eye, no engine-velocity source camera)");
}

}  // namespace

// The VR eye draw path hands over each draw while the camera probe runs: the game's tonemap, once per eye per frame of the probe (the frame before, the frames that write, the one after).
void concourseVrProbeToneDraw(ID3D11DeviceContext* ctx, char kind, uint32_t count, uint32_t instances, uint32_t startInstance) {
    if (!ctx || (kind != 'D' && kind != 'N') || count != 3 || instances != 1 || startInstance != 0) return;
    const uint32_t frame = g_vrProbeFrame.load(std::memory_order_relaxed);
    const uint32_t phase = nativeCameraProbePhase(frame);
    if (!phase || g_vrProbeToneLines.load(std::memory_order_relaxed) >= kVrProbeToneCap) return;
    ToneAdmit ta;
    if (!tonemapAdmitStructure(ctx, bindingShaderHash(BindSlot::Ps), ta) || !tonemapAdmitFull(ta, bindingShaderHash(BindSlot::Vs))) return;
    int eye = -1;
    ResourceInfo ri;
    const void* rt = nullptr;
    if (ta.rtvRes && bindingResolveResourceProbe(ta.rtvRes, &ri) && ri.isTexture2D) {
        rt = ri.resource;
        eye = uiDepthEyeOfTargetReadOnly(ri.resource);
    }
    // Flight 50: the eye table had not seen the tone's target (eye -1 on all 28 lines), so the depth and the rows were never asked for. Two tone draws a frame were admitted;
    // the eye is then taken from their order in the frame (the first eye 0), and the line says which way it was told, with the target, so the order can be checked.
    static uint32_t orderFrame = ~0u, orderNext = 0;   // the render thread only
    if (orderFrame != frame) {
        orderFrame = frame;
        orderNext = 0;
    }
    const int byOrder = static_cast<int>(orderNext++);
    const bool eyeByOrder = eye < 0 && byOrder < 2;
    if (eyeByOrder) eye = byOrder;
    static uint32_t doneFrame[2] = {~0u, ~0u};   // one line per eye per frame (the render thread only)
    if (eye >= 0 && eye < 2) {
        if (doneFrame[eye] == frame) return;
        doneFrame[eye] = frame;
    }
    ID3D11Texture2D* depth = nullptr;
    D3D11_TEXTURE2D_DESC dd{};
    const bool depthOk = eye >= 0 && depthProbeSceneDepth(ta.hdrW, ta.hdrH, eye, &depth) && depth;
    if (depthOk) depth->GetDesc(&dd);
    char depthText[96];
    if (depthOk) std::snprintf(depthText, sizeof(depthText), "available (format %u, %ux%u)", static_cast<unsigned>(dd.Format), dd.Width, dd.Height);
    else std::snprintf(depthText, sizeof(depthText), "NOT available (%s)", eye < 0 ? "the eye is unknown" : "the depth probe has no scene pair of H's size, or is not watching");
    char cam[300];
    vrProbeCameraText(eye, cam, sizeof(cam));
    g_vrProbeToneLines.fetch_add(1, std::memory_order_relaxed);
    Log::get().note("concourse vr probe: tone: frame %u [%s%s] eye %d (%s, target %p): H (PS t1) format %u %ux%u; depth %s; %s",
                    frame, phase == 1 ? "before" : phase == 2 ? "probe frame" : "after", phase == 2 ? (nativeCameraProbeWrote(frame) ? ", the stub wrote" : ", the stub did NOT write") : "", eye,
                    eyeByOrder ? "by the draw's order in the frame" : eye >= 0 ? "by the eye table" : "not told", rt, static_cast<unsigned>(ta.hdrFmt), ta.hdrW, ta.hdrH, depthText, cam);
}

namespace {

// TEMPORARY (the bake): where direction d looks, along the room's axes: 0 +a2, 1 -a2, 2 +a0, 3 -a0, 4 up.
void bakeForward(const float axes[3][3], uint32_t d, float out[3]) noexcept {
    const float* a = axes[d < 2 ? 2 : d < 4 ? 0 : 1];
    const float sign = d == 1 || d == 3 ? -1.0f : 1.0f;
    for (int k = 0; k < 3; ++k) out[k] = a[k] * sign;
}

// ---- The VR bake's tick (TEMPORARY; the state, the phases and the end are above concourseStationOk) ---------------------------------------------------------------------------
const char* vrStatusName(uint32_t s) noexcept {
    switch (s) {
        case 1: return "taken";
        case 2: return "NOT TAKEN: its eye is not within 0.5 m of the origin";
        case 3: return "NOT TAKEN: no wanted face near where it looks";
        case 4: return "NOT TAKEN: its rows do not invert";
        default: return "no status (the shader wrote none)";
    }
}

// The world check, once, in the measure phase: the newest frame with both eyes' rows read back (or one eye's, a tick later), its eyes' row 275 against the store's camera of the
// tick after it (the store resolves one frame behind its callbacks), and the game's own base of that frame, from the stub.
void vrBakeTryWorldCheck(uint32_t frame) {
    if (g_vr.phase != VrBakePhase::kMeasure || g_vr.worldChecked) return;
    int pick = -1;
    bool both = false;
    uint32_t pickFrame = 0;
    for (uint32_t i = 0; i < kVrRing; ++i) {
        const bool v0 = g_vr.rowsValid[i][0], v1 = g_vr.rowsValid[i][1];
        if (!v0 && !v1) continue;
        const uint32_t f = v0 ? g_vr.rows[i][0].frame : g_vr.rows[i][1].frame;
        const bool b = v0 && v1 && g_vr.rows[i][0].frame == g_vr.rows[i][1].frame;
        if (!b && frame - f < 2) continue;   // the other eye's may still be on its way
        if (pick < 0 || (b && !both) || (b == both && f - pickFrame < 0x80000000u)) {
            pick = static_cast<int>(i);
            both = b;
            pickFrame = f;
        }
    }
    if (pick < 0) return;
    float mid[3] = {}, eyePos[2][3] = {};
    bool have[2] = {};
    int n = 0;
    for (int e = 0; e < 2; ++e) {
        if (!g_vr.rowsValid[pick][e] || g_vr.rows[pick][e].frame != pickFrame) continue;
        have[e] = true;
        ++n;
        for (int k = 0; k < 3; ++k) {
            eyePos[e][k] = g_vr.rows[pick][e].rows[5][k];
            mid[k] += eyePos[e][k];
        }
    }
    for (int k = 0; k < 3; ++k) mid[k] /= static_cast<float>(n > 0 ? n : 1);
    int camAt = -1;
    for (uint32_t want : {pickFrame + 1, pickFrame}) {
        const uint32_t i = want % kVrRing;
        if (camAt < 0 && g_vr.camValid[i] && g_vr.camFrame[i] == want) camAt = static_cast<int>(i);
    }
    const int baseAt = g_vr.baseValid[pickFrame % kVrRing] && g_vr.baseFrame[pickFrame % kVrRing] == pickFrame ? static_cast<int>(pickFrame % kVrRing) : -1;
    float off[3] = {}, dist = 0.0f;
    bool apply = false;
    if (camAt >= 0) apply = cvb::worldOffset(g_vr.cam[camAt], mid, off, &dist);
    float baseT[3] = {}, det = 0.0f, headOff = 0.0f;
    if (baseAt >= 0) {
        cvb::Pose b;
        std::memcpy(b.v, g_vr.base[baseAt], sizeof(b.v));
        det = cvb::det3(b);
        if (std::isfinite(det) && std::fabs(det) > 0.5f) g_vr.handed = det < 0.0f ? -1.0f : 1.0f;
        for (int k = 0; k < 3; ++k) baseT[k] = b.v[12 + k];
        const float d[3] = {mid[0] - baseT[0], mid[1] - baseT[1], mid[2] - baseT[2]};
        headOff = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    }
    if (apply)
        for (int k = 0; k < 3; ++k) {
            g_vr.offset[k] = off[k];
            g_vr.centre[k] = g_vr.centreStore[k] + off[k];
        }
    char e0[64] = "not read", e1[64] = "not read", camText[96] = "NOT RECORDED for that frame", baseText[120] = "NOT READ for that frame";
    if (have[0]) std::snprintf(e0, sizeof(e0), "%.3f %.3f %.3f", static_cast<double>(eyePos[0][0]), static_cast<double>(eyePos[0][1]), static_cast<double>(eyePos[0][2]));
    if (have[1]) std::snprintf(e1, sizeof(e1), "%.3f %.3f %.3f", static_cast<double>(eyePos[1][0]), static_cast<double>(eyePos[1][1]), static_cast<double>(eyePos[1][2]));
    if (camAt >= 0)
        std::snprintf(camText, sizeof(camText), "%.3f %.3f %.3f at tick %u%s", static_cast<double>(g_vr.cam[camAt][0]), static_cast<double>(g_vr.cam[camAt][1]),
                      static_cast<double>(g_vr.cam[camAt][2]), g_vr.camFrame[camAt], g_vr.camFrame[camAt] == pickFrame + 1 ? "" : " (the same tick, not the one after)");
    if (baseAt >= 0)
        std::snprintf(baseText, sizeof(baseText), "t(%.3f %.3f %.3f), determinant %.3f, the eyes' midpoint %.3f m from it", static_cast<double>(baseT[0]), static_cast<double>(baseT[1]),
                      static_cast<double>(baseT[2]), static_cast<double>(det), static_cast<double>(headOff));
    Log::get().note("concourse bake: vr world check: frame %u: the store's camera %s; the eyes' row 275 %s %.3f %.3f %.3f [eye 0 %s; eye 1 %s]; the game's base %s; render less store "
                    "%.3f %.3f %.3f = %.2f m: %s; the aim's handedness %+.0f",
                    pickFrame, camText, n == 2 ? "midpoint" : "(one eye)", static_cast<double>(mid[0]), static_cast<double>(mid[1]), static_cast<double>(mid[2]), e0, e1, baseText,
                    static_cast<double>(off[0]), static_cast<double>(off[1]), static_cast<double>(off[2]), static_cast<double>(dist),
                    camAt < 0 ? "NOT MADE (no store camera for that frame): the aim is at the store's centre"
                    : apply   ? "past 0.5 m: the aim and the cube's origin move by it"
                              : "within 0.5 m: one world, the aim is at the store's centre",
                    static_cast<double>(g_vr.handed));
    g_vr.worldChecked = true;
}

// Each tick in VR, before the cull's own decisions: the tone draw looked for, the stub's window and the read-backs taken, the world check tried, the counts into the plan.
void vrBakeEveryTick(uint32_t frame, bool social) {
    const bool vr = !runtimeFlatProfile();
    concoursePaneVrBakeDiscover(vr && g_setting.load(std::memory_order_acquire) && social && g_vr.stubOk && cc::bakePlanFor(g_concKind).run);
    if (!vr) return;
    ConcourseVrToneSeen tone;
    if (!g_vr.toneSaid && concoursePaneVrToneSeen(&tone)) {
        g_vr.toneSaid = true;
        Log::get().note("concourse bake: vr: the game's tonemap draw is seen with an eye and its depth: H format %u %ux%u, the scene depth format %u", tone.hdrFmt, tone.hdrW, tone.hdrH,
                        tone.depthFmt);
    }
    if (!g_vr.availSaid && vrBakeAvail()) {
        g_vr.availSaid = true;
        Log::get().note("concourse bake: vr: the bake is available (the eye stub is in, and the tonemap draw was seen): a station of type %s %s", journal_station::kindName(g_concKind),
                        cc::bakePlanFor(g_concKind).run ? "runs the cull and its bake" : "is not on the list, so it stays the game's own");
    }
    if (g_vr.holdFollow && frame - g_vr.holdFollowFrame < 0x80000000u) {
        g_vr.holdFollow = false;
        const unsigned long long taken = static_cast<unsigned long long>(nativeFrameHeldLatches() - g_vr.holdBase);
        Log::get().note("concourse bake: vr hold: the runtime's latch took %llu held frame(s) over the bake (%u frames asked for at each of %u aims; the runtime's withheld, replayed and "
                        "empty counts are in its trace at the session's close)%s",
                        taken, kVrHoldLag, g_vr.holdAims, taken ? "" : "; NONE taken: the request never reached the runtime's latch");
    }
    if (g_vr.phase == VrBakePhase::kIdle) return;
    {
        const uint32_t i = frame % kVrRing;
        g_vr.camFrame[i] = frame;
        g_vr.camValid[i] = g_frameCamValid;
        for (int k = 0; k < 3; ++k) g_vr.cam[i][k] = g_frameCam[k];
    }
    // The stub's window that ended: the calls made since the last tick, the camera of the frame drawn since then (labelled with the last tick's frame, as its draws are).
    NativeCameraBakeWindow w;
    nativeCameraBakeTick(&w);
    const uint32_t wf = frame - 1;
    if (w.firstSeen && w.firstReadable) {
        const uint32_t i = wf % kVrRing;
        g_vr.baseFrame[i] = wf;
        std::memcpy(g_vr.base[i], w.firstBefore, sizeof(g_vr.base[i]));
        g_vr.baseValid[i] = true;
        g_vr.baseStack[i] = w.firstStack;
    }
    // The window an aim covered (the aim made at tick wf, closed by this tick): did the stub write it? A window with no stub call at all counts as unwritten too.
    if (g_vr.phase == VrBakePhase::kAim) {
        const uint32_t a = wf % kVrRing;
        if (g_vr.aimValid[a] && g_vr.aimFrame[a] == wf) g_vr.unwrittenRun = w.wroteMask != 0 ? 0 : g_vr.unwrittenRun + 1;
    }
    if ((g_vr.phase == VrBakePhase::kAim || w.wroteMask != 0 || wf == g_vr.startFrame) && vrLogTake()) {
        char before[200] = "unreadable";
        if (w.firstSeen && w.firstReadable) {
            vrPoseText(w.firstBefore, before, sizeof(before));
            // The fourth lanes, which the game does not set and the fence no longer reads (Flight 54): shown so a flight confirms what they hold.
            const size_t at = std::strlen(before);
            std::snprintf(before + at, sizeof(before) - at, " pads %g %g %g %g", static_cast<double>(w.firstBefore[3]), static_cast<double>(w.firstBefore[7]),
                          static_cast<double>(w.firstBefore[11]), static_cast<double>(w.firstBefore[15]));
        }
        Log::get().note("concourse bake: vr stub: frame %u: %u calls (%u at a stack address, stack mask 0x%X), written 0x%X%s, %u torn reads, %u foreign (a call whose own pose was not the "
                        "base Elite is using, so not written); the first call before: %s%s", wf, w.calls, w.stackCalls, w.stackMask, w.wroteMask,
                        w.calls == 0 ? " (THE STUB DID NOT RUN)" : "", w.torn, w.foreign, before, w.firstSeen && !w.firstStack ? " (not on a stack)" : "");
    }
    ConcourseVrBakeRows rr[32];
    const uint32_t nr = concoursePaneVrBakeTakeRows(rr, 32);
    for (uint32_t k = 0; k < nr; ++k) {
        const ConcourseVrBakeRows& r = rr[k];
        if (r.eye < 0 || r.eye > 1 || r.frame - g_vr.startFrame >= 0x80000000u) continue;
        const uint32_t i = r.frame % kVrRing;
        if (g_vr.rowsValid[i][1 - r.eye] && g_vr.rows[i][1 - r.eye].frame != r.frame) g_vr.rowsValid[i][1 - r.eye] = false;
        g_vr.rows[i][r.eye] = r;
        g_vr.rowsValid[i][r.eye] = true;
        if (!vrLogTake()) continue;
        // The aim of this frame or the one before (which lag the game has is not known): the nearer prediction.
        float best = -1.0f, pred[3] = {};
        uint32_t bestFrame = 0, bestFace = 0;
        for (uint32_t lag = 0; lag < 2; ++lag) {
            const uint32_t f = r.frame - lag, a = f % kVrRing;
            if (!g_vr.aimValid[a] || g_vr.aimFrame[a] != f) continue;
            const float* p = g_vr.aimPred[a][r.eye];
            const float dx = r.rows[5][0] - p[0], dy = r.rows[5][1] - p[1], dz = r.rows[5][2] - p[2];
            const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (best < 0.0f || d < best) {
                best = d;
                bestFrame = f;
                bestFace = g_vr.aimFace[a];
                for (int c = 0; c < 3; ++c) pred[c] = p[c];
            }
        }
        char tail[160] = "no aim for this frame (the world check's, or before the aims)";
        if (best >= 0.0f)
            std::snprintf(tail, sizeof(tail), "against the aim of frame %u (face %u): predicted %.3f %.3f %.3f, %.3f m off", bestFrame, bestFace, static_cast<double>(pred[0]),
                          static_cast<double>(pred[1]), static_cast<double>(pred[2]), static_cast<double>(best));
        Log::get().note("concourse bake: vr rows: frame %u eye %d: row 275 %.3f %.3f %.3f, row 279 %.4f %.4f %.4f, clip w row %.4f %.4f %.4f, near (row 273 z) %.3f; %s", r.frame, r.eye,
                        static_cast<double>(r.rows[5][0]), static_cast<double>(r.rows[5][1]), static_cast<double>(r.rows[5][2]), static_cast<double>(r.rows[9][0]),
                        static_cast<double>(r.rows[9][1]), static_cast<double>(r.rows[9][2]), static_cast<double>(r.rows[0][3]), static_cast<double>(r.rows[1][3]),
                        static_cast<double>(r.rows[2][3]), static_cast<double>(r.rows[3][2]), tail);
    }
    vrBakeTryWorldCheck(frame);
    ConcourseVrBakeCount cs[32];
    const uint32_t nc = concoursePaneVrBakeTakeCounts(cs, 32);
    for (uint32_t k = 0; k < nc; ++k) {
        const ConcourseVrBakeCount& c = cs[k];
        for (int e = 0; e < 2; ++e) {
            if (!c.dispatched[e]) continue;
            const uint32_t st = c.status[e] < 5 ? c.status[e] : 0;
            ++g_vr.status[st];
            float frac = 0.0f;
            // The frame's aim entry says whether it was a write frame (every aimed dwell frame is); a frame with none only counts.
            const uint32_t ai = c.frame % kVrRing;
            const bool haveAim = g_vr.aimValid[ai] && g_vr.aimFrame[ai] == c.frame;
            const bool writeFrame = haveAim && g_vr.aimWrite[ai];
            if (st == 1 && c.face[e] >= 0 && c.face[e] < static_cast<int>(cvb::kFaces) && c.onFace[e] > 0) {
                frac = static_cast<float>(c.both[e] ? c.eitherView[e] : c.inView[e]) / static_cast<float>(c.onFace[e]);
                if (writeFrame) g_vr.plan.noteCoverage(static_cast<uint32_t>(c.face[e]), frac);
                g_vr.written[e] += c.written[e];
                ++g_vr.taken;
            }
            if (vrLogTake()) {
                char dwellText[40];
                if (haveAim && g_vr.aimDwell[ai] < cvb::kVrBakeDwell)
                    std::snprintf(dwellText, sizeof(dwellText), "dwell %u of %u", g_vr.aimDwell[ai], cvb::kVrBakeDwell);
                else
                    std::snprintf(dwellText, sizeof(dwellText), "%s", haveAim ? "past the dwell, the last face stays" : "no aim for this frame");
                Log::get().note("concourse bake: vr capture: frame %u eye %d: %s; face %d; %s; %s; %u texels on it, %u in this eye's view, %u past the cut, %u written, %u nothing drawn; "
                                "cut by distance <5 %u, 5-20 %u, 20-50 %u, >=50 %u; drawn by distance <50 %u, 50-200 %u, 200-1000 %u, 1000-4000 %u, >=4000 %u; %u in either eye's view (%.1f%% covered%s)",
                                c.frame, e, vrStatusName(st), c.face[e], dwellText, writeFrame ? "writes" : "counts only", c.onFace[e], c.inView[e], c.pastCut[e], c.written[e],
                                c.nothingDrawn[e], c.cutBy[e][0], c.cutBy[e][1], c.cutBy[e][2], c.cutBy[e][3], c.drawnBy[e][0], c.drawnBy[e][1], c.drawnBy[e][2], c.drawnBy[e][3],
                                c.drawnBy[e][4], c.eitherView[e], static_cast<double>(frac) * 100.0,
                                c.both[e] ? "" : "; the other eye's rows were not this frame's, so this eye alone");
            }
        }
    }
}

// The window opens: the measure phase begins. False (and the window is closed by the caller) when the cube or the VR shader is not ready.
bool vrBakeBegin(uint32_t frame, uint64_t now) {
    if (!concoursePaneVrBakeBegin()) return false;
    recFlush(false);   // TEMPORARY (H62 instrument, Flight 56): nothing of an earlier bake carries over
    g_vr.tick = 0;
    g_vr.startFrame = frame;
    g_vr.aimTicks = g_vr.headMissing = g_vr.logLines = 0;
    g_vr.beginMs = now;
    g_vr.worldChecked = false;
    g_vr.handed = 1.0f;
    g_vr.taken = 0;
    g_vr.written[0] = g_vr.written[1] = 0;
    for (uint64_t& s : g_vr.status) s = 0;
    for (uint32_t i = 0; i < kVrRing; ++i) {
        g_vr.camValid[i] = g_vr.baseValid[i] = g_vr.aimValid[i] = false;
        g_vr.rowsValid[i][0] = g_vr.rowsValid[i][1] = false;
    }
    for (int k = 0; k < 3; ++k) {
        g_vr.centreStore[k] = g_vr.centre[k] = g_camLookCentre[k];
        g_vr.offset[k] = 0.0f;
    }
    g_vr.plan.reset(0);
    g_vr.nearRaised = false;
    nativeCameraBakeObserve(true);
    g_vr.phase = VrBakePhase::kMeasure;
    Log::get().note("concourse bake: vr: begin at frame %u: the room's centre %.2f %.2f %.2f (the store's world), up %.3f %.3f %.3f; the world check first: each eye's rows are copied and "
                    "read back, nothing aimed or held",
                    frame, static_cast<double>(g_camLookCentre[0]), static_cast<double>(g_camLookCentre[1]), static_cast<double>(g_camLookCentre[2]),
                    static_cast<double>(g_camLookAxes[1][0]), static_cast<double>(g_camLookAxes[1][1]), static_cast<double>(g_camLookAxes[1][2]));
    return true;
}

// The aims begin: the faces, the pitch that centres a face in the two eyes' frusta, the capture armed with the origin (the centre, moved by the world check's offset), the near
// value resolved once a session.
void vrBakeAimStart(uint32_t frame) {
    // Only called once the world check was made from rows that came back: with none, the bake ends before this (vrBakeTickBlock), and nothing is aimed or held.
    g_vr.faceMask = 0;
    {
        const ConcourseVrBakeStats st = concoursePaneVrBakeStats();
        g_vr.rowlessRun = 0;
        g_vr.unwrittenRun = 0;
        g_vr.stoppedUnwritten = false;
        g_vr.lastToneDraws = st.toneDraws;
        g_vr.lastNoRows = st.noRows;
    }
    float faces9[cvb::kFaces][9] = {};
    for (uint32_t d = 0; d < cvb::kFaces; ++d) {
        if (!cvb::faceOf(g_camLookAxes, d, &g_vr.faces[d])) continue;
        g_vr.faceMask |= 1u << d;
        for (int k = 0; k < 3; ++k) {
            faces9[d][k] = g_vr.faces[d].r[k];
            faces9[d][3 + k] = g_vr.faces[d].u[k];
            faces9[d][6 + k] = g_vr.faces[d].f[k];
        }
    }
    float frusta[2][4] = {}, shift[2] = {}, head[12] = {}, eyeToHead[2][12] = {};
    bool haveF[2] = {};
    uint64_t seq = 0;
    for (uint32_t e = 0; e < 2; ++e) haveF[e] = nativeTemporalEyeGeometry(e, &seq, frusta[e], shift);
    const bool haveE = nativeTemporalHeadPose(&seq, head, eyeToHead);
    g_vr.pitch = cvb::pitchForFrusta(frusta, haveF);
    const bool haveBoth[2] = {haveF[0] && haveE, haveF[1] && haveE};
    const float spare = cvb::faceSpare(frusta, eyeToHead, haveBoth, g_vr.pitch), spareLevel = cvb::faceSpare(frusta, eyeToHead, haveBoth, 0.0f);
    g_vr.plan.reset(g_camBakeMask & g_vr.faceMask);
    concoursePaneVrBakeCapture(g_vr.centre, faces9, g_vr.plan.want(), g_camBakeNear, g_camBakeNear > cc::kBakeRoomCutM ? g_camBakeNear : cc::kBakeRoomCutM);
    if (!g_vr.nearTried) {
        g_vr.nearTried = true;
        const char* why = nullptr;
        g_vr.nearOk = concourseNearValueResolve(&why);
        Log::get().note("concourse bake: vr near: the value at 0x5E85B8C %s%s", g_vr.nearOk ? "is resolved: the bake's frames take the bake's near" : "is NOT available, so the frames keep the game's near: ",
                        g_vr.nearOk ? "" : (why ? why : "no reason given"));
    }
    g_vr.heldBase = nativeFrameHeldLatches();
    g_vr.phase = VrBakePhase::kAim;
    const auto deg = [](float tangent) { return std::atan(static_cast<double>(tangent)) * 57.29578; };
    Log::get().note("concourse bake: vr aim begins at frame %u: origin %.2f %.2f %.2f (the store's centre %s), faces 0x%X of 0x%X wanted, pitch %+.1f degrees (eye 0 %s l %.1f r %.1f d %.1f "
                    "u %.1f, eye 1 %s l %.1f r %.1f d %.1f u %.1f degrees; the union's least margin %.3f at that pitch, %.3f level, positive is covered), near %.0f m; the runtime "
                    "withholds %u frames past each aim; the latch has taken %llu held frame(s) so far",
                    frame, static_cast<double>(g_vr.centre[0]), static_cast<double>(g_vr.centre[1]), static_cast<double>(g_vr.centre[2]),
                    g_vr.offset[0] != 0.0f || g_vr.offset[1] != 0.0f || g_vr.offset[2] != 0.0f ? "plus the world check's offset" : "as it is", g_vr.plan.want(), g_camBakeMask,
                    static_cast<double>(g_vr.pitch) * 57.29578, haveF[0] ? "" : "(NOT READ)", deg(frusta[0][0]), deg(frusta[0][1]), deg(frusta[0][2]), deg(frusta[0][3]),
                    haveF[1] ? "" : "(NOT READ)", deg(frusta[1][0]), deg(frusta[1][1]), deg(frusta[1][2]), deg(frusta[1][3]), static_cast<double>(spare), static_cast<double>(spareLevel),
                    static_cast<double>(g_camBakeNear), kVrHoldLag, static_cast<unsigned long long>(g_vr.heldBase));
}

// The base Elite is using, for the stub's fence: the newest window's first call before (read before any write of ours, so Elite's own), when that call was at a stack address.
const float* vrLatestBase() noexcept {
    int best = -1;
    uint32_t bestFrame = 0;
    for (uint32_t i = 0; i < kVrRing; ++i) {
        if (!g_vr.baseValid[i] || !g_vr.baseStack[i]) continue;
        if (best < 0 || g_vr.baseFrame[i] - bestFrame < 0x80000000u) {
            best = static_cast<int>(i);
            bestFrame = g_vr.baseFrame[i];
        }
    }
    return best >= 0 ? g_vr.base[best] : nullptr;
}

// One aim: the next face, the base from the latest head, the hold, the near value. What the eyes should show is kept for the rows' lines. False, with nothing aimed or held this
// tick, when the tone draws have gone kVrRowlessAimsMax aimed ticks in a row without the frame's rows: the caller ends the bake.
bool vrBakeAimTick(uint32_t frame) {
    recClose();   // TEMPORARY (H62 instrument, Flight 56): the previous aim's window ends with this tick, whatever this tick does
    {
        const ConcourseVrBakeStats st = concoursePaneVrBakeStats();
        const uint64_t dTone = st.toneDraws - g_vr.lastToneDraws, dNo = st.noRows - g_vr.lastNoRows;
        g_vr.lastToneDraws = st.toneDraws;
        g_vr.lastNoRows = st.noRows;
        if (dTone > 0) g_vr.rowlessRun = dNo >= dTone ? g_vr.rowlessRun + 1 : 0;   // a tick with no tone draw at all is a stalled game, not missing rows
        if (g_vr.rowlessRun >= kVrRowlessAimsMax) {
            nativeCameraBakeStopWriting();
            return false;
        }
        if (g_vr.unwrittenRun >= kVrUnwrittenAimsMax) {
            nativeCameraBakeStopWriting();
            g_vr.stoppedUnwritten = true;
            return false;
        }
    }
    bool fresh = false;
    uint32_t dwell = 0;   // this tick's place in the face's hold; every held frame writes (the cube keeps the union of a face's frames)
    const uint32_t d = g_vr.plan.next(g_vr.tick, &fresh, &dwell);
    const bool writes = dwell < cvb::kVrBakeDwell;
    uint64_t seq = 0;
    float head[12] = {}, eyeToHead[2][12] = {};
    if ((g_vr.faceMask & (1u << d)) == 0 || !nativeTemporalHeadPose(&seq, head, eyeToHead)) {
        ++g_vr.headMissing;
        nativeCameraBakeStopWriting();
        return true;
    }
    const cvb::Pose hg = cvb::gameFromOpenVr(head);
    const cvb::Face pitched = cvb::pitchFace(g_vr.faces[d], g_vr.pitch);
    const cvb::Pose rows = cvb::faceRows(pitched, g_vr.centre, g_vr.handed);
    const cvb::Pose b = cvb::aimBase(hg, rows);
    // The lease: this aim is valid for the one window that follows (the next nativeCameraBakeTick, in vrBakeEveryTick, ends it, and this tick comes after that one). With no base read
    // yet the stub is given none to match, so it writes nothing and counts the calls as foreign.
    nativeCameraBakeAim(b.v, vrLatestBase());
    requestSubmitHold(kVrHoldLag);
    if (g_vr.nearOk) {
        float before = 0.0f;
        const bool raised = concourseNearValueRaise(g_camBakeNear, &before);
        if (!g_vr.nearRaised)
            Log::get().note("concourse bake: vr near: read %.3f m, wrote %.1f m%s", static_cast<double>(before), static_cast<double>(g_camBakeNear), raised ? "" : " (THE WRITE FAILED)");
        g_vr.nearRaised = g_vr.nearRaised || raised;
    }
    const uint32_t a = frame % kVrRing;
    g_vr.aimFrame[a] = frame;
    g_vr.aimFace[a] = d;
    g_vr.aimDwell[a] = dwell;
    g_vr.aimWrite[a] = writes;
    concoursePaneVrBakeAimFrame(frame, static_cast<int>(d), dwell, writes);
    if (writes) recOpen(frame, d, dwell, g_vr.centre);   // TEMPORARY (H62 instrument, Flight 56): the judges' records of this aim's window
    for (int e = 0; e < 2; ++e) cvb::eyeAt(eyeToHead[e], rows, g_vr.aimPred[a][e]);
    g_vr.aimValid[a] = true;
    ++g_vr.aimTicks;
    if (vrLogTake()) {
        char hText[200], bText[200], holdText[72];
        if (dwell < cvb::kVrBakeDwell)
            std::snprintf(holdText, sizeof(holdText), "%s, dwell %u of %u, %s", fresh ? "a new aim" : "held", dwell, cvb::kVrBakeDwell, "writes");
        else
            std::snprintf(holdText, sizeof(holdText), "the last face stays, counts only");
        vrPoseText(hg.v, hText, sizeof(hText));
        vrPoseText(b.v, bText, sizeof(bText));
        Log::get().note("concourse bake: vr aim: frame %u, tick %u: face %u (%s, aim %u), the head as the game composes it %s; B %s; predicted eye 0 %.3f %.3f %.3f, eye 1 %.3f %.3f %.3f", frame,
                        g_vr.tick, d, holdText, g_vr.plan.aims(d), hText, bText, static_cast<double>(g_vr.aimPred[a][0][0]),
                        static_cast<double>(g_vr.aimPred[a][0][1]), static_cast<double>(g_vr.aimPred[a][0][2]), static_cast<double>(g_vr.aimPred[a][1][0]),
                        static_cast<double>(g_vr.aimPred[a][1][1]), static_cast<double>(g_vr.aimPred[a][1][2]));
    }
    return true;
}

// The VR part of the tick: what flat's camera-test block does for flat's camera, for the eye stub's. Returns lookOn (the store judges as the look does, from the centre) while it
// aims. camEnd may be set here (kDone, kTime, kModeOff) and every end is acted on.
bool vrBakeTickBlock(uint64_t now, uint32_t frame, bool anchor, bool camWasActive, bool camWasArmed, cc::CamTestEnd& camEnd, const char* armedWhy, const cc::LiftReading& liftRead) {
    if (g_camTimer.active() && camWasActive && g_vr.phase == VrBakePhase::kAim) {
        if (g_vr.plan.finished(g_vr.tick)) {
            g_camTimer.stop();
            camEnd = cc::CamTestEnd::kDone;
        } else if (g_vr.tick >= kVrBakeTicksMax) {
            g_camTimer.stop();
            camEnd = cc::CamTestEnd::kTime;
        }
    }
    bool acting = g_camTimer.active();
    const char* endWhy = nullptr;   // an end this block decided on (not the timer's): its finish-line reason
    bool endPublish = false;
    if (g_camTimer.armed() && !camWasArmed) {
        const std::string dirsT = Config::get().getString("diag.concourse_bake_dirs", "");
        g_camBakeMask = dirsT.empty() ? cc::bakePlanFor(g_concKind).dirs : cc::bakeParseDirs(dirsT.c_str());
        const std::string nearT = Config::get().getString("diag.concourse_bake_near", "");
        const float nearM = nearT.empty() ? cc::kBakeNearDefaultM : static_cast<float>(std::atof(nearT.c_str()));
        g_camBakeNear = nearM >= cc::kBakeNearMinM && nearM <= cc::kBakeNearMaxM ? nearM : cc::kBakeNearDefaultM;
        Log::get().note("concourse bake: vr: armed: %s (cabin %.2f m, camera %.2f %.2f %.2f); waiting for the anchor; directions 0x%X, near %.0f m", armedWhy,
                        static_cast<double>(liftRead.cabinDistM), static_cast<double>(g_frameCam[0]), static_cast<double>(g_frameCam[1]), static_cast<double>(g_frameCam[2]), g_camBakeMask,
                        static_cast<double>(g_camBakeNear));
    }
    concoursePaneVrBakeArm(g_camTimer.armed() || acting);
    if (!acting) {
        g_camLookValid = false;
    } else if (!camWasActive) {
        g_camLookValid = anchor && concourseAnchorFrame(g_camLookCentre, g_camLookAxes);
        g_camBakeTick = 0;
        if (!g_camLookValid || !vrBakeBegin(frame, now)) {
            Log::get().note("concourse bake: vr: the window closes at once: %s", g_camLookValid ? "the cube or the VR shader is not ready" : "THE ANCHOR COULD NOT BE READ");
            g_camTimer.stop();
            acting = false;
            g_camLookValid = false;
            camEnd = cc::CamTestEnd::kModeOff;
        }
    }
    if (acting && g_camLookValid && g_vr.phase != VrBakePhase::kIdle) {
        ++g_vr.tick;
        if (g_vr.phase == VrBakePhase::kMeasure) {
            if (g_vr.worldChecked) {
                vrBakeAimStart(frame);
            } else if (g_vr.tick > kVrMeasureMaxTicks) {
                // No eye's rows came back: nothing was aimed or held, and the bake ends unpublished like any refused one (the retry logic counts it).
                Log::get().note("concourse bake: vr: ended before aiming: no eye's rows came back in %u ticks, so nothing was aimed or held (the finish line's \"rows copied\" says whether any "
                                "were copied)",
                                g_vr.tick - 1);
                g_camTimer.stop();
                acting = false;
                camEnd = cc::CamTestEnd::kModeOff;
                endWhy = "no eye's rows came back, so nothing was aimed";
            }
        }
        if (acting && g_vr.phase == VrBakePhase::kAim && !vrBakeAimTick(frame)) {
            // The rows stopped, or the stub wrote nothing: the aims and the hold stop with them. Published only when every wanted face was covered before they did.
            if (g_vr.stoppedUnwritten)
                Log::get().note("concourse bake: vr: aiming stopped at frame %u: the stub wrote no call for %u aimed windows in a row (the stub lines' \"foreign\" and \"written\" say "
                                "why), so the eyes never reached the centre; faces covered 0x%X of 0x%X",
                                frame, g_vr.unwrittenRun, g_vr.plan.covered(), g_vr.plan.want());
            else
                Log::get().note("concourse bake: vr: aiming stopped at frame %u: the tone draws had no frame's rows for %u aimed ticks in a row; faces covered 0x%X of 0x%X", frame,
                                g_vr.rowlessRun, g_vr.plan.covered(), g_vr.plan.want());
            g_camTimer.stop();
            acting = false;
            camEnd = (g_vr.plan.covered() & g_vr.plan.want()) == g_vr.plan.want() ? cc::CamTestEnd::kDone : cc::CamTestEnd::kModeOff;
            endWhy = g_vr.stoppedUnwritten ? "the stub wrote nothing" : "the rows stopped arriving";
            endPublish = camEnd == cc::CamTestEnd::kDone;
        }
    }
    const bool lookOn = acting && g_camLookValid && g_vr.phase == VrBakePhase::kAim;
    if (!lookOn) vrBakeStopAim();   // the stub let go and the near put back, on every path that does not go on aiming
    if (endWhy)
        vrBakeEnd(endPublish, endWhy, now, frame);
    else if (camEnd == cc::CamTestEnd::kDone || camEnd == cc::CamTestEnd::kTime)
        vrBakeEnd(true, camEnd == cc::CamTestEnd::kDone ? "every wanted face covered, or aimed at its last time" : "its limit, with faces still short", now, frame);
    else if (camEnd != cc::CamTestEnd::kNone)
        vrBakeEnd(false, cc::camTestEndName(camEnd), now, frame);
    else if (!acting && g_vr.phase != VrBakePhase::kIdle)
        vrBakeEnd(false, "the window closed", now, frame);
    return lookOn;
}

// The tick, under the state lock, before the store gate is set (storeHookGate reads g_concWanted).
void concourseTick(uint64_t now, uint32_t frame) {
    g_concSettingOffLogged = false;
    concoursePoll(now);
    concourseVrProbeTick(frame);   // TEMPORARY (the VR proving build): the two probe keys and the camera probe's windows
    // A load on foot aboard a carrier names no carrier in the journal; Status.json says it (journal_station::atCarrier, under the state lock as the sun's reading is).
    journal_station::Kind stationKind = journalStationKind();
    const bool carrierByStatus = stationKind == journal_station::Kind::kUnknown && journal_station::atCarrier(stationKind, g_statusFlagsNow, g_statusFlags2Now);
    if (carrierByStatus) stationKind = journal_station::Kind::kFleetCarrier;
    if (!g_concKindSeen || stationKind != g_concKind) {
        g_concKindSeen = true;
        g_concKind = stationKind;
        if (g_concAuto && g_concKeyOn && stationKind != journal_station::Kind::kUnknown) {
            const bool vrNoBake = !runtimeFlatProfile() && cc::bakePlanFor(stationKind).run && !cc::runsWithoutBake(stationKind) && !vrBakeAvail();
            Log::get().note("concourse cull: a station of type %s%s: %s", journal_station::kindName(stationKind),
                            carrierByStatus ? " (Status.json: docked and on foot, neither in a station nor on a planet)" : "",
                            vrNoBake ? (g_vr.stubOk ? "VR's bake waits for the game's tonemap draw to be seen, and until then it stays the game's own"
                                                    : "VR has no bake (the eye stub is not in), so it stays the game's own")
                            : !cc::bakePlanFor(stationKind).run ? "its layout has not been flown, so it stays the game's own"
                            : runtimeFlatProfile() ? "the cull and its bake run"
                            : cc::runsWithoutBake(stationKind) ? "the cull runs (a carrier needs no bake)" : "the cull and its VR bake run");
        }
    }
    // A carrier's jump with the commander aboard: the bake is of the system left. It goes, and the next waits for the arrival to pass. A bake running then is cancelled, and the
    // release is asked for each tick until the old one is gone: the pane drops a release that comes while a bake is armed or its hold is ending (concourse_pane.cpp, bakeTick).
    {
        static bool jumpsSet = false, jumpRelease = false;
        static uint32_t jumpsSeen = 0;
        const uint32_t jumps = journalCarrierJumps();
        if (!jumpsSet) {
            jumpsSet = true;
            jumpsSeen = jumps;
        } else if (jumps != jumpsSeen) {
            jumpsSeen = jumps;
            g_bakeAfterJumpMs = now + cc::kBakeAfterJumpMs;
            g_bakeNoRideTries = 0;
            const bool running = g_camTimer.armed() || g_camTimer.active();
            if (g_camTimer.stop()) concoursePaneBakeCancel();
            concoursePaneBakeArm(false);
            jumpRelease = true;
            Log::get().note("concourse bake: the carrier jumped (CarrierJump): %s; a new one is taken after %llu ms", concoursePaneBakeDone() ? "the bake is of the system left, so it goes" : running ? "the bake running is cancelled" : "there was no bake",
                            static_cast<unsigned long long>(cc::kBakeAfterJumpMs));
        }
        if (jumpRelease) {
            if (concoursePaneBakeDone()) concoursePaneBakeRelease();
            else jumpRelease = false;
        }
    }
    bool toggledOn = false;
    if (g_concHotkey && g_concHotkey->pressed()) {
        const bool was = concourseEffective();
        g_concOverride = was ? ConcOverride::kOff : ConcOverride::kOn;
        toggledOn = !was;
        Log::get().note("concourse cull: hotkey: the cull is %s by override (diag.concourse_cull is %s)", was ? "off" : "on", g_concKeyOn ? "on" : "off");
    }
    const JournalRawStatus raw = journalRawStatus();
    const bool known = journalOnFootKnown() || (runtimeFlatProfile() && raw.gameplay);
    const bool social = cc::concourseSocialOnFoot(g_statusFlags2Now, known, raw.gameplay);
    vrBakeEveryTick(frame, social);   // TEMPORARY (the VR bake): the tone draw looked for, the stub's window and the read-backs; nothing in flat but one store
    const bool sealedNow = g_heldValid;
    const bool armed = concourseArmed();
    const bool effective = concourseEffective();
    // Built 2026-11-06: a camera cut over kRoomCutM in one frame is another room (the lift). The anchor is forgotten before it is read, so the cull's gate follows the
    // jump at once both ways (going down it would otherwise stay on until the anchor ages out). resolveTickView runs earlier in the same tick, so this is the same tick.
    bool cutNow = false;
    if (g_roomCutPending) {
        cutNow = true;
        g_roomCutPending = false;
        const uint64_t was = g_concAnchorMs.exchange(0, std::memory_order_relaxed);
        const bool wasFresh = was != 0 && now - was <= kConcAnchorFreshMs;
        if (armed || wasFresh)
            Log::get().note("concourse cull: the camera cut %.0f m in one frame (another room): the anchor is forgotten; anchor was %lld ms old, in hangar %d, social %d",
                            static_cast<double>(g_roomCutM), was != 0 ? static_cast<long long>(now - was) : -1ll, (g_statusFlags2Now & sealed::kFlags2OnFootInHangar) != 0 ? 1 : 0,
                            (g_statusFlags2Now & sealed::kFlags2OnFootSocialSpace) != 0 ? 1 : 0);
    }
    const uint64_t anchorMs = g_concAnchorMs.load(std::memory_order_relaxed);
    bool anchor = anchorMs != 0 && now - anchorMs <= kConcAnchorFreshMs;
    {
        // Which room this is: the anchor's name says, whatever the journal has said. An anchor gone stale is read afresh when it is next seen, since a record's address can be used again.
        if (!anchor) g_concLayoutAddr = 0;
        const uintptr_t anchorAddr = g_concAnchorAddr.load(std::memory_order_relaxed);
        char anchorName[64] = {};
        if (anchorAddr != 0 && anchorAddr != g_concLayoutAddr && cachedName(anchorAddr, anchorName, sizeof(anchorName)) && cc::concourseAnchorName(anchorName)) {
            g_concLayoutAddr = anchorAddr;
            const cc::RoomLayout* layout = &cc::roomLayoutOf(anchorName);
            if (layout != g_concLayout) {
                g_concLayout = layout;
                g_concLiveAddr.store(0, std::memory_order_relaxed);
                Log::get().note("concourse cull: the room is %s (its anchor is %s): the bay window has %u indices, and a bake shows on a sphere of %.0f m unless diag.concourse_backdrop_radius says",
                                layout->name, anchorName, layout->bayN, static_cast<double>(layout->backdropRadiusM));
            }
        }
    }
    if (anchor && g_concLayout->roomReachM > 0.0f) {
        // A port's hub is seen from the hangar too: the camera says whether this is the room. Said once each time it leaves, so a waiting anchor=0 here is not read as the record being unseen.
        static bool saidAway = false;
        const uintptr_t hubAddr = g_concAnchorAddr.load(std::memory_order_relaxed);
        const WorldRead hub = hubAddr != 0 ? worldOfRecord(hubAddr) : WorldRead{};
        const bool hubRead = hub.source != 4;
        const float away = hubRead && g_frameCamValid ? sealed::distance(hub.world, g_frameCam) : 0.0f;
        anchor = cc::anchorMeansRoomHere(*g_concLayout, g_frameCamValid, hubRead, away);
        if (anchor) saidAway = false;
        else if (!saidAway) {
            saidAway = true;
            Log::get().note("concourse cull: its anchor is in the store but this is not the room: the camera is %.0f m from it, past %.0f m (camera read %d, anchor read %d)", static_cast<double>(away),
                            static_cast<double>(g_concLayout->roomReachM), g_frameCamValid ? 1 : 0, hubRead ? 1 : 0);
        }
    }
    bool inLiveRoom = false;
    if (!anchor || g_concLayout->liveRoomName == nullptr) g_concLiveLatch.reset();
    if (anchor && g_concLayout->liveRoomName != nullptr) {
        // The room's live room (a carrier's bridge): there the cull stands down, as if out of the room. The bake is kept, for the way back. Said once each time it is entered.
        static bool saidLive = false;
        uintptr_t liveAddr = g_concLiveAddr.load(std::memory_order_relaxed);
        char liveName[64] = {};
        if (liveAddr != 0 && (!cachedName(liveAddr, liveName, sizeof(liveName)) || std::strcmp(liveName, g_concLayout->liveRoomName) != 0)) {
            uintptr_t expect = liveAddr;
            g_concLiveAddr.compare_exchange_strong(expect, 0, std::memory_order_relaxed);
            liveAddr = 0;
        }
        const WorldRead live = liveAddr != 0 ? worldOfRecord(liveAddr) : WorldRead{};
        const bool liveRead = live.source != 4;
        const float liveAway = liveRead && g_frameCamValid ? sealed::distance(live.world, g_frameCam) : 0.0f;
        if (g_concLiveLatch.step(*g_concLayout, g_frameCamValid, liveRead, liveAway, live.radius, now)) {
            anchor = false;
            inLiveRoom = true;
            if (!saidLive) {
                saidLive = true;
                Log::get().note("concourse cull: the camera is in %s, %.0f m from its centre (its radius %.0f m): the cull stands down there and its windows are the game's own",
                                g_concLayout->liveRoomName, static_cast<double>(liveAway), static_cast<double>(live.radius));
            }
        } else {
            saidLive = false;
        }
    }
    const bool gateWanted = armed && social && !sealedNow;
    // The door rule (concourse_cull_core.h, roomSeenSlab): at a room whose layout has doors, the camera's place along the anchor's a2, each door's state from its leaves, and the stretch of
    // a2 the camera can see into. The anchor's frame is read once a second while the rule is wanted (the pose read is not free) and held between reads; the camera's place and the leaves
    // are this tick's. A change of what is culled, or of the live room, starts the rigs' verdicts afresh. hotkey.concourse_room_test (TEMPORARY) steps the rule's level.
    if (g_roomHotkey && g_roomHotkey->pressed()) {
        g_doorMode = cc::doorRuleNext(g_doorMode);
        Log::get().note("concourse doors: the rule is %s by hotkey.concourse_room_test%s%s", cc::doorRuleName(g_doorMode),
                        g_concLayout->slabCuts > 0 ? "" : " (this room has no doors known, so it does nothing here)",
                        g_doorsSetting ? "" : " (fix.skip_rooms_behind_doors is off, so it stays off)");
        char text[64];
        std::snprintf(text, sizeof(text), "Doors: %s%s", cc::doorRuleName(g_doorMode), g_doorsSetting ? "" : " (setting off)");
        concoursePaneQueueToast(text);
        g_roomSumMs = now;
        g_concWin.roomProd.store(0, std::memory_order_relaxed);
        g_concWin.roomStore.store(0, std::memory_order_relaxed);
        bumpRigEpoch();   // the rigs' tallies mean something else at another level
    }
    RoomPub roomNow;
    float roomAlong = 0.0f;
    const uint32_t doorCuts = g_concLayout->slabCuts < 2 ? g_concLayout->slabCuts : 2;
    const cc::DoorRuleMode doorMode = g_doorsSetting ? g_doorMode : cc::DoorRuleMode::kOff;   // fix.skip_rooms_behind_doors off: the rule is off
    const bool doorsWanted = doorMode != cc::DoorRuleMode::kOff && gateWanted && effective && (anchor || inLiveRoom) && doorCuts > 0 && g_frameCamValid;
    bool leavesMissing = false;
    const int64_t doorCost0 = doorsWanted ? costTicks() : 0;
    {
        static bool blockedWas[2] = {};
        static float frameC[3] = {};
        static float frameA2[3] = {};
        static bool frameValid = false;
        static uint64_t frameMs = 0;
        if (!doorsWanted) {
            frameValid = false;
            frameMs = 0;
        } else if (!frameValid || now - frameMs >= 1000) {
            float c[3] = {};
            float ax[3][3] = {};
            frameMs = now;
            frameValid = concourseAnchorFrame(c, ax);
            for (int k = 0; frameValid && k < 3; ++k) {
                frameC[k] = c[k];
                frameA2[k] = ax[2][k];
            }
        }
        if (!doorsWanted || !frameValid) {
            for (cc::DoorWatch& dw : g_doorWatch) dw.reset();
            blockedWas[0] = blockedWas[1] = false;
        } else {
            roomAlong = (g_frameCam[0] - frameC[0]) * frameA2[0] + (g_frameCam[1] - frameC[1]) * frameA2[1] + (g_frameCam[2] - frameC[2]) * frameA2[2];
            // The doors' leaves: each sighted record read for its name and place. A record whose name is not a leaf's any more is let go.
            bool seen[2][2] = {};
            float pos[2][2][3] = {};
            for (std::atomic<uintptr_t>& slot : g_doorLeaf) {
                const uintptr_t addr = slot.load(std::memory_order_relaxed);
                if (addr == 0) continue;
                char nm[64] = {};
                const int side = !cachedName(addr, nm, sizeof(nm))              ? -1
                                 : std::strcmp(nm, cc::kDoorLeafLeft) == 0  ? 0
                                 : std::strcmp(nm, cc::kDoorLeafRight) == 0 ? 1
                                                                            : -1;
                if (side < 0) {
                    uintptr_t expect = addr;
                    slot.compare_exchange_strong(expect, 0, std::memory_order_relaxed);
                    continue;
                }
                const WorldRead wr = worldOfRecord(addr);
                if (wr.source == 4) continue;
                const float along = (wr.world[0] - frameC[0]) * frameA2[0] + (wr.world[1] - frameC[1]) * frameA2[1] + (wr.world[2] - frameC[2]) * frameA2[2];
                const int cut = cc::doorCutOf(*g_concLayout, along);
                if (cut < 0) continue;
                seen[cut][side] = true;
                for (int k = 0; k < 3; ++k) pos[cut][side][k] = wr.world[k];
            }
            bool blocks[2] = {};
            for (uint32_t i = 0; i < doorCuts; ++i) {
                const bool leavesKnown = seen[i][0] && seen[i][1];
                if (!leavesKnown) leavesMissing = true;
                const float gap = leavesKnown ? sealed::distance(pos[i][0], pos[i][1]) : 0.0f;
                const cc::DoorState was = g_doorWatch[i].state();
                const cc::DoorState st = g_doorWatch[i].step(leavesKnown, gap, now);
                const double cut = static_cast<double>(g_concLayout->slabCut[i]);
                const double camFrom = std::fabs(static_cast<double>(roomAlong) - cut);
                const double rest = static_cast<double>(g_doorWatch[i].rest());
                if (st != was) {
                    if (st == cc::DoorState::kOpen && was == cc::DoorState::kShut) ++g_doorOpenings;
                    if (g_doorLogLines < kDoorLogLinesMax) {
                        ++g_doorLogLines;
                        if (!leavesKnown)
                            Log::get().note("concourse doors: the door at a2 %.1f is not known, frame %u (its two leaves are not both seen)", cut, frame);
                        else if (st == cc::DoorState::kOpen && was == cc::DoorState::kShut)
                            // The first move: the frame the leaves stood kDoorMoveM past rest, how wide the crack was then, where the camera was, and whether the room behind
                            // was culled until this frame (it is back from this tick's publish).
                            Log::get().note("concourse doors: the door at a2 %.1f started opening, frame %u: leaves %.3f m apart, %.3f past their rest of %.3f; camera %.1f m from it; "
                                            "the room behind it was %s",
                                            cut, frame, static_cast<double>(gap), static_cast<double>(gap) - rest, rest, camFrom,
                                            blockedWas[i] ? "culled until this frame" : "not culled (the door did not block)");
                        else if (st == cc::DoorState::kShut && was == cc::DoorState::kOpen)
                            Log::get().note("concourse doors: the door at a2 %.1f is shut, frame %u: leaves %.3f m apart (rest %.3f), still for %llu ms; open for %u frames, %llu ms; "
                                            "camera %.1f m from it",
                                            cut, frame, static_cast<double>(gap), rest, static_cast<unsigned long long>(cc::kDoorStillMs), frame - g_doorChangeFrame[i],
                                            static_cast<unsigned long long>(now - g_doorChangeMs[i]), camFrom);
                        else
                            Log::get().note("concourse doors: the door at a2 %.1f is %s, frame %u (first seen: leaves %.3f m apart, rest %s%.3f); camera %.1f m from it", cut,
                                            cc::doorStateName(st), frame, static_cast<double>(gap), rest > 0.0 ? "" : "not known, ", rest, camFrom);
                    }
                    g_doorChangeFrame[i] = frame;
                    g_doorChangeMs[i] = now;
                }
                blocks[i] = cc::doorBlocks(st, roomAlong, g_concLayout->slabCut[i]);
                blockedWas[i] = blocks[i];
            }
            cc::RoomSlab seenSlab;
            if (cc::roomSeenSlab(*g_concLayout, roomAlong, blocks, &seenSlab) && !cc::roomSlabWhole(seenSlab)) {
                roomNow.room = true;
                roomNow.only = inLiveRoom;
                roomNow.rigs = doorMode == cc::DoorRuleMode::kRigs;
                roomNow.slab = seenSlab;
                for (int k = 0; k < 3; ++k) {
                    roomNow.origin[k] = frameC[k];
                    roomNow.axis[k] = frameA2[k];
                }
            }
        }
    }
    g_doorLeafWanted.store(doorsWanted && leavesMissing, std::memory_order_relaxed);
    if (doorsWanted) {
        const int64_t d = costTicks() - doorCost0;
        g_doorCostTicks += d;
        if (d > g_doorCostMax) g_doorCostMax = d;
        ++g_doorCostN;
    }
    if (roomNow.room != g_roomPub.room || roomNow.only != g_roomPub.only || roomNow.rigs != g_roomPub.rigs || (roomNow.room && !cc::sameRoomSlab(roomNow.slab, g_roomPub.slab))) {
        bumpRigEpoch();
        if (roomNow.room) {
            char lo[16] = "none", hi[16] = "none";
            if (roomNow.slab.lo > -cc::kRoomSlabOpen) std::snprintf(lo, sizeof(lo), "%.1f", static_cast<double>(roomNow.slab.lo));
            if (roomNow.slab.hi < cc::kRoomSlabOpen) std::snprintf(hi, sizeof(hi), "%.1f", static_cast<double>(roomNow.slab.hi));
            Log::get().note("concourse doors: frame %u, the camera is at a2 %.1f; behind a shut door below a2 %s and above a2 %s, the room's own records are culled (%s%s)", frame,
                            static_cast<double>(roomAlong), lo, hi, cc::doorRuleName(g_doorMode), roomNow.only ? "; the live room: that is the only rule, the cull's own stands down" : "");
        } else if (g_roomPub.room) {
            Log::get().note("concourse doors: frame %u, nothing is behind a shut door now (camera at a2 %.1f, rule %s, wanted %d)", frame, static_cast<double>(roomAlong),
                            cc::doorRuleName(g_doorMode), doorsWanted ? 1 : 0);
        }
    }
    g_roomPub = roomNow;
    if (doorsWanted && now - g_roomSumMs >= 5000) {
        g_roomSumMs = now;
        const double usPerTick = 1e6 / static_cast<double>(costFreq());
        Log::get().note("concourse doors 5s: rule %s, camera at a2 %.1f, doors %s %s (rest %.3f %.3f), openings %u; records the rule culled that the cull's own rule kept: producer %llu, "
                        "store %llu; the door watch's cost: %.1f us a tick on average, %.1f at most, over %u ticks",
                        cc::doorRuleName(g_doorMode), static_cast<double>(roomAlong), cc::doorStateName(g_doorWatch[0].state()),
                        doorCuts > 1 ? cc::doorStateName(g_doorWatch[1].state()) : "-", static_cast<double>(g_doorWatch[0].rest()), static_cast<double>(g_doorWatch[1].rest()),
                        g_doorOpenings, static_cast<unsigned long long>(g_concWin.roomProd.exchange(0, std::memory_order_relaxed)),
                        static_cast<unsigned long long>(g_concWin.roomStore.exchange(0, std::memory_order_relaxed)),
                        g_doorCostN ? static_cast<double>(g_doorCostTicks) * usPerTick / g_doorCostN : 0.0, static_cast<double>(g_doorCostMax) * usPerTick, g_doorCostN);
        g_doorOpenings = 0;
        g_doorCostTicks = g_doorCostMax = 0;
        g_doorCostN = 0;
    }
    // The cull waits for the windows' bake: until there is one, the room and its windows are the game's own, and a bake that never comes costs nothing but the saving. A ride up opens
    // the bake's window at the anchor's first sighting, so this holds for that one tick.
    // A room with the real sky in its windows (cc::RoomLayout::realSky, a carrier) takes no bake: the cull does not wait for one and neither a ride nor the room in view starts one.
    const bool realSky = g_concLayout->realSky;
    // TEMPORARY (the VR bake): in VR the bake's camera is the eye stub's, and its mode is the bake whenever it is available. Flat: vrBake is false, camMode is g_camMode, and
    // bakeMode is what it was.
    const bool vrBake = !runtimeFlatProfile() && vrBakeAvail();
    // Flat without the camera's producer hook (0x2874290), which is the only way the look and the bake move flat's camera: no bake is armed or begun, and nothing holds a Present
    // or moves the camera; the windows keep whatever cube was published before. Flight 55: after a switch from VR the install skipped that hook ("already patched by another
    // EDVR module") while the others went in, and two bakes held the screen 266 ms each with the camera never moved. The mode reads 0 on this path, so neither a ride up nor the
    // room in view arms the camera test.
    const bool camHooksBound = concourseCameraTestCanMove();
    {
        static bool saidNoHooks = false;
        if (runtimeFlatProfile() && !camHooksBound && (g_camHooked || g_camAutoTried)) {
            if (!saidNoHooks) {
                saidNoHooks = true;
                Log::get().note("concourse bake: flat: no bake: the camera's producer hook (0x2874290) is not installed in this session (the camera test's install lines say why), so "
                                "the windows keep the bake they have");
            }
        } else {
            saidNoHooks = false;
        }
    }
    const uint32_t camMode = vrBake ? cc::kCamTestModeBake : (camHooksBound ? g_camMode : 0u);
    const bool bakeMode = ((g_camHooked && camHooksBound) || vrBake) && camMode == cc::kCamTestModeBake && !realSky;
    {
        static const char* saidRealSky = nullptr;
        if (realSky && anchor && saidRealSky != g_concLayout->name) {
            saidRealSky = g_concLayout->name;
            Log::get().note("concourse cull: %s windows show the real sky: no bake, the glass is the game's own, the stars, the planet, its atmosphere and the sun are drawn with "
                            "the cull on, and the rest of the outside (hull, ships, weapons fire) is culled until a window clears",
                            g_concLayout->name);
        } else if (!realSky) {
            saidRealSky = nullptr;
        }
    }
    // VR's measure phase is inside the window but aims nothing: the cull still waits then (flat never has that phase, so this is flat's test).
    const bool bakeHolds = bakeMode && !concoursePaneBakeDone() && (!g_camTimer.active() || g_vr.phase == VrBakePhase::kMeasure);
    const bool wantCull = gateWanted && effective && anchor && !bakeHolds;   // the cull is wanted: the panes are treated whatever phase the clearing machine is in
    // A real-sky room clears as any other: from afar only the sky is kept (skySide), and a window that fills fix.station_window_distance's share brings the rest back.
    const cc::ClearOutput clr = concourseClearStep(now, wantCull, gateWanted, gateWanted ? concoursePaneShares() : ConcoursePaneShares{});
    // Built 2026-11-06: the lift. In a shut cabin the room is not drawn, in either room: the lift gate (concourse_cull_core.h) reads the cabin and its doors from the
    // survey's table. It needs the key on and the commander on foot, with the hangar not sealed; otherwise it stands down.
    const bool onFoot = (g_statusFlags2Now & sealed::kFlags2OnFoot) != 0;
    cc::LiftReading liftRead;
    bool liftOn = false;
    // TEMPORARY (the camera test): while its window is open the gate is held (not stepped: the moved camera would drop it, it reads the camera distance to the cabin) and
    // the lift cull rule is not applied. A ride up arms the window, the anchor's first sighting opens it, and it closes by time or the conditions ending.
    if (g_camHotkey && g_camHotkey->pressed()) camTestPress();
    cc::CamTestEnd camEnd = cc::CamTestEnd::kNone;
    const bool camWasActive = g_camTimer.active();
    const bool camWasArmed = g_camTimer.armed();
    // The bake belongs to this stay in the station: gone for good when the commander is neither on foot nor docked for a few seconds. The next arrival bakes afresh.
    if (onFoot || g_dockedNow.load(std::memory_order_acquire)) {
        g_bakeLeftMs = 0;
    } else if (g_bakeLeftMs == 0) {
        g_bakeLeftMs = now;
    } else if (now - g_bakeLeftMs >= 5000) {
        concoursePaneBakeRelease();
        g_bakeNoRideTries = 0;
    }
    const bool roomInView = social && anchor;
    if (!roomInView) g_bakeRoomSinceMs = 0;
    else if (g_bakeRoomSinceMs == 0) g_bakeRoomSinceMs = now;
    const bool bakeSharper = bakeMode && concoursePaneBakeWouldBeSharper(now);
    if (concoursePaneBakeDone() && !bakeSharper) g_bakeNoRideTries = 0;   // a good one, at the size the frames have now
    const char* camArmedWhy = "a ride up, the lift gate on with no room seen";
    if (armed && effective && onFoot && !sealedNow && g_frameCamValid) {
        liftRead = liftReadNow();
        if (camWasActive) {
            g_camTimer.step(camMode, false, anchor, now, &camEnd);
            liftOn = g_camTimer.active() ? false : g_liftGate.step(liftRead, cutNow, now);
        } else {
            liftOn = g_liftGate.step(liftRead, cutNow, now);
            const bool bakeWaits = now < g_bakeAfterJumpMs;   // a carrier's jump: neither a ride nor the room in view starts a bake yet
            if (bakeMode && roomInView && !camWasArmed && !liftOn && !bakeWaits) {
                const cc::BakeNoRide noRide = cc::bakeNoRide(concoursePaneBakeDone(), now - g_bakeRoomSinceMs, bakeSharper, g_bakeNoRideTries);
                if (noRide != cc::BakeNoRide::kNone) {
                    g_camTimer.armNow(now);
                    ++g_bakeNoRideTries;
                    camArmedWhy = noRide == cc::BakeNoRide::kFirst ? "no ride, the room in view with no bake" : "no ride, the render size has risen since the bake";
                }
            }
            const bool rideBakes = !(realSky && camMode == cc::kCamTestModeBake);   // a real-sky room: a ride up arms no bake
            if (g_camTimer.step(camMode, liftOn && !bakeWaits && rideBakes, anchor, now, &camEnd)) {
                liftOn = false;
                g_liftGate.stop();
            }
        }
    } else {
        g_liftGate.stop();
        if (g_camTimer.stop()) camEnd = cc::CamTestEnd::kModeOff;
    }
    bool lookOn = false;
    if (g_camHooked && camHooksBound && !vrBake) {   // flat's camera (vrBake is never true in flat); not at all while its hooks are not bound
        if (g_camMode != 0 && !runtimeFlatProfile()) {
            Log::get().note("concourse camera test: the profile is not flat any more: the mode is off");
            g_camMode = 0;
        }
        const bool baking = g_camMode == cc::kCamTestModeBake;
        // The bake ends when every direction has reached the resolve. Past its frame limit it is given up, and nothing is published: a window with a direction missing is worse than the game's own.
        if (g_camTimer.active() && baking && camWasActive) {
            if ((concoursePaneBakeCaptured() & g_camBakeMask) == g_camBakeMask) {
                g_camTimer.stop();
                camEnd = cc::CamTestEnd::kDone;
            } else if (g_camBakeTick >= cc::bakeTicks(g_camBakeMask)) {
                g_camTimer.stop();
                camEnd = cc::CamTestEnd::kTime;
            }
        }
        const bool acting = g_camTimer.active();
        if (g_camTimer.armed() && !camWasArmed && baking) {
            const std::string dirsT = Config::get().getString("diag.concourse_bake_dirs", "");
            g_camBakeMask = dirsT.empty() ? cc::bakePlanFor(g_concKind).dirs : cc::bakeParseDirs(dirsT.c_str());
            const std::string nearT = Config::get().getString("diag.concourse_bake_near", "");
            const float nearM = nearT.empty() ? cc::kBakeNearDefaultM : static_cast<float>(std::atof(nearT.c_str()));
            g_camBakeNear = nearM >= cc::kBakeNearMinM && nearM <= cc::kBakeNearMaxM ? nearM : cc::kBakeNearDefaultM;
        }
        concoursePaneBakeArm(g_camTimer.armed() && baking);
        if (!acting) g_camLookValid = false;
        else if (!camWasActive) {
            g_camLookValid = anchor && concourseAnchorFrame(g_camLookCentre, g_camLookAxes);
            g_camBakeTick = 0;
            if (baking && g_camLookValid) {
                // The frames at g_camBakeNear (5 m), and the room's cut at kBakeRoomCutM, never nearer than the frames' own near plane (H57: the two were one value, 50 m).
                concoursePaneBakeBegin(g_camLookCentre, g_camBakeNear, g_camBakeNear > cc::kBakeRoomCutM ? g_camBakeNear : cc::kBakeRoomCutM);
                float forwards[cc::kBakeDirs][3] = {};
                for (uint32_t d = 0; d < cc::kBakeDirs; ++d) bakeForward(g_camLookAxes, d, forwards[d]);
                concoursePaneBakeExpect(forwards, cc::kBakeDirs, g_camBakeMask);
                g_camBakeDir = cc::kBakeDirs - 1;   // so that the first asked for is the first in order
            }
        }
        lookOn = acting && g_camLookValid;
        if (lookOn && baking) {
            // The room's own axes, the next not yet captured. A view along the room's up takes the room's axes[2] as its own up.
            const uint32_t d = cc::bakeNextDir(g_camBakeMask, concoursePaneBakeCaptured(), g_camBakeDir);
            g_camBakeDir = d;
            ++g_camBakeTick;
            float forward[3] = {};
            bakeForward(g_camLookAxes, d, forward);
            concourseCameraTestAim(g_camLookCentre, forward, g_camLookAxes[d == 4 ? 2 : 1], cc::kBakeFovRad, g_camBakeNear);
        } else if (lookOn) {
            concourseCameraTestLook(g_camLookCentre, g_camLookAxes[1], cc::camTestLookDir(now - g_camTimer.startMs()));
        }
        concourseCameraTestControl(g_camMode, lookOn, g_camTimer.armed() || g_camTimer.active());
        if (camEnd == cc::CamTestEnd::kDone) {
            concoursePaneBakeFinish();
        } else if (camEnd != cc::CamTestEnd::kNone && baking) concoursePaneBakeCancel();
        if (g_camTimer.armed() && !camWasArmed)
            Log::get().note("concourse camera test: armed: %s (cabin %.2f m, gaps %.2f %.2f, camera %.2f %.2f %.2f); waiting for the anchor; bake directions 0x%X, near %.0f m",
                            camArmedWhy, static_cast<double>(liftRead.cabinDistM), static_cast<double>(liftRead.gap1), static_cast<double>(liftRead.gap2), static_cast<double>(g_frameCam[0]),
                            static_cast<double>(g_frameCam[1]), static_cast<double>(g_frameCam[2]), g_camBakeMask, static_cast<double>(g_camBakeNear));
        if (camWasArmed && !g_camTimer.armed() && !acting && g_camTimer.startMs() < g_camTimer.armedMs())
            Log::get().note("concourse camera test: armed for %llu ms and the anchor was not seen: it lets go", static_cast<unsigned long long>(now - g_camTimer.armedMs()));
        if (acting && !camWasActive) {
            g_camLookTicks = 0;
            g_camLookTickMs = now;
            // The doors' reading here is this tick's, with the room's rigs not yet vetoed: it says whether they are still shut at the sighting. The cut's own line gives the time to it.
            Log::get().note("concourse camera test: mode %u: the window opens %llu ms after arming, at the anchor's first sighting (cabin %.2f m, gaps %.2f %.2f): %llu ms, the gate is held and the lift cull rule is off; %s, centre %.2f %.2f %.2f, up %.3f %.3f %.3f",
                            g_camMode, static_cast<unsigned long long>(now - g_camTimer.armedMs()), static_cast<double>(liftRead.cabinDistM), static_cast<double>(liftRead.gap1),
                            static_cast<double>(liftRead.gap2), static_cast<unsigned long long>(cc::kCamTestMs), g_camLookValid ? "the look is on" : "THE ANCHOR COULD NOT BE READ, nothing is written",
                            static_cast<double>(g_camLookCentre[0]), static_cast<double>(g_camLookCentre[1]), static_cast<double>(g_camLookCentre[2]),
                            static_cast<double>(g_camLookAxes[1][0]), static_cast<double>(g_camLookAxes[1][1]), static_cast<double>(g_camLookAxes[1][2]));
            camTestLine("start", g_camMode);
            g_camLogMs = now;
        } else if (acting && now - g_camLogMs >= cc::kCamTestLogMs) {
            camTestLine("acting", g_camMode);
            g_camLogMs = now;
        }
        if (camEnd != cc::CamTestEnd::kNone) {
            Log::get().note("concourse camera test: mode %u: the window ends after %llu ms: %s", g_camMode, static_cast<unsigned long long>(now - g_camTimer.startMs()), cc::camTestEndName(camEnd));
            camTestLine("end", g_camMode);
            g_camLogMs = now;
        } else if (!acting && g_camMode == cc::kCamTestModeLog && armed && onFoot && now - g_camLogMs >= 1000) {
            camTestLine("log only", g_camMode);
            g_camLogMs = now;
        }
    }
    // TEMPORARY (the VR bake): the eye stub's bake, the same window and the same cull, its own camera and capture. Never in flat (vrBake is false there).
    if (vrBake) {
        lookOn = vrBakeTickBlock(now, frame, anchor, camWasActive, camWasArmed, camEnd, camArmedWhy, liftRead);
    } else if (g_vr.phase != VrBakePhase::kIdle) {
        vrBakeEnd(false, "VR's bake is not available any more", now, frame);   // the profile changed under it
    }
    // The look's first ticks, ms apart: what a bake of that many frames would have cost here. A stall in them is the game's arrival, or the jump.
    if (lookOn && g_camLookTicks < kCamLookTickCount) {
        g_camLookTickDt[g_camLookTicks++] = static_cast<uint16_t>(now - g_camLookTickMs > 60000 ? 60000 : now - g_camLookTickMs);
        g_camLookTickMs = now;
        if (g_camLookTicks == kCamLookTickCount) {
            char dts[kCamLookTickCount * 7] = {};
            size_t dat = 0;
            for (int k = 0; k < kCamLookTickCount; ++k) dat += static_cast<size_t>(std::snprintf(dts + dat, sizeof(dts) - dat, "%u ", static_cast<unsigned>(g_camLookTickDt[k])));
            Log::get().note("concourse camera test: the look's first %d ticks, ms apart: %s", kCamLookTickCount, dts);
        }
    }
    // The rigs are judged afresh both ways: a verdict from the other rule would hold them back up to kRigRefreshN calls, as at the lift's end.
    if (lookOn != g_camLookWasOn) {
        bumpRigEpoch();
        if (!lookOn) g_camLookGraceTicks = 12;
        else {
            // The names, once the camera is at the centre: the first frame there comes about 50 ms after the first write (Flight 25).
            g_concSurveyDueMs = now + 60;
            g_camLookSurvey = true;
        }
        g_camLookWasOn = lookOn;
    }
    g_camLookOn.store(lookOn, std::memory_order_release);
    if (liftOn && !g_liftWasOn) {
        g_liftStartMs = now;
        g_liftTicks = 0;
        for (int k = 0; k < 6; ++k) g_liftTot0[k] = g_concWin.tot[k].load(std::memory_order_relaxed);
    }
    if (liftOn) ++g_liftTicks;
    if (liftOn && !g_liftWasOn)
        Log::get().note("concourse cull: lift: doors shut with the player inside, the room is off (cabin %.2f m, gaps %.2f %.2f)", static_cast<double>(liftRead.cabinDistM),
                        static_cast<double>(liftRead.gap1), static_cast<double>(liftRead.gap2));
    if (!liftOn && g_liftWasOn)
        Log::get().note("concourse cull: lift: the room is back (%s), after %llu ms", cc::liftEndName(g_liftGate.lastEnd()), static_cast<unsigned long long>(now - g_liftGate.onSinceMs()));
    // The counts across the lift, from the never-reset totals the callbacks' counters flush into (the 5 s line's window is left alone). Counters not yet flushed at this
    // tick belong to the next line's difference. Elapsed time over the ticks counted from the first to the last in the period.
    if (!liftOn && g_liftWasOn) {
        uint64_t d[6] = {};
        for (int k = 0; k < 6; ++k) d[k] = g_concWin.tot[k].load(std::memory_order_relaxed) - g_liftTot0[k];
        const uint64_t ms = now - g_liftStartMs;
        Log::get().note("concourse cull: lift: over %llu ms: rigs seen %llu vetoed %llu, producer vetoed %llu kept %llu (close %llu), store cleared %llu, frame mean %.2f ms over %llu frames",
                        static_cast<unsigned long long>(ms), static_cast<unsigned long long>(d[0]), static_cast<unsigned long long>(d[1]), static_cast<unsigned long long>(d[2]),
                        static_cast<unsigned long long>(d[3]), static_cast<unsigned long long>(d[4]), static_cast<unsigned long long>(d[5]),
                        g_liftTicks > 0 ? static_cast<double>(ms) / static_cast<double>(g_liftTicks) : 0.0, static_cast<unsigned long long>(g_liftTicks));
    }
    const bool liftEnded = g_liftWasOn && !liftOn;
    if (liftEnded) g_liftEndMs = now;
    // The room's rigs were judged by the lift's rule: a verdict that outlives it would hold them back up to kRigRefreshN calls as the doors open. A new epoch starts them
    // empty, and a rig with no bound forwards. It matters when the cull stays on (the doors reopen in the concourse with no cut); an `active` that falls bumps it anyway.
    if (liftEnded) bumpRigEpoch();
    g_liftWasOn = liftOn;
    g_liftGateOn.store(liftOn, std::memory_order_release);
    // PROTOTYPE (the backdrop capture): a press while the cull's own part is on lifts it for a moment, as the bay's lifting step does, with the clearing machine untouched.
    // The panes keep their treatment (an exterior pane is opaque over its whole area, as in the lifted phase) and the outside effects stay withheld (feed.effects).
    const bool cullPartRaw = wantCull && clr.cullOn;
    if (!wantCull) {
        g_backdropGate.reset();
        g_backdropSummaryMs = 0;
    } else if (!g_concWasWant) {   // a new visit: the counts of the 30 s line start over
        g_backdropAutoCount = g_backdropManualCount = 0;
        g_backdropLiftedMs = 0;
    }
    if (g_viewHotkey && g_viewHotkey->pressed()) {
        concourseBackdropPress(cullPartRaw && !liftOn && !lookOn, now);
    } else if (g_backdropPhase == BackdropPhase::kIdle) {
        // Automatic capture: the same capture F6 starts, when what the panes in view show of nothing is meaningful (concourse_cull_core.h, BackdropAutoGate).
        const ConcourseBackdropMissing miss = concoursePaneBackdropMissing();
        cc::BackdropAutoInput in;
        in.nowMs = now;
        in.share = miss.share;
        in.autoOn = concoursePaneBackdropAuto();
        in.modeBackdrop = concoursePaneIsBackdropMode();
        in.cullPartOn = cullPartRaw;
        in.plainMirror = clr.phase == cc::ClearPhase::kMirror;
        in.liftOn = liftOn || lookOn;
        in.captureRunning = false;
        const cc::BackdropAutoDecision d = g_backdropGate.step(in);
        if (d.escalated)
            Log::get().note("concourse backdrop: auto capture: the missing share did not fall after a capture (%.2f%% at its start, %.2f%% now); the gap between captures is now %llu ms, up to %llu ms",
                            static_cast<double>(d.shareAtStart) * 100.0, static_cast<double>(miss.share) * 100.0, static_cast<unsigned long long>(d.gapMs),
                            static_cast<unsigned long long>(cc::kBackdropMaxGapMs));
        if (d.fire) {
            Log::get().note("concourse backdrop: auto capture: missing %.2f%% of the view", static_cast<double>(miss.share) * 100.0);
            concourseBackdropStart(now, false);
        }
    }
    // The 30 s line, while in the mode in the concourse: it says whether the cost falls to nothing once the room has been seen. It never waits on a capture.
    if (wantCull && concoursePaneIsBackdropMode()) {
        if (g_backdropSummaryMs == 0) {
            g_backdropSummaryMs = now;
        } else if (now - g_backdropSummaryMs >= 30000) {
            g_backdropSummaryMs = now;
            const ConcourseBackdropMissing miss = concoursePaneBackdropMissing();
            Log::get().note("concourse backdrop 30s: %u auto captures, %u F6, lifted %llu ms in all, missing now %.2f%% (probes %llu issued, %llu read in these 30 s; auto %s, gap %llu ms)",
                            g_backdropAutoCount, g_backdropManualCount, static_cast<unsigned long long>(g_backdropLiftedMs), static_cast<double>(miss.share) * 100.0,
                            static_cast<unsigned long long>(miss.probesIssued - g_backdropProbeIssued0), static_cast<unsigned long long>(miss.probesRead - g_backdropProbeRead0),
                            concoursePaneBackdropAuto() ? "on" : "off", static_cast<unsigned long long>(g_backdropGate.gapMs()));
            g_backdropProbeIssued0 = miss.probesIssued;
            g_backdropProbeRead0 = miss.probesRead;
        }
    } else {
        g_backdropSummaryMs = 0;
    }
    const bool backdropLift = concourseBackdropStep(now, wantCull);
    const bool cullPart = cullPartRaw && !backdropLift;
    const bool cullActive = cullPart || liftOn || lookOn;
    const bool roomOnlyOn = roomNow.room && roomNow.only && !liftOn && !lookOn;   // the live room: the door rule alone
    const bool active = cullActive || roomOnlyOn;
    ConcoursePaneFeed feed;   // the look wants the game's own picture: no pane treatment, nothing withheld; nor does the live room's slab
    feed.measure = gateWanted && !lookOn && !bakeHolds;
    // A real-sky room's glass is the game's own, never treated; it is still measured, so a window clears. In VR the glass is treated once a bake exists (the VR bake's): the steady
    // treatment without one shows as a grey haze, so until then VR's glass is the game's own (the shares are still measured).
    feed.treat = wantCull && !lookOn && !realSky && (runtimeFlatProfile() || concoursePaneBakeDone());
    feed.realSky = realSky;
    feed.withhold = cullActive && !lookOn;
    feed.effects = (cullActive || backdropLift) && !lookOn;
    feed.bayOpacity = clr.bayOpacity;
    feed.sealed = g_sealedFlag.load(std::memory_order_acquire);   // the hangar is sealed: what can only be outside is not issued, with no diag key
    feed.bayN = g_concLayout->bayN;
    feed.backdropRadiusM = g_concLayout->backdropRadiusM;
    {
        // The anchor's up axis, read at most once a second while the cull is wanted (the pose read is not free), and held between reads.
        static float up[3] = {};
        static bool upValid = false;
        static uint64_t upMs = 0;
        if (!wantCull) {
            upValid = false;
            upMs = 0;
        } else if (now - upMs >= 1000) {
            upMs = now;
            upValid = concourseAnchorUp(up);
        }
        feed.upValid = upValid;
        for (int k = 0; k < 3; ++k) feed.up[k] = upValid ? up[k] : 0.0f;
    }
    concoursePaneSetFeed(feed);
    g_concWanted.store(gateWanted || liftOn || lookOn, std::memory_order_release);   // the lift opens the store route in the hangar too (it is not social space there)
    g_concHubWanted.store(gateWanted && g_concLayout == &cc::kRoomStation && g_concHubAddr.load(std::memory_order_relaxed) == 0, std::memory_order_relaxed);   // a port's hub is its anchor
    g_concLiveWanted.store(gateWanted && g_concLayout->liveRoomName != nullptr && g_concLiveAddr.load(std::memory_order_relaxed) == 0 ? g_concLayout->liveRoomName : nullptr,
                           std::memory_order_relaxed);
    const bool transitioned = active != g_concWasActive;
    if (transitioned) {
        if (active) {
            bumpRigEpoch();   // the rigs are judged afresh: every slot and publication of the old epoch forwards
            g_concActive.store(true, std::memory_order_release);
            // The names first, at a gate or key rising edge only (a return from the clearing, which now follows the bay in view and happens each time you turn round, does not rerun them). No rig has been vetoed yet, so the
            // survey's forwarding shows nothing that was not already on screen.
            if (!g_concWasWant && cullPart && now - g_liftEndMs >= 3000) g_concSurveyDueMs = now;   // not the cull resuming after a lift's release
            g_concSumMs = now;
            g_concSumFrames = 0;
            for (int k = 0; k < 3; ++k) g_concSumCalls[k] = g_concCallsTotal[k].load(std::memory_order_relaxed);
            Log::get().note(cullPart     ? "concourse cull: on"
                            : roomOnlyOn ? "concourse cull: on (the door rule alone, in the live room: only the room's own records behind a shut door are culled)"
                                         : "concourse cull: on (the lift: doors shut with the player inside)");
        } else {
            g_concActive.store(false, std::memory_order_release);
            bumpRigEpoch();
            g_concSurveyDueMs = 0;
            char liftWhy[96] = {};
            if (liftEnded && !g_concWasCull) std::snprintf(liftWhy, sizeof(liftWhy), "the lift: %s", cc::liftEndName(g_liftGate.lastEnd()));
            const char* why = liftWhy[0] != '\0' ? liftWhy
                              : backdropLift ? "the backdrop capture lifts it for a moment (hotkey.concourse_view_test)"
                              : !effective ? "the key or its override is off"
                              : sealedNow ? "the hangar seal engaged"
                              : cutNow    ? "the camera cut to another room"
                              : !social   ? "no longer on foot in social space"
                              : wantCull  ? "the bay glass was approached: the view is clearing (diag.concourse_clear_share)"
                              : inLiveRoom ? "the camera is in the room's live room, whose windows are the game's own"
                                          : "the anchor record was not seen for 5 s";
            Log::get().note("concourse cull: off (%s)", why);
        }
        g_concWasActive = active;
        g_concWaitSet = false;
    } else if (active && toggledOn) {
        g_concSurveyDueMs = now + 2000;   // a toggle-on while the cull stays active (the override flipped back on): the names again
    }
    g_concWasWant = wantCull;
    g_concWasCull = cullPart;
    if (!wantCull && !transitioned) {
        const uint32_t keyCode = g_concOverride == ConcOverride::kNone ? (g_concKeyOn ? 1u : 0u) : (g_concOverride == ConcOverride::kOn ? 2u : 3u);
        const uint32_t waitKey = (effective ? 1u : 0u) | (social ? 2u : 0u) | (anchor ? 4u : 0u) | (g_frameCamValid ? 8u : 0u) | (sealedNow ? 16u : 0u) | (keyCode << 5) | (bakeHolds ? 128u : 0u);
        if (armed && (!g_concWaitSet || waitKey != g_concWaitKey)) {
            g_concWaitSet = true;
            g_concWaitKey = waitKey;
            Log::get().note("concourse cull: waiting (key=%s social=%d anchor=%d camera=%d sealed=%d no-bake-yet=%d)", concourseKeyText(), social ? 1 : 0, anchor ? 1 : 0,
                            g_frameCamValid ? 1 : 0, sealedNow ? 1 : 0, bakeHolds ? 1 : 0);
        } else if (!armed && g_concWasArmed) {
            g_concWaitSet = false;
            Log::get().note("concourse cull: off (the key is off)");
        }
    }
    g_concWasArmed = armed;
    // The one-shot names: due at an engage (or 2 s after a toggle-on that did not re-engage), gathered over 2 frames.
    const uint32_t stamp = g_frameStamp.load(std::memory_order_relaxed);
    const bool surveyPress = concoursePaneTakeSurveyRequest();
    if (surveyPress && active && !g_concSurveyOn) {   // an F3 press: name what is left in view, and list what is culled
        g_concSurveyDueMs = now;
        g_concSurveyListPending = true;
    }
    // TEMPORARY: an F3 press with the cull off, on foot in social space: every record's name over 2 frames. The survey above needs the cull active, and that needs the anchor's name, so a layout
    // the cull has never run at (a surface port) would otherwise say nothing about what its records are called. No veto is running, so the store sees every record as it is.
    if (surveyPress && !active && social && !g_concNamesOnlyOn) {
        try {
            Survey& sv = survey();
            std::lock_guard<std::mutex> lock(sv.m);
            sv.names.clear();
            sv.close.clear();
            sv.unnamed.clear();
        } catch (...) {
        }
        g_concNamesOnlyOn = true;
        g_concNamesOnlyStamp = stamp;
        g_concNamesOnly.store(true, std::memory_order_release);
        Log::get().note("concourse names: F3 with the cull off: reading every record's name for 2 frames; nothing is culled");
    } else if (g_concNamesOnlyOn && static_cast<uint32_t>(stamp - g_concNamesOnlyStamp) >= 2) {
        g_concNamesOnly.store(false, std::memory_order_release);
        g_concNamesOnlyOn = false;
        concourseSurveyReport(true, true);
    }
    if (!active) {
        if (g_concSurveyOn) {
            g_concSurveyArmed.store(false, std::memory_order_release);
            g_concSurveyOn = false;
        }
    } else if (!g_concSurveyOn && g_concSurveyDueMs != 0 && now >= g_concSurveyDueMs) {
        try {
            Survey& sv = survey();
            std::lock_guard<std::mutex> lock(sv.m);
            sv.names.clear();
            sv.close.clear();
            sv.unnamed.clear();
        } catch (...) {
        }
        g_concSurveyDueMs = 0;
        g_concSurveyOn = true;
        g_concSurveyList = g_concSurveyListPending;
        g_concSurveyListPending = false;
        g_concSurveyStamp = stamp;
        g_concSurveyArmed.store(true, std::memory_order_release);
    } else if (g_concSurveyOn && static_cast<uint32_t>(stamp - g_concSurveyStamp) >= 2) {
        g_concSurveyArmed.store(false, std::memory_order_release);
        g_concSurveyOn = false;
        concourseSurveyReport(g_concSurveyList);
    }
    if (gateWanted) concourseFrameTrace(now);
    if (active) ++g_concSumFrames;
    // The 5 s summary: while active; the panes' treated draws ride on it (or are said alone while only the panes are in use).
    if (now - g_concSumMs >= 5000) {
        g_concSumMs = now;
        const uint64_t treated = concoursePaneTakeTreated();
        const uint64_t withheld = concoursePaneTakeWithheld();
        const ConcoursePaneMirrorStats mir = concoursePaneTakeMirrorStats();   // all zero while the mirror never ran
        if (active) {
            const uint64_t calls[3] = {g_concCallsTotal[0].load(std::memory_order_relaxed), g_concCallsTotal[1].load(std::memory_order_relaxed),
                                       g_concCallsTotal[2].load(std::memory_order_relaxed)};
            const double frames = g_concSumFrames > 0 ? static_cast<double>(g_concSumFrames) : 1.0;
            const double ms = (ringMean(g_costStore) * static_cast<double>(calls[0] - g_concSumCalls[0]) +
                               ringMean(g_costProducer) * static_cast<double>(calls[1] - g_concSumCalls[1]) +
                               ringMean(g_costRig) * static_cast<double>(calls[2] - g_concSumCalls[2])) / frames;
            for (int k = 0; k < 3; ++k) g_concSumCalls[k] = calls[k];
            g_concSumFrames = 0;
            Log::get().note("concourse cull 5s: rigs seen %u vetoed %u, producer vetoed %u kept %u (name %u, small-close %u, unreadable-name %u, unreadable-sphere %u), "
                            "store cleared %u, callback cost %.3f ms/frame, panes treated %u draws (mode %s, %u slots), outside draws withheld %u, phase %s, "
                            "mark passes %u (refused %u), panes known %u exterior %u (dropped %u), tests %u, share samples dropped %u",
                            static_cast<unsigned>(g_concWin.rigSeen.exchange(0)), static_cast<unsigned>(g_concWin.rigVetoed.exchange(0)),
                            static_cast<unsigned>(g_concWin.prodVetoed.exchange(0)), static_cast<unsigned>(g_concWin.prodKept.exchange(0)),
                            static_cast<unsigned>(g_concWin.kept[0].exchange(0)), static_cast<unsigned>(g_concWin.kept[1].exchange(0)),
                            static_cast<unsigned>(g_concWin.kept[2].exchange(0)), static_cast<unsigned>(g_concWin.kept[3].exchange(0)),
                            static_cast<unsigned>(g_concWin.storeCleared.exchange(0)), ms, static_cast<unsigned>(treated), concoursePaneModeName(),
                            static_cast<unsigned>(concoursePaneSlotCount()), static_cast<unsigned>(withheld), cc::clearPhaseName(clr.phase),
                            static_cast<unsigned>(mir.markPasses), static_cast<unsigned>(mir.markRefused), static_cast<unsigned>(mir.known),
                            static_cast<unsigned>(mir.exterior), static_cast<unsigned>(mir.dropped), static_cast<unsigned>(mir.tests), static_cast<unsigned>(mir.shareSamplesDropped));
        } else if (concoursePaneTreating()) {
            Log::get().note("concourse pane 5s: treated %u draws (mode %s, %u slots), outside draws withheld %u, phase %s, mark passes %u (refused %u), panes known %u exterior %u "
                            "(dropped %u), tests %u, share samples dropped %u",
                            static_cast<unsigned>(treated), concoursePaneModeName(), static_cast<unsigned>(concoursePaneSlotCount()), static_cast<unsigned>(withheld),
                            cc::clearPhaseName(clr.phase), static_cast<unsigned>(mir.markPasses), static_cast<unsigned>(mir.markRefused), static_cast<unsigned>(mir.known),
                            static_cast<unsigned>(mir.exterior), static_cast<unsigned>(mir.dropped), static_cast<unsigned>(mir.tests), static_cast<unsigned>(mir.shareSamplesDropped));
        }
    }
    (void)frame;
}

}  // namespace

void optimizedStationFrameBody(uint32_t frame);

// Built 2026-10-15: the seal trace's emitter. Each line goes out as one "%s" argument, so the trace's text is never a log format.
void traceEmit(void* /*ctx*/, const char* line) noexcept {
    Log::get().note("%s", line);
}

// The frame tick, timed every frame while the setting is on (built v14.24).
void optimizedStationFrame(uint32_t frame) {
    const bool on = g_setting.load(std::memory_order_acquire);
    const int64_t t0 = on ? costTicks() : 0;
    optimizedStationFrameBody(frame);
    if (on) costAdd(g_costFrame, costMs(t0, costTicks()));
}

void optimizedStationFrameBody(uint32_t frame) {
    if (++g_settingPoll >= kSettingPollFrames) {
        g_settingPoll = 0;
        const std::string text = Config::get().getString("fix.optimized_stations", "on");   // on in VR and in flat (built v14.34)
        const bool on = text != "off";
        if (on != g_setting.load(std::memory_order_acquire)) {
            g_setting.store(on, std::memory_order_release);
            applySetting(on, frame);
        }
    }
    const uint64_t now = GetTickCount64();
    if (!g_setting.load(std::memory_order_acquire)) {
        if (g_summaryStartMs == 0) g_summaryStartMs = now;
        concourseSettingOffTick(now);   // built 2026-11-05: the concourse cull stands down with the setting, and says so once
        return;
    }
    if (g_summaryStartMs == 0) g_summaryStartMs = now;
    // Built v14.2: the frame advances here, once per tick. The observer's promote and view resolve run once per frame off this stamp.
    g_frameStamp.fetch_add(1, std::memory_order_seq_cst);   // built v14.27: the callbacks that read the old stamp are the old pass's writers
    TimedStateLock lock;
    prodSync();   // built v14.26: the producer generation moves with the frame (the callbacks never move it)
    drainGather();          // the entries of the passes the stamp just closed, if every writer of them has left (built v14.27)
    resolveTickView();      // the camera from the pass's busiest context; held when the pass is empty or unfinished (built v14.27)
    ++g_winFrames;
    if (g_dockedNow.load(std::memory_order_acquire)) ++g_winDockedFrames;
    if (g_heldValid) ++g_winSealedFrames;
    // Built v14.34: the Status.json docked flag under either profile (the flat profile's journal accessors answer the no-journal values).
    g_dockedKnownNow.store(journalStatusDockedKnown(), std::memory_order_release);
    g_dockedNow.store(journalStatusDockedKnown() && journalStatusDocked(), std::memory_order_release);
    g_statusKnownNow = journalStatusSeatedKnown();
    g_aboardNow = g_statusKnownNow && journalStatusSeated();
    g_statusFlagsNow = journalStatusFlags();
    g_statusFlags2Now = journalStatusFlags2();
    {   // The flag-change instrument: every change of the two words (known or not) is one line, so one flight shows when each bit moves against the lift's own events.
        const bool changed = !g_flagLogSet || g_flagLogKnown != g_statusKnownNow || g_flagLogFlags != g_statusFlagsNow || g_flagLogFlags2 != g_statusFlags2Now;
        if (changed && g_flagLogN < 400 && (g_dockedNow.load(std::memory_order_acquire) || g_heldValid || g_flagLogSet)) {
            g_flagLogSet = true;
            g_flagLogKnown = g_statusKnownNow;
            g_flagLogFlags = g_statusFlagsNow;
            g_flagLogFlags2 = g_statusFlags2Now;
            ++g_flagLogN;
            char text[300];
            sealed::describeStatusFlags(text, sizeof(text), g_statusKnownNow, g_statusFlagsNow, g_statusFlags2Now);
            Log::get().note("optimized station: status flags changed (frame %u): %s; aboard %s; seal %s.", frame, text, g_aboardNow ? "yes" : "no", g_heldValid ? "held" : "not held");
        }
    }
    // Built v14.20 (review 4): an undock is a lifecycle point: the rig and collection tables start afresh.
    if (g_dockedWasNow && !g_dockedNow.load(std::memory_order_acquire)) {
        bumpRigEpoch();
        g_track.reset(padStateNowQpc());   // built v14.23: an undock ends the dock's own pad (the game names a new one on the next dock)
    }
    g_dockedWasNow = g_dockedNow.load(std::memory_order_acquire);
    concourseTick(now, frame);   // built 2026-11-05: the concourse cull's state, before the gate reads it
    storeHookGate();   // built v14.20 (review 7b): the store observer gathers only while docked, or while the concourse cull is wanted (an atomic gate: no hook is installed here)
    gameStateTick();   // built v14.23: the own pad from the game, before the sun and the trigger read it
    const sealed::HeldRefresh heldRefresh = refreshHeldWorld();   // built 2026-10-15: the call's result goes to the seal trace
    // The camera against the held box: outside it by more than kCameraLeaveM releases the seal.
    bool cameraOutside = false;
    if (g_heldValid && g_frameCamValid) {
        if (g_heldCell.box) {
            float x[3] = {};
            sealed::boxCoordsOf(g_heldCell, g_frameCam, x);
            cameraOutside = !(std::fabs(x[0]) <= g_heldCell.boxHalf[0] + sealed::kCameraLeaveM &&
                              std::fabs(x[1]) <= g_heldCell.boxHalf[1] + sealed::kCameraLeaveM &&
                              std::fabs(x[2]) <= g_heldCell.boxHalf[2] + sealed::kCameraLeaveM);
        } else {
            cameraOutside = sealed::distance(g_heldCell.centre, g_frameCam) > g_heldCell.radius + sealed::kCameraLeaveM;
        }
    }
    // Built v14.6: a ship change (the own ship's type prefix, from the candidate record, changes for kShipChangeTicks readings in a row). It
    // releases the seal, resets the trigger and the safety net, and rebuilds the own-ship rule: a swapped ship in a closed bay then seals at once.
    if (g_ownCandidateDist >= 0.0f) {
        char curPrefix[32] = {};
        sealed::ownPrefixOf(g_ownCandidateName, curPrefix, sizeof(curPrefix));
        char oldPrefix[32] = {};
        std::snprintf(oldPrefix, sizeof(oldPrefix), "%s", g_shipWatch.prefix);
        if (sealed::shipPrefixNamesShip(curPrefix, prefixTallyOf(curPrefix)) && g_shipWatch.step(curPrefix)) {
            char why[160];
            std::snprintf(why, sizeof(why), "ship changed (%s -> %s)", oldPrefix, curPrefix);
            if (g_heldValid) {
                releaseSeal(why, frame);
            } else {
                Log::get().note("optimized station: %s; the trigger starts afresh", why);
            }
            g_sealState = sealed::SealState{};
            g_sealGuard = sealed::SealRateGuard{};
            g_preShellValid = false;
            setOwnPrefixAtEngage(frame);
            g_ownRecAddr = 0;
            g_ownLiveValid = false;
            g_latch.reset();   // a new ship: the mechanism is latched again
            bumpRigEpoch();    // built v14.20 (review 4): a ship change is a lifecycle point
        }
    }
    // Built v14.5: the height above the shell along the pad's own up axis, read live, with the shell picked once per pad (the same pick as the
    // freeze, from the gathered parts). The camera in the candidate box is the seal's other condition.
    // Built v14.9: a pad absent for fewer than kHeldLostFrames frames keeps its last reading (no still-clock reset).
    const bool padHeld = !g_pickCell.has && g_ownSticky.pad != 0 && g_ownSticky.unseen > 0 && g_ownSticky.unseen < sealed::kHeldLostFrames;
    if (!padHeld) {
        g_relOk = false;
        g_shellNowValid = false;
    }
    g_frameShell = sealed::ShellFrame{};   // built v14.17: the pad in the shell's frame, read when the height is
    if (g_pickCell.has) {
        g_relOk = false;
        if (!g_hangarWatch.giveUp && (!g_preShellValid || g_preShellPad != g_pickCell.pad)) {
            const size_t pick = sealed::pickShell(g_explicits, g_explicitCount, g_pickCell.centre, g_pickCell.size, 150.0f);
            g_preShellValid = pick < g_explicitCount;
            g_preShellPad = g_pickCell.pad;
            if (g_preShellValid) {
                g_preShellAddr = g_explicits[pick].addr;
                std::snprintf(g_preShellName, sizeof(g_preShellName), "%s", g_explicits[pick].name);
            }
        }
        if (g_preShellValid) {
            char nm[48] = {};
            if (!cachedName(g_preShellAddr, nm, sizeof(nm)) || std::strcmp(nm, g_preShellName) != 0) {
                g_preShellValid = false;   // the record moved on: the next tick picks again
            } else {
                const WorldRead pw = worldOfRecord(g_pickCell.pad);
                const WorldRead sw = worldOfRecord(g_preShellAddr);
                float axes[3][3] = {};
                float ext[3] = {};
                bool extOk = false;
                uint8_t src = 4;
                if (pw.source != 4 && sw.source != 4 && poseOrientOf(g_pickCell.pad, axes, ext, &extOk, &src)) {
                    const float up[3] = {axes[2][0], axes[2][1], axes[2][2]};
                    g_relM = sealed::shellRelative(pw.world, sw.world, up);
                    g_relOk = true;
                    g_shellNow[0] = sw.world[0];
                    g_shellNow[1] = sw.world[1];
                    g_shellNow[2] = sw.world[2];
                    g_shellNowSrc = sw.source;
                    g_shellNowValid = true;
                    // The pad in the shell's frame (the sealed line's lateral and yaw).
                    float shAxes[3][3] = {};
                    float shExt[3] = {};
                    bool shExtOk = false;
                    uint8_t shSrc = 4;
                    if (poseOrientOf(g_preShellAddr, shAxes, shExt, &shExtOk, &shSrc)) {
                        g_frameShell = sealed::shellFrameOf(pw.world, sw.world, up, shAxes[0], axes[0]);
                    }
                }
            }
        }
    }
    bool camIn = false;
    if (g_frameCamValid && g_pickCell.has) {
        if (g_pickCell.box) {
            float x[3] = {};
            sealed::boxCoordsOf(g_pickCell, g_frameCam, x);
            camIn = std::fabs(x[0]) <= g_pickCell.boxHalf[0] && std::fabs(x[1]) <= g_pickCell.boxHalf[1] && std::fabs(x[2]) <= g_pickCell.boxHalf[2];
        } else {
            camIn = sealed::distance(g_pickCell.centre, g_frameCam) <= g_pickCell.radius;
        }
    }
    walkSurveyTick(now, camIn);   // TEMPORARY, log only (the walk survey)
    // Built v14.12: the latch is kept while docked, unsealed, after a descent; the candidates of this frame are consumed here.
    g_watchNowMs = now;
    g_bayWatching = bayWatchKeep();
    g_bayNoting = g_bayWatching && g_shellNowValid;
    if (!g_bayWatching) {
        g_latch.reset();
    }
    // Built v14.20 (review 2): a genuine change of the unsealed bay's pad or shell clears the latch and its motion references; it is regathered below.
    // A sample that misses the pad keeps the identity (a brief miss is no change).
    if (g_latch.latched && !g_sealedFlag.load(std::memory_order_acquire) &&
        g_latchId.step(g_pickCell.has, g_pickCell.pad, g_preShellValid, g_preShellAddr)) {
        g_latch.reset();
        Log::get().note("optimized station: the bay changed (pad %s): its mechanism is latched again", g_pickCell.padName);
    }
    g_latchPending = g_bayWatching && !g_latch.latched;
    if (g_latchPending && g_pickCell.has) latchNow(now);
    g_latchCandN = 0;
    if (g_latch.latched && g_bayNoting && g_passInformative) latchRead(now);
    // Built v14.33: while sealed and not yet armed, every informative pass in which every latched record was read and none moved counts toward the arm.
    if (g_sealedFlag.load(std::memory_order_acquire) && !g_mechArm.armed && g_passInformative && g_latch.latched) {
        const uint32_t frameNow = g_frameStamp.load(std::memory_order_relaxed);
        bool allStill = true;
        for (size_t i = 0; i < g_latch.n; ++i) {
            if (g_latch.recs[i].stamp != frameNow || g_latch.recs[i].movedRun != 0) allStill = false;
        }
        if (g_mechArm.step(allStill)) {
            snapshotSealPose();   // the sealed poses are the poses of this still bay
            Log::get().note("optimized station: mechanism release armed: %u records still for 1 s.", static_cast<unsigned>(g_latch.n));
        }
    }   // built v14.27: no sample from an empty or unfinished pass (the next sample re-bases)
    // Built v14.14: a latched mechanism record moved on two frames in a row while sealed: the launch releases the seal.
    g_mechanismMoved = false;
    if (g_sealedFlag.load(std::memory_order_acquire) && g_mechArm.armed) {   // built v14.33: only once the bay has been still for 60 passes
        const int mi = g_latch.movedIndex();
        if (mi >= 0) {
            if (mi == 0) std::snprintf(g_releaseWhy, sizeof(g_releaseWhy), "%s", "the pad started moving: launch");
            else std::snprintf(g_releaseWhy, sizeof(g_releaseWhy), "the bay mechanism moved (%s): launch", g_latch.recs[mi].name);
            g_mechanismMoved = true;
        }
    }
    // Built v14.13: the hangar watch (a sized shell near the pad) and its once-per-dock names; the sun inside the hangar (its own state).
    const bool shellFound = g_preShellValid;
    const bool padChanged = g_pickCell.has && g_pickCell.pad != g_hangarPad;
    if (g_pickCell.has) {
        g_hangarPad = g_pickCell.pad;
        for (int k = 0; k < 3; ++k) g_namesPadC[k] = g_pickCell.centre[k];
    }
    const bool padInHangar = g_game.r1 == sealed::kReaderInHangar;
    const sealed::HangarWatchEvent hev = g_hangarWatch.step(shellFound, g_dockedNow.load(std::memory_order_acquire), padChanged, padInHangar);
    if (hev == sealed::HangarWatchEvent::kNote) {
        hangarNote();
    } else if (hev == sealed::HangarWatchEvent::kGiveUp) {
        Log::get().note("optimized station: no sized hangar shell within 150 m of the pad for %u frames (r0=%d r1=%d, in hangar=%s): the hangar "
                        "parts are not gathered again until undock, a new pad, or the game putting the own pad in the hangar",
                        static_cast<unsigned>(sealed::kNoShellGiveUpFrames), g_game.r0, g_game.r1, padInHangar ? "yes" : "no");
    } else if (hev == sealed::HangarWatchEvent::kRearm) {
        g_nameLogN = 0;   // the next note names what is near the pad where it is now
        std::snprintf(g_hangarWhy, sizeof(g_hangarWhy), "%s", "searching for a hangar shell");
        Log::get().note("optimized station: the game put the own pad in the hangar (r1 0 to 1) after the hangar search had stopped: the search "
                        "starts again from the pad where it is now");
    }
    if (!g_dockedNow.load(std::memory_order_acquire)) {
        g_nameLogN = 0;
        std::snprintf(g_hangarWhy, sizeof(g_hangarWhy), "%s", "searching for a hangar shell");
    }
    g_namesPending = g_dockedNow.load(std::memory_order_acquire) && !shellFound && g_pickCell.has && !g_hangarWatch.logged;
    {
        sealed::SunInputs su;
        su.setting = g_setting.load(std::memory_order_acquire);
        su.surfacePort = journal_station::atSurfacePort(journalStationKind(), g_statusFlagsNow, g_statusFlags2Now);
        su.docked = g_dockedNow.load(std::memory_order_acquire);
        {   // One line a dock, so a flight says which rule a port was under: no such line at a surface port means the sun was still on the pad's rule.
            static bool saidSurface = false;
            if (!su.docked) saidSurface = false;
            else if (su.surfacePort && !saidSurface) {
                saidSurface = true;
                Log::get().note("optimized station: a port on a planet's surface (type %s, Status flags 0x%08X 0x%08X): the sun stays on", journal_station::kindName(journalStationKind()), g_statusFlagsNow, g_statusFlags2Now);
            }
        }
        su.ownKnown = g_game.ownKnown;
        su.r0Fresh = g_game.r0Fresh;
        su.r0 = static_cast<uint8_t>(g_game.r0 < 0 ? 0 : g_game.r0);
        su.r1Fresh = g_game.r1Fresh;
        su.inHangar = g_game.r1 == sealed::kReaderInHangar;
        su.relOk = g_relOk;
        su.rel = g_relM;
        su.hangarChanged = g_flagHangarChanged;
        const char* why = "";
        if (sealed::sunStep(su, &g_sun, &why)) {
            detail::g_sunOff.store(g_sun.off, std::memory_order_release);
            // Built v14.23: the sun line carries the own pad's r0 and r1 and the Status docked flag.
            const char* dockedText = g_dockedNow.load(std::memory_order_acquire) ? "yes" : "no";
            char r0Text[16];
            char r1Text[16];
            if (g_game.r0 < 0) std::snprintf(r0Text, sizeof(r0Text), "-");
            else std::snprintf(r0Text, sizeof(r0Text), "%d", g_game.r0);
            if (g_game.r1 < 0) std::snprintf(r1Text, sizeof(r1Text), "-");
            else std::snprintf(r1Text, sizeof(r1Text), "%d", g_game.r1);
            if (g_sun.off) {
                Log::get().note("optimized station: sun light inside the hangar off (%s; r0=%s r1=%s; Status docked=%s)", why, r0Text, r1Text, dockedText);
            } else {
                Log::get().note("optimized station: sun light inside the hangar back on (%s; r0=%s r1=%s; Status docked=%s)", why, r0Text, r1Text,
                                dockedText);
            }
        }
    }
    sealed::SealInputs in;
    in.setting = true;   // the tick returns above while the setting is off
    in.docked = g_dockedNow;
    in.aboard = g_aboardNow;
    in.ownKnown = g_game.ownKnown;
    in.poseBack = mechanismPoseTick(now);   // built v14.24: the latched records back at their sealed poses (the mechanism re-seal)
    in.r1Fresh = g_game.r1Fresh;
    in.inHangar = g_game.r1 == sealed::kReaderInHangar;
    in.pickOk = g_pickCell.has && g_preShellValid;   // no sized shell: the trigger stays idle (built v14.13)
    in.padHeld = padHeld;
    in.mechanismMoved = g_mechanismMoved;
    in.cameraInBox = camIn;
    in.pad = g_pickCell.has ? g_pickCell.pad : 0;
    in.held = g_heldValid;
    in.cameraOutside = cameraOutside;
    in.padMoved = g_flagPadMoved;
    if (g_flagHangarChanged) g_sealGuard = sealed::SealRateGuard{};   // a new hangar: the safety net starts afresh
    if (g_flagHangarChanged) {   // a new hangar: the trigger, the latch and the own pad start afresh (built v14.23)
        g_sealState = sealed::SealState{};
        g_preShellValid = false;
        g_latch.reset();
        g_track.reset(padStateNowQpc());
        bumpRigEpoch();   // built v14.20 (review 4): a hangar change is a lifecycle point
    }
    in.hangarChanged = g_flagHangarChanged;
    in.launched = false;   // the pad state object's reader is not in this build: the launch is the r1 edge and the mechanism
    // Built 2026-10-15: the origin shift latchRead read this tick (before this point, on the same tick): the seal step takes it once.
    in.originShifted = g_originShiftPending;
    // Built 2026-11-06: the guard. g_stickyPadInPass and g_passInformative were set by drainGather earlier in this same tick (the T4 trigger reads the same two at
    // the end of it), so they are valid here. The run counts informative passes without the sealed own pad; at kPadMissingPasses the seal is released.
    g_padMissingRun = sealed::padMissingRun(g_padMissingRun, g_passInformative && g_ownSticky.pad != 0, g_stickyPadInPass, g_heldValid);
    in.padMissing = g_padMissingRun >= sealed::kPadMissingPasses;
    in.nowMs = now;
    sealed::ReleaseWhy why = sealed::ReleaseWhy::kNone;
    const uint64_t holdBefore = g_sealState.holdUntilMs;
    const sealed::SealAction act = sealed::sealStep(in, &g_sealState, &why);
    g_originShiftPending = false;
    // Built 2026-10-15: what set the hold, for its line: an origin-shift or Lift release, or an origin shift read while docked.
    if (act == sealed::SealAction::kRelease && why == sealed::ReleaseWhy::kOriginShift) g_holdCause = "origin shift";
    else if (act == sealed::SealAction::kRelease && why == sealed::ReleaseWhy::kMechanism) g_holdCause = "mechanism release";
    else if (act == sealed::SealAction::kRelease && why == sealed::ReleaseWhy::kPadMissing) g_holdCause = "guard release (the own pad's record was missing)";
    else if (in.originShifted && !in.held) g_holdCause = "origin shift";
    const bool holdStarted = g_sealState.holdUntilMs != holdBefore && g_sealState.holdUntilMs != 0;
    g_flagPadMoved = false;
    g_flagHangarChanged = false;
    {
        // Built v14.20 (review 1): the trigger withdrew its request (the pad rose, the camera left, the pad changed): the pending engage is cancelled.
        sealed::EngageRequest er{g_engagePending, g_engagePad, g_engageShell};
        sealed::engageTrigger(&er, g_sealState);
        g_engagePending = er.pending;
    }
    float releasedCentre[3] = {};   // the held cell's centre at a release, for that tick's trace row (releaseSeal clears the cell)
    if (act == sealed::SealAction::kRelease) {
        std::memcpy(releasedCentre, g_heldCell.centre, sizeof(releasedCentre));   // for the trace row of the release tick (releaseSeal clears the cell)
        if (why == sealed::ReleaseWhy::kPadMissing) {
            // The guard says what it saw before it lets go: the own pad's rig as the tick last saw it, and the pad's stage counts since the seal.
            const sealed::OwnRigTrace& o = g_ownRigTrace;
            const char* verdictText = o.verdict == 0 ? "unknown" : o.verdict == 1 ? "mixed" : o.verdict == 2 ? "outside" : "none";
            static const char* const kPub[] = {"current", "not published", "earlier epoch", "collection moved"};
            const auto st = [](sealed::PadStage k) { return static_cast<unsigned long long>(g_padStageSinceSeal[k].load(std::memory_order_relaxed)); };
            Log::get().note("optimized station: guard: the own pad's record was missing from %u passes in a row while sealed; releasing, because a cell that drops its own "
                            "pad may be wrong. Pad rig: known %s, publication %s, verdict %s, run %u, bounded %s, dist %.1f m, radius %.1f m; pad stages since the seal: "
                            "store %llu, early %llu, unread %llu, gathered %llu, producer vetoed %llu, kept %llu.",
                            static_cast<unsigned>(g_padMissingRun), o.known ? "yes" : "no", o.known ? kPub[o.publication < 4 ? o.publication : 1] : "-",
                            o.known ? verdictText : "-", static_cast<unsigned>(o.run), o.bounded ? "yes" : "no", o.bounded ? static_cast<double>(o.distM) : 0.0,
                            o.bounded ? static_cast<double>(o.radiusM) : 0.0, st(sealed::kPadStore), st(sealed::kPadEarly), st(sealed::kPadUnread),
                            st(sealed::kPadGathered), st(sealed::kPadProdVetoed), st(sealed::kPadProdKept));
            bumpRigEpoch();   // the rigs are judged afresh from the next pass
            g_padMissingRun = 0;
        }
        releaseSeal(releaseText(why), frame);
    } else if (act == sealed::SealAction::kRequestEngage) {
        if (g_sealGuard.tripped) {
            // refused until a hangar change or a setting toggle (the safety net, built v14.4)
        } else if (g_sealGuard.note(now)) {
            Log::get().note("optimized station: sealing stopped for this hangar: more than %u seals in a minute (the pad flickers). "
                            "It resumes at a hangar change or a setting toggle.", static_cast<unsigned>(sealed::kSealRateMax));
        } else {
            g_engagePending = true;
            g_engagePad = in.pad;          // built v14.20 (review 1): bound to the pad and the shell this request was made for
            g_engageShell = g_preShellValid ? g_preShellAddr : 0;   // the shell the request is bound to (built v14.20)
            g_engageTries = 0;
        }
    }
    // Built 2026-10-15: a request the transition hold kept back is written once, when it begins (not per tick); it ends with the request or the trigger.
    if (g_sealState.holdBlocked) {
        if (!g_holdLineOpen) {
            g_holdLineOpen = true;
            Log::get().note("optimized station: re-seal held until %.1f s after the %s (Status.json reports the seat about a second late).",
                            static_cast<double>(sealed::kTransitionHoldMs) / 1000.0, g_holdCause);
        }
    } else {
        g_holdLineOpen = false;
    }
    padStateTick(now, g_track.have ? g_track.own : 0);   // built v14.23: the own pad's change lines and the seal or release line (log only)
    // Built v14.23: the waiting line, while docked and unsealed: one line when a key changes, and at most every 10 s while docked.
    if (!g_heldValid) {
        sealed::GameView w;
        w.dockedKnown = g_dockedKnownNow.load(std::memory_order_acquire);
        w.docked = g_dockedNow.load(std::memory_order_acquire);
        w.ownKnown = g_game.ownKnown;
        w.own = g_game.own;
        w.r0 = g_game.r0;
        w.r1 = g_game.r1;
        w.r0Fresh = g_game.r0Fresh;
        w.r1Fresh = g_game.r1Fresh;
        w.cameraKnown = g_frameCamValid;
        w.cameraIn = camIn;
        w.cellBuilt = g_pickCell.has && g_preShellValid && sealed::cellInvalidWhy(g_pickCell) == nullptr;
        if (!w.cellBuilt) {
            if (!g_preShellValid) std::snprintf(w.cellReason, sizeof(w.cellReason), "%s", g_hangarWhy);
            else if (!g_pickCell.has) std::snprintf(w.cellReason, sizeof(w.cellReason), "%s", g_pickCell.reason);
            else std::snprintf(w.cellReason, sizeof(w.cellReason), "%s", sealed::cellInvalidWhy(g_pickCell));
        }
        // Built v14.27: a change is written once it has held kWaitStableTicks ticks (the key flipping every frame wrote 231 to 800 lines per 30 s in v14.26).
        const bool candChanged = !g_waitCandSet || sealed::gameKeyChanged(g_waitCand, w);
        if (candChanged) {
            g_waitCand = w;
            g_waitCandSet = true;
        }
        const bool stable = g_waitStable.step(candChanged, sealed::kWaitStableTicks);
        const bool changed = !g_waitLastSet || (stable && sealed::gameKeyChanged(g_waitLast, g_waitCand));
        const bool periodic = w.docked && g_waitLastSet && now - g_waitLastMs >= 10000;
        if (changed || periodic) {
            char line[480] = {};
            if (changed) g_waitLast = g_waitCand;   // the periodic line repeats the last stable key
            sealed::gameWaitingLine(g_waitLast, line, sizeof(line));
            Log::get().note("%s", line);
            ++g_waitingLinesWritten;   // built v14.24: the waiting lines written this window (the summary's count)
            g_waitLastSet = true;
            g_waitLastMs = now;
        }
    }
    {
        // Built v14.26: the pads, explicit parts and camera of the pass just drained make the cell for the next pass (the observer's promote, moved here).
        const uint32_t stamp = g_frameStamp.load(std::memory_order_relaxed);
        // Built v14.27: only an informative pass makes the cell. An empty or unfinished one leaves the pick, the pad and the windows as they were.
        if (g_passInformative && sealed::framePromoteDue(g_cellStamp, stamp)) promoteFrame(stamp, stamp);
    }
    if (g_passInformative && g_ownSticky.pad != 0) {
        if (g_stickyPadInPass) ++g_winPadSeen;
        else ++g_winPadMissed;
        if (!g_padWin.has(g_ownSticky.pad)) ++g_winPadAbsentWindow;
    }
    // Built v14.27: the rig verdicts are republished from an informative pass only; the last publication stands otherwise. Without a held cell the
    // publication is emptied at once (the veto answers only while sealed).
    if (g_passInformative || !g_heldValid) refreshRigVerdicts();
    publishSnapshot();   // built v14.25: the tick's view for the per-record path (under the lock)
    {
        // Built 2026-10-15: the seal trace takes this tick's record. It runs before the 30 s line resets the window totals, which it reads as deltas.
        sealed::TickRecord tr;
        tr.frame = frame;
        tr.nowMs = now;
        tr.docked = g_dockedNow.load(std::memory_order_acquire);
        tr.held = g_heldValid || why == sealed::ReleaseWhy::kPadMissing;   // the guard's release tick still counts as held, so T4 (the same three passes) dumps its trace
        tr.verdict = g_passVerdictLast;
        tr.thin = g_passThinLast;
        tr.passCalls = g_passCalls;
        tr.ownSet = g_ownSticky.pad != 0;
        tr.ownInPass = g_stickyPadInPass;
        tr.ownUnseen = static_cast<uint32_t>(g_ownSticky.unseen);
        tr.refresh = heldRefresh;
        if (g_heldValid) std::memcpy(tr.centre, g_heldCell.centre, sizeof(tr.centre));
        else if (why == sealed::ReleaseWhy::kPadMissing) std::memcpy(tr.centre, releasedCentre, sizeof(tr.centre));
        tr.frameShiftM = g_frameShiftM;
        tr.camValid = g_frameCamValid;
        std::memcpy(tr.cam, g_frameCam, sizeof(tr.cam));
        tr.cameraOutside = cameraOutside;
        tr.rigVetoes = g_winRigVetoes.load(std::memory_order_relaxed);
        tr.producerVetoes = g_winProducerVetoes.load(std::memory_order_relaxed);
        tr.culled = g_winCulled.load(std::memory_order_relaxed);
        updateOwnRigTrace();   // log only, 2026-11-03: the sealed pad's rig as the tick sees it, and the pad's stage counts (the trace takes the deltas)
        tr.rig = g_ownRigTrace;
        for (size_t k = 0; k < sealed::kPadStages; ++k) tr.pad[k] = g_padStage[k].load(std::memory_order_relaxed);
        tr.released = act == sealed::SealAction::kRelease;
        tr.why = why;
        tr.originShift = in.originShifted;
        tr.holdStarted = holdStarted;
        g_sealTrace.step(tr, traceEmit, nullptr);
    }
    if (now - g_summaryStartMs >= kSummaryMs) {
        summaryLine(now);
        g_sealTrace.windowReset();   // the window totals start again at zero
    }
    (void)frame;
}

}  // namespace edvr
