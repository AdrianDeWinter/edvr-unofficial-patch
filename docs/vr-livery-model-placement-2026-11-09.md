# VR Livery screen on foot: the commander model is misplaced on the Quest 3 (2026-11-09)

## Status

- **F232 FLOWN (12:45, Steam, `edvr_gfx_20261109_124542.log`): CAUSE FOUND.** Livery's Location child is [Livery
  preview, HMD] after the handover (briefly 3 producers), and `identity()` refused it ("iface vtable"). Fixed in F232b.
- **F232b FLOWN (13:02, Frontier, `edvr_gfx_20261109_130228.log`, 4858a6a7): SUITS FIXED** (the pilot: suit Livery
  and suit store right). **Weapons:** the loadout's weapon preview (mode 3, dump `130608220001`) and the weapons
  purchase (dump `130757369003`) still show the weapon below the viewer. The weapon upgrade (dump
  `130819788004`) is right.
  - Weapon purchase roster: [preview +0x51AA500, a DIFFERENT head-side producer: iface +0x518C350, object
    +0x518C2E0, enabled]. Refused as "foreign entry", and `applied` stays at 7174 through the purchase. The original
    check refuses it too, so this is not a regression. It is the 11-01 "weapon preview stays low".
  - Upgrade (13:08:07-29): qualifies, with applied 7887 -> 8894.
  - F231's force flag does not matter: the purchase is broken with the patch on (13:06:58) and off (13:07:40).
  - **RE 6:** X is HeadlookCamera, with producer 0x1074650 (not hooked by EDVR). See "RE 6" at the end, including
    one claim this flight contradicts.
- **F233 (dcf3b743):** the finalizer takes either winner, and HeadlookCamera counts as a head entry (hook 0x1074650);
  eye-path lines `native preview camera (F233): final|eye`. Now behaviour (cleanup).
- **F233 FLOWN (14:04, Steam, `edvr_gfx_20261109_140407.log`): WEAPON PLACEMENT FIXED** (the pilot). Left over: in
  the loadout browser's weapon selection the view jerks up and down. The camera sits 3.4-3.9 m from the neck there,
  and F107 ended and restarted 7 episodes between 14:06:23 and 14:07:05 (F208 class). The store and upgrade are held
  by F209 and do not jerk.
- **F234 FLOWN (14:31, Steam, `edvr_gfx_20261109_143112.log`):** the loadout weapon selection is steady. The weapon
  Livery still jerked, only because the pilot's Steam ini had F228's key off ("only watched" at 14:34:19.6).
- **CLEANUP (the pilot: "Approve all"), c42495e0, `v0.22.0-rc.7-287-gc42495e0`, INSTALLED STEAM, NOT FLOWN:**
  - Behaviour now, keys gone: the Livery and loadout holds (F228, F234), the any-order preview roster with
    either finalizer winner (F232/F232b/F233), and HeadlookCamera as a head entry (F233).
  - Removed with their code: F230 (`livery_camera_repair`), F231 (`preview_camera`) and F229's setup hook and
    field reader.
  - Kept: the F232 refusal and roster lines and the F233 `final` and `eye` lines.
  - Grep clean. Rig 291/0, full build green.
  - The four dead key lines removed from the pilot's Steam edvr.ini (Edit tool, diff: 4 deletions).
  - Confirmation flight (`--expect-build c42495e0`): suit Livery, weapon Livery, loadout weapon selection, weapon
    purchase. Each should be placed right and steady. Then merge `--no-ff` to main and push.
- **State:** the cleanup (c42495e0) is INSTALLED on Steam; Frontier keeps F232b (4858a6a7).
- **Report (the pilot, 2026-11-09; first from a user on RC7 with a Quest 3):** in the on-foot Livery screens (suit
  and weapon cosmetics) the preview sits over or behind the viewer instead of beside the UI.
- **Root cause (H9, EDVR's own code, settled by F232):** `native_preview_camera` (10-26, for the ship preview)
  writes the preview's pose only for a Location child that `identity()` accepts, and it rejected Livery's
  [preview, HMD] roster. That roster predates RC7, which explains the RC7 report.
- **F232:** `identity()` names the first failing check. While a watched preview screen is open, a wait-free roster
  snapshot is logged once a second per role: `native preview camera (F232): role HMD|preview refused: <reason>;
  handover on|off; roster: ... entry i: ...`. TEMPORARY key `experimental.native_on_foot_preview_handover` (on,
  live, F8 "Preview handover"). With it on, a child with 3 to 6 producers is accepted when exactly one is the HMD
  entry and the rest are previews, the last preview used; a count of 2 keeps the original checks. Off is the
  original.
- **Ruled out:** per-headset inputs (H1/H3: the Pimax fails too); the Steam ini (Frontier fails too); an invalid
  camera node (H7, F230); the camera name or force (H8b, F231); EDVR's on-foot adjustments as the cause (F228
  changes only which wrong view); the game patches EDVR applies on foot (RE 2). Details in the sections below.
- **RE:** filed on elite-analysis main (build 334925): `analysis\decomp\verify_20261109_livery_anchor.txt`
  (7e20613), `_livery_camera.txt` (a13041e), `_livery_retry.txt` (ead680f), `_livery_resetup.txt` (9d9c2d1),
  `_livery_activation.txt` (8854ad9), `verify_20261109_weapon_head_producer.txt` (9c40f2b), with their decomps.
