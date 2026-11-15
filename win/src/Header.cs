using System.Drawing.Drawing2D;

namespace Clipback;

/// <summary>
/// The bar along the top of every screen: the recording dot, "clipback", the status line, and buttons the screen
/// adds on the right (laid out right to left, each at a fixed width). The status comes from /api/status every 2 s;
/// the Pause button and the Options button's new-apps badge are kept up to date from it.
/// </summary>
sealed class Header : Painted
{
    public const int Height96 = 52;
    readonly List<(Control control, int width96)> right = [];
    readonly System.Windows.Forms.Timer poll = new() { Interval = 2000 };
    string status = "";
    bool recording, busy;

    public Btn Pause { get; } = new("Pause", "pause") { Labels = ["Pause", "Resume"] };
    public Btn Options { get; } = new("Options", "settings") { Inset = 6 };
    /// <summary>The last status clipback sent, or null if it isn't answering</summary>
    public Status? Last { get; private set; }
    public event Action<Status?>? Updated;
    readonly ToolTip tip = new();

    public Header()
    {
        BackColor = Theme.Bg;
        Pause.Click += async (_, _) =>
        {
            if (Last == null || busy) return;
            busy = true;
            try { await Api.Pause(!Last.Paused); } catch { }
            busy = false;
            await Tick();
        };
        Options.Click += (_, _) => Api.OpenPage("/options"); // temporary, until Options is native
        poll.Tick += async (_, _) => { if (FindForm() is { WindowState: not FormWindowState.Minimized }) await Tick(); };
    }

    /// <summary>Adds a control on the right, after (to the left of) the ones already added. width96 = 0: the button's own fit.</summary>
    public void AddRight(Control c, int width96 = 0)
    {
        right.Add((c, width96));
        Controls.Add(c);
    }

    public void Start()
    {
        _ = Tick();
        poll.Start();
    }

    public async Task Tick()
    {
        Status? s = null;
        try { s = await Api.Status(); } catch { }
        if (IsDisposed) return;
        Last = s;
        recording = s is { Paused: false, Live: true };
        status = s == null ? "clipback isn't running" : Describe(s);
        tip.SetToolTip(this, s == null ? null : $"{Fmt.Size(s.RingBytes)} of {Fmt.Size(s.RingCap)}");
        var paused = s?.Paused == true;
        Pause.Text = paused ? "Resume" : "Pause";
        Pause.Icon = paused ? "play" : "pause";
        Pause.Enabled = s != null;
        Options.Badge = s?.NewApps ?? 0;
        tip.SetToolTip(Options, s?.NewApps > 0 ? $"{s.NewApps} new app{(s.NewApps == 1 ? "" : "s")} to sort in Options" : null);
        Invalidate();
        Updated?.Invoke(s);
    }

    /// <summary>Same text as the web UI's statusBar (ui/common.js)</summary>
    static string Describe(Status s)
    {
        if (s.Paused) return "Paused";
        if (!s.Live) return "Waiting for the screen…";
        var span = s.OldestUs is { } o ? Fmt.Time((s.NowUs - o) / 1e6) : "0:00";
        var text = $"Recording · {span} buffered";
        if (s.Encode is { ValueKind: System.Text.Json.JsonValueKind.Array } enc && enc.GetArrayLength() > 1 && enc[1].ValueKind == System.Text.Json.JsonValueKind.String)
            text += $" · {enc[0].GetDouble():0.##} Mbps for {StripExe(enc[1].GetString()!)}";
        var apps = s.Audio.Where(a => a.Exe != "mic").Select(a => StripExe(a.Exe)).ToList();
        if (apps.Count > 0) text += " · " + string.Join(", ", apps);
        return text;
    }

    static string StripExe(string exe) => exe.EndsWith(".exe", StringComparison.OrdinalIgnoreCase) ? exe[..^4] : exe;

    int TitleRight => this.Px(16 + 11 + 8) + TextRenderer.MeasureText("clipback", Theme.Font(this, 15, FontStyle.Bold)).Width + this.Px(16);
    int ButtonsLeft = 0;

    protected override void OnLayout(LayoutEventArgs e)
    {
        base.OnLayout(e);
        int x = Width - this.Px(16), h = this.Px(30), y = (Height - this.Px(1) - h) / 2, gap = this.Px(10);
        foreach (var (c, w96) in right)
        {
            var w = c is Btn b ? Math.Max(this.Px(w96), b.FitWidth) : w96 > 0 ? this.Px(w96) : c.Width;
            if (c is Btn { Inset: > 0 } ib)
            {
                var inset = this.Px(ib.Inset);
                c.SetBounds(x - w + inset, y - inset, w, h + 2 * inset);
                x -= w - 2 * inset;
            }
            else
            {
                c.SetBounds(x - w, y, w, h);
                x -= w;
            }
            x -= gap;
        }
        ButtonsLeft = x;
    }

    protected override void OnPaint(PaintEventArgs e)
    {
        var g = e.Graphics;
        g.Clear(Theme.Bg);
        using (var line = new Pen(Theme.Line)) g.DrawLine(line, 0, Height - 1, Width, Height - 1);
        g.SmoothingMode = SmoothingMode.AntiAlias;
        float d = this.Pxf(11), cx = this.Pxf(16), cy = (Height - 1) / 2f;
        if (recording)
            using (var glow = new SolidBrush(Color.FromArgb(64, Theme.Accent))) g.FillEllipse(glow, cx - this.Pxf(3), cy - d / 2 - this.Pxf(3), d + this.Pxf(6), d + this.Pxf(6));
        using (var dot = new SolidBrush(recording ? Theme.Accent : Color.FromArgb(0x66, 0x66, 0x66))) g.FillEllipse(dot, cx, cy - d / 2, d, d);
        var titleFont = Theme.Font(this, 15, FontStyle.Bold);
        var tx = (int)(cx + d + this.Pxf(8));
        TextRenderer.DrawText(g, "clipback", titleFont, new Rectangle(tx, 0, this.Px(120), Height - 1), Theme.Text, Theme.OneLine);
        var sx = TitleRight;
        TextRenderer.DrawText(g, status, Theme.Font(this, 13), new Rectangle(sx, 0, Math.Max(0, ButtonsLeft - sx), Height - 1), Theme.Dim, Theme.Ellipsis);
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing) { poll.Dispose(); tip.Dispose(); }
        base.Dispose(disposing);
    }
}
