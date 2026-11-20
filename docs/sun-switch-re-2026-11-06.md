# Sun switch: how Elite lights a frame with the sun (read-only RE, 2026-11-06)

Method: Ghidra headless, `-readOnly` on `ED334925` (nothing written to the analysis repository), byte reads of the 334925 exe, and EDVR's own docs. Build is **334925** (PE stamp 1792704584) unless a line says 332841. Map 332841 to 334925 (`elite_rva.py map`): RenderDirLights 0x5AC9C0 to 0x5ACAC0, deferred setup 0x284CB70 to 0x284F0D0, depth builder 0x5B2C80 to 0x5B2D80, MSM 0x5B3530 to 0x5B3630, planes test 0x4F4E10 to 0x4F4F10, callback 0x288AC40 to 0x288D1C0, VisibilityAnalysis init 0x36D50E0 to 0x36D7D40, StarLightMgr 0x35EB720 to 0x35EE380 (CHANGED). Marks: VERIFIED (read in decompile or bytes), INFERRED, UNKNOWN. The game directory was not written; the shipped `GraphicsConfiguration.xml` was read.

## Status

- **State:** research done, no code changed. The code does **not** give a proven per-frame sun switch. It gives one real list that can be empty, and shows that the sun's cascades hang off a different, config-driven scheduler.
- **Q1, candidates.** (a) The directional loop in `0x5B8FC0` (334925) runs only if call-flag bit 0, a byte at `S+0xEC`, and a non-empty light vector from the provider at `S+0x70` all hold. Otherwise no directional draw and no setup work (VERIFIED). Who writes `S+0xEC`, `S+0xED` and the provider's contents: UNKNOWN. (b) `RenderDirLights` (0x5ACAC0) is not a switch: it is the one-time constructor of the light renderer (VERIFIED; matches the 10-11 flight, one call).
- **What (a) stops:** the full-screen directional draws only (one batch per two lights). It does **not** stop the cascades: their pass objects are made by `0x35EB300` from config and the manager reads no light (VERIFIED for `0x35E3800`). A second sun consumer exists (tiled path, `g_numDirectionalLights`, `g_CullingBuffer_DirectionalLight`, ctor `0x57A830`; global handles `gHasDirectionalLights` etc., ctor `0x57BA90`). Writers UNKNOWN, so emptying one list may leave the other lit.
- **Q3, the 2048 tiles:** the sun's cascades at the Ultra shadow setting. Config: Ultra has 6 partitions and `CellResolution` 2048 in all five profiles. EDVR's own census reading is "a 6144x4096 R16 shadow atlas (six 2048x2048 tiles)", exactly 3x2 cells of 2048 (INFERRED strong: count, size and layout match, no draw-to-tile tie). The 512x512 depth target is the spot and cache shadows (`CacheShadowSize` 512), INFERRED.
- **Q3, a source condition that skips them:** not found. The MSM step (`0x5B3630`) fires only from a callback when the pass index is count minus 1 (VERIFIED), so with no cascade passes it never runs and the map the compute shader binds keeps its last contents (INFERRED: stale, not invalid).
- **Q4:** a GPU sun visibility job exists (`SunSphereVisibilityAnalysis`, `SunVisibilityAnalysisMask`, `SunLensFlareVisibilityAnalysis`, `...SampleDepth`, `...ReadbackResults%u`; VERIFIED strings, creator `0x36D7D40`). Result location, consumer, and whether it runs in stations: UNKNOWN. It likely sees the hull (samples scene depth; INFERRED from names).
- **Q5:** `player_location` and `InternalSpacesManager` have no renderer reader (VERIFIED by xref; the readers are a pad-raise behaviour block). Use Status.json Flags and Flags2.
- **Q6, probes:** BC6H 256x256 cube array, 64 cubes; one face a frame over 6 frames; 91-frame cycle; camera 0.11 to 0.12 m from the head (EDVR logs and docs, not code I read). Breakers: **EDVR's own name culls apply in the probe views and would fake open sky; one face is valid per frame; depth lifetime unknown.**
- **Q7:** a light object has direction at +0x10 (3 floats), colour at +0x20 (3 floats), bytes at +0x30 and +0x31 (decompile `0x5B16A0`; the direction/colour split comes from pointer arithmetic, re-check by disassembly before use). Cheapest CPU source for EDVR: the constant buffer of the sun draw. The light object's per-frame address: UNKNOWN.
- **Ruled out:** see the last section.
- **Why the sun gets in: still not established.** A hypothesis added on review, that the cascades keep only back faces so a hollow shell shades nothing inside it, is refuted: see Ruled out. Depth bias is 0 and depth clip on in every tile draw. The census was taken with F2 on, so the hull itself was not among them.
- **Reviewer's checks:** `S+0xEC` read in `analysis\decomp\decomp_05B8FC0_334925.txt` line 333, as stated. The other decompiles are beside it (`0x5B16A0`, `0x5ACAC0`, `0x5B3630`, `0x5B1420`, `0x35E3800`, `0x35EB300`), and the queries are `analysis\ghidra_scripts\SunQuery.java` and `SunDispScan.java`, filed in elite-analysis 288c635. The tiles at six viewport origins, 3 by 2 at 2048 spacing, in the 07:01 and 07:02 censuses of log `065604`: about 2,100 draws each in three of them and about 400 in the others.
- **Next, in order:** (1) find the executor that runs the cascade pass records each frame (vtables `0x54CB078` and `0x506B1C0` neighbours) and its per-pass early-out; "no cascades" would live there. (2) Find the writer of `S+0xEC` by walking the creator of `S` (`*(*(setup+0x20)+0x18)` in `0x284F0D0`, call at 0x28506B9). (3) Trace the `0x36E02B0` job's result readback. (4) Only then a flight, and only a counting one.