- **Builds:** run in `C:\wt99` (branch `claude/f228-short-build`). Remove it when done: take its `third_party`
  junctions out first. A `launcher_test` flake ("the payload stays none") failed one F232 build, and the rerun of
  the same tree passed.
- **Temporary keys:** none left; the cleanup made four of them behaviour and removed two (approved by the pilot).
- **Issue:** none yet.

## What the flight shows

- **The screen.** The scene depth probe counts 16-18 draws a frame from 06:40:03 to 06:40:24 (the showroom: the
  model and its dark blue backdrop), against 120+ in the concourse before and after. The centre reads 2.05 to
  2.36 m. In the dump the legs fill the eye at about 2.2 m and the backdrop is at about 4.4 m.
- **The camera is the commander's own.** For the same seconds F107 never fires (770 of 770 pin calls accepted,
  `camera 3 m away 0 s (max 8.0 cm)`). The game's camera reads +8.0 cm over the animated neck (1.543 m), and the
  concourse frame's `cam_local` stays at the commander's spot (4.50 22.75 3.22). So the game drew the preview
  model and backdrop around the commander's camera, and placed the model above the eye: the pilot's head pitch
  reads +47 to +73 deg from 06:40:16 to 06:40:29 while he looked at it.
- **The game's own showroom camera also appears.** Twice it stood 22 m from the neck (episodes 2 and 4, at
  06:39:53 and 06:40:32, `cam_local` -5.97 20.09 -16.01), and once 3.9 m away (episode 3). F107 treated those as
  away, as designed. The misplaced view is the stretch in between, with the commander's camera.
- **EDVR's own view offsets are centimetres:** the raise +7.0 cm, the virtual neck 0. The misplacement is metres.

## The Quest-only input: the projection's vertical centre

`projection_query` lines in the runtime logs, the matrix EDVR answers for Elite's `GetProjectionMatrix`:

| Rig | m00 | m11 | m02 (horizontal centre) | m12 (vertical centre) |
|---|---|---|---|---|
| Quest 3, VirtualDesktopXR (this flight) | 0.922 | 0.896 | -0.243 / +0.243 | **-0.193** |
| Pimax (Frontier, `edvr_openxr_20261109_052110_952_45272.log`) | 0.939 | 0.939 | -0.178 / +0.178 | 0 |

The Quest's m11 and m12 give tangents up 0.90 (42.0 deg) and down 1.33 (53.1 deg): the frustum's centre is about
12 deg below the forward axis (F194 measured the same rig). The Pimax's vertical frustum is symmetric (46.8 deg each
way). Both rigs have a horizontal off-centre term of similar size, and the Pimax places the model correctly. So a
mishandled horizontal term would show on both rigs, and the vertical term is the one input only the Quest has.

## Hypotheses

- **H4, EDVR's native on-foot machinery engaged during Livery** (added after the RE and the Pimax visit, below).
  With the camera on the commander, F107 lets the body pin, the neck reference and the raise act. They act on the
  local avatar and the view while the game draws its showroom, and the model ends up wrong.
  - Confirms: holding F107 away while Livery is open (the F209 signal, extended) puts the model behind the UI on
    the Quest.
  - Refutes: the model stays wrong on the Quest with the hold on and the pin refusing every call.
  - Not established: how the pin or the raise would move the model by metres. Their logged moves are small
    (no clamp at 1.5 m or 0.8 m, raise +7.0 cm).
- **H1, the vertical off-centre frustum.** Weakened by the RE: the placement code does not read it. The Livery preview places the model behind the UI by a
  computation that reads the eye projection and treats it as vertically symmetric, or gets the sign of m12 wrong.
  It is right at m12 = 0.
  - Confirms: with a vertically symmetric frustum handed to Elite on the Quest, the model sits behind the UI.
  - Refutes: the model stays where it is with m12 = 0.
  - Not established: the game's code for the preview's anchor. The analysis repository has the strings
    ("Vendor Suit Livery", `decomp_3E67C20`), not the placement.
- **H2, the Steam install's settings.** The pilot flies the Quest on the Steam install and the Pimax on Frontier,
  and the two `edvr.ini` files differ (among others: `visual.world_scale` 110 on Frontier only, the
  supersample keys, `fix.openxr_resolution` entries).
  - Confirms: the Pimax on the Steam install shows the bug.
  - Refutes: the Pimax on the Steam install, same build, places the model behind the UI.
