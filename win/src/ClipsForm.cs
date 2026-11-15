using System.Globalization;

namespace Clipback;

/// <summary>
/// The clips window: header (status, search, Save 30s, Pause, Options), a chip per game, a fixed-size page of clip
/// cards with a pager, and the player over them when a clip is clicked. The same features as ui/index.html.
/// </summary>
sealed partial class ClipsForm : DarkForm
{
    readonly Header header = new();
    readonly SearchBox search = new("Search clips");
    readonly Btn save30 = new("Save 30s", "history") { Labels = ["Save 30s", "Saving…"] };
    readonly ChipBar chips = new();
    readonly ClipGrid grid = new();
    readonly Pager pager = new();
    readonly PlayerView player = new();
    readonly Toast toast = new();
    readonly System.Windows.Forms.Timer refresh = new() { Interval = 4000 };
    readonly ToolTip tips = new();

    List<Clip> clips = [];
    string game = "", filter = "", lastSig = "";

    public ClipsForm()
    {
        Text = "clipback";
        MinimumSize = new Size(760, 520);

        header.AddRight(header.Options);
        header.AddRight(header.Pause, 96);
        header.AddRight(save30);
        header.AddRight(search, 170);
        tips.SetToolTip(save30, "Save the last 30 seconds");

        Controls.AddRange([toast, player, header, chips, grid, pager]);
        player.BringToFront();
        toast.BringToFront();

        search.Box.TextChanged += (_, _) => { filter = search.Box.Text; ShowClips(false); };
        save30.Click += async (_, _) => await Save30();
        chips.Picked += g => { game = g; chips.Mark(game); ShowClips(false); };
        grid.PagesChanged += () => pager.Set(grid.Page, grid.PageCount, grid.Count);
        grid.Play += (c, f) => Play(c, f);
        grid.Menu += (c, at) => CardMenu(c, at);
        pager.Prev.Click += (_, _) => grid.GoTo(grid.Page - 1);
        pager.Next.Click += (_, _) => grid.GoTo(grid.Page + 1);
        player.OnCopy = c => _ = Copy(c);
        player.OnFolder = c => _ = Reveal(c);
        player.OnEdit = c => Edit(c, player.Position);
        player.Closed += () => grid.Focus();
        refresh.Tick += async (_, _) => { if (WindowState != FormWindowState.Minimized && !player.Visible) await LoadClips(); };
        InitOptions();
    }

    protected override void OnHandleCreated(EventArgs e)
    {
        base.OnHandleCreated(e);
        // 1200×820 like the old Edge window, scaled for this screen
        var area = Screen.FromHandle(Handle).WorkingArea;
        Size = new Size(Math.Min(area.Width, this.Px(1200)), Math.Min(area.Height, this.Px(820)));
        CenterToScreen();
    }

    protected override async void OnShown(EventArgs e)
    {
        base.OnShown(e);
        header.Start();
        refresh.Start();
        await LoadClips();
    }

    protected override void OnLayout(LayoutEventArgs e)
    {
        base.OnLayout(e);
        var w = ClientSize.Width;
        var top = this.Px(Header.Height96);
        header.SetBounds(0, 0, w, top);
        chips.SetBounds(0, top, w, this.Px(42));
        var pagerH = this.Px(44);
        grid.SetBounds(0, chips.Bottom, w, ClientSize.Height - chips.Bottom - pagerH);
        pager.SetBounds(0, grid.Bottom, w, pagerH);
        player.SetBounds(0, top, w, ClientSize.Height - top);
        LayoutOptions();
    }

    protected override void OnFormClosed(FormClosedEventArgs e)
    {
        refresh.Stop();
        player.Close();
        base.OnFormClosed(e);
    }

    // ---------- clips ----------

    async Task LoadClips()
    {
        List<Clip> list;
        try { list = await Api.Clips(); }
        catch (ApiException ex)
        {
            if (clips.Count == 0) grid.SetItems([], ex.Message, false);
            return;
        }
        var sig = string.Join("|", list.Select(c => c.Id + c.Mtime));
        if (sig == lastSig) return;
        var first = lastSig == "";
        lastSig = sig;
        // Info fetched since the last load (after a thumbnail) carries over
        var known = clips.Where(c => c.Info != null).ToDictionary(c => c.Id + c.Mtime, c => c.Info);
        foreach (var c in list) if (c.Info == null && known.TryGetValue(c.Id + c.Mtime, out var info)) c.Info = info;
        clips = list;
        if (chips.Build(clips, game) is var g && g != game) game = g;
        ShowClips(!first);
#if DEBUG
        if (first) TestHooks.Run(this, TestStep);
#endif
    }

#if DEBUG
    void Later(Action a, int ms = 1500)
    {
        var t = new System.Windows.Forms.Timer { Interval = ms };
        t.Tick += (_, _) => { t.Dispose(); a(); };
        t.Start();
    }

