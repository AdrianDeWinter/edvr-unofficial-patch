# XeSS Frame Generation and Xe Low Latency in the flat profile — 2026-11-12

## Status

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
  - Turning it on takes effect at the next launch: the bridge is made only at
    swap-chain creation.
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
  - Ruled out:
    - L1: no generated frames were dropped.
    - L2: frame generation rarely switched off and on.
    - L3: the bridge's waits were 0.
    - L5: XeLL never oversleeps.
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
- **Issues:** none filed yet for L6 or F2.
- **Next flight:** fly the review build on the Arc B580 in space and on foot,
  and check the log for `bridge ready`, no `stops for this session`, and the
  F4 strip on a fast turn. Then force a fallback (resize the window while a
  shader compile stalls the queue, or switch to an FP16 output) and check the
  game stays on screen.

## Design

- **SDK:** intel/xess `8fe81bdb` (SDK 3.0.2): XeSS-FG 1.3.1.78
  (`libxess_fg.dll`) and XeLL 1.3.2.10 (`libxell.dll`). Both ship beside
  `libxess.dll` (`tools/fetch_xess.py`). Both are D3D12 only.
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
    proxy buffer.
  - The proxy follows Elite's back buffer size and format after each resize
    and again at Present, which catches a resize the hooks missed.
  - **Fallback:** any fault (a GPU wait past 5 s, a refused resize, a format
    XeSS-FG cannot present such as FP16) stops frame generation for the
    session. XeSS-FG and XeLL are destroyed, and a plain D3D11 flip swap
    chain of EDVR's takes the real window, so Elite's frame still reaches the
    screen.
  - A second swap chain Elite makes for the same window makes the bridge let
    go of it first (`xessFgRelease`).
  - XeLL's sleep runs at the end of EDVR's Present hook.
- **Inputs:** XeSS-SR's own motion (render grid, current -> previous pixels,
  jitter removed) and depth (reversed infinite Z). Both are tagged
  `RV_ONLY_NOW` on the XeSS-SR command list after its execute
  (`xessSetInputTagger`).
- **Frame constants:** view and projection split from the resolver's clip rows
  (`cameraMatrices`), jitter in render pixels, MV scale 1, reset = the
  resolver's reset.
- **Enable and disable:**
  - Enabled after 2 consecutive presents with tagged inputs, and disabled on
    any present without them (menus, a non-XeSS mode).
  - Exclusive fullscreen passes through without generating.
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
- **Without the key:** nothing is loaded. The panel learns the GPU's maximum
  only once a bridge has been made; until then every frame count is offered
  and the runtime clamps.

## Failure signatures

| Log line | Means |
|---|---|
| `xess-fg: not started: ...` | no bridge; the swap chain was made the usual way |
| `xess-fg: bridge ready: ...` | bridge up; names the proxy size and the GPU's maximum |
| `xess-fg: frame generation enabled/disabled at frame N` | inputs started or stopped (menus, AA mode) |
| `xess-fg: tagging frame N failed` | inputs refused; no frames generated |
| `xess-fg: frame N: interpolation result ...` | XeSS-FG skipped a frame. Expected on the first frame after an enable, a reset or a resize |
| `xess-fg: ...; frame generation stops for this session, and Elite's frame goes to the window through a plain D3D11 swap chain` | bridge fault; followed by `xess-fg: plain D3D11 swap chain on window ...` with its result |
| `xess-fg: frame N not presented: ...` | the copy into the shared texture was skipped (keyed mutex); the window keeps the last image |
| `xess-fg: Elite is making another swap chain on window ...` | the bridge let go of the window for Elite's new swap chain |
| `xess-fg: XeSS-SR now runs on another D3D12 device` | XeSS-SR restarted on a device change; no more inputs are tagged |

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
