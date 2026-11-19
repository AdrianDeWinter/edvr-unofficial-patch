# XeSS Frame Generation and Xe Low Latency in the flat profile — 2026-11-12

## Status

- **State (2026-11-23): fullscreen fix flown 3 times, its final form twice.**
  0.22.3's black screen: no ResizeBuffers after SetFullscreenState, so every
  Present failed. The presenter is now resized after each switch (11-17 on).
- **State (2026-11-12): shipped behind a key, flown 4 times on an Arc B580.**
  Works in space, on foot, in settlements and near planets. Flat profile only.
  This re-opens the 2026-10-27 decline in
  `docs/design-frame-generation-2026-10-26.md` with a present bridge
  (## Design).
- **Settings:**
  - `fix.xess_frame_generation`: on | off, default off.
  - `fix.xess_generated_frames`: 1 | 2 | 3 (2x | 3x | 4x), default 1.
  - Both are on the flat F8 Performance tab, dimmed unless the AA mode is
    `xess`. Counts above the GPU's maximum are refused (Intel 3, others 1).
  - Not in VR, because XeSS-FG presents through a DXGI swap chain.
  - Needs Elite's Borderless or Windowed mode: exclusive fullscreen presents
    without generating, and the panel hint and the log say so.
  - The bridge is made only at swap-chain creation, and only when the AA mode
    is `xess` then: turning either on later takes effect at the next launch.
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
- **Deferred, L6:** XeLL is on and receives every marker, but never sleeps.
  The proxy's Present blocks instead (median 7-22 ms), so latency is higher
  than it needs to be.
  - Candidates: L6a, RENDERSUBMIT_END comes before the frame's D3D12 copy is
    submitted; L6b, XeLL sees only D3D12 work, and Elite renders on D3D11.
  - Not pursued for now (2026-11-12).
- **Known limit:** UI ghosting. The UI mode is NONE because no HUD-less image
  exists at Present, so the HUD is interpolated with the scene.
- **Never checked directly:** F2, whether XeSS-FG expects the view/projection
  split we pass. Nothing has swum on rotation in any flight.
- **Unflown (review fixes, 2026-11-12):** own fences, the D3D11 fallback,
  skipped-copy frames, the stale-slot and vehicle exclusion from F4, the
  frame-statistics and fullscreen-desc queries. Flights 1-4 predate them.
- **Unflown (PR #208 review, 2026-11-13):** no bridge unless AA is XeSS at
  launch; fallback after 30 dropped frames in a row; non-Intel cap of 1 before
  any bridge; tags count only once XeSS-SR's list is submitted; no second 5 s
  wait after a timeout, and resources a stuck queue may use are kept; a reason
  on each enable/disable line; Elite's swap chain flags, an off-thread call and
  a fullscreen change DXGI made alone are logged; the GPU-maximum query passes
  the real size; the panel's exclusive-fullscreen hint.
- **Issues:** none filed yet for L6 or F2.
- **Next flight (the original reporter, on 302a4532 or later):** the same
  Fullscreen/Borderless round trips on their machine: the picture stays,
  `resized to` beside each `exclusive fullscreen` line, no `Present failed`,
  the "paused in fullscreen" badge, frame generation back on in Borderless.

## Design

- **SDK:** intel/xess `8fe81bdb` (SDK 3.0.2): XeSS-FG 1.3.1.78
  (`libxess_fg.dll`) and XeLL 1.3.2.10 (`libxell.dll`). Both ship beside
  `libxess.dll` (`tools/fetch_xess.py`). Both are D3D12 only.
- **Packaging:** rows `xess_fg` and `xess_ll` of `package_native.FILES`, the
  one table the installer, the packages and the launcher's schema-2 manifest
  are written from: in the XeSS zip, `owned`, `when` flat profile only, and
  placed only with `xess` (`kBlurbs` in `src/installer/plan.cpp`).
- **The bridge (`src/d3d11/xess_fg.cpp`):**
  - On a flat launch with the key on, Elite's D3D11 swap chain is made on a
    hidden EDVR window and never presented.
  - An XeSS-FG proxy swap chain (D3D12, flip discard, 3 buffers, tearing where
    supported) owns Elite's window. It runs on the XeSS-SR bridge's D3D12
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
  - XeLL's sleep runs at the end of EDVR's Present hook.
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
| `xess-fg: the proxy's / the plain swap chain's Present failed 0x... (exclusive fullscreen on/off)` | the first 8 failed Presents; 0x887A0001 in exclusive fullscreen is a switch the presenter was not resized after |
| `xess-fg: ... not resized after the fullscreen switch: ...` | the resize a fullscreen switch needs did not run; the next Presents fail |

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
- **2026-11-17, 0.22.3 report:** frame generation on, game restarted: a blank
  window that advanced only on alt-tab, audio running; closed from the
  taskbar. Frame generation works for others in Borderless, so causes that
  Borderless would show too were dropped.
  - ruled out: H2 (XeLL's sleep blocks), H3 (the proxy's Present blocks), H7
    (the D3D11 -> D3D12 handoff stalls), because Borderless runs each of them
    and works.
  - ruled out: H8 (a fault, then the fallback), because it is not
    mode-specific and a fault logs `stops for this session`; H9 (a slow first
    pipeline compile), because it explains a slow start, not the alt-tab
    pattern.
- **2026-11-18, two-presenter attempt (885583ea, dropped):** a plain D3D11
  swap chain on Elite's own factory showed the frame until frame generation
  could run, then the proxy was made mid-session.
  - A proxy made mid-session hangs inside XeSS-FG's Present 2-3 frames after
    frame generation is enabled, on the main menu (flights 5-10 and the
    2026-11-19 test flight). Made at swap-chain creation it does not (switch
    test 8, flight 11).
  - ruled out as that hang's cause: K1 (the Steam overlay), K2 (no warm-up
    before enable), K3 (the contexts made at launch), because each removed
    alone hangs the same. Not pursued: K4 (the plain swap chain held the
    window first), K5 (XeSS-SR already on the shared D3D12 queue).
  - A black screen at exit and hitches on one test machine came with
    0.22.3's own code there too (control flight, 8507b6e2): environmental.
- **2026-11-19, test flight** (v0.22.3-8-gbbdc65c9, 3440x1440 at 144 Hz):
  - Fullscreen: "black screen, music plays, no intro video; after alt-tab the
    main menu shows but is frozen". Elite asked for exclusive fullscreen at
    frame 1 and the plain swap chain took it (0x00000000); from then every
    Present failed with 0x887A0001 (DXGI_ERROR_INVALID_CALL), 2096-3322 per
    5 s window while fullscreen, 0 while alt-tabbed out. The game kept
    rendering; the screen kept the last windowed image. No `plain swap chain
    resized` line: syncPlain skips an unchanged size, and DXGI requires
    ResizeBuffers after a flip-model swap chain's fullscreen switch before
    Present succeeds. 0.22.3 sends the same call to the proxy and syncProxy
    skips an unchanged size the same way, which fits the 0.22.3 report.
    - ruled out: H1 (a factory without Elite's window association), because
      the plain swap chain on Elite's own factory fails the same way.
  - Borderless: "froze as XeSS finished resolving on the main menu": the
    mid-session proxy hang above, 3 frames after enable.
- **2026-11-19, fix:** back to 0.22.3's design (the proxy made at swap-chain
  creation, passthrough with frame generation off in exclusive fullscreen).
  After every successful exclusive-fullscreen switch, from Elite or from DXGI's
  Alt+Enter, the presenting swap chain is resized at an unchanged size
  (`syncPresenter(b, true)`), because a flip-model swap chain's next Present
  fails until ResizeBuffers follows the switch (DXGI
  `IDXGISwapChain::ResizeBuffers` remarks, in both directions). The first 8
  failed Presents are logged with their HRESULT, and the F8 frame generation
  row shows "paused in fullscreen" while exclusive fullscreen holds it off.
  - Flown 2026-11-23 (c3ef17cd): Fullscreen start, then Borderless,
    Fullscreen and Borderless again: the picture stays throughout, and frame
    generation comes on as soon as the game is windowed.
  - The committed form differs: c3ef17cd resized the presenter to its own
    size, format and flags (`resizeAfterFullscreenSwitch`); the commit
    instead runs `syncProxy`/`syncPlain` forced, at the size and format of
    Elite's back buffer, so the plain path now also needs that buffer and a
    flip-model twin of its format. c3ef17cd is on no branch.
  - Flown 2026-11-23 (302a4532, the committed form; logs `_140926`,
    `_141259`): a Borderless start with two Fullscreen round trips at
    2560x1440 and 3840x2160, then a Fullscreen start, Borderless and exit.
    Every switch logs `resized to` before `exclusive fullscreen on/off
    (0x00000000)`; no `Present failed` or `not resized`; frame generation is
    off in Fullscreen and back on within 0.4 s of Borderless. The one
    `mismatch-input-resources (-12)` on the first frame after frame generation
    turns on is in the f2907cd8 logs from before the fix too.
  - Unflown: the plain-swap-chain fallback across a fullscreen switch.
