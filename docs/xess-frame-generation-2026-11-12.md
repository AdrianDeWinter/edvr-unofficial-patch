# XeSS Frame Generation and Xe Low Latency in the flat profile — 2026-11-12

## Status

- **State (2026-11-18): exclusive-fullscreen fix, unflown.** 0.22.3 on an RTX
  5060 Ti froze (journal 2026-11-17). NVIDIA and AMD users report Borderless
  working, so H1, the bridge in exclusive fullscreen, is the open hypothesis.
  Fix: two presenters, the plain one on Elite's own factory (## Design).
  Ruled out by Borderless working: H2 XeLL sleep, H3 proxy Present, H7 the
  handoff, H8 fault+fallback, H9 a slow first compile. Frame generation in
  exclusive fullscreen is out of scope (Intel: passthrough there).
- **Blocking on Intel (flights 5-11):** a proxy made mid-session hangs in
  Intel's igxess_fg.dll (not on NVIDIA); made at creation it does not.
  Journal, 2026-11-18 control: the reporter's build, `fg-blank-screen-diag`.
- **State (2026-11-12): shipped behind a key, flown 4 times on an Arc B580.**
  Works in space, on foot, in settlements and near planets. Flat profile only.
  This re-opens the 2026-10-27 decline in
  `docs/design-frame-generation-2026-10-26.md` with a present bridge
  (## Design).
- **Settings:**
  - `fix.xess_frame_generation`: on | off, default off.
  - `fix.xess_generated_frames`: 1 | 2 | 3 (2x | 3x | 4x), default 1.
  - Both are on the flat F8 Performance tab, dimmed unless the AA mode is
    `xess`, and dimmed and locked in exclusive fullscreen with a note. Counts
    above the GPU's maximum are refused (Intel 3, others 1).
  - Not in VR, because XeSS-FG presents through a DXGI swap chain.
  - Needs Elite's Borderless or Windowed mode: in exclusive fullscreen
    (Elite's Fullscreen mode, or Alt+Enter) frame generation is off and the
    plain swap chain presents.
  - The bridge is made at swap-chain creation only with the key on and AA
    `xess` then (as in 0.22.3): after such a start the key is live both ways
    and the bridge stays until exit, also under another AA mode; otherwise
    turning either on takes effect at the next launch.
- **Fixed, F4 (flights 1-2):** XeSS-FG misplaced the strip a turn brings into
  view, because the prep wrote zero motion for pixels whose previous position
  was off screen.
  - Fix: `FlatMonoResolveFrame::keepOffscreenMotion` (prep bit 64), set while
    FG is wanted.
  - Rig: `offscreenMotionGpuTests`.
  - Ruled out: F5, XeSS-FG's own edge limit at a low base rate.
- **Understood, presented-rate lows (flights 3-4):** where the GPU is full, the
  presented 1% low follows the base frame's p99, because a late real frame
  holds the last generated frame on screen.
  - Ruled out: L1 no generated frames dropped; L2 rare off/on switching; L3
    the bridge's waits were 0; L5 XeLL never oversleeps.
  - At one spot Elite alone took 37 ms of GPU time per frame.
- **Deferred, L6:** XeLL never sleeps; the proxy's Present blocks instead
  (median 7-22 ms). Candidates L6a (RENDERSUBMIT_END before the D3D12 copy),
  L6b (XeLL sees only D3D12 work). Not pursued (2026-11-12).
- **Known limit:** UI ghosting. The UI mode is NONE because no HUD-less image
  exists at Present, so the HUD is interpolated with the scene.
- **Never checked directly:** F2, the view/projection split; nothing swam.
- **Unflown:** the 2026-11-12 and PR #208 review fixes (own fences, skipped
  copies, F4 exclusions, 30-dropped-frame fallback, stuck-queue handling,
  reasons on enable/disable lines), and the whole two-presenter design.
- **Issues:** none filed yet for L6, F2 or the 0.22.3 NVIDIA report.
- **Next flight (Arc B580, then an NVIDIA card):** the six presenter cases.
  Windowed start, key off then on; windowed, key on; windowed then
  Fullscreen and Alt+Enter, and back; Fullscreen start, key off, on, and on
  at launch. Check `the proxy takes window`, `the plain swap chain takes
  window`, no `stops for this session`, the panel lock in fullscreen.

## Design

- **SDK:** intel/xess `8fe81bdb` (SDK 3.0.2): XeSS-FG 1.3.1.78
  (`libxess_fg.dll`) and XeLL 1.3.2.10 (`libxell.dll`). Both ship beside
  `libxess.dll` (`tools/fetch_xess.py`). Both are D3D12 only.
- **Packaging:** rows `xess_fg` and `xess_ll` of `package_native.FILES`, the
  one table the installer, the packages and the launcher's schema-2 manifest
  are written from: in the XeSS zip, `owned`, `when` flat profile only, and
  placed only with `xess` (`kBlurbs` in `src/installer/plan.cpp`).
- **Two presenters (2026-11-17).** On a flat launch with `fix.xess_frame_generation` on and AA `xess` (2026-11-18: back to 0.22.3's rule; the key off at launch means no bridge, so XeSS players without frame generation keep Elite's own swap chain), Elite's
  D3D11 swap chain is made on a hidden EDVR window and never presented. One of
  two swap chains of EDVR's owns Elite's real window:
  - **Plain** (D3D11 flip discard, 2 buffers): a copy of Elite's back buffer
    and a Present. No XeSS-FG context, no XeLL, no proxy. It starts on the
    window, and it is the only presenter in exclusive fullscreen. It is made
    on Elite's own factory (the hidden swap chain's parent), past
    device_hook's creation hook, so Elite's window association and Alt+Enter
    apply as without the bridge (H1). The proxy stays on the bridge's factory.
  - **Proxy** (below): made the first time frame generation can run, that is
    the key on, the flat profile, not exclusive fullscreen, and the XeSS-SR
    resolve ran on 2 presents in a row. The plain chain is released (and
    D3D11 flushed) before XeSS-FG makes its swap chain on the window.
  - **Staying on the proxy.** Menus, another AA mode or the key turned off
    only disable XeSS-FG (`xefgSwapChainSetEnabled(0)`); the proxy keeps the
    window. No swap chain is rebuilt per disable.
  - **Back to plain only for exclusive fullscreen**, whether Elite asks
    (`SetFullscreenState(TRUE)`) or DXGI's Alt+Enter does it on its own (read
    back each Present). The proxy leaves fullscreen, XeSS-FG and XeLL are
    destroyed (the proxy belongs to XeSS-FG's context), the plain chain is
    made and goes fullscreen.
  - **Back to the proxy** once the window is windowed and in the foreground
    for 30 presents in a row, so an alt-tab out of fullscreen (DXGI drops
    fullscreen on focus loss) does not rebuild twice.
  - A failed switch to the proxy is a fault: the plain chain stays for the
    session.
  - Why: every Arc flight was Borderless with the key on from launch. The
    0.22.3 NVIDIA hang came through startup and menus, where the proxy existed
    but was disabled, and XeLL's sleep ran every frame. Intel's guide: a
    disabled proxy still costs; only destroying it removes it.
- **The proxy (`src/d3d11/xess_fg.cpp`):**
  - An XeSS-FG proxy swap chain (D3D12, flip discard, 3 buffers, tearing where
    supported) owns Elite's window while frames can be generated. It runs on the XeSS-SR bridge's D3D12
    device and queue (`xessBridgeOpen`), with two shared fences of its own
    (`xessBridgeMakeFence`): `in`, signalled only by D3D11, and `out`,
    signalled only by D3D12. One writer per fence keeps both monotonic;
    sharing XeSS-SR's single counter let the fence go backwards when the two
    queues finished out of order (review, 2026-11-12).
  - Each Present copies Elite's back buffer into a shared texture, hands it
    to D3D12 over `in`, copies it into the proxy's back buffer, and presents
    between XeLL's markers. A frame whose copy could not be made (keyed
    mutex timeout) is not presented, so the window never shows an older
    proxy buffer; 30 such frames in a row are a fault.
  - The proxy follows Elite's back buffer size and format after each resize
    and again at Present, which catches a resize the hooks missed.
  - **Fallback:** any fault (a GPU wait past 5 s, a refused resize, a format
    XeSS-FG cannot present such as FP16) stops frame generation for the
    session. XeSS-FG and XeLL are destroyed, and a plain D3D11 flip swap
    chain of EDVR's takes the real window, so Elite's frame still reaches the
    screen. After a wait timed out, the queue is not waited on again, and the
    copy lists and shared texture it may still use are kept, not released.
  - A second swap chain Elite makes for the same window makes the bridge let
    go of it first (`xessFgRelease`), and frame generation ends for the
    session: like main, device_hook.cpp hooks only the first swap chain (H7).
    The hidden window stays, since the old swap chain may live on it.
  - XeLL's sleep runs at the end of EDVR's Present hook, only while the proxy
    presents.
