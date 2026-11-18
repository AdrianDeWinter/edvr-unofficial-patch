# Flat: XeSS-FG proxy only while frames can be generated; plain swap chain otherwise

## Why

A 0.22.3 report on an RTX 5060 Ti (Windows 11 24H2): with XeSS Frame Generation
turned on and the game restarted, the window stayed blank, advanced only on
alt-tab, and froze while focused with the music still playing; the game had to
be closed from the taskbar. No log yet.

Until now the XeSS-FG proxy swap chain owned Elite's window from launch, disabled
through startup and menus, and XeLL slept on every frame. Neither had run on a
non-Intel GPU or in exclusive fullscreen; every Arc flight was Borderless.

## What changes

- **When the bridge is made is unchanged from 0.22.3:** only when the game
  starts with `fix.xess_frame_generation` on and XeSS anti-aliasing. What
  changes is how it starts: EDVR's plain D3D11 swap chain shows Elite's frame
  from the start, and nothing from Intel's frame generation runs on it.
- **The XeSS-FG proxy, XeSS-FG and XeLL are made the first time frame generation
  can run:** the setting on, the game windowed and in the foreground for 30
  presents, and XeSS-SR resolving on 2 presents in a row. Startup and menus stay
  on the plain swap chain.
- **Once made, the proxy keeps the window.** Menus or the setting turned off only
  disable XeSS-FG; nothing is rebuilt.
- **Exclusive fullscreen goes back to the plain swap chain**, whether Elite asks
  for it or Windows' Alt+Enter does it. Frame generation is off there. The proxy
  returns once the game is windowed and in the foreground for 30 presents, so an
  alt-tab out of fullscreen does not rebuild it twice.
- **The plain swap chain is made on Elite's own DXGI factory**, past ED-UP's
  creation hook, so Elite's window association and Alt+Enter handling apply in
  exclusive fullscreen exactly as without the bridge. Before, every swap chain
  on the window came from a factory the bridge made itself, which never had
  Elite's association. Other NVIDIA and AMD users report frame generation
  working in Borderless, which points the freeze at exclusive fullscreen.
- **XeLL's sleep runs only while the proxy presents.**
- **Overlay:** the Frame generation and XeSS-FG frames rows are greyed out and
  locked in exclusive fullscreen, with a note to choose Borderless or Windowed.

## Behaviour users will see

| Game started with | Frame generation |
|---|---|
| Frame generation on and XeSS anti-aliasing | Can be switched off and on in the F8 panel at any time, no restart. What it set up stays until the game is closed, even if the anti-aliasing is switched to another mode: frame generation is then off, Elite's frame still goes through ED-UP's swap chain, and switching back to XeSS resumes it. |
| Frame generation off, or any other anti-aliasing | Not set up at all: the game presents through its own swap chain, as without frame generation. Turning it on, or choosing XeSS, says "Frame generation is set up as the game starts: takes effect at the next game start." |
| Fullscreen display mode, or after Alt+Enter | Off; both rows greyed out and locked until the game is windowed again. |

XeSS upscaling alone never needs this: it works inside Elite's frame, and
Elite presents as usual.

Documented in `edvr.ini` (the setting's comment, which the F8 panel and the
installer show), `docs/fixes.md`, `docs/settings-menu.md` and
`docs/xess-frame-generation-2026-11-12.md` ("Two presenters").

## Testing

- Full local build (`build.bat --wait-lock`): all gates and test rigs pass.
  `flat_sharpen_test` covers the rows' new fullscreen dim.
- **Not flown yet.** Cases to fly, on an Arc card and then on an NVIDIA card:
  1. Windowed start with XeSS, setting off, then turned on: the restart hint;
     the log shows no `xess-fg` bridge lines.
  2. Windowed start with XeSS, setting on: plain through menus, frames in game;
     off and on again in the panel without a restart.
  3. Windowed with frame generation running, then Fullscreen (Elite's option and
     Alt+Enter), then back to windowed: off in fullscreen, back on after.
  4. Fullscreen start, setting off: no bridge, the game's own swap chain.
  5. Fullscreen start, setting on: the plain swap chain on Elite's factory,
     exclusive; rows locked; frames once windowed. Alt-tab out and back must
     not freeze (the 0.22.3 report).
  6. Start with DLSS or no AA and the setting on, then XeSS: the restart hint;
     the log shows `not started: the flat anti-aliasing mode is ...`.

  The log lines to check: `bridge ready`, `the proxy takes window`, `the plain
  swap chain takes window`, and that `stops for this session` never appears.

🤖 Generated with [Claude Code](https://claude.com/claude-code)
