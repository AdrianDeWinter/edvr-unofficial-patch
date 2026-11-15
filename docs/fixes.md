# ED-UP features and fixes

ED Unofficial Patch (ED-UP, formerly EDVR) adds upscaling to Elite Dangerous in
VR and on a flat screen, draws the on-foot game in stereo in a headset, and
fixes what the game gets wrong there. Most of those fixes have one cause:
something correct on a monitor is wrong in a headset because it is drawn once
for two eyes, pinned to your face instead of standing in the world, or sized
for a screen you are not looking at.

The [player guide](../wiki/Home.md) is the short tour of these features.

This page describes release 0.22.1, made for Elite build 334925. Each entry
names its setting in `edvr.ini` and the value it ships with. Most are also a
row in the F8 menu and take effect within about a second. Where a fix has a
full write-up, with what was measured and why the game behaves as it does, the
entry links it.

The fixes are in `d3d11.dll` beside the game, apart from the few that act on
the finished frame on its way to the headset. Those are in ED-UP's own OpenXR
runtime, `openvr_api.dll` in `Openvr\win64`, and their entries say so; a
flat-only install has no ED-UP runtime.

## Settings and the F8 menu

Settings live in `edvr.ini` next to the game. The flat profile reads
`edvr-flat.ini` first and falls back to `edvr.ini` while there is none, so VR
and flat can be tuned apart. A missing file means the defaults. Most settings
take effect while the game runs, within about a second; `edvr.ini` marks the
ones that need a restart. The installer's settings window edits both files.

Press F8 in the game (`hotkey.menu`) to open the menu. In VR it is a panel
placed where you were looking and anchored in the world, so it stays put while
you read it; Shift and F8 recentres it. Up and Down pick a row, Left and Right
change it, Enter toggles, Tab changes page and Escape closes. The keys you use
for Elite's own cockpit panels work in the menu too, read from your Elite
bindings (`hotkey.read_game_bindings`), and the panel's bottom line names them.
While the menu is open the game sees no keyboard at all (`menu.keyboard =
private`), though your mouse, HOTAS and gamepad still reach it. Every change is
written to `edvr.ini`, and a row that waits for the next launch says so.

The pages are Performance, Fixes, Visual, Mirror window, Native on foot,
Hotkeys, Monitor and Status; `menu.developer = on` adds Advanced, Experimental
and Instruments. Hotkeys sets ED-UP's own keys: the menu key, the anti-aliasing
comparison key (`hotkey.compare_aa`, F7) and the camera history key
(`hotkey.dump_camera`, Pause). Pick a row, press Enter, then press the key you
want, with Ctrl, Shift or Alt for a chord, or a gamepad button, or a button or
hat on your HOTAS. Escape cancels and Delete clears; the menu key can be
changed but not cleared from the menu. ED-UP only watches these keys and never
takes them from the game, so a choice that Elite also binds is flagged "also in
Elite". In `edvr.ini` they read `F5`, `CTRL+SHIFT+F9`, `GamePad_Back` or
`231D0200:Joy_12` (the device is the vendor and product, as Elite's own
bindings file spells it).

Monitor is fpsVR's readout with a frame-time strip, and `menu.fps_overlay = on`
pins a one-line frame-rate readout to your view while the menu is closed.
[settings-menu.md](settings-menu.md) has the whole design.

On a flat screen F8 opens ED-UP's own panel, with the settings that apply to
flat on its Performance and Visual tabs.

## Upscaling: DLSS, FSR and XeSS

Elite has no temporal anti-aliasing, so edges and fine lines shimmer as you
move, and in a headset you never stop moving. ED-UP can hand each frame to a
temporal upscaler, which blends it with the frames before it. Choose one in the
Anti-aliasing row of the Performance page, or set `fix.temporal_aa`:

- `dlss` is NVIDIA's DLSS, for an RTX card. It needs NVIDIA's runtime,
  `nvngx_dlss.dll`, which the installer places on a machine with an NVIDIA
  card. Without an RTX card or the runtime, ED-UP runs its own TAA instead and
  the log says so. The DLSS preset row (`fix.temporal_aa_model`) is K by
  default, with L and M to compare.
- `fsr` is AMD's FidelityFX Super Resolution 3.1, through a community Direct3D
  11 port compiled into `d3d11.dll`. It runs on any GPU and needs no extra
  file.
