using System.IO;
using System.Windows.Controls;
using BloodborneLauncher.Core;
using Wpf.Ui.Controls;
using TextBlock = System.Windows.Controls.TextBlock;

namespace BloodborneLauncher.Views
{
    /// <summary>Upscaling and the game's own effects (patches applied at start).</summary>
    public sealed class GraphicsPage : LauncherPage
    {
        private readonly TextBlock _upscalerHint, _dlssHint;
        private readonly SettingRow _presetCard, _sharpenCard, _sharpnessCard;
        private readonly System.Windows.FrameworkElement _enginePreset;
        private readonly Dictionary<string, ToggleSwitch> _features = new();
        private bool _syncing;

        public GraphicsPage() : base(L.T("Graphics"),
            L.T("Stored in bbport.ini; the upscaler can also be changed in game (Insert or L3 + R3)."))
        {
            Add(Card(SymbolRegular.Gauge24, L.T("Best settings for this PC"), Hardware.Detect().Summary,
                SmallButton(L.T("Recommend…"), SymbolRegular.Sparkle24, (_, _) => MainWindow.Instance!.OfferBestSettings(firstRun: false)), out _));

            Group(L.T("Upscaler"));
            // FSR 4.1.1 runs through fsr4vk (in the package) or an fsr4_411 set built from the player's
            // own AMD DLL: offered when either is there. Without both the setting becomes FSR 4, the
            // model it would fall back to (#22).
            var fsr411 = Fsr4VkFound() || Directory.Exists(Fsr411Dir());
            if (!fsr411 && Ini.Get("upscaler") == "fsr411") Ini.Set("upscaler", "fsr4");
            IniCombo(SymbolRegular.Sparkle24, L.T("Upscaler"), null,
                fsr411 ? Choices.Upscalers : Choices.Upscalers.Where(c => c.Value != "fsr411").ToArray(), "upscaler", changed: UpdateUpscaler);
            _upscalerHint = (TextBlock)((SettingRow)Body.Children[^1]).Tag;

            IniCombo(SymbolRegular.ZoomIn24, L.T("Quality preset"), L.T("How far below the output resolution the game renders"),
                Choices.Presets, "preset", changed: UpdateUpscaler);
            _presetCard = (SettingRow)Body.Children[^1];

            IniToggle(SymbolRegular.Eyedropper24, L.T("Sharpening (RCAS)"), L.T("Works with every option above, TAA included: turn it up if the picture looks soft"), "sharpen");
            _sharpenCard = (SettingRow)Body.Children[^1];
            SliderRow(SymbolRegular.SlideSize24, L.T("Sharpening strength"), L.T("Up to 1: the upscaler's own sharpening. Above 1: an extra pass"), 0, 2, 0.05,
                () => ParseDouble(Ini.Get("sharpness"), 0.5), v => Ini.Set("sharpness", Fmt(v)));
            _sharpnessCard = (SettingRow)Body.Children[^1];

            IniCombo(SymbolRegular.FastForward24, L.T("Frame generation"),
                L.T("Shows an extra frame between the game's own (2x smoother motion). Adds a little input lag. Off by default."),
                Choices.FrameGeneration, "frame_gen");
            IniToggle(SymbolRegular.Timer24, L.T("Frame generation: game at half refresh rate"),
                L.T("Every generated frame reaches the screen evenly (60 + 60 at 120 Hz). Off: the game runs faster, but the screen skips some generated frames"), "frame_gen_cap");

            SettingCombo(SymbolRegular.BrainCircuit24, L.T("DLSS model"),
                L.T("Which NVIDIA model DLSS runs. Applied on the next launch"), Choices.DlssModels, "dlss_preset");
            IniToggle(SymbolRegular.ArrowMove24, L.T("Object motion vectors"), L.T("Less ghosting on characters; costs about 10% FPS"), "object_motion");
            IniToggle(SymbolRegular.DataUsage24, L.T("Show FPS"), null, "show_fps");
            // What the counter shows (on unless turned off), folded under the switch.
            var fpsParts = Advanced(L.T("What the FPS counter shows"));
            foreach (var (icon, key, title) in new[]
                     {
                         (SymbolRegular.TopSpeed24, "show_fps_rate", L.T("Frame rate")),
                         (SymbolRegular.Timer24, "show_fps_frame_time", L.T("Frame time")),
                         (SymbolRegular.ResizeLarge24, "show_fps_upscaler", L.T("Upscaler")),
                         (SymbolRegular.DeveloperBoard24, "show_fps_cpu", L.T("CPU usage")),
                         (SymbolRegular.Gauge24, "show_fps_gpu", L.T("GPU usage")),
                     })
            {
                // CPU and GPU use are off unless chosen (the engine's defaults, bbport_settings.h).
                var fallback = key is "show_fps_cpu" or "show_fps_gpu" ? "0" : "1";
                ToggleRow(icon, title, L.T("Shown in the FPS counter"), () => Ini.Get(key, fallback) == "1", v => Ini.Set(key, v ? "1" : "0"), fpsParts);
                ((System.Windows.FrameworkElement)fpsParts.Children[^1]).Margin = new System.Windows.Thickness(24, 0, 0, 0);
            }

            Add(Card(SymbolRegular.DeveloperBoard24, "DLSS", "", null, out _dlssHint));

            // The engine's picture and camera controls (bbport_settings.cpp clamps the same ranges).
            Group(L.T("Picture"));
            var picture = new StackPanel();
            IniToggle(SymbolRegular.Color24, L.T("Picture tuning"), L.T("Turns the settings below on, live. Also in the game's System menu (PC Graphics, PC Effects, PC Colour) and the Insert menu"), "graphics_controls");
            IniToggle(SymbolRegular.CircleHalfFill24, L.T("Vignette"), null, "graphics_vignette", picture);
            IniSlider(SymbolRegular.DarkTheme24, L.T("Ambient occlusion strength"), 0, 3, "graphics_ao_strength", picture);
            IniSlider(SymbolRegular.WeatherSunny24, L.T("Shadow distance"), 0.5, 4, "graphics_shadow_scale", picture);
            IniSlider(SymbolRegular.Lightbulb24, L.T("Bloom"), 0, 3, "graphics_bloom", picture);
            IniSlider(SymbolRegular.Color24, L.T("Saturation"), 0, 3, "graphics_saturation", picture);
            IniSlider(SymbolRegular.BrightnessHigh24, L.T("Brightness"), 0, 3, "graphics_brightness", picture);
            IniSlider(SymbolRegular.DarkTheme24, L.T("Contrast"), 0.8, 1.2, "graphics_contrast", picture, step: 0.01);
            IniSlider(SymbolRegular.Color24, L.T("Hue"), -180, 180, "graphics_hue", picture, step: 1, fallback: 0);
            IniSlider(SymbolRegular.WeatherFog24, L.T("Fog"), 0, 3, "graphics_fog", picture);
            Body.Children.Add(picture);

            Group(L.T("Camera"));
            IniToggle(SymbolRegular.Camera24, L.T("Camera tuning"), L.T("Turns the three settings below on"), "camera_controls");
            IniSlider(SymbolRegular.ZoomOut24, L.T("Field of view"), 1, 1.5, "camera_fov_scale");
            IniSlider(SymbolRegular.ArrowMaximize24, L.T("Distance"), 0.5, 1.5, "camera_distance_scale");
            IniSlider(SymbolRegular.ArrowBidirectionalUpDown24, L.T("Height"), 0.5, 1.5, "camera_height_scale");

            // Engine features from 0.9.4, each with a switch, as IFreemz's "Bloodborne - Experimental".
            Group(L.T("Experimental"));
            // Engine preset: New or Legacy (0.9.33) turns the newer features below on or off at
            // once; a feature changed by hand shows Custom.
            _enginePreset = ComboRow(SymbolRegular.Options24, L.T("Engine preset"),
                L.T("New is the current engine. Legacy runs it the way 0.9.33 did: try it if the game crashes or draws something wrong"),
                EnginePreset.Choices, () => EnginePreset.Current(S), v => EnginePreset.Apply(S, v), changed: SyncFeatures);
            var exp = Advanced(L.T("Experimental engine features"));
            SettingToggle(SymbolRegular.Code24, L.T("Legacy shader compiler"),
                L.T("On by default: compiles the game's shaders the way 0.9.33 did. Off uses the native compiler, which reads constant data through the GPU's constant cache but is slower in dense scenes on some GPUs for now. Switching recompiles every shader once"),
                "legacy_shaders", exp);
            SettingToggle(SymbolRegular.Broom24, L.T("VRAM cleanup"),
                L.T("Frees video memory held by textures the GPU drew, so long sessions don't fill up VRAM and slow down. Recommended: on"),
                "exp_vram_cleanup", exp);
            SettingToggle(SymbolRegular.Timer24, L.T("Precise frame pacing"),
                L.T("A high-resolution timer and even spacing between frames: steadier frame times and less VRR flicker. Uses a little CPU. Off by default"),
                "exp_frame_pacing", exp);
            Feature(SymbolRegular.Storage24, L.T("Use more VRAM for textures"),
                L.T("Keeps textures in video memory up to 80% of the card instead of about 40%, so they aren't thrown out and loaded again (the cause of most stutters on 8 GB cards)"),
                "exp_vram_budget", exp);
            SettingToggle(SymbolRegular.TopSpeed24, L.T("Graphics threads first"),
                L.T("Runs the engine's graphics threads at a higher CPU priority than the game's other threads, so a frame isn't held up behind them. Off by default"),
                "exp_thread_priority", exp);
            SettingToggle(SymbolRegular.Sleep24, L.T("Shorter sleep spin"),
                L.T("The game's many short sleeps busy-wait only as long as the timer needs (about 0.2–0.4 ms instead of 0.6 ms), leaving that CPU time to the graphics threads. Off by default"),
                "exp_sleep_spin", exp);
            SettingToggle(SymbolRegular.FlashFlow24, L.T("Background shader compiling"),
                L.T("New shaders compile on helper threads instead of freezing the frame: much shorter hitches when you reach a new area. On by default"),
                "exp_async_pipelines", exp);
            Feature(SymbolRegular.EyeOff24, L.T("Occlusion culling"),
                L.T("The game skips objects hidden behind walls instead of drawing them all: much higher FPS in dense areas. Turn it off if lamps or glare flicker"),
                "exp_occlusion", exp);
            Feature(SymbolRegular.FastForward24, L.T("Faster draw thread"),
                L.T("Remembers texture and sampler lookups and hands the end of each frame to the draw thread: higher FPS in busy areas. New: turn it off if the game hangs or crashes"),
                "exp_fast_draws", exp);
            Feature(SymbolRegular.Shapes24, L.T("No shader freezes"),
                L.T("While a new shader compiles, the objects that need it appear a moment later instead of the game freezing. New: turn it off if something stays missing"),
                "exp_compile_skip", exp);
            SettingToggle(SymbolRegular.ArrowForward24, L.T("Fast loading"),
                L.T("Loading screens run at 30 FPS: fewer frames leave the CPU to the loading work itself. Not before cutscenes. New: off by default while it is tested"),
                "exp_fast_loading", exp);
            SettingToggle(SymbolRegular.Flash24, L.T("Fewer memory write checks"),
                L.T("Memory the game rewrites before almost every use is no longer watched for changes, saving up to about 2 ms a frame in busy areas. New: off by default while it is tested"),
                "exp_hot_pages", exp);
            SettingToggle(SymbolRegular.ShieldCheckmark24, L.T("Effect trail crash fix"),
                L.T("For crashes while drawing effect trails on newer Intel CPUs (Witch of Hemwick, for example): stops the crash, and that kind of trail is not drawn. Off by default"),
                "exp_sfx_workaround", exp);
            SettingToggle(SymbolRegular.Camera24, L.T("Camera easing for high FPS"),
                L.T("With the FPS++ follow camera only: its eases converted for 60 FPS and above. Off by default"),
                "exp_camera_easing", exp);
            SettingToggle(SymbolRegular.Run24, L.T("Sharper motion (FSR 4)"),
                L.T("Uses FSR 4's balanced model at Performance and Ultra Performance: fewer trails behind moving characters, a little more GPU time"),
                "fsr4_balanced", exp);

            Group(L.T("Game effects"));
            Body.Children.Add(Text(L.T("Patches to the game, applied when it starts."), "Faint"));
            IniCombo(SymbolRegular.Cube24, L.T("Model detail"), null, Choices.ModelLod, "model_lod");
            IniCombo(SymbolRegular.Camera24, L.T("Follow camera"), L.T("Original keeps the game's camera distance, angle and follow speed at 60 FPS and above"), Choices.CameraTuning, "camera_tuning");
            foreach (var effect in Choices.Effects)
                IniToggle(EffectIcon(effect.Key), effect.Title, effect.Hint, effect.Key);

            Ws.PortChanged += UpdateUpscaler;
        }

