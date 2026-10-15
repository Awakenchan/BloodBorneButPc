using System.IO;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Threading;
using BloodborneLauncher.Core;
using Wpf.Ui.Controls;
using Button = Wpf.Ui.Controls.Button;
using TextBlock = System.Windows.Controls.TextBlock;

namespace BloodborneLauncher.Views
{
    /// <summary>Home: the game folder and its check, saves, language, a summary and the Play button.</summary>
    public sealed class PlayPage : LauncherPage
    {
        private readonly InfoBar _problem;
        private readonly SettingRow _engineCard;
        private readonly Button _profileButton;
        private readonly TextBlock _profileHint, _backupHint;
        private readonly Button _restoreButton;
        private readonly TextBlock _gameHint, _dlcHint, _userHint, _statGame, _statUpscaler, _statResolution, _statFps, _statVia;
        private readonly SymbolIcon _gameIcon;
        private readonly Button _play, _resetUser, _resetDlc;
        private readonly TextBlock _playStatus;
        private readonly DispatcherTimer _clock;

        public PlayPage() : base("Play", "Check the game folder, choose where saves go, and start Bloodborne.")
        {
            // ==================== Summary + Play ====================
            // A glass pane in the Modern layout, a card in Classic.
            var summary = new Border
            {
                Margin = new Thickness(0, 14, 0, 0),
                Padding = new Thickness(18, 16, 18, 16),
                CornerRadius = new CornerRadius(SettingRow.Classic ? 8 : 16),
                BorderThickness = new Thickness(1),
            };
            summary.SetResourceReference(Border.BackgroundProperty, "GlassPaneBrush");
            summary.SetResourceReference(Border.BorderBrushProperty, "GlassPaneStrokeBrush");
            var stack = new StackPanel();
            var stats = new UniformGrid { Rows = 1 };
            stats.Children.Add(Stat("Game", out _statGame));
            stats.Children.Add(Stat("Upscaler", out _statUpscaler));
            stats.Children.Add(Stat("Output", out _statResolution));
            stats.Children.Add(Stat("Frame rate", out _statFps));
            stats.Children.Add(Stat("Trophies", out _statVia));
            stack.Children.Add(stats);

            var actions = new Grid { Margin = new Thickness(0, 16, 0, 0) };
            actions.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            actions.ColumnDefinitions.Add(new ColumnDefinition());
            _play = new Button
            {
                Appearance = ControlAppearance.Primary,
                Content = "Play",
                Icon = Icon(SymbolRegular.Play24),
                Padding = new Thickness(26, 10, 26, 10),
                FontWeight = FontWeights.SemiBold,
            };
            _play.Click += async (_, _) => await MainWindow.Instance!.PlayOrStopAsync();
            actions.Children.Add(_play);
            _playStatus = Text("", "Muted");
            _playStatus.Margin = new Thickness(16, 0, 0, 0);
            _playStatus.VerticalAlignment = VerticalAlignment.Center;
            Grid.SetColumn(_playStatus, 1);
            actions.Children.Add(_playStatus);
            stack.Children.Add(actions);
            summary.Child = stack;
            Body.Children.Add(summary);

            _problem = new InfoBar { IsClosable = false, IsOpen = false, Margin = new Thickness(0, 12, 0, 0) };
            Body.Children.Add(_problem);

            // A package from before this launcher's engine (it updated itself, the engine did not).
            _engineCard = Card(SymbolRegular.ArrowDownload24, "Engine update available",
                "This package has an older engine than this launcher needs (it unlocks 120 FPS and uncapped). " +
                "Updating downloads the latest package and keeps your settings, saves and mods.",
                SmallButton("Update engine", SymbolRegular.ArrowDownload24, async (_, _) => await MainWindow.Instance!.UpdateEngineAsync(),
                    ControlAppearance.Primary), out _);
            _engineCard.Margin = new Thickness(0, 12, 0, 0);
            Body.Children.Add(_engineCard);

            // The community: help, news and people to play with. Hidden for good with the X
            // (Settings keeps the link).
            if (!S.Bool("discord_hidden"))
            {
                SettingRow? discord = null;
                discord = Card(SymbolRegular.PeopleCommunity24, "Join the Discord",
                    "Help, news, co-op partners and beta feedback", Row(
                        SmallButton("Join", SymbolRegular.Open24, (_, _) => OpenDiscord(), ControlAppearance.Primary),
                        IconButton(SymbolRegular.Dismiss24, "Hide", (_, _) =>
                        {
                            S.Set("discord_hidden", true);
                            Ws.SaveSoon();
                            Body.Children.Remove(discord);
                        })), out _);
                discord.Margin = new Thickness(0, 12, 0, 0);
                Body.Children.Add(discord);
            }

            // ==================== Game ====================
            Group("Game");
            var gameCard = Card(SymbolRegular.Folder24, "Game folder", "", Row(
                SmallButton("Browse", SymbolRegular.FolderOpen24, (_, _) => ChooseGame()),
                IconButton(SymbolRegular.Open24, "Open in Explorer", (_, _) => Paths.OpenFolder(GameEnvironment.GameDir(S)))), out _gameHint);
            _gameIcon = gameCard.Icon;
            gameCard.ShowIcon = true;
            Add(gameCard);

            // The Old Hunters on base-game dumps: the folder of the dumped add-ons (BB_DLC_DIR).
            _resetDlc = IconButton(SymbolRegular.Dismiss24, "No DLC folder", (_, _) =>
            {
                S.Set("dlc_dir", "");
                Ws.SaveSoon();
                Refresh();
            });
            ToggleRow(SymbolRegular.PuzzlePiece24, "The Old Hunters (DLC)",
                "Off: the game starts without its DLC. The Game of the Year edition (CUSA03173) always has it",
                () => S.Bool("dlc_enabled"), v =>
                {
                    S.Set("dlc_enabled", v);
                    Ws.SaveSoon();
                    Refresh();
                });
            Add(Card(SymbolRegular.FolderOpen24, "DLC folder", "", Row(
                SmallButton("Browse", SymbolRegular.FolderOpen24, (_, _) => ChooseDlc()),
                IconButton(SymbolRegular.Open24, "Open in Explorer", (_, _) => Paths.OpenFolder(GameEnvironment.DlcDir(S))),
                _resetDlc), out _dlcHint));

            // Steam: Bloodborne.exe as a non-Steam game (Core/SteamShortcut.cs).
            Add(Card(SymbolRegular.Games24, "Add to Steam",
                "Bloodborne as a non-Steam game in your Steam library: Steam Input, the overlay and Big Picture work with it",
                SmallButton("Add to Steam", SymbolRegular.Add24, (_, _) => AddToSteam()), out _));

            // Player profiles: each its own saves and trophies (a button with a menu, not a drop-down,
            // so a click can't pick an entry by itself).
            _profileButton = SmallButton("Main", SymbolRegular.Person24, (_, _) => ShowProfileMenu());
            Add(Card(SymbolRegular.People24, "Player profile",
                "Each profile has its own save files and trophies, so several people can play their own runs on this PC", Row(
                _profileButton,
                SmallButton("New…", SymbolRegular.PersonAdd24, (_, _) => NewProfile()),
                IconButton(SymbolRegular.Rename24, "Rename this profile", (_, _) => RenameProfile()),
                IconButton(SymbolRegular.PersonDelete24, "Remove this profile from the list (its saves stay on disk)", (_, _) => RemoveProfile())),
                out _profileHint));

            _resetUser = IconButton(SymbolRegular.ArrowUndo24, "Back to the default folder", (_, _) =>
            {
                S.Set("user_dir", "");
                Ws.SaveSoon();
                Refresh();
            });
            Add(Card(SymbolRegular.Save24, "Saves folder", "", Row(
                SmallButton("Browse", SymbolRegular.FolderOpen24, (_, _) => ChooseUser()),
                IconButton(SymbolRegular.Open24, "Open in Explorer", (_, _) => Paths.OpenFolder(GameEnvironment.SavesRoot(S))),
                _resetUser), out _userHint));

            // Save backups: made before each start when the saves changed (Workspace.StartGame).
            _restoreButton = SmallButton("Restore…", SymbolRegular.History24, (_, _) => ShowBackupMenu());
            var backupToggle = new ToggleSwitch { IsChecked = S.Bool("save_backups"), VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(8, 0, 0, 0) };
            backupToggle.Click += (_, _) =>
            {
                S.Set("save_backups", backupToggle.IsChecked == true);
                Ws.SaveSoon();
                RefreshBackups();
            };
            Add(Card(SymbolRegular.ArrowSync24, "Save backups",
                $"Before every start, when this profile's saves changed. The newest {SaveBackups.Keep} are kept", Row(
                _restoreButton,
                IconButton(SymbolRegular.Open24, "Open the backups folder", (_, _) =>
                {
                    Directory.CreateDirectory(SaveBackups.Folder(S));
                    Paths.OpenFolder(SaveBackups.Folder(S));
                }),
                backupToggle), out _backupHint));
            Add(Card(SymbolRegular.ArrowImport24, "Import saves",
                "From shadPS4, another Bloodborne port, or a PS4 save exported decrypted (Apollo Save Tool, Save Wizard). This profile's saves are backed up first",
                SmallButton("Import…", SymbolRegular.FolderArrowRight24, (_, _) => ImportSaves()), out _));

            ComboRow(SymbolRegular.LocalLanguage24, "In-game menu language", "The port's settings menu (Insert or L3 + R3)",
                Choices.MenuLanguages, () => Ini.Get("menu_language", "en"), v => Ini.Set("menu_language", v));

            _clock = new DispatcherTimer { Interval = TimeSpan.FromSeconds(1) };
            _clock.Tick += (_, _) => UpdateRunning();

            Ws.PortChanged += Refresh;
            Ws.GameStateChanged += () => { RefreshProfile(); RefreshBackups(); };
            Ws.Saved += RefreshSummary;
            Ws.TrophiesChanged += RefreshSummary;
        }

