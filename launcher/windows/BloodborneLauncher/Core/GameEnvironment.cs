using System.IO;

namespace BloodborneLauncher.Core
{
    /// <summary>
    /// The environment run_windows.py gets from the launcher. Most settings travel in bbport.ini
    /// (graphics, controls, online); these are the ones the engine takes from the environment.
    /// </summary>
    public static class GameEnvironment
    {
        public static string GameDir(LauncherSettings s) => Paths.Expand(s.Str("game_dir"));

        /// <summary>The saves folder the player chose (or the package's user folder): the Main profile.</summary>
        public static string SavesRoot(LauncherSettings s) =>
            s.Str("user_dir") is { Length: > 0 } dir ? Paths.Expand(dir) : Path.Combine(Paths.DataDir, "user");

        /// <summary>Where the game keeps saves and trophies: the current player profile's folder.</summary>
        public static string UserDir(LauncherSettings s) =>
            PlayerProfiles.Folder(s, PlayerProfiles.Current(s));

        /// <summary>The dumped add-ons' folder (The Old Hunters on base-game dumps): the one chosen on
        /// the Play page, else one found next to the game folder (<see cref="FoundDlc"/>), or "".</summary>
        public static string DlcDir(LauncherSettings s) =>
            s.Str("dlc_dir") is { Length: > 0 } dir ? Paths.Expand(dir) : FoundDlc(GameDir(s)) ?? "";

        private static (string Game, string? Found) _foundDlc = ("", null);

        /// <summary>An add-on dump near the game folder, as other launchers find it: a folder with an
        /// add-on's sce_sys\param.sfo (category "ac") inside the game folder, beside it or one level
        /// below a folder beside it (CUSA00207-DLC\SPEXPANSIONDLC03, addcont\CUSA00207\...). Returns
        /// the folder holding the add-ons (the engine reports each one in it), or null.</summary>
        public static string? FoundDlc(string game)
        {
            if (game.Length == 0 || !Directory.Exists(game)) return null;
            if (_foundDlc.Game == game) return _foundDlc.Found;
            string? found = null;
            try
            {
                var parent = Directory.GetParent(Path.GetFullPath(game).TrimEnd('\\', '/'))?.FullName;
                IEnumerable<string> Children(string dir)
                {
                    try { return Directory.EnumerateDirectories(dir); } catch (Exception) { return Array.Empty<string>(); }
                }
                var near = Children(game).ToList();
                if (parent != null)
                    foreach (var sibling in Children(parent).Take(200))
                    {
                        near.Add(sibling);
                        near.AddRange(Children(sibling).Take(50));
                        // shadPS4's addcont\<title id>\<add-on>
                        if (string.Equals(Path.GetFileName(sibling), "addcont", StringComparison.OrdinalIgnoreCase))
                            foreach (var title in Children(sibling)) near.AddRange(Children(title));
                    }
                var addon = near.FirstOrDefault(IsAddon);
                if (addon != null)
                {
                    var holder = Path.GetDirectoryName(addon)!;
                    found = parent != null && string.Equals(holder, parent, StringComparison.OrdinalIgnoreCase) ? addon : holder;
                }
            }
            catch (Exception) { }
            _foundDlc = (game, found);
            return found;
        }

        /// <summary>Whether <paramref name="dir"/> is a dumped add-on: its param.sfo's CATEGORY is "ac".</summary>
        private static bool IsAddon(string dir)
        {
            try
            {
                var path = Path.Combine(dir, "sce_sys", "param.sfo");
                if (!File.Exists(path) || new FileInfo(path).Length > 65536) return false;
                var sfo = File.ReadAllBytes(path);
                if (sfo.Length < 20 || sfo[0] != 0 || sfo[1] != (byte)'P' || sfo[2] != (byte)'S' || sfo[3] != (byte)'F') return false;
                int U32(int at) => BitConverter.ToInt32(sfo, at);
                int keys = U32(8), data = U32(12), count = U32(16);
                for (var i = 0; i < count && 20 + 16 * (i + 1) <= sfo.Length; i++)
                {
                    var e = 20 + 16 * i;
                    var k = keys + (sfo[e] | sfo[e + 1] << 8);
                    var v = data + U32(e + 12);
                    if (k + 9 > sfo.Length || v + 2 > sfo.Length) continue;
                    if (System.Text.Encoding.ASCII.GetString(sfo, k, 9) == "CATEGORY\0")
                        return sfo[v] == (byte)'a' && sfo[v + 1] == (byte)'c';
                }
            }
            catch (Exception) { }
            return false;
        }

        public static string ModsDir(LauncherSettings s) =>
            s.Str("mods_dir") is { Length: > 0 } dir ? Paths.Expand(dir) : Path.Combine(Paths.DataDir, "mods");

