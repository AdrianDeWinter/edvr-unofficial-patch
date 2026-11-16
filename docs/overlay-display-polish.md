# Overlay display polish

## Status

- **State (2026-11-16):** visual refinement of the shared menu renderer,
  stacked on [PR
  #227](https://github.com/characterecho-sean/ed-unofficial-patch/pull/227) at
  `caad8640b1fe7786246d2d0251bab7042dca30e3`.
- **Scope:** F8 menu, monitor panel, tooltips and frame-rate readouts in flat
  and VR. Settings, hotkeys and profile policy retain their meaning.
- **Validation:** local raster comparisons and mutation checks are complete.
  The initial candidate `da7171ec` passed the full Windows build and package
  workflow. See the [stacked
  PR](https://github.com/AdrianDeWinter/edvr-unofficial-patch/pull/3) for
  validation of the rebased candidate. No in-game or headset flight has been
  performed.
- **Next flight:** inspect the F8 menu and F5 panel against bright and dark
  game scenes, then check VR selection and text at the headset's own scale.

## Visual direction

The overlay is a cockpit instrument. Its primary job is to make settings and
live figures easy to scan while the game remains visible around it. Amber
identifies selection and warnings; ordinary values use soft white.

| Role | Colour |
| --- | --- |
| Panel | `#151E28` |
| Secondary surface | `#23303D` |
| Main text | `#E4EBF0` |
| Secondary text | `#A2B1BD` |
| Structure | `#445767` |
| Selection and warning | `#E8AF67` |

Segoe UI keeps the Windows-native letterforms and the renderer's existing
font-measurement path. Labels share a left edge, values have a consistent right
edge, and a gutter separates the two columns. Restart information occupies a
secondary line with enough height for its text, below aligned primary labels
and values. Graph captions have their own space above the data.

At large VR text sizes, the renderer budgets the visible settings window
against its 2048-pixel raster limit. The existing scroll controls reach the
remaining settings. The ordinary nine-row window is retained at cap heights 14,
34 and 50; the regression also covers cap heights 60 and 80.

The palette stays close to Elite's amber instrument language, with a cooler
backdrop and quieter secondary text. Selection gets a narrow marker and a
subtle fill. Decoration that does not help orientation or legibility is
omitted.

## Verification

![Settings before and after at native 1080p card
size](images/overlay-settings-comparison.png)

![Monitor before and after at native 1080p card
size](images/overlay-monitor-comparison.png)

The before/after comparison uses the production GDI raster with sample menu
content. It checks the overlay at native 1080p card size and at the larger
reference size. It does not demonstrate the game's compositing, headset optics,
or interaction in a live flight.

Local Wine resolves the requested Segoe UI face to Liberation Sans. These
images demonstrate layout and colour; native Windows validation remains the
font and build gate. The initial local layout run had 270 checks and five
failures; its original base `ac4faab6` had 228 checks and the same five
failures. Those failures concern existing heading and Status text fitting with
the substituted font. The initial candidate's native Windows run passed 264
layout checks and 135 WARP checks with zero failures.

After rebasing onto `caad8640`, the local layout run has 385 checks and the
same five font-substitution failures as that base's 343 checks. The inherited
label-fitting test measures the renderer's actual label column, including its
gutter. All 23 labels fit at 720p, 1080p, 1440p and 2160p. Deliberately
narrowing the rendered column while leaving the former width formula unchanged
produces 27 additional label-fit failures.

The added checks cover aligned primary text, full secondary line height,
selectable row bounds, restart/edit/switch transitions, and the large-text
scroll window. Restoring the old badge geometry produces 20 additional
failures; admitting one extra row at the large-text boundary produces two.
