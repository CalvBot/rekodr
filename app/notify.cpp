// notify.cpp: the desktop "Clip saved" popup. A small window in the corner of a chosen monitor, over everything,
// that never takes the focus from the game. Edit and Share open the file in the editor on that tab. It goes after
// 6 s unless the mouse is on it; a click anywhere else on it closes it.

struct Notify {
    HWND    hwnd;
    char    path[MAX_PATH * 2];
    wchar_t title[32];
    wchar_t file[MAX_PATH];
    i32     hot;     // the button under the mouse: 0 none, 1 Edit, 2 Share
    bool    tracking;
    f32     scale;   // the fonts were made for this
    HFONT   font, bold, icons;
};
static Notify g_notify;
static const UINT_PTR NOTIFY_TIMER = 1;
static const UINT     NOTIFY_MS = 6000;
static const i32      NOTIFY_W = 380, NOTIFY_H = 76; // at 100%

static COLORREF notify_rgb(u32 c) { return RGB((c >> 16) & 0xff, (c >> 8) & 0xff, c & 0xff); }

static void notify_hide()
{
    if (!g_notify.hwnd) return;
    KillTimer(g_notify.hwnd, NOTIFY_TIMER);
    ShowWindow(g_notify.hwnd, SW_HIDE);
}

// A button's rectangle in client pixels: 1 Edit, 2 Share
static RECT notify_button(i32 k)
{
    f32 s = g_notify.scale;
    i32 bw = (i32)(70 * s), bh = (i32)(32 * s), pad = (i32)(16 * s), gap = (i32)(8 * s);
    i32 w = (i32)(NOTIFY_W * s), h = (i32)(NOTIFY_H * s);
    i32 x = k == 2 ? w - pad - bw : w - pad - 2 * bw - gap, y = (h - bh) / 2;
    return {x, y, x + bw, y + bh};
}

