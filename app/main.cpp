// Startup, the single instance, commands and the main loop.
//
// `clipback.exe` with no arguments starts clipback (or opens the window of the copy already running).
// `clipback.exe <command>` hands the command to the running copy as WM_COPYDATA and exits; Stream Deck keys use
// this. With no running copy it starts clipback and runs the command there.
//
// Commands: open | timeline | options | save <length> | pause | resume | toggle | folder | quit | dev [off]

#define COPYDATA_COMMAND 0x4B424C43 // "CLBK"

static const wchar_t *MSG_CLASS = L"clipback.msg";

#define TIMER_TICK 1 // once a second on the main thread, for anything that checks the clock

// Run on the main thread for every command, whatever it came from.
static void run_command(const char *text)
{
    char cmd[512];
    str_set(cmd, text);
    char *arg = strchr(cmd, ' ');
    if (arg) *arg++ = 0;
    else arg = cmd + strlen(cmd);
    arg = trim(arg);
    DEV_LOG("command \"%s\"", text);

    if (!*cmd || !strcmp(cmd, "open")) ui_show(SCREEN_LIBRARY);
    else if (!strcmp(cmd, "timeline")) ui_show(SCREEN_TIMELINE);
    else if (!strcmp(cmd, "quick")) timeline_quick(); // the live timeline with the last 30 s picked
    else if (!strcmp(cmd, "options")) ui_show(SCREEN_OPTIONS);
    else if (!strcmp(cmd, "edit") && *arg) { // edit <path>: a saved file in the editor
        ui_show(SCREEN_EDITOR);
        editor_open_file(arg);
    }
    else if (!strcmp(cmd, "open-in") && g_app.ui.screen == SCREEN_EDITOR) // open-in premiere|davinci: the editor's XML in it
        ed_open_in(!strcmp(arg, "premiere"));
    else if (!strcmp(cmd, "save")) {
        i64 from_us, to_us;
        i32 range = parse_range_us(arg, &from_us, &to_us);
        if (range > 0) save_exact(from_us, to_us);
        else if (range < 0) {
            log_line("save: \"%s\" isn't a range (try save <from> <to> in Unix seconds)", arg);
            pipe_saved(SAVE_CLIP, false, "that isn't a range (try save <from> <to> in Unix seconds)");
        } else {
            i64 secs = parse_length_secs(*arg ? arg : "30s");
            if (secs <= 0) log_line("save: \"%s\" isn't a length (try 30s, 2m, 1h)", arg);
            else save_last(secs);
        }
    } else if (!strcmp(cmd, "session")) { // session <from> <to> in Unix seconds: saved as a session
        i64 from_us, to_us;
        if (parse_range_us(arg, &from_us, &to_us) > 0) save_range(from_us, to_us, SAVE_SESSION, "");
        else log_line("session: \"%s\" isn't a range (try session <from> <to> in Unix seconds)", arg);
    } else if (!strcmp(cmd, "pause") || !strcmp(cmd, "resume") || !strcmp(cmd, "toggle")) {
        bool pause = !strcmp(cmd, "pause") ? true : !strcmp(cmd, "resume") ? false : !g_app.recorder.paused;
        recorder_set_paused(pause);
        audio_set_paused(pause);
        tray_update(g_app.msg_hwnd, false);
        ui_wake();
    } else if (!strcmp(cmd, "mark")) { // mark [<colour>] [<note>] | mark ask: a marker at this moment
        marker_command(arg);
    } else if (!strcmp(cmd, "folder")) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        Path dir = {};
        swprintf(dir.s, MAX_PATH, L"%s\\%d\\Clips", g_app.config.save_dir.s, t.wYear);
        if (GetFileAttributesW(dir.s) == INVALID_FILE_ATTRIBUTES) dir = g_app.config.save_dir;
        ShellExecuteW(0, L"open", dir.s, 0, 0, SW_SHOWNORMAL);
    } else if (!strcmp(cmd, "dev")) { // the detailed log on (`dev`) or off (`dev off`), kept in the settings
        bool on = strcmp(arg, "off") != 0;
        g_app.config.dev_log = on;
        g_dev = on || dev_env();
        config_save(g_app.config, g_app.paths);
        log_line("detailed log %s", g_dev ? "on" : "off");
        ui_wake();
    } else if (!strcmp(cmd, "show")) { // show <page>: a page or tab by name, for screenshots and Stream Deck keys
        Library &lib = g_app.library;
        g_app.ui.help_close = true;
        if (g_app.ui.setup_open && !starts_with(arg, "setup")) setup_finish("left for another page"); // it covers every page
        if (!strcmp(arg, "clips") || !strcmp(arg, "sessions")) {
            library_close_player();
            ui_show(SCREEN_LIBRARY);
            lib_set_view(!strcmp(arg, "clips") ? LIB_CLIPS : LIB_SESSIONS);
        } else if (!strcmp(arg, "player")) { // the newest clip, playing in the library
            ui_show(SCREEN_LIBRARY);
            if (lib.shown_count > 0) library_play(lib.items[lib.shown[0]]);
        } else if (!strcmp(arg, "timeline")) ui_show(SCREEN_TIMELINE);
        else if (!strcmp(arg, "edit") || !strcmp(arg, "share")) g_app.editor.tab = !strcmp(arg, "share");
        else if (!strcmp(arg, "list") || !strcmp(arg, "list-collapsed")) // the library's list on the left, or its rail
            g_app.config.games_collapsed = !strcmp(arg, "list-collapsed");
        else if (!strcmp(arg, "delete")) g_show_dialog = 1; // the first card's Delete dialog
        else if (!strcmp(arg, "sound")) g_lib_sound_show = true; // the player's sound menu
        else if (!strcmp(arg, "comment") && lib.shown_count > 0) lib_comment_open(lib.items[lib.shown[0]]); // its Comment
        else if (starts_with(arg, "select")) { // select <n>: the first n cards selected, with the selection bar
            ui_show(SCREEN_LIBRARY);
            lib_sel_clear();
            for (i32 i = 0; i < atoi(trim(arg + 6)) && i < lib.shown_count; i++) lib_sel_set(lib.items[lib.shown[i]].key, true);
            if (lib.selected_count) lib.select_anchor = lib.items[lib.shown[0]].key;
        }
        else if (starts_with(arg, "session-toast")) { // session-toast [<more>]: Session ended for a made-up session
            Session s = {};
            str_set(s.name, "Minecraft");
            s.end_us = now_us();
            s.start_us = s.end_us - (i64)(83 * 60e6);
            toast_session(s, atoi(trim(arg + 13)));
        }
        else if (starts_with(arg, "delete-folder")) { // on the list of folders, which it belongs to
            g_app.config.side_folders = true;
            g_app.config.games_collapsed = false;
            g_show_dialog = strstr(arg, "ticked") ? 3 : 2;
        }
        else if (starts_with(arg, "game")) { // game [<text>]: the editor's game picker, with <text> typed
            g_app.editor.tab = 0;
            str_set(g_game_pick_typed, trim(arg + 4));
            g_game_pick_show = true;
        }
        else if (!strcmp(arg, "help")) { g_app.ui.help_open = true; g_app.ui.help_close = false; }
        else if (starts_with(arg, "setup")) { // setup, setup2 .. setup5: the first-run setup at that step
            ui_show(SCREEN_LIBRARY);
            g_app.ui.setup_open = true;
            g_app.ui.setup_step = arg[5] >= '1' && arg[5] <= '9' ? arg[5] - '1' : 0;
        }
        else {
            static const char *const tabs[] = {"apps", "recording", "hotkeys", "integrations", "general"};
            i32 t = -1;
            for (i32 i = 0; i < COUNT(tabs); i++)
                if (!strcmp(arg, tabs[i])) t = i;
            if (t >= 0) {
                ui_show(SCREEN_OPTIONS);
                g_app.options.tab = t;
            } else log_line("show: no page \"%s\" (clips, sessions, player, timeline, edit, share, game, list, list-collapsed, delete, delete-folder, session-toast, help, apps, recording, hotkeys, general, setup)", arg);
        }
        ui_wake();
    } else if (!strcmp(cmd, "quit")) {
        g_app.quitting = true;
        PostQuitMessage(0);
    } else {
        log_line("unknown command \"%s\"", text);
    }
    pipe_publish();
}

