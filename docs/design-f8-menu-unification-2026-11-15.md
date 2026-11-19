# One F8 menu for flat and VR

## Status

- **State (2026-11-16):** all seven steps and the flat monitor panel are built
  on branch `f8-menu-unification`, each with a green full validation build and
  rigs. Flown in part: the flat profile four times by the PR author, VR once by
  the maintainer's reviewer (see the flight journal). The checklist at the end
  says what is still open.
- **Flown, 2026-11-16:** flat four times by the PR author (Status page, Hotkeys
  page, tab titles, row labels and Monitor graph labels fixed in turn), VR once
  by the maintainer's reviewer (new look accepted; menu tick cost measured
  higher, font measurements now cached). Details in the journal.
- **Not flown:** XeSS Frame Generation on (presented against rendered rate,
  against PresentMon) and the flat Monitor panel together with the menu on
  screen.
- **Goal:** one settings menu implementation for both profiles, and the VR
  menu's tooling (tooltips, Monitor, FPS readout, Status, Hotkeys, restart
  indication) brought to the flat profile.
- **Changed on purpose in VR:** the raster is shared, so VR's panel looks
  different. Listed under "What changes in VR".
- **Comparability:** flat LONG FRAME counts before and after this branch are
  not comparable (the budget is now refresh-derived, journal entry below).
- **Out of scope:** a Plugins tab in flat (blocked on add-ons running in flat),
  developer pages in flat, a GPU frame-time graph in flat, a configurable flat
  readout position. Each becomes its own issue.
- **Open questions for the flight:** whether XeSS-FG's `framesPresented`
  matches PresentMon's displayed frames; whether the 1080p card (14 px cap
  height) is large enough; the Monitor and Status pages pass keys to the game,
  so a menu key Elite also binds reaches the game there (as in VR); whether F5
  is the right default for the monitor panel key; the GPU price of the second
  full-frame composite while the menu and the panel are both up.
- **Changed 2026-11-19 (journal):** flat no longer has a separate corner
  readout. The monitor panel (F5) has three views, `menu.monitor_panel_view`
  = full | detailed | compact (flat only, default full); `menu.fps_overlay` is
  VR only; `menu.fps_overlay_lock` ("Frame rate in menu") works on its
  own in both profiles. Flown 2026-11-19 in flat (the three views, the menu
  line, the view kept across restarts).
- **Next:** fly the open checklist items (XeSS FG, monitor panel with the
  menu), then merge.
