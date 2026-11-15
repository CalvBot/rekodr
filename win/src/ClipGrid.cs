using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.Text.RegularExpressions;

namespace Clipback;

/// <summary>
/// The clips as cards, grouped by day, one fixed page at a time (no scrolling, so nothing moves under the mouse;
/// the wheel or the pager turns pages). Drawn in one control: a page of cards costs no windows.
/// Hovering a thumbnail scrubs through the clip's sprite strip; clicking it plays from there.
/// </summary>
sealed partial class ClipGrid : Painted
{
    const int MinCard = 260, Gap = 14, Pad = 16, DayRow = 30, Meta = 52;

    List<Clip> items = [];
    int page, cols = 1, rows = 1, cardW, cardH, thumbH;
    string emptyText = "";

    // Hover state: which card, and where along its thumbnail (0–1), or -1 off the thumbnail
    int hover = -1;
    double scrub = -1;

    public event Action<Clip, double>? Play;
    public event Action<Clip, Point>? Menu;
    /// <summary>The page or the number of pages changed</summary>
    public event Action? PagesChanged;

    int PageSize => rows * cols;
    public int Page => page;
    public int PageCount => Math.Max(1, (items.Count + PageSize - 1) / PageSize);
    public int Count => items.Count;

    public ClipGrid() => BackColor = Theme.Bg;

    /// <summary>Shows a new list. `keepPage`: stay on the same page (a refresh) rather than go back to the first.</summary>
    public void SetItems(List<Clip> list, string empty, bool keepPage)
    {
        var firstShown = keepPage ? FirstOnPage() : null;
        items = list;
        emptyText = empty;
        hover = -1;
        Paginate(firstShown);
    }

    Clip? FirstOnPage() => page * PageSize < items.Count ? items[page * PageSize] : null;

    public void GoTo(int p)
    {
        p = Math.Clamp(p, 0, PageCount - 1);
        if (p == page) return;
        page = p;
        hover = -1;
        Invalidate();
        LoadThumbs();
        PagesChanged?.Invoke();
    }

    protected override void OnResize(EventArgs e)
    {
        base.OnResize(e);
        Paginate(FirstOnPage());
    }

    protected override void OnDpiChangedAfterParent(EventArgs e)
    {
        base.OnDpiChangedAfterParent(e);
        Paginate(FirstOnPage());
    }

    /// <summary>
    /// A page is always rows × columns cards, whatever the days: each row has a strip above it where a day's
    /// name sits over its first card. So the grid never reflows; only the window's size changes the page size.
    /// </summary>
    void Paginate(Clip? keep)
    {
        int pad = this.Px(Pad), gap = this.Px(Gap);
        var w = Math.Max(1, Width - 2 * pad);
        cols = Math.Max(1, (w + gap) / (this.Px(MinCard) + gap));
        cardW = (w - (cols - 1) * gap) / cols;
        thumbH = cardW * 9 / 16;
        cardH = thumbH + this.Px(Meta);
        var rowH = this.Px(DayRow) + cardH + gap;
        rows = Math.Max(1, (Height - 2 * pad + gap) / rowH);
        var at = keep != null ? items.IndexOf(keep) : -1;
        page = Math.Clamp(at >= 0 ? at / PageSize : 0, 0, PageCount - 1);
        Invalidate();
        LoadThumbs();
        PagesChanged?.Invoke();
    }

    /// <summary>Card bounds and indexes on the current page</summary>
    IEnumerable<(int index, Rectangle card, Rectangle thumb)> Cards()
    {
        int pad = this.Px(Pad), gap = this.Px(Gap), dayH = this.Px(DayRow);
        for (var n = 0; n < PageSize; n++)
        {
            var i = page * PageSize + n;
            if (i >= items.Count) yield break;
            int x = pad + n % cols * (cardW + gap), y = pad + n / cols * (dayH + cardH + gap) + dayH;
            var card = new Rectangle(x, y, cardW, cardH);
            yield return (i, card, new Rectangle(x + 1, y + 1, cardW - 2, thumbH));
        }
    }

