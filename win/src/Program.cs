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

        // One window: a second launch (tray click, hotkey, Stream Deck) brings the open one to the front
        using var single = new Mutex(true, @"Local\clipback-ui", out var first);
        if (!first)
        {
            FocusExisting();
            return;
        }

        Application.SetHighDpiMode(HighDpiMode.PerMonitorV2);
        Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault(false);
        Application.Run(new ClipsForm());
    }

    static void FocusExisting()
    {
        using var me = Process.GetCurrentProcess();
        foreach (var p in Process.GetProcessesByName(me.ProcessName))
        {
            using (p)
            {
                if (p.Id == me.Id || p.MainWindowHandle == IntPtr.Zero) continue;
                if (IsIconic(p.MainWindowHandle)) ShowWindow(p.MainWindowHandle, 9); // SW_RESTORE
                SetForegroundWindow(p.MainWindowHandle);
                return;
            }
        }
    }
}
