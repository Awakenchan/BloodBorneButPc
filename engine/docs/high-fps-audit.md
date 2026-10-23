# Above 30 FPS: what still assumes the game's 30

Bloodborne runs its logic at 30 frames a second. The frame-rate presets (`patches.py`:
`FPS_PRESETS`, `SCALED_FPS`, `corrected_frame_rate`) give it the real frame time and correct what the
community lists got wrong (effect rate, cloth, follow camera, status effects, stamina, sprint).
This note tracks what is left.

## Tools

- `tools/re/fps_constants.py out/eboot.elf [patched.txt]`: every instruction that reads a float
  constant of a 30 fps frame (1/30, 30.0, as float or double) or stores one as an immediate,
  grouped by function, `*` where a preset already writes it.
- `tools/re/fps_patched.py out/eboot.elf <uncap|60|90|120|144>`: every executable byte that
  preset's writes touch, the second argument above.

## State (2026-10-21, 1.09 eboot, the 144 preset)

211 sites in 151 functions; 80 functions have their sites written by the preset, 71 do not.

Not every 30 is a 30 fps assumption. Three kinds:

1. **Data frame counts to seconds** (`count * 1/30`, `seconds * 30`): the data is authored in
   30 fps frames, so these are right at any frame rate and must stay. Checked: `0xf1e5e0`,
   `0xf1f790`, `0xf37a40` (teardown functions converting a stored frame count, `+0xac`/`+0x90`/
   `+0x4c`, into seconds); their patched neighbour `0xf1e630` keeps 1/30 too.
2. **A step per frame** (a timer advanced by 1/30 every frame, an ease weight tuned for a 30 fps
   frame): wrong above 30; these get the frame time (`0x575A908`) or a converted weight, as the
   presets do for the camera and status effects.
3. **Not time at all** (30 degrees, 30 units, a 30-entry table): leave alone.

Still to classify: the other 68 unpatched functions, starting with `0x12ab4c0` (10 sites, 1/30
and 30.0 mixed), `0x188d8f0` (7), `0x21d9ea0` and `0x1576240` (4 each).

## Not visible to the scan

A counter that goes up by one each frame and is compared with a count from data (a wait of N
frames) uses no float constant. Above 30 it ends sooner: N frames at 120 FPS is a quarter of the
time the data meant. Finding these needs the game's state compared between a 30 and a high frame
rate run (the same save, the same input), not a constant search.