        private static StackPanel Stat(string label, out TextBlock value)
        {
            var panel = new StackPanel { Margin = new Thickness(0, 0, 10, 0) };
            panel.Children.Add(Text(label, "StatLabel"));
            value = Text("—", "StatValue");
            panel.Children.Add(value);
            return panel;
        }

        public override void OnShown() => Refresh();

        private void Refresh()
        {
            var gameDir = GameEnvironment.GameDir(S);
            var status = GameCheck.Inspect(gameDir);
            SetHint(_gameHint, string.IsNullOrEmpty(gameDir) ? "Not chosen" : gameDir);
            _gameIcon.Symbol = status.Health == Health.Good ? SymbolRegular.CheckmarkCircle24 : SymbolRegular.Warning24;
            _gameIcon.SetResourceReference(ForegroundProperty, status.Health switch
            {
                Health.Good => "GoodTextBrush",
                Health.Warning => "WarningTextBrush",
                _ => "DangerTextBrush",
            });
            _gameIcon.ToolTip = status.Summary;

            _problem.IsOpen = status.Health != Health.Good;
            _problem.Severity = status.Health == Health.Bad ? InfoBarSeverity.Error : InfoBarSeverity.Warning;
            _problem.Title = status.Health == Health.Bad ? "The game folder needs attention" : "Check the game";
            _problem.Message = status.Summary;

            var dlc = GameEnvironment.DlcDir(S);
            var chosen = S.Str("dlc_dir").Length > 0;
            SetHint(_dlcHint, dlc.Length > 0
                ? chosen ? dlc : $"Found next to the game: {dlc}"
                : "Only with a base-game dump: the folder of your dumped add-ons, one folder each (The Old Hunters: SPEXPANSIONDLC03). " +
                  "Found on its own when it is beside the game folder. The Game of the Year edition (CUSA03173) has it already");
            _resetDlc.IsEnabled = chosen;

            var custom = S.Str("user_dir").Length > 0;
            var root = GameEnvironment.SavesRoot(S);
            SetHint(_userHint, (custom ? root : $"Default: {root}") + "  ·  every profile's saves live here");
            _resetUser.IsEnabled = custom;
            RefreshProfile();
            RefreshBackups();

            _engineCard.Visibility = Updater.EngineOutdated ? Visibility.Visible : Visibility.Collapsed;
            _statGame.Text = status.Health == Health.Bad ? "Not found" : $"{status.TitleId} {status.Version}".Trim();
            RefreshSummary();
            UpdateRunning();
        }

