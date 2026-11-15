# Playing

Install first, on [Windows](Install-on-Windows.md) or [Linux](Install-on-Linux.md). For a headset, select the headset software before you launch. [Headsets](Headsets.md).

## Headset or monitor

ED-UP follows the mode Elite actually starts in.

Steam's VR option forces the headset for that launch. Steam's normal option forces the monitor. Pick the one you want. After a monitor launch, Elite's 3D menu hides the headset choice until you start again with the VR option. After a headset launch, turning 3D off and on in Elite's graphics options switches ED-UP's fixes within about half a second. A few settings wait for the next launch, and the menu says so.

On a **VR and flat** install, the headset reads `edvr.ini` and the monitor reads `edvr-flat.ini`. You can tune them apart from each other.

## Did it load?

Logs are written to `edvr_logs`, next to the game.

A headset session writes two logs. When ED-UP's headset path is up, the log with `vr` in the name contains `runtime,<name>,<version>`. That is the headset software it reached. An `error,` line in that spot means that step failed.

A headset session with only one log means Elite started on a path ED-UP does not serve. Read [Headsets](Headsets.md).

The menu is the other check. Press **F8**. If the ED-UP panel opens, the patch is running. [Settings](Settings.md) lists the keys.

## The first time on foot in a headset

With on-foot VR left on, the first headset launch opens an **Aiming on foot** page during the splash screen. It explains look-to-aim and mouse aim and asks you to pick one. Either choice stops the page coming back. **Ask me next time** keeps it. You can change the choice later on the Native on foot page.

## A spare copy of your settings

The installer keeps a copy of your settings outside the game folder. On Windows that is `%LOCALAPPDATA%\EDVR`. On Linux it is inside the Proton prefix, at `drive_c/users/steamuser/AppData/Local/EDVR`. If a game update deletes the files beside the game, the installer can put them back.