- **H3, another per-headset answer** (render size, the Pimax's canted eye-to-head rotation, IPD). Less likely: none
  of them moves anything by metres. H1's A/B would leave the model misplaced if one of these is the cause.

## RE (2026-11-09, build 334925)

From a Ghidra read of the installed build. The decompilations are in the analysis inbox (not yet filed).

- **The owners.** `HumanoidLiveryActivity` (suit, vtable 0x5612F30) and `HumanoidWeaponLiveryActivity` (weapons,
  vtable 0x5614328), OnOpen 0x482D230, OnClose 0x481E7A0. Each embeds a `CustomisationVisual` at +0x680.
- **The placement is authored.** 0x12714C0 picks nodes in a data-authored showroom scene. For the character
  these are the attach node `Root!AttachPoint_AvatarCentre` and the camera node `Root!Camera_AvatarPreview`. The
  camera pose (+0x5E0, written by 0x126DFF0) comes from node transforms, the avatar's bounds and the orbit
  inputs (pitch, yaw, zoom). Nothing from 0x1268000 to 0x1276000 reads a projection element, a FOV, the HMD
  pose, the eye-to-head transform, the render size or the UI panel's transform (CONFIRMED by scan and by eye).
- **The projection wrapper.** 0x4E2DD0 calls `GetProjectionMatrix` (return address 0x4E2E5C, the one the
  runtime logs) and stores the matrix transposed, with the third column negated. Its consumers are not isolated.
- **The signal.** Both Livery vtables carry the buy screens' shared Show 0x9C3960, IsOpen 0x9C3290 and Close
  0x9C22A0. Show sets byte +0x79 and Close clears it, so F209's existing hooks see Livery once its filter takes
  the two vtables (CANDIDATE until flown). The SuitUIStateMachine has states "Vendor Suit Livery" and "Vendor
  Weapons Livery".
- **Not established:** what draws the showroom from the commander's camera in HMD mode, and the consumers of the
  projection.

## Flight F228 (2026-11-09 08:21, Quest 3 under VirtualDesktopXR, Steam, c25fac3e)

Build matched (`--expect-build c25fac3e`). The pilot: "the model is now behind me with the key turned on, and
stands over me with it off". Eye dumps: `082443795001` (key on) and `082506704002` (key off), both looking at
the UI. They show the LIVERY / GENERAL panel front-left, a "rotate" hint to its right, and no model in the empty
space where it belongs. `082529135003` and `082540518004` are the loadout browser, with the model standing to the
right of its panel, correctly.

- **The signal works.** `HumanoidLiveryActivity Show` at 08:23:47.860, the shop signal ON, and Close at
  08:25:16.502. The key changes at 08:24:07.9 (off), 08:24:15.7 (on) and 08:24:57.9 (off) each flip the signal.
  With the key on, F107 refuses every call (episode 4: 3630 refused, "away by: the shop screen"), with the game's
  camera 0.3 to 0.5 m from the neck.
- **The render camera** (`concourse frame: cam_local`, once a second):
  - 08:23:41-44: the showroom spot (6.0 20.0 -17.5), about 21 m from the neck;
  - 08:23:45-46: the preview camera (6.62 23.09 2.29), 3.9 m from the neck;
  - 08:23:47: the showroom spot once more, and Livery's Show;
  - 08:23:48 to 08:25:15: the commander's own position (4.8 22.67 3.28), with the key on and with it off;
  - 08:25:17-37 (the loadout browser): the preview camera again (6.72 23.10 2.30, F107 at 3.9-4.0 m).
- The first flight had the same sequence: 3.9 m at 06:39:58-06:40:02, then the commander's camera from 06:40:03.

So the model stands where the game puts it, at the commander, and Livery renders from inside it. The loadout
browser keeps the preview camera and is right.

- ruled out: H4 as the whole cause, because with EDVR's on-foot adjustments held off for the whole screen
  (3630 calls refused) the model is behind the pilot, not behind the UI.

## The Pimax visit: EDVR stood down there and did not on the Quest

**Corrected after F228:** this stretch's signature (a far showroom camera, then a 4.0 m camera that restarts F107
every 8 s) is the loadout browser's in the F228 flight (08:25:19-08:25:40), not Livery's. Read it as a loadout
browser visit. The comparison below does not stand as evidence about Livery.

The pilot's correct Pimax view was on the same Steam install (Pimax OpenXR, `edvr_gfx_20261109_045925.log`,
d5241a7b). Its showroom visit (05:04:06 to past 05:06:44) has the same opening as the Quest's: a far showroom camera
(39 m), then a camera 4.0 m from the neck. Then the two differ:

- **Pimax:** F107 episodes 6 to 19 each last 8.0 s with exactly one far call ("by the distance test 1") and about
  715 near calls. The game's camera was on the commander nearly all the time there too. But one 4.0 m read about
  every 8 s restarted the episode the moment the last one ended, so the body pin refused 712 to 722 calls in
  every 8 s and the raise stayed ramped out for the whole visit.
- **Quest:** the far reads stop at 06:40:02. F107's episode ends at 06:40:07 and the pin accepts every call (770 of
  770) for the whole bad view.

So in these logs the model is right while EDVR's on-foot machinery is held off, and wrong while it is engaged.
Whether the Pimax stretch was the Livery screen is not proven from the log (the pilot's account and the matching
signature say so). Why the game gives the 4.0 m read on one rig and not the other is not known; it may not be
per-headset at all.

## The A/B instrument (proposed earlier, now second)

A temporary key that makes the runtime answer Elite with the frustum widened to vertical symmetry: the short edge
(up, 0.90) raised to the long edge's tangent (1.33). The projection layer is submitted with the same widened FOV, so
the image maps one to one and the compositor drops the extra rows. Cost on the Quest: about 19% more rendered rows
while it is on. The `projection_query` lines already show m12, so the log proves the key took effect.

This test does not settle the fix. If H1 holds, there are two candidate fixes:
- the symmetric frustum only while a preview screen is open, which needs a signal for that screen;
- correcting the game's placement itself, which needs the anchor's code by RE.

## Ruled out

- ruled out: H2, the Steam install's `edvr.ini`, because the pilot's Pimax view with the model behind the UI was
  on the same Steam install and ini (2026-11-09). Today's Steam Pimax sessions (Pimax OpenXR) ran d5241a7b and
  a89e193e, a few concourse-cull commits before 65dc3884.
- ruled out: H1 in its strong form, the Livery placement code reading the projection, because the placement
  region 0x1268000-0x1276000 and the activity's open, close, start and stop read no projection element, FOV or
  HMD input (RE above). The model's place is authored node geometry.
