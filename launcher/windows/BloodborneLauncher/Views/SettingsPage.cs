using System.Diagnostics;
using System.IO;
using System.Windows;
using System.Windows.Controls;
using BloodborneLauncher.Build;
using BloodborneLauncher.Core;
using Wpf.Ui.Controls;
using MessageBoxButton = System.Windows.MessageBoxButton;
using MessageBoxResult = System.Windows.MessageBoxResult;
using TextBlock = System.Windows.Controls.TextBlock;

namespace BloodborneLauncher.Views
{
    /// <summary>The launcher itself: where the port is, how the game is started, and rebuilding.</summary>
    public sealed class SettingsPage : LauncherPage
    {
        private readonly TextBlock _portHint, _buildHint;
        private readonly StackPanel _customRows = new();
        private readonly TextBlock _updateHint;
        private readonly Wpf.Ui.Controls.Button _updateNow;
        private readonly ComboBox _versions = new() { Width = 300, VerticalAlignment = VerticalAlignment.Center };
        private readonly Wpf.Ui.Controls.Button _installVersion;
        private readonly TextBlock _versionHint;
        private IReadOnlyList<Updater.Release>? _releases;
        private bool _versionsLoading;

        public SettingsPage() : base("Settings", "The Bloodborne PC engine, how the game is started, and keeping this launcher up to date.")
        {
            Group("Appearance");
            FrameworkElement layout = null!;
            var reverting = false;
            layout = ComboRow(SymbolRegular.PanelLeft24, "Layout", "Modern: tabs and compact rows. Classic: the sidebar and one card per setting",
                new[] { new Choice("Modern", "modern"), new Choice("Classic", "classic") },
                () => S.Str("ui_layout") == "classic" ? "classic" : "modern", v => S.Set("ui_layout", v), changed: () =>
                {
                    if (reverting || RestartForLayout()) return;
                    // Cancelled: put back the layout that is showing, so the next start doesn't switch either.
                    reverting = true;
                    var shown = SettingRow.Classic ? "classic" : "modern";
                    S.Set("ui_layout", shown);
                    Ws.SaveNow();
                    var index = shown == "classic" ? 1 : 0;
                    if (layout is Segmented segmented) segmented.Select(index, notify: false);
                    else if (layout is ComboBox box) box.SelectedIndex = index;
                    reverting = false;
                });

            Group("Bloodborne PC");
            Add(Card(SymbolRegular.FolderLink24, "Engine folder", "", Row(
                SmallButton("Browse", SymbolRegular.FolderOpen24, (_, _) => ChoosePort()),
                IconButton(SymbolRegular.Open24, "Open in Explorer", (_, _) => Paths.OpenFolder(Paths.PortDir)),
                IconButton(SymbolRegular.ArrowUndo24, "Detect automatically", (_, _) =>
                {
                    S.Set("port_dir", "");
                    Ws.ResolvePort();
                    Ws.SaveSoon();
                    Refresh();
                })), out _portHint));

            Group("Launch");
            SettingCombo(SymbolRegular.Rocket24, "Start the game with", null, Choices.LaunchMethods, "launch_method", changed: () => { ShowMethod(); Ws.SaveSoon(); });

            TextRow(SymbolRegular.Code24, "Command", "A program to run instead of Bloodborne.exe --run, with the settings as environment variables",
                @"C:\path\to\game.exe", () => S.Str("custom_command"), v => S.Set("custom_command", v), _customRows, 380);
            TextRow(SymbolRegular.TextDescription24, "Arguments", null, "Optional",
                () => S.Str("custom_args"), v => S.Set("custom_args", v), _customRows, 380);
            Body.Children.Add(_customRows);

            Group("Performance");
            SettingCombo(SymbolRegular.Flow20, "Two-stage GPU pipeline", "20–30% faster; turn off if the game is unstable",
                Choices.DrawPipe, "draw_pipe");
            SettingCombo(SymbolRegular.ArrowSwap24, "CPU reads of GPU data", "Relaxed is the default", Choices.Readbacks, "readbacks");
            SettingToggle(SymbolRegular.Speaker224, "Sound threads first",
                "For CPUs with many slower cores (older Xeons and similar): the game's sound runs before its other threads, against late or stuttering sound. Off by default",
                "audio_priority");
            SettingCombo(SymbolRegular.Speaker224, "Sound output",
                "If the sound jitters or crackles, try DirectSound. The larger buffer is steadier but the sound comes a little later",
                Choices.SoundOutput, "sound_output");

            Group("Diagnostics");
            SettingToggle(SymbolRegular.TextBulletListSquare24, "Frame statistics in the log", "BB_FRAME_STATS", "frame_stats");
            SettingToggle(SymbolRegular.DeveloperBoard24, "FSR 4 profile in the log", "BB_FSR4_PROFILE", "fsr_profile");
            SettingToggle(SymbolRegular.Speaker224, "Audio statistics in the log", "BB_AUDIO_STATS", "audio_stats");
            SettingToggle(SymbolRegular.HourglassHalf24, "Loading profile in the log",
                "BB_SAMPLE_THREADS: where the game's main, render and file threads spend their time, every 5 seconds. For slow loading; costs a little speed", "load_profile");
            SettingToggle(SymbolRegular.Timer24, "Render stage timings in the log", "BB_SECTIONS: where each frame's CPU time goes (with frame statistics). It costs FPS, so it applies to the next game only", "section_stats");
            TextRow(SymbolRegular.Code24, "Extra variables", "NAME=value pairs separated by spaces, passed to the game", "BB_EXAMPLE=1",
                () => S.Str("extra_env"), v => S.Set("extra_env", v.Trim()), width: 300);

            Group("Files");
            Add(Card(SymbolRegular.DocumentSettings20, "Launcher settings", Paths.ConfigFile,
                IconButton(SymbolRegular.Open24, "Open in Explorer", (_, _) => Paths.OpenFolder(Paths.ConfigDir)), out _));
            Add(Card(SymbolRegular.DocumentSettings20, "Port settings (bbport.ini)", Paths.IniPath,
                IconButton(SymbolRegular.Open24, "Open in Explorer", (_, _) => Paths.OpenFolder(Path.GetDirectoryName(Paths.IniPath)!)), out _));
            Add(Card(SymbolRegular.Broom24, "Shader cache",
                "The game's compiled shaders, kept between starts (a new engine version clears them itself). Clear them if " +
                "something draws wrong in every save, such as missing skin or hair; the next start compiles them again and stutters for a while",
                SmallButton("Clear", SymbolRegular.Broom24, (_, _) => ClearShaderCache()), out _));

            Group("Share settings");
            Add(Card(SymbolRegular.Share24, "Share your settings",
                "Graphics, effects, camera, controls, cheats and engine options, as a code to paste in Discord or a file. " +
                "Not included: display and HDR, the online account, folders and languages",
                Row(SmallButton("Copy code", SymbolRegular.Copy24, (_, _) => CopySettingsCode()),
                    SmallButton("Save as file", SymbolRegular.Save24, (_, _) => SaveSettingsFile())), out _));
            Add(Card(SymbolRegular.ArrowImport24, "Import settings", "Someone's code or file. You see what changes before anything is applied",
                Row(SmallButton("Paste code", SymbolRegular.ClipboardPaste24, (_, _) => ImportSettingsCode()),
                    SmallButton("Open file", SymbolRegular.FolderOpen24, (_, _) => ImportSettingsFile())), out _));

            Group("Community");
            Add(Card(SymbolRegular.PeopleCommunity24, "Discord", "Help, news, co-op partners and beta feedback",
                SmallButton("Join", SymbolRegular.Open24, (_, _) =>
                    Process.Start(new ProcessStartInfo(LauncherSettings.Discord) { UseShellExecute = true })), out _));

            Group("Updates");
            _updateNow = SmallButton("Update now", SymbolRegular.ArrowDownload24, async (_, _) => await MainWindow.Instance!.InstallUpdateAsync(),
                ControlAppearance.Primary);
            Add(Card(SymbolRegular.ArrowDownload24, "Launcher updates", "", Row(
                SmallButton("Check now", SymbolRegular.ArrowClockwise24, async (_, _) => await Ws.CheckForUpdatesAsync(quiet: false)),
                _updateNow,
                IconButton(SymbolRegular.Open24, "Open the releases page", (_, _) =>
                    Process.Start(new ProcessStartInfo(Updater.ReleasesPage) { UseShellExecute = true }))), out _updateHint));
            ComboRow(SymbolRegular.Branch24, "Update channel",
                "Stable: releases that have been tested. Beta: every change as soon as it is done, before it is tested",
                new[] { new Choice("Stable", "stable"), new Choice("Beta", "beta") },
                () => Updater.BetaChannel(S) ? "beta" : "stable",
                v =>
                {
                    if (v == (Updater.BetaChannel(S) ? "beta" : "stable")) return;
                    S.Set("update_channel", v);
                    Ws.SaveSoon();
                    Ws.Log(v == "beta" ? "Update channel: beta." : "Update channel: stable.", LogLevel.Step);
                    _ = Ws.CheckForUpdatesAsync(quiet: false);
                });
            SettingToggle(SymbolRegular.Alert24, "Check for updates at start", $"Looks at github.com/{Updater.Repository}/releases", "check_updates");
            _installVersion = SmallButton("Install", SymbolRegular.ArrowDownload24, async (_, _) => await InstallChosenVersionAsync());
            _installVersion.IsEnabled = false;
            _versions.SelectionChanged += (_, _) =>
                _installVersion.IsEnabled = _versions.SelectedItem is ComboBoxItem { Tag: Updater.Release r } && !r.IsCurrent;
            Add(Card(SymbolRegular.History24, "Version",
                "Every published release, stable and beta, newest first. Install any of them, older ones too, to go back if a " +
                "version gives you trouble. Your settings, saves, mods and profiles stay",
                Row(_versions, _installVersion,
                    IconButton(SymbolRegular.ArrowClockwise24, "Reload the list", async (_, _) => await LoadVersionsAsync(force: true))),
                out _versionHint));
            SetHint(_versionHint, $"You have {Updater.CurrentLabel}");
            Ws.UpdateStateChanged += UpdateUpdates;
            UpdateUpdates();

            // Only a copy built from the source checkout can rebuild itself.
            var build = Card(SymbolRegular.ArrowSync24, "Build", "", Row(
                SmallButton("Rebuild", SymbolRegular.ArrowClockwise24, (_, _) => Rebuild(force: false)),
                SmallButton("Force rebuild", SymbolRegular.Wrench24, (_, _) => Rebuild(force: true))), out _buildHint);
            if (Paths.SourceRoot != null)
            {
                Group("Build from source");
                Add(build);
            }

            Ws.PortChanged += Refresh;
        }

