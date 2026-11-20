# The slot registry overflowed on an ordinary session (2026-11-07)

## Status

- **Question:** why do a contributor's four Windows logs (RX 6800, flat, InPlace) print `VTableHook: the slot registry
  is full, so re-claiming is OFF for every hook this session` six times, right after the exposure fix installs, when
  the same build under Proton (LiveCopy) never does?
- **Answer, from the source:** `commit()` registers every patched slot of an InPlace hook in `g_slotOwners`, which held
  64 (`kMaxSlotOwners`). The d3d11 half alone patches 73 (flat) or 66 (VR) slots. LiveCopy and CopyVptr register
  nothing, and the two context hooks are 49 of the 73, so a LiveCopy session stays under 64. **vScreen's commit is the
  one that crosses 64** (running total 29 before it, so its 36th slot is the 65th); every InPlace commit after it
  prints the line again, which is the six prints.
- **Count, per hook, flat profile (VR in brackets).** Counted by reading every `replace()` call and its condition; not
  an in-session census (see "Not shown"):

  | Hook | Slots | Where |
  |---|---|---|
  | device | 19 [15] | `device_hook.cpp` 2733-2812: VS, input layout, PS + 4 flat-only (HS, DS, GS, GSSO) + CS + Texture2D + 7 creates + sampler + 2 capability queries |
  | swap chain | 3 [1] | Present, plus ResizeBuffers and ResizeBuffers1 in flat |
  | factory | 2 | CreateSwapChain, CreateSwapChainForHwnd |
  | exposure | 5 | CSSetShader, CSSetUAVs, Dispatch, DispatchIndirect, ClearState |
  | vScreen | 44 [43] | 32 fixed + PSSetConstantBuffers in flat + 11 Context1 "issue" slots |
  | graphics bridge | 0 or 1 | ExecuteCommandList, installed only when vScreen did not register as the owner |
  | DirectInput gate | up to 69 | W/A keyboard 2x2, game devices 16x2, joysticks 8x2, mouse 1, factories 8x2 |

  d3d11 half 73 [66]; with the bridge and every door array full, 143.
- **Checks the count against real logs:** the maintainer's flat Steam log (`edvr_gfx_20261107_113241.log`) prints
  `flat discover PS constant-buffer observer: ... installed=1` and `flat geometry issue reuse hooks: prefix=302 ...
  swap=1` with all eleven at 1, so vScreen's 44 are all staged; the device hook's format pair prints as hooked. Its
  install order (keyboard gate, device, exposure, vScreen) puts vScreen after 24 registrations plus the gate's, so
  the overflow lands at vScreen, as the contributor's line placement says.
- **Decision: raise the capacity to 256; keep one entry per slot.** 256 is 1.8x the worst case above, 8 KB of static
  storage, and the scan runs only for a slot already found displaced. Changing what is registered was weighed and
  dropped: an entry is needed for every slot some other hook might later share, and the hook that later shares it
  cannot see the first hook's patches without reading its private `m_patches` from another thread, which is a
  lifetime question the array does not raise. Registering fewer slots buys nothing a bigger array does not.
- **The safety is unchanged.** A registry that cannot hold every hook still sets `g_slotOwnersPoisoned`, and
  `reclaim()` still returns at its first line for every hook. Only the size moved. The line now names the table, the
  hook's slot count and how many fit.
- **Rig, `tools\vtable_test`:** `73 registrations` stages the flat shape on fake objects (device, swap chain, factory,
  exposure, vScreen on one context, ClearState last for both) and asks whether re-claiming is still on. Run on the
  unfixed code first: 5 failures (the first hook cannot re-claim a vouched slot, a slot past entry 64 is not
  re-claimed, the shared slot is not inspected), with the 73 commits themselves passing. `overflow` is the last cell
  (the overflow is permanent): a bystander that is fully registered re-claims while there is room and stops once
  other hooks overflow the registry. With the poison's early return removed, only that cell fails.
- **Not shown:** an in-session count (no log line reports slots per commit; the overflow line is the only one and
  fires only on overflow); that the contributor's rig reclaims correctly once it has room (no flight); anything about
  the draw slots the sibling branch asks about.
- **Flown 2026-11-09, inert there:** the maintainer's Steam flights on main's code (a89e193e, VR and flat) choose
  LiveCopy (`96 of 96 sampled vtable entries are inside Windows' d3d11.dll`), which registers nothing, and no November
  Steam log before or after the fix prints the full line. The InPlace case below is still unflown; `vtable_test` holds it.
- **Next flight:** on an InPlace Windows session no `slot registry is full` line, and `edvr_log.py` shows
  `no longer holds EDVR's hook` lines if anything re-points a slot. The registry is not the answer to `draws=0`; that
  stays with `docs/windows-d3d11-system-module-2026-11-07.md` (branch `claude/windows-d3d11-module-resolution-61f112`).
- **Left alone, adjacent:** `kMaxPrivateTables` is 8 and `registerPrivateTable` drops a ninth silently (four are
  used); the poison never clears, even after the hook that overflowed is gone, which is safe and permanent; the
  stale "two dozen slots" in the `slots[512]` comment at the top of `reclaim()`'s log line list.
- **Temporary keys:** none.

## Journal

### 2026-11-07: the count, the cells, the size

Read: `vtable_hook.cpp` (registry, `commit()`, `reclaim()`), every `replace()` site in `src\d3d11`, the sibling branch's
doc, the maintainer's flat Steam log for the conditional hooks.

Failing first: `vtable_test` built alone to a scratch directory with the new demand cell and the unfixed
`vtable_hook.cpp`: 278 earlier cells ok, then `a flat session's five InPlace hooks all commit (73 slots)` ok and five
FAIL lines (`re-claiming is off for the whole session`), exit 1. With 256 and the same cell: 288 ok, exit 0.

Validation: `build.bat --wait-lock` on the working tree over `4aeb712e`, all gates passed (509.4 s), `vtable_test`
among them. The first full build stopped at 25 of 193 jobs on one `gate_test` cell, `the frame-tick chain recorded and
named all three ticks on a frame where one faulted`. That binary does not link `vtable_hook.cpp`; run alone from the
repo root it passed 3 of 3, and the second full build passed it. A timing cell under the rigs' concurrent load, not
traced further. (Run from `build\` instead of the root, `gate_test` fails 18 source-pin cells that cannot find `src\`.)
Not done: a flight, `build.bat --dll-only`.

Changed: `src\common\vtable_hook.cpp` (`kMaxSlotOwners` 64 to 256 with its derivation; the full line carries the
numbers and no longer passes an argument its format never used), `vtable_hook.h` and `.cpp` (two `EDVR_VTABLE_TEST`
accessors for the overflow cell), `tools\vtable_test\vtable_test.cpp` (the two cells and their fixtures).
