using System.Drawing.Drawing2D;
using System.Runtime.InteropServices;

namespace Clipback;

enum BtnKind { Normal, Primary, Chip }

/// <summary>
/// A button like the web UI's: rounded, icon then text. Its size is set by the window and never follows the
/// label, so changing the label (Pause ⇄ Resume) doesn't move anything. A badge count can sit over the top-right
/// corner; the control keeps room for it all the time (Inset) so it doesn't grow when one appears.
/// </summary>
sealed class Btn : Painted
{
    bool hover, down, on;
    string? icon;
    int badge;
    string? count;
    Image? picture;

    public BtnKind Kind { get; init; }
    public string? Icon { get => icon; set { icon = value; Invalidate(); } }
    /// <summary>A picture in place of the icon (a game's own icon)</summary>
    public Image? Picture { get => picture; set { picture = value; Invalidate(); } }
    public bool On { get => on; set { on = value; Invalidate(); } }
    public int Badge { get => badge; set { badge = value; Invalidate(); } }
    /// <summary>Dim number after the label (chips)</summary>
    public string? Count { get => count; set { count = value; Invalidate(); } }
    /// <summary>Space kept free around the drawn button for the badge, in 96-dpi px</summary>
    public int Inset { get; init; }
    public float FontPx { get; init; } = 14;
    /// <summary>Every label the button can show: its width fits the widest, so it never changes size</summary>
    public string[]? Labels { get; init; }
    /// <summary>The fixed width for this button's labels</summary>
    public int FitWidth => WidthFor(Labels ?? [Text]);

    public Btn(string text, string? icon = null, BtnKind kind = BtnKind.Normal)
    {
        Text = text;
        this.icon = icon;
        Kind = kind;
        Cursor = Cursors.Hand;
    }

    /// <summary>The width that fits the widest of these labels (with this button's icon), in device px</summary>
    public int WidthFor(params string[] labels)
    {
        var font = Theme.Font(this, FontPx);
        var text = labels.Max(l => TextRenderer.MeasureText(l, font, Size.Empty, TextFormatFlags.NoPrefix | TextFormatFlags.SingleLine).Width);
        var iconW = icon != null || picture != null ? this.Px(16 + 6) : 0;
        var countW = count != null ? this.Px(6) + TextRenderer.MeasureText(count, Theme.Font(this, 11), Size.Empty, TextFormatFlags.NoPrefix).Width : 0;
        var pad = Kind == BtnKind.Chip ? 20 : 22;
        return text + iconW + countW + this.Px(pad) + 2 * this.Px(Inset);
    }

    protected override void OnTextChanged(EventArgs e) { base.OnTextChanged(e); Invalidate(); }
    protected override void OnEnabledChanged(EventArgs e) { base.OnEnabledChanged(e); Cursor = Enabled ? Cursors.Hand : Cursors.Default; Invalidate(); }
    protected override void OnMouseEnter(EventArgs e) { hover = true; Invalidate(); base.OnMouseEnter(e); }
    protected override void OnMouseLeave(EventArgs e) { hover = down = false; Invalidate(); base.OnMouseLeave(e); }
    protected override void OnMouseDown(MouseEventArgs e) { if (e.Button == MouseButtons.Left) { down = true; Invalidate(); } base.OnMouseDown(e); }
    protected override void OnMouseUp(MouseEventArgs e) { down = false; Invalidate(); base.OnMouseUp(e); }

