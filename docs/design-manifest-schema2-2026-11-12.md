# One install engine: the schema-2 manifest  2026-11-12

## Status

- **State (2026-11-14): releases publish a schema-1 bridge.** Launcher 0.1.0
  shipped in v0.22.0 and reads only `ed-up-manifest.json` at schema 1. Each
  release carries `ed-up-manifest-v2.json` (schema 2, the only one 0.2.0 and
  later fetch) and the bridge `ed-up-manifest.json` (2026-11-14 entry). The
  bridge stays until a decision retires it; not flown against a 0.1.0 launcher.
- **State (2026-11-13): stages 1, 2 and 3 done, plus the review fixes.** Branch
  `claude/launcher-manifest-schema-6880a7`; each stage passed the full build
  and is its own commit: (1) the schema-2 format, parser, validator, packager
  table and fixtures; (2) one install engine, the planner looping over the
  manifest's files; (3) per-id install records with migration and removal of
  dropped files.
- **Stage 2, as built:** a payload is `PayloadInfo` (the manifest's files that
  have bytes, with hash and `valid`, `plan.h`), built by `payload.cpp` from
  RCDATA 100 and 101 + i or from mapped cache files. `planInstall` runs one
  handler per behaviour in the order chain, owned, runtime, replace, settings;
  another profile's file is skipped, and an adapter miss skips all but `owned`.
  A target the survey has no field for is read from disk. Backups keep the file
  name; a colliding one goes under its folder.
- **Stage 3, as built:**
  - `state.ini` has one `[file.<id>]` section per placed file (`FileRecord`,
    `state.h`): root, path, existing, sha256, and before_sha256 and backup
    when a file was displaced. `serializeState` still writes the old per-file
    sections, derived from the records, for a downgrade and for
    `install_edvr.py`'s flat-receipt check.
  - Migration: a `state.ini` with no file sections is read into records by
    `kLegacySlots` (`state.cpp`). Not recoverable: the foreign file a record
    displaced (except the runtime's original), and the second settings
    file's digest of a both install. A record with an unknown root or a path
    failing `isTargetPath` is refused on load and in the planner.
  - Uninstall and update work from `removalCandidates` (`plan.cpp`): a
    recorded file goes only while its disk hash is the record's (or the tool
    receipt's, or the payload's bytes); an edited copy stays with a note.
  - Still id-specific: `kBlurbs` (report words, and which file travels with
    another), the startup config's paths and ownership, the native-image
    validity checks, and `kLegacySlots` (migration only).
  - `planUninstall` takes an optional payload: with none it works from
    records and receipts alone; given one it also vouches for payload files
    in a folder whose record was lost. Repair is unchanged.
- **Review fixes (2026-11-13), as built:**
  - An update never removes a recorded `owned` file its payload lacks (a build
    without the SDK omits DLSS and XeSS); uninstall still removes it by hash.
  - Planner: `MakeDir` for a missing parent; a target stays inside its root
    with no junction or link below it (records too); the old copy of an id
    placed at a new target is a removal candidate; a recorded file whose disk
    hash is the payload's bytes is ours (flat conversion); a dropped or moved
    id kept for an edit has its own note and never blocks.
  - Schema (C++ and Python, fixtures pin both): `chain` is `game:d3d11.dll`,
    `runtime` is `openvr:openvr_api.dll`; reserved targets are refused. The
    shape corpus is 280 cases (52 accepted, 228 refused).
- **Second review (2026-11-13), as built:** a placed path is never a removal
  candidate and Delete re-checks its hash; the Python validator takes the
  parser's integer range; `--payload` files are hash-checked; the XeSS notices
  go with the runtime; one `targetAllowed` check serves files and records.
- **Decided with the maintainer (2026-11-12), changed 2026-11-14:** one parser
  and one engine; install records are per file id. "No v1 manifest alongside"
  assumed 0.1.0 was never released; it was, hence the bridge. Supersedes the
  2026-11-12 draft "Launcher 0.2.0". No issues filed yet.

## Why

In schema 1 every file is an `item` name compiled into the launcher, which
knows where it goes:

- `idFor` and `canonicalItem` in `payload.cpp`;
- the `PayloadInfo` fields;
- the per-item code in `plan.cpp`.

