ED-UP launcher on Linux (Steam + Proton)

This wrapper is Steam-only. If it finds Elite through Heroic, Legendary,
Lutris or the Epic manager, it says so and stops. Start Epic games through
their original manager. There is no Linux installer for an Epic copy of Elite:
the ed-up-<version>.zip on the release page lists the files and the steps for
installing by hand.

This is new and untested on Linux. The launcher window, its updates and the
Play button may not work yet. Logs and reports are welcome.

What it is: the ED-UP launcher, the same Windows program people run on
Windows. It runs inside Elite's own Proton setup, so there is nothing
else to install. It needs Python 3 and the Proton that Steam already uses for
Elite. It does not need Protontricks or a system Wine.

Run Elite once from Steam, then close it. If you change Elite's Proton version
in Steam, launch Elite once again first. On Steam Deck, use Desktop Mode. Run
as your normal user, without sudo:

    ./ed-up-launcher.sh

To see what would run without starting anything or writing any files:

    ./ed-up-launcher.sh --dry-run

Options for the launcher itself go after a lone --, for example:

    ./ed-up-launcher.sh -- --channel rc

The wrapper's own options (--game-dir, --steam-dir, --prefix, --proton, --launcher-exe)
go before the --. Use --game-dir '/path/to/product' for a different product
folder, --steam-dir '/path/to/Steam' or STEAM_DIR for a nonstandard Steam root,
and --proton '/path/to/Proton' if the prefix record is missing or stale. The
shell script works from any directory. You can also run
python3 ed-up-launcher-linux.py directly.

For a custom Steam prefix, use --store steam --prefix with the existing
compatdata directory (the parent of pfx). An inherited prefix from another
Steam shortcut is ignored. Flatpak Steam uses its installed sandbox and the
recorded Proton, including compatibility-tool extensions; missing or ambiguous
tools are refused. An explicit --proton selects a host runner and bypasses
the sandbox.
For Steam, additional named system Flatpak installations require that host
override.

The launcher keeps its downloads and settings inside Elite's Proton prefix, in
drive_c/users/steamuser/AppData/Local/EDVR under
steamapps/compatdata/359320/pfx.

The launcher window installs, updates, repairs and uninstalls ED-UP for the
selected Elite folder, and its Save logs button writes the log zip to your
Downloads folder, or to your home folder when Downloads is missing.

Play starts Elite through Steam. Starting Elite directly, without Steam, is not
offered on Linux yet.

Keep any existing required Elite launch options. Headset and OpenXR setup and
Proton gameplay are not covered by this package.