    protected override void OnPaint(PaintEventArgs e)
    {
        var g = e.Graphics;
        g.Clear(Parent?.BackColor ?? Theme.Bg);
        var inset = this.Pxf(Inset);
        var r = new RectangleF(inset + 0.5f, inset + 0.5f, Width - 2 * inset - 1, Height - 2 * inset - 1);
        if (down) r.Offset(0, 1);
        Color fill, line, ink = Theme.Text;
        if (Kind == BtnKind.Primary || on)
        {
            fill = line = hover && Enabled && !on ? Theme.PrimaryHover : Theme.Accent2;
            ink = Color.White;
        }
        else
        {
            fill = hover && Enabled ? Theme.Hover : Theme.Panel2;
            line = hover && Enabled ? Theme.HoverLine : Theme.Line;
        }
        if (!Enabled) { fill = Theme.Blend(Theme.Bg, fill, 0.5); line = Theme.Blend(Theme.Bg, line, 0.5); ink = Theme.Blend(Theme.Bg, ink, 0.5); }
        Theme.FillRound(g, r, Kind == BtnKind.Chip ? r.Height / 2 : this.Pxf(7), fill, line);

        // Icon + label + count, centred as one group
        var font = Theme.Font(this, FontPx);
        var flags = TextFormatFlags.NoPrefix | TextFormatFlags.SingleLine;
        var textW = Text.Length > 0 ? TextRenderer.MeasureText(g, Text, font, Size.Empty, flags).Width : 0;
        var small = Theme.Font(this, 11);
        var countW = count != null ? TextRenderer.MeasureText(g, count, small, Size.Empty, flags).Width : 0;
        var iconS = this.Pxf(Kind == BtnKind.Chip ? 15 : 16);
        var hasIcon = icon != null || picture != null;
        var gap = this.Pxf(6);
        var total = (hasIcon ? iconS + (textW > 0 ? gap : 0) : 0) + textW + (countW > 0 ? gap + countW : 0);
        var x = r.X + (r.Width - total) / 2;
        var cy = r.Y + r.Height / 2;
        if (hasIcon)
        {
            var box = new RectangleF(x, cy - iconS / 2, iconS, iconS);
            if (picture != null)
            {
                g.InterpolationMode = InterpolationMode.HighQualityBicubic;
                g.DrawImage(picture, box);
            }
            else Icons.Draw(g, icon!, box, ink);
            x += iconS + (textW > 0 ? gap : 0);
        }
        var textBox = new Rectangle((int)x, (int)r.Y, textW + 2, (int)r.Height);
        if (textW > 0) TextRenderer.DrawText(g, Text, font, textBox, ink, flags | TextFormatFlags.VerticalCenter | TextFormatFlags.Left);
        if (countW > 0)
            TextRenderer.DrawText(g, count, small, new Rectangle((int)(x + textW + gap), (int)r.Y, countW + 2, (int)r.Height),
                on ? Color.FromArgb(220, 230, 240) : Theme.Dim, flags | TextFormatFlags.VerticalCenter | TextFormatFlags.Left);

        if (badge > 0)
        {
            var label = badge > 99 ? "99+" : badge.ToString();
            var bf = Theme.Font(this, 11, FontStyle.Bold);
            var h = this.Pxf(18);
            var w = Math.Max(h, TextRenderer.MeasureText(g, label, bf, Size.Empty, flags).Width + this.Pxf(8));
            var br = new RectangleF(Width - w, 0, w, h);
            Theme.FillRound(g, br, h / 2, Theme.Badge);
            TextRenderer.DrawText(g, label, bf, Rectangle.Round(br), Color.White, flags | TextFormatFlags.VerticalCenter | TextFormatFlags.HorizontalCenter);
        }
    }
}

/// <summary>A text box in a rounded frame, with placeholder text</summary>
sealed class SearchBox : Panel
{
    public readonly TextBox Box = new() { BorderStyle = BorderStyle.None, BackColor = Theme.Panel2, ForeColor = Theme.Text };
    bool focused;

    public SearchBox(string placeholder)
    {
        SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw, true);
        BackColor = Theme.Panel2;
        Box.PlaceholderText = placeholder;
        Box.GotFocus += (_, _) => { focused = true; Invalidate(); };
        Box.LostFocus += (_, _) => { focused = false; Invalidate(); };
        Controls.Add(Box);
        Cursor = Cursors.IBeam;
        Click += (_, _) => Box.Focus();
    }

    protected override void OnLayout(LayoutEventArgs levent)
    {
        base.OnLayout(levent);
        Box.Font = Theme.Font(this, 14);
        var pad = this.Px(9);
        var h = Box.PreferredHeight;
        Box.SetBounds(pad, (Height - h) / 2, Width - 2 * pad, h);
    }

    protected override void OnPaint(PaintEventArgs e)
    {
        e.Graphics.Clear(Parent?.BackColor ?? Theme.Bg);
        Theme.FillRound(e.Graphics, new RectangleF(0.5f, 0.5f, Width - 1.5f, Height - 1.5f), this.Pxf(7), Theme.Panel2, focused ? Theme.Accent2 : Theme.Line);
    }
}

/// <summary>Seek bar: a thin track with the played part in red; click or drag to seek</summary>
sealed class SeekBar : Painted
{
    double value;
    bool dragging, hover;
    public double Value { get => value; set { if (!dragging) { this.value = Math.Clamp(value, 0, 1); Invalidate(); } } }
    /// <summary>The user moved it (0–1); fired while dragging too</summary>
    public event Action<double>? Seek;

    public SeekBar() => Cursor = Cursors.Hand;

    void At(int x)
    {
        value = Math.Clamp((x - this.Pxf(6)) / Math.Max(1, Width - this.Pxf(12)), 0, 1);
        Invalidate();
        Seek?.Invoke(value);
    }

    protected override void OnMouseDown(MouseEventArgs e) { if (e.Button == MouseButtons.Left) { dragging = true; Capture = true; At(e.X); } }
    protected override void OnMouseMove(MouseEventArgs e) { if (dragging) At(e.X); }
    protected override void OnMouseUp(MouseEventArgs e) { if (dragging) { At(e.X); dragging = false; Capture = false; } }
    protected override void OnMouseEnter(EventArgs e) { hover = true; Invalidate(); }
    protected override void OnMouseLeave(EventArgs e) { hover = false; Invalidate(); }

