# The missing terrain at the edges of view

## Status

- **State (2026-10-10): CLOSED. Two fixes ship; neither has a setting.** (1) The head-pose fix,
  `next` (flights 4 and 5, in 0.19, unchanged here): Elite's game-thread "now" head-pose
  requests (return RVA 0x4E3881, prediction under 5 ms) are located at the latest frame's
  display time PLUS its period; that pose was 41-44 ms older than the drawn one, up to 4.1
  deg, r +0.98 with head speed. (2) The TERRAIN CULL COVER (new): Elite culls planet terrain
  against a CENTRED frustum per eye; EDVR widens the two numbers it is built from to the
  frustum the eye really has. The flashes that were left after (1) were this.
- **READ (build 332841; analysis\decomp\verify_20261010_tiles_*).** The draw-list builder
  0x4462E80 calls 0x4455A40 (call at 0x4463A83), one record per render view. For a main view
  it loads the eye camera's aspect [r14+0x70] (the recommended W/H, the fov getter's out[0])
  and vFOV [r14+0x90] (atan|t|+atan|b|, out[1]) and calls 0x443E800 (the ONLY caller, call at
  0x4456007; args 5, 6 at [rsp+0x28], [rsp+0x30]). It halves the vFOV (0.5 at 0x4DDF878),
  takes b = atan(aspect * tan a), writes four planes through the eye, +-a and +-b about the
  camera Z axis: no margin. The test 0x445BA40 -> 0x445A1C0 culls a patch box against them
  with no epsilon; LOD uses another box (0x44597B0); the image uses the projection matrix.
- **So** an asymmetric eye is under-covered on its wide side by ~5 deg (down too on the Quest
  3): cropped Crystal Super cull +-45.83 v / +-46.80 h vs an outer edge of 51.44; uncropped
  +-51.67 / +-52.02 vs 56.82; Quest 3 via VD +-49.50 / +-47.78 vs 54.00 outward, 55.00 down.
  A patch drops only when its WHOLE box lies in the band: rare at rest, common as patches
  slide through it on a turn; flat ground has the largest (shapes that float). The uncropped
  field (103 x 103 deg, no flashes) has the band in the lens periphery; the cropped 93 x 92
  and VD's 94 x 99 put it in plain view.
- **What ships.** A CodeHook at 0x443E800 (src\d3d11\terrain_cull_cover_*, tools\terrain_cover_test)
  that changes ONLY args 5 and 6. T, H = the largest vertical, horizontal tangent over both eyes
  of the frusta handed to the game; a = max(vFOV/2, atan T), b = max(atan(aspect * tan(vFOV/2)),
  atan H); vFOV' = 2a, aspect' = tan b / tan a. Never narrows; unchanged for no tangents yet
  (flat, before a frame) and non-finite or out-of-range input. VR only (flat says so, once).
  Gate: PE stamp 1788384820, size 104894464, head bytes and call site; another stamp: scan
  .text for the call-site signature, one match with the same head, else inert. Log: `terrain
  cull cover: hook live at exe+0x443E800 (..)` or `inert: <why>`; one `first call: in aspect ..
  vfov .. -> out ..`; `armed; N of M calls widened in the last 10 min (..)` every 10 min, zeros
  included. `python tools\edup_log.py --tally cover` tables them.
- **FLOWN (2026-10-10, Quest 3 via VD, 90 Hz, Frontier, test build 52fe241c; gfx
  edvr_gfx_20261010_190514.log, runtime edvr_openxr_20261010_190515_484_25604.log).** Cover
  off 19:05:14-19:10:54 and 19:14:27-end, on 19:10:54-19:14:27: pilot 20 black flashes with
  it off (~5.7 min), none with it on (~3.5 min); ~12 were expected on at the off rate, p ~ 5e-6.
  Every call took the eye's own aspect 0.9412 / vFOV 99.00 and left with 0.9638 / 110.00,
  100% widened on, 0% off; 19-25 calls a frame (one per planet patch group per eye). 89.7-90.0
  fps, 0-1 late frames a minute while on: no measurable cost.
- **OVERTURNED:** "a static frustum deficit (the centred-cull models, H-cam included) is ruled
  out because a steady head shows no squares" (Status detail, 10-09). Round 4's "the camera
  groups cannot move an eye camera's frustum" holds for the RENDERED frustum, not this consumer.
- **Ruled out** as the cause of the residual flashes: the pose timing (late frames, the stale
  fallback, overshoot, the arrival mix; test build f24ca5d0, Q3 via VD: alike under next, slot
  and arrival, 0 late frames and fallbacks from 17:13). Earlier, in Status detail: the fov
  getter as the culler's input, the 09-23 "+19.4% ask" premise, H3, the union model, H2,
  FUN_13ACC40. **UNRELIABLE:** the 09-23 "culler follows `GetProjectionRaw`".
- **Not shipped: the edge-black watch** (claude/cull-late-frames only). It failed as an
  instrument: ~5000 events a minute in both modes (on 121% of off), almost all one-cell-tall
  3x1..8x1 horizontal streaks at fixed heights (y ~0.18, ~0.80) in both eyes, border=0, head
  1-5 deg/s: high-contrast cockpit/UI edges flickering between frames, swamping the flashes.
- **Never on main:** `advanced.cull_pose_timing` (slot, arrival), `advanced.terrain_cull_cover`
  (no retired-key entry). **Not shipped (10-09):** the latch-bypass patch (RVA 0x4E36EE),
  `advanced.cull_pose`, `HandedOutPoses`. **Removed (10-09):** the terrain guard and its six
  keys, `advanced.cull_probe`. **Separate:** the flight-4 boarding crash is Elite's allocator.
- **Open: none.** A confirming flight on the cropped Crystal Super is optional.

## The bug in short

*Frontier issue [72609](https://issues.frontierstore.net/issue-detail/72609) —
"Culling of planet surface in VR too aggressive", a recurrence of
[37119](https://issues.frontierstore.net/issue-detail/37119), which was
fixed once and marked so.*

This is a write-up of what the bug actually does, measured rather than
guessed, and what EDVR does about it from outside the game. It is written for
whoever might fix it properly, inside the game, where it can be fixed at zero
rendering cost.

**Short version: Elite culls planet terrain against a frustum narrower than
the one it renders, in the way a culler behaves when it treats the per-eye
frustum as symmetric.** VR eye frusta are strongly asymmetric, so terrain
tiles inside the visible outer edges are never drawn, and the player sees
black squares where ground should be. Report a *symmetrized* frustum to the
game — while showing the player exactly what was shown before — and the
missing tiles come back. Report the truth again and they vanish again. The
culler follows the report, not the optics.

---

## Status detail (moved out of Status 2026-09-29)

**The Status block as it stood at the flight-3 result, moved verbatim
2026-10-09** (it names the temporary instruments, `advanced.cull_probe` and
`advanced.cull_pose`, which no longer exist, and the old guard):

- **State (2026-10-09, flight 3): ROOT CAUSE CONFIRMED, FIX FOUND.** Elite's
  game thread asks for the head pose "now" (GetDeviceToAbsoluteTrackingPose,
  return RVA 0x4E3881, prediction ~0 s) and selects terrain with it; the frame is
  drawn with the display-time pose. Measured on the Crystal Super / Pimax
  OpenXR, build e256e9bb, `edup_log.py --tally pose` on
  `edvr_openxr_20261009_144149_193_26440.log`: under `off` that pose is
  **-41 to -44 ms** from the drawn frame's display time and turned 0.9 deg mean,
  up to 4.1 deg, from the drawn pose, r = +0.98 against head speed. Pilot:
  `display`, `next` and `display_direct` all stopped the squares on gaze
  switches; one unreproduced possible sighting under `next_direct` (one frame
  of over-lead, 0.65 deg mean). **Fix: answer that call at the drawn frame's
  display time (`display`); no engine patch, no extra pixels, no tuning.** The
  latched-pose bypass is not needed. The old guard (render inflation + crop,
  ~6% GPU at h=0.25/v=0) was a margin covering this lag. Ruled out: a static
  frustum deficit (squares need head motion) -- **OVERTURNED 2026-10-10: the centred
  cull is READ (the last entry).** Dated entries at the bottom.
- **Pilot report 2026-10-09 (Crystal Super, Pimax OpenXR):** the squares show
  only on descent into a landable planet, come and go with head movement as the
  gaze switches, and fill in quickly once the head is steady. **Ruled out on this
  rig:** a static frustum deficit (the centred-cull models, H-cam included),
  because a steady head shows no squares. **OVERTURNED 2026-10-10:** the centred
  cull is READ at 0x443E800, and a patch drops only when its whole box lies in the
  uncovered band, which is rare at rest and happens as patches slide through it
  during turns (the last entry). The 447de48f steady-view protocol
  (`cull_probe = measure | cycle | mono`, `--tally cull`) cannot see a lag and
  was NOT flown; its counter, census and mono observers stay available.
- **Round 6, READ** (build 332841; `analysis\decomp\verify_20261009_cull6_*`).
  Elite's head-pose function 0x4E3690 has two sources. *Render thread* (arg6 =
  1): WaitGetPoses' render pose (call 0x4E3715), latched at [W+0x111] until
  Present clears it; EDVR locates it at the frame's predictedDisplayTime, so it
  is the drawn pose. *Game thread* (arg6 = 0, the controller tick, tid 24212):
  IVRSystem::GetDeviceToAbsoluteTrackingPose (`call rbx` at 0x4E387F, return RVA
  0x4E3881), prediction about 0 s, i.e. "now"; with the latch set it reuses the
  cached WaitGetPoses pose instead, by `je 0x4E384F` at 0x4E36EE (0F 84 5B 01 00
  00; reached with arg6 = 0 only). EDVR's runtime (openvr_system.cpp,
  native_runtime_host.h locateHead, head_locator.h) located that call at
  QPC-now + prediction.
- **Round 6, INFERRED (H-lag):** the planet-terrain culler takes the game-thread
  camera, so its tiles are chosen with a pose at least a frame older than the
  drawn one and a head turn's leading edge goes missing. (Census: eye cameras
  are built twice a frame, via wrapper slot 16 and a second thread via slot 26.)
- **Instruments on e256e9bb (temporary):** a `pose gap:` line per caller every
  2 s in the runtime log (`edup_log.py --tally pose`), and `advanced.cull_pose`
  = off | display | next | display_direct | next_direct (`_direct` adds a 2-byte
  latch-bypass hot patch at 0x4E36EE). Build 332841 only.
- **Next:** ship `display` as the behaviour with no key, propose removing the
  old guard's keys and every temporary instrument (quoted, asked first), then
  fly once more on the cleaned build, ideally on the Quest 3 too.
- **Ruled out** (evidence under "Status detail"): the getter as the culler's
  input, the 09-23 "same +19.4% ask" premise, H3, the union model, H2, FUN_13ACC40
  as the consumer. **UNRELIABLE:** the 09-23 verdict "the culler follows
  `GetProjectionRaw`, not the matrix" (a pilot's judgement under periphery
  flicker, a 10-degree trim and a 1.6% difference).
- **Earlier instruments** (rounds 3-5): `advanced.cull_probe` = off | all | camera
  | ui | sky | sizes | other | mono | cycle | measure (lies need `fix.cull_guard`
  off and build 332841): the caller census, the selective lie, the terrain-draw
  counter, the mono camera hooks; their eliminations are in the rounds 3-5 entry.

**Earlier moves out of Status:**

**Ruled out** (moved verbatim from the Status block; do not re-propose):

- The cached-frustum model (H3: culler derives its frustum at eye-target
  build and keeps it until the next rebuild) — refuted 2026-09-23:
  switching to `matrix` (raw channel honest) mid-session brought the
  squares back with **no** target rebuild in between.
- The union model (culler keys on the wider of the two channels;
  widening either suffices) — refuted by the same observation.
- The matrix-channel model (H2) — refuted: `matrix` mode lied wide on
  the matrix and the squares showed regardless.
- The 2026-09-09 legacy-probe `raw`-inert reading — superseded; see the
  2026-09-23 entry (instrument artifact, marked inference).
