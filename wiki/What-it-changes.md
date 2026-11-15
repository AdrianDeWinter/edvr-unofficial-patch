# What it changes

ED-UP does three jobs. It can upscale the picture. It draws Odyssey's on-foot game in stereo. It corrects drawings that are right on a monitor and wrong when each eye has its own view.

The fixes below are release 0.22.2. [Every setting and its default](https://github.com/characterecho-sean/ed-unofficial-patch/blob/main/docs/fixes.md) is the long page. The notes of each release name anything that release adds.

The changes live in memory while Elite runs. ED-UP leaves the game's files on disk as Frontier wrote them. It leaves your account, ships, cargo, credits, and missions alone. Look to aim is the one place it turns your commander, and it does that with the same kind of input a mouse sends. It leaves anti-cheat untouched, and it runs under its own name.

## A steadier picture

Fine edges shimmer because Elite has no temporal anti-aliasing of its own. ED-UP can hand each frame to DLSS, FSR, or XeSS, or run its own. Ships, vehicles, settlement parts, and characters pass their real motion through, read from the game's own record of where each one was and where it is. Anything ED-UP cannot follow is drawn with no history, so the upscaler does not invent a trail.

Menus, panels, and HUD text are drawn again after the upscaler, so the upscaler does not smear them. Weapons fire has a layer of its own and stays sharp. In the headset, an NPC's hair and eyes move with the head, and so do the Holo-Me suit, helmet, and collar.

[Upscaling](https://github.com/characterecho-sean/ed-unofficial-patch/blob/main/docs/fixes.md#upscaling-dlss-fsr-and-xess) · [the interface](https://github.com/characterecho-sean/ed-unofficial-patch/blob/main/docs/fixes.md#the-interface)

From release 0.22.2, XeSS Frame Generation can add frames on a monitor. It is a monitor row, off by default, only with XeSS. [Settings](Settings.md#frame-generation-on-a-monitor) names the limits.

## On foot in a headset

On foot, Elite normally shows one flat picture to both eyes. With native on-foot VR on, each eye gets the world from your commander's head. You walk and fight with the body under you. The head is hidden from the inside.

Look to aim, sights, scope zoom, the visor, view bob, and the on-foot HUD are separate rows on the Native on foot page. They need the Odyssey build named in the release notes. On any other build they stand down, and the log says so.

When the game still shows its flat on-foot screen, ED-UP can black out the grey surround, move that screen, curve it, and raise its resolution. The stock screen is 1920×1080 on every headset, which is why the text looks soft.

[Native on-foot VR](https://github.com/characterecho-sean/ed-unofficial-patch/blob/main/docs/fixes.md#native-on-foot-vr)

## The two eyes

These run in the headset. The helmet lines are the one with a switch.

- Near a bright light, both eyes share one exposure. The game meters each eye on its own, so one eye goes dark beside a star or a floodlight.
- A body in the surface scanner or the system scanner stays lit in both eyes, on machines where the game drops it to black in the right eye.
- The Full System Scanner reveals the same scan to both eyes.
- The RemLok helmet's faint edge lines sit toward the outside of each eye. You can hide both lines, or leave the game's overlay. The default keeps the outer line.

## Stuck to your face

A drawing pinned to a monitor rides your head once you are in a headset.

- The single frame drawn from the wrong place during a jump, or a drop from supercruise, is held back. The previous frame shows for a moment, so the world does not lurch.
- Sun glare can stay on the star. The default keeps the full glare and takes it off your head. A plainer glow, and the game's own glare, are the other two choices.
- Smoke, steam, and geyser plumes stay upright when you tilt your head. The same correction settles that swim on a monitor when you swing the mouse.
- The faint pattern inside the loading-screen ship holds still.
- The night-vision pulse keeps its distance as you turn your head.

## Planets, stations, and the maps

Terrain at the edge of your view is culled in the stock game from a head pose that is already late, so squares of ground disappear as you look around. ED-UP answers with the pose for the frame on the display, and it widens that cull on headsets whose eyes see further out than the game assumed.

In the Scorpion's gunner seat the camera starts inside the gun. ED-UP brings it back over the turret and places the turret panels and radar where they can be read. The Scarab is left as the game draws it.

Galaxy Map lines, text, and icons are drawn sharp, and they hold still as you turn your head. On the System Map a click selects a body and a second click moves to it, as on a monitor. The Orrery view is unchanged.

Inside a station, drawing skips what you cannot see. Shut in a hangar, the station outside the hangar is skipped, including sunlight leaking in, until you launch. In a concourse, the windows show a picture of the exterior until you walk up to the glass.

## On a monitor

A flat install has the upscalers, UI quality, sharpness, star glow, steady smoke and steam, the older Horizons canopy, station culling, and a choice to skip the launch movie. It has its own settings file and its own F8 panel. [On a flat screen](https://github.com/characterecho-sean/ed-unofficial-patch/blob/main/docs/fixes.md#on-a-flat-screen).

## Looks you can change, and effects you can drop

World scale, star glow, canopy style, and the picture on the desktop mirror are taste. You can also empty the hyperspace tunnel of streaking stars, turn off the dust in supercruise or near a planet, or put back the pulsing high-wake marker under the speed readout. Defaults match the game, except where a fix above names another default.

The desktop mirror can show the headset's finished picture on your monitor, cropped to a shape you pick. It does not wait on the monitor's refresh, so it cannot hold the headset at 60 Hz.

## The smallest change

Set the on-foot screen width back to 1920, leave the launch movie on its screen, and turn off anti-aliasing, UI quality, native on-foot VR, and optimized stations. Two startup redirects remain. They are how ED-UP takes the VR path, and how the F8 menu keeps the keyboard. [What the fixes touch](https://github.com/characterecho-sean/ed-unofficial-patch/blob/main/docs/fixes.md#what-the-fixes-touch).