        public override void OnShown()
        {
            Refresh();
            ShowMethod();
            _ = LoadVersionsAsync(force: false);
        }

        /// <summary>Fills the version selector from the releases on GitHub (once, or again on reload).</summary>
        private async Task LoadVersionsAsync(bool force)
        {
            if (_versionsLoading || (_releases != null && !force)) return;
            _versionsLoading = true;
            _versions.IsEnabled = false;
            _installVersion.IsEnabled = false;
            _versions.Items.Clear();
            _versions.Items.Add(new ComboBoxItem { Content = "Loading…" });
            _versions.SelectedIndex = 0;
            try
            {
                _releases = await Updater.AllAsync();
                _versions.Items.Clear();
                ComboBoxItem? current = null;
                foreach (var release in _releases)
                {
                    var label = Updater.Label(release.Version, release.Beta) +
                                (release.Beta > 0 ? "" : "  · stable") +
                                (release.IsCurrent ? "  (installed)" : "");
                    var item = new ComboBoxItem { Content = label, Tag = release };
                    _versions.Items.Add(item);
                    if (release.IsCurrent) current = item;
                }
                _versions.SelectedItem = current ?? (_versions.Items.Count > 0 ? _versions.Items[0] : null);
                _versions.IsEnabled = _versions.Items.Count > 0;
                SetHint(_versionHint, _releases.Count == 0
                    ? $"You have {Updater.CurrentLabel}; no release is published yet"
                    : $"You have {Updater.CurrentLabel}. {_releases.Count} versions to choose from");
            }
            catch (Exception e)
            {
                _releases = null;
                _versions.Items.Clear();
                SetHint(_versionHint, $"You have {Updater.CurrentLabel}. The list could not be loaded: {e.GetBaseException().Message}");
            }
            finally
            {
                _versionsLoading = false;
            }
        }

