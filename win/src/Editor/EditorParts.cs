using System.Drawing.Drawing2D;

namespace Clipback;

/// <summary>Text drawn in place (no Label flicker): one line with an ellipsis, or wrapped</summary>
sealed class EdText : Painted
{
    public Color Ink { get; set; } = Theme.Text;
    public float FontPx { get; set; } = 13;
    public FontStyle Style { get; set; }
    public bool Wrap { get; init; }
    public bool AlignRight { get; init; }

    protected override void OnTextChanged(EventArgs e) { base.OnTextChanged(e); Invalidate(); }

    protected override void OnPaint(PaintEventArgs e)
    {
        e.Graphics.Clear(Parent?.BackColor ?? Theme.Bg);
        var flags = Wrap ? TextFormatFlags.WordBreak | TextFormatFlags.NoPrefix : Theme.Ellipsis;
        if (AlignRight) flags |= TextFormatFlags.Right;
        TextRenderer.DrawText(e.Graphics, Text, Theme.Font(this, FontPx, Style), ClientRectangle, Enabled ? Ink : Theme.Blend(Theme.Bg, Ink, 0.5), flags);
    }
}

/// <summary>A range slider like the web page's: drag or click; a double-click puts it back to Default</summary>
sealed class EdSlider : Painted
{
    double value;
    bool dragging, hover;
    long lastDown;
    public double Min { get; init; }
    public double Max { get; init; } = 1;
    public double? Default { get; init; }
    public double Value { get => value; set { if (!dragging) { this.value = Math.Clamp(value, Min, Max); Invalidate(); } } }
    public bool Dragging => dragging;
    /// <summary>Moved by the user (also while dragging)</summary>
    public event Action<double>? Moved;

    public EdSlider() => Cursor = Cursors.Hand;

    float Pad => this.Pxf(7);

    void At(int x)
    {
        var f = Math.Clamp((x - Pad) / Math.Max(1, Width - 2 * Pad), 0, 1);
        value = Min + f * (Max - Min);
        Invalidate();
        Moved?.Invoke(value);
    }

    protected override void OnMouseDown(MouseEventArgs e)
    {
        if (e.Button != MouseButtons.Left || !Enabled) return;
        var now = Environment.TickCount64;
        if (Default is { } d && now - lastDown < SystemInformation.DoubleClickTime)
        {
            lastDown = 0;
            value = d;
            Invalidate();
            Moved?.Invoke(value);
            return;
        }
        lastDown = now;
        dragging = true;
        Capture = true;
        At(e.X);
    }
    protected override void OnMouseMove(MouseEventArgs e) { if (dragging) At(e.X); }
    protected override void OnMouseUp(MouseEventArgs e) { if (dragging) { At(e.X); dragging = false; Capture = false; } }
    protected override void OnMouseEnter(EventArgs e) { hover = true; Invalidate(); }
    protected override void OnMouseLeave(EventArgs e) { hover = false; Invalidate(); }
    protected override void OnEnabledChanged(EventArgs e) { base.OnEnabledChanged(e); Invalidate(); }

    protected override void OnPaint(PaintEventArgs e)
    {
        var g = e.Graphics;
        g.Clear(Parent?.BackColor ?? Theme.Bg);
        float pad = Pad, h = this.Pxf(4), y = (Height - h) / 2, w = Width - 2 * pad;
        var f = (float)((value - Min) / Math.Max(1e-9, Max - Min));
        Theme.FillRound(g, new RectangleF(pad, y, w, h), h / 2, Theme.Line);
        var on = Enabled ? Theme.Accent2 : Theme.Blend(Theme.Bg, Theme.Accent2, 0.4);
        if (f > 0) Theme.FillRound(g, new RectangleF(pad, y, Math.Max(h, w * f), h), h / 2, on);
        var knob = this.Pxf(hover || dragging ? 14 : 13);
        using var b = new SolidBrush(Enabled ? Color.White : Theme.Dim);
        g.FillEllipse(b, pad + w * f - knob / 2, Height / 2f - knob / 2, knob, knob);
    }
}

/// <summary>A checkbox and its label</summary>
sealed class EdCheck : Painted
{
    bool on, hover;
    public bool Checked { get => on; set { on = value; Invalidate(); } }
    public event Action? Toggled;

    public EdCheck(string text) { Text = text; Cursor = Cursors.Hand; }