- **Inputs:** XeSS-SR's own motion (render grid, current -> previous pixels,
  jitter removed) and depth (reversed infinite Z). Both are tagged
  `RV_ONLY_NOW` on the XeSS-SR command list after its execute
  (`xessSetInputTagger`), and count once that list is submitted.
- **Frame constants:** view and projection split from the resolver's clip rows
  (`cameraMatrices`), jitter in render pixels, MV scale 1, reset = the
  resolver's reset.
- **Enable and disable:**
  - Enabled after 2 consecutive presents with tagged inputs, and disabled on
    any present without them (menus, a non-XeSS mode).
  - Exclusive fullscreen (Elite's Fullscreen mode) passes through without
    generating. Borderless and Windowed generate.
- **Window queries:** the presenting swap chain (proxy, or the fallback)
  answers the hidden swap chain's GetDesc, GetHwnd, fullscreen state and
  GetFullscreenDesc, ResizeTarget, GetContainingOutput, GetFrameStatistics
  and GetLastPresentCount. `device_hook.cpp` routes Present and
  ResizeBuffers. A fullscreen creation goes fullscreen on the real window
  right after the bridge is made.
- **Edge strip (F4) inputs:** the kept off-screen motion reaches XeSS-SR as
  well (it takes no rejection mask). Stale-slot pixels and pixels inside the
  vehicle volume are excluded: their vectors need last frame's depth to be
  trusted, which an off-screen position cannot give.
- **Without the key:** nothing is loaded. A non-Intel GPU's maximum (1) is
  known from Elite's swap chain creation on; an Intel GPU's only once a bridge
  has been made, and until then every frame count is offered and the runtime
  clamps.
- **Window and threads:** the presenting swap chain's fullscreen state is read
  back each Present, so a change DXGI's Alt+Enter makes on the proxy (whose
  factory is the bridge's, not Elite's) does not leave it stale. The bridge
  is not locked; a call on Elite's swap chain from a thread other than the
  presenting one is logged once.

