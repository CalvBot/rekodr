// toast.cpp: the desktop toasts, small windows over everything in the corner of a monitor (Options > General >
// Popups): "Clip saved" when a save finishes while a game is in front ("Save failed" when it didn't), "Session
// ended" when a session the game is set to Ask for has ended (sessions.cpp), and the marker window `mark ask` opens. All are laid out alike, from the
// TOAST_* sizes: a tile beside a title over a line, then a row of buttons with the main one last on the right. A
// test copy (CLIPBACK_HOME) puts them on the monitor furthest right.
//
// Clip saved and Session ended never take the keyboard from the game, and a click anywhere else on them closes them.
// Each goes after its own time (TOAST_LIFE_*): Session saved or ended 15 s, Clip saved 5 s, 10 s once a save or share
// failed.
// A ring round the Dismiss X counts it down. While the mouse is on one the count holds at full, and starts again from
// full when the mouse leaves.
// Clip saved: Edit opens the file in the editor. Share does what share_decide picks and stays up while it runs, its
// line showing the step and percent over a thin bar, then what went on the clipboard; then it counts down again. A
// first link to a free host isn't agreed to here: the library opens on the clip with its Share menu instead.
// Session ended: the game's icon, its name and length. Save saves it; Review opens it on the timeline, or the
// Library's unsaved sessions when several are waiting. Closed or ignored, the sessions it asked about are unsaved.
// The marker window takes the keyboard, and gives it back to the window that had it. Enter or Save keeps what was
// typed, as does clicking away; Escape leaves the marker plain. Tab steps through the colours.
// Every toast has a Dismiss button (an X) at the bottom left: it closes the toast as ignoring it would, and on the
// marker window it is Escape.
// With Options > General > Small in games on, Clip saved over a game is only its tile, title and line: no buttons, gone
// after 3 s, and a click closes it. Popups in games picks another monitor for every toast while a game is in front.

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
    u8        colour;
    i32       hot;                // the part under the mouse
    bool      open, tracking, held; // held: the mouse is on it, so the countdown waits
    bool      brief;              // Clip saved over a game: no button row (Config.toast_small_in_game)
    u8        sharing;            // Clip saved's Share: TSH_* below
    u32       share;              // its share_start job
    i32       share_pct;          // the percent last drawn
    u64       start_ms;           // the countdown's start (GetTickCount64)
    u32       life_ms;            // how long it shows once the countdown starts (TOAST_LIFE_*)
    HBITMAP   base;               // the toast drawn without its dial, base_w x base_h; redrawn when dirty
    i32       base_w, base_h;
    bool      dirty;
    f32       scale;              // the fonts were made for this
    HFONT     font, strong, title_font, icons, small_icons;
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

// Clip saved and Session ended leave the keyboard where it is; the marker window takes it.
static bool toast_quiet(const Toast &t) { return t.kind != TOAST_MARKER; }

static void toast_session_close(i32 part);

