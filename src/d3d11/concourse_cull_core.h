#pragma once
// The concourse cull's pure half (docs\concourse-cull-2026-11-05.md): on foot in a large station's social space, the records that are not the room are
// not drawn. Nothing here touches Windows, the game or the log, so the optimized station's rig can test every rule. The engine half is in
// optimized_station.cpp (the judgement at the rig, the producer and the store) and concourse_pane.cpp (the panes).
//   concourseAllowName       the name allow-list: a prefix match, exact case, plus the temporary extra list (diag.concourse_keep)
//   concourseSocialOnFoot    the gate: Status.json Flags2 bit 0 (on foot) and bit 14 (social space), the file known, gameplay seen
//   concourseJudge           one record: keep (and why) or cull
//   concourseRigVetoed       one rig: vetoed only when a record of it was judged and none was kept
//   PaneTriple, paneTripleMatch, parsePaneList   a glass pane named by its vertex shader, pixel shader and index count (0: any count)
//   kPaneFamilyVs, kPaneFamilyPs, kPlumePs, kTwinPs, kBayIndexCount   the glass family, the plume pair, the bay window's index count
//   kWithheldPs, kWithheldVs, withheldPsIndex, withheldVsIndex   the outside draws not issued while the cull is on, named by shader
//   fillPaneT8, fillPaneT9, fillPaneT10   the texels of the mirror materials (uniform)
//   markDepthForDistance, PaneSides, paneTestThisFrame   the mirror's mark depth, and each pane's side (interior or exterior) learned from two occlusion counts
//   ClearMachine   the bay glass's clearing on approach: a pure state machine
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "engine_velocity_families.h"
#include "journal_station.h"

