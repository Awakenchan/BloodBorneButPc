using System.Diagnostics;
using System.IO;
using System.Text;
using Microsoft.Win32;

namespace BloodborneLauncher.Core
{
    /// <summary>
    /// "Add to Steam": Bloodborne.exe (starts the game with the launcher's settings and waits for it)
    /// as a non-Steam game, in shortcuts.vdf of the Steam account last signed in on this PC. Steam
    /// reads that file at start and writes it back on exit, so it must be closed while it changes.
    /// </summary>
    public static class SteamShortcut
    {
        public const string AppName = "Bloodborne";

        /// <summary>Steam's folder, or null when Steam is not installed.</summary>
        public static string? SteamDir()
        {
            var path = Registry.GetValue(@"HKEY_CURRENT_USER\Software\Valve\Steam", "SteamPath", null) as string;
            return path is { Length: > 0 } && Directory.Exists(path) ? Path.GetFullPath(path) : null;
        }

        public static bool SteamRunning() => Process.GetProcessesByName("steam").Length > 0;

        /// <summary>Adds (or updates) the shortcut. Returns the Steam account folders it went into.</summary>
        public static List<string> Add(string exe)
        {
            var steam = SteamDir() ?? throw new InvalidOperationException(L.T("Steam is not installed for this Windows user."));
            if (SteamRunning())
                throw new InvalidOperationException(L.T("Close Steam first (Steam › Exit): it rewrites its shortcut list when it closes."));
            var accounts = Accounts(steam);
            if (accounts.Count == 0)
                throw new InvalidOperationException(L.T("No Steam account has signed in on this PC yet: start Steam once and sign in."));
            var added = new List<string>();
            foreach (var account in accounts)
            {
                var config = Path.Combine(steam, "userdata", account, "config");
                Directory.CreateDirectory(config);
                var file = Path.Combine(config, "shortcuts.vdf");
                var root = File.Exists(file) ? Vdf.Read(File.ReadAllBytes(file)) : new Vdf.Node();
                var list = root.Child("shortcuts");
                var quoted = $"\"{exe}\"";
                // An earlier entry of ours (the same program) is replaced, not doubled.
                var existing = list.Children.FirstOrDefault(c => c.Value is Vdf.Node n &&
                    string.Equals((n.Get("Exe") ?? n.Get("exe")) as string, quoted, StringComparison.OrdinalIgnoreCase));
                var entry = existing.Value as Vdf.Node ?? new Vdf.Node();
                if (existing.Key is null)
                {
                    // Entries are keyed "0", "1", …: the next after the highest.
                    var next = list.Children.Select(e => int.TryParse(e.Key, out var i) ? i + 1 : 0).DefaultIfEmpty(0).Max();
                    list.Children.Add(new(next.ToString(), entry));
                }
                entry.Set("appid", AppId(quoted, AppName));
                entry.Set("AppName", AppName);
                entry.Set("Exe", quoted);
                entry.Set("StartDir", $"\"{Path.GetDirectoryName(exe)}\\\"");
                entry.Set("icon", exe);
                entry.Set("ShortcutPath", "");
                entry.Set("LaunchOptions", "");
                entry.Set("IsHidden", 0);
                entry.Set("AllowDesktopConfig", 1);
                entry.Set("AllowOverlay", 1);
                entry.Set("OpenVR", 0);
                entry.Set("Devkit", 0);
                entry.Set("DevkitGameID", "");
                entry.Set("DevkitOverrideAppID", 0);
                entry.Set("LastPlayTime", entry.Get("LastPlayTime") ?? 0);
                entry.Set("FlatpakAppID", "");
                entry.Child("tags");
                if (File.Exists(file)) File.Copy(file, file + ".bak", overwrite: true);
                File.WriteAllBytes(file, Vdf.Write(root));
                added.Add(account);
            }
            return added;
        }

