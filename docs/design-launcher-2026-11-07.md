# Standalone launcher

## Status

- **State:** build order steps 2, 3, 4 and 6, and a Linux package, are on main, merged
  2026-11-08 from `claude/hungry-heyrovsky-000d7b` after the review of that day. No
  release carries the launcher yet. Step 5 has not started.
- **Proven, 2026-11-07 (see Test results):** CI builds the split assets, the signing
  tool's signature verifies through BCrypt, and a test release on GitHub was refused
  or accepted correctly over real HTTPS. The launcher downloaded it, verified every
  file and installed it into a scratch folder with no write to the real folders.
- **Flown, 2026-11-08:** direct launch on Steam, VR and flat, and on the Frontier store,
  flat, with a first sign-in and its emailed code. A dry run then signed in from the
  saved login with no prompt (see Test results).
- **Found in that flight:** the window picked the first install with EDVR in it on
  every start, not the one played last, so a second Play went to Steam. It now opens
  on the install Play was last pressed for (rig-tested, not flown).
- **Rig-tested only, not yet run for real:** the first run's copy and registration, a
  self-update, a real UAC prompt and the elevated copy, a Frontier-store launch in VR
  (dry run only), and the EDLaunch route on any store, including closing EDLaunch.
- **Next flight:** Play VR on the Frontier store. Then, with direct launch off, Play on
  each store, watching EDLaunch close after the game exits. Self-update and the first
  run need the next real release.
- **Frontier's terms** were read on 2026-11-07 before step 4. None of them names a
  third-party launcher. The maintainer chose to make direct launch opt-in.
- **Waiting on others:** SignPath had not replied on 2026-11-07. A launcher-only
  signing answer could still move it to its own repo (see Why this repo).
- **Decisions:**
  - Direct launch is off until the user ticks it, and asks first. It starts the game
    without WatchDog64, as min-ed-launcher does by default (2026-11-07).
  - The launcher's names are `ed-up-*`, and its private keys are in the maintainer's
    password manager, with both public keys in the exe (see Updates).
  - An update needs a click. Nothing downloads or changes until Download is pressed,
    and a game folder is never updated without the plan and its confirmation.
  - It lives in this repo and grows out of `src\installer`, which it replaces.
  - The repo was renamed to `ed-unofficial-patch` on 2026-11-07 (see Name).
  - It ships as a bare exe that registers itself on first run, with no MSI.
  - It is one Windows exe everywhere. On Linux and Mac it runs inside Proton or
    CrossOver beside the game, with no cross-platform SDK.
  - It updates from GitHub Releases, using a signed manifest.
  - It can start the game directly on Steam and on the Frontier store. The sign-in
    is ported to C++ from min-ed-launcher. EDLaunch stays for Epic, for patching
    the game, as the fallback when direct launch fails, and as the default.
  - VR or flat is `/vr` or `/novr`, never a Settings.xml write on a direct
    launch.
- **Proven on Windows, 2026-11-07 (T1 to T4, see Test results):** a process Steam
  did not start gets a ticket with `SteamAppId=359320`, and the game then runs
  in VR and flat with no EDLaunch or WatchDog64 above it. The Frontier store works
  from its separate folders with a saved login.
- **Not yet shown on Windows:** playtime counting, the overlay working on screen, and
  rendering in a headset.
- **Open hypotheses, for Linux and Mac:**
  - T5: the installer's window, network access and signature checks work under
    Proton on the Deck.
  - T6: a Windows exe inside the Proton prefix can get a Steam ticket.
  - T7: the same launch works in a CrossOver bottle on the Mac.
- **Linux package built 2026-11-08, never run on a Deck:** `ed-up-launcher-linux-<version>.zip`
  runs the launcher in Elite's Proton prefix (Linux and Mac). T5 and T6 are open for it.
