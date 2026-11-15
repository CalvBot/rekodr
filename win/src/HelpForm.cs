namespace Clipback;

/// <summary>
/// Keyboard shortcuts and other tips, in one place (Help in the header, or F1). The window has no tooltips or
/// inline key hints by design: anything worth knowing that isn't on a button goes here.
/// </summary>
sealed class HelpForm : DarkForm
{
    record Section(string Title, (string keys, string what)[] Rows);

    readonly List<Section> sections =
    [
        new("Clips", [
            ("Ctrl+F", "Search"),
            ("Page Up / Page Down", "Previous / next page"),
            ("Right-click a clip", "More actions"),
        ]),
        new("Player", [
            ("Space", "Play / pause"),
            ("← / →", "Back / forward 5 s"),
            ("Esc", "Close the player"),
        ]),
        new("Editor", [
            ("Space", "Play / pause"),
            ("I / O", "Trim start / end at the playhead"),
            ("K", "Add a zoom keyframe at the playhead"),
            ("Delete", "Delete the keyframe at the playhead"),
            ("← / →", "Step one frame"),
            ("Shift+← / →", "Step 1 s"),
            ("Drag the video", "Aim the zoom"),
            ("Double-click a volume", "Back to 100%"),
        ]),
        new("Options", [
            ("Right-click an app", "Open its file location"),
        ]),
        new("Anywhere, while clipback runs", [
            ("clipbackw save 90s", "Save the last 90 s (or 30s, 2m, 1h…); for Stream Deck"),
            ("clipbackw open", "Open this window"),
            ("clipbackw pause", "Pause / resume recording"),
        ]),
        new("Help", [("F1", "This window")]),
    ];

    public HelpForm()
    {
        Text = "clipback help";
        FormBorderStyle = FormBorderStyle.FixedSingle;
        MaximizeBox = MinimizeBox = false;
        ShowInTaskbar = false;
        StartPosition = FormStartPosition.CenterParent;
    }

    /// <summary>Adds the hotkeys from Options to the top ("Anywhere") section</summary>
    public async Task LoadHotkeys()
    {
        try
        {
            var cfg = await Api.Settings();
            var keys = cfg["config"]?["hotkeys"]?.AsArray()
                .Select(h => (keys: (string?)h?["keys"] ?? "", what: Action((string?)h?["action"] ?? "")))
                .Where(h => h.keys != "").ToArray() ?? [];
            if (keys.Length == 0) return;
            var i = sections.FindIndex(s => s.Title.StartsWith("Anywhere"));
            sections[i] = sections[i] with { Rows = [.. keys, .. sections[i].Rows] };
            Fit();
            Invalidate();
        }
        catch { } // clipback not answering: the fixed list is still right
    }

    static string Action(string a) => a.Split(' ', 2) switch
    {
        ["save", var len] => $"Save the last {len}",
        ["open"] => "Open this window",
        ["pause"] => "Pause / resume recording",
        _ => a,
    };

    const int Pad = 24, Row = 26, Head = 34, KeysW = 190, Width96 = 620;

    void Fit()
    {
        var rows = sections.Sum(s => s.Rows.Length);
        ClientSize = new Size(this.Px(Width96), this.Px(Pad * 2 + sections.Count * Head + rows * Row));
    }

    protected override void OnLoad(EventArgs e)
    {
        base.OnLoad(e);
        Fit();
        _ = LoadHotkeys();
    }

    protected override bool ProcessCmdKey(ref Message msg, Keys keys)
    {
        if (keys is Keys.Escape or Keys.F1) { Close(); return true; }
        return base.ProcessCmdKey(ref msg, keys);
    }

    protected override void OnPaint(PaintEventArgs e)
    {
        var g = e.Graphics;
        g.Clear(Theme.Bg);
        var y = this.Px(Pad);
        int x = this.Px(Pad), kw = this.Px(KeysW), w = ClientSize.Width - x * 2;
        foreach (var s in sections)
        {
            var head = new Rectangle(x, y + this.Px(8), w, this.Px(Head - 8));
            TextRenderer.DrawText(g, s.Title.ToUpperInvariant(), Theme.Font(this, 11.5f, FontStyle.Bold), head, Theme.Dim, Theme.OneLine);
            y += this.Px(Head);
            foreach (var (keys, what) in s.Rows)
            {
                // The keys as a pill, then what they do
                var font = Theme.Font(this, 13);
                var kSize = TextRenderer.MeasureText(keys, font);
                var pill = new RectangleF(x, y + this.Px(3), Math.Min(kSize.Width + this.Px(10), kw - this.Px(8)), this.Px(Row - 6));
                Theme.FillRound(g, pill, this.Pxf(5), Theme.Panel2, Theme.Line);
                TextRenderer.DrawText(g, keys, font, Rectangle.Round(pill), Theme.Text, Theme.Ellipsis | TextFormatFlags.HorizontalCenter);
                TextRenderer.DrawText(g, what, font, new Rectangle(x + kw, y, w - kw, this.Px(Row)), Theme.Text, Theme.Ellipsis);
                y += this.Px(Row);
            }
        }
    }
}