## Q1. Is there a real switch

**The light renderer object (VERIFIED).** `0x5ACAC0` (3492 B) is called once from the setup state machine `0x284F0D0` at 0x28506B9, which allocates 0x420 bytes and stores the result at `[[pass+0x28]+0xC0]`. It caches handles: `c_numDirectionalLights` +0x2F8, `c_directionalLightColour` +0x300, `c_directionalLightDirection` +0x308, `c_directionalLightShadowCollectMask` +0x310, `c_directionalShadowActive` +0x318, `c_globalLightScale` +0x208, technique ids `RenderDirectionalLights` +0x3D4, and the spot, point, sphere, tube, rectangle, ellipse, SSAO, IBL and envmap techniques. It draws nothing. `0x284F0D0` is a step machine on `[this+0x1FA]` that also parses `GraphicsConfigurationOverride.xml`: setup, not per frame.

**The per-frame pass (VERIFIED).** The object's vtable (0x50BC390, 0x50BC398) holds `0x5B0CA0` and `0x5B8FC0` (6209 B). In `0x5B8FC0`:
- `S = *(this+0x40)`, the frame's lighting data. The directional block runs only if `(callflags & 1)` and `S+0xEC != 0`; otherwise control continues past it to the code that follows (not traced; local lights and IBL are separate techniques).
- It loops the entries at `S+0x90` (count) and `S+0x98` (8-byte records: enabled byte, stencil byte, a float). For each enabled entry it asks the provider at `S+0x70`, through virtual slot 0x18, for a vector of light pointers.
- It takes at most two lights per batch. A light is taken if `(S+0xED && light+0x30) || light+0x31` (meaning of the two bytes: INFERRED, shadowed and enabled). Each batch calls `0x5B16A0` then a resolve helper. A zero-light vector issues no batch.
- A GPU marker `DirectionalLights` is emitted per batch (string 0x50BC468, referenced at 0x5B960F).

**`0x5B16A0` (590 B, VERIFIED).** Per batch: `c_directionalShadowActive` = 1 if the shadow flag is set and the light's shadow slot is 0 to 2, else 0; `c_directionalLightTransparency` from an argument; maps the colour, direction and collect-mask buffers; per light writes direction (light+0x10..0x18), colour (light+0x20..0x28) and a collect-mask vector picked by shadow slot 0, 1, 2 from three constants (zero when unshadowed); then `c_numDirectionalLights` = batch size, and the technique at +0x3D4. So **three shadowed suns at most**, one channel each.