namespace edvr::concourse {

// The Flags2 bits (journal documentation): 0 OnFoot, 14 OnFootSocialSpace. Bit 3 (in station) rides with them in 0x00014009.
constexpr uint32_t kFlags2OnFoot = 1u << 0;
constexpr uint32_t kFlags2OnFootInHangar = 1u << 13;
constexpr uint32_t kFlags2SocialSpace = 1u << 14;

// The anchor record's name: the large station's concourse. Its sighting by the store is the second half of the gate.
constexpr const char* kAnchorName = "Standard_SocialArea";
constexpr const char* kHubName = "SA_StationHub_Compact";
// A surface port's room has no Standard_SocialArea. Its own hub anchors it: r=42.1, centred 3.5 m from where Sean stood in the middle of the room (Flight 35, a CraterOutpost).
constexpr const char* kPortAnchorName = "SA_PlanetPortHub_Compact";
// An orbital outpost's room, the same way: r=33.3, 9.6 m from where Sean stood at the bar (two surveys, after Flight 37).
constexpr const char* kOutpostAnchorName = "SA_OutpostHub_Compact";
// A fleet carrier's concourse: r=32.5, 4.0 m from where Sean stood in it (five surveys, after Flight 39). The bar is 25 m from it and the bridge 45 m, the bridge's own record
// (SA_Carrier_01_Bridge) 55 m from the bar's: one room for the cull, one bake from this centre for both.
constexpr const char* kCarrierAnchorName = "SA_Carrier_01";
inline bool concourseAnchorName(const char* name) noexcept {
    return name && (std::strcmp(name, kAnchorName) == 0 || std::strcmp(name, kPortAnchorName) == 0 || std::strcmp(name, kOutpostAnchorName) == 0 ||
                    std::strcmp(name, kCarrierAnchorName) == 0);
}

// The name prefixes that are the room. Exact case.
// Root and Perturbator are the room's too: off this list they stayed only while within kCloseDistanceM, so the bar's tables popped in and flickered at that distance (Flight 31).
// RandomDirt, Narrow_Door_ and Arm_ are a fleet carrier's: dirt on its concourse floor 8 m from its anchor, its doors 35 to 42 m off, arms 10 m from the bridge; seen nowhere else surveyed,
// and off this list they would pop at kCloseDistanceM too.
constexpr const char* kAllowPrefixes[] = {"Standard_SocialArea", "SA_",         "PS_",        "STAT_", "FUI_",        "PROP_", "GUI_Interactive", "Interactive_", "TP_",
                                          "HangarLobby",         "TurboLift",    "Root",        "Perturbator", "RandomDirt", "Narrow_Door_", "Arm_"};
constexpr size_t kAllowPrefixCount = sizeof(kAllowPrefixes) / sizeof(kAllowPrefixes[0]);

inline bool hasPrefixExact(const char* name, const char* prefix, size_t n) noexcept { return n > 0 && std::strncmp(name, prefix, n) == 0; }

// True when `name` starts with one of the comma-separated extra prefixes (diag.concourse_keep; spaces around an entry are ignored, an empty entry matches nothing),
// and only those: the built-in allow-list is not looked at. These names are far-exempt as well as allowed. A null or empty name or list matches nothing.
// concourseAllowName is the built-in allow-list or an extra prefix. (The judge keeps an unreadable name by its own rule.)
inline bool extraKeepName(const char* name, const char* extraPrefixes) noexcept {
    if (!name || !*name || !extraPrefixes) return false;
    const char* s = extraPrefixes;
    while (*s) {
        while (*s == ' ' || *s == '\t' || *s == ',') ++s;
        const char* e = s;
        while (*e && *e != ',') ++e;
        const char* t = e;
        while (t > s && (t[-1] == ' ' || t[-1] == '\t')) --t;
        if (t > s && hasPrefixExact(name, s, static_cast<size_t>(t - s))) return true;
        s = e;
    }
    return false;
}
inline bool concourseAllowName(const char* name, const char* extraPrefixes) noexcept {
    if (!name || !*name) return false;
    for (const char* p : kAllowPrefixes)
        if (hasPrefixExact(name, p, std::strlen(p))) return true;
    return extraKeepName(name, extraPrefixes);
}

// The gate: on foot and in social space by Status.json, the on-foot word known, and gameplay seen in this process. Gameplay is required: a leftover
// on-foot bit otherwise culls the main menu's hangar. `known` is journalOnFootKnown() or, in the flat profile (where that is hard false), gameplay.
inline bool concourseSocialOnFoot(uint32_t flags2, bool known, bool gameplay) noexcept {
    return gameplay && known && (flags2 & kFlags2OnFoot) != 0 && (flags2 & kFlags2SocialSpace) != 0;
}
// The concourse list stays off while Status says the commander is still in the hangar. That bit is set on foot beside the ship (Flags2 0x00016009) and clears at the concourse.
inline bool concourseCullGate(bool armed, bool social, bool sealed, bool inHangar) noexcept {
    return armed && social && !sealed && !inHangar;
}

// A record that is small and close is kept whatever its name: a weapon or a prop whose name is generic (Barrel, Slider, Hinge).
constexpr float kSmallRadiusM = 25.0f;
constexpr float kCloseDistanceM = 40.0f;
// Standing in the cabin with the doors open, a small floor panel's centre sits just past 40 m (Orbis flight 062511: the panel
// drew until the normal cull came on, and came back on the step out of the door, about 2 m). The wider keep covers that step.
constexpr float kOpenCabinCloseM = 48.0f;
// The doorway is outside kLiftInsideM and still inside the cabin sphere. Flight 070009: the panel dropped at 1.83 m from the
// centre, the sphere radius 2.40 m, and a run across the threshold flashed it. The wider keep holds for a step past the sphere.
constexpr float kOpenCabinPastM = 1.0f;
// An allowed name whose sphere is wholly farther than this from the camera is culled all the same (the plant stacks, the far hangar lobby, the huge logo);
// the anchor and the hub are exempt, because the gate reads them from the store.
constexpr float kAllowedFarM = 200.0f;

enum class Judge : uint8_t { kKeepName, kKeepUnreadableName, kKeepUnreadableSphere, kKeepSmallClose, kCull };

inline bool judgeKeeps(Judge j) noexcept { return j != Judge::kCull; }

// The lift: while the player stands in a shut cabin the room is not drawn. Kept: an unreadable sphere, and a record of at most kLiftFloorRadiusM whose
// surface is within kLiftKeepM. That covers the floor in front of the doors (HangarLobby is 29.3 m, just past the 25 m prop cap, and the camera in the
// cabin is outside its sphere) and the hub underfoot. The exterior shell is 90.7 m and stays culled. Everything else is culled, whatever its name.
constexpr float kLiftKeepM = 8.0f;
constexpr float kLiftFloorRadiusM = 50.0f;

// One record. A record whose name is unreadable, or an allowed name, is kept unless its sphere is readable and wholly past kAllowedFarM (distance - radius); the
// anchor and the hub (`farExempt`, computed from the name by the caller) are kept however far. Then: keep an unreadable sphere (not finite, or no radius); keep a
// small, close record; cull. `lift` (the player is in a shut lift cabin) replaces all of that by the rule above kLiftKeepM; `farExempt` does not apply to it.
// `openCabin` (the camera is within the cabin sphere plus kOpenCabinPastM, doors already open) uses kOpenCabinCloseM for that small-close keep. The lift and the look ignore it.
// `look` (TEMPORARY, the camera test) is the keep list turned inside out, what a bake would want: the room off and the rest on. An allowed name is culled however far, the anchor and
// the hub included. An unreadable name is the room's when it is small and reaches within kAllowedFarM of the camera, which is at the room's centre then (INFERRED). Any other name is the
// room's when it is small and reaches within kLookReachM: Flight 25's gaps were walkways, doors, panels and people hanging in the bake's way, which a depth filter can only leave out, with
// nothing behind them, since the bake sees from one place. Not drawn, what is behind them is captured. A large record is kept: it may be the station.
constexpr float kLookRoomRadiusM = 60.0f, kLookReachM = 60.0f;
inline Judge concourseJudge(bool nameReadable, bool allowed, bool sphereReadable, float distance, float radius, bool farExempt, bool lift = false, bool look = false,
                            bool openCabin = false) noexcept {
    const bool sphereOk = sphereReadable && std::isfinite(distance) && std::isfinite(radius) && radius > 0.0f;
    if (look) {
        if (allowed) return Judge::kCull;
        if (sphereOk && radius <= kLookRoomRadiusM && distance - radius <= (nameReadable ? kLookReachM : kAllowedFarM)) return Judge::kCull;
        return nameReadable ? Judge::kKeepName : Judge::kKeepUnreadableName;
    }
    if (lift) {
        if (!sphereOk) return Judge::kKeepUnreadableSphere;
        const float surface = distance - radius > 0.0f ? distance - radius : 0.0f;
        if (radius <= kLiftFloorRadiusM && surface <= kLiftKeepM) return Judge::kKeepSmallClose;
        return Judge::kCull;
    }
    const bool pastRoom = sphereOk && distance - radius > kAllowedFarM;
    if (!nameReadable) return pastRoom ? Judge::kCull : Judge::kKeepUnreadableName;
    if (allowed) return (!farExempt && pastRoom) ? Judge::kCull : Judge::kKeepName;
    if (!sphereOk) return Judge::kKeepUnreadableSphere;
    const float closeM = openCabin ? kOpenCabinCloseM : kCloseDistanceM;
    if (radius <= kSmallRadiusM && distance <= closeM) return Judge::kKeepSmallClose;
    return Judge::kCull;
}

// A rig's records in one pass: how many were judged and how many of those were kept.
struct RigTally {
    uint32_t judged = 0;
    uint32_t kept = 0;
    void note(bool keep) noexcept {
        ++judged;
        if (keep) ++kept;
    }
};

// A rig is vetoed only if at least one of its records was judged in the pass and none was kept. A rig with nothing judged (never seen, or only
// records the judgement could not see) forwards.
inline bool concourseRigVetoed(uint32_t judged, uint32_t kept) noexcept { return judged > 0 && kept == 0; }
inline bool concourseRigVetoed(const RigTally& t) noexcept { return concourseRigVetoed(t.judged, t.kept); }

// ---- The glass panes: a draw named by its vertex shader hash, pixel shader hash and index count -----------------------------------------------
struct PaneTriple {
    uint64_t vs = 0;
    uint64_t ps = 0;
    uint32_t n = 0;
};

constexpr size_t kPaneListCap = 16;

// The glass family seen at the bay window (flight 1): one vertex shader and one pixel shader, any index count. The bay window is 36 indices; the other
// counts of the family (192, 864) are unidentified.
constexpr uint64_t kPaneFamilyVs = 0x98397963AAEC45D3ull;
constexpr uint64_t kPaneFamilyPs = 0x66B08F89E4926C01ull;
constexpr uint32_t kBayIndexCount = 36;
// The draws that come after the panes and paint over them: the plume and its 1024x1024 twin, named by their pixel shaders (EDVR swaps the plume's vertex
// shader at draw time, so the vertex shader is not a stable name).
constexpr uint64_t kPlumePs = 0x3B0B38CD96F53BC1ull;
constexpr uint64_t kTwinPs = 0x34A1475B2C790DFAull;

// ---- The draws withheld while the cull is on: what is outside the room (smoke, volumetrics, ship lasers, engine trails, the planet) --------------------
// Named by pixel shader hash or vertex shader hash (a draw with either is not issued). A terrain patch has no pixel shader, so it is named by its vertex
// shader. Not here, on purpose: the player's first-person laser (0x88DCF1164C640EC3 / 494506A6), the room's glow quads (359BF8FF), the sky (F8FA801F),
// the stars (68DDDEF0, F7A6E916), the dust (E904D334). Beams, flares and the scatter are non-indexed draws, so the plain draw thunks carry the scope too.
// sealedToo: also withheld while the hangar is sealed (fix.optimized_stations): it can only be outside. Not so for what a hangar may draw itself: flares, the scatter,
// the particle batches and an engine glow (its own ship's on the pad); no census has been taken in a sealed hangar yet to say otherwise.
// washToo: also withheld while the windows show the bake, the cull lifted or not. The scatter is a wash over the whole screen, room included. When the cull lifted at the bay it came back in
// one frame (Flight 25's second clip, Sean: "anything we can do to make this blend better?"), and the bake has none in it, so the room and the windows jumped together.
// skySide: the planet, its atmosphere and the sun's glare. In a room whose windows show the real sky (RoomLayout::realSky, a fleet carrier) they are drawn with the cull on, as the stars
// always are (Sean, after Flight 46: "Can we exclude the skybox/planets/sunlight from being culled here?"). Ships and weapons fire stay withheld ("I want the ships to still be culled,
// it's only the background I want kept") until a window clears ("Can we still use the window clears distance to restore those culled objects in this scenario?").
struct WithheldFamily {
    uint64_t hash;
    const char* name;
    bool sealedToo = false;
    bool washToo = false;
    bool skySide = false;
};
constexpr WithheldFamily kWithheldPs[] = {
    {0x3B0B38CD96F53BC1ull, "plume", true},
    {0x34A1475B2C790DFAull, "plume twin", true},
    {0x42AC0CACC9CDF72Bull, "trail ribbon", true},
    {0xEEAAC839A9F09448ull, "trail volume", true},
    {0x6FD4C38BA927C8C7ull, "trail haze", true},
    {0xA75C1DB6562B8CA7ull, "beams and bolts", true},
    {0xC5DF9CC943476289ull, "multicannon rounds", true},
    {0xC49F999F7D3C801Dull, "shield strike", true},
    {0xBD801F2FB02522EBull, "smoke", true},
    {0x6EF82262EB12A037ull, "atmosphere", true, false, true},
    {0x5BCB6B95BE7C0700ull, "atmosphere quad A", true, false, true},
    {0x97DBC87FCAA429C4ull, "atmosphere quad B", true, false, true},
    {0xD56F859BE4781431ull, "flares"},
    {0x43E5E6EB67AC751Bull, "planet surface", true, false, true},
    {0x41152F82C6E8BE1Full, "scatter", false, true},  // VS AFFEF018: fullscreen, additive, no depth test, reads only the depth
    {0x2BAE3742FEB916D9ull, "particles A"},           // VS AFBCD3AD, 9F4BBCFC (docs\billboard-root-cause-2026-10-16.md)
    {0x3789CA2062E196FBull, "particles B"},           // VS 9AEC596A
    {0xE54F2A902E5631F6ull, "particles C"},           // VS BBAD1CA8
    {0x6EAA86EFE135B2D4ull, "shield strike part 2", true},  // VS 820E5C13, the same 3984 indices as the shield strike
    {0xDD371C57C9093BB8ull, "ship ribbon", true},           // VS 5EAFFCD0: a triangle strip of about 100 indices, two draws a frame, the orange double trail
    {0x89B662E266E5D73Eull, "ship engine glow"},      // VS 2D8263CC: n=36, additive, one vertex buffer each, seen with a ship's hull in view
    // VS 1C506222: n=384, additive, depth-tested volumes, about 46 a frame. The pale look at the lift bank, the near walls clear and the far ones washed out (Flight 33: leaving these out took
    // it away). Limited by the mark they still lit whatever was within kMirrorBeyondDefaultM of the camera, which is INFERRED to be why it stopped a few metres from the lifts.
    {0x2519C9050946D545ull, "light cones", false, true},
};
constexpr WithheldFamily kWithheldVs[] = {
    {0x72BDD292154158ADull, "body surface", true, false, true},
    {0x26FC402B1274EE7Bull, "planetary pipeline", true, false, true},
    {0x9FFA5D5E79F04873ull, "planetary 2", true, false, true},
    {0x3530A6FD15EDE145ull, "planetary 3", true, false, true},
    {0x71DD9863DCFC0986ull, "planet surface vs", true, false, true},
    {0xACE405F428C17EF6ull, "terrain patch", true, false, true},
    {0x19F70CE80DA3242Bull, "body sphere", true, false, true},
    {0xC7FA0C0F5DD49180ull, "beside the bodies", true, false, true},
    {0x8BD7C37ABCEE7E45ull, "sun", true, false, true},
    {0x1F3AD1584D7FA3C8ull, "sun arcs", true, false, true},
};
constexpr size_t kWithheldPsCount = sizeof(kWithheldPs) / sizeof(kWithheldPs[0]);
constexpr size_t kWithheldVsCount = sizeof(kWithheldVs) / sizeof(kWithheldVs[0]);

// A 64-bit bloom on (hash & 63): a draw whose bit is clear is in no list, with no scan.
constexpr uint64_t withheldBloom(const WithheldFamily* list, size_t n) noexcept {
    uint64_t b = 0;
    for (size_t i = 0; i < n; ++i) b |= uint64_t(1) << (list[i].hash & 63u);
    return b;
}
constexpr uint64_t kWithheldPsBloom = withheldBloom(kWithheldPs, kWithheldPsCount);
constexpr uint64_t kWithheldVsBloom = withheldBloom(kWithheldVs, kWithheldVsCount);

// The index of `hash` in the list, or -1. A zero hash (no shader) is never in a list.
inline int withheldIndex(const WithheldFamily* list, size_t n, uint64_t bloom, uint64_t hash) noexcept {
    if (hash == 0 || (bloom & (uint64_t(1) << (hash & 63u))) == 0) return -1;
    for (size_t i = 0; i < n; ++i)
        if (list[i].hash == hash) return static_cast<int>(i);
    return -1;
}
inline int withheldPsIndex(uint64_t hash) noexcept { return withheldIndex(kWithheldPs, kWithheldPsCount, kWithheldPsBloom, hash); }
inline int withheldVsIndex(uint64_t hash) noexcept { return withheldIndex(kWithheldVs, kWithheldVsCount, kWithheldVsBloom, hash); }

// The glow quads (VS 359BF8FF, n=6) of the light cones: part of the green haze past the glass in Flight 1's capture. Nothing yet says none of them is a room lamp's, so while the cull is on
// they are not withheld: they are limited by the mirror's stencil mark, so a pixel with nothing near behind it is dropped and one over the room stays. The cones themselves are withheld.
inline bool farLimitedPs(uint64_t ps) noexcept { return ps == 0x92FF8499ED345759ull; }

// True when a list holds the same hash twice (the tests fail on it).
constexpr bool withheldHasDuplicate(const WithheldFamily* list, size_t n) noexcept {
    for (size_t i = 0; i < n; ++i)
        for (size_t k = i + 1; k < n; ++k)
            if (list[i].hash == list[k].hash) return true;
    return false;
}

// An entry whose n is 0 matches any index count.
inline bool paneEntryMatches(const PaneTriple& e, uint64_t vs, uint64_t ps, uint32_t n) noexcept {
    return e.vs == vs && e.ps == ps && (e.n == 0 || e.n == n);
}

inline bool paneTripleMatch(const PaneTriple* list, size_t count, uint64_t vs, uint64_t ps, uint32_t n) noexcept {
    for (size_t i = 0; i < count; ++i)
        if (paneEntryMatches(list[i], vs, ps, n)) return true;
    return false;
}

// The index of the first matching triple in the list, or -1.
inline int paneTripleIndex(const PaneTriple* list, size_t count, uint64_t vs, uint64_t ps, uint32_t n) noexcept {
    for (size_t i = 0; i < count; ++i)
        if (paneEntryMatches(list[i], vs, ps, n)) return static_cast<int>(i);
    return -1;
}

// True when an entry with exactly this vs, ps and n is in the list (the parser's duplicate test: 0 and 36 are different entries).
inline bool paneTripleHas(const PaneTriple* list, size_t count, const PaneTriple& t) noexcept {
    for (size_t i = 0; i < count; ++i)
        if (list[i].vs == t.vs && list[i].ps == t.ps && list[i].n == t.n) return true;
    return false;
}

// 16 hex digits to a value; false for any other length or a non-hex digit.
inline bool parseHex16(const char* s, size_t len, uint64_t* out) noexcept {
    if (len != 16) return false;
    uint64_t v = 0;
    for (size_t i = 0; i < len; ++i) {
        const char c = s[i];
        uint32_t d = 0;
        if (c >= '0' && c <= '9') d = static_cast<uint32_t>(c - '0');
        else if (c >= 'a' && c <= 'f') d = static_cast<uint32_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = static_cast<uint32_t>(c - 'A' + 10);
        else return false;
        v = (v << 4) | d;
    }
    *out = v;
    return true;
}

// One entry "VS/PS" (any index count, n = 0) or "VS/PS/N": two 16-digit hex hashes and a decimal index count (1 to 4294967295).
inline bool parsePaneEntry(const char* s, size_t len, PaneTriple* out) noexcept {
    while (len > 0 && (s[0] == ' ' || s[0] == '\t')) {
        ++s;
        --len;
    }
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t')) --len;
    if (len < 16 + 1 + 16 || s[16] != '/') return false;
    PaneTriple t;
    if (!parseHex16(s, 16, &t.vs) || !parseHex16(s + 17, 16, &t.ps)) return false;
    if (len == 16 + 1 + 16) {
        t.n = 0;
        *out = t;
        return true;
    }
    if (len < 16 + 1 + 16 + 1 + 1 || s[33] != '/') return false;
    uint64_t n = 0;
    for (size_t i = 34; i < len; ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
        n = n * 10 + static_cast<uint64_t>(s[i] - '0');
        if (n > 0xFFFFFFFFull) return false;
    }
    if (n == 0) return false;
    t.n = static_cast<uint32_t>(n);
    *out = t;
    return true;
}

// The list "VS/PS/N,VS/PS/N": the good entries in order, up to cap, duplicates dropped. *bad counts the entries that did not parse (an empty entry between
// commas is not counted). Returns the number kept.
inline size_t parsePaneList(const char* text, PaneTriple* out, size_t cap, size_t* bad) noexcept {
    size_t n = 0;
    if (bad) *bad = 0;
    if (!text) return 0;
    const char* s = text;
    while (*s) {
        const char* e = s;
        while (*e && *e != ',') ++e;
        const char* a = s;
        const char* b = e;
        while (a < b && (*a == ' ' || *a == '\t')) ++a;
        while (b > a && (b[-1] == ' ' || b[-1] == '\t')) --b;
        if (b > a) {
            PaneTriple t;
            if (!parsePaneEntry(a, static_cast<size_t>(b - a), &t)) {
                if (bad) ++*bad;
            } else if (n < cap && !paneTripleHas(out, n, t)) {
                out[n++] = t;
            }
        }
        s = *e ? e + 1 : e;
    }
    return n;
}

// The pane colours, fixed per distinct index count in the order the counts are first seen, then repeating: red, green, blue, yellow, magenta, cyan, orange, white.
constexpr size_t kPaneColourCount = 8;
constexpr const char* kPaneColourNames[kPaneColourCount] = {"red", "green", "blue", "yellow", "magenta", "cyan", "orange", "white"};
constexpr float kPaneColours[kPaneColourCount][3] = {{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f, 0.0f},
                                                     {1.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 1.0f}, {1.0f, 0.5f, 0.0f}, {1.0f, 1.0f, 1.0f}};
constexpr float kPaneTintIntensity = 2.0f;

// ---- The mirror materials (the game's glass shader reads three texture arrays; these stand in for them) ------------------------------------------
// t8 (R8G8) x = metalness, y = the game's o0.w; t9 (R8G8) the normal's xy, decoded by the shader as xy * 2.007874 - 0.999985 (127 is flat); t10 (R8G8B8A8) rgb =
// base colour, a = roughness. Each texture is kPaneTexSize square and uniform: no seam, no noise, so the mirror is smooth.
constexpr uint32_t kPaneTexSize = 4;
constexpr uint8_t kPaneT10Rgb[3] = {184, 191, 199};   // 0.72, 0.75, 0.78 of 255
constexpr uint8_t kPaneT10Roughness = 77;             // 0.30 of 255

// out: kPaneTexSize * kPaneTexSize * 2 bytes. Metalness 255 and the second channel 255 everywhere.
inline void fillPaneT8(uint8_t* out) noexcept {
    for (uint32_t i = 0; i < kPaneTexSize * kPaneTexSize; ++i) {
        out[2 * i] = 255;
        out[2 * i + 1] = 255;
    }
}
// Flat 127, 127. out: kPaneTexSize * kPaneTexSize * 2 bytes.
inline void fillPaneT9(uint8_t* out) noexcept {
    for (uint32_t i = 0; i < kPaneTexSize * kPaneTexSize; ++i) {
        out[2 * i] = 127;
        out[2 * i + 1] = 127;
    }
}
// One colour, roughness 0.30. out: kPaneTexSize * kPaneTexSize * 4 bytes.
// The lit modes' second pass: a white, fully rough material. The glass shader has no diffuse term, only a specular one over the probes and lights, so at roughness 1 this
// reads as the room's own light, soft and at the scene's own scale, without a new shader or a constant in HDR units. out: kPaneTexSize * kPaneTexSize * 4 bytes.
inline void fillPaneGlowT10(uint8_t* out) noexcept {
    for (uint32_t i = 0; i < kPaneTexSize * kPaneTexSize * 4; ++i) out[i] = 255;
}
inline void fillPaneT10(uint8_t* out) noexcept {
    for (uint32_t i = 0; i < kPaneTexSize * kPaneTexSize; ++i) {
        for (int c = 0; c < 3; ++c) out[4 * i + c] = kPaneT10Rgb[c];
        out[4 * i + 3] = kPaneT10Roughness;
    }
}

// ---- The mirror's mark: where the scene behind a pane is farther than the beyond distance -----------------------------------------------------------
// Scene depth is infinite reversed Z (near plane 0.025 m): ndc z at distance d is 0.025 / d.
constexpr float kMirrorNearM = 0.025f;
constexpr float kMirrorBeyondDefaultM = 100.0f;
constexpr float kMirrorBeyondMinM = 20.0f;
constexpr float kMirrorBeyondMaxM = 5000.0f;
inline float markDepthForDistance(float metres) noexcept {
    if (!(metres >= 1.0f)) metres = 1.0f;
    return kMirrorNearM / metres;
}

// ---- Each pane's side, learned by occlusion queries ----------------------------------------------------------------------------------------------
// A pane is the game's glass draw, named by its index count, first index and base vertex. Interior glass has a wall behind it; exterior glass has nothing near
// behind it. One test reads two queries: the samples the game's glass passed (drawn) and the samples the marked mirror draw passed (mirrored).
struct PaneKey {
    uint32_t n = 0, startIndex = 0;
    int32_t baseVertex = 0;
};
enum class PaneSide : uint8_t { kUnknown, kInterior, kExterior };

constexpr uint64_t kPaneTestMinSamples = 64;
constexpr double kPaneExteriorRatio = 0.6;
constexpr uint32_t kPaneTestPeriod = 8;
constexpr size_t kPaneSideCap = 256;

inline uint32_t paneHash(uint32_t x, uint32_t y, uint32_t salt) noexcept {
    uint32_t h = x * 0x9E3779B1u ^ y * 0x85EBCA77u ^ salt * 0xC2B2AE3Du;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    h *= 0x297A2D39u;
    h ^= h >> 15;
    return h;
}
inline uint32_t paneKeyHash(const PaneKey& k) noexcept { return paneHash(k.n, k.startIndex, static_cast<uint32_t>(k.baseVertex) ^ 0x51ED270Bu); }

// Whether a key is tested this frame: one frame in eight per key, spread by key.
inline bool paneTestThisFrame(const PaneKey& k, uint32_t frame) noexcept { return (paneKeyHash(k) + frame) % kPaneTestPeriod == 0; }

class PaneSides {
public:
    PaneSide side(const PaneKey& k) const noexcept {
        const int i = find(k);
        return i < 0 ? PaneSide::kUnknown : sides_[i];
    }
    // The result of one note: whether the key's side is new or changed, for the log.
    enum class Change : uint8_t { kNone, kFirst, kChanged };
    Change note(const PaneKey& k, uint64_t drawn, uint64_t mirrored) noexcept {
        if (drawn < kPaneTestMinSamples) return Change::kNone;
        const PaneSide now = static_cast<double>(mirrored) >= kPaneExteriorRatio * static_cast<double>(drawn) ? PaneSide::kExterior : PaneSide::kInterior;
        int i = find(k);
        if (i >= 0) {
            // One mesh drawn at two places gives two readings of very different size: only a reading at least half the largest seen can change the side.
            if (drawn > maxDrawn_[i]) maxDrawn_[i] = drawn;
            if (drawn * 2 < maxDrawn_[i]) return Change::kNone;
        }
        if (i < 0) {
            if (count_ >= kPaneSideCap) {
                ++dropped_;
                return Change::kNone;
            }
            i = static_cast<int>(count_++);
            keys_[i] = k;
            sides_[i] = now;
            maxDrawn_[i] = drawn;
            if (now == PaneSide::kExterior) ++exterior_;
            return Change::kFirst;
        }
        if (sides_[i] == now) return Change::kNone;
        if (sides_[i] == PaneSide::kExterior) --exterior_;
        if (now == PaneSide::kExterior) ++exterior_;
        sides_[i] = now;
        return Change::kChanged;
    }
    void reset() noexcept {
        count_ = 0;
        exterior_ = 0;
        dropped_ = 0;
        for (auto& m : maxDrawn_) m = 0;
    }
    uint32_t exteriorCount() const noexcept { return exterior_; }
    uint32_t knownCount() const noexcept { return count_; }
    uint32_t dropped() const noexcept { return dropped_; }

private:
    int find(const PaneKey& k) const noexcept {
        for (uint32_t i = 0; i < count_; ++i)
            if (keys_[i].n == k.n && keys_[i].startIndex == k.startIndex && keys_[i].baseVertex == k.baseVertex) return static_cast<int>(i);
        return -1;
    }
    PaneKey keys_[kPaneSideCap];
    PaneSide sides_[kPaneSideCap] = {};
    uint64_t maxDrawn_[kPaneSideCap] = {};
    uint32_t count_ = 0, exterior_ = 0, dropped_ = 0;
};

// ---- A change of room: the camera cut --------------------------------------------------------------------------------------------------------------
// The lift is a one-frame camera jump of about 1090 m each way; a boarding or disembark shifts the origin by about 16.6 m. A step of the resolved camera over this is
// another room: the anchor is forgotten at once (Status.json's hangar flag lags the jump by up to a second, so it is not read for this).
constexpr float kRoomCutM = 100.0f;
inline bool roomCut(float stepM) noexcept { return std::isfinite(stepM) && stepM > kRoomCutM; }

// ---- The walk survey's zone (TEMPORARY, log only; goes when the arc closes) ------------------------------------------------------------------------------
enum class WalkZone : uint8_t { kSeated, kConcourse, kBay, kPastBox, kNoCell };
inline const char* walkZoneName(WalkZone z) noexcept {
    switch (z) {
        case WalkZone::kSeated: return "seated";
        case WalkZone::kConcourse: return "concourse";
        case WalkZone::kBay: return "bay";
        case WalkZone::kPastBox: return "past the box";
        default: return "no cell";
    }
}
// Seated first; the anchor fresh is the concourse; then no pick cell; then the camera in the cell's box (or sphere) or past it.
inline WalkZone walkZone(bool seated, bool anchorFresh, bool hasCell, bool camIn) noexcept {
    if (seated) return WalkZone::kSeated;
    if (anchorFresh) return WalkZone::kConcourse;
    if (!hasCell) return WalkZone::kNoCell;
    return camIn ? WalkZone::kBay : WalkZone::kPastBox;
}

// ---- The lift survey (TEMPORARY, log only; goes when the arc closes) --------------------------------------------------------------------------------------
// A record whose name contains "TurboLift", any case (the door and cabin records, and SA_TurboLiftBones).
inline bool liftPartName(const char* name) noexcept {
    if (!name) return false;
    constexpr char kKey[] = "turbolift";
    constexpr size_t kLen = sizeof(kKey) - 1;
    for (const char* s = name; *s; ++s) {
        size_t i = 0;
        while (i < kLen && s[i] != '\0' && (s[i] | 0x20) == kKey[i]) ++i;
        if (i == kLen) return true;
        if (s[i] == '\0') return false;
    }
    return false;
}
// A position that moved more than `limitM` from the last logged one (a non-finite current value does not count as moved; a non-finite last one does).
inline bool liftMoved(const float now[3], const float last[3], float limitM) noexcept {
    for (int k = 0; k < 3; ++k)
        if (!std::isfinite(now[k])) return false;
    for (int k = 0; k < 3; ++k)
        if (!std::isfinite(last[k])) return true;
    const float dx = now[0] - last[0], dy = now[1] - last[1], dz = now[2] - last[2];
    return dx * dx + dy * dy + dz * dz > limitM * limitM;
}

// The lift gate: the room is off while the player is inside a cabin whose doors are shut. The reading is from the survey's table (the cabin record named TurboLift,
// and the four door records nearest it): the doors' separation is about 1.5 m shut and 2.9 m open (the left and right pair) and about 1.4 and 2.65 (the second pair).
constexpr float kLiftInsideM = 1.3f;       // the camera to the cabin's centre: inside (0.81 to 0.98 shut inside, 2.32 just outside the door)
// The open-cabin keep, not the shut-cabin rule. True through the doorway: the sphere plus one step. A non-positive radius is not a cabin.
inline bool openCabinReach(float cabinDistM, float cabinRadiusM) noexcept {
    if (!std::isfinite(cabinDistM) || !std::isfinite(cabinRadiusM) || !(cabinRadiusM > 0.0f)) return false;
    return cabinDistM <= cabinRadiusM + kOpenCabinPastM;
}
constexpr float kLiftGap1ShutM = 1.55f;    // TurboLiftDoorLeft to TurboLiftDoorRight: over this the doors are open
constexpr float kLiftGap2ShutM = 1.42f;    // TurboLiftDoorLeft2 to TurboLiftDoorRight2
constexpr uint32_t kLiftShutReadings = 6;  // both pairs shut, after being seen open, this many ticks running
constexpr uint64_t kLiftMaxOnMs = 5000;    // the gate arms about 6 s before the camera cut; it lets go after 5 s, so the room's rigs run again about a second before the doors open (a gate stuck on stands down the same way)
struct LiftReading {
    bool cabin = false;        // a record named TurboLift is in view
    float cabinDistM = 0.0f;   // the camera to its centre
    float cabinRadiusM = 0.0f; // that record's radius; the open-cabin keep reads it
    bool doors = false;        // all four door records were read
    float gap1 = 0.0f, gap2 = 0.0f;
};
enum class LiftEnd : uint8_t { kNone, kDoorsMoved, kCut, kLeftCabin, kTimedOut, kGateOff };
inline const char* liftEndName(LiftEnd e) noexcept {
    switch (e) {
        case LiftEnd::kDoorsMoved: return "the doors moved";
        case LiftEnd::kCut: return "the camera cut";
        case LiftEnd::kLeftCabin: return "left the cabin";
        case LiftEnd::kTimedOut: return "released before arrival";
        case LiftEnd::kGateOff: return "the cull's conditions ended";
        default: return "none";
    }
}
class LiftGate {
public:
    // One tick. `cut`: the camera cut to another room this tick. Returns whether the room is off.
    bool step(const LiftReading& r, bool cut, uint64_t nowMs) noexcept {
        const bool was = on_;
        LiftEnd why = LiftEnd::kNone;
        if (cut) {
            seenOpen_ = false;
            run_ = 0;
            on_ = false;
            why = LiftEnd::kCut;
        } else if (!r.cabin || !(r.cabinDistM <= kLiftInsideM)) {
            seenOpen_ = false;
            run_ = 0;
            on_ = false;
            why = LiftEnd::kLeftCabin;
        } else if (!r.doors) {
            // the doors are not known this tick: hold
        } else if (!(r.gap1 <= kLiftGap1ShutM) || !(r.gap2 <= kLiftGap2ShutM)) {
            seenOpen_ = true;
            run_ = 0;
            on_ = false;
            why = LiftEnd::kDoorsMoved;
        } else if (seenOpen_ && !on_ && ++run_ >= kLiftShutReadings) {
            on_ = true;
            onSinceMs_ = nowMs;
        }
        if (on_ && nowMs - onSinceMs_ > kLiftMaxOnMs) {
            on_ = false;
            seenOpen_ = false;
            run_ = 0;
            why = LiftEnd::kTimedOut;
        }
        if (was && !on_) end_ = why;
        return on_;
    }
    // The cull's own conditions are not met this tick (the key is off, not on foot, the hangar sealed): off, and nothing is remembered of the doors.
    void stop() noexcept {
        if (on_) end_ = LiftEnd::kGateOff;
        on_ = false;
        seenOpen_ = false;
        run_ = 0;
    }
    bool on() const noexcept { return on_; }
    LiftEnd lastEnd() const noexcept { return end_; }   // why the gate last went from on to off
    uint64_t onSinceMs() const noexcept { return onSinceMs_; }
    void reset() noexcept { *this = LiftGate(); }

private:
    bool on_ = false, seenOpen_ = false;
    uint32_t run_ = 0;
    uint64_t onSinceMs_ = 0;
    LiftEnd end_ = LiftEnd::kNone;
};

// ---- The camera test (TEMPORARY, docs\concourse-cull-2026-11-05.md): the look test: what would a bake see from the room's centre, with the room off and the rest on? ---------
// The flow it has to serve (Sean, 2026-11-07): dock, disembark, ride up, and the stand-in is ready when the doors open. So the look runs on the ride up, from the tick the room's anchor is
// first seen, which logs put 156 to 188 ms before the cut. The window has its own timer and the gate is not stepped meanwhile (a moved camera would drop it at once: it reads the camera's
// distance to the cabin). The decision is here so a table can pin it.
constexpr uint64_t kCamTestDirMs = 800;    // each of the six directions
constexpr uint32_t kCamTestDirs = 6;       // the heading, a quarter turn three times, straight up, straight down
constexpr uint64_t kCamTestMs = kCamTestDirMs * kCamTestDirs;   // long enough to see; a bake would need a few frames of it
constexpr uint64_t kCamTestArmMs = 15000;  // armed this long with no sighting, and it lets go: the doors shut about 6 s before the cut
constexpr uint64_t kCamTestLogMs = 250;    // the acting lines' spacing
constexpr uint32_t kCamTestModeOff = 0, kCamTestModeLog = 1, kCamTestModeLook = 2, kCamTestModeBake = 3, kCamTestModes = 4;
inline uint32_t camTestNextMode(uint32_t m) noexcept { return (m + 1) % kCamTestModes; }
inline const char* camTestModeName(uint32_t m) noexcept {
    switch (m) {
        case kCamTestModeLog: return "log only";
        case kCamTestModeLook: return "look from the room's centre";
        case kCamTestModeBake: return "bake on arrival";
        default: return "off";
    }
}
// TEMPORARY (hotkey.concourse_light_test, docs\concourse-cull-2026-11-05.md "Flight 32 results"): what lays light on the room, by pixel shader. Each press leaves one out while the cull is
// on. Flight 32 left out the sun's quad and each light volume in turn and none was it; these are the rest of a census of the room from the sun on, what is drawn before the glass first. The
// last step, kLightTestAllLights, leaves out all the lights together, to say whether the look is in them at all. Step 0 leaves nothing out.
constexpr uint64_t kLightTestAllLights = ~0ull;
constexpr uint64_t kLightTestLights[] = {0x7CECABDE34FFBE9Eull, 0xC6E6E419DA9F6FADull, 0x50364C9D994141D5ull, 0x3D8442D2FC1DCADDull, 0xAC3CE66FBAF0FA9Aull, 0x81812EF97FB4A361ull,
                                         0x70E6FCA6CF692D2Aull, 0x2DB678B6B558B604ull, 0xB98B140FC83B4448ull, 0x1F64463B15189104ull, 0x37649F54AD53319Cull};
constexpr uint64_t kLightTestShaders[] = {0x2519C9050946D545ull, 0x92FF8499ED345759ull, 0x095030F27D2C362Aull, 0xC5A5C7E8216CB9AFull, 0x8DEF46452FA459F5ull, 0x98337732C955AD01ull,
                                          0x098C0764D28FC42Cull, 0x84965D3C050FB01Bull, 0x06332CA168B6DA63ull, 0x87EF79B19297B8C4ull, 0xFCFAD73924BF45B9ull, 0x057F8E2778A5AAEDull,
                                          0x7AA0441EE0E88667ull, 0x1AE6AB9A94C0456Eull, 0x66B08F89E4926C01ull, 0xAFED1D4B087E18A9ull, 0xB2DE0A41A4C2B4F5ull, 0x067CBE05E7EF7F32ull,
                                          0xAD050E528C0E8B17ull, 0x0A80DFD89B15A05Bull, 0xCC4E0188DB8E7300ull, 0xCCDB2A91490F8755ull, 0x68ABCB9FEF6CA66Cull, 0x2C3CB8F06C1CA6BCull,
                                          0xDB899F4BD577F2E5ull, 0x0146ABCC53240479ull, 0x25D5F73A4616093Bull, kLightTestAllLights};
constexpr uint32_t kLightTestSteps = static_cast<uint32_t>(sizeof(kLightTestShaders) / sizeof(kLightTestShaders[0])) + 1;
inline uint32_t lightTestNext(uint32_t step) noexcept { return (step + 1) % kLightTestSteps; }
inline uint64_t lightTestShaderAt(uint32_t step) noexcept { return step == 0 || step >= kLightTestSteps ? 0ull : kLightTestShaders[step - 1]; }
inline bool lightTestLeavesOut(uint64_t test, uint64_t ps) noexcept {
    if (test != kLightTestAllLights) return test != 0 && test == ps;
    for (uint64_t light : kLightTestLights)
        if (light == ps) return true;
    return false;
}

// The bake's plan (TEMPORARY): one tick a frame, each aiming at the next direction not yet captured, going round until every one has reached the resolve. A write takes about 4 frames to
// reach it (Flight 25: the first capture 51 ms after the first write), by which time the room's rigs have gone, so no frame waits for them. It is the captures that end the bake, not the
// count of frames: Flight 28's first two frames with the near plane raised took the flat copy route and never reached the resolve, and an open loop left those two directions out.
// The limit is bakeTicks; past it the bake is given up, since a window with a whole direction missing is worse than the game's own.
// The directions are the room's own axes (the anchor's: 0 is axes[2], 2 is axes[0], 4 is axes[1], up), so the same ones serve every visit to this room: 0 +a2, 1 -a2, 2 +a0, 3 -a0, 4 up.
// Straight down is not one of them: Flight 24's look that way was stars through the floor, and no window faces it.
constexpr uint32_t kBakeDirs = 5, kBakeTailTicks = 8, kBakeDirsAll = 0x1Fu;
constexpr float kBakeFovRad = 1.5707963f;      // camera+0x260, the vertical field of view in radians (READ, 0x4F30F0): 90 degrees, so about 121 across at 16:9, of which the capture keeps the middle 90
// The bake frames' near plane (diag.concourse_bake_near) and the room's cut are two values (2026-11-08). The near plane was 50 m and was both: it kept the room out of the frames, and it
// clipped what Elite draws close to the camera on purpose. H57, confirmed: a 50 m bake at a CraterPort had a navy sky, no stars, unlit walls and no ground, and a 5 m one had all of them
// (Flight 48). So the frames are drawn at 5 m, and the room is kept out by identity (the look's judgement, and bakePeopleVs for the people the store never judges); what is left of it
// within kBakeRoomCutM of the centre is not written (the bake's compute shader), and that texel keeps whatever another frame gave it.
constexpr float kBakeNearDefaultM = 5.0f, kBakeNearMinM = 5.0f, kBakeNearMaxM = 400.0f;
constexpr float kBakeRoomCutM = 50.0f;
// The people: an NPC, the commander's own arms and suit. Their draws do not pass the store (Flight 23: "NPCs were left" by the look), so in the bake's frames they are left out by vertex
// shader, the skinned families engine_velocity_families.h names (the NPC's rigid body 8B58 and its skin 7B0D, 114A, D99A, 61AE, the hair A6A3, the suit 01DA) and the hair's depth-only
// prepass, 3186 with 4EBA (flat_projection_recipes.h). Not drawn, what is behind them is captured. A family not named here is cut at kBakeRoomCutM, as a hole.
constexpr uint64_t kBakeHairPrepassVs = 0x318693134643131Aull;
inline bool bakePeopleVs(uint64_t vs) noexcept {
    if (vs == 0) return false;
    if (vs == kBakeHairPrepassVs) return true;
    for (const engine_velocity_family::Family& f : engine_velocity_family::kFamilies)
        if (f.skinned && f.vs == vs) return true;
    return false;
}
// What the bake does at each type of station, from the journal (journal_station.h). A row is added when its layout has been flown: the keep list's names and the anchor were read at one
// Orbis, and another room could lose its own objects or have no anchor. Everything else, and a type not known, stays the game's own; diag.concourse_cull = on, or the hotkey, forces it
// for a test. Once a type's windows are known, its row can name only the directions they need.
// CraterOutpost: flown at one, by Sean's word after Flights 36 and 37. Its room is told by its own anchor (kPortAnchorName), so a type that names another room still gets no cull.
// Outpost: flown at one with F2, "Outpost looks good, bake it" (Flight 39). The same: its own anchor, kOutpostAnchorName.
// FleetCarrier: flown at two with F2 (Flights 39, 41, 42), "Carrier looks good, bake it". Its own anchor, kCarrierAnchorName, and its bridge is the game's own (kRoomCarrier's live
// room). A load on foot there names no carrier, and Status.json says it (journal_station::atCarrier). A jump makes a new bake (kBakeAfterJumpMs).
// CraterPort: surveyed (the CraterOutpost's room: the same anchor, kPortAnchorName, and bay, n=12) and flown at one with F2 (Flight 48); Sean: "We can add this station type to the
// bake list as well". The bake's missing sun, sky and ground there (H57) is the bake's, at every type, not this row's.
// AsteroidBase: the large stations' room (Standard_SocialArea, the bay n=36), flown with F2 on 35aa1d87 (171 ms, 832 people's draws left out); Sean: "it has the same layout as the other
// orbital large stations (orbis). Tested with F2, let's add it to the same list of automatic ones".
// Dodec: the same, flown with F2 on 35aa1d87 (172 ms, 793 people's draws left out; no new room line after the asteroid base, so the same layout); Sean: "Similar to the asteroid
// base, we need to add dodec to the automatic list".
struct BakePlan {
    bool run = false;
    uint32_t dirs = kBakeDirsAll;
};
inline BakePlan bakePlanFor(journal_station::Kind k) noexcept {
    BakePlan p;
    p.run = k == journal_station::Kind::kOrbis || k == journal_station::Kind::kCoriolis || k == journal_station::Kind::kOcellus || k == journal_station::Kind::kCraterOutpost ||
            k == journal_station::Kind::kOutpost || k == journal_station::Kind::kFleetCarrier || k == journal_station::Kind::kCraterPort || k == journal_station::Kind::kAsteroidBase ||
            k == journal_station::Kind::kDodec;
    return p;
}
// A type whose cull needs no bake. A carrier's room is RoomLayout::realSky (its windows are the game's own, no bake), the only such type; VR has no bake yet, so in VR
// (when the cull runs by auto) only this type runs.
inline bool runsWithoutBake(journal_station::Kind k) noexcept { return k == journal_station::Kind::kFleetCarrier; }
// A carrier that jumps with the commander aboard: the bake is of the system left, so it goes, and the next one waits this long from the CarrierJump. The journal writes that event about
// a minute after the departure (2025-08-13's: departure 18:12:10, CarrierJump 18:13:10), as the carrier arrives; the wait is for the arrival's own light to pass. Not flown.
constexpr uint64_t kBakeAfterJumpMs = 10000;
inline uint32_t bakeDirCount(uint32_t mask) noexcept {
    uint32_t n = 0;
    for (uint32_t d = 0; d < kBakeDirs; ++d)
        if (mask & (1u << d)) ++n;
    return n;
}
inline uint32_t bakeTicks(uint32_t mask) noexcept { return 3 * bakeDirCount(mask) + kBakeTailTicks; }
// Which direction a tick asks for: the next one after `after`, in order, that is wanted and not yet captured. With none left it stays, for the frames still on their way.
inline uint32_t bakeNextDir(uint32_t mask, uint32_t captured, uint32_t after) noexcept {
    const uint32_t left = mask & ~captured;
    if (left == 0) return after;
    for (uint32_t k = 1; k <= kBakeDirs; ++k) {
        const uint32_t d = (after + k) % kBakeDirs;
        if (left & (1u << d)) return d;
    }
    return after;
}
// diag.concourse_bake_dirs: five characters, one for each direction above, 1 to take it. Anything else, or none taken, is all five.
inline uint32_t bakeParseDirs(const char* text) noexcept {
    if (!text || std::strlen(text) != kBakeDirs) return kBakeDirsAll;
    uint32_t mask = 0;
    for (uint32_t d = 0; d < kBakeDirs; ++d) {
        if (text[d] == '1') mask |= 1u << d;
        else if (text[d] != '0') return kBakeDirsAll;
    }
    return mask != 0 ? mask : kBakeDirsAll;
}
// The inverse of a 4x4 (row-major). False when it is singular or not finite.
inline bool invert4(const double m[16], double out[16]) noexcept {
    double a[4][8] = {};
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) a[r][c] = m[r * 4 + c];
        a[r][4 + r] = 1.0;
    }
    for (int col = 0; col < 4; ++col) {
        int piv = col;
        for (int r = col + 1; r < 4; ++r)
            if (std::fabs(a[r][col]) > std::fabs(a[piv][col])) piv = r;
        const double p = a[piv][col];
        if (!(std::fabs(p) > 1e-30) || !std::isfinite(p)) return false;
        if (piv != col)
            for (int c = 0; c < 8; ++c) {
                const double t = a[col][c];
                a[col][c] = a[piv][c];
                a[piv][c] = t;
            }
        for (int c = 0; c < 8; ++c) a[col][c] /= p;
        for (int r = 0; r < 4; ++r) {
            if (r == col) continue;
            const double f = a[r][col];
            for (int c = 0; c < 8; ++c) a[r][c] -= f * a[col][c];
        }
    }
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            out[r * 4 + c] = a[r][4 + c];
            if (!std::isfinite(out[r * 4 + c])) return false;
        }
    return true;
}
inline uint32_t camTestLookDir(uint64_t elapsedMs) noexcept {
    const uint64_t d = elapsedMs / kCamTestDirMs;
    return d >= kCamTestDirs ? kCamTestDirs - 1 : static_cast<uint32_t>(d);
}
enum class CamTestEnd : uint8_t { kNone, kTime, kModeOff, kDone };
inline const char* camTestEndName(CamTestEnd e) noexcept {
    switch (e) {
        case CamTestEnd::kTime: return "time ran out";
        case CamTestEnd::kDone: return "the bake took its frames";
        case CamTestEnd::kModeOff: return "the mode or the cull's conditions ended";
        default: return "none";
    }
}
constexpr uint64_t kBakeNoRideArmMs = 150;   // armed this long before a bake with no ride fires: the hold keeps a frame at each Present once it is armed
class CamTestTimer {
public:
    // One tick. `gateOn`: the lift gate's own answer this tick (read only while the window is not open). `anchorFresh`: the store has just seen the room's anchor. Returns whether
    // the window is open after this tick; `*ended` is set only on the tick it closes. A ride up arms it: the gate on with no room seen. The anchor's first sighting then fires it, and
    // it ends by time or the mode alone: the doors and the cut go on behind it. A ride down never arms it, since the room is already seen when its gate comes on.
    bool step(uint32_t mode, bool gateOn, bool anchorFresh, uint64_t nowMs, CamTestEnd* ended) noexcept {
        if (ended) *ended = CamTestEnd::kNone;
        const bool acts = mode == kCamTestModeLook || mode == kCamTestModeBake;
        if (active_) {
            CamTestEnd why = CamTestEnd::kNone;
            if (!acts) why = CamTestEnd::kModeOff;
            else if (nowMs - startMs_ >= kCamTestMs) why = CamTestEnd::kTime;
            if (why != CamTestEnd::kNone) {
                active_ = false;
                if (ended) *ended = why;
            }
            return active_;
        }
        if (!acts) {
            armed_ = false;
        } else if (armed_) {
            if (anchorFresh && nowMs - armedMs_ >= waitMs_) {
                armed_ = false;
                active_ = true;
                startMs_ = nowMs;
            } else if (nowMs - armedMs_ > kCamTestArmMs) {
                armed_ = false;
            }
        } else if (gateOn && !anchorFresh) {
            armed_ = true;
            armedMs_ = nowMs;
            waitMs_ = 0;
        }
        return active_;
    }
    // With no ride to arm it: the room is already in view, so it fires once the hold has had time to keep a frame.
    void armNow(uint64_t nowMs) noexcept {
        if (active_ || armed_) return;
        armed_ = true;
        armedMs_ = nowMs;
        waitMs_ = kBakeNoRideArmMs;
    }
    // The cull's own conditions are gone (key off, not on foot, hangar sealed): the window closes, nothing is remembered.
    bool stop() noexcept {
        const bool was = active_;
        active_ = false;
        armed_ = false;
        return was;
    }
    bool active() const noexcept { return active_; }
    bool armed() const noexcept { return armed_; }
    uint64_t startMs() const noexcept { return startMs_; }
    uint64_t armedMs() const noexcept { return armedMs_; }

private:
    bool active_ = false, armed_ = false;
    uint64_t startMs_ = 0, armedMs_ = 0, waitMs_ = 0;
};

