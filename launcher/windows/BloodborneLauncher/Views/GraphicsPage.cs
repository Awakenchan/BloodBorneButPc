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

        public GraphicsPage() : base("Graphics",
            "Stored in bbport.ini; the upscaler can also be changed in game (Insert or L3 + R3).")
        {
            Add(Card(SymbolRegular.Gauge24, "Best settings for this PC", Hardware.Detect().Summary,
                SmallButton("Recommend…", SymbolRegular.Sparkle24, (_, _) => MainWindow.Instance!.OfferBestSettings(firstRun: false)), out _));

            Group("Upscaler");
            // FSR 4.1.1 runs through fsr4vk (in the package) or an fsr4_411 set built from the player's
            // own AMD DLL: offered when either is there. Without both the setting becomes FSR 4, the
            // model it would fall back to (#22).
            var fsr411 = Fsr4VkFound() || Directory.Exists(Fsr411Dir());
            if (!fsr411 && Ini.Get("upscaler") == "fsr411") Ini.Set("upscaler", "fsr4");
            IniCombo(SymbolRegular.Sparkle24, "Upscaler", null,
                fsr411 ? Choices.Upscalers : Choices.Upscalers.Where(c => c.Value != "fsr411").ToArray(), "upscaler", changed: UpdateUpscaler);
            _upscalerHint = (TextBlock)((SettingRow)Body.Children[^1]).Tag;

            IniCombo(SymbolRegular.ZoomIn24, "Quality preset", "How far below the output resolution the game renders",
                Choices.Presets, "preset", changed: UpdateUpscaler);
            _presetCard = (SettingRow)Body.Children[^1];

            IniToggle(SymbolRegular.Eyedropper24, "Sharpening (RCAS)", "Works with every option above, TAA included: turn it up if the picture looks soft", "sharpen");
            _sharpenCard = (SettingRow)Body.Children[^1];
            SliderRow(SymbolRegular.SlideSize24, "Sharpening strength", "Up to 1: the upscaler's own sharpening. Above 1: an extra pass", 0, 2, 0.05,
                () => ParseDouble(Ini.Get("sharpness"), 0.5), v => Ini.Set("sharpness", Fmt(v)));
            _sharpnessCard = (SettingRow)Body.Children[^1];

            IniToggle(SymbolRegular.ArrowMove24, "Object motion vectors", "Less ghosting on characters; costs about 10% FPS", "object_motion");
            IniToggle(SymbolRegular.DataUsage24, "Show FPS", null, "show_fps");
            // What the counter shows (on unless turned off).
            foreach (var (icon, key, title) in new[]
                     {
                         (SymbolRegular.TopSpeed24, "show_fps_rate", "Frame rate"),
                         (SymbolRegular.Timer24, "show_fps_frame_time", "Frame time"),
                         (SymbolRegular.ResizeLarge24, "show_fps_upscaler", "Upscaler"),
                         (SymbolRegular.DeveloperBoard24, "show_fps_cpu", "CPU usage"),
                         (SymbolRegular.Gauge24, "show_fps_gpu", "GPU usage"),
                     })
            {
                // CPU and GPU use are off unless chosen (the engine's defaults, bbport_settings.h).
                var fallback = key is "show_fps_cpu" or "show_fps_gpu" ? "0" : "1";
                ToggleRow(icon, title, "Shown in the FPS counter", () => Ini.Get(key, fallback) == "1", v => Ini.Set(key, v ? "1" : "0"));
                ((System.Windows.FrameworkElement)Body.Children[^1]).Margin = new System.Windows.Thickness(24, 0, 0, 4);
            }

            Add(Card(SymbolRegular.DeveloperBoard24, "DLSS", "", null, out _dlssHint));

            // The engine's picture and camera controls (bbport_settings.cpp clamps the same ranges).
            Group("Picture");
            var picture = new StackPanel();
            IniToggle(SymbolRegular.Color24, "Picture tuning", "Turns the settings below on, live. Also in the game's System menu (PC Graphics, PC Effects, PC Colour) and the Insert menu", "graphics_controls");
            IniToggle(SymbolRegular.CircleHalfFill24, "Vignette", null, "graphics_vignette", picture);
            IniSlider(SymbolRegular.DarkTheme24, "Ambient occlusion strength", 0, 3, "graphics_ao_strength", picture);
            IniSlider(SymbolRegular.WeatherSunny24, "Shadow distance", 0.5, 4, "graphics_shadow_scale", picture);
            IniSlider(SymbolRegular.Lightbulb24, "Bloom", 0, 3, "graphics_bloom", picture);
            IniSlider(SymbolRegular.Color24, "Saturation", 0, 3, "graphics_saturation", picture);
            IniSlider(SymbolRegular.BrightnessHigh24, "Brightness", 0, 3, "graphics_brightness", picture);
            IniSlider(SymbolRegular.DarkTheme24, "Contrast", 0.8, 1.2, "graphics_contrast", picture, step: 0.01);
            IniSlider(SymbolRegular.Color24, "Hue", -180, 180, "graphics_hue", picture, step: 1, fallback: 0);
            IniSlider(SymbolRegular.WeatherFog24, "Fog", 0, 3, "graphics_fog", picture);
            Body.Children.Add(picture);

            Group("Camera");
            IniToggle(SymbolRegular.Camera24, "Camera tuning", "Turns the three settings below on", "camera_controls");
            IniSlider(SymbolRegular.ZoomOut24, "Field of view", 1, 1.5, "camera_fov_scale");
            IniSlider(SymbolRegular.ArrowMaximize24, "Distance", 0.5, 1.5, "camera_distance_scale");
            IniSlider(SymbolRegular.ArrowBidirectionalUpDown24, "Height", 0.5, 1.5, "camera_height_scale");

            // Engine features from 0.9.4, each with a switch, as IFreemz's "Bloodborne - Experimental".
            Group("Experimental");
            SettingToggle(SymbolRegular.Broom24, "VRAM cleanup",
                "Frees video memory held by textures the GPU drew, so long sessions don't fill up VRAM and slow down. Recommended: on",
                "exp_vram_cleanup");
            SettingToggle(SymbolRegular.Timer24, "Precise frame pacing",
                "A high-resolution timer and even spacing between frames: steadier frame times and less VRR flicker. Uses a little CPU. Off by default",
                "exp_frame_pacing");
            SettingToggle(SymbolRegular.Storage24, "Use more VRAM for textures",
                "Keeps textures in video memory up to 80% of the card instead of about 40%, so they aren't thrown out and loaded again (the cause of most stutters on 8 GB cards). Off by default while it is tested",
                "exp_vram_budget");
            SettingToggle(SymbolRegular.TopSpeed24, "Graphics threads first",
                "Runs the engine's graphics threads at a higher CPU priority than the game's other threads, so a frame isn't held up behind them. Off by default",
                "exp_thread_priority");
            SettingToggle(SymbolRegular.Sleep24, "Shorter sleep spin",
                "The game's many short sleeps busy-wait only as long as the timer needs (about 0.2–0.4 ms instead of 0.6 ms), leaving that CPU time to the graphics threads. Off by default",
                "exp_sleep_spin");
            SettingToggle(SymbolRegular.FlashFlow24, "Background shader compiling",
                "New shaders compile on helper threads instead of freezing the frame: much shorter hitches when you reach a new area. On by default",
                "exp_async_pipelines");
            SettingToggle(SymbolRegular.EyeOff24, "Occlusion culling",
                "The game skips objects hidden behind walls instead of drawing them all: much higher FPS in dense areas. Turn it off if lamps or glare flicker",
                "exp_occlusion");
            SettingToggle(SymbolRegular.FastForward24, "Faster draw thread",
                "Remembers texture and sampler lookups and hands the end of each frame to the draw thread: higher FPS in busy areas. New: turn it off if the game hangs or crashes",
                "exp_fast_draws");
            SettingToggle(SymbolRegular.Shapes24, "No shader freezes",
                "While a new shader compiles, the objects that need it appear a moment later instead of the game freezing. New: turn it off if something stays missing",
                "exp_compile_skip");
            SettingToggle(SymbolRegular.Run24, "Sharper motion (FSR 4)",
                "Uses FSR 4's balanced model at Performance and Ultra Performance: fewer trails behind moving characters, a little more GPU time",
                "fsr4_balanced");

            Group("Game effects");
            Body.Children.Add(Text("Patches to the game, applied when it starts.", "Faint"));
            IniCombo(SymbolRegular.Cube24, "Model detail", null, Choices.ModelLod, "model_lod");
            foreach (var effect in Choices.Effects)
                IniToggle(EffectIcon(effect.Key), effect.Title, effect.Hint, effect.Key);

            Ws.PortChanged += UpdateUpscaler;
        }

        private void IniSlider(SymbolRegular icon, string title, double min, double max, string key, Panel? into = null,
            double step = 0.05, double fallback = 1.0) =>
            SliderRow(icon, title, null, min, max, step, () => ParseDouble(Ini.Get(key, Fmt(fallback)), fallback), v => Ini.Set(key, Fmt(v)), into: into);

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
                "fsr4" => Directory.Exists(Path.Combine(Paths.PortDir, "fsr4_shaders"))
                    ? "Assets found. GPUs without the needed shader features fall back to FSR 3.1"
                    : "fsr4_shaders is missing from the port folder: the game falls back to FSR 3.1",
                "fsr411" when Fsr4VkFound() =>
                    "Through fsr4vk (included, experimental): runs on any GPU with the needed Vulkan features. Prefer FSR 4 if it looks wrong",
                "fsr411" => Fsr411Problem() is { } problem
                    ? $"{problem}: the game falls back to FSR 4"
                    : "Assets for the chosen resolution and preset found",
                "dlss" => DlssFound()
                    ? "Needs an NVIDIA GeForce RTX GPU; others fall back to FSR 3.1" +
                      (Ini.Get("preset", "0") == "0" ? ". With Native AA this is DLAA: DLSS as anti-aliasing at full resolution" : "")
                    : "DLSS files are missing: the game falls back to FSR 3.1",
                "taa" => "Anti-aliasing at the output resolution, without an FSR model. Softer than DLAA or FSR Native AA: add sharpening below",
                _ => null,
            };
            SetHint(_upscalerHint, hint);
            _presetCard.IsEnabled = value is not ("taa" or "off");
            _sharpenCard.IsEnabled = value != "off";
            _sharpnessCard.IsEnabled = value != "off";

            var ngx = Path.Combine(Paths.BinariesDir, "nvngx_dlss.dll");
            SetHint(_dlssHint, DlssFound()
                ? $"NVIDIA DLSS {System.Diagnostics.FileVersionInfo.GetVersionInfo(ngx).FileVersion} is included (out\\nvngx_dlss.dll). Choose it as the upscaler above"
                : "out\\nvngx_dlss.dll is missing from the engine folder");
        }

        /// <summary>The fsr4vk provider the package ships (FSR 4.1.1 on any GPU, vk_fsr4vk.cpp).</summary>
        private static bool Fsr4VkFound() =>
            File.Exists(Path.Combine(Paths.PortDir, "fsr4vk", "amd_fidelityfx_upscaler_vk.dll"));

        private static string Fsr411Dir()
        {
            var env = Environment.GetEnvironmentVariable("BB_FSR411_DIR");
            if (!string.IsNullOrEmpty(env)) return env;
            var local = Path.Combine(Paths.PortDir, "fsr4_411");
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
                    return $"Missing or damaged fsr4_411\\{model}\\{name}.spv";
            }
            var init = new FileInfo(Path.Combine(dir, "initializer.bin"));
            return init.Exists && init.Length == 131072 ? null : $"Missing or damaged fsr4_411\\{model}\\initializer.bin";
        }
    }
}
