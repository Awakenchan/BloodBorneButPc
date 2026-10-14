using BloodborneLauncher.Core;

static void Require(bool condition, string message)
{
    if (!condition) throw new InvalidOperationException(message);
}

var monitors = new[] {
    new WindowsMonitors.Monitor(@"\\.\DISPLAY1", "Primary", 0, 0, 1920, 1080, true),
    new WindowsMonitors.Monitor(@"\\.\DISPLAY2", "Left", -3840, -400, 3840, 2160, false)
};
var env = new Dictionary<string, string>();
WindowsMonitors.ApplyEnvironment(@"\\.\display2", monitors, env);
Require(env["BB_MONITOR_BOUNDS"] == "-3840,-400,3840,2160", "Lost negative coordinates or case-insensitive selection");
Require(env["BB_DISPLAY_SIZE"] == "3840x2160", "Auto resolution must use the selected display");

var rearranged = new[] { monitors[0], monitors[1] with { X = 1920, Y = 200 } };
WindowsMonitors.ApplyEnvironment(monitors[1].Device, rearranged, env);
Require(env["BB_MONITOR_BOUNDS"] == "1920,200,3840,2160", "Saved device must follow a rearranged monitor");
WindowsMonitors.ApplyEnvironment(@"\\.\DISPLAY99", monitors, env);
Require(env["BB_MONITOR_BOUNDS"] == monitors[0].Bounds && env["BB_MONITOR_LABEL"].Contains("disconnected"),
    "A disconnected device must fall back to the primary display");

env["BB_DISPLAY_SIZE"] = "2560x1440";
WindowsMonitors.ApplyEnvironment("", monitors, env);
Require(env["BB_MONITOR_BOUNDS"] == "" && env["BB_MONITOR_LABEL"] == "", "Default must clear inherited selection");
Require(env["BB_DISPLAY_SIZE"] == "2560x1440", "Default must preserve an explicit display-size override");
WindowsMonitors.ApplyEnvironment(monitors[1].Device, Array.Empty<WindowsMonitors.Monitor>(), env);
Require(env["BB_MONITOR_BOUNDS"] == "", "An empty display list must not reuse stale coordinates");

var odd = new[] { monitors[0] with { Width = 1919, Height = 1079 } };
WindowsMonitors.ApplyEnvironment(odd[0].Device, odd, env);
Require(env["BB_DISPLAY_SIZE"] == "1918x1078", "Output dimensions must be even");
Require(WindowsMonitors.Resolve("missing", new[] { monitors[1] }) == monitors[1], "Use the first display if none is marked primary");

var actual = WindowsMonitors.All();
Require(actual.Count > 0, "Windows returned no active displays");
foreach (var monitor in actual)
    Console.WriteLine($"{monitor.Label}: {monitor.Bounds}");
Console.WriteLine("Display selection checks passed");
