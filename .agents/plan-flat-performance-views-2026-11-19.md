# Plan: flat performance views (monitor panel variants, independent menu line)

2026-11-19. Scope: the flat profile's performance displays, plus the menu-line
setting in both profiles.

**Implemented and flown (2026-11-19, PR #241). This plan is closed: do not
act on it.** The landed behaviour is the 2026-11-19 journal entry in
`docs\design-f8-menu-unification-2026-11-15.md`. The section below records
the behaviour before the change, which no longer exists.

## What happened before (removed 2026-11-19)

Three things share the flat top-right "monitor slot" and the menu card:

| Input | Where read | Effect |
|---|---|---|
| `menu.fps_overlay` = off / compact / detailed ("Frame-rate readout") | `menuReadFpsOverlayKeys`, menu.cpp:5116 | flat: a corner readout, top right, shown whenever the menu card is not carrying it and the monitor panel is down |
| `menu.fps_overlay_lock` ("FPS readout in menu") | same | the open menu card gets the compact line at its top (menu.cpp:2606), **only if `fps_overlay` is not off**; the corner readout is then suppressed while the menu is open |
| `hotkey.monitor_panel` (F5) | menu.cpp:5678 | the full Monitor panel, same slot; session state, never written |

The rule that ties them is one line, `flatReadoutWanted` (menu_policy.h:188):
`readoutOn && !monitorPanelUp && !(menuOpen && lockedIntoMenu)`, plus the
`s.overlay && s.overlayLock` gate on the menu line (menu.cpp:2606, 5964).

So the user-visible rules are: the corner readout hides under the panel; the
menu line replaces the corner readout while the menu is open; and the menu line
silently does nothing when the readout is off. Three settings each change the
meaning of another. The readout and the panel also run two `ScreenReadoutState`
machines that hand one slot back and forth by serial (menu.cpp:5861-5900).

## Target behaviour

1. **F5 is the only thing that puts a performance view on the flat screen.**
   The monitor panel gets a **view** setting: `full` (today's panel),
   `detailed` (today's detailed readout card), `compact` (today's one line).
   F5 toggles whichever view is chosen, top right. The view is a config key,
   so it survives F5 presses and restarts. Up/down is session state and
   **must not** be stored: every session starts with the panel down
   (decided 2026-11-19).
2. **Nothing appears in the corner without F5.** The always-on flat corner
   readout goes away as a separate thing: it *is* the panel in its compact or
   detailed view.
3. **The menu line is wholly independent.** `fps_overlay_lock` on puts the
   compact line at the top of the open menu card whatever else is set, in flat
   and VR. It never hides or shows anything else.
4. **VR keeps `menu.fps_overlay`** as its head-locked readout (VR has no panel
   and no F5); only its wording changes. Flat stops reading it.

## Settings

Decided 2026-11-19: the flat view gets its own key, `menu.monitor_panel_view`,
with its own `# ui:` line, label, tooltip and choices; `menu.fps_overlay`
becomes VR only.

| Key | Profile | Values | UI label | Change |
|---|---|---|---|---|
| `menu.monitor_panel_view` (new) | flat only | `full` (default) / `detailed` / `compact` | **Monitor panel view** | new |
| `menu.fps_overlay` | VR only | off / compact / detailed | **Frame-rate readout** (unchanged) | today read in both; removed from the flat allow-list and flat Performance tab. Its behaviour in VR does not change, so its label does not either |
| `menu.fps_overlay_lock` | both | on / off | **Frame rate in menu** | stops depending on `fps_overlay` |
| `hotkey.monitor_panel` | flat only | key | **Monitor panel key** (unchanged) | description names the view setting |

Descriptions (draft). The view row's description is also its tooltip
(the schema text feeds both), and it **must** say that the setting only
shapes the monitor panel and that nothing changes on screen until the panel
is up, naming the key. Changing the view with the panel down is the case a
user will hit first, and the tooltip is where they will look. If the bound
key can be substituted into the tooltip at run time (check how the menu
renders `# ui:` descriptions), name the live binding; otherwise say "the
monitor panel key (F5 by default, on the Hotkeys page)".

Flat-facing text must be clear and concise (2026-11-19): the tooltip is
the first lines of the description, so the essentials go first and the
detail after.

- *Monitor panel view*: "How much the monitor panel shows. Open and close
  the panel with its key (F5 by default); while it is closed, this changes
  nothing on screen. full: all gauges and graphs. detailed: frame rates,
  frame times, CPU time, drops and a graph. compact: one line of frame rate,
  CPU time and drops. full | detailed | compact. Live."
- *Frame rate in menu*: "A one-line frame rate at the top of the F8
  menu while it is open. on | off. Live."
- *Monitor panel key*: append "What it shows is set by Monitor panel view."
- *Frame-rate readout* (`menu.fps_overlay`, VR): label kept. Its `edvr.ini`
  comment loses only the sentences about the flat screen and the monitor
  panel, which stop being true; the rest is untouched.

`fps_overlay_lock` keeps its key; only its label and description change
(decided 2026-11-19). It is the one relabel that also shows in VR, and it is
justified there because its behaviour changes in both profiles (it no
longer depends on the readout being on).

**No migration** (decided 2026-11-19): every user starts on `full`,
whatever their old flat `fps_overlay` said, and picks a smaller view
themselves. Flat users who had the always-on corner readout lose it; one F5
press brings back a performance view. The release notes say so.

## Code steps

1. **menu_policy.h**: delete `flatReadoutWanted`. Add a pure
   `MonitorPanelView parseMonitorPanelView(const char*)` (in
   `src\common\fps_overlay_mode.h` beside `parseFpsOverlay`, or its own
   header), default `full`, unknown values read as `full`.
2. **menu.cpp configure**: `menuReadFpsOverlayKeys` reads `fps_overlay` only
   when `!pol.monitorPanel` (VR) and reads `monitor_panel_view` when it is
   (flat). `monitorPanelOpen` stays unwritten (menu.cpp:374, 5004, 5212 keep
   it session state). Log a change of view (`menu: the monitor panel view
   is compact.`); a view change while the panel is up sets `monitorTextMs = 0`
   so the next tick rebuilds at once.
3. **menu.cpp tick, flat**: remove the readout block (5866-5882) and the
   `flatReadout` / `readoutSized` state. The monitor-panel block builds by view:
   `full` -> `buildMonitorPanel`, `detailed`/`compact` ->
   `buildReadoutContent(mc, mode, true, 0)`; refresh cadence is
   `kMonitorRefreshMs` / 250 ms / 500 ms respectively; size tracking moves to
   `monitorSized`. One `ScreenReadoutState`, one slot owner
   (`kSlotOwnerReadout` retires or becomes an alias). Check menu.cpp:5424 and
   6029, which test `flatReadout.compositing()` and the slot owner.
4. **Samplers**: `perfMonitorSetActive` (menu.cpp:5484) turns the slow
   once-a-second samplers on only for the `full` view; compact and detailed
   use the cheap per-frame figures, as the readout does today.
5. **Menu line**: drop `s.overlay &&` from menu.cpp:2606 and 5964, and from
   the dirty checks at 5126/5133, so `overlayLock` alone decides it. In VR,
   decide whether the head-locked readout still hides while the menu is open
   (it does today, independent of the lock); keep that as is.
6. **Schema and rows**: `edvr.ini` comments and `# ui:` lines for the three
   keys plus the new one (Performance tab, flat); `menu_flat_rows.h` and
   `flat_profile_keys.h`: add `monitor_panel_view`, drop `fps_overlay` from
   flat (and from `fps_overlay_yaw/pitch` if they are listed). Regenerate
   `build\gen\menu_schema.inc` through the build. The hotkey's description
   points at the view row.
7. **Rigs**: `tools\menu_policy_test` (drop `flatReadoutWanted` cases, add
   view parse and default cases), `tools\native_perf_history_test` (parse),
   `tools\flat_sharpen_test` (it references one of these symbols; adjust).
   The flat allow-list gate in the build will catch a row/list mismatch.
8. **Docs**: `docs\design-f8-menu-unification-2026-11-15.md` — update the
   Status block, "One raster", "The FPS readout" and the 2026-11-16 journal's
   Readout bullet; add a dated journal entry. `docs\settings-menu.md` if it
   lists the rows.
9. Full `build.bat --wait-lock` by absolute path; read the tail.

## Flight checklist (flat)

- F5 with each view: full, detailed, compact; change the view on the
  Performance tab while the panel is up, the view swaps within one refresh.
- F5 down, restart the game: the view is still the chosen one; the panel
  starts down.
- Menu line on with every view and with the panel up and down: it appears in
  the menu header and nothing else moves.
- Old `edvr-flat.ini` with `fps_overlay = compact`, no view key: F5 shows
  `full`; nothing appears in the corner without F5.
- Change the view with the panel down: nothing changes on screen, and the
  row's tooltip says why and names the key.
- Log lines to look for: `menu: the monitor panel is up (F5).`, the new view
  line, `menu panel: first monitor panel composite`.

## Decisions (2026-11-19)

- Panel up/down is not stored, and must not be.
- Everyone starts on `full`; no seeding from an old `fps_overlay`.
- `menu.fps_overlay_lock`: label and description only, no key rename.
- The view row's tooltip explains it only shapes the monitor panel and
  names the key, so a change with the panel closed is not a mystery.

- **A dedicated flat key** (2026-11-19): `menu.monitor_panel_view`, full |
  detailed | compact, default `full`, with its own `# ui:` line, label
  ("Monitor panel view"), tooltip and choices. Keeping the shared
  `menu.fps_overlay` was considered and dropped: one `# ui:` line per key
  (`tools\gen_settings_schema.py` has no per-profile override) would have
  given VR and flat one label, tooltip and choice list, and old flat or VR
  values would have carried over, against "everyone starts on full".
  Consequences for the steps above:
  - The new key's `# ui:` line uses the `menu flat_performance` token (as
    `fix.hud_antialiasing` does), so only the flat Performance tab shows it;
    it is listed in `menu_flat_rows.h` and the flat allow-list, and in
    `flatProfileOnlyKey` (`flat_profile_keys.h`) if VR must not read it.
  - `menu.fps_overlay` leaves `menu_flat_rows.h` and the flat allow-list and
    is VR only; its row stays on VR's Performance page with its label
    unchanged ("Frame-rate readout").
  - Since nobody has the new key yet, every flat user starts on `full`; an
    old flat `fps_overlay` line is simply no longer read.

## Still open

Nothing. Implemented; see the note at the top.