| Candidate | Reader | Writer | Effect | Status |
|---|---|---|---|---|
| `S+0xEC` byte | `0x5B8FC0` directional block | UNKNOWN | no directional pass; IBL and local lights continue | reader VERIFIED, writer UNKNOWN |
| provider vector empty (`S+0x70`, slot 0x18) | `0x5B8FC0` | UNKNOWN (filing in the light manager: `m_directionalLights`, `m_starLights`, `m_dummyStarLights`, named in `0x35EE380`) | same | reader VERIFIED, writer UNKNOWN |
| `c_numDirectionalLights` | lighting shader | `0x5B16A0` per batch | set from batch size, cannot be forced without a hook | VERIFIED |
| `c_globalLightScale` | shader | handle cached in the constructor (+0x208); no writer in `0x5B8FC0` or `0x5B16A0` | | writer UNKNOWN |
| tiled sun: `g_numDirectionalLights`, `g_CullingBuffer_DirectionalLight` | compute culling object, ctor `0x57A830` | UNKNOWN | sun for tiled and forward shading | handles VERIFIED, rest UNKNOWN |
| `gHasDirectionalLights`, `gNumDirectionalLights`, `gDirectionalLightDir`, `gDirectionalLightColour` | forward shaders; handles in ctor `0x57BA90` at +0x98 to +0xB0 | UNKNOWN | global constants | handles VERIFIED, rest UNKNOWN |

Dependence: these are different consumers; nothing shows one list feeding all. The older "collection+0x48 sun bits" are shadow admission, not lighting.

## Q2. What the engine does with no sun

- VERIFIED: with `S+0xEC` clear or an empty provider vector the directional pass issues no draw, no constant upload and no technique. The shader is never invoked, so "count is zero" is never read by it. IBL and envmap are separate technique ids in the same object (INFERRED to continue, since they sit outside the guarded block).
- VERIFIED strings, no logic found: `HyperspaceTunnelComponent`, `HyperspaceTunnel_Dark_tunnel`, `StarLightObjectColourGrading` (registry `0x35D2AD0`), `StarLightDirectionGravityVector` (`0x426BCF0`), `gAtmosphereDirectionalLightColour` (atmosphere object `0x3639000`). None gave a lighting-off path. Witchspace, eclipse and menu: UNKNOWN.
- `0x35EE380` (the manager in the older notes) names every sub-list while it walks and releases them, and tests the byte at 0x5E8B9F7 first. It reads as a clear (INFERRED), not a per-frame filter. The older "byte group" gate has no writer that I found.
- What the engine leaves behind when the list empties, for the tiled and global-constant consumers: UNKNOWN.

## Q3. The 2048 tiles and the sun's cascades

- VERIFIED (config, `GraphicsConfiguration.xml` lines 562 to 684): `DirectionalShadows_Ultra` has 6 `EndDistance` entries and `CellResolution` 2048 in `Profile_Space_Cockpit` (5, 17.7, 80.8, 355.5, 1530.6, 6500), `Space_Humanoid` (3 to 5500), `Space_AsteroidField`, `Planet_Cockpit`, `Planet_Humanoid`. Low is 1024, High 1536. `SpotShadows`: `CacheShadowSize` 512, atlas 6144 (Medium, High) or 8192 (Ultra). `Envmap`: `TextureSize` 256, `NumMips` 8.
- VERIFIED (EDVR doc `design-flat-temporal-aa-2026-09-23.md` near line 8010): a census read "11,726 depth-only draws into a 6144x4096 R16 shadow atlas (six 2048x2048 tiles)". Three columns by two rows of 2048 equals Ultra's six cells. So the tiles are the sun's cascades on Sean's setting (INFERRED strong). On Low to High they would be 1024 or 1536 tiles, so the saving depends on the setting.
- Scheduler (VERIFIED chain): `0x35E3800` (per-frame update of the cascade manager; reads config object fields +0x2E8, +0x2EC, +0x2F4 and rebuilds only on change) calls `0x35EB300`, which allocates the cascade-set objects (0xC0 bytes, vtable 0x54CB078, a 0x300-byte settings block each) and registers each through `0x5A8870`. That appends a 0xA0-byte pass record and builds it via `0x5A5AB0` and `0x5AE2E0`, which calls the depth builder `0x5B2D80`. `0x35EAC20` sizes the two sets from config (set 1 is fixed 0x300 by 0x100). `0x35FFCC0` only scales fade distances over time. None of these reads a light, a "has sun" flag or a caster count.
- MSM: `0x5B1420` forwards to `0x5B3630` only when the pass index is the last (VERIFIED). `0x5B3630` (re)creates `DirectionalShadows.MSM` through `0x1404F12F0` with engine format id 0x29 when size or format differs, then runs the moment step.
- A source condition that stops the tiles: **not found**. I did not find the executor that runs the pass records each frame, or an early-out there. I cannot say a no-sun frame stops them.
- The compute at CS slot 0 was not examined. INFERRED from the callback structure: a missing cascade pass leaves the previous moment map bound, which is valid data.