- **Related:** `docs/settings-menu.md` (the VR menu's design),
  `docs/xess-frame-generation-2026-11-12.md`,
  `docs/design-plugin-architecture-2026-09-30.md`.

## Background

F8 opens EDVR's settings menu in both profiles: in VR a panel floating in the
world, in flat a card in the upper-left corner. Each writes into `edvr.ini`. Most
of the code was already shared (row definitions from the `# ui:` lines, stepping,
typing, hotkey capture, restart badges, `buildContent`, the GDI raster, the one
composite shader). Not shared were the configure step, the per-frame driver
`menuTick` (~110 and ~400 lines), the page list, notices and panel sizing. That
duplication is where the menus drifted apart. Flat had three tabs (Performance,
Visual, Debug); VR has nine plus developer pages.

## What changes for users

Flat players get tooltips beside the card; the Monitor (frame rate and time, 1% low,
CPU time, drops, CPU load, RAM, VRAM, GPU, graphs; a panel of its own, see the
journal); an FPS readout top right in two sizes; a Status page (what is actually
running, plus Elite's graphics-settings warning, moved from the Performance tab);
a Hotkeys page; restart indication (row, footer, Status page, notice on close);
and crisper text, drawn at the screen's own pixel size.

Both profiles get `menu.fps_overlay` as `off | compact | detailed` (before:
on/off, VR only; an existing `on` still works and means `compact`), and with
XeSS Frame Generation the rendered and the presented frame rate are always
shown separately, never as one figure.

## Design

### A profile policy instead of branches

A small, pure description of what differs between the profiles replaces the
`if (flat)` branches: where the panel sits (screen or world), how it is sized
(output pixels or the headset's pixels per degree), whether it fades or aims
with the head, whether Elite's own panel keys are adopted as menu keys, where
notices appear, and which pages exist. `menuTick` and `menuConfigure` are each
one body that asks the policy. The policy lives in its own header with no
Windows or D3D dependencies, so a test rig drives the open, close, Escape,
watchdog and notice sequences for both profiles; before this, no rig compiled
`menu.cpp`.

### Settings the flat profile may read

The flat profile only reads keys on its allow-list
(`src/common/flat_profile_keys.h`). A key not on the list reads as 0 or `off`,
whatever the file says, and nothing complains. So every `menu.*` key a flat
feature uses is on that list, and the build check that fails on a flat menu row
with a refused key covers every flat page. Notices go through the toast path
and so follow `menu.toasts` (default on), which is on the flat list.

### One page table

`buildPages` is one loop over a table of pages, each with the profiles it
appears in and where its rows come from: an ini `menu` token, one of the flat
row tables, or a built-in page (Hotkeys, Plugins, Monitor, Status, developer
actions, the aiming prompt). The flat tables stay as they were, so which rows
each flat tab shows did not change. A side effect fixes a flat bug: rebuilding
the pages keeps the highlighted row in flat as it already did in VR.

### One raster, one thing on screen at a time

The panel has a single raster. The menu and a notice cannot be on screen
together; the menu has priority. With the menu open, a one-line frame rate can
be shown at the top of the menu card (`menu.fps_overlay_lock`, its own switch
since 2026-11-19, as in VR). The flat monitor panel, in any of its views
(`menu.monitor_panel_view`), is a second raster (its own slot in
`menu_panel.cpp`); flat has no separate corner readout since 2026-11-19.

### Flat sizing and placement

The flat card was a fixed 860 px raster composited at 42.5 % of the screen height
(a 0.415x resample at 1080p). The flat policy now sizes the raster from the
output's pixels and composites it 1:1, keeping the on-screen size: 0.3305 of the
output height wide (357 px at 1080p, 714 at 2160p), capped at 45 % of the width,
text cap height scaled from 34 px at 860, never below 14 px (constants in
`src/d3d11/menu_flat_layout.h`). A page taller than 85 % of the output is scaled
down. The composite takes a corner (card top-left, readout top-right); tooltips
widen the bitmap to the right without moving the card.

### Rendered and presented frame rate

Under XeSS Frame Generation, Elite's Present runs at the rendered rate and EDVR's
bridge presents rendered plus generated frames. Rendered comes from the
frame-time recorder (flat now feeds it at its own Present). Presented is the sum
of `framesPresented` from XeSS-FG's present status; whether it equals what the
display showed is for the flight to compare with PresentMon. If the field fails or
stays 0 for 30 presents, it falls back to the swap chain's DXGI `PresentCount`,
and the log names the source once. It is never computed as rendered x (1 +
generated); that is what was asked for, not what happened.

With a bridge both rates are shown (equal with frame generation off); if frame
generation stopped, the readout says so and why; without a bridge there is no
presented figure; an unreadable count says "unavailable". The graph's budget
line is the refresh period for presented frames, times the frame-generation
factor for rendered frames.

### The FPS readout

`menu.fps_overlay`: `off` (default); `compact`, one line (`render 58 / shown 116
fps  cpu 9.1 ms` with the bridge, `58 fps  cpu 9.1 ms` without, plus drops, twice a
second); `detailed`, a small card with both rates, frame time, 1% low, CPU time,
drops and one graph (about four times a second). Only cheap per-frame figures; CPU
load, RAM, VRAM and GPU stay on the Monitor page. In VR both sizes are
head-locked at `menu.fps_overlay_yaw/pitch`. In flat (since 2026-11-19) the two
sizes are the monitor panel's detailed and compact views, shown only with F5
(`menu.monitor_panel_view`, journal 2026-11-19).
Old values `on`/`true`/`1` read as `compact`, `off`/`false`/`0` as `off`.

### Restart indication

A `restart`-marked setting gets a badge and, once changed, `old -> new` and "at
next launch", in flat too. Added: a table of runtime conditions for settings that
only sometimes need a restart (XeSS Frame Generation and XeSS-SR need the bridge,
which is made only as the game starts; a failed frame generation says "stopped"
and why instead); the flat footer's count of changes waiting; one notice when the
menu closes with changes waiting (both profiles, not after a profile switch; none
per change, the log has a line each); and "Restart pending" on the Status page.

### The flat Status page

ED-UP version; profile; display; anti-aliasing (requested and running mode);
upscaling sizes; frame generation (off, on with its factor, stopped and why, or
at next launch); sharpening; HUD quality; a graphics wrapper such as ReShade;
keyboard gate; Elite's graphics-settings warning (the Performance tab keeps a
one-line pointer); changes waiting for a restart; last write. As in VR, keys pass
through to the game.

### The flat Hotkeys page

The VR page's everyone-visible rows, filtered to the hotkeys the flat profile
reads: the menu key, the AA comparison key and the monitor panel key (which only
flat reads). Capture and the pad rules are shared. Clash badges against Elite's
bindings are not shown in flat: it does not read Elite's bindings
(`hotkey.read_game_bindings` is not on its list), and turning that on is a
separate decision. Temporary investigation hotkeys have no rows.

## What was built

Seven steps, each green with rigs: one tick and one configure through the profile
policy; one page table; restart indication; flat layout (crisp sizing,
tooltips); Monitor and the FPS readout (rigs cover the counter, its wrap, a
fallback bridge, no bridge, old value parsing, the detailed card at flat and VR
sizes); the flat Status page; the flat Hotkeys page. Later: the monitor panel,
the long-frame budget and the layout polish below.

## Constraints

- Every setting a flat page shows must be on the flat allow-list; the build
  fails otherwise.
- No new settings for experiments. The `menu.*` keys already exist and gain a
  meaning in flat; their names describe what the user gets.
- The panel holds at most 23 lines (raised from 21 for the two readout rows on
  the flat Performance tab) and its raster at most 2048 px high. The flat Status
  page with Elite's settings warning needs all 23. The flat monitor panel is as
  tall as the Monitor page was, 1103 px at 2160p, and would pass 2048 px near 8K
  output height.
- New log lines name where each figure came from.

## Journal

### 2026-11-16: the monitor panel

In flat, the Monitor tab leaves the F8 menu and becomes a panel of its own, so
the frame-rate gauges can stay up while the game is played.

- **Shown by** `hotkey.monitor_panel`, default F5, a row of the flat Hotkeys
  page. Flat only: VR keeps its Monitor tab, does not read the key, and its
  Hotkeys page and duplicate check do not list it (`flatProfileOnlyKey` in
  `src/common/flat_profile_keys.h`). F5 because F2 to F9 are bound in no stock
  Elite scheme (`docs/settings-menu.md`) and F5 was Explorer Cam's, retired.
- **Independent of the menu.** Either can be up alone or both together. The
  panel never takes the keyboard or the pad and has no rows; the key is
  watched, not captured. Up or down is session state, never written.
- **Placement:** top right, the menu card's width and text height, 1:1; the menu
  card and its tooltip strip stay clear of it at 720p to 2160p.
- **A second raster:** its own pending content, raster, texture, serial and
  geometry in the shared worker. The flat composite draws the menu's slot, then
  the monitor's over it; only the menu's slot advances the menu's heartbeat.
  While both are up the composite runs twice a Present.
- **Readout:** `menu.fps_overlay` is not drawn while the panel is up and comes
  back when it goes down. The once-a-second samplers run while it is up.
  (Superseded 2026-11-19: flat has no corner readout; the readout is a view of
  the panel, and the samplers run for its full view only.)
- **Log:** `menu: monitor panel key F5 (hotkey.monitor_panel).` at configure,
  `menu: the monitor panel is up (F5).` / `down` on each press, and `menu panel:
  first monitor panel composite -- ...` once. A flight with none of these ran a
  build without the panel.

### 2026-11-16: flat long frames use a refresh-derived budget

Flat now logs long frames against `budgetNow`, the display period times
(1 + generated frames), instead of a fixed 11.1 ms. At 60 Hz the drop threshold
is 33 ms, at 144 Hz 13.9 ms. Flat LONG FRAME counts are therefore not
comparable between builds before and after this branch. Whether the new flat
long-frame log lines are too frequent is still a flight question.

### 2026-11-16: layout polish by Fenris159

Contributor Fenris159 refined the shared menu renderer (F8 menu, monitor panel,
tooltips, readouts, flat and VR) without changing what any setting, hotkey or
profile policy means. What shipped and still describes the code:

- Labels share a left edge, values a consistent right edge, with a gutter
  between the columns. Restart information has its own secondary line, tall
  enough for its text, below the aligned label and value. Graph captions have
  their own space above the data. Selection is a narrow marker and a subtle
  fill; decoration that does not help orientation is omitted. Segoe UI stays,
  with the renderer's existing font-measurement path.
- At large VR text sizes the renderer budgets the visible settings window
  against the 2048 px raster limit and the scroll controls reach the rest. The
  ordinary nine-row window holds at cap heights 14, 34 and 50; the regression
  also covers 60 and 80.
- Checks added: aligned primary text, full secondary line height, selectable
  row bounds, restart/edit/switch transitions, the large-text scroll window.
  The inherited label-fitting test measures the real label column including its
  gutter; all 23 labels fit at 720p, 1080p, 1440p and 2160p. Restoring the old
  badge geometry fails 20 more checks, admitting one extra row at the large-text
  boundary fails two, and narrowing the label column fails 27 more.
- The colours first proposed (a dark blue panel with an amber accent) were
  later replaced by the grey and white palette described under "What changes in
  VR"; the comparison images in `docs/images/` (`overlay-settings-comparison.png`,
  `overlay-monitor-comparison.png`) show the amber version and are not
  referenced here.

### 2026-11-16: What changes in VR

VR shares the raster, so its panel changed on purpose; the maintainer accepted
this after a VR flight.

- Grey and white palette with a 0.10 highlight, instead of the orange accent and
  the 0.22 highlight.
- A row with a restart badge (or reserving one) is about 1.5 rows tall, because
  the badge has its own line.
- Setting labels are cut half a cap earlier.
- Monitor graph legends are plain titles ("GPU", "CPU"); the "last N frames;
  the line is the X ms budget" text and the native "predicted period" text are
  gone. The strips are about a third shorter.
- Tab names are measured, and the tab face shrinks toward a 0.9-cap floor when
  the tabs do not fit. VR has 9 to 13 tabs, so its tab text is about 30 %
  smaller.
- A Pimax at the default text size may show one row fewer (9 to 8).

### 2026-11-16: Flat flights

The PR author flew flat four times on successive builds, each flight's findings
fixed in the next commit: (1) build `efe90e2f`: frame-time graph labels too long
(now plain titles), stats wanted as their own panel; (2) the Status page's
captions and values all cut off (now a measured caption column in the note face,
wrapped values); (3) build `82f1f5f6`: Performance and Debug tab titles
ellipsised (tab widths measured, tab face shrinks to fit), two Hotkeys rows ended
in "(e..." (labels shortened), the Hotkeys footer said Left/Right change (now
Enter rebind, naming the capture keys); (4) build `ac4faab6`: "XeSS Frame
Generation" and "Skip rooms behind doors" ellipsised (now "Frame generation"
and "Skip closed rooms"; `native_menu_test` measures every flat row label).