        public static string PatchesDir(LauncherSettings s) =>
            s.Str("patches_dir") is { Length: > 0 } dir ? Paths.Expand(dir) : Path.Combine(Paths.DataDir, "patches");

        /// <summary>30, 60 and 90 are the engine's fixed presets; 120 and uncapped use the uncapped
        /// patch (scripts\run_windows.py turns them into BB_FPS=uncap with a limit).</summary>
        public static string Fps(LauncherSettings s) => s.Str("fps_mode") is "30" or "90" or "120" or "144" or "uncap" ? s.Str("fps_mode") : "60";

        /// <summary>This launch's answer about a region-changing patch while online ("keep" or
        /// "skip"; scripts/patches.py leaves it out unless "keep"), or null when none is on.</summary>
        public static string? RegionPatchOnline { get; set; }

        /// <summary>This launch's answer about a third-party resolution patch while the output
        /// resolution isn't 1920 x 1080 ("keep" or "skip"; scripts/patches.py leaves it out unless
        /// "keep"), or null when none is on.</summary>
        public static string? ResolutionPatchChoice { get; set; }

        public static Dictionary<string, string> Build(LauncherSettings s)
        {
            var fps = Fps(s);
            var env = new Dictionary<string, string>(StringComparer.Ordinal)
            {
                // The package's engine is already built: run_windows.py must not try MSYS2.
                ["BB_PREBUILT"] = "1",
                ["BB_DATA_DIR"] = Paths.DataDir,
                ["BB_CONFIG"] = Paths.IniPath,
                ["BB_GAME_DIR"] = GameDir(s),
                ["BB_USER_DIR"] = UserDir(s),
                ["BB_MODS_DIR"] = ModsDir(s),
                ["BB_MODS_CONFIG"] = Path.Combine(Paths.DataDir, "mods.json"),
                ["BB_MODS_ENABLED"] = s.Bool("mods_enabled") ? "1" : "0",
                ["BB_PATCHES_DIR"] = PatchesDir(s),
                ["BB_PATCHES_CONFIG"] = Path.Combine(Paths.DataDir, "patches.json"),
                ["BB_LANGUAGE"] = s.Str("language"),
                ["BB_FPS"] = fps,
                ["BB_FPS_LIMIT"] = fps == "uncap" ? "0" : fps,
                ["BB_VBLANK_HZ"] = fps switch { "90" => "90", "30" or "60" => "60", _ => "0" },
                ["PYTHONUNBUFFERED"] = "1",
            };
            // The Play page's DLC switch: off, the game is told no add-ons are installed.
            if (!s.Bool("dlc_enabled")) env["BB_DLC"] = "0";
            else if (DlcDir(s) is { Length: > 0 } dlc) env["BB_DLC_DIR"] = dlc;
            if (RegionPatchOnline is { } region) env["BB_REGION_PATCH_ONLINE"] = region;
            if (ResolutionPatchChoice is { } resolution) env["BB_RESOLUTION_PATCH"] = resolution;
            if (s.Str("present_mode").Length > 0) env["BB_PRESENT_MODE"] = s.Str("present_mode");
            if (s.Bool("frame_stats")) env["BB_FRAME_STATS"] = "1";
            if (s.Bool("fsr_profile")) env["BB_FSR4_PROFILE"] = "1";
            if (s.Bool("audio_stats")) env["BB_AUDIO_STATS"] = "1";
            if (s.Bool("load_profile")) env["BB_SAMPLE_THREADS"] = "load";
            if (s.Bool("fsr4_balanced")) env["BB_FSR4_BALANCED"] = "1";
            // Experimental engine features: on by default, each can be turned off.
            env["BB_PRECISE_PACING"] = s.Bool("exp_frame_pacing") ? "1" : "0";
            env["BB_IMAGE_WRITEBACK_GC"] = s.Bool("exp_vram_cleanup") ? "1" : "0";
            // Gamepads (SDL3 reads its hints from the environment): DualShock 3 through SDL's own
            // driver (with DsHidMini or Sony's sixaxis driver installed), and the community
            // mapping database for third-party pads when the package ships it.
            foreach (var (key, value) in SdlHints())
                env[key] = value;

            // Off: the engine's earlier behaviour; on: its default (left unset).
            if (!s.Bool("exp_vram_budget")) { env["BB_GC_PRESSURE_PERCENT"] = "0"; env["BB_GC_PRESSURE_SECONDS"] = "0"; }
            if (!s.Bool("exp_thread_priority")) env["BB_GPU_THREAD_PRIORITY"] = "0";
            if (!s.Bool("exp_sleep_spin")) env["BB_SLEEP_SPIN_US"] = "600";
            if (!s.Bool("exp_async_pipelines")) env["BB_ASYNC_PIPELINES"] = "0";
            if (!s.Bool("exp_occlusion")) env["BB_OCCLUSION"] = "0";
            if (!s.Bool("exp_fast_draws"))
            {
                env["BB_SUBMIT_END_TASKS"] = "0";
                env["BB_MEMO_REVALIDATE"] = "0";
                env["BB_SAMPLER_MEMO"] = "0";
                env["BB_DRAIN_SLEEP"] = "0";
                env["BB_FAULT_SKIP_TEXTURES"] = "0";
            }
            // Sound threads first (CPUs with many slower cores): off unless chosen.
            if (!s.Bool("audio_priority") || s.Bool("safe_mode_next")) env["BB_AUDIO_THREAD_PRIORITY"] = "0";
            // Sound output: DirectSound writes further ahead than WASAPI, so a late sound thread
            // doesn't leave a gap (SDL reads its driver and buffer size from the environment).
            var output = s.Bool("safe_mode_next") ? "" : s.Str("sound_output");
            if (output.StartsWith("directsound")) env["SDL_AUDIO_DRIVER"] = "directsound";
            if (output == "directsound_large") env["SDL_AUDIO_DEVICE_SAMPLE_FRAMES"] = "2048";
            if (!s.Bool("exp_compile_skip"))
            {
                env["BB_SKIP_UNCOMPILED"] = "0";
                env["BB_LATE_PIPELINE_BIND"] = "0";
                env["BB_PIPELINE_TAKE"] = "0";
            }
            if (s.Bool("section_stats")) env["BB_SECTIONS"] = "1";
            if (s.Bool("safe_mode_next"))
            {
                // Safe mode (after a crash, one run): every experimental engine option and the mods off.
                env["BB_PRECISE_PACING"] = "0";
                env["BB_IMAGE_WRITEBACK_GC"] = "0";
                env["BB_GC_PRESSURE_PERCENT"] = "0";
                env["BB_GC_PRESSURE_SECONDS"] = "0";
                env["BB_GPU_THREAD_PRIORITY"] = "0";
                env["BB_SLEEP_SPIN_US"] = "600";
                env["BB_ASYNC_PIPELINES"] = "0";
                env["BB_OCCLUSION"] = "0";
                env["BB_SUBMIT_END_TASKS"] = "0";
                env["BB_MEMO_REVALIDATE"] = "0";
                env["BB_SAMPLER_MEMO"] = "0";
                env["BB_DRAIN_SLEEP"] = "0";
                env["BB_FAULT_SKIP_TEXTURES"] = "0";
                env["BB_SKIP_UNCOMPILED"] = "0";
                env["BB_LATE_PIPELINE_BIND"] = "0";
                env["BB_PIPELINE_TAKE"] = "0";
                env["BB_MODS_ENABLED"] = "0";
            }
            // The account password never goes to bbport.ini; the engine takes it from here.
            if (s.OnlinePassword is { Length: > 0 } password) env["BB_SHADNET_PASSWORD"] = password;
            // The Hunter's Dream account (scripts/online.py uses it in that pool only).
            if (s.HuntersDreamToken is { Length: > 0 } token && s.Str("hd_name") is { Length: > 0 } name)
            {
                env["BB_THD_TOKEN"] = token;
                env["BB_THD_ONLINE_ID"] = name;
            }
            // What this client reports to the shadNet server after signing in (it signs out clients that report none).
            env["BB_CLIENT_VERSION"] = "shadPS4 Awakenchan/v0.20.0";

            foreach (var item in s.Str("extra_env").Split(' ', StringSplitOptions.RemoveEmptyEntries))
            {
                var at = item.IndexOf('=');
                if (at > 0)
                    env[item[..at]] = item[(at + 1)..];
            }
            WindowsMonitors.ApplyEnvironment(s.Str("display_device"), WindowsMonitors.All(), env);
            return env;
        }

        /// <summary>SDL3 hints for controllers, given to the game and used by the launcher's own SDL (SdlPad).</summary>
        public static IEnumerable<(string Key, string Value)> SdlHints()
        {
            yield return ("SDL_JOYSTICK_HIDAPI_PS3", "1");
            yield return ("SDL_JOYSTICK_HIDAPI_PS3_SIXAXIS_DRIVER", "1");
            var mappings = Path.Combine(Paths.PortDir, "gamecontrollerdb.txt");
            if (File.Exists(mappings)) yield return ("SDL_GAMECONTROLLERCONFIG_FILE", mappings);
        }
    }
}
