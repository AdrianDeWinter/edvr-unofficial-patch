# Diagnosis, investigation docs, configs, and C++ checks

Consult this guide when investigating rendering or runtime bugs, writing or
updating `docs\`, touching `edup.ini` / `edup-flat.ini` or config keys, or
editing `src\`.

## Diagnosis discipline

- **Prove the hypothesis before editing code.** State the hypothesis and name
  the specific log line, draw call, counter, or struct field that will confirm
  or refute it, then collect that evidence first.
- **Verify the build on every flight log first.** Run `python tools\edup_log.py
  --target steam --expect-build HEAD` (or `--expect-build <branch commit>` when
  checking a branch flight from `main`); exit code 2 means a stale DLL, and a
  log from a stale DLL is not evidence.
- **Record refuted hypotheses immediately.** Add a one-line `ruled out: X,
  because Y` entry to the arc's investigation doc under `docs\` before
  proposing the next hypothesis, so multi-session arcs do not re-test dead
  ideas.
- **Fix root causes, not symptoms.** Address geometry, motion-vector, or timing
  bugs directly rather than masking them with sharpening, contrast tweaks, dead
  bands, or threshold nudges (sharpening over DLSS blur was once proposed and
  rejected because the motion vectors were wrong).
- **Instrument competing causes together.** When several causes are plausible,
  list each with a discriminating log signature and instrument them in one
  build so a single test flight eliminates multiple candidates.
- **State the rendering environment every fix depends on:** VR runtime (OpenVR
  / Oculus LibOVR / OpenXR), headset and per-eye render size, upscaler / DLSS
  version, and any fixed-size internal table (a supporter running Elite's
  Oculus native SDK left four OpenVR-side fixes inert with nothing saying so).

## Investigation docs and work tracking

- **Start every `docs\` investigation with a `## Status` block of at most 60
  lines** (`tools\check_status_blocks.py` gates the limit): current state, open
  hypotheses, ruled-out pointer, temporary investigation keys, and next flight.
  Read `## Status` first and enter the journal only where it points; update it
  whenever the doc changes, and add one to any investigation doc that lacks it.
- **Keep docs compact.** Append a dated entry to the arc's existing doc instead
  of opening another review file; a standalone review stays under about 15 KB.
- **Point memory topic files at the doc**; keep journals in `docs\` rather than
  mirroring them in memory.
- **Track persistent work in GitHub issues** on
  `characterecho-sean/ed-unofficial-patch` (`gh issue list`). File follow-ups,
  review items, and adjacent bugs as issues, and link them in commits and PRs
  (`(#97)`, `Closes #97`). Only single-handoff plans or task lists stay as
  files under `.agents\`.

## Config and INI files

- **Edit live INIs directly and diff.** Read `edup.ini` or `edup-flat.ini`,
  apply a targeted edit, and diff before and after — never rewrite a live INI
  with a regex or PowerShell anchor (a bad anchor previously corrupted Sean's
  working config).
- **Respect profile and mirror ownership** (`src\common\ini_name.h`,
  `src\common\mirror_location.h`, `tools\install_edup.py`):
  - `edup.ini` in the game directory holds whoever flew last; `install_edup.py`
    leaves an existing one untouched unless `--ini` is passed (and backs it up
    before writing). If missing, it restores from the newest copy in
    `%LOCALAPPDATA%\EDUP\<leaf>-<store>\` (falling back read-only to legacy
    `%LOCALAPPDATA%\EDVR\` on un-migrated installs), or writes the shipped
    default.
  - `edup-flat.ini` is the flat profile's own file: while the flat profile is
    active, ED-UP reads `edup-flat.ini` first and falls back to `edup.ini`
    (with legacy `edvr.ini` / `edvr-flat.ini` read fallback before migration).
    `--scope flat --ini` writes `edup-flat.ini` only, `--scope both --ini`
    writes both, and any scope including flat seeds a missing `edup-flat.ini`
    from `edup.ini`.
  - Settings mirror to `%LOCALAPPDATA%\EDUP\<leaf>-<store>\` so game updates
    that wipe the install folder cannot destroy user settings.
- **Name config values for functionality, not mechanism.** Expose what the user
  gets (`on`, `off`, `auto`), not the internal technique name.
- **Ship only keys a user would set.** Any A/B switch, diagnostic toggle, or
  tuning knob added during an investigation is temporary: list it in the arc's
  `## Status` block, and when the arc closes, ship the winning behaviour
  directly and propose removing the temporary key (quoting the exact key and
  current behaviour first — three separate features share "witchspace", and
  only `fix.witchspace_stars` touches explosions). A fix that always helps gets
  no toggle.

## C++ and instrument checks

- **Re-read the changed region after editing** for declaration order,
  duplicated census or log strings, and shader entry-point name collisions that
  would silently disable an instrument.
- **Name distance/range identifiers `nearZ`, `farPlane`, or `outOfRange` —
  never `near` or `far`.** `windows.h` defines both as empty macros, turning
  `Case far = spot;` into `Case = spot;` with a misleading error.
- **Trace every new instrument end to end before flying it.** Verify that no
  early return shadows the probe and no finished flag flips before the report
  logs. Confirm that "code never ran" produces an output clearly
  distinguishable from "code ran and succeeded".