        /// <summary>The account last signed in (loginusers.vdf MostRecent), else every account folder.</summary>
        private static List<string> Accounts(string steam)
        {
            var folders = Directory.Exists(Path.Combine(steam, "userdata"))
                ? Directory.GetDirectories(Path.Combine(steam, "userdata")).Select(Path.GetFileName)
                    .Where(n => n is { Length: > 0 } && n.All(char.IsDigit) && n != "0").Select(n => n!).ToList()
                : new List<string>();
            try
            {
                // "7656119…" { … "MostRecent" "1" … }: the account id is the SteamID64's low part.
                var text = File.ReadAllText(Path.Combine(steam, "config", "loginusers.vdf"));
                var blocks = System.Text.RegularExpressions.Regex.Matches(text, "\"(7656\\d{13})\"\\s*\\{([^}]*)\\}");
                foreach (System.Text.RegularExpressions.Match m in blocks)
                {
                    if (!m.Groups[2].Value.Contains("\"MostRecent\"\t\t\"1\"") &&
                        !System.Text.RegularExpressions.Regex.IsMatch(m.Groups[2].Value, "\"MostRecent\"\\s*\"1\""))
                        continue;
                    var id = (ulong.Parse(m.Groups[1].Value) - 76561197960265728UL).ToString();
                    if (folders.Contains(id)) return new List<string> { id };
                }
            }
            catch (Exception) { }
            return folders;
        }

        /// <summary>The id Steam gives a shortcut (CRC-32 of exe and name, top bit set): artwork and
        /// controller layouts found for it stay with it.</summary>
        private static int AppId(string exe, string name)
        {
            uint crc = 0xFFFFFFFF;
            foreach (var b in Encoding.UTF8.GetBytes(exe + name))
            {
                crc ^= b;
                for (var k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320 & (uint)-(int)(crc & 1));
            }
            return unchecked((int)(~crc | 0x80000000));
        }

        /// <summary>Steam's binary KeyValues: 0 a map, 1 a string, 2 an int32, 8 the end of a map.</summary>
        private static class Vdf
        {
            public sealed class Node
            {
                public List<KeyValuePair<string, object>> Children { get; } = new();

                public object? Get(string key) =>
                    Children.FirstOrDefault(c => string.Equals(c.Key, key, StringComparison.OrdinalIgnoreCase)).Value;

                public void Set(string key, object value)
                {
                    var i = Children.FindIndex(c => string.Equals(c.Key, key, StringComparison.OrdinalIgnoreCase));
                    if (i >= 0) Children[i] = new(Children[i].Key, value);
                    else Children.Add(new(key, value));
                }

                public Node Child(string key)
                {
                    if (Get(key) is Node n) return n;
                    var created = new Node();
                    Set(key, created);
                    return created;
                }
            }

            public static Node Read(byte[] data)
            {
                var at = 0;
                return ReadNode(data, ref at);
            }

            private static Node ReadNode(byte[] d, ref int at)
            {
                var node = new Node();
                while (at < d.Length)
                {
                    var type = d[at++];
                    if (type == 8) break;
                    var key = ReadString(d, ref at);
                    switch (type)
                    {
                        case 0: node.Children.Add(new(key, ReadNode(d, ref at))); break;
                        case 1: node.Children.Add(new(key, ReadString(d, ref at))); break;
                        case 2: node.Children.Add(new(key, BitConverter.ToInt32(d, at))); at += 4; break;
                        case 7: node.Children.Add(new(key, BitConverter.ToUInt64(d, at))); at += 8; break;
                        default: throw new InvalidDataException(L.F($"shortcuts.vdf: unknown value type {type}"));
                    }
                }
                return node;
            }

            private static string ReadString(byte[] d, ref int at)
            {
                var end = Array.IndexOf(d, (byte)0, at);
                if (end < 0) throw new InvalidDataException(L.T("shortcuts.vdf: unterminated string"));
                var s = Encoding.UTF8.GetString(d, at, end - at);
                at = end + 1;
                return s;
            }

            public static byte[] Write(Node root)
            {
                using var stream = new MemoryStream();
                WriteNode(stream, root);
                stream.WriteByte(8);
                return stream.ToArray();
            }

            private static void WriteNode(Stream s, Node node)
            {
                foreach (var (key, value) in node.Children)
                {
                    switch (value)
                    {
                        case Node child:
                            s.WriteByte(0); WriteString(s, key); WriteNode(s, child); s.WriteByte(8); break;
                        case string text:
                            s.WriteByte(1); WriteString(s, key); WriteString(s, text); break;
                        case int number:
                            s.WriteByte(2); WriteString(s, key); s.Write(BitConverter.GetBytes(number)); break;
                        case ulong wide:
                            s.WriteByte(7); WriteString(s, key); s.Write(BitConverter.GetBytes(wide)); break;
                    }
                }
            }

            private static void WriteString(Stream s, string text)
            {
                s.Write(Encoding.UTF8.GetBytes(text));
                s.WriteByte(0);
            }
        }
    }
}
