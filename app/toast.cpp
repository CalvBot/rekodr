// toast.cpp: the desktop toasts, small windows over everything in the corner of a monitor (Options > General >
// Popups): "Clip saved" when a save finishes while a game is in front, "Session ended" when a session the game is set
// to Ask for has ended (sessions.cpp), and the marker window `mark ask` opens. All are laid out alike, from the
// TOAST_* sizes: a tile beside a title over a line, then a row of buttons with the main one last on the right. A
// test copy (CLIPBACK_HOME) puts them on the monitor furthest right.
//
// Clip saved and Session ended never take the keyboard from the game, and a click anywhere else on them closes them.
// Clip saved: Edit and Share open the file in the editor on that tab. It goes after 6 s unless the mouse is on it.
// Session ended: the game's icon, its name and length. Save saves it; Review opens it on the timeline, or the
// Library's unsaved sessions when several are waiting. It goes after 15 s unless the mouse is on it; closed or
// ignored, the sessions it asked about are unsaved.
// The marker window takes the keyboard, and gives it back to the window that had it. Enter or Save keeps what was
// typed, as does clicking away; Escape leaves the marker plain. Tab steps through the colours.

enum ToastKind { TOAST_SAVED, TOAST_MARKER, TOAST_SESSION };
// The parts a click lands on: the buttons counted from the right, then the marker's colours (TP_COLOUR + 1..6)
enum { TP_NONE, TP_MAIN, TP_SECOND, TP_COLOUR = 10 };
enum { TR_TILE = -1, TR_FIELD = -2 }; // more rectangles, for drawing

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
    bool      open, tracking;
    f32       scale;              // the fonts were made for this
    HFONT     font, strong, title_font, icons;
    HBRUSH    field;
};
static Toast g_toast_saved = {TOAST_SAVED}, g_toast_marker = {TOAST_MARKER}, g_toast_session = {TOAST_SESSION};
static const UINT_PTR TOAST_TIMER = 1;
static const UINT     TOAST_SAVED_MS = 6000, TOAST_SESSION_MS = 15000;

// Clip saved and Session ended leave the keyboard where it is; the marker window takes it.
static bool toast_quiet(const Toast &t) { return t.kind != TOAST_MARKER; }

static void toast_session_close(i32 part);

static COLORREF toast_rgb(u32 c) { return RGB((c >> 16) & 0xff, (c >> 8) & 0xff, c & 0xff); }
static i32 toast_px(const Toast &t, f32 v) { return (i32)(v * t.scale + 0.5f); }

// At 100%
static f32 toast_height(ToastKind kind)
{
    return TOAST_PAD + TOAST_TILE + TOAST_GAP + (kind == TOAST_MARKER ? TOAST_FIELD_H + TOAST_GAP : 0) + TOAST_BUTTON_H + TOAST_PAD;
}

// A part's rectangle in client pixels
static RECT toast_rect(const Toast &t, i32 part)
{
    i32 w = toast_px(t, TOAST_W), pad = toast_px(t, TOAST_PAD), tile = toast_px(t, TOAST_TILE);
    i32 bw = toast_px(t, TOAST_BUTTON_W), bh = toast_px(t, TOAST_BUTTON_H), gap = toast_px(t, TOAST_BUTTON_GAP);
    i32 row = toast_px(t, toast_height(t.kind) - TOAST_PAD - TOAST_BUTTON_H);
    if (part == TR_TILE) return {pad, pad, pad + tile, pad + tile};
    if (part == TR_FIELD) {
        i32 y = toast_px(t, TOAST_PAD + TOAST_TILE + TOAST_GAP);
        return {pad, y, w - pad, y + toast_px(t, TOAST_FIELD_H)};
    }
    if (part == TP_MAIN) return {w - pad - bw, row, w - pad, row + bh};
    if (part == TP_SECOND) return {w - pad - 2 * bw - gap, row, w - pad - bw - gap, row + bh};
    if (part > TP_COLOUR) {
        i32 d = toast_px(t, TOAST_DOT), x = pad + (part - TP_COLOUR - 1) * (d + toast_px(t, TOAST_DOT_GAP));
        i32 y = row + (bh - d) / 2;
        return {x, y, x + d, y + d};
    }
    return {};
}