    protected override void OnClick(EventArgs e) { on = !on; Invalidate(); Toggled?.Invoke(); base.OnClick(e); }
    protected override void OnMouseEnter(EventArgs e) { hover = true; Invalidate(); }
    protected override void OnMouseLeave(EventArgs e) { hover = false; Invalidate(); }

    protected override void OnPaint(PaintEventArgs e)
    {
        var g = e.Graphics;
        g.Clear(Parent?.BackColor ?? Theme.Bg);
        var s = this.Pxf(16);
        var box = new RectangleF(0.5f, (Height - s) / 2, s, s);
        Theme.FillRound(g, box, this.Pxf(4), on ? Theme.Accent2 : hover ? Theme.Hover : Theme.Panel2, on ? Theme.Accent2 : hover ? Theme.HoverLine : Theme.Line);
        if (on)
        {
            using var pen = new Pen(Color.White, this.Pxf(2)) { StartCap = LineCap.Round, EndCap = LineCap.Round, LineJoin = LineJoin.Round };
            g.DrawLines(pen, [new PointF(box.X + s * 0.25f, box.Y + s * 0.52f), new PointF(box.X + s * 0.43f, box.Y + s * 0.7f), new PointF(box.X + s * 0.76f, box.Y + s * 0.32f)]);
        }
        var x = (int)(s + this.Pxf(8));
        TextRenderer.DrawText(g, Text, Theme.Font(this, 13), new Rectangle(x, 0, Width - x, Height), Theme.Text, Theme.Ellipsis);
    }
}

/// <summary>A thin progress bar; when not Active it draws nothing but keeps its place</summary>
sealed class EdProgress : Painted
{
    double value;
    bool active;
    public double Value { get => value; set { this.value = Math.Clamp(value, 0, 1); Invalidate(); } }
    public bool Active { get => active; set { active = value; Invalidate(); } }

    protected override void OnPaint(PaintEventArgs e)
    {
        var g = e.Graphics;
        g.Clear(Parent?.BackColor ?? Theme.Bg);
        if (!active) return;
        var r = new RectangleF(0, 0, Width, Height);
        Theme.FillRound(g, r, Height / 2f, Theme.Panel2);
        if (value > 0) Theme.FillRound(g, new RectangleF(0, 0, Math.Max(Height, (float)(Width * value)), Height), Height / 2f, Theme.Accent2);
    }
}

/// <summary>
/// The trim timeline: the clip's thumbnail strip, the parts trimmed off shaded, the selection framed in yellow with a
/// handle at each end, a diamond per zoom keyframe and the playhead. Click or drag to seek; drag a handle to trim;
/// click a diamond to go to that keyframe. Drawn in one control.
/// </summary>
sealed class EdTimeline : Painted
{
    static readonly Color Yellow = Color.FromArgb(0xf5, 0xc5, 0x42);
    Image? sprite;
    Bitmap? strip; // the sprite scaled to the bar, made once per size
    double duration, inT, outT, head;
    IReadOnlyList<ZoomKey> keys = [];
    int selected = -1;
    string drag = "";

    /// <summary>"seek", "in" or "out", and the time under the mouse</summary>
    public event Action<string, double>? Dragged;
    /// <summary>A keyframe's diamond was clicked</summary>
    public event Action<ZoomKey>? KeyClicked;

    public EdTimeline() => Cursor = Cursors.Hand;

    public Image? Sprite { get => sprite; set { sprite = value; strip?.Dispose(); strip = null; Invalidate(); } }
    public double Duration { get => duration; set { duration = value; Invalidate(); } }

    public void Set(double inT, double outT, IReadOnlyList<ZoomKey> keys, int selected)
    {
        this.inT = inT; this.outT = outT; this.keys = keys; this.selected = selected;
        Invalidate();
    }

    public double Head
    {
        get => head;
        set
        {
            if (value == head) return;
            // Only the strips around the old and new playhead are redrawn
            var old = X(head);
            head = value;
            var now = X(head);
            int pad = this.Px(4);
            Invalidate(new Rectangle((int)Math.Min(old, now) - pad, 0, (int)Math.Abs(now - old) + 2 * pad, Height));
        }
    }

