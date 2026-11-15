# ED Unofficial Patch (ED-UP, formerly EDVR)

ED-UP adds DLSS, FSR, and XeSS to Elite Dangerous: Odyssey, on a monitor or in
a headset. It keeps the menus and HUD sharp, draws the on-foot game in stereo,
and fixes visual problems that show up once each eye has its own view. In VR
it runs its own OpenXR session. The first install takes a few minutes.

The [player guide](wiki/Home.md) has the install steps, the headset setup, the
F8 menu, and what to do when something looks wrong. This page is the short
introduction. The guide matches release 0.22.1, made for Odyssey build 334925.
Each release's notes name the build that release matches.

ED-UP is free and stays free. If it improves your game, [tips are
welcome](https://ko-fi.com/seancharacterecho), and they are never required.
Bugs go on [an
issue](https://github.com/characterecho-sean/ed-unofficial-patch/issues/new/choose)
with the log attached. [Ask for help](wiki/Ask-for-help.md) says how to collect
it. Setup questions fit the [Discord](https://discord.gg/AWJ6djKKzr).

## Install

Close Elite first. The installer lists every file it will change and waits for
a yes. It keeps your settings, and it keeps a backup of anything it replaces.
It accepts Odyssey. It stops when it finds the older Horizons client.

| Where you play | Download | Steps |
| --- | --- | --- |
| Windows: Steam, Epic, or Frontier | The [launcher](https://characterecho-sean.github.io/ed-unofficial-patch/), or `ed-up-installer-<version>.zip` on the [release](https://github.com/characterecho-sean/ed-unofficial-patch/releases/latest) | [Install on Windows](wiki/Install-on-Windows.md) |
| Linux or Steam Deck, through Steam | `ed-up-installer-linux-<version>.zip` | [Install on Linux](wiki/Install-on-Linux.md) |
| Linux, through Epic, Heroic, Legendary, or Lutris | The Linux zip that contains `epic_library.py` | [Epic on Linux](wiki/Install-on-Linux.md#epic-heroic-legendary-and-lutris) |

Release 0.22.1's Linux package finds Steam. Epic on Linux is the package that
includes `epic_library.py`. The Windows installer already finds Steam, Epic,
and Frontier.

Press F8 in the game to change settings.
[Settings](wiki/Settings.md) is the tour, and
[docs/fixes.md](docs/fixes.md) names every key and its default.

## What you get

A steadier picture: DLSS on an NVIDIA RTX card, FSR on any card, or XeSS.
Menus and the HUD stay sharp on top of the upscaler. On foot in a headset,
the world is drawn from your commander's eyes, with look-to-aim and scopes
you can turn on. Shared brightness, terrain at the edge of the view, the jump
flash, and sun glare are part of the same install.
[What it changes](wiki/What-it-changes.md) walks through them.
[Headsets](wiki/Headsets.md) is which runtime to select before you launch.

EDHM and ReShade can stay installed. [Other mods](wiki/Other-mods.md).

After Frontier updates the game, Elite still starts. A fix that no longer
matches that build turns itself off, and the log names it. On-foot VR waits
for an ED-UP update for the new build. If a launcher verify deletes the patch,
run the installer again.
[Updates and uninstall](wiki/Updates-and-uninstall.md).

## Build

Building needs Visual Studio 2022 with the C++ workload, and Python.
[docs/building.md](docs/building.md) has the steps. For a code change, open a
pull request from your fork and say which checks you ran. C++ changes need a
full native Windows validation build. For a rendering change, name the headset
and VR runtime you flew, or say that a headset flight is still needed. The
maintainer handles merging and releases.

Some antivirus tools flag a graphics add-on that sits beside a game. The
source in this repository is how you can read it and build it yourself.

## Thanks

[arturbac](https://github.com/arturbac) diagnosed the Linux/Proton
constant-buffer readback cost in [issue
#65](https://github.com/characterecho-sean/ed-unofficial-patch/issues/65),
confirmed it through PDB/perf/LBR profiling, and supplied the cached mapping
approach, working staging patch, and Proton A/B tests in [PR
#66](https://github.com/characterecho-sean/ed-unofficial-patch/pull/66). The
adaptive map cache on main builds on that work. The
[investigation](docs/issue-65-proton-cb-shadows-2026-10-02.md#contribution-and-attribution-2026-10-02)
records the contribution and measurements.

## Licence and standing

ED-UP is MIT licensed; see [LICENSE](LICENSE). NVIDIA's DLSS runtime,
`nvngx_dlss.dll`, which the installer carries, is not: it is NVIDIA's software
under the NVIDIA RTX SDKs licence. It is distributed unmodified as part of this
application, as that licence allows, and only placed on machines with an NVIDIA
card. Intel's XeSS runtimes, `libxess.dll`, `libxess_fg.dll` and `libxell.dll`,
ship with Intel's licence and third-party notices (`INTEL-XESS-LICENSE.txt`,
`INTEL-XESS-THIRD-PARTY.txt`), and the Khronos OpenXR loader,
`openxr_loader.dll`, with its notice (`OPENXR-LOADER-LICENSE.txt`). The `fsr`
engine is AMD's FidelityFX Super Resolution 3.1 upscaler through its community
Direct3D 11 port (the optiscaler project's FidelityFX-SDK-DX11, MIT), compiled
into `d3d11.dll`; its notice ships as `FIDELITYFX-SDK-DX11-LICENSE.txt`.

Not affiliated with, endorsed by, or supported by Frontier Developments plc or
Valve Corporation. Elite Dangerous is a trademark of Frontier Developments plc.
