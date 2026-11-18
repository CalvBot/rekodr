// markers.cpp: moments marked in the buffer, each with an optional colour and note.
//
// The `mark` command makes one, from a hotkey, a Stream Deck key or `clipback.exe mark`:
//   mark                    a plain marker at this moment
//   mark <note>             one with a note
//   mark <colour> <note>    one with a colour (red, orange, yellow, green, blue, purple) and, if given, a note
//   mark ask                a plain marker at this moment, and a small window to give it a colour and a note
// The moment is when the command came, not when the note was finished, and the marker is kept even if the window
// is closed without a word.
//
// They are kept in <local>\markers.tsv, a line each of <time in us> <tab> <colour's name> <tab> <note>, oldest
// first, the newest MARKER_MAX of them. markers_in gives the ones in a stretch of time, for the timeline to draw
// and for a save to write into the file as chapters.

struct Marker {
    i64  t_us;      // when, on the wall clock; no two markers share one, so it also says which marker
    u8   colour;    // an index into MARKER_COLOURS; 0 = none
    char note[256]; // UTF-8, one line
};

enum { MARKER_MAX = 2048 };
struct Markers {
    SRWLOCK lock;
    bool    loaded;
    i32     count;
    Marker  items[MARKER_MAX]; // oldest first
};
static Markers g_markers;

struct MarkerColour {
    const char *name;
    u32         rgb;
};
static const MarkerColour MARKER_COLOURS[] = {
    {"", 0x8e939c}, // none: grey
    {"red", 0xf0434f}, {"orange", 0xf08a3c}, {"yellow", 0xe8c53d}, {"green", 0x3fbf74}, {"blue", 0x5b9cff}, {"purple", 0xb07cff},
};

// The colour a word names, 0 if it names none.
static u8 marker_colour_named(const char *word, size_t len)
{
    for (i32 i = 1; i < COUNT(MARKER_COLOURS); i++)
        if (strlen(MARKER_COLOURS[i].name) == len && !_strnicmp(MARKER_COLOURS[i].name, word, len)) return (u8)i;
    return 0;
}

// A note as it is kept: one line, no tabs, nothing around it.
static void marker_note_set(char (&note)[256], const char *text)
{
    while (*text == ' ') text++;
    i32 n = 0;
    for (; *text && n < (i32)sizeof note - 1; text++) note[n++] = (u8)*text < 0x20 ? ' ' : *text;
    while (n && note[n - 1] == ' ') n--;
    note[n] = 0;
}

// What follows `mark` on the command line: a colour if the first word names one, and the rest as the note.
static void marker_args(const char *arg, u8 *colour, char (&note)[256])
{
    while (*arg == ' ') arg++;
    size_t word = strcspn(arg, " ");
    *colour = marker_colour_named(arg, word);
    marker_note_set(note, *colour ? arg + word : arg);
}

static i32 marker_line(const Marker &m, char *out, i32 cap)
{
    return snprintf(out, cap, "%lld\t%s\t%s\n", (long long)m.t_us, MARKER_COLOURS[m.colour].name, m.note);
}

// A line of markers.tsv. False if it isn't one.
static bool marker_from_line(char *line, Marker *m)
{
    size_t len = strlen(line);
    while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = 0;
    char *f[3];
    if (split_tabs(line, f, 3) != 3) return false;
    *m = {};
    m->t_us = strtoll(f[0], 0, 10);
    if (m->t_us <= 0) return false;
    m->colour = marker_colour_named(f[1], strlen(f[1]));
    marker_note_set(m->note, f[2]);
    return true;
}

// Puts a marker in its place by time, moved on a microsecond at a time until its time is its own. With the list
// full, the oldest goes. Returns the time it was given.
static i64 markers_insert(Markers *ms, Marker m)
{
    if (ms->count == MARKER_MAX) {
        memmove(&ms->items[0], &ms->items[1], (MARKER_MAX - 1) * sizeof ms->items[0]);
        ms->count--;
    }
    i32 at = ms->count;
    while (at > 0 && ms->items[at - 1].t_us > m.t_us) at--;
    while (at > 0 && ms->items[at - 1].t_us == m.t_us) m.t_us++;
    while (at < ms->count && ms->items[at].t_us <= m.t_us) {
        if (ms->items[at].t_us == m.t_us) m.t_us++;
        at++;
    }
    memmove(&ms->items[at + 1], &ms->items[at], (ms->count - at) * sizeof ms->items[0]);
    ms->items[at] = m;
    ms->count++;
    return m.t_us;
}