        private void IniSlider(SymbolRegular icon, string title, double min, double max, string key, Panel? into = null,
            double step = 0.05, double fallback = 1.0) =>
            SliderRow(icon, title, null, min, max, step, () => ParseDouble(Ini.Get(key, Fmt(fallback)), fallback), v => Ini.Set(key, Fmt(v)), into: into);

        /// <summary>A switch the engine preset sets: changing it by hand shows the preset it now matches.</summary>
        private void Feature(SymbolRegular icon, string title, string hint, string key, Panel into) =>
            _features[key] = SettingToggle(icon, title, hint, key, into, changed: SyncPreset);

        /// <summary>The preset was picked: its switches show their new values.</summary>
        private void SyncFeatures()
        {
            foreach (var (key, toggle) in _features)
                toggle.IsChecked = S.Bool(key);
            SyncPreset();
        }

        private void SyncPreset()
        {
            if (_syncing) return;
            _syncing = true;
            var index = Choices.IndexOf(EnginePreset.Choices, EnginePreset.Current(S));
            if (_enginePreset is Segmented segmented) segmented.Select(index, notify: false);
            else if (_enginePreset is ComboBox box) box.SelectedIndex = index;
            _syncing = false;
        }

        private static SymbolRegular EffectIcon(string key) => key switch
        {
            "skip_intro" => SymbolRegular.FastForward24,
            "debug_camera" => SymbolRegular.Camera24,
            "debug_menu" => SymbolRegular.Bug24,
            _ => SymbolRegular.Color24,
        };

