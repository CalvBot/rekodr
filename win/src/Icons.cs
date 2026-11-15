using System.Drawing.Drawing2D;
using System.Globalization;
using System.Text.RegularExpressions;

namespace Clipback;

/// <summary>
/// Lucide icons drawn from their SVG source (IconData.cs), so the window matches the web pages exactly and needs
/// no icon font (Segoe Fluent Icons is missing on Windows 10). Each icon is a 24×24 stroke drawing: 2 px lines
/// with round ends, scaled to the size asked for.
/// </summary>
static partial class Icons
{
    static readonly Dictionary<string, GraphicsPath> cache = [];

    /// <summary>Draws an icon in a square, in the given colour</summary>
    public static void Draw(Graphics g, string name, RectangleF box, Color ink)
    {
        if (!cache.TryGetValue(name, out var path))
        {
            path = Svg.TryGetValue(name, out var svg) ? Parse(svg) : new GraphicsPath();
            cache[name] = path;
        }
        var state = g.Save();
        g.SmoothingMode = SmoothingMode.AntiAlias;
        g.PixelOffsetMode = PixelOffsetMode.HighQuality;
        g.TranslateTransform(box.X, box.Y);
        g.ScaleTransform(box.Width / 24f, box.Height / 24f);
        using var pen = new Pen(ink, box.Width < 14 ? 2.3f : 2f) { StartCap = LineCap.Round, EndCap = LineCap.Round, LineJoin = LineJoin.Round };
        g.DrawPath(pen, path);
        g.Restore(state);
    }

    // ---------- SVG → GraphicsPath ----------

    [GeneratedRegex(@"<(\w+)([^>]*)/>")] private static partial Regex ElementRx();
    [GeneratedRegex(@"([\w-]+)='([^']*)'")] private static partial Regex AttrRx();
    [GeneratedRegex(@"[MmLlHhVvCcSsQqTtAaZz]|-?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?")] private static partial Regex TokenRx();

    static GraphicsPath Parse(string svg)
    {
        var p = new GraphicsPath();
        foreach (Match el in ElementRx().Matches(svg))
        {
            var a = AttrRx().Matches(el.Groups[2].Value).ToDictionary(m => m.Groups[1].Value, m => m.Groups[2].Value);
            float F(string k, float def = 0) => a.TryGetValue(k, out var v) ? float.Parse(v, CultureInfo.InvariantCulture) : def;
            p.StartFigure();
            switch (el.Groups[1].Value)
            {
                case "path": AddPath(p, a.GetValueOrDefault("d", "")); break;
                case "circle": p.AddEllipse(F("cx") - F("r"), F("cy") - F("r"), 2 * F("r"), 2 * F("r")); break;
                case "ellipse": p.AddEllipse(F("cx") - F("rx"), F("cy") - F("ry"), 2 * F("rx"), 2 * F("ry")); break;
                case "line": p.AddLine(F("x1"), F("y1"), F("x2"), F("y2")); break;
                case "rect":
                    var rx = F("rx", F("ry"));
                    var r = new RectangleF(F("x"), F("y"), F("width"), F("height"));
                    if (rx > 0) p.AddPath(Theme.Round(r, rx), false); else p.AddRectangle(r);
                    break;
                case "polyline":
                case "polygon":
                    var n = Numbers(a.GetValueOrDefault("points", ""));
                    var pts = Enumerable.Range(0, n.Count / 2).Select(i => new PointF(n[2 * i], n[2 * i + 1])).ToArray();
                    if (pts.Length > 1) p.AddLines(pts);
                    if (el.Groups[1].Value == "polygon") p.CloseFigure();
                    break;
            }
        }
        return p;
    }

    static List<float> Numbers(string s) =>
        TokenRx().Matches(s).Select(m => float.Parse(m.Value, CultureInfo.InvariantCulture)).ToList();

