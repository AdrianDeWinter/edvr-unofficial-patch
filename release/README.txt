ED Unofficial Patch native OpenXR release
===========================

ED-UP is a graphics and VR compatibility patch for Elite Dangerous: Odyssey.
This release uses the native OpenXR route on Windows. The bundled Khronos
loader selects the system OpenXR runtime; SteamVR is allowed when selected in
Windows, but the SteamVR application is not required by this package.

LAUNCHER
--------

The usual way to install is ed-up-launcher.exe, which is a separate download
on the same release page as this zip. Close Elite Dangerous, run the launcher,
select the game directory and install. It validates the native pair before
writing, preserves the game's original OpenVR DLL for recovery, and creates a
local OpenXR config. The "Install ED-UP for" choice says what goes in: VR and
flat (the default), VR only, or flat only. On the command line, --scope
both|vr|flat does the same.

This zip holds the same files for installing by hand, described under MANUAL
INSTALL below.

FLAT (3D MODE OFF)
------------------

With Elite's 3D mode off, ED-UP runs its flat fixes from an install whose scope
includes flat: "VR and flat" (the default) or "Flat only". The flat fixes are
experimental; their visual quality is not yet qualified.

- Its settings live in edup-flat.ini. Installing never writes the VR
  profile's edup.ini for them. The settings screen's Flat selection edits that
  file.
- Press F8 for the AA menu. Up/Down selects a row; Left/Right changes it.
- Choose Off, TAA, DLSS, FSR3 or XeSS-SR and the DLSS model preset; settings save live.
- Press F8 or Escape to close. Game keys are private while the menu is drawn.
- [fix] temporal_aa also accepts off, on (TAA), dlaa, dlss, fsr or xess (default off).
- XeSS-SR needs libxess.dll beside the game; the release includes it when built with the SDK.
- libxess_fg.dll (XeSS Frame Generation) and libxell.dll (Xe Low Latency) ship beside it, from the same SDK.
- FSR 4 needs amd_fidelityfx_loader_dx12.dll and amd_fidelityfx_upscaler_dx12.dll beside the game; the launcher installs them
  with AMD-FIDELITYFX-LICENSE.txt. They are AMD's own signed files, unmodified.
- Game supersampling controls render scale; DLAA requires native SS.
- [visual] intro_video = skip leaves out the launch movie (the F8 menu's Visual
  tab, Intro movie row, does the same; it applies at the next launch). The
  default, screen, plays it as the game makes it. It was [fix] intro_video
  before; an existing edup-flat.ini keeps its choice.
- Press F7 to turn ED-UP's anti-aliasing off for a side-by-side look, and again
  to put your setting back. It writes nothing; [hotkey] compare_aa changes the
  key and empty turns it off. The same key works in VR.
- Press NumLock while the issue is visible to collect a diagnostic capture.

MANUAL INSTALL
--------------

1. Close Elite Dangerous.
2. Copy d3d11.dll beside EliteDangerous64.exe.
   Copy edup.ini only if you do not already have one; keep existing settings.
3. In Openvr\win64, rename the game's openvr_api.dll to
   openvr_api_orig.dll. Do not overwrite an existing original with an ED-UP DLL.
4. Copy openvr\openvr_api.dll, openvr\openxr_loader.dll and
   openvr\OPENXR-LOADER-LICENSE.txt into Openvr\win64.
5. Start the game.

If EDHM or another mod owns d3d11.dll, use the launcher to preserve and chain
it automatically. For manual chaining, rename that mod's DLL to d3d11_edhm.dll
and set advanced.real_dll = d3d11_edhm.dll in edup.ini before placing ED-UP. It
is set once: edup.ini applies to both the VR and the flat profile. The flat
profile's edup-flat.ini can override it with another name, or say none to run
flat without the chain.

DLSS
----

When included, nvngx_dlss.dll is an optional anti-aliasing component. The
native OpenXR route does not fall back to another VR backend if it is absent.

UNINSTALL
---------

Use the launcher's Uninstall button, or remove ED-UP's d3d11.dll and the native
OpenVR files, then rename openvr_api_orig.dll back to openvr_api.dll. Keep the
original OpenVR file until recovery is complete.

LICENSE
-------

See LICENSE.txt and openvr/OPENXR-LOADER-LICENSE.txt.