    /// <summary>The bar itself; the playhead runs 4 px past it above and below</summary>
    RectangleF Bar => new(this.Pxf(1), this.Pxf(4), Width - this.Pxf(2), Height - this.Pxf(8));
    float X(double t) { var b = Bar; return b.X + (float)(duration > 0 ? t / duration : 0) * b.Width; }
    double T(int x) { var b = Bar; return duration <= 0 ? 0 : Math.Clamp((x - b.X) / b.Width, 0, 1) * duration; }

    ZoomKey? KeyAt(Point p)
    {
        var b = Bar;
        if (p.Y < b.Bottom - this.Pxf(18)) return null;
        foreach (var k in keys)
            if (Math.Abs(p.X - X(k.T)) <= this.Pxf(7)) return k;
        return null;
    }

    string HandleAt(Point p)
    {
        var grab = this.Pxf(7);
        float di = Math.Abs(p.X - X(inT)), dout = Math.Abs(p.X - X(outT));
        if (Math.Min(di, dout) > grab) return "";
        return di < dout || (di == dout && p.X < X(inT)) ? "in" : "out";
    }

    protected override void OnMouseDown(MouseEventArgs e)
    {
        if (e.Button != MouseButtons.Left || duration <= 0) return;
        if (KeyAt(e.Location) is { } k) { KeyClicked?.Invoke(k); return; }
        drag = HandleAt(e.Location) is var h && h != "" ? h : "seek";
        Capture = true;
        Dragged?.Invoke(drag, T(e.X));
    }

    protected override void OnMouseMove(MouseEventArgs e)
    {
        if (drag != "") { Dragged?.Invoke(drag, T(e.X)); return; }
        Cursor = KeyAt(e.Location) != null ? Cursors.Hand : HandleAt(e.Location) != "" ? Cursors.SizeWE : Cursors.Hand;
    }

    protected override void OnMouseUp(MouseEventArgs e)
    {
        if (drag == "") return;
        Dragged?.Invoke(drag, T(e.X));
        drag = "";
        Capture = false;
    }

    protected override void OnResize(EventArgs e) { strip?.Dispose(); strip = null; base.OnResize(e); }

    Bitmap? Strip()
    {
        var b = Rectangle.Round(Bar);
        if (sprite == null || b.Width <= 0 || b.Height <= 0) return null;
        if (strip != null) return strip;
        strip = new Bitmap(b.Width, b.Height);
        using var g = Graphics.FromImage(strip);
        g.InterpolationMode = InterpolationMode.HighQualityBilinear;
        g.PixelOffsetMode = PixelOffsetMode.HighQuality;
        g.DrawImage(sprite, new Rectangle(0, 0, b.Width, b.Height));
        return strip;
    }

    protected override void OnPaint(PaintEventArgs e)
    {
        var g = e.Graphics;
        g.Clear(Parent?.BackColor ?? Theme.Bg);
        var bar = Bar;
        var radius = this.Pxf(6);
        using (var clip = Theme.Round(bar, radius))
        {
            var state = g.Save();
            g.SetClip(clip);
            using (var bg = new SolidBrush(Theme.Thumb)) g.FillRectangle(bg, bar);
            if (Strip() is { } s) g.DrawImageUnscaled(s, Rectangle.Round(bar).Location);
            // Trimmed off: shaded
            using (var shade = new SolidBrush(Color.FromArgb(168, 0, 0, 0)))
            {
                g.FillRectangle(shade, bar.X, bar.Y, X(inT) - bar.X, bar.Height);
                g.FillRectangle(shade, X(outT), bar.Y, bar.Right - X(outT), bar.Height);
            }
            g.Restore(state);
        }
        if (duration <= 0) return;

        // Selection frame and handles
        g.SmoothingMode = SmoothingMode.AntiAlias;
        var pw = this.Pxf(2);
        var sel = new RectangleF(X(inT) + pw / 2, bar.Y + pw / 2, Math.Max(pw, X(outT) - X(inT) - pw), bar.Height - pw);
        using (var path = Theme.Round(sel, radius))
        using (var pen = new Pen(Yellow, pw))
            g.DrawPath(pen, path);
        foreach (var t in new[] { inT, outT })
        {
            var hw = this.Pxf(4);
            var r = new RectangleF(X(t) - hw / 2, bar.Y + this.Pxf(18), hw, Math.Max(hw, bar.Height - this.Pxf(36)));
            Theme.FillRound(g, r, hw / 2, Yellow);
        }

        // Keyframes: small diamonds along the bottom
        for (var i = 0; i < keys.Count; i++)
        {
            float x = X(keys[i].T), s = this.Pxf(8), cy = bar.Bottom - this.Pxf(3) - s * 0.7f;
            PointF[] d = [new(x, cy - s * 0.7f), new(x + s * 0.7f, cy), new(x, cy + s * 0.7f), new(x - s * 0.7f, cy)];
            using (var fill = new SolidBrush(i == selected ? Color.White : Theme.Accent2)) g.FillPolygon(fill, d);
            using (var line = new Pen(Color.White, this.Pxf(1))) g.DrawPolygon(line, d);
        }

        // Playhead, with a dark edge so it shows on bright frames
        var hx = X(head);
        using (var edge = new SolidBrush(Color.FromArgb(140, 0, 0, 0))) g.FillRectangle(edge, hx - this.Pxf(2), 0, this.Pxf(4), Height);
        using (var white = new SolidBrush(Color.White)) g.FillRectangle(white, hx - this.Pxf(1), 0, this.Pxf(2), Height);
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing) strip?.Dispose();
        base.Dispose(disposing);
    }
}

