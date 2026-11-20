// The F8 menu's tab strip: the face the tab names are drawn in, shrunk from the panel's own tab face until every page's tab fits the row, down to a
// floor (then the strip shows a window with arrows, menu.cpp). The widths are measured, never estimated: an estimate from the character count ellipsised
// "Performance" and "Debug" on the flat card at 1080p (2026-11-16).
//
// Pure: no windows.h, no D3D. The measure is the panel's (menuPanelTabWidth); tools\native_menu_test drives it with the same one.
#pragma once

namespace edvr {

using TabWidthFn = int (*)(const char* utf8, int emPx);

// The tab face's em for a card's cap height (menu_panel.cpp, Font::Tab), and the smallest the strip shrinks it to.
inline constexpr int tabStripDefaultEm(int capPx) { return capPx * 9 / 7; }
inline constexpr int tabStripMinEm(int capPx) { return capPx * 9 / 10; }
// The gap between two tabs.
inline constexpr int tabStripGapPx(int capPx) { return capPx / 2; }

// The whole strip's width at `emPx`: every tab and the gaps between them.
inline int tabStripWidth(const char* const* names, int n, int emPx, int gapPx, TabWidthFn width) {
  int total = 0;
  for (int i = 0; i < n; ++i) total += width(names[i], emPx) + (i ? gapPx : 0);
  return total;
}

// The largest em from the default down to the floor at which every tab fits `roomPx`; the floor when none does.
inline int tabStripFitEm(const char* const* names, int n, int roomPx, int capPx, TabWidthFn width) {
  const int gap = tabStripGapPx(capPx);
  int em = tabStripDefaultEm(capPx);
  const int floorEm = tabStripMinEm(capPx);
  while (em > floorEm && tabStripWidth(names, n, em, gap, width) > roomPx) --em;
  return em;
}

}  // namespace edvr
