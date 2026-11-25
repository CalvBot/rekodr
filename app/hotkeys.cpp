// Global hotkeys ("Alt+F7" -> save 30s). Hotkey i has id i + 1, and every kind of key ends as a WM_HOTKEY with that
// id on the message window:
// - keyboard keys, with any of Ctrl, Alt, Shift and Win, through RegisterHotKey;
// - the mouse's side and middle buttons ("Mouse4", "Ctrl+Mouse5"), through a low-level mouse hook. The click still
//   reaches the game;
// - controller buttons, one or held together ("PadBack+PadA"), read through XInput (Xbox and compatible pads).
// The mouse hook and the controller reads run on their own thread, and only while a hotkey uses them.

#include <xinput.h>
#pragma comment(lib, "xinput.lib")

struct KeyName { const char *name; u32 vk; };

static const KeyName g_key_names[] = {
    {"Space", VK_SPACE}, {"Enter", VK_RETURN}, {"Tab", VK_TAB}, {"Esc", VK_ESCAPE}, {"Backspace", VK_BACK},
    {"Insert", VK_INSERT}, {"Delete", VK_DELETE}, {"Home", VK_HOME}, {"End", VK_END}, {"PageUp", VK_PRIOR},
    {"PageDown", VK_NEXT}, {"Up", VK_UP}, {"Down", VK_DOWN}, {"Left", VK_LEFT}, {"Right", VK_RIGHT},
    {"PrintScreen", VK_SNAPSHOT}, {"Pause", VK_PAUSE}, {"ScrollLock", VK_SCROLL},
    {"Num0", VK_NUMPAD0}, {"Num1", VK_NUMPAD1}, {"Num2", VK_NUMPAD2}, {"Num3", VK_NUMPAD3}, {"Num4", VK_NUMPAD4},
    {"Num5", VK_NUMPAD5}, {"Num6", VK_NUMPAD6}, {"Num7", VK_NUMPAD7}, {"Num8", VK_NUMPAD8}, {"Num9", VK_NUMPAD9},
    {"Mouse4", VK_XBUTTON1}, {"Mouse5", VK_XBUTTON2}, {"MouseMiddle", VK_MBUTTON},
};

// Controller buttons: XInput's button bits, and the triggers past XINPUT_GAMEPAD_TRIGGER_THRESHOLD as two more.
static const u32 PAD_LT = 1u << 16, PAD_RT = 1u << 17;
static const KeyName g_pad_names[] = {
    {"PadA", XINPUT_GAMEPAD_A}, {"PadB", XINPUT_GAMEPAD_B}, {"PadX", XINPUT_GAMEPAD_X}, {"PadY", XINPUT_GAMEPAD_Y},
    {"PadLB", XINPUT_GAMEPAD_LEFT_SHOULDER}, {"PadRB", XINPUT_GAMEPAD_RIGHT_SHOULDER}, {"PadLT", PAD_LT},
    {"PadRT", PAD_RT}, {"PadBack", XINPUT_GAMEPAD_BACK}, {"PadStart", XINPUT_GAMEPAD_START},
    {"PadLS", XINPUT_GAMEPAD_LEFT_THUMB}, {"PadRS", XINPUT_GAMEPAD_RIGHT_THUMB}, {"PadUp", XINPUT_GAMEPAD_DPAD_UP},
    {"PadDown", XINPUT_GAMEPAD_DPAD_DOWN}, {"PadLeft", XINPUT_GAMEPAD_DPAD_LEFT},
    {"PadRight", XINPUT_GAMEPAD_DPAD_RIGHT},
};

struct HotkeyKeys {
    u32 mods; // MOD_NOREPEAT and any of MOD_CONTROL, MOD_ALT, MOD_SHIFT, MOD_WIN
    u32 vk;   // a keyboard key or VK_XBUTTON1, VK_XBUTTON2, VK_MBUTTON; 0 for a controller hotkey
    u32 pad;  // the controller buttons held together (g_pad_names); 0 for a key or mouse hotkey
};

static bool hotkey_is_mouse(const HotkeyKeys &k)
{
    return k.vk == VK_XBUTTON1 || k.vk == VK_XBUTTON2 || k.vk == VK_MBUTTON;
}

