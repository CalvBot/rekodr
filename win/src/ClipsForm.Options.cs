using System.Runtime.InteropServices;

namespace Clipback;

/// <summary>
/// The Options screen inside the clips window: the header's Options button (or `clipback-ui --options`) swaps the
/// clips for it, and its Clips button swaps back. It's made the first time it's opened, and loads fresh each time,
/// like opening the web page did.
/// </summary>
sealed partial class ClipsForm
{
    OptionsView? options;

    /// <summary>Open on the Options screen (`clipback-ui --options`)</summary>
    public bool StartOnOptions { get; init; }
    bool OnOptions => options?.Visible == true;

    Control[] ClipsScreen => [chips, grid, pager, search, save30, header.Options];

    void InitOptions()
    {
        header.Options.Click += (_, _) => ShowOptions();
        // No text box starts with the focus (the first one would hide its placeholder)
        Shown += (_, _) => { if (OnOptions) options!.FocusHome(); };
    }

    protected override void OnLoad(EventArgs e)
    {
#if DEBUG
        AppDomain.CurrentDomain.UnhandledException += (_, x) => File.WriteAllText(Path.Combine(Path.GetTempPath(), "claude-options-test", "crash.txt"), x.ExceptionObject.ToString());
        Application.ThreadException += (_, x) => File.WriteAllText(Path.Combine(Path.GetTempPath(), "claude-options-test", "crash.txt"), x.Exception.ToString());
#endif
        base.OnLoad(e);
        if (StartOnOptions) ShowOptions();
    }

    /// <summary>Switches to Options (nothing happens if it's already showing, so typed changes aren't lost)</summary>
    void ShowOptions()
    {
        if (OnOptions) return;
        if (options == null)
        {
            options = new OptionsView { Toast = (m, err) => toast.Show(m, err), Visible = false };
            options.Back += ShowClipsScreen;
            Controls.Add(options);
            LayoutOptions();
        }
        if (player.Visible) player.Close();
        SuspendLayout();
        foreach (var c in ClipsScreen) c.Visible = false;
        options.Visible = true;
        options.BringToFront();
        toast.BringToFront();
        ResumeLayout();
        options.FocusHome();
        _ = options.Open();
#if DEBUG
        OptionsTest();
#endif
    }

    void ShowClipsScreen()
    {
        if (options == null) return;
        options.Close();
        SuspendLayout();
        options.Visible = false;
        foreach (var c in ClipsScreen) c.Visible = true;
        ResumeLayout();
        grid.Focus();
    }

    void LayoutOptions()
    {
        var top = this.Px(Header.Height96);
        options?.SetBounds(0, top, ClientSize.Width, Math.Max(0, ClientSize.Height - top));
    }

    // ---------- a second launch hands its screen to this window ----------

    const int WM_COPYDATA = 0x4A;
    const long ScreenMessage = 0x434C4250; // "CLBP"

    [StructLayout(LayoutKind.Sequential)]
    struct CopyData
    {
        public IntPtr Data;
        public int Size;
        public IntPtr Ptr;
    }

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    static extern IntPtr SendMessageTimeout(IntPtr hwnd, int msg, IntPtr w, ref CopyData l, int flags, int timeout, out IntPtr result);

    /// <summary>Asks the window already open to show a screen ("options")</summary>
    public static void SendScreen(IntPtr hwnd, string screen)
    {
        var text = Marshal.StringToHGlobalUni(screen);
        try
        {
            var data = new CopyData { Data = (IntPtr)ScreenMessage, Size = (screen.Length + 1) * 2, Ptr = text };
            SendMessageTimeout(hwnd, WM_COPYDATA, IntPtr.Zero, ref data, 0x2 /* SMTO_ABORTIFHUNG */, 2000, out _);
        }
        finally { Marshal.FreeHGlobal(text); }
    }

    protected override void WndProc(ref Message m)
    {
        if (m.Msg == WM_COPYDATA)
        {
            var data = Marshal.PtrToStructure<CopyData>(m.LParam);
            if ((long)data.Data == ScreenMessage)
            {
                var screen = Marshal.PtrToStringUni(data.Ptr, Math.Max(0, data.Size / 2 - 1));
                if (screen == "options") BeginInvoke(ShowOptions);
                m.Result = 1;
                return;
            }
        }
        base.WndProc(ref m);
    }

#if DEBUG
    /// <summary>Debug builds: CLIPBACK_OPTIONS_TEST="scroll=900;ignored" scrolls and opens the ignored list for screenshots</summary>
    void OptionsTest()
    {
        var script = Environment.GetEnvironmentVariable("CLIPBACK_OPTIONS_TEST");
        if (string.IsNullOrEmpty(script) || options == null) return;
        var t = new System.Windows.Forms.Timer { Interval = 2500 };
        t.Tick += (_, _) =>
        {
            t.Dispose();
            foreach (var part in script.Split(';', StringSplitOptions.RemoveEmptyEntries))
            {
                var kv = part.Split('=', 2);
                options.TestStep(kv[0].Trim(), kv.Length > 1 ? kv[1].Trim() : "");
            }
        };
        t.Start();
    }
#endif
}
