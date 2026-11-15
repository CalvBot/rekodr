using System.Drawing.Drawing2D;

namespace Clipback;

/// <summary>The web UI's colours (ui/style.css) and shared drawing helpers</summary>
static class Theme
{
    public static readonly Color Bg = Hex(0x111317);
    public static readonly Color Panel = Hex(0x1a1d23);
    public static readonly Color Panel2 = Hex(0x22262e);
    public static readonly Color Line = Hex(0x2e333d);
    public static readonly Color Text = Hex(0xe8eaee);
    public static readonly Color Dim = Hex(0x9097a3);
    public static readonly Color Accent = Hex(0xe8333f);  // recording red
    public static readonly Color Accent2 = Hex(0x4d8dff); // selection blue
    public static readonly Color Ok = Hex(0x3fbf74);
    public static readonly Color Hover = Hex(0x2b3039);
    public static readonly Color HoverLine = Hex(0x3a404c);
    public static readonly Color CardHoverLine = Hex(0x3b414d);
    public static readonly Color Thumb = Hex(0x0b0c0f);
    public static readonly Color Badge = Hex(0xef4444);
    public static readonly Color PrimaryHover = Hex(0x3a7cf0);

    static Color Hex(int rgb) => Color.FromArgb(255, (rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255);

    public static Color Blend(Color from, Color to, double amount) =>
        Color.FromArgb((int)(from.R + (to.R - from.R) * amount), (int)(from.G + (to.G - from.G) * amount), (int)(from.B + (to.B - from.B) * amount));

    static readonly Dictionary<(float, FontStyle), Font> fonts = [];

    /// <summary>Segoe UI at a size in 96-dpi pixels (like the CSS), for a control's DPI. Cached, never disposed.</summary>
    public static Font Font(Control c, float cssPx, FontStyle style = FontStyle.Regular)
    {
        var px = cssPx * c.DeviceDpi / 96f;
        if (!fonts.TryGetValue((px, style), out var f))
        {
            var family = style.HasFlag(FontStyle.Bold) ? "Segoe UI Semibold" : "Segoe UI";
            f = new Font(family, px, style & ~FontStyle.Bold, GraphicsUnit.Pixel);
            fonts[(px, style)] = f;
        }
        return f;
    }

    public static int Px(this Control c, float v) => (int)Math.Round(v * c.DeviceDpi / 96f);
    public static float Pxf(this Control c, float v) => v * c.DeviceDpi / 96f;

    public static GraphicsPath Round(RectangleF r, float radius)
    {
        var d = Math.Min(radius * 2, Math.Min(r.Width, r.Height));
        var p = new GraphicsPath();
        if (d <= 0)
        {
            if (r.Width > 0 && r.Height > 0) p.AddRectangle(r);
            return p;
        }
        p.AddArc(r.X, r.Y, d, d, 180, 90);
        p.AddArc(r.Right - d, r.Y, d, d, 270, 90);
        p.AddArc(r.Right - d, r.Bottom - d, d, d, 0, 90);
        p.AddArc(r.X, r.Bottom - d, d, d, 90, 90);
        p.CloseFigure();
        return p;
    }

    public static void FillRound(Graphics g, RectangleF r, float radius, Color fill, Color? border = null)
    {
        g.SmoothingMode = SmoothingMode.AntiAlias;
        using var path = Round(r, radius);
        using (var b = new SolidBrush(fill)) g.FillPath(b, path);
        if (border is { } bc) using (var pen = new Pen(bc)) g.DrawPath(pen, path);
    }

    public const TextFormatFlags OneLine = TextFormatFlags.SingleLine | TextFormatFlags.NoPrefix | TextFormatFlags.VerticalCenter;
    public const TextFormatFlags Ellipsis = OneLine | TextFormatFlags.EndEllipsis;
}

/// <summary>A control that paints itself, double-buffered, with no focus rectangle</summary>
class Painted : Control
{
    public Painted()
    {
        SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw, true);
        SetStyle(ControlStyles.Selectable, false);
        // Two quick clicks are two clicks, not one click and a double-click
        SetStyle(ControlStyles.StandardDoubleClick, false);
    }
}
