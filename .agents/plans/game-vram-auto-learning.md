# Plan: `advanced.game_vram = auto` learns its headroom per setup

Builds on .agents/plans/game-vram-report.md (done). Context:
docs/memory-growth-2026-10-22.md, the 2026-11-20 flight entries. Elite sizes
its streaming budget once, in its first ~30 s, as 0.85 x the
DedicatedVideoMemory our GetDesc hook reports. `auto` reports
(budget - reserve - margin) / 0.85. A fixed reserve was 59 MB from the
limit on one card; reserves differ by OS (Windows/Proton), upscaler, card
and vendor driver. So: start from safe defaults per profile and AA class,
measure the remaining headroom with DXGI figures only (no game addresses),
and persist a learned reserve per setup.

Project rules (AGENTS.md): no `near`/`far` identifiers, no agent-addressed
text in comments, never read big files whole, no exceptions thrown. Do NOT
run build.bat, do NOT edit build.bat, do NOT commit. Two parts run at the
same time; touch ONLY your part's files.

## Shared API

### Changes to `src/common/game_vram.h` (Part A)

`Inputs` gains two fields (keep every existing field and behaviour):

```cpp
uint32_t reserveMb = 0;  // Auto: EDVR's, the upscaler's and the driver's share to leave free; 0 = the old fixed rule (upscalerOn ? kReserveUpscalerMb : kReserveBaseMb)
uint32_t marginMb  = 0;  // Auto: extra room on top; 0 = kMarginMb
```

Auto with `osBudgetBytes == 0` changes: instead of "no change", use
`budgetMb = realMb * 95 / 100` and `why = "auto, no OS budget: 95% of the card"`
(the result may still be "no change" if it reaches the card). Update the
existing rig cell that expected "no OS budget" accordingly. Everything else
in `compute` stays as it is.

`formatCallLine` for auto additionally appends ` reserve_mb=<R>` after
`target_mb=<T>` (R = the reserve actually used). Update the rig's exact
expected strings.

### New `src/common/game_vram_learn.h` (Part A writes, Part B includes)

Header-only, `#pragma once`, namespace `edvr::gamevram`, standard library
only (`<cstdint> <cstdio> <cstring> <string> <vector> <algorithm> <cctype>`).

