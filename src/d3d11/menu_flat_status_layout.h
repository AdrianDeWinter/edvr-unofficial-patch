// The flat Status page's rows as drawn (docs\design-f8-menu-unification-2026-11-15.md, "The flat Status page").
//
// The page's lines (menu_flat_status.h) are drawn in the note face: the caption in a column as wide as the widest caption, the value beside it, and a value
// too long for its column wrapped at spaces onto continuation rows under it (an empty caption). A word wider than the column gets a row of its own.
// The measure is the panel's ruler at the note face's em (menuPanelMeasureLine); a rig gives it the same one.
//
// Pure: no windows.h, no D3D. tools\native_menu_test drives it at 1080p, 1440p and 2160p.
#pragma once
#include <cstdio>
#include <cstring>
#include <string>
#include "menu_flat_status.h"

namespace edvr {

constexpr int kFlatStatusMaxRows = 2 * kFlatStatusMaxLines;

struct FlatStatusRow {
  char caption[24] = {};
  char value[64] = {};
};

struct FlatStatusLayout {
  int splitPx = 0;   // MenuContent::infoSplitPx
  int rowCount = 0;
  FlatStatusRow rows[kFlatStatusMaxRows];
};

using FlatStatusMeasureFn = int (*)(const char* utf8, void* context);

// The note face's em for a card's cap height (menu_panel.cpp, Font::Hint).
inline constexpr int flatStatusEmPx(int capPx) { return capPx * 8 / 7; }

inline void flatStatusAddRow(FlatStatusLayout* out, const char* caption, const std::string& value) {
  if (out->rowCount >= kFlatStatusMaxRows) return;
  FlatStatusRow& r = out->rows[out->rowCount++];
  std::snprintf(r.caption, sizeof(r.caption), "%s", caption);
  std::snprintf(r.value, sizeof(r.value), "%s", value.c_str());
}

inline void flatStatusLayout(const FlatStatusLine* lines, int n, int cardPx, int capPx, FlatStatusMeasureFn measure, void* context,
                             FlatStatusLayout* out) {
  *out = FlatStatusLayout{};
  const int pad = capPx * 8 / 10;   // menu_panel.cpp's card padding
  int widest = 0;
  for (int i = 0; i < n; ++i) {
    const int w = measure(lines[i].caption, context);
    if (w > widest) widest = w;
  }
  int split = pad + widest + capPx;   // a cap height of gap between caption and value
  const int maxSplit = pad + (cardPx - 2 * pad) * 45 / 100;
  if (split > maxSplit) split = maxSplit;
  out->splitPx = split;
  const int valueW = cardPx - pad - split;
  for (int i = 0; i < n; ++i) {
    const char* caption = lines[i].caption;
    const std::string text = lines[i].value;
    std::string row;
    bool first = true;
    size_t p = 0;
    while (p < text.size()) {
      while (p < text.size() && text[p] == ' ') ++p;
      if (p >= text.size()) break;
      size_t e = text.find(' ', p);
      if (e == std::string::npos) e = text.size();
      const std::string word = text.substr(p, e - p);
      p = e;
      const std::string tryRow = row.empty() ? word : row + " " + word;
      if (row.empty() || measure(tryRow.c_str(), context) <= valueW) {
        row = tryRow;
      } else {
        flatStatusAddRow(out, first ? caption : "", row);
        first = false;
        row = word;
      }
    }
    flatStatusAddRow(out, first ? caption : "", row);   // the last row, or the caption alone for an empty value
  }
}

}  // namespace edvr
