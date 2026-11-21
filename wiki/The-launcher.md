# The launcher

The launcher installs ED-UP, keeps it up to date, and has a Play button. You can start Elite yourself and ignore Play, or copy the files by hand from `ed-up-<version>.zip` instead. [Install on Windows](Install-on-Windows.md) and [Install on Linux](Install-on-Linux.md) say how.

## What it talks to

It asks GitHub whether a newer release exists, and it downloads a release only after you press Download. Each request carries the launcher's name and version. A release is offered only when its signature matches a public key built into the launcher. A changed or substituted release is refused. If GitHub cannot be reached, the launcher says so in one line and carries on.

With direct launch turned on, described below, it also signs in to Frontier at `api.zaonce.net`. That is the whole list.

## Play

Play starts Elite through Steam, Epic, or Frontier's launcher, and waits until the game exits. It closes the Frontier launcher window left behind. While you play, the launcher sits minimised on the taskbar and returns when Elite exits. **Close after start** closes the launcher once Elite is running.

It reopens on the install you last pressed Play for.

From a script, Steam Launch Options, or another launcher such as `min-ed-launcher`, `--headless` starts Elite the way Play does with no launcher window, and waits until Elite exits.

- Pass `--vr` / `/vr` or `--novr` / `/novr` to pick the mode, or omit them to use the installed scope, the last mode used for that folder, or Elite's `Settings.xml`.
- Add `--close-after-start` to exit as soon as Elite is running.
- Add `--update` to check for and download a signed release before launching (non-blocking if offline).
- Add `--frontier-login` on a Steam install to sign in with a Frontier email/password instead of Steam's session ticket.
- In Steam's **Launch Options**, wrap `%command%` using the full path to `ed-up-launcher.exe` (Steam does not expand `%LOCALAPPDATA%` in Launch Options; replace `<your-name>` with your Windows user name) so Steam tracks playtime and injects the Steam Overlay / Steam Input virtual menus:

```text
"C:\Users\<your-name>\AppData\Local\EDVR\launcher\ed-up-launcher.exe" --headless %command%
```

- As a pre-launch update hook in `min-ed-launcher` (without launching the game through ED-UP):

```text
"%LOCALAPPDATA%\EDVR\launcher\ed-up-launcher.exe" --update --install --dir "C:\Path\To\Products\elite-dangerous-odyssey-64"
```

Every store offers **Play VR** and **Play flat** (filtered to the modes installed in that folder). Frontier's launcher takes `/vr` or `/novr` on its command line. For Steam and Epic without direct launch, the launcher updates `<StereoscopicMode>` in Elite's `Settings.xml` before opening Steam or Epic, keeping a one-time original backup at `backups\Settings.xml.ed-up-original` and a rolling backup of the previous state at `backups\Settings.xml.bak`:

- **Play flat** writes `0` (Off) into `<StereoscopicMode>` without changing your saved VR preference.
- **Play VR** writes your chosen **VR 3D mode** from the Launch panel (saved as `vr_3d_mode` in `launcher.ini`):
  - **Not Set** (`not_set`, default): stops before launching in VR and prompts you to choose your preferred 3D mode first.
  - **HMD (Headphones)** (`hmd_headphones`)
  - **HMD (Speakers)** (`hmd_speakers`)
  - **HMD Cinema (Headphones)** (`hmd_cinema_headphones`)
  - **HMD Cinema (Speakers)** (`hmd_cinema_speakers`)

See [Playing](Playing.md).

An Epic copy is handed to the Epic launcher. Play does not ask for a Frontier password for that install.

## Starting through Steam, past the Steam window

On Windows, a checkbox can start the game directly and let you pick headset or monitor. The launcher then signs in to Frontier with your Steam login. It sends what Frontier's own launcher sends: a sign-in ticket from Steam, and an ID for this PC. It stores no password and no token. The checkbox stays off until you turn it on. Frontier has not approved this sign-in.

If your Steam-installed copy uses a separate Frontier email/password account (such as a Frontier-store or alternate account), tick **Use Frontier account login** under Launch (or pass `--frontier-login`).

Because direct launch starts `EliteDangerous64.exe` directly outside Steam, Steam does not inject its overlay unless the launcher itself is invoked through Steam's Launch Options (`--headless %command%` above). If you rely on Steam Input virtual menus, either wrap `%command%` or leave direct launch unchecked so Steam launches Elite itself.

If any part of the sign-in fails, the launcher says why and starts the game through Steam instead.

The sign-in comes from [min-ed-launcher](https://github.com/rfvgyhn/min-ed-launcher) (MIT). `ed-up-launcher.exe --licenses` prints that licence.

## Frontier's own store

The same checkbox exists for a game bought from Frontier. The launcher asks for your Frontier email and password in its own window and sends them to Frontier's servers, the way Frontier's launcher does.

The first time on a PC, Frontier emails a code. The launcher asks for it and sends it back. Frontier answers with a token for this PC.

The launcher keeps `frontier-login.dat` in `%LOCALAPPDATA%\EDVR\launcher\`:

- your email, stored in the clear
- that token, locked to your Windows user
- your password, locked the same way, only while **Remember my password on this PC** is ticked

The lock is Windows' protection for the current user. A program running as you can still ask Windows to unlock it. Under Wine or Proton that protection is weak. The password is sent on every sign-in, saved or typed.

If Frontier refuses the saved login, the launcher forgets the saved password and token and asks again. Unticking the checkbox deletes `frontier-login.dat`. Removing the launcher deletes it too.

If a sign-in fails for another reason, the launcher says why and starts Frontier's launcher with your headset or monitor choice. Cancelling a window starts nothing.

## Linux

Download `ed-up-launcher-linux-<version>.zip` from the [release](https://github.com/characterecho-sean/ed-unofficial-patch/releases/latest). Run Elite once from Steam, close it, then:

```sh
./ed-up-launcher.sh
```

`--dry-run` prints what would run and writes nothing. Options for the launcher itself go after a lone `--`, for example `./ed-up-launcher.sh -- --channel rc`.

It runs inside Elite's Proton prefix. Downloads and settings stay there, under `drive_c/users/steamuser/AppData/Local/EDVR` in `steamapps/compatdata/359320/pfx`.

Play starts Elite through Steam. Starting the game directly, without Steam, is a Windows checkbox. On Linux, Play is for a Steam install. An Epic copy can be installed with the launcher, which is run under the copy's own Wine or Proton ([Epic on Linux](Install-on-Linux.md#epic-on-linux)), and it starts from Heroic, Legendary, Lutris or Epic.

The window, the updater, and Play are new on Linux. If one of them fails, send the log. [Ask for help](Ask-for-help.md).

## Removing the launcher

Remove **ED-UP** from Windows' list of installed programs, or run:

```text
ed-up-launcher.exe --unregister
```

That removes the Start menu shortcut, the launcher's files, and a saved Frontier login. The patch stays in the game folders, and your settings copies stay.