    // ---------- thumbnails ----------

    sealed class Thumb
    {
        public byte[]? Jpeg;
        public Bitmap? First; // frame 0, ready to draw
        public bool Loading, Failed;
        public long Used;
    }

    static readonly Dictionary<string, Thumb> thumbs = [];
    static readonly SemaphoreSlim fetching = new(3);
    static long useClock;
    // The hovered clip's whole strip, decoded (the only full strip kept: each is ~5 MB)
    string? stripKey;
    Bitmap? strip;

    static string Key(Clip c) => c.Id + "|" + c.Mtime;

    void LoadThumbs()
    {
        foreach (var (i, _, _) in Cards().ToList())
        {
            var c = items[i];
            var key = Key(c);
            if (!thumbs.TryGetValue(key, out var t)) thumbs[key] = t = new Thumb();
            t.Used = ++useClock;
            if (t.Loading || t.First != null || t.Failed) continue;
            t.Loading = true;
            _ = Fetch(c, t);
        }
        Trim();
    }

    async Task Fetch(Clip c, Thumb t)
    {
        await fetching.WaitAsync();
        try
        {
            var bytes = await Api.Bytes(Api.SpriteUrl(c));
            var first = bytes == null ? null : await Task.Run(() => FirstFrame(bytes));
            t.Jpeg = first != null ? bytes : null;
            t.First = first;
            t.Failed = first == null;
            if (c.Info == null)
                try { c.Info = await Api.Info(c.Id); } catch { }
        }
        finally
        {
            t.Loading = false;
            fetching.Release();
        }
        if (!IsDisposed) Invalidate();
    }

    static Bitmap? FirstFrame(byte[] jpeg)
    {
        try
        {
            using var ms = new MemoryStream(jpeg);
            using var img = Image.FromStream(ms, false, false);
            var fw = img.Width / Api.SpriteFrames;
            var bmp = new Bitmap(fw, img.Height, PixelFormat.Format32bppPArgb);
            using var g = Graphics.FromImage(bmp);
            g.DrawImage(img, new Rectangle(0, 0, fw, img.Height), new Rectangle(0, 0, fw, img.Height), GraphicsUnit.Pixel);
            return bmp;
        }
        catch { return null; }
    }

    /// <summary>Keeps memory small: thumbnails of the 48 most recently shown clips</summary>
    void Trim()
    {
        if (thumbs.Count <= 48) return;
        foreach (var (key, t) in thumbs.Where(kv => !kv.Value.Loading).OrderBy(kv => kv.Value.Used).Take(thumbs.Count - 48).ToList())
        {
            t.First?.Dispose();
            thumbs.Remove(key);
        }
    }

    Bitmap? Strip(Clip c)
    {
        var key = Key(c);
        if (stripKey == key) return strip;
        strip?.Dispose();
        strip = null;
        stripKey = key;
        if (thumbs.TryGetValue(key, out var t) && t.Jpeg != null)
            try
            {
                using var ms = new MemoryStream(t.Jpeg);
                using var img = Image.FromStream(ms, false, false);
                strip = new Bitmap(img.Width, img.Height, PixelFormat.Format32bppPArgb);
                using var g = Graphics.FromImage(strip);
                g.DrawImage(img, 0, 0, img.Width, img.Height);
            }
            catch { strip = null; }
        return strip;
    }

    // ---------- mouse ----------

    protected override void OnMouseMove(MouseEventArgs e)
    {
        int newHover = -1;
        double newScrub = -1;
        foreach (var (i, card, thumb) in Cards())
        {
            if (!card.Contains(e.Location)) continue;
            newHover = i;
            if (thumb.Contains(e.Location)) newScrub = Math.Clamp((e.X - thumb.X) / (double)thumb.Width, 0, 0.999);
            break;
        }
        Cursor = newHover >= 0 ? Cursors.Hand : Cursors.Default;
        if (newHover != hover || newScrub != scrub)
        {
            hover = newHover;
            scrub = newScrub;
            Invalidate(); // the red bar under the thumbnail follows the mouse smoothly
        }
    }

#if DEBUG
    /// <summary>The n-th card on this page, as if hovered at this point along its thumbnail</summary>
    public void TestHover(int n, double frac)
    {
        var cards = Cards().ToList();
        if (n >= cards.Count) return;
        hover = cards[n].index;
        scrub = frac;
        Invalidate();
    }