- **Issues:** [#196](https://github.com/characterecho-sean/ed-unofficial-patch/issues/196).

## Why this repo

`src\installer` is 9,941 lines: store detection, the chain planner, SHA-256
repair, mirror restore, the settings window and the log bundler. Only launching
and networking are missing. It also shares source with the mod: the `src\common`
ini and scope headers, the settings table generated from `edvr.ini`, and
`advanced.real_dll`, which the installer writes and the DLL reads. A change to
any of those has to land with the DLL, behind the same build gate.

The usual reason to split is independent releases, and this repo can do that.
The launcher gets its own version number instead of `git describe`, and ships as
an asset on every mod release. With a deterministic build (`/Brepro`, not used
today) its bytes change only when its source does, so its hash can build
reputation with Defender. Seven release candidates in 11 days would otherwise
reset that each time. It must not get its own tags here: `git describe --tags`
would stamp mod builds from them, and `releases/latest` would point at the
launcher.

**What would change this:** SignPath's terms exclude proprietary components, and
`d3d11.dll` links NGX. A launcher with neither NGX nor an embedded `nvngx_dlss.dll`
is a much cleaner signing candidate. If SignPath would sign it only from a repo
without NGX, split it. Keeping release-specific data in the manifest keeps that
split cheap.

## Name

EDVR now covers flat as well as VR, so the repo went from `edvr-unofficial-patch`
to `ed-unofficial-patch` on 2026-11-07, before any launcher compiles in the update
URL. A separate launcher repo would have left the mod's releases under the old name.

Old URLs tested straight after the rename, with a browser's `Accept` header: repo,
issue, pull request, discussion, release, file, commit and Actions pages all return
301 to the new name. Release assets and raw files return 200, `git ls-remote` gives
the same head, and `gh api` on the old name resolves to the new one. A plain `curl`
gets a 404 on `issues/<n>`, because that page redirects only for a browser's
`Accept` header, so test those links with one. The redirects stop if a repo is ever
created as `edvr-unofficial-patch` under this account, so that name is never reused.

What users see follows the new name: the README title, release titles, ZIP and exe
names, `ProductName`, `CompanyName` and window text. The window's heading and title
bar read "ED Unofficial Patch", and its running text keeps ED-UP. "Elite: Dangerous
Unofficial Patch" was tried on 2026-11-07 and dropped the same day, because "Elite:
Dangerous" is one of Frontier's listed trade marks. What code and existing installs rely on stays: `edvr.ini`, `edvr-flat.ini`,
the `edvr_*` exports the planner recognises, `%LOCALAPPDATA%\EDVR`, `edvr_install\`
and log file names. Dated docs keep their old links, which redirect.

## Updates

GitHub Releases is enough, with no server of our own. The API already returns a
SHA-256 `digest` for every asset, and `/releases/latest` skips pre-releases.
Anonymous API calls are limited to 60 an hour per IP, and the maintainer's PC read 0
of 60 on 2026-11-07 before the rename tests had made any such calls. So a 403 means
skip the check and try again next launch, never a launch failure, and requests send
`If-None-Match`, since a 304 does not count against the limit.

New assets, added beside today's:

| Asset | Holds | Fetched |
|---|---|---|
| `ed-up-launcher.exe` | The launcher, no embedded payload | When its digest changes |
| `ed-up-manifest.json`, `ed-up-manifest.json.sig` | Per-file hashes, asset names, where each file goes | Every check |
| `ed-up-core-<ver>.zip` | `d3d11.dll`, `openvr_api.dll`, loader, inis, descriptor | Every update |
| `ed-up-dlss-<hash>.zip`, `ed-up-xess-<hash>.zip` | The DLSS and XeSS files | Only when their hash changes |

Today no asset is a DLL zip. The installer ZIP is 97 MB and the full ZIP 194 MB,
and DLSS and XeSS are 130 MB of the 164 MB installer exe.

The maintainer chose these names on 2026-11-07. `%LOCALAPPDATA%\EDVR` keeps its name
under the rename rule above.

### The manifest

The manifest is ASCII JSON, signed as exact bytes. The launcher checks the signature
before it parses anything.

**Superseded (2026-11-12):** the manifest is now schema 2, which says where each file
goes and how an existing file is treated, and `min_launcher` is gone: the schema number
is the compatibility contract. The format, the schema versioning and the shared parser
are in `docs\design-manifest-schema2-2026-11-12.md`. What stays as written here:

- The `launcher` block, `assets`, `version`, `build` (the stamp in the DLLs, which the
  install record stores), `tag` and `issued`. A file's `path` is its path in the zip and
  in the unpacked `ed-up-<ver>.zip`; `--payload <dir>` reads
  `<dir>\ed-up-manifest.json` (unsigned is fine) and takes each file by that `path`.
- `tag` must match the release it was fetched from, so a signed manifest cannot be
  moved onto another release.
- `issued` only goes up. The launcher records the highest it has accepted and ignores
  a lower one on an automatic check, so a compromised account cannot roll users back
  to an older signed build. Rolling back by hand reads the local copy of a manifest it
  already accepted.
- Cache file names come from `sha256` after a hex check, and zip entries are found by
  the manifest's `path`. Nothing is ever written to a path named inside a zip.
- The hash in `ed-up-dlss-<hash>.zip` is taken over the file names and file hashes, and
  the zips are built with fixed timestamps and order. The same DLSS files therefore
  keep the same asset name from release to release.

### Signing and the key

The signature is P-256 ECDSA over the SHA-256 of the manifest's bytes, written as 128
hex characters (`r` then `s`). `tools\sign_manifest.py` signs with a deterministic
nonce (RFC 6979) in the Python standard library, and the launcher verifies through
BCrypt, so a build test proves the two agree.

The maintainer decided on 2026-11-07 that the private key lives in their password
manager. It is never on GitHub or in a repository, and never in a session: `generate`
and `sign` refuse to run without a real terminal, and `sign` reads the key from a
hidden prompt or from a password manager's command line on stdin.

Actions cannot sign, so a release gains one local step. CI or `package.bat` writes the
unsigned manifest, the maintainer signs it, and the `.sig` goes up with the other
assets. A release with no `.sig` is simply not offered as an update. Today's assets do
not depend on any of this.

The exe embeds two public keys, the main one and a recovery one, and accepts a
signature from either. Losing the main key therefore does not strand installed
launchers. Replacing a leaked key needs a launcher update signed by the other key.

- **Compare digests, not tags.** rc.7 was re-cut and its tag moved after 24
  downloads. A version comparison would have told those users they were current.
- **Channels.** Stable reads `/releases/latest`. RC is an opt-in that reads
  `/releases`.
- **Signing.** `SHA256SUMS` catches corruption but not a compromised GitHub
  account, and an updater that drops DLLs beside a game turns that account into
  code execution on every user's PC. The manifest is signed with a key kept off
  GitHub, and the public key is in the exe. P-256 through Windows' BCrypt needs
  no dependency. This has to be in the first version: a launcher that does not
  verify cannot be fixed by an update it has already trusted.
- **Self-update.** Rename the running exe, write the new one, restart, delete
  the old one on the next start. The exe lives in `%LOCALAPPDATA%\EDVR\launcher\`,
  which survives a game wipe, needs no admin and serves all three installs.
- **File cache.** Verified files go in `%LOCALAPPDATA%\EDVR\payload\<sha256>`, so
  repair works offline and rolling back is one click. `payloadItem()` in
  `src\installer\payload.cpp` is the only reader of the embedded files, so the
  planner does not need to know where the bytes came from.
- **Offline install.** `--payload <dir>` reads the same files from an unpacked
  `ed-up-<ver>.zip`. The Linux wrapper uses this too once the embedded payload goes.
- **Cache lag.** After an asset is replaced on a release, GitHub's API lists the new
  digest at once while its download cache can serve the old file for a few minutes,
  and the cache differs by `Accept` header. A download that differs from the listing
  therefore skips the check rather than refusing, and only the signature refuses a
  release. Assets are requested with no `Accept` header.
- **Same route for every release.** The launcher's bytes repeat between two builds on
  one machine, but a CI build and a local build differ, and an installed launcher
  replaces itself when the digest changes.

Rolling back is not built yet. The cache and the stored manifests keep what it needs,
the last three releases, but nothing in the window offers it.

The README has to disclose the network access. The launcher contacts
`api.github.com`, GitHub's release downloads, and `api.zaonce.net` for sign-in.

## Launching

### Direct, on Steam and the Frontier store

The port's source is min-ed-launcher v0.14.0, MIT, commit
`139e4ff7ab9027925648fb27767cb87acdcf290a`. `Steam.fs`, `MachineId.fs`, `Api.fs`,
`Product.fs` and `App.fs` hold what is needed. Ported files keep its licence
notice. Estimated from its source: 800 to 1,000 lines of F# for Steam, about 150
more for Frontier, so roughly 1,500 lines of C++.

1. Find the folders from what `detect.cpp` already knows, then read
   `VersionInfo.txt` from the product folder.
2. Work out the machine ID: SHA-1 of `HKLM\SOFTWARE\Microsoft\Cryptography\MachineGuid`
   and `HKCU\SOFTWARE\Frontier Developments\Cryptography\MachineGuid`, each
   trimmed, as hex, first 16 characters, lower case. The second key is created
   with a random GUID if missing.
3. Sign in at `https://api.zaonce.net`:
   - **Steam:** set `SteamAppId`, load `steam_api64.dll` by absolute path from the
     install, then `SteamAPI_Init`, `SteamUser019`, `GetSteamID` (must be non-zero)
     and `GetAuthSessionTicket`. Send the ticket as hex to `/3.0/user/steam/auth`.
     Cancel the ticket and call `SteamAPI_Shutdown` before starting the game.
     `SteamAppId` is put back as it was once Steam is released, and set again only
     for the Steam game's own start. Left set, it reached every later start from the
     same window, a Frontier-store game included (review of 2026-11-08).
   - **Frontier:** POST to `/3.0/user/frontier/auth`. A first login returns a 2FA
     challenge, and `/3.0/user/frontier/token` turns the typed code into a machine
     token.
4. Fetch `/3.0/user/purchases` for the Odyssey product's server arguments and
   game arguments.
5. Compare `/3.0/user/installer?sku=` with the local version. A difference hands
   over to EDLaunch to patch. The launcher never patches the game, which leaves
   out the largest part of min-ed-launcher.
6. Start `EliteDangerous64.exe` from the product folder, unelevated:
   `"ServerToken <machineToken> <authToken> <serverArgs>"`, then `/steam` on
   Steam, then exactly one of `/vr` and `/novr`, then the product's game arguments.
7. Minimise the window and wait on the process handle. When the game exits the
   window comes back and stays open, as on the EDLaunch route. It first hid and
   exited with the game, which read as the launcher closing when the game started
   (changed 2026-11-08).

The game folders on the maintainer's machine, 2026-11-07:

| Store | Launcher folder | Product folder | `steam_api64.dll` |
|---|---|---|---|
| Steam | `<library>\steamapps\common\Elite Dangerous` | `Products\elite-dangerous-odyssey-64` inside it | Beside `EDLaunch.exe` |
| Epic | `C:\Program Files\Epic Games\EliteDangerous` | `Products\elite-dangerous-odyssey-64` inside it | None |
| Frontier | `C:\Program Files (x86)\Frontier\EDLaunch` | `%LOCALAPPDATA%\Frontier_Developments\Products\elite-dangerous-odyssey-64` | None |

`VersionInfo.txt` says `useWatchDog64: true`. min-ed-launcher's shipped default
does not use WatchDog64, so direct launches run the game without it. T3 ran that
way with no trouble.

### Epic stays on EDLaunch for now

- Epic issues its exchange code only to the exe it launches, which is why
  min-ed-launcher replaces `EDLaunch.exe`, a swap that has to be redone after
  every update or verify. The manifest's `LaunchExecutable` field might be an
  alternative, but that is untried.
- min-ed-launcher reads the Epic client ID and secret from the game's .NET DLLs by
  reflection (`EosIF.dll`, `EosSdk.dll`). A C++ exe cannot do that without hosting
  .NET, and Frontier's secret should not be embedded in EDVR.

So Epic keeps EDLaunch's slow start. The launcher does remove the lingering
window, as below.

### The EDLaunch route

This is the Epic route, the fallback, and the setting for anyone who prefers
Frontier's launcher. It goes in first because all three stores need it.

| Store | Starts with |
|---|---|
| Steam | `steam://rungameid/359320` |
| Frontier | `EDLaunch.exe /autorun /autoquit /edo /vr` or `/novr` |
| Epic | `com.epicgames.launcher://` URI, app `9c203b6ed35846e8a4a9ff1e314f6593` |

The launcher waits for the game to start and exit, then closes EDLaunch with
`WM_CLOSE`, since `/autoquit` is not reliable. If the game never started, EDLaunch
is probably patching and is left alone.

Whether the Epic URI can carry arguments is unknown. If it cannot, VR or flat on
Epic needs a Settings.xml write, and that is the only place the launcher writes
that file.

As built in step 3, only the Frontier route carries VR or flat, so it gets Play VR and
Play flat and remembers the choice per install. Steam and Epic get one Play button and
a line saying the game uses the mode it is set to. Nothing writes Settings.xml. Steam
gains the flags with direct launch in step 4, and Epic's question stays open.

Frontier's store keeps the game under AppData and EDLaunch under Program Files, so that
route looks in the launcher's install folders as well as above the game. A folder the
user picked looks only above itself, because the store's EDLaunch would start the
store's copy of the game, not that folder's.

### Falling back

| Failure | Action |
|---|---|
| Network failure, or a 400, 401 or 403 | Say so, then use the EDLaunch route |
| HTML ownership page | Retry once, as these are reported as intermittent, then the EDLaunch route |
| Game version differs from the API's | EDLaunch route, to patch |
| `steam_api64.dll` fails, or Steam is not running | Say so, then the EDLaunch route |

A Frontier API change therefore makes launching slower but does not stop anyone
playing.

### Steam's Play button

`--wrap %command%` in Steam's launch options is optional, for anyone who wants
Play in Steam to run the pre-launch check. If the command ends in `EDLaunch.exe`
the wrapper replaces it with a direct launch. Anything else, such as
`MinEdLauncher.exe`, is passed through after the check, so existing min-ed-launcher
setups still work. T1 showed the launcher does not need it to sign in on Steam.

### Elevation

Default Frontier and Epic folders are under Program Files, and the game must never
start from an elevated process. Today `relaunchElevated` reruns the whole app
elevated. The launcher instead stays unelevated, writes its plan to a file, and
starts a short-lived elevated copy that applies that plan and exits.

The plan file names no path that the copy writes to or installs from. It holds only the
choices and the adopted manifest's hash. Any program running as the user could write a
plan and start the copy, and the permission prompt would name this launcher. So a path in
the file, as the first version had, would have let that program write anywhere as
administrator and install an unsigned folder. The copy works every path out from where the
plan sits, `<data root>\launcher\plan-<n>-<n>.txt`, and refuses a plan anywhere else or
reached through a link. The result goes beside the plan. The manifest and the payload come
from the same root, and the settings mirror from the game folder and that root. It
installs only a signed download, so a `--payload` folder for a game under Program Files is
refused, with a line saying so (review of 2026-11-08). One race remains: a link swapped in
between the check and the write.

## VR or flat

Flags, for a launch that builds its own command line. The 2026-10-12 flights
(`design-unified-install-2026-10-12.md`) showed both directions on Steam: `/vr`
over a file at 0 went into VR, and `/novr` over a file at 4 stayed flat with no
`openvr_api.dll` load. A flat start with no `/novr` opens a live OpenXR session,
and `/novr` avoids that. Settings.xml is shared across stores and outlives the
launch, while a flag touches nothing. The launcher remembers the last mode per
install, in its own `launcher.ini`, and which install Play was last pressed for. The
window opens on that install while it is still there, rather than on the first one
found with EDVR in it.

## Pre-launch check and repair

The planner already covers the EDHM case (`installer.md`, "The two halves"): a mod
that has replaced EDVR's `d3d11.dll` is parked as `d3d11_<mod>.dll`, an older
parked copy goes to the backup folder, and EDVR's file goes back. Comparison is by
SHA-256. The check runs that plan and applies it if anything needs changing.

- Automatic for EDVR's own files and recognised mods. An unknown foreign DLL is
  asked about once. An unreadable one is reported and nothing is touched, as today.
- **Developer installs are left alone.** `install_edvr.py` puts branch builds in
  the same folders, and a hash-based repair would overwrite a build about to be
  flown. A folder whose `edvr_native_receipt.json` or `edvr_flat_receipt.json` is
  newer than the install record belongs to the developer.
- An uninstall clears the install record, so nothing puts EDVR back afterwards.
- The launcher only sees launches that go through it. Play in Epic's launcher skips
  the check, and so does Play in Steam without `--wrap`.

## Restart settings

Most of this exists: `# ui: ... | restart` feeds `needsRestart`, and the settings
window writes through `mergeIni` and the mirror. Two changes:

- **Per-eye resolution.** `fix.openxr_resolution` is free text keyed by a token
  known only once OpenXR is running, such as `oculus/meta-quest-3`. The DLL records
  the last headset and its native size in the mirror folder, and the launcher shows
  a width field for that headset. Opening OpenXR from the launcher instead would
  start SteamVR.
- **The settings table becomes data.** `settings_schema.inc` is compiled in, so
  every `edvr.ini` edit would change the launcher's bytes. It ships in the core
  asset instead, and that also keeps it correct for the installed mod's version.

## Credentials and tokens

- Steam and Epic store nothing. Only Frontier-store users enter a password.
- The login file, `frontier-login.dat` in the launcher folder, holds the email in the
  clear and the machine token encrypted with DPAPI for the current user. Frontier's
  sign-in takes the password every time, so the password is kept the same way only
  when "Remember my password on this PC" is ticked, and is asked for otherwise.
  Unticking direct launch deletes the file.
- Tokens are on the game's command line, as they are from EDLaunch. EDVR already
  redacts that line in its logs (`runtime_profile.h`). The launcher never logs it,
  and any dry-run option masks tokens. min-ed-launcher's `/dryrun` does not mask them.
- A build test proves the login file and the launcher's log never reach the log
  bundle.

## Test plan

T1 to T4 run with min-ed-launcher on Windows. `/dryrun` prints live tokens, so its
output is not shared or pasted.

| Test | Launch | Confirms | If it fails |
|---|---|---|---|
| T1 | Steam running, `SteamAppId=359320` set, `MinEdLauncher.exe "<Steam install>\EDLaunch.exe" /steam /autorun /autoquit /edo /vr /dryrun`, from a plain shell | Prints the dry-run command line. Repeat without the variable as a control | `--wrap` is required on Steam |
| T2 | The same without `/dryrun` | The game reaches the menu with no relaunch through Steam. Steam shows it running, playtime counts, the overlay works, and SteamVR files it as `steam.app.359320` | As T1, or note what is lost |
| T3 | T2 with EDVR installed, once with `/vr` and once with `/novr` | `edvr_log.py` shows the right build and the `profile:` line with the flag applied, both logs exist with no stand-down, and `/novr` has no `openvr_api.dll` load | Direct launch changes something EDVR depends on: find what before porting |
| T4 | Frontier store, `MinEdLauncher.exe /frontier <profile> /autorun /autoquit /edo /vr`, launched twice | Finds the separate product folder. 2FA is asked once. The second launch is silent | Frontier stays on the EDLaunch route |
| T5 | Deck, the current installer through `proton runinprefix` | The window draws and works, `--collect-logs` reaches the host, and a test build can reach GitHub and verify a P-256 signature | Fix it, or rethink the one exe |
| T6 | Deck, Windows min-ed-launcher under Proton, with `EDLaunch.exe` swapped for it in Steam's `%command%` | A Windows exe in the prefix gets a ticket and starts the game | Linux stays on the EDLaunch route |
| T7 | Mac, the same launch in a CrossOver bottle | The same, with Windows Steam in the bottle | The Mac stays on the EDLaunch route |

After that, `/dryrun` is the reference for the port: its command line, with tokens
masked, must match ours. The API client gets a test rig against a local fake server
with recorded, scrubbed responses, so an unexpected response shape fails in the build
and costs no flight.

## Test results

T1 to T4, 2026-11-07, on Windows, with min-ed-launcher v0.13.0 and its automatic game
updates turned off by `/settingsOverlay`. Real launches used `/autoquit waitForExit`,
so min-ed-launcher stayed open as the launcher will. Each ran about two minutes at the
main menu.

| Test | Result | Evidence |
|---|---|---|
| T1 | Pass | Without `SteamAppId`: "Unable to initialize Steam". With it: signed in, and the dry run's command line ended `/steam /vr`. There is no `steam_appid.txt` in the game folder |
| T2 flat, Steam | Pass | The game's parent was min-ed-launcher, EDLaunch never ran, `RunningAppID` was 359320 during the run and 0 after, and `gameoverlayrenderer64.dll` was loaded |
| T3 flat, Steam | Pass | Build `v0.22.0-rc.6-326-gac4ab507`. `profile: flat (... command line /novr applied)`, no `openvr_api.dll`, gfx log present, no stand-down |
| T2 and T3 VR, Steam | Pass | `profile: vr (... command line /vr applied)`, `openvr_api.dll` and `openxr_loader.dll` loaded, both logs present. The runtime log has `steam_identity,variable=SteamAppId,source=inherited,app_id=359320`, 10,483 submitted frame pairs and a graceful close. Whether a headset was on is unknown |
| T4 flat, Frontier | Pass, with a saved login | Build `v0.22.0-rc.7-15-g4d5bd6e4`. Two silent dry runs and one real launch from `%LOCALAPPDATA%\Frontier_Developments\Products\elite-dangerous-odyssey-64`. `/novr` applied, no `/steam`, `RunningAppID` stayed 0 |

Not covered: a first Frontier login with 2FA, because a saved login already existed;
playtime counting; whether the overlay works on screen; rendering in a headset; play
past the main menu; and the Frontier launch's process parent, because that query
failed during a concurrent build. The port's source is v0.14.0, so its changes since
v0.13.0 need reading during the port.

The update path, 2026-11-07, on a throwaway public pre-release `v0.0.1-ci.2`, built by
Actions run 42385097986 from `0b9fe4c0` and signed with the main key. The launcher ran
with `--data-root` on a scratch folder. The release and its tag were deleted afterwards.

| Test | Result | Evidence |
|---|---|---|
| CI builds the assets | Pass | Core 5.9 MB, DLSS 29.4 MB, XeSS 56.9 MB, launcher 1.2 MB. All match `SHA256SUMS`, and `sign_manifest.py verify` passes every check on the signed folder |
| Published with no `.sig` | Not offered | `no release carries a signed update`, and the stable channel did not change |
| Real signature | Accepted | `Signed by: the main key`, `update available` |
| `--update` | Pass | The three zips in 13 s, all 11 files verified, manifest adopted with `highest_issued` set |
| Second check | Up to date | The digest matched, so nothing downloaded |
| Install from the cache | Pass | Into a scratch folder holding a copy of Elite's exe. The six files checked match the signed manifest, and `d3d11.dll` is stamped `v0.0.1-ci.2` |
| Real signature on a changed manifest | Refused, not conclusive | `the signature does not match a trusted key`, but GitHub's view was not confirmed settled, so this may have read the previous upload. The rig's flipped-byte case covers it exactly |
| Real manifest, signed with another key | Refused | The same message, with the API and the downloads both confirmed serving those files |
| Anonymous rate limit | Skipped | The test's repeated checks hit the real limit: `status 403`, exit 4, and no failure |
| Real folders | Untouched | No launcher or payload folder under `%LOCALAPPDATA%\EDVR`, no uninstall entry, no Start menu change |

An earlier pass at the refusal tests was refused for the wrong reason: a stale signature
from the cache lag described under Updates, which this test found and `5033812c` fixes.
The acceptance, download and install rows used the CI-built launcher at the tag. The
refusal rows used a local build with the fix, which does not touch the signature check.

Direct launch on Steam (step 4), from `18ac1c3f` with `--data-root` on a scratch folder. The
Steam install held DLL build `v0.22.0-rc.6-394-g35aa1d87` from another branch, which is not
under test here; only the launcher is.

| Test | Result | Evidence |
|---|---|---|
| Dry run, 2026-11-07 | Pass | Signed in on the first try. Six purchases, with the field names the port reads (`product_sku`, `directory`, `filter`, `gameargs`, `serverargs`, `sortkey`). The game version matched Frontier's (`2026.10.23.334925`). The command line ended `"ServerToken <masked> <masked> " /steam /vr`, and nothing was started |
| Play VR, 2026-11-08 06:35 | Pass | `command line /vr applied`, the game's arguments `<redacted> /steam /vr` with no `/language`, so EDLaunch did not start it. The runtime log has `steam_identity,source=inherited,app_id=359320`, and the OpenXR session reached VISIBLE and shut down cleanly |
| Play flat, 06:37 | Pass | `command line /novr applied`, `<redacted> /steam /novr`, and no runtime log, so `openvr_api.dll` never loaded |

Before the port touched Frontier, a second reading of min-ed-launcher's source found three
places where it differed: the purchase fields are `product_sku` and `product_name`, `fTime`
carries five decimals, and Odyssey and live Horizons share one folder, so the product is
chosen by the `edo` filter. All three were fixed before the dry run.

Not shown yet: whether Steam counts playtime and the overlay works on screen, whether
the game's language is right without the `/language` EDLaunch passes, and what each
fallback looks like for real.

Direct launch on the Frontier store (step 6), 2026-11-08, from `24925c52` with
`--data-root` on the same scratch folder, which held no Frontier login before. The
Frontier install held DLL build `v0.22.0-rc.7-57-g5432fd63` from another branch, which is
not under test here.

| Test | Result | Evidence |
|---|---|---|
| Play flat, first sign-in, 09:32 | Pass | The email, password and emailed code went into the launcher's two windows. `frontier-login.dat` was written at 09:32:44 with the email, the password and a machine token, the last two as DPAPI blobs; the launcher stores a machine token only after Frontier accepts a code. The game started at 09:32:49 with `<redacted> /novr`, no `/steam` and no `/language`, and `command line /novr applied` |
| Dry run from the saved login, VR, 09:36 | Pass | No window and no code. Signed in, six purchases, the game version matched Frontier's (`2026.10.23.334925`), and the command line would be `"ServerToken <masked> <masked> " /vr`. Nothing was started |
| Play VR | Not flown | The second Play went to the Steam install, which the window picks on every start (see Status). That run, at 09:35, is a second Steam VR pass: `<redacted> /steam /vr`, `command line /vr applied` |

## Build order

1. T1 to T4 (done).
2. Publish the new assets and the signed manifest beside today's. Nothing breaks.
   (Built, test release proven.)
3. Launcher from the installer: file cache in place of the embedded payload, update,
   self-update, and the EDLaunch route with the close after exit. It ships beside the
   installer. (Built, update path proven, not flown.)
4. Direct launch on Steam, opt-in. (Built, flown VR and flat.)
5. Pre-launch check and `--wrap`.
6. Direct launch on the Frontier store, with its login window. (Built, flown flat
   with a first sign-in, VR by dry run only.)
7. The per-eye field, the headset record and the settings table as data.
8. Linux and Mac launching, once T5 to T7 pass.
9. Retire `edvr-installer-*` once the launcher has proved itself.

## Linux and Mac

The game only runs through a Windows compatibility layer on those systems, and an
exe in the same prefix or bottle sees the same files, registry and Steam bridge.
`install_proton.py` already runs the installer that way, and the Mac flights'
`edvr-logs-*` bundles suggest it has run in a bottle too. SteamOS on Arm runs it
through Proton and FEX like the game. A native app would need its own prefix code on
Linux, an Arm build, and could not reach the Windows Steam inside a Mac bottle.

- **Starting it on Linux, as built (2026-11-08):** `ed-up-launcher.sh` runs `install_proton.py
  --launcher`, which finds Elite's Proton prefix as the installer wrapper does and runs
  `proton runinprefix ed-up-launcher.exe --dir <game>`. Arguments after a lone `--` go to the
  launcher. `package_native.py` packs it as `ed-up-launcher-linux-<version>.zip` (the same
  exe as the Windows asset, the wrapper, the script, `README-launcher-linux.txt`); it is a
  convenience package, outside the signed manifest. Swapping `EDLaunch.exe` for the launcher
  in Steam's `%command%` is not built.
- **What the launcher does differently under Wine:** it runs where it is, with no copy to
  the canonical folder, no Start menu entry and no restart, because a process it starts may
  be ended with Steam's runtime container when the first one exits (unverified). A
  self-update replaces the exe and shows "close this window and start ED-UP again". Direct
  launch is not offered: the checkbox is hidden and a saved `direct_launch=on` is ignored, so
  Play on a Steam install opens `steam://rungameid/359320`. `--direct-dry-run` stays, as the
  way to find out whether the prefix can get a ticket.
- **Unverified on a Deck:** the window under Proton; the GitHub check and the manifest
  signature from inside the prefix; Play through `steam://` leaving Wine for the host's
  Steam; the watcher seeing the game's process; a self-update; and Gaming Mode, which the
  package does not touch.
- **Gaming Mode:** the window needs a mouse, so a launch from Steam runs silently with
  the saved mode and shows a window only on an error.
- **Stored logins:** as far as we know, DPAPI under Wine is not real protection. This
  affects only Frontier-store logins.
- **Log bundles:** under Wine the bundle goes to the host's Downloads folder, found
  through `WINEHOMEDIR`. The window shows the host path and can open the folder through
  `winebrowser`. This goes into the current installer first, and the launcher inherits it.

## Ruled out

- ruled out: treating a download that differs from GitHub's listing as a refusal,
  because the test release showed GitHub's own download cache does that for a few
  minutes after an asset is replaced.
- ruled out: the game closing itself after a direct launch, because the two early
  launches that seemed to were closed by hand, and three later launches ran until the
  test closed them.
- ruled out: needing `--wrap` or `steam://` to sign in on Steam, because T1 got a
  ticket from a process Steam did not start.
- ruled out: a cross-platform SDK, because the planner, chain and ini code would need
  rewriting or duplicating beside the DLL's, and a native app still could not reach
  Steam inside a Mac bottle. One Windows exe already runs wherever the game does.
- ruled out: a separate launcher repo for branding, because the mod's releases would
  stay under the old name. Renaming this repo moves everything at once.
- ruled out: an MSI for now, because Windows Installer tracks the exe it installed and
  so conflicts with self-update, it cannot own files in game folders found at run time,
  and an unsigned MSI gets the same SmartScreen warning. First run adds a Start menu
  shortcut and a per-user uninstall entry instead.
- ruled out: bundling min-ed-launcher, because it logs in through a console, adds a
  second unsigned exe, writes tokens to its debug log, and does not help on Epic.
- ruled out: EDLaunch as the default launch route, because it is slow to load and
  stays running in spite of `/autoquit`. It stays as the fallback.
- ruled out: Settings.xml as the VR switch on a direct launch, because flags already
  work and touch nothing.
- ruled out: a game patcher in the launcher, because EDLaunch already does it and
  Steam and Epic patch their own installs.
