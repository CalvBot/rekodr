// busy.cpp: the Busy layer, how busy the player's hands are, a value a second for the timeline's busy bar. Raw
// Input brings every key, mouse button, wheel notch and mouse move to a window of its own, from any app; each
// second's count is all that is kept, never which key. Recorded while Config.input_busy is on (off by default).
//
// A second's value: each key or button pressed (a held key's repeats don't count), each wheel notch, and each tenth
// of the second in which the mouse moved. So 0 is nobody there, and a still mouse with hands off the keys is too.

static const char BUSY_LAYER[] = "Busy";
static const f64 BUSY_HI = 12; // the value the bar's colours top out at

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
    BusyCount count;
    i64    since;      // when the second being counted began
} g_busy;

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
        i64 now = now_us();
        layer_value(g_busy.layer, g_busy.since, busy_take(&g_busy.count));
        g_busy.since = now;
        return 0;
    }
    if (msg == WM_DESTROY) {
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
    if (g_busy.layer >= 0) g_busy.thread = CreateThread(0, 0, busy_thread, 0, 0, 0);
}
