# Memory growth: "Elite leaks memory, especially in VR"

## Status

- **State (2026-11-20): the leak question is static analysis only, no flight;
  the 2026-11-19 overflow arc (below) has flown and ships a fix.** Nothing that
  grows at gigabyte scale was found in Elite's VR code (build 334473, OpenVR
  and LibOVR paths) or in EDVR's own source. Three small leaks in Elite are
  confirmed in code (below); none is large enough to explain the reports.
- **Open hypotheses:**
  - H1: the growth is in the VR runtime's own process (`OVRServer_x64` after
    Meta Link v74, or `vrserver`), which a player reading Task Manager's total
    sees as Elite. This is the best match for the one VR-specific report.
  - H2: Elite's texture streaming or map screens hold memory (forum anecdote:
    galaxy and system map textures are never freed).
  - H3: Elite's CPU heaps grow regardless of VR.
  - H4: EDVR's bounded retention (engine-motion pools and shaders) is larger
    in practice than the source suggests.
- **Evidence gap:** no log in hand covers a multi-hour session. Since
  2026-10-22 the code writes a `mem:` line (below, "The `mem:` line") that
  covers process memory and survives the gfx log's 16 MB cap; it has not flown.
  The `vram:` line still stops at the cap.
- **Next:** one VR flight of two hours or more with the `mem:` line, that
  includes map screens, jumps, and taking the headset off and on. Check first
  that the armed `mem:` line is in the log and says no group is unreadable; the
  `eheap` group is a first reading, to be compared with `commit_mb`.
- **2026-11-19 overflow arc** (entries at the end): VRAM grows until the OS
  demotes into shared memory and the frame rate collapses; the main menu
  recovers. The growth is Elite's own streaming (flight `20261119_215431`), and
  its budget is 0.85 x the adapter's `DedicatedVideoMemory`, never the OS
  budget (Ghidra, build 334925), so EDVR's 2-3.5 GB tips the card over.
  - Fix: `advanced.game_vram` (`auto` shipped, `off`, or MB; hidden) changes
    `DedicatedVideoMemory` for the game exe's GetDesc calls only, with no game
    address (`src/common/game_vram*.h`, `src/d3d11/game_vram_hook.cpp`).
  - Flown: 8192 levelled Elite at 0.85 x 8192 and it freed in flight, with
    textures unchanged. `auto` learns its reserve per setup from DXGI alone,
    starting from safe defaults (flat 1536/2048/3584, VR 3072/3584/4608 MB
    for off/TAA/upscaler), and keeps it in
    `%LOCALAPPDATA%\EDVR\game_vram.txt`. A 30 min run held Elite at its
    target and tightened 3584 -> 3963 MB after a near miss from the other
    share.
  - Any setup, flown before or not (VR, Wine/Proton, another vendor or
    card), starts from the default for its profile and AA mode and learns
    from there. Where the OS reports no budget, `auto` takes 95% of the
    card. The log's `game vram:` lines name the setup, the reserve used and
    each learning step, so a report from a new setup is read from its log.
  - Under Wine, a budget at or above the card size is treated as no budget:
    95% of the card, the default reserve, nothing learned ("Wine and
    Proton" at the end). Known false positive: AMD on RADV without
    Resizable BAR; its log says "is the whole card".
- Ruled out: see "Ruled out" at the end.

Evidence files (scripts, query logs, decompiles) are under
`analysis/leak-2026-10-22/`, which is local and not in the repository.

## What players report

Research of the Frontier tracker, forums and patch notes, 2021-2026:

- No patch note from 2023 to 2026 mentions a memory leak. Odyssey's launch
  notes fixed a memory stomp and a video-memory streaming budget; Update 17
  reduced Titan audio memory.
- The only VR-specific report is tracker
  [74677](https://issues.frontier.co.uk/issue-detail/74677) (Quest 2 on Link,
  April 2025): system RAM rises 1-2% a minute, even at the main menu, stops
  when the headset is taken off and resumes when it is worn. It expired
  unconfirmed and does not say which process grew. Meta's Link v74 (March
  2025) was reported leaking RAM through `OVRServer_x64` in DCS and MSFS in the
  same weeks.
