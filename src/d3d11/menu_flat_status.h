// The flat profile's Status page, as a pure function (docs\design-f8-menu-unification-2026-11-15.md, "The flat Status page").
//
// menu.cpp's buildStatus gathers what each line's source says into a FlatStatusInput and asks flatStatusLines which lines the page has and what they read. A line
// whose source does not exist is not there (a display size not published yet, a scene not measured, no graphics wrapper), never a guess. The wrapped notes of
// Elite's graphics-settings warning follow the "Elite graphics settings" line and are the caller's (flat_elite_settings.h, the same words the Performance tab
// carried before): up to FlatSettingsWarning::kMaxLines of them, so the page is at most kFlatStatusMaxLines + that many lines (menu.cpp static_asserts it
// against menu_panel.h's kMenuMaxLines).
//
// Pure: no windows.h, no D3D, no config. tools\menu_policy_test and tools\native_menu_test drive it.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace edvr {

enum class FlatDisplayMode { Unknown, Windowed, Exclusive };   // what the swap chain's description says: a windowed chain is windowed or borderless (one word: windowed/borderless)
enum class FlatAaRun { NotRunning, Running, Paused };           // Paused: stood down for the shape of Elite's post chain (the settings warning)
enum class FlatFgState { Off, NextLaunch, Stopped, NeedsXess, On, OnIdle };

struct FlatStatusInput {
  const char* version = "";        // EDVR_VERSION_STRING
  const char* gameBuild = "";      // "" when not known
  uint32_t outW = 0, outH = 0;     // the swap chain's back buffer; 0 until the first Present
  float refreshHz = 0.0f;          // the display's refresh rate; 0 until read
  FlatDisplayMode displayMode = FlatDisplayMode::Unknown;
  bool aaOn = false;               // a temporal mode is selected
  const char* aaLabel = "";        // the selected mode as the Anti-aliasing row names it (DLSS, DLAA, TAA, FSR 3.1, XeSS-SR)
  const char* aaModel = "";        // the DLSS preset's label (K, L, M) while the mode reads it, else ""
  FlatAaRun aaRun = FlatAaRun::NotRunning;
  bool sceneKnown = false;         // the runtime has measured the scene's and the output's sizes (flatRuntimeSceneSizes)
  uint32_t renderW = 0, renderH = 0, sceneOutW = 0, sceneOutH = 0;
  FlatFgState fg = FlatFgState::Off;
  unsigned fgFactor = 0;           // frames shown per rendered frame while On
  bool fgExclusiveFullscreen = false;
  const char* fgProblem = "";      // why it stopped, while Stopped
  const char* sharpening = "";     // the Sharpening row's value as the panel shows it
  const char* hudQuality = "";     // the HUD quality row's value
  const char* hudAntiAliasing = "";  // the HUD Anti-Aliasing row's value, "" while the HUD quality is off
  const char* wrapper = nullptr;   // the graphics wrapper that handles the game's calls (contextWrapperFile()), null when none was found
  bool keyboardReady = false;      // the game's keyboard has reached the gate
  bool eliteWarningActive = false; // the settings warning is up (its notes follow the line)
  int waitingCount = 0;            // changes waiting for a restart
  const char* waitingLabels = "";  // their row labels, comma separated
  const char* lastWrite = "";      // "" for none this session
};

struct FlatStatusLine {
  char caption[24] = {};
  char value[64] = {};
};

// What the Performance tab says in place of the warning while it is up: a pointer to the Status tab, one line at the flat card's width at 1080p
// (tools\native_menu_test --layout measures it).
inline constexpr char kFlatWarningPointer[] = "Elite's settings limit AA: see the Status tab.";

// The most lines the page's own lines can be. The notes of the warning come on top.
constexpr int kFlatStatusMaxLines = 13;

inline void flatStatusAdd(FlatStatusLine* out, int max, int* n, const char* caption, const char* value) {
  if (*n >= max) return;
  FlatStatusLine& l = out[(*n)++];
  std::snprintf(l.caption, sizeof(l.caption), "%s", caption);
  std::snprintf(l.value, sizeof(l.value), "%s", value);
}