## Failure signatures

| Log line | Means |
|---|---|
| `xess-fg: not started: ...` | no bridge; the swap chain was made the usual way (includes an AA mode other than xess at launch) |
| `xess-fg: bridge ready: ...` | bridge up; names the proxy size, Elite's swap chain flags and the GPU's maximum |
| `xess-fg: frame generation enabled/disabled at frame N: <reason>` | inputs started or stopped; the reason tells the setting, fullscreen, a failed tag and no resolve apart |
| `xess-fg: N frames in a row could not be copied to the proxy ...` | the dropped-frame limit; a fault follows |
| `xess-fg: ... came from thread T, but Present runs on thread P` | a call off the presenting thread; the bridge's state is not locked |
| `xess-fg: exclusive fullscreen is now on/off without a call from Elite` | DXGI changed the proxy's fullscreen alone (Alt+Enter) |
| `xess-fg: tagging frame N failed` | inputs refused; no frames generated |
| `xess-fg: frame N: interpolation result ...` | XeSS-FG skipped a frame. Expected on the first frame after an enable, a reset or a resize |
| `xess-fg: ...; frame generation stops for this session, and Elite's frame goes to the window through a plain D3D11 swap chain` | bridge fault; followed by `xess-fg: plain D3D11 swap chain on window ...` with its result |
| `xess-fg: frame N not presented: ...` | the copy into the shared texture was skipped (keyed mutex); the window keeps the last image |
| `xess-fg: Elite is making another swap chain on window ...` | the bridge let go of the window for Elite's new swap chain |
| `xess-fg: XeSS-SR now runs on another D3D12 device` | XeSS-SR restarted on a device change; no more inputs are tagged |
| `xess-fg: the proxy takes window ... at frame N` | plain -> proxy: frame generation can run from here |
| `xess-fg: the plain swap chain takes window ... at frame N: <reason>` | proxy -> plain: exclusive fullscreen (Elite's call, or DXGI's Alt+Enter) |