// "Ctrl+Alt+F9", "Shift+Mouse4" or "PadLB+PadA" -> its keys. Returns false if it isn't a combination we understand:
// one key or mouse button with any modifiers, or controller buttons alone.
static bool hotkey_parse(const char *text, HotkeyKeys *k)
{
    *k = {MOD_NOREPEAT, 0, 0};
    char buf[64];
    str_set(buf, text);
    for (char *part = strtok(buf, "+"); part; part = strtok(0, "+")) {
        part = trim(part);
        u32 pad = 0;
        for (const KeyName &p : g_pad_names)
            if (!_stricmp(part, p.name)) pad = p.vk;
        if (pad) {
            k->pad |= pad;
            continue;
        }
        if (!_stricmp(part, "Ctrl") || !_stricmp(part, "Control")) k->mods |= MOD_CONTROL;
        else if (!_stricmp(part, "Alt")) k->mods |= MOD_ALT;
        else if (!_stricmp(part, "Shift")) k->mods |= MOD_SHIFT;
        else if (!_stricmp(part, "Win")) k->mods |= MOD_WIN;
        else if (k->vk) return false; // two keys
        else if ((part[0] == 'F' || part[0] == 'f') && part[1] >= '1' && part[1] <= '9') {
            int n = atoi(part + 1);
            if (n < 1 || n > 24) return false;
            k->vk = VK_F1 + (n - 1);
        } else if (strlen(part) == 1 && isalnum((u8)part[0])) {
            k->vk = (u32)toupper((u8)part[0]);
        } else {
            for (const KeyName &n : g_key_names)
                if (!_stricmp(part, n.name)) k->vk = n.vk;
            if (!k->vk) return false;
        }
    }
    if (k->pad) return !k->vk && k->mods == MOD_NOREPEAT;
    return k->vk != 0;
}

// The name of controller buttons held together, "PadLB+PadA", in g_pad_names' order. "" for none.
static void hotkey_pad_name(u32 pad, char *out, i32 cap)
{
    out[0] = 0;
    for (const KeyName &p : g_pad_names)
        if (pad & p.vk) snprintf(out + strlen(out), cap - strlen(out), "%s%s", out[0] ? "+" : "", p.name);
}

// Whether controller buttons held together just became all held: none of a hotkey's buttons count until the last
// one goes down, and holding them fires it once.
static bool hotkey_pad_fired(u32 before, u32 now, u32 mask)
{
    return mask && (now & mask) == mask && (before & mask) != mask;
}

// The buttons held on controller `slot` (0 to 3), 0 when none is there.
static u32 hotkey_pad_read(i32 slot, bool *connected)
{
    XINPUT_STATE st = {};
    *connected = XInputGetState((DWORD)slot, &st) == ERROR_SUCCESS;
    if (!*connected) return 0;
    u32 b = st.Gamepad.wButtons;
    if (st.Gamepad.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) b |= PAD_LT;
    if (st.Gamepad.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) b |= PAD_RT;
    return b;
}

// The buttons held on every controller together, for setting a hotkey's keys in Options.
static u32 hotkey_pads_now()
{
    u32 b = 0;
    for (i32 s = 0; s < XUSER_MAX_COUNT; s++) {
        bool on;
        b |= hotkey_pad_read(s, &on);
    }
    return b;
}

// ---------------------------------------------------------------------------------------------------------------
// The input thread: the mouse hook and the controller reads.

enum { INPUT_SET = WM_APP + 1 };
struct InputBind { HotkeyKeys keys; i32 index; };
static struct {
    SRWLOCK   lock;
    InputBind binds[16]; // the mouse and controller hotkeys
    i32       count;
    HWND      target;    // gets WM_HOTKEY
    DWORD     tid;
} g_input;

static LRESULT CALLBACK hotkey_mouse_hook(int code, WPARAM wp, LPARAM lp)
{
    if (code == HC_ACTION && (wp == WM_XBUTTONDOWN || wp == WM_MBUTTONDOWN)) {
        const MSLLHOOKSTRUCT *m = (const MSLLHOOKSTRUCT *)lp;
        u32 vk = wp == WM_MBUTTONDOWN ? VK_MBUTTON : HIWORD(m->mouseData) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2;
        auto down = [](int key) { return (GetAsyncKeyState(key) & 0x8000) != 0; };
        u32 mods = MOD_NOREPEAT | (down(VK_CONTROL) ? MOD_CONTROL : 0) | (down(VK_MENU) ? MOD_ALT : 0) |
                   (down(VK_SHIFT) ? MOD_SHIFT : 0) | (down(VK_LWIN) || down(VK_RWIN) ? MOD_WIN : 0);
        AcquireSRWLockShared(&g_input.lock);
        for (i32 i = 0; i < g_input.count; i++)
            if (g_input.binds[i].keys.vk == vk && g_input.binds[i].keys.mods == mods)
                PostMessageW(g_input.target, WM_HOTKEY, (WPARAM)g_input.binds[i].index + 1, 0);
        ReleaseSRWLockShared(&g_input.lock);
    }
    return CallNextHookEx(0, code, wp, lp);
}

