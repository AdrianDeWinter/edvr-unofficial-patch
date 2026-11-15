# Install on Windows

Use this page for Steam, the Epic Games Launcher, and Frontier's own launcher. Linux and the Steam Deck are on [Install on Linux](Install-on-Linux.md).

## Get ED-UP

Two downloads do the same install.

The **launcher** is the one most people want. It installs ED-UP, offers later updates, and has a Play button.

1. Download [ed-up-launcher.exe](https://github.com/characterecho-sean/ed-unofficial-patch/releases/latest/download/ed-up-launcher.exe), or use the [download page](https://characterecho-sean.github.io/ed-unofficial-patch/).
2. On the [release](https://github.com/characterecho-sean/ed-unofficial-patch/releases/latest), open `SHA256SUMS` and compare it with the file's SHA-256. That hash is how you check the download is the one this project published.
3. Windows reports the program as unrecognised, because it is unsigned. Choose **More info**, then **Run anyway**.
4. The first run copies the launcher to `%LOCALAPPDATA%\EDVR\launcher\`, adds a Start menu shortcut, and adds an entry in Windows' list of installed programs. Later runs start from there.

The **installer zip** is the same install without the Play button.

1. On the release, download `ed-up-installer-<version>.zip` and check it against `SHA256SUMS`.
2. Unzip it and run `edvr-installer.exe`. That one file holds the patch. If Windows asks, choose **More info**, then **Run anyway**.

## Close the game

Exit Elite completely. The installer stops while the game is running. If you already closed the window and it still refuses, look in Task Manager for `EliteDangerous64.exe`.

## Let it find Odyssey

The installer looks the game up in three places:

- Steam, including libraries on other drives
- The Epic Games Launcher
- Frontier's launcher

It offers a folder only when `EliteDangerous64.exe` is there. Odyssey is listed first. When you have more than one copy, the window pre-selects the copy that already has ED-UP. You can pick another, or browse to the folder. A folder that is not Odyssey is refused. An Epic copy can use any folder name. The installer reads Epic's own install record.

## Choose headset, monitor, or both

Under **Install ED-UP for:**

| Choice | What you get |
| --- | --- |
| VR and flat | Both. This is the usual choice. Elite's own 3D setting decides which one runs. |
| VR only | The headset fixes. |
| Flat only | The monitor fixes. The game's headset files stay as Frontier shipped them. |

You can change this later with Update. A change of choice leaves your settings files in place.

Read the list of files, then confirm. The installer copies anything it replaces into `edvr_backup`, then writes the patch.

Your choices live in `edvr.ini` (headset) and `edvr-flat.ini` (monitor) next to the game. A later install keeps those files unless you ask it to replace them. A spare copy is kept under `%LOCALAPPDATA%\EDVR`, so a game update that wipes the game folder can be put back.

## Start Elite

Start the game the way you already do.

- **Steam.** Pick Steam's VR launch or its normal launch, matching how you want to play. That choice decides headset or monitor for the session. [Playing](Playing.md) has the detail.
- **Epic.** Start from the Epic Games Launcher. An Epic copy of Odyssey starts through Epic.
- **Frontier.** Start from Frontier's launcher.

Then read [Playing](Playing.md). The optional Play button is [the launcher](The-launcher.md).

## A command instead of the window

The window is enough. The same program also accepts:

```text
edvr-installer.exe --install --scope both
edvr-installer.exe --repair
edvr-installer.exe --uninstall
edvr-installer.exe --collect-logs
```

`--scope` is `both`, `vr`, or `flat`. With one install on the PC, you can omit `--dir`. With several copies, pass `--dir` and the product folder, or use the window. `--dry-run` prints the plan and writes nothing.

Placing each file yourself is [written up separately](https://github.com/characterecho-sean/ed-unofficial-patch/blob/main/docs/manual-install.md). Use the installer unless you have a reason to copy the files by hand.
