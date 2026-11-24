# Maintainer workflow (Sean's workstation, releases, and `analysis\`)

Applies **only** when acting as the maintainer on Sean's machine, merging to
`main`, cutting/signing releases, or working with the private `analysis\`
repository. External contributors in a fork use
[CONTRIBUTING.md](../CONTRIBUTING.md) and their own shell and Git workflow.

## Shell (Windows PowerShell 5.1)

- **Use the PowerShell tool**, not Bash.
- **PowerShell 5.1 syntax:** chain commands with `;` and `if ($?)` (5.1 has no `&&`, `||`, ternary, or `??`).
- **Capture native stderr separately instead of `2>&1`:** 5.1 wraps stderr
  lines in `ErrorRecord`s and sets `$?` false even on a clean exit.
- **Redirect file-writing tools to `*> $null` or a log file** and read the log
  (piping into `Select-Object -First N` kills the writer mid-write; an install
  died that way, #100).
- **Pass paths to native exes as arguments:** piping strings over stdin (`$files | git checkout --pathspec-from-file=-`) prepends a UTF-8 BOM to the first item so it matches nothing.
- **Write commit messages to a UTF-8 file without BOM and pass `git commit -F
  <file>`:** 5.1 does not escape embedded `"` on native command lines,
  splitting messages into stray pathspecs.
- **Decode UTF-8 explicitly when reading files back:** use
  `[Text.Encoding]::UTF8.GetString([IO.File]::ReadAllBytes(path))`.
  `Get-Content` on a BOM-less UTF-8 file uses the ANSI codepage and turns
  em-dashes into mojibake (once reached a public GitHub comment).

## Git (solo maintainer workflow)

- **Branch, commit, merge to `main`, push** — no pull requests for solo
  maintainer work.
- **Merge with `git merge --no-ff -F <file> <branch>`** (never fast-forward or
  squash). This keeps one commit per branch on `git log --first-parent`,
  enables `git bisect start --first-parent` (each step can cost a flight),
  allows `git revert -m 1 <merge>`, and keeps branch commit hashes reachable
  for flight logs and `docs\` citations. Write a descriptive merge subject
  naming what the branch did and any issue number — never an auto-generated
  branch-name merge subject.
- **If a push is rejected because `main` moved**, redo the merge cleanly: `git
  fetch origin`, `git reset --keep origin/main`, merge again, push (avoid `git
  pull`, which hides the other merge from `--first-parent`, and `git pull
  --rebase=merges`, which rewrites branch commit hashes).
- **Checking a branch flight log from `main`:** the log records the branch
  commit, not the merge commit, so pass `--expect-build <branch commit>`
  instead of `HEAD`.
- **Confirm the push landed:** run `git push`, then `git log origin/main -1`
  and verify the expected commit is on `origin/main` before calling the task
  done.

## Releases, signing, and credits

### Release validation and packaging

- **Native Windows release:** a green full `build.bat --wait-lock` followed by
  `package.bat <version>` at the clean `vX.Y.Z` tag is sufficient; GitHub
  Actions is optional.
- **Non-native host release:** merge the validated source, push the exact clean
  `vX.Y.Z` tag, and require that tag's full Windows `release.yml` build and
  package run to pass. Download and verify every asset against `SHA256SUMS`,
  have the maintainer sign the manifest, prepare and review release notes
  (including `## Thanks`), and create a **draft** release; publishing stays an
  explicit step. See [docs/building.md](../docs/building.md) for download and
  release commands.

### Signing and credits tools

| Operation | Use | Never |
|---|---|---|
| Credit contributors in release notes | `python tools\release_credits.py --from <previous-tag> --thanks` | writing credits from memory or ad-hoc `gh` queries |
| Sign a release manifest (maintainer's own terminal only: `generate` and `sign` refuse without one; private keys never enter an agent session) | `python tools\sign_manifest.py sign <manifest>`, then `python tools\sign_manifest.py verify <folder>` | signing in CI or letting any other tool see the private key |
| Sign shipped binaries (Authenticode via Azure Artifact Signing; maintainer's own terminal after `az login`: set `EDUP_SIGN=1`, then run full `build.bat`) | `python tools\artifact_signing.py check`, `fetch`, `sign`, `verify` | `signtool` by hand, or storing any certificate or key file in the repo |

### `## Thanks` policy

Every release's notes carry a `## Thanks` section generated from
`release_credits.py --thanks` and finished by hand:

- Name only authors of PRs merged in the release, plus anyone the maintainer
  approved by name in `tools\release_credits_approved.json` for a commit in the
  release.
- Add entries to `tools\release_credits_approved.json` only on the maintainer's
  word; a person writes each sentence.
- Reporters who opened issues or sent logs get the tool's single general line
  and are not named.
- Keep people known only by an anonymised label ("user 2", "the reporter")
  unnamed unless the maintainer consents.

## Private `analysis\` repository

`analysis\` in the **main checkout** is a separate private git repository
(`characterecho-sean/elite-analysis`), git-ignored here. It holds
decompiler/disassembly notes, address maps, Ghidra scripts, and (kept local and
never committed) archived game executables (builds 332841, 334473, 334925, ...)
plus the Ghidra projects. **Read `analysis\AGENTS.md` before working in it**;
it defines its own rules and skills (`elite-rva`, `retarget-build`,
`ghidra-headless`, `record-finding`).

- **Reaching `analysis\` from a worktree:** worktrees have no `analysis\`
  folder. Reference the main checkout's absolute path
  (`C:\Users\seanm\projects\edvr-unofficial-patch\analysis`; `git rev-parse
  --git-common-dir` gives `<main>\.git`) and pass its executables to
  `tools\build_diff.py` and per-arc `pins.py` by path. Never junction or
  symlink `analysis\` into a worktree — deleting the worktree would wipe ~4 GB
  of Ghidra projects through the link.
- **Separate git history:** commit inside it with `git -C analysis ...` to its
  own `main`, staging files by path. Nothing in `analysis\` ships in an ED-UP
  archive.
- **Public citations:** source comments and docs here cite
  `analysis\decomp\...` as a maintainer pointer only. Because those files are
  private, state the fact directly in the public doc as well, and never paste
  decompiled code into this repository or its issues.
- **Single-writer handover from worktrees:** sessions in worktrees may read
  `analysis\` freely and hand writes over to the main-checkout `elite-analysis
  intake` session (do not bypass the write guard with shell writes, a second
  clone, or a junction):
  1. Create
     `C:\Users\seanm\AppData\Local\EDVR\analysis-inbox\<yyyymmdd-hhmmss>-<topic>\`
     (outside every repository so it survives worktree removal).
  2. Place notes/scripts inside it with a `MANIFEST.md` (build, from, summary,
     and destination under `analysis\` per file; see `analysis\AGENTS.md`,
     "Intake").
  3. Notify the `elite-analysis intake` session (`ListAgents` or
     `mcp__ccd_session_mgmt__list_sessions`) if running; otherwise report that
     an inbox item is waiting.
  4. Cite `analysis\decomp\<name>` in public docs only after it is filed.
