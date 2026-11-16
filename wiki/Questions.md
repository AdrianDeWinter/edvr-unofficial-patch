# Questions

## Does it cost anything?

No. It stays free. A tip at [Ko-fi](https://ko-fi.com/seancharacterecho) is welcome and is never required.

## Can it affect my account?

ED-UP leaves your account, ships, cargo, credits, and missions alone. The patch itself stays off the network. The optional launcher asks GitHub about updates and, if you turn direct launch on, signs in to Frontier the way the game's own launcher does. It leaves anti-cheat untouched, and it runs under its own name. Frontier has not endorsed it. Frontier's rules are Frontier's. [What it changes](What-it-changes.md) is the concrete list.

## Does it work with Horizons?

The launcher accepts Odyssey. It stops when it finds the older Horizons client.

## Do I need OpenComposite or SteamVR?

ED-UP ships the loader it uses. You select the OpenXR runtime you already play on. SteamVR is one of those runtimes, and it is optional. Leave OpenComposite uninstalled. [Headsets](Headsets.md).

## I use a Quest, Air Link, or a Pimax. Will it run?

The headset path has been flown on a Quest 3 with Virtual Desktop's VDXR, on Air Link, and on Pimax with PiOpenXR. Set that runtime as Windows' active OpenXR runtime before you launch. Other combinations are unclaimed. [Headsets](Headsets.md).

## Can I play on a monitor only?

Yes. Choose **Flat only**, or choose **VR and flat** and launch Elite on the monitor. [Install on Windows](Install-on-Windows.md).

## Does it run on a Steam Deck?

Yes. Use Desktop Mode and the Linux launcher. [Install on Linux](Install-on-Linux.md).

## I own it on Steam and on Epic.

On Windows the launcher lists each copy. Pick the one you want. On Linux the launcher installs for a Steam copy only. When both a Steam and an Epic copy are installed, name the store with `--store steam`. [Epic on Linux](Install-on-Linux.md#epic-on-linux).

## Where do my settings live?

Beside the game, in `edvr.ini` and `edvr-flat.ini`, and in a spare copy under `%LOCALAPPDATA%\EDVR` on Windows. On Linux the spare copy is inside the Proton prefix. Press F8 to change them while you play. [Settings](Settings.md).

## The launcher remembered my Frontier password. Where is it?

In `frontier-login.dat`, under `%LOCALAPPDATA%\EDVR\launcher\`, and only while remember is ticked. Untick the box, or remove the launcher, and that file is deleted. [The launcher](The-launcher.md).

## How do I remove it?

Press **Uninstall**. Settings stay unless you ask to remove them. Taking out the launcher leaves the patch in the game. [Updates and uninstall](Updates-and-uninstall.md).
