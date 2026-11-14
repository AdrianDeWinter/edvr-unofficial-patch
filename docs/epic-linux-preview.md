# Epic Games Launcher Preview on Linux

## Status

2026-11-14: Epic's native Linux Preview exists. Official public documentation
and the public 0.115.0 Debian package were inspected without installing or
running the launcher. No Epic account was used and no game was launched.

Verified from the official executable: the XDG data root, installation journal
filename and recognized fields, Proton compatibility directories, and ordinary
`proton waitforexitandrun` invocation. The exact artifact-ID directory binding
and journal replacement policy have independent community implementation
corroboration; their complete official call chains were not reconstructed.

No official statement that Elite Dangerous works through this Preview was
found. A real existing Elite installation and prefix remain the next useful
verification, after deterministic discovery and dry-run tests pass.

## Official availability and installation

[Epic's Preview FAQ](https://launcher.epicgames.com/) explicitly describes a
native Linux launcher and Windows games potentially requiring a compatibility
layer. Its download link currently selects an x86-64 `.deb`. The
[support introduction](https://www.epicgames.com/help/c-32735058/c-Trending_0/a12104725)
dates availability to 2026-10-14 and also mentions AppImage packaging. That does
not establish that an AppImage is currently offered by the download page.

The FAQ currently names Ubuntu 22.04+ and Debian 12+, with Fedora coming soon.
The separate [system requirements article](https://www.epicgames.com/help/c-32735058/c-36403860/quais-sao-os-requisitos-de-sistema-para-o-epic-games-launcher-early-preview-a11320878)
also names Fedora 43+, and disagrees on required disk space. These pages are
inconsistent; Fedora support should not be promised from this research.

[Epic's release notes](https://launcher.epicgames.com/release-notes) list 0.115.0
on 2026-11-11 and Linux-specific fixes in earlier builds. The
[Elite Dangerous store page](https://store.epicgames.com/p/elite-dangerous?lang=en-US)
lists Windows requirements and a Frontier account requirement, with no native
Linux platform or Preview compatibility guarantee.

## Official artifact evidence

The [official download endpoint](https://launcher-public-service-prod06.ol.epicgames.com/launcher/api/installer/download/EpicGamesLauncher-x86_64.deb)
resolved to [Epic's versioned 0.115.0 package](https://content-download-egs.distro.on.epicgames.com/Builds/EGLv2/0.115.0%2Bgd6d2ef8/Linux/EpicGamesLauncher-x86_64.deb?launcherfilename=EpicGamesLauncher-x86_64.deb).
It was downloaded into the ignored `reviews/epic-linux-preview-20261114/`
scratch directory and unpacked there only.

| Object | SHA-256 |
| --- | --- |
| `.deb`, 89,394,442 bytes | `618adc0bcc778bbb1f49738110d5c219e861ed4cb7adb72b24fb5503156a6d63` |
| `usr/bin/EpicGamesLauncher`, ELF x86-64 | `94e71c223d3641d86ca1e87094194e884f0a772b395b51ce2955a44f66c458a5` |

The package also contains `usr/lib/epic-games-launcher/libEOSSDK-Linux-Shipping.so`,
third-party notices and a desktop entry. These are shipped paths, not evidence
that the package has been installed on this machine.

REA opened the ELF and identified its digest/architecture. Its `.deb` route
rejected that archive format; its native string/decompiler queries failed with
an execution error and oversized diagnostic response. Its separate ELF-layout
provider lacked its configured Python dependencies. Bounded GNU `nm`,
`readelf -rW` and `objdump` inspection provided the evidence below; the target
was never executed. Addresses are ELF virtual addresses for this digest only.

| Observation | Static evidence |
| --- | --- |
| Data root is `$XDG_DATA_HOME/com.epicgames.eglv2`, otherwise `$HOME/.local/share/com.epicgames.eglv2` | `app_paths::local_data_base_from_env` at `0x3b0b4f0` reads XDG/HOME and joins `.local` and `share`; `resolve_real_root` at `0x3b0b1b0` joins `com.epicgames.eglv2`. Relocation `0x5908640` resolves the first call. |
| Journal is `<data-root>/installations.jsonl` | `installations::paths_in` at `0x30c2f70`, literal at file offset `0x690278`. |
| Installation parser recognizes `installationId`, `complete`, `dir`, `platform`, `namespace`, `itemId`, `artifactId` | `Installation` deserializer field visitor at `0x30a0380`. Extra fields include `pendingVersion`, `pendingTags`, `parentInstallationId`, `sidecar`, `preload` and `writtenAt`. |
| Journal has a header format | `jsonl_txlog::Header` field visitor at `0x3041cd0` recognizes `version`, `writtenAt`, `entries`; the deletion discriminator visitor at `0x30421b0` recognizes `deleted`. Presence of a header must not resurrect or invalidate older installation records silently. |
| Compatibility data is `<data-root>/compat/compatdata/<game-string>` | `compat::compat_data_dir` at `0x2aeb420` joins `compat`, `compatdata`, then its caller's string using `download::types::safe_join`. |
| Wine prefix adds `/pfx` | `compat::wine_prefix` at `0x2aea8a0` calls that function and joins `pfx`. |
| Runtime root is `<data-root>/compat/runtime` | `compat::runtime::runtime_root` at `0x31b2630`; `CompatProfile::new` at `0x2aeaa40` calls this and `version_dir` (relocations `0x58b8738`, `0x58b7c20`). |
| Launch uses ordinary Proton | `compat::wrap` at `0x2aeb4e0` appends `proton`, adds `waitforexitandrun`, and builds `STEAM_COMPAT_DATA_PATH`, `STEAM_COMPAT_INSTALL_PATH`, `WINEPREFIX`, `WINEESYNC` and `WINEFSYNC`. |

The executable contains a built-in GE-Proton10-34 pin and code for a CDN pin,
`EGL_PROTON_PIN`, `pin-cache.json`, and installed-version selection. The
[official Linux manifest endpoint](https://launcher-public-service-prod06.ol.epicgames.com/launcher/api/installer/download/linux-manifest.txt)
also currently returns GE-Proton10-34 with a URL and SHA-512. This pin does not
prove which runtime last initialized any particular existing game prefix.

## Corroboration and remaining limits

The community project's own [game-reader source](https://github.com/DXVSI/egl-fedora/blob/7e93ede140a22bea2cc861055d40b170db4c69bd/proton-gui/src/epic.rs)
at `7e93ede140a22bea2cc861055d40b170db4c69bd` keeps the last record per
`installationId`, removes records with boolean `deleted: true`, and selects
completed Windows installations using `artifactId`, `dir`, `platform`,
`complete` and `itemId`. This is primary evidence of that project's reader,
not an Epic-supported storage contract. Its [implementation notes](https://github.com/DXVSI/egl-fedora/blob/7e93ede140a22bea2cc861055d40b170db4c69bd/docs/2026-10-16.md)
corroborate `compat/compatdata/<app_name>` and the ordinary Proton invocation.
Together with the official fields and path construction, this supports using
`artifactId` for that game-string; the official caller's field mapping remains
an inference, not a claimed full decompilation result.

[GE-Proton's own launcher source](https://github.com/GloriousEggroll/proton-ge-custom/blob/master/proton#L1370-L1399)
writes `config_info` with runtime-derived font, library and default-prefix
paths. An existing prefix's consistent paths can identify its recorded Proton
installation; an arbitrary installed runtime or the current CDN pin cannot.
EDVR's existing `tools/install_proton.py` parser is the appropriate shared owner
for this check. A missing, inconsistent or removed runtime requires an explicit
existing runtime selection rather than downloading or guessing one.

Exact header values, all torn-write/recovery rules, sparse tombstone schema,
and every `complete`/`platform` variant were not fully reconstructed from the
official binary. Discovery should reject ambiguous or malformed state, preserve
deletion semantics, and validate the actual game executable and existing prefix.
No result here establishes a successful Elite launch, Frontier authentication,
or EDVR installation through the native Preview.