        private async Task InstallChosenVersionAsync()
        {
            if (_versions.SelectedItem is not ComboBoxItem { Tag: Updater.Release release }) return;
            await MainWindow.Instance!.InstallVersionAsync(release);
        }

        private void Refresh()
        {
            SetHint(_portHint, Paths.IsPortDir(Paths.PortDir)
                ? Paths.PortDir + (Paths.DetectPortDir() == Paths.PortDir ? "  ·  the package this launcher is in" : "") + "\nThe engine that runs your game: out\\bb-probe.exe, its scripts and Python. Your game files stay where they are"
                : "Not found. It is normally the folder this launcher is in (with bin\\bb-probe.exe and run.py)");
            SetHint(_buildHint, BuildStatus());
        }

        private void UpdateUpdates()
        {
            var update = Ws.Update;
            var latest = Ws.Latest;
            _updateNow.Visibility = update != null ? Visibility.Visible : Visibility.Collapsed;
            SetHint(_updateHint,
                update != null ? $"{update.Name} is available (you have {Updater.CurrentLabel})" +
                                 (update.IsNewerThan(Updater.Current, Updater.CurrentBeta) ? "" : ": back to stable")
                : Ws.CheckedAt == null ? $"You have {Updater.CurrentLabel}"
                : latest == null ? $"You have {Updater.CurrentLabel}; no release is published yet"
                : $"You have {Updater.CurrentLabel}, the latest ({latest.Tag})");
        }

