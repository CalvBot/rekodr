using System.Drawing.Drawing2D;

namespace Clipback;

// The form controls of the Options screen, drawn like the web page's (ui/style.css). Each has a size set by the
// screen and never changes it itself, so nothing moves when it's clicked.

static class Blur
{
    /// <summary>Takes the focus off any text box, like a click on the page does in a browser (so a bitrate box
    /// "changes" when you click elsewhere)</summary>
    public static void Now(Control c)
    {
        if (c.FindForm() is not { ActiveControl: TextBox or Field }) return;
        var p = c;
        while (p != null && p is not OptionsView) p = p.Parent;
        if (p is OptionsView o) o.FocusHome();
    }
}

/// <summary>Painted text placed by the screen's layout: labels, units, hints</summary>
sealed record TextRun(Rectangle Rect, string Text, float Px, Color Color, bool Bold = false, Func<bool>? Off = null, bool Wrap = false);

/// <summary>A section of the page ("section.box"): rounded panel, icon + heading, help text, and the labels and hints
/// the layout adds as text runs. Its controls are its children, so they sit on the panel colour.</summary>
sealed class Box : Painted
{
    public string Title { get; }
    public string Icon { get; }
    public string? Help { get; set; }
    public readonly List<TextRun> Runs = [];

    public Box(string title, string icon, string? help = null)
    {
        Title = title;
        Icon = icon;
        Help = help;
        BackColor = Theme.Panel;
    }

    public int PadX => this.Px(16);
    public int PadY => this.Px(14);

    /// <summary>Where the content starts below the heading and help, for this width</summary>
    public int ContentTop(int width)
    {
        var y = PadY + this.Px(22) + this.Px(4);
        if (Help != null)
            y += TextRenderer.MeasureText(Help, Theme.Font(this, 13), new Size(width - 2 * PadX, int.MaxValue), TextFormatFlags.WordBreak | TextFormatFlags.NoPrefix).Height + this.Px(10);
        else y += this.Px(6);
        return y;
    }

    protected override void OnMouseDown(MouseEventArgs e) { Blur.Now(this); base.OnMouseDown(e); }

    protected override void OnPaint(PaintEventArgs e)
    {
        var g = e.Graphics;
        g.Clear(Parent?.BackColor ?? Theme.Bg);
        Theme.FillRound(g, new RectangleF(0.5f, 0.5f, Width - 1.5f, Height - 1.5f), this.Pxf(10), Theme.Panel, Theme.Line);
        var ix = PadX;
        var iy = PadY + this.Px(2);
        Icons.Draw(g, Icon, new RectangleF(ix, iy, this.Pxf(18), this.Pxf(18)), Theme.Dim);
        TextRenderer.DrawText(g, Title, Theme.Font(this, 15, FontStyle.Bold), new Rectangle(ix + this.Px(26), PadY, Width, this.Px(22)), Theme.Text, Theme.OneLine);
        if (Help != null)
        {
            var w = Width - 2 * PadX;
            var font = Theme.Font(this, 13);
            var h = TextRenderer.MeasureText(Help, font, new Size(w, int.MaxValue), TextFormatFlags.WordBreak | TextFormatFlags.NoPrefix).Height;
            TextRenderer.DrawText(g, Help, font, new Rectangle(PadX, PadY + this.Px(26), w, h), Theme.Dim, TextFormatFlags.WordBreak | TextFormatFlags.NoPrefix);
        }
        foreach (var r in Runs)
        {
            var color = r.Off?.Invoke() == true ? Theme.Blend(Theme.Panel, r.Color, 0.45) : r.Color;
            var flags = r.Wrap ? TextFormatFlags.WordBreak | TextFormatFlags.NoPrefix : Theme.Ellipsis;
            TextRenderer.DrawText(g, r.Text, Theme.Font(this, r.Px, r.Bold ? FontStyle.Bold : FontStyle.Regular), r.Rect, color, flags);
        }
    }

    protected override void Dispose(bool disposing)
    {
        base.Dispose(disposing);
    }
}

/// <summary>A text box in a rounded frame (input[type=text|number]). Not Active = greyed and read-only, but it keeps
/// its place and its value.</summary>
sealed class Field : Panel
{
    public readonly TextBox Box = new() { BorderStyle = BorderStyle.None, BackColor = Theme.Panel2, ForeColor = Theme.Text };
    bool focused, active = true;
    /// <summary>Only digits and a decimal point can be typed</summary>
    public bool Numeric { get; init; }
    public float FontPx { get; init; } = 14;
    public int PadX96 { get; init; } = 8;
    /// <summary>Like the web's "change": the text was edited and the box lost focus, or Enter was pressed</summary>
    public event Action? Committed;
    string atFocus = "";

