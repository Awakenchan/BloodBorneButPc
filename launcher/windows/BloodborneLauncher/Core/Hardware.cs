using System.IO;
using System.Runtime.InteropServices;
using Microsoft.Win32;

namespace BloodborneLauncher.Core
{
    /// <summary>
    /// What this PC has (GPU and its memory, CPU, RAM, the main display) and the settings that
    /// suit it: upscaler, quality preset, output resolution, frame rate cap and motion vectors.
    /// The GPU comes from the display adapters' registry keys (the one with the most memory, so
    /// the discrete card on a laptop), the display from its current mode.
    /// </summary>
    public sealed record Hardware(string Gpu, string Vendor, long VramBytes, string Cpu, int Threads, long RamBytes,
        int ScreenWidth, int ScreenHeight, int RefreshHz)
    {
        public enum Tier { Low, Mid, High }

        /// <summary>One setting the recommendation would change.</summary>
        public sealed record Change(string Label, string From, string To, Action Apply);

        public int VramGb => (int)Math.Round(VramBytes / 1073741824.0);
        public bool Rtx => Vendor == "NVIDIA" && Gpu.Contains("RTX", StringComparison.OrdinalIgnoreCase);
        /// <summary>Radeon RX 9000 (RDNA 4): FSR 4 runs natively.</summary>
        public bool Rdna4 => Vendor == "AMD" && System.Text.RegularExpressions.Regex.IsMatch(Gpu, @"RX\s*9\d{3}");
        /// <summary>Integrated graphics: little dedicated memory, or a known integrated name.</summary>
        public bool Integrated => VramBytes < 2L << 30 || Gpu.Contains("UHD", StringComparison.OrdinalIgnoreCase) ||
                                  Gpu.Contains("Iris", StringComparison.OrdinalIgnoreCase) ||
                                  Gpu.Contains("Custom GPU", StringComparison.OrdinalIgnoreCase) ||
                                  Gpu.EndsWith("Radeon(TM) Graphics", StringComparison.OrdinalIgnoreCase);

        /// <summary>By memory in whole GB (an "8 GB" card reports a little under 8 GiB).</summary>
        public Tier GpuTier => Integrated ? Tier.Low : VramGb >= 12 ? Tier.High : VramGb >= 8 ? Tier.Mid : Tier.Low;

        /// <summary>One line for the UI: "NVIDIA GeForce RTX 5060 (8 GB) · AMD Ryzen 5 5600 (12 threads) · 2560 × 1440 at 165 Hz".</summary>
        public string Summary =>
            L.F($"{Gpu}{(VramBytes > 0 ? $" ({VramGb} GB)" : "")} · {Cpu} ({Threads} threads) · ") +
            (ScreenWidth > 0 ? L.F($"{ScreenWidth} × {ScreenHeight} at {RefreshHz} Hz") : L.T("display unknown"));

        private static Hardware? _detected;
        public static Hardware Detect() => _detected ??= Probe();

        private static Hardware Probe()
        {
            var (gpu, vendor, vram) = DetectGpu();
            var cpu = "CPU";
            try
            {
                using var key = Registry.LocalMachine.OpenSubKey(@"HARDWARE\DESCRIPTION\System\CentralProcessor\0");
                if (key?.GetValue("ProcessorNameString") is string name && name.Trim().Length > 0) cpu = name.Trim();
            }
            catch { /* unreadable: "CPU" */ }
            var ram = GC.GetGCMemoryInfo().TotalAvailableMemoryBytes;
            var mode = new DevMode { dmSize = (short)Marshal.SizeOf<DevMode>() };
            var (w, h, hz) = EnumDisplaySettings(null, EnumCurrentSettings, ref mode)
                ? (mode.dmPelsWidth, mode.dmPelsHeight, mode.dmDisplayFrequency)
                : (0, 0, 60);
            return new Hardware(gpu, vendor, vram, cpu, Environment.ProcessorCount, ram, w, h, Math.Max(hz, 1));
        }

        private static (string Name, string Vendor, long Vram) DetectGpu()
        {
            var best = ("GPU", "", 0L);
            try
            {
                using var adapters = Registry.LocalMachine.OpenSubKey(
                    @"SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}");
                foreach (var sub in adapters?.GetSubKeyNames() ?? Array.Empty<string>())
                {
                    if (sub.Length != 4 || !sub.All(char.IsDigit)) continue;
                    using var key = adapters!.OpenSubKey(sub);
                    if (key?.GetValue("DriverDesc") is not string name || name.Contains("Basic Display", StringComparison.OrdinalIgnoreCase)) continue;
                    var id = (key.GetValue("MatchingDeviceId") as string ?? "").ToUpperInvariant();
                    var vendor = id.Contains("VEN_10DE") ? "NVIDIA" : id.Contains("VEN_1002") ? "AMD" : id.Contains("VEN_8086") ? "Intel" : "";
                    long memory = key.GetValue("HardwareInformation.qwMemorySize") switch
                    {
                        long q => q,
                        byte[] b when b.Length >= 8 => BitConverter.ToInt64(b, 0),
                        _ => key.GetValue("HardwareInformation.MemorySize") switch
                        {
                            int d => (uint)d,
                            byte[] b when b.Length >= 4 => BitConverter.ToUInt32(b, 0),
                            _ => 0L,
                        },
                    };
                    if (memory > best.Item3 || best.Item1 == "GPU") best = (name.Trim(), vendor, memory);
                }
            }
            catch { /* no access: unknown GPU */ }
            return best;
        }

        // ---------------------------------------------------------------- recommendation

        /// <summary>What would change to suit this PC; empty when the settings already do.</summary>
        public List<Change> Recommend(LauncherSettings s, IniFile ini)
        {
            var changes = new List<Change>();
            void Ini(string label, string key, string to, IReadOnlyList<Choice>? names = null, string fallback = "")
            {
                var from = ini.Get(key, fallback);
                if (from == to) return;
                changes.Add(new Change(label, Name(names, from), Name(names, to), () => ini.Set(key, to)));
            }
            void Setting(string label, string key, string to, IReadOnlyList<Choice>? names = null)
            {
                var from = s.Str(key);
                if (from == to) return;
                changes.Add(new Change(label, Name(names, from), Name(names, to), () => s.Set(key, to)));
            }

            var tier = GpuTier;
            var dlss = File.Exists(Path.Combine(Paths.BinariesDir, "nvngx_dlss.dll"));
            Ini(L.T("Upscaler"), "upscaler", Rtx && dlss ? "dlss" : Rdna4 ? "fsr4" : "fsr3", Choices.Upscalers);

            // The output: the display's own size, by name where it is one of the choices, else
            // "Display (auto)"; 1920 x 1080 when the display is unknown.
            var output = ScreenWidth == 0 ? "1920x1080"
                : Choices.OutputResolutions.Select(c => c.Value).FirstOrDefault(v => v == $"{ScreenWidth}x{ScreenHeight}") ?? "auto";
            Ini(L.T("Output resolution"), "output_res", output, Choices.OutputResolutions, "1920x1080");

            // How far below the output the game renders: more for bigger outputs and smaller GPUs.
            var height = Choices.OutputSize(output).H;
            var preset = (height, tier) switch
            {
                (<= 720, Tier.Low) => 1,
                (<= 720, _) => 0,
                (<= 1080, Tier.High) => 0,
                (<= 1080, Tier.Mid) => 1,
                (<= 1080, _) => 2,
                (<= 1440, Tier.High) => 1,
                (<= 1440, Tier.Mid) => 2,
                (<= 1440, _) => 3,
                (_, Tier.High) => 2,
                (_, Tier.Mid) => 3,
                _ => 4,
            };
            Ini(L.T("Quality preset"), "preset", preset.ToString(), Choices.Presets, "0");

            // 120 needs a fast display, a mid or better GPU and a CPU with 12+ threads (the game's
            // render thread is the limit); 60 otherwise.
            var fps = RefreshHz >= 119 && tier != Tier.Low && Threads >= 12 ? "120" : "60";
            Setting(L.T("Frame rate"), "fps_mode", fps, Choices.FpsModes);

            // Character motion vectors cost about 10%: on for the biggest GPUs, off for the smallest.
            if (tier == Tier.High) Ini(L.T("Object motion vectors"), "object_motion", "1", OnOff, "1");
            if (tier == Tier.Low) Ini(L.T("Object motion vectors"), "object_motion", "0", OnOff, "1");
            return changes;
        }

        private static readonly Choice[] OnOff = { new(L.T("On"), "1"), new(L.T("Off"), "0") };

        private static string Name(IReadOnlyList<Choice>? names, string value) =>
            names?.FirstOrDefault(c => c.Value == value)?.Label.Split(" (")[0] ?? (value.Length > 0 ? value : L.T("default"));

        // ---------------------------------------------------------------- display mode

        private const int EnumCurrentSettings = -1;

        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        private static extern bool EnumDisplaySettings(string? deviceName, int modeNum, ref DevMode devMode);

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        private struct DevMode
        {
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmDeviceName;
            public short dmSpecVersion, dmDriverVersion, dmSize, dmDriverExtra;
            public int dmFields, dmPositionX, dmPositionY, dmDisplayOrientation, dmDisplayFixedOutput;
            public short dmColor, dmDuplex, dmYResolution, dmTTOption, dmCollate;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmFormName;
            public short dmLogPixels;
            public int dmBitsPerPel, dmPelsWidth, dmPelsHeight, dmDisplayFlags, dmDisplayFrequency;
            public int dmICMMethod, dmICMIntent, dmMediaType, dmDitherType, dmReserved1, dmReserved2, dmPanningWidth, dmPanningHeight;
        }
    }
}
