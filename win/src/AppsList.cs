using System.Globalization;

namespace Clipback;

/// <summary>
/// The apps table of Options: always PerPage rows (blank ones pad a short page) and a pager, so it never changes
/// height and the buttons never move under the mouse when an app moves to the other list.
/// </summary>
sealed class AppsList : Painted
{
    public const int PerPage = 10, Row96 = 44, Pager96 = 42;
    public static int Height96 => PerPage * Row96 + Pager96;

    readonly AppRowView[] rows = new AppRowView[PerPage];
    readonly Btn prev = new("", "chevron-left"), next = new("", "chevron-right");
    int page, pages = 1;

    /// <summary>exe, category</summary>
    public event Action<string, string>? Category;
    /// <summary>exe, Mbps text ("" = the normal bitrate)</summary>
    public event Action<string, string>? Rate;
    /// <summary>exe, the row, where in it</summary>
    public event Action<string, Control, Point>? Menu;

    List<AppInfo> list = [];
    HashSet<string> live = [];
    Dictionary<string, double> rates = [];
    string placeholder = "", empty = "";

    public AppsList()
    {
        BackColor = Theme.Panel;
        for (var i = 0; i < PerPage; i++)
        {
            var row = rows[i] = new AppRowView();
            row.Seg.Picked += cat => { if (row.App is { } a) Category?.Invoke(a.Exe, cat); };
            row.Rate.Committed += () => { if (row.App is { } a) Rate?.Invoke(a.Exe, row.Rate.Text.Trim()); };
            row.MenuAt += p => { if (row.App is { } a) Menu?.Invoke(a.Exe, row, p); };
            Controls.Add(row);
        }
        Controls.AddRange([prev, next]);
        prev.Click += (_, _) => { page--; Fill(); };
        next.Click += (_, _) => { page++; Fill(); };
    }

    public void Show(List<AppInfo> apps, HashSet<string> live, Dictionary<string, double> rates, string placeholder, string empty)
    {
        list = apps;
        this.live = live;
        this.rates = rates;
        this.placeholder = placeholder;
        this.empty = empty;
        Fill();
    }

    void Fill()
    {
        pages = Math.Max(1, (list.Count + PerPage - 1) / PerPage);
        page = Math.Clamp(page, 0, pages - 1);
        for (var i = 0; i < PerPage; i++)
        {
            var k = page * PerPage + i;
            if (k < list.Count)
            {
                var a = list[k];
                rows[i].Bind(a, live.Contains(a.Exe), rates.TryGetValue(a.Exe, out var r) ? r.ToString(CultureInfo.InvariantCulture) : "", placeholder, null);
            }
            else rows[i].Bind(null, false, "", "", i == 0 && list.Count == 0 ? empty : null);
        }
        prev.Enabled = page > 0;
        next.Enabled = page < pages - 1;
        Invalidate();
    }

    /// <summary>A bitrate box of this list has the focus (someone may be typing in it)</summary>
    public bool Typing => rows.Any(r => r.Rate.Box.Focused);

    Rectangle PageLabel => new(next.Left - this.Px(8 + 44), 0, this.Px(44), 0);

    protected override void OnLayout(LayoutEventArgs e)
    {
        base.OnLayout(e);
        if (rows[^1] == null) return; // still being made
        var rh = this.Px(Row96);
        for (var i = 0; i < PerPage; i++) rows[i].SetBounds(0, i * rh, Width, rh);
        int top = PerPage * rh, s = this.Px(34), h = this.Px(28), y = top + this.Px(8);
        next.SetBounds(Width - s, y, s, h);
        prev.SetBounds(next.Left - this.Px(8 + 44 + 8) - s, y, s, h);
    }

    protected override void OnPaint(PaintEventArgs e)
    {
        var g = e.Graphics;
        g.Clear(Theme.Panel);
        var top = PerPage * this.Px(Row96);
        using (var line = new Pen(Theme.Line)) g.DrawLine(line, 0, top, Width, top);
        var r = PageLabel;
        TextRenderer.DrawText(g, $"{page + 1} / {pages}", Theme.Font(this, 12), new Rectangle(r.X, prev.Top, r.Width, prev.Height), Theme.Dim, Theme.OneLine | TextFormatFlags.HorizontalCenter);
    }

    protected override void Dispose(bool disposing)
    {
        base.Dispose(disposing);
    }
}

/// <summary>One app: icon, name (+ New), when it was last heard, the game bitrate box (only shown for games, but its
/// space is always kept) and the category buttons</summary>
sealed class AppRowView : Painted
{
    public readonly Segments Seg = new();
    public readonly Field Rate = new() { Numeric = true, FontPx = 12, PadX96 = 5 };
    public AppInfo? App { get; private set; }
    public event Action<Point>? MenuAt;
    bool playing;
    string? empty;

    static readonly Dictionary<string, Task<Image?>> icons = new(StringComparer.OrdinalIgnoreCase);

    public AppRowView()
    {
        BackColor = Theme.Panel;
        Controls.AddRange([Seg, Rate]);
        Seg.MouseUp += (_, e) => { if (e.Button == MouseButtons.Right) MenuAt?.Invoke(PointToClient(Seg.PointToScreen(e.Location))); };
    }

