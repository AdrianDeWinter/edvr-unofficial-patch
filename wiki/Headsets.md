# Headsets

ED-UP answers the VR start Elite already uses, then opens its own session with the OpenXR runtime Windows has set as active. That runtime is whichever headset software you selected: SteamVR, Virtual Desktop (VDXR), Pimax's PiOpenXR, Meta's runtime, or another one Windows lists. ED-UP has no second choice inside its own menu.

Before you launch Elite:

1. Set Windows' active OpenXR runtime to the headset software you intend to use.
2. Start that software and connect the headset.
3. Launch Elite as you usually do.

ED-UP ships the loader it needs. Leave OpenComposite uninstalled, and skip any extra OpenXR loader offered to "make it work."

## What has been flown

This path has been flown on Pimax over PiOpenXR, on a Quest 3 over Virtual Desktop's VDXR, and on a recent Air Link flight. Other headsets and runtimes are unclaimed. If yours misbehaves, [send the log](Ask-for-help.md). The log line `runtime,<name>,<version>` is the name of the software ED-UP reached. [Playing](Playing.md) says where that line is.

## Elite's Oculus mode

Elite can also start VR through the Oculus PC software (LibOVR). ED-UP serves the other start, the one that speaks OpenXR. On the Odyssey build a release was made for, ED-UP sends the game down that path. On a newer game update the redirect can stand down, and the Oculus runtime can then pass ED-UP by, until an ED-UP build for that game update. The log names this.

## Pimax headsets with canted displays

This is the Vision 8K X, 8K Plus, 8K, 5K Super, 5K Plus, 5K XR, and Artisan.

In Pimax Play, turn **Enable Parallel Projection** off, then restart the game. ED-UP draws those canted displays with the option off. With it on, the headset asks for much larger pictures: 8268×3948 per eye instead of 5016×3160 on an 8K X at FOV Wide. Turning it off can cut GPU time by about 30%.

That saving shows up with PimaxXR as the OpenXR runtime. With Pimax Play's own OpenXR runtime the switch can do nothing. When ED-UP sees the option on together with PimaxXR, it posts a notice in the headset.

## Linux

The Linux launcher places the files. Proton and your headset software still have to agree, and the launcher leaves Elite's launch options as they are. [Install on Linux](Install-on-Linux.md).
