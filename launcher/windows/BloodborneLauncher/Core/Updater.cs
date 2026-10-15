using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Net.Http;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace BloodborneLauncher.Core
{
    /// <summary>
    /// Updates from the GitHub releases of the repository. Each release carries the whole package
    /// (BloodbornePC-*.zip), the launcher on its own ("Bloodborne Launcher.exe") and package.json,
    /// which names the engine build inside the package.
    ///
    /// When only the launcher changed, the exe alone is downloaded. When the engine changed (or the
    /// package has no package.json yet), the whole package is downloaded and copied over this one,
    /// keeping the player's settings, saves, mods and profiles. A running exe cannot be overwritten
    /// but can be renamed, so the launcher moves itself aside and is removed on the next start.
    ///
    /// Two channels: stable follows GitHub's latest release; beta also takes pre-releases, tagged
    /// v&lt;version&gt;-beta.&lt;n&gt;. A beta comes before the stable release of the same version.
    /// </summary>
    public static class Updater
    {
        public const string Repository = "Awakenchan/BloodBorneButPc";
        public static string ReleasesPage => $"https://github.com/{Repository}/releases";

        /// <summary>The engine build this launcher is made for (package.json "engine").</summary>
        public const string EngineId = "bbhost-0.4.0-fps49";

        private const string ExeName = "Bloodborne Launcher.exe";
        private const string PackageFile = "package.json";

        /// <summary>Files and folders of the player's that a package update never touches.</summary>
        private static readonly string[] Kept = { "bbport.ini", "mods.json", "patches.json", "user", "mods", "logs", "network", "engine-assets" };

        public sealed record Release(Version Version, int Beta, string Tag, string Name, string Notes, string Page,
            string? ExeUrl, string? ZipUrl, string? PackageUrl, long ExeSize, long ZipSize, DateTime Published = default)
        {
            public bool IsNewerThan(Version version, int beta) => Compare(Version, Beta, version, beta) > 0;
            public bool IsCurrent => Compare(Version, Beta, Current, CurrentBeta) == 0;
        }

        public static Version Current
        {
            get
            {
                var v = typeof(Updater).Assembly.GetName().Version ?? new Version(0, 0, 0);
                return new Version(v.Major, v.Minor, Math.Max(0, v.Build));
            }
        }

        /// <summary>The beta number of this build (from its informational version, "0.9.7-beta.2"), 0 when stable.</summary>
        public static int CurrentBeta { get; } =
            TryVersion(typeof(Updater).Assembly.GetCustomAttributes(typeof(System.Reflection.AssemblyInformationalVersionAttribute), false)
                .OfType<System.Reflection.AssemblyInformationalVersionAttribute>().FirstOrDefault()?.InformationalVersion ?? "", out _, out var beta)
                ? beta
                : 0;

        /// <summary>This build as players see it: "v0.9.6" or "v0.9.7 beta 2".</summary>
        public static string CurrentLabel => Label(Current, CurrentBeta);

        /// <summary>The channel the player chose; until they choose, a beta build stays on beta.</summary>
        public static bool BetaChannel(LauncherSettings s) => s.Str("update_channel") switch
        {
            "beta" => true,
            "stable" => false,
            _ => CurrentBeta > 0,
        };

        public static string Label(Version version, int beta) => beta > 0 ? $"v{version} beta {beta}" : $"v{version}";

        /// <summary>Orders builds: by version, and a version's betas before its stable release.</summary>
        public static int Compare(Version a, int betaA, Version b, int betaB)
        {
            var byVersion = a.CompareTo(b);
            if (byVersion != 0) return byVersion;
            return (betaA == 0 ? int.MaxValue : betaA).CompareTo(betaB == 0 ? int.MaxValue : betaB);
        }

        /// <summary>The package this launcher runs in (it ships inside it), or null when it runs on its own.</summary>
        public static string? OwnPackage =>
            Paths.DetectPortDir() is { } dir && Environment.ProcessPath is { } self &&
            string.Equals(Path.GetDirectoryName(self)?.TrimEnd('\\'), dir.TrimEnd('\\'), StringComparison.OrdinalIgnoreCase)
                ? dir
                : null;

        /// <summary>The engine build in the package, from its package.json; null when it has none.</summary>
        public static string? LocalEngine()
        {
            try
            {
                var file = Path.Combine(Paths.PortDir, PackageFile);
                return File.Exists(file) && JsonNode.Parse(File.ReadAllText(file)) is JsonObject o ? (string?)o["engine"] : null;
            }
            catch
            {
                return null;
            }
        }

        /// <summary>The package needs the engine this launcher is made for.</summary>
        public static bool EngineOutdated => OwnPackage != null && LocalEngine() != EngineId;

        /// <summary>The latest release on the channel, or null when there is none (or no network).
        /// Stable: GitHub's latest release. Beta: the newest of all releases, pre-releases included.</summary>
        public static async Task<Release?> LatestAsync(bool beta = false)
        {
            using var http = Client(TimeSpan.FromSeconds(10));
            using var response = await http.GetAsync(beta
                ? $"https://api.github.com/repos/{Repository}/releases?per_page=30"
                : $"https://api.github.com/repos/{Repository}/releases/latest");
            if (response.StatusCode == System.Net.HttpStatusCode.NotFound)
                return null;
            response.EnsureSuccessStatusCode();
            using var doc = JsonDocument.Parse(await response.Content.ReadAsStringAsync());
            if (!beta)
                return Parse(doc.RootElement);

            Release? newest = null;
            foreach (var item in doc.RootElement.EnumerateArray())
            {
                if (item.TryGetProperty("draft", out var draft) && draft.GetBoolean()) continue;
                if (Parse(item) is { } release && (newest == null || release.IsNewerThan(newest.Version, newest.Beta)))
                    newest = release;
            }
            return newest;
        }

        /// <summary>Every published release, stable and beta, newest first (the version selector).</summary>
        public static async Task<IReadOnlyList<Release>> AllAsync()
        {
            using var http = Client(TimeSpan.FromSeconds(15));
            using var response = await http.GetAsync($"https://api.github.com/repos/{Repository}/releases?per_page=100");
            response.EnsureSuccessStatusCode();
            using var doc = JsonDocument.Parse(await response.Content.ReadAsStringAsync());
            var all = new List<Release>();
            foreach (var item in doc.RootElement.EnumerateArray())
            {
                if (item.TryGetProperty("draft", out var draft) && draft.GetBoolean()) continue;
                if (Parse(item) is { } release && (release.ZipUrl != null || release.ExeUrl != null))
                    all.Add(release);
            }
            all.Sort((a, b) => Compare(b.Version, b.Beta, a.Version, a.Beta));
            return all;
        }

        /// <summary>Whether switching to a chosen release replaces the whole package: its engine differs
        /// from this one's, or it does not say which engine it has (older releases).</summary>
        public static async Task<bool> SwitchNeedsPackageAsync(Release release)
        {
            if (OwnPackage == null || release.ZipUrl == null) return false;
            if (release.ExeUrl == null) return true;
            var remote = await RemoteEngineAsync(release);
            return remote == null || remote != LocalEngine();
        }

        private static Release? Parse(JsonElement root)
        {
            var tag = root.GetProperty("tag_name").GetString() ?? "";
            if (!TryVersion(tag, out var version, out var betaNumber))
                return null;

            string? exe = null, zip = null, package = null;
            long exeSize = 0, zipSize = 0;
            foreach (var asset in root.GetProperty("assets").EnumerateArray())
            {
                var name = asset.GetProperty("name").GetString() ?? "";
                var url = asset.GetProperty("browser_download_url").GetString();
                var size = asset.GetProperty("size").GetInt64();
                // GitHub stores "Bloodborne Launcher.exe" as "Bloodborne.Launcher.exe".
                if (name.Replace(".", " ").Replace(" exe", ".exe").Equals(ExeName, StringComparison.OrdinalIgnoreCase))
                    (exe, exeSize) = (url, size);
                else if (name.EndsWith(".zip", StringComparison.OrdinalIgnoreCase) && zip == null)
                    (zip, zipSize) = (url, size);
                else if (name.Equals(PackageFile, StringComparison.OrdinalIgnoreCase))
                    package = url;
            }
            return new Release(version, betaNumber, tag,
                root.TryGetProperty("name", out var n) && n.GetString() is { Length: > 0 } title ? title : tag,
                root.TryGetProperty("body", out var b) ? b.GetString() ?? "" : "",
                root.GetProperty("html_url").GetString() ?? ReleasesPage, exe, zip, package, exeSize, zipSize,
                root.TryGetProperty("published_at", out var p) && p.ValueKind == JsonValueKind.String &&
                DateTime.TryParse(p.GetString(), null, System.Globalization.DateTimeStyles.RoundtripKind, out var when)
                    ? when.ToLocalTime()
                    : default);
        }

        /// <summary>Reads "v0.9.7", "0.9.7-beta.2" or "0.9.7-beta2+abc123"; <paramref name="beta"/> is 0 for a stable build.</summary>
        public static bool TryVersion(string tag, out Version version, out int beta)
        {
            var text = tag.Trim().TrimStart('v', 'V');
            var plus = text.IndexOf('+');
            if (plus > 0) text = text[..plus];
            beta = 0;
            var marker = text.IndexOf("-beta", StringComparison.OrdinalIgnoreCase);
            if (marker > 0)
            {
                var digits = new string(text[(marker + 5)..].Where(char.IsDigit).ToArray());
                beta = int.TryParse(digits, out var n) && n > 0 ? n : 1;
            }
            var cut = text.IndexOfAny(new[] { '-', '+', ' ' });
            if (cut > 0) text = text[..cut];
            if (Version.TryParse(text.Contains('.') ? text : text + ".0", out var v))
            {
                version = new Version(v.Major, v.Minor, Math.Max(0, v.Build));
                return true;
            }
            version = new Version(0, 0, 0);
            return false;
        }

        /// <summary>The engine build the release's package has, or null when it does not say.</summary>
        public static async Task<string?> RemoteEngineAsync(Release release)
        {
            if (release.PackageUrl == null) return null;
            try
            {
                using var http = Client(TimeSpan.FromSeconds(10));
                return JsonNode.Parse(await http.GetStringAsync(release.PackageUrl)) is JsonObject o ? (string?)o["engine"] : null;
            }
            catch
            {
                return null;
            }
        }

        /// <summary>Whether this update has to replace the whole package (a new engine).</summary>
        public static async Task<bool> NeedsPackageAsync(Release release)
        {
            if (OwnPackage == null || release.ZipUrl == null) return false;
            var remote = await RemoteEngineAsync(release);
            return remote != null ? remote != LocalEngine() : LocalEngine() == null && release.ExeUrl == null;
        }

        /// <summary>Downloads the update, puts it in place and starts the new launcher; the caller then exits.
        /// Returns the package files that could not be put in place and were left as they were (only
        /// optional ones: see <see cref="Optional"/>).</summary>
        public static async Task<IReadOnlyList<string>> InstallAsync(Release release, bool package, IProgress<double>? progress = null)
        {
            var self = Environment.ProcessPath ?? throw new InvalidOperationException("Cannot tell where this launcher is.");
            var folder = Path.GetDirectoryName(self)!;
            // A folder of its own each time: one an antivirus still holds a file in (from an earlier
            // attempt) cannot be deleted, and must not stop this one.
            var parent = Path.Combine(Path.GetTempPath(), "bbport-launcher-update");
            if (Directory.Exists(parent))
            {
                foreach (var old in Directory.GetDirectories(parent).Append(parent))
                    try { if (old != parent) Directory.Delete(old, recursive: true); } catch (Exception) { }
                foreach (var old in Directory.GetFiles(parent))
                    try { File.Delete(old); } catch (Exception) { }
            }
            var work = Path.Combine(parent, DateTime.Now.ToString("yyyyMMdd-HHmmss"));
            Directory.CreateDirectory(work);
            var skipped = new List<string>();

            if (package)
            {
                if (Process.GetProcessesByName("bb-probe").Length > 0)
                    throw new InvalidOperationException("Close the game first: the update replaces the engine.");
                var zip = Path.Combine(work, "package.zip");
                await DownloadAsync(release.ZipUrl!, zip, release.ZipSize, progress);
                var unpacked = Path.Combine(work, "unpacked");
                Extract(zip, unpacked, skipped);
                var root = Directory.GetFiles(unpacked, ExeName, SearchOption.AllDirectories).Select(Path.GetDirectoryName).FirstOrDefault()
                           ?? throw new InvalidOperationException($"The package has no {ExeName}.");
                CopyPackage(root!, folder, self, skipped);
            }
            else
            {
                var url = release.ExeUrl ?? throw new InvalidOperationException("This release has no launcher download attached.");
                var exe = Path.Combine(work, ExeName);
                await DownloadAsync(url, exe, release.ExeSize, progress);
                if (new FileInfo(exe).Length < 100_000)
                    throw new InvalidOperationException("The download is too small to be the launcher.");
                ReplaceSelf(self, exe);
            }
            Process.Start(new ProcessStartInfo(self) { UseShellExecute = true, WorkingDirectory = folder });
            return skipped;
        }

        /// <summary>Package folders the game runs without: a file there an antivirus will not let
        /// through (bbbridge, a new unsigned program that goes online, is the one seen) is left as it
        /// was and reported, instead of stopping the whole update.</summary>
        private static readonly string[] Optional = { "bbbridge" };

        private static bool IsOptional(string relative) =>
            Optional.Any(o => relative.Split(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar)
                .Contains(o, StringComparer.OrdinalIgnoreCase));

        /// <summary>An antivirus scanning a new file holds it for a moment: tried again for a few
        /// seconds before giving up.</summary>
        private static void Retry(Action action)
        {
            for (var attempt = 1; ; attempt++)
            {
                try { action(); return; }
                catch (Exception e) when (attempt < 6 && e is IOException or UnauthorizedAccessException)
                {
                    Thread.Sleep(500 * attempt);
                }
            }
        }

        private static Exception Blocked(string file, Exception e) => new InvalidOperationException(
            $"{e.Message}\n\nAn antivirus is most likely holding or removing {Path.GetFileName(file)}. Allow it (or the " +
            "Bloodborne PC folder) in the antivirus and update again.", e);

        private static void Extract(string zip, string to, List<string> skipped)
        {
            using var archive = ZipFile.OpenRead(zip);
            var root = Path.GetFullPath(to) + Path.DirectorySeparatorChar;
            foreach (var entry in archive.Entries)
            {
                var target = Path.GetFullPath(Path.Combine(to, entry.FullName));
                if (!target.StartsWith(root, StringComparison.OrdinalIgnoreCase)) continue; // outside the folder
                if (entry.FullName.EndsWith('/') || entry.FullName.EndsWith('\\'))
                {
                    Directory.CreateDirectory(target);
                    continue;
                }
                Directory.CreateDirectory(Path.GetDirectoryName(target)!);
                try { Retry(() => entry.ExtractToFile(target, overwrite: true)); }
                catch (Exception e) when (e is IOException or UnauthorizedAccessException)
                {
                    if (!IsOptional(entry.FullName)) throw Blocked(target, e);
                    skipped.Add(entry.FullName.Replace('\\', '/'));
                }
            }
        }

        /// <summary>Copies a new package over this one, leaving the player's own files alone.</summary>
        private static void CopyPackage(string from, string to, string self, List<string> skipped)
        {
            foreach (var file in Directory.GetFiles(from, "*", SearchOption.AllDirectories))
            {
                var relative = Path.GetRelativePath(from, file);
                var top = relative.Split(Path.DirectorySeparatorChar)[0];
                if (Kept.Contains(top, StringComparer.OrdinalIgnoreCase)) continue;

                var target = Path.Combine(to, relative);
                if (string.Equals(Path.GetFullPath(target), Path.GetFullPath(self), StringComparison.OrdinalIgnoreCase))
                {
                    ReplaceSelf(self, file);
                    continue;
                }
                Directory.CreateDirectory(Path.GetDirectoryName(target)!);
                try { Retry(() => File.Copy(file, target, overwrite: true)); }
                catch (Exception e) when (e is IOException or UnauthorizedAccessException)
                {
                    if (!IsOptional(relative)) throw Blocked(file, e);
                    skipped.Add(relative.Replace('\\', '/'));
                }
            }
        }

        private static void ReplaceSelf(string self, string replacement)
        {
            var aside = self + ".old";
            if (File.Exists(aside)) File.Delete(aside);
            File.Move(self, aside);
            try
            {
                File.Copy(replacement, self);
            }
            catch
            {
                File.Move(aside, self); // put the working one back
                throw;
            }
        }

        private static async Task DownloadAsync(string url, string path, long expected, IProgress<double>? progress)
        {
            using var http = Client(TimeSpan.FromMinutes(30));
            using var response = await http.GetAsync(url, HttpCompletionOption.ResponseHeadersRead);
            response.EnsureSuccessStatusCode();
            var total = response.Content.Headers.ContentLength ?? expected;
            await using var input = await response.Content.ReadAsStreamAsync();
            await using var output = File.Create(path);
            var buffer = new byte[81920];
            long done = 0;
            int read;
            while ((read = await input.ReadAsync(buffer)) > 0)
            {
                await output.WriteAsync(buffer.AsMemory(0, read));
                done += read;
                if (total > 0) progress?.Report((double)done / total);
            }
        }

        /// <summary>Removes the exe an update moved aside (it was still running then).</summary>
        public static void CleanUp()
        {
            try
            {
                if (Environment.ProcessPath is { } self && File.Exists(self + ".old"))
                    File.Delete(self + ".old");
            }
            catch
            {
                // Still in use for a moment; removed next time.
            }
        }

        private static HttpClient Client(TimeSpan timeout)
        {
            var http = new HttpClient { Timeout = timeout };
            http.DefaultRequestHeaders.UserAgent.ParseAdd($"BloodborneLauncher/{Current}");
            http.DefaultRequestHeaders.Accept.ParseAdd("application/vnd.github+json");
            return http;
        }
    }
}