        /// <summary>Restarts into the chosen layout; false if the player cancelled.</summary>
        private static bool RestartForLayout()
        {
            if (!Prompt.Confirm("Switch layout", "The launcher restarts to change its layout.", "Restart now")) return false;
            Restart();
            return true;
        }

        private static void Restart()
        {
            Ws.SaveNow();
            if (Environment.ProcessPath is { } self)
                Process.Start(new ProcessStartInfo(self) { UseShellExecute = true, WorkingDirectory = Path.GetDirectoryName(self)! });
            Application.Current.Shutdown();
        }

        /// <summary>Deletes the engine's shader and pipeline caches (user\cache beside the engine;
        /// scripts\run_windows.py also clears them when the engine changes).</summary>
        private static void ClearShaderCache()
        {
            if (Process.GetProcessesByName("bb-probe").Length > 0)
            {
                Prompt.Tell("Shader cache", "Close the game first: it is using the cache.");
                return;
            }
            var cache = Path.Combine(Paths.PortDir, "user", "cache");
            var failed = 0;
            if (Directory.Exists(cache))
            {
                foreach (var entry in new DirectoryInfo(cache).EnumerateFileSystemInfos())
                {
                    if (entry.Name == "engine.txt") continue;
                    try
                    {
                        if (entry is DirectoryInfo directory) directory.Delete(recursive: true);
                        else entry.Delete();
                    }
                    catch (Exception) { failed++; }
                }
            }
            Ws.Log(failed == 0 ? "Shader cache cleared: the next start compiles the shaders again."
                               : $"Shader cache: {failed} entries could not be removed ({cache}).",
                   failed == 0 ? LogLevel.Good : LogLevel.Warn);
        }

        // ---------------------------------------------------------------- shared settings

