# Install on Linux

This page covers a desktop PC and a Steam Deck. ED-UP is a Windows program. On Linux its launcher runs inside the Proton prefix the game already uses. Python 3 is enough. The launcher uses the Proton already set up for Elite.

The Linux launcher is new and untested on Linux. The window, its updates and the Play button may not work yet. If one of them fails, send the log. [Ask for help](Ask-for-help.md).

## Steam

The Linux launcher installs for a Steam copy. That includes Flatpak Steam, when Steam's files are available to your user, and the Steam Deck.

1. Install Odyssey in Steam, run it once, and close it. If you change Elite's Proton version, launch it once more and close it, so the prefix matches that Proton.
2. On a Steam Deck, switch to Desktop Mode.
3. Download `ed-up-launcher-linux-<version>.zip` from the [release](https://github.com/characterecho-sean/ed-unofficial-patch/releases/latest). Check it against `SHA256SUMS`.
4. Unzip it. In that folder, run:

```sh
./ed-up-launcher.sh
```

Run it as your normal user, without sudo. The launcher window opens on your Odyssey folder. Install, Update, Repair and Uninstall are buttons in that window. The launcher installs from a release it has downloaded: press **Download** when it says an update is available.

To see what would run first:

```sh
./ed-up-launcher.sh --dry-run
```

`--dry-run` prints the command and writes nothing. Options for the launcher itself go after a lone `--`, for example `./ed-up-launcher.sh -- --channel rc`.

Start Elite from Steam afterwards, or press **Play**, which starts it through Steam. [Playing](Playing.md) says how to tell that the patch loaded. [The launcher](The-launcher.md#linux) has the rest.

### If Steam is in an unusual place

When the prefix record is missing or stale, point the script at the Proton you use:

```sh
./ed-up-launcher.sh --proton '/path/to/Proton'
```

`--proton` accepts the Proton directory or its `proton` script. For a custom prefix, use `--store steam --prefix` with the existing compatdata folder, the one that contains `pfx`. `pfx/drive_c` has to exist already. Launch Elite once so that prefix exists. The script uses the Proton recorded for it.

`--steam-dir`, or the variable `STEAM_DIR`, selects a Steam folder in a nonstandard place. `--game-dir` selects one product folder when several are installed.

Headset software under Proton is your own setup. This package places ED-UP's files and leaves Elite's launch options as they are.

## Repair, uninstall, and logs

Repair and Uninstall are buttons in the launcher window.

**Save logs** writes a zip to your Downloads folder, or to your home folder when Downloads is missing. **Copy path** and **Show folder** appear beside it afterwards.

## Epic on Linux

There is no installer for an Epic copy of Elite on Linux. Earlier releases carried a Linux installer that could install one. It is gone, and the Linux launcher is for Steam only. Given a copy installed through Epic's own launcher, Heroic, Legendary or Lutris, `./ed-up-launcher.sh` says so and stops. When a Steam copy and an Epic copy are both installed, it asks you to add `--store steam`.

Placing the files yourself is [written up separately](https://github.com/characterecho-sean/ed-unofficial-patch/blob/main/docs/manual-install.md). `ed-up-<version>.zip` on the release holds them. That page is written for Windows paths, and it has not been tested on an Epic copy under Linux.

Start the game from Heroic, Legendary, Lutris, or Epic, so that program can sign you in.