    public void Bind(AppInfo? a, bool playing, string rate, string placeholder, string? empty)
    {
        App = a;
        this.playing = playing;
        this.empty = empty;
        Seg.Visible = a != null;
        Seg.Lit = a is { New: false } ? a.Category : null;
        Rate.Visible = a?.Category == "game";
        if (!Rate.Box.Focused) Rate.Text = rate;
        Rate.Box.PlaceholderText = placeholder;
        if (a != null && !icons.ContainsKey(a.Exe)) icons[a.Exe] = LoadIcon(a.Exe);
        if (a != null && icons[a.Exe] is { IsCompleted: false } t) _ = t.ContinueWith(_ => { if (!IsDisposed) Invalidate(); }, TaskScheduler.FromCurrentSynchronizationContext());
        Invalidate();
    }

    static async Task<Image?> LoadIcon(string exe)
    {
        var png = await Api.Bytes(Api.IconUrl(exe));
        if (png == null) return null;
        try { return Image.FromStream(new MemoryStream(png)); } catch { return null; }
    }

    int RightEdge => Width - this.Px(4);
    int RateLeft => Seg.Left - this.Px(10) - RateBoxW - this.Px(4) - MbpsW;
    int RateBoxW => this.Px(58);
    int MbpsW => TextRenderer.MeasureText("Mbps", Theme.Font(this, 12), Size.Empty, TextFormatFlags.NoPrefix).Width;

    protected override void OnLayout(LayoutEventArgs e)
    {
        base.OnLayout(e);
        var sw = Seg.FitWidth;
        var sh = this.Px(25);
        Seg.SetBounds(RightEdge - sw, (Height - sh) / 2 + this.Px(1), sw, sh);
        var rh = this.Px(24);
        Rate.SetBounds(RateLeft, (Height - rh) / 2 + this.Px(1), RateBoxW, rh);
    }

    protected override void OnMouseDown(MouseEventArgs e) { Blur.Now(this); base.OnMouseDown(e); }
    protected override void OnMouseUp(MouseEventArgs e)
    {
        base.OnMouseUp(e);
        if (e.Button == MouseButtons.Right) MenuAt?.Invoke(e.Location);
    }

    protected override void OnPaint(PaintEventArgs e)
    {
        var g = e.Graphics;
        g.Clear(Theme.Panel);
        using (var line = new Pen(Theme.Line)) g.DrawLine(line, 0, 0, Width, 0);
        if (App == null)
        {
            if (empty != null) TextRenderer.DrawText(g, empty, Theme.Font(this, 13), new Rectangle(this.Px(4), 0, Width, Height), Theme.Dim, Theme.OneLine);
            return;
        }
        var s = this.Px(20);
        if (icons.TryGetValue(App.Exe, out var t) && t.IsCompletedSuccessfully && t.Result is { } img)
        {
            g.InterpolationMode = System.Drawing.Drawing2D.InterpolationMode.HighQualityBicubic;
            g.DrawImage(img, new Rectangle(this.Px(4), (Height - s) / 2, s, s));
        }
        var x = this.Px(4 + 28 + 4);
        var maxW = (App.Category == "game" ? RateLeft : Seg.Left) - this.Px(10) - x;
        var name = App.Exe.EndsWith(".exe", StringComparison.OrdinalIgnoreCase) ? App.Exe[..^4] : App.Exe;
        var bold = Theme.Font(this, 14, FontStyle.Bold);
        var tagW = App.New ? TagWidth() + this.Px(6) : 0;
        var nameW = Math.Min(maxW - tagW, TextRenderer.MeasureText(g, name, bold, Size.Empty, TextFormatFlags.NoPrefix).Width);
        var top = Height / 2 - this.Px(19);
        TextRenderer.DrawText(g, name, bold, new Rectangle(x, top, nameW, this.Px(20)), Theme.Text, Theme.Ellipsis);
        if (App.New)
        {
            var tw = TagWidth();
            var r = new RectangleF(x + nameW + this.Px(6), top + this.Px(4), tw, this.Px(14));
            Theme.FillRound(g, r, this.Pxf(4), Theme.Accent2);
            TextRenderer.DrawText(g, "NEW", Theme.Font(this, 10, FontStyle.Bold), Rectangle.Round(r), Color.White, Theme.OneLine | TextFormatFlags.HorizontalCenter);
        }
        var status = playing ? "● recording now" : App.LastHeardUs is { } us ? "heard " + Fmt.Ago(us / 1000) : "not heard yet";
        TextRenderer.DrawText(g, status, Theme.Font(this, 12), new Rectangle(x, top + this.Px(20), maxW, this.Px(18)), playing ? Theme.Ok : Theme.Dim, Theme.Ellipsis);
        if (App.Category == "game")
            TextRenderer.DrawText(g, "Mbps", Theme.Font(this, 12), new Rectangle(Rate.Right + this.Px(4), 0, MbpsW + 2, Height + this.Px(2)), Theme.Dim, Theme.OneLine);
    }

    int TagWidth() => TextRenderer.MeasureText("NEW", Theme.Font(this, 10, FontStyle.Bold), Size.Empty, TextFormatFlags.NoPrefix).Width + this.Px(6);
}