// The markers in [from, to], oldest first. How many.
static i32 markers_range(const Markers *ms, i64 from, i64 to, Marker *out, i32 cap)
{
    i32 n = 0;
    for (i32 i = 0; i < ms->count && n < cap; i++)
        if (ms->items[i].t_us >= from && ms->items[i].t_us <= to) out[n++] = ms->items[i];
    return n;
}

static void markers_load_locked()
{
    if (g_markers.loaded) return;
    g_markers.loaded = true;
    FILE *f = _wfopen(path_join(g_app.paths.local_dir, L"markers.tsv").s, L"rb");
    if (!f) return;
    char line[512];
    Marker m;
    while (fgets(line, sizeof line, f))
        if (marker_from_line(line, &m)) markers_insert(&g_markers, m);
    fclose(f);
}

// Writes markers.tsv whole, through a temp file. Called with the lock held.
static void markers_write_locked()
{
    Path file = path_join(g_app.paths.local_dir, L"markers.tsv"), tmp = file;
    wcscat_s(tmp.s, L".tmp");
    FILE *f = _wfopen(tmp.s, L"wb");
    if (!f) { log_line("markers: couldn't write markers.tsv (%d)", (int)GetLastError()); return; }
    char line[512];
    for (i32 i = 0; i < g_markers.count; i++) {
        i32 n = marker_line(g_markers.items[i], line, sizeof line);
        if (n > 0 && n < (i32)sizeof line) fwrite(line, 1, (size_t)n, f);
    }
    fclose(f);
    if (!MoveFileExW(tmp.s, file.s, MOVEFILE_REPLACE_EXISTING)) log_line("markers: couldn't replace markers.tsv (%d)", (int)GetLastError());
}

// A new marker. Returns its time, which is how it is named afterwards.
static i64 markers_add(i64 t_us, u8 colour, const char *note)
{
    Marker m = {};
    m.t_us = t_us;
    m.colour = colour < COUNT(MARKER_COLOURS) ? colour : 0;
    marker_note_set(m.note, note);
    AcquireSRWLockExclusive(&g_markers.lock);
    markers_load_locked();
    i64 id = markers_insert(&g_markers, m);
    markers_write_locked();
    ReleaseSRWLockExclusive(&g_markers.lock);
    return id;
}

// Gives the marker made at t_us a colour and a note.
static void markers_set(i64 t_us, u8 colour, const char *note)
{
    AcquireSRWLockExclusive(&g_markers.lock);
    markers_load_locked();
    for (i32 i = 0; i < g_markers.count; i++) {
        Marker &m = g_markers.items[i];
        if (m.t_us != t_us) continue;
        m.colour = colour < COUNT(MARKER_COLOURS) ? colour : 0;
        marker_note_set(m.note, note);
        markers_write_locked();
        break;
    }
    ReleaseSRWLockExclusive(&g_markers.lock);
}

// The markers in [from, to], oldest first, for the timeline and for a save's chapters. Any thread.
static i32 markers_in(i64 from, i64 to, Marker *out, i32 cap)
{
    AcquireSRWLockExclusive(&g_markers.lock);
    markers_load_locked();
    i32 n = markers_range(&g_markers, from, to, out, cap);
    ReleaseSRWLockExclusive(&g_markers.lock);
    return n;
}

// ---------------------------------------------------------------------------------------------------------------
// The window `mark ask` opens: a field for the note and a row of colours, at the bottom of the screen in use. It
// takes the keyboard, and gives it back to the window that had it. Enter or Save keeps what was typed, as does
// clicking away; Escape leaves the marker plain. Tab steps through the colours.

struct MarkPop {
    HWND    hwnd, edit, before; // `before` had the keyboard
    WNDPROC edit_proc;
    i64     id;      // the marker being described
    u8      colour;
    i32     hot;     // under the mouse: 1..6 a colour, 7 Save, 0 none
    bool    open, tracking;
    f32     scale;
    HFONT   font;
    HBRUSH  field;
};
static MarkPop g_markpop;
static const i32 MARKPOP_W = 440, MARKPOP_H = 112; // at 100%
static const u32 MARKPOP_FIELD = 0x2c2e34;