- The `AstroSurfaceRenderManager::Cull` chain (`FUN_1412772b0` ->
  `FUN_143d097b0` -> `FUN_14444b4a0` -> `FUN_1444d0200`) is a **mono
  horizon-cone LOD culler**, not the view-frustum culler: its 48-plane
  table is built once at construction from planet geometry
  (`FUN_144497d30`), its fov scalar feeds only the LOD screen-size gate
  (`tan(fov/2)` at subobj+0x8E0, written by `FUN_14448e0b0`), and the
  per-object worker `FUN_14444d6c0` is LOD-band selection, not a frustum
  window test. Decompiles in `analysis\decomp\cull_round3*.txt`. Also
  ruled out: `EnableFrustum0Override` / `CullingBias` are shadow-cascade
  config (`FUN_1428555A0`), unrelated to terrain tile culling.
- The fov-getter patch (hook RVA 0x4E2F50, return angle sums widened to the
  per-axis superset, modes off/observe/widen) — WITHDRAWN 2026-10-09: the
  getter is not the culler's input.
- The getter as "the culler's fov source" and "the asymmetry-loss point" —
  refuted 2026-10-09: its outputs are read only by the eye camera setter and
  the UI scale, and its read outputs were identical in the 09-23 windows.
- The 09-23 "raw and matrix windows asked for the same +19.4%" premise —
  refuted 2026-10-09: from 05:16:29 a 10-degree outer trim was on, the ask was
  2554x3032 in both modes, and raw differed from matrix only on the inner
  edge, 1.6% of span.
- The getter's "r+b / l+t" output pairing — wrong: atan|t|+atan|b| vertical,
  atan|l|+atan|r| horizontal, aspect first (2026-10-09).
- FUN_13ACC40 as H-cam's consumer — refuted 2026-10-09 (round 4): it is
  "FSSRenderingComponent".
- The 09-23 verdict "the culler follows `GetProjectionRaw`, not the matrix" —
  demoted to UNRELIABLE 2026-10-09 (not ruled out): a pilot's judgement under
  periphery flicker, with a 10-degree outer trim on and a 1.6% difference.

**Established, moved from Status 2026-10-09:** the game wraps IVRSystem in a
class (vtable VA 0x144E245B8; instance heap-held), loads
`openvr\win64\openvr_api.dll` dynamically (RVA 0x4E4870) and holds
`IVRSystem_012` in global VA 0x145F1A860. The matrix wrapper is `FUN_1404e2d30`
(RVA 0x4E2D30, slot 27): it calls `GetProjectionMatrix` and returns the matrix
with row 3 negated, asymmetry (m02/m12) preserved; the renderer's path
(canted-projection.md, the fold experiment). The LibOVR implementation's
vtable is at VA 0x144E243F0 with parallel slots (slot 25 -> RVA 0x4E2F30).

**Corrections and established, moved from Status 2026-10-09 (round 5):**

- **Corrections** (Elite build 332841, FileVersion 332841 / ProductVersion
  4.4.1.1; the "332753" label was wrong): `FUN_1404e2f50` (RVA 0x4E2F50) returns
  `{aspect = recommended W/H (not tangents), atan|t|+atan|b| (vertical),
  atan|l|+atan|r| (horizontal)}`, not the old "r+b / l+t"; only out[0] and out[1]
  are read, by the eye camera setter 0x2878DC0 and the UI scale at 0x2842AE3
  (k = tan(0.782)/tan(vFOV/2)), so widening the getter resizes the UI.
- **Established** (`analysis\decomp\verify_20261009_cull2_*`): six
  `GetProjectionRaw` call sites in three wrappers (0x4E2FA5 in the getter;
  0x4E3C93 via 0x4E3C50, a quad pass, INFERRED sky; 0x4E42FA, 0x4E4351,
  0x4E43A6, 0x4E43F4 in 0x4E4270, UI sizes); the getter's aspect comes from its
  own GetRecommendedRenderTargetSize call, expected to return at 0x4E2FBE (the
  census confirms). Static analysis found no culler.

---

## The symptom

Over a planet surface — gliding down, flying low, or just turning your head
at altitude — squares of terrain at the edges of view are missing, showing
black, and pop in and out as the view moves. The tracker has confirmations
across Quest 3, Valve Index, Bigscreen Beyond and Pimax Crystal, at every
graphics setting ("even on the lowest"), and the original report notes the
same tiles vanishing slightly too early at the edges of a flat monitor.

Two things make it a VR complaint in practice. A monitor's frustum is
symmetric, so a symmetric-assumption culler is only wrong there by whatever
thin margin the tuning left; a headset's per-eye frustum is not symmetric at
all, and the error grows to several degrees. And the edge of a monitor is
peripheral by definition, while the edge of a headset's view is somewhere
your eyes actually go — especially through pancake lenses, which are sharp
to the very edge. That is why Quest 3 reports dominate the tracker: not the
worst-affected headset, the best-corrected one.

## What is actually happening

Everything below was measured through an `openvr_api.dll` proxy that records
what the game asks the VR runtime, and can answer differently. No game file,
game memory or game code is touched at any point.

**Elite queries the projection continuously.** `GetProjectionRaw`,
`GetProjectionMatrix` (near=1, far=50,000, DirectX convention),
`GetEyeToHeadTransform` and `GetRecommendedRenderTargetSize` are each called
about a dozen times per frame, every frame — roughly 1,080 calls per second
each at 90 Hz. Whatever consumes them re-reads them live.

**The per-eye frustum is strongly asymmetric.** On the Quest 3 rig that
reproduces the bug (via Virtual Desktop), the left eye's tangents are:

| Edge | Tangent | Half-angle |
|---|---|---|
| outer (l) | −1.3764 | 54.0° |
| inner (r) | +0.8391 | 40.0° |
| t | −1.4281 | 55.0° |
| b | +0.9657 | 44.0° |

94.0° × 99.0° per eye, mirrored for the right eye, with the wide side
outward — the normal shape of VR optics.

**A centered frustum of the same extent misses the visible outer edges.** A
culler that keeps the frustum's half-angles but centres them covers about
±47° horizontally where the eye actually sees 54° outward — roughly **7° of
visible-but-culled terrain at each outer edge**, which is precisely where the
tracker's reports put the missing squares.

**The decisive experiment.** Report each axis widened to ± its larger
tangent — the left eye becomes ±54.0° × ±55.0°, a symmetric superset of the
truth — while (a) asking the game for correspondingly larger render targets
so pixel density is unchanged, and (b) handing the runtime only the
true-frustum region of each rendered frame, at the same texture size the
session had always submitted. Nothing the player sees changes except one
thing: **the missing tiles are gone, at every edge**
(`edvr_vr_20260818_125338.log`, build v0.7.5-10-g99aca9c). Turn the guard
off and they return.

That establishes, from outside the binary:

1. The terrain culler's frustum is **derived from the projection the game
   queries**, not from a hardcoded angle — lie to the query and the culler
   follows, live, mid-session.
2. A **symmetrized** frustum is sufficient to cover it on a strongly
   asymmetric eye. This is consistent with — though not strict proof of —
   the culler assuming a centred frustum; the minimal sufficient margin has
   not been walked down yet.

## What it is not

- **Not terrain streaming or generation lag.** The tiles pop in *and out*
  with head rotation. Generation lag fills in once and stays; nothing about
  rotating your head un-generates a tile.
- **Not a settings problem.** Reported identical at minimum settings, and
  the terrain work/quality sliders move tile *detail*, not tile
  *visibility at the edge*.
- **Not headset-specific.** The same class shows on a flat monitor, thinner.
  Which headsets *notice* it is mostly an optics question — edge-sharp
  lenses and no peripheral foveation make it unmissable.

## What a fix inside the game would look like

The renderer and the culler must use the same frustum. The game already
queries the true asymmetric per-eye tangents a dozen times a frame;
wherever terrain tiles are tested for visibility, that test appears to use
a symmetric approximation of them instead. Culling against the union of the
two eyes' true frusta — plus whatever small guard band covers one frame of
head motion at the pose-prediction horizon — removes the artifact at zero
rendering cost, and the same change covers the flat-screen version of the
complaint, whose margin is evidently also thin.

Worth knowing: this was fixed once. Issue 37119 — "culling is too
aggressive, hiding stuff that is visible on the edges of the view" — was
closed as fixed in the Odyssey era, and the class returned in Trailblazers
(4.1). Wherever the frustum choice lives, it is somewhere a rebuild can
quietly regress.

## Reproducing the diagnosis

The artifact itself needs only a planet and a headset. The *diagnosis* — 
that the culler follows the reported projection — needs the one experiment
above: change what `GetProjectionRaw`/`GetProjectionMatrix` return, keep
what the player sees constant, and watch the tile set follow the report.
EDVR's `observe_projection` mode (on by default) logs the tangents any rig
actually reports, which is also how the numbers in this document were
collected on two headsets in one afternoon.

---

## What EDVR does now

The culler's input was a head pose Elite asks for on its game thread, 41 to 44
ms older than the pose the frame is drawn with. The runtime now answers that
one request one display period after the drawn frame's display time, the instant
the next frame is drawn at (flights 4 and 5, below). That is the whole of the
lag fix: no pixels are added, nothing is cropped, and there is no key. A second fix
ships beside it, for a different defect (the cull cover, below); it writes one
function of the game, and it has no key either.

**Measured (flight 3, 2026-10-09, Crystal Super via Pimax OpenXR, build
e256e9bb, `edvr_openxr_20261009_144149_193_26440.log`, `edup_log.py --tally
pose`).** Under `off` the call returning to exe+0x4E3881 was located
**-41 to -44 ms** from the drawn frame's display time, its pose turned **0.9
deg mean and up to 4.1 deg** from the drawn pose, and that angle correlated
**r = +0.98** with head speed. Under `display` the gap is 0 by construction and
the angle 0; `display`, `next` and `display_direct` all stopped the squares on
gaze switches. (History: `display` shipped first; flights 4 and 5 moved the
shipped behaviour to `next`.)

**Which calls.** The filter (src/openxr/head_pose_time.h) is a return address
inside the game's mapped image and a prediction under 5 ms either way
(exclusive, both signs). It names no build, so a game update does not turn it
off; if an update moves the caller, the first-sight line names the new return
address. The instant is the latest frame's predictedDisplayTime PLUS that
frame's predictedDisplayPeriod, taken at the WaitGetPoses publish point; with no
frame yet, a display time that is not positive, a stale frame, a frame that
reports no period, or a reference-space change between the two, the call falls
back to now + prediction and is counted in `fallback`. A call that does not reach
the owner thread (no live session, a bad origin) is counted in `failed`.

**Why this and not the guard.** The guard (below, kept as history) covered the
missing tiles with a margin: it told the game a wider frustum, cost about 6% GPU
at h=0.25/v=0 and cropped the extra away. The gaze-switch squares were a lag, and
fixing the lag at its source costs nothing. The flashes that remained on headsets
whose eyes are not symmetric were the centred cull (the next paragraph, the last
entry).

**The cull cover (2026-10-10, flown; the last entry).** Elite builds the four planes
it culls terrain against from the eye camera's aspect and vFOV in one function,
0x443E800 (build 332841), a CENTRED frustum with no margin, while each eye is drawn
with its own asymmetric projection; on a Quest 3 or a cropped Crystal Super the
image reaches 5 to 6 degrees past the cull on the outer side (and down). EDVR hooks
that function and widens its two float arguments to the largest tangents of the
frusta the game is handed, over both eyes. No render size changes, nothing is
cropped, and the cost is not measurable (19 to 25 calls a frame of arithmetic). VR
only; another game build is searched for the call site and left alone unless there
is exactly one match. `python tools\edup_log.py --target <store> --tally cover`
reads the `terrain cull cover:` lines.

**Reading a log.** `python tools\edup_log.py --target <store> --tally pose`
tables the `pose gap:` lines by (thread, return address). Elite's game-thread
caller should read a gap of about one display period (+11 ms at 90 Hz) and an
angle to the drawn pose of about head speed times that period (r near +1: it is
one period ahead by design); a caller that passes a real prediction, or is
outside the image, shows its true lag.

## What EDVR does about it — the cull guard

> **Historical. The terrain guard was removed 2026-10-09** (Sean's decision after
> flight 3); none of its keys exist, and an old line in an `edvr.ini` is carried
> by the installer as "no longer used by this version" and does nothing. This
> section and "Reading the log" below describe the code as it was. The FOV trim
> that rode the guard's stage machine still exists, as `native_fov_trim.h`.

`cull_guard = symmetric` under `[fix]` in `edvr.ini`, **off by default**,
and it must be set before launch (turning it *off*, or changing mode or
margin, is live; turning it *on* installs a hook that only installs at
startup).

