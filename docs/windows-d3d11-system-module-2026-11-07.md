# The d3d11 proxy's handle to Windows' d3d11.dll (2026-11-07)

## Status

- **Question:** why does a Windows rig with EDHM ahead of EDVR log `0 of 96 sampled vtable entries are inside
  Windows' d3d11.dll`, then `draws=0 copies=0` and a flat stand-down (`no-known-output-copy`), when the same build
  under Proton logs 96 of 96 and LiveCopy?
- **Report (2026-11-07, a contributor, passed on by the maintainer):** the official
  `edvr-installer-linux-0.22.0-rc.7.zip`, not a fork. Its `build 6AED3509` is the DLL's link time stamp
  (2026-11-06 02:01:45 UTC, six minutes after the `v0.22.0-rc.7` commit `eedd5d04`), not a commit. Chain edworld,
  EDHM, EDVR: edloader is the game folder's `d3d11.dll`, EDVR sits in `edloader\plugins`, `real_dll` names edloader's
  `d3d11.dll`. The player's rig is an RX 6800 on Windows; the contributor's own is Proton, EDHM ahead of EDVR too.
- **Proven, from the report's log lines and the source:** `g_systemModule` was not System32's `d3d11.dll`. `0 of 96`
  is counted against that handle's image range; `d3d11.dll backs 96 of the 96` comes from the module owning the
  entries once that handle is excluded (`vtableDominantOtherModule`); both print only if the two differ. Slot 12
  (DrawIndexed) is `C:\Windows\system32\d3d11.dll+0x12DC30` in the same log.
- **Four player logs, each:** `VTableHook: the slot registry is full, so re-claiming is OFF for every hook this
  session`, six times, right after the exposure fix installs with 327 methods in InPlace. The Proton run (LiveCopy,
  152 methods) never prints it. In the player's logs flat map bounce reads 130k to 632k maps per 5 s and ClearState
  (slot 110) is seen, while draws stays 0 for the session.
- **What that says, read from the source:** InPlace registers every patched slot in a registry of 64
  (`kMaxSlotOwners`); LiveCopy and CopyVptr register none (`commit()` returns before it), which is why Proton never
  prints the line. A full registry sets `g_slotOwnersPoisoned`, and `reclaim()` returns at its first line, so no
  re-point is adopted and none is logged. On the player's rig reclaim therefore never ran, whatever module it was
  told implements the context: the wrong handle matters there through the mode the probe picks, not through reclaim.
  **The `no longer holds EDVR's hook` grep is not evidence in those logs: it cannot print.**
- **Not shown:** which module the handle was (nothing logged it); that a hook on `LoadLibraryExW` returned it
  (`system_d3d11.h` documents 3Dmigoto and EDHM with `load_library_redirect = 2` doing so; the Proton run has the
  same EDHM ahead of EDVR and resolves right, so the redirect alone is not the whole of it); why draws is 0 in
  InPlace. Map and ClearState hooks are seen and the draw hooks are not, which fits the runtime moving the draw slots
  (`device_hook.h`: slot 12 is the one the field data says moves) with reclaim off, but no line reads the draw slots
  without reclaim, and `advanced.vtable_flip_timeline`, which would, is not on the flat allow-list.
- **Fix, on branch `claude/windows-d3d11-module-resolution-61f112`:** `chooseSystemD3D11` (`system_d3d11.h`) checks
  the handle against the module's own file path and, when it is not System32's, finds the System32 module by a lookup of
  its full path or by walking the module list. `probeSystemModule` (`d3d11_proxy.cpp`) calls it at the first
  probe, once a device exists, and writes one `system d3d11.dll:` line saying which route was taken and what the
  DllMain handle was. With System32's module found, the probe should read about 96 of 96 and `auto` should pick
  LiveCopy, the Proton run's shape, with no registry. Rig `system_module_test`. Not flown; not run on the reporting
  rig. It does not touch the registry or the displaced draw slots.
- **To read after a flight on that rig:** the `system d3d11.dll:` line (names the substitute, if there was one);
  `context hook mode:` (expect LiveCopy, 96 of 96); no `slot registry is full` line (LiveCopy registers nothing);
  draws above 0. If the line says the handle was Windows' own, this fix is not the cause. A session without EDHM, which
  the contributor is arranging, splits edloader from EDHM's proxy as the substitute.
- **Not folded in:** InPlace overflows the 64-slot registry on this build (vScreen alone patches about 45 slots,
  with the exposure fix, device hook and input gate beside it), so any session that lands in InPlace on Windows has
  reclaim off. The loader phase still uses the unchecked handle for the export fallback in `chainThroughOtherProxy`
  and for `g_systemCreateDevice` and `g_systemCreateDeviceAndSwapChain` (the re-entrancy route).
  `advanced.context_hook_mode` is not on the flat allow-list (`flat_profile_keys.h`), so flat reads it as `off`; a
  one-off test build adds the key to that list. No temporary key was added.
- **Ruled out:** the loader alone answering a full path with a same-named module mapped first, because rig case S3
  got System32's module back with a decoy `d3d11.dll` already mapped (Windows 11 10.0.26200); a hook is needed.
  The `no longer holds EDVR's hook` grep as a test of the reclaim hypothesis on these logs, because reclaim is off
  in all four.

## Journal

### 2026-11-07: the check, and the rig

Read: `d3d11_proxy.cpp` (`loaderPhase`, `contextHookModeFor`), `proxy.cpp` (`loadRealModule`), `vtable_hook.cpp`
(`vtableEntriesInModule`, `vtableDominantOtherModule`, reclaim), `system_d3d11.h`, the flat allow-list.

Changed: `system_d3d11.h` (`mappedD3D11Modules`, `walkMappedD3D11AtPath`, `chooseSystemD3D11`;
`mappedD3D11Paths` is now built on the first), `d3d11_proxy.cpp` (`probeSystemModule`; the probe and
`systemD3D11Module()` read it), `device_hook.h` (comment), `build.bat` (the rig's block),
`tools\system_module_test`.

The rig stages a copy of `version.dll` named `d3d11.dll`, maps it first, then System32's. Run alone, on the fixed
header: S0 to S7 pass. Run on a scratch header that keeps the old behaviour (trust the handle as it came): S2, S5
and S6 fail, so the rig fails without the fix. It does not hook `LoadLibraryExW`; it stands in for the substitute
with a decoy handle.

Validation: `build.bat --wait-lock` on the working tree over `4aeb712e` (describe
`v0.22.0-rc.7-53-g4aeb712e-dirty`): all gates passed, 348.8 s, `system_module_test` and `flat_temporal_test`
among them. The first full build failed `flat_temporal_test`: a source-text pin held the old `g_systemModule`
argument of `vtableDominantOtherModule` in `d3d11_proxy.cpp`. It now holds `systemModule`, and two more pins hold the
probe to `probeSystemModule()` and the entry count to that module. Not done: a flight, a run on the reporting rig, a
`mutants.py` for the new rig, `build.bat --dll-only`.

### 2026-11-07, later: the contributor's reply

The first version of this doc named the `no longer holds EDVR's hook` grep as the test of the reclaim hypothesis. The
contributor's agent asked whether the line can print with re-claiming off. It cannot: `reclaim()` returns at
`if (g_slotOwnersPoisoned) return 0;` before the line, and all four logs carry the registry-full line. The grep and
the hypothesis it tested are retired above; the doc and the code comment's reclaim mechanism are true of a session
whose registry is not full and moot on this one. The same reply corrected the build identity (a link time stamp, not
a commit) and gave the Proton comparison, which is why the redirect is no longer offered as the whole cause.