static i32 toast_hit(const Toast &t, LPARAM lp)
{
    POINT p = {(i16)LOWORD(lp), (i16)HIWORD(lp)};
    i32 parts[8], n = 0;
    parts[n++] = TP_MAIN;
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
    KillTimer(t.hwnd, TOAST_TIMER);
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

// Shapes are drawn four times the size and scaled down, which smooths their edges (GDI's own are jagged); the
// text and the icons go on afterwards at their real size.
static void toast_paint(Toast &t)
{
    const i32 SS = 4;
    PAINTSTRUCT ps;
    HDC wdc = BeginPaint(t.hwnd, &ps);
    RECT r;
    GetClientRect(t.hwnd, &r);
    HDC big = CreateCompatibleDC(wdc), dc = CreateCompatibleDC(wdc);
    HBITMAP big_bmp = CreateCompatibleBitmap(wdc, r.right * SS, r.bottom * SS), bmp = CreateCompatibleBitmap(wdc, r.right, r.bottom);
    HGDIOBJ old_big = SelectObject(big, big_bmp), old_bmp = SelectObject(dc, bmp);
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
    brush(t.hot == TP_MAIN ? COL_ACCENT_HOVER : COL_ACCENT);
    rounded(TP_MAIN, TOAST_BUTTON_ROUND);
    if (quiet) {
        brush(t.hot == TP_SECOND ? COL_HOVER : COL_PANEL2);
        rounded(TP_SECOND, TOAST_BUTTON_ROUND);
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
    i32 tx = tile.right + toast_px(t, TOAST_GAP), right = r.right - toast_px(t, TOAST_PAD), mid = (tile.top + tile.bottom) / 2;
    SelectObject(dc, t.title_font);
    SetTextColor(dc, toast_rgb(COL_TEXT));
    RECT t1 = {tx, tile.top - toast_px(t, 4), right, mid + toast_px(t, 1)};
    DrawTextW(dc, t.title, -1, &t1, DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, t.font);
    SetTextColor(dc, toast_rgb(COL_DIM));
    RECT t2 = {tx, mid + toast_px(t, 3), right, tile.bottom + toast_px(t, 4)};
    DrawTextW(dc, t.line, -1, &t2, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

    RECT main = toast_rect(t, TP_MAIN);
    SelectObject(dc, t.strong);
    SetTextColor(dc, RGB(255, 255, 255));
    DrawTextW(dc, saved ? L"Share" : L"Save", -1, &main, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    if (quiet) {
        RECT second = toast_rect(t, TP_SECOND);
        SelectObject(dc, t.font);
        SetTextColor(dc, toast_rgb(COL_TEXT));
        DrawTextW(dc, saved ? L"Edit" : L"Review", -1, &second, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    SelectObject(dc, old_font);

    BitBlt(wdc, 0, 0, r.right, r.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(big, old_big);
    SelectObject(dc, old_bmp);
    DeleteObject(big_bmp);
    DeleteObject(bmp);
    DeleteDC(big);
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
        InvalidateRect(t.hwnd, 0, FALSE);
        return 0;
    }
    if (msg == WM_CHAR && (wp == '\r' || wp == '\t' || wp == 27)) return 0; // no beep for the keys taken above
    return CallWindowProcW(t.edit_proc, hwnd, msg, wp, lp);
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
            InvalidateRect(hwnd, 0, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        t.tracking = false;
        t.hot = TP_NONE;
        InvalidateRect(hwnd, 0, FALSE);
        if (quiet) SetTimer(hwnd, TOAST_TIMER, 2000, 0); // a moment more once the mouse leaves
        return 0;
    case WM_LBUTTONUP: {
        i32 k = toast_hit(t, lp);
        if (saved) {
            toast_hide(t);
            if (k) {
                log_line("toast: %s %s", k == TP_SECOND ? "edit" : "share", t.path);
                ui_show(SCREEN_EDITOR);
                editor_open_file(t.path);
                g_app.editor.tab = k == TP_MAIN ? 1 : 0;
            }
        } else if (t.kind == TOAST_SESSION) {
            toast_session_close(k);
        } else if (k == TP_MAIN) {
            toast_marker_close(true);
        } else if (k > TP_COLOUR) {
            u8 c = (u8)(k - TP_COLOUR);
            t.colour = t.colour == c ? 0 : c; // a second click takes the colour off
            InvalidateRect(hwnd, 0, FALSE);
            SetFocus(t.edit);
        }
        return 0;
    }
    case WM_TIMER: {
        POINT p;
        RECT r;
        GetCursorPos(&p);
        GetWindowRect(hwnd, &r);
        if (PtInRect(&r, p)) return 0;
        if (t.kind == TOAST_SESSION) toast_session_close(TP_NONE);
        else toast_hide(t);
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
    t.hwnd = CreateWindowExW(ex, L"clipback_toast", L"clipback", WS_POPUP | WS_CLIPCHILDREN, 0, 0, 1, 1, 0, 0, inst, 0);
    if (!t.hwnd) return false;
    SetWindowLongPtrW(t.hwnd, GWLP_USERDATA, (LONG_PTR)&t);
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
    if (t.font) DeleteObject(t.font), DeleteObject(t.strong), DeleteObject(t.title_font), DeleteObject(t.icons);
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
    if (t.edit) SendMessageW(t.edit, WM_SETFONT, (WPARAM)t.font, TRUE);
}

// The monitor toasts go on: Config.popup_monitor, an output index or -2 the one recorded. -1 turns Clip saved off;
// the others then go on the one recorded. A test copy's go on the monitor furthest right. False when there's none.
static bool toast_monitor(ToastKind kind, RECT *work)
{
    HMONITOR right;
    if (is_test_copy()) return rightmost_monitor(work, &right);
    i32 out = g_app.config.popup_monitor;
    if (out == -1 && kind == TOAST_SAVED) return false;
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
    i32 w = toast_px(t, TOAST_W), h = toast_px(t, toast_height(t.kind)), m = toast_px(t, TOAST_MARGIN);
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
    InvalidateRect(t.hwnd, 0, FALSE);
}

// A save finished: Clip saved, unless clipback's own window is in front (it shows the save itself).
static void toast_saved(SaveKind kind, const char *path)
{
    if (kind == SAVE_PREVIEW) return;
    if (g_app.ui.shown && GetForegroundWindow() == g_app.ui.hwnd) return;
    Toast &t = g_toast_saved;
    RECT work;
    if (!toast_monitor(TOAST_SAVED, &work) || !toast_window(t)) return;
    str_set(t.path, path);
    utf8_to_wide(kind == SAVE_SESSION ? "Session saved" : "Clip saved", t.title, COUNT(t.title));
    const char *name = strrchr(path, '\\');
    utf8_to_wide(name ? name + 1 : path, t.line, COUNT(t.line));
    toast_place(t, work);
    t.open = true;
    ShowWindow(t.hwnd, SW_SHOWNOACTIVATE);
    SetTimer(t.hwnd, TOAST_TIMER, TOAST_SAVED_MS, 0);
}

// Session ended was answered: Save (TP_MAIN), Review (TP_SECOND), or closed or ignored (anything else). Every
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
    InvalidateRect(t.hwnd, 0, FALSE);
    t.open = true;
    ShowWindow(t.hwnd, SW_SHOWNOACTIVATE);
    SetTimer(t.hwnd, TOAST_TIMER, TOAST_SESSION_MS, 0);
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
