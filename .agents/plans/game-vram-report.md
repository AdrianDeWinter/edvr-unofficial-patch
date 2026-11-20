# Plan: tell Elite a smaller video memory size (`advanced.game_vram`)

Context: docs/memory-growth-2026-10-22.md, entries "Flight 20261119_215431" and
"Elite's streaming budget". Elite sizes its texture/mesh streaming budget as
0.85 x `DXGI_ADAPTER_DESC::DedicatedVideoMemory`, read through
`IDXGIAdapter::GetDesc` (vtable slot 8, byte offset 0x40) at the moment it
sizes the budget. It never asks Windows for the real per-process budget, and
EDVR's own 2-2.8 GB comes on top, so Elite overflows. This feature lets EDVR
change what Elite's own GetDesc calls return, and logs every such call.

Project rules that apply (AGENTS.md): Windows PowerShell 5.1 for shell work,
never read big files whole (grep, then read ranges), no names `near`/`far` in
C++, no agent-addressed text in source comments, every tool/rig has
`--self-test`, `--dry-run` writes nothing. Do NOT run build.bat; the
coordinator builds. Do NOT edit build.bat; the coordinator wires it. Do NOT
commit.

There are two parts. Each is done by a different agent at the same time.
Touch ONLY the files listed for your part.

## The shared API (both parts code against exactly this)

File `src/common/game_vram.h` (Part A writes it; Part B only includes it).
Header-only, `#pragma once`, includes only `<cstdint>`, `<cstddef>`,
`<cstdio>`, `<string>`, `<cctype>`, `<cstring>`. Namespace `edvr::gamevram`.

```cpp
enum class Mode : uint32_t { Off = 0, Auto, Fixed };

struct Setting {
    Mode     mode = Mode::Off;
    uint32_t fixedMb = 0;     // Fixed only
    bool     recognised = true; // false: the text was not off/auto/a number in range; mode is then Off
};

constexpr double   kEliteBudgetFactor = 0.85;  // Elite's own factor (build 334925, .data 0x145E78ADC)
constexpr uint32_t kAutoFloorMb   = 4096;      // auto never reports less: Elite takes other branches below 4 GB
constexpr uint32_t kFixedMinMb    = 256;
constexpr uint32_t kFixedMaxMb    = 1048576;
constexpr uint32_t kReserveUpscalerMb = 2560;  // EDVR's own share with an anti-aliasing/upscaler mode on
constexpr uint32_t kReserveBaseMb     = 1024;  // ... with it off
constexpr uint32_t kMarginMb          = 512;

// "off", "auto" (case-insensitive, surrounding spaces ignored), or a whole
// number of MB in [kFixedMinMb, kFixedMaxMb]. Empty text is Off and
// recognised. Anything else is Off with recognised = false.
Setting parseSetting(const std::string& text);

struct Inputs {
    uint64_t realDedicatedBytes = 0;  // what GetDesc really returned
    uint64_t osBudgetBytes = 0;       // QueryVideoMemoryInfo LOCAL Budget; 0 = unknown
    bool     upscalerOn = false;      // fix.temporal_aa is not off
};

struct Result {
    bool        change = false;       // true: write reportedBytes into DedicatedVideoMemory
    uint64_t    reportedBytes = 0;    // == realDedicatedBytes when change is false
    uint32_t    targetMb = 0;         // auto only: the streaming budget aimed at; else 0
    const char* why = "";             // short reason, static string, no spaces at the ends
};

// Off: change=false, why "off".
// Fixed: reportedMb = min(fixedMb, realMb); change = reportedMb < realMb;
//   why "fixed" or "fixed, not below the card" when fixedMb >= realMb.
// Auto: osBudgetBytes == 0 -> change=false, why "no OS budget".
//   reserve = upscalerOn ? kReserveUpscalerMb : kReserveBaseMb
//   targetMb = budgetMb - reserve - kMarginMb (0 if that would go negative)
//   reportedMb = floor(targetMb / kEliteBudgetFactor), then max(kAutoFloorMb), then min(realMb)
//   change = reportedMb < realMb; why "auto" or "auto, the card already fits".
// MB here means bytes >> 20 (integer). reportedBytes = reportedMb << 20 when change.
// realDedicatedBytes == 0 -> change=false, why "no card size".
Result compute(const Setting& s, const Inputs& in);

// The setting as the log names it: "off", "auto", or the number of MB.
// Writes into buf (always NUL-terminated); returns buf.
const char* settingText(const Setting& s, char* buf, size_t len);

// One line per GetDesc call that came from the game's exe. Exact format:
// "game vram: call=<n> caller=exe+0x<RVA hex uppercase> real_mb=<R> reported_mb=<P> setting=<settingText> (<why>)"
// plus, for auto only, " budget_mb=<B> target_mb=<T>" appended before " (".
// Returns the number of characters written (snprintf semantics, clamped to len-1).
int formatCallLine(char* buf, size_t len, uint32_t callIndex, uint64_t callerRva,
                   const Setting& s, const Inputs& in, const Result& r);
```