        private void CopySettingsCode()
        {
            Clipboard.SetText(SharedSettings.ToCode(SharedSettings.Take(Ini, S)));
            Ws.Log("Settings code copied. Paste it in Discord; others import it in Settings › Share settings.", LogLevel.Good);
            Prompt.Tell("Settings code copied", "Paste it in Discord or anywhere else. Others use Import settings › Paste code.");
        }

        private void SaveSettingsFile()
        {
            var dialog = new Microsoft.Win32.SaveFileDialog { Title = "Save settings", FileName = "Bloodborne PC settings.json", Filter = "Settings|*.json" };
            if (dialog.ShowDialog() != true) return;
            try
            {
                File.WriteAllText(dialog.FileName, SharedSettings.ToJson(SharedSettings.Take(Ini, S)));
                Ws.Log("Settings saved: " + dialog.FileName, LogLevel.Good);
            }
            catch (Exception e)
            {
                Ws.Log("Could not save the settings: " + e.Message, LogLevel.Bad);
            }
        }

        private void ImportSettingsCode()
        {
            var clipboard = Clipboard.ContainsText() ? Clipboard.GetText() : "";
            var values = Prompt.Ask("Import settings", $"Paste the code someone shared (it starts with {SharedSettings.CodePrefix}).",
                new Prompt.Field("Code", clipboard.Contains(SharedSettings.CodePrefix, StringComparison.OrdinalIgnoreCase) ? clipboard.Trim() : "",
                    SharedSettings.CodePrefix + "…", Multiline: true));
            if (values == null || values[0].Trim().Length == 0) return;
            OfferSettings(values[0]);
        }

        private void ImportSettingsFile()
        {
            var dialog = new Microsoft.Win32.OpenFileDialog { Title = "Import settings", Filter = "Settings|*.json;*.txt|All files|*.*" };
            if (dialog.ShowDialog() != true) return;
            try
            {
                if (new FileInfo(dialog.FileName).Length > 256 * 1024)
                    throw new FormatException("That file is far too large to be shared settings.");
                OfferSettings(File.ReadAllText(dialog.FileName));
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException or FormatException)
            {
                Prompt.Tell("Could not import", e.Message);
            }
        }

        /// <summary>Shows what imported settings change and, if accepted, applies them (the previous ones are
        /// kept in a file) and restarts the launcher so every page shows them.</summary>
        private void OfferSettings(string text)
        {
            if (Ws.Game.Running)
            {
                Prompt.Tell("Close the game first", "The game changes these settings too while it runs.");
                return;
            }
            SharedSettings.Snapshot shared;
            try
            {
                shared = SharedSettings.Parse(text);
            }
            catch (FormatException e)
            {
                Prompt.Tell("Could not import", e.Message);
                return;
            }

            var everything = new HashSet<SharedSettings.Part> { SharedSettings.Part.Settings, SharedSettings.Part.Controls, SharedSettings.Part.Cheats };
            var changes = SharedSettings.Changes(shared, Ini, S, everything);
            if (changes.Count == 0)
            {
                Prompt.Tell("Nothing to change", "These settings are the same as yours.");
                return;
            }
            var settings = changes.Count(c => c.Part == SharedSettings.Part.Settings);
            var controls = changes.Count(c => c.Part == SharedSettings.Part.Controls);
            var cheats = changes.Count(c => c.Part == SharedSettings.Part.Cheats);
            var counts = string.Join(", ", new[]
            {
                settings > 0 ? $"{settings} graphics and engine" : null,
                controls > 0 ? $"{controls} controls" : null,
                cheats > 0 ? $"{cheats} cheats" : null,
            }.Where(p => p != null));
            const int shown = 12;
            var list = string.Join("\n", changes.Take(shown).Select(SharedSettings.Describe)) +
                       (changes.Count > shown ? $"\n…and {changes.Count - shown} more" : "");
            var message = $"From {shared.From}: {changes.Count} changes ({counts}).\n\n{list}\n\n" +
                          "Your current settings are saved first. The launcher restarts to show the new ones.";

            HashSet<SharedSettings.Part> parts;
            if (settings > 0 && controls + cheats > 0)
            {
                var choice = Prompt.Choose("Import settings", message, "Cancel", "Keep my controls and cheats", "Import all");
                if (choice <= 0) return;
                parts = choice == 2 ? everything : new() { SharedSettings.Part.Settings };
            }
            else if (!Prompt.Confirm("Import settings", message, "Import"))
            {
                return;
            }
            else
            {
                parts = everything;
            }

            try
            {
                var backup = Path.Combine(Paths.ConfigDir, "settings-before-import.json");
                File.WriteAllText(backup, SharedSettings.ToJson(SharedSettings.Take(Ini, S)));
                SharedSettings.Apply(shared, Ini, S, parts);
                Ws.Log($"Imported settings from {shared.From}. Your previous ones: {backup} (Import settings › Open file).", LogLevel.Good);
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException)
            {
                Prompt.Tell("Could not import", e.Message);
                return;
            }
            Restart();
        }