- `xess` is Intel's XeSS-SR. It runs on any GPU and is fastest on an Intel Arc
  card. Elsewhere it costs more than DLSS or FSR: on an RTX 5090 in VR it took
  about 1.2 ms a frame more than DLSS. It needs Intel's runtime, `libxess.dll`,
  which the installer places beside the game with its licence notices.
- `on` is ED-UP's own TAA.

The default is `off`, and a change takes effect at the next frame. Set Elite's
own anti-aliasing to Off or SMAA while one of these is on.

In VR the render size is set by Elite's HMD Image Quality. Below 1.0 the game
renders smaller and the upscaler brings the picture back to the headset's size,
which is where the frame time is saved. At 1.0 or above DLSS runs at your full
render size, which is DLAA. Leave Elite's Supersampling at 1.0 in VR: below it,
the game scales the world up itself before ED-UP sees it, and the headset shows
a notice saying so. On a flat screen, Elite's Supersampling sets the render
size instead.

Moving ships, vehicles, settlement parts and characters give the upscaler their
exact motion, read from the game's own records of where each one was last frame
and where it is now; anything ED-UP cannot follow gets no history rather than a
guess. Elite draws its interface without writing depth, so a temporal pass
would treat a panel a metre from your face as infinitely far away; ED-UP
records the interface's depth, and the smoke's, for the upscaler. All of this
comes with `fix.temporal_aa` and has no switch of its own.

The comparison key, F7, turns anti-aliasing off for a side-by-side look and
back again without writing to `edvr.ini`. [anti-aliasing.md](anti-aliasing.md)
describes the temporal pass.

## Shimmer and sharpness

### The interface

In VR, `fix.ui_quality` draws the game's interface as sharp as it would be at
HMD Quality 1.0 (`100`, the default), 1.25 or 1.5, whatever HMD Quality the
scene runs at. The cockpit's panels are made at that size inside the game's own
panel code. Menus, station services, the loading screen and the 2D screen are
drawn into a separate layer and laid over the finished picture after the
upscaler and the sharpening, so their text is never upscaled, sharpened or
smeared. The cockpit's holo panels, the flight HUD, the target markers and the
holograms go through a layer of their own at the same size, and keep the bloom
halo the game gives them. The supercruise bars, the orbit lines and the space
dust are drawn into the layer too, so the upscaler does not smear them. The
layer needs anti-aliasing on, and larger sizes cost video memory: at `150` it
has 2.25 times the pixels it has at `100`. A saved `200` reads as `150`. `off`
leaves the interface as the game draws it. On a flat screen the same setting
makes the panels at the display's size or larger, whatever Supersampling the
scene renders at.

### Stair steps on the HUD

The HUD layer goes over the picture after the anti-aliasing, so its slanted
lines and the edges of its panels can show steps, most at a low UI quality and,
on a flat screen, below 4K. `fix.hud_antialiasing = on` smooths everything in
the layer before it is laid over: text, panels, the supercruise bars and the
dust. It is off by default and works while UI quality and anti-aliasing are
both on. It costs GPU time and about 10 bytes of video memory for every pixel
of the layer, per eye in VR: about 160 MB an eye at a 4032x4032 layer, and 37
MB at 2560x1440 on a flat screen.

### Softness

Every temporal filter trades a little edge contrast for its calm.
`fix.render_sharpness` runs AMD's RCAS to hand some of it back, from 0 (off,
the default) to 1; 0.3 is a fair start. In VR it runs just before the frame
leaves for the headset, with the menus' layer laid over after it. On a flat
screen it runs before the game draws its interface, so text and HUD are never
sharpened there. It adds contrast at edges, not detail, and too much of it
rings.

### Weapons fire and faces

Lasers, tracers and other weapons fire are drawn into a layer of their own and
added after the upscaler, so they stay sharp. In VR an NPC's hair and eyes move
with the head, and so do the Holo-Me commander's suit, helmet and collar, so
none of them smear when a head turns. None of this has a setting.

## The two eyes disagreeing

### One eye darker than the other near bright lights

Elite meters brightness separately for each eye, so near a star or a floodlight
one eye stops down and the other does not. ED-UP copies one eye's exposure into
both. At a held view of a star the eyes were about 1.5 stops apart without it
and 0.4 with it, and what remains is the glow in the eye that can see the star.
Always on in VR, with no setting. [eye-brightness.md](eye-brightness.md).

### The planet you are scanning going black in one eye

