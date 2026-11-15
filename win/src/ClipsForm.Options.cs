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