    public void TestPlay(int n, double frac)
    {
        var cards = Cards().ToList();
        if (n < cards.Count) Play?.Invoke(items[cards[n].index], frac);
    }

    public void TestPick(int n, Action<Clip> act)
    {
        var cards = Cards().ToList();
        if (n < cards.Count) act(items[cards[n].index]);
    }
#endif

    protected override void OnMouseLeave(EventArgs e)
    {
        hover = -1;
        scrub = -1;
        Invalidate();
    }

    protected override void OnMouseUp(MouseEventArgs e)
    {
        foreach (var (i, card, _) in Cards())
        {
            if (!card.Contains(e.Location)) continue;
            if (e.Button == MouseButtons.Left) Play?.Invoke(items[i], Math.Max(0, scrub));
            else if (e.Button == MouseButtons.Right) Menu?.Invoke(items[i], e.Location);
            return;
        }
    }

    protected override void OnMouseWheel(MouseEventArgs e) => GoTo(page + (e.Delta < 0 ? 1 : -1));

    // ---------- painting ----------

    [GeneratedRegex(@" \d{4}-\d\d-\d\d \d\d-\d\d-\d\d")] private static partial Regex DateRx();

    protected override void OnPaint(PaintEventArgs e)
    {
        var g = e.Graphics;
        g.Clear(Theme.Bg);
        if (items.Count == 0)
        {
            TextRenderer.DrawText(g, emptyText, Theme.Font(this, 14), new Rectangle(0, this.Px(60), Width, this.Px(30)), Theme.Dim, Theme.OneLine | TextFormatFlags.HorizontalCenter);
            return;
        }
        // A day's name over the first card of each day in a row (and over the first card of the page)
        var dayFont = Theme.Font(this, 12, FontStyle.Bold);
        var cards = Cards().ToList();
        for (var k = 0; k < cards.Count; k++)
        {
            var (i, card, _) = cards[k];
            var day = Fmt.Day(items[i].Mtime);
            var rowStart = k % cols == 0;
            if (k > 0 && Fmt.Day(items[i - 1].Mtime) == day && !(rowStart && k == 0)) continue;
            // As wide as the day's cards in this row
            var span = 1;
            while (k + span < cards.Count && (k + span) % cols != 0 && Fmt.Day(items[i + span].Mtime) == day) span++;
            var label = new Rectangle(card.X, card.Y - this.Px(DayRow), span * cardW + (span - 1) * this.Px(Gap), this.Px(DayRow) - this.Px(6));
            TextRenderer.DrawText(g, day.ToUpperInvariant(), dayFont, label, Theme.Dim, Theme.Ellipsis);
        }
        foreach (var (i, card, thumb) in Cards()) DrawCard(g, items[i], card, thumb, i == hover);
    }

