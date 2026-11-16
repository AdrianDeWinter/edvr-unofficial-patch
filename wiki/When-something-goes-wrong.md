# When something goes wrong

If you are going to ask someone, collect the log first. [Ask for help](Ask-for-help.md) is one button. The cases below have a known next step. Past them, the [technical notes](https://github.com/characterecho-sean/ed-unofficial-patch/blob/main/docs/troubleshooting.md) list the edits that need `edvr.ini`.

## Windows blocked the program

The launcher is unsigned. Windows calls them unrecognised. Choose **More info**, then **Run anyway**. Compare the file with `SHA256SUMS` on the [release](https://github.com/characterecho-sean/ed-unofficial-patch/releases/latest) if you want to check it first.

## The launcher stops before it writes anything

It stops while Elite is running. Close the game, including a copy left in Task Manager.

It stops on the Horizons client, and on an executable it cannot read. ED-UP is for Odyssey.

An Odyssey build this release was not made for still installs, with a warning. The game starts. Fixes that depend on the game build stand down, and the log names them.

## The game closes a second or two after launch

Update ED-UP. Current releases handle the crash that did this on older builds, with no edit to `edvr.ini`. If it still closes after that update, send the log zip. The longer trials are in the [technical notes](https://github.com/characterecho-sean/ed-unofficial-patch/blob/main/docs/troubleshooting.md#if-the-game-dies-a-second-or-two-after-launch).

## VR fails just after an ED-UP update

The two headset files have to be from the same package. A new file beside an old one stops VR startup on purpose, and Elite reports an init error. Press **Repair**. Repair writes both files from the copy you are running.

## Elite always opens in the headset, or always on the monitor

Steam's launch choice overrides the 3D setting for that launch. The VR option forces the headset. The normal option forces the monitor. Start with the option you want. After a monitor launch, the VR option is how you get the headset back. [Playing](Playing.md).

## The picture shimmers or smears as you look around

On the F8 Performance page, set anti-aliasing to Off and look again. If the shimmer leaves with it, the upscaler is involved. Turn it back on, go to the place where it shows, and [send the log](Ask-for-help.md). NumLock, pressed while you are looking at it, writes a short report into the log. Shift+NumLock writes a much larger one. Use NumLock on its own unless someone asks for the long capture.

Angled cockpit panels can smear when EDHM recolors them. Include that in the report, and press Repair if the EDHM chain was lost. [Other mods](Other-mods.md).

## The patch disappeared after the launcher checked the files

Frontier's verify can delete `d3d11.dll`. Run the launcher again. [Updates and uninstall](Updates-and-uninstall.md).

## A headset session wrote one log

ED-UP's headset half did not load. Elite started on a path this patch does not serve. [Headsets](Headsets.md).

## An antivirus tool flagged it

A graphics add-on beside a game looks, to a scanner, like something to quarantine. Some scanners will flag it. The source is public. [How to build it](https://github.com/characterecho-sean/ed-unofficial-patch/blob/main/docs/building.md).