        private void RefreshSummary()
        {
            _statUpscaler.Text = Label(Choices.Upscalers, Ini.Get("upscaler"));
            var (outW, outH) = Choices.OutputSize(Ini.Get("output_res", "1920x1080"));
            _statResolution.Text = $"{outW} × {outH}";
            _statFps.Text = Label(Choices.FpsModes, GameEnvironment.Fps(S));
            _statVia.Text = $"{Ws.Trophies.Count} / {TrophyTracker.All.Length} · {Ws.Trophies.Completion * 100:0}%";
        }

        private static string Label(IReadOnlyList<Choice> choices, string value) =>
            choices.FirstOrDefault(c => c.Value == value)?.Label.Split(" (")[0] ?? value;

        public void UpdateRunning()
        {
            var game = Ws.Game;
            _play.Content = game.Running ? "Stop" : "Play";
            _play.Icon = Icon(game.Running ? SymbolRegular.Stop24 : SymbolRegular.Play24);
            _play.Appearance = game.Running ? ControlAppearance.Danger : ControlAppearance.Primary;

            if (game.Running)
            {
                var up = DateTime.Now - game.StartedAt;
                _playStatus.Text = $"Running for {(int)up.TotalMinutes}:{up.Seconds:00}  ·  output on the Game output page";
                _clock.Start();
            }
            else
            {
                _clock.Stop();
                _playStatus.Text = GameCheck.Inspect(GameEnvironment.GameDir(S)).Health == Health.Bad
                    ? "Choose the game folder first"
                    : Ws.Session is { Length.TotalSeconds: > 10 } last
                        ? "Last session: " + last.Summary
                        : "Settings are saved as you change them";
            }
        }