    void DrawCard(Graphics g, Clip c, Rectangle card, Rectangle thumb, bool hovered)
    {
        var radius = this.Pxf(10);
        g.SmoothingMode = SmoothingMode.AntiAlias;
        var cardF = new RectangleF(card.X + 0.5f, card.Y + 0.5f, card.Width - 1, card.Height - 1);
        using var shape = Theme.Round(cardF, radius);
        using (var fill = new SolidBrush(Theme.Panel)) g.FillPath(fill, shape);

        // Thumbnail, clipped to the card's rounded top
        var state = g.Save();
        g.SetClip(shape);
        using (var tb = new SolidBrush(Theme.Thumb)) g.FillRectangle(tb, thumb);
        thumbs.TryGetValue(Key(c), out var t);
        var scrubbing = hovered && scrub >= 0;
        g.InterpolationMode = InterpolationMode.Bilinear;
        g.PixelOffsetMode = PixelOffsetMode.Half;
        if (scrubbing && Strip(c) is { } s)
        {
            var fw = s.Width / Api.SpriteFrames;
            var frame = Math.Min(Api.SpriteFrames - 1, (int)(scrub * Api.SpriteFrames));
            g.DrawImage(s, thumb, new Rectangle(frame * fw, 0, fw, s.Height), GraphicsUnit.Pixel);
        }
        else if (t?.First is { } first)
            g.DrawImage(first, thumb, new Rectangle(0, 0, first.Width, first.Height), GraphicsUnit.Pixel);
        if (scrubbing)
            using (var bar = new SolidBrush(Theme.Accent)) g.FillRectangle(bar, thumb.X, thumb.Bottom - this.Px(3), (float)(thumb.Width * scrub), this.Px(3));
        g.Restore(state);

        // Badges: tracks top-left (when there's more than one), duration bottom-right
        var info = c.Info;
        var small = Theme.Font(this, 12);
        if (info is { Tracks.Count: > 1 })
        {
            var n = info.Tracks.Count.ToString();
            var nw = TextRenderer.MeasureText(g, n, Theme.Font(this, 11), Size.Empty, TextFormatFlags.NoPrefix).Width;
            var r = new RectangleF(thumb.X + this.Px(6), thumb.Y + this.Px(6), this.Px(6 + 12 + 4 + 6) + nw - this.Px(4), this.Px(20));
            Theme.FillRound(g, r, this.Pxf(4), Color.FromArgb(153, 0, 0, 0));
            Icons.Draw(g, "audio-lines", new RectangleF(r.X + this.Px(6), r.Y + this.Px(4), this.Px(12), this.Px(12)), Theme.Text);
            TextRenderer.DrawText(g, n, Theme.Font(this, 11, FontStyle.Bold), new Rectangle((int)r.X + this.Px(20), (int)r.Y, nw + 4, (int)r.Height), Theme.Text, Theme.OneLine);
        }
        if (info != null)
        {
            var len = Fmt.Time(info.Duration);
            var lw = TextRenderer.MeasureText(g, len, small, Size.Empty, TextFormatFlags.NoPrefix).Width;
            var r = new RectangleF(thumb.Right - this.Px(6) - lw - this.Px(8), thumb.Bottom - this.Px(7) - this.Px(20), lw + this.Px(8), this.Px(20));
            Theme.FillRound(g, r, this.Pxf(4), Color.FromArgb(184, 0, 0, 0));
            TextRenderer.DrawText(g, len, small, Rectangle.Round(r), Theme.Text, Theme.OneLine | TextFormatFlags.HorizontalCenter);
        }
        if (t?.Failed == true && t.First == null)
            TextRenderer.DrawText(g, "No preview", small, thumb, Theme.Dim, Theme.OneLine | TextFormatFlags.HorizontalCenter);

        // Name and "today 9:04 pm · 42 MB"
        var name = DateRx().Replace(c.Name, "");
        if (name.Length == 0) name = c.Name;
        int tx = card.X + this.Px(10), tw = card.Width - this.Px(20), ty = thumb.Bottom + this.Px(8);
        TextRenderer.DrawText(g, name, Theme.Font(this, 14, FontStyle.Bold), new Rectangle(tx, ty, tw, this.Px(20)), Theme.Text, Theme.Ellipsis);
        TextRenderer.DrawText(g, $"{Fmt.Ago(c.Mtime)} · {Fmt.Size(c.Size)}", small, new Rectangle(tx, ty + this.Px(20), tw, this.Px(18)), Theme.Dim, Theme.Ellipsis);

        using var edge = new Pen(hovered ? Theme.CardHoverLine : Theme.Line);
        g.SmoothingMode = SmoothingMode.AntiAlias;
        g.DrawPath(edge, shape);
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing) strip?.Dispose();
        base.Dispose(disposing);
    }
}