        private void ShowMethod() =>
            _customRows.Visibility = GameRunner.Custom(S) ? Visibility.Visible : Visibility.Collapsed;

        private void ChoosePort()
        {
            var path = PickFolder("Engine folder (with run.py and bin\\bb-probe.exe)", Paths.PortDir);
            if (path == null) return;
            if (!Paths.IsPortDir(path))
            {
                System.Windows.MessageBox.Show(Application.Current.MainWindow!, "That folder has no run.py with Bloodborne.exe or bin\\bb-probe.exe.",
                    "Engine folder", MessageBoxButton.OK, MessageBoxImage.Warning);
                return;
            }
            S.Set("port_dir", path);
            Ws.ResolvePort();
            Ws.SaveSoon();
            Refresh();
        }

        // ---------------------------------------------------------------- rebuild

        private static string? BuilderPath()
        {
            var beside = Path.Combine(AppContext.BaseDirectory, SourceStamp.BuilderExe);
            if (File.Exists(beside)) return beside;
            if (Paths.SourceRoot is { } root)
            {
                var installed = Path.Combine(SourceStamp.AppDirOf(root), SourceStamp.BuilderExe);
                if (File.Exists(installed)) return installed;
            }
            return null;
        }

        private static string BuildStatus()
        {
            var version = typeof(SettingsPage).Assembly.GetName().Version;
            var root = Paths.SourceRoot;
            if (root == null)
                return $"Version {version?.ToString(3)}. The sources (launcher/windows) are not next to this copy, so it cannot rebuild itself.";

            var stamp = SourceStamp.Read(SourceStamp.AppDirOf(root));
            var current = SourceStamp.Compute(root, SourceStamp.LauncherProject);
            var state = stamp.Launcher == current
                ? "Up to date with the sources"
                : "The sources changed since this build: rebuild to pick the changes up";
            var built = stamp.Built == default ? "" : $" · built {stamp.Built.ToLocalTime():yyyy-MM-dd HH:mm}";
            return $"Version {version?.ToString(3)}{built}. {state}.";
        }

        private void Rebuild(bool force)
        {
            var builder = BuilderPath();
            if (builder == null)
            {
                System.Windows.MessageBox.Show(Application.Current.MainWindow!,
                    $"{SourceStamp.BuilderExe} was not found next to the launcher.", "Rebuild", MessageBoxButton.OK, MessageBoxImage.Warning);
                return;
            }
            if (Ws.Game.Running)
            {
                System.Windows.MessageBox.Show(Application.Current.MainWindow!, "Stop the game before rebuilding the launcher.",
                    "Rebuild", MessageBoxButton.OK, MessageBoxImage.Information);
                return;
            }

            Ws.SaveNow();
            var args = $"--wait-pid {Environment.ProcessId}" + (force ? " --force" : "");
            Process.Start(new ProcessStartInfo(builder, args) { UseShellExecute = true, WorkingDirectory = Path.GetDirectoryName(builder)! });
            Application.Current.Shutdown();
        }
    }
}
