# XeSS-FG: blank, frozen window (0.22.3 report, RTX 5060 Ti)

## Goal
A flat launch with `fix.xess_frame_generation = on` gave an RTX 5060 Ti user a blank window that froze while focused
and advanced only on alt-tab (audio running; closed from the taskbar). Make that combination run the game normally,
with the fewest test flights.

## Scope and evidence (Martin, 2026-11-18)
- **Frame generation in exclusive fullscreen is out of scope.** Intel's guide: XeSS-FG cannot be enabled in exclusive
  fullscreen and runs as passthrough there. In scope: exclusive fullscreen with the bridge present must run the game
  normally, with no frames generated. Anything aimed at generating frames in fullscreen is dropped.
- **Multiple NVIDIA and AMD users report XeSS-FG working flawlessly in Borderless.** So XeSS-FG, XeLL, the proxy and
  the D3D11 -> D3D12 handoff work on non-Intel GPUs. Every hypothesis that would also break Borderless is dropped.
- **What is left points at exclusive fullscreen.** The reporter's display mode is not known yet; Step 1 asks. If they
  were in Borderless, this plan is wrong and the parked hypotheses come back (see "Parked").

## How it worked in 0.22.3 (the reported build)
- Elite's swap chain on a hidden window; the XeSS-FG proxy on the real window from launch, on the bridge's **own DXGI
  factory**, which never calls `MakeWindowAssociation` (no MWA call anywhere in `src\`). DXGI therefore watches the real
  window for Alt+Enter and focus loss on the bridge's behalf, not Elite's.
- A Fullscreen creation made the proxy exclusive at once (`device_hook.cpp:2060`). No Arc flight ever used Fullscreen
  (all Borderless, journal 2026-11-13), so the bridge in exclusive fullscreen was unflown on every GPU.

## Hypotheses (only those that fit "Borderless works on NVIDIA and AMD")
- **H1. Exclusive fullscreen through the bridge.**
  - Why: unflown everywhere. The bridge's factory is not Elite's, so DXGI's focus-loss handling drops the proxy to
    windowed on alt-tab (the image advances), and Elite, which still believes it is fullscreen, re-enters exclusive on
    refocus (freeze). Reproducible on Arc: no NVIDIA rig needed.
  - Log signature (0.22.3 already logs it): `xess-fg: exclusive fullscreen on (0x...)` right after `bridge ready`, then
    `exclusive fullscreen is now off without a call from Elite` at each alt-tab.
  - Sub-cases, all fullscreen-only, all decided by the same flight:
    - **H1a, OCCLUDED loop** (was H4): Present returns `DXGI_STATUS_OCCLUDED` while foreground in exclusive mode, and
      Elite throttles or stops. Not logged today after the first Present.
    - **H1b, cross-thread SendMessage deadlock** (was H5): DXGI's mode change, made through the unassociated factory,
      sends to the window's thread while Present holds the render thread. Signature: `stall:` on `win32u`/`user32`
      under `dxgi.dll`; a `came from thread` line.
- **Parked (only if the reporter was in Borderless):** H6, a third-party hook (Smooth Motion, RTSS, overlays) on the new
  D3D12 swap chain. It is per-user, so other users' Borderless reports do not rule it out, but nothing points at it.

**Ruled out by the Borderless reports** (record in the doc as `ruled out:` lines):
- H2, XeLL's sleep blocking on NVIDIA: XeLL sleeps every frame in Borderless too.
- H3, the proxy's Present blocking while foreground (a never-enabled passthrough proxy, tearing/MPO, G-SYNC windowed):
  all present in Borderless, where it works.
- H7, the cross-API handoff stalling: the same handoff runs in Borderless.
- H8, a fault then the fallback: not mode-specific. A fault logs `stops for this session`.
- H9, a long first pipeline compile: explains a slow start, not the alt-tab pattern.

## Intel's "Enabling Frame Generation" rules
Rule 2 (re-enable after leaving exclusive fullscreen) is satisfied: every fullscreen change resets `enabledState`. The
others concern frame generation outside fullscreen, which works, and need no change for this report.

## Steps

### Step 1. Evidence (no build)
1. The reporter's `edvr_gfx_*.log` from the session that froze and their display mode, if they send them (Martin
   handles the contact and the issue).
2. Read a log with `python tools\edvr_log.py --file <log> --expect-build v0.22.3 --version`. H1 is confirmed by the
   fullscreen lines above. Write `ruled out:` lines for what it kills.

### Step 2. Arc repro flight (no NVIDIA needed; can run before the reporter answers)
- **0.22.3 on the Arc B580, Elite in Fullscreen, frame generation on:** start to the main menu, alt-tab out and back.
  A freeze there confirms H1 on Arc and makes the fix testable locally. No freeze: H1 needs the reporter's log.
- **The Step 5 build, same steps**, and the six presenter cases (doc "Next flight").

### Step 3. Instrumentation (only if Step 1 and Step 2 do not decide H1a vs H1b, or the plain chain still hangs)
Fullscreen-relevant only:
1. The window's, creation's and Present's thread ids at `bridge ready` (H1b).
2. A Present HRESULT tally (S_OK, OCCLUDED, other + last code) and foreground counts, as one line every 5 s and at exit
   (H1a).
3. A log line on every foreground change.
4. Arm the stall sampler on the first bridged Present call, not its return, so a first Present that never returns is
   sampled.
5. Optional: a log-only `IDXGIFactory::MakeWindowAssociation` hook on the game factory, to learn Elite's flags (input to
   the H1 fix).
Trace end to end: a log with `bridge ready` and no tally line within 5 s of `first present` means the instrument is
dead; a hang shows the first-Present breadcrumb without `first present`.

### Step 4. The H1 fix: exclusive fullscreen behaves as without the bridge (no frame generation)
Step 5 already moves exclusive fullscreen off the proxy onto EDVR's plain swap chain. That plain chain is still made on
the bridge's own factory, so H1's mechanism (DXGI's focus handling not being Elite's) may remain. Candidates, chosen on
the Step 2 evidence:
- **Chosen (Martin, 2026-11-18): H1-fix A.** Make the plain swap chain on Elite's own factory (the hidden swap chain's
  `GetParent`), so Elite's window association and Alt+Enter handling apply exactly as without the bridge. The creation
  goes straight to the real `CreateSwapChainForHwnd`, past device_hook's creation hook, so it neither re-enters
  `createBridged` (which would release the bridge) nor gets hooked as the game's swap chain. The proxy stays on the
  bridge's factory: XeSS-FG takes a factory, and the proxy is never exclusive.
- Not taken unless A fails a flight: B, mirror Elite's MWA flags onto the bridge's factory (needs Step 3 item 5); C,
  pass OCCLUDED through correctly on the plain chain, including the TEST path (H1a).
Alt+Enter must keep working: it switches to exclusive fullscreen with frame generation off, and back.

### Step 5. Two presenters (implemented, commit 5e83ac0c, unflown)
Elite's swap chain stays on the hidden window; the real window carries one of EDVR's two presenters.
- **Plain** at start: no XeSS-FG, no XeLL, no proxy. Startup and menus present through it.
- **Proxy** the first time frame generation can run (key on, flat, windowed and foreground for 30 presents, XeSS-SR
  resolving on 2 presents in a row). Then it keeps the window; menus, another AA mode or the key off only
  `SetEnabled(0)`.
- **Back to plain for exclusive fullscreen**, Elite's call or DXGI's Alt+Enter; back to the proxy after 30 windowed
  foreground presents.
- **When the bridge exists (decided 2026-11-18): 0.22.3's rule.** Only when the flat profile launches with
  `fix.xess_frame_generation` on **and** AA `xess`. Then the key is live both ways, and the bridge stays until exit,
  also after a switch to another AA mode (frame generation disabled). Without both, no bridge: Elite's swap chain is
  made and presents the usual way, and turning the key on or choosing XeSS takes effect at the next game start.
  Rejected: bridging on AA `xess` alone (key live from any XeSS start), because every XeSS player without frame
  generation would pay a full-frame copy and run on the unflown plain chain; XeSS-SR alone never needs the bridge (it
  upscales inside Elite's frame). Also rejected: bridging on every flat launch (the XeSS-FG DLLs ship with every
  launcher install, so a file check does not narrow it).
- **Overlay:** both rows dimmed and locked in exclusive fullscreen (in 5e83ac0c). Still to write (Step 6): the dim
  and restart hints saying frame generation needs a start with XeSS anti-aliasing.

### Step 6. User-facing documentation
- Overlay dim hint, AA not XeSS: after a bridged start "Needs XeSS anti-aliasing: choose XeSS above to use this.";
  otherwise "Needs XeSS anti-aliasing: choose XeSS above, then restart the game."
- Overlay restart hint (key on, no bridge): "Frame generation is set up as the game starts: takes effect at the next
  game start."
- `edvr.ini` comments for both keys (the panel's tooltip and the installer read them): set up at start only with the
  key on and XeSS-SR; then live both ways and kept until exit; otherwise next game start; off and dimmed in Fullscreen
  or after Alt+Enter.
- `docs/fixes.md`, `docs/settings-menu.md`, the design doc: the same, plus "the bridge stays until the game is closed,
  even if AA is switched away from XeSS".
- PR description: `.agents/plans/xess-fg-blank-screen-pr.md`.

### Dropped (2026-11-18)
- D1 crash marker, D3 slow-call guard, D4 non-Intel refusal: they guarded against H2/H3/H7, which Borderless reports
  rule out.
- The per-call XeLL/copy/Present timing, module census and non-Intel fixes from the old Step 2/4.
- Anything aimed at generating frames in exclusive fullscreen.

## Files to change (remaining)
- `src\d3d11\xess_fg.cpp`: the H1 fix (Step 4); Step 3 instruments if needed.
- `src\d3d11\device_hook.cpp`: let the plain chain's creation on Elite's factory through (H1-fix A); optional MWA hook.
- `src\d3d11\menu.cpp`, `edvr.ini`, `docs\fixes.md`, `docs\settings-menu.md`: Step 6.
- `docs\xess-frame-generation-2026-11-12.md`: `ruled out:` lines for H2, H3, H7, H8, H9; the journal.

## Tests
- Full build on native Windows: `build.bat --wait-lock` by absolute path; read the tail and the receipt.
- Arc flights (Step 2): Fullscreen start, alt-tab, Alt+Enter both ways, and the six presenter cases.
- NVIDIA or AMD: the same Fullscreen steps, by the reporter or another non-Intel user, once Arc passes.
