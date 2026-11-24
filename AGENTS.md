# ED-UP — instructions for coding agents

`CLAUDE.md` is a one-line `@AGENTS.md` import for Claude Code; keep it that
way.

ED Unofficial Patch (ED-UP, formerly EDVR) fixes and upscales Elite Dangerous:
Odyssey via a proxy `d3d11.dll` beside the game, its own OpenXR runtime
(`openvr_api.dll` in `Openvr\win64`), a settings file (`edup.ini` /
`edup-flat.ini`), and a launcher (`ed-up-launcher.exe`).

The expensive resource on this project is **test flights**. Every wrong
hypothesis costs a build, an install, a headset session, and a log. Everything
here exists to spend fewer of them.

## Context pointers

Read the matching guide before acting on its branch:

| Branch | Read |
|---|---|
| Every diff or review | [CODING_STANDARDS.md](CODING_STANDARDS.md) — self-review and PR review bar (proven claims, one owner, shared state, every route, game addresses) |
| Game-build retargeting, draws, cameras | [GLOSSARY.md](GLOSSARY.md) — canonical domain terms |
| Fork PRs and contributor workflow | [CONTRIBUTING.md](CONTRIBUTING.md) — PR scope, validation reporting, flight notes |
| Building from source, SDK fetchers, CI artifacts | [docs/building.md](docs/building.md) — prerequisites, `fetch_*.py`, local build, Actions downloads |
| `tools\`, builds, installs, logs, worktrees, Linux/CI | [.agents/build-and-tools.md](.agents/build-and-tools.md) — sanctioned `tools\` table, `--compile-only` Proton flights, `build.bat --wait-lock`, compile cache, `--dll-only`, Actions validation |
| Bugs, rendering, `src\`, `.ini` configs, `docs\` | [.agents/diagnosis-and-config.md](.agents/diagnosis-and-config.md) — flight diagnosis, `## Status` docs, `edup.ini`/`edup-flat.ini`, C++ gotchas (`near`/`far`, instruments), issue tracking |
| Codebase map, hubs, blast radius | [.agents/codemap.md](.agents/codemap.md) — subsystem map and top hubs; run `codemap .` / `codemap --importers <file>` when `codemap` is installed (`.codemap/config.json`) |
| Maintainer workstation, releases, `analysis\` | [.agents/maintainer.md](.agents/maintainer.md) — Sean's PowerShell 5.1 shell, solo `--no-ff` Git, release signing & `## Thanks` credits, private `analysis\` handover |

## Universal rules

- **Apply [CODING_STANDARDS.md](CODING_STANDARDS.md) to every changed file**
  before pushing, and use [GLOSSARY.md](GLOSSARY.md) terms. Validation reports
  name every check run, the commit SHA, and every skipped check with its
  reason.
- **Keep context tight.** Grep for the key or symbol first, then read a bounded
  line range rather than whole large files (`edup.ini`, `build.bat`,
  `vscreen.cpp`, `temporal_pass.cpp`, long docs). Read a `docs\`
  investigation's `## Status` block (at most 60 lines) before its journal.
  Delegate jobs larger than 2–3 tool calls by tier: cheapest tier for
  mechanical, checkable work (reflow, install/verify, log read, rename); mid
  tier for cross-file judgement or build gates; keep diagnosis and decisions
  that risk a test flight in the main context. If a cheaper tier fails its
  check or reports doubt, rerun one tier up with its report attached rather
  than finishing by hand.
- **Use `tools\` for owned operations** (installs, flight logs, worktrees,
  Linux builds, PR replies, note reflow) instead of ad-hoc one-liners. Every
  file-writing tool takes `--dry-run` and writes nothing on it (no copies,
  backups, temp files, or directories). Every CLI in `tools\` carries
  `--self-test` gated in `build.bat`.
- **Match validation to the host.** Native Windows runs the full local
  validation build (`build.bat --wait-lock` by absolute path) before commit.
  Linux and other non-native hosts validate C++ changes through the `Native
  Windows build` workflow (`.github/workflows/release.yml`); Wine and Proton
  compile-only builds are not validation.
- **Hold scope.** Quote the exact feature or config key and its current
  behaviour and ask before removing or renaming it. Do only the asked work;
  report adjacent findings or file a GitHub issue rather than folding them in.

## Agent material

Text addressed to agents rather than people stays where a clone can strip it
without reading it: `AGENTS.md` and `CLAUDE.md` at the root, `.codemap\` for
Codemap configuration, and all other agent-only material (guides, handoff
prompts, plans, task lists) under `.agents\`. All of it is `export-ignore` in
`.gitattributes`, so `git archive` and GitHub's source zips leave it out.
`tools\check_agent_material.py` enforces `export-ignore` on recognized agent
paths and rejects zero-width, bidi, and Unicode tag characters across
repository text.

- `docs\` is written for people and is not agent material, even when an agent
  drafted it.
- Keep source comments, commit messages, shipped docs (`README.md`,
  `CONTRIBUTING.md`, `CODING_STANDARDS.md`), and release notes free of
  agent-addressed text: none of them can be stripped.

## Layout

| Path | What |
|---|---|
| `src\d3d11\` | graphics half: hooks, shader fixes, upscalers, draw census |
| `src\openxr\` | native OpenXR runtime translating Elite's OpenVR ABI calls |
| `src\openvr\compat\` | OpenVR ABI header |
| `src\installer\` | install engine (planner, settings, log bundler) and launcher (`update_*`, `launcher_*`, built with `EDUP_LAUNCHER`); standalone installer exe is retired |
| `src\common\` | config, logging, crash sentinel |
| `tools\` | Python tools (each with `--self-test`) and C++ test rigs in subdirs |
| `docs\` | human-facing docs and one investigation doc per arc — read `## Status` first |
| `build.bat` | full build and all gates; `--dll-only` is the receipt-guarded promotion path |
| `package.bat <version>` | packages a release from the last green full build; refuses unless the build receipt matches the tree |
