# One F8 menu for flat and VR

## Status

- **State (2026-11-16):** all seven steps built on branch
  `f8-menu-unification`, each with a green full validation build and rigs.
  NOT FLOWN: nothing has run in the game yet. The flight checklist is at the end.
- **Goal:** one settings menu implementation for both profiles, and the VR
  menu's tooling (tooltips, Monitor, FPS readout, Status, Hotkeys, restart
  indication) brought to the flat profile.
- **Work:** seven steps, below, each building green and testable on its own.
  Steps 1 and 2 change nothing a user sees.
- **Out of scope:** a Plugins tab in flat (blocked on add-ons running in flat),
  developer pages in flat, a GPU frame-time graph in flat, a configurable flat
  readout position. Each becomes its own issue.
- **Open questions for the flight:** whether XeSS-FG's `framesPresented`
  matches PresentMon's displayed frames; whether the 1080p card (14 px cap
  height) is large enough; the Monitor and Status pages pass keys to the game,
  so a menu key Elite also binds reaches the game there (as in VR).
- **Next:** fly the checklist, then open the PR.
- **Related:** `docs/settings-menu.md` (the VR menu's design),
  `docs/xess-frame-generation-2026-11-12.md`,
  `docs/design-plugin-architecture-2026-09-30.md`.

## Background

F8 opens EDVR's settings menu in both profiles. In VR it is a panel floating in
the world, aimed at with the head and driven by keyboard or pad. In flat it is
a card in the upper-left corner of the screen. Each writes straight into
`edvr.ini`, so the file stays the single source of truth.

The two menus look like separate implementations, but most of the code is
already shared:

| Part | Shared? |
|---|---|
| Row definitions (generated from the `# ui:` lines in `edvr.ini`) | yes |
| Stepping, typing a value, hotkey capture, writing the ini, restart badges | yes |
| Building the page content (`buildContent`) | yes |
| Rasterising the panel (GDI, on a worker thread) | yes |
| Compositing it (one compute shader; flat passes a screen-shaped view) | yes |
| Configure step (`menuConfigure`) | **no**: two branches |
| Per-frame driver (`menuTick`) | **no**: two branches, ~110 and ~400 lines |
| Which pages exist (`buildPages`) | **no**: flat uses three hand-kept tables |
| Notices | **no**: flat has its own notice card, VR has toasts |
| Panel sizing | **no**: flat is a fixed 860 px card |

The two `menuTick` branches repeat the same steps in different shapes (summon,
Escape, the "panel not drawn" watchdog, keyboard gate, keys and pad, building
and submitting content), and VR adds the rest. That duplication is where the
two menus drift apart, and it is why features built for VR never reached flat.

Flat today has three tabs: Performance, Visual, Debug. VR has Performance,
Fixes, Visual, Mirror window, Native on foot, Hotkeys, Plugins, Monitor and
Status, plus three developer pages.

## What changes for users

Flat players get:

- **Tooltips:** an explanation of the highlighted setting beside the card.
- **Monitor page:** frame rate, frame time, 1% low, CPU time, drops, CPU load,
  RAM, VRAM and GPU figures, with frame-time graphs.
- **FPS readout** while playing, in the top-right corner, in two sizes.
- **Status page:** what is actually running (mode, upscaling, frame
  generation, display mode, wrappers), and Elite's graphics-settings warning,
  which moves here from the Performance tab.
- **Hotkeys page:** rebind the hotkeys the flat profile uses without editing
  the ini.
- **Restart indication:** settings that only apply at the next game start say
  so on their row, in the footer, on the Status page and in a notice when the
  menu closes.
- **Crisper text:** the card is drawn at the screen's own pixel size instead
  of being scaled.

Both profiles get:

- `menu.fps_overlay` becomes `off | compact | detailed` (today: on/off, VR only).
  An existing `on` keeps working and means `compact`.
- **With XeSS Frame Generation, the rendered and the presented frame rate are
  always shown separately**, never as one figure.

## Design

### A profile policy instead of branches

A small, pure description of what differs between the profiles replaces the
`if (flat)` branches:

- where the panel sits: on the screen, or anchored in the world;
- how it is sized: from the output's pixels, or from the headset's pixels per
  degree;
- whether it fades, whether it aims with the head;
- whether Elite's own panel keys are adopted as menu keys;
- where notices appear;
- which pages exist.

`menuTick` and `menuConfigure` each become one body that asks the policy. The
policy lives in its own header with no Windows or D3D dependencies, so a test
rig can drive the open, close, Escape, watchdog and notice sequences for both
profiles. Today no rig compiles `menu.cpp`, so this logic is untested outside a
flight; pulling the decisions into a testable core is part of the point.

### Settings the flat profile may read

The flat profile only reads keys on its allow-list
(`src/common/flat_profile_keys.h`). A key not on the list reads as 0 or `off`,
whatever the file says, and nothing complains. So every `menu.*` key a flat
feature uses must be added to that list in the step that uses it, and the
existing build check that fails on a flat menu row with a refused key is
extended to every flat page.

This matters for notices in particular: flat shows its notices (the AA
comparison key, the concourse window mode) unconditionally today. Once notices
go through the toast path they are governed by `menu.toasts`, which flat cannot
read yet and would therefore see as `off`. Until `menu.toasts` is on the flat
list, the flat policy keeps notices on, so step 1 changes nothing.

### One page table

`buildPages` becomes one loop over a table of pages, each with the profiles it
appears in and where its rows come from: an ini `menu` token, one of the flat
row tables, or a built-in page (Hotkeys, Plugins, Monitor, Status, developer
actions, the aiming prompt). The flat tables themselves stay as they are, so
which rows each flat tab shows does not change.

A side effect fixes a small flat bug: rebuilding the pages keeps the
highlighted row in VR but jumps back to the first row in flat.

### One raster, one thing on screen at a time

The panel has a single raster. The menu, a notice and the FPS readout cannot
be on screen together; the priority is the menu, then a notice, then the
readout. With the menu open, the readout's single line can be shown at the top
of the menu card (`menu.fps_overlay_lock`, as VR does today). A notice briefly
hides the readout. A second raster is possible later if that proves annoying.

### Flat sizing and placement

The flat card was a fixed 860 px raster, composited at a size relative to the
screen. Measured in the layout rig, the Performance tab's raster is 860 x 1106
px and the composite fitted it to 42.5 % of the screen height: 357 px wide at
1080p, a 0.415x resample there and 0.83x at 4K. The flat policy now sizes the
raster from the output's own pixels instead, as VR sizes it from the headset's
pixels per degree, and composites it 1:1. The on-screen size is kept: the card
is 0.3305 of the output height wide (357 px at 1080p, 476 at 1440p, 714 at
2160p), capped at 45 % of the width, with text cap height scaled from 34 px at
860 and never below 14 px. Both fractions are constants in
`src/d3d11/menu_flat_layout.h`. A page taller than 85 % of the output is the
one case still scaled down.

The composite today pins the card to the top-left corner. The readout needs
the top-right corner, so the flat placement gains a corner parameter. Tooltips
widen the bitmap to the right of the card; the card itself keeps its size and
position.

### Rendered and presented frame rate

Under XeSS Frame Generation, Elite's own D3D11 Present runs at the rendered
rate, and EDVR's bridge presents rendered plus generated frames on the real
window. The two are measured separately:

- **Rendered:** the existing frame-time recorder, which the flat profile does
  not feed today. Flat will feed it at its own Present.
- **Presented:** the sum of `framesPresented` from XeSS-FG's per-frame present
  status (`xefg_swapchain_present_status_t`). Intel documents it as the number
  of frames sent to be presented by the last call; whether that equals what
  the display showed is for the flight to compare with PresentMon. When that
  field fails or stays 0 for 30 presents, the count comes from the presenting
  swap chain's DXGI frame statistics (`PresentCount`), which also covers the
  bridge's plain fallback swap chain. The log names the source once. It is
  never computed as rendered x (1 + generated frames): that is what was asked
  for, not what happened.

Display rules:

- Whenever the bridge exists this session, both rates are shown, even with
  frame generation switched off (they are then equal).
- If frame generation stopped, the readout says so and why, instead of showing
  two equal numbers.
- Without a bridge, there is no presented figure at all (not a 0).
- If the presented count cannot be read, it says "unavailable".
- The graph's budget line is the display's refresh period for presented
  frames, and that period times the frame-generation factor for rendered
  frames.

### The FPS readout

`menu.fps_overlay`:

- `off`: nothing (the default).
- `compact`: one line. With the bridge: `render 58 / shown 116 fps  cpu 9.1 ms`
  and drops; without: `58 fps  cpu 9.1 ms` and drops. Refreshed twice a second.
- `detailed`: a small card. Rendered and presented rate; frame time, 1% low,
  CPU time and drops; one frame-time graph (presented overlaid on rendered when
  bridged). Refreshed about four times a second.

The readout uses only the cheap per-frame figures. CPU load, RAM, VRAM and GPU
load stay on the Monitor page, whose samplers run only while that page is
showing. In VR both sizes are head-locked at `menu.fps_overlay_yaw/pitch`, as
today; in flat the readout sits in the top-right corner. The F8 row for it
joins the flat Performance tab, along with "FPS readout in menu"; the VR
position rows do not, as they mean nothing on a screen.

Old values: `on`, `true` and `1` read as `compact`; `off`, `false` and `0` as
`off`. The menu writes the new words. The installer does not write this key.

### Restart indication

A setting marked `restart` in `edvr.ini` already gets a badge, and once
changed shows `old -> new` and "at next launch". That works in flat too. What
is missing:

- **Settings that only sometimes need a restart.** XeSS Frame Generation is
  live once its bridge exists, but the bridge can only be made as the game
  starts: turning it on, or choosing XeSS-SR, after a launch without it needs a
  restart. Today only the highlighted row's hint says so. A table of runtime
  conditions, one entry per such setting, marks a row as waiting for a restart
  exactly when its current value cannot take effect until then. When frame
  generation failed, the row says "stopped" and why instead, since a restart
  will not fix that.
- **Flat footer:** the count of changes waiting for a restart (VR's footer
  already shows it).
- **Notice on close:** one when the menu closes with changes still waiting
  ("2 changes apply at next game start"), in both profiles, except when a
  profile switch closed it. There is no notice per change: a change made in the
  menu is made with the menu open, where the row and the footer already say it,
  and notices only show once it is closed. The log has one line per change.
- **Flat notices follow `menu.toasts`**, as VR's toasts do (default on).
- **Status page:** "Restart pending" with the settings' names.

### The flat Status page

ED-UP version; profile; display (size, refresh rate, borderless or exclusive
fullscreen); anti-aliasing (requested and running mode, model, DLSS or DLAA);
upscaling (internal to output size); frame generation (off, on with its
factor, stopped and why, or at next launch); sharpening; HUD quality; a
graphics wrapper such as ReShade if one handles the game's calls; keyboard
gate; Elite's graphics-settings warning; changes waiting for a restart; last
write. The Performance tab keeps a one-line pointer while the warning is
active. As on VR's Status page, keys pass through to the game.

### The flat Hotkeys page

The VR page's everyone-visible rows, filtered to the hotkeys the flat profile
reads: today the menu key and the AA comparison key. Capture and the pad rules
are shared. Clash badges against Elite's bindings are not shown in flat: the
flat profile does not read Elite's bindings (`hotkey.read_game_bindings` is
not on its list), and turning that on is a separate decision. Temporary
investigation hotkeys have no menu rows, so they stay off.

