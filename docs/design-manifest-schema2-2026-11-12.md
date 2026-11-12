# One install engine: the schema-2 manifest  2026-11-12

## Status

- **State (2026-11-12): being implemented in three stages** on branch
  `claude/launcher-manifest-schema-6880a7`. Each stage passes the full
  build and is committed on its own.
  1. Schema 2 format: one shared parser, the Python validator, the
     packager's file table, fixtures, and the launcher's update path.
  2. One install engine: the installer embeds a schema-2 manifest, and the
     planner loops over its files.
  3. Per-id install records in `state.ini`, with migration, and removal of
     dropped files.
- **Supersedes** the 2026-11-12 draft "Launcher 0.2.0: a data-driven update
  manifest". That draft was written against an older tree:
  - It assumed an openvr proxy route, which was retired 2026-09-16.
  - It assumed an XeSS opt-in, which does not exist.
  - It assumed `xess_fg`, which is not in the tree.
  - It assumed a runtime registration step, which does not exist.
  - It assumed `ITEMS_SINCE_LAUNCHER`, which is not in the tree.
- **Decided with the maintainer (2026-11-12):**
  - Launcher 0.1.0 and schema 1 are retired. No v1 manifest is published
    alongside.
  - The installer and the launcher share one parser and one install engine.
  - `when` has two keys, `profile` and `adapter`; there is no `route` and no
    `option`.
  - `existing` has five behaviours: `replace`, `chain`, `runtime`,
    `settings` and `owned`.
  - Install records are kept per file id, starting in this change.
- **Out of scope:** the launcher updating itself. The `launcher` block stays
  so it can later.
- **Issues:** none filed yet.

## Why

In schema 1 every file is an `item` name compiled into the launcher, which
knows where it goes:

- `idFor` and `canonicalItem` in `payload.cpp`;
- the `PayloadInfo` fields;
- the per-item code in `plan.cpp`.

An unknown item fails the manifest. A new DLL therefore needs a launcher
release before any manifest may name it. In schema 2 the manifest says
where each file goes and how an existing file is treated. The launcher keeps
only a small fixed set of named behaviours.

## Schema 2

```json
{
  "schema": 2,
  "version": "0.23.0",
  "build": "...",
  "tag": "v0.23.0",
  "issued": 1794000000,
  "launcher": { "version": "0.2.0", "asset": "...", "sha256": "...", "size": 0 },
  "assets": [ { "name": "edvr-0.23.0-core.zip", "sha256": "...", "size": 0 } ],
  "files": [
    {
      "id": "graphics",
      "asset": "edvr-0.23.0-core.zip",
      "path": "d3d11.dll",
      "sha256": "...",
      "size": 0,
      "target": { "root": "game", "path": "d3d11.dll" },
      "when": { "profile": ["vr", "flat"] },
      "existing": "chain"
    }
  ]
}
```

- **`min_launcher` is gone.** The schema number is the compatibility
  contract.
- **`id`:** `[a-z0-9_]{1,32}`, unique. It is a stable name for the file
  across releases, and the install record is keyed by it.
- **`asset`, `path`, `sha256`, `size`:** where the bytes come from, as in
  schema 1.
- **`target.root`:** one of two roots:
  - `game`, the game directory;
  - `openvr`, the surveyed `Openvr\win64` folder, or its default under
    `game` when none is found.

  The draft's `settings` root has no file to carry and is left out.
- **`target.path`:** a relative path under the root, checked like a zip path
  but also for Windows names. The rule is the same in C++ and Python. A path
  is refused when it has any of the following:
  - a backslash, a colon (which covers drives and ADS), or a control or
    non-ASCII character;
  - an empty, `.` or `..` segment;
  - a segment ending in a dot or a space;
  - a reserved device name as a segment's stem (`CON`, `PRN`, `AUX`, `NUL`,
    `COM0-9`, `LPT0-9`, any case).

  After the root is resolved, the full path must still lie inside the root:
  the planner refuses a file whose resolved path leaves its root or passes
  through a junction or symbolic link below the root (the root itself may be
  one), for manifest targets and for install records alike.

  Reserved targets are refused too, case-folded, in either root:
  - under the `game` root, a path under `openvr/` (the two roots must not
    alias);
  - the fixed steps' own names: `edvr_profile.ini`, `edvr_openxr.ini`,
    `openvr_api_orig.dll`, any `d3d11_*.dll` (the names a chained DLL is
    parked under), all at the top level of a root;
  - any file whose name ends in `.base`;
  - anything under `edvr_install/` or `edvr_backup/`.