        private void OpenDiscord()
        {
            try { System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(LauncherSettings.Discord) { UseShellExecute = true }); }
            catch (Exception e) { Ws.Log("Could not open the Discord link: " + e.Message, LogLevel.Warn); }
        }

        private void ChooseGame()
        {
            var path = PickFolder("Game folder (the one with eboot.bin)", GameEnvironment.GameDir(S));
            if (path == null) return;
            S.Set("game_dir", path);
            if (!Paths.IsPortDir(Paths.PortDir))
                Ws.ResolvePort(); // the port may sit beside the new game folder
            Ws.SaveSoon();
            Ws.Log("Game folder: " + path, LogLevel.Step);
            Refresh();
        }

        private void AddToSteam()
        {
            var exe = Path.Combine(Paths.PortDir, "Bloodborne.exe");
            if (!File.Exists(exe))
            {
                Prompt.Tell("Add to Steam", "Bloodborne.exe is missing from the Bloodborne PC folder: reinstall the package.");
                return;
            }
            if (SteamShortcut.SteamDir() is null)
            {
                Prompt.Tell("Add to Steam", "Steam is not installed for this Windows user.");
                return;
            }
            while (SteamShortcut.SteamRunning())
            {
                if (Prompt.Choose("Close Steam first",
                        "Steam rewrites its list of non-Steam games when it closes, so it has to be closed while Bloodborne is added. " +
                        "Exit Steam (Steam › Exit, or right-click its tray icon › Exit), then press Try again.",
                        "Cancel", "Try again") <= 0)
                    return;
            }
            try
            {
                SteamShortcut.Add(exe);
                Ws.Log("Added Bloodborne to Steam as a non-Steam game", LogLevel.Good);
                Prompt.Tell("Added to Steam",
                    "Bloodborne is in your Steam library after Steam starts again. It starts the game with this launcher's settings; " +
                    "Steam shows it as running until the game ends.");
            }
            catch (Exception e)
            {
                Prompt.Tell("Add to Steam", e.Message);
            }
        }

        private void ChooseDlc()
        {
            var path = PickFolder("DLC folder (your dumped add-ons, like SPEXPANSIONDLC03)", GameEnvironment.DlcDir(S));
            if (path == null) return;
            S.Set("dlc_dir", path);
            Ws.SaveSoon();
            Ws.Log("DLC folder: " + path, LogLevel.Step);
            Refresh();
        }

        // ---------------------------------------------------------------- player profiles

        private void RefreshProfile()
        {
            var profile = PlayerProfiles.Current(S);
            _profileButton.Content = profile;
            var dir = GameEnvironment.UserDir(S);
            var saves = Path.Combine(dir, "savedata");
            var found = Directory.Exists(saves) && Directory.EnumerateFileSystemEntries(saves).Any();
            var played = SessionStats.Playtime(S, profile);
            SetHint(_profileHint, dir + (found ? "  ·  saves found" : "  ·  no saves yet; the game creates them here") +
                                  $"  ·  {Ws.Trophies.Count} / {TrophyTracker.All.Length} trophies" +
                                  (played.TotalMinutes >= 1 ? $"  ·  {SessionStats.Duration(played)} played" : ""));
        }

        // ---------------------------------------------------------------- save backups

        private void RefreshBackups()
        {
            List<SaveBackups.Backup> list;
            try { list = SaveBackups.List(S); }
            catch { list = new(); }
            _restoreButton.IsEnabled = list.Count > 0;
            SetHint(_backupHint, !S.Bool("save_backups") ? "Off: no backups are made"
                : list.Count == 0 ? "None yet: the first is made when you next press Play"
                : $"{list.Count} for {PlayerProfiles.Current(S)}  ·  newest {list[0].When:d MMM, HH:mm}");
        }

