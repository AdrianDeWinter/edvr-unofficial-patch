# Sealed hangar flicker while boarding (flat), 2026-11-06

## Status

State: DIAGNOSED to ranked hypotheses only. No cause proven. The video matches no EDVR log, so nothing here ties a log line to the event. **A defence is built, not flown (see the 2026-11-06 entry in `docs\optimized-station-2026-10-12.md`):** rig bounds are dropped at every seal and origin shift, and a seal that loses its own pad for 3 passes in a row releases, with a line naming why.

**What the video shows (VERIFIED, frames extracted with ffmpeg, 190 frames, 2560x1440, 26.28 fps, 7.21 s, constant frame interval 35-38 ms).**
- 0-2.1 s: on foot in a hangar at Jameson Memorial (an Orbis), walking to a Diamondback Explorer; blue boarding-volume cylinder, Board panel.
- 2.4-4.3 s: fade to black (mean luma 55 falls to a flat 16 from frame 83 to 111). Cockpit fades in 4.3-5.2 s; fully lit by frame 138.
- **Frames 138-147: normal view through the canopy, lit hangar interior.**
- **Frames 148-152 (5 frames): the whole hangar interior is gone.** The canopy shows a flat dark grey void with white steam and one light cone through the steam. No walls, ceiling, floor, no "Jameson Memorial" sign, no banner. The cockpit, HUD, panels and steam stay, sharp and unchanged.
- **Frame 153 (1 frame): the hangar is back, identical to 147.** Then **frames 154-170 (17 frames): gone again.** Frame 171 onward: hangar back and stable to the end.
- So: 23 consecutive video frames (0.875 s, 5.63-6.50 s), one of them normal. Not a period: a run with one gap. Whole scene or nothing per frame: there is no partial pop-in, no brightness ramp, no colour shift, no ghosting. Mean luma jumps 64 to 71.6 (steam is bright), then back to 63 at frame 171.
- Player is seated, boarding finished, doing nothing. The in-game HUD clock reads 15:51:28.

**Reading of that (INFERRED).** Opaque scene geometry and its lighting vanish; particles (steam), the light cone and the cockpit still draw. That is a cull or a draw suppression of the hangar's records, not a lighting change. The sun skip would darken surfaces, not remove them, and its state is a rare, logged edge.

**Does a log match the video? UNKNOWN, probably no.** The clip name says 16-51-42 and the file was saved 08:59 local. Sean's local time is UTC-7 (DLL link times against log starts), and the HUD clock reads 15:51 (UTC if it is the game clock), so the name is local time in a UTC+1 zone: another person's recording, downloaded about 8 minutes later. 15:51 UTC is 08:51 local, when no Sean session was in a hangar: Steam sessions end 07:36:51, then 08:31:04-08:41:05; Frontier sessions 07:37-07:41, 08:41-08:48, 08:51:38-08:52:09 (VR, no boarding). No log contains "Jameson". Station names are not logged.

**Review, which changes the ranking (INFERRED, no log ties it to the clip).** Three things in the 2026-11-03 entry of `docs\optimized-station-2026-10-12.md` fit the clip closely, and hypothesis 1 now leads clearly. (a) The one frame back: the rig veto refuses an outside rig 30 calls and lets it through once (`kRigRefreshN`), so a bay judged outside vanishes with single frames of normal view between. Elite hiding its own bay has no reason to give one frame back. (b) Length: seal 2 in that log began after a boarding and lasted 1.1 s, against 0.875 s here. (c) Cadence: the report's objection does not hold unless the game ran above about 48 fps, because the 17 frames after the gap are 0.65 s, and 31 frames need at least that long. The clip's constant 26 fps may even be the game's own rate. The likely chain: a boarding re-seal holds a cell that no longer fits, the bay and the pad's rig read as outside and are vetoed, the seal then loses sight of its own pad and releases, which is why it heals itself in about a second.

