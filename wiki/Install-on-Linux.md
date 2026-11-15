# Install on Linux

This page covers a desktop PC and a Steam Deck. ED-UP is a Windows program. The Linux package runs that installer inside the Proton or Wine prefix the game already uses. Python 3 is enough. The package uses the Proton or Wine already set up for Elite.

## Steam

Release 0.22.1 installs a Steam copy. That includes Flatpak Steam and the Steam Deck.

1. Install Odyssey in Steam, run it once, and close it. If you change Elite's Proton version, launch it once more and close it, so the prefix matches that Proton.
2. On a Steam Deck, switch to Desktop Mode.
3. Download `ed-up-installer-linux-<version>.zip` from the [release](https://github.com/characterecho-sean/ed-unofficial-patch/releases/latest). Check it against `SHA256SUMS`.
4. Unzip it. In that folder, run:

```sh
./edvr_installer.sh
```

Run it as your normal user. The installer window opens with your Odyssey folder selected.

To install from the terminal:

```sh
./edvr_installer.sh --install --scope both
./edvr_installer.sh --install --scope flat
./edvr_installer.sh --install --scope vr
```

`both` is headset and monitor. `flat` is the monitor. `vr` is the headset.

To see the plan first:

```sh
./edvr_installer.sh --dry-run
```

`--dry-run` prints the command and writes nothing. `--preview` starts Proton and shows the installer's plan. The game files stay as they are. Prefix bookkeeping can still update.

Start Elite from Steam afterwards. [Playing](Playing.md) says how to tell that the patch loaded.

The optional [launcher](The-launcher.md) can start a Steam copy. Download `ed-up-launcher-linux-<version>.zip`, run Elite once from Steam, close it, and run `./ed-up-launcher.sh`.

### If Steam is in an unusual place

The script reads Steam's library list, including Flatpak Steam. When the prefix record is missing or stale, point it at the Proton you use:

```sh
./edvr_installer.sh --proton '/path/to/Proton'
```

For a custom prefix, pass the compatdata folder, the one that contains `pfx`. `pfx/drive_c` has to exist already:

```sh
./edvr_installer.sh --store steam --prefix '/path/to/compatdata/359320'
```

`--steam-dir`, or the variable `STEAM_DIR`, selects a Steam folder in a nonstandard place. `--game-dir` selects one product folder when several are installed.

The script uses the Proton that last set up that prefix. Launch Elite once so that prefix already exists. Flatpak Steam runs inside Steam's sandbox. If the script cannot tell which compatibility tool is installed, it stops and says so. `--proton` selects a runner on the computer and skips the sandbox.

## Epic, Heroic, Legendary, and Lutris

Look in the unzipped folder.

If `epic_library.py` is beside `edvr_installer.sh`, this package can install an Epic copy. Use the steps below. If that file is absent, you have the Steam-only package shipped with release 0.22.1. Use the Steam steps above. Epic on Linux is the installer that includes `epic_library.py`.

That package can find:

| The game was installed with | What the script reads |
| --- | --- |
| Epic's native Linux launcher (the Preview) | Epic's install journal, and the matching Proton prefix |
| Heroic, normally or as a Flatpak | Heroic's Legendary records |
| Legendary on its own | Legendary's install list |
| Lutris, normally or as a Flatpak | Lutris's game and runner settings |
| The Windows Epic launcher in an existing Wine prefix | Epic's records inside that prefix |

A journal folder on its own is not an install. The script checks that `EliteDangerous64.exe` is there, then uses the Wine or Proton already recorded for that copy.

1. Run Odyssey once from that program, then close it. After you change its runner, launch it once more before you install.
2. When Steam is installed as well, name the store:

```sh
./edvr_installer.sh --store epic --dry-run
./edvr_installer.sh --store epic --install --scope both
```

`--game-dir` picks one copy when several Epic installs exist.

When the records do not name the runner, pass the prefix you already play in, and the Wine or Proton that runs it:

```sh
./edvr_installer.sh --store epic --prefix '/existing/prefix' --wine '/path/to/wine'
```

`--proton` with an installed Proton works the same way. When a record is missing, the script reports it and stops. Pass `--prefix` with `--wine`, or `--proton`, and run it again.

Start the game afterwards from Heroic, Legendary, Lutris, or Epic, so that program can sign you in. The Linux ED-UP launcher starts Steam copies. Epic copies stay with the program that installed them.

Headset software under Proton is your own setup. This package places ED-UP's files and leaves Elite's launch options as they are.

## Repair, uninstall, and logs

```sh
./edvr_installer.sh --repair
./edvr_installer.sh --uninstall
./edvr_installer.sh --collect-logs
```

Add `--store epic` when both stores are installed and the package includes `epic_library.py`. **Save logs** writes a zip to your Downloads folder, or to your home folder when Downloads is missing, and prints the path.