/// <summary>The keyframes as small chips ("0:05.0 · 1.50×"); click one to go there. Two rows are always kept, so
/// adding keyframes never pushes the rest of the panel down; past two rows the last chip says how many more.</summary>
sealed class EdKeyList : Painted
{
    IReadOnlyList<ZoomKey> keys = [];
    int selected = -1;
    readonly List<(RectangleF box, ZoomKey key)> hits = [];
    public event Action<ZoomKey>? KeyClicked;

    public const int Rows = 2, RowHeight96 = 24;

    public void Set(IReadOnlyList<ZoomKey> keys, int selected)
    {
        this.keys = keys;
        this.selected = selected;
        Invalidate();
    }

    protected override void OnMouseDown(MouseEventArgs e)
    {
        foreach (var (box, key) in hits)
            if (box.Contains(e.Location)) { KeyClicked?.Invoke(key); return; }
    }

    protected override void OnPaint(PaintEventArgs e)
    {
        var g = e.Graphics;
        g.Clear(Parent?.BackColor ?? Theme.Bg);
        hits.Clear();
        var font = Theme.Font(this, 12);
        float x = 0, y = 0, h = this.Pxf(20), gap = this.Pxf(4), row = this.Pxf(RowHeight96);
        if (keys.Count == 0)
        {
            TextRenderer.DrawText(g, "No keyframes: the whole clip is unzoomed.", font, new Rectangle(0, 0, Width, (int)h), Theme.Dim, Theme.Ellipsis);
            return;
        }
        var flags = TextFormatFlags.NoPrefix | TextFormatFlags.SingleLine;
        for (var i = 0; i < keys.Count; i++)
        {
            var label = $"{EdFmt.Time(keys[i].T)} · {keys[i].Z:0.00}×";
            var w = TextRenderer.MeasureText(g, label, font, Size.Empty, flags).Width + this.Pxf(12);
            var lastRow = y + row >= Rows * row - 0.5f;
            var more = $"+{keys.Count - i} more";
            var moreW = TextRenderer.MeasureText(g, more, font, Size.Empty, flags).Width + this.Pxf(12);
            // On the last row, keep room for "+N more" unless this is the last chip
            var need = w + (lastRow && i < keys.Count - 1 ? gap + moreW : 0);
            if (x > 0 && x + need > Width)
            {
                if (lastRow) { Chip(g, new RectangleF(x, y, moreW, h), more, font, false); return; }
                x = 0;
                y += row;
                lastRow = true;
                need = w + (i < keys.Count - 1 ? gap + moreW : 0);
                if (x + need > Width) { Chip(g, new RectangleF(x, y, moreW, h), more, font, false); return; }
            }
            var box = new RectangleF(x, y, w, h);
            Chip(g, box, label, font, i == selected);
            hits.Add((box, keys[i]));
            x += w + gap;
        }
    }

    void Chip(Graphics g, RectangleF box, string text, Font font, bool on)
    {
        Theme.FillRound(g, new RectangleF(box.X + 0.5f, box.Y + 0.5f, box.Width - 1, box.Height - 1), this.Pxf(5), Theme.Panel2, on ? Theme.Accent2 : Theme.Line);
        TextRenderer.DrawText(g, text, font, Rectangle.Round(box), Theme.Text, Theme.OneLine | TextFormatFlags.HorizontalCenter);
    }
}