static COLORREF markpop_rgb(u32 c) { return RGB((c >> 16) & 0xff, (c >> 8) & 0xff, c & 0xff); }

// In client pixels: 0 the note's field, 1..6 the colours, 7 Save.
static RECT markpop_rect(i32 k)
{
    f32 s = g_markpop.scale;
    i32 pad = (i32)(16 * s), w = (i32)(MARKPOP_W * s), h = (i32)(MARKPOP_H * s);
    if (k == 0) return {pad, pad, w - pad, pad + (i32)(38 * s)};
    i32 bw = (i32)(76 * s), bh = (i32)(30 * s), d = (i32)(24 * s), gap = (i32)(10 * s);
    i32 cy = h - pad - bh / 2;
    if (k == 7) return {w - pad - bw, cy - bh / 2, w - pad, cy + bh / 2};
    i32 x = pad + (k - 1) * (d + gap);
    return {x, cy - d / 2, x + d, cy + d / 2};
}

static i32 markpop_hit(LPARAM lp)
{
    POINT p = {(i16)LOWORD(lp), (i16)HIWORD(lp)};
    for (i32 k = 1; k <= 7; k++) {
        RECT r = markpop_rect(k);
        if (PtInRect(&r, p)) return k;
    }
    return 0;
}

// Closes the window. With keep, the marker takes the colour picked and the note typed.
static void markpop_close(bool keep)
{
    if (!g_markpop.open) return;
    g_markpop.open = false;
    if (keep) {
        wchar_t wide[128];
        char note[384];
        GetWindowTextW(g_markpop.edit, wide, COUNT(wide));
        wide_to_utf8(wide, note, sizeof note);
        markers_set(g_markpop.id, g_markpop.colour, note);
        log_line("marker: %s%s%s", MARKER_COLOURS[g_markpop.colour].name, g_markpop.colour && note[0] ? ", " : "", note);
        ui_wake();
    }
    ShowWindow(g_markpop.hwnd, SW_HIDE);
    if (g_markpop.before && IsWindow(g_markpop.before)) SetForegroundWindow(g_markpop.before);
}