## Steps

Each step builds green and can be tested on its own. A "flight" is a test
session in the game.

1. **One tick, one configure.** The profile policy, the merged `menuTick` and
   `menuConfigure`, flat notices on the toast path. No visible change; the log's
   `menu:` lines keep their wording. Before merging, record the current open,
   close, Escape, watchdog and notice behaviour in a rig, and keep it passing.
   *Flights:* flat (open, change a setting, AA comparison notice, close, switch
   VR to flat live) and VR (open, aim, toast).
2. **One page table.** No visible change except the kept highlight in flat. A
   rig pins each profile's pages and rows in order.
3. **Restart indication.** The conditional-restart table, the flat footer
   count, the notices, the Status line. `menu.toasts` joins the flat allow-list
   here. A rig covers every case of the XeSS conditions and the footer text.
   *Flight:* flat, XeSS FG turned on without a bridge, then after a restart.
4. **Flat layout: crisp sizing and tooltips.** Raster from output pixels,
   tooltip strip, `menu.tooltip_delay` on the allow-list. *Flight:* flat at
   1080p and 4K.
5. **Monitor and FPS readout.** Flat feeds the frame recorder; the presented
   count from the bridge; the Monitor page in flat; `menu.fps_overlay` gains its
   three values in both profiles. A rig covers the counter, its wrap, a fallback
   bridge and no bridge, the old values' parsing, and the detailed card's layout
   at flat and VR sizes. *Flights:* flat on an Intel Arc (2x and 3x) and on a
   non-Intel GPU, frame generation on and off, compared with PresentMon's
   displayed-frames figure; VR with the detailed card.
