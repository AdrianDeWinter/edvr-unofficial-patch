# Concourse cull: on foot, the station outside the bar is not drawn (2026-11-05)

## Status

- **State:** Flight 9 flown 2026-11-06 flat on Steam, log `edvr_gfx_20261106_092241`, build 4eead728. Flight 10 flown 2026-11-06 on 990a2b44, log `edvr_gfx_20261106_095708`: Sean asked for brighter skylights, dimmer bay windows, a middle level elsewhere, and the shop window to clear like the bay. Flight 11 flown 2026-11-06 on 481d97b0, log `edvr_gfx_20261106_110851` (results below; the log hit its size cap at 11:22:26, so the trip back is lost). Flight 12 flown 2026-11-06 on f09f42df, log `edvr_gfx_20261106_114951`: results below. Flight 13 flown 2026-11-06 on eb9b6b8e, log `edvr_gfx_20261106_130939`: the doors can be read. Flight 14 flown 2026-11-06 on a3f17170, log `edvr_gfx_20261106_134043`: the lift cull reaches the cap on the hangar side and barely helps in the concourse. Flight 15 flown 2026-11-06 on 9a1d039f, log `edvr_gfx_20261106_141917`: the lift cull now works both ways, the view masks do what RE said, and they save less than the census suggested. Flight 16 flown on b4f82839, log `edvr_gfx_20261106_150756`: keeping by size refuted. Flight 17 flown on a2fca02e: Sean, of a capture under a skylight, "This looks like the right idea, but if I shift my position, it moves out of alignment. Also not sure what the white dots are?" f50a46a2 flown, log `edvr_gfx_20261106_184018`: two F6 captures; Sean, "it looks decent when stationary, I think with some glass/tinting effects it could work effectively if we capture the whole cube so there's some parallax". He did not step F9, so H43 is not answered. Build 2a140a30 flown, log `edvr_gfx_20261106_192543`: automatic capture refuted. Sean, with a clip: "this is what I was afraid of, I think we need to capture the station as a simple textured geometric shape once". See "A baked shell". Build bad4cf70 flown 2026-11-07, log `edvr_gfx_20261107_032222`: the ride camera test (Flight 20): the view slot follows and the constants do not. See "Flight 20 results". Build 1bba08e9 flown 2026-11-07, log `edvr_gfx_20261107_042506`: the second camera test (Flight 21): the VR camera path does not run in flat, and the log names flat's. See "Flight 21 results". Build ac4ab507 flown 2026-11-07, log `edvr_gfx_20261107_050539`: the third camera test (Flight 22): the whole picture follows a camera moved where flat makes it, so a chosen camera works in flat. See "Flight 22 results". Build 19881a34 flown 2026-11-07, log `edvr_gfx_20261107_060254`: the look test (Flight 23): the station mostly whole from the room's centre, at 7.2 to 8 ms a frame. See "Flight 23 results". Build c1f3ca32 (the source of 61b8e9f6, with a doc commit on top) flown 2026-11-07, log `edvr_gfx_20261107_064844`: the look on a ride up (Flight 24): the station is whole and lit from the first frame the clip caught at arrival, on a second ride. See "Flight 24 results". Build 5cdb2575 flown 2026-11-07, log `edvr_gfx_20261107_072457`: the first bake at arrival, on a true first ride: the real station in the real windows, "pretty good for a first attempt, with some smearing and artifacts". See "Flight 25 results". Build 31720543 flown 2026-11-07, log `edvr_gfx_20261107_081242`: the hold down to 141 ms, and "basically almost done, just need to hide this rear seam better". See "Flight 26 results". Build 93a3e4f8 flown 2026-11-07, logs `edvr_gfx_20261107_083829` and `edvr_gfx_20261107_084626`: the near write never reached `cb1`, every capture 0.025 m. See "Flight 27 results". Build f14a8134 flown 2026-11-07, log `edvr_gfx_20261107_093300`: the near value took and the automatic bake ran, but two directions were never captured: "Looks like the bake regressed". See "Flight 28 results". Build cf61f513 flown 2026-11-07, log `edvr_gfx_20261107_095540`: all five directions, "much better", motion still poor. See "Flight 29 results". Build edbc145c flown 2026-11-07, log `edvr_gfx_20261107_102412`: Sean, "Sphere looks best", and some bar tables flicker, then stay solid. See "Flight 30 results". Build 0448d071 flown 2026-11-07, log `edvr_gfx_20261107_105506`: the tables are fixed with `Root` and `Perturbator` kept; the lighting is blown out at the lift bank and not a few metres in. See "Flight 31 results". Build c32cd19b flown: both names on the allow-list, the key out of Sean's `edvr-flat.ini`, `hotkey.concourse_light_test` on HOME in both Steam inis, and no light family is the pale look. See "Flight 32 results". Build baac9204 flown 2026-11-07, log `edvr_gfx_20261107_115242`: leaving out the light cones `2519C905` takes the pale look away. See "Flight 33 results". Build 8e317e3f flown 2026-11-07, log `edvr_gfx_20261107_121643`: they are withheld, "That worked"; and at a surface port the sun went off with the pad and stayed off. See "Flight 34 results". Build 073e8dac flown 2026-11-07, log `edvr_gfx_20261107_125458`: the sun stays on at a surface port, and F3 with the cull off names every record: the room has its own anchor and the keep list parts it from the settlement. See "Flight 35 results". Build c8c23caf flown 2026-11-07, log `edvr_gfx_20261107_134444`: the port's cull and bake work behind F2, "Looks great", with glass behind glass flickering and the cull coming on in the hangar. See "Flight 36 results". Build 6813f060 flown 2026-11-07: both fixed, "Flight is clean". See "Flight 37 results". A `CraterOutpost` now runs with no keys (Flight 38). Build 84f55c1e flown 2026-11-07, log `edvr_gfx_20261107_145834`: the outpost's room behind F2, "Outpost looks good, bake it". See "Flight 39 results". An `Outpost` now runs with no keys too (Flight 40), and a fleet carrier's room is known behind F2: build 7738c48c flown on the carrier, log `edvr_gfx_20261107_160109`, "mostly good", the bridge's windows wrong. See "Flight 41 results". The bridge is now left to the game: build 975cb8e0 flown 2026-11-07, log `edvr_gfx_20261107_171641`, "Carrier looks good, bake it". See "Flight 42 results". A fleet carrier now runs with no keys, known from Status.json on a load on foot, with a new bake after a jump: build f88df803 flown 2026-11-07, log `edvr_gfx_20261107_184209`, with the rooms survey for the bulkhead doors. See "Flight 43 results, and the rooms survey". The jump is not flown. The rooms ceiling test, f081d22b, flown 2026-11-07, log `edvr_gfx_20261107_193601`: at least 1.6 ms in the elevator bay, 0.7 in the central space, 0.3 on the bridge, every spot at the cap with it on; and the bridge's floor went with it. See "Flight 44 results: the rooms ceiling". Flight 45 (same log): the floor inferred to ride in a rig; the bar's navy is the bake's (H54), not Elite's. The door rule with its first-move watch, ed2627b9, flown 2026-11-08, log `edvr_gfx_20261108_040806`: every opening began within 4.6 m of the door, the watch costs 1.2 to 3.2 us a tick, so reverse engineering Elite's door state is not needed. See "Flight 46 results". After it a carrier's windows show the real sky with no bake (ships and the hull culled until a window clears), and the door rule has its own switch, `fix.skip_rooms_behind_doors`: c841a58b installed on Steam 2026-11-08, not flown (Flight 47). See "The real sky at a carrier, and the doors' own switch". Sean also saw the bridge's side windows stay on the bake when the main one cleared: other meshes (n=12 beside n=54), and the clear lifts one at a time; on the bridge this change covers it, elsewhere it stands. Sean's goal for the windows, 2026-11-06: retain as much of the cull's saving as possible while making the view through them the best. The lift cull is paused at his word: it stays on, with its last change, a release 5 s after shutting. Not merged. Extends `fix.optimized_stations`; the concourse cull is off unless the temporary keys turn it on, but the sealed-hangar withhold is on with `fix.optimized_stations`.
- **Goals (Sean, 2026-11-05):** (1) on foot in the large-station concourse, cull everything outside the bay windows; (2) skylights and the window above the lift bank become a permanent mirrored surface, so missing structure is never seen; (3) approaching the bay windows clears the mirror and stops the cull, so the station interior is visible from there. **Added 2026-11-06:** (4) light behind the mirror, for the impression of a brightly lit station beyond it; (5) with `fix.optimized_stations` on, the same outside draws go while the hangar is sealed.
- **Direction:** identify records by the store's name, keep an allow-list, and make the panes opaque. The fitted hall, the shader-pair lists and the shell read-back from the earlier attempt are not carried over (see Ruled out).
- **System Map with most bodies missing while docked (issue 203, 2026-11-17):** cause found in two logs, fix built, NOT FLOWN. A sealed hangar withheld the planet, sun and atmosphere shaders (`kWithheldVs`) while the System Map was open, and the map draws its bodies with the same ones. `updateArmed` now issues everything while GuiFocus is 6, 7 or 8. See "2026-11-17: the maps" at the end.
- **Answered by Flight 1:** H1 yes, with named survivors. H2 yes. H3 yes for the bay window: VS `98397963AAEC45D3` / PS `66B08F89E4926C01`, n=36. H5 yes by eye: the room stayed lit. See "Flight 1 results".
- **Answered by Flight 2:** H4b yes, by eye. H7 yes: lifts at 20.2-22.3% of the view, all near the bay. H6b no: the family covers every pane in the room at about 20 index counts, so no count names the exterior. See "Flight 2 results".
- **Answered by Flight 3:** H9 yes as far as the eye went: Sean called the mirror much better and reported no wrong pane, with 0 mark refusals. H10 yes: `culled PlantStack0-4` logged, and his screenshot has no plates. H11 not asked directly: no complaint about the grid, and the log shows `mirror-soft` tried. The side table needed one fix: one mesh drawn in two places flipped within a frame. See "Flight 3 results".
- **Answered by Flight 4:** H14 yes: Sean, "the turn away from the bay windows works well". H12 half: "I don't see anything missing or incorrect on the interior", no planet in his screenshot or his report, and the lists matched 15 of 24 families, but the haze and lasers stayed. See "Flight 4 results".
- **Answered by Flight 5:** H15 yes: `beams and bolts` is no longer on Sean's list, no lasers. H16 half: the wash went with `scatter`, so that is what it was; the ellipses and wedges stayed, so they were not the particles. H13 still open: `flares` matched and no flares in the screenshot; the room was not reported.
- **Answered by Flight 6:** H18 yes: Sean, "the green haze and light fills are gone", with `far glows limited` at about 8000 a second. The room was not reported wrong.
- **Answered by Flight 8:** H23 (a): Sean, "the bright washed out look stays on when F2 is off". Elite's own, and not this arc's.
- **Answered by Flight 9:** H24 half: Sean likes `mirror-brightest`, the best so far, but the pass brings the sun's moving shadows with it. He chose a steady light instead.
- **Answered by Flight 11:** H27 yes by eye: the skylights are the bright ones, up 0.866 -0.500 0.000. H28 half: the bay clears, a skylight looked at directly stayed mirrored with no `clearing pane` line, but the shop did not clear, at 7.3 to 8.3% of the view against 20%. H29 half: the cut is logged going up (784 m), and the way down fell past the log cap. H26 replaced by H30.
- **Answered by Flight 12:** H30 yes: Sean, "bar windows look great". H28 yes for the shop: "Window cleared at the ship services counter". H31 yes: frames in the cabin going down match the view across the room from there, and rise when the doors open on the hangar side.
- **Answered by Flight 13:** H32 yes: the door records move in `c`. H33 yes: the cabin record's distance tells inside from outside. See "Flight 13 results".
- **Answered by Flight 14:** H34 half: going up the ride sat at the cap (6.94 ms), and going down it read 11.8-12.2 ms against 12.2-15 ms. Sean: "No issue on the way down, but saw a small pop on the way up when the doors opened."
- **Answered by Flight 15:** H35 yes: going down the ride read 8.47 ms against 11.8-12.2, with 83% of rigs vetoed. H37 yes, with a smaller gain than expected: see "Flight 15 results".
- **Open hypotheses (the next flight answers them):**
  - **H63, answered by Flight 57:** the Orbis's glass is drawn with the family's vertex shader and another pixel shader, `500D228FA85C483C`, on the immediate context. The family's `66B08F89` was not drawn at all there, with the cull on or off. See "Flight 57 results".
  - **H64, confirmed by Flight 58:** Material Quality and Spot Shadows choose the glass's pixel shader, and Directional Shadows does not. The pane family matched only Material Ultra with Spot Shadows at High or Ultra. Five pixel shaders in the two dumps have the glass's layout. See "Flight 58 results".
  - **H65, confirmed by Flight 59** (a89e193e): a pixel shader with the glass's layout, checked once at its creation, is the glass at any setting. Four were learned as the settings changed, and each was treated. Sean: "it looked good at all levels". Material Medium is not in the log. See "Flight 59 results".
  - **H62** (Flight 52's hole in the VR bake): the room's upper structure, 20 to 50 m from the centre, is drawn on some of VR's bake frames and not others, with the camera unchanged. H58, H59 and H61 are ruled out by Flight 53. The union of a face's three frames covers it (Flights 53 and 55), and Flight 55's draw diff names it as one mesh, VS `EB5234DB6ADB491D` and PS `CB9F297EFF264251`. Its record, and why the look passes it on some frames, are open. See "Flight 55 results".
  - **H36** The small pop on the way up, when the doors open, comes from the lift cull. Unknown: Sean had not said what popped or whether earlier rides had it. One candidate: the pane table is forgotten on each visit, and its first half second is visible as the doors open (`changed side` lines at 13:44:16.7).
  - **H38, refuted for the name list:** Sean, "The keep list leaves out large sections of the station interior". `concourse_keep` cost nothing measurable (13.5-13.7 ms against 13.3-14.7 ms with no list and 12.8 ms with the flat fill, one spot, a few seconds).
  - **H41** A view captured from a frame with the cull off behind opaque panes keeps nearly all of the cull's saving and looks like the station. By direction alone it read right at one spot, at no measurable frame cost (8.62-8.70 ms against 8.62 ms), and slid when the camera moved.
  - **H42** The white dots are texels the forward splat left empty, where the cube's density exceeds the screen's, showing the fallback (4.0 on a skylight). Filling from the mips removes them. Confirmed or refuted by eye.
  - **H45** A shell baked once from an unobstructed render looks right and costs about one small draw. It rests on a chosen camera during a lift ride, untested. The smallest test is in "Rendering from the concourse's centre"; Flight 20 flies it.
  - **H46, refuted by Flight 20 for this hook point:** moving view 0's camera before `0x1EA9C0` reaches the view slot and not `cb1[275]`, held or not. Sean, on all three rides: "nothing moved". A write further upstream is H48.
  - **H48, refuted by Flight 21:** `0x283FA20` and `0x2845730` never run in flat, so nothing was written. View 0's camera comes from the call at `0x287D95D`, in `0x287D5A0`.
  - **H49, confirmed by Flight 22 for the picture:** moving the camera on `0x281D190`'s stack after `0x2874290` returns moves `cb1` and the image, with no fault and nothing odd reported from the game. Whether the per-view cull follows the turn exactly has no census behind it yet.
  - **H50, half answered by Flight 23:** Sean, "the station appears mostly whole". Lighting and detail not reported yet; cached shadows, streaming and exposure after a jump not measured. One of the six directions showed only the skybox, and NPCs were left, see "Flight 23 results".
  - **H51, confirmed by Flight 25:** on a true first ride the station can be baked from the room's centre as the lift arrives, and it shows in the windows when the doors open. One ride, one station.
  - **H43** A cube keyed by position around a fixed origin, with distance stored, keeps the view aligned as the camera moves and lets captures from different spots agree. It rests on v2 being relative to `cb1[275]` and on `cb1[279]` being the view Z axis, both INFERRED: a wrong row shows as the whole image swimming in the sphere and depth modes, not in the off mode. The three modes on one key compare them in a flight. 14 cube samples a pixel in depth mode, against about 7 in the others; about 1.4 GB. F4 against `mirror-steady` gives the cost.
  - **H40** The black flash Sean sees as the doors open (H36's pop) is the destination room's rigs catching up after a ride vetoed. The release at 5 s tests it, but the lift work is paused, so this is not being chased.
  - **H30** The two bands on the bar and shop windows are the glass shader's reflection, a horizon at eye level, and a flat fill with no glass shader removes them. Evidence: the mark covers 81 to 91% of those panes (n=30 drawn 686924, mirrored 627855), though the lower band is about a third of the pane. Flat fill also makes each pane one draw instead of two.
  - **H31** The lift's frame rate is the room behind the closed cabin. 12.7 to 13.2 ms from 11:22:22 in the cabin with the cull on, as the lift bank was in earlier flights (12 to 13 ms). Census 2: at least 5932 main-view draws, against 3974 standing in the room. Discriminator for whether the cut's change costs anything: `walk survey` frame means in the first 5 s after a cut on the way down. Earlier builds, with the cull still on for those 5 s, read p50 6.95 ms there and 7.17 ms after it.
  - **H27** The glass's lighting normal and the anchor's axes share a frame, so `abs(dot(n, up))` finds the skylights. Unproven: the log's `concourse pane: glow levels ... up X Y Z` gives the axis, and the eye says whether skylights are the bright ones. If every pane looks the same, this is refuted or `up` was not read.
  - **H28** A window that fills the entry share and reads exterior can clear like the bay. `concourse pane: the clearing pane is now n=N (share S%)` and the `concourse clear:` lines with n say which opened. Known risk: looking straight up at a skylight can clear it too, since no camera direction is available here. The same n opens together.
  - **H29** The lift is one camera cut, so the cull ends on it. `concourse cull: off (the camera cut to another room)` confirms. `the anchor record was not seen for 5 s` after a cut means a late sighting re-armed the anchor.
  - **H26** A flat colour added over the mirror, with no light or shadow in it, reads as a bright station behind the glass and stays still as the station turns. It costs almost nothing, a white pixel shader in place of a third full pass. The scene's HDR scale is unmeasured, so F4 steps 0.25 to 4 around a guess of 1. `mirror-brightest` is on F4 to compare.
  - **H25** The families marked `sealedToo` (lasers, plumes, trails, smoke, the planet, its atmosphere, the sun's glare) draw nothing a sealed hangar needs. No census has been taken in a sealed hangar, so this is by reasoning. Flares, the scatter, the particle batches and engine glows stay for that reason. `concourse withheld 30s:` says what matched there.
  - **H22** The hull is a nameless record. Not reported in Flight 8. If it still draws, an F3 press names what is kept.
  - **H20** and **H21** The ribbon: ask whether either trail is left.
  - **H6** `Standard_SocialArea` marks the large-station concourse at other stations too. Still unflown.
- **Temporary keys (all removed when the arc closes):** `diag.concourse_cull` (on, off), `diag.concourse_keep` (extra name prefixes), `diag.concourse_pane_mode`, `diag.concourse_panes`, `diag.concourse_clear_share`, `diag.concourse_mirror_beyond`, `diag.concourse_glow` (sky, angled, other, bay), `hotkey.concourse_cull`, `hotkey.concourse_pane_capture`, `hotkey.concourse_pane_mode`, `hotkey.concourse_view_test` (now the backdrop's capture), `hotkey.concourse_backdrop_parallax`, `diag.concourse_backdrop_radius`, `diag.concourse_glass`, `hotkey.concourse_camera_test`, `diag.concourse_bake_near`, `diag.concourse_bake_dirs`, `hotkey.concourse_room_test` (the door rule's step, numpad `/` in both Steam inis).
- **Hangar to concourse and back (Sean, 2026-11-06):** steps 1 to 3 built, not flown; the seal on foot waits on the survey's numbers. See the section of that name at the end.
- **Ruled out:** see the section of that name. **Issues:** none yet.
- **Answered by Flight 20:** H47 yes: the game writes this camera every frame before `0x1EA9C0` (284 of 284 held writes overwritten), so a write need not be put back.
- **2026-11-16, a user's bar windows:** one bar pane cleared while the panes beside it kept the bake. The clear now fades all the outside glass together and is judged on its total share of the main view: 25e49f5d, FLOWN OK from a flat launch (log `edvr_gfx_20261116_120304`), merged to main. A session started in VR still gets no flat bake. See "A user's bar windows, and the clear on all the outside glass".
- **Next flight:** none set. Flight 59 flown on Steam's a89e193e, log `edvr_gfx_20261109_055831`: the windows were treated at every setting stepped, in VR and in flat (see "Flight 59 results"). The H63 glass census is removed, at Sean's word (see "The glass census for H63"). Flight 58 flown on Frontier's a6caa1b8, log `edvr_gfx_20261109_052109`. Steam's `edvr.ini` is back as it was before Flight 58. Flight 57 flown on d5241a7b, log `edvr_gfx_20261109_045925`: since a settings change, the Orbis's glass draws with pixel shader `500D228FA85C483C`. H62's record data from Flight 56 is not yet read. See "Flight 57 results".
- **Merged to main 2026-11-08** as 649c4639 at Sean's word, after the cube dump was removed and flat's bake was guarded on the camera's producer hook (see "Before the merge to main"). The next flight is whatever main carries. Flight 55 flown on e9e3e1f3, log `edvr_gfx_20261108_130757`: the VR bake works (every face covered, every dump unwritten 0.0%), and the draw diff names H62's band as one mesh (VS `EB5234DB6ADB491D`, PS `CB9F297EFF264251`). Open: a flat bake in a session started in VR (the flat camera hooks are not bound), and VR's re-bake right after a switch back (it starts before the tone draw returns). See "Flight 55 results". Flight 54 was on e08534eb: Flight 54 flown on e08534eb, log `edvr_gfx_20261108_124344`: the fence refused every call, so three bakes captured nothing. See "Flight 54 results". Flight 53 flown on d7189728, log `edvr_gfx_20261108_104337`: Sean, "that seems to have fixed the issue". But a band of the room's upper structure comes and goes between frames (H62), and the third bake published face 1 with it unwritten. Proposed: write every dwell frame, so the cube keeps whatever any frame showed past the cut, and log which draws differ between a face's frames, to name the band's source. The instrument's dwell, counters and cube dump are TEMPORARY and go when H58 to H61 are settled. Flight 52 flown on 194805e2, log `edvr_gfx_20261108_092319`: the VR bake runs by itself and lands within 7 mm, with a pale hole where the far structure should be. H58 to H61 are open (see "Flight 52 results"). Flight 51 flown: the camera route, the capture point and the hold all work; the per-eye reader does not. Flight 50 flown: the hold works (replayed 15, empty 0), the camera probe refused the head's stack address, and the tone draws were found with no eye. Flight 49 flown: the bake's fix holds. New temporary keys: `hotkey.concourse_vr_hold_probe` (numpad `*`), `hotkey.concourse_vr_camera_probe` (numpad `-`). An `AsteroidBase` and a `Dodec` now run with no keys in flat. Flight 47 (the carrier in flat) and Flight 40 are still to fly. Then Flight 40 (an `Outpost` and a `CraterOutpost`, no keys), both on c841a58b: `python tools\edup_log.py --target steam --expect-build c841a58b --grep "concourse|CarrierJump"`. H54, the bar's bake, is moot at a carrier now that it takes none. The lists are in their sections. A carrier's jump is built and not flown: during the jump itself the windows keep the bake of the system left, as no journal event marks a passenger's departure. Flight 35's descent and orbital hangar are still to fly, and Flight 34's items 2 to 4 (lamps, the bar's clear, a wedge in the windows) are still open. `hotkey.concourse_light_test` has done its job and is to be proposed for removal with the others. Flight 32 flown on c32cd19b, log `edvr_gfx_20261107_113241`: no one light family is it. `Root` and `Perturbator` are on the allow-list in that build. New temporary key: `hotkey.concourse_light_test`. `fix.station_window_distance` is meant to stay. `diag.concourse_cull` now defaults to `auto`, and the keys below are temporary overrides, to be proposed for removal, quoted, when the arc closes. Keys: `hotkey.concourse_camera_test` Insert in both Steam inis (F10 is Elite's screenshot key, and F11 is the photo camera in Sean's bindings); `diag.concourse_glass` in `edvr-flat.ini`; `hotkey.concourse_backdrop_parallax` F9 in both Steam inis; `diag.concourse_backdrop_radius`; `hotkey.concourse_view_test` F6 in both Steam inis; `hotkey.concourse_cull` F2, `hotkey.concourse_pane_capture` F3, `hotkey.concourse_pane_mode` F4 in both Steam inis, `diag.concourse_glow` in `edvr-flat.ini`, and `log.max_mb = 200` there for these flights only (two censuses filled the default). Keys in both Steam inis: `hotkey.concourse_cull` F2 (the cull and the mirrors), `hotkey.concourse_pane_capture` F3 (logs a ranking by screen coverage and the kept names, changes nothing), `hotkey.concourse_pane_mode` F4.

## What the earlier attempt learned

Grok session `render-thread-optimization`, branch `interior-portal-clear`, uncommitted, 2026-10-23 to 10-27, flat on Steam. Its notes are in `%USERPROFILE%\.grok\memory-v2\workspaces\edvr-unofficial-patch-2afe111a\topics\edvr-concourse-windows.md`. Facts worth keeping:

- Status.json Flags2 is the gate. `0x00014009` is on foot, in station, social space. `journalOnFootKnown()` is hard false in the flat profile, so flat reads Flags2 once `journalRawStatus().gameplay` is set. Gameplay is required: a leftover on-foot bit otherwise culls the main menu's hangar.
- The producer-B veto on foot works: about 185k-236k records dropped per 120 frames, 8k kept, room lit, faults none (logs 10-26 and 10-27).
- Light volumes are the room's fill. Additive cones and boxes (`8289669D`, `0357BBB2`, `A1B7CFCD`, about 512 cones) cannot be cut by centre position: dropping those outside the hall left the room dark, even with the ceiling opened.
- A hook on the context class also sees deferred contexts. The binding shadow follows the immediate context only, so a draw decision must read the shader bound on the issuing context.
- A flat draw never reaches `beginPanelOverride`. The four flat thunks return first, so a flat and a VR path are both needed.
- Ships, plumes and fog are not hall records. Engine trails are VS `EB787F98`, the green fog is `B12F7A61/42AC0CAC`, flares are `529D6238`, the sky billboard is `68DDDEF0/06332CA1`.

## What this review adds

**The store names every record, and the names split the scene cleanly.** The earlier work read names only from the settlement part test (greebles), found concourse kit only, and concluded the station had no name. The admission store's node names were logged by its own part census and never used for the cull. Log `edvr_gfx_20261025_133258`, census 2, at the glass, Flags2 `0x00014009`: 801 names, 5022 records. A prefix allow-list keeps 71 names and 488 records and culls 730 names and 4534 records, 90.3%.

- Kept: `Standard_SocialArea`, `SA_*` (`SA_StationHub_Compact`, `SA_Station_01_Bar`, `SA_TurboLiftBones`, posters), `PS_*`, `STAT_*`, `FUI_*`, `PROP_*`, `GUI_Interactive*`, `Interactive_*`, `PlantStack*`, `TP_*`, `HangarLobby`.
- Culled: `WheelSection*`, `Spoke*`, `RingRoad*`, `StrutA*`, `StadiumSegment*`, `SolarPanel*`, `HabitatRingA`, `DockingHub`, `Hanger*`, `LandingPad*`, `BlastShield*`, every ship part, every turret.
- These names recur in 31 logs across several sessions. They are probably one station; Flight 1 visits a second.

**Why the ring survived.** A record of the station model has its pivot near the player (2 to 13 m, log 195538), so a centre-in-box test keeps it. Its name does not depend on its pivot. This also answers Sean's 10-27 question about dropping external draws: judge the record that produced them, by name, where Elite still knows what it is.

**An opaque pane replaces the chase.** What survived the hall cull (lights, flares, fog, plumes, sky) is only visible through glass. Once the glass is opaque none of it needs its own rule, and interior fill stays untouched.

**The veto belongs at the rig.** The earlier work vetoed at producer B only. The hangar seal's rig veto (`0x431AFE0`, Elite's own off-screen branch) also skips the view loop, store, pose update, LOD and integrator, and gave -1.4 ms CPU (-26%) in the hangar (log 124634).

**Where the frame goes.** Log `edvr_gfx_20261026_185344`, census 1, two frames, hall cull on, at the window: 16,240 draws and 50.0M index-instances. The main view is 2.24M (4.5%). The `512x512` depth target is 20.0M (40%, 3868 draws). The six `2048x2048` tiles are 27.8M (55.5%). Depth-only passes are cheap per pixel (0.2 to 1.3 ms in the GPU profiles read), but they hold about 85% of the draw calls, which is render-thread and EDVR hook time. A veto at the rig reaches them; a draw-time filter in the colour view does not.

**The glass is not yet identified.** Dual-source blend (`bl=12,16,1/2,18,1`) is coloured transmission, and three pairs of it survive the hall cull: `E308565B/EDF86F13` n=1338 (5 or 6 draws, VS has an extra texture, probably NPC visors), `EFE42AC6/CC4E0188` n=804 (one draw, probably a helmet), `B553BB47/68ABCB9F` n=3 (the full-screen visor). `F512712C/AFED1D4B` with n=336 to 1098 appears at the glass before any cull (log 133258) and could be panes or ship canopies. Hence the capture by covered pixels.

## Design

### The cull

- **Gate.** Flags2 bit 0 (on foot) and bit 14 (social space), gameplay set, `fix.optimized_stations` on, and a record named `Standard_SocialArea` seen recently. The last condition keeps outposts and carriers out until they are added by name. The hangar seal is released on foot already, so the two never overlap.
- **Rule, per record.** Past a far limit (the sphere starts more than 200 m from the camera), cull whatever the name says, or if the name is unreadable; the anchor and the hub are exempt. Otherwise: keep if the name is unreadable, keep if it is on the allow-list, keep if the sphere is unreadable, keep if it is small and near (radius at most 25 m, centre within 40 m), which protects a weapon or prop whose name is generic (`Barrel`, `Slider`, `Hinge`). Otherwise cull.
- **Where.** The same three levels as the seal: rig veto (no record of the rig kept), producer-B veto, and the store's clear for path A.
- **Cost.** The class is one bit in the existing node cache, so the name is read once per node per cache period.

### The panes

The glass is VS `98397963AAEC45D3` / PS `66B08F89E4926C01`, which is Elite's full forward PBR material drawn `One`, `DestAlpha`, depth test on, no depth write. It draws after sky, fog, flares and light volumes, so an opaque pane overwrites them. A pixel shader is not needed: the material takes its values from textures, so EDVR's own textures in its slots turn glass into a metal.

| Slot | Reads | Glass | The mirror |
|---|---|---|---|
| `t8` | x metalness, y output alpha | metal 0, F0 0.04 | metal 1 |
| `t9` | normal xy | | flat |
| `t10` | rgb base colour, a roughness | | uniform bright grey, roughness 0.30 |

The reflection is the game's own probe cube array at `t5`, and the lights and shadows are the game's. `cb2[2].y` scales F0 and `cb2[2].z` scales roughness, and only row 2 of that buffer is read, so mode `mirror-soft` replaces it to compare a softer reflection.

| Mode | Around the draw | Result |
|---|---|---|
| normal | nothing | the game's glass |
| `mirror-steady-100` (default), `-25`, `-50`, `-200`, `-400` | `mirror`, then a third draw over it: a white pixel shader, `BlendFactor`, `One`, a pale colour at 0.25 to 4 | a mirror with steady light behind it; no shadows, and it does not move |
| `mirror-lit`, `mirror-bright`, `mirror-brightest` | `mirror`, then a third draw over it: a white, fully rough `t10`, `BlendFactor`, `One`, at 1, 2 or 4 | a mirror lit from behind, by the room's own probes and lights |
| `mirror` | a second draw with the three textures; `One`, `Zero`, RGB only; depth write on; marked pixels only | a lit metal mirror, nothing behind |
| `mirror-soft` | as `mirror`, plus `cb2` row 2 = 0, 1, 1.8, 0 | roughness 0.54, to compare |
| `tint` | as `mirror`, with a white pixel shader; `BlendFactor`, `Zero`; colour per index count | shows which pixels the mirror takes, by index count |

A draw is identified by VS and PS hash; the index count only separates the bay (n=36, opens on approach) from the rest. The start index is not stable across a reload, so the learned sides below last one session.

**Which pixels mirror (from the third build).** The whole room's glass is this one family, so the draw cannot say which side it is on. The depth buffer can. The game's own glass always draws, unchanged. Once a frame, before the first mirror, a colourless fullscreen triangle at z = 0.025 / D sets stencil bit 6 wherever the scene behind is farther than D metres (infinite reversed Z; 100 m by default, `diag.concourse_mirror_beyond`). Then each pane draws a second time with the mirror's textures and stencil `EQUAL 0x40`. So a pane with a wall behind it keeps the game's look, and a pane facing the emptied outside turns to metal, per pixel, with no table per station. Bit 6 is free: nothing after the opaque pass reads it, and the frame's clear resets it. The opaque pass's own stencil bits cannot serve, because a fullscreen draw clears them before the panes draw.

**Each pane's side.** When the cull lifts there is real structure behind the exterior panes, so the depth test would open holes in them. So one frame in eight each draw (keyed by index count, start index and base vertex) is counted twice: the game's draw and the marked mirror. At least 60% of its pixels mirrored (of at least 64) makes it exterior, otherwise interior. A reading under half the mesh's largest so far is ignored, because one mesh can be drawn in two places (Flight 3: 133k pixels at 66% and 17k at 30%, flipping within a frame). So that mesh takes the side of its large instance, and its small one is wrong while the cull is lifted. With the cull lifted, exterior panes draw whole without the stencil test, and interior ones not at all. The 60% keeps an interior pane out when the exterior is only seen through part of it. For 8 frames after the cull returns, a known exterior pane stays whole, because the frame in flight may still hold the depth of what was just taken away. The key is the mesh, so a mesh used both inside and out shows as `changed side` lines; the plain mirror phase is unaffected, because its test is per pixel.

**What the cull withholds (from the fourth build).** While the cull is on, whatever F4 says, these draws are not issued, so F4 at `off` shows exactly what is left. Nothing here can be seen past a mirror, so it is saving, not looks, except where a draw would paint over the glass. The bay's clear brings them all back.

| Sean's words | Family | Evidence |
|---|---|---|
| engine trails | plume `3B0B38CD` and its twin `34A1475B` (matched by PS because EDVR swaps the plume's VS) | depth test off, Flight 1 |
| smoke, volumetric | ribbon `B12F7A61/42AC0CAC`, volume `203DF517/EEAAC839`, haze `0A298DE7/6FD4C38B`, smoke `5E417E9D/BD801F2F` | `docs\per-object-motion.md` |
| ship lasers | beams and bolts `1F17BF54/A75C1DB6`, multicannon `939D01F2/C5DF9CC9`, shield strike `5C1D8EF5/C49F999F` | `docs\dlss-blur-elements-2026-10-14.md` |
| the planet | atmosphere `41E245D4`, `D95905C1`, `D1281DF4` (`docs\scanner-body.md`); body VS `72BDD292`, `26FC402B`, `9FFA5D5E`, `3530A6FD`, `71DD9863`, `ACE405F4`, `19F70CE8`, `C7FA0C0F` (`docs\eye-split.md`, `docs\fss-scanner.md`, `eye_draw_snapshot.h`) | older docs; not in view in any census here |
| the sun's glare | `8BD7C37A`, `1F3AD158` | `eye_draw_snapshot.h` |
| the haze (from the fifth build) | scatter `AFFEF018/41152F82`; particles `AFBCD3AD` and `9F4BBCFC/2BAE3742`, `9AEC596A/3789CA20`, `BBAD1CA8/E54F2A90`; shield strike part 2 `820E5C13/6EAA86EF` | Flight 4's census: what was left after the rest went; `docs\billboard-root-cause-2026-10-16.md` for the particles |
| the orange trail (from the seventh build) | ribbon `5EAFFCD0/DD371C57` | Flight 6's census: a strip, two draws a frame |
| not asked, paints on the mirror | flares `B75A6FF2/D56F859B` | after the panes, no depth test |

**Limited, not withheld (from the sixth build).** The light cones `1C506222/2519C905` and glow quads `359BF8FF/92FF8499` may also be the room's lamps, and nothing here separates them by draw. So while the cull is on they keep drawing, with the game's depth state plus stencil `NOT_EQUAL 0x40`: the mirror's mark, run first if it has not run this frame. A pixel with nothing within 100 m behind it is dropped, and one over the room stays. A cone outside can only show over far pixels, because the room's surfaces in front of it fail the depth test.

Not withheld: `88DCF116/494506A6` (the first-person laser), the sky `F8FA801F`, the stars and the space dust `E904D334` (see "Performance candidates").

The scope is on all four draw thunks. Beams, flares, stars, dust and the scatter are non-indexed, and until the fifth build only the indexed thunks looked at the lists.

**The sealed hangar (from the ninth build).** While the hangar is sealed, the families marked `sealedToo` are withheld too, with no diag key and whatever F4 says: everything above except flares, the scatter, the particle batches and the engine glow, which a hangar may draw itself (its own ship's glow on the pad, sparks, a lamp's glint). The light cones are not limited there.

**A known loss:** the logo on the glass (same VS, PS `98337732`, n=162) draws before the pane, so the mirror covers it. It returns when the bay clears.

**The trigger (goal 3).** Pane coverage, not a room coordinate: an occlusion query around the game's n=36 draws gives its share of the view. This needs no table per station and covers every bay window at once. At 20% of the view (12% in VR), on two readings running, the cull lifts, and 150 ms later the pane fades to the game's glass over 600 ms. From the fourth build it follows the view (Sean, after Flight 3: turning round at the glass left the cull off until he walked away). Under 40% of the entry share (8%, or 4.8% in VR) for 250 ms, it closes: straight back to the mirror and the cull if the bay is out of view (1% or less, or no reading), where a fade would only delay the saving, or by a fade if some of the bay is still in view. Coming back over the entry share during the fade turns it round. Coordinates were the first plan, but Flight 1's walk could not place the pane, and a table would have needed flights at each station type.

**Light behind the mirror.** The steady modes add a flat colour and need nothing below. The lit modes came first, and Flight 9 showed their cost: the sun's shadows, moving. The glass shader has no diffuse term. Its base colour only feeds F0, and the rest is the probes and lights through a specular lobe. So at roughness 1, F0 1, it returns a very soft sample of the room's light. Drawn additively over the mirror, it reads as light from behind and sits at the scene's own scale, which a constant in HDR units would not: nothing here has measured that scale. It follows the pane through its fade.

**The fade.** `BlendFactor`, `InvBlendFactor` with the mirror's textures, over the game's own glass, on exterior panes. At 0 the second draw stops.

### Later, if the numbers ask for it

See "Performance candidates".

## Flight 1

Flat on Steam is enough; VR after. Fly from the hangar, so the lift transition is seen. Take a screenshot at each step.

1. Stand in the middle of the bar, away from the windows. Toggle `hotkey.concourse_cull` on. Note what vanished and anything wrongly missing. Hold still for 15 s, off for 15 s, on for 15 s (the A/B).
2. With the cull on, press `NumLock` for a census (flat), and capture the GPU profile if bound.
3. Face a bay window from 2 m. With the cull off, press `hotkey.concourse_pane_capture`. The top eight candidates turn red, green, blue, yellow, magenta, cyan, orange and white. Screenshot. Each capture replaces the last one's colours, so one screenshot names one capture. Repeat under a skylight, then facing the window above the lift bank.
4. Press `hotkey.concourse_pane_mode` to see `mirror`, then `grey`. Screenshot each, facing a window, with the cull on.
5. Walk the bay window's length, then the room's edge, so `concourse frame:` traces the room. Stand still 3 s at each end of the bay window, and at the lift doors.
6. Take the lift to the hangar and come back, cull on. Note any flash at the doors.
7. If it is cheap, repeat steps 1 and 5 at a second station type.

After the flight: `python tools\edup_log.py --target steam --expect-build c9f096db --grep "concourse"`.

## Flight 1 results

Flown 2026-11-05 flat, 3840x2160, log `edvr_gfx_20261105_194826`, build c9f096db. One census, cull on, at the bay window (19:51:12).

**The cull.** The ring, spokes and ships are gone and the room stayed lit (Sean's screenshots). 71 names kept, 768 culled; about 45% of rigs vetoed outright (119k of 263k in a 5 s window).

**Frame time.** At the bay the game sat at its 90 fps cap, so the A/Bs there read nothing. At cam_local (-4.7, 21.7, -18.7) it ran uncapped, and the cull was toggled twice:

| Window | Cull | Present p50 | Frames in 5 s |
|---|---|---|---|
| 19:53:58-19:54:03 | on | 12.29 ms | 404 |
| 19:54:03-19:54:08 | off for 2.9 s of it | 14.31 ms | 365 |
| 19:54:53-19:54:58 | on | 12.51 ms | 390 |
| 19:54:58-19:55:03 | off | 14.53 ms | 335 |
| 19:55:03-19:55:08 | on | 12.64 ms | 382 |

So the cull saves about 1.9 ms a frame there (13%). CPU timing was off, so the split between CPU and GPU is not known.

**The census.** Against the uncut census at the glass from 10-25 (log `133258`, census 2, frame 0, a different day, same kinds of names):

| View | Uncut indices | Cull on | Change | Uncut draws | Cull on |
|---|---|---|---|---|---|
| Main 3840x2160 | 7.38M | 0.88M | -88% | 2293 | 1355 |
| Shadow tiles 2048 | 29.41M | 11.67M | -60% | 10583 | 5642 |
| Probes 512 | 12.40M | 9.87M | -20% | 3322 | 3317 |
| Total | 49.2M | 22.4M | -54% | 16198 | 10314 |

The remaining 21.5M of shadow and probe indices is about what the middle of the room drew uncut on 10-25 (23.2M), so the room's own casters now dominate.

**What is still visible through the glass with the cull on.**

- Green plate cluster: probably the single mesh of 81,744 indices, `EB5234DB/E06E61AE`, one draw in the main view, 3 in the tiles and 4 in the probes. `SA_StationHub_Agricultural` (one record per frame) fits; not proven. It costs about 650k indices a frame.
- Orange gantry and crates below the bay window: a kept name, unidentified. Flight 2's `kept` lines add radius and distance.
- Glows `1C506222/2519C905` and `359BF8FF/92FF8499`, fog `B12F7A61/42AC0CAC`, stars `68DDDEF0` (8232 instances), plumes `EB787F98`. All but the plume draw before the pane.
- Light volumes: about 4700 instances (`0357BBB2` 3900, `8289669D` 780), many of them exterior. 33.9M samples (409% of the screen) on `A1B7CFCD` alone in one capture.

**The glass.** `98397963/66B08F89` n=36 covers 87.3% of the screen close up in the bay and 33-45% from about 7 m. The logo on the glass (same VS, PS `98337732`, n=162) rises and falls with it. n=192 and n=864 are in the same family; which windows they are was not captured.

**Why the tint showed only the visor.** The picks included `3064D7F4/CCDB2A91` n=192, the helmet visor (no depth test, 100.0% of the screen in every capture), and `C4B4B334/0146ABCC` n=3600, the GUI layer (a constant 83.2%). Both draw after the panes and, tinted, cover them. The mirror and grey keypresses went the same way, so H4 was not tested. The panes' PS has metal 0 and F0 0.04, so a plain opaque draw would have been nearly black anyway: hence the texture swap.

**Bugs in the instrument.** `callback cost` printed garbage about every 30 s. `TurboLiftDoor*` was culled beyond 40 m and kept inside it, a pop-in risk. Both are fixed in the second build.

**The room's coordinates.** Bar platform y 26.16, lower floor y 22.16, lift area y 21.68. Near the bay: (-17.5, 26.16, 39.6). Lift bank: (-4.7, 21.68, -18.7). `Standard_SocialArea` reports r=130.7 centred 256 m from the camera in the same frame, so its sphere does not bound the room.

## Flight 2

On the second build, flat. Start with F2 off, in the hangar, so the lift transition is seen. Screenshot each step.

1. F2 on in the room. Look at the bay window from across the room, a skylight, and the window above the lift bank: all mirrored? Anything mirrored that should not be? Anything drawing on top of a mirror?
2. Cycle F4 once at each window: `mirror-cb`, `tint`, `off`, back to `mirror`. Screenshot each. `tint` names each window type by colour, and the log says which index count it was.
3. With the mirror on, walk slowly up to the bay window, stop at the glass, look away and back, then walk at least 15 m away. Note the distance it opened at, whether the station was missing when it opened, and any flash.
4. Stand at the bay with `off`: is the view right? Look at the logo, then at a skylight (still mirrored?).
5. Lift to the hangar and back, cull on. Any flash at the doors?
6. If it is cheap, repeat steps 1 and 3 at a second station type.

Then: `python tools\edup_log.py --target steam --expect-build e7b4b49b --grep "concourse"`.

## Flight 2 results

Log `edvr_gfx_20261106_040008`, build e7b4b49b, flat, 3840x2160, Steam, the same station type as Flight 1. NumLock was pressed at the bay window, and the log has no census from it and no line saying a press was missed, so nothing below comes from a census of this flight. Why the press did not land is not known. The stencil finding in Ruled out comes from Flight 1's census.

**Frame time.** `flat cpu` present p50, around the F2 press at 04:09:46: 16-18 ms in the 40 s before, then 11.1 ms (the 90 fps cap) almost throughout. With the bay clear and the cull lifted it was 15.6 ms. The cap hides the true figure, so the saving here is at least 5 ms, against 1.9 ms in Flight 1, whose panes were the game's glass. The position before the press is not logged, so the before and after are not at one proven spot. CPU timing was off.

**The mirror.** Sean: it works well on the exterior panes, and should be smoother, without the tile grid. It is also on every other pane in the room: a terminal's screen, the green and blue shop windows. The log shows why: about 20 index counts (6 to 864) pass the family test, and the four material lines logged all match (`t8`, `t9` 1x1; `t10` BC7 128x128; `cb2[2]` 1 1 1 0).

**The clear.** Lifts at 20.2-22.3% of the view, at z 26-34 in the room's frame, with the bay at z about 40. No lift elsewhere in the room, and it released on walking away each time.

**What is left outside.** With mode `off` and the cull on, one big plate cluster, a white bar and flares. `concourse names:` now gives distances, and the only kept names beyond 50 m are `PlantStack0-4` (x62, r 49-103 m, nearest 349-428 m), `HangarLobby` (794 m) and `PS_PP_Logo_Federation_Huge` (1190 m). `PlantStack` was on the allow-list by its name alone; the "plants" are the size of a building. The earlier guess that the cluster was `SA_StationHub_Agricultural` is dropped: its radius reads 0. `SA_StationHub_Windows_Exterior` (x2, r 35.9) is a record of its own. It may be most of the exterior panes, but nothing ties a draw to a record cheaply, and the depth rule does not need it.

## Flight 3

On the third build, flat, at the same station. Start with F2 off.

1. Stand still with a view of the room for a few seconds, then press F2 and stay still for 10 s. That gives a before and after at one place, if the frame rate is uncapped.
2. F2 on. The skylights, the lift-bank window, the terminals and the shop windows: is the exterior mirrored and the interior the game's own? Any pane mirrored in part, and is the edge where it should be?
3. The bay view with F4 at `off`: are the plates gone? What is left?
4. F4 at a skylight, for `mirror-soft`: which of the two looks right? Is the grid gone?
5. Walk up to the bay until it opens, stand there, and look at a skylight: still whole? Anything in a pane that should not be? Then walk 15 m away.
6. NumLock with the mirror on, looking at the bay from across the room, then NumLock at a shop window.
7. If it is cheap, repeat 2 and 5 at a second station type.

Then: `python tools\edup_log.py --target steam --expect-build b19b368d --grep "concourse"`.

## Flight 3 results

Log `edvr_gfx_20261106_050431`, build b19b368d, flat, Steam, the same station type. Sean: "much better", with one issue (the cull stayed off after turning round at the bay) and three requests: remove the smoke and volumetrics, the ship lasers and their trails, and the planet.

- **The mark.** D32_FLOAT_S8X24_UINT, z 0.00025, about 440 passes per 5 s, 0 refused. Three sampled 5 s lines show 41-78 meshes known and 10-15 exterior.
- **The sides.** Exteriors read 99-100% (n=18: 7636 of 7656; n=30: 11659 of 11659), and interiors mostly 0%, with some in between (n=24: 496 of 1079). One mesh (n=12, start 12078150) logged 84 `changed side` lines: see "Each pane's side".
- **The plates.** All of `PlantStack0-4` culled. The farthest kept name is now at about 50 m.
- **NumLock.** Shift+NumLock worked: census at 05:14:00, phase clear, at the glass (z 39.6), with the cull lifted. So it shows the families but not what the cull leaves, and the planet was not in view.
- **Sean's screenshot, F4 at `off`, cull on.** Pale haze, a planet, stars, white bars and a long white line, pale wedges, a small white cloud, orange flares.
- **Frame time.** Mostly the 90 fps cap (p50 11.1 ms), with stretches at 12-13 ms; 16.9 ms in the 5 s window of the clear at 05:07:29-37.

## Flight 4

On the fourth build, flat, at the same station. Start with F2 off.

1. F2 on, F4 at `off`, look out through the bay from across the room. What is left? A planet, lasers, trails, smoke or flares still there means a family was missed: take a NumLock with it in view.
2. F2 off and on with F4 at `off`, looking at the room: does anything in the room change? Lamp glints, haze, a hologram.
3. F4 at `mirror`. Do the panes look clean, with nothing painted on them?
4. Walk up until the bay opens. Turn round: the skylights and the cull should return within a moment. Turn back: the bay should open again. Is the wait on turning back acceptable? Try a quick glance away too, which should not close it.
5. Back away from the bay while looking at it: where does it close, and does it flicker?
6. If it is cheap, repeat 3 and 4 at a second station type.

Then: `python tools\edup_log.py --target steam --expect-build bc09083f --grep "concourse"`.

## Flight 4 results

Log `edvr_gfx_20261106_055426`, build bc09083f, flat, Steam, the same station type. NumLock at 06:00:41, cull on, F4 at `off`, in the mirror phase, looking out through the glass.

- **The clear.** Sean: the turn away works well, and nothing is missing or wrong in the room. Lifts at 20.2-23.1%, and it returns straight to the mirror at 0.0%.
- **Frame time, by F2.** `flat cpu` present p50, F2 off: 15-21 ms before the press at 05:56:53, 17-24 ms after the release at 05:56:57. F2 on: 11.3 ms in the 5 s window that held it, and 11.1 ms for most of the flight after, the 90 fps cap. So at least 4 ms and more often 6 or more, with the cap hiding the rest. The camera's position with F2 off is not logged.
- **The lists.** 15 families matched, including `planet surface`, `planetary 2` and `3`, and neither Sean's screenshot nor his report has a planet. Never matched: `beams and bolts`, `flares`, and with no ship or planet to show them `trail haze`, `body surface`, `planetary pipeline`, `terrain patch`, `beside the bodies`, `sun`, `sun arcs`.
- **Why the lasers and flares stayed.** The census's kind column: `1F17BF54` and `B75A6FF2` are `N`, DrawInstanced, and the scope was only on the indexed thunks. A census records a draw at the thunk's head, so a withheld draw still appears in it; the 30 s line is the evidence for what was skipped.
- **Why the haze stayed.** The plume was withheld at 12 draws a frame, yet the pale wedges are in the screenshot, so the wedges were never the plume; the "plume" label in the Flight 3 results was a guess. The trail ribbon and volume were withheld too, so the older note here calling `B12F7A61` the green fog is wrong, or at least not all of it.

## Flight 5

On the fifth build, flat, at the same station.

1. F2 on, F4 at `off`, looking out through the bay. Are the haze, the white bars, the flares and the ellipses gone? What is left? If anything stays, a NumLock with it in view.
2. F2 off and on, F4 at `off`, at a fixed view of the room: does the room change, a slight wash or a lamp's glint?
3. F4 at `mirror`: clean panes? Then the bay's clear once, for the view through it with everything back.
4. If it is cheap, a second station type.

Then: `python tools\edup_log.py --target steam --expect-build 0e586d70 --grep "concourse"`.

## Flight 5 results

Log `edvr_gfx_20261106_061847`, build 0e586d70, flat, Steam, the same station type. Two NumLocks, 06:22:17 and 06:23:38. Sean's screenshot has F4 at `off` and the cull on: stars on a dark blue sky, five green ellipses and a large green glow at bottom left, pale wedges, faint thin streaks, no lasers, no flares, no planet. The sky was pale before: that wash was the `scatter`.

The 30 s line has all the new families matching, `particles B` most (52432 in one window). So the shapes that stayed are not particles. The census's blended main-view draws, less everything on the lists, leave the cones and glow quads as the only large additive family. The stars, the dust, some decals and the room's holograms and NPCs are the rest.

## Flight 6

On the sixth build, flat, at the same station.

1. F2 on, F4 at `off`, looking out through the bay. Are the ellipses, the green glow and the wedges gone? If anything big stays, press F3 with it in view. It logs what covers the screen.
2. F2 off and on, F4 at `off`, at a fixed view of the room: does a lamp, a glow or a wash change?
3. F4 at `mirror`: clean panes? Then the bay's clear once.
4. If it is cheap, a second station type.

Then: `python tools\edup_log.py --target steam --expect-build d42c6471 --grep "concourse"`.

## Flight 6 results

Log `edvr_gfx_20261106_063641`, build d42c6471, flat, Steam. NumLocks at 06:39:14 and 06:40:39. Sean: "the green haze and light fills are gone", and two trails left, in screenshots sent to another session by mistake and to this one.

- **The orange double trail** (F4 at `off`), with a loop at its end, on a clean dark sky. In the second census the only thing on screen that fits is `5EAFFCD0/DD371C57`: topology 5 (a strip), n=95, two draws, VB stride 96. This is not the "grey ribbon" of `docs\per-object-motion.md`, which is `5E417E9D` and has been withheld since the fourth build.
- **The blue-white streak** on a mirrored pane, wavy and soft. Its census, taken at the lift bank (z -14.7), has 16251 lines in frame 0, 15910 of them in the 2048 and 512 passes, so the 16384 line cap ended it before the panes. It names nothing. The cap is 32768 now.
- **Why the streak is odd.** The glass shader has no screen-space input: `t0`-`t2` are raw lighting buffers, `t3`, `t4`, `t6`, `t7` shadows and lookups, `t5` the probes. And no blended draw of a trail reaches the 512 passes in the census that did complete. So neither a refraction nor a probe capture is shown, and a draw after the panes is the remaining route. The list after the panes in the second census is the visor, a glow, `B553BB47` and `5B0068AF`, none a trail, but that census was not looking at the streak.

## Flight 7

On the seventh build, flat, at the same station.

1. F4 at `off`, looking out: is the orange trail gone? Anything else left?
2. F4 at `mirror`, at the window where the streak showed: is it gone? If not, a NumLock with it in view. Its census now reaches the main view.
3. F2 off and on at a fixed view of the room: does anything change?
4. If it is cheap, a second station type.

Then: `python tools\edup_log.py --target steam --expect-build a41de79b --grep "concourse"`.

## Flight 7 results

Log `edvr_gfx_20261106_065604`, build a41de79b, flat, Steam. NumLocks at 07:01:40 (z 30.8, the ship) and 07:02:59 (z -15.3, the entrance); both censuses reached the panes, so the longer cap works. F4 at `off` for both. Sean has moved his frame cap to 144.

- **Frame time.** `flat cpu` present p50 6.94 ms looking out from the bay, the 144 fps cap, and 13-14 ms at the lift bank.
- **The ship.** A dark hull with blue engine glows, in the first census only: `66DE2CAD/864F1F94` n=32310 one draw (the hull plating family), and `2D8263CC/89B662E2` n=36 six draws, additive, each with its own vertex buffer. Its other parts did not draw, so the name cull took them. Only records kept with no distance limit could carry a hull past 200 m, and that left the nameless ones. The 5 s line has about 15000-23000 nameless keeps. No survey ran then: it runs at an engage, and this ship came later.
- **The entrance.** Sean: "the lights are really bright, if I move a bit inside, it goes to normal". The far ceiling is paler than the near beams, so it may be haze rather than exposure, but `scatter` was withheld and no other fullscreen draw appears. See H23.

## Flight 8

On the eighth build, flat, at the same station.

1. At the entrance where it looks washed out, stay still. Press F2 off and on. Then F4 between `off` and `mirror`. Which of them changes the look, if any?
2. At the bay with a ship in the docking area, F4 at `off`: is it gone? If not, F3 with it in view.
3. F4 at `mirror`: are the two trails gone?
4. If it is cheap, a second station type.

Then: `python tools\edup_log.py --target steam --expect-build b14a788d --grep "concourse"`.

## Flight 8 results

Flat, Steam, build b14a788d. Sean toggled F2 at the entrance: "the bright washed out look stays on when F2 is off". He asked for light behind the mirror, and for the same culling in a sealed hangar. His screenshot has the panes a flat blue-grey with a horizontal band, which is the probes' dark sky in the reflection now that the outside is gone.

## Flight 9

On the ninth build, flat, at the same station.

1. In the room with the mirrors on, F4 through `mirror-lit`, `mirror-bright`, `mirror-brightest` and `mirror`, from across the room and at a skylight. Which looks like a bright station beyond the glass? Is a lamp's highlight blotchy on a pane? The frame rate of each, if under the cap.
2. Board and stay on the pad, with the hangar sealed: does anything in the hangar go, such as a light, steam or a glint? Then launch: does anything pop back at the doors?
3. With a ship in the docking area, F4 at `off`: is it gone? If not, F3 with it in view.
4. Are the two trails gone?
5. If it is cheap, a second station type.
6. For `docs\sun-switch-re-2026-11-06.md`: in the concourse, F2 off, one NumLock.

Then: `python tools\edup_log.py --target steam --expect-build 4eead728 --grep "concourse"`.

## Flight 9 notes

Log `edvr_gfx_20261106_092241`, build 4eead728. Sean sees rotating shadows on the lit mirrors, with a shadow's edge sharp across a pane. The glow pass is the game's glass shader at F0 1 and roughness 1, and that shader reads the sun's moment shadow map at `t7` (`ps_66B08F89`, four-moment gather, `cb1[297]`, `cb1[261]`), so the pane now shows the sun and its shadows, moving as the station turns. The look depends on the sun's direction, so it also changes over a rotation. He likes the look of the mode he is on, which the log says is `mirror-brightest`, and chose a steady light over keeping the sun without its shadows. The steady modes step around a guess at the HDR scale, so the first flight is to find the level.

## Flight 10

On build 990a2b44, flat, at the same station.

1. In the room, F4 through `mirror-steady-100`, `-200`, `-400`, `mirror-brightest`, `mirror`, `tint`, `off`, `-25`, `-50`. Which steady level looks right, from across the room and at a skylight? Does anything still move across a pane?
2. The sealed hangar: on the pad, is anything missing? Then launch.
3. A ship in the docking area, F4 at `off`: is it gone? If not, F3 with it in view.
4. Are the two trails gone?
5. If it is cheap, a second station type.

Then: `python tools\edup_log.py --target steam --expect-build 990a2b44 --grep "concourse"`.

## Flight 11

On build 481d97b0, flat, Steam. One out-and-back, F2 pressed once while still seated (the walk survey runs only with the key armed).

1. Sit on the pad about 15 s, then get out, cross the bay, take the lift. In the room: do the skylights, bay windows and other windows look right? `diag.concourse_glow` in `edvr-flat.ini` changes them live.
2. Walk up to the ship services counter: does its window clear, and does it return when you turn away? If not, F4 to `tint` there and read the share from the log.
3. Look straight up at a skylight: does it stay mirrored?
4. Lift back, cross the bay, board, sit 15 s. Did anything vanish on the way out of the lift or after boarding?

Then: `python tools\edup_log.py --target steam --expect-build 481d97b0 --grep "walk survey|concourse cull|concourse clear|clearing pane|guard"`.

## Flight 11 results

Log `edvr_gfx_20261106_110851`, build 481d97b0, flat, Steam, small pad.

- Sean: the skylights are brightest and the bay clears; the shop window did not clear; the bar windows show two bands; the angled skylights want less light than the overhead ones; frames dropped in the lift going back.
- The shop window is n=30, `start=7301760`, read exterior at 91%, 7.3 to 8.3% of the view at the counter, cam_local 9.2 26.17 30.8. In three flights elsewhere n=30 stayed under 3%. So n=30 now enters at 0.3 of the entry share.
- The walk from the ship to the lift stayed inside the bay box: the lift cabin sits at 7.0 17.9 -98.4 of half 76 37.5 129. Up the lift the frame mean climbs from 7 ms to 10.5-17 ms over the 8 s ride, then 17-19 ms for 3 s after the cut, then 11 ms.
- Names, up to 3.8 1.0 -43.8 in the bay: the seal would cull 89-90% of records, with the nearest would-cull 86 m from the camera. The survey spent its 300 reports in 11 s, because the pick cell flickers tick to tick on foot, so there is nothing from the lift door.
- ruled out: the mark as the cause of the two bands, because it covers 81 to 91% of those panes while the lower band is about a third of the pane.
- ruled out: the concourse names survey as the 3 s of 17-19 ms after arriving, because it gathers over 2 frames.

## Flight 12

On build f09f42df, flat, Steam. F2 once while still seated, as before.

1. Get out, cross the bay, stand about 5 s at the lift door, take the lift.
2. In the room: are the bar windows one even colour now? Do the angled skylights read dimmer than the overhead ones? `diag.concourse_glow` is `sky,angled,other,bay`, live, and the bay's 0.5 is a guess.
3. Walk up to the ship services counter: does its window clear, and does it return when you turn away?
4. Lift back. Say when the frames drop: in the cabin before it arrives, or after. Cross the bay, board, sit 15 s.

Then: `python tools\edup_log.py --target steam --expect-build f09f42df --grep "walk survey|concourse cull|concourse clear|clearing pane|guard"`.

## Flight 12 results

Log `edvr_gfx_20261106_114951`, build f09f42df, flat, Steam, small pad.

- Sean: the bar windows look great; the same colour for all skylights that are not directly overhead; the shop window cleared at the counter; frames in the lift going down match the view across the room from the lift, then rise when the doors open on the hangar side.
- Cuts: 670 m going up, 673 m going down. `concourse cull: off (the camera cut to another room)` came 1 ms after the cut going down, and no `not seen for 5 s` line followed. H29 yes. The first 5 s after that cut read 11.49, 10.75, 10.42, 9.09, 9.01 ms with the cull off, with the camera still for the first three and the doors shut. Earlier builds, with the cull on for those 5 s, read p50 6.95 ms over a window that also held the corridor, so the two are not directly comparable.
- Names at the hangar's lift door, 7.2 19.4 -96.6 in the bay: would cull 90%, and the nearest would-cull is `HangerSmall` at 53.5 m (another bay's shell), then `DecalB` at 96 m. Nothing near the camera would go. So a seal on foot looks safe at this pad size, by the records. Not tried, and not asked for yet.

## Flight 13

On build eb9b6b8e, flat, Steam. F2 once while still seated, as before. No NumLock is needed: this flight is for the log. Move at an ordinary pace and pause where it says, so the log's positions can be told apart.

1. Get out, walk to the lift door, stand just outside it for 3 s. If it opens for you, step back 3 m and wait until it has closed, then walk up again.
2. Go in and ride up. Stay still until the doors have opened, then step out 5 m.
3. Turn round, stand just outside a lift door for 3 s, go in and ride down. Stay still until the doors have opened, then walk out. Board if you like.
4. Are the angled skylights right at the bar window's colour?

Then: `python tools\edup_log.py --target steam --expect-build eb9b6b8e --grep "lift survey|concourse cull: the camera"`.

## Flight 13 results

Log `edvr_gfx_20261106_130939`, build eb9b6b8e, flat, Steam, small pad, three rides.

- The door records move in `c` (and in `k`), not in the pose translation. Between `TurboLiftDoorLeft` and `TurboLiftDoorRight` the gap is 1.49-1.50 m shut and 2.90-2.91 m open. Between the `2` pair it is 1.36-1.40 m shut and 2.64-2.66 m open. The first pair moves first on opening and the second about 0.3 s later.
- A record named exactly `TurboLift`, r 2.4, is the cabin. It is in the store only while the lift is in use, and it moves with the player through the cut. Camera to its centre: 0.81-0.98 m inside and shut; 2.32 m standing just outside the door; 3.24-7.48 m outside and shut.
- Both pairs read shut 6.05-6.16 s before each cut, and the doors start opening 46-145 ms after it. So the window is about 6 s a ride, all before the cut, and there is no shut time on the destination side worth using.
- Frame mean in that window: 13-15 ms going down, 8.5-9.9 and 10-11.8 ms going up.
- Sean did not report on the angled skylights in this flight.

## Flight 14

On build a3f17170, flat, Steam. F2 once while still seated, as before.

1. Ride up and down once each, and stay still in the cabin. Does the cabin stay lit and whole? Does anything pop when the doors open?
2. Note the frame rate in the cabin going down, against the 13-15 ms it was.
3. Step into a lift and, if the panel lets you, back out without riding, so the doors reopen with no cut. Is the room whole when they open?
4. If you have a minute: the `concourse_keep` test in "The view through the windows".

Then: `python tools\edup_log.py --target steam --expect-build a3f17170 --grep "concourse cull: lift|concourse cull: on|concourse cull: off|walk survey: zone"`.

## Flight 14 results

Log `edvr_gfx_20261106_134043`, build a3f17170, flat, Steam, small pad.

- Going down: on for 5984 ms, ended by the cut, frame mean 11.76-12.20 ms. The 5 s line fell 0.1 s after the cut, so no counts for that ride.
- Going up: on for 6015 ms, ended by the cut, 6.94 ms throughout (the cap). 5 s line: rigs seen 601200, vetoed 481876, kept 29903. The cabin looked right, which shows it does not need the anchor or the hub.
- `concourse cull: on` came 21 ms after the cut going up. The doors had not started to open by then.
- `concourse_keep`: only `RingRoad02`, `07`, `08` and `RingRoadOffRamp02`, `03`, `08` came back. ruled out: the extra keep list as a way to see the big structure in this build, because the far rule culls an allowed name whose sphere starts past 200 m.

## Flight 15

On build 9a1d039f, flat, Steam. Take `concourse_keep` out of `edvr-flat.ini` first: it now works and would confuse the mask test. F2 once while still seated.

1. Ride up. If the pop comes, what popped: lighting, props, the windows?
2. Stand at the lift bank facing across the room, the costly spot, away from the bay windows. F6 steps the test: mode 1, 2, 3, 4, 5, then back to 0. Give each mode about 5 s, and press NumLock in modes 1, 2 and 3. In each: is the station there through the windows? Does the sun flood it? Does the room change?
3. F2 off for 5 s at the same spot, NumLock, F2 on.
4. Ride down, and press NumLock once the doors are shut. Frame rate in the cabin?
5. Put `concourse_keep` back, F4 at `off`, and F2 to compare at the same spot. Then take it out.

Then: `python tools\edup_log.py --target steam --expect-build 9a1d039f --grep "concourse view test|concourse cull: lift|walk survey: zone"`.

## Flight 15 results

Log `edvr_gfx_20261106_141917`, build 9a1d039f, flat, Steam, small pad.

**The lift.** Going down: 5969 ms, frame mean 8.47 ms, rigs seen 452761, vetoed 373868, kept 21545. Going up: 6016 ms, 6.95 ms (the cap), twice. Sean has not said whether the pop came again.

**The views (VERIFIED from 23 dumps; the labels INFERRED from the flags and the census).** View 0: flags 0x3, layer 0x17, the main eye. Views 1-8: flags 0x5000, layers 0x11, 0x16 or 0x17: spot and cache shadows. Views 9-14: flags 0x4010, 0x4020, 0x4040, 0x4080, 0x4100, 0x4200, layer 0x17, cameras reaching to about 8000 m: the six sun cascades. So `+0x3DC` holds both kinds of shadow view, and it cannot tell them apart. In 1 dump of 23 a view had flags 0xB, layer 0x10, bit 3 set: possibly a probe face. Every outside rig had all three masks equal: 0x2 (75%), 0x1 (23%), 0x3 (2%).

**The census, frame 0, indices in millions and draws, at one spot (cam_local -0.49 22.16 -14.88). Frame 1 fell past the census cap in most, so frame 0 only.**

| Mode | Main | Tiles 2048 | 512 |
|---|---|---|---|
| 0, cull | 8.29, 8360 | 20.50, 12009 | 10.06, 4110 |
| 1, `+0x3DC` | 12.73, 9574 | 20.88, 12305 | 10.15, 4059 |
| 2, `+0x3DA` | 13.11, 10229 | 30.04, 15340 | 10.51, 5899 |
| 3, both | 12.80, 9840 | 22.50, 12625 | 10.44, 4359 |
| F2 off | 12.79, 10376 | 28.12, 14942 | 10.35, 4289 |

So zeroing `+0x3DC` keeps the whole main view and takes the tiles down to the cull's level, as RE predicted. `+0x3DA` does nothing here. The 512 row does not move in any mode.

**Frame time at that spot (camera still, steady readings, the view direction not logged).** Mode 0: 12.8 ms. Mode 1: 15.3. Mode 3: 15.5. Mode 4: 15.5. Mode 5: 15.4. F2 off: 16.5. Mode 2 climbed from 16.4 to 18.5 and is not read further.

- So the full cull saves about 3.7 ms there, and main view only about 1.2 ms. The shadow passes hold 76% of the indices the cull removes and only about a third of the time it saves, which refutes the expectation in "The view through the windows".
- ruled out: `+0x3DA` as a shadow mask, because no shadow view has flag bit 3, and mode 2's census matches F2 off.
- ruled out at this spot: the outside effects and the sun pair as a measurable frame cost, because modes 3, 4 and 5 read the same.
- The cause of the 2.6 ms left in the main view is not known, CPU or GPU: CPU timing was off.

**The keep list.** At cam_local 2.63 22.16 -14.65: flat fill 12.8 ms; plain glass 13.3-14.7 ms; plain glass with `concourse_keep` 13.5-13.7 ms. So the big structure costs nothing measurable there, and the flat fill itself saves about a millisecond over the game's glass. Few seconds each, one spot. The readings of 19-45 ms at 14:26:57-14:27:39 sit around an ini edit and are not counted: INFERRED that the game was in the background.

**Where that leaves the window view.** Candidates at that kind of spot: flat fill 12.8 ms, keep list with plain glass about 13.6, main view only about 15.4, F2 off 16.5. The keep list needs no offsets and no hook: if it looks right, it is the answer. A captured backdrop stays the fallback for a fuller view, since a frame that draws the outside can stay hidden behind an opaque pane. The mask test (`hotkey.concourse_view_test`) can go when this is decided.

## The window plan (Sean's goal, 2026-11-06)

Judge each candidate by how many of the 3.7 ms the full cull saves at the lift bank it keeps, and let Sean judge the view. Order: the size test first, since it is built and takes two minutes. Unless it wins clearly, a captured backdrop.

**The backdrop, as designed; a first prototype is built, with capture on a key press only, no refresh and no VR work. Softness: a cube's centre has fewer texels a degree than its edges, 27 at 3072 a face against about 38 pixels on a 4K screen.** The bay's lifting step already takes the cull off for 150 ms with the glass opaque (`kClearLiftMs`), flown. So a capture is that step, then a copy of the scene before the pane covers it, on the last frame. Nothing shows: the frame rate dips for 150 ms. The pixels go into a cube of view directions, about 100 MB at 2048 a face, softer than a 4K screen. A pane pixel looks up its direction: the glass PS has its position in `__USER_VERTEX_M_LIGHTINGPOSITION` (v2), and reads cb1 rows 277-279 against it; EDVR's flat logs read the camera at row 275. That the PS can use row 275 this way is INFERRED. The mark pass splits near from far, so what is within 100 m stays real and only the far station is backdrop. Captures: on a pane first seen, fading from a plain tint; after a few metres' walk; on a slow timer. Ships are left out of it. Known risks: ships and moving lights freeze between refreshes, DLSS may smear it on a turn, and the look-up's cost is unmeasured. First prototype: one capture from where Sean stands, to judge the look.

## Flight 16 results

Log `edvr_gfx_20261106_150756`, build b4f82839, flat, Steam, at cam_local 0.10 21.68 -17.05, camera still. Frame mean: flat fill before 13.2-15.4 ms; 150 m 14.5 rising to 16.9; 80 m 16.1-17.2; 40 m 17.5-18.2; 20 m 17.2-17.8; 10 m 16.9-18.2; flat fill after 14.7-16.9. Records kept by size per 5 s: about 97,000 at 150 m, up to about 190,000 at 10 m. Sean: "None of them seemed to impact frame rate, they all dropped well below the full cull."

- ruled out: keeping outside records by size, because even 150 m gives up most of the saving and the sizes barely differ from each other. About 1 ms of each is the plain glass.

## Flight 17 results, first part

Log `edvr_gfx_20261106_163158`, build a2fca02e. One capture, at cam_local -0.17 21.66 -16.75: lifted 328 ms including making the cube, scene 3840x2160 R11G11B10_FLOAT, 18 pane draws. Nothing was refused. Sean's screenshot, from nearer the bay, shows the flat light with patches of station view with stippled edges.

- INFERRED: the patches are the directions the far windows covered from the lift bank, where they subtend few degrees; walking closer, a window covers many directions never captured. The stipple is the capture's point splat. So one capture cannot fill a room. Sean had not yet pressed F6 again nearer the window.
- Frame time: `mirror-backdrop` and `mirror-steady` read the same at one spot (8.62-8.70 ms against 8.62 ms), so the look-up has no measurable cost.

**Next, if the look holds up when captured from close:** capture when a meaningful share of the visible panes has nothing captured, rate-limited, so the cost falls to nothing once the room has been seen; fill holes and uncovered areas from low mips of the capture instead of the flat colour, so they read as frosted glass in the station's colours. Whether to persist a cube per station is for later.

## Rendering from the concourse's centre (Sean, 2026-11-06)

Sean's idea: a cube around the room, filled from a snapshot at the room's centre, with the render off screen during a lift ride. F6 cannot do that: it records only what shows through the windows from where the player stands. RE, build 334925, not flown, `analysis\decomp\verify_20261106_concourse_probes.txt`:

- **The game's probe views: not the route.** They come from an EnvMap state machine, at a placed probe's position (INFERRED). Their faces draw no rigs: layer 0x10 against rig masks of `cfg & 0xF` gives zero (READ). Their size, 256 with 8 mips, is a code constant (READ). Their output feeds the game's reflection cubes through global objects. To carry the station a face would need a rig mask write, an unidentified target and lighting passes, and a way to stay out of the reflections. ruled out as the small route, because of those.
- **A chosen camera for whole frames: the smaller route, unproven.** `0x1EA9C0` is handed the render views and fills the view slots. A view's camera object is at `RV+0x310` (dirty byte `cam+0x230`). Editing it before `0x1EA9C0` should reach cull, LOD and the rig eval, and may reach the constant buffers, whose source was not found (INFERRED). The frame would still have to be kept from the player, which nobody has looked at. Appending a view is not small.
- **Smallest test:** during a ride, in a pre-forward hook on `0x1EA9C0`, move view 0's camera by a fixed offset, and log the view slot's position and the draw counts afterwards. If counts and image follow, the route works. A log-only form comes first. Offsets are per build.
- **Open beyond that:** no ride means no cube on a load straight into the concourse; the centre is unknown on a first ride up, since the anchor appears only 0.1-0.2 s before the cut.

## A baked shell (Sean, 2026-11-06)

**Flight result.** 30 automatic captures in about 70 s, one every 1.7 s or so while he moved, at 234-344 ms lifted each, with the missing share 0.2-5% each time. ruled out: capturing on movement, because walking keeps revealing directions, so it does not settle while the player moves, and the panes in his clip show flat colour with streaks between captures. The 19-45 ms readings then are partly the game in the background (his clip shows Alt-Tab).

**The proposal, not built.** Bake once, unobstructed, then draw a simple textured shape.
1. **Prove the camera.** Done for flat in Flight 22; VR has its own route at `0x283FA20`'s pose.
2. **Hide the frames.** Show the last cabin image at Present for those few frames. Not looked at yet. DLSS history would need a reset. VR not considered yet.
3. **Bake.** In one ride, from the room's centre, with the room's and cabin's rigs off and the outside on, ships left out: colour and depth for the faces windows look through.
4. **The shape.** A coarse grid per face, displaced by the baked depth, textured by the colour: one small mesh, so parallax comes from geometry with no per-pixel march and no captures at run time. Drawn behind the panes only. A tint from EDVR's glass shader on top, or the game's glass at about 1 ms.
5. **Keep it.** Per station by market ID (not read today), so a return costs nothing.

**The fallback, needing no hook:** real big structure through cheap glass. Flight 16's 150 m step read about 2 ms over the flat fill, about 1 ms of it the game's glass; EDVR's own glass could recover that, keeping about 2.7 of the 3.7 ms, live and exactly aligned. The gaps would have to be closed by name, from the `concourse names: culled` lines.

**Bake at the disembark fade (Sean, 2026-11-07).** It would cover the frames and come before the player sees a window. Two past disembarks (logs `edvr_gfx_20261106_141917`, `edvr_gfx_20261106_130939`): frames run at the cap up to the moment the camera jumps 10.37 m from cockpit to avatar, a `flat resolve reset event` fires there anyway, Status follows 0.7 to 0.95 s later, and that 5 s window held 561 and 613 frames against 720, so the game already drops frames there. Not known: when the screen is black relative to the jump, and whether the world is drawn under it. The blocker is the room's position: in the hangar `anchor age -1`, the anchor is not seen until the first ride's cut, and the concourse's offset from the hangar differs by pad and station. Loading straight in on foot has no disembark.

**A separate off-screen camera (Sean, 2026-11-07), RE only, not flown, `analysis\decomp\verify_20261107_second_view_owners.txt`:** the engine keeps four view owners (Primary, Secondary, Thumbnail, Auxiliary), each with its own G-buffer, RenderViews and pass objects, so a second view would not overwrite the main one (READ; the target names are in the exe). The Thumbnail owner has a camera setter, `0x2879AB0`, which fills `W+0x2D20` and `W+0x3010` and sets `W+0x3300` (READ, checked), and an enable byte, `W+0x2D10`. Against it: its RenderViews carry a flag that drops ordinary rigs, so no station draws (`0x432F180`, reported READ; a quick check of the first bytes found the `+0x3D5` test but not the flag test); its layer mask, shadows, size and the caller of the setter are not found; and it serves Holo-Me's isolated scene, so it may not see this world at all. A `SecondViewSceneTexture` owner (enable `W+0x331C`) has an unknown camera source and was not studied. Not small, with several unknowns. Parked in favour of the player's camera at the fade: the main thing a second view would save, hiding the frames, the fade already gives.

**Open:** no ride means flat fill until the first ride. The centre is unknown on a first ride up, so a first bake waits for a ride down. The lighting is frozen at the bake.

## Flight 26 results

Build 31720543, flat, Steam, 1920x1080, log `edvr_gfx_20261107_081242`, one screenshot. Sean: "I think this is basically almost done, just need to hide this rear seam better?"

- **The hold is well down.** Five frames captured, one per direction, finished in 94 ms against 172, and the hold ended at 141 ms against 219: 8 Presents. Bake begun 93 ms after the cut, doors at 1.51 and 1.37.
- **The "seam" is a gap, not a join between captures.** In the screenshot, sharp station runs up to one hard straight edge, and past it is smooth pale blur. A hole with a straight edge is the shape of a large near polygon the depth filter left out, with its fill taken from the bright station beside it. The join between frames would run along the room's axes at 45 degrees and has no blur to it.
- **The likely polygon.** The survey in the bake lists what stays near the centre: `Floor07`, r 1017.7 m, its nearest part 44 m from the centre, inside the 50 m filter. A record that size cannot be culled by record: it is the station.
- ruled out: leaving out near points by depth alone, because the frame has already drawn them, and they hide what is behind them.
- The dark band of Flight 24 is likely the same floor sections, beyond 50 m in that case. INFERRED.

**Fix, Flight 27:** the bake frames' near plane is set to the near limit, 50 m. The hardware then clips anything nearer, whatever its name and however large its record, and what is behind it is drawn and captured. The name rule stays for what lies past 50 m.

## Flight 31 results

Build 0448d071, flat, Steam, 3840x2160, log `edvr_gfx_20261107_105506`, NumLocks at 10:59:23 (cam_local 0.4 21.7 -17.8) and 10:59:27 (about 0.3 22.2 -14.8). Sean: "That fixed the tables it seems. I took two dump draws, when I'm close to the elevator bank, the lighting is blown out, but if I step a few meters into the concourse that light source seems to go away. I'd like to consistently remove it when culling. Turning culling off has the light source everywhere".

- **The tables.** Gone with the key on. He did not comment it out to compare, so the two names are not separated, and it is "seems". Enough to put both on the allow-list.
- **The light is the entrance's bright look of Flights 6 and 8,** now with one more fact: with the cull off it is everywhere, and with it on, only at the entrance. Flight 8 said only that it stays with F2 off.
- **The two censuses draw the same lights, in the same order:** the sun quad `7E38A6AA/7CECABDE`, the room's light volumes, then the additive quads `D95905C1/5BCB6B95`, `D1281DF4/97DBC87F` and `B75A6FF2/D56F859B`. Two more `0357BBB2/81812EF9` volumes are drawn further in, not nearer. So it is no extra light at the lift bank, unless a draw's constants or coverage differ, which a census does not show. The census records a draw before the withholding, so the scatter appears in both and says nothing.
- ruled out: a record kept only while within 40 m of the camera as the cause, because the small-close count stays at about one record a frame on both sides of the line (445 at -17.8, 406 at -14.8).
- ruled out: `Perturbator`, because the key now keeps it everywhere and the light still goes a few metres in.
- **Not known yet:** whether it is a draw, shading that depends on where the camera stands, or exposure. The camera moved only about 3 m between the censuses, so a line that sharp is unlikely to be exposure settling, but the censuses were only 4 s apart.
- **Next, no build:** at the lift bank and a few metres in, the same way round: a screenshot and F3 at each. F3 ranks the blended draws by screen coverage, and a screenshot pair says whether the room's lit surfaces or a haze over everything changed.

**The screenshots and F3, log `edvr_gfx_20261107_110953`,** at cam_local 6.0 21.7 -17.5 (11:12:41) and 4.7 22.2 -14.5 (11:12:55):

- **It is light on the room's surfaces.** At the lift bank the walls are pale and flat, with the shadows under the balconies filled in; a few metres in they are deep and warm. The windows keep their deep blue in both. An exposure change or a veil over the finished frame would lift the windows too, and the glass is drawn after the deferred lights. INFERRED from the pictures.
- ruled out: exposure and any fullscreen draw after the glass, because the windows do not change (by eye).
- **F3 does not name it.** The same draws lead at both spots, with their coverage moving as the view does: `A1B7CFCD/2DB678B6` about 600 percent (108 instances), `A7339D1F/37649F54` 78 to 93, `0357BBB2/70E6FCA6` 78 to 70, `24DE25E4/3D8442D2` 75 to 58. The volumes are instanced (the census's `i=`), up to 3194 lights in one family, so one light's constants cannot be seen from outside.
- The kept records are the same on both sides of the line, so the cull's judgement does not move it: the light is there or not by where the camera stands, and with the cull off it is there everywhere. Why is not known.

**Next, Flight 32:** a key that leaves out one light family a press while the cull is on, the families being the ones a census of the room lists.

## A large surface port's survey

c841a58b, log `edvr_gfx_20261108_045456`, flat, Steam, a `CraterPort` (`concourse cull: a station of type CraterPort: its layout has not been flown, so it stays the game's own`, 05:01:53). Two F3 presses with the cull off, 26.7 m apart: the middle of the room at 05:03:28, then the bay at 05:03:40. Sean: "Made it to a large surface port. Took the usual two surveys".

- **The same room as the CraterOutpost's (Flight 35).** The anchor is `SA_PlanetPortHub_Compact`, r=42.4 against 42.1, 7.0 m from the middle press. There is no `Standard_SocialArea`. The shell is another record: `SA_PlanetPort_Exterior_Radial`, r=90.7, 47.5 m from the middle press, kept by `SA_` (the outpost's was `SA_PlanetPort_Exterior`, r=102.2).
- **The bay is the same glass:** `98397963/66B08F89` n=12, 96.5% of the screen at the bay press. From the middle, two other panes of the family show: n=36 at 11.2% and n=24 at 10.5% (13 draws), not yet placed. A second glass, `6DB587D2/B2DE0A41` n=744 and n=1308 (5.6% and 3.2%), is not of the treated family. Both captures hit the 1024-draw pool cap, so they are partial.
- **The keep list parts them:** 64 names kept and 758 culled, `0 culled records within 60 m` at both presses. Nothing is culled between 33 m and 270 m. Past the bay: `PB_LampPost` from 270 m, roof decor, crate groups, pad doors and `DoorWay_PadSide_Large0` from 370 to 470 m, and the farthest at 2.6 km. The nearest building is about 290 m off, against 84 m at the CraterOutpost, so the 300 m sphere the bake is shown on sits about at the first buildings.
- **Frames, cull off:** present p50 11.5 to 15.3 ms in the room (05:03:13 to 05:03:53), against 7.7 to 9.7 ms on the way in. The CraterOutpost read 10.2 to 14.3 ms there with the cull off.
- **What it needs:** nothing built. `roomLayoutOf` already gives `kRoomPort` (n=12, 300 m sphere, 100 m reach) for this anchor, and `atSurfacePort` keeps the sun on for a `CraterPort`. Only `bakePlanFor` leaves the type to the game. F2 in the room runs it now (Flight 48); `kCraterPort` joins `bakePlanFor` on Sean's word, as the CraterOutpost did.

## Flight 48, the list as written

Flat, Steam, c841a58b, at the `CraterPort`, in its room, with F2 (the type is not automatic yet).

1. Press F2. The log should show `the room is a surface port's (its anchor is SA_PlanetPortHub_Compact)`, then the bake's lines and `concourse cull: on`.
2. Does the room stay whole? The shell `SA_PlanetPort_Exterior_Radial` is kept, and nothing within 270 m is culled.
3. The bay from the middle: are the buildings in the right place? They stand near the 300 m sphere the bake is shown on, so any parallax shows here first. Walk to the bay: it should clear only near it.
4. The two other panes seen from the middle (n=36 and n=24): what are they, and do they look right?
5. F2 on and off at one spot for the frame time: 11.5 to 15.3 ms there with the cull off.

## The bake's fix for H57 (built 2026-11-08, 35aa1d87)

Sean: "Build the fix". The near value did two jobs, and they are two values now.

- **The frames' near plane is 5 m** (`kBakeNearDefaultM`; `diag.concourse_bake_near` still overrides it, 5 to 400), so the sky, the stars and the sunlight are drawn in the bake's frames.
- **The room's cut stays at 50 m** (`kBakeRoomCutM`): the bake's compute shader writes no texel whose point lies nearer the centre than that, and the texel keeps what another frame gave it. Texels with no depth (the sky) are written at the far limit as before.
- **The room's own records** were already culled in the bake's frames by the look's judgement (`concourseJudge`, `look`): allowed names, and small records within 60 m.
- **The people** never pass the store (Flight 23: "NPCs were left"). While the bake captures, a draw is not issued if its vertex shader is one of the skinned families in `engine_velocity_families.h` (an NPC's rigid body 8B58 and skin 7B0D, 114A, D99A, 61AE, the hair A6A3, the suit 01DA) or the hair's depth prepass 3186 (`cc::bakePeopleVs`). What is behind them is captured. The pane hook is armed for the bake's frames to do it; it was not armed then before.
- **The log:** `concourse bake: begin: ... near 5 m, the room's cut 50 m ...`, and the finish line counts `the people's draws left out over its frames`. 0 there with people in the room means a family not named. A bake that ends by neither finish nor cancel gives the people back after 2 s, with a line.
- **Risk:** if Elite's sky writes depth nearer than 50 m, the sky's texels are cut and show the pale fallback. The 5 m test does not say, since nothing was cut there.

## Flight 49, the list as written

Flat, Steam, 35aa1d87, no keys, at the `CraterPort` (now automatic): arrive in the room, or ride up.

1. The log: `concourse bake: begin: ... near 5 m, the room's cut 50 m`, then `finished ... the people's draws left out over its frames: N`, N above 0 if anyone was in the room.
2. Through the bay: a black sky with stars, sunlit buildings, the ground, and no people outside.
3. The other windows (n=36, n=24 from the middle): anything of the room in them, or pale patches?
4. If the sky is pale white instead, H57's fix cut it: say so, and the cut needs to leave the sky alone.
5. Any other station type on the way (a Coriolis or Orbis): the same checks, as its bake changed too.

## Flight 50 results

VR, Steam, 7bc497b5, log `edvr_gfx_20261108_064006` (06:40 to 06:44), with the runtime log `edvr_openxr_20261108_064008_518_57028`. Pimax Crystal Super at 4032x4034 per eye, `fix.temporal_aa = dlss`, `fix.ui_quality` on. Sean: "ran it in vr". One station, a `Dodec`; no carrier.

- **The dodec stayed the game's own:** `a station of type Dodec: VR has no bake yet, so it stays the game's own`. After docking the frame rate fell from about 85 to 44 fps (`vScreen totals`), with long frames of 22 to 25 ms against an 11.1 ms period. That is what a VR cull there would be for.
- **The hold works (P1).** One press: the latch took 3 of 10 held frames by +3 and 10 by +12. The runtime's trace at close: `native_features_summary,changes=0,withheld=15,replayed=15,empty=0`. Every withheld frame was replayed and none went out empty. The 5 beyond the probe's 10 are INFERRED to be the transition flash's own marks.
- **The camera probe wrote nothing (P2),** both presses: `0 of 5 frames wrote a pose; the stub ran 42 calls in 7 frames`, 6 calls a frame, 3 of them at stack addresses. Each frame's first call is at a stack address and carries a head pose: rows rotated, translation `t(2.783 0.672 -4.893)`. The second call is on the heap and is the identity. The build refused a stack address, because the spec read the RE as "the second call is the stack one". 0x2845730's working copy of W+0x3330 is on its stack, so that refusal was wrong (ruled out below). The anchor was not read (no cull at the dodec in VR), so the raise was along world +Y.
- **The capture point is found (P3):** two admitted tonemap draws a frame. H is format 26 (R11G11B10_FLOAT) at 2016x2017, DLSS's input size, under the 4032 output. But every line had `eye -1`: the eye table had not seen the tone's target, so the depth and the per-eye rows were never asked for.
- `ruled out: refusing a camera stub call whose pose lies on the stack, because Flight 50's head pose came on 0x2845730's stack on every frame's first call, and the heap call after it was the identity.`
- **Fixed after it (4b74d75c, installed on Steam 2026-11-08):** the probe writes a frame's first call wherever it lies, never an identity pose, and logs all six calls. The tone lines take the eye from the draw's order in the frame when the table has not seen the target, and name the target.

## Flight 52 results

Flown 2026-11-08 in VR on 194805e2, log `edvr_gfx_20261108_092319`, at a `Dodec`: the same station as the flat Dodec bake on 35aa1d87, by the names survey's distances from the centre (`Floor07` nearest 44.0 m, `DockingHub` 601.9 m in both logs). Sean, with a screenshot: "Mostly works, just missing some parts of the back at the front in this large station type". The windows show the station in stereo with the cull on (79 fps, GPU 11.9 ms, CPU 8.0 ms in the overlay), and a large pale patch with a ragged outline sits in the middle of the view where the far structure should be.

- **The mechanism works.**
  - The bake ran by itself: available at 09:25:24.2, began at 26.4, finished after 172 ms.
  - The world check found one world (render less store 0.03 m), so no offset was applied.
  - Every aim landed within 0.007 m of its prediction; rows 273 read 5.000 m on the aimed frames, and the near went back to 0.025 m.
  - Every face was 100% in view of an eye. The latch took 9 held frames. No frame was refused or off the centre.
  - The order was rows 0 #29, rows 1 #30, tone 0 #31, tone 1 #32.
  - The panes were treated afterwards (`mirror-backdrop`), and the bay cleared on approach at 12%.
- **The hole.** A texel left unwritten shows the glass's fallback colour (the level's), and the patch is that colour. Unwritten texels (in eye 0's view, less those written) were 13.1% on face 0, 0 on faces 1 and 2, 3.8% on face 3 and 13.2% on face 4. A texel is unwritten only when its point lies inside the 50 m room cut. Flat at the same station, with the same rule, showed no hole.
- **Each face had one frame,** the first after its aim (frames 9614 to 9618). Flat captures a face about 4 frames after its write.
- **Open, with what tells them apart (instrumented together in "The hole instrument"):**
  - **H58:** the game decides what to draw in frame k from frame k-1's camera. The stub writes where both the cull's and the draw's copies are made, but if the cull's copy feeds the next frame, each face's first frame lacks what the previous face's camera did not see. Flat writes the game's camera, upstream of both. Tell: on a 3-frame dwell, the first frame's distance counts differ from the last's.
  - **H59:** the game always culls from the real head. Tell: the three dwell frames agree, and in the dump the face away from the real head (face 1 here) lacks the smaller records (hangars, pads, skirts) and keeps the big shells (`Floor00` to `11` at r 1018 m, `DockingHub`).
  - **H60:** something within 50 m of the centre is still drawn in VR's bake frames, and the cut leaves its shape. Tell: cut texels 5 to 50 m from the eye, and magenta shapes the distance image shows as near.
  - **H61:** the depth decodes wrong for some geometry. Tell: cut texels under 5 m (inside the near plane), or a far object's texels cut.
  - The colour favours H60 and H61, since objects missing (H58, H59) would show what is behind them, written. The screenshot cannot decide it: the far shell behind would be fogged pale too.

## Every dwell frame writes, and the draw diff (built 2026-11-08, bb69891c)

- **Behaviour:** each face's three frames all write the cube where they are past the cut, so the cube keeps their union: a texel the band covers on one frame is filled by another. In all three of Flight 53's bakes every face had a frame free of the band. The dwell is no longer temporary.
- **The draw diff (TEMPORARY, for H62):** on the bake's aim frames, every drawn draw with a depth target is keyed by VS, PS, index count and start index (in `ConcoursePaneScope::begin`, after the cull's and the people's withholds). When a face ends, the keys whose count differs between its frames are logged, the largest first. The frame numbers are the `vr capture` lines', so the band's frames can be matched to the draws.

## The record diff for H62 (built 2026-11-08, 8fcbec09 and a fix on top)

Which store record is the band, and why does its verdict change between frames with the camera unchanged? The four candidates, each with what would show it:

1. Its name cannot be read on some frames (kept as unreadable-name): kept with that reason on the band frames.
2. A second record, another LOD or variant with a kept name, appears on the band frames only.
3. Its sphere cannot be read on some frames: kept as unreadable-sphere on the band frames.
4. It never passes the store: no record within 60 m differs between band and clean frames.

- **The instrument (TEMPORARY):** both judges (the store's, path S, and producer B's veto, path P) note, on the VR bake's aim frames, every record within 60 m of the room's centre, or with no readable sphere. Each note has its name, sphere, verdict and the rule that decided it. The key is the name plus the sphere's centre rounded to 0.5 m plus the path, with the record's address beside it.
- **What it logs:** per face, the records whose presence or verdict differs between its frames (`vr record diff`); once a bake, the records kept within 50 m on any aim frame (`vr record kept near`). The frames are the aim's, as on the capture lines, with the judge's own stamp beside them.
- **Fixed before flight:** the judges' own distance is from the store's camera, and in VR that is the real head (the world check's "store's camera"). So nearness is measured from the room's centre instead, or the band, 20 to 50 m from the centre, could have fallen past 60 m from where the commander stands.

## Flight 56, the list as written

VR, Steam, the doc commit on top of 5507d8e7, promoted by `--dll-only` (the version line names it; `python tools\edup_log.py --target steam --expect-build <that commit> --grep "concourse bake: vr"`). A station on the list (the Orbis or the Dodec), on foot in the room, by a ride up or a load in the room; nothing to look at in particular. Read the `vr capture` lines for the band's frames (the 20 to 50 m cut), then the `vr record diff` lines for the same face and frames, and the `vr record kept near` table.

## Flight 56 results

Flown 2026-11-08 on 44ce478f, log `edvr_gfx_20261108_201340`. Sean, during it: "not seeing any bake at all".

- **Every bake ran and published:** 20:16:17 at a `CraterPort`, then 20:21:22 and 20:22:04 at an `Orbis`, each with all five faces covered and "the windows show it now".
- **The Orbis's windows were never treated (H63).** At the crater port the panes worked: 19 to 61 known, 3,300 to 37,800 treated draws a 5 s line, side tests running. At the Orbis, the session's second station, every 5 s line from 20:22:03 to the log's end reads `panes treated 0`, `panes known 0 exterior 0`, `tests 0`. The shares line said `no family draw` once at 20:21:22 and never changed, while mark passes ran (416 to 820 a line). So no draw of the glass family (VS `98397963AAEC45D3`, PS `66B08F89E4926C01`) matched on the immediate context there, and the culled station showed through untreated glass. Flight 55's Orbis, first station of its session on e9e3e1f3 (before main was merged), had 34 panes known. Session 054706 (a `CraterPort`, then an `AsteroidBase`) treated panes at its second station, so a second station alone does not break it.
- **H63's candidates**, each with what tells it apart:
  1. The glass is not drawn there (the store or rig cull, or the game's own culling): with F2 off the shares line lists family draws again if the cull is the cause.
  2. It is drawn with another shader pair: an F3 capture there lists it among the blended draws, with the pair read from the context.
  3. It is drawn on a deferred context: the pane path measures and treats only the immediate one.
  4. It is withheld earlier in the scope (the people's vertex shader, the light test, a withheld family): a count by exit names the step.
  5. The binding shadow names another pixel shader for it: a sampled `PSGetShader` against the shadow shows the mismatch.
- **H62's data came in at all three bakes;** not yet read record by record. First reading: on the crater port's face 1, the band frames (12782 and 12783, 487,145 and 486,990 texels cut at 20 to 50 m) carry 349 records within 60 m of the centre. The clean frame (12784, 223,264 cut) carries 142. The room's own records are judged, and culled by the look, on the first two and absent from the third.
- **Main merged into the branch** as 4faddbd8 at Sean's word: 9bd1b9dd (perf A1 to A5, which rewrites the pane scope and keeps the backdrop shaders past the cube's release) and b8a71fdc. Not built.

## The glass census for H63 (built 2026-11-08, on the merged tree; removed 2026-11-09)

- **Removed after Flight 59,** at Sean's word ("Yes, take it out"). H63 and H65 are answered, and it cost a few reads on every armed draw. The pane scope is back to the code before d5241a7b, plus the learned glass shaders. Each learned shader's `joins the glass family` line stays, and the 5 s cull line's `panes treated` still shows the windows being treated.

- **The instrument (TEMPORARY):** every armed draw outside a capture frame passes `glassCensus` at the top of the pane scope. It is tallied by context (immediate or other) and by whether its vertex shader, its pixel shader or both are the glass family's, read as the scope reads them: the binding shadow on the immediate context, the context's own elsewhere. The pair's draws are tallied by the step that turns them away (the people, the light test, a withheld family, sealed, a far glow, no pixel shader, not listed, the vertex shader or index count) or as matched. One immediate draw in 64 has the shadow's two shaders checked against the context's own. The pixel shaders drawn with the glass's vertex shader are named, the first four in a window.
- **What it logs:** `concourse glass census 5s (TEMPORARY, H63)` every 5 s while the scope tallied a draw, with the feeds' state (treat, shares, effects, withhold, mode, list). No line means the scope was not armed.
- **How it answers H63's candidates:** (1) not drawn: the pair reads 0 everywhere with the cull on, and F3 with F2 off lists it; (2) another pair: the glass's vertex shader shows with another pixel shader, named; (3) another context: the pair reads under "other"; (4) turned away: an exit other than "matched" holds the pair's draws; (5) the shadow: the sampled mismatch counts are not 0.
- **Cost:** two shadow reads a draw on the immediate context, two `Get` calls a draw on any other, two more on 1 immediate draw in 64. Temporary, for one flight.

## Flight 57, the list as written

VR, Steam, the merged tree with the census, promoted by `--dll-only` (`python tools\edup_log.py --target steam --expect-build <that commit> --grep "concourse glass census|concourse cull 5s|concourse pane capture|concourse bake: vr: finished"`). Fly to a first station and go up to its room, then to an `Orbis` (or any second station) and up to its room, as in Flight 56. In each room: look straight at a large window and press F3 once; press F2 (the cull off), wait 5 s, look at the same window and press F3 again; press F2 to put the cull back.

## Flight 57 results

Flown 2026-11-09 in VR on d5241a7b, log `edvr_gfx_20261109_045925`, at the `Orbis` MacLeod Market (38 Lyncis, from Elite's journal), the session's only station. The bake ran at 05:01:08 in 344 ms, with all five faces covered. F3 was pressed at 05:02:28 with the cull on. F2 turned it off at 05:02:31, F3 was pressed again at 05:02:33, and F2 turned it back on at 05:02:35.

- **The census answers H63 with candidate 2.** Every line from 05:01:12 to 05:04:42 reads the same. The glass pair drew 0 times on either context. Its vertex shader drew with PS `500D228FA85C483C`, 11,659 to 48,090 draws a line (about 45 an eye a frame), and with the logo's `98337732`, up to 4,862. Other contexts drew 0. The shadow matched the context's own shaders on every sampled draw, 50,000 to 139,000 a line. With F2 off (the 05:02:32 and 05:02:37 lines, treat 0) the pair still drew 0, so the cull does not hide it.
- **Why the pixel shader changed:** Flights 55, 56 and 57 were all at MacLeod Market. In Flight 55 the glass matched there, with 34 panes known. In Flight 56 a settings apply in Elite's menu came at 20:21:09, docked in the hangar before the room: `vr ssaa gate: copy call 5: ... e+0xA0 3->2; e+0xAC 3->2; e+0xC4 3->2; e+0x118 3->2`. Elite wrote `Custom.4.4.fxcfg` and `Settings.xml` at 20:21. The crater port before that apply matched and the Orbis after it did not. Flight 57 has no apply, and the settings are the same. All three flights agree with this (H64).
- **Which setting:** the file now reads `MaterialQuality` 2 (High). Ultra is 3 in `GraphicsConfiguration.xml`'s `Materials`, which sets a `MaterialQualityLevel`, and a material's level would pick its shader. Three other settings also went from 3 to 2, and the log names them only by offset.
- **`500D228F` is in no shader dump:** the folder holds 1226 shaders up to 2026-11-08 10:46, `66B08F89` among them. That fits a shader the game creates only at that setting.
- **Neither F3 capture lists the glass.** Both overran the pool (576 and 1135 blended draws past its 1024). The glass draws after the light volumes, so it never got a query. In a busy frame the capture cannot see late draws.

## Material Quality to Ultra (Sean, after Flight 57)

Log `edvr_gfx_20261109_051401`, d5241a7b, MacLeod Market. Sean: "setting it to ultra fixes".

- **One setting changed:** the apply at 05:16:04.999 reads `e+0xA0 2->3`, so `e+0xA0` is Material Quality. The other three dwords lowered on 11-08 played no part.
- **The glass matched from the next line on:** the pair had drawn 0 times at High. From 05:16:07 it drew 6,638, then 12,441 to 14,152 a 5 s line, every draw matched as a pane. `500D228F` was gone. The logo's `98337732` is the same at both levels.
- **Not yet known:** Low's and Medium's pixel shaders. Whether `500D228F` draws only the glass at High: it ran 11,659 to 48,090 a line against Ultra's steady 13,000, and the census logs neither index count nor blend. Whether another setting also changes the glass's shader: it reads the shadows, so the two shadow settings are the likeliest.

## Flight 58, the list as written

VR, Steam, on the installed d5241a7b, with nothing to build. Two lines in Steam's `edvr.ini`, both temporary: `glare_shader_dump = 1` under `[advanced]`, which writes each new shader the game creates to `edvr_logs\shaders` and is read at launch, and a `[diag]` section with `concourse_panes = 98397963AAEC45D3/500D228FA85C483C`, so High's glass is treated too. Start Elite fresh and go to MacLeod Market's room with the cull on.

1. At a large window, set Material Quality to Low, Medium, High and Ultra in turn. Apply each and look at the window for 20 s. At High: does the bake show in the windows, and does anything that is not a window take it?
2. At Ultra, set Shadow Quality to its lowest, apply and look for 15 s, then put it back. Do the same for Spot Shadow Quality.
3. Set back what you fly with.

Read with `python tools\edup_log.py --target steam --expect-build d5241a7b --grep "concourse glass census|copy call|concourse cull 5s"`. At each level the census names the glass's pixel shader, and the `copy call` line names the offset each apply changed. The dump holds each new shader's code, which can be compared with `66B08F89`'s. The 5 s cull line's panes known, at High against Ultra, says whether `500D228F` draws only the glass. After the flight, take both lines out of `edvr.ini`.

## Flight 58 results

Flown 2026-11-09 in VR on the Frontier install (`elite-dangerous-odyssey-64`), build a6caa1b8 (no census), log `edvr_gfx_20261109_052109`, at MacLeod Market. The two lines put in Steam's `edvr.ini` played no part. Frontier's own `edvr.ini` already had the shader dump on. Sean, by eye: Material Quality High, Medium and Low "did not work". With Material at Ultra, Spot Shadows "did not work on medium and low, but did on high/ultra". Directional Shadows "did not affect it".

- **The log agrees with each step** (the 5 s cull line's `panes treated`, against each apply's `copy call` line):
  - Material Quality is `e+0xA0`. At 3 (Ultra) the windows were treated, about 14,600 draws a line. Within one line of 3 to 2 at 05:23:19 that fell to 0, and it stayed 0 at 1 and 0. It came back at 3 (05:24:44).
  - Spot Shadows is `e+0x10`. At 4 and 3 the windows were treated, about 12,000 a line. At 2 (05:25:23) and 1 the count was 0, and it came back at 4.
  - Directional Shadows is `e+0x4`. From 4 down to 1 and back, every line treated 10,855 to 12,106.
- **The rest of the cull's lists held:** across the sweep the light cones (83,933 to 112,340 a 30 s line), the far glows, the particles, the smoke and the planet's families kept matching. Only the glass broke.
- **The glass's layout:** its pixel shader takes constant buffers `CB1[327]` and `CB2[3]` and inputs `v0.x v1.xyz v2.xyz v3.xyz v4.xy`, and writes one output, `o0.xyzw`. Of the 1030 pixel shaders in the two dumps (Steam and Frontier), five have it, all reading textures t3 to t8 or t10:
  - `66B08F89E4926C01`, 2440 instructions: Material Ultra, Spot Shadows High or Ultra (measured).
  - `CAB49794BB439D03`, 2264 instructions, first dumped 2026-10-17 on both installs: likely Material Ultra with Spot Shadows Medium or Low. Spot Shadows was at 2 in September's saved file, and its apply created no new glass shader, so its shader was already in the dump.
  - `500D228FA85C483C`, 1368 instructions, created at the High apply (05:23:19): Material High (measured by the Steam census in Flight 57). Medium's apply created none with the layout, so Medium likely shares it.
  - `33DFFE4870508698`, 1305 instructions, Steam only, first dumped 2026-09-07: not placed. Likely High or Medium with low Spot Shadows.
  - `FFBE11D8C54B2696`, 835 instructions, created at the Low apply (05:24:12): Material Low.
  - Low with low Spot Shadows is in neither dump.
- **The vertex shader stays `98397963`:** its outputs are exactly the five's inputs. None of the four vertex shaders created at Medium and Low has those outputs. The logo's `98337732` has `CB2[2]`, and the other forward materials created at the applies differ too, for example `A30A55D7` has `CB2[4]` and `v2.xyzw`.

## The glass by its layout (built 2026-11-09, at Sean's word "Build it")

- **What it does:** each pixel shader the game creates is checked once, by its bytecode, for the glass's layout (`concourse_glass_layout.h`, `glassLayoutPs`). The layout is constant buffers 1 (any size) and 2 (size 3) and no others, the inputs v0.x, v1.xyz, v2.xyz, v3.xyz and v4.xy and no others, and one output, o0.xyzw. Called from `hookedCreatePS` (`concoursePaneNoteShader`), a shader that matches joins a learned set of up to 32 hashes. In `beginBody` a learned pixel shader is a pane when the vertex shader is `98397963`, at any index count. `g_list`, its entry 0 and `diag.concourse_panes` are unchanged. This covers every Material and Spot Shadows level, and any other setting that keeps the layout, including combinations nobody has flown. The five hashes alone would leave Low with low Spot Shadows untreated, and any setting not tested here.
- **Cost:** the check reads the container in place: the chunk table, then at most 1024 words of the program, and only its declarations. It neither copies the shader nor recomputes its checksum. Per draw it adds one scan of the learned set, which holds one to a few hashes, for the draws whose pixel shader is not in `g_list`.
- **Logged:** `concourse pane: a new pixel shader has the glass's layout: ps <hash> joins the glass family (<n> known)`, once per shader. The TEMPORARY census counted the glass's vertex shader with any glass-family pixel shader as "the glass pair" until it was removed after Flight 59.
- **Checked:** `optimized_station_test` has synthetic containers for the layout. CB1 of another size passes. A CB2 of size 2 (the logo's) fails, and so do an extra constant buffer, `v2.xyzw`, a sixth or a missing input, a second output, an `oDepth` output, a vertex shader, a wrong size field, truncations and garbage. Its local `--scan` mode over both dump folders matched exactly the five, of 1030 distinct pixel shaders.

## Flight 59, the list as written

VR, Steam, the layout build (its commit is in the Status block). In MacLeod Market's room with the cull on, start at Material Quality Ultra and Spot Shadows Ultra. Look at a large window for 20 s after each step:

1. Material Quality High, then Medium, then Low.
2. At Material Low: Spot Shadows Medium, then Low.
3. Material Quality Ultra, with Spot Shadows still Low.
4. Set back what you fly with.

At each step: do the windows show the bake, and does anything that is not a window (a wall, the logo on the glass) take it?

## Flight 59 results

Flown 2026-11-09 on Steam's a89e193e, log `edvr_gfx_20261109_055831`, at MacLeod Market, one session: VR, then the 3D mode off (flat) and back to VR. Sean: "tested in flat and then back to 3D HMD mode and it looked good at all levels".

- **Each new glass shader was learned and treated:**
  - At load, Material Ultra with Spot Shadows Ultra: `66B08F89` (1 known). Treated, 4,266 to 30,212 draws a 5 s line.
  - 06:02:10, the 3D mode from 3 to 0 (flat): still treated, with the VR bake kept.
  - 06:02:49, Spot Shadows from 4 to 1: `CAB49794` learned (2 known), treated 5,328 to 5,850. This confirms it is Ultra with low Spot Shadows.
  - 06:03:08, Material Low with Spot Shadows Medium (and `e+0x7C` from 0 to 2, not named): `FFBE11D8` learned (3 known), treated 2,412 to 5,213. In Flight 58 Low with Spot Shadows Ultra created the same shader, so Low's glass does not change with Spot Shadows.
  - 06:03:32, Material High with Spot Shadows High: `500D228F` learned (4 known), treated.
  - 06:03:44, back to VR: treated, 4,818 to 11,616.
- **The logo stayed out:** at low Spot Shadows it is drawn with `8717694A527EC745` in place of `98337732`, and the layout check passed it by. No line shows either one matched.
- **Not in the log:** Material Medium, and `33DFFE48` (probably High or Medium with low Spot Shadows).
- **VR's re-bake after the switch back worked:** the first try (06:03:46.705) ended before aiming, with no rows back. The second, 0.28 s later, covered every face in 250 ms. Flight 55 had two tries end before aiming and no later one.
- **A separate problem, by eye:** at the start, in VR, Sean saw blurry double vision on parts of the scene. The game had been launched with Steam's VR option while Elite's `Settings.xml` held `StereoscopicMode` 0 (the file was written at 05:37:02; this log reads "3D mode 0" at startup, and the mode rose to 3 at frame 325). It went away after the 3D mode was cycled off and on. Sean takes it for a separate problem. The frame median there was 11.18 ms against a 11.1 ms period. Not chased in this arc. Read with `python tools\edup_log.py --target steam --expect-build <commit> --grep "glass's layout|concourse glass census|copy call|concourse cull 5s"`. Each new glass shader should log one `joins the glass family` line. At every step the census should read `matched as a pane` above 0, with only the logo's `98337732` named beside the glass's vertex shader.

## A user's bar windows, and the clear on all the outside glass (2026-11-16)

A user's report, passed on by Sean: v0.22.2 (6AF9E061), flat, DLAA preset K at 2560x1440, a large station's concourse (anchor `SA_StationHub_Compact`), log `edvr_gfx_20261115_203436` in two bundles (the tail runs to 03:01), a screenshot and two clips.

- **What they saw:** from the concourse at Window 3D distance 100 to 150, the whole bar window showed the bake, and it read right: the live view's layout, darker and greyer. Inside the bar, at the glass, the bake was soft and out of place (a lit shape, probably a ship baked in passing) until the clear faded it. They raised the setting to 250, and from the concourse one bar pane went live while the pane beside it kept the bake (the screenshot).
- **Why:** the bar's outside glass is three meshes. From cam_local -1.69 25.26 14.26 they fill 2.2% (n=24), 1.5% (n=30) and 1.4% (n=36) of the view. The clear followed one index count, and in the lifting, fading and clear phases every other exterior pane kept the bake over its whole area. At 100 to 150 none of the three reaches its trigger from there. At 250 n=30 alone did, since it enters at 0.3 of the share: it went back to the bake at 02:42:26 at 100, lifted at 1.1% at 02:43:15 at 250, and lifted again at 02:52:45.
- **Also in the log:** the side tests chose the clearing pane from any viewport. Two readings of n=30, "41.4%" and "63.9%", were shares of about 65,500 pixels (27,122 drawn / 0.414), not of the screen. The share bins held 8 index counts, and at cam_local 4.34 22.16 -8.52 n=30 was not among them. At 250 the clearing role passed between n=36 and n=30, and the cull went on and off 0.03 to 6 s apart (02:40:53 to 02:41:12).
- **The change (25e49f5d):** in the lifting, fading and clear phases every exterior pane fades together. The trigger is the total share of all the outside glass in the main view (`cc::exteriorGlassShare`). Each pane counts once, at its larger share; n=30 is divided by 0.3, as its entry was; viewports under half the main view's size are left out (the main view is the visit's largest viewport, decaying 0.1% a frame); and before the side tests have run, or in VR, a pane of the bay's index count counts. The clearing pane, its pick in the side tests and the `the clearing pane is now` line are gone.
- **What it should do:** from that concourse spot the bar's total is about 8.6%, so it clears there from about 150 (8.9%) and stays the bake at 100. The bay clears a little sooner when other windows are in view as well. The bake's look up close is unchanged.
- **Logged:** `concourse clear: <phase> (exterior glass X% of the view, weighted, cam_local ...)`. `concourse pane shares:` starts with `exterior X% (weighted)`. Both 5 s lines end with `share samples dropped N`, which should stay 0.
- **Checked:** `optimized_station_test`, 1511 checks, 0 failures. The new cases: the bar's three meshes (0.086), two eyes counted once, a 256x256 sample left out, interior left out, unknown side only at the bay's index count, an outpost whose bay is n=30, the main view's growth and decay, and the clamp at 1. Installed on Frontier (`elite-dangerous-odyssey-64`, scope both, with XeSS). NOT FLOWN.

## The bar flight, the list as written

Flat, Frontier, the 25e49f5d install, a large station (Coriolis or Orbis), Window 3D distance 100.

1. From the concourse, look at the bar: all its outside glass shows the bake, and no pane is live.
2. Walk into the bar to its outside windows. When they clear, every pane fades at once, and none keeps the bake beside a live one.
3. Back on the concourse, set 150, then 250: the whole bar window clears together, from further away as the value rises.
4. The bay, then the ship services counter: each clears about where it did before, and walking away it goes back to the bake once, without flicker.

Read with `python tools\edup_log.py --target <the elite-dangerous-odyssey-64 folder> --expect-build 25e49f5d --grep "concourse clear|pane shares|pane 5s"` (`--target frontier` names two installs on Sean's machine).

**First try, not a test of the change:** flown 2026-11-16 on 25e49f5d, log `edvr_gfx_20261116_115714`, at a Coriolis. Sean: "the snapshot is not displaying correctly and it's falling back to a grey haze". The game started in VR (3D mode 3) and switched to flat at 11:57:46 (`profile switch: vr -> flat`). The flat bake then refused: `concourse camera test: concourse-camera-test-producer: 0x2874290 is already patched by another EDVR module; skipped`, then `concourse bake: flat: no bake`. With no cube the panes were drawn `mirror-steady`, a flat fill: that is the grey (`panes treated 45407 draws (mode mirror-steady, 1 slots)`). This is the open item "a flat bake in a session started in VR", not 25e49f5d. The clear itself ran on the total: it lifted at 3.2 to 8.3% of the view against a 3.2% entry (Window 3D distance 250 in Frontier's `edvr-flat.ini`). Fly the list again from a flat launch: Elite's 3D mode off before the game starts, or `/novr`.

**Flown 2026-11-16 from a flat launch, FLOWN OK:** 25e49f5d, log `edvr_gfx_20261116_120304`, `/novr`, the same Coriolis. Sean: "Fixed". The bake took 188 ms with 5 of 5 frames, and the panes were drawn `mirror-backdrop`. Walking to the bar (cam_local -1.65 23.84 10.24 to -3.28 25.80 14.16), the clear lifted at 3.4% of the view and was clear at 7.0%, against the 3.2% entry at Window 3D distance 250. At the bay it lifted at 20.2% and was clear at 23.8%. In the clear phase the 5 s lines read `treated 0 draws` (no pane kept the bake) and `share samples dropped 0`. One return to the bake, at 12:05:08.6, printed 5.2% (above the entry), and the next lift came at 12:05:13.7. The log does not say why, and Sean did not report it.

## Before the merge to main (2026-11-08)

Sean: "Let's merge this to main if it looks good", then "Fix, then merge".

- **The cube dump is gone** (f5c8e2a2): its job (H58 to H61) was done, and it wrote ten BMPs after every bake. The per-distance counters and the draw diff stay, TEMPORARY, for H62.
- **Flat takes no bake without the camera's producer hook.** The bake and the look move flat's camera only through the hook on 0x2874290. After Flight 55's switch from VR, the install skipped it as "already patched by another EDVR module" while the other hooks went in. So the guard tests that hook (`concourseCameraTestCanMove`), not "any hook". Without it no bake is armed and nothing holds a Present; the windows keep the bake they have. Not flown.
- **Still open at the merge:** H62's record; VR's re-bake right after a switch back (two tries end before aiming, nothing held, no later try); the temporary keys and instruments in the Status block, to be proposed for removal when the arc closes.

## Flight 55 results

Flown 2026-11-08 in VR on e9e3e1f3, log `edvr_gfx_20261108_130757`, at an `Orbis`, then switched to flat and back to VR. Sean: "seems to work now".

- **The fence works:** every aimed window read `written 0x15`, `0 foreign`. The first call's fourth lanes read `0 0 0 nan`, which confirms Flight 54's inference.
- **The VR bake works:** 313 ms, every face covered on its first aim, and every direction's dump `unwritten 0.0%`. The dump took 160 ms over 6 ticks, none of them blocking.
- **The union covered the band:** face 1's dwells 1 and 2 had it (16.2%, 20 to 50 m) and dwell 0 did not, so the cube took face 1 from dwell 0.
- **H62's band is named by the draw diff.** On face 1, five draws at start indices 12785936 to 12871242 (base vertex about 2.90 to 2.96 million; 51066, 26667, 14727, 9555 and 7515 indices) are absent on the clean frame and present on both band frames. Each was drawn in three passes:
  - the colour pass, VS `EB5234DB6ADB491D` and PS `CB9F297EFF264251`, once an eye;
  - two depth-only passes, VS `C2208C162D010083` (4 or 5 times) and `8BC3FFBCA98BCD4C` (3 times).

  It is one mesh, drawn on some frames of the bake and not others. Which record it is, and why the look's judgement passes it on some frames, is not known. The other differing keys (counts 0 0 1, 1 0 0) are single depth-only draws, likely shadow cascades.
- **Flat after the switch could not bake.** Two flat bakes held the screen for 266 ms each, with "0 frames at the centre", and were cancelled. The flat camera hooks are bound only when the session starts in flat ("bind: not installed: flat only for now, and this is the VR profile" at 13:07:58). The windows kept the VR bake, since only a publish replaces the front cube.
- **Back in VR, the re-bake came too soon.** "The render size has risen since the bake" armed two tries 0.8 s after the switch. Neither saw a tone draw ("tone draws 0"), so both "ended before aiming", with nothing held, which is the review's fix at work. No later try came, and the windows kept the first VR bake.

## Flight 55, the list as written

VR, Steam, e9e3e1f3 (a1e8a4c8's source with a doc commit on top, promoted by `--dll-only`). Read with `python tools\edup_log.py --target steam --expect-build e9e3e1f3 --grep "concourse bake: (vr|dump)"`. It is Flight 54's list again: the Dodec or this Orbis, on foot in the room, by a ride up or a load in the room. Look at the window, then at the room's far end.

- **The log:** the stub lines in aimed windows read `written 0x15` with `0 foreign`, and show the `pads` values. Every face's capture lines say `writes`, each `dump` line reads `unwritten 0.0%`, and the `vr draw diff` lines name the band's draws.
- **If the stub writes nothing again,** the bake should stop after six windows with "the stub wrote no call", not hold the headset for a second.

## Flight 54 results: the fence refused Elite's own base

Flown 2026-11-08 in VR on e08534eb, log `edvr_gfx_20261108_124344`, at an `Orbis`. Sean: "it didn't seem to engage riding the elevator up".

- **Three bakes ran and captured nothing:** two at the load (12:46:06, 828 ms; 12:46:07, 656 ms) and one after the ride up (12:47:14, 1234 ms). Every face read "NEVER FILLED" and 0 frames were taken.
- **The stub wrote no call:** every aimed window read `written 0x0` with `3 foreign`, so the eyes stayed with the head ("863 m off" the aim) and every capture was "not within 0.5 m of the origin".
- **The fence's own test was wrong.** Between windows the base moved about 3 cm and its axes well under a degree, inside the 0.25 m and 0.995 limits. But `baseMatches` required all 16 floats of the pose to be finite, and the game sets only 12 (each row's x, y, z; `previewcam::finite` reads the same 12). Every call failed on the unset fourth lanes. The rig's poses had finite fourth lanes, so the 8 checks passed. The unset lanes are INFERRED from this; the next build logs them.
- **Each bake still held the headset:** 656 to 1234 ms. The rows came back, so the stop on missing rows never fired.
- **Fixed in a1e8a4c8:** `baseMatches` reads the 12 lanes, with a rig check on NaN and infinite fourth lanes. Six aimed windows in a row with no call written now stop the aims ("the stub wrote no call"), as six without rows do. The stub line shows the first call's fourth lanes (`pads`).
- **Also seen:** each failed bake was followed at once by `concourse cull: off (the anchor record was not seen for 5 s)`. Not chased.

## The branch review's fixes (2026-11-08, b3d00cb1 and e08534eb)

An adversarial review of the branch against main found eight items. All are fixed, built with every gate passed, and none are flown.

1. **No blind aim.** The aims, the hold and the near raise start only after a world check made from rows that came back. With none, the bake ends before aiming ("ended before aiming"). Six aimed ticks in a row with no frame's rows stop the aims ("aiming stopped"). Flight 53's second bake held the headset 969 ms this way.
2. **The eye stub's writes are fenced.** A call is written only if its own pose matches the base Elite is using: within 0.25 m, each axis dotting above 0.995 (`baseMatches`, 8 rig checks). Others count as `foreign` on the stub line. An aim is a lease of two windows (e08534eb: one window left a gap between the tick's close and the same tick's aim), with a 500 ms backstop.
3. **The near is put back to the value it replaced,** not a fixed 0.025, in flat and VR.
4. **Flat's held image is copied back only onto a back buffer of the same size and format.** Otherwise it is dropped, with a line once per bake.
5. **The three indirect draw hooks pass through the pane scope,** so the cull's withholds and the draw diff see them (kinds Y, Z, A).
6. **The draw diff's key adds the base vertex and the draw's kind.**
7. **The draw diff no longer works in the draw hook.** The hook only appends keys; the present tick sorts and logs one finished face a tick.
8. **The cube dump no longer blocks.** The staging copy is mapped without waiting on later ticks, one direction's files a tick, and the time is from publish to the last file.

## Flight 54, the list as written

VR, Steam, e08534eb (the fixes above on bb69891c, promoted by `--dll-only`). Read with `python tools\edup_log.py --target steam --expect-build e08534eb --grep "concourse bake: (vr|dump)"`. Also watch the stub lines' `foreign` count (it should be 0 on aimed frames) and the finish line's "frames not at the centre" (a lease gap would show there).

1. **The same Dodec,** on foot in the room, by a ride up or a load in the room. Look at the window, then turn round and look up the room's other end (face 1, where Flight 53's last bake left a band).
2. **The log:** every face's `vr capture` lines say `writes`, and each `dump` line should read `unwritten 0.0%`. The `vr draw diff` lines on the band's frames name the draws to identify.

## Flight 53 results

Flown 2026-11-08 in VR on d7189728, log `edvr_gfx_20261108_104337`, at the same Dodec. Sean: "that seems to have fixed the issue". There were three bakes: 10:45:29 (343 ms), 10:46:25 (969 ms, face 0 short at 94.5% after 3 aims) and 10:46:26 (281 ms, which followed it). The dump images are in `edvr_logs\concourse_bake\`; the second and third bakes share a timestamp, so their files are the third's.

- **Every aim was exact:** 0.000 to 0.004 m against the prediction, near 5.000 m on every aimed frame. Both eyes agreed on every count, frame by frame.
- **Something 20 to 50 m away comes and goes with the camera unchanged.** It is a straight-edged band across the top of the face: in the dump of face 1 it is 17% of the image, magenta, with the far structure below it.
  - First bake: face 0's dwell 0 (13.1%), then face 1's dwells 0 and 1 (16.2%).
  - Third bake: face 0's dwell 0 (13.1%), then face 1's dwells 1 and 2 (16.3%). Dwell 2 was the write frame, so face 1's published bake has the band unwritten.
  - On frames without it, those texels read 200 to 4000 m: the far structure behind.
  - Face 4 holds a steady 0.1% under 50 m in every frame. Faces 2 and 3 have none.
- **Flight 52's hole was this band:** face 0's single frame was its first aimed frame, cut at 13.09%, the share this flight's face 0 dwell 0 has in both bakes. Face 0's write frame (dwell 2) was clean in every bake, which is why the window looked fixed.
- **Ruled out:** H58, H59 and H61 (see Ruled out). H60 holds in a narrower form, H62 below.
- **The floor of every horizontal face is nothing drawn** (blue in the dump, 31% of faces 0 and 1): below the culled room there is no geometry, as Flight 24 saw in flat. No window looks there.
- **H62 (open):** a piece of the room's upper structure, 20 to 50 m from the centre along both ends of a2, is drawn on some frames and not others in VR's bake. It is gone for most frames, so the look's judgement does cull it, but not on every frame. Its source is unknown: a draw the store does not judge, or a judgement that lags. Tell: the draws present on the band's frames and absent on the clean ones, with the same camera.

## Flight 53, the list as written

VR, Steam, d7189728 (c168c2af's source with the doc commit on top, promoted by `--dll-only`). Read with `python tools\edup_log.py --target steam --expect-build d7189728 --grep "concourse bake: (vr|dump)"`, and look at the images in `edvr_logs\concourse_bake\`.

1. **The same Dodec, or any station on the list,** on foot in its room, by a ride up or a load in the room. The freeze is about three times as long (15 aimed frames). Look at the same window and say whether the hole is still there.
2. **The log:** each `vr capture` line has the frame's dwell (0, 1 or 2 of 3), whether it wrote, and the counts by distance. The `dump` lines give each direction's unwritten and far shares.
3. **If convenient, the same station in flat.** The dump runs there too, for a side-by-side.

## The hole instrument (built 2026-11-08, c168c2af)

All of it is TEMPORARY, for H58 to H61. The build passed every gate, and the two new shaders were also compiled offline. Installed on Steam (both profiles verified).

- **Dwell:** each face is aimed for 3 frames. All three are counted, and only the third writes the cube. If H58 holds, writing a settled frame is also the fix.
- **Counters:** per eye per frame, the texels past the cut, those written, those with nothing drawn, those cut by distance from the eye (under 5, 5 to 20, 20 to 50, 50 m and over), and those drawn by distance (under 50, 50 to 200, 200 to 1000, 1000 to 4000, 4000 m and over).
- **Dump:** after every published bake, flat and VR, each direction goes to `edvr_logs\concourse_bake\` as a 512x512 image of the cube over 90 degrees: colour, and distance with nothing drawn in blue, unwritten texels magenta in both. The unwritten and far shares are logged.

## The VR bake (built 2026-11-08, 194805e2)

Built on Flights 50 and 51 and the research in "Flight 51 results". The pure math is in `concourse_vr_bake_core.h`, checked in the station rig (1462 checks): the game's row multiply, the head's conjugation (`gameFromOpenVr`, G(X) = (S X S)^T), the inverse (one macro text for C++ and HLSL), the face rows (u×f, u, f, centre), the aim `B = H^-1 · D`, the pitch from the frusta (+5.5 degrees for a Quest 3's, 0 for the Pimax's), and the face test.

- **When:** in VR, once the stub is installed and the game's tonemap draw has been seen (`the bake is available`). `concourseStationOk` then lets every type on the list run in VR; a carrier still takes none. It arms as flat does: a ride up, or the room in view with no bake.
- **Measure, then aim.** First a world check: the store's camera against the eyes' row 275 midpoint, on frames before any aim. Then each aim tick writes B for the next face, into every stack call of the stub that is not the identity (calls 0, 2 and 4), with the eye cameras' near (RVA 0x5E85B8C) at 5 m. Each tick asks the runtime's hold for 3 frames, so the held frames run from the first aim to 3 frames after the last. The near is put back after.
- **Capture.** Each eye's b1 rows 270 to 279 are copied on the GPU at its first scene draw (`glitchFrameIsSceneDraw`'s five scene shaders; the eye from the depth probe's first binds). At its tonemap draw, eye by the draw's order, a VR variant of the bake's shader inverts them on the GPU. It takes the frame only if that eye's row 275 lies within 0.5 m of the centre, and writes the texels of the face in view, on the face's own square and inside the eye's clip. Both eyes write, and texels are counted per eye.
- **Ends:** every wanted face covered, or 48 ticks, after which what was captured is published and the short faces are named. The panes are treated in VR once a bake exists; until then the glass stays the game's.
- **Not done:** no re-bake in VR when the render size rises. Coverage is the best single frame, not the union.
- **The implementer's worries:**
  1. The store's world against the render world: a floating render origin would make the windows slide.
  2. The eye order of the tone draws against the rows' order: a mismatch would capture twice the cant off. The finish line logs both orders.
  3. A frame with none of the five scene shaders captures nothing.
  4. The stub's calls 2 and 4, and forward = +row 2, are unproven in VR. A sign error shows as face 4 never filled.
  5. Mode 5/6 cameras (perhaps the 2D screen) ignore B: every frame would read "not at the centre".
  6. Two full-cube dispatches per bake frame, during held frames only.

## Flight 52, the list as written

VR, Steam, 194805e2, DLSS on. Read with `python tools\edup_log.py --target steam --expect-build 194805e2 --grep "concourse bake: vr|concourse cull"`.

1. **A station that bakes in flat** (a Coriolis, Orbis, asteroid base, dodec or port), on foot in its room, by a ride up or a load in the room. In the headset: a short freeze (the hold), then the windows show the station, sunlit, in stereo, with the cull on. Say whether anything moves wrongly with your head (sliding means the worlds differ), and whether a band at the top or bottom of the windows is empty or pale.
2. **The log:** `vr: the bake is available`, `vr world check` (what it found, and whether it applied an offset), the `vr aim` and `vr capture` lines, then `vr: finished ... faces covered`. Both orders are on the finish line.
3. **Walk to the bay:** the clear on approach, as in flat.
4. **Frame time:** F2 off and on in the room.
5. **A carrier, if near:** unchanged from Flight 50's list.

## Flight 51 results

VR, Steam, 4b74d75c, log `edvr_gfx_20261108_065519` (06:55 to 06:57), runtime log `edvr_openxr_20261108_065521_109_41700`; Pimax Crystal Super at 4032 per eye, DLSS. Sean: "I saw the flashes, no carrier yet, otherwise menu looks fine". Only a `Dodec`, again left to the game in VR.

- **The camera route works.** Two presses, `5 of 5 frames wrote a pose` each. Each frame has 6 stub calls in one order. Calls 0, 2 and 4 are at stack addresses and carry the head pose, `t(1.363 -0.787 0.000)`, rows rotated. Calls 1, 3 and 5 are the identity on the heap. On every probe frame call 0 was written from `t(1.363 -0.787 0.000)` to `t(1.363 29.213 0.000)`, and Sean saw the flashes. The pose is in its own space, not the store's world: the store's camera at the same time was tens of metres off.
- **The capture point works.** Two tonemap draws a frame, eye 0 then eye 1, on stable targets (`...930D20`, `...931DA0`). Each has H, format 26 (R11G11B10_FLOAT), and depth, format 19 (R32G8X24_TYPELESS), both 2016x2017.
- **The hold works again.** 10 of 10 latched; the runtime trace has `withheld=14,replayed=14,empty=0`.
- **The per-eye camera reader is not usable for this.** `temporal eye rows` gave the two eyes identical positions and axes on every line (an IPD should part them), switched between a camera near the head pose and an axis-aligned one at (11.6 6.0 -45.5), and never showed the 30 m raise that was seen. So the VR bake cannot read each eye's camera back. It has to build each eye's matrices from what EDVR tells the game: the head pose it writes, and the runtime's eye-to-head transforms and projections.
- `ruled out: temporalPassEyeRows as the VR bake's per-eye camera, because in Flight 51 both eyes read the same rows, switching between two cameras, and missed a 30 m move that was drawn.`
- **The hold by eye:** Sean, "Didn't see any issues with *".
- **Research after it (2026-11-08, from the decompiler and a disassembly of 0x287B330, 0x8D2610 and 0x4E2640, build 334925):**
  - **W+0x3330 is not the head; it is the base.** That is the transform placing the runtime's seated tracking space in the render world. Each eye's camera is `eye_i = E_i · H · B` in row vectors, with E the eye-to-head transform and H the tracked head. 0x287B330 builds the camera from that alone: no origin or avatar term, and the runtime's projection copied verbatim. So the +30 m write moved the tracking origin, and both eyes with it.
  - **Aiming:** `B = H_g^-1 · D`, where D is the face's axes at the room's centre and H_g is the head in the game's z-negated convention. The game fetches H inside 0x283FA20, after the stub, so the latest head EDVR knows is a frame old. That error moves the aim, not the capture.
  - **Each eye's matrices:** the runtime publishes the head, both eye-to-head transforms (with the true cant) and the unjittered frusta every native frame (`native_temporal.h`; `nativeTemporalHeadPose`, `nativeTemporalEyeGeometry`). The near and far planes are stored but have no accessor. The exact matrices, with the jitter, the IPD and the world scale, are each eye's own b1 rows 270 to 279.
  - **The display's rows** (275, 270 to 273, 279) are the same layout in VR's per-eye draws (INFERRED from the shared writer 0x596830). Row 275 is each eye's own origin (READ: 6.06 cm apart), so the windows' march is true stereo.
  - **Coverage with no field-of-view change** (|tan| ≤ 1 on both axes per face):
    - Pimax Crystal Super (Pimax OpenXR): eye 0 L -51.4, R +41.2, U/D ±46.8 degrees. The union covers a face with about 1.5 degrees spare, and there is no hidden-area mesh.
    - Quest 3 (VDXR): U 42.0, under the 45 needed, so the head is pitched up 5.5 degrees, leaving about 2.5 spare. It has a hidden-area mesh that likely blacks the face's corners.
  - Not verified: that B's world is the store's world in VR. The bake's first frame compares the store's camera with each eye's row 275, and can take the offset.
- `ruled out: writing the head pose for the VR bake, because the stub's pose is the base B and the game multiplies the tracked head in after it (0x4E2640 returns eyeToHead x head).`

## Flight 51, the list as written

VR, Steam, 4b74d75c, DLSS on. Read with `python tools\edup_log.py --target steam --expect-build 4b74d75c --grep "concourse"`.

1. **On foot at a station: numpad `-` twice,** a few seconds apart. A flash 30 m up for 5 frames should be seen. The log: `call 0 ... wrote yes` on frames 1 to 5, the six calls of a frame, `tone: ... eye 0 (by the draw's order ...)` and `eye 1` with depth and `temporal eye rows`.
2. **Numpad `*` once:** say how the hold looked (frozen, world-locked, or black).
3. **A carrier,** if one is near: `the cull runs (a carrier needs no bake)`, the bar's windows, the clear, F2 off and on for the frame time.
4. **The menu:** Window 3D distance and Skip rooms behind doors on VR's Performance page.
5. **Quit the game** at the end, for the runtime's trace.

## The VR proving build (built 2026-11-08, 7bc497b5)

Sean: "Build it". Also in it, on his word after F2 at each: an `AsteroidBase` (91734df8) and a `Dodec` (a new journal type; `"StationType":"Dodec"` read as another type before) join the automatic list. Both have the large stations' room (`Standard_SocialArea`, n=36); their F2 bakes took 171 and 172 ms, leaving out 832 and 793 people's draws.

- **Carriers in VR:** `concourseAutoOn` is on with the setting in VR. `concourseStationOk` lets a type run there only if it is on the list and needs no bake (`runsWithoutBake`: the carrier). Other types log `VR has no bake yet, so it stays the game's own`. In VR the glass is never treated (no haze), and the shares are still measured, so a window clears. Window 3D distance and Skip rooms behind doors are on VR's Performance page too.
- **Probe keys (TEMPORARY, VR only, log only, `concourse vr probe:` lines),** bound in Sean's Steam `edvr.ini`:
  - `hotkey.concourse_vr_hold_probe` (numpad `*`): `requestSubmitHold(10)`. Follow-ups at +3, +12 and +30 frames count the latches that took a held frame. The runtime's withheld, replayed and empty counts are in its trace at the session's close, not in the log.
  - `hotkey.concourse_vr_camera_probe` (numpad `-`): the rendered head raised 30 m along the room's up for 5 frames, visible on purpose. It goes through a second client of the ship preview's stub on `0x283FA20`, which writes only a frame's first call whose pose is not on the stack. Per frame it logs the stub's calls and the pose before and after. On those frames and the ones either side, per eye at the game's tonemap draw: H's format and size, the eye's depth, and the camera the eye was drawn with. That camera comes from the temporal pass's per-eye rows, which exist only with a temporal mode on; otherwise from one source camera.
- **Worries the implementer named:** the pose may be head-local rather than world (the before/after lines show it); "the first call is the head" is unproven (the per-call lines show it); the depth read can refresh the depth probe's pick for those 7 frames.

## Flight 50, the list as written

VR, Steam, 7bc497b5, with a temporal mode on (DLSS or TAA) so the camera probe has per-eye rows.

1. **A carrier:** `a station of type FleetCarrier: the cull runs (a carrier needs no bake)` and `concourse cull: on`. The bar's windows show the real sky with no haze, and the ships come back when the bar's glass clears. Numpad `/` steps the door rule. F2 off and on in one place for the frame time.
2. **A station that bakes in flat** (a Coriolis, Orbis, port or the like): `VR has no bake yet, so it stays the game's own`. No cull and no haze.
3. **On foot at any station or the carrier: numpad `*` once.** By eye: about 0.1 s frozen, world-locked, not black. The log: `hold: requested 10 frames`, then `the runtime's latch took 10 held frame(s)`.
4. **Numpad `-` once** (a flash 30 m up for 5 frames). The log: `camera frame ... wrote yes`, the pose before and after, and `tone: ... eye 0` and `eye 1` lines. Do it twice, a few seconds apart.
5. **The menu:** Window 3D distance and Skip rooms behind doors on VR's Performance page.
6. **Then quit the game,** so the runtime's trace holds the hold's counts.
7. **Flat, if passing one:** an asteroid base or a dodec runs with no keys (`the cull and its bake run`).

## Flight 49 results

35aa1d87, log `edvr_gfx_20261108_054121`, flat, Steam, the `CraterPort` with no keys (`the cull and its bake run`, 05:43:13). The bake at 05:43:16: `near 5 m, the room's cut 50 m`, 171 ms, 5 frames captured, `the people's draws left out over its frames: 1123`. Sean: "Bake looks good, let's move on to VR so we can release this feature". H57's fix holds: the sky and the sun are in the bake, and the sky was not cut.

## VR, next (Sean, 2026-11-08)

"After we fix this bake issue, let's pivot to VR so it works just as well as flat." His first VR look at the `CraterPort`: "it did not automatically take a bake, when I pressed F2, it culled it seemed but left a grey haze on the bay window". Then, after Flight 49: "let's move on to VR so we can release this feature".

**What keeps it flat (code survey, 2026-11-08):**
- `concourseAutoOn` returns false outside flat. `camTestInstall` refuses VR, so there is no bake.
- The bake's capture is called only from flat's resolve (`flat_runtime.cpp`, two sites). The hold is a desktop Present trick (`device_hook.cpp`, `concoursePaneBakePresent`).
- Window 3D distance and Skip rooms behind doors are `menu flat_performance` rows, so they are not on VR's menu.
- The haze is the designed no-bake treatment: steady panes at `kSteadyColour`, the bay at level 0.5.
- Nothing else is flat-gated: the store cull, the withheld families, the lift, the door rule, the real sky and the pane hooks (the VR draw paths build `ConcoursePaneScope`) all run in VR. INFERRED, not checked: whether VR's per-eye `cb1` rows 270 to 279, which the backdrop shaders read, match flat's; whether the withheld hashes match; and which eye `g_frameCam` reads.

**The VR bake's pieces (research, 2026-11-08):**
- **Camera:** `native_preview_camera.cpp`'s stub on `0x283FA20` writes the head pose before the eye offsets, and it shipped in rc.2 for the ship preview. In VR both the cull's copy and the draw's copy of the camera follow it, unlike flat's Flight 20. Any direction can be written. The eye offsets come after, so the IPD stays unless the runtime answers an identity eye-to-head transform. On Sean's canted Pimax each eye has its own yaw, so the bake's direction match (dot above 0.999) needs each eye's real forward. The near plane is the same `.data` float, so 5 m reaches VR too. Not found: which mode takes the cameras from `W+0x790` instead (`vfn+0x28` returning 5 or 6), possibly the 2D screen.
- **Field of view:** EDVR's runtime answers `GetProjectionMatrix` per frame, and the scope zoom (B3, flown) already narrows it frame by frame through the frame answer, at one to two frames' lag. A square 90 for the bake frames goes the same way. Traps: the hidden-area mesh, the light-grid cache and the optics announcement. The render size stays the headset's, not square, so the compute shader's aspect test needs changing.
- **Capture:** the game's tonemap draw, once per eye (`tonemap_admit.h`, used by `fix.ui_quality`). There is H, HDR, per eye, plus the depth probe's texture and rows EDVR built itself. This is the same stage as flat's. The 2D screen has the VR world route's resolve, which already carries what flat's bake takes.
- **Hold:** `requestSubmitHold` / a per-frame mark at the first eye's latch. The compositor replays the last good pair, world-locked, and no production caller uses it today. It is refused (a black frame) with no kept pair or after a reference or recentre change; on foot it is unflown.

**Plan:**
1. **A proving build, one flight.** VR runs the cull automatically where no bake is needed: a carrier, with its real sky, door rule and clear. The two rows go on VR's menu, and VR's panes stay the game's glass where there is no bake (no haze). Bake-type stations stay the game's own in VR until the VR bake works. Three temporary probe keys, at a station: (1) the hold alone for 10 frames: withheld, replayed, empty; (2) the camera and a square 90 for 5 frames, visible on purpose: stub calls, `cb1` row 275 against the written pose, the projection's span, by eye; (3) at the tone draw per eye on those frames: H's format and size, depth, the eye, and the panes' `cb1` rows.
2. **The VR bake**, built on what the probes show: capture per eye into the same cube, the hold around it, automatic like flat's.
3. **After it:** DLSS and exposure after the bake, the light-grid cache, the clear's 12 percent share, frame time, and the 2D screen.

## Flight 48 results, first part: the bake has no sun

c841a58b, log `edvr_gfx_20261108_050858`, the `CraterPort` with F2 at 05:10:40: `the room is a surface port's (its anchor is SA_PlanetPortHub_Compact)`. The bake took 110 ms, all five directions captured, near plane 50 m. `the sun stays on` (05:10:35). Sean, with two screenshots of the bay: "I notice the bake doesn't seem to include the sunlight and possibly terrain/skybox?". With the window clear, the buildings are sunlit, the ground grey, the sky black with stars. In the bake the buildings show only their windows' lights on near-black walls, there is no ground, and the sky is navy with no stars. That matches H54 at the carrier's bar (navy, no stars, no planet).

- `ruled out: withheld families in the bake's frames, because feed.withhold and feed.effects are false while the camera is moved (lookOn), so terrain, sun and sky families are issued then (code).`
- `ruled out: the display's fallback as the navy, because the fallback is kSteadyColour, a pale white, and the capture writes texels with no depth too (at kBackdropFarM) (code).`
- **H57** The bake's near plane, 50 m, clips what Elite draws close to the camera on purpose: the sky dome and stars, the sun's disc and glare, and perhaps the sun's lighting pass. That would explain the navy sky (nothing drawn there), the missing stars, and at the carrier the missing planet. Test, no build: `diag.concourse_bake_near = 5` in Sean's `edvr-flat.ini` (set 2026-11-08), relaunch, a new bake at the `CraterPort`. At 5 m the room's own walls are captured too, so most of the windows will show the room. Look only through the bay, where the glass lets the outside in: a black sky with stars and sunlit buildings there confirms H57 for what lies between 5 and 50 m. Unchanged leaves it open below 5 m.
- **H58** The sun's shadows follow the player's camera, not the moved one: the shadow cascades are built for where Sean stood, so what the bake looks at from the room's centre falls outside them and reads as shadowed. It does not explain the navy sky. Discriminator: in a bake, the buildings in the direction Sean was facing when it ran are lit and the others are not.
- **If H57 holds:** the fix is to bake at the game's own near plane and leave out the room's own records in the bake's frames by name (the store already tells the room from the outside), instead of cutting them with the near plane.
- **H57 confirmed (log `edvr_gfx_20261108_051732`, 05:19:41):** a bake at 5 m (`near plane 5.000 m` on all five captures, 140 ms) shows a black sky with stars, sunlit buildings and the grey ground and hills through the bay, where the 50 m bake had navy, no stars, unlit walls and no ground. What Elite draws for the sky, the stars and the sunlight lies between 5 and 50 m of the camera. The room's NPCs stood "outside" in that bake: at 5 m the room is captured too, which is why 50 was chosen. H58 is not needed for what this flight showed. The test line is out of Sean's `edvr-flat.ini` again.
- **Built after it:** `CraterPort` joined `bakePlanFor` on Sean's word, "We can add this station type to the bake list as well" (8127e90a, installed on Steam 2026-11-08).

## Flight 47, first part: the suspended bar

c841a58b, log `edvr_gfx_20261108_044500`, build matched. Sean suspended the carrier's bar service mid-session: "Interesting bug, I suspended the bar service." A 3.3 s clip from 04:51:50, standing still, flickers every few frames between a grey panelled wall filling the view and the corridor behind it to the bar's dark window. A restart cleared it: "I restarted the game and its not doing the bug anymore".

- **The door rule held still through it:** camera at a2 -5.6 from 04:51:41 to 04:52:21, both doors shut, no door events, no change to the culled stretch, and the producer and store tallies agreed.
- **His F3 and census** came at 04:50:40 and 04:50:43, a minute before the clip and elsewhere, so they do not name the wall. Candidates from that F3: `SA_Carrier_01_Bar_BlankingPlate` (x2, r=11.2, kept then); the reload's netLog also loads a `carrier01_corridorfalsewall`. The census drew 0 on-screen calls in its 2 frames.
- **H55** The flicker is Elite's own, from a service suspended mid-session. Discriminator: suspend or restore a service mid-session; at the flicker, F3 there (names the wall and whether it is culled), then F2 off. Flicker with F2 off means Elite's.
- **The crash:** quitting to the main menu and reloading on the carrier, 04:53:39.5 (`edvr_breadcrumbs.txt`): 0xC0000005 executing address 0x57, called from `EliteDangerous64.exe+0x390A61`, every frame of the stack in Elite (+0x25AD7C to +0x452403), right after the netLog's `Calculated hash of 0 for carrier01_bar`. No EDVR hook lies in that range; the nearest are UI glyph points at 0x30FCBA and 0x3124F0, not on the stack. The signature is new among the file's 11 unhandled crashes. INFERRED to be Elite's own, on loading a carrier whose bar was just suspended; not proven.

## Flight 47, the list as written

Flat, Steam, on the carrier, on c841a58b, no keys needed. Read with `python tools\edup_log.py --target steam --expect-build c841a58b --grep "concourse"`.

1. **Arrival.** The log: `concourse cull: a fleet carrier's windows show the real sky`, and no `concourse camera test` bake lines.
2. **The bar from afar,** from the central space: stars, the planet and the sun in its windows; no ships. Look down and to the sides through the glass too: is any of the carrier's own hull missing, with stars where it should be?
3. **Walk to the bar's glass** until it clears (`concourse clear:` lines): the ships and the hull should come back. Back away: they go. Say how the pop looks. Frame times at the glass before and after the clear are in the `walk survey` lines.
4. **The switch.** F8, Performance tab: Skip rooms behind doors under Window 3D distance. Off in the central space facing the bridge's door: `concourse doors: fix.skip_rooms_behind_doors = off`, then `nothing is behind a shut door now`. On again. With Optimized stations off, the row is dimmed and refuses a change.
5. **The bridge's floor** (Flight 46's open question): on the bridge, numpad `/` to records and rigs. Does the floor go?
6. Any door an NPC opens from more than 5 m away: does anything pop in?

## The real sky at a carrier, and the doors' own switch (2026-11-08)

Sean, after Flight 46, of the bar: "I can see the square edges of the bake we're doing in the bar. I think maybe the best thing to do here is not bake an image at all", then "Can we exclude the skybox/planets/sunlight from being culled here?", then "I want the ships to still be culled, it's only the background I want kept", then "Can we still use the window clears distance to restore those culled objects in this scenario?". And: "can we make portal culling a separate performance switch?"

- **What hid the sky:** the stars are never withheld (`concourse_cull_core.h`, "Not here, on purpose": the sky F8FA801F, the stars 68DDDEF0 and F7A6E916); they were missing at the bar only because its glass showed the bake. The planet, its atmosphere and the sun's glare are withheld families, withheld whenever the cull is on (`concourse_pane.cpp`, the withhold decision). The sun's disc (7E38A6AA / 7CECABDE) is not withheld, and the sunlight on the room is untouched at a carrier.
- **The change:** `RoomLayout::realSky`, set for `kRoomCarrier` only. In such a room no bake is taken (the cull does not wait for one, and neither the room in view nor a ride up arms one), the glass is the game's own (never treated), and the families marked `skySide` (atmosphere and its quads, planet surface, the body and planetary vertex families, terrain patch, sun, sun arcs) are drawn with the cull on. Everything else outside stays culled from afar: the carrier's `MS_` hull, ships, weapons fire, plumes. The clear still runs: the glass is measured though not treated, and when the bar's glass (n=30, the clearing pane by default) fills `fix.station_window_distance`'s share, the cull lifts and all of it comes back at once; it goes again when the share falls below 0.4 of that. A sealed hangar still withholds the `skySide` families.
- **Risks:** where the carrier's own hull is in view through the glass from afar, it is missing, with stars behind it; Flight 45's F2-off screenshot from the glass showed only black sky, stars and the planet, and looking down or sideways has not been checked. With no mirror over the glass, what the clear brings back appears through clear glass in one frame, and goes the same way.
- **The switch:** `fix.skip_rooms_behind_doors` (on, off; Skip rooms behind doors on the flat panel's Performance tab, under Window 3D distance, dimmed while Optimized stations is off; live). Off, the door rule stands down whatever `hotkey.concourse_room_test` says. It is meant to stay; the hotkey is still temporary.

## Flight 46 results

Flat, Steam, ed2627b9, log `edvr_gfx_20261108_040806` (04:08 to 04:19), build matched.

- **The first move:** five openings, two at the elevator bay's door (a2 -11) and three at the bridge's (a2 19). Each was caught with the leaves 0.023 to 0.027 m past their rest of 0.906 m, with the camera 3.4 to 4.6 m from the door, so in every one the door did not block and the room behind was already drawn. Four closings were logged, after 3.0 to 16.2 s open. No door was seen opening from afar (step 5), so the first move was never what brought a room back.
- **The cost:** the door watch averaged 1.2 to 3.2 us a tick over 115 five-second lines. Its maximum was 1070 us once (04:19:46), then 141 and 75 us; INFERRED to be the thread pre-empted inside the timed block or the anchor frame's once-a-second read, not told apart. A hook would save next to nothing (Ruled out).
- **Frame times** (`walk survey` means, a second each, placed by the latest a2, not a standing-still comparison):

  | Place | records | records and rigs | off |
  |---|---|---|---|
  | Elevator bay | 7.44, later 8.22 ms | 7.97 ms | not sampled |
  | Central space | 8.40, later 8.63 ms | 7.16 ms (48,585 frames) | 8.65 ms |
  | Bridge | not sampled | 9.26 ms | not sampled |

  INFERRED: in the central space records alone saves little against off, and the saving is in the rig vetoes, as the ceiling found. Not a controlled comparison.
- **The bridge's floor** with records and rigs: not reported. Sean was on records and rigs from 04:14:06 to the end, the bridge included.
- **The bar:** the bake's square edges show in its windows. That led to the real sky, above.

## Flight 46, the list as written

Flat, Steam, on the carrier, no keys needed, on ed2627b9 (the door rule with the first-move watch; 6982c973 was not flown). The door rule runs by itself; numpad `/` now steps it: records (the default), records and rigs, off, and round again, with a "Doors: ..." notice each time. Read with `python tools\edup_log.py --target steam --expect-build ed2627b9 --grep "concourse doors"`.

1. **The elevator bay.** Stand by the lift, more than 5 m from its shut door, facing it. The log: `concourse doors: the door at a2 -11.0 is shut` with its rest, then `the camera is at a2 ...; behind a shut door ... above a2 -11.0`. Ten seconds still at each of the three steps of `/` (records, records and rigs, off), then once more back to records.
2. **Through the door.** Walk to it: the central space should be there before its leaves part, with no empty room or pop as it opens. Through, and back. The log's `started opening, frame N` line says how far from the door you were when it began, which is what `kDoorNearM` (5 m) should be.
3. **The central space,** about 10 m from the bridge's shut door, facing it: the same three steps.
4. **The bridge,** more than 5 m inside its door, facing back toward it, with the place the floor went in view: the same three steps. With records the floor should stay; if it goes with records and rigs, the rig veto was the cause.
5. **A door opened from afar.** Watch a door from more than 5 m while an NPC goes through it, if one does: does anything behind it pop in, or a sliver of empty room show as it cracks? Note the time. The log's `started opening` line at that time should say `culled until this frame` and a crack of 0.02 to 0.05 m.

## Flight 45 results: the floor and the bar

Same build and log as Flight 44 (`edvr_gfx_20261107_193601`), later in the session.

- **The bridge's floor.** An F3 on the bridge at a2 22.5 with the slab alone (19:45:00) names what it culled. Nearest the camera: a terminal at 7.1 m, room signs, the services' doors at 13 m, sofas and countdown screens at 14 m, the ship model screens at 20 m, all of them in the central space; unnamed records culled near there are all r=0.9. No floor-sized record was culled, named or not, while the slab vetoed 171,231 rigs in those 5 s. A rig is vetoed when every record of it the store judged was culled, and draws all its parts. INFERRED: the floor is a part of a rig whose judged records all lie in the central space. The producer and the store agreed exactly in Flight 44, so their spheres are not the cause (Ruled out).
- **The bar window.** Three screenshots from one place at the bar's glass (cam_local a0 about -35). 19:46:24, the cull on and the bake showing: flat navy, no stars, no planet, a black band across the top. 19:46:55, cleared: black sky, stars, the planet. 19:47:17, F2 off: the same as cleared. So H52 and H53 are both ruled out, and the fault is the bake's.
- **H54** The bake holds no stars or planet in the direction the bar's windows face, the room's -a0 (direction 3, the second capture). Flight 43's first screenshot from the bar, looking up and out, showed both, from direction 4 or the top of 3. Unknown whether that capture is flat navy across its whole face or only near the bar's line of sight, and why. The next step is an instrument that writes what each capture holds (its colours and how much of it has depth), not built.

## The door rule (built 2026-11-07, 6982c973)

Sean, after Flight 45: "build the door rule". In `concourse_cull_core.h` (`roomSeenSlab`, `DoorWatch`) and the tick.

- **Doors:** a carrier's two, at a2 -11 and +19 (`kRoomCarrier.slabCut`). Each door's state comes from its two leaves, `Narrow_Door_Left` and `_Right`, sighted by name in the store while wanted and matched to the door within 3 m along a2. Open as soon as they part past 1.3 m; shut again only after 300 ms at or under 1.1 m. A door with a leaf not seen is not known, and never blocks.
- **The rule:** a door blocks when it is shut and the camera is more than 5 m from its plane (`kDoorNearM`, a first value: no flight has said how near a door opens). Past the first blocking door each way along a2, the room's own records are culled: an allowed name, not the anchor, its sphere wholly past the door. What is outside the room, the hull and what the bridge's windows show, is never the rule's; Sean, of the structure under the bridge floor: it shows through the bridge's windows.
- **Rigs:** by default a record the rule culls still counts as kept in its rig's tally, so the rule never vetoes a whole rig (the floor, above). `/` steps to records and rigs, the ceiling's behaviour, so one flight can show the floor going with it and what the rig vetoes are worth.
- **Where it runs:** wherever the cull runs at a carrier, and on the bridge, where the cull stands down, as the only rule. The ceiling test is gone; `hotkey.concourse_room_test` is the rule's step key now.

### The leaves' first move (2026-11-08, ed2627b9)

Sean asked whether Elite's own door state could be reverse engineered and hooked, for precision, for performance, and so a door an NPC opens brings its room back. The leaf watch already reads every opening, whoever makes it. Its weak part was the 1.3 m threshold: against a rest of 0.8 to 1.0 m, a door opened from more than 5 m away showed up to about 40 cm of crack with the room behind still culled. Sean: "log the frame the leaves first move".

- **The watch:** it learns each door's rest, the least gap it has seen at or under 1.1 m, and calls the door open on the first step its leaves stand more than 2 cm past that rest (`kDoorMoveM`). It is shut again once they are back within 2 cm of rest and have moved no more than 5 mm a step for 100 ms (`kDoorStillM`, `kDoorStillMs`), which replaces the 300 ms hold. A door first seen at rest is not known until it is still, so it does not block before then. The rest stays through a leaf lost and is forgotten when the rule stands down.
- **The log:** `started opening, frame N` gives the crack it was caught at, the camera's distance from the door, and whether the room behind was culled until that frame. `is shut, frame N` gives how long it was open, in frames and ms. The slab lines carry the frame. The 5 s line adds each door's rest, the openings in the window and the door block's own cost per tick (QPC). Changes past 400 lines are counted, not logged.
- **What decides the reverse engineering:** a hook on Elite's door state would know the decision to open a frame or more before the leaves move. If the flight's first-move lines show cracks of a few centimetres with no sliver seen, it buys nothing here. If Sean sees a sliver, or the cost line is more than a few microseconds a tick, it is worth the RE, and the log names what it must find. Unmeasured: whether the leaves' positions read in a tick are that frame's, which sets whether one frame shows the crack with the room still culled.

## Flight 44 results: the rooms ceiling

Build f081d22b, log `edvr_gfx_20261107_193601`, flat, Steam, on the carrier. The bar window's check (step 2) was not flown.

- **Frame times,** the walk survey's mean each second, standing still. With the test on every spot read the 144 fps cap (6.94 ms), so each saving is at least what is shown:

  | Spot | Test off | Test on | At least |
  |---|---|---|---|
  | Elevator bay, a2 -17.3, facing its door | 8.6 to 8.8 ms | 7.0 to 7.2 ms | 1.6 ms |
  | Central space, a2 12.5, 7 m from the bridge's door | 7.7 to 8.4 ms | 6.9 to 7.1 ms | 0.7 ms |
  | Bridge, a2 23.6 to 26.9 (the cull standing down) | 7.3 to 8.0 ms | 6.9 to 7.0 ms | 0.3 ms |

  In the elevator bay, at the same spot, 11.1 to 12.2 ms for the ten seconds before the 8.6 (19:37:51 to 19:38:01), the test off; which way the camera faced is not logged.
- **Records the slab culled** that the cull's own rule kept: 20,000 to 51,000 every 5 s in the elevator bay, 64,000 to 98,000 in the central space, 68,000 to 164,000 on the bridge (slab alone). Producer and store counts were equal throughout.
- **The bridge's floor went with the test on,** about the COMMAND lift, with the carrier's structure below showing (Sean's screenshot at 19:40:55, a2 21.5). The slab culls only a record whose sphere ends before the door's plane (a2 + r < 19), so the floor is drawn by something whose sphere does not hold what it draws, or rides in a rig whose records were all culled. Flight 43's lists name nothing like a floor there; the records ending nearest the door are the ship model screens (`FUI_*`, ending 18.5 to 18.9), the services' doors (15.5 to 16.1), and `SA_Carrier_01_Bar` (r=22.5, ending 17.9). Unanswered; an F3 on the bridge with the test on lists what the slab culled there, by distance. A door rule needs this answered: it decides by the same spheres.
- Sean, of the structure under the missing floor: culling more there is not wanted, as it shows through the bridge's windows.

## Flight 44, the list as written

Flat, Steam, on the carrier, no keys needed for the cull. New: `hotkey.concourse_room_test` on numpad `/` (DIVIDE) in both Steam inis, which Elite and EDVR leave unbound.

1. **The rooms ceiling.** At the three spots from Flight 43, each facing its shut door: the elevator bay by the lift, facing its door; the central space about 15 m from the bridge's door, facing it; the bridge, facing back toward its door. At each: stand still 10 s, press `/` (a "Rooms test: on" notice), stand still 10 s, press `/` again ("off"). The log: `concourse rooms: the test is on`, `the camera is at a2 ..., in the slab from ... to ...`, and `concourse rooms 5s` with what it culled. With the test on an open door shows nothing behind it, and a walk through one shows the next room missing until you cross: it ignores the doors on purpose.
2. **The bar window** (see "The bar window, 2026-11-07"): at the glass where the second screenshot was taken, once the window has cleared, a screenshot; then F2 (the cull off, nothing of EDVR's on the windows), a screenshot from the same place and direction; then F3 (lists what is outside, by position); then F2 again.

## The bar window, 2026-11-07

Sean, with two screenshots from Flight 43's log: "can we fix the bar window. For some reason the star field/terrain doesn't really show up on these 2d captures either", and "it doesn't look as black as it should?"

- The first, at 18:50:52, three metres in from the bar's glass: the cull on and the window showing the bake. Black sky, stars, the planet. A pale square at the bottom.
- The second, at 18:51:06, at the glass (cam_local a0 -38.1): the window had cleared at 18:50:57, as it filled the whole view (Sean had set the window distance so a window clears only at 100%). The log for those seconds: `treated 0 draws`, `phase clear`, and only the scatter and the light cones withheld (about 140 draws a second). So it is not a bake: it is Elite's own picture through the bar's glass, with those two left out. Most of it a flat navy area with no stars; the planet only in a strip at the right.
- **H52** The navy area is something Elite draws just outside the bar's glass, which the bake cannot have: its near plane is 50 m from the room's centre, and the bar's glass is 39 m out, so the first 11 m outside it are left out. The candidates are `SA_Carrier_01_Bar_WindowShutters0` (r=8.6, its centre on the glass) and the command tower's hull. The pale square in the first screenshot may be a piece of the same past the 50 m. Confirmed if the navy stays with F2 off (Flight 44, step 2), and F3 then names what is there.
- **H53** Something EDVR does in the cleared phase makes it navy. Confirmed if the F2-off screenshot shows black and stars where the cleared one was navy. Neither the scatter nor the light cones should darken a black sky to navy, so this is the less likely.
- If H52: the bake and the real view differ in the bar, and the window jumps between them as it clears. The fix would be the bake keeping what is outside the bar's glass, not a nearer plane everywhere (that brings the room back into the bake).

## Flight 43 results, and the rooms survey

Build f88df803, log `edvr_gfx_20261107_184209`, flat, Steam, on the carrier, loaded on foot. Sean: a sealed door in the elevator bay, then a central space with open entrances to the carrier's services where they are installed, then a further door to the bridge and command area. He surveyed the two rooms either side of the doors.

- **No keys:** `a station of type FleetCarrier (Status.json: ...)` at 18:43:40, the anchor 13 ms later, and the bake 2 s after that with no ride (78 ms, five directions). The walk onto the bridge at 18:45:38 stood the cull down with no tick of it on. The bar was not reported. The survey's own header still says `Type unknown`: it prints the journal's type, not the one from Status.
- **The layout,** in the anchor's frame (a2 along the carrier, the bridge at +a2; a1 up), from the four F3 lists. Positions from the nearest of each name.
  - The elevator bay: the lift at a2 = -26. Its door, the sealed one, at a2 = -11.
  - The central space: the anchor, `SA_Carrier_01` (r=33.1), at 0. The services to either side: `SA_Carrier_01_Bar` (r=22.5), `_Store` (15.8), `_Shipyard` (22.9), `_VistaGenomics` (16.3), and `_BlankingPlate` records (about 11) where a service is not installed. Their entrances, `SA_Carrier_DoorA0` to `E0` with `SA_Carrier_DoorFrame`, did not move between the lists.
  - The bridge door at a2 = +19. Beyond it `SA_Carrier_01_Bridge` (r=25.0, centre +46, so its sphere starts at +21), a second lift at +32, escape pod doors at +21, and a `_Tech` doorway at +36 to +38.
  - **Both doors are the same parts:** `Narrow_Door_Left` and `Narrow_Door_Right`, with `PS_Doorway_Wide_Door_Left0` and `Right0` and `PS_Doorway_Wide_01`. The two narrow leaves slide sideways, from 0.8 to 1.0 m apart shut to 4.4 m open (the wide leaves 3.1 to 5.1 m), at both doors. The names are shared, so a door is told by where it is, not what it is called.
  - **No record names the elevator bay,** and the anchor's sphere reaches past both doors (to a2 = -33 and +33). Spheres do not bound these rooms: two planes across the carrier at the doors do, giving three slabs along a2.
- **Frame times, standing** (the walk survey's mean over each second; the cap is 6.94 ms):
  - The elevator bay at a2 = -25.4, 14 m from its shut door: 9.6 to 9.9 ms with the cull on, 10.9 to 11.6 with it off, F2 pressed back and forth. For the 15 s before that, at the same spot with the cull on, 11.1 to 13.5 ms; which way the camera faced then is not logged.
  - The central space at a2 = +4.5, 15 m from the bridge's shut door: 7.5 to 7.9 ms on, 9.4 to 10.1 off.
  - The bridge, the cull standing down: 8.3 to 9.2 ms.
- **What it suggests, not measured:** the elevator bay is the prize. Facing its shut door, everything behind it (the central space, the services, the bridge) is drawn, and the cull keeps it, as those are the carrier's own names; the bay alone should be near the cap, so up to about 3 ms. The central space is already within 1 ms of the cap with the cull, so the bridge behind its door is worth little there. On the bridge, the central space behind its door is the open question. The ceiling key answers all three.

## Flight 43, the list as written

Flat, Steam, no keys pressed: the carrier now runs as the other flown types do.

1. Load in on foot aboard the carrier. The log should say `concourse cull: a station of type FleetCarrier (Status.json: docked and on foot, neither in a station nor on a planet): the cull and its bake run`, and the bake should follow once the room has been in view for 2 s, or at the top of a ride up.
2. Walk onto the bridge and back: the bridge's windows the game's own, with no frame of the bake on them as you walk in or out (Flight 42 had one).
3. The bar's windows and the frame times in the concourse should be as they were with F2.
4. Only if you choose to spend the tritium: a jump with you aboard on foot. During the jump itself the windows will show the bake of the system you left; after it, `concourse bake: the carrier jumped (CarrierJump)`, the windows the game's own for 10 s, then a new bake. Whether 10 s is long enough for the arrival is the open question.
5. Last, the rooms survey for "Rooms behind bulkhead doors", after the steps above (it uses F2). In each room you can walk into (the concourse, the bar, the bridge, and any corridor or lobby between them), on its side of a bulkhead door:
   1. Stand still for 10 s facing the shut door, as you would normally stand: the frame time with the cull as it is.
   2. Press F2 so the cull is off (not needed on the bridge, where it already stands down), then F3: `concourse names: F3 with the cull off` and a list of every record.
   3. Walk up until the door opens and press F3 again in the doorway while it is open.
   4. Press F2 again, so the cull is back on, before the next room.

   Say in the report which rooms you did, in order. Each list names the carrier's anchor too, so positions from different rooms can be put in one frame.

## Rooms behind bulkhead doors (Sean, 2026-11-07)

Sean: on a fleet carrier, could the interior behind its bulkhead doors be culled? The idea is a room table with the doors as portals: a room is culled only while every door between it and the camera's room is shut and the camera is more than a few metres from each of those doors, so the room is back before a door can open, as Elite's doors open on approach. A door's state comes from its records' positions, as the lift's do (1.5 m shut, 2.9 m open). What the windows look out on stays the bake's job, and nothing seen through glass between rooms is culled. Every carrier shares one interior, so one survey serves them all. This is not the occlusion arc's computed occluders, which removed almost nothing: the doors and rooms are a fixed table, and nothing is worked out per frame.

Order: (1) the survey, Flight 43 step 5: which records mark each room and how far it reaches, which are the doors and how they move, frame times facing each shut door; (2) the ceiling: a temporary key that culls every record outside the camera's room, doors ignored, read by frame time in each room; below about 1 ms where people stand, it stops there; (3) only then the door rule. The risk to look for is a pop as a room comes back, the failure the lift's way up may have (H36, H40). On the bridge, where the outside cull stands down, this could win back part of the 1.5 to 2 ms the stand-down costs, if the bridge's cost is the interior and not the carrier's exterior: the ceiling says.

## Flight 42 results

Build 975cb8e0, log `edvr_gfx_20261107_171641`, flat, Steam, on the fleet carrier Q1H-2DZ with F2. Sean: "Carrier looks good, bake it".

- **The bridge:** `the camera is in SA_Carrier_01_Bridge` on each entry, and the windows were the game's own. Frames there 8.8 to 10.3 ms with the cull standing down, against 6.9 to 7.7 with it on in Flight 41. The concourse read 8.1 to 8.8 ms with the cull.
- **One tick with the cull on in the bridge,** at 17:20:26.621, 6 ms before the stand-down was logged again 6 m inside: the bridge's record was unread for that tick (a read at 6 m is inside, and the camera was read, as the anchor's reach check had passed). The stand-down now holds through up to 500 ms of an unread record (`LiveRoomLatch`).
- **The type was never known.** The game was loaded on foot aboard: the journal's `Location` said `"Docked":false, "OnFoot":true, "Body":"38 Lyncis 5", "BodyType":"Planet"`, the same shape as a load at a surface port, so nothing said FleetCarrier, and without F2 the cull would not have run. Flight 41's journal also showed the carrier's `Disembark` saying `"OnPlanet":true` near a planet, which dropped the type. Status.json tells a carrier apart: every on-foot visit to two carriers read Flags 0x00000005 with Flags2 0x00014001 (0x00016001 in the hangar), with neither bit 3 (in a station, as at the Orbis, Coriolis and Outpost) nor bit 4 (on a planet, as at the CraterOutposts).
- **Built on that:** `journal_station::atCarrier` names a carrier from Status when the journal gives no type, the latch holds the carrier's type on `"OnStation":false` alone, `bakePlanFor` runs FleetCarrier, and a `CarrierJump` drops the bake, with no new one for `kBakeAfterJumpMs` (10 s). The jump part is not flown.

## Flight 42, the list as written

Flat, Steam, on the fleet carrier, with F2.

1. Walk from the concourse to the bridge: the bridge windows should show the real deck, and the log `concourse cull: the camera is in SA_Carrier_01_Bridge`.
2. Is there a pop at the bridge's door as the cull stands down, and again walking back?
3. The bar windows should be as they were.

## Flight 41 results

Build 7738c48c, log `edvr_gfx_20261107_160109`, flat, Steam, on the fleet carrier with F2. Sean: "Carrier looks mostly good. Looks like there's some strange artifacting in the window bake for the command deck", with a screenshot: in the bridge's windows a flat grey-blue surface with a lighter band, and below it the deck's lights.

- **The bake** took 78 ms at `SA_Carrier_01`, all five directions. The bar's windows cleared as they were approached; so did the bridge's (`the clearing pane is now n=54 (share 31.8%)`), and an n=12 pane there.
- **Frames with the cull on:** present p50 7.1 to 8.4 ms in the concourse, against 10.1 to 12.0 there in the survey without it; 6.9 to 7.7 ms on the bridge, against 8.7 to 9.4.
- **Why the bridge's windows are wrong.** In the room's own axes the bridge lies at +a2, 42 to 51 m from the anchor, and its windows face +a2 too. The bake looks from the anchor's centre, so its view toward the bridge's windows passes the bridge and meets what stands around it. Kept in the bake and nearest the centre: `MS_Command_Tower4`, r=156, its centre 87 m off, so its sphere holds the centre. The grey surface is most likely the tower from inside; INFERRED, as no capture of that direction was saved.
- **The fix:** `RoomLayout::liveRoomName`. On a carrier it is the bridge: while the camera is inside `SA_Carrier_01_Bridge`'s sphere (r=24.8) the cull stands down there, as if out of the room, and keeps its bake for the way back. It costs the bridge's saving, about 1.5 to 2 ms. A bake of its own, taken from the bridge, would keep it, but it would hold the picture for about 80 ms as you walk in, as there is no lift to hide it.

## Flight 41, the list as written

Flat, Steam, on a fleet carrier, with F2.

1. In the concourse, press F2: `concourse cull: the room is a fleet carrier's (its anchor is SA_Carrier_01)`, then the bake and `concourse cull: on`.
2. The bar windows: the right scene, and do they clear only when you are near them?
3. The bridge windows. The bake is taken from the concourse's centre, 54 m from the bridge, and what is outside the bridge is near (the carrier's core 116 m off), so things there may slide as you move. F9 cycles sphere and depth: say which holds better there.
4. Anything missing in the concourse, the bar, the stores or the bridge? Small things whose names are not on the list go past 40 m: an armed figure's rifle at 50 m was one.
5. F2 on and off in the concourse for the frame time: 10.1 to 12 ms there without the cull.
6. Ride down to the hangar: it should stay whole.

Not handled yet: a carrier jump. The bake would keep the old system in the windows until the next bake.

## A fleet carrier's survey

Build 84f55c1e, log `edvr_gfx_20261107_145834`, flat, Steam, five F3 presses with the cull off from 15:42:58 to 15:44:58: the hangar, the concourse, the bar window, between the concourse and the bridge, and the bridge window. Sean: "there's two window areas, in the bar and the command deck bridge windows".

- **The anchor:** `SA_Carrier_01`, r=32.5, 4.0 m from the concourse press. Also kept, by `SA_`: the bar (r=22.8, 25 m from the anchor), the store, the shipyard, Vista Genomics, the bridge (`SA_Carrier_01_Bridge`, r=24.8, 45 m off), blanking plates, and the bar's window shutters 1.5 m from the bar press. Every centre agrees across the four presses within 0.1 m.
- **The keep list parts them, with three additions.** Kept 688 records at each press, culled 3,676 to 4,878. Culled within 70 m, and so popping at 40 m: `RandomDirt_*` 8 m from the anchor, `Narrow_Door_*` 35 to 42 m off, `Arm_*` 10 m from the bridge press; none of these was seen at the Orbis, the port or the outpost, so they are on the list now. Left off: a rifle's parts at 50 m from the bar (`Wpn_M_AssaultRifle_Kinetic_FAuto`, `Sight_Mount`, `PipVent_Left`), names too general to keep.
- **The windows:** the bar's glass `98397963/66B08F89` n=30, 99.7% of the screen at it, 39 m from the anchor; the bridge's n=54, 99.9%, 54 m from it. Outside, the carrier's hull, `MS_` records: the core hub 116 m past the bridge window, the core cylinder 149 m, the command tower around the rooms, the engines 255 m and more.
- **The lift** to the hangar is a camera cut of 1,331 m. A cut of 1,311 m at 15:44:17, between the bar press and the next, came back in the same tick: the bridge is in the same frame of coordinates.
- **Frames, cull off:** present p50 10.1 to 12.0 ms in the concourse (15:43:22 to 15:43:32), 7.2 to 7.5 ms at the bar window, 8.7 to 9.4 ms on the way to and at the bridge.
- **The journal:** `Disembark` on a carrier has `OnStation:false, OnPlanet:false`, no station. The type now holds through it until `Undocked`. A load on foot aboard names a star or planet, and still reads unknown.
- **Built on this (behind F2):** `kCarrierAnchorName` and `kRoomCarrier` (n=30, 300 m sphere, 100 m reach), one bake from the concourse's centre for both window areas.

## Flight 40, the list as written

Flat, Steam, no keys: at an orbital `Outpost` and a `CraterOutpost`, dock, disembark and ride up; the bake should be ready when the doors open, with `concourse cull: a station of type Outpost: the cull and its bake run`. Then a load on foot in each room.

## Flight 39 results

Build 84f55c1e, log `edvr_gfx_20261107_145834`, flat, Steam, at an `Outpost` with F2. Sean: "Outpost looks good, bake it".

- **The bake** took 79 ms and 78 ms, all five directions, at the anchor `SA_OutpostHub_Compact` (`the room is an orbital outpost's`). The bay cleared as it was approached and the cull came back after.
- **Frames** in the room with the cull on, present p50 6.9 to 7.3 ms, near the 6.94 ms cap, against 9.5 to 10.1 ms at the bar without it in the survey. Not one spot compared.
- **The hangar** stayed out of it: `its anchor is in the store but this is not the room: the camera is 436 m from it`, and again at 444 m.
- **Built on this:** `bakePlanFor` runs an `Outpost`. The `CraterOutpost` with no keys (Flight 38) was not flown on its own: this flight did not go to one.

## Flight 39, the list as written

Flat, Steam, at an orbital `Outpost`, with F2: the type table leaves an outpost to the game until this has been seen.

1. In the room, press F2. The log should say `concourse cull: the room is an orbital outpost's (its anchor is SA_OutpostHub_Compact)`, then the bake and `concourse cull: on`.
2. Does the room stay whole? The outpost's hull is one record, `Outpost`, r=570, and it is culled: if a wall or the ceiling around the room goes with it, that is the hull, and it needs keeping.
3. The bay from the bar, and walking to it: the right scene, and does it clear only when you are near it?
4. F2 on and off at the bar, for the frame time: 9.5 to 10.1 ms there without the cull.
5. The hangar, riding down with the cull on: it should stay whole.

## An orbital outpost's survey

Build 6813f060, log `edvr_gfx_20261107_140715`, flat, Steam, two F3 presses with the cull off at 14:25:08 and 14:25:17, 23.5 m apart: at the bar, then at the bay. The F2 override was still on from the ports, so a second `CraterOutpost` on the way baked by itself (110 ms, all five directions, 14:18:58) and cleared at its bay: the same room there.

- **The anchor:** `SA_OutpostHub_Compact`, r=33.3, 9.6 m from the bar press and 15.6 m from the bay press. No `Standard_SocialArea`. Also kept: `SA_Outpost_01_Bar` r=24.2, `SA_Outpost_01_Store` r=25.1, `SA_Outpost_Exterior` r=16.9, centred 2 m from the bay press.
- **The keep list parts them again:** 43 names and 350 records kept, 272 names and 894 records culled, 72%, `0 culled records within 60 m` both times. The nearest culled is the hull, `Outpost`, r=570.5, its centre 165 m away, so its surface may be nearer. Then blast shields at 185 m, pads at 234 m, hangar skirts at 249 m.
- **The bay:** glass `98397963/66B08F89` n=30, 79.7% of the screen at it; about 16 m from the hub, direction 0.29, -0.18, 0.94 in world axes. n=30 is the Orbis shop window's count, which enters the clear at 0.3 of the share; that would clear an outpost's bay from across the room, so it enters at the share where the bay is n=30. A second glass, PS `98337732` n=444, 16.1% of the screen, is not of the treated family.
- **Frames, cull off:** present p50 9.5 to 10.1 ms at the bar, 7.4 to 8.0 at the bay, near the 6.94 ms cap. A smaller room and fewer records than a port: the saving is likely smaller too.
- **Built on this (behind F2):** `kOutpostAnchorName` and `kRoomOutpost` (n=30, 300 m sphere, 100 m reach).

## Flight 38, the list as written

Flat, Steam, no keys.

1. Dock at a `CraterOutpost`, disembark and ride up. The bake should be ready when the doors open, with `concourse cull: a station of type CraterOutpost: the cull and its bake run` in the log and no F2.
2. Quit to the menu and load back in on foot in the room: the same, about 2 s after the load.
3. Dock at a port of another type: it should stay the game's own, and F3 with the cull off still names its records.

## Flight 37 results

Build 6813f060, Steam, at the same `CraterOutpost`, with F2. Sean: "Flight is clean, I'm off to another port to survey it. Bake this station type". So the flicker and the hangar are fixed by eye, and `ruled out` is not needed for the explanation in "Flight 36 results".

- **Built on this:** `bakePlanFor` runs a `CraterOutpost`. The room's anchor still has to be seen, so a type with another room gets no cull.
- **The type at a port.** A surface `Disembark` read unknown, so the row alone would have run only between `Docked` and the disembark. Across Sean's journals `Disembark` and `Embark` on a planet name no station (579 and 574 lines) and neither does a `Location` on foot there (399, `BodyType` `Planet`). The type now holds through those until `Undocked`, and a load on foot on a planet takes the saved type if it is a port on a planet. So at a settlement after the last port it reads that port's type, which does nothing without the anchor. One wrong case left: a port with this same room but another type, reached without docking, then loaded on foot.

## Flight 37, the list as written

On build 6813f060, flat, Steam, at the same surface port, with F2. Then: `python tools\edup_log.py --target steam --expect-build 6813f060 --grep "concourse"`.

1. Stand where the clip was, looking through the logo glass at the bay, and move about: is the flicker gone? That patch should now hold still, with the logo glass's own reflection on top of the bake.
2. Does the logo glass look like itself, and does the bay keep its look where nothing stands in front of it?
3. At an Orbis, if you pass one: the bay and the skylights should look as they did. They go through the same change.
4. Ride down to the hangar and walk around for a minute with the cull still on: the hangar should stay whole. `its anchor is in the store but this is not the room: the camera is 503 m from it` should be in the log, once, and no `concourse cull: on` until you ride back up.
5. Ride back up: the bake and the cull should come back when the doors open.

If the patch still flickers, the explanation in "Flight 36 results" is ruled out.

## Flight 36 results

Build c8c23caf, log `edvr_gfx_20261107_134444`, flat, Steam, at the `CraterOutpost`, with F2. Sean: "Looks great, the only bug I see is this glass through glass flickering, doesn't happen with cull turned off".

- **The first cull and bake at a port work.** The bake took 109 ms, all five directions, the hold 156 ms. Present p50 7.1 to 8.5 ms with the cull on, most windows 7.4 to 8.1 ms, against 10.2 to 14.3 ms in the room without it in Flight 35. Not one spot compared, and the frame cap is 6.94 ms.
- **The flicker.** In the clip the camera looks through a glass wall standing inside the room, the one with the logo, at the bay behind it. Over the clip's still frames (160 to 190) the change is one soft patch inside that overlap, grainy at pixel level, hard-edged where the wall's pane ends. The census has three n=12 glass draws and nine others in one frame.
- **Why (from the code, and the patch's shape; not measured):** each pane's bake pass writes its picture's depth, and two panes on one pixel compute that depth along the same ray, equal but for rounding. So the later pane's greater-or-equal test is a coin toss per pixel and per frame: it either repaints the bake, wiping its own glass, or leaves its glass on the bake. The soft patch would be that glass's reflection, on and off. Nobody has reported it at an Orbis, which goes through the same code.
- **The fix:** a pixel takes the bake once, by a second stencil bit (`DsVariant::kMarkedOnce`, `kBakedBit` 0x02). A later pane keeps the game's own glass on top of it, as with the cull off, whatever order the game sorts them in. Bit 0x80 was the first choice and is not free: draw `AFBCD3AD/2BAE3742` reads it after the glass.
- **The hangar.** Sean: "went to the hanger and this happened", and a screenshot: the hangar gone, his ship and the stars around it. The log has `concourse cull: on` at 13:51:15, 13:51:59 and 13:52:54, and `concourse frame: cam_local -460.72 -137.72 147.74`, 503 m from the hub. A station's room leaves the store when the lift cuts away, so the anchor's sighting was enough; this hub is seen from the hangar too. The fix is `RoomLayout::roomReachM`, 100 m at a port: between the bay at 23 m and the hub's 42 m radius on one side and the hangar on the other.
- **The room's axes.** From the `concourse frame` lines, r 42.0, axes a0 (-0.780 -0.503 -0.371), a1 (0.040 -0.633 0.773), a2 (-0.624 0.589 0.514). The bay's direction from the hub is 0.11, -0.16, 0.98 in those: it faces direction 0, +a2, and direction 1 looks the other way. Trimming that one direction would save about a fifth of the bake's frames, once; not done, since it has shown no need.
- **Still open at a port:** F9 and the 300 m sphere, whether the sun's fall changed, frozen ships. None was reported as wrong.

## Flight 36, the list as written

On build c8c23caf, flat, Steam, at a surface port. F2 turns the cull and bake on there: the type table still leaves a surface port to the game until this has been seen. Then: `python tools\edup_log.py --target steam --expect-build c8c23caf --grep "concourse"`.

1. Walk into the room and press F2. About 2 s later the bake should run and the cull come on. In the log: `concourse cull: the room is a surface port's`, `concourse camera test: armed: no ride`, `concourse bake:`, `concourse cull: on`.
2. Frame time: stand still, facing away from the bay, F2 on and off a few times. Then the same facing the bay.
3. The bay from the middle of the room. If it clears straight away you are looking at the real thing, so lower Window 3D distance in the menu until the bake shows; it moves live. At 80 the trigger is 31% of the view, at 50 it is 80%, about at the glass (85.5% in Flight 35), and at 40 it never clears. Is it the right scene, and how much does it slide as you walk?
4. Anything missing from the room, and does the sun still fall where it did? The sun's shadows lose the outside's casters when the cull is on.
5. Walk to the bay until it clears, and back: any jump?
6. F9 cycles off, sphere and depth, for a look at each. The sphere's radius is 300 m here; `diag.concourse_backdrop_radius` in `edvr-flat.ini` changes it live.
7. An F3 with the cull on lists what is kept and culled from where you stand.

## Flight 35 results

Build 073e8dac, log `edvr_gfx_20261107_125458`, flat, Steam, already at the `CraterOutpost`. Two F3 presses, from the middle of the room (13:02:34) and at the bay (13:03:00).

- **The sun.** The log has `a port on a planet's surface (type unknown, Status flags 0x00200005 0x00014011): the sun stays on`, and no `sun light` line. The sun pair `7E38A6AA/7CECABDE` now ranks in F3 at 100% and 71% of the screen; it was absent from Flight 34's. This was a load on foot, so the descent itself, and an orbital hangar, are not flown. Sean has not yet said how it looks.
- **The room's anchor.** No `Standard_SocialArea` here, so F2 alone would have logged only `waiting (anchor=0)`. `SA_PlanetPortHub_Compact`, r=42.1, one record a frame, centred 3.5 m from where Sean stood in the middle. `SA_PlanetPort_Exterior`, r=102.2, 64.9 m from it, is the shell, and its centre lies behind the room as seen from the bay. Names at press A and B agree on all of this.
- **The station's keep list parts them cleanly.** From the middle: 64 names and 626 records kept, 525 names and 3,694 records culled, 85% of the records. From the bay: 62 and 594 kept, 552 and 4,214 culled, 88%. `0 culled records within 60 m` both times. The nearest culled name is `PB_LampPost` at 136 m and 126 m, then `PB_Gen_BuildingSectionF` and `C`, then the first ship at about 200 m.
- **What is outside.** `PB_` buildings, `PP_` docks, `Hanger`, `LandingPad`, ships, bunkers and turrets. Past the bay, by the surface of each sphere from where Sean stood: `SectionB` 84 m, `SectionC` 98 m, `SectionF` 104 m, `SectionG` 217 m, `SectionA` 355 m, the science station 444 m, then the docks and pads past 1,000 m. No other named record stands within 50 m of the hub, so the bake's near limit needs no change.
- **The bay.** It is glass `98397963/66B08F89` with n=12, at 85.5% of the screen standing at it and 2.5% from the middle facing away. It lies about 23 m from the hub. World direction from the hub: -0.70, 0.63, 0.34; in the room's own axes it waits on the `concourse frame` lines of the next flight.
- **The terrain is already on the withheld lists.** In Flight 34's census, frame 0, main view, `ACE405F4` (`terrain patch`) is 48% of the indices and `72BDD292` (`body surface`) 48%. So the bake can replace the ground too. Whether anything else draws ground here is for the next flight's F3.
- **Frames.** Present p50 10.2 to 14.3 ms in the room with the sun on, against 10 to 11 ms by frame count with it off in Flight 34: different spots, so not a comparison.
- **Built on this (behind F2):** `kPortAnchorName` is an anchor, and `RoomLayout` carries n=12 and a 300 m sphere for it. 300 m is a first value. The clear share and the five directions are as they were. Down stays uncaptured: the view through the bay from inside the room stays within the side faces, and at the glass the window clears.

## Flight 35, the list as written

On build 073e8dac, flat, Steam. Answers two things: the sun at a surface port, and what that port's records are called. Then: `python tools\edup_log.py --target steam --expect-build 073e8dac --grep "concourse names"`.

1. Dock at a surface port, stay aboard until the pad has gone down, then walk to the concourse: is the sun on through the bay window, and does the room look as it does with `fix.optimized_stations` off?
2. In the log, `optimized station: a port on a planet's surface (type CraterOutpost, ...): the sun stays on`, and no `sun light inside the hangar off` while you are there.
3. At that port, F3 twice with no other keys: once in the middle of the room, once standing at the bay window looking out. A note of which was which is enough. Each writes `concourse names: F3 with the cull off`, then `concourse names: all <name> ...`, nearest first, with the record's position from the camera.
4. Then an orbital station: the hangar should still lose the leak, with `sun light inside the hangar off (pad moving in or locked in (r0); ...)`. That is the old behaviour kept.

What item 3 decides, each with the line that says it:

- **Is this room the Orbis room?** `all Standard_SocialArea` appears, or it does not. Absent, the largest near name with a room-like position is the candidate anchor.
- **The keep list:** `name=` counts are records the Orbis list would keep, `culled=` those it would lose. A settlement building counted under `name=`, or a piece of the room under `culled=`, means this layout needs its own list.
- **The bake's near limit:** the nearest names through the window, against the 50 m the bake treats as the room.
- **The bake's direction:** the camera's move between the two presses, and the positions of the same names in both.

If a press logs `0 kept names, 0 culled names` the store did not run, and that is the thing to fix. No `concourse names: F3` line means the press was refused: `concourse pane capture: refused` says why.

## Flight 34 results

Build 8e317e3f, log `edvr_gfx_20261107_121643`, flat, Steam, Orbis first, then a surface port.

- **The pale look.** Sean: "That worked". The log has `light cones` withheld 92,819 times in one 30 s line, about 3,100 a second, and 45,269 in a later one. Lamps, the bar's clear and a wedge in the windows were not reported, so items 2 to 4 of the list are open.
- **The bake** took 125 ms at the Orbis. Turning the setting off and on baked again, as designed.
- **A surface port is its own layout.** A `CraterOutpost`. Sean: "It only has this one big bay window". The table left it to the game (`a station of type CraterOutpost: its layout has not been flown, so it stays the game's own`), so nothing here was culled.
- **Its F3, cull off, by screen share:** `94D5C556/912477AE` n=6 blend 5,2 262%; `0357BBB2/81812EF9` 189%; the scatter 100%; `53211E8C/B403F48C` 100%; `B553BB47/68ABCB9F` 100%; `0357BBB2/70E6FCA6` 84%; `C4B4B334/0146ABCC` n=3600 83%; `24DE25E4/3D8442D2` 50%; `A7339D1F/37649F54` 30%. Ranked, none identified. The numlock census is at line 17086.
- **The sun went off there, and stayed off on foot.** Sean: "I think we need to explicitly not turn the sun off on these surface ports". The times: 12:24:48 the journal names the type; 12:24:56 `sun light inside the hangar off (pad moving in or locked in (r0); r0=3 r1=0; Status docked=yes)`; 12:25:16 sealed in a small hangar, box 152 x 75 x 258 m; 12:25:17 the surface `Disembark` names no station, so the type reads unknown; 12:25:18 the seal released on an origin shift; then r0=1 r1=1. No `sun light` line follows up to 12:34.
- **Why.** `sunStep` follows the pad, not the seal or the type. Its comment said an outpost surface never turns it off because r0 stays 0 there. That is true of a pad open to the sky. This port's pad descends into a hangar as a station's does.
- **The fix, `journal_station::atSurfacePort`:** `CraterPort`, `CraterOutpost`, `SurfaceStation` and `OnFootSettlement` are surface ports; `Coriolis`, `Orbis`, `Ocellus`, `Outpost`, `AsteroidBase`, `FleetCarrier` and `MegaShip` are not, whatever Status says; with no type, or a type not listed, `Flags` bit 21 or `Flags2` bit 4 says so. Docked there aboard reads `0x0121000D`, on foot `0x00200005` and `0x00014011`; at an Orbis on foot `0x00000005` and `0x00014009`. `sunStep` then never turns the sun off, and turns it back on if it already was.
- **Can a cull and bake run there? Not yet.** The cull goes by record name, and this log has none from that port: the names survey needs the cull active, and that needs the `Standard_SocialArea` anchor, so a table refusal or another room name leaves it silent. The glass is the same shader as the Orbis: `98397963/66B08F89` n=12 draws=3 at 23.7% of the screen, probably the bay, not proved. That share is past the shipped 20% clear, so at the default slider the cull would be off where Sean stood; at his 80 the trigger is 31%. The census had 4,029 main-view draws and about 14 million indices, about 19,000 draws in what look like shadow views, and 442 to 495 frames per 5 s. Still to settle after the names: the bake's 50 m near limit against what stands outside the window, the 600 m sphere against nearby ground and buildings, and whether frozen ships in the window are acceptable.
- **Left as they are.** The seal's `sealedToo` families, which include the sun's own draws (`sun`, `sun arcs`), still go while sealed in the ship in a surface hangar, about two seconds at this port. A Stronghold Carrier reads `SurfaceStation`, so it keeps its sun too, and its hangar would show the leak if it has one; not flown. An `Outpost` keeps the old rule, and r0 stays 0 at its open pads.

## Flight 34, the list as written

On build 8e317e3f, flat, Steam, no keys.

1. Walk from the lift bank into the room and back, looking both ways: is the look now the same throughout?
2. Is anything missing that you miss? Lamps may have lost a soft shaft of light, since the room's own cones and the outside's share this shader. Stand near a few lamps and look.
3. Walk up to the bar until it clears, and back: any jump in the room's brightness as it does? The cones stay off then too, as the scatter does.
4. The windows: does a pale wedge still show in them? The bake's frames still draw the cones, so a wedge there would be the same thing, in the picture. If it does, say, and they can go from the bake too.

Then: `python tools\edup_log.py --target steam --expect-build 8e317e3f --grep "concourse withheld"`. `light cones` should appear with a count of about 46 a frame.

## Flight 33 results

Build baac9204, flat, Steam, log `edvr_gfx_20261107_115242`. Sean: "Light 1 turned it off". He held step 1 at the lift bank (cam_local 0.6 21.7 -17.3) for 26 s, then went through the rest.

- **It is the light cones,** `1C506222/2519C905`, n=384, additive and depth-tested. Step 1 left out 76513 draws over those 26 s.
- **Why the line is sharp, and why "everywhere" with the cull off.** INFERRED: with the cull on the mark drops a cone's pixels with nothing within 100 m behind them, so cones outside the room only ever touched near pixels. Near the lift bank the camera is close enough for one or more to lay over the room. With the cull off nothing limits them. Which cone it is was not measured.
- **The fix, Flight 34:** the cones join the withheld list, off with the cull and off while the windows show the bake, as the scatter is, so the bay's clearing does not bring them back in one frame. Their glow quads, `359BF8FF/92FF8499`, stay limited by the mark. This takes the room's own lamps' cones too, if any are its own; Sean asked for the look gone consistently, and Flight 34's second item is the check.
- Sean did not report what step 1 changes further in.
- Open: the bake's frames still draw the cones, since the look's frames withhold nothing.

## Flight 33, the list as written

On build baac9204, flat, Steam, HOME as before, now 28 steps. The order puts first what is drawn before the glass.

1. At the lift bank, the same wall: press HOME and wait a second each time. Which step takes the pale look away? `2519C905` and `92FF8499`, the first two, are the room's glow cones and their sprites, the leading suspects. Some later steps will wreck the picture, such as the tone-side draws; that is fine, and HOME moves on.
2. The last step turns every light off together. Is the pale look still there on an unlit room?
3. A few metres in, the step that did it: what does it change there?

Then: `python tools\edup_log.py --target steam --expect-build baac9204 --grep "concourse light test"`.

What each answer means: a glow step means the veil is additive sprites over the room, and the next question is which instance, since the room's own lamps share the family. Still pale with every light off means it is laid down in the material pass or the sky before the lights, which this key cannot reach, and the next step reads the material pass's constants.

## Flight 32 results

Build c32cd19b, flat, Steam, log `edvr_gfx_20261107_113241`. Sean: "Tested it, none of those turned the light off". He stepped all 15 at the lift bank (cam_local 6.0 21.7 -17.5) and 9 of them further in.

- **Each step acted.** The log counts 54 to 402 draws left out per step while he held it.
- ruled out: any one light family alone (the sun's quad, the eleven volumes), and the half-size chain's composite `DFCBA0EC`. `5BCB6B95`, `97DBC87F` and `D56F859B` turned out to be on the withheld list already (the atmosphere quads and the flares), so those three steps said nothing.
- **A correction to the windows.** Measured on the screenshots, the darks are lifted strongly: luma percentiles 1, 10, 50, 90, 99 go 35, 64, 116, 196, 236 inside and 60, 97, 153, 207, 247 at the lift bank, so the highs barely move. That is the shape of something added, not a gain. The windows' deep blue stays about the same, and the black reveal around them does not: it goes grey-green. So what is added comes before the windows' own draws, which is what the `ruled out: exposure` above rests on.
- **Where the list stops.** The frame's order from the sun on is: lights, the half-size chain, sky, stars, the first-person draws, `DFCBA0EC`, dust, particles, then about 100 draws of the room's own glows, `1C506222/2519C905` (384 indices, additive, depth-tested) and `359BF8FF/92FF8499`, then the glass from `#18276`. F3 never measured the glows: it stops at 1024 draws, and the frame has about 4300.
- **The clip (8.6 s, panning at the lift bank).** The near walls and pipes keep their full contrast, and what is further off goes pale in the same frame, more with distance. So it grows with depth, like fog, and is not a lamp on a nearby surface. Sean's Flight 6 note says the same: "the far ceiling is paler than the near beams". The scatter, the one known depth wash, was withheld on every frame at these spots.
- **Why the glows lead.** They are additive and come before the glass. They are on the far-limited list, so with the cull on their far pixels are dropped, and with the cull off they are not, which fits "everywhere" with the cull off. A cone the camera is inside covers the whole screen, which would fit a sharp line a few metres wide. INFERRED, and what Flight 33's first step tests.

## Flight 32, the list as written

On build c32cd19b, flat, Steam, no other keys. `hotkey.concourse_light_test` is `HOME` in both Steam inis.

1. Stand at the lift bank where it is blown out, and look at the same wall. Press HOME, wait a second, and watch the wall. A toast says `Light test N of 15: XXXXXXXX off`.
2. Go through all 15. Which step takes the pale look away, leaving the room lit as it is further in? Note the eight digits. Others will darken things in their own ways: that is fine, and it is useful to know which.
3. Do the same a few metres in, for that step only: does it change anything there?

Then: `python tools\edup_log.py --target steam --expect-build c32cd19b --grep "concourse light test"`. Each line says how many draws the step before left out; 0 says a step matched nothing, and that it proves nothing.

What it would say if no step takes it away: the light is in a family's constants before its draw, or in a draw not on the list, and the next step is to dump one family's instance data.

## Flight 31, the list as written

On build 0448d071, flat, Steam, no keys pressed, with `concourse_keep = Root,Perturbator` in `edvr-flat.ini`. The log line: `python tools\edup_log.py --target steam --expect-build 0448d071 --grep "concourse bake|concourse cull: (diag|a station|a window|on|off)|concourse names: (kept|small|culled)|concourse backdrop: distance"`.

1. Walk the length of the bar: do any tables still flicker or pop in? If they do, stand where one does and press F3, which names what is kept close to you.
2. If they do not, comment the key out, which is live, and walk it again: does the flicker come back? That is what proves it.
3. Windows at the sphere, the default now: still the best of the three? Any problem it brings that the others did not, such as near structures sliding as you walk? `diag.concourse_backdrop_radius`, 600 m, is live if the distance feels wrong.
4. Frame rate at a spot against the last build: the sphere takes 6 cube samples a window pixel, against up to 23 for the march.

## Flight 30 results

Build edbc145c, flat, Steam, 3840x2160, log `edvr_gfx_20261107_102412`, one screenshot and a NumLock at 10:30:43. Sean stepped F9 through the three modes for three minutes. "Sphere looks best, I also noticed some of the bar tables seem to flicker and then stay solid - not sure why."

- **A first load on foot needs no key now.** `journal: the last station saved is of type Orbis`, `the station's type is Orbis`, then `armed: no ride` and the bake at 10:25:50.
- **The sphere wins by eye.** A rigid picture at 600 m, with depth written to match, so the motion agrees with the picture exactly, and it is the cheapest of the three. The march stays behind F9 for now.
- **The flicker is not the panes.** Three panes changed side many times (n=30 start=11834224 and n=24 start=12012808 and 11983064), but in the first 10 s after the bake; none in the two minutes before the NumLock.
- **The census's two frames have the same draws:** 1783 and 1784 bits of geometry in the main view, differing in one particle. So nothing was vanishing frame to frame at that moment. The world's pixel shaders differ throughout between the two frames (E06E61AE and CB9F297E, for 538 draws), which is not explained and not new evidence for this.
- **Leading hypothesis, unproven.** The judge keeps a name off the allow-list only while it is small and within 40 m of the camera. The room is about 130 m across, so such a record pops in at 40 m, and flickers at that distance as the head moves and the rig's verdict refreshes. The bake's survey names the room's records: `Root` x8, r=0.9, is off the list and about the size of a table. `Perturbator` x2, r=1.5, is the other. No `Root` is kept anywhere else in this station.
- What would refute it: the flicker staying with the key on.

**If it holds:** those names go on the allow-list and the key comes out. Comparing by the camera's distance is the weakness behind it: a record's place in the room does not change as you walk, so a rule by distance from the room's centre would have no pop-in for names not yet known. That costs an unknown number of extra draws, so it is not proposed yet.

## Flight 30, the list as written

On build edbc145c, flat, Steam, pressing neither F2 nor Insert unless a step says so. A clip while walking would help, since that is the question. The log line: `python tools\edup_log.py --target steam --expect-build edbc145c --grep "concourse bake|concourse cull: (diag|a station|a window|waiting|on|off)|concourse camera test: (armed|mode 3: the window)|concourse backdrop: distance|journal: (the station|the last station|no station)"`.

1. Walk with a window in view: do the edges still ripple and swim? Does the far station still smear? Turning on the spot tells the camera's motion from the camera's position.
2. Step F9 through `distance by depth`, `distance by sphere` and `distance off`, the same walk each time. Which looks best? By depth is now a march, so this compares it with a rigid picture at a fixed distance, and with no parallax at all.
3. Edges where a near structure stands against a far one: a smear of its colour behind it is what one layer of picture does when you walk past. How much of it is there?
4. People, kiosks and anything else standing in front of a window: still in front? Holograms and signs on the glass: still drawn?
5. Frame rate at the same spot against the last build: the march is up to 23 cube samples a window pixel, against 14.
6. The bay: walk up until it clears. Any flicker as the picture hands over to the real station? The picture's depth now meets the real station's there.
7. To test a first load on foot again: delete `%LOCALAPPDATA%\EDVR\last-station.txt`, then load on foot. `journal: the station is named in an older journal` and the bake should follow with no key.

What it would say if the depth did not reach the resolve: the smear stays with the ripple gone. Then the temporal resolve's depth is not the target the pane writes, and that is the next thing to read.

## Flight 29 results

Build cf61f513, flat, Steam, 3840x2160, log `edvr_gfx_20261107_095540`, one clip of 2.1 s while walking. Sean: "Bake looks much better, though I had to hit F2 to get it to load? Or maybe it took longer? Can we fix the motion issues?"

- **All five directions arrived,** with the first two frames lost as expected: finished after 125 ms, the hold 172 ms, 11 Presents.
- **F2.** He loaded on foot with no `last-station.txt` yet, so the type was unknown and the cull stayed off, as designed; F2 forced it. Boarding wrote the file (`Orbis`). Not delay: he had been on foot for 36 s before the press, and the bake followed about 2 s after it.
- **The clip.** Near parts move a long way and the far picture moves little, so the parallax is right in kind. The edges in the picture ripple, and change shape from frame to frame while the camera moves.
- **Cause 1, seen in the shader, not yet proven by flight:** the depth mode repeated t = the distance at the last guess's direction, four times, from a lod 1 distance. At an edge that does not settle, so a small move of the camera changes which side each pixel lands on.
- **Cause 2, read in the code, not yet proven by flight:** the pane wrote its own depth, and the temporal resolve works out the camera's motion from depth. A far picture was moved as if it were on the glass: at the clip's walking speed, far further than it really moved.
- The pale leaf shape was not named on this flight.

**Fix, Flight 30:** a march along the ray, 12 steps even in 1 / distance at lod 1, then 5 bisections at lod 0. The pixel shader writes the picture's depth (`SV_Depth`, near / view depth) under the game's own depth test, so what stands in front of the glass stays in front. And a load on foot with nothing saved reads the newest journals until one names a station.

- The cost: up to 23 cube samples a window pixel against 14, and no early depth test on those pixels. Sean's goal is the saving first, so that is what item 5 is for.

## Flight 29, the list as written

On build cf61f513, flat, Steam, a fresh load in the ship, **pressing neither F2 nor Insert**. The log line: `python tools\edup_log.py --target steam --expect-build cf61f513 --grep "concourse bake|concourse cull: (diag|a station|a window|waiting|on|off)|concourse camera test: (armed|mode 3: the window)|journal: (the station|the last station)|status flags changed"`. The list is Flight 28's below, which still stands, and:

1. The first ride: are the large pale areas gone, and the seam with them? The sharp station should now fill every window. `finished` should say 5 frames captured, with the hold somewhat longer: about 12 frames against 9.
2. The pale leaf shape, if it is still there: F2 off, then F9, as in Flight 28's list.
3. The station's type: `journal: the station's type is Orbis` after the load, `concourse cull: a station of type Orbis: the cull and its bake run`.
4. Disembark once, quit in the concourse, load back in: the saved station should name the type (`journal: the last station saved is of type Orbis`). Loading on foot before any disembark on this build reads unknown, and stays the game's own.

What it would say if a direction never arrived: `the window ends ...: time ran out`, and `cancelled: nothing is published`, with the game's own station kept.

## Flight 28 results

Build f14a8134, flat, Steam, log `edvr_gfx_20261107_093300`, two screenshots at 4K with large flat pale areas and stair-stepped edges. Sean: "Looks like the bake regressed".

- **The near value took.** Every capture logged `near plane 50.000 m`, and sharp station now shows where the seam was.
- **Automatic worked.** `diag.concourse_cull = auto: on`, hooks in at launch, with no key pressed. On a direct load: `armed: no ride`, 2 s after the room came into view, and the bake ran. On the ride up, as before. The slider moved the share live (24.7, 100, 0).
- **Only 3 of 5 directions were captured,** on both bakes. +a2 and -a2, along the room, were missing.
- **Why.** The first two frames after the near value changes take the copy route: `flat hdr route: declined ... producer-source-identity-mismatch`, then `engine-source-not-ready`, with `flat runtime early fallback`. That path returns before `concoursePaneBakeFrame`. The same two frames follow when the value goes back, under the hold.
- ruled out: one frame for each direction with no look at what arrived, because any frame that does not reach the resolve loses its direction for good.
- **Why it showed so badly.** The crop to 90 degrees left no overlap to cover the loss. Where nothing is captured, the window shows its level colour: that is the flat pale. The stair-steps and the wave are INFERRED to be the depth look-up warping the frame's straight edge.

**Fix, Flight 29:** the tick aims at the next direction not yet captured and goes round until all have arrived, with a limit of 23 frames. Past it nothing is published, since a window with a direction missing is worse than the game's own. The rig simulates 2 frames lost at 5 frames' latency: every direction in about 12 frames.

**Also in that build:** the station's type from the journal, as "Other stations, and loading straight in" below proposed: Coriolis, Orbis and Ocellus run, everything else and unknown stay the game's own, and `%LOCALAPPDATA%\EDVR\last-station.txt` holds the last station's type and a hash of its name.

## Flight 28, the list as written

On build f14a8134, flat, Steam, a fresh load in the ship. **Press neither F2 nor Insert**: F2 now turns the cull off, and Insert steps the bake's mode. A clip would help.

1. First ride: does the bake run by itself, and is the seam gone? Each capture should end `near plane 50.000 m`.
2. A pale leaf shape with stair-stepped edges sat in the blue on Flight 27. If it is still there, stand under it with F2 off: if the real view shows it, it is the game's own, perhaps a patch of sunlight. If that does not settle it, step F9: a change points at the depth look-up.
3. Is the station in the windows lit as before? Is anything missing that was there?
4. Quit to the menu while in the concourse and load back in: is the station real for about 2 s, then a small hitch, then the bake? Does the bake look whole? The risk is that the station has not finished streaming.
5. Raise the resolution while standing in the room: does it bake again about a second later? Does anything blur after the freeze, since the camera leaves and returns with no cut to hide it?
6. Performance tab: is Window 3D distance under Optimized stations, dimmed with its hint when that is off? At 200, does the bar clear from further away?
7. Undock: `concourse bake: the station is left behind: the cube is released` about 5 s later.

Then: `python tools\edup_log.py --target steam --expect-build f14a8134 --grep "concourse bake|concourse cull: (diag|a window|waiting|on|off)|concourse camera test: (armed|mode 3: the window)|status flags changed"`.

What it would say if the new code never ran: `diag.concourse_cull = auto: off`, or no `armed:` line. `camera hooks` says whether the install took. `armed: no ride` names the direct path, and `waiting (... no-bake-yet=1)` is the cull held for it.

## Flight 27 results

Build 93a3e4f8, flat, Steam, logs `edvr_gfx_20261107_083829` (a bake at 1920x1080, then a resize to 3840x2160 at 08:41:51) and `edvr_gfx_20261107_084626` (a bake at 3840x2160). One screenshot at 4K: "Still seeing this at 4k now".

- **The near write did not take.** Every capture logged `near plane 0.025 m`, so the seam says nothing about the idea. That is what the capture line was added to tell apart.
- ruled out: writing the near plane to the camera at the producer (`+0x234`), because `0x287AAE0` stores a float in `.data` at RVA `0x5E85B8C`, holding 0.025, over the four view owners' cameras right after the views take their slots (`0x287AEC4` and on, camera object `+0x254`, dirty bits `0xD`). A rip-relative scan of build 334925 finds 15 reads of it and no write, among them 5 in `0x2845730`, VR's camera path.
- ruled out: the render size as the cause of the seam, because it stayed in a bake captured at 3840x2160.
- **A resize keeps the bake.** `ResizeBuffers` on the same device, DLAA made anew at 3840x2160, the cube not released; the picture stays as sharp as its frames. A 1080-row frame fills about half the cube's detail, a 2160-row frame all of it.
- **The screenshot.** The sharp station ends at a rounded corner like a window frame, with the pale fan past it, which looks like an opening in something nearer. INFERRED. Separately, a flat pale leaf shape with stair-stepped edges sits in the blue. Not explained: it could be the game's sunlight through a coarse shadow map, or the depth look-up.

**Fix, Flight 28:** the producer hook raises that value with the camera it writes, and each tick that is not acting puts it back. The hold does not end on a frame back at the player with the plane raised. Each frame keeps only its middle 90 degrees, so the plane clips one box around the centre and no frame overwrites what its neighbour kept.

**Also in that build, at Sean's word ("productionize this"):**

- `diag.concourse_cull` defaults to `auto`: on with `fix.optimized_stations`, in flat, once the camera hooks are in. VR and other game builds stay off. The hooks' gate is shut unless a bake is armed or running.
- The cull waits for a bake. Until there is one, the room and its windows are the game's own, and a bake that fails leaves them so.
- A bake with no ride: the room in view for 2 s with no bake, or a render height risen past the bake's and steady for 1 s, up to the cube's size. Each is tried twice.
- The cube is released 5 s after the commander is neither on foot nor docked, or when the setting goes off.
- Sean's skylight and glass values are the defaults: `diag.concourse_glow` 0.5,0.5,1,0.5 and `diag.concourse_glass` 0.92,0.97,0.96,0.03,0.25,0.25.
- `fix.station_window_distance`, the Window 3D distance row, 0 to 250 percent, flat Performance tab, dimmed while `fix.optimized_stations` is off. Sean asked for the distance at which the bar turns 3D. The trigger stays how much of the view the window fills, so 200 divides the share by 4 and is about twice as far: not metres, and it applies to every window that can clear. `diag.concourse_clear_share` still overrides it with a number.

## Other stations, and loading straight in

Proposed 2026-11-07. The direct-load trigger is built (Flight 28), and so is the type table (Flight 29). Sean expects the Coriolis, Ocellus and Orbis to share a layout and will confirm; surface ports and outposts need checking too.

- **What ties the cull to one layout:** the anchor's name, `Standard_SocialArea`, and the keep list's prefixes (`kAnchorName`, `kAllowPrefixes` in `concourse_cull_core.h`). A layout with another room name gets no cull and no bake, and only the absence of the `concourse camera test: mode 3` line says so. Other prefixes could lose the room's own objects. The near limit, the five directions and the window draws have only been tried in this room.
- **The journal:** `Location` on a docked load, and `Embark`, carry `StationName`, `StationType` and `MarketID`. `Location` on an on-foot load carries only `Body` and `BodyType`, so on a direct load the type is not known at bake time, and the bake takes all five directions unless the name matches an earlier event. EDVR does not read any of these today.
- **Loading straight in:** one trigger for both, the room's first sighting on foot with no bake yet. Only a ride arms the bake today, so a direct load shows the plain fill until the first ride. The hold should read as a slightly longer load. The risk: after a load the station may not have finished streaming when the room is first seen, so the bake could catch missing pieces or low-res textures. One flight tells. If it does, wait for the load to finish.
- **Saving bakes to disk:** not proposed. A bake takes about 94 ms, and a saved one would take several hundred MB per station with stale lighting.

**The type table, Sean's idea:** save the station type at each `Disembark`, and use it to choose the bake. Across 3,097 journals `Disembark` at a station always carries `StationType`, `StationName` and `MarketID` (`Embark` and `Docked` too), and an on-foot `Location` never does, in 580 of 580. So the last disembark names the station a direct load is in, by matching `Body`. Save a hash of the name and the type under `%LOCALAPPDATA%\EDVR`. Rows: Coriolis, Orbis and Ocellus (`Bernal` looks the same) run the cull and bake; everything else, and unknown, stays the game's own. `diag.concourse_cull = on` forces a type for a test. Later, a row can name only the directions its windows need.

- Fleet carriers are `FleetCarrier`, named by callsign. Stronghold carriers are `SurfaceStation`, named `Stronghold Carrier` (last seen January 2025), so a row for surface ports would need to check the name.
- On a docked load `Body` is the star or planet on both, so an on-foot load there probably does not match and reads unknown. No `Disembark` on a carrier in Sean's journals, so that is not known.
- A surface port is a different layout: Sean, at a `CraterOutpost`, "It only has this one big bay window". Its anchor name and keep list are untried, and `bakePlanFor` leaves it to the game. A `Disembark` there has `OnStation:false, OnPlanet:true`, Body the planet, and no station fields, so the type reads unknown from then on, and a row for surface ports needs either the saved type or Status (`journal_station::atSurfacePort`). See "Flight 34 results".

**VR, after flat:** the camera route at `0x283FA20` with both eyes at the centre and a square 90 degrees (the biggest unknown); a capture point, since flat's resolve is not on VR's path; hiding the frames with the runtime's withhold and replay (`native_runtime_host.h`), which may refuse across the lift's cut. To test: stereo windows, native on-foot and the 2D screen, temporal history and exposure after the bake, frame time, the 12 percent share. About three flights.

## Flight 27, the list as written

On build 93a3e4f8, flat, Steam, the keys and steps as Flight 25: a fresh load in the ship, disembark, in the hangar F2 once and Insert three times to `bake on arrival`, before the first ride. A clip would help.

1. The window above the lifts: is the seam gone? What is there in its place: more station, or a new gap?
2. Is the station in the windows lit as before? A moved near plane could change anything the game works out from it.
3. Is anything missing that was there on Flight 26? Clipping removes whatever is within 50 m along each axis, in a box around the room's centre.
4. If the seam is still there: F9 off the depth mode, and tell me whether it changes. `diag.concourse_bake_near` is read at each ride up, 5 to 400, and now moves the plane too.

Then: `python tools\edup_log.py --target steam --expect-build 93a3e4f8 --grep "concourse bake|concourse names: (kept near|the survey above)|concourse camera test: (armed|mode 3: the window)|concourse cull: the camera cut|concourse clear|status flags changed"`.

What it would say: each `capture N` ends `near plane 50.000 m`. `near plane 0.025 m` means the write at camera+0x234 did not reach the projection, and the seam says nothing about the idea.

## Flight 26, the list as written

On build 31720543, flat, Steam, the keys and steps as Flight 25: a fresh load in the ship, disembark, in the hangar F2 once and Insert three times to `bake on arrival`, before the first ride. A clip would help.

1. Arrival: any flash, jump or stutter? The hold should be shorter.
2. Skylights: are the white shapes, the pale shapes and the grid gone? Do they match the angled ones?
3. What is still wrong in the windows: dark band, blur, anything floating, anything missing that belongs there?
4. Walk up to the bay and back. Does the jump to pale still happen? Does anything else change as it clears?
5. Moving, with windows in view: does it still smear? Standing still and turning, against walking, tells history from the rest.
6. `diag.concourse_glass` in `edvr-flat.ini` is live: `r,g,b,haze,blur,fresnel`.

Then: `python tools\edup_log.py --target steam --expect-build 31720543 --grep "concourse bake|concourse names: (kept near|the survey above)|concourse camera test: (armed|mode 3: the window)|concourse cull: the camera cut|concourse clear|status flags changed"`.

What it would say: `capture N` five times, one per direction, then `finished` and `the hold ends` with its milliseconds, against 219 before. `kept near the centre` names what could still stand in the bake's way. No such lines: the survey did not run in the bake.

## Flight 25 results

Build 5cdb2575, flat, Steam, 1920x1080, log `edvr_gfx_20261107_072457`, one screenshot and a 10 s clip. A true first ride: disembark at 07:26:36, ride up at 07:27:01, no room seen before. Sean: "Pretty good for a first attempt, with some smearing and artifacts."

- **The flow works.** Armed with the doors shut, the cube ready in 62 ms. The cut at 07:27:07.663; the anchor seen again and the bake begun 78 ms after it; the doors at 1.52 and 1.37, just starting. 11 frames captured, all five directions in order, field of view 90.0 in every one. Finished 172 ms after beginning, and the hold ended at 219 ms: 14 Presents. Longer than the 60 ms estimated, and not reported as visible.
- **The frame rate held.** With the bake in the windows, 7.1 to 10 ms a frame across the room once the arrival's slow seconds passed (17.9, 15.9, 17.5, 12.2), and 6.94, the cap, for a stretch. The bake's own frames ran about 11 ms.
- **White shapes in the skylights, in the shape of people and props, are holes.** The depth filter left those texels empty, and the display fills what is empty with the window's level, which is 4 on a skylight. The grid over the skylights is the same thing at a finer scale: one 1080p frame across 90 degrees is about 12 pixels a degree, against 23 texels, so a forward splat fills about one texel in four.
- **The wash is the glass.** The haze and the Fresnel term mix toward that same level, so 0.06 of 4 adds 0.24 to a scene whose darks are near 0.01. It is strongest on skylights, and the deep blacks of Flight 24's look are a mid blue here.
- **The smearing is not settled.** Coarse mips filling the holes look like smears in the stills. It could also be DLSS history in motion, since a pane's content moves unlike the pane. The next flight separates them.
- The helmet's arcs did not reach the capture. The dark band, the near limit and the directions are not judged yet.
- **Gaps (Sean's second screenshot).** Pale shapes in the skylights are the room's things hanging between the centre and the station: walkways, doors, panels, people. The depth filter leaves them out, but the bake sees from one place, so nothing was captured behind them.
- **The jump at the bay (Sean's clip, "anything we can do to make this blend better?").** As the cull lifts, the whole picture, room included, goes from deep and clear to pale in one frame. That is the scatter, the fullscreen additive wash (Flight 5), coming back when the withheld families do. The bake holds none, and neither does the room with the cull on. So it stays off while the windows show the bake.

**Fixes, Flight 26:** the display takes coverage from the neighbours (colour over coverage) and never the level for a hole; the sheen is not scaled by the level; the capture samples the frame per cube texel, so there are no holes from the splat; no settle frames and an end on the last direction's arrival, for a shorter hold; small records reaching within 60 m of the centre are not drawn in the bake frames, whatever their name, so what is behind them is captured, and the log lists what is still kept near the centre; the scatter stays off while the windows show the bake. Live in Sean's `edvr-flat.ini`, his word on skylights matching the angled ones: `concourse_glow` 0.5,0.5,1,0.5, and `concourse_glass` haze 0.03 and blur 0.25; not yet the code's defaults. Then the windows' own measure of which parts they use.

## Flight 25, the list as written

On build 5cdb2575, flat, Steam, with any anti-aliasing mode that runs the resolve (DLSS, DLAA, FSR, XeSS or TAA): the capture sits in its path. A clip would help. Insert steps the mode, with a notice: `log only`, `look from the room's centre`, `bake on arrival`, off.

1. Start the game fresh, loaded into the ship, docked. Disembark.
2. In the hangar, on foot, **before the first ride**: F2 once, and Insert three times to `bake on arrival`.
3. Ride up. Does the arrival look normal? It should hold the cabin for about a tenth of a second as the doors start. Any flash, jump or stutter you would notice?
4. Look at the windows, in the order you meet them. Do they show the station? Is it lit right against the room? Do people, crates or the helmet's arcs show in it? Is it sharp enough? The first half second may be plain, as the pane table learns its sides.
5. Walk with windows in view. Does the view hold, or slide? F9 steps how distance is used: `distance by depth` is the default.
6. Frame rate looking out from the lift bank, with the bake on the windows, against what you are used to. F4 steps the pane modes, and `Windows: captured view` is the bake's.
7. If you ride down and up again it bakes again. `diag.concourse_bake_near` (default 50) and `diag.concourse_bake_dirs` (default 11111, in the order +a2 -a2 +a0 -a0 up) in `edvr-flat.ini` are read at each ride up, so a window that goes blank with a digit at 0 needed that direction.

Then: `python tools\edup_log.py --target steam --expect-build 5cdb2575 --grep "concourse bake|concourse camera test: (armed|mode 3 \(|mode 3: the window)|concourse cull: the camera cut|concourse pane: mode|status flags changed|walk survey: zone"`.

If the code never ran: no `concourse camera test: armed` line, so F2 or the mode was off. Not ready: `the cube is NOT READY`. Fired with nothing captured: `NOTHING CAPTURED`, and `concourse bake: a frame refused` if it got that far. Ran: `capture N` lines with the forward axis of each direction and the field of view, about 90, then `finished`, then `the hold ends` with its Presents and milliseconds. A direction missing from the capture lines means the tail is too short.

## Flight 24 results

Build c1f3ca32, flat, Steam, 1920x1080 this flight, log `edvr_gfx_20261107_064844`, with a 30 fps clip. The first ride up (06:52:07) ran with the mode off: Insert came at 06:52:39, after the ride down. So the one look was on the second ride up (06:53:06), and this is not yet a first visit.

- **Whole and lit at once.** Frame by frame, the clip goes from the shut cabin to the station, pad 31 and the hub, with nothing filling in afterwards that a 30 fps clip can see. Lighting looks right. The ride-down look's mid-distance structure is there too.
- **The empty direction is the floor side.** Six directions in order: 0 ahead, pad 31 and the hub; 1 a dark block with lit barrels; 2 pad 33; 3 the block and barrels again; 4 stars only; 5 the far wall, seen from below, past the room's leftovers. Direction 5 is up: people's soles face the camera. So direction 4 is straight down through the floor, out of the station's hull. No window faces it, so it costs nothing.
- **Room's leftovers, in the up direction.** People, crates, panels, doors and furniture hang in space above the centre, with the room's shell gone. None of it is on the keep list. Depth should remove them: all are nearer than the windows.
- **A dark band fills the lower part of directions 0 to 3,** with a pale surface below it. Not identified, and its distance is not known. If it is nearer than the windows, depth removes it. If it is past them, the windows should see it, and it would be right.
- **The visor is in the picture.** The helmet glass's arcs and the HUD are drawn over the look. A capture has to be taken before them.
- ruled out: the anchor seen well before the cut. Armed 6047 ms before; the cut's tick at 06:53:06.001 read the anchor 31 ms old, yet no tick before it fired, and the window opened 20 ms after it with the doors at 1.51 and 1.37, just starting (shut is 1.50 and 1.40). So the sighting looks like the cut's own frame, and the 125 to 188 ms ages on first rides look like that frame's stall, not time in hand. INFERRED from one fire; the first rides have no look to test it.
- **So the bake overlaps the doors starting.** The first 16 ticks ran about 9.6 ms apart, 153 ms in all, so six frames take about 60 ms, at 1080p. The first door pair moves from 46 to 145 ms after the cut. The picture would have to be held about 60 ms over a shut cabin whose doors are just starting. The second after the cut already runs slow on first rides (18.9 and 25.6 ms a frame), and the cut resets DLSS anyway. Not flown: the hold.
- No faults. Frame means in the look 6.9 to 7.1 ms once settled; 8.62 ms in the second holding the cut.

**Next, built as Flight 25:** at arrival, capture from the centre into the backdrop cube, before the visor, with a depth filter, holding the last image at Present meanwhile. Flown as a true first ride. VR has its own camera route and would come later.

**Only what the windows need (Sean, 2026-11-07):** "only capture the directions the available windows dictate", "and the dimensions necessary to do so": this concourse has a length of skylights looking up, no windows on the horizontal sides, and some angled ones near the roof. A first arrival cannot ask the room, since the panes are only learned once it is drawn. But the layout should be fixed against the anchor's axes, so a table in that frame holds for the room. So Flight 25's directions are the room's axes, with a key to leave any out. A narrower frustum draws less, and a strip plus patches stores far less than a cube, most of which no window looks at. Next after the look is judged: have the window draws log which parts of the cube they sample, as the room is walked, and bake exactly that.

## Flight 24, the list as written

On build c1f3ca32, flat, Steam. Hides nothing: the view jumps just before you arrive, turns through six directions, 0.8 s each, then comes back with you standing in the concourse's lift. A clip would help most here, since the first frames are what matter. Insert steps the mode, with a notice: `log only`, `look from the room's centre`, off.

1. Start the game fresh, loaded into the ship, docked: the first ride has to be the first time this session the room is seen. Disembark.
2. In the hangar, on foot, F2 once and Insert twice to `look from the room's centre`.
3. Ride up. It fires by itself just before arrival. In the first direction especially: is the station there at once, or does it fill in? Blurred, then sharp? Lit properly? Anything missing that the look from the ride down had? Which direction is the empty one, if any?
4. Ride down and up again for a second look. It fires on rides up only.

Then: `python tools\edup_log.py --target steam --expect-build c1f3ca32 --grep "concourse camera test|concourse cull: the camera cut|flat camera origin|flat resolve reset|walk survey: zone|status flags changed"`.

If the code never ran: `not installed` or `NOT CALLED` per hook. Never armed: no `armed:` line, so F2 was off or the lift gate did not come on. Armed with no sighting: `the anchor was not seen`. Fired: `the window opens N ms after arming`, its gaps saying whether the doors were still shut, and the camera cut's line coming after it, which gives the time left. `the look's first 16 ticks` gives what a bake of that many frames would have cost right there.

## Flight 23 results

Build 19881a34, flat, Steam, log `edvr_gfx_20261107_060254`. Two looks on rides down; a ride up logged `NO ANCHOR HERE` and wrote nothing, as designed. Sean: "It only triggers going down. the station appears mostly whole, there was one angle which appeared to look into empty space (skybox only). I also was looking up and saw NPCs walking above me at one point".

- **A bake frame is cheap.** With the room off and the rest on, 7.14 to 8.06 ms a frame over both looks, against 12 to 13 ms in the cabin just before and after, with the cap at 6.94. Two rides, four directions' worth of seconds each. Records kept went from about 280 to about 2500 a frame. My earlier estimate of 16 ms a frame was far too high: six directions would be about 50 ms.
- **What that suggests about cost.** At the lift bank F2 off read 16.5 ms and the full cull 12.8 ms. Here the outside on and the room off reads under 8 ms. So the room itself may cost more than the outside. Different views, one spot: not measured directly.
- **The camera held.** `RV cam minus camera after` 0.00, written on every call, directions as asked, no faults. The centre, 47.11 -27.24 281.58, is 33 m from that lift's cabin. The window ended by time both rides, and the cut came 1.2 s later.
- **The empty direction.** Not yet known which of the six. If no window faces it, it costs nothing. If one does, a station part is being culled with the room: an allow-listed name that is really structure. Which it is decides whether the keep list needs work.
- **NPCs stayed.** They are not caught by name or by the small-record rule, so they do not reach the judgement that way. For a bake, depth may be the better filter than names: anything nearer the centre than the windows is the room's, whatever it is.
- **The disembark.** The game itself stalled for most of a second there: 32.26 ms a frame over 31 frames in the second holding the camera jump, then 7.35 ms. So up to about 0.7 s of the transition is already a held picture, the frames for a bake would cost about 50 ms of it, and nothing here says the screen is black yet. NumLock landed 1.8 s before the jump; the census shows nothing that names a fade, and when the screen goes black was not reported.

**The flow it has to serve (Sean, 2026-11-07):** "player docks -> disembarks -> enters the lift at the hanger -> a baked stand in for the cull will need to be generated by the time the lift doors open". A ride down is no use for that, so the capture does not start there. Past logs for a true first ride, from a disembark with no room seen before (`edvr_gfx_20261106_130939`), and a later ride up after a disembark (`edvr_gfx_20261107_060254`): the anchor is first seen no later than 156 and 188 ms before the cut, the doors open 46 to 145 ms after it, and the second after the cut holds a game stall already (18.9 and 25.6 ms a frame). So the room's position looked to be known about 200 to 330 ms before the doors open; Flight 24 corrects that, see its results. Once latched, the anchor's record stays readable from the hangar on later rides in a session (`concourse frame` lines). The cut also resets DLSS anyway. Not known: how complete and detailed the station is at that moment.

**Next, Flight 24:** the look on a ride up, from the anchor's first sighting.

## Flight 23, the list as written

On build 19881a34, flat, Steam. Hides nothing: the view jumps and turns, and that is the test. A clip would help, as the last one did. Insert steps the mode, with a notice: `log only`, `look from the room's centre`, off. The look runs for 4.8 s from the moment the lift's doors have shut, once per ride, on a ride that starts in the concourse; on one from the hangar it logs `NO ANCHOR HERE` and writes nothing.

1. Load into the ship, docked. Disembark. As the screen fades, press NumLock. When does it go fully black: before the view changes, or after?
2. Walk to the lift and ride up. In the concourse, F2 once, and Insert twice to `look from the room's centre`.
3. Ride down. When the doors have shut the view goes to the room's centre and turns through six directions, 0.8 s each: ahead, three quarter turns, straight up, straight down. What can you see? Is the station whole? Is it lit properly, or dark or flat? Sharp, or blurred and low in detail? Does it pop in over the 0.8 s? Is any of the room left floating, and any people or ships?
4. Ride up and down once more, for a second look.

Then: `python tools\edup_log.py --target steam --expect-build 19881a34 --grep "concourse camera test|flat camera origin|flat resolve reset|flat draw census|DC (begin|end)|walk survey: zone|status flags changed"`.

If the code never ran: `not installed` or `NOT CALLED` per hook. A ride with no room: `NO ANCHOR HERE` and `written 0`. Ran: `written N`, `camera after` equal to the centre in the window's first line, and `RV cam minus camera after` near 0. `walk survey` frame means in the window price an unobstructed frame, one ride only.

## Flight 22 results

Build ac4ab507, flat, Steam, log `edvr_gfx_20261107_050539`. Log only, then a lift ride down and a turn ride up. Sean: "on the lift 30m, the elevator vanished and I saw some unculled structure and skybox and the turn 90 degrees shifted my view".

- **The picture follows.** Lift: `flat camera origin` step max 30.03, against 0.05 before and after, and `flat resolve reset event` fired at both ends of the window. `RV cam minus camera after` 0.00 and the view slot took it too. Turn: forward row (0.02 0.03 -1.00) to (-1.00 0.03 -0.02), written 36 of 36 calls an interval.
- **What he saw is what should be there.** The cull was on, so 30 m above the cabin leaves the kept names and the sky. It is not yet a look at the station itself.
- **One call a frame.** `0x2874290` matched `0x1EA9C0` call for call, other call sites 0. In log only, the camera equals the RenderView's.
- **No reset on the turn.** The reset fired on position only, so a bake's turned frames will need their own reset or to stay out of the history.
- **Frame time in the lift window:** 9.2 to 9.9 ms, against 12 to 13 ms just before and after. One ride, cull on; not a bake's cost.
- No faults, and the ride went on normally: the cut came 6 s after the doors shut, as on other rides. No census was taken.

**Smallest next test (H50), built as Flight 23:** in the window, put the camera at the room's centre with the cull off and the room's rigs vetoed (the keep list turned inside out), and step it through directions, with the view visible. That shows what a bake would capture, before any capture, shape or hiding is built. Then, in order: capture with the existing backdrop code, the shape, the trigger at the fade.

## Flight 22, the list as written

On build ac4ab507, flat, Steam. Hides nothing, and the view should jump: that is the test. F2 once. Insert steps the mode, with a notice: `log only`, `lift 30 m`, `turn 90 degrees`, off. Lift and turn act for 3 s from the moment the lift's doors have shut, once per ride.

1. Insert to `log only`. Stay on foot in the concourse about 10 s.
2. Insert to `lift 30 m`. Ride. When the doors have shut, does the view jump, and does it come back after 3 s? What can you see from up there? Does anything vanish or look broken?
3. Insert to `turn 90 degrees`. Ride back. Same questions.
4. On a ride where the view moved, a NumLock inside the 3 s for a census, if it is easy to catch.
5. Did your character, the cabin's sound or the lift itself do anything odd during the 3 s?

Then: `python tools\edup_log.py --target steam --expect-build ac4ab507 --grep "concourse camera test|flat camera origin|flat resolve reset"`.

If the code never ran: `not installed` or `NOT CALLED` per hook. Ran and wrote, nothing followed: `written N`, with `flat camera origin` steady. Reached: `RV cam minus camera after` near 0, `flat camera origin` stepping about 30 m, and the view moves. In log only, `RV cam minus camera after` near 0 says this producer's camera is view 0's, with nothing written.

**Flat's camera, RE 2026-11-07, not flown, `analysis\decomp\verify_20261107_flat_camera_build.txt`:** `0x281D190` takes a pose record from the camera manager, builds a camera on its stack with `0x2874290`, then copies it into the camera set: slot 0 (the view node's copy, for draws), slot 1 (same camera with other near and far, for `RV+0x310`) (READ; that slot 1 differs only by near and far is INFERRED). `0x287D5A0` later hands the slots to the views. So the first write point upstream of every copy is that stack local. The pose record's upstream writers were not traced.

## Flight 21 results

Build 1bba08e9, flat, Steam, log `edvr_gfx_20261107_042506`. Log only, then a lift ride and a turn ride. All hooks installed, 0 faults.

- ruled out: `0x283FA20`'s pose as a write point in flat, because `0x283FA20` and `0x2845730` were `NOT CALLED` in every line, in log only and on both rides, so `written 0`. The mailbox route is VR's alone.
- **Flat's camera.** `view 0 camera handed over by the call at 0x287D95D` in every line. Call sites of `0x2874760`, once a frame each: `0x287D916`, `0x287D95D`, `0x288A35B`; `0x287D753` occasionally. `0x287D5A0` holds three of those.
- **Same path on foot and in the lift.** The call sites did not change across the rides.
- Nothing was written, so nothing could have moved; the eye has nothing to add here.

## Flight 21, the list as written

On build 1bba08e9, flat, Steam. Hides nothing, and may glitch. F2 once. Insert steps the mode, with a notice: `log only`, `lift 30 m at the eye pose`, `turn 90 degrees at the eye pose`, off. Lift and turn act for 3 s from the moment the lift's doors have shut, once per ride.

1. Insert to `log only`. Stay on foot in the concourse about 10 s. This names flat's camera with no writes.
2. Insert to `lift 30 m at the eye pose`. Ride. When the doors have shut, does the view jump, and does it come back after 3 s? Does anything vanish?
3. Insert to `turn 90 degrees at the eye pose`. Ride back. Same questions.

Then: `python tools\edup_log.py --target steam --expect-build 1bba08e9 --grep "concourse camera test|flat camera origin|flat resolve reset"`.

If the code never ran: `not installed` or `NOT CALLED` per hook. Ran and wrote, nothing followed: `written N`, `RV cam minus eye pose after` not near 0, `flat camera origin` steady. Reached: that difference near 0, `flat camera origin` stepping about 30 m, a reset, and the view moves.

## Flight 20 results

Build bad4cf70, flat, Steam, log `edvr_gfx_20261107_032222`. Three rides; mode 5 (turn, held) was not flown. The hook ran every frame, 0 faults, and wrote the camera on every call in each window.

| Ride | Mode | Window | Slot minus written | `cb1[275]` step max in that 5 s | Resets |
|---|---|---|---|---|---|
| down | 2, lift | 03:25:16.5 to 19.5 | 0.00 (y 22.27 against a real -7.73) | 0.031 m | none |
| up | 3, turn | 03:25:37.6 to 40.6 | 0.00 | 0.047 m (position, not moved by this mode) | none |
| down | 4, lift held | 03:26:00.4 to 03.4 | 0.00 (y 25.90 against a real -4.10) | 0.0017 m | none |

- **The constants did not follow.** A 30 m move would read as a step of 30 in `flat camera origin`. The actual cuts on the same rides read 225. `flat resolve reset event` fired on the cuts and not in any window.
- ruled out: writing `RV+0x310` before `0x1EA9C0` as the way to move the game's constants, because `cb1[275]` stayed on the real camera, held or not, while the view slot's `+0x540` took the write. Holding it for the whole frame changed nothing, so `cb1` is not filled from this camera later in the frame either.
- **The game rewrites the camera each frame.** Mode 4: 284 held writes overwritten, 0 still there at the next call. `RV cam before` stayed on the real position.
- **By eye:** Sean, "No, nothing moved", on all three rides. So the objects' own matrices did not move either.
- **The cull probably did not follow.** A view turned 90 degrees would cull most of what the real camera sees, and that was not reported. INFERRED: he was asked whether the view moved, not whether anything vanished. Rigs seen were level (about 570 a frame in and out of the windows), but that list is ahead of the per-view cull, so it says nothing. Frame time rose to about 12 ms on both down rides at different moments (3.7 s and 0.9 s after the doors shut), which fits a change of view direction and is not evidence. The census in mode 4 (NumLock at 03:26:02.4, 42465 draws in 2 frames) has no unmoved census from that spot to compare with.
- **Noted, not chased:** after the window on a down ride, no `lift: doors shut` line appeared, though it did on the up ride. The lift work is paused.

**Upstream, RE pass 2026-11-07, build 334925, not flown, `analysis\decomp\verify_20261107_camera_upstream.txt`:**

- **Why the hook missed.** `0x2845730` builds the eye cameras with `0x283FA20` from a pose, then hands out two copies of one camera: `node+0x20`, and `RV+0x310` through `0x2874760` (READ). `cb1` is written at draw time from the draw view's camera (`0x596830`, via `0x5921F0`), not from `RV+0x310` (READ); that this camera is the `node+0x20` copy is INFERRED. So Flight 20 edited only the copy that feeds the cull context.
- **The pose is known to this project.** It is `W+0x3330`, the transition flash's mailbox (`docs\design-transition-flash-engine-fix-2026-09-23.md`, Static rounds 6 and 7): consumed and reset each frame, refilled by the `HMDCamera` tick through `0x2877090` (INFERRED to be that doc's `FUN_142874b20`).
- **In VR the route has already worked.** The flash was the whole frame drawn from the wrong place while the mailbox was empty. The ship-preview camera in rc.2 reaches the rendered eyes by a hook on `0x283FA20` (`native_preview_camera.cpp`), after a write to the stage output did not (`docs\design-native-onfoot-vr-2026-10-13.md`, 2026-10-26).
- **In flat it is not known.** That doc calls the job feeding the driver the per-frame VR job, and `HMDCamera` is a VR class, so `0x2845730` may not run in flat at all. `0x2874760` has six other callers (`0x287D5A0`, `0x287B050`, `0x28007D0`, `0x2801680`, `0x28740B0`, `0x2889940`), and any of them could hand view 0 its camera in flat.
- **The next test:** built as Flight 21 below. A static pass found no flat/VR test in these functions (INFERRED), so which path runs in flat is left to the log. `0x287D5A0` hands cameras from persistent blocks without going through `0x283FA20` (READ), and is the likeliest other path.
- **Still in the way even if it passes:** hiding the frames, DLSS history, a square wide view for the bake, the first-ride centre, and game code reading the pose (`[this+0x7B8]`, unknown; a write at `0x283FA20` is after that listener).

## Flight 20, the list as written

On build bad4cf70, flat, Steam. Hides nothing, and may glitch: this test only asks whether the view follows. F2 once. The camera modes act for 3 s from the moment the lift's doors have shut, once per ride. Insert steps the mode, with a notice: `log only`, `lift 30 m`, `turn 90 degrees`, `lift 30 m, held`, `turn 90 degrees, held`, off.

1. Insert to `lift 30 m`. Ride. Does the view jump when the doors have shut, and come back after 3 s?
2. Insert to `turn 90 degrees`, ride back. Same question.
3. Insert to `lift 30 m, held`. Ride. Does it jump where the first ride did not, or look different?
4. Insert to `turn 90 degrees, held`, ride back. Same question.
5. On any ride that moved the view, a NumLock inside the 3 s for a census, if it is easy to catch.

Then: `python tools\edup_log.py --target steam --expect-build bad4cf70 --grep "concourse camera test|flat camera origin|flat resolve reset|walk survey: zone|lift survey"`.

What would appear if the code never ran: `the hook on 0x1EA9C0 is installed` is missing (or a line says why not), or `hook calls 0` with `THE HOOK WAS NOT CALLED`. Acting with no effect: `camera written on N` with the slot minus written not 0.

## Flight 19, the list as written

On build 2a140a30, flat, Steam. F2 once, then F4 to `Windows: captured view`: automatic capture starts with that, and F6 still captures by hand.

1. Look around the room, and walk. Do the windows fill by themselves? How long is a new direction blurred or flat before it sharpens? Do you notice the captures?
2. Stand still for 30 s looking at the same view, so the log can say whether the captures stop.
3. Walk with windows in view and step F9: `distance by depth`, `distance off`, `distance by sphere`. Which holds up best?
4. `diag.concourse_glass` in `edvr-flat.ini`, `r,g,b,haze,blur,fresnel`, live: what looks most like glass?
5. At the lift bank, camera still, F4 between `Windows: captured view` and `Windows: flat light`, about 5 s each, with the room filled, for the frame time.

Then: `python tools\edup_log.py --target steam --expect-build 2a140a30 --grep "concourse backdrop|concourse pane: mode|walk survey: zone"`.

## Flight 18, the list as written

On build f50a46a2, flat, Steam. F2 once.

1. Under a skylight, looking up, press F6. Are the white dots gone? Is it sharp enough?
2. Walk a few metres, keeping it in view. F9 steps `Windows: distance by depth`, `distance off`, `distance by sphere`. Which holds alignment? If the whole image swims in sphere and depth and not in off, the camera row is wrong.
3. Press F6 from a second spot on the same window, and at a bay window, close but before it clears. Do the captures agree where they meet?
4. Turn quickly: does it smear?
5. With the camera still, F4 between `Windows: captured view` and `Windows: flat light`, about 5 s each, for the frame time.

Then: `python tools\edup_log.py --target steam --expect-build f50a46a2 --grep "concourse backdrop|concourse pane: mode|walk survey: zone"`.

## Flight 17, the list as flown

On build a2fca02e, flat, Steam. F2 once. F6 lifts the cull for 200 ms and captures what is in view; the toast says the mode and the result.

1. At the lift bank, facing across the room, press F6. Do the windows in view fill with the station, and does anything flash? The windows not in view keep the flat colour.
2. F4 between `Windows: captured view` and `Windows: flat light`, with the camera still, about 5 s each. Is the captured view right, and is the image sharp enough?
3. Turn quickly: does it smear or swim? Walk 10-20 m: how soon does it read wrong? Press F6 again from there.
4. Look up at a skylight and press F6. Try to catch a ship in view, and say how much a frozen one bothers you.

Then: `python tools\edup_log.py --target steam --expect-build a2fca02e --grep "concourse backdrop|concourse pane: mode|walk survey: zone"`.

## Earlier: the size test

On build b4f82839, flat, Steam. Take `concourse_keep` out of `edvr-flat.ini` first if it is there. F2 once. At the lift bank, facing across the room, away from the bay windows, F6 steps 150, 80, 40, 20 and 10 m, then back to the flat fill. About 5 s each, no NumLock. What is the smallest size that looks right? Is anything still missing there?

Then: `python tools\edup_log.py --target steam --expect-build b4f82839 --grep "concourse view test|concourse names: culled|walk survey: zone"`.

## Performance candidates

Ranked by expected gain against risk. The first two and the plume are in the build.

1. **Rig veto on foot.** In the build. Part of the measured 1.9 ms.
2. **Cull by name in every view.** In the build. Shadow tiles -60%, probes -20%.
3. **Withhold what only the glass shows.** In the fourth build for smoke, trails, lasers, the planet and flares. Left: stars `68DDDEF0/F7A6E916` (about 9300 instances), space dust `E904D334/095030F2` (512 quads). They only appear past glass, and the mirror covers them, so they are cheap candidates once Flight 4 shows the list is safe.
4. **Exterior light volumes.** About 4700 instances a frame in the main view. Most effective GPU target left, and hard: earlier work showed they cannot be cut by centre position. A pane depth write early in the frame would reject the part behind glass.
5. **Drop kept names that are outside the room.** In the third build: `PlantStack` off the list, and any allowed name whose sphere starts more than 200 m from the camera is culled (the anchor and the hub excepted, because the gate reads them). Camera distance on purpose: in the hangar the `HangarLobby` is near and stays.
6. **The sun in the concourse.** The sun pair (`7E38A6AA/7CECABDE`) still draws. With mirrored skylights it may contribute nothing; the seal's sun skip can be A/B tested with one line.
7. **The reverse cull.** From the cockpit (docked with the pad raised, or flying inside), veto `Standard_SocialArea` and `SA_*` and show the panes as mirrors. The name list makes this cheap, and it was the original report (the concourse fully rendered while in the ship).
8. **Reflection probes and shadow tiles.** Three `2048` tiles draw identical caster sets each frame. With 21.5M of the remaining 22.4M indices here, refreshing static tiles or probes less often could save more than any of the above, but it needs reverse engineering first, and moving NPCs complicate it.

## Ruled out

- `ruled out: Directional Shadows choosing the glass's pixel shader, because every level of it, 4 down to 1, kept the panes treated, 10,855 to 12,106 draws a 5 s line (Flight 58).`
- `ruled out: the glass's vertex shader changing with Material Quality, because none of the four vertex shaders created at Medium and Low has its outputs, which are the glass pixel shaders' inputs (Flight 58's dump).`
- `ruled out: the cull's withheld lists breaking with Material Quality or Spot Shadows, because the light cones, far glows, particles, smoke and planet families kept matching across the sweep (Flight 58's withheld 30 s lines).`
- `ruled out: H63's deferred context, because the census counted 0 draws on any other context at Flight 57's Orbis (05:01:12 to 05:07:02).`
- `ruled out: H63's turned-away glass, because the glass pair drew 0 times there, so no step had one to turn away (Flight 57).`
- `ruled out: H63's binding shadow, because 1 immediate draw in 64 checked against PSGetShader and VSGetShader, 50,000 to 139,000 a 5 s line, read 0 mismatches (Flight 57).`
- `ruled out: the cull hiding the Orbis's glass, because with F2 off the glass pair still drew 0 times (Flight 57, the 05:02:32 and 05:02:37 lines).`
- `ruled out: the pane feeds (treat, measure) being off at Flight 56's Orbis, because the "concourse backdrop 30s" line, printed there at 20:22:34 and 20:23:04, needs wantCull, which needs the bake done, and about 45,000 withheld draws a 5 s line show the look was off.`
- `ruled out: the record diff changing a verdict, because it only notes what the judges decided; 649c4639..44ce478f changes no return of either judge (Flight 56).`
- `ruled out: H58 (the game culls a frame from the previous frame's camera), because a face's three dwell frames, with the same camera, drew the same far structure (the distance bands equal to 0.1%) and differed only by a 20 to 50 m band (Flight 53).`
- `ruled out: H59 (the game culls from the real head), because face 1's clean frames read the same distance bands (22.0, 32.1, 14.9%) in two bakes with the real head 50 m apart, and its dump shows the whole far structure (Flight 53).`
- `ruled out: H61 (the depth decodes wrong), because no cut texel lay under 20 m in any frame of three bakes, and the cut band had the same shape and share each time it came (13.1% on face 0, 16.2% on face 1) (Flight 53).`
- `ruled out: a hook on Elite's door state for its cost, because the leaf watch costs 1.2 to 3.2 us a tick on average, about 0.02% of a 6.94 ms frame (Flight 46, 115 five-second lines).`
- `ruled out: a door starting to open while the camera is more than 5 m from it on approach, because all five openings began with the camera 3.4 to 4.6 m from the door (Flight 46).`
- `ruled out: writing RV+0x310 before 0x1EA9C0 as the way to move the game's constants, because cb1[275] stayed on the real camera, held or not, while the view slot took the write (Flight 20).`
- `ruled out: shader-pair withholding for the exterior, because the room and the station share VS, PS and stencil (flights 10-26 10:03 and 12:54 lost the walls or NPC parts while the station stayed).`
- `ruled out: greeble names as station identity, because every named part at the glass is concourse kit (log 143601).`
- `ruled out: a fitted hall box as the cull key, because the ring's record has its pivot inside any such box (log 195538); it also needed the short axis clamped, and it could only shrink.`
- `ruled out: Elite+0x1600DD0 (the portal update) as the glass-clear gate, because three flights never saw it arm (logs 10-24).`
- `ruled out: culling light volumes by centre position, because it removes the room's fill (flights 10-26 16:47 to 17:13).`
- `ruled out: reading pool records back from the GPU to cull shell draws, because it pays traversal, submission and staging copies, and crashed once on a reused staging buffer; the store has the same answer earlier.`
- `ruled out: bay windows named by dual-source blend, because the bay pane is 98397963/66B08F89 with One, DestAlpha, 87% of the screen at the glass; no dual-source pair reached the capture's top 24 (log 194826).`
- `ruled out: picking panes automatically from the capture's top draws, because the visor and the GUI layer cover the screen in every capture and, tinted, hide the panes (log 194826).`
- `ruled out: the game's glass drawn opaque with its own material as the mirror, because its metalness is 0 and F0 is 0.04, so it would be nearly black (PS 66B08F89 disassembly; not flown, the visor hid the test).`
- `ruled out: 529D6238 as flares, because it draws last into its own target with no depth view and is what docs\desktop-window.md found Elite drawing into its desktop window; earlier notes here were wrong.`
- `ruled out: a room-coordinate zone as the approach trigger for now, because Flight 1's walk could not place the pane and pane coverage needs no per-station table (not flown; Flight 2 tests coverage).`
- `ruled out: the index count as the test for exterior glass, because about 20 counts pass the family and the interior shares them (Sean's screenshots and log 040008).`
- `ruled out: the opaque pass's stencil bits as a "nothing behind this pane" mask, because a fullscreen draw (#9100, write mask 0x0D, ref 0) clears them before the panes draw (Flight 1's census).`
- `ruled out for now: naming each draw's record through t33 and the instance stream, because it needs both mapped each frame and a proven join of record to pool row, and the depth rule needs neither. Worth returning to only if the depth rule fails.`
- `ruled out: a walk-away latch as the clear's release, because turning round at the bay left the cull off until Sean walked 10 m (Flight 3).`
- `ruled out: 88DCF116 as a ship's laser, because docs\design-flat-temporal-aa-2026-09-23.md names it the first-person laser, a pool draw; it stays in the room's list.`
- `ruled out: EB787F98/3B0B38CD as the pale wedges, because Flight 4 withheld it at 12 draws a frame and the wedges stayed in the screenshot.`
- `ruled out: B12F7A61/42AC0CAC as all of the green haze, because Flight 4 withheld it and the haze stayed.`
- `ruled out: the particle batches (2BAE3742, 3789CA20, E54F2A90) as the green ellipses and pale wedges, because Flight 5 withheld them all and the shapes stayed.`
- `ruled out: the cull as the cause of the washed-out look at the concourse's entrance, because it stays with F2 off (Flight 8).`
- `ruled out: H52, something of Elite's just outside the carrier bar's glass, because with F2 off there the window shows black sky, stars and the planet (Flight 45, 19:47:17).`
- `ruled out: H53, the cleared window drawn wrong by EDVR, because the cleared view (19:46:55) and the F2-off view (19:47:17) are the same picture.`
- `ruled out: the producer and the store judging the door rule's spheres differently (the bridge floor's cause), because their counts of records the slab culled were equal in every 5 s line of Flight 44.`

## Hangar to concourse and back

Sean's goal, 2026-11-06: leave the ship, cross the bay, take the lift, stay culled the whole way, and do the reverse. Sean approved steps 2 and 3 for the next build; steps 1 to 3 are built and not flown, step 4 is undecided.

**Measured (VERIFIED, log `edvr_gfx_20261106_083104`, flat, Steam, small pad, unless named).**

- The lift is a one-frame camera cut of about 1090 m, both ways: six trips, 1089.4 to 1091.4 m (`flat resolve reset event ... camera-cut=1`). No origin shift line comes with it.
- Going to the concourse, `concourse cull: on` comes 1 ms after the cut on all three trips, so the anchor is seen on the cut's tick.
- Coming back, the cull stays on for 5 s after the cut (4.99, 5.00, 5.00 s), which is `kConcAnchorFreshMs`. For those 5 s the concourse rule, which culls `Hanger*` and `LandingPad*`, runs on the hangar side. Nobody has reported it. UNKNOWN whether the lift cabin hides it.
- Flags2 bit 13 (in hangar) lags the cut by 0.13 to 1.07 s. That was the 2026-10 reason not to seal on foot in `docs\optimized-station-2026-10-12.md`: the flag is too late. The cut is not.
- The pad: on the 11-03 build (`edvr_gfx_20261103_044624`, no concourse key) the own pad is in every informative pass on foot, in the hangar and in the concourse. On this branch, with the key on, it is in 0 passes on foot, in the concourse and on the hangar side. INFERRED: the concourse store path returns before the seal's gathers.
- The box for a small pad is 152 x 75 x 258 m. UNKNOWN where the lift door sits against it. After one cut the camera travelled about 84 m in 20 s before it stopped.
- Frame time at the 144 cap (6.94 ms floor), present p50 per 5 s: hangar side on foot, unculled, 6.94 to 9.61 ms. Seated and sealed, 6.94 ms. Seated and unsealed, up to 11.41 ms. Concourse culled at the lift bank, 12 to 13 ms. So on this rig the on-foot seal has at most about 2.7 ms to save, and the cap hides anything under it. VR is unmeasured.

**Plan.**

1. Prove the seal guard first (build after 990a2b44). A seal on foot means more seals after an origin shift, which is where the boarding flicker is suspected.
2. Change rooms on the cut. A camera step over 100 m on foot ends the concourse cull on that tick and forgets the anchor. It removes the 5 s overlap whether or not the rest goes ahead.
3. A log-only walk survey: while on foot on the hangar side, once a second, the camera in the cell's frame, what the seal would veto by name, and frame time by zone. One flight: ship, bay, lift, concourse, and back. It answers whether the lift door, `HangarLobby` and the corridor fall inside the box, and how much the seal would save.
4. The seal on foot, if step 3 justifies it: held while seated, or on foot with Flags2 bit 13 and the camera in the box. It releases on the cut at once, since `kCameraFrames` (10) would leave the concourse empty for 10 frames. The pad gather comes back on the concourse path while the anchor is absent. The withheld families follow, since `feed.sealed` comes from the same flag.
5. Close the arc: default on with `fix.optimized_stations`, propose removing the temporary keys, a VR flight and a second station type.

**The walk survey's lines (log only, armed key, docked; removed when the arc closes).** `walk survey: on` once. Then each second, up to 900 a session: `walk survey: zone Z, ... cam in bay x y z of half a b c m, past the box by M m, pad NAME, pad in pass P, seal H, frame mean T ms over N frames`. Zones: `seated`, `bay`, `past the box`, `concourse`, `no cell`. Every 5 s and on a zone change while unsealed, up to 300 a session: `walk survey names (zone Z, cam ...): would cull A of B records (P%), N names within 300 m; nearest would-cull: ...`. A would-cull name close to the camera in the bay or the lift corridor is what a seal on foot would wrongly remove. `on` with per-second lines and no names line means the report never ran. `would cull 0 of 0` means the armed pass saw no records.

**The lift (Sean, 2026-11-06, Flight 12): can the room be off while the player rides?** Built after Flight 13, not flown (H34). The camera holds still for the ride, the game keeps drawing the room being left, then cuts with the doors still shut and draws the destination behind them too. Measured, flat, small pad: going down, 12.2 ms for about 7 s in the cabin, then 11 to 10.4 ms for about 3 s after the cut, until the doors open. Going up, 8.4 ms for about 7 s, then 15 to 17 ms for about 2.5 s after arriving. The plan: read the doors (H32), tell inside from outside (H33), then while shut with the player inside keep only the cabin and what is right around it, and release on the first door movement. It would also cover the hangar side's cabin, which is most of what a seal on foot would gain there.

**The view through the windows (Sean, 2026-11-06): does the mirror have to replace it?** Tested in Flight 15; the order below is from before that test, and its results change it: see "Flight 15 results". Sean asked whether a texture of the static view, or the lowest LOD with lighting tricks, could keep the look. Flight 1's census says most of what the cull removes is unseen shadow work, though the uncut census is from another day: main view 6.5M indices and 938 draws, sun tiles 17.7M and 4941, the 512 shadows 2.5M and 5. In order: (1) outside in the main view only, out of the shadow passes, with the sun off, since an outside with no casters would otherwise be flooded by sun; (2) a backdrop captured from frames with the cull off, sampled by view direction, which freezes ships and lets near structure slide; (3) the lowest LOD, which cuts indices and not draws, and is global unless it can be set per record. A haze or glare from the flat window shader could cover what is lost in any of them. RE, `analysis\decomp\verify_20261106_concourse_views.txt`, build 334925: no hook serves one view, since a rig is gathered once and each record stored once with a view mask, and passes filter by it (READ). The rig eval, `0x431DC40` in 334925 (`0x431AFE0` was 332841), takes a view only if one of three u16 masks on the rig passes: `+0x3DA` for view flag bit 3, `+0x3DC` for shadow-ish views, else `+0x3D8` (READ); which bit is which pass was not found. LOD is chosen per view and can be forced per record at existing hook sites, and lowers indices rather than draws (INFERRED). ruled out: `DAT_145ea39f8` as a mesh LOD control, because it is read only by terrain tile code. Cascades: no per-pass early-out, and the executor that runs them was not found, so with no outside in them they still cost their setup and the room's casters; stale moment-shadow data at t7 is valid to read. A test with no build, which Flight 14 found restored only RingRoad pieces within 200 m because the far rule beat the name, and which works from the next build: `concourse_keep = WheelSection,Spoke,RingRoad,StrutA,StadiumSegment,HabitatRingA` under `[diag]` in `edvr-flat.ini`, F4 at `off`, F2 to compare at one spot, which costs the big structure with its shadows.

**Not in this plan:** the sun, which Sean wants decoupled and has its own doc. A player with no own pad (arrived by taxi) gets no seal on foot, the same as today.

## 2026-11-17: the maps (issue 203)

Two Steam logs from one flat user on v0.22.3 (`edvr_gfx_20261117_042000`, `edvr_gfx_20261117_045259`), docked in a hangar (a fleet carrier in the second), AA mode DLSS, System Map open (GuiFocus 7). Neither log has an AA-off stretch. The user's screenshot shows "Anti-aliasing: Off" with bodies missing. The map's temporal AA is off in both whatever the setting, so the AA setting was not the variable.

Evidence:
- The second log's NumLock census (#2, frames 9477-9479) shows the game issuing the bodies' draws: VS `9FFA5D5E` (16 draws a frame, in the map selector list), `3530A6FD`, `71DD9863`, `19F70CE8`, `8BD7C37A`, `1F3AD158` and `4D516EF0`. All of them are in `kWithheldVs`. `E79BF326`, the one body family in the census that is not, is the "some bodies render OK" of the issue.
- Both logs print `concourse withheld 30s` over the map window with those families at four and five figures (first log 04:23:02: planetary 2 12537, planetary 3 10746, planet surface vs 12450, body sphere 1782, sun 1791, sun arcs 2363). The sealed hangar's seal (`optimized station: sealed in ... hangar`) was held in both.
- `ConcoursePaneScope::beginBody` withholds by shader with `sealedOn` and nothing in it knows a map is open.
- The map cut-out of fc05ee07 is VR only (`g_systemOpen`), and nothing in the flat map path names these hashes: ruled out as the cause.

Fix: `updateArmed` clears the effects and sealed withholding while Status.json GuiFocus is 6, 7 or 8 (`journalFlatGuiFocus` in flat, `journalGuiFocus` elsewhere). Unknown focus leaves the withholding as it was. Log signature: `concourse cull: a map is open (GuiFocus 7): ...` on opening, and the next `concourse withheld 30s` line has no planetary, body sphere, sun or sun arcs counts for the map's seconds. Fly: dock in a hangar, open the System Map, expect every body.

Open: `optimized station` also reports `sun draws skipped` (a separate path, `fix.optimized_stations`); the sun showed in the screenshot, so it is not touched here. #203's reporter is on 0.22.0 and the same hangar seal applies; not confirmed from their log.