    void TestStep(string name, string arg)
    {
        var a = arg.Split(',');
        double F(int i) => a.Length > i ? double.Parse(a[i], CultureInfo.InvariantCulture) : 0;
        switch (name)
        {
            case "game": game = arg; chips.Mark(game); ShowClips(false); break;
            case "search": search.Box.Text = arg; break;
            case "page": grid.GoTo((int)F(0)); break;
            case "hover": Later(() => grid.TestHover((int)F(0), F(1))); break;
            case "play": Later(() => grid.TestPlay((int)F(0), F(1))); break;
            case "pause": Later(() => player.Toggle(), (int)(a[0] == "" ? 2500 : F(0))); break;
            case "close": Later(() => player.Close(), (int)F(0)); break;
            case "badge": header.Updated += _ => header.Options.Badge = (int)F(0); break;
            case "toast": toast.Show(arg); break;
            case "copy": Later(() => grid.TestPick((int)F(0), c => _ = Copy(c)), 2500); break;
            case "folder": Later(() => grid.TestPick((int)F(0), c => _ = Reveal(c)), 2500); break;
        }
    }
#endif

    List<Clip> Filtered()
    {
        var q = filter.Trim();
        return clips.Where(c => (game == "" || c.Game == game) &&
            (q == "" || c.Name.Contains(q, StringComparison.OrdinalIgnoreCase) || c.Game.Contains(q, StringComparison.OrdinalIgnoreCase))).ToList();
    }

    void ShowClips(bool keepPage)
    {
        var empty = clips.Count == 0 ? "No clips yet. Press Alt+F10 (or your save hotkey) to save the last 30 seconds." : "No clips match.";
        grid.SetItems(Filtered(), empty, keepPage);
    }

    async Task Save30()
    {
        if (!save30.Enabled) return;
        save30.Enabled = false;
        save30.Text = "Saving…";
        try
        {
            var r = await Api.Save(30);
            toast.Show($"Saved {r.Tracks.Count} audio tracks");
            await LoadClips();
        }
        catch (ApiException ex) { toast.Show(ex.Message, true); }
        save30.Text = "Save 30s";
        save30.Enabled = true;
    }

    async Task Copy(Clip c)
    {
        try { await Api.Copy(c.Id); toast.Show("Copied — paste it anywhere"); }
        catch (ApiException ex) { toast.Show(ex.Message, true); }
    }

    async Task Reveal(Clip c)
    {
        try { await Api.Reveal(c.Id); }
        catch (ApiException ex) { toast.Show(ex.Message, true); }
    }

    /// <summary>The web editor in the default browser (temporary, until the editor is native)</summary>
    static void Edit(Clip c, double t) =>
        Api.OpenPage($"/editor?id={Api.Enc(c.Id)}" + (t > 0 ? "&t=" + t.ToString("0.###", CultureInfo.InvariantCulture) : ""));

    void Play(Clip c, double frac)
    {
        var dir = header.Last?.ClipsDir;
        if (string.IsNullOrEmpty(dir)) { toast.Show("clipback isn't running", true); return; }
        var path = Path.Combine(dir, c.Id.Replace('/', Path.DirectorySeparatorChar));
        player.Open(c, path, frac);
    }

    void CardMenu(Clip c, Point at)
    {
        var menu = new DarkMenu { Font = Theme.Font(this, 14) };
        menu.Add("Play", () => Play(c, 0));
        menu.Add("Copy", () => _ = Copy(c));
        menu.Add("Edit", () => Edit(c, 0));
        menu.Add("Show in folder", () => _ = Reveal(c));
        menu.Closed += (_, _) => BeginInvoke(menu.Dispose);
        menu.Show(grid, at);
    }

    protected override bool ProcessCmdKey(ref Message msg, Keys keys)
    {
        if (OnOptions) return base.ProcessCmdKey(ref msg, keys);
        if (player.Visible)
        {
            switch (keys)
            {
                case Keys.Escape: player.Close(); return true;
                case Keys.Space: player.Toggle(); return true;
                case Keys.Left: player.Nudge(-5); return true;
                case Keys.Right: player.Nudge(5); return true;
            }
        }
        else
        {
            switch (keys)
            {
                case Keys.Control | Keys.F: search.Box.Focus(); search.Box.SelectAll(); return true;
                case Keys.PageDown: grid.GoTo(grid.Page + 1); return true;
                case Keys.PageUp: grid.GoTo(grid.Page - 1); return true;
                case Keys.Escape when search.Box.Focused && search.Box.Text != "": search.Box.Text = ""; return true;
            }
        }
        return base.ProcessCmdKey(ref msg, keys);
    }
}

/// <summary>
/// A chip per game: All, then each game (most recent first), then Desktop and Other, with counts and the game's
/// icon. Rebuilt only when the set of games changes; picking one only moves the highlight. If they don't all fit,
/// the wheel slides them sideways.
/// </summary>
sealed class ChipBar : Panel
{
    readonly List<Btn> chips = [];
    static readonly Dictionary<string, Image?> icons = new(StringComparer.OrdinalIgnoreCase);
    string sig = "";
    int offset;
    public event Action<string>? Picked;

    public ChipBar()
    {
        BackColor = Theme.Bg;
        DoubleBuffered = true;
    }