        private void ShowBackupMenu()
        {
            var menu = new ContextMenu { PlacementTarget = _restoreButton, Placement = PlacementMode.Bottom };
            foreach (var backup in SaveBackups.List(S))
            {
                var item = new System.Windows.Controls.MenuItem
                {
                    Header = $"{backup.When:d MMM yyyy, HH:mm}  ·  {backup.Bytes / 1048576.0:0.0} MB" +
                             (backup.MadeBeforeRestore ? "  ·  before a restore" : ""),
                };
                var chosen = backup;
                item.Click += (_, _) => RestoreBackup(chosen);
                menu.Items.Add(item);
            }
            menu.IsOpen = true;
        }

        private void ImportSaves()
        {
            if (Ws.Game.Running)
            {
                Prompt.Tell("Import saves", "Close the game first: it has the saves open.");
                return;
            }
            string? from = null;
            if (SaveImport.Known(S).FirstOrDefault() is { } found)
            {
                var choice = Prompt.Choose("Import saves",
                    $"Bloodborne saves found ({found.From}), last saved {found.Saved:d MMM yyyy, HH:mm}:\n{found.Dir}",
                    "Cancel", "Choose others…", "Import these");
                if (choice <= 0) return;
                if (choice == 2) from = found.Dir;
            }
            if (from == null)
            {
                var how = Prompt.Choose("Import saves",
                    "Choose the folder with your saves (another port's whole folder is fine), or a save file: " +
                    "a .zip of the save, or one of its files (userdata0000).",
                    "Cancel", "A folder…", "A file or .zip…");
                if (how <= 0) return;
                string? folder;
                if (how == 1)
                {
                    folder = PickFolder("The folder with your saves (another port's whole folder is fine)", "");
                }
                else
                {
                    var dialog = new Microsoft.Win32.OpenFileDialog
                    {
                        Title = "A save file: a .zip of the save, or one of its files (userdata0000)",
                        Filter = "Bloodborne saves (*.zip;userdata*)|*.zip;userdata*|All files|*.*",
                    };
                    try { folder = dialog.ShowDialog() == true ? SaveImport.FolderOfFile(dialog.FileName) : null; }
                    catch (Exception e)
                    {
                        Prompt.Tell("Import saves", "Could not open that file: " + e.Message);
                        return;
                    }
                }
                if (folder == null) return;
                if (SaveImport.Find(folder, "chosen folder").FirstOrDefault() is not { } inside)
                {
                    Prompt.Tell("Import saves", SaveImport.HasEncryptedPs4Save(folder)
                        ? "That is a PS4 save as the console copies it to USB: it is encrypted with that PS4's own keys, " +
                          "which only the console has, so no PC program can open it.\n\n" +
                          "Export it decrypted instead: on a jailbroken PS4 with Apollo Save Tool (Copy save game → decrypted, to USB), " +
                          "or with Save Wizard for PS4. Then import that folder here."
                        : "No Bloodborne saves in that folder or the folders under it (a SPRJ0005 folder with userdata files).");
                    return;
                }
                from = inside.Dir;
            }
            var target = SaveImport.Target(S);
            if (string.Equals(Path.GetFullPath(from), Path.GetFullPath(target), StringComparison.OrdinalIgnoreCase))
            {
                Prompt.Tell("Import saves", "Those are already this profile's saves.");
                return;
            }
            var replacing = SaveImport.HasSaves(target)
                ? "\n\nThis profile's current saves are backed up first: Save backups › Restore… brings them back."
                : "";
            if (!Prompt.Confirm("Import saves", $"Copy the saves from\n{from}\ninto {PlayerProfiles.Current(S)}'s saves?{replacing}", "Import"))
                return;
            try
            {
                SaveImport.Import(S, from);
                Ws.Log($"Saves imported from {from}.", LogLevel.Good);
                Prompt.Tell("Saves imported", "Start the game and choose Continue or Load Game.");
            }
            catch (Exception e)
            {
                Ws.Log("Could not import the saves: " + e.Message, LogLevel.Bad);
                Prompt.Tell("Import saves", "Could not import: " + e.Message);
            }
            Refresh();
        }

