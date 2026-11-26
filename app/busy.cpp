// busy.cpp: the Busy layer, how busy the player's hands are, a value a second for the timeline's busy bar. Raw
// Input brings every key, mouse button, wheel notch and mouse move to a window of its own, from any app; each
// second's count is all that is kept, never which key. Recorded while Config.input_busy is on (off by default).
//
// A second's value: each key or button pressed (a held key's repeats don't count), each wheel notch, and each tenth
// of the second in which the mouse moved. So 0 is nobody there, and a still mouse with hands off the keys is too.
//
// AFK: with a game in front, seconds of 0 for Config.afk_after_min or longer are a span on the Away layer, from the
// first quiet second to the first busy one (or the game leaving the front). The busy bar draws it grey.

static const char BUSY_LAYER[] = "Busy";
static const f64 BUSY_HI = 12; // the value the bar's colours top out at
static const char BUSY_AWAY_LAYER[] = "Busy/away";

struct BusyAway {
    i64  quiet_since; // the first second of 0 in a row with a game in front, 0 when there's none
    bool away;        // an AFK span is open
};

// The second from `since` was worth `v`, with or without a game in front. Returns 1 when an AFK span begins (at
// *at), 2 when the open one ends (at *at), else 0.
static i32 busy_away_step(BusyAway *w, i64 since, f64 v, bool game, i64 afk_us, i64 *at)
{
    if (game && v <= 0) {
        if (!w->quiet_since) w->quiet_since = since;
        if (!w->away && since + 1000000 - w->quiet_since >= afk_us) {
            w->away = true;
            *at = w->quiet_since;
            return 1;
        }
        return 0;
    }
    w->quiet_since = 0;
    if (!w->away) return 0;
    w->away = false;
    *at = since;
    return 2;
}

struct BusyCount {
    i32 presses;
    u32 moved;     // the tenths of the second the mouse moved in, a bit each
    u8  held[32];  // the keys down now (by virtual key), so a held key's repeats aren't presses
};

// One Raw Input event, at t_us
static void busy_feed(BusyCount *b, const RAWINPUT &in, i64 t_us)
{
    if (in.header.dwType == RIM_TYPEKEYBOARD) {
        const RAWKEYBOARD &k = in.data.keyboard;
        if (k.VKey == 0 || k.VKey >= 255) return; // a fake key, sent as part of another
        u8 bit = (u8)(1u << (k.VKey & 7));
        u8 &byte = b->held[k.VKey >> 3];
        if (k.Flags & RI_KEY_BREAK) byte &= (u8)~bit;
        else if (!(byte & bit)) byte |= bit, b->presses++;
    } else if (in.header.dwType == RIM_TYPEMOUSE) {
        const RAWMOUSE &m = in.data.mouse;
        static const USHORT downs[] = {RI_MOUSE_BUTTON_1_DOWN, RI_MOUSE_BUTTON_2_DOWN, RI_MOUSE_BUTTON_3_DOWN,
                                       RI_MOUSE_BUTTON_4_DOWN, RI_MOUSE_BUTTON_5_DOWN};
        for (USHORT d : downs) b->presses += (m.usButtonFlags & d) != 0;
        if (m.usButtonFlags & (RI_MOUSE_WHEEL | RI_MOUSE_HWHEEL)) {
            i32 notches = abs((i32)(SHORT)m.usButtonData) / WHEEL_DELTA;
            b->presses += notches ? notches : 1;
        }
        if (m.lLastX || m.lLastY) b->moved |= 1u << (u32)((t_us / 100000) % 10);
    }
}

// The second's value, and a fresh count for the next
static f64 busy_take(BusyCount *b)
{
    i32 moved = 0;
    for (u32 m = b->moved; m; m &= m - 1) moved++;
    f64 v = b->presses + moved;
    b->presses = 0, b->moved = 0;
    return v;
}

static struct {
    HANDLE thread;
    HWND   hwnd;
    volatile bool on;
    i32    layer;      // the layer's id
    i32    away_layer;
    BusyCount count;
    i64    since;      // when the second being counted began
    BusyAway away;
    volatile bool game;   // a game is in front (busy_tick)
    volatile i64  afk_us; // Config.afk_after_min
} g_busy;