static void handle_event(const Event &e)
{
    static const char *const kinds[] = {"none", "command", "log", "recorder", "audio", "save", "library", "timeline", "gamedb", "r2"};
    if (e.kind != EV_LOG && e.kind != EV_COMMAND)
        DEV_LOG("event %s %lld %lld", (u32)e.kind < (u32)COUNT(kinds) ? kinds[e.kind] : "?", e.a, e.b);
    switch (e.kind) {
    case EV_COMMAND: run_command(e.text); break;
    case EV_LOG: log_line("%s", e.text); break;
    case EV_RECORDER: recorder_on_event(e); break;
    case EV_AUDIO: audio_on_event(e); break;
    case EV_SAVE: save_on_event(e); editor_on_save_event(e); r2_on_save_event(e); break;
    case EV_LIBRARY: library_on_event(e); break;
    case EV_TIMELINE: timeline_on_event(e); break;
    case EV_GAMEDB: gamedb_on_event(e); break;
    case EV_R2: r2_on_event(e); break;
    default: break;
    }
    pipe_publish();
    ui_wake();
}

static LRESULT msg_wndproc_body(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_TIMER:
        if (wp == TIMER_TICK) {
            sessions_tick();
            timeline_tick();
            pipe_publish();
        }
        return 0;
    case WM_APP_EVENT: {
        Event e;
        while (event_take(&e)) handle_event(e);
        return 0;
    }
    case WM_COPYDATA: {
        COPYDATASTRUCT *cd = (COPYDATASTRUCT *)lp;
        if (cd->dwData != COPYDATA_COMMAND || cd->cbData == 0 || cd->cbData > 4096) return FALSE;
        char text[1024] = {};
        memcpy(text, cd->lpData, cd->cbData < sizeof text - 1 ? cd->cbData : sizeof text - 1);
        run_command(text);
        return TRUE;
    }
    case WM_HOTKEY: {
        i32 i = (i32)wp - 1;
        DEV_LOG("hotkey %d", i);
        if (i >= 0 && i < g_app.config.hotkey_count) {
            const char *action = g_app.config.hotkeys[i].action.s;
            run_command(!strcmp(action, "timeline") ? "quick" : action); // from a game: quick save
        }
        return 0;
    }
    case WM_APP_TRAY:
        switch (LOWORD(lp)) {
        case NIN_SELECT:
        case NIN_KEYSELECT: run_command("open"); break;
        case WM_CONTEXTMENU: {
            char command[64];
            tray_menu(hwnd, command, sizeof command);
            if (command[0]) run_command(command);
            break;
        }
        }
        return 0;
    }
    if (msg == g_taskbar_created && msg != 0) {
        tray_update(hwnd, true);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------------------------------------------
// Crashes: a log line, a minidump, the stack, and then clipback starts itself again. (Windows' own restart,
// RegisterApplicationRestart, only acts on crashes that reach Windows Error Reporting; this handler ends the
// process first.) The new copy is told the old one's pid in CLIPBACK_AFTER_CRASH and waits for it to be gone. A
// copy that was itself a restart and crashes within a minute is not restarted, so a crash at startup can't loop.

static u64  g_started_ms;
static bool g_after_crash; // this copy was started by a crashing one

static void restart_after_crash()
{
    static volatile LONG once;
    if (InterlockedExchange(&once, 1) || g_app.quitting) return;
    if (g_after_crash && GetTickCount64() - g_started_ms < 60000) {
        log_line("not restarting: this copy was started after a crash and ran for under a minute");
        return;
    }
    wchar_t exe[MAX_PATH], cmd[MAX_PATH + 16], pid[16];
    GetModuleFileNameW(0, exe, MAX_PATH);
    swprintf(pid, COUNT(pid), L"%lu", GetCurrentProcessId());
    SetEnvironmentVariableW(L"CLIPBACK_AFTER_CRASH", pid);
    swprintf(cmd, COUNT(cmd), L"\"%s\"%s", exe, g_app.ui.shown ? L" open" : L""); // the window comes back if it was open
    STARTUPINFOW si = {sizeof si};
    PROCESS_INFORMATION pi = {};
    if (CreateProcessW(exe, cmd, 0, 0, FALSE, 0, 0, 0, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        log_line("restarting clipback (pid %lu)", pi.dwProcessId);
    } else {
        log_line("couldn't restart clipback (%lu)", GetLastError());
    }
}

static LONG WINAPI on_crash(EXCEPTION_POINTERS *info)
{
    u32 code = (u32)info->ExceptionRecord->ExceptionCode;
    log_line("crashed (0x%08x at %p) on thread %lu", code, info->ExceptionRecord->ExceptionAddress, GetCurrentThreadId());
    Path dump = diag_dump(info, L"crash");
    log_line("dump: %ls", dump.s);
    if (code != EXCEPTION_STACK_OVERFLOW) { // no stack left to walk with
        log_line("it was at:");
        diag_log_stack(GetCurrentThread(), *info->ContextRecord, false);
    }
    restart_after_crash();
    return EXCEPTION_EXECUTE_HANDLER;
}

// ---------------------------------------------------------------------------------------------------------------

// The command line after the exe name, as UTF-8 ("save 30s").
static void command_from_args(char *out, i32 out_size)
{
    out[0] = 0;
    i32 argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (i32 i = 1; i < argc; i++) {
        char part[512];
        wide_to_utf8(argv[i], part, sizeof part);
        if (i > 1) strncat_s(out, out_size, " ", _TRUNCATE);
        strncat_s(out, out_size, part, _TRUNCATE);
    }
    LocalFree(argv);
}

// Sends the command to a running copy. Returns false if there isn't one.
static bool send_to_running(const char *command)
{
    HWND other = FindWindowExW(HWND_MESSAGE, 0, MSG_CLASS, 0);
    if (!other) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(other, &pid);
    AllowSetForegroundWindow(pid); // so "open" can bring its window to the front
    const char *text = *command ? command : "open";
    COPYDATASTRUCT cd = {COPYDATA_COMMAND, (DWORD)strlen(text), (void *)text};
    DWORD_PTR result = 0;
    return SendMessageTimeoutW(other, WM_COPYDATA, 0, (LPARAM)&cd, SMTO_ABORTIFHUNG, 5000, &result) != 0;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(0, COINIT_APARTMENTTHREADED);
    InitializeSRWLock(&g_log_lock);
    InitializeSRWLock(&g_events.lock);

    char command[1024];
    command_from_args(command, sizeof command);

    // Started by a copy that crashed: wait until it has gone, so its mutex, hotkeys and tray icon are free.
    g_started_ms = GetTickCount64();
    wchar_t after[16] = {};
    if (GetEnvironmentVariableW(L"CLIPBACK_AFTER_CRASH", after, COUNT(after)) && after[0]) {
        g_after_crash = true;
        SetEnvironmentVariableW(L"CLIPBACK_AFTER_CRASH", 0);
        HANDLE old = OpenProcess(SYNCHRONIZE, FALSE, wcstoul(after, 0, 10));
        if (old) {
            WaitForSingleObject(old, 10000);
            CloseHandle(old);
        }
    }

    // One clipback per user (per CLIPBACK_HOME in development, so a test copy can run next to the real one)
    wchar_t mutex_name[MAX_PATH + 32] = L"Local\\clipback.native";
    if (u32 h = test_home_hash()) {
        swprintf(mutex_name, COUNT(mutex_name), L"Local\\clipback.native.%08x", h);
        // Per home too, so a command (quit) from one test copy never reaches another test copy.
        static wchar_t dev_class[64];
        swprintf(dev_class, COUNT(dev_class), L"clipback.msg.dev.%08x", h);
        MSG_CLASS = dev_class;
    }
    HANDLE single = CreateMutexW(0, TRUE, mutex_name);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        for (i32 tries = 0; tries < 20; tries++) { // the running copy may still be starting
            if (send_to_running(command)) return 0;
            Sleep(250);
        }
        return 1;
    }

    paths_init(&g_app.paths);
    proc_init();
    diag_init();
    SetUnhandledExceptionFilter(on_crash);
    // The C runtime's own fatal errors (abort, a bad argument to a CRT function, a pure virtual call) would end
    // the process with no log line and no dump: turn them into a crash the handler sees.
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    signal(SIGABRT, [](int) { log_line("abort() called"); __debugbreak(); });
    _set_invalid_parameter_handler([](const wchar_t *, const wchar_t *, const wchar_t *, unsigned, uintptr_t) {
        log_line("a C runtime function was given a bad argument");
        __debugbreak();
    });
    _set_purecall_handler([] { log_line("pure virtual call"); __debugbreak(); });
    if (!config_load(&g_app.config, g_app.paths)) {
        config_first_run(&g_app.config, g_app.paths);
        g_app.config.setup_done = false;
        config_save(g_app.config, g_app.paths);
    }
    g_app.ui.setup_open = !g_app.config.setup_done;
    g_dev = g_app.config.dev_log || dev_env();
    {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(0, exe, MAX_PATH);
        log_line("clipback (native) starting: %ls, built %s %s, pid %lu%s%s", exe, __DATE__, __TIME__,
                 GetCurrentProcessId(), g_dev ? ", detailed log on" : "", g_after_crash ? ", restarted after a crash" : "");
    }
    diag_watchdog_start();

    WNDCLASSEXW wc = {sizeof wc};
    wc.lpfnWndProc = diag_wndproc<msg_wndproc_body>;
    wc.hInstance = inst;
    wc.lpszClassName = MSG_CLASS;
    RegisterClassExW(&wc);
    g_app.msg_hwnd = CreateWindowExW(0, MSG_CLASS, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, 0, inst, 0);
    CHECK(g_app.msg_hwnd, "the message window couldn't be created (%d)", (int)GetLastError());

    if (!ui_init()) log_line("the window couldn't be created; clipback keeps recording without it");
    tray_init(g_app.msg_hwnd);
    SetTimer(g_app.msg_hwnd, TIMER_TICK, 1000, 0);
    // Test copies (CLIPBACK_HOME) leave the hotkeys to the real clipback.
    if (!test_home_hash()) hotkeys_apply(g_app.config, g_app.msg_hwnd);
    recorder_start();
    audio_start();
    // On a first start the setup asks how games are looked up first; it starts the look-up when it closes.
    if (!g_app.ui.setup_open) gamedb_start(g_app.config.game_lookup);
    pipe_start();
    if (g_app.ui.setup_open) ui_show(SCREEN_LIBRARY);
    if (command[0]) run_command(command);

    // The main loop. While the window is hidden or idle it sleeps in GetMessage; it only draws while shown, for a
    // few frames after input or a change, or continuously while something animates (Present waits for vsync).
    MSG msg;
    while (!g_app.quitting) {
        Ui &ui = g_app.ui;
        bool draw = ui.shown && (ui.frames_left > 0 || ui.animating);
        diag_beat();
        if (!draw) {
            g_diag.waiting = 1; // idle, not stuck
            BOOL got = GetMessageW(&msg, 0, 0, 0);
            g_diag.waiting = 0;
            diag_beat();
            if (got <= 0) break;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        while (PeekMessageW(&msg, 0, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) g_app.quitting = true;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (g_app.quitting) break;
        if (ui.shown && (ui.frames_left > 0 || ui.animating)) {
            u64 t0 = GetTickCount64();
            ui_frame();
            u64 took = GetTickCount64() - t0;
            if (took > 200) {
                char parts[600];
                frame_parts_text(parts, sizeof parts);
                log_line("slow frame: %llu ms on screen %d; since the last frame: %s", took, (int)ui.screen, parts);
            } else if (took > 100) DEV_LOG("slow frame: %llu ms", took);
            frame_parts_reset();
            if (ui.frames_left > 0) ui.frames_left--;
        }
    }

    log_line("clipback (native) quitting");
    pipe_stop();
    sessions_stop();
    gamedb_stop();
    r2_stop();
    audio_stop();
    recorder_stop();
    save_stop();
    tray_remove(g_app.msg_hwnd);
    ReleaseMutex(single);
    return 0;
}
