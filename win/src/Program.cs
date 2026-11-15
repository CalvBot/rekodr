using System.Diagnostics;
using System.Runtime.InteropServices;

namespace Clipback;

static class Program
{
    [DllImport("user32.dll")] static extern bool SetForegroundWindow(IntPtr hwnd);
    [DllImport("user32.dll")] static extern bool ShowWindow(IntPtr hwnd, int cmd);
    [DllImport("user32.dll")] static extern bool IsIconic(IntPtr hwnd);

    [STAThread]
    static void Main(string[] args)
    {
        // The video player, started by the clips window in a process of its own (see PlayerProcess)
        if (args is ["--player", var parent])
        {
            Application.SetHighDpiMode(HighDpiMode.PerMonitorV2);
            PlayerHost.Run((IntPtr)long.Parse(parent));
            return;
        }

        // One window: a second launch (tray click, hotkey, Stream Deck) brings the open one to the front,
        // and `--options` switches it to Options
        var screen = args.Contains("--options") ? "options" : null;
        var name = @"Local\clipback-ui";
#if DEBUG
        // Test copies can run beside the installed window: CLIPBACK_UI_INSTANCE=x gives them a lock of their own
        name += Environment.GetEnvironmentVariable("CLIPBACK_UI_INSTANCE");
#endif
        using var single = new Mutex(true, name, out var first);
        if (!first)
        {
            FocusExisting(screen);
            return;
        }

        Application.SetHighDpiMode(HighDpiMode.PerMonitorV2);
        Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault(false);
        Application.Run(new ClipsForm { StartOnOptions = screen == "options" });
    }

    static void FocusExisting(string? screen)
    {
        using var me = Process.GetCurrentProcess();
        foreach (var p in Process.GetProcessesByName(me.ProcessName))
        {
            using (p)
            {
                if (p.Id == me.Id || p.MainWindowHandle == IntPtr.Zero) continue;
#if DEBUG
                if (Environment.GetEnvironmentVariable("CLIPBACK_UI_INSTANCE") != null && p.MainModule?.FileName != Environment.ProcessPath) continue;
#endif
                if (screen != null) ClipsForm.SendScreen(p.MainWindowHandle, screen);
                if (IsIconic(p.MainWindowHandle)) ShowWindow(p.MainWindowHandle, 9); // SW_RESTORE
                SetForegroundWindow(p.MainWindowHandle);
                return;
            }
        }
    }
}