In the surface and system scanners the body can render as a featureless black
disc in the right eye and correctly in the left (Frontier issue
[78021](https://issues.frontierstore.net/issue-detail/78021)). The game issues
the second eye's lighting draw with its vertex buffer missing, because its own
record of what it last bound goes stale after two draws that use none. ED-UP
makes that record agree with what the game asked for as the game flushes its
draw state. The bug shows on some machines and not others; where it is absent
there is nothing to repair. Always on, with no setting.
[scanner-body.md](scanner-body.md).

### The FSS showing each eye a different scan

In the Full System Scanner the zoomed body's unresolved cells could be black in
the left eye and already filled in the right, so the reveal animation split.
One draw causes it: the reveal's coarse input is made in the left eye's pass
before the ring layer it samples is finished. ED-UP runs that draw again from
the finished layer just before the left eye's reveal, so both eyes show the
right eye's look. Always on in VR, with no setting.
[fss-scanner.md](fss-scanner.md), and the report to Frontier in
[frontier-fss-bug-report.md](frontier-fss-bug-report.md).

### The RemLok helmet's lines along your nose

When the emergency helmet deploys, the game stamps the same overlay, with a
line at each edge, into each eye, so its faint lines sit in the middle of your
view instead of at your temples (Frontier issue
[69074](https://issues.frontierstore.net/issue-detail/69074)). With
`fix.remlok_lines = outer` (the default) each eye keeps only the line on its
own outward side; `hide` removes both and `stock` leaves the game alone.
`fix.remlok_line_angle` (46 degrees from straight ahead, 0 to 60) sets where
the line sits, the same on any headset. [remlok-lines.md](remlok-lines.md).

## Drawn on your face instead of in the world

### The flash when you jump or drop out of supercruise

Once per transition Elite draws a single frame from the wrong viewpoint. On a
monitor it is a blink; in a headset the world lurches. ED-UP knows from the
game's own camera driver which frame will be drawn from the reset camera and
does not send it, so the runtime shows the previous frame for a moment instead.
Always on, with no setting, and it needs ED-UP's `openvr_api.dll`. Each held
frame is logged as `transition flash: frame N held`. If a flash still gets
through, press Pause (`hotkey.dump_camera`) straight after it and send the log:
with anti-aliasing on, it writes the temporal pass's recent camera history and
whether the hold was armed. [transition-flash.md](transition-flash.md).

### The sun's glare riding your head

A star's whole glare (corona, smudge, beams, rays and lens flare) is drawn flat
on your view like a camera overlay: it rolls when you roll your head, and the
beams stay pinned to your face. `fix.sun_glare = vivid` (the default) keeps
every element, with the corona, smudge and beams locked to the world, which is
the movie-camera look without the head coupling. `realistic` keeps only the
glow a real eye would see, the same in both eyes, and `stock` leaves the game
alone. It works on every star and is live, so you can switch mid-flight and
compare. [sun-glare.md](sun-glare.md).

### Smoke and steam swimming as you look around

Geyser plumes, smoke, steam and other particle effects are flat cards that the
game turns to face you using the camera's own up direction, so tilting your
head rolls every puff with you, and a plume seems to spin as you look past it.
`fix.particle_billboard = steady` (the default) builds them against the world's
up instead: they still face you, and a rising plume stays upright. On a flat
screen it settles the same swim when you swing the mouse. The menu row is
called Billboards. [particle-billboards.md](particle-billboards.md).

### The loading screen's shimmering ship

The spinning ship hologram carries a faint, low-resolution pattern inside its
outline that is sampled in screen space, so in a headset it rides your head.
`fix.holo_pattern = steady` (the default) holds the pattern still for that one
draw per eye; `stock` leaves the game alone.
[loading-hologram.md](loading-hologram.md).

### The night-vision pulse moving with your head

`fix.night_vision_stability = 1` (on) keeps the night-vision pulse line at a
steady distance as you turn your head, with the game's terrain shading and
brightness kept, whether anti-aliasing is on or off.
`experimental.night_vision_realistic` (off) is a separate, experimental look
for night vision, with its brightness control beside it on the Experimental
page. [night-vision.md](night-vision.md).

## Over a planet

### Terrain missing at the edges of view

Over planets Elite culls terrain with a head pose 41 to 44 ms older than the
display time of the frame it draws, so squares of ground at the edges of your
view go undrawn as you move your head (Frontier issue
[72609](https://issues.frontierstore.net/issue-detail/72609)). ED-UP answers
the game's request for the head pose "now" with the pose for the display time
of the frame being drawn. Some headsets' eyes see further out than the centred
frustum Elite culls against, as a Quest 3 or a cropped Pimax does; there ED-UP
also widens that frustum to what each eye shows, which stops the black flashes
at the outer corners over flat ground. On a Quest 3 through Virtual Desktop at
90 Hz that took the flashes from 20 in 5.7 minutes to none in 3.5, with no
change in frame rate. VR only, with no setting. If ED-UP cannot find the one
function it widens, it leaves the game alone and the log says `terrain cull
cover: inert`. [terrain-culling.md](terrain-culling.md).

### The Scorpion's turret view

In the Scorpion SRV's turret (gunner) view in VR the game puts the camera
inside the gun. ED-UP moves it back over the turret's axis, places the turret
HUD's panels and radar where you can read them, keeps the radar locked to the
turret, and holds both eyes at black on entry until the camera has settled.
ED-UP tells which SRV you are in from the game's journal; the Scarab, and any
SRV it cannot identify, are left as the game draws them. The placement is set
by the `experimental.srv_turret_*` keys.
[srv-turret-hud-2026-10-30.md](srv-turret-hud-2026-10-30.md).

## On foot

### Native on-foot VR

On foot, Elite asks for a flat screen and shows the same image to both eyes.
With `fix.native_on_foot` on (the default), ED-UP answers that request with the
headset's own stereo while a game session is live, so the world is drawn for
each eye from your commander's head. You walk, use tools and fight with your
commander's body beneath you, and the head is hidden from the inside. The
settings are on the menu's Native on foot page. It needs Elite build 334925 and
stands down on any other build, and the log says so.
[design-native-onfoot-vr-2026-10-13.md](design-native-onfoot-vr-2026-10-13.md)
covers how it works.

The first time you start Elite in VR with it on, the headset menu opens an
"Aiming on foot" page during the splash screen, explains look to aim and mouse
aim, and asks you to pick one. Picking either stops the page coming back, and
"Ask me next time" keeps it. `menu.aim_choice_prompt = off` turns it off.

Explorer Cam, the free camera that came before native on-foot VR, has been
removed. Its settings in an older `edvr.ini` are ignored, and one that differs
from its last default is named once in the log.

### Look to aim

With `fix.native_on_foot_look_to_aim = on` (off by default) your head is your
look: where you look, up and down included, is where your commander faces,
walks and points the weapon. The mouse and a controller's right stick stop
looking up and down. They turn you in steps, with a brief dimming of the view
(`fix.native_on_foot_turn_style = snap`, 45 degrees a step by default in
`fix.native_on_foot_snap_turn_deg`, from 15 to 90), or continuously (`smooth`).
With nothing in hand the body follows your head with a slight easing,
`fix.native_on_foot_look_to_aim_smoothing_ms` (50 ms), and a drawn weapon
follows at once. `fix.native_on_foot_look_to_aim_ads_vertical = on` lets the
mouse and stick fine-tune the aim up and down while you aim down sights; it is
off by default. Look to aim pauses while an on-foot panel, the inventory or the
Esc menu is open. With it off, the mouse and stick look as the game's own do,
and your head looks around on its own.

### Aiming down sights

Your eye comes to each weapon's own sight line, the one the game uses in flat,
at the distance the game's aiming camera gives that weapon.
`fix.native_on_foot_ads_sight_eye` says which eye. `both` (the default) brings
the right eye to the sights and shows its view to both eyes, so the sights sit
centred in each and depth flattens while you aim; `right` or `left` brings that
eye and keeps normal stereo. With `fix.native_on_foot_ads_head_aim` on (the
default; the menu calls it "ADS: Look to Aim") the weapon follows your head
while you aim. Off, it stays where the mouse or stick points it.

Through a scope, `fix.native_on_foot_ads_scope_zoom` (on) magnifies the picture
inside the scope's frame by the game's own zoom for that weapon, as the flat
game zooms its camera, and leaves the rest of the view at true scale. It is a
digital zoom of the right eye's picture, so a 4x zoom shows a quarter of the
detail, and with the sight eye set to `left` there is no lens. Iron, reflex and
holo sights get no lens either. With an anti-aliasing mode on, the scope's
gunsight mark keeps its own size over the magnified picture; with it off, the
mark is magnified too. For the scoped Executioner,
`experimental.native_on_foot_ads_scope_zoom_full_detail` (on) has the game draw
the zoomed view itself, at full detail. That needs an anti-aliasing mode, uses
both eyes whatever the sight-eye setting says, and shows the view inside a
circle round the sight with black outside it.
[design-vr-scope-magnification-2026-10-28.md](design-vr-scope-magnification-2026-10-28.md).

### Body, view and HUD

Eye height (`fix.native_on_foot_eye_height_cm`, 15) sets how far your eyes sit
above your commander's neck. In breathable air the visor is held up
(`fix.native_on_foot_breathable_visor = up`), which also hides the RemLok
helmet's edge lines; `game` leaves the visor to the game. When you run, ED-UP
takes most of the game's bob out of the view (`fix.native_on_foot_view_bob =
off`), and `on` keeps it. The HUD rows on the page set the on-foot HUD's
distance, size, element scale and spread, and whether it follows your body or
your head (`fix.native_on_foot_hud_follow`, `body` by default).

Lean far from where your commander stands, or into a wall, and the view fades
and can recentre, keeping the way you face. Boarding your ship or SRV puts you
back in your stored seat. At a terminal the HUD hides while the terminal's
interface fills the view, which needs an anti-aliasing mode. On lockers, the
inventory and other on-foot panels the mouse cursor is drawn where your clicks
land. Where the game moves the camera away from you (a shop's buy screen, the
loadout and Livery screens, a fleet carrier, a terminal) you see the game's
camera, with the preview model beside the panel, and ED-UP takes the view back
once the camera has stayed with you for 2 seconds.

### The on-foot screen

Where the game still shows its flat on-foot screen, with native on-foot VR off,
on another game build or in HMD Cinema Mode, these settings apply:

- `fix.black_void = 1` (on) turns the dark grey around the screen black. On an
  OLED headset that grey is lit pixels, so the screen floats in a glowing
  rectangle.
- `fix.panel_distance` (1.0, the game's own distance) moves the screen nearer
  or further. 0.7 pairs well with the curve below.
- `fix.panel_curvature` (0, flat) bends the screen toward you, the way Virtual
  Desktop curves its own; 0.3 with a distance of 0.7 is a comfortable start.
  The intro movie and the splash screen follow the same curve.
  [screen-curvature.md](screen-curvature.md).
- `fix.vscreen_res_width = auto` (the default) raises the screen's resolution.
  Elite renders it at 1920x1080 whatever your headset, which is why on-foot
  text looks soft. `auto` sizes it from the width your headset rendered per eye
  last session: about 70% of the screen's width in your eye when ED-UP
  anti-aliases the on-foot world itself, and otherwise 125% of the eye's width.
  A fresh install stays at 1920 until one session with VR running has
  completed. Type a width instead (640 to 8192; 2880, 3840 and 5120 are useful)
  to choose your own, and the height follows at 16:9; `1920` turns it off. It
  costs GPU time and video memory in proportion, needs a game restart, and
  changes the game's code in memory, so read [What the fixes
  touch](#what-the-fixes-touch) first.
- `fix.weapon_stability = 1` (on) keeps weapons and tools steady as you move
  and turn on foot, by moving the game's wait for the next frame so the arms
  are drawn for the pose they are shown at. It costs about a frame of latency
  on foot and keeps SteamVR's motion smoothing and Oculus ASW from engaging
  there. Native on-foot VR does not use it.

## The Galaxy Map and System Map in VR

The Galaxy Map's lines, text and icons are drawn in the interface layer at full
size, so the map holds the headset's frame rate and does not smear as it moves.
Its icons and labels stay put as you turn your head, and the panels sit at the
game's depth, so they do not cut through the map in stereo. On both maps the
cursor selects what it points at whatever your head does, and the arrow stays
on the side buttons as you reach for them. On the System Map a click selects a
body and a second click moves to it, as in flat. The Orrery view does not have
this treatment. [galaxy-map-vr-2026-11-03.md](galaxy-map-vr-2026-11-03.md).

## Visual options

These options, on the menu's Visual and Mirror window pages, are matters of
taste. Each ships at the game's own look apart from the mirror window.

### World scale

`visual.world_scale` (100) sets how big the world looks, from 50 to 200
percent: above 100 it looks bigger, below 100 smaller. The units and direction
are those of OpenXR Toolkit's and SteamVR's world scale. It changes only the
distance between the two eyes the game renders with; your head movement, the
field of view and ED-UP's own menu are untouched. VR only.
[world-scale.md](world-scale.md).

### Star glow

`visual.star_glow` strengthens the halo Elite's bloom draws around every star,
from 100 percent (the game as it ships) up to 400, in VR and on a flat screen.
It needs Bloom on in Elite's graphics options, and the menu says so when Bloom
is off. At 300 and above the brightest stars can show specks.
[star-glow-2026-10-22.md](star-glow-2026-10-22.md).

### Cockpit glass

`visual.cockpit_glass = legacy` gives the canopy the look of the older Horizons
client: the sun lights it plainly, with a soft glare toward the sun, in place
of Odyssey's mirror-like reflections. `odyssey`, the default, leaves the game's
glass alone. Only the intact canopy changes, and ice and condensation stay as
the game draws them. `visual.cockpit_glass_dirt` (100) sets how much dirt
shows, from 0 for a clean canopy to 300. `visual.cockpit_glass_lights =
station` lets a station's lamps and floodlights light the dirt as well, at some
GPU cost; at `off`, the default, the sun and the surroundings light it. The
canopy is drawn after the upscaler, so the dirt does not smear; where that
cannot be done, as with anti-aliasing off, it is drawn with the scene. VR and
flat. [cockpit-glass-legacy-port.md](cockpit-glass-legacy-port.md).

### Mirror window

`visual.desktop_window` sets what Elite's window on your desktop shows while
you play in VR. `left_eye` (the default), `right_eye` or `both_eyes` fills it
with the finished image your headset is shown, and `game` leaves the window as
the game draws it. `visual.desktop_window_aspect` cuts the picture to 1:1, 4:3,
16:10, 16:9 or 21:9 (`full` keeps its own shape), `visual.desktop_window_crop`
places the cut as Elite's own mirror does (`native`) or in the centre
(`center`), `visual.desktop_window_vertical_shift` moves it up or down, and
`visual.desktop_window_style` makes the window an ordinary one or `borderless`
full screen. While Elite shows its flat screen in the headset, as on foot with
native on-foot VR off, the window keeps the game's own shape, because the game
sizes that screen's HUD by it. In VR the desktop window never waits for the
monitor's vertical sync, so it cannot hold the headset to 60 Hz.
[desktop-window.md](desktop-window.md).

## Launch and loading

### The launch movie

Elite plays its launch movie at 1024x576, about 27 degrees wide, pinned to your
face. `visual.intro_video = screen` (the default) plays it on the splash
screen's own virtual screen instead, anchored in the world and sized to what
the splash covers, so the cut from one to the other lines up. The movie is
resampled first with AMD's FSR so the magnification does not look pixelated,
and its sharpness follows the on-foot screen's resolution. The world anchor
needs ED-UP's `openvr_api.dll`; without it the movie stays on your face and the
log says so. `skip` does not play the movie at all: when the game opens the
movie file, ED-UP answers that it is not there, the same answer renaming the
file gives, and nothing on disk is touched. `skip` is decided at launch, and on
a flat screen it is the only value that does anything. `stock` leaves the game
alone. [intro-video.md](intro-video.md).

### The banding behind the intro and the menu

The backdrop behind the intro and the menu is stored in an old compressed
format and is very dark, so its gradients come out stepped, and magnified
across a headset's field of view the steps read as blocks. `fix.intro_backdrop
= splash` (the default) keeps the nicer of the two pictures the game already
loaded, the large splash it shows first and then throws away, and smooths the
banding out of it once, when it first draws. It is the game's own art either
way, and nothing is added to your install. `stock` leaves the game alone.
[menu-backdrop.md](menu-backdrop.md).

### Loading dialogs dimming your whole view

While a loading dialog is up the game lays a frosted wash and a tint over your
entire view, which is ordinary on a monitor and oppressive in a headset.
`fix.loading_dim = screen` (the default) moves the dimming where it belongs:
the wash and the tint go, and the splash screen steps back by the same tint
while a dialog is up. The dialog's own box, border and text are untouched. It
applies to the intro only and retires for the session once a rendered scene
arrives. `stock` leaves the game alone.
[loading-panel-handoff.md](loading-panel-handoff.md).

## Stations and frame rate

### Optimized stations

`fix.optimized_stations` (on) skips drawing what cannot be seen inside a
station, in VR and on a flat screen. While your ship sits in a sealed hangar
(docked, the pad lowered and still, the hatch closed), everything outside the
hangar is skipped, the sun's light leaking in included. That ends when you
launch, the pad moves, the camera leaves the hangar or you undock.

On foot in the concourse of a Coriolis, Orbis, Ocellus or Dodec station, an
asteroid base, an outpost or a surface port, the station outside the room is
skipped too, and the windows show a picture of it taken from the room's centre
as you arrive. Close to a window the real station comes back;
`fix.station_window_distance` (100 percent of the distance as shipped, 0 for
never) sets how close. Aboard a fleet carrier the windows show the real sky,
the rest outside is skipped until you come close to a window, and a room behind
a shut bulkhead door is not drawn while you are more than a few metres from it
(`fix.skip_rooms_behind_doors`, on). Other stations are drawn as the game draws
them. [optimized-station-2026-10-12.md](optimized-station-2026-10-12.md) and
[concourse-cull-2026-11-05.md](concourse-cull-2026-11-05.md).

### Turbo mode

`fix.turbo_mode` (off, experimental) is for a headset whose runtime drops to
half the refresh rate whenever a frame misses its deadline. On, the game stops
waiting on the runtime's frame pacing before it starts the next frame, which is
what turns one slow frame into a stretch at half rate. It costs up to about a
frame of latency and does not change the display period the runtime chooses, so
on some runtimes the drop remains. While native on-foot VR is active the
runtime's own pacing stays.

### Render resolution per headset

`fix.openxr_resolution` keeps a per-eye render width for each headset, set from
the Performance page with that headset on. A headset with no entry runs at what
its runtime recommends. The width is in pixels, held between a quarter and
twice the runtime's current recommendation, and Elite's HMD Image Quality and
the upscaler apply on top of it. A change applies after a game restart.

## On a flat screen

The installer serves a flat screen as well: choose "VR and flat" or "Flat
only". The flat profile has its own settings file, `edvr-flat.ini`, and its own
F8 panel. It offers:

- the upscalers, the DLSS preset, the sharpening, UI quality and HUD
  Anti-Aliasing described above, with the sub-pixel jitter the upscalers need
  applied through the game's own camera;
- the supercruise bars, the orbit lines and the dust drawn in the interface
  layer, and the Holo-Me screen and the Galaxy and System Maps opened on foot
  handled for the upscaler, so none of them smear or ghost;
- depth of field below native resolution averaged over the jitter's cycle, so
  it does not flicker in the lobby and the main menu;
- star glow, the steady billboards, the Legacy cockpit glass, Optimized
  stations and the launch movie's `skip`.

You can switch between VR and flat with Elite's own 3D setting while the game
runs. ED-UP follows, reading `edvr.ini` in VR and `edvr-flat.ini` in flat.

## Effects you can turn off

- `fix.witchspace_stars = off` leaves the hyperspace tunnel empty, so a jump is
  the swirling walls with nothing streaking past inside them. The draw is
  withheld, so nothing else about the jump changes. `on`, the game's own, is
  the default.
- `fix.supercruise_dust = off` drops the dust streaks in supercruise, which can
  help Virtual Desktop's Synchronous Space Warp, and `fix.normal_space_dust =
  off` drops the drifting specks near planets, stations and ships. Both are on
  by default.
- The high-wake marker under the speed readout pulses in time with the target
  indicator's blue flash, because both are drawn into the same cockpit holo
  panel. `fix.wake_pulse = off`, the default, drops that one draw and leaves
  the rest of the panel alone; `stock` keeps the game's.

## Surviving a game update

On build 334925 every hook goes exactly where it was made to go. After a game
update:

- The fixes that work on frames find their targets by what the game draws,
  whichever build drew it. The brightness fix looks for the calculation that
  writes the exposure result exactly twice per frame, and waits for that to
  hold for five frames running before it acts. The black surround and the
  screen's distance key off image sizes and a clear colour.
- Each hook into the game's code looks for its target by the shape of the whole
  function. A hook goes in only when everything it needs is found, and one that
  cannot find its targets stands down on its own while the rest carry on; the
  log names each one. The transition flash hold works this way: on a build
  where its hook cannot be found no frame is held, and the log says so.
- The resolution fix recognises the code it changes by its shape, as described
  under [What the fixes touch](#what-the-fixes-touch), and refuses if what it
  finds looks wrong.
- UI quality sizes panels inside the game's panel code, checked against the
  game build. After an update that changes that code, the panels stay at the
  game's own size until ED-UP is updated, and the log says so.
- Native on-foot VR and the Oculus redirect (below) need the exact build. On
  any other they stand down until an ED-UP update for it, and the log says so.

## What the fixes touch

ED-UP loads with the game as a `d3d11.dll` proxy that forwards every call to
Windows' real `d3d11.dll`. In VR its `openvr_api.dll` is ED-UP's own OpenXR
runtime, which answers the OpenVR calls Elite makes and speaks OpenXR to your
headset's runtime. No file on disk is modified: every change below exists only
in memory while the game runs.

Many fixes work only on the frames. Both eyes share one exposure through five
small copies per frame, with the second eye's exposure shaders not sent to the
GPU. The black surround is one changed argument to a screen-clearing call, and
the screen's distance one changed copy of the panel's position. The upscalers
and the sharpening each run a GPU pass over the game's finished frame into a
texture ED-UP owns, and the headset receives that copy; the game's own texture
is read and never written. The Legacy cockpit glass draws the canopy with
ED-UP's own shader in place of the game's. For the transition flash, ED-UP
reads a constant buffer the game has already filled and, on the rare frame
drawn from the wrong place, hands the runtime its copy of the previous frame,
which is always the game's own picture.

Other fixes hook the game's own functions in memory so that ED-UP's code runs
at a chosen point in the game's frame:

- The transition flash hold reads the game's camera driver.
- The upscalers shift the projection the game is told by a fraction of a pixel
  each frame; on a flat screen that goes through the game's own camera. For
  moving objects they write last frame's position into an unused part of each
  object's render record, and draw those objects with copies of their shaders
  that write one extra output into a target ED-UP owns. What those objects draw
  into the game's own targets is unchanged.
- The scanner fix corrects the game's record of the vertex buffers it bound.
- The terrain fix widens two numbers as the game builds its culling frustum.
- UI quality sizes panels inside the game's own panel code.
- Native on-foot VR answers the game's request for a flat on-foot screen with
  stereo, and changes how your commander is posed and drawn for the view from
  inside. Look to aim and ADS: Look to Aim turn your commander by passing your
  head's turn to the game as look input, as a mouse would.
- Optimized stations skip the game's work for what cannot be seen. To take the
  picture for the concourse windows, they move the game's camera to the room's
  centre for a few frames while your view is held still.
- Some advanced settings, all off by default, hook the game for diagnosis or
  experiments, and `edvr.ini` describes each.

At load ED-UP redirects two of the game's imports. `LoadLibraryW` is redirected
so that Elite's attempt to load the Oculus runtime (LibOVR) fails and the game
takes the OpenVR path ED-UP serves; on a game build ED-UP has no profile for
this redirect stands down, and the Oculus PC runtime may then bypass ED-UP.
`DirectInput8Create` is redirected so that the game sees no keyboard while the
menu is open. Every other library load, and all input while the menu is closed,
pass through unchanged. With `visual.intro_video = skip`, a third import,
`CreateFileW`, answers the game's open of the launch movie with "not found".

ED-UP reads the game's journal and `Status.json` in your Saved Games folder to
know when you are on foot, which SRV you are in and what kind of station you
are at. It writes nothing there.

The resolution fix (`fix.vscreen_res_width`, `auto` by default) rewrites twelve
numbers in the game's code: the width and height the game forces for the
on-foot screen, in the places it forces them. It changes nothing else, neither
the surrounding instructions nor the game's choice of which screen to draw.
These safeguards are the reason to trust it:

- The change exists only in memory, and the original values are put back when
  the game closes.
- It recognises the code it changes rather than trusting a version number. The
  shape is very specific: a check on which screen is being drawn, a forced 16:9
  size only in that case, and the real size read from the game's data
  otherwise. On the build it was developed against that shape occurred in six
  places in 81 MB of code. ED-UP accepts between three and twelve, provided
  they all agree, so an update that adds or drops one does not switch the
  feature off.
- If what it finds looks wrong (too few, too many, or places that disagree), it
  refuses, does nothing, and says so in the log.
- It replaces a number with another number of the same size, so instruction
  lengths and program flow are untouched. If it ever matched the wrong thing,
  the worst case is something drawn at an odd size, which you would see and
  which is gone on restart.
- It logs what it found before changing it, so the log shows the resolution the
  game was really forcing.
- It is all or nothing: if any single write fails, every earlier one is undone.

None of the fixes touches the network or your account, and none reads or
changes your ship, cargo, credits or missions. Look to aim, above, is the one
way ED-UP steers your commander, with the same kind of input a mouse gives.
None interacts with anti-cheat, and none attempts to hide from anything. The
launcher is the one part of ED-UP that goes online; the
[launcher page](../wiki/The-launcher.md) says what it contacts.

If you would rather ED-UP changed as little of the game as possible, set
`fix.vscreen_res_width` to `1920` (the stock size, meaning "do not patch"),
keep `visual.intro_video` at `screen`, and turn off `fix.temporal_aa`,
`fix.ui_quality`, `fix.native_on_foot` and `fix.optimized_stations`. The two
import redirects still apply, because they are how ED-UP takes over VR startup
and the menu's keyboard.
