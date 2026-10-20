using System.IO;
using System.Windows;
using System.Windows.Media.Imaging;

namespace BloodborneLauncher.Core
{
    /// <summary>
    /// Settings › Appearance › App icon (app_icon): the launcher window's and the game window's
    /// icon. Auto is the build's own (the red lantern for betas, the blue one for stable releases);
    /// the exe file keeps the build's icon whatever is chosen.
    /// </summary>
    public static class AppIcon
    {
        public static readonly Choice[] Choices =
        {
            new(L.T("Auto"), "auto", L.T("Red lantern in betas, blue in stable releases")),
            new(L.T("Red lantern"), "red"),
            new(L.T("Blue lantern"), "blue"),
            new(L.T("Lantern by j3loo"), "j3loo", L.T("Fan art by j3loo")),
        };

        private static string Resource(string choice) => choice switch
        {
            "red" => "lantern-beta.ico",
            "blue" => "lantern-stable.ico",
            "j3loo" => "lantern-j3loo.ico",
            _ => "app.ico",
        };

        /// <summary>The chosen icon's .ico file, as the launcher carries it.</summary>
        public static byte[]? Bytes(string choice)
        {
            try
            {
                using var source = Application.GetResourceStream(new Uri($"pack://application:,,,/{Resource(choice)}"))?.Stream;
                if (source == null) return null;
                using var memory = new MemoryStream();
                source.CopyTo(memory);
                return memory.ToArray();
            }
            catch (IOException)
            {
                return null;
            }
        }

        /// <summary>The launcher window shows the chosen icon; the game window takes it at its next start.</summary>
        public static void Apply(Window? window, string choice)
        {
            if (window == null) return;
            try
            {
                window.Icon = BitmapFrame.Create(new Uri($"pack://application:,,,/{Resource(choice)}"));
            }
            catch (Exception)
            {
                // The window keeps the icon it has.
            }
        }

        /// <summary>The game window's icon file (engine\out\bloodborne.ico), when it differs.</summary>
        public static void SyncGameWindow(string package, string choice)
        {
            try
            {
                var target = Path.Combine(Paths.EngineDirOf(package), "out", "bloodborne.ico");
                if (!Directory.Exists(Path.GetDirectoryName(target))) return;
                if (Bytes(choice) is not { } bytes) return;
                if (!File.Exists(target) || !File.ReadAllBytes(target).AsSpan().SequenceEqual(bytes))
                    File.WriteAllBytes(target, bytes);
            }
            catch (Exception)
            {
                // The old icon stays: nothing else depends on it.
            }
        }
    }
}