// The lines, in the page's order. Returns how many (at most `max`).
inline int flatStatusLines(const FlatStatusInput& in, FlatStatusLine* out, int max) {
  int n = 0;
  char v[160];
  if (in.gameBuild && *in.gameBuild) std::snprintf(v, sizeof(v), "%s / game build %s", in.version, in.gameBuild);
  else std::snprintf(v, sizeof(v), "%s", in.version);
  flatStatusAdd(out, max, &n, "ED-UP", v);
  flatStatusAdd(out, max, &n, "Profile", "flat");

  // Display: each part only when it is known.
  {
    size_t len = 0;
    v[0] = 0;
    if (in.outW && in.outH) len += static_cast<size_t>(std::snprintf(v + len, sizeof(v) - len, "%ux%u", in.outW, in.outH));
    if (in.refreshHz >= 1.0f)
      len += static_cast<size_t>(std::snprintf(v + len, sizeof(v) - len, "%s%.0f Hz", len ? ", " : "", static_cast<double>(in.refreshHz)));
    if (in.displayMode != FlatDisplayMode::Unknown)
      len += static_cast<size_t>(std::snprintf(v + len, sizeof(v) - len, "%s%s", len ? ", " : "",
                                               in.displayMode == FlatDisplayMode::Exclusive ? "exclusive fullscreen" : "windowed/borderless"));
    if (len) flatStatusAdd(out, max, &n, "Display", v);
  }

  // Anti-aliasing: what is selected, the model it reads, and whether it runs.
  if (!in.aaOn) {
    flatStatusAdd(out, max, &n, "Anti-aliasing", "off");
  } else {
    const char* run = in.aaRun == FlatAaRun::Running ? "running" : in.aaRun == FlatAaRun::Paused ? "paused: Elite's settings" : "not running";
    if (in.aaModel && *in.aaModel) std::snprintf(v, sizeof(v), "%s %s, %s", in.aaLabel, in.aaModel, run);
    else std::snprintf(v, sizeof(v), "%s, %s", in.aaLabel, run);
    flatStatusAdd(out, max, &n, "Anti-aliasing", v);
    if (in.sceneKnown && in.renderW && in.renderH && in.sceneOutW && in.sceneOutH) {
      if (in.renderW == in.sceneOutW && in.renderH == in.sceneOutH) std::snprintf(v, sizeof(v), "none, %ux%u", in.renderW, in.renderH);
      else std::snprintf(v, sizeof(v), "%ux%u to %ux%u", in.renderW, in.renderH, in.sceneOutW, in.sceneOutH);
      flatStatusAdd(out, max, &n, "Upscaling", v);
    }
  }

  switch (in.fg) {
    case FlatFgState::Off: std::snprintf(v, sizeof(v), "off"); break;
    case FlatFgState::NextLaunch: std::snprintf(v, sizeof(v), "at next launch"); break;
    case FlatFgState::Stopped: std::snprintf(v, sizeof(v), "stopped: %s", in.fgProblem ? in.fgProblem : ""); break;
    case FlatFgState::NeedsXess: std::snprintf(v, sizeof(v), "on, needs XeSS-SR"); break;
    case FlatFgState::On: std::snprintf(v, sizeof(v), "on, %ux", in.fgFactor); break;
    case FlatFgState::OnIdle:
      std::snprintf(v, sizeof(v), in.fgExclusiveFullscreen ? "on, none in exclusive fullscreen" : "on, no frames made now");
      break;
  }
  flatStatusAdd(out, max, &n, "Frame generation", v);

  if (in.sharpening && *in.sharpening) flatStatusAdd(out, max, &n, "Sharpening", in.sharpening);
  if (in.hudQuality && *in.hudQuality) {
    if (in.hudAntiAliasing && *in.hudAntiAliasing) std::snprintf(v, sizeof(v), "%s, edges %s", in.hudQuality, in.hudAntiAliasing);
    else std::snprintf(v, sizeof(v), "%s", in.hudQuality);
    flatStatusAdd(out, max, &n, "HUD quality", v);
  }
  if (in.wrapper && *in.wrapper) flatStatusAdd(out, max, &n, "Graphics wrapper", in.wrapper);
  flatStatusAdd(out, max, &n, "Keyboard", in.keyboardReady ? "ready" : "gate unavailable");
  if (in.eliteWarningActive) flatStatusAdd(out, max, &n, "Elite graphics settings", "see below");

  if (in.waitingCount <= 0) std::snprintf(v, sizeof(v), "none");
  else std::snprintf(v, sizeof(v), "%d: %s", in.waitingCount, in.waitingLabels ? in.waitingLabels : "");
  flatStatusAdd(out, max, &n, "Waiting for a restart", v);
  flatStatusAdd(out, max, &n, "Last write", in.lastWrite && *in.lastWrite ? in.lastWrite : "none this session");
  return n;
}

}  // namespace edvr
