// The tray icon: red dot while recording, grey with a pause sign while paused. Left click opens the window; right
// click has the menu. Icons 1-3 come from rekodr.rc (app, recording, paused).

#define WM_APP_TRAY (WM_APP + 2)

enum TrayCommand : u32 { TRAY_OPEN = 1, TRAY_SAVE, TRAY_PAUSE, TRAY_FOLDER, TRAY_QUIT };

// What the menu's Save clip does: the first save hotkey's tap length, else 30 s. `label` is its menu text,
// "Save clip (30 s)".
static void tray_save(const Config &c, char *action, i32 action_cap, wchar_t *label, i32 label_cap)
{
    snprintf(action, action_cap, "save 30s");
    for (i32 i = 0; i < c.hotkey_count; i++)
        if (!strncmp(c.hotkeys[i].action.s, "save ", 5)) {
            snprintf(action, action_cap, "%s", c.hotkeys[i].action.s);
            break;
        }
    char *end = 0;
    f64 v = strtod(action + 5, &end);
    const wchar_t *unit = *end == 'h' ? L"h" : *end == 'm' ? L"min" : L"s";
    swprintf(label, label_cap, L"Save clip (%g %s)", v, unit);
}

static UINT g_taskbar_created; // Explorer restarted: add the icon again
static bool g_tray_paused_shown;

static void tray_update(HWND hwnd, bool add)
{
    NOTIFYICONDATAW n = {sizeof n};
    n.hWnd = hwnd;
    n.uID = 1;
    n.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_SHOWTIP;
    n.uCallbackMessage = WM_APP_TRAY;
    bool paused = g_app.recorder.paused;
    n.hIcon = (HICON)LoadImageW(GetModuleHandleW(0), MAKEINTRESOURCEW(paused ? 3 : 2), IMAGE_ICON,
                                GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    wcscpy_s(n.szTip, paused ? L"" APP_NAME " (not recording)" : L"" APP_NAME);
    if (add) {
        Shell_NotifyIconW(NIM_ADD, &n);
        n.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &n);
    } else {
        Shell_NotifyIconW(NIM_MODIFY, &n);
    }
    if (n.hIcon) DestroyIcon(n.hIcon);
    g_tray_paused_shown = paused;
}

static void tray_init(HWND hwnd)
{
    g_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    tray_update(hwnd, true);
}

static void tray_remove(HWND hwnd)
{
    NOTIFYICONDATAW n = {sizeof n};
    n.hWnd = hwnd;
    n.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &n);
}

// Returns the command the menu picked as text (empty if none), for run_command.
static void tray_menu(HWND hwnd, char *command, i32 command_size)
{
    command[0] = 0;
    HMENU menu = CreatePopupMenu();
    char save[64];
    wchar_t save_label[64];
    tray_save(g_app.config, save, sizeof save, save_label, COUNT(save_label));
    AppendMenuW(menu, MF_STRING, TRAY_OPEN, L"Open " APP_NAME);
    AppendMenuW(menu, MF_STRING, TRAY_SAVE, save_label);
    AppendMenuW(menu, MF_STRING, TRAY_PAUSE, g_app.recorder.paused ? L"Start recording" : L"Stop recording");
    AppendMenuW(menu, MF_STRING, TRAY_FOLDER, L"Open clips folder");
    AppendMenuW(menu, MF_SEPARATOR, 0, 0);
    AppendMenuW(menu, MF_STRING, TRAY_QUIT, L"Quit");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd); // so the menu closes when clicking elsewhere
    u32 cmd = (u32)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hwnd, 0);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);

    switch (cmd) {
    case TRAY_OPEN: snprintf(command, command_size, "open"); break;
    case TRAY_SAVE: snprintf(command, command_size, "%s", save); break;
    case TRAY_PAUSE: snprintf(command, command_size, "toggle"); break;
    case TRAY_FOLDER: snprintf(command, command_size, "folder"); break;
    case TRAY_QUIT: snprintf(command, command_size, "quit"); break;
    default: break;
    }
}