- The only acknowledged memory issue is
  [82038](https://issues.frontierstore.net/issue-detail/82038): a 22-32 GB
  runaway spike when a second wingmate drops into a Thargoid signal. It is a
  spike, not slow growth, and not VR.
- Nobody has traced the growth to a DLL or subsystem.

## Static findings in Elite (build 334473)

RVAs against image base `0x140000000`.

| Finding | Where | Size | Confidence |
|---|---|---|---|
| The Wwise controller-enumeration thread (`AkGameControllerEnum`) leaks its thread handle on every device removal, and on an arrival that does not match a known device | create `0x4d04c90`, thread proc `0x4d044e0` zeroes the handle without `CloseHandle` | one kernel thread object per event; not process heap | confirmed in code |
| The 1x1 RGBA8 texture made for `SetSkyboxOverride` is never released | `0x4e5a00` | one tiny texture per loading transition (three in a logged session) | confirmed in code |
| A failure partway through LibOVR swap chain creation skips the destroy and keeps the chains already made | `0x4e5360`, guard `+0x32c` | once, on a failure path | confirmed in code; inert under EDVR, which keeps Elite off LibOVR |
| The engine's thread creator discards its handle | `0x543f90`, five callers | probably long-lived threads | plausible |

What Elite uses from OpenVR: `IVRSystem_012`, `IVRCompositor_014`,
`IVRChaperone_003`, `IVRExtendedDisplay_001` and `IVROverlay_011` (debug
keyboard only). It has no render model, mirror texture, input, settings or
texture-load interface, so those classic acquire/release leaks cannot occur.

## Elite's own memory accounting

The retail exe keeps live GPU resource accounting, a possible source for an
in-process memory line. All offsets are build-bound.

- Device: `qword [0x5f1dca8]` + `0x18` = device D (null before the device
  exists).
- D+`0x1ee0`: 12 u64 live byte counters by type, in this order:
  RenderTarget/UAV, DepthTarget/UAV, Texture/UAV, Immutable, Texture, Vertex,
  Index, Buffer, Shader, Descriptor, CommandList, Unknown. Updated on
  allocation (`0x51fa70`) and free (`0x51fbf0`) under the critical section at
  D+`0x1ea0`.
- D+`0x1f40`: the high-water mark of their sum. D+`0x1f48`: a second set of 12
  that excludes resources with flag `0x40000`.
- D+`0x1ff8`: GPU heap count; D+`0x2000`: heap pointers. A heap has used bytes
  at `+0xb0`, its high water at `+0xb8`, capacity at `+0x4c8`, largest free
  block at `+0x4e8` and type at `+0x510`.
- The game's own report formats this table in KB (`0x519160`), and the crash
  log path (`0x520e90`) reaches it.

CPU side, plausible but not checked at run time: the heap manager at
`[0x5f1de58]` (static object `0x5f28120`) lists heaps at `+0x1400`, 16 bytes
each, with the count at `+0x1510`. Bytes obtained from the OS are at heap
`+0x60`. The main allocator's statistics block is at `*(heap+0x1a8)`, with
current bytes at `+0x10`, the peak at `+0x18` and the live count at `+0x20`.
Heap names are discarded in retail, so there are no categories.
`fHeapVirtualStore` always reports 0.

## EDVR's own retention (source audit)

No unbounded growth was found. EDVR never hooks `Release` and never wraps D3D
objects, so it can keep a game object alive only through a held reference in a
cache, and every cache found is capped. The largest bounded holds:

- `engine_velocity.cpp:3098-3123`: up to 16 of the game's stride-336 buffers,
  plus up to 32 copy pools, until engine motion stands down. Stale buffers are
  pinned only if the game recreates the pool.
- `engine_velocity.cpp:134-144`: up to 512 vertex and 512 pixel shaders with
  bytecode copies.
- `light_grid_cache_core.h`: 32 slots of the game's cluster tables, up to about
  32 MB of game heap plus GPU buffers.

## Existing logs

The `vram:` lines (local GPU memory) were read from 81 game-directory and
`analysis/` logs and seven logs players sent. Only four span 30 minutes or
more:

- Two VR sessions: `edvr_gfx_20261016_203043` (72 min) and
  `edvr_gfx_20261021_092731` (Quest 3 on Link, 47 min). Neither climbs. Each
  peaks early (9.3 GB at minute 13; 20.6 GB, 94% of budget, at minute 6), then
  holds about 5.5 GB and 6.0 GB.
- Flat sessions: `20261019_082411` rises in steps, about 620 MB an hour over 57
  minutes, before the log cap stops it. `20261021_123140` saw-tooths between 3
  and 9 GB, and its drops coincide with drops in the OS budget.

## Ruled out

- ruled out (static): leaks of OpenVR mirror textures, render models, overlays
  or loaded textures in Elite, because the exe requests none of those
  interfaces.
- ruled out (static): LibOVR per-frame buffer references, because Elite fetches
  each swap chain buffer once at creation and releases every one in its destroy
  (`0x4e5e50`). The per-frame submit (`0x4e14b0`) only copies and commits.
- ruled out (static): allocation on headset visibility changes, because the
  session status handler (`0x4e5150`) only sets flags.
- ruled out (static): OpenVR Submit creating objects each frame, because Submit
  (`0x4e19c0`) takes reference-counted holders with balanced counts.
- ruled out (static): growth in the VR error logs, because both are deques
  capped at 20 that free their strings.
- ruled out (static): leaks on audio default-device changes
  (`IMMNotificationClient`, `0x4a2bac0`), in DirectInput hot-plug
  re-enumeration (de-duplicated, at most 16 joysticks) and in SetupDi
  enumeration.
- ruled out (source): a per-frame, per-draw or per-event COM reference leak in
  EDVR, from an audit of about 1300 getter, AddRef and Attach sites.

## The `mem:` line (built 2026-10-22, not flown)

Code: `src/common/mem_watch.h` (cadence, text, plausibility),
`src/d3d11/mem_tick.cpp` (one thread of its own, started at the first Present;
the OS and Elite reads), `src/common/log_reserve.h` and `Log::noteReserved`
(the line survives the size cap, from a fixed 1 MiB reserve a session; after it
the log says so once). Rig: `tools/mem_watch_test`. A line every 60 s, every 5
s while machine commit is at or over 90% of its limit, and one `armed` line at
the first sample that says what each group is and why any is missing. Whole MB,
rounded.

- `commit_mb` (private bytes), `commit_peak_mb`, `ws_mb`, `handles`, `threads`.
- `sys_commit_mb`, `sys_commit_limit_mb`, `sys_avail_mb`: the machine.
- `local_used_mb`, `local_budget_mb`: the `vram:` local group again.
- `egpu_*`: Elite's twelve GPU counters by kind (`rt ds texuav imm tex vb ib
  buf shader desc cmd unk`), their sum and its high water, and the GPU heaps.
  Read only on build 334473 (PE identity plus eighteen byte pins). A heap whose
  type (`+0x510`) is 3 counts descriptors, not bytes (the game's report prints
  it so), so those are counted in `egpu_desc_heaps` and left out of
  `egpu_heap_used_mb` and `egpu_heap_cap_mb`.
- `eheap_*`: the CPU heap manager. `eheap_os_mb` sums heap `+0x60` over the
  table; `eheap_main_mb` and `eheap_main_peak_mb` are the default heap's
  allocator statistics (current and peak bytes, `stats+0x10` and `+0x18`).
  `eheap_main_allocs` is `stats+0x20`: it is incremented by every allocation
  path and decremented by none that was found, so it counts allocations made,
  not allocations live. `+0x60` is added to only on the C-runtime path of the
  segment allocator (`0x553480`); a heap that takes its segments with
  `VirtualAlloc` (flag `0x40000`) keeps it at 0.
- `ovrserver_*`, `vrserver_*`, `vrcompositor_*`, `vdstreamer_*`: commit and
  handles, summed over instances, when running. No other process is named.

Reading it: a climb in `commit_mb` with flat `egpu_mb` and `eheap_main_mb`
points at memory neither accounts for (`eheap_os_mb` against `commit_mb` says
how much of commit the heap manager covers); a climb in one `egpu_` kind is H2
and names the kind; flat Elite figures with `sys_commit_mb` climbing and a
runtime's `_commit_mb` climbing is H1. `egpu=implausible` or
`eheap=implausible` means the values failed their checks and are not written;
`egpu=unread` means the game had no device yet at that sample.

## Proposed instrument (what the line was specified from)

A `mem:` line on the same clock as `vram:`, in a budget that survives the gfx
log cap, carrying:

- process private bytes, commit (`PagefileUsage`) and handle count;
- Elite's 12 live GPU byte counters and their high water, read from the device
  above;
- after a first check of their values, the CPU allocator's current bytes.

A climb in private bytes with flat GPU counters points at H3. A climb in one
GPU type points at H2, and names the type. If Elite's own numbers hold flat
while the machine runs out of memory, the growth is in another process (H1);
the next step would then be to sample `OVRServer_x64` and `vrserver` by name.

## 2026-11-19: GPU memory overflowing into shared memory

### The report

Several players, the reporter of this entry among them, see the same pattern:

- Elite's GPU memory climbs over a session.
- Task Manager then shows it spilling into "Shared GPU memory", often while the
  card still shows more free dedicated memory than the spill.
- Frame rate falls further as the spill grows, to a fraction of the start.
- Quitting to the main menu drops usage. A few seconds later the spill moves
  back to VRAM and the frame rate recovers. Some players have to close the
  game.

The reports date it to v0.20.0 or earlier. It happens in several anti-aliasing
modes and is milder or absent on the stock game. Textures on low slows it but
does not stop it, even on 12 and 16 GB cards. No flight log of an episode is in
hand yet.

### Why it spills while VRAM looks free

The limit is the per-process budget, not physical VRAM. DXGI's
`QueryVideoMemoryInfo` budget (the `local_budget_mb` of the `vram:` line) is
below the card's size, and smaller still when other processes, a VR compositor
among them, hold VRAM. On the reporter's Arc B580 (12 GB) it reads 11,350 MB.
Once the process goes over it, the OS video memory manager demotes allocations
to system memory until the process is back under, and it leaves slack when it
does. Task Manager's per-card figure then shows free dedicated memory beside
the shared figure. "The spill is smaller than the free VRAM" is therefore the
expected shape of budget eviction, not evidence of a placement bug.

Under D3D11 the OS manages residency for each command buffer. Once a frame's
working set no longer fits the budget, allocations are paged in and out every
frame, which is the frame-rate collapse. Demoted allocations are promoted back
only once usage falls clearly under the budget, as it does at the main menu.

The one placement case that does not fit is memory landing in non-local while
local usage is well under budget. That would mean dynamic or staging resources,
not eviction. Its signature is `nonlocal_used_mb` rising with `local_pct` well
below 100 and no `over_budget` line.

So something has to grow until it reaches the budget. The process's usage
splits as:

    local + nonlocal = egpu (Elite's live resources)
                     + EDVR's own resources
                     + upscaler SDK and driver internals
                     + Elite resources Elite has freed but EDVR still
                       references

`egpu_mb` is decremented when Elite destroys a resource, whatever the D3D
reference count is. A game resource that EDVR keeps alive therefore shows up as
growth in the gap (usage minus `egpu_mb`), not in `egpu_mb`.

### Static audit of event- and cache-driven growth (2026-11-19)

This complements the per-frame audit under "Ruled out". It covered every EDVR
creation of GPU memory and every container that holds a game object.
`native_on_foot.cpp`, `flat_runtime.cpp`, `vscreen.cpp` and `ui_layer.cpp` were
sampled by search, not read whole.

No unbounded path was found. Every recreate path releases the old object first:

- the DLSS feature (`dlaa.cpp`, at most 3 slots), the FSR3 context, the XeSS
  context and the DittoSR history all on a size or key change;
- the temporal pass's per-eye set;
- the flat mono resolve;
- the UI layer and UI depth;
- the OpenXR eye copies.

Bounded holds that are large, or that pin game objects:

- Engine motion, flat: `g_flatMarkerPlanes` (`engine_velocity.cpp`) holds up to
  4 render-size RGBA32F planes, each with a reference to a game depth texture.
  That is about 530 MB plus 4 game depth buffers at 3840x2160. A plane is
  evicted only when all 4 are in use, and the set is freed only when engine
  motion stands down or pauses (`clearLocked`).
- Engine motion: up to 16 of the game's stride-336 buffers, never evicted (a
  full table stops tracking), and up to 32 copy pools.
- `depth_probe`: up to 32 game depth textures, evicted after 120 unseen frames.
- `backdrop_fix`: 4 stills of about 33 MB each, never evicted.
- The light grid: about 32 states, 20-30 MB.
- `desktop_window`: 6 eye-texture views.

SDK churn: each key change destroys and recreates an upscaler's feature or
context, the old one first. Whether the driver or SDK hands that memory back
promptly is not known.

No cache is cleared on a main-menu transition. The recovery at the menu is
therefore Elite tearing down its own resources, or engine motion standing down
as a side effect.

### What the existing logs say

These are the reporter's 18 flat logs: Arc B580, v0.22.2 to v0.22.3, each 1 to
58 minutes long. None reproduces the effect:

- local peaks at 4.5 GB against an 11.35 GB budget;
- non-local stays at or under 89 MB;
- `egpu_mb` is flat once a session is loaded.

The gap (local minus `egpu_mb`) is 815 MB at load. In flight it rises to about
1.6-1.8 GB, and some sessions fall back to about 0.93 GB. That gap is EDVR, the
upscaler and the driver. It is also headroom that stock does not spend, so on a
card that is nearly full it brings the budget crossing forward by that amount,
with or without a leak.

The `egpu` pins still read on the game build these logs came from.

### Instrument gap

The `vram:` line, which carries `nonlocal_used_mb` and the budget crossings,
stops at the gfx log's 16 MB cap. The `mem:` line survives the cap (from its
reserve) but has no non-local figure. In the multi-hour session this effect
needs, the spill itself is therefore unlogged after the cap.

Proposed, not built:

- `nonlocal_used_mb` and `local_budget_mb` crossings in the `mem:` line;
- a live total of EDVR's own GPU bytes, to split the gap into EDVR and
  everything else.

### Hypotheses and their signatures

| | Hypothesis | Signature (`mem:` and `vram:`) |
|---|---|---|
| V1 | Elite's own growth: streaming, or a render-target pool kept per size that grows with resolution changes and is freed at the menu. Base game; EDVR's overhead only brings the crossing forward | `egpu_mb` climbs, and the gap is flat. A per-size pool shows as `egpu_rt_mb` stepping at mode, menu or resolution changes |
| V2 | EDVR keeps game resources alive after Elite frees them | `egpu_mb` flat or saw-toothed while the gap climbs, and the gap drops at an engine-motion stand-down or pause |
| V3 | Upscaler or driver memory not returned on recreate | The gap steps at the `dlss`, `fsr3`, `xess` or DittoSR create lines |
| V4 | The budget shrinks because another process (compositor, browser, capture) takes VRAM | `local_budget_mb` falls while usage holds. `20261021_123140` already showed drops that coincided with budget drops |
| V5 | Placement in non-local while local is under budget | `nonlocal_used_mb` rises with `local_pct` well below 100 and no `over_budget` line |

### Next

1. A session of two hours or more that reproduces the slowdown, in the player's
   usual profile and AA mode, with the wall-clock times of the slowdown and of
   the return to the menu written down. Read the `mem:` and `vram:` lines
   against those times. Reporters on v0.21 or later already write the `mem:`
   line, so their gfx logs of such a session settle V1 to V3 without a new
   build.
2. If V2 or V3 leads: the same route with `fix.temporal_aa = off` and engine
   motion off, to see whether the gap still climbs.
3. Build the instrument additions above before asking for long flights from
   anyone whose logs reach the cap.

### Flight 20261119_215431 (2026-11-19)

The flight:

- **Build:** v0.22.3-24-g1d33eb24. The commit is not on this branch, but the
  build has the `mem:` and `vram:` lines.
- **Setup:** flat, XeSS, render 3264x1836 on a 1920x1080 output, textures on
  high, Arc B580 (12 GB, budget 11,350 MB). The player saw about 1 GB in Task
  Manager's shared GPU memory towards the end.
- **Log:** the gfx log reached its cap, and its first tail segment was deleted.
  The `mem:` lines survive the whole session.

Elite's own figures against the gap (local usage minus `egpu_mb`):

| time | local / budget MB | egpu (peak) | tex | vb | rt | heap used/cap | gap |
|---|---|---|---|---|---|---|---|
| 21:56 | 5931 / 11350 | 3710 (3968) | 1646 | 271 | 1086 | 270/296 | 2221 |
| 22:11 | 7750 / 11350 | 5528 (5618) | 2762 | 909 | 1135 | 830/960 | 2222 |
| 22:17:38 | `over_budget` 11266 / 11256 | | | | | | |
| 22:27 | 12082 / 11294 | 9642 (9708) | 6104 | 1156 | 1414 | 1172/1272 | 2440 |
| 22:32 | 5795 / 11350 | 3232 (9781) | 1369 | 268 | 1092 | 249/308 | 2563 |
| 22:38 | 10655 / 11205 | 8167 (9781) | 4976 | 909 | 1332 | 921/992 | 2488 |

From 22:28:33 to 22:31:33 every figure stood still (a pause or the menu). The
drop at 22:32 is the menu return; engine motion restarted at 22:32:31.

What the flight shows:

- **The growth is Elite's.** `egpu_mb` rose by 7.4 GB while the gap moved
  between 2.0 and 2.8 GB with no trend.
- **Elite frees almost nothing while in flight.** `egpu_peak_mb` stays within
  about 1% of `egpu_mb` until the menu return. Textures and the vertex-buffer
  heaps (`egpu_heap_cap_mb` 0.3 to 1.27 GB) only accumulate.
- **Elite does not respond to the budget.** It was 0.7-0.8 GB over budget from
  22:17 to 22:28 and kept loading: textures went from 4.9 to 6.1 GB.
- **After the menu it refilled quickly:** 3.2 to 8.2 GB within six minutes.
- **On Arc, the spill is not in the non-local figure.** DXGI's
  `nonlocal_used_mb` stayed under 100 MB throughout, while local usage was
  above the budget, which is about the 1 GB the player saw. So on this driver
  the signal is local usage over budget, which the `mem:` line already carries.
  The proposed non-local addition is not needed to see this.
- **The budget itself barely moved:** it dipped by 50-180 MB at times (another
  process).

Ruled out for this flight:

- ruled out (flight 20261119_215431): EDVR keeping freed game resources alive
  (V2) as the growth, because the gap held at 2.0-2.8 GB while `egpu_mb`
  climbed by 7.4 GB.
- ruled out (flight 20261119_215431): upscaler or driver memory not returned on
  recreate (V3) as the growth, for the same reason.
- ruled out (flight 20261119_215431): the budget shrinking (V4) as the cause,
  because it moved by at most 180 MB.
- ruled out (flight 20261119_215431): placement in non-local while under budget
  (V5), because non-local stayed under 100 MB.

What EDVR adds here is headroom, not growth. The gap was 2.0-2.8 GB at this
render size against 0.8 GB at load, before XeSS and the flat runtime are live.
On a card where Elite's own accumulation stops short of the budget, that
difference decides whether it overflows. This matches "worse than stock". A
shorter session or a smaller render size moves the crossing later.

Patch options, ordered by what this flight supports:

1. **Learn how Elite sizes its streaming.** Find whether Elite has a texture or
   mesh budget, and whether it reads the card's `DedicatedVideoMemory` or
   `QueryVideoMemoryInfo`. If it does, report EDVR's gap to it (a smaller size,
   or a lowered budget). If it has no budget, this option is gone.
2. **Evict in flight what the menu evicts.** The menu return frees 6 GB in
   seconds, so Elite has a release path. Identify it and the cache it empties
   (the texture streamer and the mesh heaps), and call only that step at a
   scene boundary or when over budget.
3. **Cap Elite's texture and mesh caches** with least-recently-used eviction at
   the cache itself. Build-bound, and the most work.
4. **Mitigations that do not stop the growth:** eviction priority on Elite's
   per-frame targets so cold textures are demoted first, and a smaller EDVR
   gap.

The player's account of 22:11-22:17: landed at a station, went on foot, and
switched textures from low to high to force the effect. That matches the
textures rising by 2 GB there.

### Elite's streaming budget (Ghidra, build 334925, 2026-11-19)

Game exe build 334925 (SHA-256 `ED5A8F8C...0E264B63`, the installed one),
analysed headless in Ghidra 12.1.4. RVAs are against image base `0x140000000`.

Byte scan of the exe:

- The exe contains no `IDXGIAdapter3` or `IDXGIAdapter4` IID. Elite never calls
  `QueryVideoMemoryInfo`, so it never learns the budget the OS enforces.
- It does contain IIDs for `IDXGIFactory1` and `IDXGIFactory4`.
- Its streaming system reports "Memory budget: CPU/GPU", "Over budget" and
  "Inside budget". It has a debug slider labelled "Memory Budget (CPU MBs/GPU
  MBs)" (`0x9E16B0`) that writes the two budgets at manager `+0x10` and
  `+0x18`.

How the budget is sized:

- **Adapter query** (`0x50EDB0`): calls the adapter's vtable slot `0x40`
  (`GetDesc`). It copies out `DedicatedVideoMemory`, `DedicatedSystemMemory`
  and `SharedSystemMemory`, at output `+0x458`, `+0x460` and `+0x468`.
- **Sizing call** (`0x9E5DD0`): picks a size, then passes it to the budget
  function `0x9E79A0`. The size depends on `DedicatedVideoMemory`:
  - under 1 GB: the larger of it and `SharedSystemMemory`;
  - 1 to 4 GB: the average of the two, if shared is larger;
  - 4 GB or more: `DedicatedVideoMemory` unchanged.
- **Budget function** (`0x9E79A0`):
  - Unless the bss flag `0x5F36C6C` is set, it scales the size by the float
    at `.data` `0x5E78ADC`, which is 0.85 in the file. The factor is only
    read in code, so it is set statically or through a pointer.
  - It converts to MB and clamps between a minimum and a maximum at config
    `+0x28` and `+0x30`. The config object is supplied at construction
    (`0x9DCAC0`), probably from game data, and its values are not known
    statically.
  - It walks a table of categories. For each, it takes the first entry
    whose MB threshold the budget meets, or 512 MB if none does, and
    assigns that pool size through the manager's vtable `+0x18`. The
    remaining budget goes to a remainder category.
  - It stores (1536 MB - budget) at manager `+0x140` when the budget is
    under 1.5 GB.
- The constructor calls the budget function with size 0. The real sizing comes
  later, from `0x9E5DD0`, through a virtual call whose trigger has not been
  traced.

Applied to the flight: the B580 reports a `DedicatedVideoMemory` of about 12
GB. That gives a streaming budget of about 10.4 GB, before the clamp, which is
unknown. Elite's own total reached 9.6-9.8 GB and held there. Add EDVR's gap of
2.0-2.8 GB and the total is well over the OS budget of 11.35 GB. Stock's gap,
about 0.8 GB at load, would land at about 11.2 GB, just inside it. This
accounts for "milder on stock" and for textures on low only delaying the
crossing. It is not yet proven: whether the streaming budget is what bounds
`egpu_tex_mb` and `egpu_vb_mb`, and what the clamp is, are open. Render targets
(1.4 GB) are probably outside the streaming budget, so the match may be partly
coincidence.

The proposed patch follows from this. Before the budget is sized, set the
factor so that 0.85 x VRAM becomes the OS budget minus EDVR's own use minus a
margin. This is a write to one float in `.data`, with no code hook. It needs:

1. the factor's address pinned for the build, as for every other address;
2. the write made before `0x9E5DD0` runs, or a re-run of the sizing;
3. a test flight with a temporary lower factor that shows Elite's total
   plateauing lower and staying under the OS budget.

What to rule out by that flight: the budget not bounding the growth. That would
look like Elite's total climbing past the scaled budget.

### Flight 20261120_000141 (2026-11-20): the setting did not apply

The first build of the key (before the flat-profile fix), flat, XeSS, textures
on high. The route was on foot in a hangar and concourse, a launch, a landing
and back to the concourse. `edvr-flat.ini` had `game_vram = 8192`, but the
log's only `game vram:` line reads "off". Under the flat profile,
`runtimeProfileAllowsKey` (`src/common/runtime_profile.h`) refuses every key
outside its list, and `getString` then answers "off" with no line in the log.
The key was not on the list. Fixed by adding it to the keys every profile
reads, with a rig cell in `tools/game_vram_test` that fails if it is missing.

The flight is a second baseline:

- Elite's total grew from 2.7 to 8.15 GB in 13 minutes, mostly textures (to 4.9
  GB) and vertex buffers (to 1.2 GB). The gap stayed at 1.3-3.5 GB.
- Non-local stayed at 65-114 MB.
- The budget dipped from 11,350 to about 11,184 MB at times, from another
  process.
- The process crossed the budget three times between 00:15:12 and 00:15:25, for
  1-3 s each. At 00:15:43 Elite's total was 2.1 GB, which is the menu or the
  exit.

Any spill the player saw before 00:15 was another program's, not Elite's.

### Flight 20261120_003100 (2026-11-20): `game_vram = 8192` bounds the growth

The build with the flat-profile fix and a fixed reserve, flat, XeSS, textures
on high, the same route as 20261120_000141.

The hook:

- It installed at 00:31:00.620 on an enumerated adapter (`dxgi.dll+0x2EE90`),
  before the game's first query.
- All 12 calls from the game came from `exe+0x50EDFB`, inside the adapter query
  `0x50EDB0` that feeds the streaming budget. Each was told 8192 MB instead of
  the real 12,118 MB.
- No call came before the install.

Against the baseline on the same route:

- **Elite's total levelled off.** It sat at 6.5-6.95 GB from about minute 5 to
  minute 15. The baseline reached 8.15 GB at minute 13 and was still climbing.
  The level matches 0.85 x 8192 = 6963 MB.
- **Elite now frees in flight.** `egpu_peak_mb` reached 7498 while the current
  figure held at 6.5-6.95 GB. Textures fell from 3943 to 3442 MB at 00:41, and
  the mesh heap capacity fell from 974 to 818 MB. In both earlier flights the
  peak tracked the current figure and nothing was freed before the menu.
- **No budget line was written.** There was no `over_budget` and no `pressure`
  line. Local usage peaked at 9.6 GB against a budget of 11.23 GB. The gap was
  2.5-2.8 GB.

Ruled out: the streaming budget not bounding the growth (the hypothesis left
open in "Elite's streaming budget"). With a smaller adapter size, Elite's total
levelled off at 0.85 x the reported size and it freed memory in flight.

Open:

- Whether textures looked worse. That needs the player's eye.
- Whether `auto` leaves enough room. On this card, with the upscaler on, `auto`
  gives Elite 9738 MB, so a streaming level of about 8.28 GB. With the gaps
  seen so far (2.0-3.5 GB), that comes to 10.3-11.8 GB against a budget of
  about 11.2-11.35 GB. The upper end of that range crosses the budget, so the
  2560 MB reserve may need to rise.

### Flight 20261120_005312 (2026-11-20): `game_vram = auto`, too tight

The build with the flat-profile fix and a fixed reserve, flat, XeSS, textures
on high, the same route. The session ran 14.5 min.

The hook installed before the first query. Every call came from `exe+0x50EDFB`
and was told 9737 MB, from a budget of 11,349 MB and a target of 8277 MB.

What happened:

- Elite's total rose to 8.26 GB at minute 14, with a peak of 8546 MB. That is
  the target, overshot by up to 270 MB, and Elite still freed memory in flight.
- The process never crossed the budget.
- From 01:06:30, `pressure` lines ran every 5 s (90% of the budget or more).
  The closest was 11,161 MB against 11,220 MB, a 59 MB margin. The gap was
  about 2.7 GB, and the budget itself dipped to 11,139 MB at times.

So `auto` holds Elite at the level it aims for, but the 2560 MB reserve plus
the 512 MB margin (3072 MB) leaves almost no room. The gap on this card has
reached 3.5 GB, and Elite overshoots its own budget by about 0.3 GB.

Proposed: raise the reserve with an upscaler to 3072 MB, so 3584 MB including
the margin. On this card that gives a target of 7765 MB, a reported size of
9135 MB and roughly 0.5 GB of headroom at the observed peaks. The reserve
without an upscaler (1024 MB) has not been measured.

### Flights 20261120_015601 to 022313 (2026-11-20): auto's learning, first sessions

The first build with learning, flat. There were four sessions. The store
(`%LOCALAPPDATA%\EDVR\game_vram.txt`) ends with one record: `3584 1 0 0 v=8086
d=E20B drv=32.0.101.9175 layer=windows prof=flat aa=xess`.

- **015601** (XeSS, 8.5 min): started from the default of 3584 MB (nothing
  learned yet). At 310 s it wrote the record: peak 9349 of 11,120 MB, closest
  1770 MB, no crossing, not plateaued, so "held: usage still rising or session
  under 20 min". No later line, because the result did not change. Elite's
  total held at about 6.8 GB (target 7254 MB), with local usage at most 9.24
  GB.
- **021959** (XeSS, under 5 min): read the record back ("learned over 1
  session(s)") and wrote nothing.
- **022159** (XeSS, then TAA in the menu): stopped learning at the first mode
  change. The log names `off`, the menu's intermediate step on the way to TAA.
  The store was untouched.
- **022313** (TAA): started under its own key, `aa=on`, with the TAA default of
  2048 MB. It stopped learning at the change towards XeSS; the log names
  `dlss`, again an intermediate step. The store was untouched.

Not yet exercised in a flight: tightening, and relaxing, which needs a session
of 20 minutes or more that levels off. Both are covered only by the rig.

Log wording changed after these flights, not yet built:

- The mode-change line no longer names the new mode, because the menu's
  intermediate steps made it name the wrong one.
- A record that has only ever held now reads "kept unchanged by N earlier
  session(s)". "Learned over N session(s)" is kept for a record that has been
  raised or lowered.

### Flight 20261120_023231 (2026-11-20): 30 minutes on `auto`, the first tightening

This was the first build with learning, flat, XeSS, over multiple environments,
from 02:32 to 03:03. It started from the record of the earlier sessions, at
3584 MB.

- **Elite held its target the whole time.** `egpu_mb` stayed at 6.95-7.24 GB
  from minute 6 to the end, against a target of about 7254 MB (the budget of
  11,350 MB less 3584 and 512). It never crossed the budget.
- **The near miss was the other share.** The gap (local usage minus `egpu_mb`:
  EDVR, XeSS, the driver) moved between 1.2 and 3.2 GB in the per-minute
  samples. At 02:53:17 a spike brought the process within 389 MB of the budget:
  a peak of 10,756 MB against 11,145 MB.
- **The reserve rose by the expected amount.** At 1244 s the learning line read
  "tightened: came within the margin", 3584 -> 3963 MB, which is (512 - 389) +
  256 = 379 MB more, as the rule says. Later passes gave the same result and
  wrote nothing.
- **The store holds** `3963 2 1 0 ... aa=xess`: 2 sessions, raised once.
- **At 02:37:32** local usage briefly read 4675 MB, under `egpu_mb` (5985). The
  OS had trimmed the process's residency during a transition; it is not a fault
  of the figures.

What this means for the rule: tightening reacts to one-second spikes in the
non-Elite share, which is the safe direction. Relaxing needs the closest
approach to stay more than 768 MB from the budget over a whole settled session,
so on this card it will rarely happen. The reserve will settle a little above
the largest spike seen.

### Wine and Proton (2026-11-20, from source, not flown)

What DXVK (Proton's default `dxgi.dll`, read at its master branch on
2026-11-20) and wined3d do with the calls `auto` makes:

- **The hook is unaffected.** All DXVK adapters share one vtable, however
  they are obtained, and slot 8 is `GetDesc`; the caller filter and the
  in-place patch work as on Windows. DXVK's `GetDesc1` to `GetDesc3` do not
  route through slot 8, but Elite calls slot 8 (Ghidra, `exe+0x50EDFB`).
- **`DedicatedVideoMemory`** is the largest Vulkan device-local heap, less
  32 MB on sizes that are a multiple of 4 GiB.
- **The setup key is stable but not faithful.** DXVK's profile for
  `EliteDangerous64.exe` reports vendor `10de` on any GPU, and
  `CheckInterfaceSupport` returns a fixed version per vendor, so a Linux
  driver update keeps the record. The Wine flag keeps Linux and Windows
  records apart.
- **With `VK_EXT_memory_budget`** (NVIDIA, RADV and ANV have it) the budget is
  the driver's estimate for the process and the usage the driver's figure.
  DXVK holds its own allocations under that budget and places the rest in
  system memory, so usage sits just under the budget instead of crossing it;
  auto raises on a near miss as well, so this is covered.
- **Without it, or on wined3d,** the budget is the sum of the device-local
  heaps and the usage only DXVK's own allocations. The budget never moves and
  usage never nears it, so every settled session would lower the reserve by
  512 MB down to the 768 MB floor.

**Guard:** under Wine, a budget at or above `DedicatedVideoMemory` at install
(`gamevram::budgetIsWholeCard`) means no budget. Auto then sizes from 95% of
the card with the safe default reserve, does not read or write the store, and
logs "the reported budget (N MB) is the whole card (M MB)". Windows never takes
this path.

**Known failure case:** an AMD card on RADV without Resizable BAR exposes a
second, 256 MB CPU-visible device-local heap. DXVK sums both heaps' budgets,
which on a quiet desktop can reach the `GetDesc` size even though the driver
reports a real budget. Such a setup is then treated as having none: it keeps
the default reserve every session and never tightens or relaxes. If the
default is too small for it, overflow is not corrected; DXVK's own spill to
system memory limits the damage, but the frame-rate drop can return. Its log
shows the "is the whole card" line with a budget only slightly above the card
size. A fix would compare against the largest heap's own budget, which DXGI
does not expose. wined3d's figures were not read in source and may slip
through the test the other way.
