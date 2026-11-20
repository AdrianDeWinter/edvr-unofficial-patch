#pragma once
// A VIEW WITH NO MOTION SOURCE, AND THE WAY BACK (design doc section 104, the training mission's turned-left view).
//
// The copy route's selector needs one draw whose shader pair the motion producer can substitute (engine_velocity_families.h) to name the
// scene's depth, camera and motion slots. A view made of ground and sky has none, and every frame of it is refused with
// no-supported-motion-source-pair. The 2026-10-07 training-mission log showed the camera injector in "warming" for the whole spell
// (injected=0, clean-closes=0): not a second fault, but the phase machine's right answer to frames that are never accepted, since a phase
// is chosen only after two clean accepted zero-phase frames (FlatLivePhase). This tracks the spell itself and the way out of it, so a
// flight can read, instead of infer, that the machine recovers and how long it takes:
//
//   spell     a run of consecutive frames refused for no source; the frame that ends it either is treated again (a recovery) or is refused
//             for another reason (abandoned: the view became a menu or a loading screen);
//   warm-up   the frames from the first treated frame after a spell until the first frame that carries a non-zero jitter phase.
//
// Plain counters; tools\flat_temporal_test runs the sequences of the log.
#include <cstdint>
#include <cstdio>
#include "flat_mono_frame.h"