// ---- The backdrop cube (PROTOTYPE on the concourse arc, docs\concourse-cull-2026-11-05.md "The window plan") -----------------------------------------------
// One capture, on a key press, of the view through the exterior panes, kept in a cube of view directions (6 slices, D3D TextureCube addressing, faces +X -X +Y -Y +Z -Z).
// The capture shader writes the texel of normalize(v2); the display shader samples the cube with the same direction. The helper below is the C++ twin of the HLSL text
// beside it (the tests pin the C++; the two are written to be read side by side).
constexpr uint32_t kBackdropCubeSize = 2048;     // about 23 texels a degree at a face's centre (a bake's frame at 90 degrees is 12 pixels a degree at 1080p, 24 at 4K); 2 textures of 192 MB, the distance pair a quarter of that. Still far more than the windows use: most of a cube is never looked at
// What starts a bake with no ride. A load straight into the room has no lift to arm one, so the room's being in view for a moment with no bake starts it: until then the cull waits,
// and the room and its windows are the game's own. A render size that has risen since the bake starts another, once it has held steady: the picture is as sharp as the frames it was
// captured from, up to the cube's own size. Lowering it does not: a sharper picture does no harm. A bake that fails is tried kBakeNoRideTries times, and the game's own picture stays.
constexpr uint64_t kBakeRoomSettleMs = 2000;
constexpr uint64_t kBakeSharperSettleMs = 1000;
constexpr uint32_t kBakeNoRideTries = 2;
enum class BakeNoRide : uint8_t { kNone, kFirst, kSharper };
inline bool bakeWouldBeSharper(uint32_t frameHeight, uint32_t bakedHeight) noexcept {
    const uint32_t now = frameHeight < kBackdropCubeSize ? frameHeight : kBackdropCubeSize;
    const uint32_t was = bakedHeight < kBackdropCubeSize ? bakedHeight : kBackdropCubeSize;
    return now > was;
}
inline BakeNoRide bakeNoRide(bool baked, uint64_t roomMs, bool sharper, uint32_t tries) noexcept {
    if (tries >= kBakeNoRideTries) return BakeNoRide::kNone;
    if (!baked) return roomMs >= kBakeRoomSettleMs ? BakeNoRide::kFirst : BakeNoRide::kNone;
    return sharper ? BakeNoRide::kSharper : BakeNoRide::kNone;
}
// fix.station_window_distance: how far from a window its view turns from the bake to the real station, in percent of the shipped distance. The trigger is how much of the view the
// window fills, which falls with the square of the distance, so twice the distance is a quarter of the share. 0 never clears.
constexpr int kWindowDistanceDefault = 100, kWindowDistanceMin = 0, kWindowDistanceMax = 250;
inline float windowClearPercent(float shippedPercent, int distancePercent) noexcept {
    if (distancePercent <= 0) return 0.0f;
    const float k = static_cast<float>(distancePercent) / 100.0f;
    const float p = shippedPercent / (k * k);
    return p < 100.0f ? p : 100.0f;
}
constexpr uint64_t kBackdropLiftMs = 200;       // the cull is lifted this long before the capture frame is armed
constexpr uint32_t kBackdropWaitFrames = 3;     // armed frames with no pane captured before the capture is given up

