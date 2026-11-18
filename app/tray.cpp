// The tray icon: red dot while recording, grey with a pause sign while paused. Left click opens the window; right
// click has the menu. Icons 1-3 come from clipback.rc (app, recording, paused).

#define WM_APP_TRAY (WM_APP + 2)

enum TrayCommand : u32 {
    TRAY_OPEN = 1,
    TRAY_TIMELINE,
    TRAY_PAUSE,
    TRAY_OPTIONS,
    TRAY_FOLDER,
    TRAY_QUIT,
    TRAY_SAVE_FIRST = 100, // + index into g_tray_saves
};

static const char *g_tray_saves[] = {"30s", "2m", "10m", "30m", "1h"};
static const wchar_t *g_tray_save_labels[] = {L"Save last 30 s", L"Save last 2 min", L"Save last 10 min",
                                              L"Save last 30 min", L"Save last hour"};

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
    wcscpy_s(n.szTip, paused ? L"clipback (not recording)" : L"clipback");
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
    AppendMenuW(menu, MF_STRING, TRAY_OPEN, L"Open clipback");
    AppendMenuW(menu, MF_STRING, TRAY_TIMELINE, L"Timeline");
    AppendMenuW(menu, MF_SEPARATOR, 0, 0);
    for (i32 i = 0; i < COUNT(g_tray_saves); i++) AppendMenuW(menu, MF_STRING, TRAY_SAVE_FIRST + i, g_tray_save_labels[i]);
    AppendMenuW(menu, MF_SEPARATOR, 0, 0);
    AppendMenuW(menu, MF_STRING, TRAY_PAUSE, g_app.recorder.paused ? L"Start recording" : L"Stop recording");
    AppendMenuW(menu, MF_STRING, TRAY_OPTIONS, L"Options");
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
    case TRAY_TIMELINE: snprintf(command, command_size, "timeline"); break;
    case TRAY_PAUSE: snprintf(command, command_size, "toggle"); break;
    case TRAY_OPTIONS: snprintf(command, command_size, "options"); break;
    case TRAY_FOLDER: snprintf(command, command_size, "folder"); break;
    case TRAY_QUIT: snprintf(command, command_size, "quit"); break;
    default:
        if (cmd >= TRAY_SAVE_FIRST && cmd < TRAY_SAVE_FIRST + (u32)COUNT(g_tray_saves))
            snprintf(command, command_size, "save %s", g_tray_saves[cmd - TRAY_SAVE_FIRST]);
    }
}
