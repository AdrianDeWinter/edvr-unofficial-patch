# Eye dump size (2026-11-10)

## Status

- **State:** the no-loss set is built on branch `claude/eye-dump-file-size-407eb5`.
  - Every `.bin` the dump key writes to `edvr_logs\eyes\` and `edvr_logs\pool\` is now `NAME.bin.z`, zlib level 1 in 4 MiB chunks (miniz 3.1.2).
  - `P##.bmp` is stored as an XOR delta against `T##.bmp`.
  - The overview pair is stored the same way: `L0.bmp` is written first and stays plain, and `FinalCrisp_LOverview.bmp` is stored as an XOR delta against it. Whichever of the two is written second becomes the delta.
  - If compression fails, the file is written plain under its original name.
  - Every press logs three `dump compression` lines: one for the run, one for the FinalCrisp run and one for the ledger.
  - `tools\dump_io.py` reads both forms, and `--expand` restores the plain files. NOT FLOWN.
- **Expected:** one press goes from about 3.7 GB to about 1.2 GB. That estimate comes from compressing the 2026-11-09 16:45:15 dump offline (the tables below), not from a flight.
- **Next flight:** press the dump key once on foot with AA on. Then:
  - Read the run's and the ledger's compression summary lines (files, MB raw against MB written, deltas, plain fallbacks, wall time).
  - Compare the hitch with a press on the build before, from the `monitor: long frame` lines.
  - Run `character_motion_check.py` and `eye_decisions.py` on the new dump.
- **Not done (decisions for Sean, measured below):**
  - Cuts of data nothing reads: the marker crops, JumpLattice, the FinalCrisp crops, and the body snapshot cap.
  - PNG for the remaining BMPs.
  - Pruning old dumps. About 98 GB was on disk across the Steam and Frontier installs on 2026-11-10, and nothing deletes them.

## The measured dump (2026-11-09 16:45:15, Steam, build a0ac939b)

- The render was 2016x2007 a eye, and the DLSS output 4032x4014 (2x).
- The eye files came to 2,426 MB in 187 files, and the pool ledger to 1,244 MB in 130 files.
- The crops: `C##` is 1400 square of the input (`kEyeCrop`). `T##` and `P##` are that crop scaled to the output (2800 square). `D##.bin` is 1400 square of RGBA32F.

Eye files:

| Kind | MB | zlib-1 ratio | Read by |
|---|---|---|---|
| `D##.bin` x16 | 478 | 2.7x | eye_decisions, character_motion_check, layer_fixtures |
| `P##.bmp` x16 | 359 | 2.4x; as XOR against T: 45x | eye_decisions |
| `T##.bmp` x16 | 359 | 2.4x | eye_decisions |
| `JumpLattice.bin` | 309 | 4.5x | eye_jump_lattice (validator only) |
| marker crops x64 (F53/F59, `fix.native_on_foot_marker_probe`) | 192 | 2.3x | none |
| `FinalCrisp_L/R##.bmp` x32 | 179 | 2.3x | none |
| `FinalCrisp_L/ROverview.bmp` | 93 | 2.3x | none |
| `C##.bmp` x16 | 90 | 1.9x | eye_decisions, character_motion_check |
| `L0.bmp` | 46 | 2.3x; as XOR against FinalCrisp_LOverview: 46x | none |
| SkinE, DlssBeforeUi `.bin` | 62 + 62 | 13x, 2.9x | character_motion_check; none |
| HoloCoverage, HoloContribution, UiPrevious, UI, Bias `.bin` | 31+31+15+4+4 | 229x | none |
| EngineSlots, SceneZ, MV, Z, PrevZ `.bin` | 31+31+15+15+15 | 19x, 5x, 1.7x, 2.4x, 2.4x | various |

Pool ledger:

| Kind | MB | zlib-1 ratio |
|---|---|---|
| `drawstate_*.eyemesh.bin` (32 body snapshots, 768 MiB cap) | 628 | 8.5x |
| `candidates_*.eyemesh.bin` | 115 | 4.6x |
| `bones*` (20 frames x 2) | 120 | 4.2x |
| `menu_*.eyemesh.bin` | 90 | 11.4x |
| `pool_*` (20 frames) | 66 | 22x |
| `classification_*.json` / `.bin` | 64 / 35 | 16x / 11x |
| `skin_*`, `drawstate_*.bin`, `tonemap_*`, `draws_*` | 47+31+30+7 | 7.5x, 12.6x, 2.1x, 7.2x |

Image overlap:

| Pair | Pixels identical |
|---|---|
| P00 / T00 | 97.5% |
| L0 / FinalCrisp_LOverview | 97.1% |
| L0 centre 2800 square / T00 | 100% |
| consecutive T frames | 20% |
| consecutive C frames | 27% |

Speed: zlib level 1 ran at 70-130 MB/s a thread on the images and 270-900 MB/s on the sparse and ledger files. That is why the writer spreads chunks over worker threads instead of compressing inline.

## Cuts not taken (2026-11-10)

- **Marker crops (192 MB):** these come from a TEMPORARY probe and nothing reads them. Turning off `fix.native_on_foot_marker_probe` removes them without a code change.
- **JumpLattice (309 MB raw):** only its validator reads it. Its arc, the Holo-Me head flick, was fixed on main on 2026-11-05.
- **FinalCrisp crops (179 MB):** nothing reads them. They were last cited for #200, which closed on 2026-11-09.
- **Body snapshot (628 MB raw):** only `eye_draw_snapshot.py`'s general-purpose reader and the small geometry section in `cull_gate_probe.py` read it. It was added for the F82/F83 on-foot pose evidence (2026-10-17). It may still be read by hand.
- **`kEyeRun = 16`, `kEyeCrop = 1400`:** the 16-frame span is the 0.75 deg baseline chosen on 2026-09-09. The 1400 crop already covers 69% of a 2016-wide eye.
