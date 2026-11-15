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
        // The editor's video, the same way (see EditorPlayer)
        if (args is ["--editor-player", var editor])
        {
            Application.SetHighDpiMode(HighDpiMode.PerMonitorV2);
            EditorHost.Run((IntPtr)long.Parse(editor));
            return;
        }

        // One window: a second launch (tray click, hotkey, Stream Deck) brings the open one to the front,
        // and hands it its arguments (e.g. --edit <clip id> --t <seconds>)
        using var single = new Mutex(true, @"Local\clipback-ui", out var first);
        if (!first)
        {
            FocusExisting(args);
            return;
        }

        Application.SetHighDpiMode(HighDpiMode.PerMonitorV2);
        Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault(false);
        Application.Run(new ClipsForm { StartArgs = args });
    }

    static void FocusExisting(string[] args)
    {
        using var me = Process.GetCurrentProcess();
        foreach (var p in Process.GetProcessesByName(me.ProcessName))
        {
            using (p)
            {
                if (p.Id == me.Id || p.MainWindowHandle == IntPtr.Zero) continue;
                if (IsIconic(p.MainWindowHandle)) ShowWindow(p.MainWindowHandle, 9); // SW_RESTORE
                if (args.Length > 0) SendArgs(p.MainWindowHandle, args);
                SetForegroundWindow(p.MainWindowHandle);
                return;
            }
        }
    }

    // ---------- arguments for the running window (WM_COPYDATA) ----------

    const int WM_COPYDATA = 0x4A;
    const nint ArgsTag = 0x434C4241; // "CLBA": our own message, not something else's WM_COPYDATA

    [StructLayout(LayoutKind.Sequential)]
    struct COPYDATASTRUCT { public nint dwData; public int cbData; public IntPtr lpData; }

    [DllImport("user32.dll")] static extern IntPtr SendMessageTimeout(IntPtr hwnd, int msg, IntPtr w, ref COPYDATASTRUCT l, int flags, int timeout, out IntPtr result);

    static void SendArgs(IntPtr hwnd, string[] args)
    {
        var text = string.Join('\n', args);
        var data = Marshal.StringToHGlobalUni(text);
        try
        {
            var cds = new COPYDATASTRUCT { dwData = ArgsTag, cbData = (text.Length + 1) * 2, lpData = data };
            SendMessageTimeout(hwnd, WM_COPYDATA, IntPtr.Zero, ref cds, 0x2 /* SMTO_ABORTIFHUNG */, 3000, out _);
        }
        finally { Marshal.FreeHGlobal(data); }
    }

    /// <summary>The arguments a second launch sent (see SendArgs), or null if this message isn't that</summary>
    public static string[]? ReadArgs(ref Message m)
    {
        if (m.Msg != WM_COPYDATA || m.LParam == IntPtr.Zero) return null;
        var cds = Marshal.PtrToStructure<COPYDATASTRUCT>(m.LParam);
        if (cds.dwData != ArgsTag || cds.lpData == IntPtr.Zero) return null;
        m.Result = 1;
        return (Marshal.PtrToStringUni(cds.lpData) ?? "").Split('\n');
    }

    /// <summary>"--edit &lt;clip id&gt; [--t seconds]": the clip and time to open in the editor, or null</summary>
    public static (string id, double t)? EditArgs(string[] args)
    {
        var i = Array.IndexOf(args, "--edit");
        if (i < 0 || i + 1 >= args.Length || args[i + 1] == "") return null;
        var t = 0.0;
        var ti = Array.IndexOf(args, "--t");
        if (ti >= 0 && ti + 1 < args.Length) double.TryParse(args[ti + 1], System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out t);
        return (args[i + 1], double.IsFinite(t) ? Math.Max(0, t) : 0);
    }
}