- **`when` (optional; absent means always):** every key present must match.
  - `profile`: a non-empty list of distinct values from `vr` and `flat`. It
    matches when it shares a value with the profiles being installed. The
    installer's scope maps to profiles: `both` to vr and flat, `vr` to vr,
    `flat` to flat.
  - `adapter`: `nvidia`. It matches when the survey found an NVIDIA adapter
    (today's DLSS gate).

  An empty `when`, an unknown key, or an unknown value is a schema error,
  not a skip.
- **`existing`:** how a file already at the target is treated. Every
  behaviour that overwrites backs up first.

  | Value | Behaviour |
  |---|---|
  | `replace` | back up whatever is there, then write |
  | `owned` | write when absent or when the record shows EDVR placed the copy there; leave a foreign copy alone and say so (today's DLSS and XeSS rule) |
  | `chain` | if a foreign DLL holds the target, move it to its chain name and record the chain in the vr settings file; otherwise as `replace` (today's graphics chaining) |
  | `runtime` | as `replace`, and keep the game's genuine OpenVR runtime as `openvr_api_orig.dll` for restore, adopting the tool's original when needed (today's runtime rule) |
  | `settings` | today's settings handling: three-way merge against `<target>.base`, fresh write when absent, replace when settings are not kept; a flat-only settings file is seeded from the vr one |

- **Constraints on the behaviours:** `chain` and `runtime` are not general.
  The planner's chain rules, the runtime's restore rules and the startup
  config are written for one file each, so the schema pins them to today's
  names. `replace`, `owned` and `settings` work for any legal target.
  - At most one `chain` file, and its target is `game`: `d3d11.dll`.
  - At most one `runtime` file, and its target is `openvr`:
    `openvr_api.dll`.
  - At most one `settings` file per profile.
  - A `settings` file's `profile` is exactly `["vr"]` or exactly `["flat"]`.
- **Fixed steps:** files that are not payload bytes stay fixed steps in the
  planner:
  - the profile descriptor `edvr_profile.ini`;
  - `edvr_openxr.ini`;
  - the `.base` copies.

  Runtime retirement on a flat install also stays fixed.
- **Strictness:**
  - An unknown key anywhere is an error.
  - A duplicate `id` is an error.
  - Two files with the same root and case-folded target path are an error
    when their `when` conditions can both hold.

  A new field, key, value or behaviour means a new schema number.

## Schema versioning

- The launcher reads exactly one schema, `kManifestSchema = 2` in
  `install_manifest.h`. Python has `MANIFEST_SCHEMA = 2` in
  `sign_manifest.py`, and its self-test reads the header as text and pins the
  two together.
- The checks run in this order:
  1. size;
  2. signature;
  3. ASCII;
  4. JSON;
  5. then `schema` is read on its own, before any shape check.

  The outcomes are:
  - **Higher than 2:** the update is refused with
    "This update needs a newer ED-UP launcher. Download the latest launcher
    from the releases page." The log line reads
    `manifest schema N, launcher reads 2`. Nothing is downloaded or written.
  - **Lower, missing, or not an integer:** refused as malformed.
- Bumping the schema is a deliberate act:
  - change both constants;
  - add fixtures;
  - release a launcher before the first manifest of the new schema.

## One engine

- **`src/installer/install_manifest.{h,cpp}` (new):**
  - It holds the schema-2 structs, the parser and the path rule.
  - It evaluates `when`, resolves `target`, and runs the overlap check.
  - Both the installer and the launcher link it.
  - `update_manifest.cpp` keeps the size, signature and ASCII checks,
    `compareVersions`, and the call into the shared parser.
- **Payload:**
  - A payload is the parsed manifest, with each file's bytes and verified
    hash.
  - The installer embeds the manifest as `RCDATA` 100, with no signature,
    since the exe is the unit of trust. Each file is `RCDATA` `101 + its
    index in files[]`.
  - The launcher maps the content-addressed cache files instead.
  - `--payload <dir>` reads `<dir>\ed-up-manifest.json` and takes each file
    from its `path` under the folder.
  - `idFor`, `canonicalItem`, the per-item fields of `PayloadInfo`, and the
    resource ids 101-111 are removed.
- **Planner:** `planInstall` keeps its gates, its survey, and the fixed
  steps. Payload files are a loop: evaluate `when`, resolve `target`, then
  apply `existing`.
- **Packager:**
  - `tools/package_native.py` has one table: `id`, source, zip group,
    target, `when` and `existing`. It is the only place a new DLL is
    declared.
  - `gen_installer_rc.py` writes the embedded manifest from the same table.
  - `MIN_LAUNCHER` is removed.

## Install records

- `state.ini` gains one `[file.<id>]` section per file placed. Each section
  holds:
  - `root` and `path`;
  - `existing`;
  - `sha256` of what was written;
  - `before_sha256` and `backup`, when a file was displaced.
- **Uninstall** removes a recorded file only when its hash still matches.
  An edited copy is kept, and the plan says so.
- **Update:** a recorded `id` that the new install does not place is removed
  under the same rule.
- **Migration:** a `state.ini` without file sections, written by installer
  0.22 or launcher 0.1.0, is read into records from its component fields.
  The planner then works from records only.

## Tests

- **Shape fixtures:** `tools/launcher_fixtures.py` cases cover:
  - each `existing` value and each `when` key;
  - every refused path form (`..`, absolute, `C:`, `NUL`, `con.txt`, ADS,
    trailing dot);
  - a duplicate id;
  - an overlapping target;
  - an unknown key;
  - schema 1, schema 3 and a missing schema.

  The Python validator's verdict is pinned, and the C++ parser must agree.
- **Schema 3:** refused before any download, with the update-the-launcher
  line. Nothing is written, and `--dry-run` writes nothing.
- **Dropped file:** removed on update only when the record and hash match;
  an edited copy is kept.
- **Packager:** `package_native.py --self-test` checks that every table entry
  resolves to a legal target, and that the schema constants match.
- **Parity:** the existing `installer_test` planner cases keep their
  expectations. Only how their payload is built changes.
