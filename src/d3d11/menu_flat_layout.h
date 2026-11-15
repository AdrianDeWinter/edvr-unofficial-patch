#pragma once
// The flat profile's card, sized and placed from the output's own pixels (docs\design-f8-menu-unification-2026-11-15.md, "Flat sizing and placement").
//
// The VR panel is rasterised at the headset's pixels per degree and composites near 1:1. The flat card used to be one fixed raster (860 px, cap 34) that the
// composite then scaled to a fraction of the screen, so the text was resampled at every resolution. Now the raster is sized from the output, and the composite
// puts it at exactly its own pixel size: one bitmap pixel is one output pixel.
//
// The reference is the size the card had at 1920x1080 before this change, so nothing moves for anyone who plays there. That size was: half of
// min(0.62, 0.85 * outH / outW / aspect) of the output's width, where `aspect` is the Performance tab's bitmap height over its width (1106 / 860 at the old
// raster; the tab's eleven rows, its hint box and the two-line footer). The 0.85 term bound, so the old card was 0.425 * outH / aspect wide and
// 42.5 % of the output's height tall: 357 x 459 px at 1080p. Expressed against the output's HEIGHT, so an ultrawide does not grow the card.
//
// Pure: no windows.h, no D3D, no config. tools\menu_policy_test drives it, and tools\native_menu_test measures the aspect the reference was derived from.
#include <algorithm>
#include <cmath>

namespace edvr {

// The old fixed raster's width and text height. A flat raster keeps this card width : cap ratio, so every row, hint and footer lays out as it did.
constexpr int kFlatRefRasterPx = 860;
constexpr int kFlatRefCapPx = 34;

// The card's width as a fraction of the output's height: the old card (0.425 * outH / 1.286) at any resolution; and a notice's, which was 0.31 of the output's
// width at 1080p (0.31 * 1920 / 1080).
constexpr float kFlatMenuCardHeightFrac = 0.3305f;
constexpr float kFlatNoticeHeightFrac = 0.551f;

// Bounds. Below this cap the row text stops being readable whatever the output; a card is never more than this share of the output's width.
constexpr int kFlatMinCapPx = 14;
constexpr float kFlatMaxCardWidthFrac = 0.45f;

// Where the bitmap's upper-left corner sits, as a share of the output's width and height from its upper-left corner, and the share of the height the bitmap
// may take (a tall page past it is scaled down: the one case that is not 1:1).
constexpr float kFlatMarginFrac = 0.02f;
constexpr float kFlatMaxHeightFrac = 0.85f;

struct FlatCardSize {
  int cardPx = 0;  // the card's width in raster pixels
  int capPx = 0;   // the row text's height
};

// The raster for a card on an `outW` x `outH` output. `heightFrac` is kFlatMenuCardHeightFrac or kFlatNoticeHeightFrac. An output not known yet (0) is the
// 1920x1080 reference.
inline FlatCardSize flatCardSize(int outW, int outH, float heightFrac) {
  if (outW <= 0 || outH <= 0) {
    outW = 1920;
    outH = 1080;
  }
  int card = static_cast<int>(std::lround(heightFrac * static_cast<float>(outH)));
  int cap = static_cast<int>(std::lround(static_cast<double>(card) * kFlatRefCapPx / kFlatRefRasterPx));
  if (cap < kFlatMinCapPx) {
    cap = kFlatMinCapPx;
    card = static_cast<int>(std::ceil(static_cast<double>(cap) * kFlatRefRasterPx / kFlatRefCapPx));
  }
  const int widest = static_cast<int>(kFlatMaxCardWidthFrac * static_cast<float>(outW));
  if (card > widest && widest >= 1) {
    card = widest;
    cap = static_cast<int>(std::lround(static_cast<double>(card) * kFlatRefCapPx / kFlatRefRasterPx));
    if (cap < 1) cap = 1;
  }
  FlatCardSize s;
  s.cardPx = card;
  s.capPx = cap;
  return s;
}

// The tallest raster that is a notice and not the menu: a one-line notice card is about 0.09 of the output's height, the menu's smallest page (Debug, padded
// to its window) about 0.34. The flat screen's notice waits for a raster below this before it is composited (menu.cpp, ScreenNoticeState::tick).
inline int flatNoticeMaxRasterPx(int outH) {
  if (outH <= 0) outH = 1080;
  return static_cast<int>(std::lround(0.2 * outH));
}

// The bitmap's width with the tooltip's strip beside the card; `tipRatio` is menu_panel.h's kTipRatio. The card keeps its size and place.
inline int flatBitmapWidth(int cardPx, bool withTip, float tipRatio) {
  return withTip ? static_cast<int>(static_cast<float>(cardPx) * tipRatio + 0.5f) : cardPx;
}

// The card the composite drew before this change, in output pixels, for a bitmap `aspect` (height / width): the reference the sizes above were derived from.
inline float flatLegacyCardPx(int outW, int outH, float aspect) {
  const float fitHeight = 0.85f * static_cast<float>(outH) / static_cast<float>(outW) / aspect;
  return 0.5f * std::min(0.62f, fitHeight) * static_cast<float>(outW);
}

// Where a `bmpW` x `bmpH` bitmap lands on an `outW` x `outH` output, in output pixels, and the composite's geometry for it at a distance of 1 in the units
// menu_panel.cpp's flat path uses (the output's width spans -1..1; a surface `halfW` wide is `halfW` of it; positive `shift` is to the right; `originY`
// moves the surface's centre so that its top edge is at `y`).
struct FlatPlacement {
  int x = 0, y = 0, w = 0, h = 0;
  float halfW = 0.0f;
  float shift = 0.0f;
  float originY = 0.0f;
  bool oneToOne = false;  // w == bmpW and h == bmpH: no resample
};

inline FlatPlacement flatPlace(int outW, int outH, int bmpW, int bmpH) {
  FlatPlacement p;
  if (outW <= 0 || outH <= 0 || bmpW <= 0 || bmpH <= 0) return p;
  double scale = 1.0;
  const double maxH = static_cast<double>(kFlatMaxHeightFrac) * outH;
  if (bmpH > maxH) scale = maxH / bmpH;
  const double maxW = (1.0 - 2.0 * kFlatMarginFrac) * outW;
  if (bmpW * scale > maxW) scale = maxW / bmpW;
  p.w = scale < 1.0 ? static_cast<int>(std::lround(bmpW * scale)) : bmpW;
  p.h = scale < 1.0 ? static_cast<int>(std::lround(bmpH * scale)) : bmpH;
  p.oneToOne = scale >= 1.0;
  // Whole pixels: a corner on a fraction of a pixel would resample even at 1:1.
  p.x = static_cast<int>(std::lround(kFlatMarginFrac * outW));
  p.y = static_cast<int>(std::lround(kFlatMarginFrac * outH));
  const float tanY = static_cast<float>(outH) / static_cast<float>(outW);
  p.halfW = static_cast<float>(p.w) / static_cast<float>(outW);
  const float halfH = p.halfW * static_cast<float>(bmpH) / static_cast<float>(bmpW);
  const float leftTan = -1.0f + 2.0f * static_cast<float>(p.x) / static_cast<float>(outW);
  const float topTan = tanY * (1.0f - 2.0f * static_cast<float>(p.y) / static_cast<float>(outH));
  p.shift = leftTan + p.halfW;
  p.originY = halfH - topTan;
  return p;
}

}  // namespace edvr
