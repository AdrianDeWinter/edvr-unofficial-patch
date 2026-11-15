// The frame-rate readouts' arithmetic for the rendered AND the presented rate (docs\design-f8-menu-unification-2026-11-15.md, "Rendered and presented frame rate",
// "The FPS readout"), header-only and free of windows.h so tools\native_perf_history_test can pin it.
//
// Under XeSS Frame Generation Elite's own Present runs at the RENDERED rate (perf_monitor.cpp rings that interval); the screen shows the rendered frames plus the
// generated ones. The presented rate comes from a counter of frames that reached the screen (xess_fg_presented.h), turned into a second ring here, and is
// shown beside the rendered one -- never computed from it. A session that never had the bridge has no presented data at all: the state says Absent and no
// function answers a zero.
//
// Three words, kept apart on purpose wherever the readout says them:
//   absent       no bridge this session: nothing is shown
//   unavailable  a bridge, and neither source can count presented frames: "Presented: unavailable"
//   stopped      the bridge fell back: "FG stopped: <reason>", and the presented rate follows the plain swap chain
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "perf_math.h"

namespace edvr {

// ---- The budgets -------------------------------------------------------------------------------------------------------------------------------------
// The display's refresh period; 11.1 ms (90 Hz) only when the rate is unknown. The presented budget is the period; the rendered one is that period times the
// frames each rendered frame becomes (1 + the generated frames active now), and without frame generation the two are the same.
constexpr float kUnknownRefreshBudgetMs = 11.1f;
inline float displayBudgetMs(float refreshHz) {
  return refreshHz >= 24.0f && refreshHz <= 1000.0f ? 1000.0f / refreshHz : kUnknownRefreshBudgetMs;
}
inline float renderedBudgetMs(float refreshHz, uint32_t activeGenerated) {
  return displayBudgetMs(refreshHz) * static_cast<float>(1u + activeGenerated);
}

// ---- Which of the words applies --------------------------------------------------------------------------------------------------------------------
enum class PresentedState {
  Absent,       // no bridge this session
  Measuring,    // a bridge, a source not sampled yet (or one sample: a rate needs two)
  Counting,     // the count is flowing
  Unavailable,  // a bridge, and no source can count
};
inline PresentedState presentedStateOf(bool bridgeThisSession, bool sourceCounting, bool sourceUnavailable, bool haveEntries) {
  if (!bridgeThisSession) return PresentedState::Absent;
  if (sourceUnavailable && !sourceCounting) return PresentedState::Unavailable;
  if (sourceCounting && haveEntries) return PresentedState::Counting;
  return PresentedState::Measuring;
}

// ---- The presented period ring ---------------------------------------------------------------------------------------------------------------------
// One entry per rendered frame that moved the counter: the time since the last entry and the frames the counter gained in it. The period is the time divided by
// the frames, so a 2x frame generation gives about half the rendered period. A frame that did not move the counter (the count lags a Present) makes no entry:
// its time is added to the next one, so no entry is a short fiction of zero frames.
class PresentedRing {
 public:
  static constexpr int kCapacity = 900;

  void clear() {
    head_ = count_ = 0;
    primed_ = false;
    prevCount_ = 0;
    prevQpc_ = 0;
  }
  // Forget the counter's baseline and keep the strip: the next observation is a new first one (a profile switch, a source that restarted).
  void rebase() { primed_ = false; }

  // One call per rendered frame, from the frame boundary, with the cumulative count and the QPC time of its latest sample. A count that went backwards (a
  // restart, a wrap past what the source allows) or a time gap of 5 s or more (a load, a suspend) is a new baseline, not an entry.
  void observe(uint64_t count, int64_t qpc, int64_t freq) {
    if (freq <= 0) return;
    if (!primed_ || count < prevCount_ || qpc <= prevQpc_) {
      primed_ = true;
      prevCount_ = count;
      prevQpc_ = qpc;
      return;
    }
    if (count == prevCount_) return;
    const double dt = static_cast<double>(qpc - prevQpc_) * 1000.0 / static_cast<double>(freq);
    const uint64_t frames = count - prevCount_;
    prevCount_ = count;
    prevQpc_ = qpc;
    if (!(dt > 0.0) || dt >= 5000.0 || frames > 255) return;
    Entry& e = ring_[head_];
    e.dtMs = static_cast<float>(dt);
    e.frames = static_cast<uint8_t>(frames);
    head_ = (head_ + 1) % kCapacity;
    if (count_ < kCapacity) ++count_;
    ++pushes_;
  }