The guard tells the game a wider frustum than the headset shows, in one of
two modes — `symmetric` (each axis to ± its larger tangent: the exact fit if
the culler centres the frustum) or `percent` (every tangent widened by
`cull_guard_percent`: a plain margin, for finding the smallest number that
keeps the edges clean). It then:

1. **Asks the game for larger render targets** in the same proportion, so
   the wider frustum is rendered in *new* pixels and nothing gets softer.
   Elite adopts the changed recommendation live — measured at about 14
   seconds to rebuild its eye targets mid-session, no restart.
2. **Starts the projection lie only after both eyes submit at the new
   size**, at a frame boundary, so every answer within a frame — raw
   tangents, the projection matrix, and the submitted image at the end —
   tells one story. "New" is measured against the sizes the game was
   submitting when stage 1 began, seeded from the first submissions after
   that boundary (2026-09-02: with Elite's HMD Quality at 1.25 the
   un-rebuilt 1.25x targets already cleared the size threshold, and the
   guard adopted a canonical the game abandoned two seconds later).
3. **Hands the runtime only the true-frustum region** of each frame, copied
   into an EDVR-owned texture at exactly the size the session had always
   submitted.

The cost is GPU load, not sharpness: full symmetric mode on the Quest 3
frustum above renders about 48% more pixels. Two knobs cut it.
`cull_guard_fraction_h` / `_v` cover only part of each axis's shortfall —
live-tunable, so the staircase described in `edvr.ini` finds the cheapest
value that still keeps the edges clean, and the log's `cull guard margins`
line names what each step leaves uncovered. On the Quest 3 frustum the
staircase settled at **`fraction_h = 0.25`, `fraction_v = 0` — about 6%
extra pixels** — with the edges still clean: the vertical margin proved
entirely unnecessary (matching the tracker's sides-only reports), and the
outermost ~4.4° horizontal either genuinely needs no cover or is not
resolvable through the lens edge. And `cull_guard_headsets` gates the whole
guard to listed FOV signatures — the log prints each headset's, like
`94x99` — so a rig that swaps headsets pays only on the headset its owner
listed, with no config edits at swap time; unlisted headsets run
observation only, at no cost.

One suspected side effect, measured and cleared the same day it was
instrumented — recorded here because the first reading blamed the guard.
An early tuning flight showed the transition-flash fix's recognition
counters climbing with the margin (29 at guard-off against 3,277 at
`fraction_h = 0.5`, with withheld frames felt as judder), and the obvious
reading was that the wider frustum admits more near-surface render passes
for the detector to learn. So the detector was taught to attribute before
anything was changed: it stamps every camera-history frame with the
margin that was live when it was drawn, splits its running totals under
the guard, and prints its learned tables when the history is dumped. The
staircase then re-flew with per-frame attribution, including a guard-OFF
control window mid-flight, and the correlation inverted: the guard-off
stretches carried the *highest* recognition rates of the day, and the
camera populations doing the churning appear identically with the guard
off. The churn belongs to low flight over terrain, not to the margin —
the earlier numbers had confounded margin with flight profile. No
margin-aware detection is needed: the flash fix certifies the recurring
geometry within a few frames and excuses it from then on, its burst
governor bounds any storm, and a withheld frame is resubmitted on time
rather than stalled. The attribution stays in the build, so any future
"is this the guard?" report answers itself from one history dump.

### Why the submit side is this elaborate

Because the two obvious designs are both refuted, in the field, on the same
day — recorded here so neither is rediscovered:

- **Narrowing the submitted texture bounds** (`VRTextureBounds_t`) is
  correct by the OpenVR contract and free. OpenComposite over VDXR ignored
  the narrowed bounds and displayed the full wide-rendered image against the
  true-FOV mapping — experienced as the whole world distorting with every
  head turn. It survives as `advanced.cull_guard_submit = bounds` for
  measuring whether real SteamVR honours it; `copy` is the default.
- **Submitting the cropped region at its natural size** was clean by every
  contract read — and sheared the image into a parallelogram under head
  rotation, because the session's first-ever submission was then a texture
  whose aspect the transport had never served. The lesson, twice paid:
  **that transport stack reliably serves only submission shapes the session
  has already established.** Hence the two-stage go-live and the crop
  snapped to the canonical size — the runtime never sees a shape change at
  all.

One more recorded assumption that cost a flight: texture v runs from the
**positive** vertical tangent (v=0 is the *b* edge — derived from the
runtime's own matrix, `(1+m12)/m11 = b` at NDC y=+1, D3D11's top row).
The first build measured the vertical crop from *t*, kept the wrong end of
every column, and turned forward leans into a vertical stretch. The test
fixture's vertical is asymmetric now, so a flipped axis cannot pass a build.

### Guard rails

The guard edits `GetProjectionMatrix` only after checking, per eye, that
the runtime's matrix actually matches the tangent formula the edit assumes;
a runtime that builds its matrix differently makes the whole guard **inert,
loudly**, and everything forwards the truth. Any submit-side failure stands
the guard down at the next frame boundary — one plainly-shown wide frame at
worst, never a mismatched crop. Foreign consumers of the hooked interface
always receive the truth; the lie is for the game alone.

### Reading the log

A working session says, in order: `cull guard stage 1` (targets asked
bigger), then two `cull guard LIVE` lines naming the true and reported
tangents, the crop, and `submissions stay at the session's own size`. The
exit totals count crops by mechanism. `cull guard INERT` or
`STANDING DOWN` means the guard refused to run on this rig's runtime —
everything still renders normally, and the log's own line says exactly why.

**If terrain squares are still visible with the guard live:** switch
`cull_guard` to `percent` and raise `cull_guard_percent` (both live — save
the file, no restart). If a 20% margin does not move the artifact at all,
what you are seeing is probably not this bug — send `edvr_logs\` either
way, because the LIVE lines plus the tangent lines are what distinguish
"guard not engaged", "margin too small", and "different bug entirely".

### Known limits, stated plainly

The guard has been field-verified on both of this project's rigs, which
are both OpenComposite paths: Quest 3 over Virtual Desktop (where the
missing tiles reproduced, and are gone) and Pimax over PiOpenXR (where the
guard runs clean and invisible — horizontal-only there, about 19% extra
pixels, since that frustum's vertical is already symmetric; that rig never
showed the tiles, so it verifies the machinery rather than the cure). Real
SteamVR has not been measured at all — the observation half ran there long
before the guard existed, but the guard itself has not. And the game's
culler is being *covered*, not fixed: the tiles were always renderable,
and the correct fix is one line of frustum arithmetic away from whoever
owns the culler.

---

## 2026-09-20 — the projection chokepoint, and a proposed memory patch

Static analysis against build 332753 (Ghidra project `analysis\ghidra`,
scripts `analysis\ghidra_scripts\CullProjection*.java`, decompiles
`analysis\decomp\cull_projection*.txt`) followed the projection query from
the OpenVR import to the point where per-eye asymmetry is discarded.

**The chokepoint.** The game loads `openvr\win64\openvr_api.dll`
dynamically, stores `IVRSystem_012` in a global (VA 0x145F1A860), and wraps
it in a class (concrete vtable VA 0x144E245B8, heap instance). Every
projection query in the game flows through two wrapper methods — they are
the only call sites of `GetProjectionRaw` (vtable +0x10) and
`GetProjectionMatrix` (+0x8) in the binary:

- **Fov getter** `FUN_1404e2f50` (RVA 0x4E2F50, wrapper slot 25). Per eye
  it queries the raw tangents and the render-target size, then returns
  three floats: `aspect = resX/resY`, `atan(|right|)+atan(|bottom|)`,
  `atan(|left|)+atan(|top|)` (helper at RVA 0x48B54DC is the CRT `atanf`;
  abs masks via `ANDPS 0x7fffffff`; full asm verified in
  cull_projection9.txt). Angle sums of magnitudes — **the per-side
  asymmetry dies here**. Whatever consumes this triple can only build a
  symmetric frustum.
- **Matrix getter** `FUN_1404e2d30` (RVA 0x4E2D30, wrapper slot 27).
  Returns the projection matrix with row 3 negated; m02/m12 offsets
  intact. The renderer's path — asymmetry survives.

This bifurcation is the whole bug shape: the renderer draws the true
asymmetric frustum, while any culler fed by the fov getter culls a
centered one of the same total extent — on the Quest 3 rig, ±47° against
an eye that sees 54° outward. It also explains why the cull guard works:
it lies at the OpenVR query, upstream of both paths, and pays for the lie
with extra rendered pixels because the *renderer* also believes it.

**What the flown probes add (canted-projection.md, 2026-09-09).** The
separability probe lied through one projection call at a time: `raw`
alone did not move the terrain quads, `matrix` alone widened the picture
but not the quads, and only `both` — which additionally enlarged the
render targets and so forced an eye-target rebuild — covered them. Read
against the chokepoint above, this says the terrain culler does **not**
consume either getter live, per frame. The working model is a **cached
cull frustum**: computed once per eye-target build from one of the two
channels (which one is unflown — the probe's missing cell is one channel
lied-to plus a forced rebuild), symmetric by construction if it derives
from the fov getter's angle sums, and combined per frame with the live
view matrix — which is why tiles still pop with head rotation under a
cached frustum shape. It also explains the guard's own mechanics: the
guard only moves the quads because its stage 1 forces the rebuild that
recomputes the cache.

**Proposed patch: replace the fov getter, leave the matrix getter alone.**
Inline-hook `FUN_1404e2f50` via the existing `code_hook` relay machinery
(same pattern as `kinematic_eval_hook.cpp`: RVA + prologue-bytes check +
PE timestamp gate, inert on any other build). The hook reimplements the
getter — it is 60 instructions — calling the wrapper's own IVRSystem
pointer (`this+8`) for tangents and render size, then computes the
outputs from tangents widened to the per-axis symmetric superset,
`m_h = max(|l|,|r|)`, `m_v = max(|t|,|b|)`: both angle sums become
`atan(m_h)+atan(m_v)`. Cullers fed by this getter then cover the true
per-eye frusta; the renderer, on the matrix getter, never sees the
change. Cost: zero extra pixels, zero crop machinery — the cull guard's
entire stage 1-3 apparatus becomes unnecessary for rigs where this holds.
Under H3 the hook must install **at startup**, so the game's initial
eye-target build already sees widened values, and mid-session margin
changes take effect only at the next target rebuild (a quality toggle),
not on ini save.

Modes (config names functionality): `off` (default until flown),
`observe` (true values out, but log a census of call-site return addresses
and per-eye values — names every consumer of the triple in one session),
`widen` (symmetric-superset values out, plus the census). Mutually
exclusive with `fix.cull_guard` at startup, loudly.

**Verification, one flight:** guard OFF, hook in `widen` from launch, near
a planet surface. Tiles clean from session start = the cache derives from
the fov getter and the patch stands (H1-as-cached confirmed). Tiles still
dropping after a forced target rebuild (toggle HMD quality to force it)
= the cache derives from the matrix channel (H2-as-cached), and the
census on both getters — call sites, rates, values — names the consumers
for the next static round. **Watch item:** the AstroSurface LOD gate's
scalar fov (renderer field +0x2C, read in `FUN_141277370`) may be fed
from this getter; widening shifts terrain subdivision thresholds
slightly. Compare LOD/pop behaviour and the `cull guard margins` numbers
against a guard-off baseline in the same flight.

**Why not patch the binary on disk:** EDVR's whole value is per-build
gating and inert-by-default failure; a runtime hook with a prologue check
keeps that. The RVA is build-specific (332753); other builds get the
guard, as today.

**The probe's missing cell is now flyable.** The legacy proxy's
`advanced.cull_guard_channel` (976b4f2, removed with the proxy in 1a54e9e)
is reimplemented in the native OpenXR runtime, with one deliberate
difference from the flown probe: the split channel no longer shrinks the
pipeline. The grown size ask, the adoption and the crop run unchanged, so
the eye-target rebuild that recomputes the cull cache still happens; only
the lie is split. `both` (the default) is the guard exactly as flown;
`raw` tells the widened frustum to GetProjectionRaw alone (the matrix
channel answers the content frustum, and the crop is the identity);
`matrix` tells it to GetProjectionMatrix alone (the raw channel answers
the content frustum). Flight protocol: guard on, `channel = raw` for one
session, `= matrix` for another, parked over terrain, watching the edges.
Tiles covered under `raw` means the cache derives from the fov getter;
covered only under `matrix` means it derives from the matrix getter — and
the patch above hooks the wrong place.

---

## 2026-09-23 — the channel flight: it is the tangents, live

Flown on the Pimax Crystal Super (Pimax OpenXR; per-eye frustum
`-1.5293/+1.0324` x `±1.2648`, render 3070x3032) on build
v0.17.0-382-g8ce12926, two sessions. **The Crystal Super reproduces the
bug** — the first Pimax-family rig here that does; the older "this rig
never showed the tiles" note below belongs to the Crystal over PiOpenXR.

Session 051523: `channel = raw`, guard on, from launch — Live at 05:15:58
(+19.4% horizontal), edges clean (trim 0 until 05:16:29; the 10-degree
outer trim afterwards was the pilot's own menu edit). Switched to
`channel = matrix` at 05:21:40 — Live immediately at the same size ask,
so **no target rebuild happened**, and **the black squares came back**.
Session 053407 repeated it with a guard-off baseline: matrix, off,
matrix, raw — squares whenever the guard was off or in `matrix`.

The matrix-period squares with no intervening rebuild decide the model:

- **The culler follows `GetProjectionRaw`, live.** With `raw` the only
  channel lying, the edges were clean; the moment the raw channel went
  honest (matrix mode), the squares returned — at the same target sizes,
  so no re-derivation event can explain it. The cached-frustum model
  (this doc's H3) is refuted, and so is any "wider of the two channels"
  union model.
- **The matrix channel feeds nothing the culler reads.** `matrix` mode
  lied wide on the matrix and the squares showed anyway.

This re-reads the 2026-09-09 legacy-probe `raw`-inert result
(canted-projection.md): that row said lying through `GetProjectionRaw`
alone changed nothing. *(Inference, marked as such: the legacy proxy's
matrix-channel edit was proven live — the fold experiment moved the
rendered picture through it — but its raw-channel edit was never proven
to reach the game at all. Today's flight, on an instrument whose raw
edit demonstrably lands, says the culler does follow that channel. The
09-09 `raw` row should not be cited as evidence again.)*

**What this settles for the patch.** The culler's fov source is the
tangents channel, consumed live — and the game's only symmetric
projection source on that channel is the fov getter `FUN_1404e2f50`
(RVA 0x4E2F50), which folds the four tangents into angle sums of their
magnitudes. Hooking it to emit the per-axis symmetric superset covers
the true per-eye frusta while the renderer (matrix channel) is never
touched: zero extra pixels, no target growth, no crop, and — because
the following is live — immediate effect with live tuning. That is the
build described in the Status block's Next bullet, and it makes the
whole render-wider-and-crop guard unnecessary on rigs where it flies
clean.

One caution carried forward: the only other known consumer of the
getter's outputs is fov-priced LOD (the AstroSurface screen-size gate);
widening shifts subdivision thresholds slightly. Compare LOD/pop against
a guard-off baseline on the patch's first flight.

## 2026-10-09 — the fov getter is not the culler's input

Offline disassembly of Elite build 332841 (FileVersion 332841 / ProductVersion
4.4.1.1; the "332753" label in the entries above is wrong); dumps in
`analysis\decomp\verify_20261009_cull2_*`. **READ** is what the instructions
and the 09-23 logs say; **INFERRED** is what follows from them.

**READ**

- 0x4E2F50 (wrapper slot 25) calls `GetProjectionRaw` at 0x4E2FA2
  (`call [rax+0x10]`, return RVA 0x4E2FA5) and returns `{aspect = recommended
  W/H, atan|t| + atan|b|, atan|l| + atan|r|}`. The first number is an aspect,
  not a tangent, and the vertical sum pairs t with b and the horizontal sum l
  with r; the "r+b / l+t" pairing above was wrong.
- Only out[0] and out[1] are read: by the eye camera setter 0x2878DC0, and by
  the UI scale at 0x2842AE3, k = tan(0.782) / tan(vFOV/2). Widening the getter
  would therefore resize the UI.
- Six `GetProjectionRaw` call sites in three wrappers: slot 25 (0x4E2F50) at
  0x4E2FA5; slot 28 (0x4E29B0) through its helper 0x4E3C50 at 0x4E3C93; slot 24
  (0x4E4270) at 0x4E42FA, 0x4E4351, 0x4E43A6 and 0x4E43F4, which return sizes
  for the UI.
- The slot 28 helper returns fx = W/(|l|+|r|) and fy. They are read by
  0x28219D0 -> 0x2860FA0, a full-screen quad pass.
- From the 09-23 logs, re-read: the getter's read outputs were identical in the
  raw window and the matrix window, so the getter cannot be what changed
  between the squares being absent and present. And the windows did not ask the
  same +19.4%: from 05:16:29 a 10-degree outer trim was on, the ask was
  2554x3032 in both modes, and raw differed from matrix only on the inner edge,
  1.6% of span.

**INFERRED**

- The quad pass behind slot 28 is sky or background.
- The only model that fits the 09-23 windows is a frustum centred on the eye
  axis with half-width (|l|+|r|)/2 taken from Raw, built by a caller not yet
  identified. Static analysis found no culler.

**Ruled out:** the getter as the culler's input, and with it the withdrawn
patch; the "same +19.4% ask" premise. Nothing else is newly ruled out.

**What the test build does.** The runtime sees who calls, because EDVR
implements `GetProjectionRaw` itself and the game's `call [rax+0x10]` lands in
it directly.

- A census, always on, records the game's return address (frame 1, as an RVA in
  the game's image, or "outside") for `GetProjectionRaw`, `GetProjectionMatrix`
  and `GetEyeToHeadTransform`. Frames 2 and 3 come from a stack capture taken on
  the first sight of a new frame 1, on every 16th call of each frame 1 (so a
  caller that turns up later behind a known frame 1 is named within moments;
  calls not captured are counted as unsampled), and on every call of the first
  site while the probe needs it. The log has a `projection callers:` line on first sight
  of each distinct (method, frame 1, frame 2), and count summaries at 30 s,
  every 5 minutes and before every probe change.
- `advanced.cull_probe` answers the selected callers of `GetProjectionRaw` with
  the per-axis symmetric superset of what they would have been told, jitter
  shift included. Groups: camera is frame 1 0x4E2FA5 with frame 2 0x2878E1B; ui
  is 0x4E2FA5 with frame 2 0x8D269A; sky is 0x4E3C93; sizes is 0x4E42FA,
  0x4E4351, 0x4E43A6 or 0x4E43F4; other is anything not matched, including
  0x4E2FA5 with any other frame 2; all is every caller. It acts only with the
  cull guard off and only on build 332841 (PE stamp 1788384820, image
  104894464), and the log says when it does not.
- The getter's other half (READ, 2026-10-09, new disassembly): the eye camera
  takes its HORIZONTAL extent from out[0], the aspect, which the getter gets
  from its own GetRecommendedRenderTargetSize call (`call qword ptr [rax]` at
  0x4E2FBC, two bytes, so the return is expected at 0x4E2FBE; the census shows
  the real value) and only its VERTICAL extent from `GetProjectionRaw` (out[1]).
  So on a vertically symmetric headset a `GetProjectionRaw` lie cannot widen a
  centred (aspect, vFOV) frustum at all, and a probe that touched only
  `GetProjectionRaw` would read "no group matters" whatever the culler does.
  The probe therefore also answers that one call: under a selected group (all,
  camera, ui, other; the same frame-2 table as the getter's raw site) it is told
  the height kept and the width height * A to the nearest even number, with A
  the larger over the eyes of max(|l|,|r|) / max(|t|,|b|) from the true located
  tangents (before any lie or jitter), which makes the camera's centred frustum
  the per-axis symmetric superset. Every other asker, the render-target
  allocation first, is told what it was, bit for bit. The log says it once per
  change: `cull probe: <group> also told aspect A' (true A) at the fov getter`.
- H-cam (INFERRED; READ in outline 2026-10-10, consumer 0x443E800 and not FUN_13ACC40):
  the culler is a centred frustum built from the camera's
  (aspect, vFOV) fields. It fits the old guard flying with v=0: that guard's
  horizontal widening reached the game as a bigger render size, i.e. aspect.
  FUN_13ACC40, which builds a double-precision view-projection from those
  fields, is the candidate consumer (struck later the same day, round 4: it is
  "FSSRenderingComponent"; see the rounds 3-5 entry below).
- `advanced.cull_probe = cycle` drives the groups itself so the result is a
  number and not an impression (the black squares flicker as the head moves, so
  20 s windows cannot be judged by eye): off, all, off, camera, off, ui, off,
  sky, off, sizes, off, other, in 2.0 s windows, the first 30 frames of each
  dropped, counting the planet-terrain draws each eye gets (the colour pass's
  patch VS, 72BDD292154158AD, the draws the planet patch motion already keys
  on) and their summed index counts, since tile LOD varies. A culler whose
  frustum widens admits more tiles at the edges, so the group that feeds it
  should show more draws and indices than its neighbouring off windows.
  `edup_log.py --tally cull` tables the windows and the paired differences,
  leaving out windows in which the head moved faster than 20 deg/s.
- `advanced.cull_probe = measure` is the positive control. The old guard
  (`fix.cull_guard = symmetric`) is proven to remove the squares, and a counter
  that cannot see its terrain draws rise when the guard goes live is blind: its
  silence under the lie probe would mean nothing. Measure is the same windows
  and counting with no lie, each window labelled by the guard's stage as the
  runtime last told this half (off when not configured, otherwise waiting,
  adopting, live; the channel does not separate waiting from inert), and a
  window dropped when the stage changes under it. It keeps counting while the
  guard runs: only the lying stands down. `--tally cull` prints live minus off
  (the difference of the stage means, with the standard error of the
  difference), leaving out the staging windows and those the head moved in.
  Lines: `cull cycle: measure[guard live] window N: frames F, terrain draws L/R
  mean a/b, indices L/R mean c/d, head x.x deg/s`.
- The guard arms live (READ, native_cull_guard.h `beginFrame`): any change of
  its settings, off to symmetric included, resets it to Off, and from Off it goes
  to Adopting as soon as the scene is ready (it asks the game for bigger render
  targets while still telling the truth), then to Live once both eyes submit at
  the new size; terrain-culling.md measured the game's rebuild at about 14 s
  mid-session. So the positive control needs no relaunch, and an empty
  `cull_guard_headsets` runs it everywhere. Not yet flown on the native runtime:
  if the log has not said live 60 s after the switch, relaunch with the guard on.
- The same flight carries the canted-display test keys (canted-projection.md).

## 2026-10-09, rounds 3-5 — static eliminations, and the mono camera

Offline disassembly of Elite build 332841 again (dumps in `analysis\decomp\`),
then a test build. Nothing here is flown. **READ** is what the instructions say;
**INFERRED** is what follows.

**READ**

- The controller tick 0x107346A (the census's off-thread caller, 0x1073470)
  reads only out[1] of `GetProjectionRaw`, into controller+0x30.
- Eye cameras build their frustum from the kind-5 matrix; 4F3770 inverts
  +0x1B0. It is exact, not a centred (aspect, vFOV) frustum.
- FUN_13ACC40, the double-precision view-projection builder named as H-cam's
  candidate consumer, is "FSSRenderingComponent".
- A mono camera exists, built at 2871D30 (kind 0/3). Its aspect field is B+0x80,
  written only by FUN_28634E0 (rcx = B, edx = width, r8d = height, xmm3 = the
  minimum aspect, a float* out at [rsp+0x28] at entry, written conditionally),
  and defaults to 16/9. The getter at 0x2841190 reads a camera's aspect
  (`F3 0F 10 41 70 C3`, `movss xmm0,[rcx+70h]`, `ret`); the mono filler calls it at
  0x2871D86 (`call qword ptr [rax+0x40]`), which returns to 0x2871D89 and stores
  the result at 0x2871D92. No terrain reader of the mono camera was found.
- The AstroSurface chain uses camera 0's pose, a fov setting in degrees and the
  viewport height (a LOD gate), as the earlier entry said.
**Demoted, not ruled out:** the 09-23 verdict that the culler follows
`GetProjectionRaw` and not the matrix. It was a pilot's judgement of flickering
squares at the periphery, with a 10-degree outer trim on and a 1.6% difference
between the windows (the 10-09 entry above). UNRELIABLE: nothing is built on it.

**INFERRED**

- The probe's eye groups (all, camera, ui, and the aspect lie with them) cannot
  be expected to move an eye camera's frustum, which does not use those values.
  If H-cam holds, the carrier is another camera, and the mono camera is the one
  that exists. **2026-10-10:** true of the RENDERED frustum; the cull's centred frustum
  is built in 0x443E800 from the eye camera's own aspect and vFOV ([r14+0x70],
  [r14+0x90] at the call site 0x4455FDD), which the probe groups did not reach.

**What the test build does.** `advanced.cull_probe = mono` and the cycle's last
window multiply the mono camera's aspect by 1.30. `all` keeps its meaning
(`GetProjectionRaw` and the aspect lie only).

- The hook is on the getter at 0x2841190. It goes in lazily, on the first frame
  `cull_probe` is cycle, measure or mono, once, through a gate: PE stamp
  1788384820, image size 104894464 and the bytes of both functions. On any
  mismatch nothing is patched and one line says why: `cull probe: mono windows
  multiply the mono camera's aspect by 1.30 (hook live | hook inert: <why>)`.
- The detour calls the original and returns its value untouched, except when the
  game's return address is 0x2871D89 AND a mono window is active: then
  aspect * 1.30. It reads the return address because the relay jumps into the
  detour. Every other caller, and every call while no window is active or the
  key is off, gets the original bit for bit (with the key off the relay does not
  enter the detour at all).
- Two observe-only records, written at the frame boundary: the first sight of
  each distinct return address that calls the getter (16 at most; the line
  names the mono filler), and one line per call of FUN_28634E0 (50 at most:
  width, height, the minimum aspect, `*out` before and after, the return
  address). The writer's hook calls the original with its arguments and returns
  its result.
- The cycle is now 14 windows: off, all, off, camera, off, ui, off, sky, off,
  sizes, off, other, off, mono. `mono` is paired with the next cycle's first off
  window like any group. With the hook live every window line ends `, mono reads
  N`, the calls from 0x2871D89 over its counted frames; `--tally cull` tables
  it by group and prints the hooks' own lines. A steady `mono` run writes no
  windows, only the hooks' lines.

**Reading it.** A mono window whose terrain draws and indices rise against its
two off neighbours names the mono camera's frustum as the culler's input. A
mono window with `mono reads` above zero and no rise retires the aspect read
through that one call, not the mono camera: the observe lines list every other
caller of the getter and every call of the writer, which is where to look next.
A mono window with `mono reads` 0 says nothing (the filler was not reached in
that scene). The positive control (`measure`, flight A) still has to show live
above off first.

## 2026-10-09, round 6 — the squares follow the head: two poses, and the test build

Offline disassembly of Elite build 332841 (`analysis\decomp\verify_20261009_cull6_*`), then a test build. Nothing here is flown. **READ** is what
the instructions say; **INFERRED** is what follows from them.

**READ**

- The function at 0x4E3690 hands Elite a head pose. Its entry sets `[r9] = 0`, loads the IVRCompositor (`[rcx+0x108]`) and, at 0x4E36DE, tests
  arg6 (`[rbp+0xE0]`): non-zero jumps to 0x4E36F4. For arg6 = 0 it tests the latch `[rsi+0x111]` at 0x4E36E7 and, when it is clear, branches at
  0x4E36EE to 0x4E384F.
- 0x4E36F4 tests the latch again: set goes to 0x4E37E8 (the cached pose); clear falls through to `call [rax+0x10]` at 0x4E3715, WaitGetPoses.
  So the render thread (arg6 = 1) always takes WaitGetPoses' pose, and the game thread (arg6 = 0) takes it only while the latch is set.
- 0x4E384F loads the IVRSystem (`[rsi+8]`), reads its vtable slot 0x50 (GetDeviceToAbsoluteTrackingPose, slot 10) into rbx, forms the prediction
  as arg3 (a double) less the result of a call through `[rax+0x50]` at 0x4E3860, narrows it to float (`cvtsd2ss xmm2` at 0x4E387B) and calls
  at 0x4E387F (`FF D3`). The return address is 0x4E3881. The prediction is about 0 s in practice, so the pose is "now". A missing compositor
  (`test rcx,rcx` at 0x4E36D5) also goes to 0x4E384F.
- The branch at 0x4E36EE is `0F 84 5B 01 00 00`: `je` with displacement 0x15B, ending at 0x4E36F4, so its target is 0x4E384F. The bytes at
  0x4E36E8..0x4E36F7 are `BE 11 01 00 00 00 0F 84 5B 01 00 00 80 BE 11 01` (the tail of the `cmp` at 0x4E36E7, the branch, the start of the `cmp` at
  0x4E36F4). Both opcode bytes are in the aligned word 0x4E36E8..0x4E36EF.
- The caller that asks with arg6 = 0 is the controller tick on tid 24212, which reaches 0x4E3690 through V+0x60 (round 6 trace).

**The patch.** Replace `0F 84` with `90 E9`. The displacement that follows is untouched, so the result reads `nop` at 0x4E36EE and `jmp rel32` at
0x4E36EF, ending at 0x4E36F4 with the same 0x15B: the same target, 0x4E384F. The branch becomes unconditional, so an arg6 = 0 caller never reaches
0x4E36F4's latch test and never takes the cached pose; arg6 != 0 callers jump over this code at 0x4E36E5 and are unaffected.

**INFERRED**

- The planet-terrain culler takes the game-thread camera (H-lag): its tiles are chosen with a pose at least one frame older than the drawn one.
  The pilot's pattern (squares only while the head moves, filled in when it is steady) is what a lag of that size would do and a static frustum
  would not.

**What the test build does.**

- *Instrument, always on.* GetDeviceToAbsoluteTrackingPose records, before the hop to the owner thread, the caller's thread, its return address as an
  RVA in the game's image and the prediction. After the locate it takes the located instant less the latest frame's predictedDisplayTime (ms), the angle
  between the pose handed back and the pose that frame was drawn with, and the head's angular speed from the render pose. Per (thread, return RVA)
  and 2.0 s window, flushed from the WaitGetPoses publish point (a mode change closes the window at once):
  `pose gap: mode <m> tid T calls n from exe+0x… prediction p ms target-minus-display mean a ms (min b max c) angle-to-drawn mean d max e deg head f deg/s
  waitgetposes w failed k`, with ` fallback n` appended when a call could not be formed. Failed locates, including the silent busy-gate path, are in
  `failed`. `from` reads `outside` or `?` for a caller that is not in the image or was not captured.
- *Switch.* `advanced.cull_pose` is read by the graphics half and carried to the runtime in `EdvrNativeFrameOutput::cullPose` (version 8, under the
  same hand-copied-DLLs rule as the others: an older runtime reads off). The runtime acts on it: the call returning to 0x4E3881, on build 332841, is
  located at the latest frame's display time (`display`, `display_direct`) or one period later (`next`, `next_direct`) instead of now + prediction;
  every other caller is unchanged, bit for bit. A Display or Next instant that cannot be formed (no frame yet, no positive period) falls back to
  today's answer and is counted.
- *Patch.* The `_direct` modes make the graphics half write the 2-byte patch above as one interlocked eight-byte store over 0x4E36E8..0x4E36EF, gated
  on the PE stamp, the image size and the six original bytes, from its frame boundary (the owner thread inside WaitGetPoses, which every pose call
  hops to as well, so it is never inside one), and put the original bytes back when the mode leaves `_direct`. A refusal is final until the mode leaves
  `_direct`.
- *The line a change writes* (graphics log): `cull pose: <mode> -- Elite's game-thread head pose is located at <now+prediction | the frame's display
  time | display time + one period>; latched-pose bypass <on|off|refused: why>`. On another build a time mode adds `(standing down: not build 332841)`
  after the instant it falls back to.

**Reading it.** Under `off` the game thread's gap should be near the time since the last frame began (several ms) and its angle should grow with
head speed (r near +1 in `--tally pose`); display should read 0 ms and next one period. If squares go with a smaller angle, the culler is on that
pose; if the squares do not change under any mode, it is not (and the instrument has still said how stale the pose was).

## 2026-10-09, flights 4 and 5 — `next`, and no write to the game

**Flights.** Flight 4 and flight 5 are the same test build, e3692ed4 (`advanced.cull_pose` with `display`, `next`, `display_direct`, `next_direct`; on branch
claude/cull-pose-next-frame, 81e26374), on Steam, Crystal Super, on approach to a planet with RAPID head turns, flown by the pilot by eye. The rc flight
(v0.18.3-164-gf9597bd3) is main's `display`.

| mode | rapid turns on approach | DSS scanner UI |
|---|---|---|
| `display` (main, rc flight) | squares at the outer edges | not reported |
| `next_direct` (flight 4) | squares | followed his head |
| `display_direct` (flights 4, 5) | clean in flight 4; a black flash on a rapid turn in flight 5 | did not follow in flight 4 |
| `next`, no bypass (flight 5) | no flash on a repeat test | stays put |

**The model.** Elite's game thread calls GetDeviceToAbsoluteTrackingPose directly BETWEEN frames, for work that feeds the NEXT frame, so it needs D_N + period.
While a frame renders, it reads its own latched WaitGetPoses pose of that frame, which is already right for what it computes then (the scanner UI included).
So `next` gives each path the right pose. `display` answers the direct calls a period short (the rc flight's residual squares). The bypass is harmful: it forces
the latched readers onto the direct path, where they are located a period ahead of the frame they belong to (the scanner UI following the head under
`next_direct`, the flash under `display_direct`). The earlier reading of flight 4 alone (the bypass plus the display time as the fix) did not survive flight 5.

**Caution (review e3692ed4, finding 3).** The comparison is the pilot's eye and fits all four observations; it does not observe the cull camera's final
consumer, and a terrain-generation latency could still contribute rare flashes. `angle-to-next-drawn`, the test build's measurement, was circular under `next`
(~0 by construction) and saw only the requests that reached the runtime; it is not evidence and was not kept.

**What ships.** `answerTime` (head_pose_time.h): display time of the latest frame + that frame's period, via space_pose.h `nextPredictionTime`; freshness on the
cached frame; no period, overflow, or a reference-space change between (`ReferenceChanges::crosses`) gives no target and the call is located at now + prediction,
counted as a fallback. No `advanced.cull_pose`, no modes, no pose_latch_patch files, no engine write of any kind, frame ABI unchanged (version 5): review
findings 1 (restore ownership) and 5 (the next-pose table) and the patch-lifetime note belong to code that is not here. The first-sight lines read "one display
period after the drawn frame's display time". The `pose gap:` line is unchanged: Elite's caller reads about +11 ms (90 Hz) and an angle to the drawn pose of about
head speed times that period. Tests: tools\openxr_pose_test (the arithmetic, the boundary of the freshness test on the cached frame, no period, overflow,
a reference change, the wording) and tools\openxr_native_test (the same through the host: the real hop from a foreign thread, a fake runtime that records the
instant it was handed, a refused target, a reference change, unflagged callers still at now + prediction).

**A crash in the same session.** The pilot's boarding crash that session was traced to Elite's allocator on a job thread; no EDVR frame was on the stack. It is
under separate check and is not part of this arc.

## 2026-10-10 — late frames: `pose slot:`, `pose arrival:` and `advanced.cull_pose_timing` (test build, branch claude/cull-late-frames)

*Not shipped: this was the test build that ruled the pose timing out. The key, the slot and arrival answers and the two diagnostics stay on that branch (see the last entry).*

**Evidence.** A Quest 3 via Virtual Desktop (runtime `VirtualDesktopXR`) at 72 and at 90 Hz shows infrequent quick black flashes at the outer edges of either eye
on head turns. The logs show the fix engaged and one period ahead in steady windows, but the game misses display slots 40 to 60 times a minute, and steady
windows still show 4 to 44 fallbacks a minute on one of Elite's two calling threads. Sean's own Quest 3 at 72 Hz with the checkerboard ON showed no blocks at one
planet, so the checkerboard alone does not explain it.

**Hypotheses.** All of them give a frame culled with a pose older or newer than the frame it is drawn in, which is a flash at the edge. The instruments below
measure each; none is a finding yet. (H-mix, added the same day, is in "Arrival" below; `pose arrival:` is its instrument.)

| | mechanism | what `pose slot:` shows |
|---|---|---|
| H-late | the game misses a slot; the next frame is shown at D+2P (or later), so `next` (D+P) is a period short for it | answered calls that are `short` by one period (1p) with the angle above 1 degree, rising with the window's late frames |
| H-stale | the cached D goes more than a period behind now (the late frame's own wait is still pending), the freshness guard drops the call to now + prediction, 40+ ms short | `fallback` with reason `stale` and the angle above 1 degree |
| H-overshoot | the cache had already advanced when the call came, so D+P is a frame ahead | `long` with the angle above 1 degree |
| none | the pose timing is not the cause | every class near zero in the windows where the pilot saw flashes |

**The instrument (always on, both modes).** For every call the fix handles (a return address inside the game's image and a prediction under 5 ms: answered at an
instant of its own OR fallen back to now + prediction) the runtime remembers the instant it was located at (T), the reason it had none of its own if it had none,
the calling thread and the orientation it returned (pose_slot.h `noteCall`, from `locateHeadOwned`, owner thread, no allocation, no lock; a fixed store of 256
calls, the rest counted unresolved). At the next WaitGetPoses (the next time the cached frame changes: `publishPoseGap`) every call made since the previous one is
resolved against that frame, before the cache is replaced: delta = T - D', and the angle between the orientation the call returned and the one EDVR located for D'
(the pose that frame is drawn with). Classes, with P' the next frame's period: `match` |delta| < P'/4; `short` delta <= -P'/4 (`1p` within 1.5 periods, `2p+`
beyond); `long` delta >= +P'/4. A call that fell back is its own class by reason (`none` no frame yet or an unreadable clock, `stale`, `period` no usable period or lead
or an overflow, `cross` a reference-space change, `cap` slot mode past 250 ms, `refused` the runtime refused the instant with XR_ERROR_TIME_INVALID and the call was
retried at now + prediction), and is also split by its delta. calls = match + short + long + fallback + unresolved. The cache being dropped (a session, origin or
geometry change), a next frame with no usable period, a full store and the end of the run leave a call unresolved. One line per calling thread per 60 s window, in the
runtime log next to the unchanged `pose gap:` line; a mode change closes the window at the change so a line has one mode, and a window with calls always writes its line,
zeros included (no line at all means the instrument never ran):

`pose slot: tid T mode next|slot|arrival calls N match M short S (1p a, 2p+ b) long L (1p c, 2p+ d) fallback F (none e, stale f, period g, cross h, cap i, refused j; delta match k,
short l, long m) unresolved U; >1deg match .. short .. (1p .., 2p+ ..) long .. (1p .., 2p+ ..) fallback .. (none .., stale .., period .., cross .., cap .., refused ..); >2deg ...;
max X deg (class); late frames n; window S s`

`late frames` is the runtime's late-frame count (frame_boundary.h) over the window, `window` its length (60 s, less for a window cut short by a mode change or the end of
the run, which writes its last lines at close). `python tools\edup_log.py --target <t> --tally pose` tables them per mode and thread with per-minute rates after the
`pose gap:` table. Tests: tools\openxr_pose_test (the classification at the quarter-period and 1.5-period boundaries on both signs, the thresholds, the store and its
overflow, the unresolved paths, the window and the mode split, the exact line, tools\pose_slot_fixture.log held byte for byte), tools\openxr_native_test (the same through
the host), the log tool's `--self-test`.

**The candidate (`advanced.cull_pose_timing = slot`).** `next` is D + P. The slot answer is the next display slot the next frame will really take, as far as it can be told
from the cached frame (head_pose_time.h `slotAnswerTime`): L is the frame's lead, its display time minus the runtime time at which the xrWaitFrame that produced it returned
(read the moment `boundary.waitAndBegin` comes back, `noteWaitReturned`, from the clock a pose call reads, and kept with the frame; on a turbo-paced frame, which
the runtime opens at once with a synthesized display time, it is that time less the moment of opening); e = now + L is when a frame whose wait
returns now would be shown; k = max(1, ceil((e - D) / P)); T = D + k*P. With no frame, no period, or L unknown or not positive it falls back to now + prediction as `next`
does (reasons `none`, `period`); k beyond 250 ms / P (at least 1) is a `cap` fallback (a stall or a loading screen, where `next` would have said `stale`); an overflow is
`period`; a reference-space change between D and T is `cross`. The freshness guard is not applied (k replaces it). In steady flight a call comes within a period of the wait
that returned D, so e <= D + P and k = 1: exactly `next`. It differs only after a late or stale frame (k = 2 for a call 1.2 periods after the wait, 3 for 2.5, and so on).
Off is bit-identical to the shipped answer (`answerTime` is now `answerTimeWhy` and its yes/no; the cells of the shipped behaviour are unchanged).

**Arrival (the same day, after review): the pose call is served on the owner thread.** Elite's game thread asks on its own thread; `locateHeadFor` hops the call to the
owner (`service.invoke`, a FIFO), and the owner is the thread that runs WaitGetPoses, blocked in xrWaitFrame for much of each frame. Relative to the frame that was latest
when the call ARRIVED (N): a call that arrives while the owner is blocked in the wait for N+1 is served after N+1 is published and is answered D(N+1)+P, about D(N)+2P; a
call that arrives between Elite's Submit of N and the start of that wait (the owner in xrEndFrame, or idle) is answered D(N)+P. Same place in Elite's pipeline, answers one
period apart. `pose slot:` classes a call against the next publish after it was SERVED, so it calls both "match" by construction and cannot see this. The pilot judged `next`
right on the Pimax, where most between-frames calls probably straddled; on Virtual Desktop the mix may differ, and a mix makes the cull jump a period back and forth, which
fits flashes at either edge. This is H-mix. The instrument that sees it:
- *The stamp.* `locateHeadFor` stamps the call on the CALLING thread before the hop (HeadCall: `arrived`, `publishSeq`, `submitSeq`, `arrivalTicks`; a call made on the owner
  thread is stamped where it is made). `publishSeq` counts the frames published to the pose cache (incremented in `publishPoseGap`, the end of waitPoses); `submitSeq` is set
  to the `publishSeq` at the entry of `submitEyeAdmitted` on Elite's thread, before the hop to the owner, the first moment EDVR knows Elite has begun to submit the latest
  frame (either eye; not again when the owner re-enters). Classes, from the two at arrival and `publishSeq` when served (head_pose_time.h `classifyPoseArrival`): `Between`
  (submitSeq >= publishSeq at arrival, no publish while it queued), `Straddled` (the same, but a publish happened while it queued), `Render` (submitSeq < publishSeq at
  arrival: published, not yet submitted), `Other` (a Render call that straddled). Queue time = service minus arrival, on the head clock.
- *The line* (pose_arrival.h; next to `pose slot:`, same window and mode split, always on): `pose arrival: tid T mode next|slot|arrival calls N between B straddled S render R
  other O fallback F unresolved U; queue ms between m/x, straddled m/x, render m/x; vs first frame after arrival: between (0p a, +1p b, +2p+ c, -1p- d), straddled (...),
  render (...), other (...); >1deg between x straddled y render z other w; >2deg ...; max X deg (class); late frames n; window S s`. "First frame after arrival" is, for a
  Straddled or Other call, the frame published during its queue (the one cached when it was served, compared at once); for the others, the next publish. Buckets are
  delta = T - D(that frame) to the nearest period: 0p, +1p, +2p+, -1p-; the angle is between the orientation returned and that frame's drawn head pose. Fallbacks are counted
  (`fallback`) and left out of the buckets and the angles. calls = between + straddled + render + other; buckets + fallback + unresolved = calls.
- *The timing `arrival`* (head_pose_time.h `timingAnswerTime`): the slot target computed from the cache at service, PLUS ONE PERIOD when the call is Between (not
  straddled); Straddled, Render and Other calls get the slot target unchanged. So every between-frames call is answered at D(the first frame published after it arrived) + P,
  whether or not it queued behind the wait, and render-window calls are answered as `next`. The cap is judged on k before the extra period; the extra period has its own
  overflow check and the crossing check covers the final span.
- *Reading it.* Under `next`, between-frames calls answered at 0p AND at +1p are the mix. If Between (not straddled) calls are a real share of the between-frames calls on
  Virtual Desktop with calls above 1 degree, the mix explains the flashes and `arrival` should clear them. If nearly every between-frames call straddles on VD too, the mix is
  not it. Under `arrival` Between and Straddled both read +1p and Render reads 0p. The same unknowns hold as before: this is the pilot's eye plus the instrument, not a
  direct view of the cull camera.

**The key.** `advanced.cull_pose_timing = next | slot | arrival` in [advanced] (default next), TEMPORARY: it goes when the arc closes and its winning value ships as behaviour.
(Born `advanced.cull_pose_slot = off | on` on this branch earlier the same day; that name never left it.) It is read by the graphics half at every `beginFrame` (like
fix.weapon_stability) from the live-reloaded config and handed to the runtime in version 4's word of EdvrNativeFrameOutput, retired 2026-10-09 with the terrain guard and put
to use again (`cullPoseTiming`: 0 next, 1 slot, 2 arrival, anything else next), so there is no new ABI version and a half from before this key reads `next` either way; the
runtime takes it at the next WaitGetPoses publish, and the calls after that publish are made under it. The graphics log writes `cull pose timing: next|slot|arrival (<what it
does>)` at the session's first frame and on every change. `next` is bit-identical to the shipped answer.

**Protocol.** Quest 3 via Virtual Desktop (72 or 90 Hz), planet descent and low flight with head turns, three segments of about 4 minutes each with the key at `next`, then
`slot`, then `arrival` (edit edvr.ini in the game folder while flying; it is live), the pilot noting flashes in each segment, then the key back at `next` before leaving the
game: a mode change closes and writes the window, whereas a window still open when the game exits (up to the last 59 s) is written only if the runtime's `close()` runs (the
end-of-run flush is there; no rig reaches it, and whether Elite's exit does is not known; `pose gap:` has no end-of-run flush at all). Read `--tally pose`: the `pose arrival:`
`next` rows say whether the mix is there (above); the `pose slot:` `next` rows say whether any other hypothesis is: short 1p above 1 degree rising with late frames is H-late;
fallback stale above 1 degree is H-stale (under `slot` it becomes a slot or a `cap`); long above 1 degree is H-overshoot; the `slot` and `arrival` rows say whether each
candidate removed it, and the pilot's flashes per segment say whether any of it mattered. Every class near zero in all three while the flashes persist means the pose timing
is not the cause and the reporter's other questions (terrain-generation latency, VD's timewarp edges) are next. Tests: tools\openxr_pose_test (the class rule, the three timings
for each class on time, late, stale, at the cap, on an overflow of the extra period and across a reference change; the buckets at the half-period boundaries; the arrival
store, unresolved paths, window and mode split, the exact line, tools\pose_arrival_fixture.log), tools\openxr_native_test (the stamp; a straddle made as production makes one,
the owner held in a job while the call queues behind it; Between, Render, Straddled, Other and the owner-thread path under `next` and `arrival`; the real Submit setting
submitSeq in frame_end_overlap_cases.h), the log tool's `--self-test`. Not flown.

**Flown 2026-10-10 17:10-17:25 local (Sean, Frontier, Quest 3 via VD at 72 Hz, f24ca5d0; runtime log
edvr_openxr_20261010_171008_107_23432.log).** Segments: next 17:10-17:16 (the game load inside it), slot 17:16-17:20, arrival
17:20-17:24, next again from 17:24:16. Two Elite threads call from exe+0x4E3881: tid 25756 (~5 calls a frame, 100% Between, queue
0.00 ms mean) and tid 28956 (~1.3 a frame, 97% Straddled with a 5.7 ms mean queue, 3% Render once in flight). The load minute carried
395 late frames and 282 stale fallbacks on 28956 (the worst 11.9 deg from the next drawn pose); the next minute 14, then none. From
17:13 on, every window had 0 fallbacks and 0 to 12 late frames a minute, so `slot` gave the very answers `next` gave, and each thread
sat in one class frame after frame: no mix. `arrival` moved 25756 one period later (720 calls a minute above 1 deg from the next drawn
pose, by construction). The pilot saw "several flashes in each segment", alike, and only over flat ground. **Ruled out:** the pose
timing (late frames, the stale fallback, overshoot, the arrival mix) as the cause of these flashes, because the answers were steady
and consistent where the flashes came, and moving one thread a whole period changed nothing. The stale fallback during loads is real
but harmless (no terrain then). Flat ground only fits large tiles, where one late or dropped tile is a big square: next are temporal
AA off, the same flight on the Crystal Super and in flat mode, then terrain-generation latency.

## 2026-10-10 — the rendered field decides; the edge-black watch (test build, branch claude/cull-late-frames)

*Not shipped: the watch failed as an instrument (see the last entry). H-gen-edge, its "late tiles", turned out to be the centred cull of the next entry.*

**The result (Sean, Crystal Super, f24ca5d0, log edvr_openxr_20261010_173409_735_33536.log, 17:34 to 17:37 local, 5424 x 5356 per eye).** With the Pimax Play FOV crop
REMOVED there were NO flashes over flat ground. The three fields that have been flown over flat ground, from the projection the runtime hands Elite:

| rig | m00 | m11 | other | about | flashes |
|---|---|---|---|---|---|
| Crystal Super, uncropped (this flight) | 0.780744255 | 0.7906425 | m02 -0.193965569, m12 0 | 103 x 103 deg | none |
| Crystal Super, cropped in Pimax Play (since 2026-09-23 14:28) | 0.939449012 | 0.971494555 | | 93 x 92 deg | yes |
| Quest 3 via Virtual Desktop Godlike | 0.902738333 | 0.835478902 | m12 -0.193187431 | 94 x 99 deg | yes |

The crop kept the Pimax vertically symmetric (m12 still 0), so symmetry is not what separates them. What separates them is how much of the lens the rendered field covers.

**Correction, same day (the pilot).** The black shapes also appear FLOATING INSIDE the view, with terrain around them, in varying shapes (square, diamond, parallelogram).
That changes the reading of the table above. The first reading (H-after: the runtime's ordinary reprojection shifts the frame for head motion and, with a field cropped
inside the lens's visible area, the edge of the rendered image, black beyond it, shows at a corner) is WEAKENED: an exposed image edge sits at the border and cannot float.
Leading instead is **H-gen-edge**: tiles that enter the view at its edge during a head turn are generated or selected a few frames late and travel inward while missing.
With the full 103 x 103 field that edge is in the lens periphery, where nothing is seen clearly; with a field cropped inside the clearly visible area (the 93 x 92 crop, VD's
Quest 3 field) the late tiles are in plain view; flat ground has the largest tiles, so one missing tile is a big shape. It fits: flat ground only, varying shapes, outer corners
where a turn brings new ground in, never in flat mode (a flat screen has no lens periphery to hide an edge in), and it predates EDVR's temporal work. It is a hypothesis: the pose timing was the last thing
that could be measured from the runtime side, and it is out (above); nothing so far looks at the image itself.

**The split the watch makes.** H-image: the black is IN the texture Elite submits (H-gen-edge is its late-tile form). H-after: the black is added after EDVR submits (the
runtime's reprojection, the vendor compositor, the headset). Game-stage events at about the pilot's count say H-image; zero events while the pilot sees flashes say H-after.

**The instrument (always on, TEMPORARY; src/d3d11/edge_black.cpp and edge_black_core.h, the grid shader edge_black_shader.h).** Hooked into the native temporal channel's
`treat()` after the submit's own validation and before any EDVR pass touches the texture, so it runs whether or not temporal AA is on and reads exactly what the luma probe
calls the stage "game". Per eye per frame: one 16 x 16 dispatch of 8 x 8 thread groups, each thread one point-sampled texel of a 128 x 128 grid (the centre of the cell, `u =
(x + 0.5) / 128`, the largest of R, G, B), written to a 64 KB buffer, copied to a staging ring four frames deep, and the oldest ready staging buffer mapped without waiting (a
buffer not yet ready waits for the next frame; the verdict is two frames behind the draw plus the readback, three to four frames). No full-eye copy, no query, no device wait.
The shader is compiled at build time by tools\temporal_shader_build into temporal_shader_bytecode.h (d3d11.dll must not import d3dcompiler_47.dll, the PE gate). The
texture is read through a plain UNORM view: R8G8B8A8 and B8G8R8A8 as code/255, R10G10B10A2 as code/1023, R16G16B16A16 UNORM as code/65535, float formats as they are; an sRGB
texture, an unlisted format or a texture with no shader view is said once (`edge black: eye E is not watched: ...`) and left alone. Elite's eye texture is
R8G8B8A8_TYPELESS, so the encoding is code/255.

*The rule.* Per point, black is the largest channel under 0.004 (an 8-bit code of 0 or 1: 1/255 = 0.00392), lit is over 0.03 (a code of 8 and up), anything between (or NaN)
is dim and counts as neither. A point is TRANSIENT BLACK in frame f when it is black in f and lit in both f-1 and f+1, or black in f and f+1 with lit in f-1 and f+2 (two
frames). The verdict for frame f is given when f+2 has arrived. A frame whose sequence number does not follow the last one, or a texture resize, empties that eye's history
(the frames it held and never judged are counted as `samples lost`). An EVENT is an eye-frame with at least three transient points that touch (8-neighbourhood); the largest
cluster is reported, `clusters=N` is appended when there are several. BLACK-START points (black now, lit before, still black two frames on) are the control: a horizon or a dark
object moving in, which is not a flash. Region labels per eye: eye 0's outer (temporal) side is x < 0.5, eye 1's is x > 0.5; bottom, middle and top are thirds; `centre` is the
middle third within 0.1 of the midline. `head=` is the angular speed between the frame's head pose and the previous one, `unknown` when the graphics half has no pose.

*The lines (graphics log).* `edge black: watching stage game (...)` once, when the first eye is sampled (grid, metric, format and encoding, thresholds, rule, readback depth);
`edge black: event eye=E frame=N t=HH:MM:SS.mmm points=K cells=WxH centre=X,Y region=R border=B head=H deg/s` for the first 60 events (then `edge black: 60 events listed;
further events are counted in the summaries only.`); every 60 s, events or none, `edge black: 60 s: frames N eyes-sampled M events E (outer-bottom a, outer-middle b, ...,
centre c) largest K points; transient points total T; black-start points (black now, lit before, still black) S; samples lost L`; and `edge black: closed -- totals for the
run: ...` when the channel closes. `python tools\edup_log.py --target <t> --tally edge` tables them. No `edge black:` start line means the watch never ran.

*Cost.* The CPU side is about a dozen D3D calls and the judgement of one grid (three passes over 16384 bytes: 25 to 28 us measured in the rig (28.4 in the loaded full build), on the development machine, a render-thread cost per eye-frame, under the 40 us line); the GPU
side is 16384 single-texel loads and stores, a 64 KB copy and a 64 KB Map. The log's `periodic work: edge_black` line carries the measured per-eye figure from a real flight. Memory: 64 KB x 4 x 2 staging.

*The grid size (review, same day).* The first build sampled 64 x 64. On the cropped Crystal Super (4508 px wide, about 48 px a degree) that is a sample every 70 px, and an event
needs three touching points, so a black shape under about 140 px (3 degrees) could go uncounted; a false negative would have read as H-after. The grid is 128 x 128: the floor
is about 70 px (1.5 degrees). Nothing else changed (thresholds, the rule, three touching points, regions, lines). The judgement of one grid costs 25 to 28 us on the render
thread per eye-frame in the rig (it was 6.6 us at 64), under the 40 us line that would have wanted it moved off that thread; the staging buffer is 64 KB a slot. The grid size is
written once, in edge_black_shader.h (`kEdgeBlackGrid`, the shader text is built from it) and held to the judgement's `kGrid` by a static_assert.

*Tests.* tools\edge_black_test (build.bat `:rig_edge_black_test`): the thresholds including the NaN and the boundaries, the region labels for both eyes, the clustering, the
transient rule over a history (lit-black-lit, lit-black-black-lit, a black of three frames, a dim neighbour, a frame missing, a resize), black-start, the window, the event cap,
the exact lines, the production shader on WARP against a synthetic eye texture (a scripted flash at the cells it should come out at, a sub-region of the texture, a mirrored
eye, a resize in the middle of a flash, a format and a bind the watch refuses), and tools\edge_black_fixture.log held byte for byte; tools\native_temporal_test (the hook is
fed per eye, closed once); tools\temporal_shader_build (the shader's contract and reflection); the log tool's `--self-test` (`--tally edge`).

**Protocol.** The same flat low flight, on the cropped Crystal Super (or the Quest 3 via VD), the pilot counting flashes over a segment of known length, ideally two or three
segments of about 4 minutes. Read `--tally edge` for the segment: events at about the pilot's count (same eye and corner where the pilot can say) are H-image, and border=0
events are the late-tile signature; zero events with flashes seen is H-after (the next question is then the vendor compositor, with the runtime's own reprojection as the
first suspect); a few events and many flashes is a threshold question (check `black-start` and `transient points`, which the thresholds cannot hide). Not flown.

**The remedy, if H-gen-edge** (not built; waits for the flight). Elite needs to prepare terrain beyond what it shows. Either (a) widen only what the terrain culler reads, the
old guard's route (removed 2026-10-09, never proved: the watch now gives an objective count of black events to test it against), or (b) render a margin beyond the cropped field
and crop it, which is certain and costs pixels (the full uncropped field is 1.20 times as wide and 1.23 times as tall in tangent space as the crop, from the m00 and m11 above, so about 1.5 times the pixels at the
same density; a margin of a few degrees costs correspondingly less, with the cropped view cut from it). The earlier plan of a temporary AA-off flight is superseded: the watch reads the image before EDVR's AA, so AA cannot be part of an event.

## 2026-10-10 — the terrain cull is a centred frustum: READ, and the cover (test build, branch claude/cull-late-frames)

*The READ and the rule stand. The key `advanced.terrain_cull_cover`, the mode lines, the four first-call lines and the 60 s census below were the test build's; the shipped cover has none of them (see the last entry).*

**READ (build 332841, PE stamp 1788384820, image size 104894464; disassembled offline with capstone; dumps in analysis\decomp\verify_20261010_tiles_*).** The planet draw-list builder
0x4462E80 calls 0x4455A40 (at 0x4463A83), which writes one record per render view. For a main (non-ortho) view it builds four cull planes at one place:

```
4455FDD  F3 41 0F 10 46 70            movss xmm0,[r14+0x70]     aspect   (the recommended W/H: the fov getter's out[0])
4455FE3  0F 29 8D C0 00 00 00         movaps [rbp+0xC0],xmm1    (interleaved)
4455FEA  F3 41 0F 10 8E 90 00 00 00   movss xmm1,[r14+0x90]     vFOV     (atan|top| + atan|bottom|: out[1])
4455FF3  F3 0F 11 44 24 28            movss [rsp+0x28],xmm0     arg 6
4455FF9  F3 0F 11 4C 24 20            movss [rsp+0x20],xmm1     arg 5
4455FFF  44 0F 29 A5 E0 00 00 00      movaps [rbp+0xE0],xmm12   (interleaved)
4456007  E8 F4 87 FE FF               call 0x14443E800          rcx, rdx, r8, r9 = rbp+0x160, +0xE0, +0xC0, +0xA0 (lea)
```

0x443E800 is entered with arg 5 (vFOV) at [rsp+0x28] and arg 6 (aspect) at [rsp+0x30]. Its head is `48 8B C4 / 48 89 58 10 / 48 89 68 18 / 48 89 70 20 / 57 / 48 81 EC 90 00 00 00 /
F3 0F 10 8C 24 C0 00 00 00` (movss xmm1,[rsp+0xC0] = arg 5), `48 8B E9` (mov rbp,rcx), `F3 0F 59 0D rel32` (mulss by the 0.5 at 0x4DDF878). It computes a = vFOV/2, b =
atan(aspect * tan a), and writes four planes through the eye: +-a about the camera's Z axis vertically, +-b horizontally. A CENTRED frustum, no margin. A .text scan finds exactly
one E8 to it, the one at 0x4456007. The per-(patch, view) test 0x445BA40 -> 0x445A1C0 culls a patch box against these planes with no epsilon. LOD and selection use another box
(0x44597B0), not a frustum. The rendered image uses the explicit kind-5 projection matrix, not these values. The call site's next instructions read the four planes back from [rbp+0x160..0x190]
and nothing reads RAX or XMM0, so the builder's return value is nobody's.

**What it explains.** From the frusta the runtime hands the game (analysis\decomp\verify_20261010_tiles_frustum_emu.txt, _coverage.txt):

| rig | cull half-angles, v / h | the image reaches | the cull is short by |
|---|---|---|---|
| Crystal Super, cropped in Pimax Play (93 x 92 deg) | +-45.83 / +-46.80 | outer 51.44 | 4.64 deg outward (8.9% of the eye's width) |
| Crystal Super, uncropped (103 x 103 deg) | +-51.67 / +-52.02 | outer 56.82 | 4.80 deg outward (9.7%) |
| Quest 3 via Virtual Desktop (94 x 99 deg) | +-49.50 / +-47.78 | outer 54.00, down 55.00 | 6.22 deg outward (12.4%), 5.50 deg down (10.7%) |

The inner side and the top are covered with 5.6 to 7.8 deg to spare. A patch is dropped only when its WHOLE box lies in the uncovered band: rare at rest, and it happens as patches slide
through the band during a head turn, one frame's cull deciding what the next frame draws. Flat ground has the largest patches, so one dropped patch is a big shape; as it slides on it
floats inside the view with terrain around it. The uncropped Crystal Super is short by the same 4.8 deg and shows nothing: its band lies at 52 to 57 deg, in the lens periphery where
nothing is seen clearly; the cropped field and VD's put the band at 47 to 51 and 48 to 54 deg, in plain view. That comparison is the pilot's (the two fields were flown on different days);
nothing here measures what each lens shows.

**Overturned.** (1) The Status-detail ruling "a static frustum deficit (the centred-cull models, H-cam included) is ruled out because a steady head shows no squares" (10-09): the centred
cull is READ, and a steady head should show almost none, because a patch needs its whole box inside a thin band. (2) Rounds 3-5's "the probe's eye groups cannot be expected to move an eye
camera's frustum": true of the rendered frustum, not of this consumer, which takes the camera's aspect and vFOV from [r14+0x70] and [r14+0x90]. H-cam (the culler is a centred frustum
built from the camera's aspect and vFOV) held in outline; FUN_13ACC40 was the wrong function. The old guard's horizontal widening fits it (it reached the game as a bigger render size, i.e. aspect) and cost render pixels; this widens the
two numbers instead.

**The fix (TEMPORARY, `advanced.terrain_cull_cover = on | off`, default on, live within a second).** src\d3d11\terrain_cull_cover_core.h (pure: the rule, the gate, the lines),
terrain_cull_cover_hook.cpp (the glue), tools\terrain_cover_test. A CodeHook at 0x443E800 through the project's relay (a copy of the vertex resync's: the target is more than two
gigabytes from this DLL), with a typed detour `(rcx, rdx, r8, r9, float vfov, float aspect)` that calls the original with the same four pointers and two floats it computed. The head's
first two instructions are stolen (`mov rax,rsp`, `mov [rax+10h],rbx`: 7 bytes, nothing rip-relative; the relay's RAX is overwritten by the first). The detour reads none of the game's memory
and so has no SEH and no stand-down; it is arithmetic and atomics, about 180 calls a second.

*The rule.* With T and H the largest vertical and horizontal tangent over BOTH eyes of the frusta handed to the game (the native temporal channel's begin(), unjittered, after any FOV
trim; published as one atomic word), a_in = vFOV/2, b_in = atan(aspect * tan a_in): a_out = max(a_in, atan T), b_out = max(b_in, atan H), vFOV' = 2 a_out, aspect' = tan b_out / tan a_out.
The builder then derives a_out and b_out. An axis is widened only when the tangent reaches past it by more than 1e-5 rad (a float's rounding of the game's own sums, well under a pixel),
so a covered headset is passed through bit for bit and `widened` means widened; the other axis keeps the game's own value. It never narrows (to the float's 1e-7). Passed through
unchanged: the key off, no tangents known (flat, or before the first VR frame), a vFOV that is not finite or not in (0, pi), an aspect that is not finite and positive. Predicted values
(the rig holds them): cropped Crystal Super aspect 1.0344 -> 1.2186 (vFOV 91.65 unchanged); uncropped 1.0127 -> 1.2092 (103.34 unchanged); Quest 3 vFOV 99.00 -> 110.00, aspect 0.9412 -> 0.9637.
Taking the maximum over both eyes makes the cull the per-axis symmetric superset of both eyes' frusta: the narrower eye keeps a few more patches near its edge than it shows.

*The gate.* The PE stamp 1788384820 and image size 104894464, the callee's 32-byte head plus a `mulss` in the next 16 bytes whose operand is exactly 0.5f, and the call-site chain
(fragments A..D with at most 16 bytes of interleaved instructions between them, then the E8) at 0x4455FDD whose call lands on 0x443E800. Another stamp: the code section is scanned for
the chain, and one match whose callee has the same head is accepted (`found by signature scan`); none, two, or a callee with another head is inert. A flat session is inert. CodeHook's own
checks (an aligned entry, a decodable prologue, a trampoline in reach) stand as for every hook here. Every refusal is one line: `terrain cull cover: inert: <why>`.

*The lines (graphics log).* `terrain cull cover: hook live at exe+0x443E800 (<how found>)`; `terrain cull cover: on|off (<what>)` at the first poll and at each change of the live key (read
once a second with the config tick, the window closed at the change); `terrain cull cover: call N of 4: in aspect A vfov V deg -> out aspect A' vfov V' deg (widened|unchanged); T=t H=h;
mode on|off` for the session's first four calls; `terrain cull cover: S s: mode on|off calls N widened W; last in aspect A vfov V deg -> out aspect A' vfov V' deg; T=t H=h` every 60 s,
calls or none, and at each change. In VR expect about two calls a frame (180 a second at 90 Hz) with the eye's own aspect and vFOV as the in-values.
`python tools\edup_log.py --target <t> --tally cover` tables them per mode and joins the edge-black 60 s summaries to the mode in force at their start and end (`mixed` when a change fell
inside one).

*Tests.* tools\terrain_cover_test (build.bat `:rig_terrain_cover_test`, 11308 checks): the rule for the three rigs, a symmetric headset, an input already wider, bad input, the key off, a
sweep that never narrows and leaves alone what it should; T and H from the frusta; the callee head byte by byte, the signature with 0 to 16 interleaved bytes and each way of being wrong,
the scan on a synthetic buffer; the module gate on synthetic modules (the known build, a stamp one off, no PE, one match, none, two, a wrong callee, a wrong constant, no code section);
the production hook with the real CodeHook on a synthetic builder of the same six-argument shape, in the known build's layout and in another build's: only arguments 5 and 6 change, the
four register arguments and the return value are the caller's, the key off and no-tangent calls are bit for bit, a flat session patches nothing, the first four calls and the 60 s windows
are exactly what the log says; tools\terrain_cover_fixture.log held byte for byte; the wiring (device_hook.cpp, native_temporal.cpp, build.bat, edvr.ini) read as text. The log tool's
`--self-test` has the `--tally cover` cells.

**Protocol (the flight).** The cropped Crystal Super (crop on) or the Quest 3 via VD, low over flat ground with head turns. About 4 minutes with `terrain_cull_cover = off` in the game's
edvr.ini (live), then about 4 minutes `on`, the pilot counting flashes in each segment. `--tally cover` says the hook was live and called about twice a frame with widened = all while on;
the edge-black events by region (outer, and bottom on the Quest 3) per minute, off against on, are the objective count. A clear drop with it on says the centred cull was the source. Events
that stay with it on are not the cull: the black is then in tiles Elite generates late (H-gen-edge) or added after EDVR submits (H-after, weakened: it cannot float). Not flown.

## 2026-10-10 — flown: the cover ships with no key (branch claude/terrain-cull-cover, from main)

**The flight.** Sean, Quest 3 via Virtual Desktop at 90 Hz, Frontier, test build 52fe241c (graphics log edvr_gfx_20261010_190514.log, runtime log
edvr_openxr_20261010_190515_484_25604.log; local time). `advanced.terrain_cull_cover` was off 19:05:14 to 19:10:54 and from 19:14:27 to the end, on 19:10:54 to 19:14:27.
He counted **20 black flashes with it off (about 5.7 minutes) and none with it on (about 3.5 minutes)**; at the off rate about 12 were expected in the on segment, and the
chance of none is about 5e-6. The log: the hook live (every gate); every call took the eye's own aspect 0.9412 and vFOV 99.00 degrees and left, with it on, with 0.9638 and
110.00; 100% of the calls were widened on and 0% off; 19 to 25 calls a frame, not two: one per planet patch group per eye, all with the eye's values. The frame rate stayed
89.7 to 90.0 with 0 to 1 late frames a minute while it was on: no measurable cost.

**The edge-black watch failed as an instrument and is not shipped.** In both modes it counted about 5000 events a minute (the cover on read 121% of off), almost all of them
one-cell-tall horizontal streaks, 3x1 to 8x1 cells, at fixed heights (y about 0.18 and 0.80) in both eyes, border=0, with the head at 1 to 5 deg/s: high-contrast cockpit and UI
edges flickering from frame to frame, which pass the lit-black-lit rule and swamp the real flashes. It was the right question and the wrong instrument; the pilot's count and the
cover's own call census were the evidence. The watch, its shader, staging, rig, fixture, the stack flag and the tally stay on claude/cull-late-frames, the record.

**What ships (Sean approved: ship the cover as behaviour with no key, keep `next` exactly as in 0.19, remove what is no longer needed).** A fresh branch from main with only:
src\d3d11\terrain_cull_cover_core.h (the rule, the gate, the signature scan, the lines), terrain_cull_cover_hook.cpp/.h (the relay, the detour, the heartbeat), the wiring (device_hook.cpp
installs it in the startup block and polls it with the config tick; native_temporal.cpp's begin() tells it the frusta the game is handed), tools\terrain_cover_test and its
build.bat rig, `edup_log.py --tally cover`. Always on in VR; installed in VR only (a flat session says it is inert, once). The key is gone everywhere (it never reached main, so
there is no retired-key entry), and so are the mode lines, the four first-call lines and the 60 s census. The lines, as shipped (the fixture's values):

```
terrain cull cover: hook live at exe+0x443E800 (PE stamp 1788384820 and image size 104894464 are build 332841's; the callee's head and the call site at exe+0x4455FDD verified)
terrain cull cover: first call: in aspect 1.0344 vfov 91.65 deg -> out aspect 1.2186 vfov 91.65 deg (widened); T=1.0293 H=1.2543
terrain cull cover: armed; 60000 of 60001 calls widened in the last 10 min (last in aspect 1.0344 vfov 91.65 deg -> out aspect 1.2186 vfov 91.65 deg)
terrain cull cover: inert: <why>
```

The first-call line is the first call made with the frusta known (a call before the first VR frame has no tangents). The heartbeat is the vertex resync's style: every ten
minutes while the hook is live, zero counts included, so a log shows the hook engaged; `0 of 0` is a window with no planet in view (loading, a station, supercruise) and `0 of M`
a headset the centred cull already covers. A rule detail that is new since the test build: an axis is widened only past 1e-5 rad (a float's rounding of the game's own sums).

**Not brought over (they stay on claude/cull-late-frames):** `advanced.cull_pose_timing` and its slot and arrival answers (timingAnswerTime, slotAnswerTime, PoseArrival, the
stamp and publish/submit sequence numbers, the lead capture), pose_slot.h and pose_arrival.h with their lines, fixtures, tests and tally code, the version-4 frame word (main's word
is as it was), and the edge-black watch. head_pose_time.h, native_runtime_host.h, native_frame*.h/cpp are as on main: `next` is exactly what 0.19 ships.

**Tests.** tools\terrain_cover_test (build.bat `:rig_terrain_cover_test`): the rule for the three rigs, a symmetric headset, an input already wider, bad input, a sweep that never
narrows; the callee head byte by byte, the signature with 0 to 16 interleaved bytes and each way of being wrong, the scan; the module gate on synthetic modules; the production hook
with the real CodeHook on a synthetic six-argument builder (only arguments 5 and 6 change; the four register arguments and the return value are the caller's; no tangents, a
covered frustum and a flat session are bit for bit); the exact first-call and heartbeat lines and that nothing else is written; tools\terrain_cover_fixture.log byte for byte; the
wiring and the absence of any key read as text. The log tool's `--self-test` has the `--tally cover` cells.