- ruled out: the showroom camera distances (22 m, 39 m, 3.9-4.0 m) as a computed fit-to-view distance, because
  the camera nodes are authored scene data and the camera code has no FOV or tangent math.
- ruled out: the Steam ini's `openvr.head_offset_forward/right/up`, `openvr.head_yaw_degrees` and
  `fix.head_offset_gate`, because no source file reads them (only `tools\config_test` and docs name them).
- ruled out: a zero seated-to-standing transform, because Elite never called
  `GetSeatedZeroPoseToStandingAbsoluteTrackingPose` in this flight (the runtime log's `system_unavailable` lines are
  slots 23 and 26 only, not 12).
- ruled out: a floor-height difference between the runtimes' natural spaces, because both logs read `LOCAL in STAGE
  (0.000 0.000 0.000)` (STAGE coincides with LOCAL on both).
- ruled out: EDVR's on-foot view offsets as the cause, because they are centimetres (raise +7.0 cm, virtual neck 0)
  and the model is metres off. F107 kept the camera as the commander's throughout, which is correct: it is.

## F229 instrument (2026-11-09)

Read-only, no behaviour change; the lines start `livery camera (F229):`. Once a second per open screen it logs
the client's fields by name (new RE: the first draft's +0x340 and +0x344 were relative to the interface at
object+0x58): mode (+0x398 int, the setup mode), kind (+0x39C int: 0 node distance, 1 avatar-bounds distance,
2 none), step (+0x570 zoom step), dist (+0x580 current orbit distance), max (+0x590), min (+0x5A0), retry
(+0x5B0 byte, 1 = the game recomputes the distance on its next update) and the 16 floats at +0x5E0 as four rows,
with the length of the translation read as the last row and as the last column.

- Client offsets, from each constructor's `lea rcx,[object+X]` before the call to 0x126B670: suit and weapons
  buy screens 0x448, suit Livery 0x680, weapon Livery 0x630 (not 0x680), loadout browser
  (`HumanoidOutfittingInteraction`, watched only; its vtable slots +0x70/+0x78/+0x80 hold the shared Show,
  IsOpen and Close) 0xC0.
- The hook on 0x12714C0 (16 prologue bytes verified, no stack arguments, r9 unused) logs rcx, rcx - 0x58 as the
  object, r8, `*(int*)r8` and which watched screen's client that object is. The function has no direct caller:
  it is slot 3 of the vtable at client+0x58 (0x51AA4B8, stored there by the client constructor), so its rcx is
  client+0x58 and the line says "client +0x0" when the object is a watched client.
- No render-camera distance column: the concourse `cam_local` is anchor-local and not reachable from the hooks.
- Reading it: Livery against the loadout browser (correct) at the same second. (b) predicts dist and the
  translation near 0 (kind 1 with the avatar still empty) in Livery only, and retry 1; (a) predicts setup calls
  after the open with a different mode.

## RE 2 (2026-11-09, build 334925, inbox `20261109-101500-livery-camera`, not filed)

- No EDVR patch is on Livery's camera choice: no read of the gate byte or the PostGUI override dword in the camera
  classes (0x1060000-0x10B0000) or the Livery activity region (0x4780000-0x4890000). The SetMode detour acts on the
  local manager only, and the preview avatars are other managers. CONFIRMED by scan.
- No HMD, projection, IPD or render-size input on those paths: no call into the HMD wrapper or HMDService blocks.
- Livery, the weapon Livery, HumanoidOutfittingInteraction (the loadout browser) and both buy screens bind the same
  preview client (ctor 0x126B670) with the same mode byte 0.
- The model is a separate preview avatar: six new AMCs appear at the open with 0 parts (`native on foot avatars:`).
- The 334925 RVAs of EDVR's on-foot patches differ from the older ones in memory (gate byte 0x5E85B8A, override
  dword 0x5E85B94); none is in the camera or activity regions.

## RE 3 (2026-11-09, build 334925, inbox `20261109-120000-livery-retry`, not filed)

- **Offsets.** Setup 0x12714C0 is the client's interface slot at client+0x58, so its `this+0x340/+0x344` are the
  client's +0x398 (mode) and +0x39C (camera kind). The distance functions take the client itself.
- **The camera kind** (set in setup): modes 0-5 get kind 0, the node distance (0x12743D0). Mode 6 (carrier) gets
  kind 2, and mode 7 (station) gets kind 1, the avatar-bounds distance (0x12740A0), when a global flag is set.
- **Kind 0** sets dist = max = the camera node's distance d to the attach point, and min = d/2. The zoom cannot go
  under d/2 (about 2 m for the 3.9 m camera). Only kind 1 can produce a zero distance, from an empty avatar.
- **The retry byte +0x5B0** is read only by the client's per-frame update 0x1272780: while it is 1 the update
  recomputes the distance (kind 0 or 1); while it is 0 it only applies the zoom within [min, max]. Both distance
  functions set it to 1 when their input is not ready.
- **Consequence:** the planned fix (set the retry byte again when the distance comes out near zero) helps only
  kind 1. Suit Livery is almost certainly kind 0 (mode 1 or 2 is INFERRED, not read), so the fix was dropped
  before it was built.
- **Entry bytes for possible hooks:** 0x12740A0 `40 53 57 48 81 EC B8 00 00 00 | 48 8B 05 ...` (steal 10),
  0x12743D0 `48 89 5C 24 10 | 57 | 48 81 EC A0 00 00 00 | 48 8B 05 ...` (steal 5, 6 or 13), 0x1272780
  `4C 8B DC | 49 89 5B 18 | 56 | ...` (steal 7 or 8); rcx is the client in all three.

## Flight F229 (2026-11-09 09:44, Quest 3 under VirtualDesktopXR, Steam, 6591818b)

Build matched (`--expect-build 6591818b`). The pilot: rotating the model in the loadout browser seemed to snap
it to the right place. Both Livery screens are wrong: the suit model and the weapon are behind him. In the weapon
preview, before the weapon Livery, his view jumped up and back down before settling. Eye dumps `094729819001`
(suit Livery), `094842891002` (the weapon preview during the jump) and `094859700003`.

- **Two preview clients.** Every setup call (`livery camera (F229): setup call`) ran on one of two objects, and
  neither lies inside a watched screen object:
  - 0xBFB40080 is the loadout browser's (HumanoidOutfittingInteraction): mode 0, then 2 at the open and on
    every selection, and 3 for its weapon preview;
  - 0xBFB40700 is the Livery screens': mode 0 then 1 at the suit Livery's open (09:46:57.66 / 58.76), mode 0 then
    4 at the weapon Livery's (09:48:20.46 / 20.71). Once each; no further setup while open.
- **The render camera** (`cam_local`): the loadout browser at the preview spot (6.45 23.09 2.14) from 09:46:38;
  the suit Livery at the commander (4.83 22.66 3.26) from 09:46:59 to its close at 09:47:38.
- **Setup's nodes** (decomp of 0x12714C0, case 1 and case 2 share code): modes 1 and 2 both look up
  `Root!AttachPoint_AvatarCentre` and `Root!Camera_AvatarPreview` on the client's own scene (`param_1[0x32]`),
  kind 0. Mode 4 looks up `Root!Camera_Livery_<id>`, falling back to `Root!Camera_WeaponStart`; mode 3
  `Root!Camera_<id>`. So the mode does not decide it; the Livery client's scene, or what consumes that client's
  camera, does.
- **F229's per-second fields read zeros**: the screen objects hold a proxy at +0x680/+0xC0, not the client.
- **The weapon preview jump.** The loadout's weapon camera (mode 3) is about 2.5 m from the neck, under F107's
  3 m test. F107's episode ended at 09:48:39.68, and EDVR's on-foot adjustments re-engaged on that camera. The
  render camera then stepped between two heights 43 cm apart (09:48:40-46) before settling. EDVR's or the game's
  alternation: not split by these lines. Same class as the 11-01 suit shop (F208).

## Build split (2026-11-09 10:12-10:21; moved out of the Status block when F230 was built)

- **The split now:** the EDVR build (a6caa1b8 works; 65dc3884+ broken; between them 8 commits of concourse window
  glass, then F228 and F229) or the Steam install's `edvr.ini`. The game build is 334925 on both installs.
- **Steam, Pimax, F228 key off from the start (10:12, `edvr_gfx_20261109_101256.log`, 6591818b): broken.** Livery
  Show 10:15:30.6, setup mode 1 on the Livery client. Same headset as the working Frontier run, so only the EDVR
  build and the ini differ.
- **6591818b INSTALLED on Frontier (`--scope both`, verified, 2026-11-09 10:17; it replaced a6caa1b8; the Frontier
  ini untouched). NOT FLOWN.** Read with `--target <Frontier odyssey dir> --expect-build 6591818b`.
- **BROKEN on Frontier with 6591818b (10:18, Pimax, Frontier ini, F228 key off, `edvr_gfx_20261109_101830.log`):**
  Livery Show 10:20:55.6, setup mode 1. So the EDVR BUILD is the cause, not the ini. a6caa1b8 is an ancestor
  of 65dc3884; the regression is one of 16 code commits: the perf audit A1-A5 (main side: concourse shader
  precompile and pane scope, reports and UI timers, hotkey reads, F9 probe text, flat draw) and the concourse
  branch (VR bake record diff 8fcbec09/5507d8e7, glass census d5241a7b, glass by layout a89e193e, f293ccb3).
  During the broken Steam Livery (10:15:30-40) the concourse cull was off (anchor unseen) and the pane silent.
- **Next:** F8 Performance "Optimized stations" off (live, all concourse code), Livery twice. Right: concourse
  code. Wrong: the audit's non-concourse parts or a timing race; then build b8a71fdc (a6caa1b8 + audit, no
  concourse-branch commits) to split.

## RE 5 (2026-11-09, Opus, build 334925, inbox `20261109-112804-livery-activation`, not filed)

- The client is the CustomisationVisualCamera component (pooled 0x680-byte objects: the two clients are neighbouring
  pool slots). client+0x218 is the OutfittingEnvironmentLighting component, not a camera sink.
- **Activation.** Setup 0x12714C0 calls 0x126FE50 in modes 1-5 and 7 (mode 0 not). If byte client+0x5B1 is 0, it
  takes the LocationCameraCreator `lcc = 0x126FEE0(client)` and calls `lcc->vt+0x30` = SetCamera 0x1D7BEC0 with the
  client's camera name `*(char**)(*(client+0x50)+0x20)` and force 0 (bytes at 0x126FE6C:
  `48 8B 53 50 | 45 33 C0 | 4C 8B 08 | 48 8B C8 | 48 8B 52 20 | 41 FF 51 30`). Then client+0x5B1 = 1, selected or not.
- **Release.** 0x1D7C1E0 (`lcc->vt+0x38`) always selects "First Person Camera" (or "Cockpit Cam"), with force 0. It
  is called from the client's stop 0x1274880 (when client+0x5B1 is set) and from setup mode 6.
- **The HMD branch in SetCamera (CONFIRMED).** Force is r8's low byte. When the location camera's "HMD" entry is
  set and the name is not "StartCamera", SetCamera tries "HMD " + name. If that camera exists it is selected; if not
  and force is 0, nothing is selected (`74 78` at 0x1D7C0B5) and the current camera stays. In HMD mode a preview
  whose camera has no HMD variant cannot take the frame. Who sets the "HMD" entry: not established.
- client+0x2EC is byte 4 of the 8-byte setup request (the kind-0 orbit flag), not an activation switch.
- **Hypotheses (H8):** (a) Livery's client already has +0x5B1 = 1 and never asks; (b) the HMD skip: "HMD <Livery
  camera>" does not exist; (c) the loadout client's release lands after Livery's request. The flight timeline (the
  camera falls to the commander at the handover, Livery's setup 0.8 s later never moves it) fits (b) best.
- **Fix options** (none built): (1) from the Update hook, for a Livery client while its screen is open, if
  `lcc->vt+0x40(name, 1)` says the camera is not current, call `lcc->vt+0x30(name, 1)` (force); covers a, b, c.
  (2) patch 0x126FE70 `45 33 C0` -> `41 B0 01` (force 1 at every preview request) while EDVR forces HMD mode on foot;
  covers b only, for every preview client. (3) not recommended: NOP the skip in SetCamera (all HMD cameras).
- Same class, probably: the 11-01 weapon showroom that rendered from the commander's own camera.

## F230 (2026-11-09; moved out of the Status block when F231 was built)

- **F230 FLOWN, NO CHANGE (11:02, Steam, Pimax OpenXR, `edvr_gfx_20261109_110250.log`, e55a4f3a):** Livery's
  mode-1 setup reads `camera node valid` (11:05:15.716, 11:06:20.372), so the repair never fired, correctly.
  ruled out: H7 (an invalid camera node at Livery's setup), because the game's own node test reads valid after
  it. The render camera leaves the loadout browser's preview spot for the commander at 11:05:14, around Livery's
  Show (11:05:14.911) and before Livery's setup (11:05:15.716): Livery's client's camera never takes the frame.
  Open: how a client's camera becomes the render camera (candidates from RE 4: the pose step's enable byte
  client+0x2EC, unread; the camera sink at client+0x218 that Update pushes the pose to; whether Livery's client
  is updated at all).
- **State:** F230 (e55a4f3a, `v0.22.0-rc.7-278-ge55a4f3a`) INSTALLED on Steam and Frontier (`--scope both`,
  verified); its premise is refuted, and it is harmless (it acts only on an invalid node).
- **Lead H8b (RE 5):** a preview client asks the game's camera director for its camera by name with force 0
  (SetCamera 0x1D7BEC0). In HMD mode SetCamera tries only "HMD <name>" and, when that camera does not exist and
  force is 0, selects nothing, so "HMD First Person Camera" stays. The loadout client's release puts the first-person
  camera back at the handover; Livery's request 0.8 s later is dropped.
- **F231 fix (0b5665c0, `v0.22.0-rc.7-281-g0b5665c0`, BUILT, INSTALLED Steam and Frontier `--scope both`, verified,
  NOT FLOWN; the pilot chose RE 5's option 2):** `45 33 C0` -> `41 B0 01` at 0x126FE70, so every preview client's
  camera request is forced. It is applied only while the key is on and native on-foot VR holds the HUD override (the
  panel patch's condition). Restored otherwise and at shutdown. 21 bytes are verified, and the write is one aligned
  8-byte compare-exchange. SetCamera reads only r8b (capstone, the whole function). Log-only hooks on SetCamera
  (name, force, caller RVA, thread) and Release. TEMPORARY key `experimental.native_on_foot_preview_camera` (on,
  live, F8 Experimental "Preview screens use their camera").
- **Next flight (F231, `--expect-build 0b5665c0`, either install, either headset):** loadout browser, then the suit
  Livery (expected beside the panel; INSERT), the weapon Livery and the weapon preview, and a Pioneer Supplies buy
  screen if near one. A/B: F8 "Preview screens use their camera" off, reopen the suit Livery (expected broken).
  Read `preview camera (F231): SetCamera ... camera name, force` and the patch lines. Earlier instrument builds: F228 (Livery hold, A/B key) and F229 (read-only camera
  instrument, 6591818b), both flown.
- **Cause (H7, RE 4, inbox `20261109-140000-livery-resetup`):** setup resolves the camera node into client+0x478
  once, with no check or fallback for modes 1 and 2. Nothing re-resolves it, and with an invalid node the pose
  step writes an identity pose, so the camera sits at the scene origin (the commander). Livery runs setup once at
  open; the loadout browser re-runs it on every selection. The node being invalid at Livery's setup is
  CANDIDATE until F230's setup line shows it.
- **F230 fix:** a hook on the client's Update 0x1272780 re-runs the game's own setup (rcx client+0x58, rdx
  client+0x390, r8 a copy of the original qword) on the setup's thread, when a Livery screen is open, at least
  0.5 s after the last setup, and the game's node test (0x3175B40 on client+0x478) reads invalid. At most 5 tries
  per open. TEMPORARY key `experimental.native_on_foot_livery_camera_repair` (on by default, live, F8
  Experimental "Livery camera repair"; off logs the evidence only).

- **Cause (lead H8b, RE 5):** a preview client asks the game's camera director for its camera by name with force 0
  (SetCamera 0x1D7BEC0). In HMD mode SetCamera tries only "HMD <name>" and, when that camera does not exist and
  force is 0, selects nothing, so "HMD First Person Camera" stays. The loadout client's release puts the
  first-person camera back at the handover; Livery's request 0.8 s later is dropped.
- **F231 fix (0b5665c0, `v0.22.0-rc.7-281-g0b5665c0`, INSTALLED Steam and Frontier `--scope both`, verified; the
  pilot chose RE 5's option 2):** `45 33 C0` -> `41 B0 01` at 0x126FE70, so every preview client's camera request
  is forced. It is applied only while the key is on and native on-foot VR holds the HUD override (the panel
  patch's condition), restored otherwise and at shutdown. 21 bytes are verified, and the write is one aligned
  8-byte compare-exchange. SetCamera reads only r8b (capstone, the whole function). Log-only hooks on SetCamera
  (name, force, caller RVA, thread) and Release. TEMPORARY key `experimental.native_on_foot_preview_camera` (on,
  live, F8 Experimental "Preview screens use their camera").
- **Next flight (F231, `--expect-build 0b5665c0`, either install, either headset):** the loadout browser, then the
  suit Livery (expected beside the panel; INSERT), the weapon Livery and the weapon preview, and a Pioneer
  Supplies buy screen if one is near. A/B: F8 "Preview screens use their camera" off, reopen the suit Livery
  (expected broken). Read `preview camera (F231): SetCamera ... camera name, force` and the patch lines.

## Status history (the Status block before F232's rewrite, 2026-11-09)

- **F231 FLOWN, NO CHANGE (12:08, Steam, `edvr_gfx_20261109_120828.log`, 0b5665c0).** The patch was applied
  (12:10:07.779), and Livery's client asked for "Exhibit Cam" with force 1 (12:10:45.117), the SAME camera the loadout
  browser asks for (12:10:36.376). ruled out: H8b, because the camera name and force are not the difference.
- **ROOT CAUSE LEAD (H9, EDVR's own code):** `native_preview_camera.cpp` / `native_preview_camera_core.h` (10-26,
  written for the ship preview). In HMD mode the game's camera manager has two equal-weight producers on its
  "Location" camera, the HMD activity and the CustomisationVisual preview, and its first-maximum finalizer keeps
  the HMD one, discarding the preview. EDVR writes the preview's pose at finalization, but only for a Location
  child that passes `identity()` exactly: count == 2, HMD in slot 0, a preview in slot 1, both pointing back, the
  weights. In this flight the loadout browser qualified on every frame (HMD/preview-qualified and applied rose to
  1772), and during Livery (12:10:45-12:11:16) preview calls went on (2599 -> 4398) while HMD-qualified,
  preview-qualified and applied stayed at 1772. Livery's Location child fails `identity()`, so the game's HMD
  camera wins. Which check fails is not logged. CANDIDATE: a third producer (the loadout's preview still
  registered at the handover), or Livery's preview not in slot 1. That would also explain the intermittency (one
  good run on a6caa1b8) and the RC7 report (the feature predates RC7).
- **State:** F232 BUILT and INSTALLED, NOT FLOWN: 5310d7e9 (`v0.22.0-rc.7-282-g5310d7e9`), Steam and Frontier
  `--scope both`, verified. F231's patch is moot (propose reverting it at the arc's close).
- **F232:** `identity()` names the first failing check (a reason enum). While a watched preview screen is open, a
  wait-free roster snapshot is logged once a second per role: `native preview camera (F232): role HMD|preview
  refused: <reason>; handover on|off; roster: ... entry i: ...`. TEMPORARY key
  `experimental.native_on_foot_preview_handover` (on, live, F8 "Preview handover"). With it on, a Location child
  with 3 to 6 producers is accepted when exactly one is the HMD entry, the rest are previews, and the last preview
  is used; a count of 2 keeps the original checks. Off is the original `identity()`.
- **Next flight (F232, `--expect-build 5310d7e9`, either install):** first F8 "Preview handover" OFF, loadout browser
  then suit Livery (broken; the refusal lines name the check and the roster). Then ON, reopen the suit Livery
  (expected beside the panel; INSERT), and the weapon Livery. Earlier builds on the branch, all flown: F228 (Livery hold,
  A/B key), F229 (read-only camera instrument, 6591818b), F230 (e55a4f3a, the node repair, harmless; it acts only
  on an invalid node, and the node reads valid).
- **H8b and F231 (history):** RE 5 found SetCamera's HMD skip; F231 forces every preview camera request
  (0x126FE70 `45 33 C0` -> `41 B0 01`, TEMPORARY key `experimental.native_on_foot_preview_camera`). Flown above: no
  change. Details in "RE 5" and the moved F230/F231 notes below.
- **ruled out: H7** (an invalid camera node at Livery's setup), because F230's flight (11:02, Steam, Pimax,
  `edvr_gfx_20261109_110250.log`) reads `camera node valid` after Livery's mode-1 setup (11:05:15.716, 11:06:20.372).
  There the render camera left the loadout preview for the commander at Livery's Show (11:05:14.9), before its setup.
- **Report (the pilot, 2026-11-09):** in the on-foot Livery screens (suit and weapon cosmetics) the preview sits
  over or behind the viewer instead of beside the UI.
- **Established:**
  - Broken on the Steam install on BOTH headsets (Quest 3 VDXR and Pimax OpenXR) with 65dc3884, c25fac3e and
    6591818b. So it is not per-headset: ruled out the frustum (H1) and every other headset input (H3).
  - WORKS on the Frontier install (Pimax, a6caa1b8, Frontier's ini, 10:04): the game's camera stays about 4 m from
    the neck, the model beside the panel (dump `100703552001`).
  - When broken, the game renders Livery from the commander's own position after about a second on the 4 m
    preview camera. The bug follows Livery's own preview client (setup modes 1 suit / 4 weapon). The loadout
    browser's client (modes 0/2/3) is right. Setup gives modes 1 and 2 the same scene nodes.
  - F228's hold changes only which wrong view (model behind with it, over without it).
  - No EDVR patch and no HMD input on Livery's camera code (RE 2). The retry-byte fix does not apply (RE 3).
- **Build split (2026-11-09 10:12-10:21):** Frontier with 6591818b and the Steam install both broken on the Pimax, so
  the EDVR build mattered for that one good run, not the ini. A user reported the bug on RC7 (eedd5d04, 11-05),
  older than a6caa1b8, so the one good run reads as a race won, not a fixed version. The bisect was stopped on the
  pilot's word; details in "Build split".
- **Side finding:** the loadout weapon preview's camera (mode 3) is about 2.5 m from the neck, under F107's 3 m
  test, and the view jumps 43 cm when F107 lets go (09:48:40; F208 class). Candidate fix: hold F107 while the
  loadout browser is open.
- **RE:** four inbox notes, not filed (`20261109-090000-livery-anchor`, `-101500-livery-camera`,
  `-120000-livery-retry`, `-140000-livery-resetup`).
- **Builds:** run in `C:\wt99` (branch `claude/f228-short-build`). The long-path `launcher_test` failure is fixed
  on main 51201072. Remove `C:\wt99` when done: take its `third_party` junctions out first.
- **Temporary keys:** `experimental.native_on_foot_livery_game_view` (on|off, default on, live, F8 row "Livery
  uses game view") and `experimental.native_on_foot_livery_camera_repair` (F230, on|off, default on, live, F8
  "Livery camera repair"). When the arc closes, the winning values become the behaviour and the keys go (ask first).
- **Ruled out:** the "Ruled out" list and the lines in each flight section.
- **Issue:** none yet.

## RE 6 (2026-11-09, Opus, build 334925, inbox `20261109-132023-weapon-head-producer`, not filed)

- X, the weapon screens' head-side producer, is **HeadlookCamera** (string "HeadlookCamera", iface slot 0 0x10872C0;
  HMDCamera's is 0x1087260). It is the head-look sibling of HMDCamera: same base and layout, with object vtable
  0x518C2E0 (HMDCamera 0x518C8B0), ctor 0x10618B0, start 0x109B320 (reads HeadlookCamYPR/Fov/RelDisp/AbsDisp),
  stop 0x1089C00.
- Its producer is 0x1074650, reached through thunk 0x1031EE0 (`LEA RCX,[RDX-0x150]; JMP`). HMDCamera's is
  0x1074140 via 0x1031D10. Entry bytes `48 89 5C 24 08 | 48 89 74 24 10 | 57 | 48 83 EC 60 | 48 8B 81 40 01 00 00`;
  pin 15. A second per-frame listener (object+0x168 -> 0x107F170) writes its look rotation and offset at
  +0x250..+0x288.
- Same pose layout as the HMD role: primary at entry+0x50, local at +0xD0, back-pointer at +0x120, enabled byte at
  entry-4. Both producers start primary = predecessor+0x40 (the base). HMDCamera sets primary = head x base and
  local = the raw head, and hands the base on. HeadlookCamera sets primary = R x base + d, applies the same to its
  local (not a head pose), adds its own FOV, and makes no hand-off. With idle look input its primary equals the base
  (INFERRED), so EDVR's `primary != base` HMD test would never pass for it.
- Weight and state are the same as HMDCamera's (0x8BEA20, 0x5294E0). The finalizer's mode-0 loop
  (0x1071ECF..0x1071F38) is a strict first maximum in roster order (`CMPLTSS` at 0x1071EF1) and copies the winner's
  state to child+0x70, so child+0xB0 is the winner's entry+0x50. With [preview, X] the preview wins.
- CONTRADICTED in part by F232b's flight: the RE says a preview-first roster can never pass EDVR's
  `output == h.primary` write, but applied rose by 705 across the 13:05:18-13:05:29 Livery (Frontier,
  `edvr_gfx_20261109_130228.log`). Either that Livery's roster was head-first, or the loop's order reading is off.
- Proposed: add HeadlookIface 0x518C350 and HeadlookObject 0x518C2E0; generalise the head entry to either class;
  extend the weights check; replace `primary != base` for Headlook; in the finalizer, compare the output with the
  first-maximum winner's primary; hook 0x1074650 into `hmd()`. First an instrument at finalization (child+0xB0,
  preview +0x50, X +0x50, base, X +0x250..+0x288) and in the eye stub 0x283FA20 (the head and its distance to
  each), because the eyes may follow a head copied before the finalizer.