    protected override void OnPaint(PaintEventArgs e)
    {
        var g = e.Graphics;
        g.Clear(Parent?.BackColor ?? Theme.Bg);
        float pad = this.Pxf(6), h = this.Pxf(hover || dragging ? 6 : 4), y = (Height - h) / 2, w = Width - 2 * pad;
        Theme.FillRound(g, new RectangleF(pad, y, w, h), h / 2, Theme.Line);
        if (value > 0) Theme.FillRound(g, new RectangleF(pad, y, Math.Max(h, (float)(w * value)), h), h / 2, Theme.Accent);
        var knob = this.Pxf(12);
        using var b = new SolidBrush(Color.White);
        g.FillEllipse(b, pad + (float)(w * value) - knob / 2, Height / 2f - knob / 2, knob, knob);
    }
}

/// <summary>A short message at the bottom of the window ("Copied — paste it anywhere")</summary>
sealed class Toast : Painted
{
    readonly System.Windows.Forms.Timer timer = new();
    bool error;

    public Toast()
    {
        Visible = false;
        timer.Tick += (_, _) => { timer.Stop(); Visible = false; };
    }

    public void Show(string message, bool err = false)
    {
        Text = message;
        error = err;
        var font = Theme.Font(this, 14);
        var size = TextRenderer.MeasureText(message, font, Size.Empty, TextFormatFlags.NoPrefix | TextFormatFlags.SingleLine);
        var w = Math.Min(Parent!.ClientSize.Width - this.Px(40), size.Width + this.Px(28));
        var h = this.Px(36);
        SetBounds((Parent.ClientSize.Width - w) / 2, Parent.ClientSize.Height - h - this.Px(22), w, h);
        Visible = true;
        BringToFront();
        Invalidate();
        timer.Stop();
        timer.Interval = err ? 5000 : 2200;
        timer.Start();
    }

    protected override void OnPaint(PaintEventArgs e)
    {
        var g = e.Graphics;
        g.Clear(Theme.Bg);
        Theme.FillRound(g, new RectangleF(0.5f, 0.5f, Width - 1.5f, Height - 1.5f), this.Pxf(8), Theme.Hover, error ? Theme.Accent : Theme.Line);
        TextRenderer.DrawText(g, Text, Theme.Font(this, 14), ClientRectangle, Theme.Text, Theme.Ellipsis | TextFormatFlags.HorizontalCenter);
    }
}

/// <summary>Right-click menu in the window's colours</summary>
sealed class DarkMenu : ContextMenuStrip
{
    public DarkMenu()
    {
        Renderer = new Painter();
        BackColor = Theme.Panel2;
        ForeColor = Theme.Text;
        ShowImageMargin = false;
        Padding = new Padding(4);
    }

    [DllImport("dwmapi.dll")]
    static extern int DwmSetWindowAttribute(IntPtr hwnd, int attr, ref int value, int size);

    protected override void OnHandleCreated(EventArgs e)
    {
        base.OnHandleCreated(e);
        var round = 2; // DWMWCP_ROUND on Windows 11
        DwmSetWindowAttribute(Handle, 33, ref round, 4);
    }

    public void Add(string text, Action onClick)
    {
        var item = new ToolStripMenuItem(text) { ForeColor = Theme.Text, Padding = new Padding(6, 5, 6, 5) };
        item.Click += (_, _) => onClick();
        Items.Add(item);
    }

    sealed class Painter : ToolStripProfessionalRenderer
    {
        public Painter() : base(new Colors()) => RoundedEdges = false;
        protected override void OnRenderToolStripBorder(ToolStripRenderEventArgs e) { }
        protected override void OnRenderMenuItemBackground(ToolStripItemRenderEventArgs e)
        {
            if (!e.Item.Selected || !e.Item.Enabled) return;
            Theme.FillRound(e.Graphics, new RectangleF(0.5f, 0.5f, e.Item.Width - 1.5f, e.Item.Height - 1.5f), 5 * e.Item.Owner!.DeviceDpi / 96f, Theme.Line);
        }
        protected override void OnRenderItemText(ToolStripItemTextRenderEventArgs e)
        {
            var r = new Rectangle(e.TextRectangle.X, 0, e.TextRectangle.Width, e.Item.Height);
            TextRenderer.DrawText(e.Graphics, e.Text, e.TextFont, r, Theme.Text, Theme.OneLine);
        }
    }

    sealed class Colors : ProfessionalColorTable
    {
        public override Color ToolStripDropDownBackground => Theme.Panel2;
        public override Color MenuBorder => Theme.Line;
        public override Color MenuItemBorder => Theme.Line;
        public override Color MenuItemSelected => Theme.Line;
    }
}
