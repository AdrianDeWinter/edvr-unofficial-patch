# ED Unofficial Patch (ED-UP formerly EDVR)

## Upscaling and fixes for Elite Dangerous, in VR and flat

ED-UP adds DLSS, FSR and XeSS upscaling to Elite Dangerous in VR and on a flat
screen, draws Odyssey's on-foot game in stereo in a headset, and fixes about
two dozen things that are right on a monitor and wrong in a headset. In VR it
is a native OpenXR package. It takes about three minutes to install.

Bugs get fixed through [an
issue](https://github.com/characterecho-sean/ed-unofficial-patch/issues/new/choose),
because an issue can carry the log, and the log usually shows the cause;
[Reporting a problem](#reporting-a-problem) says what to attach. The
[Discord](https://discord.gg/AWJ6djKKzr) is for setup questions, "is this
normal", or just talking about it.

ED-UP is free and stays free. If it improves your game, [tips are
welcome](https://ko-fi.com/seancharacterecho), but please do not feel any
obligation.

## What it does

[docs/fixes.md](docs/fixes.md) describes every feature and fix, with its
setting and its default:

- [Upscaling with DLSS, FSR or
  XeSS](docs/fixes.md#upscaling-dlss-fsr-and-xess), and an interface kept sharp
  on top of it.
- [Native on-foot VR](docs/fixes.md#native-on-foot-vr): the on-foot game in
  stereo from your commander's eyes, with look to aim and scopes that zoom.
- Fixes for things [the two eyes see
  differently](docs/fixes.md#the-two-eyes-disagreeing), things [pinned to your
  face](docs/fixes.md#drawn-on-your-face-instead-of-in-the-world) and [terrain
  missing at the edges of view](docs/fixes.md#over-a-planet).
- [Visual options](docs/fixes.md#visual-options): world scale, star glow, the
  Horizons cockpit glass and the desktop mirror window.
- [What ED-UP changes in the running game](docs/fixes.md#what-the-fixes-touch),
  and what it never touches.

## Headsets and VR runtimes

Elite still starts VR through its OpenVR path, and ED-UP keeps that entry
point; behind it, ED-UP's own runtime runs the OpenXR session. The bundled
Khronos loader reaches whichever runtime Windows has set as its active OpenXR
runtime, whether SteamVR, Pimax's PiOpenXR, Virtual Desktop's VDXR or Meta's,
so there is no backend to choose. Elite's own Oculus (LibOVR) path is not
supported, even as a fallback.

ED-UP's native OpenXR route has been checked in the field on Pimax over
PiOpenXR, Quest 3 over Virtual Desktop's VDXR, and the latest Air Link flight.
Nothing is claimed for other headsets and runtimes.

Before launching Elite, set the Windows active OpenXR runtime to the one you
intend to use, and make sure that runtime is available and your headset
connected. ED-UP needs no SteamVR loader; do not install OpenComposite or a
vendor OpenXR loader to make it work. Then launch Elite as you normally would.

The installer checks the game's executable and refuses Elite Dangerous
(Horizons), or an executable it cannot read, before it writes anything. An
Odyssey build that ED-UP was not made for installs with a warning: the game
starts, the fixes tied to the game build stand down and the log names each one,
and the Oculus PC runtime may bypass ED-UP's OpenXR runtime until an ED-UP
update for that build.

Canted Pimax headsets (Vision 8K X, 8K Plus, 8K, 5K Super, 5K Plus, 5K XR and
Artisan) should turn "Enable Parallel Projection" off in Pimax Play and restart
the game. ED-UP renders canted displays correctly without it. With it on, the
headset renders much larger frames (8268x3948 instead of 5016x3160 per eye on
an 8K X at FOV Wide), and turning it off cuts GPU time by up to about 30%. This
works with PimaxXR as the OpenXR runtime; with Pimax Play's own OpenXR runtime
the toggle may have no effect. When ED-UP sees the setting on with PimaxXR, it
shows a notice in the headset.

## Install

Download `ed-up-installer-<version>.zip` from the release, unzip it, and run
`edvr-installer.exe`. It is a single file with nothing else to extract, and it
carries ED-UP's graphics DLL and OpenXR runtime, the bundled OpenXR loader and
its notice, `edvr.ini`, `edvr-flat.ini`, and the DLSS and XeSS runtimes. It
finds the game and keeps your settings and other mods working. Before it
changes anything it shows you exactly what it is about to do and waits for a
yes, and it copies every file it replaces into `edvr_backup\` first.
[docs/installer.md](docs/installer.md) covers its buttons and explains what it
decides and why.

The same installer serves flat-screen players. Under "Install ED-UP for:",
choose "VR and flat" (the default), "VR only" or "Flat only"; an ordinary
Update can change the choice later, and no settings file is deleted by it.

Because the installer is unsigned, Windows will say the program is
unrecognised; choose More info, then Run anyway. Every release lists the
installer's SHA-256, and without a signature that hash is the only way to check
where the file came from.

To place the files yourself, follow
[docs/manual-install.md](docs/manual-install.md), which is the same install
done by hand. It shows where each file goes and covers the `openvr_api.dll`
rename that most manual installs get wrong. If you run EDHM, read [Running
alongside other mods](#running-alongside-other-mods) first.

### The launcher

`ed-up-launcher.exe`, a separate download on the release page, is the
installer's window with two additions: it keeps itself and the ED-UP files up
to date, and it has a Play button. It carries no files of its own. The first
time it runs it copies itself to `%LOCALAPPDATA%\EDVR\launcher\`, adds a Start
menu shortcut and an entry in Windows' list of installed programs, and starts
from there; removing it from that list (or `ed-up-launcher.exe --unregister`)
takes all of that away again, and leaves ED-UP in your game folders and your
settings copies alone.

It contacts `api.github.com`, to ask this project's releases whether a newer
one exists, and GitHub's release downloads, for the files of a release you
chose to install. Each request to GitHub carries the launcher's name and
version and nothing that identifies you or your PC. With direct launch turned
on (below) it also signs in to Frontier's servers, `api.zaonce.net`, and
nothing else. Nothing is downloaded or changed until you press Download. A
release is offered only if its manifest carries a signature that verifies
against a public key inside the launcher, so a changed or substituted release
is refused. If GitHub cannot be reached the launcher says so in one line and
carries on.

Play starts the game through Steam, Epic or Frontier's own launcher, and
watches until it exits so that it can close the Frontier launcher window left
behind. Only Frontier's launcher takes a VR or flat choice from the launcher;
through Steam or Epic the game starts in the mode it is already set to. While
the game runs, the launcher waits minimised on the taskbar, and it comes back
when the game exits. It opens on the install you last pressed Play for.

On Steam, a checkbox can start the game directly instead, with VR or flat as
you choose. The launcher then signs in to Frontier's servers with your Steam
login, sending what Frontier's own launcher sends (a sign-in ticket from Steam
and an ID for this PC), and starts the game itself. It is off until you turn it
on, and Frontier has not approved it. No password or token is stored. If any
part of the sign-in fails, the launcher says why and starts the game through
Steam instead. The sign-in is ported from
[min-ed-launcher](https://github.com/rfvgyhn/min-ed-launcher) (MIT licence;
`ed-up-launcher.exe --licenses` prints it).

The same checkbox is there for a game installed through Frontier's own store.
The launcher then asks for your Frontier email and password in its own window
and sends them to Frontier's servers as Frontier's launcher does, as hex in the
request, and nowhere else. The first time on a PC, Frontier emails you a code,
which the launcher asks for and sends back; Frontier answers with a token for
this PC. The launcher keeps `frontier-login.dat` in its own folder
(`%LOCALAPPDATA%\EDVR\launcher\`): your email in the clear, that token
encrypted for your Windows user, and your password, encrypted the same way,
only if you leave "Remember my password on this PC" ticked. Encrypted here
means Windows' DPAPI: a program running as you can still ask Windows to decrypt
it, and under Wine or Proton it is not strongly protected. The password is sent
with every sign-in, saved or typed. If Frontier refuses the saved login, the
launcher forgets the saved password and token and asks again. Unticking the
checkbox deletes `frontier-login.dat`, and so does `--unregister`. If a sign-in
fails for any other reason, the launcher says why and starts Frontier's
launcher with your VR or flat choice instead; cancelling a window starts
nothing.

### Linux on Steam (Proton)

Download `ed-up-installer-linux-<version>.zip` from the release and unzip it.
It contains `edvr_installer.sh`, `edvr-installer-linux.py`, the Windows
`edvr-installer.exe`, and `README-linux.txt`. The script needs Python 3 and the
Proton installation Steam already uses for Elite. It needs no Protontricks or
system Wine. Run Elite once from Steam, then close it. On Steam Deck, use
Desktop Mode and run as your normal user:

```sh
./edvr_installer.sh
```

This opens the Windows installer's window with the Odyssey product folder
preselected. For a flat install with a current installer build:

```sh
./edvr_installer.sh --install --scope flat
```

`--preview` shows the Windows install plan without changing game files (it
starts Steam's Proton and runtime, and may update prefix bookkeeping), and
`--dry-run` prints the exact launch command without starting anything or
writing files. The script forwards every option to the Python wrapper, `python3
edvr-installer-linux.py`, which you can also run directly; `README-linux.txt`
in the ZIP lists them.

The launcher runs the same way. Download `ed-up-launcher-linux-<version>.zip`,
unzip it, run Elite once from Steam and close it, then run
`./ed-up-launcher.sh` (`--dry-run` shows what would run; options for the
launcher itself go after a lone `--`). It runs inside Elite's Proton prefix and
keeps its downloads and settings there, under
`drive_c/users/steamuser/AppData/Local/EDVR`. Play starts Elite through Steam;
direct launch is not offered on Linux yet. The Linux launcher is new, so if its
window, updates or Play do not work, logs and reports are welcome. See
`README-launcher-linux.txt` in the ZIP.

### Checking it worked

Logs appear in `edvr_logs\` next to the game. A VR session writes two. Once the
native OpenXR path is up, the second one, with `vr` in its name, says
`runtime,<name>,<version>`, naming the runtime it reached; an `error,` line in
its place means that startup step failed. If a VR session leaves no second log
at all, the game is not on its OpenVR path and half the patch never loaded: see
[Headsets and VR runtimes](#headsets-and-vr-runtimes).

### Reporting a problem

Run `edvr-installer.exe` and press Save logs. It writes one zip to your Desktop
with the last session's two logs, `edvr_breadcrumbs.txt`, any `edvr_FATAL.txt`,
your `edvr.ini` and, on a VR install, the OpenXR runtime's `edvr_openxr.ini`,
all from the right session. Under Wine or Proton the zip goes to your Downloads
folder instead. Then open an issue and attach the zip. The zip is also copied
to your clipboard, so you can press Ctrl+V in a Discord message or a GitHub
comment to attach it.

If the game crashed in that session, Save logs also writes a second zip,
`edvr-crash-dump-<time>.zip`, with the game's crash dump. The dump holds a copy
of the game's memory, so it is kept out of the log zip: attach only the log zip
to a public issue, and send the dump privately if you are asked for it.

To do it by hand, attach `edvr_logs\` (both files if there are two). The log
records the build stamp, which fixes ED-UP managed to install, and what each
one decided, so a report with the log attached can usually be diagnosed in one
pass; without it there is very little to go on. If the game will not start at
all, send `edvr_breadcrumbs.txt` from next to `EliteDangerous64.exe`. It is
written unbuffered, so it survives a crash that eats the log.

If something has gone wrong and the log has not answered it,
[docs/troubleshooting.md](docs/troubleshooting.md) covers the faults with a
known cause and what each one needs, among them a game that dies a second or
two after launch and VR that fails to start after an ED-UP update.

### Uninstall

Run `edvr-installer.exe` and press Uninstall. It removes ED-UP's files and
renames the game's `openvr_api.dll` back. If another mod was chained behind
ED-UP, it also puts that mod back under its own name, the step a manual
uninstall usually forgets. It leaves your `edvr.ini` alone unless you ask for
it to go, so a reinstall finds your settings again.

To uninstall by hand, delete `d3d11.dll` and `edvr.ini` from the game folder,
then, in whichever `Openvr` folder you used, delete ED-UP's `openvr_api.dll`
and rename `openvr_api_orig.dll` back.

## Settings

Settings are in `edvr.ini` next to the game, and the flat profile has its own
`edvr-flat.ini`; a missing file means the defaults. Press F8 in the game to
change them from a menu, in the headset or on a flat screen, or use the
installer's settings window. [Settings and the F8
menu](docs/fixes.md#settings-and-the-f8-menu) describes the menu, and each
feature in that document names its setting.

## Game updates

ED-UP is made for one Elite build at a time; 0.22.0 is made for build 334925.
After a game update the game still starts with ED-UP. Each fix that hooks the
game's code looks for its target by the shape of the code, and one that cannot
find it stands down while the rest carry on, with the log naming each. Native
on-foot VR needs the exact build and waits for an ED-UP update. [Surviving a
game update](docs/fixes.md#surviving-a-game-update) has the detail.

Frontier's launcher may remove `d3d11.dll` when it verifies the install.
Nothing is broken when it does: it has simply uninstalled ED-UP. Run the
installer again (or copy the file back). The installer keeps a copy of your
settings files in `%LOCALAPPDATA%\EDVR` and offers to restore them if an update
has removed them from the game folder.

## Running alongside other mods

EDHM also installs itself as `d3d11.dll`, and only one file can have that name.
The installer handles this: it recognises what is in the `d3d11.dll` slot,
renames it, writes the setting, and tells you what it did. ED-UP then passes
everything through EDHM.
[docs/manual-install.md](docs/manual-install.md#running-alongside-edhm-by-hand)
has the same procedure by hand, one rename and one setting.

EDHM's uninstaller runs `del d3d11.dll`, which after this is ED-UP's file. If
that has happened, leaving ED-UP gone and EDHM still parked under the renamed
file, the installer's Repair recognises it and puts both back.

ReShade needs no configuration. Install it the way ReShade tells you to
(normally as `dxgi.dll`) and ED-UP composes with it, so both mods' effects
apply.

## Antivirus

A DLL that sits next to a game executable and intercepts graphics calls looks,
structurally, like something worth flagging, and some scanners will flag it.
The source is here so you can read exactly what it does and build it yourself.

## Build

Building needs Visual Studio 2022 with the C++ workload, and Python.
[docs/building.md](docs/building.md) has the steps.

To contribute code, start with that guide, make the change in your fork, and
open a pull request with the checks you ran. C++ changes need a full native
Windows validation build. For rendering changes, say which headset and VR
runtime you tested, or say that a headset flight is still needed. The
maintainer handles merging and releases.

## Contributor credit

[arturbac](https://github.com/arturbac) diagnosed the Linux/Proton
constant-buffer readback cost in [issue
#65](https://github.com/characterecho-sean/ed-unofficial-patch/issues/65),
confirmed it through PDB/perf/LBR profiling, and supplied the cached mapping
approach, working staging patch and Proton A/B tests in [PR
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
`INTEL-XESS-THIRD-PARTY.txt`), and the Khronos OpenXR loader, `openxr_loader.dll`, with its notice
(`OPENXR-LOADER-LICENSE.txt`). The `fsr` engine is AMD's FidelityFX Super
Resolution 3.1 upscaler through its community Direct3D 11 port (the optiscaler
project's FidelityFX-SDK-DX11, MIT), compiled into `d3d11.dll`; its notice
ships as `FIDELITYFX-SDK-DX11-LICENSE.txt`.

Not affiliated with, endorsed by, or supported by Frontier Developments plc or
Valve Corporation. Elite Dangerous is a trademark of Frontier Developments plc.