Inline all functions (header-only). Use `std::snprintf`. No exceptions, no
allocation beyond the std::string argument.

## Part A: the logic, its rig, and the ini key

Files you create or edit (only these):

1. `src/common/game_vram.h`: the API above, implemented exactly.
   A comment block at the top for people: what it is for (Elite sizes
   streaming from the card's size, ignoring the OS budget and EDVR's share),
   the three settings, and a pointer to docs/memory-growth-2026-10-22.md.
2. `tools/game_vram_test/game_vram_test.cpp`: a console rig, pattern after
   `tools/mem_watch_test/mem_watch_test.cpp` (read its first ~80 lines and its
   main() for the argument handling and the pass/fail reporting style).
   - `--dry-run`: print what the self-test would check, write nothing, exit 0.
   - `--self-test <repo root>`: run the cells, print one line per failure,
     print a final count, exit 1 on any failure, 0 otherwise.
   Cells (each a named check):
   - parseSetting: "off", " OFF ", "", "auto", "Auto", "8192", "256",
     "1048576" recognised with the right mode/fixedMb; "255", "1048577",
     "8 GB", "-1", "on", "12.5" are Off with recognised=false.
   - compute Off: never changes.
   - compute Fixed 8192 on a 12216 MB card: change, reported 8192 MB.
     Fixed 16384 on the same card: no change, reported == real.
     Fixed 1024 (below the auto floor) is allowed: reported 1024 MB.
   - compute Auto, card 12216 MB, budget 11350 MB, upscalerOn: target
     11350-2560-512 = 8278, reported floor(8278/0.85) = 9738, change.
     Same with upscalerOn false: target 9814, reported 11545, change.
     Budget 0: no change, why "no OS budget".
     Tiny budget 3000 MB, upscalerOn: target 0 -> reported 4096 (floor).
     Card 4000 MB, budget 3800 MB: floor 4096 then min(real) -> 4000, no change.
   - realDedicatedBytes 0: no change, why "no card size".
   - formatCallLine: the exact string for one fixed and one auto case
     (write the expected strings literally in the test), and that a 16-byte
     buffer is NUL-terminated and not overrun.
   - Source-text cells (read files under <repo root>; fail with a clear message
     if a file is missing):
     - edvr.ini contains a line `#game_vram = off` in the `[advanced]`
       section, and the comment block directly above it contains `# dev: hidden`.
     - src/d3d11/d3d11_proxy.cpp calls `gameVramInstall(` in both
       `edvr_impl_D3D11CreateDevice` and `edvr_impl_D3D11CreateDeviceAndSwapChain`
       (count >= 2 occurrences of `gameVramInstall(adapter`).
     - src/d3d11/game_vram_hook.cpp contains `_ReturnAddress()` and
       `formatCallLine(`.
   Compile flags the coordinator will use:
   `cl /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /I"src\common"`
   so it must build warning-free at /W4 with only the header and the
   standard library.
3. `edvr.ini`: add the key at the END of the `[advanced]` section (just
   before the `[experimental]` section's banner comment; grep for
   `^\[experimental\]` and the `# ====` banner lines above it, and insert
   before that banner, after one blank line). Format, following the other
   `[advanced]` keys (read lines 1937-1970 for the style):

   ```
   # The video memory Elite is told the graphics card has. Elite sizes its
   # texture and mesh streaming at 85% of the card's size and never asks
   # Windows how much it may really use, so with ED-UP's own work on the card
   # it can outgrow what Windows allows, spill into shared memory and slow to
   # a crawl until the next visit to the main menu.
   #   off   -- Elite is told the card's real size (the shipped behaviour).
   #   auto  -- Elite is told a size whose 85% fits Windows' budget for the
   #            game, less ED-UP's own share and a margin. Never less than
   #            4096 MB, never more than the card.
   #   a number of MB, 256 or more -- Elite is told that, or the card's real
   #            size if smaller. For reproducing a player's smaller card.
   # Only Elite's own questions get the changed answer; everything else in
   # the game process (the upscalers, the driver, ED-UP) sees the real card.
   # Read when the game creates its graphics device: a change takes effect
   # at the next start. The log's "game vram:" lines name each time Elite
   # asked and what it was told.
   # dev: hidden
   #game_vram = off
   ```

When done, reply with the files you changed and anything you did differently.

## Part B: the hook

Files you create or edit (only these):

1. `src/d3d11/game_vram_hook.h`:
   ```cpp
   #pragma once
   struct IDXGIAdapter;
   namespace edvr {
   // Install the GetDesc interception once per process, before the game's
   // device is created. `adapter` is what the game passed to
   // D3D11CreateDevice (may be null). Safe to call more than once.
   void gameVramInstall(IDXGIAdapter* adapter);
   }
   ```