An unknown item fails the manifest. A new DLL therefore needs a launcher
release before any manifest may name it. In schema 2 the manifest says where
each file goes and how an existing file is treated. The launcher keeps only a
small fixed set of named behaviours.

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

- **`min_launcher` is gone.** The schema number is the compatibility contract.
- **`id`:** `[a-z0-9_]{1,32}`, unique. It is a stable name for the file across
  releases, and the install record is keyed by it.
- **`asset`, `path`, `sha256`, `size`:** where the bytes come from, as in
  schema 1.
- **`target.root`:** one of two roots:
  - `game`, the game directory;
  - `openvr`, the surveyed `Openvr\win64` folder, or its default under
    `game` when none is found.

  The draft's `settings` root has no file to carry and is left out.
- **`target.path`:** a relative path under the root, checked like a zip path
  but also for Windows names. The rule is the same in C++ and Python. A path is
  refused when it has any of the following:
  - a backslash, a colon (which covers drives and ADS), or a control or
    non-ASCII character;
  - an empty, `.` or `..` segment;
  - a segment ending in a dot or a space;
  - a reserved device name as a segment's stem (`CON`, `PRN`, `AUX`, `NUL`,
    `COM0-9`, `LPT0-9`, any case).

  After the root is resolved, the full path must still lie inside the root: the
  planner refuses a file whose resolved path leaves its root or passes through
  a junction or symbolic link below the root (the root itself may be one), for
  manifest targets and for install records alike.

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

  An empty `when`, an unknown key, or an unknown value is a schema error, not a
  skip.
