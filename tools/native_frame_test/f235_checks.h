// F235 checks (native on-foot VR), run235 below. Header only, run by native_frame_test.
//
// F235 (native_on_foot_core.h adsActionRejectStep, native_on_foot.cpp adsAimTick): a drop of the game's ADS under a held request the game had accepted (a reload) is held
// through for up to kAdsAcceptedDropMaxMs from the game's last ADS read, where the 1.5 s grace used to veto it near the reload's end. A request the game never accepted
// keeps the 1.5 s grace. The rig replays the Steam flight's reload (edvr_gfx_20261109_164250: Status.json off from 16:44:29.808 to 16:44:31.774 with the button held; the old
// rule latched at 16:44:31.306), a press the game refuses, a drop longer than the bridge, a release between presses, and the sites.
#pragma once

#include "../../src/d3d11/native_on_foot_core.h"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

namespace f235test {

namespace nf = edvr::nofoot;

inline std::string slurp235(const char* path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// One run of the step at 90 Hz from t0 to t1 (ms) with the request and the journal as given; Status.json's content changes every 250 ms. Returns the first ms the
// veto latched in the run (0 when it never did); `adsAll` says whether the chosen ADS (a fresh action) stayed on over the whole run.
struct Run {
    nf::AdsActionReject s;
    uint64_t revision = 1;
    uint64_t latchedAt = 0;
    bool adsAll = true;
    void span(uint64_t t0, uint64_t t1, bool action, bool journal) {
        for (uint64_t t = t0; t < t1; t += 11) {
            if (t / 250 != (t > 11 ? (t - 11) / 250 : 0)) ++revision;
            const bool veto = nf::adsActionRejectStep(s, action, t, true, journal, revision);
            if (veto && !latchedAt) latchedAt = t;
            const nf::AdsInputChoice c = nf::adsInputChoice(true, true, journal, true, action, 8000, veto);
            if (!c.ads) adsAll = false;
        }
    }
};

template <class Check>
inline void run235(Check&& check) {
    char msg[400];
    // ---- 1. the flight's reload: pressed (the game says ADS 0.57 s later), held, the game drops ADS for 1.97 s, takes it back --------------------------------------------
    {
        Run r;
        r.span(1000, 1570, true, false);    // 16:44:25.186: the press; the journal still off (its lag)
        r.span(1570, 5620, true, true);     // 16:44:25.760: the game says ADS
        r.span(5620, 7590, true, false);    // 16:44:29.808: the reload; the old rule latched 1.5 s in (16:44:31.306)
        r.span(7590, 10000, true, true);    // 16:44:31.774: the game takes ADS back
        std::snprintf(msg, sizeof(msg), "F235: the flight's reload is held through (latched at %llu ms, ADS on throughout %d, accepted %d)",
                      static_cast<unsigned long long>(r.latchedAt), r.adsAll ? 1 : 0, r.s.accepted ? 1 : 0);
        check(r.latchedAt == 0 && r.adsAll && r.s.accepted, msg);
    }
    // ---- 2. a press the game never accepts keeps the 1.5 s grace ------------------------------------------------------------------------------------------------------------
    {
        Run r;
        r.span(1000, 4000, true, false);
        std::snprintf(msg, sizeof(msg), "F235: a refused press is vetoed at the 1.5 s grace (latched at %llu ms, expected 2500..2520)", static_cast<unsigned long long>(r.latchedAt));
        check(r.latchedAt >= 2500 && r.latchedAt <= 2520 && !r.s.accepted, msg);
    }
    // ---- 3. a drop the game does not end is vetoed once the bridge runs out, measured from the game's last ADS read ------------------------------------------------------------
    {
        Run r;
        r.span(1000, 2000, true, true);
        r.span(2000, 9000, true, false);
        std::snprintf(msg, sizeof(msg), "F235: a drop past the bridge is vetoed at %llu ms (expected %llu..%llu, not at the 1.5 s grace)", static_cast<unsigned long long>(r.latchedAt),
                      static_cast<unsigned long long>(1989 + nf::kAdsAcceptedDropMaxMs), static_cast<unsigned long long>(2011 + nf::kAdsAcceptedDropMaxMs));
        check(r.latchedAt >= 1989 + nf::kAdsAcceptedDropMaxMs && r.latchedAt <= 2011 + nf::kAdsAcceptedDropMaxMs, msg);
        r.span(9000, 9500, true, true);   // the game takes ADS back: the veto clears, as before
        check(!r.s.latched && nf::adsActionRejectStep(r.s, true, 9500, true, false, r.revision) == false, "F235: the game's ADS read clears a latched veto and the bridge starts again");
    }
    // ---- 4. a release ends the press: the next press is a new one, with the 1.5 s grace -------------------------------------------------------------------------------------
    {
        Run r;
        r.span(1000, 2000, true, true);
        r.span(2000, 2100, false, false);   // released
        check(!r.s.accepted && !r.s.latched && r.s.edgeMs == 0, "F235: a release clears the accepted press");
        r.span(2100, 5000, true, false);    // pressed again, the game refuses it
        std::snprintf(msg, sizeof(msg), "F235: a new refused press after a release is vetoed at the 1.5 s grace (latched at %llu ms, expected 3600..3620)",
                      static_cast<unsigned long long>(r.latchedAt));
        check(r.latchedAt >= 3600 && r.latchedAt <= 3620, msg);
    }
    // ---- 5. unchanged Status.json content is no evidence, accepted or not ---------------------------------------------------------------------------------------------------------
    {
        nf::AdsActionReject s;
        nf::adsActionRejectStep(s, true, 1000, true, true, 7);
        bool any = false;
        for (uint64_t t = 1011; t < 1000 + 2 * nf::kAdsAcceptedDropMaxMs; t += 11) any = nf::adsActionRejectStep(s, true, t, true, false, 7) || any;
        check(!any && s.accepted, "F235: a drop with no changed Status.json content is never vetoed");
    }
    // ---- 6. the sites ---------------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        const std::string cpp = slurp235("src/d3d11/native_on_foot.cpp"), core = slurp235("src/d3d11/native_on_foot_core.h");
        const auto has = [&](const char* needle) { return cpp.find(needle) != std::string::npos; };
        check(core.find("const uint64_t graceMs = s.accepted ? kAdsAcceptedDropMaxMs : kAdsActionRejectGraceMs;") != std::string::npos &&
                  core.find("s.accepted = true;") != std::string::npos,
              "F235: the step's grace follows the accepted press (native_on_foot_core.h)");
        check(has("g_adsActionReject.accepted && !actionVeto && journalKnown && !journalValue") && has("native on foot ADS held through a game drop (F235)") &&
                  has("f235SightNote(g_adsSight.pos);"),
              "F235: adsAimTick opens and closes the drop and writes its line; adsSightTick notes the sight lock's movement");
    }
}

}  // namespace f235test