2. `src/d3d11/game_vram_hook.cpp`. Includes: `<windows.h>`, `<dxgi1_4.h>`,
   `<intrin.h>`, `<atomic>`, `"game_vram_hook.h"`, `"game_vram.h"`,
   `"config.h"`, `"log.h"` (check the real header names with Glob/Grep:
   `src/common/log.h` and `src/common/vtable_hook.h`), `"vtable_hook.h"`.
   Link libs: dxgi.lib is already linked by the DLL build (grep build.bat for
   `dxgi.lib` to confirm; if it is not, say so in your reply, do not edit
   build.bat).

   Behaviour:
   - `gameVramInstall(adapter)`: a function-static `std::atomic<bool>` makes
     it run once. Read the setting:
     `Config::get().getString("advanced.game_vram", "off")` -> `parseSetting`.
     If `!recognised`, log once:
     `game vram: advanced.game_vram = "<text>" is not off, auto or a number of MB from 256; read as off.`
     If mode is Off: log one line
     `game vram: off -- Elite is told the card's real size.` and return
     without hooking anything.
     Otherwise get an adapter: `adapter` if non-null; else
     `CreateDXGIFactory1(__uuidof(IDXGIFactory1), ...)` then
     `EnumAdapters(0, ...)` (release both when done, AFTER the hook is
     committed; the hook patches the class's table in place so it outlives
     the object). Use `edvr::VTableHook` (src/common/vtable_hook.h):
     `attach(adapterObj, 64)`, leave the mode InPlace (default),
     `replace(8, (void*)&hookedGetDesc, (void**)&g_realGetDesc)`, `commit()`.
     Keep the VTableHook in a function-static/global that is never destroyed
     (a leaked `new VTableHook` is fine). On any failure log one line naming
     the step that failed (`attach`, `replace`, `commit`, `factory`,
     `adapter`) and return.
     On success log one line:
     `game vram: installed, setting=<settingText>, adapter=<passed by the game|enumerated>, GetDesc slot 8 -> <vtableOwnerModuleName(original entry)>; the game's own calls get the changed size, every other caller the real one.`
     (`vtableOwnerModuleName` is in vtable_hook.h; give it a MAX_PATH buffer.)
     Store the parsed Setting and `upscalerOn` in globals for the hook body:
     `upscalerOn = Config::get().requestedTemporalMode()` is non-empty and
     not `"off"`.
   - `hookedGetDesc(IDXGIAdapter* self, DXGI_ADAPTER_DESC* desc)`
     (`HRESULT STDMETHODCALLTYPE`): take `_ReturnAddress()` FIRST, then call
     `g_realGetDesc(self, desc)`. Return its HRESULT unchanged in every path.
     If it failed, or `desc` is null, or the return address is not inside
     the game exe's image, return immediately (no logging, no change).
     "Inside the game exe": base = `GetModuleHandleW(nullptr)`, extent =
     `SizeOfImage` from its PE headers, computed once (pattern:
     `addressInEdvr` in src/d3d11/device_hook.cpp ~line 895, but for the exe).
     For a game call: Inputs.realDedicatedBytes = desc->DedicatedVideoMemory;
     Inputs.osBudgetBytes: only when the mode is Auto, `self->QueryInterface`
     for `IDXGIAdapter3`, `QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)`,
     take `info.Budget`, Release; 0 on any failure. Inputs.upscalerOn from the
     global. `compute(...)`; if `change`, set `desc->DedicatedVideoMemory =
     reportedBytes`. Then log with `formatCallLine` (callIndex from a
     `std::atomic<uint32_t>` counter starting at 1; callerRva = return
     address - exe base) for the first 32 calls; at call 33 log once
     `game vram: further calls are counted, not logged.`; no line after that.
     Wrap the body after the real call in nothing fancy: no exceptions are
     thrown by any of it.
   - Logging: use the same call the codebase uses for one-off lines
     (`Log::get().note("%s", line)`, see src/d3d11/format_support_log.cpp
     line ~41). A 256-byte stack buffer for lines.
   - Comments for people: say why this exists in 3-6 lines at the top of the
     file (Elite sizes streaming from DedicatedVideoMemory x 0.85; EDVR's
     share; docs/memory-growth-2026-10-22.md), and why the caller filter is
     needed (the vtable patch is shared by every adapter object in the
     process: NGX, XeSS, FSR, the driver and EDVR must see the real size).
3. `src/d3d11/d3d11_proxy.cpp`: `#include "game_vram_hook.h"` near the other
   project includes, and in BOTH `edvr_impl_D3D11CreateDevice` and
   `edvr_impl_D3D11CreateDeviceAndSwapChain`, right after the
   `CreateDepth depth;` / target selection block and BEFORE the call to
   `target(...)`, add:
   ```cpp
   if (!depth.reentrant()) edvr::gameVramInstall(adapter);
   ```
   (The exact token `gameVramInstall(adapter` must appear in both.)
   Check that `edvr::` is the right qualification at that point in the file
   (the functions are at global scope after `}  // namespace edvr`).

When done, reply with the files you changed and anything you did differently
(in particular any API in the codebase that did not match this plan).
