#pragma once
// Which F8 rows wait for a game restart, said as data, and the words the menus use for it (menu.cpp). Pure: no windows.h, no D3D, no config.
// tools\menu_policy_test drives all of it.
//
// A setting marked `restart` in edvr.ini is waiting once its value differs from the launch value (menu.cpp: rowPending). A few settings are live in
// the file but only SOMETIMES need a restart; this is that table. XeSS Frame Generation is live once its bridge exists, but the bridge is made only as
// the game starts, and only when Anti-aliasing is XeSS-SR then (xess_fg.cpp). So with Frame Generation on and XeSS-SR chosen but no bridge, the
// frame generation rows wait for a restart, and so does the Anti-aliasing row when its value changed since the launch (the choice that made the
// difference). When the bridge was made and then failed (xessFgProblem), a restart alone does not fix it: those rows are Stopped, which is not
// pending.
#include <cstddef>
#include <cstdio>
#include <cstring>

namespace edvr {

// The condition a table entry is judged by.
enum class RestartRule {
  XessFrameGeneration,   // fix.xess_frame_generation, fix.xess_generated_frames: the bridge was never made this launch
  XessAntiAliasing,      // fix.temporal_aa: the choice (made since launch) that the bridge needs
};

struct ConditionalRestartEntry {
  const char* section;
  const char* key;
  RestartRule rule;
};

inline constexpr ConditionalRestartEntry kConditionalRestarts[] = {
    {"fix", "xess_frame_generation", RestartRule::XessFrameGeneration},
    {"fix", "xess_generated_frames", RestartRule::XessFrameGeneration},
    {"fix", "temporal_aa", RestartRule::XessAntiAliasing},
};

enum class RestartState { NotPending, PendingRestart, Stopped };

struct RestartInput {
  bool flatProfile = false;           // the rules are the flat profile's: VR has no XeSS Frame Generation
  bool aaIsXess = false;              // Anti-aliasing is XeSS-SR now
  bool fgOn = false;                  // fix.xess_frame_generation is on now
  bool bridged = false;               // the bridge was made this launch (xessFgBridged)
  bool problemPresent = false;        // frame generation was tried and failed or stopped (xessFgProblem)
  bool aaChangedSinceLaunch = false;  // fix.temporal_aa differs from its value at launch
};

inline const ConditionalRestartEntry* conditionalRestartFor(const char* section, const char* key) {
  if (!section || !key) return nullptr;
  for (const ConditionalRestartEntry& e : kConditionalRestarts)
    if (std::strcmp(e.section, section) == 0 && std::strcmp(e.key, key) == 0) return &e;
  return nullptr;
}

// The state of the row section.key. A row that is not in the table is NotPending (its own `restart` mark is rowPending's business, not this table's).
inline RestartState conditionalRestartState(const RestartInput& in, const char* section, const char* key) {
  const ConditionalRestartEntry* e = conditionalRestartFor(section, key);
  if (!e || !in.flatProfile || !in.fgOn) return RestartState::NotPending;
  if (in.problemPresent) return e->rule == RestartRule::XessFrameGeneration ? RestartState::Stopped : RestartState::NotPending;
  if (!in.aaIsXess || in.bridged) return RestartState::NotPending;
  if (e->rule == RestartRule::XessAntiAliasing && !in.aaChangedSinceLaunch) return RestartState::NotPending;
  return RestartState::PendingRestart;
}

// ---- The words -----------------------------------------------------------------------------------------------------------------------------
inline constexpr const char* kRestartReasonConditional = "set up when the game starts";   // the log line's reason for a table row
inline constexpr const char* kRestartReasonSetting = "restart setting";                    // ...and for a row marked `restart`

// The count, as the flat footer says it ("1 change waits for a game restart"); the short form is for a footer line the long one would overflow, and is the
// VR footer's own wording. Empty for none.
inline void menuRestartCountText(int n, bool shortForm, char* out, size_t cap) {
  if (!out || cap == 0) return;
  out[0] = 0;
  if (n <= 0) return;
  if (shortForm) std::snprintf(out, cap, "%d change%s at next launch", n, n == 1 ? "" : "s");
  else if (n == 1) std::snprintf(out, cap, "1 change waits for a game restart");
  else std::snprintf(out, cap, "%d changes wait for a game restart", n);
}

// The notice a closing menu leaves while changes still wait. Empty for none.
inline void menuRestartCloseNotice(int n, char* out, size_t cap) {
  if (!out || cap == 0) return;
  out[0] = 0;
  if (n <= 0) return;
  if (n == 1) std::snprintf(out, cap, "1 change applies at next game start");
  else std::snprintf(out, cap, "%d changes apply at next game start", n);
}

// The flat footer's status, the tail of its second line. A write that failed or is being saved speaks alone; otherwise the count of changes waiting comes
// first, then "keyboard gate unavailable" or "saved".
enum class FooterWrite { None, Failed, Saving, Saved };

inline void menuFlatFooterStatus(FooterWrite write, bool keysReady, int pendingN, bool shortForm, char* out, size_t cap) {
  if (!out || cap == 0) return;
  out[0] = 0;
  if (write == FooterWrite::Failed) { std::snprintf(out, cap, " | write failed"); return; }
  if (write == FooterWrite::Saving) { std::snprintf(out, cap, " | saving"); return; }
  char count[64];
  menuRestartCountText(pendingN, shortForm, count, sizeof(count));
  const char* tail = !keysReady ? " | keyboard gate unavailable" : write == FooterWrite::Saved ? " | saved" : "";
  if (count[0]) std::snprintf(out, cap, " | %s%s", count, tail);
  else std::snprintf(out, cap, "%s", tail);
}

}  // namespace edvr
