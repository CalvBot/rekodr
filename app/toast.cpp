// toast.cpp: the desktop toasts, small windows over everything in the corner of a monitor (Options > General >
// Popups): "Clip saved" when a save finishes while a game is in front ("Save failed" when it didn't), "Session
// ended" when a session the game is set to Ask for has ended (sessions.cpp), and the marker window `mark ask` opens.
// All are laid out alike, from the TOAST_* sizes: a tile beside a title over a line, then a row of buttons with the
// main one last on the right. A test copy (CLIPBACK_HOME) puts them on the monitor furthest right.
//
// Clip saved and Session ended never take the keyboard from the game, and a click anywhere else on them closes them.
// Each goes after its own time (TOAST_LIFE_*): Session saved or ended 15 s, Clip saved 5 s, 10 s once a save or share
// failed.
// A ring round the Dismiss X counts it down. While the mouse is on one the count holds at full, and starts again from
// full when the mouse leaves.
// Clip saved: Edit opens the file in the editor. Share does what share_decide picks and stays up while it runs, its
// line showing the step and percent over a thin bar, then what went on the clipboard; then it counts down again. A
// link with no host fails with the way to one (sign in with Cloudflare).
// Session ended: the game's icon, its name and length. Save saves it; Review opens it on the timeline, or the
// Library's unsaved sessions when several are waiting. Closed or ignored, the sessions it asked about are unsaved.
// The marker window takes the keyboard, and gives it back to the window that had it. Enter or Save keeps what was
// typed, as does clicking away; Escape leaves the marker plain. Tab steps through the colours.
// Every toast has a Dismiss button (an X) at the bottom left: it closes the toast as ignoring it would, and on the
// marker window it is Escape.
// With the Clip saved popup set to Small, Clip saved is a pill sized to its title: a round tick tile and "Clip saved",
// no file name and no buttons, gone after 3 s, and a click closes it. Its rounded ends are see-through, so it is a
// layered window drawn with its own alpha. Save failed is the full toast without its buttons. Popups in games picks
// another monitor for every toast while a game is in front.
// Clip saved, Session saved and Session ended have the game's grey art behind them, the window's own (game_art_grey):
// filling the toast and cut to fit, strongest at the right and faded out towards the left, as the window's. With no
// art, or Flat look, they have the plain panel.

enum ToastKind { TOAST_SAVED, TOAST_MARKER, TOAST_SESSION };
// The parts a click lands on: the buttons counted from the right, Dismiss at the left, then the marker's colours
// (TP_COLOUR + 1..6)
enum { TP_NONE, TP_MAIN, TP_SECOND, TP_DISMISS, TP_COLOUR = 10 };
enum { TR_TILE = -1, TR_FIELD = -2, TR_DIAL = -3 }; // more rectangles, for drawing

struct Toast {
    ToastKind kind;
    HWND      hwnd, edit, before; // the marker's note field, and the window that had the keyboard
    WNDPROC   edit_proc;
    wchar_t   title[64], line[MAX_PATH];
    char      path[MAX_PATH * 2]; // Clip saved: the file
    i64       marker;             // the marker being described
    u32       session;            // Session ended: the newest session asked about
    i32       more;               // and how many more
    u32      *tile_px;            // Session ended: the game's icon over the panel, tile_w x tile_h BGRA; null for none
    i32       tile_w, tile_h;
    u8       *art;                // the game's grey art behind it, art_w x art_h (game_art_grey, malloc); null for none
    i32       art_w, art_h;
    u8        colour;
    i32       hot;                // the part under the mouse
    bool      open, tracking, held; // held: the mouse is on it, so the countdown waits
    bool      brief;              // Clip saved over a game: no button row (toast_clip_now)
    bool      failed;             // Save failed: a red tile with an X
    bool      pill;               // small Clip saved: a pill, drawn by toast_pill_paint (the window is layered)
    u8        sharing;            // Clip saved's Share: TSH_* below
    u32       share;              // its share_start job
    i32       share_pct;          // the percent last drawn
    u64       start_ms;           // the countdown's start (GetTickCount64)
    u32       life_ms;            // how long it shows once the countdown starts (TOAST_LIFE_*)
    HBITMAP   base;               // the toast drawn without its dial, base_w x base_h; redrawn when dirty
    i32       base_w, base_h;
    bool      dirty;
    f32       scale;              // the fonts were made for this
    HFONT     font, strong, title_font, icons, small_icons, pill_font, pill_icons;
    HBRUSH    field;
};
static Toast g_toast_saved = {TOAST_SAVED}, g_toast_marker = {TOAST_MARKER}, g_toast_session = {TOAST_SESSION};
enum { TSH_NONE, TSH_PROBING, TSH_RUNNING, TSH_DONE };
static const UINT_PTR TOAST_TICK = 1;
static const UINT     TOAST_TICK_MS = 33; // the dial moves at 30 fps
// How long each toast shows, by how much it matters: a session most, a failure next, a clip less, a brief one over
// a game least
static const u32      TOAST_LIFE_SESSION = 15000, TOAST_LIFE_FAILED = 10000, TOAST_LIFE_CLIP = 5000, TOAST_LIFE_BRIEF = 3000;
static const f32      TOAST_DIAL = 24, TOAST_DIAL_RING = 2;
// The pill, at 100%: the tile's padding left and round it, the tile, the gap to the title and the padding right
static const f32      TOAST_PILL_PAD = 8, TOAST_PILL_TILE = 24, TOAST_PILL_GAP = 8, TOAST_PILL_RIGHT = 14;

// Clip saved and Session ended leave the keyboard where it is; the marker window takes it.
static bool toast_quiet(const Toast &t) { return t.kind != TOAST_MARKER; }

static void toast_session_close(i32 part);

static void toast_pill_paint(Toast &t);

// The whole toast is drawn again on its next paint (a part changed); the countdown's ticks only redraw the dial. The
// pill is drawn at once.
static void toast_redraw(Toast &t)
{
    if (t.pill) {
        toast_pill_paint(t);
        return;
    }
    t.dirty = true;
    InvalidateRect(t.hwnd, 0, FALSE);
}

// A toast that goes by itself starts its countdown, of life_ms
static void toast_countdown(Toast &t, u32 life_ms)
{
    t.life_ms = life_ms;
    t.start_ms = GetTickCount64();
    t.held = false;
    SetTimer(t.hwnd, TOAST_TICK, TOAST_TICK_MS, 0);
}

static COLORREF toast_rgb(u32 c) { return RGB((c >> 16) & 0xff, (c >> 8) & 0xff, c & 0xff); }
static i32 toast_px(const Toast &t, f32 v) { return (i32)(v * t.scale + 0.5f); }

// At 100%
static f32 toast_height(const Toast &t)
{
    if (t.pill) return TOAST_PILL_PAD + TOAST_PILL_TILE + TOAST_PILL_PAD;
    if (t.brief) return TOAST_PAD + TOAST_TILE + TOAST_PAD;
    return TOAST_PAD + TOAST_TILE + TOAST_GAP + (t.kind == TOAST_MARKER ? TOAST_FIELD_H + TOAST_GAP : 0) + TOAST_BUTTON_H + TOAST_PAD;
}

// A part's rectangle in client pixels
static RECT toast_rect(const Toast &t, i32 part)
{
    i32 w = toast_px(t, TOAST_W), pad = toast_px(t, TOAST_PAD), tile = toast_px(t, TOAST_TILE);
    i32 bw = toast_px(t, TOAST_BUTTON_W), bh = toast_px(t, TOAST_BUTTON_H), gap = toast_px(t, TOAST_BUTTON_GAP);
    i32 row = toast_px(t, toast_height(t) - TOAST_PAD - TOAST_BUTTON_H);
    if (part == TR_TILE) return {pad, pad, pad + tile, pad + tile};
    if (t.brief) return {}; // no buttons
    if (part == TR_DIAL) { // round the Dismiss X, inside its button
        i32 d = toast_px(t, TOAST_DIAL), x = pad + (bh - d) / 2, y = row + (bh - d) / 2;
        return {x, y, x + d, y + d};
    }
    if (part == TR_FIELD) {
        i32 y = toast_px(t, TOAST_PAD + TOAST_TILE + TOAST_GAP);
        return {pad, y, w - pad, y + toast_px(t, TOAST_FIELD_H)};
    }
    if (part == TP_MAIN) return {w - pad - bw, row, w - pad, row + bh};
    if (part == TP_SECOND) return {w - pad - 2 * bw - gap, row, w - pad - bw - gap, row + bh};
    if (part == TP_DISMISS) return {pad, row, pad + bh, row + bh};
    if (part > TP_COLOUR) {
        i32 d = toast_px(t, TOAST_DOT), x = pad + bh + toast_px(t, TOAST_GAP) + (part - TP_COLOUR - 1) * (d + toast_px(t, TOAST_DOT_GAP));
        i32 y = row + (bh - d) / 2;
        return {x, y, x + d, y + d};
    }
    return {};
}