    public Field(string placeholder = "")
    {
        SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw, true);
        BackColor = Theme.Panel2;
        Box.PlaceholderText = placeholder;
        Box.GotFocus += (_, _) => { focused = true; atFocus = Box.Text; Invalidate(); };
        Box.LostFocus += (_, _) => { focused = false; Invalidate(); Commit(); };
        Box.KeyPress += (_, e) =>
        {
            if (e.KeyChar == '\r') { e.Handled = true; Commit(); return; }
            if (Numeric && !char.IsControl(e.KeyChar) && !char.IsDigit(e.KeyChar) && e.KeyChar != '.') e.Handled = true;
        };
        Controls.Add(Box);
        Cursor = Cursors.IBeam;
        Click += (_, _) => { if (active) Box.Focus(); };
    }

    void Commit()
    {
        if (Box.Text == atFocus) return;
        atFocus = Box.Text;
        Committed?.Invoke();
    }

    [System.Diagnostics.CodeAnalysis.AllowNull] public override string Text { get => Box.Text; set { Box.Text = value ?? ""; if (!focused) atFocus = Box.Text; } }

    public bool Active
    {
        get => active;
        set
        {
            if (active == value) return;
            active = value;
            Box.ReadOnly = !value;
            Box.TabStop = value;
            var fill = value ? Theme.Panel2 : Theme.Blend(Theme.Panel, Theme.Panel2, 0.45);
            Box.BackColor = BackColor = fill;
            Box.ForeColor = value ? Theme.Text : Theme.Blend(Theme.Panel, Theme.Text, 0.45);
            Cursor = value ? Cursors.IBeam : Cursors.Default;
            Box.Cursor = Cursor;
            if (!value && Box.Focused) Blur.Now(this);
            Invalidate();
        }
    }

    protected override void OnLayout(LayoutEventArgs levent)
    {
        base.OnLayout(levent);
        Box.Font = Theme.Font(this, FontPx);
        var pad = this.Px(PadX96);
        var h = Box.PreferredHeight;
        Box.SetBounds(pad, (Height - h) / 2, Width - 2 * pad, h);
    }

    protected override void OnPaint(PaintEventArgs e)
    {
        e.Graphics.Clear(Parent?.BackColor ?? Theme.Bg);
        var line = focused && active ? Theme.Accent2 : active ? Theme.Line : Theme.Blend(Theme.Panel, Theme.Line, 0.45);
        Theme.FillRound(e.Graphics, new RectangleF(0.5f, 0.5f, Width - 1.5f, Height - 1.5f), this.Pxf(7), BackColor, line);
    }
}

/// <summary>A tick box (input[type=checkbox])</summary>
sealed class Check : Painted
{
    bool value, hover;
    public bool Checked { get => value; set { this.value = value; Invalidate(); } }
    public event Action? Changed;

    public Check() => Cursor = Cursors.Hand;

    protected override void OnMouseEnter(EventArgs e) { hover = true; Invalidate(); base.OnMouseEnter(e); }
    protected override void OnMouseLeave(EventArgs e) { hover = false; Invalidate(); base.OnMouseLeave(e); }
    protected override void OnMouseDown(MouseEventArgs e) { Blur.Now(this); base.OnMouseDown(e); }
    protected override void OnEnabledChanged(EventArgs e) { base.OnEnabledChanged(e); Cursor = Enabled ? Cursors.Hand : Cursors.Default; Invalidate(); }

    protected override void OnClick(EventArgs e)
    {
        base.OnClick(e);
        value = !value;
        Invalidate();
        Changed?.Invoke();
    }

    protected override void OnPaint(PaintEventArgs e)
    {
        var g = e.Graphics;
        g.Clear(Parent?.BackColor ?? Theme.Bg);
        var s = this.Pxf(16);
        var r = new RectangleF(0.5f, (Height - s) / 2, s, s);
        Color fill = value ? Theme.Accent2 : hover ? Theme.Hover : Theme.Panel2, line = value ? Theme.Accent2 : hover ? Theme.HoverLine : Theme.Dim, ink = Color.White;
        if (!Enabled) { fill = Theme.Blend(Theme.Panel, fill, 0.45); line = Theme.Blend(Theme.Panel, line, 0.45); ink = Theme.Blend(Theme.Panel, ink, 0.45); }
        Theme.FillRound(g, r, this.Pxf(3), fill, line);
        if (value)
        {
            using var pen = new Pen(ink, this.Pxf(2)) { StartCap = LineCap.Round, EndCap = LineCap.Round, LineJoin = LineJoin.Round };
            g.DrawLines(pen, [new PointF(r.X + s * 0.22f, r.Y + s * 0.52f), new PointF(r.X + s * 0.42f, r.Y + s * 0.72f), new PointF(r.X + s * 0.78f, r.Y + s * 0.3f)]);
        }
    }
}