struct CubeTexel {
    uint32_t face = 0;   // 0 +X, 1 -X, 2 +Y, 3 -Y, 4 +Z, 5 -Z
    float u = 0.0f, v = 0.0f;   // 0 to 1 across the face
};
// False for a direction that is zero or not finite. The major axis is the largest absolute value (a tie goes to x, then y); ma is its magnitude.
inline bool cubeFaceOf(float x, float y, float z, CubeTexel* out) noexcept {
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return false;
    const float ax = std::fabs(x), ay = std::fabs(y), az = std::fabs(z);
    float ma = 0.0f, sc = 0.0f, tc = 0.0f;
    uint32_t face = 0;
    if (ax >= ay && ax >= az) {
        ma = ax;
        face = x > 0.0f ? 0u : 1u;
        sc = x > 0.0f ? -z : z;
        tc = -y;
    } else if (ay >= az) {
        ma = ay;
        face = y > 0.0f ? 2u : 3u;
        sc = x;
        tc = y > 0.0f ? z : -z;
    } else {
        ma = az;
        face = z > 0.0f ? 4u : 5u;
        sc = z > 0.0f ? x : -x;
        tc = -y;
    }
    if (!(ma > 0.0f)) return false;
    if (out) {
        out->face = face;
        out->u = 0.5f * (sc / ma + 1.0f);
        out->v = 0.5f * (tc / ma + 1.0f);
    }
    return true;
}
// The texel of a face of `size`: u = 1.0 clamps to the last one.
inline void cubeTexelOf(const CubeTexel& c, uint32_t size, uint32_t* tx, uint32_t* ty) noexcept {
    const uint32_t last = size > 0 ? size - 1 : 0;
    const float fx = c.u * static_cast<float>(size), fy = c.v * static_cast<float>(size);
    uint32_t x = fx > 0.0f ? static_cast<uint32_t>(fx) : 0u;
    uint32_t y = fy > 0.0f ? static_cast<uint32_t>(fy) : 0u;
    if (x > last) x = last;
    if (y > last) y = last;
    if (tx) *tx = x;
    if (ty) *ty = y;
}
// The HLSL twin: the face and texel of a direction. Included by the capture shader.
inline const char* cubeTexelHlsl() noexcept {
    return "uint3 cubeTexel(float3 d, uint size) {\n"
           "    float3 a = abs(d);\n"
           "    float ma, sc, tc; uint face;\n"
           "    if (a.x >= a.y && a.x >= a.z) { ma = a.x; face = d.x > 0 ? 0u : 1u; sc = d.x > 0 ? -d.z : d.z; tc = -d.y; }\n"
           "    else if (a.y >= a.z) { ma = a.y; face = d.y > 0 ? 2u : 3u; sc = d.x; tc = d.y > 0 ? d.z : -d.z; }\n"
           "    else { ma = a.z; face = d.z > 0 ? 4u : 5u; sc = d.z > 0 ? d.x : -d.x; tc = -d.y; }\n"
           "    float2 uv = 0.5 * (float2(sc, tc) / max(ma, 1e-20) + 1.0);\n"
           "    uint2 xy = min((uint2)(uv * (float)size), uint2(size - 1, size - 1));\n"
           "    return uint3(xy, face);\n"
           "}\n";
}
// The reverse (TEMPORARY, the bake): the direction of a place on a face, with u and v from 0 to 1, not normalised. cubeFaceOf of it gives the same face, u and v back. And its HLSL twin.
inline void cubeDirOf(uint32_t face, float u, float v, float out[3]) noexcept {
    const float sc = 2.0f * u - 1.0f, tc = 2.0f * v - 1.0f;
    switch (face) {
        case 0: out[0] = 1.0f; out[1] = -tc; out[2] = -sc; break;
        case 1: out[0] = -1.0f; out[1] = -tc; out[2] = sc; break;
        case 2: out[0] = sc; out[1] = 1.0f; out[2] = tc; break;
        case 3: out[0] = sc; out[1] = -1.0f; out[2] = -tc; break;
        case 4: out[0] = sc; out[1] = -tc; out[2] = 1.0f; break;
        default: out[0] = -sc; out[1] = -tc; out[2] = -1.0f; break;
    }
}
inline const char* cubeDirHlsl() noexcept {
    return "float3 cubeDir(uint face, float2 uv) {\n"
           "    float sc = 2.0 * uv.x - 1.0, tc = 2.0 * uv.y - 1.0;\n"
           "    if (face == 0) return float3(1.0, -tc, -sc);\n"
           "    if (face == 1) return float3(-1.0, -tc, sc);\n"
           "    if (face == 2) return float3(sc, 1.0, tc);\n"
           "    if (face == 3) return float3(sc, -1.0, -tc);\n"
           "    if (face == 4) return float3(sc, -tc, 1.0);\n"
           "    return float3(-sc, -tc, -1.0);\n"
           "}\n";
}
// ---- The backdrop's distance (PROTOTYPE, docs\concourse-cull-2026-11-05.md): where the captured view is taken to be --------------------------------------
// The cube is keyed by direction from a fixed origin O (the camera at the first capture of a visit). A pane pixel looks up a direction from the camera C; off = C - O. None looks up
// the pixel's own direction (everything infinitely far), Sphere puts the view on a sphere of radius R around O, Depth marches the captured distances (concourse_pane.cpp).
constexpr uint32_t kParallaxNone = 0, kParallaxSphere = 1, kParallaxDepth = 2;
inline uint32_t parallaxNext(uint32_t m) noexcept { return m == kParallaxNone ? kParallaxSphere : m == kParallaxSphere ? kParallaxDepth : kParallaxNone; }
inline const char* parallaxName(uint32_t m) noexcept { return m == kParallaxSphere ? "sphere" : m == kParallaxDepth ? "depth" : "off"; }
constexpr float kBackdropRadiusDefaultM = 600.0f, kBackdropRadiusMinM = 10.0f, kBackdropRadiusMaxM = 60000.0f;   // diag.concourse_backdrop_radius
// What differs between the rooms, named by the anchor. At a surface port the bay is one window of n=12 (85.5% of the view standing at it, Flight 35), and what stands outside begins nearer than in a
// station: buildings from about 110 m from the hub, the docks past 1100 m, so the sphere the bake is shown on sits nearer. 300 m is a first value, between those.
// roomReachM: a station's room leaves the store when the lift cuts away, so its anchor being seen says the commander is there; 0 says so. A port's hub stays in the store from the hangar, 503 m off,
// and the cull came on down there and took the hangar's walls (Flight 36). So there the camera must be near it too: 23 m at the bay, the hub's own radius 42 m, the hangar 503 m.
// liveRoomName: a room inside the anchor's reach that the bake, taken at the anchor's centre, cannot show: while the camera is inside that record's sphere the cull stands down there and
// its windows are the game's own. Null: none.
// slabCut: the room's doors as planes across its a2, in metres from the anchor along a2 (the door rule, roomSeenSlab). Zero cuts: none known.
// realSky: the room's windows look out on open space, so they show the real sky and no bake is taken: the glass is the game's own, the stars, the planet, its atmosphere and the sun are
// drawn with the cull on (WithheldFamily::skySide), and everything else the cull takes outside (the hull, ships, weapons fire) is culled until a window clears on approach
// (fix.station_window_distance), which brings it all back as at any other room.
struct RoomLayout {
    uint32_t bayN;
    float backdropRadiusM;
    const char* name;
    float roomReachM;
    const char* liveRoomName = nullptr;
    uint32_t slabCuts = 0;
    float slabCut[2] = {};
    bool realSky = false;
};
inline constexpr RoomLayout kRoomStation{kBayIndexCount, kBackdropRadiusDefaultM, "a large station's", 0.0f};
inline constexpr RoomLayout kRoomPort{12u, 300.0f, "a surface port's", 100.0f};
// An orbital outpost: the bay is n=30 (79.7% of the view standing at it), 15.6 m from the hub, the hub's radius 33.3 m, the hangar lobby 431 m off. Outside it, the outpost's own hull
// (one record, r=570, centred 165 m off), then the medium hangar 223 m, pads 330 m, blast shields 380 m: the same first sphere as a port.
inline constexpr RoomLayout kRoomOutpost{30u, 300.0f, "an orbital outpost's", 100.0f};
// A fleet carrier: the bar's windows are n=30 (99.7% of the view at them), the bridge's n=54 (99.9%), 39 m and 54 m from the anchor. Outside, the carrier's own hull, `MS_` records culled by the
// list: the core 116 m past the bridge window, the command tower around the rooms, the engines 255 m off. The bridge is inside the 100 m reach; the lift to the hangar cuts 1331 m.
// The bridge is the live room. Its windows lie 50 m from the bake's centre along the room's +a2, with the command tower (MS_Command_Tower4, kept in the bake, its sphere around the centre) in
// between, and in them the bake showed a flat grey surface (Flight 41, Sean: "some strange artifacting in the window bake for the command deck").
// Its doors (Flight 43's survey): the elevator bay's sealed door at a2 = -11 and the bridge's at +19, both a Narrow_Door_Left and _Right pair. They split it into the elevator bay,
// the central space with its services, and the bridge and command area.
// The real sky (2026-11-08): Sean, after Flight 46, saw the bake's square edges in the bar's windows: "maybe the best thing to do here is not bake an image at all, but just restore the
// culled ships and weapons fire outside once a player triggers the approach to the window", then "Can we exclude the skybox/planets/sunlight from being culled here?". Flight 45 had
// the bake flat navy toward the bar's -a0 (H54) where Elite's own view is black sky, stars and the planet.
inline constexpr RoomLayout kRoomCarrier{30u, 300.0f, "a fleet carrier's", 100.0f, "SA_Carrier_01_Bridge", 2u, {-11.0f, 19.0f}, true};
inline const RoomLayout& roomLayoutOf(const char* anchorName) noexcept {
    if (!anchorName) return kRoomStation;
    if (std::strcmp(anchorName, kPortAnchorName) == 0) return kRoomPort;
    if (std::strcmp(anchorName, kOutpostAnchorName) == 0) return kRoomOutpost;
    if (std::strcmp(anchorName, kCarrierAnchorName) == 0) return kRoomCarrier;
    return kRoomStation;
}
// Whether a fresh sighting of the anchor means the commander is in that room. With no reading of where either is, a room with a reach says no: the game's own.
inline bool anchorMeansRoomHere(const RoomLayout& room, bool cameraKnown, bool anchorRead, float cameraDistM) noexcept {
    if (room.roomReachM <= 0.0f) return true;
    return cameraKnown && anchorRead && std::isfinite(cameraDistM) && cameraDistM <= room.roomReachM;
}
// Whether the camera is in the room's live room: inside that record's own sphere. Not known is not inside: the cull goes on as in the rest of the room.
inline bool insideLiveRoom(const RoomLayout& room, bool cameraKnown, bool liveRead, float cameraDistM, float liveRadiusM) noexcept {
    return room.liveRoomName != nullptr && cameraKnown && liveRead && std::isfinite(cameraDistM) && std::isfinite(liveRadiusM) && liveRadiusM > 0.0f && cameraDistM <= liveRadiusM;
}
// The door rule (docs\concourse-cull-2026-11-05.md "Rooms behind bulkhead doors"). The room's doors are planes across its a2 (RoomLayout::slabCut). The stretch of a2 the camera can see
// into runs to the first door each way that blocks: one known shut, with the camera more than kDoorNearM from its plane, as Elite's doors open on approach and the room behind must be back
// before they do. A record whose sphere lies wholly past either end is behind a shut door. Flight 44's ceiling, every door taken as shut, culled to the 144 fps cap everywhere: at least
// 1.6 ms in the elevator bay, 0.7 in the central space, 0.3 on the bridge.
constexpr float kRoomSlabOpen = 1.0e30f;
struct RoomSlab {
    float lo = -kRoomSlabOpen;
    float hi = kRoomSlabOpen;
};
inline bool roomSlabOf(const RoomLayout& room, float camAlong, RoomSlab* out) noexcept {
    if (!out || room.slabCuts == 0 || room.slabCuts > 2 || !std::isfinite(camAlong)) return false;
    RoomSlab s;
    for (uint32_t i = 0; i < room.slabCuts; ++i) {
        const float c = room.slabCut[i];
        if (c <= camAlong) {
            if (c > s.lo) s.lo = c;
        } else if (c < s.hi) {
            s.hi = c;
        }
    }
    *out = s;
    return true;
}
inline bool roomSlabOutside(const RoomSlab& s, float along, float radius) noexcept {
    if (!std::isfinite(along) || !std::isfinite(radius) || radius < 0.0f) return false;
    return along + radius < s.lo || along - radius > s.hi;
}
inline bool sameRoomSlab(const RoomSlab& a, const RoomSlab& b) noexcept { return a.lo == b.lo && a.hi == b.hi; }
// The stretch the camera can see into: only the cuts whose door blocks bound it. None blocking is the whole of a2.
inline bool roomSeenSlab(const RoomLayout& room, float camAlong, const bool blocks[2], RoomSlab* out) noexcept {
    if (!out || !blocks || room.slabCuts == 0 || room.slabCuts > 2 || !std::isfinite(camAlong)) return false;
    RoomSlab s;
    for (uint32_t i = 0; i < room.slabCuts; ++i) {
        if (!blocks[i]) continue;
        const float c = room.slabCut[i];
        if (c <= camAlong) {
            if (c > s.lo) s.lo = c;
        } else if (c < s.hi) {
            s.hi = c;
        }
    }
    *out = s;
    return true;
}
inline bool roomSlabWhole(const RoomSlab& s) noexcept { return s.lo <= -kRoomSlabOpen && s.hi >= kRoomSlabOpen; }