  int count() const { return count_; }
  // Entries ever made (the ring holds the newest kCapacity): a reader that sees it stand still knows the count stopped moving.
  uint64_t pushes() const { return pushes_; }
  bool any() const { return count_ > 0; }
  // Entry `i`, 0 the oldest: its period in ms.
  float periodAt(int i) const {
    const Entry& e = at(i);
    return e.frames ? e.dtMs / static_cast<float>(e.frames) : 0.0f;
  }
  // The last `max` periods, oldest first; the number copied.
  int copy(float* out, int max) const {
    if (!out || max <= 0) return 0;
    const int n = count_ < max ? count_ : max;
    for (int i = 0; i < n; ++i) out[i] = periodAt(count_ - n + i);
    return n;
  }
  // The same ring summarised the way the rendered one is -- the mean, the 1% low, the worst -- over the newest entries that cover `seconds` (0: every entry).
  PerfStats stats(float seconds = 0.0f) const {
    int n = count_;
    if (seconds > 0.0f) {
      double ms = 0.0;
      n = 0;
      while (n < count_ && ms < static_cast<double>(seconds) * 1000.0) {
        ms += at(count_ - 1 - n).dtMs;
        ++n;
      }
    }
    float v[kCapacity];
    const int got = copy(v, n < kCapacity ? n : kCapacity);
    PerfStats s = perfStatsOf(v, got);
    if (s.count) {
      double ms = 0.0, frames = 0.0;
      for (int i = count_ - got; i < count_; ++i) {
        ms += at(i).dtMs;
        frames += at(i).frames;
      }
      if (frames > 0.0) s.avgMs = static_cast<float>(ms / frames);   // the true mean (every frame over the time it took), not the mean of the means
    }
    return s;
  }

 private:
  struct Entry {
    float dtMs = 0.0f;
    uint8_t frames = 0;
  };
  const Entry& at(int i) const { return ring_[(head_ - count_ + i + 2 * kCapacity) % kCapacity]; }