/// <summary>A drop-down (select): the chosen label and a chevron; a click opens the choices as a menu under it</summary>
sealed class Choice : Painted
{
    bool hover, open;
    string value = "";
    public readonly List<(string value, string label)> Items = [];
    public string Value { get => value; set { this.value = value; Invalidate(); } }
    public event Action? Changed;

    public Choice() => Cursor = Cursors.Hand;

    public void SetItems(IEnumerable<(string value, string label)> items)
    {
        Items.Clear();
        Items.AddRange(items);
        Invalidate();
    }

    public void SetLabel(int i, string label)
    {
        Items[i] = (Items[i].value, label);
        Invalidate();
    }

    string Label => Items.FirstOrDefault(i => i.value == value).label ?? value;

    protected override void OnMouseEnter(EventArgs e) { hover = true; Invalidate(); base.OnMouseEnter(e); }
    protected override void OnMouseLeave(EventArgs e) { hover = false; Invalidate(); base.OnMouseLeave(e); }
    protected override void OnEnabledChanged(EventArgs e) { base.OnEnabledChanged(e); Cursor = Enabled ? Cursors.Hand : Cursors.Default; Invalidate(); }

    protected override void OnMouseDown(MouseEventArgs e)
    {
        base.OnMouseDown(e);
        Blur.Now(this);
        if (e.Button == MouseButtons.Left) Open();
    }

    /// <summary>Shows the choices under the box</summary>
    public void Open()
    {
        if (Items.Count == 0) return;
        var menu = new DarkMenu { Font = Theme.Font(this, 14), MinimumSize = new Size(Width, 0) };
        foreach (var (v, label) in Items)
        {
            var pick = v;
            menu.Add(label, () => { if (pick == value) return; Value = pick; Changed?.Invoke(); });
            if (v == value) menu.Items[^1].Font = Theme.Font(this, 14, FontStyle.Bold);
        }
        menu.Closed += (_, _) => { open = false; Invalidate(); BeginInvoke(menu.Dispose); };
        open = true;
        Invalidate();
        menu.Show(this, new Point(0, Height + this.Px(2)));
    }

    protected override void OnPaint(PaintEventArgs e)
    {
        var g = e.Graphics;
        g.Clear(Parent?.BackColor ?? Theme.Bg);
        Color fill = hover && Enabled ? Theme.Hover : Theme.Panel2, line = open ? Theme.Accent2 : hover && Enabled ? Theme.HoverLine : Theme.Line, ink = Theme.Text;
        if (!Enabled) { fill = Theme.Blend(Theme.Panel, fill, 0.45); line = Theme.Blend(Theme.Panel, line, 0.45); ink = Theme.Blend(Theme.Panel, ink, 0.45); }
        Theme.FillRound(g, new RectangleF(0.5f, 0.5f, Width - 1.5f, Height - 1.5f), this.Pxf(7), fill, line);
        var pad = this.Px(9);
        var chev = this.Pxf(14);
        TextRenderer.DrawText(g, Label, Theme.Font(this, 14), new Rectangle(pad, 0, Width - 2 * pad - (int)chev - this.Px(4), Height), ink, Theme.Ellipsis);
        Icons.Draw(g, "chevron-down", new RectangleF(Width - pad - chev, (Height - chev) / 2, chev, chev), Theme.Blend(Theme.Panel, ink, 0.75));
    }
}

/// <summary>The category buttons of an app row (".seg"): Communication / Game / Own track / Ignore. The lit one has
/// the category's colour; none is lit for a new app.</summary>
sealed class Segments : Painted
{
    public static readonly (string cat, string label, Color on)[] Cats =
    [
        ("communication", "Communication", Color.FromArgb(0x8a, 0x5c, 0xf6)),
        ("game", "Game", Theme.Ok),
        ("app", "Own track", Theme.Accent2),
        ("ignored", "Ignore", Color.FromArgb(0x55, 0x5b, 0x66)),
    ];
    int lit = -1, hover = -1;
    public event Action<string>? Picked;