// A door's state, from its two leaves (Narrow_Door_Left and _Right on a carrier: Flight 43, 0.8 to 1.0 m apart shut, 4.4 m open, at both doors), whoever opens it: the player, an NPC
// or the game. The watch learns the door's rest, the least gap it has seen at or under kDoorShutGapM, and calls the door open on the first step its leaves stand more than kDoorMoveM
// past that rest, so the room behind is back while the crack is a centimetre or two. It is shut again only once the leaves are back within kDoorMoveM of rest and have moved no more
// than kDoorStillM a step for kDoorStillMs, so a closing door keeps its room until it has stopped. Before a rest is known (a door first seen apart), past kDoorOpenGapM is open and
// between the two first seen is open. Not read is not known, and a door not known never blocks. Elite's own door state (the decision to open, a frame or more before the leaves move)
// is not read: the log's first-move line says how wide the crack was when the watch caught it, which says whether that would buy anything.
constexpr float kDoorNearM = 5.0f;          // a first value: no flight has said how near a door opens
constexpr float kDoorMatchM = 3.0f;         // a leaf belongs to the cut it lies within this of, along a2
constexpr float kDoorShutGapM = 1.1f, kDoorOpenGapM = 1.3f;
constexpr float kDoorMoveM = 0.02f;         // past rest by more than this is open
constexpr float kDoorStillM = 0.005f;       // a step that moves the gap more than this is not still
constexpr uint64_t kDoorStillMs = 100;
constexpr const char* kDoorLeafLeft = "Narrow_Door_Left";
constexpr const char* kDoorLeafRight = "Narrow_Door_Right";
enum class DoorState : uint8_t { kUnknown, kShut, kOpen };
inline const char* doorStateName(DoorState s) noexcept { return s == DoorState::kShut ? "shut" : s == DoorState::kOpen ? "open" : "not known"; }
class DoorWatch {
public:
    DoorState step(bool gapKnown, float gapM, uint64_t nowMs) noexcept {
        if (!gapKnown || !std::isfinite(gapM)) {
            state_ = DoorState::kUnknown;   // the rest is the door's, and stays
            havePrev_ = false;
            still_ = false;
            return state_;
        }
        const bool moved = !havePrev_ || std::fabs(gapM - prev_) > kDoorStillM;
        prev_ = gapM;
        havePrev_ = true;
        if (gapM <= kDoorShutGapM && (rest_ <= 0.0f || gapM < rest_)) rest_ = gapM;
        const bool restKnown = rest_ > 0.0f;
        if (restKnown ? gapM > rest_ + kDoorMoveM : gapM > kDoorOpenGapM) {
            state_ = DoorState::kOpen;
            still_ = false;
        } else if (!restKnown) {
            if (state_ == DoorState::kUnknown) state_ = DoorState::kOpen;   // between the two, first seen: parting or closing, so not shut
            still_ = false;
        } else if (moved) {
            still_ = false;
        } else {
            if (!still_) stillSinceMs_ = nowMs;
            still_ = true;
            if (nowMs - stillSinceMs_ >= kDoorStillMs) state_ = DoorState::kShut;
        }
        return state_;
    }
    DoorState state() const noexcept { return state_; }
    float rest() const noexcept { return rest_; }   // 0: not known
    void reset() noexcept {
        state_ = DoorState::kUnknown;
        rest_ = 0.0f;
        prev_ = 0.0f;
        havePrev_ = false;
        still_ = false;
        stillSinceMs_ = 0;
    }

private:
    DoorState state_ = DoorState::kUnknown;
    float rest_ = 0.0f;
    float prev_ = 0.0f;
    bool havePrev_ = false;
    bool still_ = false;
    uint64_t stillSinceMs_ = 0;
};
inline bool doorBlocks(DoorState s, float camAlong, float cut) noexcept {
    return s == DoorState::kShut && std::isfinite(camAlong) && std::fabs(camAlong - cut) > kDoorNearM;
}
// Which cut a leaf at `along` belongs to, or -1.
inline int doorCutOf(const RoomLayout& room, float along) noexcept {
    if (!std::isfinite(along)) return -1;
    for (uint32_t i = 0; i < room.slabCuts && i < 2; ++i)
        if (std::fabs(along - room.slabCut[i]) <= kDoorMatchM) return static_cast<int>(i);
    return -1;
}
// The door rule's levels (TEMPORARY, hotkey.concourse_room_test steps them for a flight to compare): records only, the default, where a record the rule culls still counts as kept in
// its rig's tally, so the rule never vetoes a whole rig; rigs too, where it counts as culled; and off. Flight 44's ceiling counted it as culled, and the bridge's floor went: a rig is
// vetoed when every record of it the store judged was culled, and the floor was in no list of culled records, so it is INFERRED to ride in such a rig.
enum class DoorRuleMode : uint8_t { kRecords, kRigs, kOff };
inline DoorRuleMode doorRuleNext(DoorRuleMode m) noexcept { return m == DoorRuleMode::kRecords ? DoorRuleMode::kRigs : m == DoorRuleMode::kRigs ? DoorRuleMode::kOff : DoorRuleMode::kRecords; }
inline const char* doorRuleName(DoorRuleMode m) noexcept { return m == DoorRuleMode::kRecords ? "records" : m == DoorRuleMode::kRigs ? "records and rigs" : "off"; }