    static void AddPath(GraphicsPath p, string d)
    {
        var tokens = TokenRx().Matches(d).Select(m => m.Value).ToList();
        var i = 0;
        char cmd = 'M';
        PointF cur = default, start = default, lastCtrl = default;
        char last = ' ';
        bool IsCmd() => i < tokens.Count && char.IsLetter(tokens[i][0]) && tokens[i][0] is not ('e' or 'E');
        float N() => float.Parse(tokens[i++], CultureInfo.InvariantCulture);
        // Arc flags can be written run together ("0 01"); read them one digit at a time
        bool Flag()
        {
            var t = tokens[i];
            if (t.Length > 1) { tokens[i] = t[1..]; return t[0] == '1'; }
            i++;
            return t == "1";
        }

        while (i < tokens.Count)
        {
            if (IsCmd()) cmd = tokens[i++][0];
            var rel = char.IsLower(cmd);
            PointF Pt(float x, float y) => rel ? new PointF(cur.X + x, cur.Y + y) : new PointF(x, y);
            switch (char.ToUpperInvariant(cmd))
            {
                case 'M':
                    cur = start = Pt(N(), N());
                    p.StartFigure();
                    cmd = rel ? 'l' : 'L'; // further pairs are lines
                    break;
                case 'L': { var to = Pt(N(), N()); p.AddLine(cur, to); cur = to; break; }
                case 'H': { var x = N(); var to = new PointF(rel ? cur.X + x : x, cur.Y); p.AddLine(cur, to); cur = to; break; }
                case 'V': { var y = N(); var to = new PointF(cur.X, rel ? cur.Y + y : y); p.AddLine(cur, to); cur = to; break; }
                case 'C':
                    {
                        PointF c1 = Pt(N(), N()), c2 = Pt(N(), N()), to = Pt(N(), N());
                        p.AddBezier(cur, c1, c2, to);
                        lastCtrl = c2; cur = to; break;
                    }
                case 'S':
                    {
                        var c1 = last is 'C' or 'S' ? new PointF(2 * cur.X - lastCtrl.X, 2 * cur.Y - lastCtrl.Y) : cur;
                        PointF c2 = Pt(N(), N()), to = Pt(N(), N());
                        p.AddBezier(cur, c1, c2, to);
                        lastCtrl = c2; cur = to; break;
                    }
                case 'Q':
                    {
                        PointF q = Pt(N(), N()), to = Pt(N(), N());
                        AddQuad(p, cur, q, to);
                        lastCtrl = q; cur = to; break;
                    }
                case 'T':
                    {
                        var q = last is 'Q' or 'T' ? new PointF(2 * cur.X - lastCtrl.X, 2 * cur.Y - lastCtrl.Y) : cur;
                        var to = Pt(N(), N());
                        AddQuad(p, cur, q, to);
                        lastCtrl = q; cur = to; break;
                    }
                case 'A':
                    {
                        float rx = N(), ry = N(), rot = N();
                        bool large = Flag(), sweep = Flag();
                        var to = Pt(N(), N());
                        AddArc(p, cur, to, rx, ry, rot, large, sweep);
                        cur = to; break;
                    }
                case 'Z':
                    p.CloseFigure();
                    cur = start;
                    break;
                default:
                    i++; // unknown: skip a token so a bad icon can't loop forever
                    break;
            }
            last = char.ToUpperInvariant(cmd);
        }
    }

    static void AddQuad(GraphicsPath p, PointF a, PointF q, PointF b) =>
        p.AddBezier(a, new PointF(a.X + 2f / 3 * (q.X - a.X), a.Y + 2f / 3 * (q.Y - a.Y)),
            new PointF(b.X + 2f / 3 * (q.X - b.X), b.Y + 2f / 3 * (q.Y - b.Y)), b);

    /// <summary>An SVG endpoint arc as Bézier curves (SVG spec, appendix F.6)</summary>
    static void AddArc(GraphicsPath p, PointF a, PointF b, double rx, double ry, double rotDeg, bool large, bool sweep)
    {
        if (rx == 0 || ry == 0 || a == b) { p.AddLine(a, b); return; }
        rx = Math.Abs(rx); ry = Math.Abs(ry);
        var phi = rotDeg * Math.PI / 180;
        double cos = Math.Cos(phi), sin = Math.Sin(phi);
        double dx = (a.X - b.X) / 2.0, dy = (a.Y - b.Y) / 2.0;
        double x1 = cos * dx + sin * dy, y1 = -sin * dx + cos * dy;
        var lambda = x1 * x1 / (rx * rx) + y1 * y1 / (ry * ry);
        if (lambda > 1) { var s = Math.Sqrt(lambda); rx *= s; ry *= s; }
        var num = rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1;
        var den = rx * rx * y1 * y1 + ry * ry * x1 * x1;
        var k = Math.Sqrt(Math.Max(0, num / den)) * (large == sweep ? -1 : 1);
        double cx1 = k * rx * y1 / ry, cy1 = -k * ry * x1 / rx;
        double cx = cos * cx1 - sin * cy1 + (a.X + b.X) / 2.0, cy = sin * cx1 + cos * cy1 + (a.Y + b.Y) / 2.0;
        static double Angle(double ux, double uy, double vx, double vy)
        {
            var ang = Math.Atan2(ux * vy - uy * vx, ux * vx + uy * vy);
            return ang;
        }
        var t1 = Angle(1, 0, (x1 - cx1) / rx, (y1 - cy1) / ry);
        var dt = Angle((x1 - cx1) / rx, (y1 - cy1) / ry, (-x1 - cx1) / rx, (-y1 - cy1) / ry);
        if (!sweep && dt > 0) dt -= 2 * Math.PI;
        else if (sweep && dt < 0) dt += 2 * Math.PI;
        var segs = (int)Math.Ceiling(Math.Abs(dt) / (Math.PI / 2) - 1e-9);
        var step = dt / segs;
        var t = 4.0 / 3 * Math.Tan(step / 4);
        PointF At(double th) => new((float)(cx + rx * Math.Cos(th) * cos - ry * Math.Sin(th) * sin), (float)(cy + rx * Math.Cos(th) * sin + ry * Math.Sin(th) * cos));
        (double, double) Deriv(double th) => (-rx * Math.Sin(th) * cos - ry * Math.Cos(th) * sin, -rx * Math.Sin(th) * sin + ry * Math.Cos(th) * cos);
        var from = a;
        for (var s = 0; s < segs; s++)
        {
            double th1 = t1 + s * step, th2 = th1 + step;
            var (d1x, d1y) = Deriv(th1);
            var (d2x, d2y) = Deriv(th2);
            var to = s == segs - 1 ? b : At(th2);
            p.AddBezier(from, new PointF((float)(from.X + t * d1x), (float)(from.Y + t * d1y)), new PointF((float)(to.X - t * d2x), (float)(to.Y - t * d2y)), to);
            from = to;
        }
    }
}