## Q4. Does the engine know the sun is occluded

- VERIFIED: `0x36D7D40` is the initialiser of an object class (0x800 B, ctor `0x36D5110`, vtable 0x54FEF10), called from `0x36EC6D0`, the same job that builds the `gSystemScanResolve*` handles. It creates effects `SunSphereVisibilityAnalysis`, `SunVisibilityAnalysisMask`, `SunLensFlareVisibilityAnalysis` and `StellarBillboard_Scan`, and registers the marker `VisibilityAnalysis`. Nearby strings: `VisibilityAnalysisDepthWrite`, `...DepthTexture`, `...SampleDepth`, `...CalcResult`, `...ReadbackResults%u`, `VisibilityAnalysis_DepthTarget`.
- INFERRED: it samples scene depth at the sun's screen disc on the GPU and reads the result back a few frames late (the `%u`), so it sees the hull as an occluder. Slot 7 of the class (`0x36E02B0`, 2651 B) early-outs on two bytes of its last argument (+0x129, +0x12A) and on a camera-kind field equal to 2; it is the likely execute method.
- UNKNOWN: the result's CPU address, any consumer, and whether it runs in a station. EDVR's sun glare path (`docs/sun-glare.md`) is a separate instanced draw. `0EE43D81` and `CFCA8FFC` as the "star depth pre-pass": shader hashes only, no code tie made.

## Q5. Does the engine know the player is inside

- VERIFIED strings: `InternalSpacesManager` (0x5216438) and `StationInteriorComponent` (0x5216D98) are referenced only by `0x1B0E400` (the factory; 332841 0x1B0BBC0). `player_location` and its values are registered in `0x41DC7D0`; the values are interned ids in .data slots (`inside_station` 0x6034524, `inside_docking_bay` 0x6034538, `player_location` key 0x60345A8, `OnFootLocation` 0x60345D8).
- VERIFIED: the only code referencing those slots is in `0x41E0040`, `0x41E45D0`, `0x41E4B40` and `0x41E5190`; `0x41E45D0` contains `RaisePad` and `Short raise pad`. No renderer function references them (INFERRED: a pad behaviour block).
- So I found no "inside" input to the renderer. Cheap read for EDVR: Status.json Flags (Docked bit 0, unverified at surface ports) and Flags2 `0x00014009` (on foot, in station, social space), as the concourse arc already does, plus the store's record names.
- Why the hull does not shadow the interior: not established. The Ultra cascades reach 6.5 km (VERIFIED, config), so range does not exclude it. The admission gates in the 332841 notes were not re-read for 334925.

## Q6. The reflection probes (trying to break the depth test)

Facts from EDVR logs and docs, not code I read: a BC6H 256x256 cube array of 64 cubes (`docs/cockpit-glass-legacy-port.md` C6); the forward face is written last of six, one face a frame, camera 0.112 to 0.122 m from the head, bursts 12 of 91 frames, 1.006 s cycle at 90 Hz (`docs/sky-flicker-2026-10-13.md`; `design-flat-camera-integration.md`: cycle 91 in VR); the mini-render is 256x256 deferred, about 400 draws (INFERRED there). Config `Envmap` is 256, 8 mips (VERIFIED). A concourse census saw one hull-like mesh drawn once in the main view, 3 times in the tiles and 4 times in the probes (`concourse-cull-2026-11-05.md` line 167), so probes draw station records. The names `LocationLightProbeManager`, `LocationEnvmapManager`, `LightProbeComponent`, `EnvMapTextureStore` are registered in `0x35D2AD0` (VERIFIED strings); how they relate to the dynamic faces is UNKNOWN.