// The live room from tick to tick. A tick with its record not read (the address lost and sighted again on the next pass) keeps "inside" for up to kLiveRoomHoldMs: Flight 42 had
// the cull on for one tick 6 m inside the bridge, its record unread, and the bake on the bridge windows for that frame. A tick that reads it, inside or out, says at once.
constexpr uint64_t kLiveRoomHoldMs = 500;
class LiveRoomLatch {
public:
    bool step(const RoomLayout& room, bool cameraKnown, bool liveRead, float cameraDistM, float liveRadiusM, uint64_t nowMs) noexcept {
        if (room.liveRoomName == nullptr) {
            inside_ = false;
            return false;
        }
        if (cameraKnown && liveRead) {
            inside_ = insideLiveRoom(room, cameraKnown, liveRead, cameraDistM, liveRadiusM);
            readMs_ = nowMs;
            return inside_;
        }
        if (inside_ && nowMs - readMs_ <= kLiveRoomHoldMs) return true;
        inside_ = false;
        return false;
    }
    void reset() noexcept { inside_ = false; }

private:
    bool inside_ = false;
    uint64_t readMs_ = 0;
};
constexpr float kBackdropNearZ = 0.025f;       // the game's near plane (reversed infinite Z: view depth = this / z)
constexpr float kBackdropMinDistM = 5.0f;      // a captured pixel nearer than this along its ray is not backdrop and is not written
constexpr float kBackdropFarM = 60000.0f;      // the distance written for a pixel with no depth (z = 0), and the largest stored distance
// The display's march along the view's ray (concourse_pane.cpp, backdropS). It starts this far along, inside what the bake's near plane left, and its steps are even in 1 / distance.
constexpr float kBackdropMarchNearM = 30.0f;
constexpr int kBackdropMarchSteps = 12, kBackdropMarchBisections = 5;
// The point s = off + dir * t with |s| = radius, t the far root (off inside the sphere has one). A direction that misses (off outside the sphere) takes the nearest point,
// as the shader does (the discriminant is clamped at 0). `dir` need not be unit; False for a zero direction. With off = 0 the result is dir * radius.
inline bool backdropSphereHit(const float off[3], const float dir[3], float radius, float out[3]) noexcept {
    const float dd = dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2];
    if (!(dd > 0.0f) || !std::isfinite(dd)) return false;
    const float inv = 1.0f / std::sqrt(dd);
    const float d[3] = {dir[0] * inv, dir[1] * inv, dir[2] * inv};
    const float b = off[0] * d[0] + off[1] * d[1] + off[2] * d[2];
    const float c = off[0] * off[0] + off[1] * off[1] + off[2] * off[2] - radius * radius;
    const float disc = b * b - c;
    const float t = -b + std::sqrt(disc > 0.0f ? disc : 0.0f);
    for (int i = 0; i < 3; ++i) out[i] = off[i] + d[i] * t;
    return true;
}
// ---- Automatic capture (PROTOTYPE, docs\concourse-cull-2026-11-05.md) --------------------------------------------------------------------------------------
// While mirror-backdrop is the mode, the panes in view are probed for what the cube cannot show (a colourless pass whose occlusion count is the pixels with captured coverage
// under one half through lod 3). When the missing share of the viewport is at least kBackdropMissingShare, a capture is started, as F6 does, if nothing blocks it and
// kBackdropMinGapMs have passed since the last capture ended. A capture that does not make the share fall doubles the gap, to kBackdropMaxGapMs, so a direction the capture cannot
// fill does not cost a lift every 1.5 s; the gap resets when the share falls. Pure: the tick owns the clock and the log.
constexpr float kBackdropMissingShare = 0.002f;     // 0.2% of the view
constexpr uint64_t kBackdropMinGapMs = 1500;
constexpr uint64_t kBackdropMaxGapMs = 30000;
constexpr float kBackdropFallFactor = 0.9f;          // a share below this fraction of what it was at the capture's start has fallen

