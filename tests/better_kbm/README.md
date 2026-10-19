# Integration checks

After applying the cumulative engine patch:

```bash
python3 tests/better_kbm/run.py --engine ../BetterKBM-engine-expanded
python3 tests/better_kbm/run.py --engine ../BetterKBM-engine-expanded --sdk
```

The first command compiles and executes the actual input, bindings, capture and pointer sources with mocked SDL/guest services. It exercises diagonal sprint, world movement versus title navigation, keyboard/controller coexistence, separate E/R/C actions, Options/Back, chord precedence and release, forward/reverse requests, mouse clicks and cursor timeout.

`--sdk` additionally requires the native build dependencies (SDL3 headers through pkg-config, zlib, C/C++ compilers). It tests the production controller queue and persistence, real movie serialization over a synthetic two-frame layout, the native message converter and eight-language BND/FMG/DCX localization. No game assets are needed. If SDL3 is not registered with pkg-config, pass `--sdl-include /path/to/include` explicitly.

An optional `--game /path/to/plaintext/eboot.bin` validates the original 1.09 signatures, vanilla section handlers, camera fields, child stack, native Defaults confirmation and quick-item entry points. It reads the user's local file without modifying it.

These checks do not launch the game, exercise a real Vulkan driver or replace an interactive Windows play test.

## Windows runtime deployment

From MSYS2 CLANG64, test the runtime deployer independently:

```bash
python3 tests/better_kbm/test_runtime_deployment.py --runtime-dir /clang64/bin --objdump llvm-objdump
```

This cross-links real x64 Windows PE fixtures, checks transitive runtime copying and case-insensitive DLL names, verifies repeated runs preserve output, and checks missing-library and malformed-image diagnostics. The fixtures are never executed.

## Native menu update checks

`--sdk` also tests the actual runtime Quit to Desktop state machine, including
mounted saves, the title fallback, cancellation and expiry. Pointer checks cover
left/right selector hit regions and binding-row capture.

With a locally built converter and your own game dump, optionally run:

```bash
python3 tests/better_kbm/test_game_menus.py --engine ../BetterKBM-engine-expanded --converter ../BetterKBM-engine-expanded/out/bb-engine-assets.exe --game-dir /path/to/game
```

This checks the real movie layout, independent Controls/Graphics frames, native
action-button columns, Defaults, title/System quit rows, and localized bundles.
It reads your game files and writes only temporary outputs. No game data is
included in the test suite.

The real-menu check also verifies both mirrored texture bounds for all five
resized frames, independent frame characters, unchanged text/widget matrices
across the animation timelines, Key Bindings row spacing, and Defaults placement
below the compact Effects/Image settings borders.

Image/Camera revision: the real-menu test checks a four-setting Image section,
a three-setting Camera section, six aligned private frames, Defaults outside
both settings regions, and an unchanged original Environment sprite. The main
test runner also checks mirror cache hits skip transformation, while corrupted
cache output regenerates and modified source layouts are rejected.

Resolution/input revision: tests check the real resolution widget's private
opaque RGBA background, display depth above the action buttons, unchanged
animation matrices, native tick priority, T before a profile is reapplied, and
preservation of explicit custom keyboard bindings.

## Launcher chord capture

With .NET SDK 8 or newer:

```bash
dotnet run --project tests/better_kbm/launcher_capture/LauncherCapture.csproj -c Release
```

This dependency-free console suite compiles the production BindingCapture class.
It covers a first key before any timer tick, chords with staggered release,
additional inputs while a chord is held, opening-gesture exclusion, wheel pulses,
release fallback, cancellation and the next capture. Interactive WPF validation
must also check focus after clicking a binding, Tab/Alt, the capture pad and
Cancel/Unbind in the launcher's actual window.

Sprint/controller camera revision: the normal test runner now executes the
production AVX store-hook assembly in a synthetic camera frame. It verifies
that disabled mouse input cannot call the host callback or alter controller
pitch/yaw, while enabled mouse input still affects both axes. Input bridge
checks cover ordered keyboard rollover, repeat rejection, preserved held sprint,
diagonal additions, every controller axis pair and controller priority over a
pending mouse target-switch flick. These tests require an x86-64 CPU with AVX;
they do not assert that the game's final skid animation has been play-tested.

Compatible sprint turns: the normal runner checks the production sprint-turn
policy across all eight movement targets at six update rates. Compatible turns
retain a fully pushed axis and cap per-sample rotation; opposing-axis changes
snap immediately. Tests also cover mid-turn reversals, release without a delay,
non-sprint passthrough and multiple samples at the same timestamp. The numerical
invariants do not prove the game's final sprint/animation state.

Movement tuning: the normal runner checks configurable rates, 60 ms release
grace, expiry, reversals during grace, immediate sprint release, circular range,
and zero-rate instant turns. It also compiles the production settings reader/
writer and checks defaults, save/reload, bounds and preserved bindings. Launcher
cache behavior and UI responsiveness need an interactive Windows WPF check.
