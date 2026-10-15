using System.IO;
using System.Text;

namespace BloodborneLauncher.Core
{
    public enum Health { Good, Warning, Bad }

    public sealed record GameStatus(Health Health, string Summary, string TitleId, string Version);

    /// <summary>
    /// A quick look at the game folder: eboot.bin present, and param.sfo naming a retail
    /// Bloodborne at version 01.09. The full check (the eboot.bin image hash, damaged shaders)
    /// is scripts/game_check.py, which the port runs at every start.
    /// </summary>
    public static class GameCheck
    {
        public static readonly string[] SupportedTitles =
        {
            "CUSA00900", "CUSA00207", "CUSA00208", "CUSA00299", "CUSA01363",
            "CUSA03179", "CUSA03173", "CUSA03014", "CUSA03023",
        };

        public const string SupportedVersion = "01.09";

        public static GameStatus Inspect(string gameDir)
        {
            if (string.IsNullOrWhiteSpace(gameDir))
                return new GameStatus(Health.Bad, "No game folder chosen", "", "");
            if (!File.Exists(Path.Combine(gameDir, "eboot.bin")))
                return new GameStatus(Health.Bad, "No eboot.bin in this folder: choose the folder that contains it", "", "");

            var sfo = ReadSfo(Path.Combine(gameDir, "sce_sys", "param.sfo"));
            var title = sfo.GetValueOrDefault("TITLE_ID", "?");
            var version = sfo.GetValueOrDefault("APP_VER", "?");
            // The update dumped beside the game (CUSA03173-UPDATE, as shadPS4 keeps it): the port
            // layers it over the game at start (scripts/mods.py update_folder).
            var update = UpdateFolder(gameDir);
            if (update != null && ReadSfo(Path.Combine(update, "sce_sys", "param.sfo")).GetValueOrDefault("APP_VER", "?") is var updated && updated != "?")
                version = updated;

            if (title != "?" && !SupportedTitles.Contains(title))
                return new GameStatus(Health.Bad,
                    $"This is not a retail Bloodborne (found {title}): any region with update 1.09 is needed", title, version);
            if (version != "?" && version != SupportedVersion)
                return new GameStatus(Health.Warning,
                    $"Update 1.09 is needed: put the dumped 1.09 update beside the game as {Path.GetFileName(Path.TrimEndingDirectorySeparator(gameDir))}-UPDATE, or copy it into the game folder (found {version})", title, version);
            if (title == "?")
                return new GameStatus(Health.Warning, "sce_sys/param.sfo is missing or unreadable; the game may not start", title, version);

            return new GameStatus(Health.Good, update != null
                ? $"Bloodborne {title}, version {version} (update from {Path.GetFileName(update)})"
                : $"Bloodborne {title}, version {version}", title, version);
        }

        /// <summary>The game update beside the game folder (&lt;game&gt;-UPDATE or &lt;game&gt;-patch, any case)
        /// with an eboot.bin, or null.</summary>
        public static string? UpdateFolder(string gameDir)
        {
            try
            {
                var game = new DirectoryInfo(Path.TrimEndingDirectorySeparator(gameDir));
                if (game.Parent is not { } parent) return null;
                return parent.EnumerateDirectories()
                    .Where(d => d.Name.Equals(game.Name + "-UPDATE", StringComparison.OrdinalIgnoreCase) ||
                                d.Name.Equals(game.Name + "-patch", StringComparison.OrdinalIgnoreCase))
                    .FirstOrDefault(d => File.Exists(Path.Combine(d.FullName, "eboot.bin")))?.FullName;
            }
            catch (Exception)
            {
                return null;
            }
        }

        /// <summary>param.sfo: the PS4's key/value table (prepare.py sfo()).</summary>
        public static Dictionary<string, string> ReadSfo(string path)
        {
            var result = new Dictionary<string, string>(StringComparer.Ordinal);
            try
            {
                var data = File.ReadAllBytes(path);
                if (data.Length < 20 || data[0] != 0 || data[1] != 'P' || data[2] != 'S' || data[3] != 'F')
                    return result;
                var keys = BitConverter.ToInt32(data, 8);
                var values = BitConverter.ToInt32(data, 12);
                var count = BitConverter.ToInt32(data, 16);
                for (var i = 0; i < count; i++)
                {
                    var entry = 20 + i * 16;
                    var keyOffset = BitConverter.ToUInt16(data, entry);
                    var format = BitConverter.ToUInt16(data, entry + 2);
                    var size = BitConverter.ToInt32(data, entry + 4);
                    var offset = BitConverter.ToInt32(data, entry + 12);

                    var start = keys + keyOffset;
                    var end = Array.IndexOf(data, (byte)0, start);
                    var key = Encoding.ASCII.GetString(data, start, end - start);
                    var at = values + offset;
                    result[key] = format == 0x404
                        ? BitConverter.ToUInt32(data, at).ToString()
                        : Encoding.UTF8.GetString(data, at, size).TrimEnd('\0');
                }
            }
            catch
            {
                // Unreadable: whatever was parsed so far.
            }
            return result;
        }
    }
}
