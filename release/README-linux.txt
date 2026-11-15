ED-UP Linux installer for Steam and Epic

Requirements: Python 3 and an existing Elite Dangerous install and Wine/Proton
prefix. The wrapper reads the game manager's local records and uses its
configured runner. Steam keeps its existing Proton/runtime path. Epic support
covers the native Epic Launcher Preview, native and Flatpak Heroic,
standalone Legendary, Lutris, and Epic Games Launcher installs in an existing
Wine prefix.

Run Elite once through its usual manager, then close it. After changing its
runner, launch Elite once again before using this wrapper. On Steam Deck, use
Desktop Mode. Run as your normal user, without sudo:

    ./edvr_installer.sh

This opens the Windows installer GUI with the Odyssey product folder selected.
Automatic selection requires one confirmed installation. If Steam and Epic
are both installed, choose --store steam or --store epic. For Epic:

    ./edvr_installer.sh --store epic --dry-run
    ./edvr_installer.sh --store epic --install --scope flat

Epic selection verifies the game executable and its associated prefix. Journal
folders alone are not treated as installations. --game-dir selects one product
when several copies are present. The Windows installer receives Epic identity
so settings saves and repair use the Epic settings mirror.
For a flat install with a current installer build:

    ./edvr_installer.sh --install --scope flat

To see the Windows install plan without changing game files:

    ./edvr_installer.sh --preview --scope flat

Preview starts the selected Wine/Proton runtime. It may update prefix bookkeeping,
but the Windows installer's --dry-run makes no game file changes. The wrapper's
own --dry-run instead resolves and prints the exact paths and command without
starting a process or writing files. The shell launcher works from any
directory and forwards every option to the Python wrapper. You can also run
python3 edvr-installer-linux.py directly.
Keep the shipped epic_library.py beside that Python wrapper.

The wrapper also accepts --repair, --uninstall, and --collect-logs.
--collect-logs saves the log zip to your Downloads folder (your home folder if
there is none) and prints the Linux path. Use
--game-dir '/path/to/product' for a different product folder inside the
installed game tree, or --installer '/path/to/edvr-installer.exe' for a
separate EXE. --steam-dir '/path/to/Steam' or STEAM_DIR selects a nonstandard
Steam root. --proton '/path/to/Proton' accepts a Proton directory or its proton
script if the prefix record is missing or stale. For a custom Steam prefix,
use --store steam --prefix with the existing compatdata directory (the parent
of pfx); its pfx/drive_c must already exist. Inherited prefixes are used only
when their Steam app identity names Elite, so another shortcut cannot select
the wrong prefix. Steam identity is also passed to the Windows installer.

Flatpak Steam uses its installed sandbox and recorded Proton, including
compatibility-tool extensions. Missing or ambiguous tools are refused. An
explicit --proton selects a host runner and bypasses the sandbox.
Additional named system Flatpak installations require that host override.

For an Epic install whose prefix or runner is not fully recorded, pass
--prefix '/existing/prefix' and --wine '/installed/wine/bin/wine', or select
an installed Proton with --proton. These are explicit host runner overrides;
otherwise a Flatpak manager's recorded runner executes inside its sandbox.
The wrapper does not create a new game prefix or choose a replacement runner.
Missing or conflicting metadata is reported rather than guessed. Flatpak
access grants apply only to this invocation; global permissions are unchanged.

Start Epic games through their original Heroic, Legendary, Lutris or Epic
manager so it supplies the normal authentication. The Linux ED-UP launcher
wrapper remains Steam-only and refuses Epic --launcher mode.

The wrapper only launches the installer. Keep any existing required Elite
launch options; headset/OpenXR setup and Proton gameplay remain unverified
here.
