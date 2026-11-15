#pragma once
// How many frames really reached the screen while XeSS Frame Generation runs (docs\design-f8-menu-unification-2026-11-15.md, "Rendered and presented frame rate").
//
// Elite's own Present runs at the RENDERED rate under the bridge (xess_fg.cpp). The frames the screen shows are the rendered ones plus the generated ones,
// and the count of those is read, never computed from the rendered rate and the setting (a generated frame XeSS-FG skipped would be counted anyway).
// Two sources, in order:
//
//   1. xefg_swapchain_present_status_t::framesPresented, "number of frames sent to be presented during the last call": read after every Present on the proxy
//      and added up. It is XeSS-FG's own statement, so a Present that generated nothing says 1. A status that never says anything usable (the call fails, or
//      every Present reports 0) is given up on after kProbePresents calls.
//   2. IDXGISwapChain::GetFrameStatistics on the presenting swap chain: PresentCount, a cumulative count that wraps at 2^32. A DISJOINT result (a mode change,
//      a minimised window) cannot be compared with the one before it, so the next good sample is a new baseline. After a fall back to the plain swap chain this
//      is the only source: the proxy is gone, and the plain chain presents one frame per Present.
//
// Neither source (the status call failing and frame statistics failing or never advancing) is "unavailable", said with its reason: never a count of zero.
//
// Pure: no windows.h, no D3D. tools\native_perf_history_test drives it with a counting stand-in for each source. The caller (xess_fg.cpp) is the render
// thread; the published copy is its atomics.
#include <cstdint>
#include <cstdio>

namespace edvr {
namespace fgpresented {

enum class Source : int { None = 0, StatusFrames = 1, FrameStatistics = 2 };

inline const char* sourceName(Source s) {
  switch (s) {
    case Source::StatusFrames: return "XeSS-FG present status (framesPresented, summed per Present)";
    case Source::FrameStatistics: return "DXGI frame statistics (PresentCount)";
    default: return "none";
  }
}

// A present status above this is not a count of frames for one Present.
constexpr uint32_t kMaxFramesPerPresent = 8;
// Calls a source gets to show it works before the next is tried (status), or before it is called unavailable (frame statistics).
constexpr unsigned kProbePresents = 30;
// A PresentCount step larger than this between two samples is a restart of the counter, not frames.
constexpr uint32_t kMaxStatsStep = 1000;

class Tally {
 public:
  // After XeSS-FG's present status was read for one Present on the proxy. `statusOk` is the call's success; `framesPresented` its field.
  void onStatus(bool statusOk, uint32_t framesPresented, int64_t nowQpc) {
    if (!statusUsable_) return;
    if (statusOk && framesPresented >= 1 && framesPresented <= kMaxFramesPerPresent) {
      total_ += framesPresented;
      qpc_ = nowQpc;
      statusCounted_ = true;
      badRun_ = 0;
      setSource(Source::StatusFrames);
      return;
    }
    if (statusCounted_) return;   // one odd sample in a status that has worked is dropped, not a verdict
    if (++badRun_ >= kProbePresents) {
      statusUsable_ = false;
      statusWhy_ = statusOk ? "the present status never reports a frame presented" : "the present status call fails";
    }
  }

  // Whether the caller should read frame statistics for this Present: the status is given up on, or the proxy is gone.
  bool wantsStats() const { return !statusUsable_; }

  // After GetFrameStatistics on the presenting swap chain. `ok` is the call's success, `disjoint` the DXGI_ERROR_FRAME_STATISTICS_DISJOINT result.
  void onStats(bool ok, bool disjoint, uint32_t presentCount, int64_t syncQpc, int64_t nowQpc) {
    if (statusUsable_) return;
    if (disjoint) {
      havePrev_ = false;
      return;
    }
    if (!ok) {
      if (!statsAdvanced_ && ++statsBad_ >= kProbePresents) markUnavailable("frame statistics are not available on this swap chain");
      return;
    }
    if (!havePrev_) {
      prev_ = presentCount;
      havePrev_ = true;
      return;
    }
    const uint32_t step = presentCount - prev_;   // modulo 2^32: a wrap is a small step
    prev_ = presentCount;
    if (step > kMaxStatsStep) return;             // a restarted counter: the baseline is now this one
    if (step) {
      statsAdvanced_ = true;
      total_ += step;
      qpc_ = syncQpc > 0 ? syncQpc : nowQpc;
      statsFlat_ = 0;
      setSource(Source::FrameStatistics);
    } else if (!statsAdvanced_ && ++statsFlat_ >= 4 * kProbePresents) {
      markUnavailable("frame statistics never advance");
    }
  }

  // The proxy is gone (the bridge fell back to the plain swap chain): the status source ends, the count carries on from frame statistics of the new chain.
  void onSwapChainChanged() {
    statusUsable_ = false;
    statusWhy_ = "the bridge fell back to a plain swap chain";
    havePrev_ = false;
    statsBad_ = 0;
    statsFlat_ = 0;
    statsAdvanced_ = false;
    unavailable_ = false;
    unavailableWhy_ = nullptr;
  }

  // The last good sample: the cumulative count and its time. False until a source has said anything, and when unavailable.
  bool get(uint64_t* total, int64_t* qpc, Source* source) const {
    if (unavailable_ || source_ == Source::None) return false;
    if (total) *total = total_;
    if (qpc) *qpc = qpc_;
    if (source) *source = source_;
    return true;
  }
  bool unavailable() const { return unavailable_; }
  const char* unavailableWhy() const { return unavailableWhy_ ? unavailableWhy_ : ""; }
  // Why the status source was given up on, or null while it is in use.
  const char* statusWhy() const { return statusUsable_ ? nullptr : statusWhy_; }

  // One log line per change of verdict (the first source, a switch of source, unavailable), false when there is nothing new to say.
  bool takeNote(char* buf, int len) {
    if (!buf || len <= 0 || !dirty_) return false;
    dirty_ = false;
    if (unavailable_) std::snprintf(buf, static_cast<size_t>(len), "perf: presented frames unavailable (%s)", unavailableWhy_ ? unavailableWhy_ : "unknown");
    else std::snprintf(buf, static_cast<size_t>(len), "perf: presented frames from %s", sourceName(source_));
    return true;
  }

 private:
  void setSource(Source s) {
    if (s == source_ && !unavailable_) return;
    source_ = s;
    unavailable_ = false;
    dirty_ = true;
  }
  void markUnavailable(const char* why) {
    if (unavailable_) return;
    unavailable_ = true;
    unavailableWhy_ = why;
    dirty_ = true;
  }

  bool statusUsable_ = true;
  bool statusCounted_ = false;
  unsigned badRun_ = 0;
  const char* statusWhy_ = nullptr;
  bool havePrev_ = false;
  uint32_t prev_ = 0;
  unsigned statsBad_ = 0, statsFlat_ = 0;
  bool statsAdvanced_ = false;
  bool unavailable_ = false;
  const char* unavailableWhy_ = nullptr;
  Source source_ = Source::None;
  bool dirty_ = false;
  uint64_t total_ = 0;
  int64_t qpc_ = 0;
};

}  // namespace fgpresented
}  // namespace edvr
