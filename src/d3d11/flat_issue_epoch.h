#pragma once
// Owner/trust epoch for the flat geometry Issue certificate.
//
// A successful fresh validation may be reused only while this epoch is unchanged.
// Any setter that can change the shaders, predication, depth view, depth-stencil
// state, VS b1 binding/range, or the certified buffer's bytes bumps it. An
// unhooked Context1 writer freezes reuse instead of guessing. The fresh
// OM/VS/PS queries stay the failure path.

#include <atomic>
#include <cstdint>

#include "../common/runtime_profile.h"

namespace edvr {

struct FlatIssueEpochState {
    std::atomic<uint64_t> epoch{1};
    // Sub-epochs for OM state that survives shader/CB switches:
    // - omDepthEpoch: DSV binding and depth-stencil state (bumped by RT/DSV, DepthStencilState, Revoke).
    // - omOutputEpoch: RTV[0..7], OM UAVs, and blend state (bumped by RT/DSV, OM UAV, BlendState, Revoke).
    // - omUavMaybeBound: 0 only when OMSetRenderTargets/ClearState proved no OM UAVs are bound; conservatively 1 otherwise.
    std::atomic<uint64_t> omDepthEpoch{1};
    std::atomic<uint64_t> omOutputEpoch{1};
    std::atomic<uint32_t> omUavMaybeBound{1};
    std::atomic<uintptr_t> omLastDsv{1}; // sentinel 1 never matches a valid pointer or nullptr
    std::atomic<uintptr_t> buffer{0};
    std::atomic<uint32_t> frozen{0};
    std::atomic<uint64_t> reuse{0};
    std::atomic<uint64_t> fresh{0};
    std::atomic<uint64_t> observeReuse{0};
    std::atomic<uint64_t> observeFresh{0};
    std::atomic<uint64_t> depthSets{0};
    std::atomic<uint64_t> stageSets{0};
    std::atomic<uint64_t> rtSets{0};
    std::atomic<uint64_t> cb1Sets{0};
    std::atomic<uint64_t> cbWrites{0};
    std::atomic<uint64_t> revokes{0};
};

inline FlatIssueEpochState& flatIssueEpochState() {
    static FlatIssueEpochState state;
    return state;
}
inline uint64_t flatIssueEpochLoad() noexcept {
    return flatIssueEpochState().epoch.load(std::memory_order_acquire);
}
inline uint64_t flatOmDepthEpochLoad() noexcept {
    return flatIssueEpochState().omDepthEpoch.load(std::memory_order_acquire);
}
inline uint64_t flatOmOutputEpochLoad() noexcept {
    return flatIssueEpochState().omOutputEpoch.load(std::memory_order_acquire);
}
inline bool flatOmUavKnownZero() noexcept {
    return flatIssueEpochState().omUavMaybeBound.load(std::memory_order_acquire) == 0;
}
inline bool flatIssueEpochFrozen() noexcept {
    return flatIssueEpochState().frozen.load(std::memory_order_acquire) != 0;
}
inline void flatIssueEpochFreeze() noexcept {
    auto& state = flatIssueEpochState();
    state.omUavMaybeBound.store(1u, std::memory_order_release);
    state.frozen.store(1, std::memory_order_release);
}
inline void flatIssueEpochBump(std::atomic<uint64_t>* counter) noexcept {
    auto& state = flatIssueEpochState();
    state.epoch.fetch_add(1, std::memory_order_acq_rel);
    if (counter) counter->fetch_add(1, std::memory_order_relaxed);
}
// The setter-class bumps: the hooks on the owner context's state setters (vscreen.cpp) and EDVR's own Raw
// setters. Only the flat Issue certificate reads the epoch they move, and that certificate is stamped only in
// a flat draw scope and reused only at the epoch it was stamped at, so while the VR profile runs the two
// locked read-modify-writes buy nothing and are skipped. Every other profile (Flat, Passthrough, Invalid)
// still bumps. The test is runtimeVrProfile(), not "not flat": a profile that is neither stays as it was.
//
// A live switch stays correct because leaving Flat clears the certificate and revokes the epoch before the
// profile flips (flatRuntimeResize -> forwardReset -> geometryCertClear -> flatIssueEpochRevoke, run by
// mode_watch.cpp commitPosted ahead of runtimeProfileSet), and VR never stamps one: a setter skipped in
// VR can never touch a certificate that is still reusable. Revoke and the NoteResource path below are not
// gated: they cover ClearState, command lists, context swaps and writes of the certified buffer, not the
// per-draw setters.
inline bool flatIssueEpochSettersCounted() noexcept { return !runtimeVrProfile(); }
inline void flatOmNoteUavBound(bool maybeBound) noexcept {
    if (flatIssueEpochSettersCounted())
        flatIssueEpochState().omUavMaybeBound.store(maybeBound ? 1u : 0u, std::memory_order_release);
}
inline void flatIssueEpochBumpOutput() noexcept {
    if (flatIssueEpochSettersCounted())
        flatIssueEpochState().omOutputEpoch.fetch_add(1, std::memory_order_acq_rel);
}
inline void flatIssueEpochBumpSetter(std::atomic<uint64_t>* counter) noexcept {
    if (flatIssueEpochSettersCounted()) flatIssueEpochBump(counter);
}
inline void flatIssueEpochBumpDepth() noexcept { flatIssueEpochBumpSetter(&flatIssueEpochState().depthSets); if (flatIssueEpochSettersCounted()) flatIssueEpochState().omDepthEpoch.fetch_add(1, std::memory_order_acq_rel); }
inline void flatIssueEpochBumpStage() noexcept { flatIssueEpochBumpSetter(&flatIssueEpochState().stageSets); }
inline void flatIssueEpochBumpRt() noexcept { flatIssueEpochBumpSetter(&flatIssueEpochState().rtSets); if (flatIssueEpochSettersCounted()) { auto& s = flatIssueEpochState(); s.omLastDsv.store(1u, std::memory_order_release); s.omDepthEpoch.fetch_add(1, std::memory_order_acq_rel); s.omOutputEpoch.fetch_add(1, std::memory_order_acq_rel); } }
inline void flatIssueEpochBumpRtDsv(const void* dsv) noexcept {
    flatIssueEpochBumpSetter(&flatIssueEpochState().rtSets);
    if (flatIssueEpochSettersCounted()) {
        auto& s = flatIssueEpochState();
        s.omOutputEpoch.fetch_add(1, std::memory_order_acq_rel);
        const uintptr_t cur = reinterpret_cast<uintptr_t>(dsv);
        if (s.omLastDsv.exchange(cur, std::memory_order_acq_rel) != cur)
            s.omDepthEpoch.fetch_add(1, std::memory_order_acq_rel);
    }
}
inline void flatIssueEpochBumpCb() noexcept { flatIssueEpochBumpSetter(&flatIssueEpochState().cb1Sets); }
// Frame changes, ClearState, command lists and context swaps. No setter counter:
// a flight with fresh draws and quiet setters reads this instead.
inline void flatIssueEpochRevoke() noexcept {
    auto& state = flatIssueEpochState();
    state.epoch.fetch_add(1, std::memory_order_acq_rel);
    state.omDepthEpoch.fetch_add(1, std::memory_order_acq_rel);
    state.omOutputEpoch.fetch_add(1, std::memory_order_acq_rel);
    state.omUavMaybeBound.store(1u, std::memory_order_release);
    state.omLastDsv.store(1u, std::memory_order_release);
    state.buffer.store(0, std::memory_order_release);
    state.revokes.fetch_add(1, std::memory_order_relaxed);
}
inline void flatIssueEpochPublishBuffer(uintptr_t buffer) noexcept {
    flatIssueEpochState().buffer.store(buffer, std::memory_order_release);
}
// A write of the buffer a live certificate names. A null resource is an unknown
// destination and revokes too. Any other resource leaves the epoch alone.
inline void flatIssueEpochNoteResource(const void* resource) noexcept {
    auto& state = flatIssueEpochState();
    const uintptr_t watched = state.buffer.load(std::memory_order_acquire);
    if (!watched) return;
    if (resource && reinterpret_cast<uintptr_t>(resource) != watched) return;
    state.epoch.fetch_add(1, std::memory_order_acq_rel);
    state.cbWrites.fetch_add(1, std::memory_order_relaxed);
}
inline void flatIssueEpochCountReuse() noexcept {
    flatIssueEpochState().reuse.fetch_add(1, std::memory_order_relaxed);
}
inline void flatIssueEpochCountFresh() noexcept {
    flatIssueEpochState().fresh.fetch_add(1, std::memory_order_relaxed);
}
inline void flatIssueEpochCountObserveReuse() noexcept {
    flatIssueEpochState().observeReuse.fetch_add(1, std::memory_order_relaxed);
}
inline void flatIssueEpochCountObserveFresh() noexcept {
    flatIssueEpochState().observeFresh.fetch_add(1, std::memory_order_relaxed);
}

struct FlatIssueEpochWindow {
    uint64_t reuse = 0, fresh = 0, observeReuse = 0, observeFresh = 0;
    uint64_t depthSets = 0, stageSets = 0, rtSets = 0, cb1Sets = 0, cbWrites = 0, revokes = 0;
    uint32_t frozen = 0;
};
inline uint64_t flatIssueEpochTake(std::atomic<uint64_t>& counter) noexcept {
    return counter.exchange(0, std::memory_order_relaxed);
}
inline FlatIssueEpochWindow flatIssueEpochTakeWindow() noexcept {
    auto& state = flatIssueEpochState();
    FlatIssueEpochWindow window;
    window.reuse = flatIssueEpochTake(state.reuse);
    window.fresh = flatIssueEpochTake(state.fresh);
    window.observeReuse = flatIssueEpochTake(state.observeReuse);
    window.observeFresh = flatIssueEpochTake(state.observeFresh);
    window.depthSets = flatIssueEpochTake(state.depthSets);
    window.stageSets = flatIssueEpochTake(state.stageSets);
    window.rtSets = flatIssueEpochTake(state.rtSets);
    window.cb1Sets = flatIssueEpochTake(state.cb1Sets);
    window.cbWrites = flatIssueEpochTake(state.cbWrites);
    window.revokes = flatIssueEpochTake(state.revokes);
    window.frozen = state.frozen.load(std::memory_order_acquire);
    return window;
}

}  // namespace edvr
