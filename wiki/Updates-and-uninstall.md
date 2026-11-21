# Updates and uninstall

## When Frontier updates Elite

Each ED-UP release is made for one Odyssey build. The release notes name it. Release 0.22.2 was made for build 334925.

After a game update, Elite still starts with ED-UP in place. Each fix looks for its target by the shape of the game's code, or by what the frame contains. A fix that cannot find its target turns itself off. The rest keep running. The log names each one that stood down.

On-foot VR in stereo needs that exact build. On any other build it waits for an ED-UP update.

Frontier's launcher can delete `d3d11.dll` when it verifies the game files. Elite itself is fine. The verify took ED-UP out. Run the launcher again, or press **Repair**. If the game folder lost your settings, the spare copy under `%LOCALAPPDATA%\EDUP` is what the launcher offers to put back.

Starting the game from Epic does not put the patch back after a verify. Repair does.

## Updating ED-UP

The [launcher](The-launcher.md) offers a newer release when one exists, and downloads it when you press Download. The release signature has to match the key inside the launcher.

An update keeps settings you have changed. A file you edited is kept when a later release no longer ships that file. DLSS and XeSS files stay when a release does not carry them.

## Uninstall

Open the launcher and press **Uninstall**.

It removes ED-UP's files and restores the game's `openvr_api.dll`. If another mod was chained behind ED-UP, that mod goes back under its own name. `edup.ini` and `edup-flat.ini` stay by default so a later install finds them. Tick **Clean purge on uninstall** before pressing **Uninstall** (or pass `--purge`) to also delete both settings files, `edup_logs`, `edup_backup`, `edup_install`, crash artifacts, and this folder's settings mirror.

From a terminal on Windows:

```text
ed-up-launcher.exe --uninstall
ed-up-launcher.exe --uninstall --remove-settings
ed-up-launcher.exe --uninstall --purge
```

On Linux, run `./ed-up-launcher.sh` and press **Uninstall**. For an Epic copy, add `--store epic`. [Epic on Linux](Install-on-Linux.md#epic-on-linux).

By hand, delete `d3d11.dll` from the game folder. In the `Openvr` folder you used, delete ED-UP's `openvr_api.dll` and rename `openvr_api_orig.dll` back to `openvr_api.dll`. Delete `edup.ini` only when you want the settings gone. The launcher is the one that also restores a chained mod. The hand steps miss that.

Removing the [launcher](The-launcher.md) leaves the patch installed.