// The whole toast is drawn again on its next paint (a part changed); the countdown's ticks only redraw the dial.
static void toast_redraw(Toast &t)
{
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

// The toast without its dial, into dc (r, the client size): the shapes first, then the text and the icons at their
// real size.
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
    HGDIOBJ old_pen = SelectObject(big, GetStockObject(NULL_PEN));
    HGDIOBJ old_brush = SelectObject(big, CreateSolidBrush(0));
    // The tile: the accent behind a tick, the game's icon (a game glyph on a soft accent without one), or the
    // marker's colour behind a flag
    brush(saved ? COL_ACCENT : session ? COL_ACCENT_SOFT : MARKER_COLOURS[t.colour].rgb);
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
    // The buttons, as the window's own: the main one in the accent, the other secondary
    if (!t.brief) brush(t.hot == TP_MAIN && !t.sharing ? COL_ACCENT_HOVER : COL_ACCENT);
    if (!t.brief) rounded(TP_MAIN, TOAST_BUTTON_ROUND);
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
    SetTextColor(dc, session ? toast_rgb(COL_ACCENT) : RGB(255, 255, 255));
    RECT tile = toast_rect(t, TR_TILE);
    const wchar_t glyph[] = {(wchar_t)(saved ? 0xE73E : session ? 0xE7FC : 0xE7C1), 0}; // Segoe MDL2: CheckMark, Game, Flag
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
    SetTextColor(dc, RGB(255, 255, 255));
    const wchar_t *main_label = !saved ? L"Save" : t.sharing == TSH_DONE ? L"Copied" : t.sharing ? L"Sharing" : L"Share";
    DrawTextW(dc, main_label, -1, &main, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
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
// over the toast's life (when it ends, the toast is dismissed), full while the mouse is on it.
static void toast_draw_dial(Toast &t, HDC wdc)
{
    const i32 SS = TOAST_SS;
    RECT d = toast_rect(t, TR_DIAL);
    i32 w = d.right - d.left, h = d.bottom - d.top, W = w * SS, H = h * SS;
    f32 left = t.held ? 1 : 1 - (f32)(GetTickCount64() - t.start_ms) / t.life_ms;
    left = left < 0 ? 0 : left;
    HDC big = CreateCompatibleDC(wdc), dc = CreateCompatibleDC(wdc);
    HBITMAP big_bmp = CreateCompatibleBitmap(wdc, W, H), bmp = CreateCompatibleBitmap(wdc, w, h);
    HGDIOBJ old_big = SelectObject(big, big_bmp), old_bmp = SelectObject(dc, bmp);
    RECT all = {0, 0, W, H};
    u32 back = t.hot == TP_DISMISS ? COL_HOVER : COL_PANEL; // the Dismiss button's own fill
    HBRUSH bg = CreateSolidBrush(toast_rgb(back));
    FillRect(big, &all, bg);
    DeleteObject(bg);
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
    brush(back); // hollowed out to a ring
    i32 ring = toast_px(t, TOAST_DIAL_RING) * SS;
    Ellipse(big, ring, ring, W - ring + 1, H - ring + 1);
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
        toast_draw_dial(t, wdc);
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
    if (d.kind == SHARE_LINK && !r2_ready() && !g_app.config.free_links_seen) { // asked in the library first
        log_line("toast: share %s needs the free links notice", t.path);
        toast_hide(t);
        t.sharing = TSH_NONE;
        library_share_file(t.path);
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
        toast_share_end(t, true, j.error[0] ? j.error : j.kind == SHARE_LINK ? "Link copied" : "Copied, ready to paste");
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
            if (k == TP_MAIN && t.sharing != TSH_DONE) return 0; // under way
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
                  DeleteObject(t.small_icons);
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
    if (t.edit) SendMessageW(t.edit, WM_SETFONT, (WPARAM)t.font, TRUE);
}

// The monitor toasts go on: Config.popup_monitor, an output index or -2 the one recorded. -1 turns Clip saved off;
// the others then go on the one recorded. While a game is in front, Config.popup_monitor_game picks instead unless
// it is -3. A test copy's go on the monitor furthest right. False when there's none.
static bool toast_monitor(ToastKind kind, RECT *work)
{
    HMONITOR right;
    if (g_app.config.popup_monitor == -1 && kind == TOAST_SAVED) return false;
    if (is_test_copy()) return rightmost_monitor(work, &right);
    i32 out = g_app.config.popup_monitor;
    if (g_app.config.popup_monitor_game != -3 && sessions_game_in_front()) out = g_app.config.popup_monitor_game;
    if (out < 0) out = g_app.config.output_idx;
    i32 w, h, hz;
    u64 hmon = 0;
    HMONITOR m = monitor_mode(out, &w, &h, &hz, &hmon) && hmon ? (HMONITOR)hmon : MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = {sizeof mi};
    if (!GetMonitorInfoW(m, &mi)) return false;
    *work = mi.rcWork;
    return true;
}

// In the monitor's corner, sized for its scale, and above the other toasts that are up there
static void toast_place(Toast &t, const RECT &work)
{
    SetWindowPos(t.hwnd, HWND_TOPMOST, work.right - 2, work.bottom - 2, 1, 1, SWP_NOACTIVATE);
    toast_fonts(t, GetDpiForWindow(t.hwnd) / 96.f);
    i32 w = toast_px(t, TOAST_W), h = toast_px(t, toast_height(t)), m = toast_px(t, TOAST_MARGIN);
    i32 bottom = work.bottom - m;
    const Toast *others[] = {&g_toast_saved, &g_toast_marker, &g_toast_session};
    for (const Toast *other : others) {
        RECT o;
        if (other != &t && other->open && GetWindowRect(other->hwnd, &o) && o.left < work.right && o.right > work.left &&
            o.bottom > work.top && o.top - m < bottom)
            bottom = o.top - m;
    }
    SetWindowPos(t.hwnd, HWND_TOPMOST, work.right - w - m, bottom - h, w, h, SWP_NOACTIVATE);
    if (t.edit) {
        RECT f = toast_rect(t, TR_FIELD);
        i32 in = toast_px(t, 12), eh = toast_px(t, 20);
        SetWindowPos(t.edit, 0, f.left + in, (f.top + f.bottom - eh) / 2, f.right - f.left - 2 * in, eh, SWP_NOZORDER);
    }
    t.hot = TP_NONE;
    toast_redraw(t);
}

// Clip saved for path, brief (no buttons) or with its buttons
static void toast_saved_show(SaveKind kind, const char *path, bool brief)
{
    Toast &t = g_toast_saved;
    RECT work;
    if (!toast_monitor(TOAST_SAVED, &work) || !toast_window(t)) return;
    str_set(t.path, path);
    t.brief = brief;
    t.sharing = TSH_NONE; // one still running carries on, unseen
    utf8_to_wide(kind == SAVE_SESSION ? "Session saved" : "Clip saved", t.title, COUNT(t.title));
    const char *name = strrchr(path, '\\');
    utf8_to_wide(name ? name + 1 : path, t.line, COUNT(t.line));
    toast_place(t, work);
    t.open = true;
    ShowWindow(t.hwnd, SW_SHOWNOACTIVATE);
    toast_countdown(t, kind == SAVE_SESSION ? TOAST_LIFE_SESSION : brief ? TOAST_LIFE_BRIEF : TOAST_LIFE_CLIP);
}

// A save finished: Clip saved, unless clipback's own window is in front (it shows the save itself).
static void toast_saved(SaveKind kind, const char *path)
{
    if (kind == SAVE_PREVIEW || kind == SAVE_LINK) return;
    if (g_app.ui.shown && GetForegroundWindow() == g_app.ui.hwnd) return;
    toast_saved_show(kind, path, g_app.config.toast_small_in_game && sessions_game_in_front());
}

// A save failed: Save failed with the first line of the reason, no buttons, unless clipback's own window is in front
// (it shows the failure itself). Shares, previews and links say so where they were asked for.
static void toast_save_failed(SaveKind kind, const char *why)
{
    if (kind != SAVE_CLIP && kind != SAVE_SESSION && kind != SAVE_KEEP) return;
    if (g_app.ui.shown && GetForegroundWindow() == g_app.ui.hwnd) return;
    Toast &t = g_toast_saved;
    RECT work;
    if (!toast_monitor(TOAST_SAVED, &work) || !toast_window(t)) return;
    t.path[0] = 0;
    t.brief = true;
    t.sharing = TSH_NONE;
    utf8_to_wide("Save failed", t.title, COUNT(t.title));
    char line[120];
    str_set(line, why && why[0] ? why : "Something went wrong");
    line[strcspn(line, "\r\n")] = 0;
    utf8_to_wide(line, t.line, COUNT(t.line));
    toast_place(t, work);
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
    if (!toast_monitor(TOAST_SESSION, &work) || !toast_window(t)) return false;
    t.session = s.id;
    t.more = more;
    if (t.tile_px) HeapFree(GetProcessHeap(), 0, t.tile_px);
    t.tile_px = toast_game_pixels(s, &t.tile_w, &t.tile_h);
    char len[32], line[192];
    fmt_length(s.end_us - s.start_us, len, sizeof len);
    if (more) snprintf(line, sizeof line, "%s - %s, +%d more", s.name, len, more);
    else snprintf(line, sizeof line, "%s - %s", s.name, len);
    utf8_to_wide("Session ended", t.title, COUNT(t.title));
    utf8_to_wide(line, t.line, COUNT(t.line));
    if (!t.open) toast_place(t, work);
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
    if (!toast_monitor(TOAST_MARKER, &work) || !toast_window(t)) return;
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
    toast_place(t, work);
    t.open = true;
    ShowWindow(t.hwnd, SW_SHOW);
    SetForegroundWindow(t.hwnd);
    SetFocus(t.edit);
}