static i32 toast_hit(const Toast &t, LPARAM lp)
{
    POINT p = {(i16)LOWORD(lp), (i16)HIWORD(lp)};
    if (t.brief) return TP_NONE;
    i32 parts[8], n = 0;
    parts[n++] = TP_MAIN;
    parts[n++] = TP_DISMISS;
    if (toast_quiet(t)) parts[n++] = TP_SECOND;
    else
        for (i32 k = 1; k <= 6; k++) parts[n++] = TP_COLOUR + k;
    for (i32 i = 0; i < n; i++) {
        RECT r = toast_rect(t, parts[i]);
        if (PtInRect(&r, p)) return parts[i];
    }
    return TP_NONE;
}

static void toast_hide(Toast &t)
{
    t.open = false;
    if (!t.hwnd) return;
    KillTimer(t.hwnd, TOAST_TICK);
    ShowWindow(t.hwnd, SW_HIDE);
}

// Closes the marker window. With keep, the marker takes the colour picked and the note typed.
static void toast_marker_close(bool keep)
{
    Toast &t = g_toast_marker;
    if (!t.open) return;
    if (keep) {
        wchar_t wide[128];
        char note[384];
        GetWindowTextW(t.edit, wide, COUNT(wide));
        wide_to_utf8(wide, note, sizeof note);
        markers_set(t.marker, t.colour, note);
        log_line("marker: %s%s%s", MARKER_COLOURS[t.colour].name, t.colour && note[0] ? ", " : "", note);
        ui_wake();
    }
    toast_hide(t);
    if (t.before && IsWindow(t.before)) SetForegroundWindow(t.before);
}

// Shapes are drawn four times the size and scaled down, which smooths their edges (GDI's own are jagged).
static const i32 TOAST_SS = 4;

// The Dismiss X, centred in r
static void toast_draw_x(Toast &t, HDC dc, RECT r)
{
    HGDIOBJ old = SelectObject(dc, t.small_icons);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, toast_rgb(t.hot == TP_DISMISS ? COL_TEXT : COL_DIM));
    const wchar_t cancel[] = {(wchar_t)0xE711, 0}; // Segoe MDL2: Cancel
    DrawTextW(dc, cancel, 1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, old);
}

// The pill's width for its title, in pixels
static i32 toast_pill_w(Toast &t)
{
    HDC dc = GetDC(0);
    HGDIOBJ old = SelectObject(dc, t.pill_font);
    SIZE ts = {};
    GetTextExtentPoint32W(dc, t.title, (i32)wcslen(t.title), &ts);
    SelectObject(dc, old);
    ReleaseDC(0, dc);
    return toast_px(t, TOAST_PILL_PAD + TOAST_PILL_TILE + TOAST_PILL_GAP + TOAST_PILL_RIGHT) + ts.cx;
}

// The pill, w x h, into a DIB of its own (premultiplied BGRA at *out; 0 when it can't be made): the shapes drawn
// four times the size and scaled down, then the tick and the title at their real size, then each pixel's alpha from
// the pill's own shape.
static HBITMAP toast_pill_bitmap(Toast &t, HDC screen, i32 w, i32 h, u32 **out)
{
    const i32 SS = TOAST_SS;
    HDC dc = CreateCompatibleDC(screen), big = CreateCompatibleDC(screen);
    BITMAPINFO bi = {};
    bi.bmiHeader = {sizeof bi.bmiHeader, w, -h, 1, 32, BI_RGB};
    u32 *px = 0;
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, (void **)&px, 0, 0);
    HBITMAP big_bmp = CreateCompatibleBitmap(screen, w * SS, h * SS);
    if (!bmp || !big_bmp || !px) {
        if (bmp) DeleteObject(bmp);
        if (big_bmp) DeleteObject(big_bmp);
        DeleteDC(dc), DeleteDC(big);
        return 0;
    }
    HGDIOBJ old_dc = SelectObject(dc, bmp), old_big = SelectObject(big, big_bmp);
    HGDIOBJ old_pen = SelectObject(big, GetStockObject(NULL_PEN));
    HGDIOBJ old_brush = SelectObject(big, GetStockObject(BLACK_BRUSH));
    auto brush = [&](u32 rgb) {
        HGDIOBJ b = SelectObject(big, CreateSolidBrush(toast_rgb(rgb)));
        if (b != GetStockObject(BLACK_BRUSH) && b != GetStockObject(WHITE_BRUSH)) DeleteObject(b);
    };
    RECT all = {0, 0, w * SS, h * SS};
    i32 edge = toast_px(t, 1) * SS, pad = toast_px(t, TOAST_PILL_PAD), tile = toast_px(t, TOAST_PILL_TILE);
    // The shape alone, white on black, gives the alpha
    FillRect(big, &all, (HBRUSH)GetStockObject(BLACK_BRUSH));
    brush(0xffffff);
    RoundRect(big, 0, 0, w * SS + 1, h * SS + 1, h * SS, h * SS);
    SetStretchBltMode(dc, HALFTONE);
    SetBrushOrgEx(dc, 0, 0, 0);
    StretchBlt(dc, 0, 0, w, h, big, 0, 0, w * SS, h * SS, SRCCOPY);
    GdiFlush();
    u8 *alpha = (u8 *)HeapAlloc(GetProcessHeap(), 0, (size_t)w * h);
    if (alpha)
        for (i32 i = 0; i < w * h; i++) alpha[i] = (u8)(px[i] & 0xff);
    // Then the pill: the edge colour round the panel, and the round accent tile
    FillRect(big, &all, (HBRUSH)GetStockObject(BLACK_BRUSH));
    brush(COL_LINE);
    RoundRect(big, 0, 0, w * SS + 1, h * SS + 1, h * SS, h * SS);
    brush(COL_PANEL);
    RoundRect(big, edge, edge, w * SS - edge + 1, h * SS - edge + 1, h * SS - 2 * edge, h * SS - 2 * edge);
    brush(COL_ACCENT);
    i32 ty = (h - tile) / 2;
    Ellipse(big, pad * SS, ty * SS, (pad + tile) * SS + 1, (ty + tile) * SS + 1);
    StretchBlt(dc, 0, 0, w, h, big, 0, 0, w * SS, h * SS, SRCCOPY);
    // The tick and the title
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ old_font = SelectObject(dc, t.pill_icons);
    SetTextColor(dc, toast_rgb(COL_ACCENT_INK));
    RECT tr = {pad, ty, pad + tile, ty + tile};
    const wchar_t tick[] = {(wchar_t)0xE73E, 0}; // Segoe MDL2: CheckMark
    DrawTextW(dc, tick, 1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, t.pill_font);
    SetTextColor(dc, toast_rgb(COL_TEXT));
    RECT nr = {pad + tile + toast_px(t, TOAST_PILL_GAP), 0, w, h};
    DrawTextW(dc, t.title, -1, &nr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, old_font);
    GdiFlush();
    // Premultiplied: black outside the shape scales the edge down with its alpha
    for (i32 i = 0; i < w * h; i++) {
        u32 a = alpha ? alpha[i] : 255, c = px[i];
        u32 r = ((c >> 16) & 0xff) * a / 255, g = ((c >> 8) & 0xff) * a / 255, b = (c & 0xff) * a / 255;
        px[i] = a << 24 | r << 16 | g << 8 | b;
    }
    if (alpha) HeapFree(GetProcessHeap(), 0, alpha);
    DeleteObject(SelectObject(big, old_brush));
    SelectObject(big, old_pen);
    SelectObject(big, old_big);
    SelectObject(dc, old_dc);
    DeleteObject(big_bmp);
    DeleteDC(big);
    DeleteDC(dc);
    *out = px;
    return bmp;
}

