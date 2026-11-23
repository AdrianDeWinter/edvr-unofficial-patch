# Building ED Unofficial Patch

How to build ED-UP from source, check the build without the game or a headset,
and what the optional DLSS mode and the release package need.

Building needs Visual Studio 2022 with the C++ workload, and Python with the
numpy and Pillow packages, which several of the tools the build runs import:

```
python -m pip install --require-hashes --only-binary :all: -r tools\requirements.txt
```

### Python requirements

`tools\requirements.txt` is a hash-pinned lock compiled from
`tools\requirements.in`; edit the `.in` and regenerate, never the lock by hand.
`--require-hashes` refuses any downloaded file whose SHA-256 is not in the
lock, and `--only-binary :all:` refuses source distributions, which run
arbitrary code at install time. CI installs the same way, so a package that
PyPI republishes or a compromised release cannot reach the build that makes
release assets.

To bump a version, run from the repository root:

```
python -m piptools compile --generate-hashes --strip-extras tools\requirements.in -o tools\requirements.txt
```

Read the new release's notes and PyPI release history first, since the hashes
are taken from PyPI at that moment, then review the diff and run the build.
Use a clean virtual environment, with only this lock installed, for
`tools\sign_manifest.py` and `tools\artifact_signing.py`: they hold the signing
keys and need nothing beyond the standard library.

For a release-capable build, fetch the pinned dependencies. The fetch tools
verify their sources and staged files; the build verifies them again:

```
python tools\fetch_ngx.py
python tools\fetch_ffx_dx11.py
python tools\fetch_xess.py
python tools\fetch_fsr4.py
python tools\fetch_openxr_loader.py
```

