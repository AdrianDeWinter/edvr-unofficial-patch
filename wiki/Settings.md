# Settings

Press **F8** in the game. That opens ED-UP's menu, in the headset or on a monitor. The launcher's settings window edits the same files.

[Every setting, with the value it ships with](https://github.com/characterecho-sean/ed-unofficial-patch/blob/main/docs/fixes.md), is in the repository. This page is the short tour.

## The files

| File | When Elite reads it |
| --- | --- |
| `edvr.ini` | Headset sessions. A missing file means the defaults. |
| `edvr-flat.ini` | Monitor sessions. While this file is missing, the monitor reads `edvr.ini`. |

Both sit next to `EliteDangerous64.exe`. Most changes apply within about a second. A row that waits for the next launch says so on the menu. The menu writes the file for you.

## Keys

In the headset the panel is placed where you were looking, and it stays there. **Shift+F8** recentres it.

| Key | What it does |
| --- | --- |
| Up and Down | Pick a row |
| Left and Right | Change that row |
| Enter | Toggle, or start capturing a key |
| Tab | Next page |
| Escape | Close |

Elite's own cockpit-panel keys work in the menu too. While the menu is open, Elite receives no keyboard input. Mouse, HOTAS, and gamepad still reach the game.

The pages are Performance, Fixes, Visual, Mirror window, Native on foot, Hotkeys, Monitor, and Status. On a monitor, F8 opens the settings that apply there, on Performance and Visual.

**F7** turns anti-aliasing off and back on for a comparison, and it does not save that flip.

## The choices most people touch

Leave the rest until a picture bothers you. The names here are the menu rows. The linked page gives the `edvr.ini` key and the default.

### Anti-aliasing

The default is off. Elite draws edges without temporal anti-aliasing, so fine lines shimmer as you move. In a headset you are always moving.

| Choice | Which machine |
| --- | --- |
| DLSS | An NVIDIA RTX card. The launcher places NVIDIA's file on a machine with an NVIDIA card. |
| FSR | Any graphics card. Nothing further to download. |
| XeSS | Any graphics card, and fastest on Intel Arc. On other cards it costs more frame time than DLSS or FSR. |
| On | ED-UP's own anti-aliasing. |

Set Elite's own anti-aliasing to Off or SMAA while one of these is on. If DLSS is selected on a machine without an RTX card, or without NVIDIA's file, ED-UP runs its own anti-aliasing and the log says so.

In a headset, **HMD Image Quality** is the size the game renders. Below 1.0 the game renders smaller and the upscaler fills the headset size back in. That is where the frame time is saved. At 1.0 and above, DLSS runs at full size. Leave Elite's **Supersampling** at 1.0 in VR. Below that, the game scales the world before ED-UP sees it, and the headset shows a notice. On a monitor, Supersampling is the render size.

[The full upscaler notes](https://github.com/characterecho-sean/ed-unofficial-patch/blob/main/docs/fixes.md#upscaling-dlss-fsr-and-xess).

### Frame generation on a monitor

From release 0.22.2, XeSS Frame Generation is a row on the monitor Performance page, with a second row for how many frames it adds. Leave it off until you want it. It is available when anti-aliasing is XeSS, in Borderless or Windowed, and it applies the next time Elite starts. It is not offered in a headset. Menus and the HUD are generated with the scene, so they can ghost. An Intel Arc card can insert more frames than other cards. The menu refuses a count the card will not run.

### Sharp interface

**UI quality** draws menus, panels, and the HUD at a sharp size on top of the upscaler. 100 is the default. 125 and 150 are larger and use more video memory. 150 is the top. A saved 200 is read as 150.

**Sharpness** puts a little edge contrast back after the temporal filter. 0 is off. 0.3 is a fair start. High values ring.

### On foot

Native on-foot VR is on by default, on the Odyssey build the release was made for. The world is drawn for each eye from your commander's head. Look to aim stays off until you turn it on: then the direction you look is the direction the commander faces. Scopes can zoom. The first headset launch asks which aiming style you want. [On foot](https://github.com/characterecho-sean/ed-unofficial-patch/blob/main/docs/fixes.md#native-on-foot-vr).

### Scale and look

**World scale** sets how large the world looks, from 50 to 200 percent. 100 matches the game. Headset only. Field of view, and the ED-UP menu, stay as they are.

**Sun glare**, **cockpit glass**, **star glow**, and the **desktop mirror** are on the Visual and Mirror window pages. They ship looking like the game, except the mirror, which can show the picture the headset is already showing.

## Everything else

Hotkeys can rebind F8, F7, and the camera-history key (Pause). Monitor is a frame-time readout. Status shows what loaded this session.

The list of every key, including the fixes that stay on because they have no row, is [docs/fixes.md](https://github.com/characterecho-sean/ed-unofficial-patch/blob/main/docs/fixes.md).