```cpp
enum class AaClass : uint32_t { Off = 0, Taa, Upscaler };
// "" or "off" -> Off; "on" or "taa" -> Taa; anything else (dlss, dlaa, fsr, xess, dittosr, ...) -> Upscaler. Case-insensitive.
AaClass aaClassOf(const std::string& aaMode);
const char* aaClassName(AaClass c);   // "off", "taa", "upscaler"

// Safe starting reserves in MB, before anything is learned. They err towards
// giving Elite less: a flight at 8192 MB on a 12 GB card looked unchanged.
//   flat: Off 1536, Taa 2048, Upscaler 3584
//   VR:   Off 3072, Taa 3584, Upscaler 4608
uint32_t defaultReserveMb(bool vrProfile, AaClass aa);

constexpr uint32_t kReserveFloorMb = 768;
constexpr uint32_t kReserveCeilMb  = 16384;
constexpr uint32_t kLearnStepMb    = 256;   // extra added on every tightening
constexpr uint32_t kRelaxMaxMb     = 512;   // most a reserve shrinks per session
constexpr uint32_t kMinSamples     = 300;   // 5 min of 1 s samples before a session teaches anything except a crossing
constexpr uint64_t kPlateauSessionMs = 20ull * 60 * 1000;
constexpr uint64_t kPlateauLeadMs    = 5ull * 60 * 1000;
constexpr size_t   kMaxRecords     = 64;

// "v=<VENDOR hex4> d=<DEVICE hex4> drv=<a.b.c.d|unknown> layer=<windows|wine> prof=<flat|vr> aa=<mode lowercased, empty -> off>"
// driverVersion is the LARGE_INTEGER from IDXGIAdapter::CheckInterfaceSupport: a = bits 63..48, b = 47..32, c = 31..16, d = 15..0; 0 -> "unknown".
// Hex digits uppercase, zero-padded to 4.
std::string makeKey(uint32_t vendorId, uint32_t deviceId, uint64_t driverVersion,
                    bool wine, bool vrProfile, const std::string& aaMode);

struct Record {
    std::string key;
    uint32_t reserveMb = 0;
    uint32_t sessions = 0;    // sessions that produced an update or a hold
    uint32_t tightened = 0;
    uint32_t relaxed = 0;
};

// File format: UTF-8 text, one record per line:
//   <reserveMb>\t<sessions>\t<tightened>\t<relaxed>\t<key>\n
// Lines starting with '#', empty lines, and malformed lines (wrong field
// count, a non-number, reserve outside [kReserveFloorMb, kReserveCeilMb],
// empty key) are skipped. CR before LF is tolerated. A later line with the
// same key replaces the earlier one.
std::vector<Record> parseStore(const std::string& text);
// First line: "# EDVR: what advanced.game_vram = auto learned per setup. Safe to delete.\n", then one line per record in order.
std::string serializeStore(const std::vector<Record>& records);
const Record* findRecord(const std::vector<Record>& records, const std::string& key);
// Replace the record with the same key (moving it to the end), else append.
// Then, while more than kMaxRecords, erase the first (oldest) record.
void upsertRecord(std::vector<Record>& records, const Record& r);

// One session's DXGI figures, fed once a second.
class SessionWatch {
public:
    // Ignored when budgetBytes == 0. Usage and budget are this process's
    // local segment (QueryVideoMemoryInfo). nowMs is monotonic.
    void sample(uint64_t nowMs, uint64_t usageBytes, uint64_t budgetBytes);
    uint32_t samples() const;
    uint64_t durationMs() const;       // last sample time - first sample time; 0 with < 2 samples
    uint32_t peakUsageMb() const;      // max usage >> 20
    uint32_t budgetAtPeakMb() const;   // budget >> 20 at the sample that set the peak
    int32_t  minHeadroomMb() const;    // min over samples of (budget - usage) in MB (signed; floor division toward minus infinity is not needed, use (int64(budget) - int64(usage)) / 1048576); INT32_MAX with no samples
    uint32_t crossings() const;        // samples with usage > budget
    // True when durationMs() >= kPlateauSessionMs and the peak usage had
    // already been reached (to within 3%) at least kPlateauLeadMs before the
    // last sample: max usage over samples with t <= last - kPlateauLeadMs
    // is >= 97% of the session peak.
    bool plateaued() const;
private:
    // Keep per-minute maxima (minute = (t - first) / 60000) so plateaued()
    // needs no per-sample history; plus the scalars above.
};

struct LearnResult {
    uint32_t reserveMb = 0;   // what to use next session
    bool changed = false;
    const char* why = "";     // static string
};

// Rules, in order (margin = marginMb):
//  1. crossings() > 0 or minHeadroomMb() < margin -> tighten:
//     reserve + (margin - minHeadroom) + kLearnStepMb, clamped to kReserveCeilMb.
//     why "tightened: came within the margin" (or "tightened: crossed the budget" when crossings() > 0).
//     This applies even to a short session.
//  2. samples() < kMinSamples -> hold, why "held: session too short".
//  3. !plateaued() -> hold, why "held: usage still rising or session under 20 min".
//  4. minHeadroom > margin + kLearnStepMb -> relax by
//     min(minHeadroom - margin - kLearnStepMb, kRelaxMaxMb), not below kReserveFloorMb.
//     why "relaxed: room to spare" (changed=false and why "held: at the floor" if already at the floor).
//  5. otherwise hold, why "held: headroom within the margin".
LearnResult learn(uint32_t reserveMb, const SessionWatch& s, uint32_t marginMb);

// "game vram: learned for <key>: peak <P> MB of a <B> MB budget, closest <H> MB, crossings <C>, plateau <yes|no>, <N> s -> reserve <old> -> <new> MB next session (<why>)"
int formatLearnLine(char* buf, size_t len, const std::string& key, uint32_t oldReserveMb,
                    const SessionWatch& s, const LearnResult& r);
```

## Part A: the logic and its rig

Files (only these): `src/common/game_vram.h`, `src/common/game_vram_learn.h`
(new), `tools/game_vram_test/game_vram_test.cpp`.

1. Implement the API above exactly. All functions inline.
2. Rig: keep every existing cell (adjust only those the plan changes: the
   auto no-OS-budget cell and the auto call-line strings). Add:
   - V7 key and classes: makeKey for (0x10DE, 0x2684, driver
     0x0020000F17C40000 -> "32.15.6084.0"... compute the expected string by
     hand from the bit rule and write it literally), wine true/false, vr
     true/false, aa "" -> "aa=off", "XeSS" -> "aa=xess"; driver 0 ->
     "drv=unknown". aaClassOf for "", "off", "on", "TAA", "dlss", "xess",
     "dittosr". defaultReserveMb for all six combinations.
   - V8 store: round trip of three records; a comment, an empty line, a CRLF
     line, a line with 4 fields, a reserve of 100, a non-number -> skipped;
     duplicate key -> the later wins; upsertRecord replaces and moves to the
     end; 65 distinct upserts keep 64 and drop the first.
   - V9 SessionWatch and learn, with synthetic 1 s samples:
     * 120 samples, no crossing, big headroom -> held, too short.
     * 60 samples including one with usage > budget by 100 MB, reserve 3584,
       margin 512 -> tightened to 3584 + (512 - (-100)) + 256 = 4452.
     * 1500 samples (25 min) rising linearly to the peak at the last sample
       -> not plateaued -> held.
     * 1500 samples reaching the peak at minute 8 and flat after, min headroom
       2000 MB, reserve 3584 -> relaxed by min(2000-512-256, 512) = 512 -> 3072.
     * same with reserve 1000 and headroom 2000 -> 768 (floor), changed.
     * same with reserve 768 -> held: at the floor.
     * plateaued with min headroom 600 -> held: headroom within the margin.
     * budget 0 samples are ignored (samples() unchanged).
     * formatLearnLine exact string for one case; 16-byte buffer stays
       NUL-terminated.
   - V10 auto with reserveMb: card 12118, budget 11349, reserve 3584,
     margin 512 -> target 7253, reported floor(7253/0.85) = 8532, change;
     reserveMb 0 keeps the old result (9737 for upscaler on, budget 11349);
     budget 0 -> 95% of 12118 = 11512 as budget -> target 11512-3584-512 =
     7416 -> 8724, why "auto, no OS budget: 95% of the card".
   - V6 source text additions: src/d3d11/vram_tick.cpp contains
     `gameVramObserve(`; src/d3d11/game_vram_hook.cpp contains
     `defaultReserveMb(`, `learn(`, `MoveFileExW` and `wine_get_version`.