static DWORD WINAPI hotkey_input_thread(void *)
{
    HHOOK hook = 0;
    UINT_PTR timer = 0;
    u32 before[XUSER_MAX_COUNT] = {};
    u64 retry[XUSER_MAX_COUNT] = {}; // a slot with no controller is asked again only every 2 s: that read is slow
    MSG msg;
    PeekMessageW(&msg, 0, 0, 0, PM_NOREMOVE); // the thread's queue, before hotkeys_apply posts to it
    while (GetMessageW(&msg, 0, 0, 0) > 0) {
        if (msg.message == INPUT_SET) {
            bool mouse = false, pad = false;
            AcquireSRWLockShared(&g_input.lock);
            for (i32 i = 0; i < g_input.count; i++) {
                mouse |= hotkey_is_mouse(g_input.binds[i].keys);
                pad |= g_input.binds[i].keys.pad != 0;
            }
            ReleaseSRWLockShared(&g_input.lock);
            if (mouse && !hook) hook = SetWindowsHookExW(WH_MOUSE_LL, hotkey_mouse_hook, GetModuleHandleW(0), 0);
            if (!mouse && hook) UnhookWindowsHookEx(hook), hook = 0;
            if (pad && !timer) timer = SetTimer(0, 0, 16, 0);
            if (!pad && timer) KillTimer(0, timer), timer = 0;
            if (mouse && !hook) log_line("hotkeys: couldn't watch the mouse (%d)", (int)GetLastError());
        } else if (msg.message == WM_TIMER) {
            u64 now = GetTickCount64();
            for (i32 s = 0; s < XUSER_MAX_COUNT; s++) {
                if (now < retry[s]) continue;
                bool on;
                u32 b = hotkey_pad_read(s, &on);
                if (!on) retry[s] = now + 2000;
                AcquireSRWLockShared(&g_input.lock);
                for (i32 i = 0; i < g_input.count; i++)
                    if (hotkey_pad_fired(before[s], b, g_input.binds[i].keys.pad))
                        PostMessageW(g_input.target, WM_HOTKEY, (WPARAM)g_input.binds[i].index + 1, 0);
                ReleaseSRWLockShared(&g_input.lock);
                before[s] = b;
            }
        }
    }
    return 0;
}

// Hands the input thread its mouse and controller hotkeys (none: it stops watching).
static void hotkey_input_set(const InputBind *binds, i32 n, HWND hwnd)
{
    AcquireSRWLockExclusive(&g_input.lock);
    if (n) memcpy(g_input.binds, binds, n * sizeof binds[0]);
    g_input.count = n;
    g_input.target = hwnd;
    bool start = n && !g_input.tid;
    ReleaseSRWLockExclusive(&g_input.lock);
    if (start) {
        HANDLE t = CreateThread(0, 0, hotkey_input_thread, 0, 0, &g_input.tid);
        if (t) CloseHandle(t);
        else g_input.tid = 0;
    }
    // The thread may not have its queue yet: then it reads the binds as it starts its loop
    for (i32 i = 0; i < 50 && g_input.tid && !PostThreadMessageW(g_input.tid, INPUT_SET, 0, 0); i++) Sleep(2);
}

static i32 g_hotkeys_registered;

// Drops every hotkey (while Options sets one's keys, so pressing them doesn't fire the old one).
static void hotkeys_clear(HWND hwnd)
{
    for (i32 i = 0; i < g_hotkeys_registered; i++) UnregisterHotKey(hwnd, i + 1);
    g_hotkeys_registered = 0;
    hotkey_input_set(0, 0, hwnd);
}

// Re-registers every hotkey from the config (call after Options changes them).
static void hotkeys_apply(const Config &c, HWND hwnd)
{
    hotkeys_clear(hwnd);
    g_hotkeys_registered = c.hotkey_count;
    InputBind binds[COUNT(c.hotkeys)];
    i32 n = 0;
    for (i32 i = 0; i < c.hotkey_count; i++) {
        HotkeyKeys k;
        if (!hotkey_parse(c.hotkeys[i].keys.s, &k)) {
            log_line("hotkey %s: not a key combination", c.hotkeys[i].keys.s);
            continue;
        }
        if (k.pad || hotkey_is_mouse(k)) binds[n++] = {k, i};
        else if (!RegisterHotKey(hwnd, i + 1, k.mods, k.vk)) {
            log_line("hotkey %s: taken by another program (%d)", c.hotkeys[i].keys.s, (int)GetLastError());
            continue;
        }
        log_line("hotkey %s -> %s", c.hotkeys[i].keys.s, c.hotkeys[i].action.s);
    }
    hotkey_input_set(binds, n, hwnd);
}