// The pill onto its layered window
static void toast_pill_paint(Toast &t)
{
    RECT wr;
    if (!t.hwnd || !GetWindowRect(t.hwnd, &wr)) return;
    i32 w = wr.right - wr.left, h = wr.bottom - wr.top;
    if (w <= 0 || h <= 0) return;
    HDC screen = GetDC(0);
    u32 *px;
    HBITMAP bmp = toast_pill_bitmap(t, screen, w, h, &px);
    if (bmp) {
        HDC dc = CreateCompatibleDC(screen);
        HGDIOBJ old = SelectObject(dc, bmp);
        POINT at = {wr.left, wr.top}, zero = {0, 0};
        SIZE size = {w, h};
        BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        UpdateLayeredWindow(t.hwnd, screen, &at, &size, dc, &zero, 0, &blend, ULW_ALPHA);
        SelectObject(dc, old);
        DeleteDC(dc);
        DeleteObject(bmp);
    }
    ReleaseDC(0, screen);
}

// The window as a pill (layered, its corners left to its alpha) or as the other toasts (Windows 11's rounded
// corners and edge)
static void toast_set_pill(Toast &t, bool pill)
{
    t.pill = pill;
    LONG_PTR ex = GetWindowLongPtrW(t.hwnd, GWL_EXSTYLE);
    LONG_PTR want = pill ? ex | WS_EX_LAYERED : ex & ~(LONG_PTR)WS_EX_LAYERED;
    if (want != ex) SetWindowLongPtrW(t.hwnd, GWL_EXSTYLE, want);
    i32 round = pill ? 1 /* DWMWCP_DONOTROUND */ : 2 /* DWMWCP_ROUND */;
    COLORREF edge = pill ? 0xfffffffe /* DWMWA_COLOR_NONE */ : toast_rgb(COL_LINE);
    DwmSetWindowAttribute(t.hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &round, sizeof round);
    DwmSetWindowAttribute(t.hwnd, 34 /* DWMWA_BORDER_COLOR */, &edge, sizeof edge);
    if (!pill && want != ex) RedrawWindow(t.hwnd, 0, 0, RDW_ERASE | RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN);
}

// The game's art behind the toast into `big` (SS times the toast's size), over the panel, faded across it as the
// window's is (art_fade): strongest at the right, none at the left. It fills the toast, cut to fit, and is smoothed
// between its texels.
static void toast_draw_art(Toast &t, HDC big, RECT r, i32 SS)
{
    i32 w = r.right, h = r.bottom, aw = t.art_w, ah = t.art_h;
    if (!t.art || w <= 0 || h <= 0 || aw < 2 || ah < 2) return;
    u32 *px = (u32 *)malloc((size_t)w * h * 4);
    if (!px) return;
    f32 su = 1, sv = 1; // the part of the picture used: its middle, of the toast's shape
    if ((f32)w / h > (f32)aw / ah) sv = ((f32)aw / ah) / ((f32)w / h);
    else su = ((f32)w / h) / ((f32)aw / ah);
    u32 ground = COL_PANEL;
    for (i32 y = 0; y < h; y++) {
        f32 fy = ((1 - sv) / 2 + sv * (y + 0.5f) / h) * ah - 0.5f;
        i32 y0 = art_clamp((i32)floorf(fy), 0, ah - 1), y1 = art_clamp(y0 + 1, 0, ah - 1);
        f32 dy = fy - floorf(fy);
        for (i32 x = 0; x < w; x++) {
            f32 fx = ((1 - su) / 2 + su * (x + 0.5f) / w) * aw - 0.5f;
            i32 x0 = art_clamp((i32)floorf(fx), 0, aw - 1), x1 = art_clamp(x0 + 1, 0, aw - 1);
            f32 dx = fx - floorf(fx), a = art_fade((x + 0.5f) / w);
            const u8 *g = t.art;
            f32 v = (g[y0 * aw + x0] * (1 - dx) + g[y0 * aw + x1] * dx) * (1 - dy) +
                    (g[y1 * aw + x0] * (1 - dx) + g[y1 * aw + x1] * dx) * dy;
            u32 out = 0;
            for (i32 sh = 0; sh < 24; sh += 8) {
                f32 c = ((ground >> sh) & 255) * (1 - a) + v * a;
                out |= (u32)(c + 0.5f) << sh;
            }
            px[y * w + x] = out;
        }
    }
    BITMAPINFO bi = {};
    bi.bmiHeader = {sizeof bi.bmiHeader, w, -h, 1, 32, BI_RGB};
    SetStretchBltMode(big, COLORONCOLOR);
    StretchDIBits(big, 0, 0, w * SS, h * SS, 0, 0, w, h, px, &bi, DIB_RGB_COLORS, SRCCOPY);
    free(px);
}