## Journal

- **Flight 1** (build 6AF5C4E0): space works.
  - 74 -> 152 fps at 1 generated frame, 50 -> 200 at 3.
  - On foot at a low base rate, artifacts appeared on the edge being turned
    toward (F4).
- **Flight 2** (build 6AF5DA06): F4 fixed in concourses, operations,
  settlements, space and near planets. Lows below 30 fps where the GPU is
  full.
- **Flight 3** (build 6AF5E8D3, with pacing counters since removed):
  - Every present sent all its frames.
  - Base frame median/p99: 28-35 / 56-70 ms at 3x, 16-23 / 37-45 ms at 1x.
  - The proxy Present blocked for a median of 12-16 ms at 3x and 6-9 ms at 1x.
  - F9: 3x 36.8 ms (Elite 25.2), 1x 24.5 ms (Elite 16.6).
  - XeSS-SR ran at 3840x2160 in and out, with Elite's back buffer at 2560x1440
    (4K display at 150%).
- **Flight 4** (build 6AF5EEDE, with XeLL reports since removed):
  - XeLL had low-latency on, and all 64 reports were complete in every window.
    Present end -> next sim start was 0.3 ms median.
  - Spot A, F9: 28.8 ms at 2x, 33.5 ms at 4x.
  - Spot B, F9: 44-45 ms at both 2x and 4x, of which Elite took 37 ms.
- **Logs read for the PR #208 review (2026-11-13)**, the 12 flat logs of
  11-12 and 11-13 (newest `20261113_012553`, build 6AF63993, 80 min):
  - No `not presented` frame and no fault in any of them.
  - Every bridged launch logged XeSS-FG's `Back buffer width is zero`: the
    GPU-maximum query passed 0x0. Fixed (real size passed).
  - Elite's swap chain flags are 0x2 (ALLOW_MODE_SWITCH), not frame-latency
    waitable; its window is borderless (style 0x94020000), and exclusive
    fullscreen was set only off, at exit.
  - Present and Elite's window-mode request ran on the same thread (30332);
    ResizeBuffers' thread was not logged before `threadCheck`.
  - Frame generation was disabled and re-enabled 2-3 frames later a few times
    per hour; the reason was not logged before this build. Open: read the new
    reason on the next flight before changing the disable rule.