struct BackdropAutoInput {
    uint64_t nowMs = 0;
    float share = 0.0f;           // the missing share of the viewport over the rolling window (0 to 1)
    bool autoOn = false;          // a hotkey (F4 to backdrop, or F6) turned automatic capture on
    bool modeBackdrop = false;    // the pane mode is mirror-backdrop
    bool cullPartOn = false;      // the cull's own part is on
    bool plainMirror = false;     // the bay machine is in its plain mirror phase
    bool liftOn = false;          // the lift cull is on
    bool captureRunning = false;  // a capture (auto or F6) is in progress
};
struct BackdropAutoDecision {
    bool fire = false;
    bool escalated = false;       // the gap was doubled by this call and the escalation has not been said yet (the caller logs one line)
    float shareAtStart = 0.0f;    // with escalated: the share the last capture started from
    uint64_t gapMs = kBackdropMinGapMs;
};
class BackdropAutoGate {
public:
    uint64_t gapMs() const noexcept { return gapMs_; }
    void reset() noexcept {
        gapMs_ = kBackdropMinGapMs;
        haveEnd_ = false;
        judged_ = true;
        logged_ = false;
    }
    // Any capture's end, auto or F6.
    void captureEnded(uint64_t nowMs) noexcept {
        lastEndMs_ = nowMs;
        haveEnd_ = true;
    }
    BackdropAutoDecision step(const BackdropAutoInput& in) noexcept {
        BackdropAutoDecision d;
        d.gapMs = gapMs_;
        if (!in.autoOn || !in.modeBackdrop || !in.cullPartOn || !in.plainMirror || in.liftOn || in.captureRunning) return d;
        if (!(in.share >= kBackdropMissingShare)) {   // nothing meaningful is missing: whatever the last capture did, the gap starts over
            gapMs_ = kBackdropMinGapMs;
            judged_ = true;
            logged_ = false;
            d.gapMs = gapMs_;
            return d;
        }
        if (haveEnd_ && in.nowMs - lastEndMs_ < kBackdropMinGapMs) return d;   // too soon to read the effect of the last capture
        if (haveEnd_ && !judged_) {
            judged_ = true;
            if (in.share < startShare_ * kBackdropFallFactor) {
                gapMs_ = kBackdropMinGapMs;
                logged_ = false;
            } else {
                gapMs_ = gapMs_ * 2 > kBackdropMaxGapMs ? kBackdropMaxGapMs : gapMs_ * 2;
                if (!logged_) {
                    logged_ = true;
                    d.escalated = true;
                    d.shareAtStart = startShare_;
                }
            }
        }
        d.gapMs = gapMs_;
        if (haveEnd_ && in.nowMs - lastEndMs_ < gapMs_) return d;
        startShare_ = in.share;
        judged_ = false;
        d.fire = true;
        return d;
    }

private:
    uint64_t gapMs_ = kBackdropMinGapMs;
    uint64_t lastEndMs_ = 0;
    bool haveEnd_ = false;
    bool judged_ = true;     // the last capture's effect has been read (or there was none)
    bool logged_ = false;    // the escalation has been said since the gap last reset
    float startShare_ = 0.0f;
};