        public override void OnShown() => UpdateUpscaler();

        private static bool DlssFound() =>
            File.Exists(Path.Combine(Paths.BinariesDir, "nvngx_dlss.dll")); // the engine has DLSS built in; NVIDIA's runtime sits beside it

        private void UpdateUpscaler()
        {
            var value = Ini.Get("upscaler");
            string? hint = value switch
            {
                "fsr4" => Directory.Exists(Path.Combine(Paths.EngineDir, "fsr4_shaders"))
                    ? L.T("Assets found. GPUs without the needed shader features fall back to FSR 3.1")
                    : L.T("fsr4_shaders is missing from the port folder: the game falls back to FSR 3.1"),
                "fsr411" when Fsr4VkFound() =>
                    L.T("Through fsr4vk (included, experimental): runs on any GPU with the needed Vulkan features. Prefer FSR 4 if it looks wrong"),
                "fsr411" => Fsr411Problem() is { } problem
                    ? L.F($"{problem}: the game falls back to FSR 4")
                    : L.T("Assets for the chosen resolution and preset found"),
                "dlss" => DlssFound()
                    ? L.T("Needs an NVIDIA GeForce RTX GPU; others fall back to FSR 3.1") +
                      (Ini.Get("preset", "0") == "0" ? L.T(". With Native AA this is DLAA: DLSS as anti-aliasing at full resolution") : "")
                    : L.T("DLSS files are missing: the game falls back to FSR 3.1"),
                "taa" => L.T("Anti-aliasing at the output resolution, without an FSR model. Softer than DLAA or FSR Native AA: add sharpening below"),
                _ => null,
            };
            SetHint(_upscalerHint, hint);
            _presetCard.IsEnabled = value is not ("taa" or "off");
            _sharpenCard.IsEnabled = value != "off";
            _sharpnessCard.IsEnabled = value != "off";

            var ngx = Path.Combine(Paths.BinariesDir, "nvngx_dlss.dll");
            SetHint(_dlssHint, DlssFound()
                ? L.F($"NVIDIA DLSS {System.Diagnostics.FileVersionInfo.GetVersionInfo(ngx).FileVersion} is included (out\\nvngx_dlss.dll). Choose it as the upscaler above")
                : L.T("out\\nvngx_dlss.dll is missing from the engine folder"));
        }

