using BloodborneLauncher.Core;
using Wpf.Ui.Controls;
using System.Windows;
using System.Windows.Controls;

namespace BloodborneLauncher.Views
{
    /// <summary>Screen, frame rate, game text and spoken dialogue languages.</summary>
    public sealed class DisplayPage : LauncherPage
    {
        private readonly ComboBox _monitor = new() { Width = 360, VerticalAlignment = VerticalAlignment.Center };
        private bool _refreshingMonitors;

        public override void OnShown() => RefreshMonitors();

        private void RefreshMonitors()
        {
            _refreshingMonitors = true;
            try
            {
                var saved = S.Str("display_device");
                _monitor.Items.Clear();
                _monitor.Items.Add(new ComboBoxItem { Content = "Default (Windows primary monitor)", Tag = "" });
                foreach (var monitor in WindowsMonitors.All())
                    _monitor.Items.Add(new ComboBoxItem { Content = monitor.Label, Tag = monitor.Device,
                        ToolTip = $"{monitor.Width} × {monitor.Height}; {monitor.Device}" });
                var selected = _monitor.Items.Cast<ComboBoxItem>().FirstOrDefault(i => string.Equals(i.Tag as string, saved, StringComparison.OrdinalIgnoreCase));
                if (selected == null && saved.Length > 0)
                {
                    selected = new ComboBoxItem { Content = $"Disconnected monitor ({saved})", Tag = saved };
                    _monitor.Items.Add(selected);
                }
                _monitor.SelectedItem = selected ?? _monitor.Items[0];
            }
            finally { _refreshingMonitors = false; }
        }

        public DisplayPage() : base("Display", "Resolution, window mode, frame rate and game languages. Applied when the game starts.")
        {
            Group("Display & FPS");
            IniToggle(SymbolRegular.DataUsage24, PcText.Get("Show FPS"), null, "show_fps");
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


            _monitor.SetResourceReference(StyleProperty, "DefaultComboBoxStyle");
            Group("Language");
            GameLanguages.Ensure(S);
            ComboRow(SymbolRegular.LocalLanguage24, PcText.Get("Text language"), PcText.Get("Language for all game text, including dialogue, items and menus. Applied on the next launch."),
                Choices.GameLanguages, () => GameLanguages.Text(S), v => {
                    S.Set("text_language",v);Ini.Set("menu_language",GameLanguages.MenuLocale(v));
                });
            ComboRow(SymbolRegular.Translate24, PcText.Get("Voice language"), PcText.Get("Spoken dialogue language. Requires the selected voice files in your game copy. Applied on the next launch."),
                Choices.VoiceLanguages, () => GameLanguages.Voice(S), v => S.Set("language",v));

            Group("Screen");
            Add(Card(SymbolRegular.Desktop24, "Monitor",
                "The game opens here from the start, including fullscreen and HDR. If this monitor is disconnected, the Windows primary monitor is used. Applied on the next launch.",
                _monitor, out _));
            RefreshMonitors();
            _monitor.SelectionChanged += (_, _) =>
            {
                if (_refreshingMonitors || _monitor.SelectedItem is not ComboBoxItem item) return;
                S.Set("display_device", item.Tag as string ?? "");
                Ws.SaveSoon();
            };
            IniCombo(SymbolRegular.ResizeLarge24, "Output resolution",
                "The upscaler renders below this and fills it in", Choices.OutputResolutions, "output_res");
            IniCombo(SymbolRegular.ArrowSync24, "Live resolution change",
                "Change the resolution in the game menu without restarting. Off is faster; turn on only if you switch resolutions in game", Choices.LiveResolution, "live_resolution");
            IniToggle(SymbolRegular.FullScreenMaximize24, "Fullscreen", "Borderless, so trophy notifications show on top", "fullscreen");
            ToggleRow(SymbolRegular.RatioOneToOne24, "Widescreen", "21:9 and 32:9 outputs (3440 × 1440, 5120 × 1440) with the right field of view",
                () => Ini.Get("widescreen", "1") != "0", v => Ini.Set("widescreen", v ? "1" : "0"));
            SettingCombo(SymbolRegular.Image24, "Presentation mode", null, Choices.PresentModes, "present_mode");

            Group("HDR");
            var hdrHint = WindowsHdr.AnyOn()
                ? "The game's picture on an HDR screen, with brighter highlights. Applied when the game starts"
                : WindowsHdr.AnySupported()
                    ? "Turn on Use HDR in Windows' display settings first (Settings › System › Display)"
                    : "This display shows no HDR";
            IniToggle(SymbolRegular.Hdr24, "HDR output", hdrHint, "hdr");
            ComboRow(SymbolRegular.Lightbulb24, "HDR source",
                "Native: highlights as bright as they are in the game's scene. Expanded: the SDR picture's bright parts made brighter",
                Choices.HdrMode, () => Ini.Get("hdr_mode", "native"), v => Ini.Set("hdr_mode", v));
            SliderRow(SymbolRegular.BrightnessHigh24, "Paper white", "How bright the game's white is, in nits. Range: 10–500",
                10, 500, 1, () => ParseDouble(Ini.Get("hdr_paper_white", "200"), 200),
                v => Ini.Set("hdr_paper_white", v.ToString("0", System.Globalization.CultureInfo.InvariantCulture)),
                format: "0' nits'", valueWidth: 82);
            SliderRow(SymbolRegular.WeatherSunny24, "Peak brightness", "Your display's peak brightness, in nits. Range: 10–4000",
                10, 4000, 10, () => ParseDouble(Ini.Get("hdr_peak", "1000"), 1000),
                v => Ini.Set("hdr_peak", v.ToString("0", System.Globalization.CultureInfo.InvariantCulture)),
                format: "0' nits'", valueWidth: 82);
            ComboRow(SymbolRegular.Sparkle24, "Highlight boost", "Off keeps the game's own look; higher makes lamps, fire and the sky brighter",
                Choices.HdrBoost, () => Ini.Get("hdr_boost", "0.25"), v => Ini.Set("hdr_boost", v));
            ComboRow(SymbolRegular.TextFont24, "UI brightness", "Native source only: how bright the HUD, menus and subtitles are, against paper white",
                Choices.HdrUiBrightness, () => Ini.Get("hdr_ui_brightness", "1"), v => Ini.Set("hdr_ui_brightness", v));
            ComboRow(SymbolRegular.Color24, "UI colour", "Native source only: Vivid shows the HUD and menus with the colours a wide-gamut monitor gives SDR; Exact keeps sRGB's",
                Choices.HdrUiColor, () => Ini.Get("hdr_ui_color", "1"), v => Ini.Set("hdr_ui_color", v));

            Group("Frame rate");
            SettingCombo(SymbolRegular.Gauge24, "Frame rate", "30, 60, 90, 120 and 144 are the game's frame-rate patches for that rate; Uncapped uses the community uncapped patch", Choices.FpsModes, "fps_mode");
        }
    }
}