3. Build check (outside build.bat, objects in %TEMP%):
   `cl /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /I"src\common" tools\game_vram_test\game_vram_test.cpp`
   via vcvars64.bat; warning-free; run `--dry-run` and
   `--self-test <repo root>`. The V6 cells on Part B's files may fail until
   Part B lands: report, do not touch those files.

## Part B: the glue

Files (only these): `src/d3d11/game_vram_hook.h`,
`src/d3d11/game_vram_hook.cpp`, `src/d3d11/vram_tick.cpp`.

1. `game_vram_hook.h`: add
   ```cpp
   // Once a second from the graphics memory watch: this process's local usage
   // and budget (bytes). Teaches advanced.game_vram = auto; free otherwise.
   void gameVramObserve(uint64_t nowMs, uint64_t usageBytes, uint64_t budgetBytes);
   ```
2. `vram_tick.cpp`: include "game_vram_hook.h"; in the read lambda, after
   `vramRead`, when `figures.local.valid`, call
   `gameVramObserve(nowMs(), figures.local.usage, figures.local.budget);`
   (check the VramFigures/VramSegment field names in src/common/vram_watch.h
   and use the real ones).
3. `game_vram_hook.cpp`, Auto mode only (Off and Fixed unchanged):
   - At install, before the hook is committed, read the adapter's real
     DXGI_ADAPTER_DESC (`obj->GetDesc`) for VendorId/DeviceId, and
     `obj->CheckInterfaceSupport(__uuidof(IDXGIDevice), &li)` for the driver
     version (0 on failure). Wine: `GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version") != nullptr`.
     VR profile: `runtimeVrProfile()` from src/common/runtime_profile.h (grep
     its exact name/namespace). AA mode: `Config::get().requestedTemporalMode()`.
     Build the key with `makeKey`; remember the AA mode string.
   - Store path: `%LOCALAPPDATA%\EDVR\game_vram.txt` (GetEnvironmentVariableW;
     CreateDirectoryW the EDVR folder if missing; no store when LOCALAPPDATA
     is unset -> defaults, log says so). Read it whole (it is tiny), parseStore,
     findRecord. Reserve = record ? record->reserveMb : defaultReserveMb(vr, aaClassOf(aa)).
     Log: `game vram: auto for <key>: reserve <R> MB, learned over <n> session(s).`
     or `game vram: auto for <key>: reserve <R> MB, the safe default for <flat|vr> with <aaClassName> (nothing learned yet).`
   - The hook body passes `in.reserveMb = g_reserveMb; in.marginMb = kMarginMb;`.
   - `gameVramObserve`: return unless Auto and installed. If the AA mode
     (requestedTemporalMode) differs from the one at install, log once:
     `game vram: the AA mode changed from <a> to <b>; Elite's budget was sized for <a>, so this session teaches nothing more. A restart sizes it for <b>.`
     and stop learning. Otherwise feed a function-static `SessionWatch`.
     Every 60 s of samples (and only once samples() >= 60 or crossings() > 0):
     compute `learn(g_reserveMb, watch, kMarginMb)` (always from the starting
     reserve, so repeated passes never compound). If the result's reserveMb or
     why differs from the last saved one: re-read the store (another game
     instance may have written it), upsertRecord {key, result.reserveMb,
     sessions = base.sessions + 1, tightened = base.tightened + (result
     tightened ? 1 : 0), relaxed likewise} where base is the record as found
     at install (or zeros), write it atomically (write
     `game_vram.txt.tmp` in the same folder, then
     `MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)`),
     and log `formatLearnLine(...)`. On a write failure log once
     `game vram: could not save what auto learned to <path> (error <n>).`
     The game usually ends by TerminateProcess, so nothing may wait for exit.
   - All of this runs on the render thread once a second; keep it cheap and
     never throw.
4. Comments for people: why the reserve is learned (it differs by OS,
   upscaler, card and driver; only DXGI's own figures are used, no game
   address), and why learning stops on an AA change.

Reply with the files changed and any deviation (especially API names in the
codebase that differed from this plan).