        /// <summary>The fsr4vk provider the package ships (FSR 4.1.1 on any GPU, vk_fsr4vk.cpp).</summary>
        private static bool Fsr4VkFound() =>
            File.Exists(Path.Combine(Paths.EngineDir, "fsr4vk", "amd_fidelityfx_upscaler_vk.dll"));

        private static string Fsr411Dir()
        {
            var env = Environment.GetEnvironmentVariable("BB_FSR411_DIR");
            if (!string.IsNullOrEmpty(env)) return env;
            var local = Path.Combine(Paths.EngineDir, "fsr4_411");
            return Directory.Exists(local) ? local : Path.Combine(Paths.DataDir, "fsr4_411");
        }

        /// <summary>The FSR 4.1.1 model files the chosen output and preset need (fsr4_411\).</summary>
        private static string? Fsr411Problem()
        {
            var (w, h) = Choices.OutputSize(Ini.Get("output_res", "1920x1080"));
            var model = (w > 1920 || h > 1080 ? "t2160" : "t1080") + (Ini.Get("preset", "4") == "4" ? "_m1" : "_m0");
            var passes = new List<string> { "spd", "prepass", "pass0_post" };
            for (var i = 1; i <= 12; i++)
            {
                passes.Add($"pass{i}");
                passes.Add($"pass{i}_post");
            }
            passes.Add("postpass");
            passes.Add("rcas");

            var dir = Path.Combine(Fsr411Dir(), model);
            foreach (var name in passes)
            {
                var file = new FileInfo(Path.Combine(dir, name + ".spv"));
                if (!file.Exists || file.Length < 20 || file.Length % 4 != 0)
                    return L.F($"Missing or damaged fsr4_411\\{model}\\{name}.spv");
            }
            var init = new FileInfo(Path.Combine(dir, "initializer.bin"));
            return init.Exists && init.Length == 131072 ? null : L.F($"Missing or damaged fsr4_411\\{model}\\initializer.bin");
        }
    }
}
