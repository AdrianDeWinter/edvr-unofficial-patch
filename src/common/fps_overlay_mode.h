// menu.fps_overlay: off | compact | detailed, VR only since 2026-11-19 (docs\design-f8-menu-unification-2026-11-15.md, "The FPS readout"); flat's
// menu.monitor_panel_view below. Pure, so tools\native_perf_history_test can pin them.
//
// The key was a switch (on | off) before. The old words still read: on, true, yes and 1 are compact; off, false, no and 0 are off. An empty or unknown value is the
// default, off. The menu writes the new words.
#pragma once

#include <cctype>
#include <cstring>

namespace edvr {

enum class FpsOverlayMode { Off, Compact, Detailed };

// One config word, blanks trimmed and lowercased, cut at 15 characters; empty for a null text. Both parsers below read through it.
inline void normalizeConfigWord(const char* text, char (&word)[16]) {
  std::memset(word, 0, sizeof(word));
  if (!text) return;
  size_t n = 0;
  const char* p = text;
  while (*p == ' ' || *p == '\t') ++p;
  for (; *p && n + 1 < sizeof(word); ++p) word[n++] = static_cast<char>(std::tolower(static_cast<unsigned char>(*p)));
  while (n && (word[n - 1] == ' ' || word[n - 1] == '\t' || word[n - 1] == '\r')) word[--n] = 0;
}

inline FpsOverlayMode parseFpsOverlay(const char* text) {
  char word[16];
  normalizeConfigWord(text, word);
  if (!std::strcmp(word, "compact") || !std::strcmp(word, "on") || !std::strcmp(word, "true") || !std::strcmp(word, "yes") || !std::strcmp(word, "1"))
    return FpsOverlayMode::Compact;
  if (!std::strcmp(word, "detailed")) return FpsOverlayMode::Detailed;
  return FpsOverlayMode::Off;
}

inline const char* fpsOverlayWord(FpsOverlayMode m) {
  return m == FpsOverlayMode::Compact ? "compact" : m == FpsOverlayMode::Detailed ? "detailed" : "off";
}

// menu.monitor_panel_view: full | detailed | compact, flat only (2026-11-19). What the flat monitor panel (hotkey.monitor_panel) shows while it is up: full is
// the Monitor page's gauges and graphs, detailed and compact the readout card and line menu.fps_overlay gives VR. An empty or unknown value is the default, full.
enum class MonitorPanelView { Full, Detailed, Compact };

inline MonitorPanelView parseMonitorPanelView(const char* text) {
  char word[16];
  normalizeConfigWord(text, word);
  if (!std::strcmp(word, "detailed")) return MonitorPanelView::Detailed;
  if (!std::strcmp(word, "compact")) return MonitorPanelView::Compact;
  return MonitorPanelView::Full;
}

inline const char* monitorPanelViewWord(MonitorPanelView v) {
  return v == MonitorPanelView::Detailed ? "detailed" : v == MonitorPanelView::Compact ? "compact" : "full";
}

// The readout shape a view draws with (buildReadoutContent); Off for full, which draws the Monitor page instead.
inline FpsOverlayMode monitorPanelReadoutMode(MonitorPanelView v) {
  return v == MonitorPanelView::Detailed ? FpsOverlayMode::Detailed : v == MonitorPanelView::Compact ? FpsOverlayMode::Compact : FpsOverlayMode::Off;
}

}  // namespace edvr
