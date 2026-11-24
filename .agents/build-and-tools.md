# Build, validation, and sanctioned tools

Consult this guide when running builds, installing to a game directory, reading
flight logs, managing worktrees, posting PR comments, or adding or editing
`tools\*.py`. For maintainer-only signing, credits, PowerShell 5.1 quirks, and
release tagging, read [maintainer.md](maintainer.md).

## The sanctioned tools

Every operation below goes through `tools\`, and only through `tools\`. Each
replaced an ad-hoc one-liner that left inconsistent backups or corrupted
configs; extend the script instead of regenerating a one-liner.

| Operation | Use | Never |
|---|---|---|
| Install a build next to the game | `python tools\install_edup.py` | `Copy-Item` / `cp` onto the game directory |
| Verify the install matches the build | `python tools\install_edup.py --verify-only` | eyeballing timestamps |
| Find and read a flight log | `python tools\edup_log.py` (census aggregate: `--tally vh [--frame N]`; first command on an on-foot flight: `--health [--since HH:MM:SS] [--until HH:MM:SS]`, reporting which tagged features ran/applied and exiting 3 on any `NEVER APPLIED`) | tailing and grepping the log by hand |
| Measure skinned character motion error in an eye dump | `python tools\character_motion_check.py --target frontier [--stamp S]` (face, named draw, and body against raw input; exits 3 past limits) | retyping a scratch warp script |
| Read an on-foot recording (`log.on_foot_detail` on; F9 marks a moment) | `python tools\onfoot_check.py --target <game dir> --expect-build HEAD [--mark N]` | reading 1 s on-foot lines by hand |
| Reflow release notes or docs | `python tools\reflow_notes.py` | reflowing by hand or regex |
| Try the launcher without touching real folders | `ed-up-launcher.exe --data-root <scratch folder>` (with `--check-update`, `--update`, or `--channel rc`) | running it bare, which copies and registers itself on the machine |
| Make a worktree that can build | `python tools\worktree.py add <path>` | `git worktree add` alone (lacks linked deps) |
| Post PR replies, a review, or a summary | `python tools\pr_post.py --pr N --since <time> ...` (re-fetch PR reviews and comments right before posting so newly landed reviews are answered — PR #92) | hand-built `gh api` / `gh pr comment` / `gh pr review` calls |
| Compile on Linux for a test flight | `python3 tools/linux_toolchain.py build-dlls` | running `build.bat` under Wine by hand |
| Archive the game exe before Steam updates it (`install_edup.py` also does this; then see `docs\game-binary-map.md`, "Re-deriving after a patch") | `python tools\archive_game_exe.py --target steam` (`--dry-run` first) | copying the exe by hand or committing it |

Maintainer-only release tools (`release_credits.py`, `sign_manifest.py`,
`artifact_signing.py`) live in [maintainer.md](maintainer.md).

Common install and log checks:

```bash
python tools\install_edup.py --target steam --dry-run
python tools\install_edup.py --target steam
python tools\install_edup.py --target steam --scope flat --xess
python tools\install_edup.py --target steam --scope flat --verify-only
python tools\edup_log.py --target steam --expect-build HEAD --version
```

### Tool gates and worktree safety

- **Mechanical enforcement:** `tools\check_tool_conventions.py` fails the build
  if a `tools\*.py` CLI lacks `--self-test`, is ungated in `build.bat`, or
  reads the Steam registry outside `steam_library.py`. Self-tests on
  file-writing tools assert `--dry-run` writes nothing, so parser or write-path
  drift fails in the build rather than after a flight.
- **Worktree stash isolation:** commit work in progress to a temporary branch
  or patch rather than running bare `git stash` / `git stash pop` — worktrees
  (including `.claude\worktrees\` and `.Codex\worktrees\`) share one stash
  stack with the main checkout.

## Build and verify before commit

### Native Windows (full local validation)

- **Launch `build.bat --wait-lock` by absolute path** before committing C++
  changes. A `cmd` invocation that fails to start the batch still exits 0 and
  can look like a green build. The GitHub workflow is optional there for
  ordinary commits.
- **One build at a time on a machine.** `build.bat --wait-lock` waits for any
  active build and takes the lock atomically; a bare `build.bat` exits 1 if
  another build holds the lock. Run it as one command rather than
  `build_lock.py --wait` followed by `build.bat` (another build can take the
  lock between them).
- **Normal `build.bat` is the validation build.** It runs the Python self-tests
  and mechanical gates (`tools\check_*.py`), compiles the production DLLs, runs
  the test rigs (including building and checking the launcher), and writes
  `build\full_build_receipt.json` only after every gate passes. Read the tail
  of the output; green means every gate ran and passed.
- **Compile cache.** Compiles go through `tools\compile_cache.py`, skipping
  units whose sources, project headers, flags, toolchain, and `INCLUDE` are
  unchanged and printing a `compile_cache:` summary (compiled / reused, and
  reasons for the first misses). Cache hits do not replay compiler warnings.
  Use `build.bat --clean` or `EDUP_NO_COMPILE_CACHE=1` to force full
  recompiles. System and SDK headers are keyed by `INCLUDE` path and compiler
  identity, not header contents.
- **Clean-version DLL promotion (`--dll-only`).** After a green full build,
  commit the exact validated source tree, then launch `build.bat --dll-only` by
  absolute path to rebuild and validate the production DLLs with the clean
  commit's version while skipping the test rigs and launcher build. It requires
  a clean working tree and a matching `build\full_build_receipt.json`; it is a
  promotion step, never a substitute for validating changed source. If the
  receipt check fails, rerun the full build. `tools\build_receipt.py` is the
  single source of truth for fingerprinted inputs (`DEPENDENCY_ROOTS`, tracked
  files, untracked non-ignored files, and excluded paths such as `docs\` and
  `reviews\`).

### Non-Windows hosts (GitHub Actions validation)

1. Commit and push the candidate branch so Actions validates that exact commit.
2. `.github/workflows/release.yml` runs automatically on every pull request and
   every push to `main` or `codex/release-*`. For a maintainer branch with none
   of those, dispatch it with `gh workflow run release.yml --ref <branch>`.
3. Find the run with `gh run list --workflow release.yml --branch <branch>`,
   confirm `headSha` matches the candidate commit with `gh run view <run-id>
   --json headSha`, and require `gh run watch <run-id> --exit-status` to pass
   before merging or releasing.
4. On failure, inspect the full job log and
   `edvr-diagnostics-<run-id>-<attempt>`. On success, inspect
   `edvr-build-<run-id>-<attempt>` and `edvr-packages-<run-id>-<attempt>`.
   Branch proof packages use a runner-local CI tag (or an exact existing tag)
   and are not release assets. See [docs/building.md](../docs/building.md) for
   download and checksum commands.

### Linux compile-only builds and Proton test flights

`tools/linux_toolchain.py` runs `build.bat --compile-only` under Wine with the
same MSVC and Windows SDK as CI and writes production DLLs to
`<root>/build/compile-build` (for the main checkout or a `--root <worktree>`).
It skips all gates, so it is neither validation nor a release asset; the full
`build.bat` currently fails Wine's process/window gates (leave those gates
intact).

- **One-time host setup** (run with host `python3`, no wrapper): install `wine
  msitools git`, run `python3 tools/linux_toolchain.py setup --dry-run`, then
  `setup --accept-license` only after the user accepts the VS 2022 Build Tools
  licence. Binaries, the Wine prefix, and SDKs live in the git-ignored `<main
  checkout>/.toolchains/windows/` and are never shared; `doctor` verifies them.
- **Test flight sequence (Steam Deck / Proton).** For a build Sean will fly, CI
  first is not required; CI is for a merge or release. Commit to the branch
  first — a dirty tree builds as `vX.Y.Z-dirty`, which `edup_log.py
  --expect-build HEAD` rejects. Close Elite, then run under host `python3` (use
  host `python3` rather than Wine Python, which cannot see Proton's
  `EliteDangerous6*` processes in `/proc` and passes the running-game check
  vacuously):

```bash
python3 tools/linux_toolchain.py build-dlls
python3 tools/install_edup.py --target steam --scope flat --xess --compile-only --dry-run
python3 tools/install_edup.py --target steam --scope flat --xess --compile-only
python3 tools/install_edup.py --target steam --scope flat --xess --compile-only --verify-only
python3 tools/edup_log.py --target steam --expect-build HEAD --version
```

- Pass `--root <worktree>` to both `build-dlls` and `install_edup.py` when
  flying a worktree.
- `--target steam` resolves installs through `tools/steam_library.py`
  (`~/.local/share/Steam`, `~/.steam/steam`, and every library in
  `libraryfolders.vdf`).
- `--compile-only` installs from `<root>/build/compile-build` and refuses if
  `compile_only_receipt.json` is absent, an output disagrees with it, or the
  tree changed since the compile (`tools/compile_only.py` owns the
  fingerprint). It warns that the build is not Windows-validated and records
  `"build": "compile-only"` in the install receipt; `--verify-only` checks the
  game directory against that build and warns if the tree moved.