// The toast without its dial, into dc (r, the client size): the panel and the game's art, the shapes, then the text
// and the icons at their real size.
static void toast_draw(Toast &t, HDC wdc, HDC dc, RECT r)
{
    const i32 SS = TOAST_SS;
    HDC big = CreateCompatibleDC(wdc);
    HBITMAP big_bmp = CreateCompatibleBitmap(wdc, r.right * SS, r.bottom * SS);
    HGDIOBJ old_big = SelectObject(big, big_bmp);
    auto up = [&](RECT q) { return RECT{q.left * SS, q.top * SS, q.right * SS, q.bottom * SS}; };
    auto brush = [&](u32 rgb) { DeleteObject(SelectObject(big, CreateSolidBrush(toast_rgb(rgb)))); };
    auto rounded = [&](i32 part, f32 radius) {
        RECT q = up(toast_rect(t, part));
        i32 d = toast_px(t, 2 * radius) * SS;
        RoundRect(big, q.left, q.top, q.right + 1, q.bottom + 1, d, d);
    };
    bool saved = t.kind == TOAST_SAVED, session = t.kind == TOAST_SESSION, quiet = toast_quiet(t);

    RECT all = {0, 0, r.right * SS, r.bottom * SS};
    HBRUSH bg = CreateSolidBrush(toast_rgb(COL_PANEL));
    FillRect(big, &all, bg);
    DeleteObject(bg);
    if (!t.pill) toast_draw_art(t, big, r, SS);
    HGDIOBJ old_pen = SelectObject(big, GetStockObject(NULL_PEN));
    HGDIOBJ old_brush = SelectObject(big, CreateSolidBrush(0));
    // The tile: the accent behind a tick (red behind an X when the save failed), the game's icon (a game glyph on a
    // soft accent without one), or the marker's colour behind a flag
    brush(saved && t.failed ? COL_RED : saved ? COL_ACCENT : session ? COL_ACCENT_SOFT : MARKER_COLOURS[t.colour].rgb);
    rounded(TR_TILE, TOAST_ROUND);
    if (session && t.tile_px) {
        RECT q = up(toast_rect(t, TR_TILE));
        i32 d = toast_px(t, 2 * TOAST_ROUND) * SS;
        HRGN clip = CreateRoundRectRgn(q.left, q.top, q.right + 1, q.bottom + 1, d, d);
        SelectClipRgn(big, clip);
        BITMAPINFO bi = {};
        bi.bmiHeader = {sizeof bi.bmiHeader, t.tile_w, -t.tile_h, 1, 32, BI_RGB};
        SetStretchBltMode(big, HALFTONE);
        SetBrushOrgEx(big, 0, 0, 0);
        StretchDIBits(big, q.left, q.top, q.right - q.left, q.bottom - q.top, 0, 0, t.tile_w, t.tile_h, t.tile_px, &bi,
                      DIB_RGB_COLORS, SRCCOPY);
        SelectClipRgn(big, 0);
        DeleteObject(clip);
    }
    if (t.kind == TOAST_MARKER) {
        brush(COL_PANEL2);
        rounded(TR_FIELD, TOAST_BUTTON_ROUND);
        for (i32 k = 1; k <= 6; k++) {
            RECT c = up(toast_rect(t, TP_COLOUR + k));
            if (t.colour == k) { // the one picked: a ring of the text colour around it
                i32 o = toast_px(t, 4) * SS, in = toast_px(t, 2) * SS;
                brush(COL_TEXT);
                Ellipse(big, c.left - o, c.top - o, c.right + o + 1, c.bottom + o + 1);
                brush(COL_PANEL);
                Ellipse(big, c.left - in, c.top - in, c.right + in + 1, c.bottom + in + 1);
            }
            brush(MARKER_COLOURS[k].rgb);
            i32 grow = t.hot == TP_COLOUR + k ? toast_px(t, 1) * SS : 0;
            Ellipse(big, c.left - grow, c.top - grow, c.right + grow + 1, c.bottom + grow + 1);
        }
    }
    // The buttons, as the window's own: the main one in the accent (secondary while it's Share's Cancel), the other
    // secondary
    bool cancel = t.sharing == TSH_PROBING || t.sharing == TSH_RUNNING;
    if (!t.brief && cancel) brush(t.hot == TP_MAIN ? COL_HOVER : COL_PANEL2);
    else if (!t.brief) brush(t.hot == TP_MAIN && !t.sharing ? COL_ACCENT_HOVER : COL_ACCENT);
    if (!t.brief) rounded(TP_MAIN, TOAST_BUTTON_ROUND);
    i32 cancel_x = 0; // Cancel's label, right of its ring: an X in a ring filled to Share's percent
    if (!t.brief && cancel) {
        RECT m = toast_rect(t, TP_MAIN);
        SIZE ts = {};
        HGDIOBJ of = SelectObject(dc, t.strong);
        GetTextExtentPoint32W(dc, L"Cancel", 6, &ts);
        SelectObject(dc, of);
        i32 rr = toast_px(t, 7), gap = toast_px(t, 6);
        i32 x0 = m.left + (m.right - m.left - 2 * rr - gap - ts.cx) / 2;
        cancel_x = x0 + 2 * rr + gap;
        i32 cx = (x0 + rr) * SS, cy = (m.top + m.bottom) / 2 * SS, R = rr * SS, th = toast_px(t, 2) * SS;
        HGDIOBJ ob = SelectObject(big, GetStockObject(NULL_BRUSH));
        HPEN track = CreatePen(PS_SOLID, th, toast_rgb(COL_FAINT));
        HGDIOBJ op = SelectObject(big, track);
        Ellipse(big, cx - R, cy - R, cx + R, cy + R);
        HPEN arc = CreatePen(PS_SOLID, th, toast_rgb(COL_ACCENT));
        if (t.share_pct > 0) {
            SelectObject(big, arc);
            SetArcDirection(big, AD_CLOCKWISE);
            f32 a = 2 * 3.14159265f * (t.share_pct >= 100 ? 0.9999f : t.share_pct / 100.f);
            Arc(big, cx - R, cy - R, cx + R, cy + R, cx, cy - R, cx + (i32)(R * sinf(a)), cy - (i32)(R * cosf(a)));
        }
        i32 lw = (i32)(1.5f * t.scale * SS + 0.5f), x = (i32)(R * 0.42f);
        HPEN ink = CreatePen(PS_SOLID, lw, toast_rgb(COL_TEXT));
        SelectObject(big, ink);
        MoveToEx(big, cx - x, cy - x, 0);
        LineTo(big, cx + x, cy + x);
        MoveToEx(big, cx - x, cy + x, 0);
        LineTo(big, cx + x, cy - x);
        SelectObject(big, op);
        SelectObject(big, ob);
        DeleteObject(track);
        DeleteObject(arc);
        DeleteObject(ink);
    }
    if (quiet && !t.brief) {
        brush(t.hot == TP_SECOND ? COL_HOVER : COL_PANEL2);
        rounded(TP_SECOND, TOAST_BUTTON_ROUND);
    }
    if (t.hot == TP_DISMISS) { // a ghost button: only its X until the mouse is on it
        brush(COL_HOVER);
        rounded(TP_DISMISS, TOAST_BUTTON_ROUND);
    }
    DeleteObject(SelectObject(big, old_brush));
    SelectObject(big, old_pen);

    SetStretchBltMode(dc, HALFTONE);
    SetBrushOrgEx(dc, 0, 0, 0);
    StretchBlt(dc, 0, 0, r.right, r.bottom, big, 0, 0, r.right * SS, r.bottom * SS, SRCCOPY);
    SetBkMode(dc, TRANSPARENT);

    HGDIOBJ old_font = SelectObject(dc, t.icons);
    SetTextColor(dc, session ? toast_rgb(COL_ACCENT) : saved && !t.failed ? toast_rgb(COL_ACCENT_INK) : RGB(255, 255, 255));
    RECT tile = toast_rect(t, TR_TILE);
    // Segoe MDL2: Cancel, CheckMark, Game, Flag
    const wchar_t glyph[] = {(wchar_t)(saved && t.failed ? 0xE711 : saved ? 0xE73E : session ? 0xE7FC : 0xE7C1), 0};
    if (!(session && t.tile_px)) DrawTextW(dc, glyph, 1, &tile, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    // The title over its line, centred on the tile
    i32 tx = tile.right + toast_px(t, TOAST_GAP), mid = (tile.top + tile.bottom) / 2;
    i32 right = r.right - toast_px(t, TOAST_PAD);
    SelectObject(dc, t.title_font);
    SetTextColor(dc, toast_rgb(COL_TEXT));
    RECT t1 = {tx, tile.top - toast_px(t, 4), right, mid + toast_px(t, 1)};
    DrawTextW(dc, t.title, -1, &t1, DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, t.font);
    SetTextColor(dc, toast_rgb(COL_DIM));
    RECT t2 = {tx, mid + toast_px(t, 3), right, tile.bottom + toast_px(t, 4)};
    DrawTextW(dc, t.line, -1, &t2, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    if (t.sharing == TSH_PROBING || t.sharing == TSH_RUNNING) { // Share's progress, a thin bar under the line
        i32 y = tile.bottom + toast_px(t, 6), bh = toast_px(t, 3) > 2 ? toast_px(t, 3) : 2;
        RECT track = {tx, y, right, y + bh}, done = {tx, y, tx + (right - tx) * t.share_pct / 100, y + bh};
        HBRUSH b1 = CreateSolidBrush(toast_rgb(COL_HOVER)), b2 = CreateSolidBrush(toast_rgb(COL_ACCENT));
        FillRect(dc, &track, b1);
        FillRect(dc, &done, b2);
        DeleteObject(b1);
        DeleteObject(b2);
    }

    RECT dismiss = toast_rect(t, TP_DISMISS);
    if (!quiet) toast_draw_x(t, dc, dismiss); // a quiet toast's X goes in with its dial
    if (t.brief) {
        SelectObject(dc, old_font);
        SelectObject(big, old_big);
        DeleteObject(big_bmp);
        DeleteDC(big);
        return;
    }

    RECT main = toast_rect(t, TP_MAIN);
    SelectObject(dc, t.strong);
    if (cancel) {
        SetTextColor(dc, toast_rgb(COL_TEXT));
        main.left = cancel_x;
        DrawTextW(dc, L"Cancel", -1, &main, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    } else {
        SetTextColor(dc, toast_rgb(COL_ACCENT_INK));
        const wchar_t *main_label = !saved ? L"Save" : t.sharing == TSH_DONE ? L"Copied" : L"Share";
        DrawTextW(dc, main_label, -1, &main, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    if (quiet) {
        RECT second = toast_rect(t, TP_SECOND);
        SelectObject(dc, t.font);
        SetTextColor(dc, toast_rgb(COL_TEXT));
        DrawTextW(dc, saved ? L"Edit" : L"Review", -1, &second, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    SelectObject(dc, old_font);
    SelectObject(big, old_big);
    DeleteObject(big_bmp);
    DeleteDC(big);
}

// The countdown dial onto wdc: a thin ring in the accent round the Dismiss X, which empties clockwise from the top
// over the toast's life (when it ends, the toast is dismissed), full while the mouse is on it. Round it and inside it
// is the toast as drawn in `under` (the panel, the art, the Dismiss button's hover).
static void toast_draw_dial(Toast &t, HDC wdc, HDC under)
{
    const i32 SS = TOAST_SS;
    RECT d = toast_rect(t, TR_DIAL);
    i32 w = d.right - d.left, h = d.bottom - d.top, W = w * SS, H = h * SS;
    f32 left = t.held ? 1 : 1 - (f32)(GetTickCount64() - t.start_ms) / t.life_ms;
    left = left < 0 ? 0 : left;
    HDC big = CreateCompatibleDC(wdc), dc = CreateCompatibleDC(wdc);
    HBITMAP big_bmp = CreateCompatibleBitmap(wdc, W, H), bmp = CreateCompatibleBitmap(wdc, w, h);
    HGDIOBJ old_big = SelectObject(big, big_bmp), old_bmp = SelectObject(dc, bmp);
    SetStretchBltMode(big, COLORONCOLOR);
    StretchBlt(big, 0, 0, W, H, under, d.left, d.top, w, h, SRCCOPY);
    HGDIOBJ old_pen = SelectObject(big, GetStockObject(NULL_PEN));
    HGDIOBJ old_brush = SelectObject(big, CreateSolidBrush(toast_rgb(COL_PANEL2)));
    auto brush = [&](u32 rgb) { DeleteObject(SelectObject(big, CreateSolidBrush(toast_rgb(rgb)))); };
    Ellipse(big, 0, 0, W + 1, H + 1); // the track
    brush(COL_ACCENT);
    if (left >= 0.999f) Ellipse(big, 0, 0, W + 1, H + 1);
    else if (left > 0) { // a pie from the top round to where the time left ends (GDI's pies run anticlockwise)
        f32 a = 3.14159265f * (0.5f - 2 * left), cx = W / 2.f, cy = H / 2.f;
        Pie(big, 0, 0, W + 1, H + 1, (i32)(cx + cx * cosf(a)), (i32)(cy - cy * sinf(a)), (i32)cx, 0);
    }
    i32 ring = toast_px(t, TOAST_DIAL_RING) * SS; // hollowed out to a ring: the toast again inside it
    HRGN inside = CreateEllipticRgn(ring, ring, W - ring + 1, H - ring + 1);
    SelectClipRgn(big, inside);
    StretchBlt(big, 0, 0, W, H, under, d.left, d.top, w, h, SRCCOPY);
    SelectClipRgn(big, 0);
    DeleteObject(inside);
    DeleteObject(SelectObject(big, old_brush));
    SelectObject(big, old_pen);
    SetStretchBltMode(dc, HALFTONE);
    SetBrushOrgEx(dc, 0, 0, 0);
    StretchBlt(dc, 0, 0, w, h, big, 0, 0, W, H, SRCCOPY);
    RECT x = {0, 0, w, h};
    toast_draw_x(t, dc, x);
    BitBlt(wdc, d.left, d.top, w, h, dc, 0, 0, SRCCOPY);
    SelectObject(big, old_big);
    SelectObject(dc, old_bmp);
    DeleteObject(big_bmp);
    DeleteObject(bmp);
    DeleteDC(big);
    DeleteDC(dc);
}

// The toast is kept drawn in base, so the dial's ticks only copy it out and draw the dial. Each part reaches the
// window once, opaque, so nothing flickers.
static void toast_paint(Toast &t)
{
    PAINTSTRUCT ps;
    HDC wdc = BeginPaint(t.hwnd, &ps);
    RECT r;
    GetClientRect(t.hwnd, &r);
    if (t.base && (t.base_w != r.right || t.base_h != r.bottom)) DeleteObject(t.base), t.base = 0;
    if (!t.base) {
        t.base = CreateCompatibleBitmap(wdc, r.right, r.bottom);
        t.base_w = r.right;
        t.base_h = r.bottom;
        t.dirty = true;
    }
    HDC dc = CreateCompatibleDC(wdc);
    HGDIOBJ old_bmp = SelectObject(dc, t.base);
    if (t.dirty) {
        toast_draw(t, wdc, dc, r);
        t.dirty = false;
    }
    bool dial = toast_quiet(t) && !t.brief;
    if (dial) {
        RECT d = toast_rect(t, TR_DIAL);
        ExcludeClipRect(wdc, d.left, d.top, d.right, d.bottom);
    }
    BitBlt(wdc, 0, 0, r.right, r.bottom, dc, 0, 0, SRCCOPY);
    if (dial) {
        SelectClipRgn(wdc, 0);
        toast_draw_dial(t, wdc, dc);
    }
    SelectObject(dc, old_bmp);
    DeleteDC(dc);
    EndPaint(t.hwnd, &ps);
}

// The marker's note field: Enter keeps, Escape doesn't, Tab steps through the colours (and none).
static LRESULT CALLBACK toast_edit_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    Toast &t = g_toast_marker;
    if (msg == WM_KEYDOWN && wp == VK_RETURN) { toast_marker_close(true); return 0; }
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE) { toast_marker_close(false); return 0; }
    if (msg == WM_KEYDOWN && wp == VK_TAB) {
        i32 n = COUNT(MARKER_COLOURS), step = GetKeyState(VK_SHIFT) < 0 ? n - 1 : 1;
        t.colour = (u8)((t.colour + step) % n);
        toast_redraw(t);
        return 0;
    }
    if (msg == WM_CHAR && (wp == '\r' || wp == '\t' || wp == 27)) return 0; // no beep for the keys taken above
    return CallWindowProcW(t.edit_proc, hwnd, msg, wp, lp);
}

// Clip saved's Share. The library's card is used when it has the file already; else ffprobe reads it on a thread
// (g_toast_probe) while the toast says so.
static struct {
    Path          ffprobe, file;
    MediaInfo     m;
    char          game[64];
    volatile LONG state; // 0 idle, 1 running, 2 read, 3 failed
} g_toast_probe;

static DWORD WINAPI toast_probe_thread(void *)
{
    bool ok = lib_probe(g_toast_probe.ffprobe, g_toast_probe.file, &g_toast_probe.m, g_toast_probe.game,
                        sizeof g_toast_probe.game);
    InterlockedExchange(&g_toast_probe.state, ok ? 2 : 3);
    return 0;
}

static void toast_line(Toast &t, const char *text)
{
    utf8_to_wide(text, t.line, COUNT(t.line));
    toast_redraw(t);
}

// The end of a Share: its line says what happened, and the countdown starts again
static void toast_share_end(Toast &t, bool ok, const char *text)
{
    t.sharing = (u8)(ok ? TSH_DONE : TSH_NONE);
    toast_line(t, text);
    toast_countdown(t, ok ? TOAST_LIFE_CLIP : TOAST_LIFE_FAILED);
}

// Starts the Share once the clip is known: what share_decide picks for the whole file
static void toast_share_start(Toast &t, const MediaInfo &m, const char *game, u64 lib_key)
{
    ShareDecision d = share_decide(m, 0);
    if (d.kind == SHARE_LINK && !d.link_ok) {
        toast_share_end(t, false, d.link_why);
        return;
    }
    wchar_t w[MAX_PATH];
    utf8_to_wide(t.path, w, MAX_PATH);
    char name[200];
    const char *slash = strrchr(t.path, '\\');
    str_set(name, slash ? slash + 1 : t.path);
    if (char *dot = strrchr(name, '.')) *dot = 0;
    t.share = share_start(d.kind, w, m, 0, name, game, lib_key);
    if (!t.share) {
        toast_share_end(t, false, "Share is busy: try again in a moment");
        return;
    }
    t.sharing = TSH_RUNNING;
    t.share_pct = 0;
    log_line("toast: share %s, job %u", t.path, t.share);
}

static void toast_share(Toast &t)
{
    t.held = true; // the dial stays full while it runs
    t.share_pct = 0;
    t.sharing = TSH_PROBING;
    Library &lib = g_app.library;
    wchar_t want[MAX_PATH];
    utf8_to_wide(t.path, want, MAX_PATH);
    for (i32 i = 0; i < lib.item_count; i++) {
        const LibItem &it = lib.items[i];
        if (!it.probed || _wcsicmp(lib_item_path(it).s, want)) continue;
        toast_line(t, "Getting it ready");
        toast_share_start(t, it.info, it.info.game, it.key);
        return;
    }
    Path ffmpeg;
    if (g_toast_probe.state == 1 || !ffmpeg_find(g_app.config.ffmpeg, &ffmpeg)) {
        toast_share_end(t, false, g_toast_probe.state == 1 ? "Busy: try again in a moment" : "Share needs ffmpeg");
        return;
    }
    g_toast_probe.ffprobe = ffprobe_for(ffmpeg);
    wcsncpy(g_toast_probe.file.s, want, MAX_PATH - 1);
    g_toast_probe.state = 1;
    HANDLE th = CreateThread(0, 0, toast_probe_thread, 0, 0, 0);
    if (!th) {
        g_toast_probe.state = 0;
        toast_share_end(t, false, "Couldn't read the clip");
        return;
    }
    CloseHandle(th);
    toast_line(t, "Getting it ready");
}

// Each tick while Share runs: the probe's answer, then the job's step and percent, then its end
static void toast_share_poll(Toast &t)
{
    if (t.sharing == TSH_PROBING) {
        LONG st = g_toast_probe.state;
        if (st == 3) {
            g_toast_probe.state = 0;
            toast_share_end(t, false, "Couldn't read the clip");
        } else if (st == 2) {
            g_toast_probe.state = 0;
            toast_share_start(t, g_toast_probe.m, g_toast_probe.game, 0);
        }
        return;
    }
    ShareJobView j = share_job(t.share);
    if (!j.id) toast_share_end(t, false, "Couldn't share it");
    else if (j.state == SHARE_FAILED) toast_share_end(t, false, j.error);
    else if (j.state == SHARE_DONE) {
        char done[64];
        str_set(done, j.kind == SHARE_LINK ? "Link copied" : "Copied, ready to paste");
        toast_share_end(t, true, j.error[0] ? j.error : done);
        if (j.kind != SHARE_LINK) editor_refresh_shares();
        ui_wake();
    } else {
        i32 pct = (i32)(j.progress * 100 + 0.5f);
        if (pct == t.share_pct && t.line[0] != L'G') return;
        t.share_pct = pct;
        char line[64];
        snprintf(line, sizeof line, "%s %d%%", j.verb[0] ? j.verb : "Sharing", pct);
        toast_line(t, line);
    }
}

static LRESULT CALLBACK toast_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    Toast *tp = (Toast *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!tp) return DefWindowProcW(hwnd, msg, wp, lp);
    Toast &t = *tp;
    bool saved = t.kind == TOAST_SAVED, quiet = toast_quiet(t);
    switch (msg) {
    case WM_MOUSEACTIVATE:
        if (quiet) return MA_NOACTIVATE; // the game keeps the keyboard
        break;
    case WM_PAINT: toast_paint(t); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && (HWND)wp == hwnd) { SetCursor(LoadCursorW(0, (LPCWSTR)IDC_ARROW)); return TRUE; }
        break;
    case WM_CTLCOLOREDIT:
        SetTextColor((HDC)wp, toast_rgb(COL_TEXT));
        SetBkColor((HDC)wp, toast_rgb(COL_PANEL2));
        return (LRESULT)t.field;
    case WM_MOUSEMOVE: {
        if (!t.tracking) {
            TRACKMOUSEEVENT tme = {sizeof tme, TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tme);
            t.tracking = true;
        }
        i32 hot = toast_hit(t, lp);
        if (hot != t.hot) {
            t.hot = hot;
            toast_redraw(t);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        t.tracking = false;
        t.hot = TP_NONE;
        toast_redraw(t);
        return 0;
    case WM_LBUTTONUP: {
        i32 k = toast_hit(t, lp);
        if (saved) {
            if (k == TP_MAIN && t.sharing == TSH_NONE) { // Share, in place
                log_line("toast: share %s", t.path);
                toast_share(t);
                return 0;
            }
            if (k == TP_MAIN && t.sharing == TSH_RUNNING) { // Cancel: the poll then ends it as "Cancelled"
                log_line("toast: share %u cancelled", t.share);
                share_cancel(t.share);
                return 0;
            }
            if (k == TP_MAIN && t.sharing == TSH_PROBING) { // the probe's answer is left unread
                toast_share_end(t, false, "Cancelled");
                return 0;
            }
            toast_hide(t);
            if (k == TP_SECOND) {
                log_line("toast: edit %s", t.path);
                ui_show(SCREEN_EDITOR);
                editor_open_file(t.path);
                g_app.editor.tab = 0;
            }
        } else if (t.kind == TOAST_SESSION) {
            toast_session_close(k);
        } else if (k == TP_MAIN) {
            toast_marker_close(true);
        } else if (k == TP_DISMISS) {
            toast_marker_close(false);
        } else if (k > TP_COLOUR) {
            u8 c = (u8)(k - TP_COLOUR);
            t.colour = t.colour == c ? 0 : c; // a second click takes the colour off
            toast_redraw(t);
            SetFocus(t.edit);
        }
        return 0;
    }
    case WM_TIMER: { // the countdown's tick: held at full while the mouse is on it, from full again once it leaves
        if (t.sharing == TSH_PROBING || t.sharing == TSH_RUNNING) { // held until Share ends
            toast_share_poll(t);
            return 0;
        }
        POINT p;
        RECT r, d = toast_rect(t, TR_DIAL);
        GetCursorPos(&p);
        GetWindowRect(hwnd, &r);
        u64 now = GetTickCount64();
        bool on = PtInRect(&r, p) != 0;
        if (on || t.held) t.start_ms = now;
        if (on != t.held) t.held = on;
        else if (on) return 0; // still held: the dial is already full
        if (now - t.start_ms >= t.life_ms) {
            if (t.kind == TOAST_SESSION) toast_session_close(TP_NONE);
            else toast_hide(t);
            return 0;
        }
        if (!t.brief) InvalidateRect(hwnd, &d, FALSE);
        return 0;
    }
    case WM_ACTIVATE:
        if (quiet) break;
        if (LOWORD(wp) == WA_INACTIVE) toast_marker_close(true); // clicked away: what was typed is kept
        else SetFocus(t.edit);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static bool toast_window(Toast &t)
{
    if (t.hwnd) return true;
    HINSTANCE inst = GetModuleHandleW(0);
    static bool registered;
    if (!registered) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = toast_proc;
        wc.hInstance = inst;
        wc.lpszClassName = L"clipback_toast";
        RegisterClassW(&wc);
        registered = true;
    }
    DWORD ex = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | (toast_quiet(t) ? WS_EX_NOACTIVATE : 0);
    t.hwnd = CreateWindowExW(ex, L"clipback_toast", L"" APP_NAME, WS_POPUP | WS_CLIPCHILDREN, 0, 0, 1, 1, 0, 0, inst, 0);
    if (!t.hwnd) return false;
    SetWindowLongPtrW(t.hwnd, GWLP_USERDATA, (LONG_PTR)&t);
    // Left out of every capture, ours included, so a toast over a game never ends up in a clip. Windows before
    // 10 2004 refuse it and show the toast as before.
    SetWindowDisplayAffinity(t.hwnd, 0x11 /* WDA_EXCLUDEFROMCAPTURE */);
    // Corners rounded by Windows 11 (TOAST_ROUND), which smooths them; a window region's are jagged. Windows 10
    // leaves them square.
    i32 round = 2; // DWMWCP_ROUND
    COLORREF edge = toast_rgb(COL_LINE);
    DwmSetWindowAttribute(t.hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &round, sizeof round);
    DwmSetWindowAttribute(t.hwnd, 34 /* DWMWA_BORDER_COLOR */, &edge, sizeof edge);
    if (t.kind == TOAST_MARKER) {
        t.field = CreateSolidBrush(toast_rgb(COL_PANEL2));
        t.edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 1, 1, t.hwnd, 0, inst, 0);
        SendMessageW(t.edit, EM_SETLIMITTEXT, 80, 0);
        SendMessageW(t.edit, EM_SETCUEBANNER, TRUE, (LPARAM)L"Note");
        t.edit_proc = (WNDPROC)SetWindowLongPtrW(t.edit, GWLP_WNDPROC, (LONG_PTR)toast_edit_proc);
    }
    return true;
}

static void toast_fonts(Toast &t, f32 s)
{
    if (t.font && t.scale == s) return;
    if (t.font) DeleteObject(t.font), DeleteObject(t.strong), DeleteObject(t.title_font), DeleteObject(t.icons),
                  DeleteObject(t.small_icons), DeleteObject(t.pill_font), DeleteObject(t.pill_icons);
    t.scale = s;
    auto make = [&](f32 px, i32 weight, const wchar_t *face) {
        return CreateFontW(-(i32)(px * s + 0.5f), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                           CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
    };
    // The window's own sizes: g_font, g_font_bold and g_font_big
    t.font = make(14, FW_NORMAL, L"Segoe UI");
    t.strong = make(14, FW_SEMIBOLD, L"Segoe UI");
    t.title_font = make(18, FW_SEMIBOLD, L"Segoe UI");
    t.icons = make(18, FW_NORMAL, L"Segoe MDL2 Assets");
    t.small_icons = make(12, FW_NORMAL, L"Segoe MDL2 Assets");
    t.pill_font = make(13, FW_SEMIBOLD, L"Segoe UI");
    t.pill_icons = make(12, FW_NORMAL, L"Segoe MDL2 Assets");
    if (t.edit) SendMessageW(t.edit, WM_SETFONT, (WPARAM)t.font, TRUE);
}

// The monitor an output index is on (monitor_mode); 0 when there's no such output now.
static HMONITOR popup_output_monitor(i32 out)
{
    i32 w, h, hz;
    u64 hmon = 0;
    return monitor_mode(out, &w, &h, &hz, &hmon) ? (HMONITOR)hmon : 0;
}

// The monitor and corner the toasts use: Config.popup_monitor (an output index or POPUP_RECORDED) in
// Config.popup_corner. With a game in front (`in_game`), Config.popup_monitor_game and popup_corner_game instead,
// unless it is POPUP_AS_NORMAL. `front` is the monitor the window in front is on, the game's while one is; an
// output that isn't there (output_monitor gives 0) falls back to it.
static HMONITOR popup_monitor_for(const Config &c, bool in_game, HMONITOR front, HMONITOR (*output_monitor)(i32),
                                  PopupCorner *corner)
{
    *corner = c.popup_corner;
    i32 out = c.popup_monitor;
    if (in_game && c.popup_monitor_game != POPUP_AS_NORMAL) {
        out = c.popup_monitor_game;
        *corner = c.popup_corner_game;
    }
    HMONITOR m = output_monitor(out < 0 ? c.output_idx : out);
    return m ? m : front;
}

// Where the toasts go now (popup_monitor_for, the game being the window in front). A test copy's go on the monitor
// furthest right. `mon` (may be 0) is the monitor. False when there's none.
static bool toast_monitor(RECT *work, PopupCorner *corner, HMONITOR *mon)
{
    HMONITOR m = 0;
    if (is_test_copy()) {
        *corner = g_app.config.popup_corner;
        if (!rightmost_monitor(work, &m)) return false;
        if (mon) *mon = m;
        return true;
    }
    m = popup_monitor_for(g_app.config, sessions_game_in_front(),
                          MonitorFromWindow(GetForegroundWindow(), MONITOR_DEFAULTTOPRIMARY), popup_output_monitor,
                          corner);
    MONITORINFO mi = {sizeof mi};
    if (!GetMonitorInfoW(m, &mi)) return false;
    *work = mi.rcWork;
    if (mon) *mon = m;
    return true;
}

// Where a w x h toast goes: in `corner` of `work`, `m` from its edges, past the `n` toasts already up in its column
// (their window rects). Bottom corners stack upward, top ones downward.
static POINT toast_spot(const RECT &work, PopupCorner corner, i32 w, i32 h, i32 m, const RECT *others, i32 n)
{
    bool left = corner == CORNER_BL || corner == CORNER_TL, top = corner == CORNER_TL || corner == CORNER_TR;
    i32 x = left ? work.left + m : work.right - w - m;
    i32 y = top ? work.top + m : work.bottom - m - h;
    for (bool moved = true; moved;) { // again after a move, as it may then be in another's way
        moved = false;
        for (i32 i = 0; i < n; i++) {
            const RECT &o = others[i];
            if (o.left >= x + w || o.right <= x || o.bottom <= work.top || o.top >= work.bottom) continue;
            if (o.top - m < y + h && o.bottom + m > y) {
                y = top ? o.bottom + m : o.top - m - h;
                moved = true;
            }
        }
    }
    return {x, y};
}

// In the monitor's corner, sized for its scale, and past the other toasts that are up there (toast_spot)
static void toast_place(Toast &t, const RECT &work, PopupCorner corner)
{
    SetWindowPos(t.hwnd, HWND_TOPMOST, work.right - 2, work.bottom - 2, 1, 1, SWP_NOACTIVATE);
    toast_fonts(t, GetDpiForWindow(t.hwnd) / 96.f);
    i32 w = toast_px(t, TOAST_W), h = toast_px(t, toast_height(t)), m = toast_px(t, TOAST_MARGIN);
    if (t.pill) w = toast_pill_w(t); // sized to its title
    const Toast *all[] = {&g_toast_saved, &g_toast_marker, &g_toast_session};
    RECT others[COUNT(all)];
    i32 n = 0;
    for (const Toast *other : all)
        if (other != &t && other->open && GetWindowRect(other->hwnd, &others[n])) n++;
    POINT at = toast_spot(work, corner, w, h, m, others, n);
    SetWindowPos(t.hwnd, HWND_TOPMOST, at.x, at.y, w, h, SWP_NOACTIVATE);
    if (t.edit) {
        RECT f = toast_rect(t, TR_FIELD);
        i32 in = toast_px(t, 12), eh = toast_px(t, 20);
        SetWindowPos(t.edit, 0, f.left + in, (f.top + f.bottom - eh) / 2, f.right - f.left - 2 * in, eh, SWP_NOZORDER);
    }
    t.hot = TP_NONE;
    toast_redraw(t);
}

// The art behind the toast: `game`'s (game_art_grey), or none
static void toast_set_art(Toast &t, const char *game)
{
    free(t.art);
    t.art = game && game[0] ? game_art_grey(game, &t.art_w, &t.art_h) : 0;
}

// Clip saved (Session saved) for path, of `game` ("" for none), brief (no buttons) or with its buttons
static void toast_saved_show(SaveKind kind, const char *path, bool brief, const char *game)
{
    Toast &t = g_toast_saved;
    RECT work;
    PopupCorner corner;
    if (!toast_monitor(&work, &corner, 0) || !toast_window(t)) return;
    str_set(t.path, path);
    t.brief = brief;
    t.failed = false;
    toast_set_pill(t, brief);
    toast_set_art(t, brief ? "" : game);
    t.sharing = TSH_NONE; // one still running carries on, unseen
    utf8_to_wide(kind == SAVE_SESSION ? "Session saved" : "Clip saved", t.title, COUNT(t.title));
    const char *name = strrchr(path, '\\');
    utf8_to_wide(name ? name + 1 : path, t.line, COUNT(t.line));
    toast_place(t, work, corner);
    t.open = true;
    ShowWindow(t.hwnd, SW_SHOWNOACTIVATE);
    toast_countdown(t, kind == SAVE_SESSION ? TOAST_LIFE_SESSION : brief ? TOAST_LIFE_BRIEF : TOAST_LIFE_CLIP);
}

// The Clip saved popup as a picture for Options (pill: the small one), drawn by the code above at `scale` with
// `name` as its file, the dial full: a texture of *w x *h, the full one's corners left square (Windows rounds the
// window's). Kept while the look, the scale and the name stay; 0 when it can't be made.
static ImTextureID toast_preview(bool pill, f32 scale, const char *name, i32 *w, i32 *h)
{
    static Toast t = {TOAST_SAVED};
    static ID3D11ShaderResourceView *tex[2];
    static i32 tw[2], th[2];
    static u32 accent[2];
    static f32 made_scale[2];
    static char made_name[2][MAX_PATH];
    i32 k = pill ? 1 : 0;
    if (tex[k] && accent[k] == COL_ACCENT && made_scale[k] == scale && !strcmp(made_name[k], name)) {
        *w = tw[k], *h = th[k];
        return (ImTextureID)(intptr_t)tex[k];
    }
    if (tex[k]) tex[k]->Release(), tex[k] = 0;
    toast_fonts(t, scale);
    t.pill = pill;
    t.brief = pill;
    t.held = true;
    t.hot = TP_NONE;
    utf8_to_wide("Clip saved", t.title, COUNT(t.title));
    utf8_to_wide(name, t.line, COUNT(t.line));
    i32 pw = pill ? toast_pill_w(t) : toast_px(t, TOAST_W), ph = toast_px(t, toast_height(t));
    HDC screen = GetDC(0);
    u32 *px = 0;
    HBITMAP bmp = 0;
    if (pill) {
        bmp = toast_pill_bitmap(t, screen, pw, ph, &px);
        if (bmp) // straight alpha, as ImGui blends
            for (i32 i = 0; i < pw * ph; i++) {
                u32 a = px[i] >> 24, c = px[i];
                if (a && a < 255)
                    px[i] = a << 24 | (((c >> 16) & 0xff) * 255 / a) << 16 | (((c >> 8) & 0xff) * 255 / a) << 8 | (c & 0xff) * 255 / a;
            }
    } else {
        BITMAPINFO bi = {};
        bi.bmiHeader = {sizeof bi.bmiHeader, pw, -ph, 1, 32, BI_RGB};
        bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, (void **)&px, 0, 0);
        if (bmp && px) {
            HDC dc = CreateCompatibleDC(screen);
            HGDIOBJ old = SelectObject(dc, bmp);
            RECT r = {0, 0, pw, ph};
            toast_draw(t, screen, dc, r);
            toast_draw_dial(t, dc, dc);
            GdiFlush();
            SelectObject(dc, old);
            DeleteDC(dc);
            for (i32 i = 0; i < pw * ph; i++) px[i] |= 0xff000000; // GDI leaves the alpha at 0
        }
    }
    ReleaseDC(0, screen);
    if (bmp && px) tex[k] = gpu_texture(px, pw, ph);
    if (bmp) DeleteObject(bmp);
    if (!tex[k]) return 0;
    tw[k] = pw, th[k] = ph;
    accent[k] = COL_ACCENT;
    made_scale[k] = scale;
    str_set(made_name[k], name);
    *w = pw, *h = ph;
    return (ImTextureID)(intptr_t)tex[k];
}

// Small is for a popup over the game: on another monitor (`popup_mon`, the game's `game_mon`) it is the whole one.
static ToastClip toast_clip_on(ToastClip style, u64 popup_mon, u64 game_mon)
{
    return style == TOAST_CLIP_SMALL && popup_mon != game_mon ? TOAST_CLIP_FULL : style;
}

// How Clip saved shows now: over a game, the game being played's choice (toast_clip_for); else the whole popup.
static ToastClip toast_clip_now()
{
    if (!sessions_game_in_front()) return TOAST_CLIP_FULL;
    return toast_clip_for(g_app.config, g_app.audio.game.s);
}

// A save finished: Clip saved as toast_clip_now says (Session saved always whole), unless clipback's own window is
// in front (it shows the save itself).
static void toast_saved(SaveKind kind, const char *path, const char *game)
{
    if (kind == SAVE_PREVIEW || kind == SAVE_LINK) return;
    if (g_app.ui.shown && GetForegroundWindow() == g_app.ui.hwnd) return;
    ToastClip style = kind == SAVE_SESSION ? TOAST_CLIP_FULL : toast_clip_now();
    if (style == TOAST_CLIP_OFF) return;
    RECT work;
    PopupCorner corner;
    HMONITOR mon = 0;
    if (style == TOAST_CLIP_SMALL && toast_monitor(&work, &corner, &mon))
        style = toast_clip_on(style, (u64)mon, (u64)MonitorFromWindow(GetForegroundWindow(), MONITOR_DEFAULTTONULL));
    toast_saved_show(kind, path, style == TOAST_CLIP_SMALL, game);
}

// A save failed: Save failed with the first line of the reason, no buttons, unless clipback's own window is in front
// (it shows the failure itself). Shares, previews and links say so where they were asked for.
static void toast_save_failed(SaveKind kind, const char *why)
{
    if (kind != SAVE_CLIP && kind != SAVE_SESSION && kind != SAVE_KEEP) return;
    if (g_app.ui.shown && GetForegroundWindow() == g_app.ui.hwnd) return;
    Toast &t = g_toast_saved;
    RECT work;
    PopupCorner corner;
    if (!toast_monitor(&work, &corner, 0) || !toast_window(t)) return;
    t.path[0] = 0;
    t.brief = true;
    t.failed = true;
    toast_set_pill(t, false);
    toast_set_art(t, "");
    t.sharing = TSH_NONE;
    utf8_to_wide("Save failed", t.title, COUNT(t.title));
    char line[120];
    str_set(line, why && why[0] ? why : "Something went wrong");
    line[strcspn(line, "\r\n")] = 0;
    utf8_to_wide(line, t.line, COUNT(t.line));
    toast_place(t, work, corner);
    t.open = true;
    ShowWindow(t.hwnd, SW_SHOWNOACTIVATE);
    toast_countdown(t, TOAST_LIFE_FAILED);
}

// Session ended was answered: Save (TP_MAIN), Review (TP_SECOND), or dismissed, closed or ignored (anything else). Every
// session it asked about that isn't being saved is unsaved.
static void toast_session_close(i32 part)
{
    Toast &t = g_toast_session;
    if (!t.open) return;
    toast_hide(t);
    Session *s = session_find(t.session);
    if (part == TP_MAIN && s && s->state == SES_ASKING) session_save(s);
    sessions_not_now();
    if (part != TP_SECOND) return;
    log_line("toast: review %s", t.more ? "the unsaved sessions" : s ? s->name : "a session that's gone");
    if (!t.more && s) {
        session_seen(s->id);
        ui_show(SCREEN_TIMELINE);
        timeline_review(s->start_us, s->end_us, s->name);
    } else {
        library_close_player();
        ui_show(SCREEN_LIBRARY);
        lib_set_view(LIB_SESSIONS);
    }
}

// The game's icon for the tile, laid over the panel: from the games database, else the program's own. Null for none.
static u32 *toast_game_pixels(const Session &s, i32 *w, i32 *h)
{
    static IWICImagingFactory *wic;
    GameInfo g;
    Path p = {};
    u32 *px = 0;
    if (gamedb_for_art(s.name, &g) && gamedb_art(g, ART_ICON, &p) &&
        (wic || SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, 0, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))))
        px = wic_load(wic, p.s, w, h);
    if (!px && s.exe[0] && app_find_path(s.exe, &p) && (px = app_icon_pixels(p))) *w = *h = APP_ICON_PX;
    if (!px) return 0;
    u32 bg = COL_PANEL; // straight alpha over the panel, so GDI can draw it opaque
    for (i32 i = 0; i < *w * *h; i++) {
        u32 c = px[i], a = c >> 24, out = 0xff000000u;
        for (i32 sh = 0; sh < 24; sh += 8)
            out |= ((((c >> sh) & 255) * a + ((bg >> sh) & 255) * (255 - a) + 127) / 255) << sh;
        px[i] = out;
    }
    return px;
}

// Session ended, for session s with `more` others waiting behind it. Shown again with the new ones when another
// ends while it's up. False when it can't be shown.
static bool toast_session(const Session &s, i32 more)
{
    Toast &t = g_toast_session;
    RECT work;
    PopupCorner corner;
    if (!toast_monitor(&work, &corner, 0) || !toast_window(t)) return false;
    t.session = s.id;
    t.more = more;
    if (t.tile_px) HeapFree(GetProcessHeap(), 0, t.tile_px);
    t.tile_px = toast_game_pixels(s, &t.tile_w, &t.tile_h);
    toast_set_art(t, s.name);
    char len[32], line[192];
    fmt_length(s.end_us - s.start_us, len, sizeof len);
    if (more) snprintf(line, sizeof line, "%s - %s, +%d more", s.name, len, more);
    else snprintf(line, sizeof line, "%s - %s", s.name, len);
    utf8_to_wide("Session ended", t.title, COUNT(t.title));
    utf8_to_wide(line, t.line, COUNT(t.line));
    if (!t.open) toast_place(t, work, corner);
    toast_redraw(t);
    t.open = true;
    ShowWindow(t.hwnd, SW_SHOWNOACTIVATE);
    toast_countdown(t, TOAST_LIFE_SESSION);
    return true;
}

// `mark ask`: the window for marker t_us's colour and note
static void toast_marker(i64 t_us)
{
    Toast &t = g_toast_marker;
    RECT work;
    PopupCorner corner;
    if (!toast_monitor(&work, &corner, 0) || !toast_window(t)) return;
    if (t.open) toast_marker_close(true); // one being described already: it keeps what it has
    HWND front = GetForegroundWindow();
    t.before = front == t.hwnd ? 0 : front;
    t.marker = t_us;
    t.colour = 0;
    char when[64];
    fmt_clock(t_us, when, sizeof when);
    utf8_to_wide("Marker", t.title, COUNT(t.title));
    utf8_to_wide(when, t.line, COUNT(t.line));
    SetWindowTextW(t.edit, L"");
    toast_place(t, work, corner);
    t.open = true;
    ShowWindow(t.hwnd, SW_SHOW);
    SetForegroundWindow(t.hwnd);
    SetFocus(t.edit);
}