## Flight checklist

Flown 2026-11-16 means seen in the flights above; everything else is open.
The XeSS checks need an Intel Arc and, for the 1x case, a non-Intel GPU.

Flat:
- Flown 2026-11-16: the Status page, the Hotkeys page, the tab titles, the row
  labels and the Monitor graph labels read in full after the fixes above.
- Open: F8 opens and closes, Escape closes, a value can be stepped and typed,
  the AA comparison key's notice appears top-left, a live switch VR to flat.
- Open: text is crisp at 1080p and at 4K; resizing the window rebuilds the card.
- Open: tooltips appear after the delay, beside the card, scroll with
  PageUp/PageDown, on keyboard and on pad.
- Open, monitor panel (F5) together with the menu: the F8 menu has no Monitor
  tab; F5 shows the panel top right and hides it with the menu open and closed;
  walking, flying and the game's own menus work with it up (no key is taken);
  the CPU, GPU, VRAM and RAM tiles fill; resizing rebuilds it; no stale card
  flashes when it comes up. The log has the three lines of the monitor entry.
- Open, XeSS Frame Generation on: rendered and presented rates at 2x, 3x and
  off, compared with PresentMon's displayed frames; the refresh rate and budget
  lines are right; the log names the presented source.
- Retired 2026-11-19: the flat corner readout (`menu.fps_overlay` compact and
  detailed top right) is gone. Its replacement, the monitor panel's detailed
  and compact views, was flown 2026-11-19.