// Shapes are drawn four times the size and scaled down, which smooths their edges (GDI's own are jagged); the
// text goes on afterwards at its real size.
static void markpop_paint(HWND hwnd)
{
    const i32 SS = 4;
    PAINTSTRUCT ps;
    HDC wdc = BeginPaint(hwnd, &ps);
    RECT r;
    GetClientRect(hwnd, &r);
    HDC big = CreateCompatibleDC(wdc), dc = CreateCompatibleDC(wdc);
    HBITMAP big_bmp = CreateCompatibleBitmap(wdc, r.right * SS, r.bottom * SS), bmp = CreateCompatibleBitmap(wdc, r.right, r.bottom);
    HGDIOBJ old_big = SelectObject(big, big_bmp), old_bmp = SelectObject(dc, bmp);
    f32 s = g_markpop.scale * SS;
    auto up = [&](i32 k) {
        RECT q = markpop_rect(k);
        return RECT{q.left * SS, q.top * SS, q.right * SS, q.bottom * SS};
    };
    auto fill = [&](u32 rgb) {
        HBRUSH br = CreateSolidBrush(markpop_rgb(rgb));
        DeleteObject(SelectObject(big, br));
    };
    RECT all = {0, 0, r.right * SS, r.bottom * SS};
    HBRUSH bg = CreateSolidBrush(markpop_rgb(COL_PANEL2));
    FillRect(big, &all, bg);
    DeleteObject(bg);
    HGDIOBJ old_pen = SelectObject(big, GetStockObject(NULL_PEN));
    HGDIOBJ old_brush = SelectObject(big, CreateSolidBrush(markpop_rgb(MARKPOP_FIELD)));
    i32 rr = (i32)(10 * s);
    RECT f = up(0);
    RoundRect(big, f.left, f.top, f.right + 1, f.bottom + 1, rr, rr);

    for (i32 k = 1; k <= 6; k++) {
        RECT c = up(k);
        if (g_markpop.colour == k) { // the one picked: a ring of the text colour around it
            i32 o = (i32)(4 * s), in = (i32)(2 * s);
            fill(COL_TEXT);
            Ellipse(big, c.left - o, c.top - o, c.right + o + 1, c.bottom + o + 1);
            fill(COL_PANEL2);
            Ellipse(big, c.left - in, c.top - in, c.right + in + 1, c.bottom + in + 1);
        }
        fill(MARKER_COLOURS[k].rgb);
        i32 grow = g_markpop.hot == k ? (i32)(1 * s) : 0;
        Ellipse(big, c.left - grow, c.top - grow, c.right + grow + 1, c.bottom + grow + 1);
    }
    fill(g_markpop.hot == 7 ? COL_ACCENT_HOVER : COL_ACCENT);
    RECT b = up(7);
    i32 br = (i32)(8 * s);
    RoundRect(big, b.left, b.top, b.right + 1, b.bottom + 1, br, br);
    DeleteObject(SelectObject(big, old_brush));
    SelectObject(big, old_pen);

    SetStretchBltMode(dc, HALFTONE);
    SetBrushOrgEx(dc, 0, 0, 0);
    StretchBlt(dc, 0, 0, r.right, r.bottom, big, 0, 0, r.right * SS, r.bottom * SS, SRCCOPY);
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ old_font = SelectObject(dc, g_markpop.font);
    SetTextColor(dc, RGB(255, 255, 255));
    RECT label = markpop_rect(7);
    DrawTextW(dc, L"Save", -1, &label, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, old_font);

    BitBlt(wdc, 0, 0, r.right, r.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(big, old_big);
    SelectObject(dc, old_bmp);
    DeleteObject(big_bmp);
    DeleteObject(bmp);
    DeleteDC(big);
    DeleteDC(dc);
    EndPaint(hwnd, &ps);
}

// The note's field: Enter keeps, Escape doesn't, Tab steps through the colours (and none).
static LRESULT CALLBACK markpop_edit_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_KEYDOWN && wp == VK_RETURN) { markpop_close(true); return 0; }
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE) { markpop_close(false); return 0; }
    if (msg == WM_KEYDOWN && wp == VK_TAB) {
        i32 n = COUNT(MARKER_COLOURS), step = GetKeyState(VK_SHIFT) < 0 ? n - 1 : 1;
        g_markpop.colour = (u8)((g_markpop.colour + step) % n);
        InvalidateRect(g_markpop.hwnd, 0, FALSE);
        return 0;
    }
    if (msg == WM_CHAR && (wp == '\r' || wp == '\t' || wp == 27)) return 0; // no beep for the keys taken above
    return CallWindowProcW(g_markpop.edit_proc, hwnd, msg, wp, lp);
}