namespace edvr {

struct FlatSourceSpellWindow {
    uint64_t frames = 0, noSourceFrames = 0, spells = 0, recoveries = 0, abandoned = 0, warmDone = 0, warmFrames = 0, warmMax = 0, warmAborted = 0;
    uint64_t longestSpell = 0;
    // Source-free frames (FlatMonoFrame::sourceFree): the scene held no pool draw and was treated from the camera term alone. Counted at
    // the Present, whether the resolver ran on them or not; the overlay count is the weapon glow passes admitted while no world was named.
    uint64_t sourceFreeFrames = 0, sourceFreeTreated = 0, overlaysUnnamed = 0;
    // The first-person path (2026-10-29: a tool in hand over bare terrain): the overlays above it admitted, the frames that held one, and
    // those of them the resolver ran on.
    uint64_t overlaysFirstPerson = 0, firstPersonFrames = 0, firstPersonTreated = 0;
    bool inSpell = false;
    uint64_t openSpell = 0;
};

class FlatSourceSpell {
public:
    // One frame that had a verdict (a final copy was seen). noSource: refused for no supported source pair. selected: the selector
    // selected it. nonzeroPhase: the frame carried a non-zero jitter phase.
    void frame(bool noSource, bool selected, bool nonzeroPhase) {
        ++w_.frames;
        if (noSource) {
            ++w_.noSourceFrames;
            if (!inSpell_) { inSpell_ = true; run_ = 0; ++w_.spells; }
            ++run_;
            if (run_ > w_.longestSpell) w_.longestSpell = run_;
            if (warming_) { ++w_.warmAborted; warming_ = false; }
            return;
        }
        if (inSpell_) {
            inSpell_ = false; run_ = 0;
            if (selected) { ++w_.recoveries; warming_ = true; warm_ = 0; }
            else ++w_.abandoned;
        }
        if (warming_) {
            if (nonzeroPhase) { ++w_.warmDone; w_.warmFrames += warm_; if (warm_ > w_.warmMax) w_.warmMax = warm_; warming_ = false; }
            else if (selected) ++warm_;
            else { ++w_.warmAborted; warming_ = false; }
        }
    }
    // A frame the selector took with no motion source (the view is the camera term alone), and whether the resolver treated it.
    void sourceFreeFrame(bool treated) { ++w_.sourceFreeFrames; if (treated) ++w_.sourceFreeTreated; }
    // A weapon glow pass admitted as an overlay although no draw had named the world: the HDR target's own first camera was the world's.
    // firstPerson: admitted on the first-person path (a first person in the frame, the first camera the last world's by its shape).
    void overlayUnnamed(bool firstPerson = false) { ++w_.overlaysUnnamed; if (firstPerson) ++w_.overlaysFirstPerson; }
    // A frame that held a first-person-path admission, and whether the resolver treated it.
    void firstPersonUnnamedFrame(bool treated) { ++w_.firstPersonFrames; if (treated) ++w_.firstPersonTreated; }
    // The window since the last take (the state, an open spell and a warm-up in progress, carries over).
    FlatSourceSpellWindow take() {
        FlatSourceSpellWindow out = w_;
        out.inSpell = inSpell_; out.openSpell = inSpell_ ? run_ : 0;
        w_ = FlatSourceSpellWindow{};
        // A spell that is still open continues into the next window as its own run; its length so far is the next window's floor.
        if (inSpell_) w_.longestSpell = run_;
        return out;
    }
    void reset() { *this = FlatSourceSpell{}; }
private:
    FlatSourceSpellWindow w_;
    bool inSpell_ = false, warming_ = false;
    uint64_t run_ = 0, warm_ = 0;
};

// The pair's words for the line: whether the motion producer substitutes its vertex shader (a pool family with another pixel shader), and
// whether the flat projection recipes know the pair. The runtime fills them; the selector's summary cannot (it is plain data).
struct FlatSourcelessPairNote { bool familyVs = false, recipe = false; };

// Two lines (the logger cuts a line at about 1167 characters and this one was 1,020 with a legend and up to four pairs): the fields, and the top
// pairs. What the legend said: a spell is a run of frames refused for no supported motion source pair (the view holds a draw of a pool family the
// motion producer leaves stock); source-free frames held no pool-family draw at all (open ground and sky) and take the camera term alone, treated
// counts those the resolver ran on; recoveries end a spell with a treated frame and warm-frames count the zero-phase frames after it until the
// first non-zero phase (two, by FlatLivePhase), abandoned ends one with another refusal (a menu or a loading screen); family-vs is a pool-family
// vertex shader drawn with an unkeyed pixel shader, recipe a pair the projection recipes know.
inline constexpr unsigned kFlatSourceLineParts = 2;
inline int flatSourceSpellLine(char* out, size_t n, const FlatSourceSpellWindow& w, const FlatMonoSourceless& last, uint64_t lastFrame,
                               const FlatSourcelessPairNote (&notes)[4], unsigned part = 0) {
    char pairs[1200];
    size_t at = 0;
    pairs[0] = 0;
    for (uint32_t i = 0; i < last.topCount; ++i) {
        const auto& p = last.top[i];
        const int k = std::snprintf(pairs + at, sizeof(pairs) - at,
            "%s[VS=%016llX PS=%016llX draws=%u records=%u same-camera=%u pool-kind=%u family-vs=%u recipe=%u]", i ? " " : "",
            static_cast<unsigned long long>(p.vs), static_cast<unsigned long long>(p.ps), p.draws, p.records, p.sameCamera ? 1u : 0u,
            p.pool ? 1u : 0u, notes[i].familyVs ? 1u : 0u, notes[i].recipe ? 1u : 0u);
        if (k < 0 || at + static_cast<size_t>(k) >= sizeof(pairs)) break;
        at += static_cast<size_t>(k);
    }
    if (!at) std::snprintf(pairs, sizeof(pairs), "none");
    if (part == 1) return std::snprintf(out, n, "flat source 5s (2/2): top: %s", pairs);
    if (part >= kFlatSourceLineParts) return 0;
    return std::snprintf(out, n,
        "flat source 5s: frames=%llu no-source-frames=%llu spells=%llu longest-spell=%llu open-spell=%llu recoveries=%llu abandoned=%llu "
        "warm-ups-done=%llu warm-frames-total=%llu warm-frames-max=%llu warm-ups-aborted=%llu source-free-frames=%llu source-free-treated=%llu "
        "overlays-without-named-world=%llu first-person-path=%llu first-person-frames=%llu first-person-treated=%llu; last no-source frame=%llu: "
        "records-on-scene-depth=%u draws=%u same-camera-draws=%u pool-kind-records=%u distinct-pairs=%u; top pairs in the next line",
        static_cast<unsigned long long>(w.frames), static_cast<unsigned long long>(w.noSourceFrames),
        static_cast<unsigned long long>(w.spells), static_cast<unsigned long long>(w.longestSpell),
        static_cast<unsigned long long>(w.openSpell), static_cast<unsigned long long>(w.recoveries),
        static_cast<unsigned long long>(w.abandoned), static_cast<unsigned long long>(w.warmDone),
        static_cast<unsigned long long>(w.warmFrames), static_cast<unsigned long long>(w.warmMax),
        static_cast<unsigned long long>(w.warmAborted), static_cast<unsigned long long>(w.sourceFreeFrames),
        static_cast<unsigned long long>(w.sourceFreeTreated), static_cast<unsigned long long>(w.overlaysUnnamed),
        static_cast<unsigned long long>(w.overlaysFirstPerson), static_cast<unsigned long long>(w.firstPersonFrames),
        static_cast<unsigned long long>(w.firstPersonTreated),
        static_cast<unsigned long long>(lastFrame),
        last.records, last.draws, last.sameCameraDraws, last.poolRecords, last.distinctPairs);
}

// WHAT VETOED THE SOURCE-FREE ADMISSION (2026-11-08, a flat report of DLSS dropping to spatial on a planet when the SRV left the view).
// The log of it, on foot, had nine spells of no-supported-motion-source-pair over views of ground and sky: records-on-scene-depth 2, one pair, no
// pool-kind record, the same pair that other windows took source-free. The selector refuses a no-source view only for a stock pool-family draw
// (FlatMonoFrameInput::unsupportedFamilyDraws) or an unsupported pool-kind record on the HDR's depth, and the summary above lists the HDR's depth
// alone, so the draw that decided it was named nowhere. This window counts the frames and names the draws:
//   frames                  frames with a verdict (a final copy was seen);
//   with-stock-family       of them, frames that held a stock pool-family draw anywhere in a scene-shaped depth;
//   of-those-selected       of those, the frames the selector took anyway (a supported draw was in view, so the veto was never asked: the SRV
//                           on screen, if the report is right);
//   refused-by-family       frames refused for no source whose verdict counted a stock family draw (the selector's own answer);
//   refused-by-pool         frames refused for no source whose verdict counted none (so an unsupported pool-kind record on the HDR's depth).
struct FlatSourceVetoWindow {
    uint64_t frames = 0, withFamily = 0, withFamilySelected = 0, refusedByFamily = 0, refusedByPool = 0;
    // noSource: refused for no supported motion source pair. selected: the selector took it. familyDraws: the frame held a stock family draw at
    // its end. verdictFamily: the refusing verdict counted one (FlatMonoSourceless::vetoFamilyDraws).
    void frame(bool noSource, bool selected, bool familyDraws, bool verdictFamily) {
        ++frames;
        if (familyDraws) { ++withFamily; if (selected) ++withFamilySelected; }
        if (noSource) { if (verdictFamily) ++refusedByFamily; else ++refusedByPool; }
    }
};

// One line (under 850 characters at the widest digits, inside kFlatLogLineBudget): the window, then the last refused frame's causes and the
// stock draws that frame held. depth=hdr says the draw wrote the HDR's own depth, depth=other a different scene-shaped depth. A depth=hdr draw
// can still be missing from the source line's pairs: that summary walks only the records the selector assembled (the targets' writes, the
// tone, the sources), and a G-buffer draw is none of them (Sean's 11:03 trace). target is the colour target's size (0x0 for a depth-only draw).
inline int flatSourceVetoLine(char* out, size_t n, const FlatSourceVetoWindow& w, const FlatMonoSourceless& last, uint64_t lastFrame,
                              const FlatFamilyVeto (&v)[kFlatFamilyVetoKinds], uint32_t kinds, uint32_t overflowDraws) {
    char draws[760];
    size_t at = 0;
    draws[0] = 0;
    if (kinds > kFlatFamilyVetoKinds) kinds = kFlatFamilyVetoKinds;
    for (uint32_t i = 0; i < kinds; ++i) {
        const auto& e = v[i];
        const int k = std::snprintf(draws + at, sizeof(draws) - at,
            "%s[VS=%016llX PS=%016llX draws=%u depth=%s target=%ux%u color=%u camera=%u]", i ? " " : "",
            static_cast<unsigned long long>(e.vs), static_cast<unsigned long long>(e.ps), e.draws,
            e.depth && e.depth == last.depth ? "hdr" : "other", e.width, e.height, e.color ? 1u : 0u, e.camera ? 1u : 0u);
        if (k < 0 || at + static_cast<size_t>(k) >= sizeof(draws)) break;
        at += static_cast<size_t>(k);
    }
    if (!at) std::snprintf(draws, sizeof(draws), "none");
    return std::snprintf(out, n,
        "flat source veto 5s: frames=%llu with-stock-family=%llu of-those-selected=%llu refused-by-family=%llu refused-by-pool=%llu; "
        "last refused frame=%llu: stock-family-draws=%u unsupported-pool-draws=%u kinds=%u past-the-kinds=%u; stock draws: %s",
        static_cast<unsigned long long>(w.frames), static_cast<unsigned long long>(w.withFamily),
        static_cast<unsigned long long>(w.withFamilySelected), static_cast<unsigned long long>(w.refusedByFamily),
        static_cast<unsigned long long>(w.refusedByPool), static_cast<unsigned long long>(lastFrame),
        last.vetoFamilyDraws, last.vetoPoolDraws, kinds, overflowDraws, draws);
}

}  // namespace edvr
