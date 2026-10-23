# Linux/RADV startup, dispatch hangs, and remaining visual issues

Reported with v0.9.37 beta 9 (`lantern-fps93`) on an RX 7900 XT (20 GiB), Ryzen 9
9900X, Mesa/RADV 26.2.5, and Linux 7.2.11. Output and scene were 2560x1440, using
FSR 4 Native AA and FSR 3.1 frame generation (90 game FPS, 180 displayed FPS), HDR off.

## Linux online startup

Discord sign-in completed in the launcher, but the game remained offline. The launcher
already passes the account to the child; `run.sh` did not call the shared configuration
used by `run_windows.py`. It therefore lacked the backend flags and game-host redirects.

With the startup handoff restored, a real Linux session reached The Hunter's Dream
matching context callback and opened the P2P/relay path. This confirms the previous
offline-only startup was resolved; co-op, invasions, and a connection to another player
have not been verified by this investigation. Authentication tokens and server acceptance
remain controlled by the existing online backend.

## Indirect dispatch hangs

Six session logs stopped on indirect compute shaders `0000000042f2a521` or
`000000002da7fe60`, with X counts between 2.28 billion and 4.26 billion. One recorded
2,811,384,408 x 1 x 1 groups. The kernel attributed the graphics-ring timeouts to
`bb-probe`, and ring recovery succeeded. Reported VRAM use before that crash was
about 4 GiB of 20 GiB.

The original safeguard used `maxComputeWorkGroupCount` directly. This RADV device
advertises 4,294,967,295 x 65,535 x 65,535, so the reported X values pass unchanged.
The safety bounds now also stop at 65,535 per dimension, retaining the existing
`BB_INDIRECT_CLAMP=0` opt-out. The breadcrumb report still captures the original guest
counts, rather than the copied dispatch arguments.

This is a hang workaround, not a correction to the code that produces those counts.
Capping incorrect work may change effects rendering. A local shader equivalent passed
an isolated GPU test and was used in later sessions, but gameplay coverage is limited.
The source test checks the production bounds on a real GPU without ever executing
adversarial indirect counts; its indirect execution case is one known-safe workgroup.

## Unresolved lighting and missing effects

At the same location, changing the camera direction can make the scene severely
overexposed/almost white, then return it to normal or dark lighting. This is tied to
view direction rather than random flickering while the camera stays still. Torch flame
effects are also reported missing.

These symptoms were reported to exist before the local startup and GPU workaround
changes. No source fix for either visual symptom has been established.

Isolation results:

- Disabling the Intel SFX workaround removed that patch from the next session's patch
  list, but the torch flame was still reported missing.
- Disabling FSR 4 auto exposure did not resolve the lighting; auto exposure was restored.
- Removing the 1440p Light Grid patch was reported to cause a different lighting fault;
  that patch was restored. It is not presented as a workaround.
- Disabling experimental guest-in-place GPU memory was prepared as the next isolated
  test; its visual result has not been confirmed.

Other selected patches were 1440p Light Grid, DS1-like physics, Sensitive Analog Input,
and the launcher-managed 90 FPS/effect/resolution/heap patches. Dynamic light shadows,
motion blur, depth of field, and chromatic aberration were disabled. Neither FSR 4 nor
frame generation was disabled during the reported exposure test.

To investigate further, reproduce at the same location while rotating the camera,
record the exact direction-dependent transition and torch effect, and compare one
rendering/memory setting per launch. Keep the visual report separate from claims that
the startup handoff or dispatch cap fixes lighting.
