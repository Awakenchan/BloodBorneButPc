using System.Globalization;
using System.IO;
using System.Security.Cryptography;
using System.Text;

namespace BloodborneLauncher.Core
{
    /// <summary>
    /// Copies of the current profile's savedata, made before each start when it changed since the
    /// last copy; the newest ten are kept, in launcher-backups\&lt;profile&gt;\&lt;time&gt; in the saves
    /// folder. Restoring first backs up what is there now.
    /// </summary>
    public static class SaveBackups
    {
        public const int Keep = 10;
        private const string Stamp = "yyyy-MM-dd_HH-mm-ss";
        private const string BeforeRestore = "_before-restore";
        private const string FingerprintFile = "fingerprint.txt";

        public sealed record Backup(string Dir, DateTime When, long Bytes, bool MadeBeforeRestore);

        public static string SaveData(LauncherSettings s) => Path.Combine(GameEnvironment.UserDir(s), "savedata");

        public static string Folder(LauncherSettings s) =>
            Path.Combine(GameEnvironment.SavesRoot(s), "launcher-backups", PlayerProfiles.Current(s));

        /// <summary>This profile's backups, newest first.</summary>
        public static List<Backup> List(LauncherSettings s) => ListIn(Folder(s));

        /// <summary>The backups in a profile's backup folder, newest first. Reads the disk only (each
        /// backup's size too), so it can run on a worker thread.</summary>
        public static List<Backup> ListIn(string folder)
        {
            if (!Directory.Exists(folder)) return new();
            var list = new List<Backup>();
            foreach (var dir in Directory.GetDirectories(folder))
            {
                var name = Path.GetFileName(dir);
                var before = name.EndsWith(BeforeRestore, StringComparison.Ordinal);
                if (before) name = name[..^BeforeRestore.Length];
                if (DateTime.TryParseExact(name, Stamp, CultureInfo.InvariantCulture, DateTimeStyles.None, out var when))
                    list.Add(new Backup(dir, when, Size(dir), before));
            }
            return list.OrderByDescending(b => b.When).ToList();
        }

        /// <summary>Copies the savedata unless the newest backup already holds the same files.
        /// Returns the new backup's folder, or null when there was nothing new to copy.</summary>
        public static string? Create(LauncherSettings s, bool beforeRestore = false)
        {
            var source = SaveData(s);
            if (!Directory.Exists(source) || !Directory.EnumerateFiles(source, "*", SearchOption.AllDirectories).Any())
                return null;
            var print = Fingerprint(source);
            if (!beforeRestore && List(s).FirstOrDefault() is { } newest && ReadFingerprint(newest.Dir) == print)
                return null;

            var target = Path.Combine(Folder(s), DateTime.Now.ToString(Stamp, CultureInfo.InvariantCulture) + (beforeRestore ? BeforeRestore : ""));
            if (Directory.Exists(target)) return null; // one a second is plenty
            Copy(source, Path.Combine(target, "savedata"));
            File.WriteAllText(Path.Combine(target, FingerprintFile), print);
            foreach (var old in List(s).Skip(Keep))
            {
                try { Directory.Delete(old.Dir, recursive: true); } catch { /* in use: next time */ }
            }
            return target;
        }

        /// <summary>Puts a backup's savedata back, after backing up the current one.</summary>
        public static void Restore(LauncherSettings s, Backup backup)
        {
            var from = Path.Combine(backup.Dir, "savedata");
            if (!Directory.Exists(from)) throw new DirectoryNotFoundException(L.T("This backup has no savedata folder."));
            Create(s, beforeRestore: true);

            var target = SaveData(s);
            // Copied next to it first, so a failed copy leaves the current saves in place.
            var staging = target + ".restoring";
            if (Directory.Exists(staging)) Directory.Delete(staging, recursive: true);
            Copy(from, staging);
            if (Directory.Exists(target)) Directory.Delete(target, recursive: true);
            Directory.Move(staging, target);
        }

        /// <summary>A save import's staging folder (SaveImport): never part of the saves. One left by
        /// an older import held a whole Downloads folder, copied into every backup.</summary>
        private static bool Staging(string from, string path) =>
            Path.GetRelativePath(from, path).Split(Path.DirectorySeparatorChar)
                .Any(part => part.EndsWith(".importing", StringComparison.OrdinalIgnoreCase));

        private static void Copy(string from, string to)
        {
            Directory.CreateDirectory(to);
            foreach (var dir in Directory.GetDirectories(from, "*", SearchOption.AllDirectories).Where(d => !Staging(from, d)))
                Directory.CreateDirectory(Path.Combine(to, Path.GetRelativePath(from, dir)));
            foreach (var file in Directory.GetFiles(from, "*", SearchOption.AllDirectories).Where(f => !Staging(from, f)))
                File.Copy(file, Path.Combine(to, Path.GetRelativePath(from, file)), overwrite: true);
        }

        /// <summary>Names, sizes and times of the files: equal when nothing was saved since.</summary>
        private static string Fingerprint(string dir)
        {
            var text = new StringBuilder();
            foreach (var file in Directory.GetFiles(dir, "*", SearchOption.AllDirectories).Where(f => !Staging(dir, f)).OrderBy(f => f, StringComparer.Ordinal))
            {
                var info = new FileInfo(file);
                text.Append(Path.GetRelativePath(dir, file)).Append('|').Append(info.Length).Append('|')
                    .Append(info.LastWriteTimeUtc.Ticks).Append('\n');
            }
            return Convert.ToHexString(SHA1.HashData(Encoding.UTF8.GetBytes(text.ToString())));
        }

        private static string? ReadFingerprint(string backup)
        {
            try { return File.ReadAllText(Path.Combine(backup, FingerprintFile)).Trim(); }
            catch { return null; }
        }

        private static long Size(string dir)
        {
            try { return Directory.GetFiles(dir, "*", SearchOption.AllDirectories).Sum(f => new FileInfo(f).Length); }
            catch { return 0; }
        }
    }
}
