using System.IO;
using System.Text;
using System.Windows;
using System.Windows.Threading;

namespace BloodborneLauncher
{
    public partial class App : Application
    {
        private static readonly string CrashLog = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
            "bbport-launcher", "crash.log");

        protected override void OnStartup(StartupEventArgs e)
        {
            L.Initialize();
            base.OnStartup(e);

            // A UI-thread fault is recoverable; the other two are only logged, so a hard exit
            // still leaves evidence behind.
            DispatcherUnhandledException += OnDispatcherException;
            AppDomain.CurrentDomain.UnhandledException += (_, args) =>
                Record("AppDomain", args.ExceptionObject as Exception);
            TaskScheduler.UnobservedTaskException += (_, args) =>
            {
                Record("UnobservedTask", args.Exception);
                args.SetObserved();
            };

            // The launcher's new name (0.9.34 beta 8): once, before anything else.
            if (Core.LanternRename.Run(e.Args))
            {
                Shutdown();
                return;
            }

            // An update to the engine\ layout: the old top-level engine folders go first.
            if (Core.Paths.DetectPortDir() is { } package)
            {
                FixShippedMouseBinds(package);
                var failed = Core.LayoutCleanup.Run(package);
                if (failed.Count > 0)
                    MessageBox.Show("These old files could not be removed (in use?) and can be deleted by hand:\n\n" +
                                    string.Join("\n", failed), "Lantern Engine", MessageBoxButton.OK, MessageBoxImage.Information);
            }
        }

        /// <summary>Packages before 0.9.34 beta 9 shipped lock on as Mouse3 and strong attack as Mouse2,
        /// meant as middle and right click; the engine counts buttons as SDL does (2 middle, 3 right),
        /// so they were swapped. Fixed once, and only while both are still those shipped values.</summary>
        private static void FixShippedMouseBinds(string package)
        {
            try
            {
                var ini = Path.Combine(package, "bbport.ini");
                if (!File.Exists(ini)) return;
                var lines = File.ReadAllLines(ini);
                var lockOn = Array.FindIndex(lines, l => l.Trim() == "bind.lock_on=Mouse3");
                var strong = Array.FindIndex(lines, l => l.Trim() == "bind.strong_attack=Mouse2");
                if (lockOn < 0 || strong < 0) return;
                lines[lockOn] = "bind.lock_on=Mouse2";
                lines[strong] = "bind.strong_attack=Mouse3";
                File.WriteAllLines(ini, lines);
            }
            catch (Exception)
            {
                // The binds stay as they were; the launcher's Controls page can change them.
            }
        }

        private void OnDispatcherException(object sender, DispatcherUnhandledExceptionEventArgs args)
        {
            // WPF's window frame (WindowChrome) throws when Windows switches desktop composition
            // off for a moment (a display mode change or a driver reset as a game exits): nothing
            // is wrong with the launcher, and the frame is redrawn when composition returns.
            if (args.Exception is System.Runtime.InteropServices.COMException { HResult: unchecked((int)0x80263001) })
            {
                args.Handled = true;
                return;
            }
            Record("Dispatcher", args.Exception);
            MessageBox.Show(L.F($"{args.Exception.Message}\n\nLogged to:\n{CrashLog}"),
                L.T("Unexpected error"), MessageBoxButton.OK, MessageBoxImage.Error);
            args.Handled = true;
        }

        private static void Record(string source, Exception? exception)
        {
            try
            {
                Directory.CreateDirectory(Path.GetDirectoryName(CrashLog)!);
                var entry = new StringBuilder()
                    .AppendLine(new string('=', 70))
                    .AppendLine($"{DateTime.Now:yyyy-MM-dd HH:mm:ss}  [{source}]")
                    .AppendLine(exception?.ToString() ?? L.T("(no exception object)"))
                    .AppendLine();
                File.AppendAllText(CrashLog, entry.ToString());
            }
            catch
            {
                // Logging must never be the thing that takes the app down.
            }
        }
    }
}