// The AFK span's edges, on the Away layer
static void busy_away_mark(i32 what, i64 at)
{
    if (what == 1) layer_hold(g_busy.away_layer, at, "AFK");
    if (what == 2) layer_hold(g_busy.away_layer, at, "");
    if (what) log_line("busy: AFK %s", what == 1 ? "from a while ago" : "ended");
}

static LRESULT CALLBACK busy_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_INPUT) {
        RAWINPUT in;
        UINT size = sizeof in;
        if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, &in, &size, sizeof in.header) != (UINT)-1)
            busy_feed(&g_busy.count, in, now_us());
        return DefWindowProcW(h, msg, wp, lp);
    }
    if (msg == WM_TIMER) {
        if (!g_busy.on) {
            DestroyWindow(h);
            return 0;
        }
        i64 now = now_us(), at = 0;
        f64 v = busy_take(&g_busy.count);
        layer_value(g_busy.layer, g_busy.since, v);
        busy_away_mark(busy_away_step(&g_busy.away, g_busy.since, v, g_busy.game, g_busy.afk_us, &at), at);
        g_busy.since = now;
        return 0;
    }
    if (msg == WM_DESTROY) {
        i64 at = 0;
        busy_away_mark(busy_away_step(&g_busy.away, now_us(), 1, false, 0, &at), at);
        RAWINPUTDEVICE off[2] = {{1, 6, RIDEV_REMOVE, 0}, {1, 2, RIDEV_REMOVE, 0}};
        RegisterRawInputDevices(off, 2, sizeof off[0]);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static DWORD WINAPI busy_thread(void *)
{
    WNDCLASSW wc = {};
    wc.lpfnWndProc = busy_proc;
    wc.hInstance = GetModuleHandleW(0);
    wc.lpszClassName = L"RekodrBusy";
    RegisterClassW(&wc);
    HWND h = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, 0, wc.hInstance, 0);
    RAWINPUTDEVICE on[2] = {{1, 6, RIDEV_INPUTSINK, h}, {1, 2, RIDEV_INPUTSINK, h}}; // keyboards, mice
    if (!h || !RegisterRawInputDevices(on, 2, sizeof on[0])) {
        log_line("busy: Raw Input couldn't be opened (%d)", (int)GetLastError());
        if (h) DestroyWindow(h);
    } else {
        g_busy.hwnd = h;
        g_busy.count = {};
        g_busy.away = {};
        g_busy.since = now_us();
        SetTimer(h, 1, 1000, 0);
        log_line("busy: counting keys and the mouse");
        MSG m;
        while (GetMessageW(&m, 0, 0, 0) > 0) DispatchMessageW(&m);
        log_line("busy: stopped");
    }
    g_busy.hwnd = 0;
    return 0;
}

static bool sessions_game_in_front(); // sessions.cpp

// Once a second, main thread: whether a game is in front, and the AFK setting
static void busy_tick()
{
    if (!g_busy.on) return;
    g_busy.game = sessions_game_in_front();
    g_busy.afk_us = (i64)(g_app.config.afk_after_min * 60e6);
}

// At startup and when its switch changes. Main thread.
static void busy_apply()
{
    bool want = g_app.config.input_busy;
    g_busy.on = want;
    if (!want) {
        if (g_busy.thread) {
            if (g_busy.hwnd) PostMessageW(g_busy.hwnd, WM_CLOSE, 0, 0);
            WaitForSingleObject(g_busy.thread, 3000);
            CloseHandle(g_busy.thread);
            g_busy.thread = 0;
        }
        return;
    }
    if (g_busy.thread) return;
    g_busy.layer = layer_register(BUSY_LAYER, LAYER_VALUE, "Busy", "/s", 0, BUSY_HI);
    g_busy.away_layer = layer_register(BUSY_AWAY_LAYER, LAYER_SPANS, "AFK", 0, 0, 0);
    busy_tick();
    if (g_busy.layer >= 0 && g_busy.away_layer >= 0) g_busy.thread = CreateThread(0, 0, busy_thread, 0, 0, 0);
}