- Open: F5 pressed twice quickly (down and up within a quarter second) draws
  the panel at once, in every view (PR #241 review fix).
- Open: Status page lines (display, anti-aliasing, upscaling sizes, frame
  generation) match the session; Elite's settings warning shows there and the
  Performance tab shows the one-line pointer.
- Open: XeSS FG turned on without a bridge: "at next launch" on the row, the
  footer count, the notice on close; after a restart the rows are live.
- Open: Hotkeys page rebinds the menu key, the AA comparison key and the monitor
  panel key; clearing the monitor panel key hides the panel.
- Open: whether the new flat long-frame log lines are too frequent.

VR:
- Flown 2026-11-16 (reviewer): the Performance and Monitor pages and the new
  look at 90 Hz.
- Open: toasts and aiming behave as before; `menu.fps_overlay` compact (an old
  `on`) and detailed, head-locked; the close notice when a restart setting
  changed; the Hotkeys page has no monitor panel key row.

### 2026-11-19: the monitor panel's views; the menu-header line on its own

Before, three settings changed each other's meaning in flat: the corner
readout (`menu.fps_overlay`) hid under the monitor panel, the menu-header
line (`menu.fps_overlay_lock`) replaced the corner readout while the menu was
open, and that line did nothing at all while the readout was off. The rule
was one function, `flatReadoutWanted` (menu_policy.h), now removed.

- **One performance view in flat, on F5.** `menu.monitor_panel_view` = full
  (the Monitor page, the default) | detailed (the readout card) | compact (the
  readout line). F5 shows and hides whichever is chosen, top right. The view
  is a setting, so it survives F5 presses and restarts; whether the panel is
  up is still session state and is never written. Nothing appears in the
  corner without F5.
- **No migration.** Flat users who had the corner readout on lose it; F5
  shows the full panel until they pick a smaller view. A flat
  `menu.fps_overlay` line is no longer read.
- **One raster machine.** The readout's own `ScreenReadoutState` and the
  `kSlotOwnerReadout` tag are gone: the panel in any view is the slot's only
  owner. Cadence per view: full on the Monitor page's 250 ms, detailed 250 ms,
  compact 500 ms; a view change rebuilds at once.
- **Samplers.** The slow once-a-second samplers run only for the full view;
  detailed and compact read the cheap per-frame figures, as VR's readout does.
- **The menu-header line** is its own switch in both profiles: it no longer
  needs `menu.fps_overlay` on and hides nothing. Its row is now labelled
  "Frame rate in menu"; the key is unchanged.
- **VR:** `menu.fps_overlay` keeps its label and behaviour; only its
  description lost the flat sentences.
- **Log:** `menu: the monitor panel view is full (menu.monitor_panel_view).` at
  configure and on each change; `menu: the frame-rate line at the top of the
  menu is on.` / `off` on a change. A flat flight without the view line ran a
  build without this change.
- **Flown 2026-11-19 (flat):** the three views, the menu line on its own, the
  view kept across a restart.
- **Review fix (2026-11-20, PR #241):** a raise of the panel within 250 ms of
  its last raster drew nothing until that window ran out: the failed-build
  backoff was measured from the last raster, which F5 lowering the panel did
  not reset. The refresh clock is now cleared while the panel is down, so a
  raise builds at once; the backoff still paces a failing build while it is
  up. Not flown.