    /// <summary>Makes the chips for these clips; returns the game still picked ("" if it's gone)</summary>
    public string Build(List<Clip> clips, string picked)
    {
        var games = new Dictionary<string, (int n, string? exe)>();
        foreach (var c in clips)
        {
            games.TryGetValue(c.Game, out var g);
            games[c.Game] = (g.n + 1, g.exe ?? c.GameExe);
        }
        static int Last(string n) => n == "Other" ? 2 : n == "Desktop" ? 1 : 0;
        var names = games.Keys.OrderBy(Last).ToList(); // stable: most recent first within each group
        var newSig = clips.Count + "|" + string.Join("|", names.Select(n => n + games[n].n + games[n].exe));
        if (!games.ContainsKey(picked)) picked = "";
        if (newSig == sig) { Mark(picked); return picked; }
        sig = newSig;

        SuspendLayout();
        foreach (var c in chips) { Controls.Remove(c); c.Dispose(); }
        chips.Clear();
        Add("", "All", clips.Count, "layout-grid", null);
        foreach (var n in names)
        {
            var exe = games[n].exe;
            Add(n, n, games[n].n, n == "Desktop" ? "monitor" : n == "Other" ? "folder" : "gamepad-2", n is "Desktop" or "Other" ? null : exe);
        }
        offset = 0;
        ResumeLayout();
        PerformLayout();
        Mark(picked);
        return picked;
    }

    void Add(string value, string label, int count, string icon, string? exe)
    {
        var b = new Btn(label, icon, BtnKind.Chip) { Count = count.ToString(), FontPx = 13, Tag = value };
        b.Click += (_, _) => Picked?.Invoke(value);
        chips.Add(b);
        Controls.Add(b);
        if (exe != null) _ = SetIcon(b, exe);
    }

    static async Task SetIcon(Btn b, string exe)
    {
        if (!icons.TryGetValue(exe, out var img))
        {
            icons[exe] = null; // asked once per run
            var png = await Api.Bytes(Api.IconUrl(exe));
            if (png != null) try { img = Image.FromStream(new MemoryStream(png)); } catch { }
            icons[exe] = img;
        }
        // Same size as the stand-in icon, so the chip doesn't change width
        if (img != null && !b.IsDisposed) b.Picture = img;
    }

    public void Mark(string picked)
    {
        foreach (var c in chips) c.On = (string)c.Tag! == picked;
    }

    protected override void OnLayout(LayoutEventArgs e)
    {
        base.OnLayout(e);
        int x = this.Px(16) - offset, y = this.Px(10), h = this.Px(30), gap = this.Px(6);
        foreach (var c in chips)
        {
            var w = c.FitWidth;
            c.SetBounds(x, y, w, h);
            x += w + gap;
        }
    }

    int ContentWidth => chips.Count == 0 ? 0 : chips[^1].Right + offset + this.Px(16);

    protected override void OnMouseWheel(MouseEventArgs e)
    {
        var max = Math.Max(0, ContentWidth - Width);
        offset = Math.Clamp(offset - e.Delta / 2, 0, max);
        PerformLayout();
    }
}

/// <summary>"75 clips" on the left, "‹ Page 1 of 4 ›" on the right; fixed places, buttons greyed at the ends</summary>
sealed class Pager : Painted
{
    public readonly Btn Prev = new("", "chevron-left"), Next = new("", "chevron-right");
    string left = "", middle = "";

    public Pager()
    {
        BackColor = Theme.Bg;
        Controls.AddRange([Prev, Next]);
    }

    public void Set(int page, int pages, int count)
    {
        left = count == 1 ? "1 clip" : $"{count} clips";
        middle = $"Page {page + 1} of {pages}";
        Prev.Enabled = page > 0;
        Next.Enabled = page < pages - 1;
        Invalidate();
    }

    int LabelWidth => TextRenderer.MeasureText("Page 888 of 888", Theme.Font(this, 13)).Width;

    protected override void OnLayout(LayoutEventArgs e)
    {
        base.OnLayout(e);
        int s = this.Px(34), h = this.Px(28), y = (Height - h) / 2, pad = this.Px(16);
        Next.SetBounds(Width - pad - s, y, s, h);
        Prev.SetBounds(Next.Left - this.Px(8) - LabelWidth - this.Px(8) - s, y, s, h);
    }

    protected override void OnPaint(PaintEventArgs e)
    {
        var g = e.Graphics;
        g.Clear(Theme.Bg);
        using (var line = new Pen(Theme.Line)) g.DrawLine(line, 0, 0, Width, 0);
        var font = Theme.Font(this, 13);
        TextRenderer.DrawText(g, left, font, new Rectangle(this.Px(16), 0, this.Px(200), Height), Theme.Dim, Theme.OneLine);
        TextRenderer.DrawText(g, middle, font, new Rectangle(Prev.Right + this.Px(8), 0, LabelWidth, Height), Theme.Dim, Theme.OneLine | TextFormatFlags.HorizontalCenter);
    }
}
