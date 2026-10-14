using System.Runtime.InteropServices;

namespace BloodborneLauncher.Core
{
    /// <summary>Active Windows monitors in physical pixels; the saved device survives rearrangement.</summary>
    public static class WindowsMonitors
    {
        public sealed record Monitor(string Device, string Name, int X, int Y, int Width, int Height, bool Primary)
        {
            public string Number => Device.StartsWith(@"\\.\DISPLAY", StringComparison.OrdinalIgnoreCase)
                ? Device[11..] : Device;
            public string Label => $"Monitor {Number}: {Name}" + (Primary ? " (primary)" : "");
            public string Bounds => FormattableString.Invariant($"{X},{Y},{Width},{Height}");
        }

        public static List<Monitor> All()
        {
            var result = new List<Monitor>();
            var previousDpi = SetThreadDpiAwarenessContext(new IntPtr(-4));
            try
            {
                EnumDisplayMonitors(IntPtr.Zero, IntPtr.Zero, (handle, _, _, _) =>
                {
                    var info = new MonitorInfo { Size = Marshal.SizeOf<MonitorInfo>() };
                    if (!GetMonitorInfo(handle, ref info)) return true;
                    var display = new DisplayDevice { Size = Marshal.SizeOf<DisplayDevice>() };
                    var name = EnumDisplayDevices(info.Device, 0, ref display, 0) && !string.IsNullOrWhiteSpace(display.Description)
                        ? display.Description : "Display";
                    var bounds = info.Monitor;
                    if (bounds.Right > bounds.Left && bounds.Bottom > bounds.Top)
                        result.Add(new(info.Device, name, bounds.Left, bounds.Top,
                            bounds.Right - bounds.Left, bounds.Bottom - bounds.Top, (info.Flags & 1) != 0));
                    return true;
                }, IntPtr.Zero);
            }
            finally
            {
                if (previousDpi != IntPtr.Zero) SetThreadDpiAwarenessContext(previousDpi);
            }
            return result.OrderBy(m => int.TryParse(m.Number, out var n) ? n : int.MaxValue).ToList();
        }

        public static Monitor? Resolve(string device, IReadOnlyList<Monitor> monitors) =>
            monitors.FirstOrDefault(m => string.Equals(m.Device, device, StringComparison.OrdinalIgnoreCase))
            ?? monitors.FirstOrDefault(m => m.Primary) ?? monitors.FirstOrDefault();

        public static void ApplyEnvironment(string device, IReadOnlyList<Monitor> monitors, IDictionary<string, string> env)
        {
            // Do not inherit an old selection from a parent process when the UI says Default.
            env["BB_MONITOR_BOUNDS"] = "";
            env["BB_MONITOR_LABEL"] = "";
            if (string.IsNullOrWhiteSpace(device)) return;
            var selected = Resolve(device, monitors);
            if (selected == null) return;
            env["BB_MONITOR_BOUNDS"] = selected.Bounds;
            env["BB_MONITOR_LABEL"] = string.Equals(device, selected.Device, StringComparison.OrdinalIgnoreCase)
                ? selected.Label : $"Selected monitor disconnected; using {selected.Label}";
            env["BB_DISPLAY_SIZE"] = FormattableString.Invariant($"{selected.Width - selected.Width % 2}x{selected.Height - selected.Height % 2}");
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct Rect { public int Left, Top, Right, Bottom; }
        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        private struct MonitorInfo
        {
            public int Size; public Rect Monitor, Work; public uint Flags;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string Device;
        }
        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        private struct DisplayDevice
        {
            public int Size;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string Device;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string Description;
            public uint Flags;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string Id;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string Key;
        }
        private delegate bool MonitorCallback(IntPtr handle, IntPtr dc, IntPtr rect, IntPtr data);
        [DllImport("user32.dll")] [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool EnumDisplayMonitors(IntPtr dc, IntPtr rect, MonitorCallback callback, IntPtr data);
        [DllImport("user32.dll", EntryPoint = "GetMonitorInfoW", CharSet = CharSet.Unicode)] [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool GetMonitorInfo(IntPtr monitor, ref MonitorInfo info);
        [DllImport("user32.dll", EntryPoint = "EnumDisplayDevicesW", CharSet = CharSet.Unicode)] [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool EnumDisplayDevices(string device, uint index, ref DisplayDevice info, uint flags);
        [DllImport("user32.dll")]
        private static extern IntPtr SetThreadDpiAwarenessContext(IntPtr value);
    }
}