**Ranked causes, each with its discriminator. Percentages are the report's, before the review above.**
1. **EDVR seal, boarding re-seal culls the hangar for whole frames (the open "pad in the store 1 frame in 31" arc, `docs\optimized-station-2026-10-12.md`, 11-03 and 10-15 entries). INFERRED, about 40%.** Fits: whole-bay, all-or-nothing frames; a boarding; flat; intermittent (the pad-missing signature appeared in some boarding seals of that arc and not in others); 10-15 Sean himself saw "the whole hangar bay flicker on boarding" on flat. Does not fit: 1 gap in 23 frames is not the 1-in-31 cadence if the game ran above 26 fps; and that arc's symptom was never seen. Confirm: a `sealed in ... hangar` line within a second of the flicker, a T3/T4 trace dump, the 30 s line `own pad's record missing from N` with N in the hundreds, and the flicker gone with F8 > Optimized stations off.
2. **Elite's own behaviour with EDVR not involved, about 35%.** The hangar is hidden by the game itself when the ship is ready or the lift starts (an occlusion or visibility switch on the bay). UNKNOWN: no evidence either way. Confirm: window shows `sealed for 0 of N frames` or no seal line, and the same boarding with the setting off still flickers. A vanilla run without EDVR is the clean control.
3. **Flat camera or history path, 10%.** A one-frame flip of the flat camera pick at the on-foot to cockpit change makes the seal's camera rule or the culling use another view. The seal needs the camera outside the box for `kCameraFrames` in a row before it releases, so one bad frame cannot drop the scene by that rule; a wrong view fed to the veto could. Temporal and upscaler history resets blur or ghost; they do not remove geometry for 17 frames and return for 1, so they are not the vanishing itself. Confirm: `camera held through N failed resolutions / jumps over 2 m` nonzero in that window.
4. **Sun skip toggling, under 5%.** Refuted by the frames (geometry gone, not dim) and by the code: `sealed::sunStep` has hysteresis and changes only on a pad r0 edge, undock, hangar change or pad raised; each change logs a line. Confirm: a `sun light inside the hangar` line at the flicker time.

**Ruled out for this video:** see the last section.

**Next step (cheapest).** When it recurs, immediately press the log mark key (NumLock, as in the 11-03 flights) and keep the log; then read `python tools\edvr_log.py --target steam --expect-build HEAD --grep "optimized station"` at that time. A `sealed in` within 1 s, plus trace dumps or `missing from N`, proves hypothesis 1. On a build with the guard, the line to look for is `optimized station: guard:`; the bay vanishing with no such line argues against hypothesis 1. Also run one boarding with `fix.optimized_stations` off and flip it on in F8 (a cheap A/B). Ask Sean whether the clip is his and which build; a clip with a log changes everything above.

## Detail

### Timeline of the boarding in the video (VERIFIED from luma and tiles)
- Frame rate is a constant 26.28 fps, so the game ran at or above that and the recorder captured every nth frame. A one-frame hangar blip is one video frame, which could be several game frames. The 31-frame cadence of the 11-03 arc cannot be tested from this clip.
- The coordinate-origin shift at boarding probably sits inside the black (frames 83-111, 3.2-4.3 s). The re-seal hold (`kTransitionHoldMs`, 2.5 s, `optimized_station_core.h`) would then expire between 5.7 s and 6.8 s, which overlaps the vanish window (5.63-6.50 s). The overlap is consistent with the re-seal engaging near the vanish. It is INFERRED only: the shift time is not visible and the hold's expiry would end an unsealed state, not start one. Observe: in the 11-03 and 10-16 boarding seals the sealed line arrived on the hold's expiry, and the pad was missing then.
- Frames 148-152 and 154-170 are identical in content (same steam shape, moving slowly); only the walls differ.

### What the logs do show (VERIFIED, Steam flat, `edvr_gfx_20261106_083104`, v0.22.0-rc.6-285-g2b487014)
- Boarding at 08:36:04.072 (`aboard yes`), `sealed in small hangar ... seal 1` at 08:36:04.151 (80 ms later; no origin-shift hold line was logged), released at 08:36:04.926 (`own pad left the hangar, r1 1 to 0`), i.e. a 0.8 s seal inside the boarding. The next 30 s windows: pad in 3104 of 3104 passes, missing 0; the window across the seal: missing from 1 pass. No flicker signature (not the 1-in-31).
- This is a different boarding from the video (the hangar in the video has an Orbis and a Diamondback; this log's station and hangar size is small and unnamed). UNKNOWN whether it flickered for Sean.

### The flat versus VR question
- The seal and the sun skip are one code path; the flat difference is the draw thunks (`flatStationSkipsDraw`, `FlatStationSunGate` in `vscreen.cpp`, on main since v14.34) and the camera source (flat has one camera). The 11-03 arc also failed in VR (`edvr_gfx_20261102_191605`), so "flat only" is not supported by logs for the seal. The report that it appears only on flat may simply reflect where Sean flies hangars most.
- Branch note: the seal, the hold (`7681ad45`), the trace and the flat sun gate are on main. The concourse cull on this branch (`claude/station-culling-concourse-4dfec5`, unmerged) is a separate on-foot cull; whether it is involved is UNKNOWN (the video's build is unknown). The seal code blamed above exists on main.

### What the evidence cannot show
- Which build, which settings, which session. No station name in any log.
- Whether the vanish happens with the setting off. No A/B exists.
- The per-frame state of the seal: the 30 s lines cannot show a per-frame flicker (the 10-15 entry says the same).

## Ruled out

- ruled out: the sun skip toggling as the cause of this video, because the frames show geometry absent (walls, floor, sign), not dimmer surfaces, and `sunStep` changes only on logged edges, not within a 23-frame run.
- ruled out: a periodic alternation (every frame or every Nth frame), because the run is 5 gone, 1 back, 17 gone, with no repeat in 7.2 s.
- ruled out: a temporal AA or upscaler history reset as the vanishing itself, because those blur and ghost; whole opaque geometry returns identical for one frame (153) and stays sharp elsewhere.
- ruled out: a log from Sean's 11-06 Steam or Frontier sessions as this clip's log, because no session ran at 07:51 or 08:51 local and the HUD clock and file name put the clip in another time zone.
- ruled out: a cockpit, HUD or panel layer fault, because those layers are unchanged in every frame of the run.