- **2026-11-17, 0.22.3 report (RTX 5060 Ti, Windows 11 24H2):** key turned
  on, game restarted, then a blank window that advanced only on alt-tab and
  froze while focused, audio running; closed from the taskbar. No log. First
  known non-Intel run. Candidates in the plan: exclusive fullscreen on the
  proxy (no `MakeWindowAssociation` on the bridge's factory), XeLL's sleep
  with no time limit on NVIDIA, the disabled proxy's Present. Response: the
  two-presenter design above; ask the reporter for the log and display mode.
- **2026-11-18:** several NVIDIA and AMD users report XeSS-FG working well in
  Borderless.
  - ruled out: H2 (XeLL's sleep blocks on NVIDIA), because XeLL sleeps every
    frame in Borderless, which works.
  - ruled out: H3 (the proxy's Present blocks while focused), because the
    proxy presents in Borderless, which works.
  - ruled out: H7 (the D3D11 -> D3D12 handoff stalls), same handoff in
    Borderless.
  - ruled out: H8 (a fault, then the fallback), not mode-specific, and a
    fault logs `stops for this session`.
  - ruled out: H9 (a slow first pipeline compile), explains a slow start, not
    the alt-tab pattern.
  - Open: H1, exclusive fullscreen through the bridge's own factory (no
    `MakeWindowAssociation`, never flown). Fix chosen: the plain swap chain on
    Elite's own factory. Parked: H6 (a third-party hook), only if the reporter
    was in Borderless.
- **Flight 5, 2026-11-18** (885583ea, Arc B580, Borderless 1920x1080,
  `edvr_gfx_20261118_012209`): main menu, then a hard freeze; the overlay
  does not open; killed. Bridge ready on the plain swap chain on Elite's
  factory; at frame 1266 `the proxy takes window`, at 1268 `frame generation
  enabled` and `interpolation result mismatch-input-resources (-12)`, then no
  render-thread line for 14 s to the end (no `stops for this session`: no
  bounded wait timed out). A regression from 0.22.3, where the proxy was made
  at creation: the hang follows the proxy made mid-session. All on the main
  menu: its 3D scene runs XeSS-SR, so the proxy is made there. The stall
  sampler was off: `runtimeProfileAllowsKey` refused `advanced.freeze_location`
  on flat, so it read 0 (it never ran on flat). `mismatch-input-resources` on
  the first enabled frame also appears in working 0.22.3 flights (16 Nov):
  not the cause.
- **Flight 6, 2026-11-18** (same build, `edvr_gfx_20261118_012716`): the same
  freeze, 3 frames after `frame generation enabled` at frame 1261; sampler
  still off (the key refused, above). Next: a build that lets
  `advanced.freeze_location` through on every profile and traces each call of
  the first 32 presents after `the proxy takes window` (TEMPORARY). Open, each
  told apart by the last trace line or the `stall:` module: R1 XeLL's first
  sleep (its context made at launch, no markers for ~1260 frames); R2 XeSS-FG's
  Present; R3 a cross-queue GPU wait; R4 two factories on the window.
- **Flight 7, 2026-11-18** (074aaad8, trace and sampler on flat,
  `edvr_gfx_20261118_014355`): the proxy took the window at frame 1253 on the
  main menu; `SetEnabled(1)` at 1255; Present 1256 returned (17 ms); Present
  1257 never returned. Stall samples at 154, 504 and 1001 ms: a kernel wait at
  `igxess_fg.dll+0x1cac86` (< +0x1b54d8 < +0x1b604d), called from EDVR's proxy
  Present, under Steam's overlay hook.
  - ruled out: R1 (XeLL's sleep), because every `xellSleep` returned.
  - ruled out: our copies and SetEnabled, because each returned.
  - Confirmed: R2, Intel's igxess_fg.dll waits forever inside the proxy's
    Present on the 2nd frame-generation frame. What it waits on (its present
    thread, a GPU fence, DXGI) is not visible from the render thread.
- **Flight 8, 2026-11-18** (same build, Steam overlay off,
  `edvr_gfx_20261118_015036`): the same hang at the 2nd frame after enable
  (frame 1264), same igxess_fg stack, no overlay on it.
  - ruled out: K1 (the Steam overlay re-hooking the window's new swap chain),
    because the hang is the same without it.
  - Open, what differs from 0.22.3's working start: K2 frame generation
    enabled 2 presents after the proxy is made (0.22.3: ~1300 disabled
    presents first); K3 the XeLL and XeSS-FG contexts made at launch, ids
    starting at ~1253, but the proxy and sleep mode only mid-session (0.22.3:
    all together at frame 1). Next: TEMPORARY `advanced.xess_fg_switch_test`
    (1 warm-up 120 presents, 2 contexts made with the proxy, 4 ids restart at
    1), one launch per value.
- **Flight 9, 2026-11-18** (4e42eefd, switch test 1,
  `edvr_gfx_20261118_021527`): frame generation held off for 120 proxy
  presents, enabled at frame 1372, then the same igxess_fg hang.
  - ruled out: K2 (no warm-up before enable), because 120 disabled presents
    first hang the same.
- **Flight 10, 2026-11-18** (4e42eefd, switch test 6,
  `edvr_gfx_20261118_021715`): contexts made with the proxy, ids from 1;
  enabled at frame 3, the same igxess_fg hang at frame 5.
  - ruled out: K3 (contexts made at launch, ids from ~1253), because made
    with the proxy and numbered from 1 they hang the same.
  - Open: K4 the window carried EDVR's plain swap chain (Elite's factory)
    before the proxy; K5 the proxy is made while XeSS-SR already runs on the
    shared D3D12 queue. Next: switch test 8 (the proxy at creation, no plain
    chain, as 0.22.3) and 16 (the plain chain on the bridge's factory).
- **Flight 11, 2026-11-18** (329f26f8, switch test 8,
  `edvr_gfx_20261118_023451`): the proxy at creation; frame generation
  enabled at 1266 on the main menu and ran 3 minutes with no freeze.
  - Confirmed: the main-menu freeze comes from the proxy taking the window
    mid-session (K4 or K5 still to split; switch test 16 next).
  - New: tabbing out, or Exit on the main menu, froze the whole desktop until
    Ctrl+Alt+Del. The log stops at frame 3842: frame generation disabled at
    3840 (the 3D scene went), a 250 ms wait in d3d12 under igxess_fg, two more
    frames, nothing after; no `exclusive fullscreen off` exit line, which
    every 16 Nov flight on this Arc logged. Not a GPU hang: the Ctrl+Alt+Del
    screen drew at once and smoothly. 0.22.3 does not do it (Martin), so it is
    this branch, even with switch test 8. The one branch change that acts in
    this flight and not in 0.22.3: the stall sampler on flat (e2c21d2d), which
    suspended the render thread in d3d12 under igxess_fg at frame 3840 just
    before the log stopped. Next: switch test 8 with freeze_location off.
- **Flight 12, 2026-11-18** (329f26f8, switch test 8, sampler off,
  `edvr_gfx_20261118_024315`): no menu lock; hitches with a fan smearing more
  at each; Exit froze the desktop again.
  - ruled out: the stall sampler as the desktop freeze, because with it off
    the freeze is the same.
  - The hitches match flight 11's: about 290 ms right after enable and
    about 790 ms 26 s later, both busy outside Present (the game's CPU), so
    XeSS-FG interpolates across a long gap (the smear).
  - This log ends as the clean 16 Nov exits do (`exclusive fullscreen off`,
    then `vertex resync`): the desktop freeze comes at or after process
    exit, after EDVR's last line. Next: a control flight of origin/main
    (0.22.3 plus later merges, no XeSS-FG change) on this rig today.
- **Control flight, 2026-11-18** (origin/main 8507b6e2: 0.22.3 plus VR-map
  changes only, `edvr_gfx_20261118_144839`): no menu freeze, frame generation
  enabled at 1401; Exit left a black screen until Ctrl+Alt+Del. Windows logged
  no Application Hang for this flight or flight 12 (it did for each menu
  freeze), so Elite had exited. The 1 s monitor also logged a 264 ms freeze
  near the end, like the hitches of flights 11-12.
  - ruled out: this branch as the exit black screen and the hitches, because
    0.22.3's code does both on this rig today. Driver (32.0.101.9175, 9 Nov)
    and XeSS libraries (12 Nov) unchanged since the clean 16 Nov exits; the
    TeamViewer Virtual Monitor adapter (the rig is reached only through
    TeamViewer) re-arrived at 14:07. Suspected environmental; not pursued.
  - Still this branch: the main-menu freeze (flights 5-10), because the
    control and switch test 8 make the proxy at creation and do not freeze.
    It waits inside igxess_fg.dll, which ships with Intel's driver
    (DriverStore `iigd_dch_d.inf`), so an NVIDIA card never runs it.
  - The TEMPORARY trace, `advanced.xess_fg_switch_test` and the sampler on
    flat (e2c21d2d, 074aaad8, 4e42eefd, 329f26f8) are taken off this branch
    and kept on local `fg-blank-screen-diag`; the branch is 885583ea again,
    the build for the 0.22.3 reporter (RTX 5060 Ti). Open before merge: K4 or
    K5 on Intel (switch test 16 on the diag branch).
- **2026-11-18, bridge rule:** back to 0.22.3's: the bridge only with the key
  on and AA `xess` at launch. Making it with AA `xess` alone (so the key could
  be turned on without a restart) put every XeSS player, with or without frame
  generation, through a full-frame copy and the unflown plain swap chain
  instead of Elite's own Present. XeSS-SR alone never needs the bridge: it
  upscales inside Elite's frame.
