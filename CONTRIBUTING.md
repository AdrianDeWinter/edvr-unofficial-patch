# Contributing to ED Unofficial Patch

Contributions come in as pull requests from a fork. The maintainer handles
merging, tagging, and releases.

## Before you open a PR

Read the project references that apply to your change:

- **[CODING_STANDARDS.md](CODING_STANDARDS.md)** — the review bar applied to
  every diff (evidence-backed claims, red-before-green tests, single ownership
  of logic and vocabulary, safe shared outputs, full route coverage, and
  address/offset hygiene).
- **[docs/building.md](docs/building.md)** — prerequisites, pinned dependency
  fetchers (`tools/fetch_*.py`), local Windows builds, Linux compile-only
  setup, and GitHub Actions artifacts.
- **[GLOSSARY.md](GLOSSARY.md)** — canonical terms for game builds, bindings,
  build-map statuses, cameras, and draw paths.

## Scope and config rules

- **Keep changes focused.** Address one issue or topic per PR; open a GitHub
  issue for adjacent bugs rather than folding them in. Reference tracked issues
  in commits and PRs (`(#N)` or `Closes #N`).
- **Ask before removing or renaming features or config keys.** Quote the exact
  key and its current behaviour in an issue or PR discussion first.
- **Config keys describe user-visible behaviour**, not internal implementation
  names (`on`, `off`, `auto`). A fix that always helps gets no toggle.
- **Never commit or paste decompiled game code** into the repository, pull
  requests, or issues.
- **Keep tracked text clean** of hidden Unicode characters (zero-width, bidi,
  or tag characters; checked by `tools/check_agent_material.py`).

## Validation expectations

Match validation to what your PR changes, and report every check you ran, the
commit SHA it ran on, and every check you skipped with the reason:

- **Docs-only changes** (`*.md` at root, `docs/*.md`, `wiki/*.md`, issue
  templates): pass the repository's light gates (`tools/check_status_blocks.py`
  and `tools/check_agent_material.py`, including their `--self-test` modes).
- **Python tools (`tools/*.py`):** every CLI carries `--self-test` and is wired
  into `build.bat` (verified by `tools/check_tool_conventions.py`). Every
  file-writing tool supports `--dry-run`, which writes nothing at all.
- **C++ or build changes:** require a full native Windows validation build:
  - On native Windows, run `build.bat --wait-lock` (by absolute path) before
    committing.
  - On Linux or macOS, push your PR branch so GitHub Actions runs the full
    **Native Windows build** workflow (`.github/workflows/release.yml`). A
    Linux Wine `--compile-only` build is for local compile checks and Proton
    test flights only, not merge validation.
- **Bug fixes:** include a test that fails before the fix and passes after it,
  and say so in the PR description.
- **Rendering or VR changes:** name the headset, VR runtime (OpenVR, Oculus
  LibOVR, or OpenXR), and per-eye render setup you flew, or state clearly that
  a headset flight is still needed.