- **`existing`:** how a file already at the target is treated. Every behaviour
  that overwrites backs up first.

  | Value | Behaviour |
  |---|---|
  | `replace` | back up whatever is there, then write |
  | `owned` | write when absent or when the record shows EDVR placed the copy there; leave a foreign copy alone and say so (today's DLSS and XeSS rule) |
  | `chain` | if a foreign DLL holds the target, move it to its chain name and record the chain in the vr settings file; otherwise as `replace` (today's graphics chaining) |
  | `runtime` | as `replace`, and keep the game's genuine OpenVR runtime as `openvr_api_orig.dll` for restore, adopting the tool's original when needed (today's runtime rule) |
  | `settings` | today's settings handling: three-way merge against `<target>.base`, fresh write when absent, replace when settings are not kept; a flat-only settings file is seeded from the vr one |

- **Constraints on the behaviours:** `chain` and `runtime` are not general. The
  planner's chain rules, the runtime's restore rules and the startup config are
  written for one file each, so the schema pins them to today's names.
  `replace`, `owned` and `settings` work for any legal target.
  - At most one `chain` file, and its target is `game`: `d3d11.dll`.
  - At most one `runtime` file, and its target is `openvr`:
    `openvr_api.dll`.
  - At most one `settings` file per profile.
  - A `settings` file's `profile` is exactly `["vr"]` or exactly `["flat"]`,
    and it targets game `edvr.ini` for `["vr"]`, game `edvr-flat.ini` for
    `["flat"]`.
  - Install records are held to the same target check: path rule, reserved
    names, and the chain, runtime and settings pins by behaviour.
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
  `install_manifest.h`. Python has `MANIFEST_SCHEMA = 2` in `sign_manifest.py`,
  and its self-test reads the header as text and pins the two together.
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
- After the upgrade a stored schema-1 manifest is not loadable, and the
  launcher has no current manifest until a schema-2 release is adopted.
  Launcher 0.1.0 shipped in v0.22.0 and reads schema 1 only; the bridge below
  is how it reaches a launcher that reads schema 2.
- A schema-3 manifest that fails strict JSON parsing (an integer over 2^63-1,
  or nesting deeper than `kJsonMaxDepth`) is reported as invalid JSON rather
  than as needing a newer launcher. Nothing downloads either way. Future
  schemas should stay within today's JSON limits.
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
  - `--payload <dir>` reads `<dir>\ed-up-manifest-v2.json` and takes each file
    from its `path` under the folder.
  - `idFor`, `canonicalItem`, the per-item fields of `PayloadInfo`, and the
    resource ids 101-111 are removed.
- **Planner:** `planInstall` keeps its gates, its survey, and the fixed steps.
  Payload files are a loop: evaluate `when`, resolve `target`, then apply
  `existing`.
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
- **Uninstall** removes a recorded file only when its hash still matches. An
  edited copy is kept, and the plan says so.
- **Update:** a recorded `id` that the new install does not place is removed
  under the same rule (replace behaviour: backed up first). So is the old copy
  of an id placed at a new target. An edited copy is kept, with its own note,
  and blocks nothing. A recorded `owned` file (a vendor runtime) that the
  payload does not carry is never removed by an update: a build without the SDK
  leaves those ids out of its manifest, and the earlier copies stay where they
  are, recorded. Only an uninstall removes them, by the hash rule. A recorded
  file also counts as ours when its disk hash equals the payload's own bytes
  for its id.
- `before_sha256` and `backup` record what each placement displaced. Uninstall
  restores only the chained graphics DLL and the runtime's original, as before;
  other displaced files stay in the backup folder. The fields are provenance
  for a later restore feature.
- **Migration:** a `state.ini` without file sections, written by installer 0.22
  or launcher 0.1.0, is read into records from its component fields. The
  planner then works from records only.

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
- **Schema 3:** refused before any download, with the update-the-launcher line.
  Nothing is written, and `--dry-run` writes nothing.
- **Dropped file:** removed on update only when the record and hash match; an
  edited copy is kept.
- **Packager:** `package_native.py --self-test` checks that every table entry
  resolves to a legal target, and that the schema constants match.
- **Parity:** the existing `installer_test` planner cases keep their
  expectations. Only how their payload is built changes.

## 2026-11-14: the schema-1 bridge

v0.22.0 shipped `ed-up-launcher.exe` 0.1.0. It downloads `ed-up-manifest.json`
and its `.sig` (names fixed in `updater.h`) and its parser accepts only
`"schema": 1`. Main's 0.2.0 reads schema 2, so under the old name every 0.1.0
launcher would refuse the next release and be stranded.

What 0.1.0 can do: when the verified manifest's `min_launcher` is newer than
0.1.0 and the manifest has a `launcher` block newer than 0.1.0 and at least
`min_launcher`, it reports an update with the launcher itself as the thing to
update, downloads `launcher.asset`, checks its size and sha256, swaps itself
and restarts with `--updated`. 0.2.0 still handles `--updated` and migrates
0.1.0's `state.ini`.

Every release now carries two signed manifests:

- **`ed-up-manifest-v2.json` and `.sig`:** the schema-2 manifest, as before,
  under a new name. `kManifestAssetName` and `kSignatureAssetName` in
  `updater.h` name it, and `--payload <dir>` reads the same name. 0.2.0 and
  later fetch only this pair.
- **`ed-up-manifest.json` and `.sig`:** a schema-1 bridge, valid under the
  v0.22.0 rules (`tools/sign_manifest.py` `validate_manifest_v1`, ported rule
  for rule from that release). Its `min_launcher` is the launcher version this
  build packages, and its `launcher` block names `ed-up-launcher.exe` with that
  version, hash and size, so 0.1.0 never installs from it and only replaces
  itself. Its `assets` and `files` are still true: each schema-2 file that has
  a schema-1 item (`BRIDGE_ITEM_OF_FILE`) is listed with the same zip, path,
  hash and size, and a file with none (a licence notice) is left out. Tag,
  version, build and `issued` equal the schema-2 manifest's.

`package_native.py` writes both, unsigned, and refuses to write a bridge that
fails `validate_bridge` or disagrees with the schema-2 manifest
(`bridge_agreement`). `sign_manifest.py sign` takes either file, by its schema,
and `verify` requires and verifies both pairs and their agreement.
`release.yml` checks both manifests are in `SHA256SUMS` and uploads both.

Ruled out: publishing schema 2 under the old name (0.1.0 would refuse it), and
a bridge whose `min_launcher` is the old 0.1.0 (0.1.0 would then install from
it, and a schema-1 manifest cannot carry what the schema-2 engine installs).

Open: the bridge is kept until the maintainer decides that no 0.1.0 launcher is
worth stranding. Not flown: a 0.1.0 launcher against a signed bridge.
