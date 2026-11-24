// Global hotkeys ("Alt+F7" -> save 30s) through RegisterHotKey on the message window. Hotkey i has id i + 1.

struct KeyName { const char *name; u32 vk; };

static const KeyName g_key_names[] = {
    {"Space", VK_SPACE}, {"Enter", VK_RETURN}, {"Tab", VK_TAB}, {"Esc", VK_ESCAPE}, {"Backspace", VK_BACK},
    {"Insert", VK_INSERT}, {"Delete", VK_DELETE}, {"Home", VK_HOME}, {"End", VK_END}, {"PageUp", VK_PRIOR},
    {"PageDown", VK_NEXT}, {"Up", VK_UP}, {"Down", VK_DOWN}, {"Left", VK_LEFT}, {"Right", VK_RIGHT},
    {"PrintScreen", VK_SNAPSHOT}, {"Pause", VK_PAUSE}, {"ScrollLock", VK_SCROLL},
    {"Num0", VK_NUMPAD0}, {"Num1", VK_NUMPAD1}, {"Num2", VK_NUMPAD2}, {"Num3", VK_NUMPAD3}, {"Num4", VK_NUMPAD4},
    {"Num5", VK_NUMPAD5}, {"Num6", VK_NUMPAD6}, {"Num7", VK_NUMPAD7}, {"Num8", VK_NUMPAD8}, {"Num9", VK_NUMPAD9},
};

// "Ctrl+Alt+F9" -> modifiers and virtual key. Returns false if it isn't a key combination we understand.
static bool hotkey_parse(const char *text, u32 *mods, u32 *vk)
{
    *mods = MOD_NOREPEAT;
    *vk = 0;
    char buf[64];
    str_set(buf, text);
    for (char *part = strtok(buf, "+"); part; part = strtok(0, "+")) {
        part = trim(part);
        if (!_stricmp(part, "Ctrl") || !_stricmp(part, "Control")) *mods |= MOD_CONTROL;
        else if (!_stricmp(part, "Alt")) *mods |= MOD_ALT;
        else if (!_stricmp(part, "Shift")) *mods |= MOD_SHIFT;
        else if (!_stricmp(part, "Win")) *mods |= MOD_WIN;
        else if (*vk) return false; // two keys
        else if ((part[0] == 'F' || part[0] == 'f') && part[1] >= '1' && part[1] <= '9') {
            int n = atoi(part + 1);
            if (n < 1 || n > 24) return false;
            *vk = VK_F1 + (n - 1);
        } else if (strlen(part) == 1 && isalnum((u8)part[0])) {
            *vk = (u32)toupper((u8)part[0]);
        } else {
            for (i32 i = 0; i < COUNT(g_key_names); i++)
                if (!_stricmp(part, g_key_names[i].name)) *vk = g_key_names[i].vk;
            if (!*vk) return false;
        }
    }
    return *vk != 0;
}

static i32 g_hotkeys_registered;

// Re-registers every hotkey from the config (call after Options changes them).
static void hotkeys_apply(const Config &c, HWND hwnd)
{
    for (i32 i = 0; i < g_hotkeys_registered; i++) UnregisterHotKey(hwnd, i + 1);
    g_hotkeys_registered = c.hotkey_count;
    for (i32 i = 0; i < c.hotkey_count; i++) {
        u32 mods, vk;
        if (!hotkey_parse(c.hotkeys[i].keys.s, &mods, &vk)) {
            log_line("hotkey %s: not a key combination", c.hotkeys[i].keys.s);
            continue;
        }
        if (!RegisterHotKey(hwnd, i + 1, mods, vk)) {
            log_line("hotkey %s: taken by another program (%d)", c.hotkeys[i].keys.s, (int)GetLastError());
            continue;
        }
        log_line("hotkey %s -> %s", c.hotkeys[i].keys.s, c.hotkeys[i].action.s);
    }
}