  Entry ring_[kCapacity];
  int head_ = 0, count_ = 0;
  uint64_t pushes_ = 0;
  bool primed_ = false;
  uint64_t prevCount_ = 0;
  int64_t prevQpc_ = 0;
};

// ---- One reading, and the words that print it --------------------------------------------------------------------------------------------------------
struct PerfReadout {
  bool haveRendered = false;
  float renderedFps = 0.0f, renderedMs = 0.0f, renderedLowFps = 0.0f, renderedLowMs = 0.0f;
  bool haveCpu = false;
  float cpuMs = 0.0f;
  int drops = 0;                          // long frames in the last ten seconds
  PresentedState presented = PresentedState::Absent;
  float presentedFps = 0.0f, presentedMs = 0.0f, presentedLowFps = 0.0f, presentedLowMs = 0.0f;
  uint32_t activeGenerated = 0;           // generated frames per rendered frame right now
  bool fgStopped = false;
  char stopped[120] = "";                 // why, when fgStopped
  char unavailableWhy[80] = "";           // why, when presented is Unavailable
};

inline bool readoutHasBridge(const PerfReadout& r) { return r.presented != PresentedState::Absent; }

// The compact line when a bridge exists, false when it does not (the caller prints the line it always printed). Three spaces between fields, which the
// head-locked overlay's builder keeps as one measured line:
//   "render 58 / shown 116 fps   cpu 9.1 ms   drops 2"
// An unavailable count says so in place of the number ("render 58 fps   shown n/a   cpu 9.1 ms"); a count not measured yet is "--".
inline bool formatCompactLine(const PerfReadout& r, char* buf, size_t len) {
  if (!buf || !len) return false;
  if (!readoutHasBridge(r)) return false;
  if (!r.haveRendered) {
    std::snprintf(buf, len, "measuring");
    return true;
  }
  char rate[96];
  if (r.presented == PresentedState::Counting) std::snprintf(rate, sizeof(rate), "render %.0f / shown %.0f fps", r.renderedFps, r.presentedFps);
  else if (r.presented == PresentedState::Unavailable) std::snprintf(rate, sizeof(rate), "render %.0f fps   shown n/a", r.renderedFps);
  else std::snprintf(rate, sizeof(rate), "render %.0f / shown -- fps", r.renderedFps);
  char cpu[32] = "";
  if (r.haveCpu) std::snprintf(cpu, sizeof(cpu), "   cpu %.1f ms", r.cpuMs);
  char drops[24] = "";
  if (r.drops > 0) std::snprintf(drops, sizeof(drops), "   drops %d", r.drops);
  std::snprintf(buf, len, "%s%s%s", rate, cpu, drops);
  return true;
}

// The tiles, handed one at a time to `tile(caption, value, sub)`: the rendered rate, its frame time and 1% low, then -- when a bridge exists this session --
// the presented rate, its 1% low and the active factor. Captions are at most 23 characters, values 23, subs 39 (PerfTile).
template <class Tile>
inline void emitRateTiles(const PerfReadout& r, Tile&& tile) {
  char v[32], sub[48];
  if (r.haveRendered) {
    std::snprintf(v, sizeof(v), "%.1f", r.renderedFps);
    tile("RENDERED", v, "fps from the game");
    std::snprintf(v, sizeof(v), "%.1f", r.renderedMs);
    tile("FRAME TIME", v, "ms per rendered frame");
    std::snprintf(v, sizeof(v), "%.0f", r.renderedLowFps);
    std::snprintf(sub, sizeof(sub), "fps, worst 1%% %.1f ms", r.renderedLowMs);
    tile("1% LOW", v, sub);
  } else {
    tile("RENDERED", "--", "measuring");
    tile("FRAME TIME", "--", "");
    tile("1% LOW", "--", "");
  }
  if (!readoutHasBridge(r)) return;
  switch (r.presented) {
    case PresentedState::Counting:
      std::snprintf(v, sizeof(v), "%.1f", r.presentedFps);
      tile("PRESENTED", v, "fps on the screen");
      std::snprintf(v, sizeof(v), "%.0f", r.presentedLowFps);
      std::snprintf(sub, sizeof(sub), "fps, worst 1%% %.1f ms", r.presentedLowMs);
      tile("PRESENTED 1% LOW", v, sub);
      break;
    case PresentedState::Unavailable:
      tile("PRESENTED", "--", "unavailable");
      tile("PRESENTED 1% LOW", "--", "");
      break;
    default:
      tile("PRESENTED", "--", "measuring");
      tile("PRESENTED 1% LOW", "--", "");
      break;
  }
  if (r.fgStopped) {
    tile("FRAME GENERATION", "x1", "stopped");
  } else if (r.activeGenerated > 0) {
    std::snprintf(v, sizeof(v), "x%u", static_cast<unsigned>(1u + r.activeGenerated));
    tile("FRAME GENERATION", v, "frames per rendered frame");
  } else {
    tile("FRAME GENERATION", "x1", "off: shown = rendered");
  }
}

// The Monitor page's one line about the bridge, or empty: "FG stopped: <why>" and "Presented: unavailable (<why>)". Never both, and never for a session
// without a bridge.
inline void formatBridgeNote(const PerfReadout& r, char* buf, size_t len) {
  if (!buf || !len) return;
  buf[0] = 0;
  if (!readoutHasBridge(r)) return;
  if (r.fgStopped) std::snprintf(buf, len, "FG stopped: %s", r.stopped[0] ? r.stopped : "unknown reason");
  else if (r.presented == PresentedState::Unavailable)
    std::snprintf(buf, len, "Presented: unavailable%s%s%s", r.unavailableWhy[0] ? " (" : "", r.unavailableWhy, r.unavailableWhy[0] ? ")" : "");
}

}  // namespace edvr
