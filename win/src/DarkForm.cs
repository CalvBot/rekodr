using System.Runtime.InteropServices;

namespace Clipback;

/// <summary>Base for every window: dark colours and title bar, the app icon, and no flash of blank controls on open
/// (adapted from Compress for Discord's DarkForm)</summary>
class DarkForm : Form
{
    public DarkForm()
    {
        AutoScaleMode = AutoScaleMode.None; // everything is sized from DeviceDpi by hand
        BackColor = Theme.Bg;
        ForeColor = Theme.Text;
        StartPosition = FormStartPosition.CenterScreen;
        KeyPreview = true;
        Icon = AppIcon;
        DoubleBuffered = true;
    }

    static Icon? appIcon;
    public static Icon? AppIcon
    {
        get
        {
            if (appIcon == null)
                try { appIcon = new Icon(typeof(DarkForm).Assembly.GetManifestResourceStream("clipback.ico")!); } catch { }
            return appIcon;
        }
    }

    [DllImport("dwmapi.dll")]
    static extern int DwmSetWindowAttribute(IntPtr hwnd, int attr, ref int value, int size);
    [DllImport("user32.dll")]
    static extern bool RedrawWindow(IntPtr hwnd, IntPtr rect, IntPtr region, int flags);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    static extern IntPtr LoadImage(IntPtr hinst, IntPtr name, int type, int cx, int cy, int flags);
    [DllImport("kernel32.dll")]
    static extern IntPtr GetModuleHandle(IntPtr name);
    [DllImport("user32.dll")]
    static extern int GetSystemMetricsForDpi(int index, uint dpi);
    [DllImport("user32.dll")]
    static extern IntPtr SendMessage(IntPtr hwnd, int msg, IntPtr w, IntPtr l);
    const int DWMWA_CLOAK = 13, RDW_ALLCHILDREN = 0x80, RDW_UPDATENOW = 0x100;

    bool cloaked, revealed;

    protected override void OnHandleCreated(EventArgs e)
    {
        base.OnHandleCreated(e);
        var dark = 1;
        DwmSetWindowAttribute(Handle, 20, ref dark, 4); // immersive dark mode
        var caption = Theme.Bg.R | Theme.Bg.G << 8 | Theme.Bg.B << 16;
        DwmSetWindowAttribute(Handle, 35, ref caption, 4); // caption colour (Windows 11)
        SharpIcons();
        // Kept out of sight until it's drawn, so it never shows as blank boxes for a moment
        if (!revealed) { var on = 1; cloaked = DwmSetWindowAttribute(Handle, DWMWA_CLOAK, ref on, 4) == 0; }
    }

    /// <summary>The exe's own icon at exactly the sizes the taskbar and title bar want for this screen's scaling
    /// (Form.Icon would scale one image and look soft)</summary>
    void SharpIcons()
    {
        var module = GetModuleHandle(IntPtr.Zero);
        foreach (var (kind, metric) in new[] { (1, 11), (0, 49) }) // ICON_BIG/SM_CXICON, ICON_SMALL/SM_CXSMICON
        {
            var px = GetSystemMetricsForDpi(metric, (uint)DeviceDpi);
            // Resource 32512 is the icon the SDK embeds from ApplicationIcon; LR_SHARED (0x8000): loaded once
            var icon = LoadImage(module, 32512, 1, px, px, 0x8000);
            if (icon != IntPtr.Zero) SendMessage(Handle, 0x80, kind, icon); // WM_SETICON
        }
    }

    protected override void OnDpiChanged(DpiChangedEventArgs e)
    {
        base.OnDpiChanged(e);
        SharpIcons();
    }

    protected override void OnShown(EventArgs e)
    {
        base.OnShown(e);
        BeginInvoke(Reveal);
    }

    /// <summary>Draws everything waiting to be drawn, then lets the window be seen. Only the first call does anything.</summary>
    protected void Reveal()
    {
        if (revealed || IsDisposed) return;
        revealed = true;
        if (!cloaked || !IsHandleCreated) return;
        RedrawWindow(Handle, IntPtr.Zero, IntPtr.Zero, RDW_UPDATENOW | RDW_ALLCHILDREN);
        var off = 0;
        DwmSetWindowAttribute(Handle, DWMWA_CLOAK, ref off, 4);
    }
}
