namespace BloodborneLauncher.Core
{
    /// <summary>
    /// Graphics › Engine preset: the newer engine features on or off in one go. New is the default;
    /// Legacy runs the engine the way the 0.9.33 stable did (the features that were only on in
    /// betas off). The shader compiler is its own switch: the legacy one is the default in both. Changing a single feature by hand shows Custom.
    /// </summary>
    public static class EnginePreset
    {
        public const string New = "new", Legacy = "legacy", Custom = "custom";

        /// <summary>Each feature's setting and its value in New; Legacy has the opposite.</summary>
        private static readonly (string Key, bool New)[] Features =
        {
            ("exp_occlusion", true),
            ("exp_fast_draws", true),
            ("exp_compile_skip", true),
            ("exp_vram_budget", true),
        };

        public static readonly Choice[] Choices =
        {
            new(L.T("New (recommended)"), New, L.T("The current engine: occlusion culling, faster draw thread, quicker shader compiling, more VRAM for textures")),
            new(L.T("Legacy (0.9.33)"), Legacy, L.T("The engine as 0.9.33 ran it, with those features off. Try it if the new engine crashes or draws something wrong")),
            new("Custom", Custom, L.T("Features below changed one by one")),
        };

        public static string Current(LauncherSettings s)
        {
            if (Features.All(f => s.Bool(f.Key) == f.New)) return New;
            if (Features.All(f => s.Bool(f.Key) != f.New)) return Legacy;
            return Custom;
        }

        public static void Apply(LauncherSettings s, string preset)
        {
            if (preset is not (New or Legacy)) return;
            foreach (var (key, value) in Features)
                s.Set(key, preset == New ? value : !value);
        }
    }
}
