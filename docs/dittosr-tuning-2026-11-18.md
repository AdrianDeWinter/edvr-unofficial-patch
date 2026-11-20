# DittoSR tuning keys

## Status

DittoSR 0.5.0 is selectable with `fix.temporal_aa = dittosr`. Sharpness and the filter are settings a player would choose. Frame weight and anti-shimmer ship at DittoSR's own defaults and stay tunable until a flight settles them.

- Open: whether `fix.dittosr_frame_weight` and `fix.dittosr_anti_shimmer` should remain keys or become fixed behaviour.
- Ruled out: nothing yet. No flight has been flown.
- Next flight: one flat session and one VR session (cockpit, and on foot so the world route runs) before those two keys are frozen.

## The four keys

`fix.dittosr_sharpness` is 0.6. 0 is soft, 0.6 is the look DittoSR ships, and 1 is sharpened.

`fix.dittosr_sharpen` is `cas` or `rcas`. CAS is the usual filter. RCAS avoids halos and costs more GPU time.

`fix.dittosr_frame_weight` is 2, clamped to 0.25..4. Lower holds the previous frame longer. This is a tuning key carried from DittoSR's own settings.

`fix.dittosr_anti_shimmer` is `auto`. Auto uses 0.6 up to 2x and eases to 0.45 at 3x. `off` is 0. A number is clamped to 0..0.9. This is a tuning key carried from DittoSR's own settings.

In the game the four rows stay on the Performance page and dim until DittoSR is selected. The launcher hides them until then.