    public Segments() => Cursor = Cursors.Hand;

    /// <summary>The category to light, or null for none</summary>
    public string? Lit
    {
        set { var i = Array.FindIndex(Cats, c => c.cat == value); if (i != lit) { lit = i; Invalidate(); } }
    }

    int SegWidth(int i) => TextRenderer.MeasureText(Cats[i].label, Theme.Font(this, 12), Size.Empty, TextFormatFlags.NoPrefix).Width + this.Px(18);
    public int FitWidth => Enumerable.Range(0, Cats.Length).Sum(SegWidth) + this.Px(2);

    int At(int x)
    {
        var left = this.Px(1);
        for (var i = 0; i < Cats.Length; i++)
        {
            var w = SegWidth(i);
            if (x >= left && x < left + w) return i;
            left += w;
        }
        return -1;
    }

    protected override void OnMouseMove(MouseEventArgs e) { var h = At(e.X); if (h != hover) { hover = h; Invalidate(); } base.OnMouseMove(e); }
    protected override void OnMouseLeave(EventArgs e) { hover = -1; Invalidate(); base.OnMouseLeave(e); }
    protected override void OnMouseDown(MouseEventArgs e) { Blur.Now(this); base.OnMouseDown(e); }

    protected override void OnMouseClick(MouseEventArgs e)
    {
        base.OnMouseClick(e);
        if (e.Button == MouseButtons.Left && At(e.X) is >= 0 and var i) Picked?.Invoke(Cats[i].cat);
    }

    protected override void OnPaint(PaintEventArgs e)
    {
        var g = e.Graphics;
        g.Clear(Parent?.BackColor ?? Theme.Bg);
        var outer = new RectangleF(0.5f, 0.5f, Width - 1.5f, Height - 1.5f);
        var radius = this.Pxf(7);
        using var path = Theme.Round(outer, radius);
        var state = g.Save();
        g.SetClip(path);
        float x = this.Px(1);
        var font = Theme.Font(this, 12);
        using var sep = new Pen(Theme.Line);
        for (var i = 0; i < Cats.Length; i++)
        {
            var w = SegWidth(i);
            var r = new RectangleF(x, 0, w, Height);
            if (i == lit) using (var b = new SolidBrush(Cats[i].on)) g.FillRectangle(b, r);
            else if (i == hover) using (var b = new SolidBrush(Theme.Hover)) g.FillRectangle(b, r);
            if (i > 0) g.DrawLine(sep, x, 0, x, Height);
            TextRenderer.DrawText(g, Cats[i].label, font, Rectangle.Round(r), i == lit ? Color.White : Theme.Text, Theme.OneLine | TextFormatFlags.HorizontalCenter);
            x += w;
        }
        g.Restore(state);
        g.SmoothingMode = SmoothingMode.AntiAlias;
        using var border = new Pen(Theme.Line);
        g.DrawPath(border, path);
    }
}

/// <summary>A borderless text button with an icon ("button.linkish"): dim, lighter on hover</summary>
sealed class LinkBtn : Painted
{
    bool hover;
    string icon;
    public string Icon { get => icon; set { icon = value; Invalidate(); } }

    public LinkBtn(string text, string icon)
    {
        Text = text;
        this.icon = icon;
        Cursor = Cursors.Hand;
    }

    protected override void OnTextChanged(EventArgs e) { base.OnTextChanged(e); Invalidate(); }
    protected override void OnMouseEnter(EventArgs e) { hover = true; Invalidate(); base.OnMouseEnter(e); }
    protected override void OnMouseLeave(EventArgs e) { hover = false; Invalidate(); base.OnMouseLeave(e); }
    protected override void OnMouseDown(MouseEventArgs e) { Blur.Now(this); base.OnMouseDown(e); }

    protected override void OnPaint(PaintEventArgs e)
    {
        var g = e.Graphics;
        g.Clear(Parent?.BackColor ?? Theme.Bg);
        var ink = hover ? Theme.Text : Theme.Dim;
        var s = this.Pxf(16);
        Icons.Draw(g, icon, new RectangleF(0, (Height - s) / 2, s, s), ink);
        TextRenderer.DrawText(g, Text, Theme.Font(this, 13), new Rectangle(this.Px(22), 0, Width - this.Px(22), Height), ink, Theme.OneLine);
    }
}