What breaks "count open-sky pixels in the probe depth":
1. **EDVR's own culls run in the probe views.** The concourse arc culls by name in every view (shadow tiles -60%, probes -20%, same doc) and the sealed-hangar withhold does likewise. A culled hull leaves far depth, which reads as open sky, so the test would see its own cull. Strongest breaker.
2. **One face is valid per frame.** Six faces take six frames and a full refresh is about 91 frames, so the signal is about a second old and mixes faces from different instants.
3. **Probe draw distance and LOD.** The probe is a reduced render (about 400 draws against thousands). Far hull may drop out by range or LOD and read as sky. UNKNOWN, plausible.
4. **Depth lifetime.** Targets come from pooled, refcounted resources (the `FUN_1404FA400` pattern in the constructors). Whether the probe depth survives its pass or is aliased by a later one: UNKNOWN. The doc mentions probe targets of 256, 512, 1024 and 2048x1024, so several sizes exist.
5. **Sky is not only the sun.** Planet, atmosphere, a gap in the hull and an open hangar mouth all give far depth. The count measures openness, not sun line of sight.
6. **Stations may use baked location probes** (names above). The 4-draw count says dynamic faces ran at that station at least.

## Q7. Sun direction and colour on the CPU

- VERIFIED (decompile): a light object has direction float3 at +0x10, colour float3 at +0x20, bytes at +0x30 and +0x31; the pass reads them each frame from a vector the provider returns (`0x5B8FC0`).
- UNKNOWN: the provider's address, and how to reach it per frame without a hook. `0x5B8FC0` is in the vtable of a long-lived object, so a hook there sees `this` and `S` directly. That is a code hook, not a read.
- Cheapest read without a new hook (INFERRED): the constant buffer of the sun draw. `c_directionalLightColour` and `c_directionalLightDirection` live in the shader's buffers and EDVR's cb-watch already reads constant buffers. `gAtmosphereDirectionalLightColour` (`0x3639000`) is a second copy for the atmosphere renderer.

## Checked and came up empty

- A writer of `S+0xEC`, by scanning for stores to displacements 0xE8, 0xEC, 0xED and 0x70 together: the hits were a constructor and copy constructors of an unrelated struct (`0x4503D20`, `0x24D1390`, `0x1D03A90`).
- A direct caller of `0x5B8FC0`, `0x5B0CA0` or `0x5B1420`: none; only vtables (0x50BC398, 0x506B1C0). The executor is not found.
- A "has sun" or light-count read inside any cascade function (`0x35E3800`, `0x35EB300`, `0x35EAC20`, `0x35FFCC0`, `0x5A8870`, `0x5A5AB0`, `0x5AE2E0`).
- A consumer or CPU address for the visibility-analysis result.
- A renderer reader of `player_location`.

## Ruled out

- ruled out: `RenderDirLights` (0x5ACAC0) as a per-frame switch or draw, because it is the light renderer's constructor, called once from the setup step machine, and issues no draw.
- ruled out: the light list as the controller of the cascades, because `0x35E3800`, `0x35EB300` and `0x35EAC20` read only config fields and make the pass objects regardless of lights.
- ruled out: `0x36D7D40` as per-frame visibility work, because it is an initialiser that creates the effects once; `0x36E02B0` is the per-frame candidate.
- ruled out: `player_location` and the docking-bay enum as renderer inputs, because their id slots are read only by a pad-raise behaviour block.
- ruled out: `0x35EE380` as a per-frame light filter, because it walks and releases every sub-list and reads as a clear.
- ruled out: reading probe depth after EDVR's own name cull as a sun-visibility test, because the cull turns hull into far depth in that view.
- ruled out: the sun's cascades keeping only back faces as why a hollow hull does not shade its interior, because every draw at 2048x2048 in log `092241`, 27.3M indices, has cull mode none (`rs=Nw/0/-0.00/1`), against back-face culling for most of the main view. Taken with F2 on, so a hull with a state of its own is not excluded, only unlikely.