static void notify_fonts(f32 s)
{
    if (g_notify.font && g_notify.scale == s) return;
    if (g_notify.font) DeleteObject(g_notify.font), DeleteObject(g_notify.bold), DeleteObject(g_notify.icons);
    g_notify.scale = s;
    auto make = [&](i32 px, i32 weight, const wchar_t *face) {
        return CreateFontW(-(i32)(px * s), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                           CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
    };
    g_notify.font = make(13, FW_NORMAL, L"Segoe UI");
    g_notify.bold = make(15, FW_SEMIBOLD, L"Segoe UI");
    g_notify.icons = make(18, FW_NORMAL, L"Segoe MDL2 Assets");
}

// Shapes are drawn four times the size and scaled down, which smooths their edges (GDI's own are jagged); the
// text and the tick go on afterwards at their real size.
static void notify_paint(HWND hwnd)
{
    const i32 SS = 4;
    PAINTSTRUCT ps;
    HDC wdc = BeginPaint(hwnd, &ps);
    RECT r;
    GetClientRect(hwnd, &r);
    HDC big = CreateCompatibleDC(wdc), dc = CreateCompatibleDC(wdc);
    HBITMAP big_bmp = CreateCompatibleBitmap(wdc, r.right * SS, r.bottom * SS), bmp = CreateCompatibleBitmap(wdc, r.right, r.bottom);
    HGDIOBJ old_big = SelectObject(big, big_bmp), old_bmp = SelectObject(dc, bmp);
    f32 s = g_notify.scale;
    i32 pad = (i32)(16 * s), d = (i32)(36 * s), cy = r.bottom / 2;

    RECT all = {0, 0, r.right * SS, r.bottom * SS};
    HBRUSH bg = CreateSolidBrush(notify_rgb(COL_PANEL2));
    FillRect(big, &all, bg);
    DeleteObject(bg);
    HGDIOBJ old_pen = SelectObject(big, GetStockObject(NULL_PEN));
    // The accent circle the tick sits in
    HGDIOBJ old_brush = SelectObject(big, CreateSolidBrush(notify_rgb(COL_ACCENT)));
    Ellipse(big, pad * SS, (cy - d / 2) * SS, (pad + d) * SS, (cy + d / 2) * SS);
    // Edit (grey) and Share (accent)
    for (i32 k = 1; k <= 2; k++) {
        RECT q = notify_button(k);
        bool hot = g_notify.hot == k;
        u32 fill = k == 2 ? (hot ? COL_ACCENT_HOVER : COL_ACCENT) : (hot ? 0x34373e : 0x2c2e34);
        DeleteObject(SelectObject(big, CreateSolidBrush(notify_rgb(fill))));
        i32 rr = (i32)(8 * s) * SS;
        RoundRect(big, q.left * SS, q.top * SS, q.right * SS + 1, q.bottom * SS + 1, rr, rr);
    }
    DeleteObject(SelectObject(big, old_brush));
    SelectObject(big, old_pen);

    SetStretchBltMode(dc, HALFTONE);
    SetBrushOrgEx(dc, 0, 0, 0);
    StretchBlt(dc, 0, 0, r.right, r.bottom, big, 0, 0, r.right * SS, r.bottom * SS, SRCCOPY);
    SetBkMode(dc, TRANSPARENT);

    HGDIOBJ old_font = SelectObject(dc, g_notify.icons);
    SetTextColor(dc, RGB(255, 255, 255));
    RECT ir = {pad, cy - d / 2, pad + d, cy + d / 2};
    const wchar_t tick[] = {0xE73E, 0}; // Segoe MDL2 Assets: CheckMark
    DrawTextW(dc, tick, 1, &ir, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    // What was saved
    RECT e = notify_button(1);
    i32 tx = pad + d + (i32)(14 * s), tr = e.left - (i32)(12 * s);
    SelectObject(dc, g_notify.bold);
    SetTextColor(dc, notify_rgb(COL_TEXT));
    RECT t1 = {tx, cy - (i32)(20 * s), tr, cy};
    DrawTextW(dc, g_notify.title, -1, &t1, DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, g_notify.font);
    SetTextColor(dc, notify_rgb(COL_DIM));
    RECT t2 = {tx, cy + (i32)(2 * s), tr, cy + (i32)(22 * s)};
    DrawTextW(dc, g_notify.file, -1, &t2, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    for (i32 k = 1; k <= 2; k++) {
        RECT q = notify_button(k);
        SetTextColor(dc, k == 2 ? RGB(255, 255, 255) : notify_rgb(COL_TEXT));
        DrawTextW(dc, k == 1 ? L"Edit" : L"Share", -1, &q, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
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

static i32 notify_hit(LPARAM lp)
{
    POINT p = {(i16)LOWORD(lp), (i16)HIWORD(lp)};
    for (i32 k = 1; k <= 2; k++) {
        RECT b = notify_button(k);
        if (PtInRect(&b, p)) return k;
    }
    return 0;
}

static LRESULT CALLBACK notify_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE; // the game keeps the keyboard
    case WM_PAINT: notify_paint(hwnd); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SETCURSOR: SetCursor(LoadCursorW(0, (LPCWSTR)IDC_ARROW)); return TRUE;
    case WM_MOUSEMOVE: {
        if (!g_notify.tracking) {
            TRACKMOUSEEVENT t = {sizeof t, TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&t);
            g_notify.tracking = true;
        }
        i32 hot = notify_hit(lp);
        if (hot != g_notify.hot) {
            g_notify.hot = hot;
            InvalidateRect(hwnd, 0, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        g_notify.tracking = false;
        g_notify.hot = 0;
        InvalidateRect(hwnd, 0, FALSE);
        SetTimer(hwnd, NOTIFY_TIMER, 2000, 0); // a moment more once the mouse leaves
        return 0;
    case WM_LBUTTONUP: {
        i32 k = notify_hit(lp);
        notify_hide();
        if (k) {
            log_line("saved popup: %s %s", k == 1 ? "edit" : "share", g_notify.path);
            ui_show(SCREEN_EDITOR);
            editor_open_file(g_notify.path);
            g_app.editor.tab = k == 2 ? 1 : 0;
        }
        return 0;
    }
    case WM_TIMER: {
        POINT p;
        RECT r;
        GetCursorPos(&p);
        GetWindowRect(hwnd, &r);
        if (!PtInRect(&r, p)) notify_hide();
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// The monitor the popup goes on: Config.saved_popup, -2 = the one recorded, -1 = none. False when there's none.
static bool notify_monitor(RECT *work)
{
    i32 out = g_app.config.saved_popup == -2 ? g_app.config.output_idx : g_app.config.saved_popup;
    if (out < 0) return false;
    i32 w, h, hz;
    u64 hmon = 0;
    HMONITOR m = monitor_mode(out, &w, &h, &hz, &hmon) && hmon ? (HMONITOR)hmon : MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = {sizeof mi};
    if (!GetMonitorInfoW(m, &mi)) return false;
    *work = mi.rcWork;
    return true;
}

static void notify_show(const char *title, const char *path)
{
    RECT work;
    if (!notify_monitor(&work)) return;
    if (!g_notify.hwnd) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = notify_proc;
        wc.hInstance = GetModuleHandleW(0);
        wc.lpszClassName = L"clipback_saved";
        RegisterClassW(&wc);
        g_notify.hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"clipback",
                                        WS_POPUP, work.right - 1, work.bottom - 1, 1, 1, 0, 0, wc.hInstance, 0);
        if (!g_notify.hwnd) return;
        // Corners rounded by Windows 11, which smooths them; a window region's are jagged. Windows 10 leaves them square.
        i32 round = 2; // DWMWCP_ROUND
        COLORREF edge = notify_rgb(COL_LINE);
        DwmSetWindowAttribute(g_notify.hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &round, sizeof round);
        DwmSetWindowAttribute(g_notify.hwnd, 34 /* DWMWA_BORDER_COLOR */, &edge, sizeof edge);
    }
    str_set(g_notify.path, path);
    utf8_to_wide(title, g_notify.title, COUNT(g_notify.title));
    const char *name = strrchr(path, '\\');
    utf8_to_wide(name ? name + 1 : path, g_notify.file, COUNT(g_notify.file));
    g_notify.hot = 0;
    // Sized for the monitor it lands on: put it there first, then read that monitor's scale
    SetWindowPos(g_notify.hwnd, HWND_TOPMOST, work.right - 2, work.bottom - 2, 1, 1, SWP_NOACTIVATE);
    f32 s = GetDpiForWindow(g_notify.hwnd) / 96.f;
    notify_fonts(s);
    i32 w = (i32)(NOTIFY_W * s), h = (i32)(NOTIFY_H * s), m = (i32)(16 * s);
    SetWindowPos(g_notify.hwnd, HWND_TOPMOST, work.right - w - m, work.bottom - h - m, w, h, SWP_NOACTIVATE);
    InvalidateRect(g_notify.hwnd, 0, FALSE);
    ShowWindow(g_notify.hwnd, SW_SHOWNOACTIVATE);
    SetTimer(g_notify.hwnd, NOTIFY_TIMER, NOTIFY_MS, 0);
}

// A save finished: the popup, unless clipback's own window is in front (it shows the save itself).
static void notify_saved(SaveKind kind, const char *path)
{
    if (kind == SAVE_PREVIEW) return;
    if (g_app.ui.shown && GetForegroundWindow() == g_app.ui.hwnd) return;
    notify_show(kind == SAVE_SESSION ? "Session saved" : "Clip saved", path);
}