        private void RestoreBackup(SaveBackups.Backup backup)
        {
            if (Ws.Game.Running)
            {
                Prompt.Tell("Restore saves", "Close the game first: it has the saves open.");
                return;
            }
            if (!Prompt.Confirm("Restore saves",
                    $"{PlayerProfiles.Current(S)}'s saves go back to {backup.When:d MMM yyyy, HH:mm}. " +
                    "What is there now is backed up first, so you can undo this from the same list.", "Restore"))
                return;
            try
            {
                SaveBackups.Restore(S, backup);
                Ws.Log($"Saves restored from {backup.When:d MMM yyyy, HH:mm}.", LogLevel.Good);
            }
            catch (Exception e)
            {
                Ws.Log("Could not restore the saves: " + e.Message, LogLevel.Bad);
                Prompt.Tell("Restore saves", "Could not restore: " + e.Message);
            }
            Refresh();
        }

        private void ShowProfileMenu()
        {
            var menu = new ContextMenu { PlacementTarget = _profileButton, Placement = PlacementMode.Bottom };
            var current = PlayerProfiles.Current(S);
            foreach (var profile in PlayerProfiles.All(S))
            {
                var name = profile;
                var item = new System.Windows.Controls.MenuItem { Header = name, IsCheckable = true, IsChecked = name.Equals(current, StringComparison.OrdinalIgnoreCase) };
                item.Click += (_, _) => SwitchProfile(name);
                menu.Items.Add(item);
            }
            menu.IsOpen = true;
        }

        private bool GameBlocksProfiles()
        {
            if (!Ws.Game.Running) return false;
            Prompt.Tell("Player profile", "Close the game before changing profiles: it is using this profile's saves.");
            return true;
        }

        private void SwitchProfile(string name)
        {
            if (name.Equals(PlayerProfiles.Current(S), StringComparison.OrdinalIgnoreCase) || GameBlocksProfiles()) return;
            S.Set("player_profile", PlayerProfiles.IsMain(name) ? "" : name);
            Directory.CreateDirectory(GameEnvironment.UserDir(S));
            Ws.Trophies.SetUserDir(GameEnvironment.UserDir(S));
            Ws.SaveSoon();
            Ws.Log($"Player profile: {name}.", LogLevel.Step);
            Refresh();
        }

        private void NewProfile()
        {
            if (GameBlocksProfiles()) return;
            var values = Prompt.Ask("New player profile", "Its own save files and trophies. The game starts a new character the first time.",
                new Prompt.Field("Name", "", "e.g. Alex"));
            if (values == null) return;
            var name = PlayerProfiles.Validate(S, values[0], out var error);
            if (name == null) { Prompt.Tell("New player profile", error); return; }
            PlayerProfiles.Add(S, name);
            SwitchProfile(name);
        }

        private void RenameProfile()
        {
            var current = PlayerProfiles.Current(S);
            if (PlayerProfiles.IsMain(current)) { Prompt.Tell("Rename profile", "Main is the original saves folder and keeps its name."); return; }
            if (GameBlocksProfiles()) return;
            var values = Prompt.Ask("Rename profile", null, new Prompt.Field("Name", current));
            if (values == null) return;
            var name = PlayerProfiles.Validate(S, values[0], out var error, renaming: current);
            if (name == null) { Prompt.Tell("Rename profile", error); return; }
            try
            {
                PlayerProfiles.Rename(S, current, name);
            }
            catch (Exception e)
            {
                Prompt.Tell("Rename profile", "Could not rename its folder: " + e.Message);
                return;
            }
            Ws.Trophies.SetUserDir(GameEnvironment.UserDir(S));
            Ws.SaveSoon();
            Refresh();
        }

        private void RemoveProfile()
        {
            var current = PlayerProfiles.Current(S);
            if (PlayerProfiles.IsMain(current)) { Prompt.Tell("Remove profile", "Main is the original saves folder and can't be removed."); return; }
            if (GameBlocksProfiles()) return;
            if (!Prompt.Confirm("Remove profile",
                    $"{current} is taken off the list and the launcher switches to Main. Its saves and trophies are not deleted: " +
                    $"they stay in {GameEnvironment.UserDir(S)}, and adding a profile with the same name brings them back.", "Remove"))
                return;
            PlayerProfiles.Remove(S, current);
            S.Set("player_profile", "");
            Ws.Trophies.SetUserDir(GameEnvironment.UserDir(S));
            Ws.SaveSoon();
            Refresh();
        }

        private void ChooseUser()
        {
            if (GameBlocksProfiles()) return;
            var path = PickFolder("Saves folder", GameEnvironment.SavesRoot(S));
            if (path == null) return;
            S.Set("user_dir", path);
            Ws.SaveSoon();
            Refresh();
        }
    }
}
