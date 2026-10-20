using System.Windows;
using System.Windows.Media;
using BloodborneLauncher.Core;
using Wpf.Ui.Appearance;

namespace BloodborneLauncher.Views
{
    /// <summary>
    /// Colour themes (Settings › Appearance › Theme, live): an accent for everything interactive and
    /// a faint tint of it in the surfaces, merged over Charcoal.xaml / Glass.xaml as one more
    /// dictionary of the same brush keys. Default is no dictionary at all: the look as designed.
    /// </summary>
    public static class LauncherTheme
    {
        public static readonly Choice[] Themes =
        {
            new(L.T("Default"), "default"),
            new(L.T("Crimson"), "crimson", L.T("The blood moon: deep red")),
            new(L.T("Moonlight"), "moon", L.T("Pale blue, like the Moonlight Greatsword")),
            new(L.T("Amber"), "amber", L.T("Lamplight gold")),
            new(L.T("Nightmare"), "nightmare", L.T("Arcane violet")),
            new(L.T("Verdant"), "verdant", L.T("Forbidden Woods green")),
            new("Custom", "custom", L.T("Your own colour, below")),
        };

        private static readonly Dictionary<string, Color> Accents = new()
        {
            ["crimson"] = Color.FromRgb(0xC4, 0x1E, 0x3A),
            ["moon"] = Color.FromRgb(0x8F, 0xB8, 0xE8),
            ["amber"] = Color.FromRgb(0xD9, 0x9A, 0x2B),
            ["nightmare"] = Color.FromRgb(0x8E, 0x63, 0xD6),
            ["verdant"] = Color.FromRgb(0x3F, 0xA8, 0x6B),
        };

        public const string DefaultCustomColour = "#C41E3A";

        private static ResourceDictionary? _overlay;
        private static Color? _originalAccent;

        /// <summary>The theme's accent; null for Default (or a custom colour that doesn't parse).</summary>
        public static Color? Accent(LauncherSettings s)
        {
            var theme = s.Str("ui_theme");
            if (theme == "custom")
                return ParseColour(s.Str("ui_theme_colour"));
            return Accents.TryGetValue(theme, out var colour) ? colour : null;
        }

        public static Color? ParseColour(string text)
        {
            text = text.Trim();
            if (text.Length is 6 or 8 && !text.StartsWith('#')) text = "#" + text;
            try
            {
                return ColorConverter.ConvertFromString(text) is Color c ? Color.FromRgb(c.R, c.G, c.B) : null;
            }
            catch (FormatException)
            {
                return null;
            }
        }

        /// <summary>Applies the saved theme; glass: the Modern layout's dictionary is in use.</summary>
        public static void Apply(LauncherSettings s, bool glass)
        {
            var resources = Application.Current.Resources;
            if (_overlay != null)
            {
                resources.MergedDictionaries.Remove(_overlay);
                _overlay = null;
            }
            _originalAccent ??= resources["SystemAccentColor"] is Color original ? original : null;

            var accent = Accent(s);
            if (accent is not { } a)
            {
                if (_originalAccent is { } back)
                    ApplicationAccentColorManager.Apply(back, ApplicationTheme.Dark);
                return;
            }
            ApplicationAccentColorManager.Apply(a, ApplicationTheme.Dark);

            var d = new ResourceDictionary();
            void Solid(string key, Color c) => d[key] = Freeze(new SolidColorBrush(c));
            Solid("AccentTextFillColorPrimaryBrush", Mix(a, Colors.White, 0.25));
            if (glass)
            {
                d["SegmentSelectedBrush"] = Freeze(new LinearGradientBrush(WithAlpha(Mix(a, Colors.White, 0.2), 0xC0),
                    WithAlpha(a, 0x80), 90));
                Solid("WindowBaseBrush", Mix(Color.FromRgb(0x0C, 0x0D, 0x10), a, 0.10));
                Solid("PillTrackBrush", WithAlpha(Mix(Colors.Black, a, 0.15), 0x40));
            }
            else
            {
                Solid("SegmentSelectedBrush", Mix(a, Colors.Black, 0.1));
                Solid("WindowBaseBrush", Mix(Color.FromRgb(0x24, 0x24, 0x26), a, 0.06));
                Solid("ContentAreaBrush", Mix(Color.FromRgb(0x29, 0x29, 0x2B), a, 0.06));
                Solid("CardBackgroundFillColorDefaultBrush", Mix(Color.FromRgb(0x2E, 0x2E, 0x31), a, 0.07));
                Solid("CardStrokeColorDefaultBrush", Mix(Color.FromRgb(0x3A, 0x3A, 0x3E), a, 0.12));
                Solid("SidebarItemSelectedBrush", Mix(Color.FromRgb(0x37, 0x37, 0x3B), a, 0.20));
                Solid("SidebarItemHoverBrush", Mix(Color.FromRgb(0x2E, 0x2E, 0x31), a, 0.08));
                Solid("PillTrackBrush", Mix(Color.FromRgb(0x1E, 0x1E, 0x21), a, 0.05));
                Solid("DialogBackgroundBrush", Mix(Color.FromRgb(0x2C, 0x2C, 0x2F), a, 0.06));
                Solid("PanelDeepBrush", Mix(Color.FromRgb(0x1E, 0x1E, 0x20), a, 0.04));
            }
            resources.MergedDictionaries.Add(d);
            _overlay = d;
        }

        /// <summary>The backdrop shade's colour (the dark wash over the background picture).</summary>
        public static Color ShadeColour(LauncherSettings s) =>
            Accent(s) is { } a ? Mix(Color.FromRgb(0x0A, 0x0B, 0x0E), a, 0.12) : Color.FromRgb(0x0A, 0x0B, 0x0E);

        public static Color Mix(Color from, Color to, double amount) => Color.FromRgb(
            (byte)Math.Round(from.R + (to.R - from.R) * amount),
            (byte)Math.Round(from.G + (to.G - from.G) * amount),
            (byte)Math.Round(from.B + (to.B - from.B) * amount));

        public static Color WithAlpha(Color c, byte alpha) => Color.FromArgb(alpha, c.R, c.G, c.B);

        private static T Freeze<T>(T brush) where T : Freezable
        {
            brush.Freeze();
            return brush;
        }
    }
}