`fetch_fsr4.py` stages AMD's FidelityFX (FSR 4.1) headers and its two signed
D3D12 DLLs (about 29 MB) into `%LOCALAPPDATA%\EDVR\fsr4-sdk`; `third_party\fsr4`
or `EDVR_FSR4_SDK` name another copy, and `EDVR_FSR4_SDK=none` forces the build
without it. A build with no copy still succeeds, and `fix.temporal_aa = fsr4`
then refuses at run time. The DLLs land in `build\` and
`python tools\install_edup.py --fsr4` places them beside the game. A release
carries them: `package_native.py` packs the two DLLs and
`AMD-FIDELITYFX-LICENSE.txt` into an `ed-up-fsr4-<hash>.zip` and lists them in the
manifest, the launcher installs them as it installs XeSS's, and packaging fails
("the build has no AMD FSR 4 runtime") when the build has none. AMD's licence
allows redistributing the binaries unmodified, so they keep AMD's own Authenticode
signature and no signing step touches them.

Then build:

```
build.bat --wait-lock --jobs 4
```

That produces the native graphics/runtime pair, `build\openxr_loader.dll`, and
`build\smoke.exe`; the last of these checks the build without the game or a
headset:

```
build\smoke.exe build\d3d11.dll
```

A full build leaves your desktop alone, so you can keep typing in another
window while it runs. The runner holds every test exe to that and fails the
build if one shows a window or takes the keyboard focus; see
`docs\build-focus-steal-2026-09-29.md`.

For a native Windows build from Linux, use the **Native Windows build**
workflow (`.github/workflows/release.yml`). It also runs by itself on every pull
request and every push to `main` (a `codex/release-*` pull request is left to
its branch's push run). It runs on Windows Server 2022 with
Visual Studio 2022, Python 3.12, and all five pinned dependencies. It invokes
the full `build.bat --wait-lock --jobs 4`, including every test rig (one of
them builds the launcher), then calls `package.bat`. The packages contain the
exact version stamp checked by `package_native.py`. Each successful run uploads
the full build receipt, launcher, native DLLs, the release ZIPs, and
`SHA256SUMS`. Build logs, phase timings, and failed flat pixel fixtures are
uploaded separately even when a gate fails. The workflow does not create a
GitHub release or use signing secrets.

A push to a `codex/release-*` branch runs the entire build and package path. If
the commit already has a release tag, the runner uses its exact version;
otherwise it creates a temporary local tag, `v0.0.0-ci`, before building. This
makes the version stamped into the DLLs match the package
version. The temporary tag exists only on the runner; either branch result is a
proof artifact, not a release asset. The branch push trigger works before this
workflow reaches `main`. GitHub does not offer its manual **Run workflow**
control for a new workflow until the workflow file is on the default branch.

### Linux: compile-only builds under Wine

For a test flight on a Steam Deck or another Linux machine that runs Elite
under Proton, `tools/linux_toolchain.py` compiles the production DLLs with
Microsoft's compiler under Wine. It runs `build.bat --compile-only`, which
skips every test and rig, and the launcher build, so the result is not validated and
not a release asset: a merge or a release still needs CI's full build. Use host
`python3`, from any shell:

```
python3 tools/linux_toolchain.py setup --dry-run
python3 tools/linux_toolchain.py setup --accept-license
python3 tools/linux_toolchain.py doctor
python3 tools/linux_toolchain.py build-dlls [--root <worktree>]
```

`setup` needs the system packages `wine msitools git`, which you install
yourself (it never runs sudo), and downloads VS 2022's Build Tools, a Windows
Python and the pinned SDKs. The Build Tools are under Microsoft's licence:
using them needs a valid Visual Studio licence, and they must never be shared,
published or committed. `--accept-license` says you have read and accept those
terms; without it `setup` stops before downloading. Everything lives in the
Git-ignored `.toolchains/windows/` of the main checkout (`EDVR_TOOLCHAIN_HOME`
moves it) and serves every worktree. A tree's output, a worktree's included, is
`build/compile-build` in that tree, with a `compile_only_receipt.json` that
`install_edup.py --compile-only` checks (`tools/compile_only.py`).

It differs from CI's build in a few ways: the MSVC (14.44.35207) and Windows
SDK (10.0.26100) are CI's own; shaders compile with the SDK's own
`d3dcompiler_47.dll`, not Wine's; the proxy's export list comes from Wine's
`d3d11.dll`, which lacks some exports Windows has (Elite imports
`D3D11CreateDevice` alone from it); the FSR 3 DX11 port builds with CMake and
Ninja (`fetch_ffx_dx11.py --generator ninja`), not the Visual Studio generator;
and no gates run. The docstring of `tools/linux_toolchain.py` has the details.

### The caches in CI

The workflow keeps two caches (`actions/cache`), and a third file set inside
the second. Measured 2026-10-22 on the same branch: a cold run took 16.5 min
(build step 12.9 min), the next run on a new commit 8.8 min (build step 6.7
min); the unchanged workflow on `main`, run alongside, took 22.3 min (build
step 17.9 min). Runner speed varies a good deal between runs (the same cold
build was 20 min a day earlier), so compare runs made together.

- **Pinned dependencies** (`third_party\ngx`, `ffx-dx11`, `xess`, `fsr4`,
  `openxr\loader`; 229 MB measured before `fsr4`, whose DLLs add about 29 MB).
  Key: the runner image and the hash of the five `tools\fetch_*.py` files,
  which hold the pins. A hit skips the fetch and the
  FidelityFX build (2.7 min); the `--verify` step runs on a restored copy
  exactly as on a fetched one, and a failed fetch or verify never saves.
- **Compile cache** (about 100 MB compressed). Key: the runner image, the
  branch and the commit; a run restores the newest cache of its own branch,
  else of the default branch (GitHub shows a run only its own ref's caches and
  the default branch's, so one `gh workflow run release.yml --ref main` after a
  merge seeds every branch). It is saved after the build even when a rig
  failed, so a rerun after a flake starts warm. Release tags (`v*`) neither
  restore nor save it: they compile from scratch. The build log's
  `compile_cache:` lines are in the `edvr-diagnostics` artifact; the run's job
  summary carries the totals (`python tools\compile_cache.py --summary LOG`).
- **Riding with it**: `build\rig_times.json`, so the longest rigs start first
  and timeouts are set from the last run, and the two generated shader headers,
  which carry their own key over the HLSL and the compiler DLL and regenerate
  when it differs (about 60 s saved).

Why a hit is safe. `compile_cache.py` already decides every compile on a hash
of the source, of every project header the compiler reported, of the flags, of
the compiler's identity and of INCLUDE, LIB and CL. Nothing about that changes:
a restored entry for an input that changed is a miss, and the tool's self-test
proves it. What the archive adds is a file whose size and time the tool cannot
trust, so `--pack` stores only entries whose outputs still match their
manifest, and `--unpack` installs an entry only when each output's sha256
equals the one recorded when cl wrote it, then sets the file's time back; an
entry that fails is left out and recompiles. The cache skips compiles only:
every rig still runs on every build. Proof builds carry one version stamp,
`0.0.0-ci`, because the stamp is in a header that `log.cpp`, `menu.cpp` and
several other headers and sources include, and a stamp that changed per run
missed all of them. Two things still recompile every run: the launcher (it is
built twice on purpose, to prove two builds under different versions give the
same bytes) and `object_classification_test` (an `ml64` object carries a
timestamp).

After the workflow is on `main`, a manual branch run is also possible:

```powershell
gh workflow run release.yml --ref main
```

These examples use PowerShell syntax compatible with Windows PowerShell 5.1. On
Linux, run them in `pwsh` with GitHub CLI installed. Inspect a proof run and
download its packages. Use the run ID shown by `gh run list`; a rerun
increments `run_attempt`:

```powershell
gh run list --workflow release.yml --branch codex/release-workflow --limit 5
$runId = 'RUN_ID'
gh run view $runId --log-failed
$repoName = gh repo view --json nameWithOwner --jq .nameWithOwner
if ($LASTEXITCODE -ne 0) { throw 'Cannot resolve repository name' }
$runAttempt = gh api "repos/$repoName/actions/runs/$runId" --jq .run_attempt
if ($LASTEXITCODE -ne 0) { throw 'Cannot resolve run attempt' }
New-Item -ItemType Directory -Force -Path "edvr-artifacts/$runId" | Out-Null
gh run download $runId -n "edvr-packages-$runId-$runAttempt" -D "edvr-artifacts/$runId"
if ($LASTEXITCODE -ne 0) { throw 'Package download failed' }
Push-Location "edvr-artifacts/$runId"
try {
    sha256sum -c SHA256SUMS
    if ($LASTEXITCODE -ne 0) { throw 'Package checksum verification failed' }
} finally { Pop-Location }
```

If a run fails, download `edvr-diagnostics-$runId-$runAttempt` in the same way
and read its build logs and saved fixture before changing a gate. The complete
Actions job log is also available through `gh run view $runId --log`.

For a real release, merge and push the validated source to `main`, then make
and push its exact version tag from a clean checkout. A pushed `v*` tag runs
the same full validation and packaging at that tag; the workflow refuses a tag
whose name is not a semantic version:

```powershell
git switch main
if ($LASTEXITCODE -ne 0) { throw 'Cannot switch to main' }
git pull --ff-only
if ($LASTEXITCODE -ne 0) { throw 'Cannot update main' }
if (git status --porcelain) { throw 'Checkout must be clean before tagging' }
$version = 'VERSION'            # for example '0.20.0'
git tag "v$version"
if ($LASTEXITCODE -ne 0) { throw 'Cannot create version tag' }
git push origin "v$version"
if ($LASTEXITCODE -ne 0) { throw 'Cannot push version tag' }
gh run list --workflow release.yml --limit 10
$runId = 'TAG_RUN_ID'
gh run watch $runId --exit-status
if ($LASTEXITCODE -ne 0) { throw 'Tagged Windows build failed' }
$repoName = gh repo view --json nameWithOwner --jq .nameWithOwner
if ($LASTEXITCODE -ne 0) { throw 'Cannot resolve repository name' }
$runAttempt = gh api "repos/$repoName/actions/runs/$runId" --jq .run_attempt
if ($LASTEXITCODE -ne 0) { throw 'Cannot resolve run attempt' }
New-Item -ItemType Directory -Force -Path "edvr-artifacts/$runId" | Out-Null
gh run download $runId -n "edvr-packages-$runId-$runAttempt" -D "edvr-artifacts/$runId"
if ($LASTEXITCODE -ne 0) { throw 'Package download failed' }
Push-Location "edvr-artifacts/$runId"
try {
    sha256sum -c SHA256SUMS
    if ($LASTEXITCODE -ne 0) { throw 'Package checksum verification failed' }
} finally { Pop-Location }
```

Write and review release notes before creating the draft. Generate the Thanks
section from the previous tag, then write each contributor's sentence by hand.
Only merged PR authors are named, plus anyone the maintainer approved by name
in `tools/release_credits_approved.json` for a commit in the release. Issue and
log reporters get the tool's general line. Review the final notes file.

Besides the two ZIPs, the package artifact carries the split assets the
launcher updates from: `ed-up-core-<version>.zip`, an `ed-up-dlss-<hash>.zip`
an `ed-up-xess-<hash>.zip` when the build has those runtimes, an
`ed-up-fsr4-<hash>.zip` (AMD's FSR 4 runtime, which every release has),
`ed-up-launcher.exe` when the build has one, and two manifests. `ed-up-manifest-v2.json`
is the schema-2 manifest launcher 0.2.0 and later read; it names them all with their
hashes. `ed-up-manifest.json` is a schema-1 bridge: launcher 0.1.0 (shipped in v0.22.0)
reads only that name and schema, so the bridge sets its `min_launcher` to the
launcher this build packages and carries that launcher block, which makes
0.1.0 replace itself and read the schema-2 manifest from then on. It also lists,
truthfully, each file that has a schema-1 item. It stays until a decision retires it
(`docs/design-manifest-schema2-2026-11-12.md`). `SHA256SUMS` lists every one of them. The
workflow never signs. Sign both manifests in your own terminal, because
`sign_manifest.py` refuses to run anywhere else, and it reads the private key
from a hidden prompt or, with `--key-stdin`, from one line on stdin so your
password manager's command line can supply it. The key is never kept in a
repository or in a session. The signing tool only signs with a key whose public
half is listed in `src/installer/update_keys.h`. Check the folder before
attaching anything; the check exits 0 only when both signatures, both schemas,
the bridge's agreement with the schema-2 manifest, and the size and hash of
every asset and every file inside the zips all pass:

```powershell
python tools/sign_manifest.py sign "edvr-artifacts/$runId/ed-up-manifest-v2.json"
if ($LASTEXITCODE -ne 0) { throw 'Signing the schema-2 manifest failed' }
python tools/sign_manifest.py sign "edvr-artifacts/$runId/ed-up-manifest.json"
if ($LASTEXITCODE -ne 0) { throw 'Signing the schema-1 bridge failed' }
python tools/sign_manifest.py verify "edvr-artifacts/$runId"
if ($LASTEXITCODE -ne 0) { throw 'The signed release folder did not verify' }
```

Then attach the two verified ZIPs (`ed-up-<version>.zip`, the files for a manual install,
and `ed-up-launcher-linux-<version>.zip`, a convenience package outside the signed
manifest), the split assets, both manifests with
their `.sig` files, and `SHA256SUMS` to a draft release. A release missing either `.sig` is simply
not offered to the launcher as an update:

```powershell
python tools/release_credits.py --from vPREVIOUS --to "v$version" --thanks --out thanks.md
if ($LASTEXITCODE -ne 0) { throw 'Review release credits before continuing' }
# Edit release-notes.md and include the reviewed ## Thanks section from thanks.md.
$folder = "edvr-artifacts/$runId"
$assets = @(
    "$folder/ed-up-$version.zip"
    "$folder/ed-up-launcher-linux-$version.zip"
    "$folder/ed-up-manifest-v2.json"
    "$folder/ed-up-manifest-v2.json.sig"
    "$folder/ed-up-manifest.json"
    "$folder/ed-up-manifest.json.sig"
    "$folder/SHA256SUMS"
)
$assets += Get-ChildItem "$folder/ed-up-core-$version.zip", "$folder/ed-up-dlss-*.zip", "$folder/ed-up-xess-*.zip", "$folder/ed-up-fsr4-*.zip", "$folder/ed-up-launcher.exe" -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName }
gh release create "v$version" @assets --draft --verify-tag --title "ED Unofficial Patch $version" --notes-file release-notes.md
if ($LASTEXITCODE -ne 0) { throw 'Draft release creation failed' }
```

For a prerelease tag, add `--prerelease` to `gh release create` as well.

On native Windows, where `package.bat <version>` packages the release without
Actions, the same assets land in `dist\`, `SHA256SUMS` included:
`package_native.py` writes it, and the workflow only checks that it lists the
files above with their hashes. `package.bat` prints the signing command when
it finishes. Run `python tools\sign_manifest.py sign
dist\ed-up-manifest-v2.json` and `... sign dist\ed-up-manifest.json`, then `python tools\sign_manifest.py verify dist`,
and attach the files from `dist\` the same way. `package_native.py` writes the
split zips with fixed timestamps and file order, so packaging twice gives the
same bytes, and it removes older `ed-up-dlss-*.zip`, `ed-up-xess-*.zip` and `ed-up-fsr4-*.zip` from
`dist\` when a runtime's contents change.

To sign the binaries (Authenticode, through Azure Artifact Signing), set
`EDVR_SIGN=1` for the full `build.bat`. It signs `d3d11.dll` and
`edvr_openxr_runtime.dll` before anything copies them, then
`ed-up-launcher.exe`, so `package.bat` zips and hashes the signed bytes. Once per machine, run `python
tools\artifact_signing.py fetch` (Microsoft's signing client, pinned by hash,
from NuGet), then `az login` as an account that holds `Artifact Signing
Certificate Profile Signer` on the `ed-up-release` profile. `python
tools\artifact_signing.py check` says what is missing and `... verify <files>`
re-checks a file. A build asked to sign that cannot, fails. Every signature is
timestamped and every signing makes new bytes, so a signed `ed-up-launcher.exe`
differs from release to release even when its code did not, and an installed
launcher replaces itself each time (see below). The workflow does not sign.

Build every release by the same route. `ed-up-launcher.exe` is the same bytes
from two builds on one machine, but a CI build and a local build differ, and an
installed launcher replaces itself when the digest changes. Replacing an asset on
a published release also takes a few minutes to reach everyone: the API lists the
new digest at once while GitHub's download cache can still serve the old file. The
launcher skips its check while the two disagree and looks again on its next start.

The draft is the review point before publication. A failed build has no package
artifact and should never be used for a release.

The optional DLSS mode, and a release that carries NVIDIA's runtime, need
the DLSS SDK on the machine. Its licence keeps it out of this repository, and
it is not in the graphics driver either. This command

```
python tools\fetch_ngx.py
```

fetches one pinned commit of NVIDIA's public SDK repository into
`%LOCALAPPDATA%\ED-UP\ngx-sdk` and checks the runtime's hash against the pin in
the script. `EDVR_NGX_SDK` points the build at a copy somewhere else.

Writing the release's settings files (`tools\gen_installer_rc.py
--release-files`, a step of the full build) needs the native pair and the
bundled loader; there is no legacy OpenVR-only package. `package.bat <version>
--no-dlss` packages a build made without the DLSS SDK. When the build contains
DLSS, the archives keep its matching DLL and NVIDIA license notice.
