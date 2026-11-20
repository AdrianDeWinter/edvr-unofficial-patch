# Install on Linux

This page covers a desktop PC and a Steam Deck. ED-UP is a Windows program. On Linux its launcher runs inside the Proton prefix the game already uses. Python 3 is enough. The launcher uses the Proton already set up for Elite.

The Linux launcher is new and untested on Linux. The window, its updates and the Play button may not work yet. If one of them fails, send the log. [Ask for help](Ask-for-help.md).

## Steam

This section is for a Steam copy. That includes Flatpak Steam, when Steam's files are available to your user, and the Steam Deck. An Epic copy is [further down](#epic-on-linux).

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

`./ed-up-launcher.sh` also runs for an Epic copy of Elite. It is the same launcher, run under the Wine or Proton that the Epic copy already uses. This route is new and untested on Linux hardware, like the Steam launcher above. A Linux installer package used to do this job. The launcher package has replaced it.

The script can find:

| The game was installed with | What the script reads |
| --- | --- |
| Epic's native Linux launcher (the Preview) | Epic's install journal, and the matching Proton prefix |
| Heroic, normally or as a Flatpak | Heroic's Legendary records |
| Legendary on its own | Legendary's install list |
| Lutris, normally or as a Flatpak | Lutris's game and runner settings |
| The Windows Epic launcher in an existing Wine prefix | Epic's records inside that prefix |

The script requires `EliteDangerous64.exe` in the recorded folder. A journal folder without that executable is skipped. It then uses the Wine or Proton already recorded for that copy.

1. Run Odyssey once from that program, then close it. After you change its runner, launch it once more before you run the launcher.
2. When Steam is installed as well, name the store:

```sh
./ed-up-launcher.sh --store epic --dry-run
./ed-up-launcher.sh --store epic
```

`--dry-run` prints the command and writes nothing. `--game-dir` picks one copy when several Epic installs exist. The launcher window opens on that copy. Options for the launcher itself go after a lone `--`, for example `./ed-up-launcher.sh --store epic -- --install --scope both`.

When the records do not name the runner, pass the prefix you already play in, and the Wine or Proton that runs it:

```sh
./ed-up-launcher.sh --store epic --prefix '/existing/prefix' --wine '/path/to/wine'
```

`--proton` with an installed Proton works the same way. When a record is missing, the script reports it and stops. Launch the game once so the prefix already exists. The script uses that prefix, or the prefix and runner you pass.

Play starts Elite through Steam, so it is not for an Epic copy. Start the game from Heroic, Legendary, Lutris, or Epic, so that program can sign you in.