// ---- The backdrop's glass look (diag.concourse_glass = "r,g,b,haze,blur,fresnel") ---------------------------------------------------------------------------
// r,g,b multiply the captured colour (0 to 2); haze is how far the glass's own level colour covers it (0 to 1); blur is added to every mip level of the look-up (0 to 8);
// fresnel adds up to that much more haze at a grazing angle (0 to 2).
constexpr float kGlassDefault[6] = {0.92f, 0.97f, 0.96f, 0.03f, 0.25f, 0.25f};
inline bool parseGlass(const char* text, float out[6]) noexcept {
    if (!text) return false;
    static const float kMax[6] = {2.0f, 2.0f, 2.0f, 1.0f, 8.0f, 2.0f};
    float v[6] = {};
    const char* s = text;
    for (int i = 0; i < 6; ++i) {
        while (*s == ' ' || *s == '\t') ++s;
        char* end = nullptr;
        const double d = std::strtod(s, &end);
        if (end == s || !std::isfinite(d) || d < 0.0 || d > static_cast<double>(kMax[i])) return false;
        v[i] = static_cast<float>(d);
        s = end;
        while (*s == ' ' || *s == '\t') ++s;
        if (i < 5) {
            if (*s != ',') return false;
            ++s;
        }
    }
    if (*s != '\0') return false;
    for (int i = 0; i < 6; ++i) out[i] = v[i];
    return true;
}
// ---- The glow levels by window type (diag.concourse_glow = "sky,angled,other,bay") ---------------------------------------------------------------------
// The steady light of a pane: the brightest on overhead skylights, a level between on angled skylights, a middle level on any other window, and its own level on the
// bay window. Absolute levels, each from 0 to 16. Overhead, angled and other are told by how much the pane faces the anchor's up axis (the pixel shader).
constexpr float kGlowMax = 16.0f;
constexpr float kGlowSkyDefault = 0.5f, kGlowAngledDefault = 0.5f, kGlowOtherDefault = 1.0f, kGlowBayDefault = 0.5f;
// "a,b,c,d": four numbers, each 0 to 16, separated by commas, spaces allowed around each. False for anything else (the caller keeps the last value).
inline bool parseGlowLevels(const char* text, float out[4]) noexcept {
    if (!text) return false;
    float v[4] = {};
    const char* s = text;
    for (int i = 0; i < 4; ++i) {
        while (*s == ' ' || *s == '\t') ++s;
        char* end = nullptr;
        const double d = std::strtod(s, &end);
        if (end == s || !std::isfinite(d) || d < 0.0 || d > static_cast<double>(kGlowMax)) return false;
        v[i] = static_cast<float>(d);
        s = end;
        while (*s == ' ' || *s == '\t') ++s;
        if (i < 3) {
            if (*s != ',') return false;
            ++s;
        }
    }
    if (*s != '\0') return false;
    for (int i = 0; i < 4; ++i) out[i] = v[i];
    return true;
}

// ---- The windows' clearing, following the outside glass in view ----------------------------------------------------------------------------------
// Mirror (the glass stands in, the cull is on) -> Lifting (the cull is off, the glass still opaque, so the station is back before it opens) -> FadingOut ->
// Clear. A reading of the total share of the view that all the outside glass fills (exteriorGlassShare), at or over enterShare, twice in a row in Mirror, starts
// it, and every outside pane then fades together. In Lifting, FadingOut and Clear the share below enterShare * kClearExitRatio for releaseMs running releases it:
// with the glass out of view (share at most kClearUnseenShare, or no reading) straight back to Mirror, else FadingIn, which returns to Mirror when it is done,
// turns round to FadingOut if the share is over enterShare again, and jumps to Mirror if the glass goes out of view.
enum class ClearPhase : uint8_t { kMirror, kLifting, kFadingOut, kClear, kFadingIn };

inline const char* clearPhaseName(ClearPhase p) noexcept {
    switch (p) {
        case ClearPhase::kMirror: return "mirror";
        case ClearPhase::kLifting: return "lifting";
        case ClearPhase::kFadingOut: return "fading-out";
        case ClearPhase::kClear: return "clear";
        default: return "fading-in";
    }
}

// The shop window (the ship services counter) is the mesh n=30, start 7301760. It reads exterior, and at the counter it fills 7.3 to 8.3% of the view (Flight 11, cam_local
// 9.2 26.17 30.8), its maximum 9.8%; elsewhere, in three flights, n=30 stayed under 3%. The 20% entry share never reached it, so it enters at 0.3 of the share.
// The exit follows at kClearExitRatio of that. Any other pane enters at the share itself. The scale is applied per pane, inside the total (exteriorGlassShare divides
// each pane's share by it), so the machine compares the total with the entry share as it stands.
// In a room whose bay is itself n=30 (an orbital outpost's) that is the bay, and it enters at the share.
inline float clearEntryScale(uint32_t n, uint32_t bayN = kBayIndexCount) noexcept { return n == 30 && bayN != 30 ? 0.3f : 1.0f; }

// ---- The window share: all the outside glass in the main view ---------------------------------------------------------------------------------------
// One window can be several meshes (a bar's outside glass is three, 2.2%, 1.5% and 1.4% of the view), none big enough alone, so the clearing machine reads their
// total. A sample is one glass draw's occlusion-query share of the viewport it was drawn into, with its pane and the side the table gave it when the query opened.
struct GlassSample {
    PaneKey key;
    float share = 0.0f;       // 0 to 1, of that viewport
    double pixels = 0.0;      // that viewport's width times its height
    PaneSide side = PaneSide::kUnknown;
};

// The most pixels a viewport had in the bins since the visit began, decaying 0.1% a frame so a drop in render scale is followed within seconds.
constexpr double kMainViewDecay = 0.999;
inline double mainViewPixelsNext(double prev, double frameMax) noexcept {
    const double decayed = prev * kMainViewDecay;
    return frameMax > decayed ? frameMax : decayed;
}

// The total of one frame's glass. Only viewports of at least half the main view count (the offscreen passes that draw the same glass into small targets are not
// the screen); a pane drawn twice (two eyes) counts once, at its larger share. Exterior counts; so does a pane of unknown side whose index count is the room's bay
// window (before the side tests have run, and in VR, where no side test runs); interior never. Each share is divided by clearEntryScale, so the shop window's 30%
// entry share is inside the total. The sum is clamped to 1.
inline float exteriorGlassShare(const GlassSample* samples, uint32_t count, double mainPixels, uint32_t bayN) noexcept {
    double main = mainPixels;
    for (uint32_t i = 0; i < count; ++i)
        if (samples[i].pixels > main) main = samples[i].pixels;   // a frame's own largest viewport is the main view when none is remembered yet
    double total = 0.0;
    for (uint32_t i = 0; i < count; ++i) {
        const GlassSample& s = samples[i];
        if (!(s.pixels >= main * 0.5) || !(s.pixels > 0.0)) continue;
        if (!(s.side == PaneSide::kExterior || (s.side == PaneSide::kUnknown && s.key.n == bayN))) continue;
        bool larger = false;   // a later sample of the same pane with a bigger share stands for it
        bool earlierSame = false;
        for (uint32_t j = 0; j < count; ++j) {
            if (j == i) continue;
            const GlassSample& o = samples[j];
            if (o.key.n != s.key.n || o.key.startIndex != s.key.startIndex || o.key.baseVertex != s.key.baseVertex) continue;
            if (!(o.pixels >= main * 0.5) || !(o.pixels > 0.0)) continue;
            if (o.share > s.share) larger = true;
            else if (o.share == s.share && j < i) earlierSame = true;
        }
        if (larger || earlierSame) continue;
        total += static_cast<double>(s.share) / static_cast<double>(clearEntryScale(s.key.n, bayN));
    }
    return total > 1.0 ? 1.0f : static_cast<float>(total);
}

constexpr float kClearExitRatio = 0.4f;     // the exit share is enterShare * this (20% in, 8% out)
constexpr uint64_t kClearReleaseMs = 250;   // the share must stay below the exit share this long
constexpr float kClearUnseenShare = 0.01f;  // at or under this (or no reading) the glass is out of view
constexpr uint64_t kClearLiftMs = 150;
constexpr uint64_t kClearFadeMs = 600;
constexpr uint32_t kClearReadingsToEnter = 2;

struct ClearInput {
    uint64_t nowMs = 0;
    bool gate = false;        // the cull is wanted: key on, on foot in social space, the anchor seen, not sealed
    float windowShare = -1.0f;   // the latest completed frame's total share of the view for all the outside glass (exteriorGlassShare, 0 to 1); negative when there is no reading
    uint32_t shareSeq = 0;    // moves with each new reading: one frame read on two ticks is one reading
    float enterShare = 0.0f;   // 0 never clears
    uint64_t liftMs = kClearLiftMs;
    uint64_t fadeMs = kClearFadeMs;
    uint64_t releaseMs = kClearReleaseMs;
};
struct ClearOutput {
    bool cullOn = true;
    float bayOpacity = 1.0f;
    ClearPhase phase = ClearPhase::kMirror;
};

class ClearMachine {
public:
    ClearPhase phase() const noexcept { return phase_; }
    float opacity() const noexcept { return opacity_; }
    void reset() noexcept {
        phase_ = ClearPhase::kMirror;
        opacity_ = 1.0f;
        hits_ = 0;
        seqSeen_ = false;
        holding_ = false;
    }
    ClearOutput step(const ClearInput& in) noexcept {
        if (!in.gate) {
            reset();
            return out();
        }
        const bool fresh = !seqSeen_ || in.shareSeq != lastSeq_;
        seqSeen_ = true;
        lastSeq_ = in.shareSeq;
        const bool over = in.enterShare > 0.0f && in.windowShare >= 0.0f && in.windowShare >= in.enterShare;
        const bool below = in.enterShare <= 0.0f || in.windowShare < in.enterShare * kClearExitRatio;   // a negative share (no reading) is below
        const bool unseen = in.windowShare <= kClearUnseenShare;                                         // negative included
        switch (phase_) {
            case ClearPhase::kMirror:
                opacity_ = 1.0f;
                if (fresh) hits_ = over ? hits_ + 1 : 0;
                if (in.enterShare > 0.0f && hits_ >= kClearReadingsToEnter) enter(ClearPhase::kLifting, in.nowMs);
                break;
            case ClearPhase::kLifting:
            case ClearPhase::kFadingOut:
            case ClearPhase::kClear:
                if (released(below, in)) {
                    if (unseen || phase_ == ClearPhase::kLifting) {
                        toMirror(in.nowMs);
                    } else {
                        fromOpacity_ = opacity_;
                        enter(ClearPhase::kFadingIn, in.nowMs);
                    }
                    break;
                }
                if (phase_ == ClearPhase::kLifting) {
                    opacity_ = 1.0f;
                    if (in.nowMs - startMs_ >= in.liftMs) {
                        fromOpacity_ = 1.0f;
                        enter(ClearPhase::kFadingOut, in.nowMs);
                    }
                } else if (phase_ == ClearPhase::kFadingOut) {
                    opacity_ = fromOpacity_ - fadeFraction(in);
                    if (opacity_ <= 0.0f) {
                        opacity_ = 0.0f;
                        enter(ClearPhase::kClear, in.nowMs);
                    }
                } else {
                    opacity_ = 0.0f;
                }
                break;
            case ClearPhase::kFadingIn:
                opacity_ = fromOpacity_ + fadeFraction(in);
                if (opacity_ > 1.0f) opacity_ = 1.0f;
                if (over) {   // turned round: fading out from where the fade had got to
                    fromOpacity_ = opacity_;
                    enter(ClearPhase::kFadingOut, in.nowMs);
                    break;
                }
                if (unseen || opacity_ >= 1.0f) toMirror(in.nowMs);   // done, or the bay went out of view: the cull returns now
                break;
        }
        return out();
    }

private:
    ClearOutput out() const noexcept {
        ClearOutput o;
        o.cullOn = phase_ == ClearPhase::kMirror;
        o.bayOpacity = opacity_;
        o.phase = phase_;
        return o;
    }
    void enter(ClearPhase p, uint64_t now) noexcept {
        phase_ = p;
        startMs_ = now;
        if (p != ClearPhase::kFadingOut && p != ClearPhase::kClear) holding_ = false;   // a hold carries on from Lifting to FadingOut to Clear
    }
    void toMirror(uint64_t now) noexcept {
        opacity_ = 1.0f;
        hits_ = 0;
        enter(ClearPhase::kMirror, now);
    }
    // The release hold: `below` held continuously for releaseMs. A reading at or over the exit share cancels it.
    bool released(bool below, const ClearInput& in) noexcept {
        if (!below) {
            holding_ = false;
            return false;
        }
        if (!holding_) {
            holding_ = true;
            holdStart_ = in.nowMs;
        }
        return in.nowMs - holdStart_ >= in.releaseMs;
    }
    // The elapsed part of one full fade: the rate is one unit of opacity per fadeMs, so a fade turned round part-way takes only the time it needs.
    float fadeFraction(const ClearInput& in) const noexcept {
        if (in.fadeMs == 0) return 1.0f;
        return static_cast<float>(static_cast<double>(in.nowMs - startMs_) / static_cast<double>(in.fadeMs));
    }

    ClearPhase phase_ = ClearPhase::kMirror;
    float opacity_ = 1.0f;
    float fromOpacity_ = 1.0f;
    uint32_t hits_ = 0;
    uint32_t lastSeq_ = 0;
    bool seqSeen_ = false;
    uint64_t startMs_ = 0;
    bool holding_ = false;
    uint64_t holdStart_ = 0;
};

}  // namespace edvr::concourse