static LRESULT CALLBACK markpop_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: markpop_paint(hwnd); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && (HWND)wp == hwnd) { SetCursor(LoadCursorW(0, (LPCWSTR)IDC_ARROW)); return TRUE; }
        break;
    case WM_CTLCOLOREDIT:
        SetTextColor((HDC)wp, markpop_rgb(COL_TEXT));
        SetBkColor((HDC)wp, markpop_rgb(MARKPOP_FIELD));
        return (LRESULT)g_markpop.field;
    case WM_MOUSEMOVE: {
        if (!g_markpop.tracking) {
            TRACKMOUSEEVENT t = {sizeof t, TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&t);
            g_markpop.tracking = true;
        }
        i32 hot = markpop_hit(lp);
        if (hot != g_markpop.hot) {
            g_markpop.hot = hot;
            InvalidateRect(hwnd, 0, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        g_markpop.tracking = false;
        g_markpop.hot = 0;
        InvalidateRect(hwnd, 0, FALSE);
        return 0;
    case WM_LBUTTONUP: {
        i32 k = markpop_hit(lp);
        if (k == 7) markpop_close(true);
        else if (k) {
            g_markpop.colour = g_markpop.colour == k ? 0 : (u8)k; // a second click takes the colour off
            InvalidateRect(hwnd, 0, FALSE);
            SetFocus(g_markpop.edit);
        }
        return 0;
    }
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) markpop_close(true); // clicked away: what was typed is kept
        else SetFocus(g_markpop.edit);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void markpop_show(i64 id)
{
    HINSTANCE inst = GetModuleHandleW(0);
    if (!g_markpop.hwnd) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = markpop_proc;
        wc.hInstance = inst;
        wc.lpszClassName = L"clipback_marker";
        RegisterClassW(&wc);
        g_markpop.hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, wc.lpszClassName, L"clipback", WS_POPUP | WS_CLIPCHILDREN,
                                         0, 0, 1, 1, 0, 0, inst, 0);
        if (!g_markpop.hwnd) return;
        // Corners rounded by Windows 11, which smooths them; a window region's are jagged. Windows 10 leaves them square.
        i32 round = 2; // DWMWCP_ROUND
        COLORREF edge = markpop_rgb(COL_LINE);
        DwmSetWindowAttribute(g_markpop.hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &round, sizeof round);
        DwmSetWindowAttribute(g_markpop.hwnd, 34 /* DWMWA_BORDER_COLOR */, &edge, sizeof edge);
        g_markpop.field = CreateSolidBrush(markpop_rgb(MARKPOP_FIELD));
        g_markpop.edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 1, 1, g_markpop.hwnd, 0, inst, 0);
        SendMessageW(g_markpop.edit, EM_SETLIMITTEXT, 80, 0);
        SendMessageW(g_markpop.edit, EM_SETCUEBANNER, TRUE, (LPARAM)L"Note");
        g_markpop.edit_proc = (WNDPROC)SetWindowLongPtrW(g_markpop.edit, GWLP_WNDPROC, (LONG_PTR)markpop_edit_proc);
    }
    if (g_markpop.open) markpop_close(true); // one being described already: it keeps what it has
    HWND front = GetForegroundWindow();
    g_markpop.before = front == g_markpop.hwnd ? 0 : front;
    g_markpop.id = id;
    g_markpop.colour = 0;
    g_markpop.hot = 0;
    SetWindowTextW(g_markpop.edit, L"");

    // At the bottom of the screen in use, sized for it: put it there first, then read that monitor's scale
    MONITORINFO mi = {sizeof mi};
    HMONITOR mon = front ? MonitorFromWindow(front, MONITOR_DEFAULTTOPRIMARY) : MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    if (!GetMonitorInfoW(mon, &mi)) return;
    RECT work = mi.rcWork;
    SetWindowPos(g_markpop.hwnd, HWND_TOPMOST, work.left + 1, work.bottom - 2, 1, 1, SWP_NOACTIVATE);
    f32 s = GetDpiForWindow(g_markpop.hwnd) / 96.f;
    if (!g_markpop.font || g_markpop.scale != s) {
        if (g_markpop.font) DeleteObject(g_markpop.font);
        g_markpop.font = CreateFontW(-(i32)(15 * s), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                     CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        g_markpop.scale = s;
        SendMessageW(g_markpop.edit, WM_SETFONT, (WPARAM)g_markpop.font, TRUE);
    }
    i32 w = (i32)(MARKPOP_W * s), h = (i32)(MARKPOP_H * s);
    SetWindowPos(g_markpop.hwnd, HWND_TOPMOST, (work.left + work.right - w) / 2, work.bottom - h - (i32)(96 * s), w, h, SWP_NOACTIVATE);
    RECT f = markpop_rect(0);
    i32 in = (i32)(12 * s), eh = (i32)(22 * s);
    SetWindowPos(g_markpop.edit, 0, f.left + in, (f.top + f.bottom - eh) / 2, f.right - f.left - 2 * in, eh, SWP_NOZORDER);
    g_markpop.open = true;
    ShowWindow(g_markpop.hwnd, SW_SHOW);
    SetForegroundWindow(g_markpop.hwnd);
    SetFocus(g_markpop.edit);
    InvalidateRect(g_markpop.hwnd, 0, FALSE);
}

// The `mark` command. Main thread.
static void marker_command(const char *arg)
{
    i64 t = now_us();
    if (!strcmp(arg, "ask")) {
        markpop_show(markers_add(t, 0, ""));
        log_line("marker: asking for a colour and a note");
    } else {
        u8 colour;
        char note[256];
        marker_args(arg, &colour, note);
        markers_add(t, colour, note);
        log_line("marker: %s%s%s", MARKER_COLOURS[colour].name, colour && note[0] ? ", " : "", note[0] ? note : colour ? "" : "plain");
    }
    ui_wake();
}
