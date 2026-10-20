using System.IO;

namespace BloodborneLauncher.Core
{
    /// <summary>
    /// Player profiles: several people progressing on one PC. The engine keeps saves and the trophy log
    /// in its user folder (BB_USER_DIR), so a profile is just its own folder: Main is the saves folder
    /// itself (what existed before profiles), every other profile is saves\profiles\&lt;name&gt;.
    /// Profiles are never deleted from disk here; removing one only takes it off the list.
    /// </summary>
    public static class PlayerProfiles
    {
        public const string Main = "Main";

        public static List<string> All(LauncherSettings s)
        {
            var list = new List<string> { Main };
            foreach (var name in s.Str("player_profiles").Split('|', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries))
                if (!list.Contains(name, StringComparer.OrdinalIgnoreCase))
                    list.Add(name);
            return list;
        }

        public static string Current(LauncherSettings s)
        {
            var name = s.Str("player_profile");
            return name.Length > 0 && All(s).Contains(name, StringComparer.OrdinalIgnoreCase) ? name : Main;
        }

        /// <summary>The folder a profile's saves and trophies live in.</summary>
        public static string Folder(LauncherSettings s, string profile) =>
            IsMain(profile) ? GameEnvironment.SavesRoot(s) : Path.Combine(GameEnvironment.SavesRoot(s), "profiles", SafeName(profile));

        public static bool IsMain(string profile) => string.Equals(profile, Main, StringComparison.OrdinalIgnoreCase);

        /// <summary>A usable profile name, or null with the reason in <paramref name="error"/>.</summary>
        public static string? Validate(LauncherSettings s, string name, out string error, string? renaming = null)
        {
            name = name.Trim();
            error = "";
            if (name.Length == 0) { error = L.T("Enter a name."); return null; }
            if (name.Length > 32) { error = L.T("Use 32 characters or fewer."); return null; }
            if (SafeName(name) != name) { error = L.T("Use letters, numbers, spaces, - or _ only."); return null; }
            if (All(s).Any(p => p.Equals(name, StringComparison.OrdinalIgnoreCase) && !p.Equals(renaming, StringComparison.OrdinalIgnoreCase)))
            {
                error = L.F($"There is already a profile called {name}.");
                return null;
            }
            return name;
        }

        public static void Add(LauncherSettings s, string name)
        {
            var others = All(s).Where(p => !IsMain(p)).Append(name);
            s.Set("player_profiles", string.Join("|", others));
            Directory.CreateDirectory(Folder(s, name));
        }

        /// <summary>Renames a profile and moves its folder with it.</summary>
        public static void Rename(LauncherSettings s, string from, string to)
        {
            var oldDir = Folder(s, from);
            var newDir = Folder(s, to);
            if (Directory.Exists(oldDir) && !Directory.Exists(newDir))
                Directory.Move(oldDir, newDir);
            s.Set("player_profiles", string.Join("|", All(s).Where(p => !IsMain(p)).Select(p => p.Equals(from, StringComparison.OrdinalIgnoreCase) ? to : p)));
            if (Current(s).Equals(from, StringComparison.OrdinalIgnoreCase) || s.Str("player_profile").Equals(from, StringComparison.OrdinalIgnoreCase))
                s.Set("player_profile", to);
        }

        /// <summary>Takes a profile off the list; its folder (saves and trophies) stays on disk.</summary>
        public static void Remove(LauncherSettings s, string name)
        {
            s.Set("player_profiles", string.Join("|", All(s).Where(p => !IsMain(p) && !p.Equals(name, StringComparison.OrdinalIgnoreCase))));
            if (Current(s).Equals(Main) || s.Str("player_profile").Equals(name, StringComparison.OrdinalIgnoreCase))
                s.Set("player_profile", "");
        }

        private static string SafeName(string name) =>
            new string(name.Where(c => char.IsLetterOrDigit(c) || c is ' ' or '-' or '_').ToArray()).Trim();
    }
}