6. **Flat Status page**, with the graphics-settings warning moved to it.
7. **Flat Hotkeys page.**

Not in this work, each its own issue: a Plugins tab in flat (needs add-ons to
run in flat first; with step 2 it becomes one row of the page table), developer
pages in flat, a GPU frame-time graph in flat (needs GPU timing of its own), a
configurable flat readout position.

## Constraints

- Every setting a flat page shows must be on the flat allow-list; the build
  fails otherwise.
- No new settings for experiments. The `menu.*` keys already exist and gain a
  meaning in flat; their names describe what the user gets.
- The panel holds at most 23 lines (raised from 21 for the two readout rows on
  the flat Performance tab) and its raster at most 2048 px high. The flat
  Status page with Elite's settings warning needs all 23. The flat Monitor page
  is 1103 px tall at 2160p and would pass 2048 px near 8K output height.
- New log lines name where each figure came from, so a log shows which counter
  was read.

## Flight checklist

Steps 1 to 7 are built and rig-tested, not flown. One flat session and one VR
session cover most of it; the XeSS checks need an Intel Arc and, for the 1x
case, a non-Intel GPU.

Flat:
- F8 opens and closes, Escape closes, a value can be stepped and typed, the
  AA comparison key's notice appears top-left, a live switch VR to flat works.
- Text is crisp at 1080p and at 4K; no row is cut off on any tab at 1080p.
  Resizing the window rebuilds the card.
- Tooltips appear after the delay, beside the card, and scroll with
  PageUp/PageDown, on keyboard and on pad.
- Monitor page: rendered and presented rates with frame generation at 2x, 3x
  and off, compared with PresentMon's displayed frames; the refresh rate and
  budget lines are right; the log names the presented source.
- `menu.fps_overlay` compact and detailed in the top-right corner, giving way
  to the menu and to notices without a stale frame flashing.
- Status page lines (display, anti-aliasing running or paused, upscaling
  sizes, frame generation) match the session; Elite's settings warning shows
  there and the Performance tab shows the one-line pointer.
- XeSS FG turned on without a bridge: "at next launch" on the row, the footer
  count, the notice on close; after a restart the rows are live.
- Hotkeys page: rebind the menu key and the AA comparison key.
- Whether the new flat long-frame log lines are too frequent.

VR:
- The menu, toasts and aiming behave as before (steps 1 and 2 must not have
  changed anything).
- `menu.fps_overlay` compact (an old `on`) and detailed, head-locked.
- The close notice when a restart setting changed.
