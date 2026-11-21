// Installing and uninstalling, done by clipback.exe itself. It installs per user, with no admin rights:
//   clipback-setup-<version>.exe (or clipback.exe install)
//       asks once, quits a clipback that's running, copies this exe to %LOCALAPPDATA%\Programs\clipback, adds the
//       Start menu shortcut, the Run value (start with Windows) and the Apps entry, and starts the installed copy.
//   clipback.exe uninstall (the Apps entry's uninstall command)
//       asks whether to delete the player's clips, sessions and settings too (they stay unless asked), quits
//       clipback, and removes everything the install added. Deleted clips go to the Recycle Bin.
//
// CLIPBACK_INSTALL_DIR (for tests) puts the program folder and the Start menu under that folder, and writes the
// registry values into <it>\registry.ini instead of the registry, so nothing outside it changes. It needs
// CLIPBACK_HOME too, so that the data it would delete and the clipback it quits are a test copy's.

static const wchar_t INSTALL_RUN_KEY[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t INSTALL_APPS_KEY[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\clipback";

struct InstallPlace {
    bool test;     // CLIPBACK_INSTALL_DIR is set
    Path root;     // the test folder
    Path dir;      // the program folder
    Path exe;      // dir\clipback.exe
    Path old_exe;  // dir\clipback.old.exe, the copy moved aside while it may still be running
    Path shortcut; // the Start menu's clipback.lnk
};

static InstallPlace install_place()
{
    InstallPlace p = {};
    p.test = GetEnvironmentVariableW(L"CLIPBACK_INSTALL_DIR", p.root.s, MAX_PATH) && p.root.s[0];
    wchar_t *programs = 0, *menu = 0;
    if (p.test) {
        swprintf(p.dir.s, MAX_PATH, L"%s\\Programs\\clipback", p.root.s);
        swprintf(p.shortcut.s, MAX_PATH, L"%s\\Start Menu\\Programs\\clipback.lnk", p.root.s);
    } else {
        SHGetKnownFolderPath(FOLDERID_UserProgramFiles, KF_FLAG_CREATE, 0, &programs);
        SHGetKnownFolderPath(FOLDERID_Programs, 0, 0, &menu);
        swprintf(p.dir.s, MAX_PATH, L"%s\\clipback", programs ? programs : L".");
        swprintf(p.shortcut.s, MAX_PATH, L"%s\\clipback.lnk", menu ? menu : L".");
        CoTaskMemFree(programs);
        CoTaskMemFree(menu);
    }
    p.exe = path_join(p.dir, L"clipback.exe");
    p.old_exe = path_join(p.dir, L"clipback.old.exe");
    return p;
}

// A message box. A test install with CLIPBACK_INSTALL_ANSWER set (yes, no or cancel) shows none and gives that
// answer, so tests open no windows.
static i32 install_ask(const InstallPlace &p, const wchar_t *text, const wchar_t *title, UINT flags)
{
    wchar_t answer[16] = {};
    if (p.test && GetEnvironmentVariableW(L"CLIPBACK_INSTALL_ANSWER", answer, COUNT(answer)) && answer[0]) {
        char t[1024];
        wide_to_utf8(text, t, sizeof t);
        for (char *c = t; *c; c++)
            if (*c == '\n') *c = ' ';
        log_line("install: asked \"%s\", answered %ls", t, answer);
        return !_wcsicmp(answer, L"yes") ? IDYES : !_wcsicmp(answer, L"no") ? IDNO : !_wcsicmp(answer, L"cancel") ? IDCANCEL : IDOK;
    }
    return MessageBoxW(0, text, title, flags);
}

// A string value under HKCU\<key>, or in registry.ini's [<key>] section for a test install. 0 deletes it.
static bool install_reg_set(const InstallPlace &p, const wchar_t *key, const wchar_t *name, const wchar_t *value)
{
    if (p.test) {
        Path ini = path_join(p.root, L"registry.ini");
        return WritePrivateProfileStringW(key, name, value, ini.s) != 0;
    }
    if (!value) {
        LSTATUS r = RegDeleteKeyValueW(HKEY_CURRENT_USER, key, name);
        return r == ERROR_SUCCESS || r == ERROR_FILE_NOT_FOUND;
    }
    HKEY h = 0;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, key, 0, 0, 0, KEY_SET_VALUE, 0, &h, 0) != ERROR_SUCCESS) return false;
    LSTATUS r = RegSetValueExW(h, name, 0, REG_SZ, (const BYTE *)value, (DWORD)((wcslen(value) + 1) * sizeof(wchar_t)));
    RegCloseKey(h);
    return r == ERROR_SUCCESS;
}

static bool install_reg_dword(const InstallPlace &p, const wchar_t *key, const wchar_t *name, DWORD value)
{
    if (p.test) {
        wchar_t text[16];
        swprintf(text, COUNT(text), L"%lu", value);
        return install_reg_set(p, key, name, text);
    }
    return RegSetKeyValueW(HKEY_CURRENT_USER, key, name, REG_DWORD, &value, sizeof value) == ERROR_SUCCESS;
}

// The whole Apps entry, or (test) its section of registry.ini
static bool install_reg_drop_apps(const InstallPlace &p)
{
    if (p.test) return WritePrivateProfileStringW(INSTALL_APPS_KEY, 0, 0, path_join(p.root, L"registry.ini").s) != 0;
    LSTATUS r = RegDeleteTreeW(HKEY_CURRENT_USER, INSTALL_APPS_KEY);
    return r == ERROR_SUCCESS || r == ERROR_FILE_NOT_FOUND;
}

static bool install_shortcut(const InstallPlace &p)
{
    Path menu_dir = p.shortcut;
    if (wchar_t *slash = wcsrchr(menu_dir.s, L'\\')) *slash = 0;
    SHCreateDirectoryExW(0, menu_dir.s, 0);
    IShellLinkW *link = 0;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, 0, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) return false;
    link->SetPath(p.exe.s);
    link->SetWorkingDirectory(p.dir.s);
    link->SetIconLocation(p.exe.s, 0);
    link->SetDescription(L"Save clips from the last minutes of play");
    IPersistFile *file = 0;
    bool ok = SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&file))) && SUCCEEDED(file->Save(p.shortcut.s, TRUE));
    if (file) file->Release();
    link->Release();
    return ok;
}

// Asks a running clipback (the one whose message window has this class) to quit, and waits up to 10 s for it to
// go. False if it's still running.
static bool install_quit_running(const wchar_t *msg_class, ULONG_PTR copydata_command)
{
    HWND other = FindWindowExW(HWND_MESSAGE, 0, msg_class, 0);
    if (!other) return true;
    DWORD pid = 0;
    GetWindowThreadProcessId(other, &pid);
    HANDLE proc = OpenProcess(SYNCHRONIZE, FALSE, pid);
    static const char quit[] = "quit";
    COPYDATASTRUCT cd = {copydata_command, (DWORD)strlen(quit), (void *)quit};
    DWORD_PTR result = 0;
    SendMessageTimeoutW(other, WM_COPYDATA, 0, (LPARAM)&cd, SMTO_ABORTIFHUNG, 5000, &result);
    bool gone = !proc || WaitForSingleObject(proc, 10000) == WAIT_OBJECT_0;
    if (proc) CloseHandle(proc);
    log_line("install: asked the running clipback (pid %lu) to quit: %s", pid, gone ? "it has" : "it's still running");
    return gone;
}

// Puts `from` in place of `to`, which may be a running exe: Windows lets a running exe be renamed, so the old one
// moves aside to `aside` (deleted when it can be, see install_cleanup) and the new one is copied or moved in.
static bool install_swap(const wchar_t *from, const wchar_t *to, const wchar_t *aside, bool move)
{
    DeleteFileW(aside);
    bool had = GetFileAttributesW(to) != INVALID_FILE_ATTRIBUTES;
    if (had && !MoveFileExW(to, aside, MOVEFILE_REPLACE_EXISTING)) {
        log_line("install: couldn't move %ls aside (%lu)", to, GetLastError());
        return false;
    }
    bool ok = move ? MoveFileExW(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED) != 0
                   : CopyFileW(from, to, FALSE) != 0;
    if (!ok) {
        log_line("install: couldn't put %ls in place (%lu)", to, GetLastError());
        if (had) MoveFileExW(aside, to, MOVEFILE_REPLACE_EXISTING); // the old one back
        return false;
    }
    return true;
}

// Starts an exe with its arguments, not waiting for it.
static bool install_start(const wchar_t *exe, const wchar_t *args, DWORD *pid = 0)
{
    wchar_t cmd[MAX_PATH * 2];
    swprintf(cmd, COUNT(cmd), L"\"%s\"%s%s", exe, args && args[0] ? L" " : L"", args ? args : L"");
    Path dir = {};
    wcscpy_s(dir.s, exe);
    if (wchar_t *slash = wcsrchr(dir.s, L'\\')) *slash = 0;
    STARTUPINFOW si = {sizeof si};
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(exe, cmd, 0, 0, FALSE, 0, 0, dir.s, &si, &pi)) {
        log_line("install: couldn't start %ls (%lu)", exe, GetLastError());
        return false;
    }
    if (pid) *pid = pi.dwProcessId;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

// At start: the copy moved aside by an install or update, if it has gone by now.
static void install_cleanup()
{
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(0, exe, MAX_PATH);
    if (wchar_t *slash = wcsrchr(exe, L'\\')) wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - exe), L"clipback.old.exe");
    if (DeleteFileW(exe)) log_line("install: deleted %ls", exe);
}

static bool install_is_test_safe(const InstallPlace &p)
{
    if (!p.test || is_test_copy()) return true;
    MessageBoxW(0, L"CLIPBACK_INSTALL_DIR is set without CLIPBACK_HOME. A test install needs both, so that it "
                   L"leaves the real clipback and its files alone.", L"clipback", MB_ICONERROR);
    return false;
}

static i32 install_run(const wchar_t *msg_class, ULONG_PTR copydata_command)
{
    InstallPlace p = install_place();
    if (!install_is_test_safe(p)) return 1;
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(0, self, MAX_PATH);
    if (!_wcsicmp(self, p.exe.s)) {
        install_ask(p, L"This is the installed clipback already.", L"clipback", MB_ICONINFORMATION);
        return 0;
    }
    wchar_t ask[MAX_PATH + 256];
    swprintf(ask, COUNT(ask),
             L"Install clipback %hs for you?\n\nIt goes in %s, gets a Start menu entry and starts when you sign in "
             L"(Options can turn that off).",
             CLIPBACK_VERSION, p.dir.s);
    if (install_ask(p, ask, L"Install clipback", MB_YESNO | MB_ICONQUESTION) != IDYES) return 0;
    log_line("install: clipback %s from %ls to %ls%s", CLIPBACK_VERSION, self, p.dir.s, p.test ? " (test)" : "");
    if (!install_quit_running(msg_class, copydata_command)) {
        install_ask(p, L"clipback is still running. Quit it from its tray icon and run the installer again.",
                    L"Install clipback", MB_ICONWARNING);
        return 1;
    }
    SHCreateDirectoryExW(0, p.dir.s, 0);
    if (!install_swap(self, p.exe.s, p.old_exe.s, false)) {
        install_ask(p, L"clipback couldn't be copied to its folder. The log says why.", L"Install clipback", MB_ICONERROR);
        return 1;
    }
    bool ok = install_shortcut(p);
    wchar_t run[MAX_PATH + 8], uninstall[MAX_PATH + 16];
    swprintf(run, COUNT(run), L"\"%s\"", p.exe.s);
    swprintf(uninstall, COUNT(uninstall), L"\"%s\" uninstall", p.exe.s);
    ok &= install_reg_set(p, INSTALL_RUN_KEY, L"clipback", run);
    ok &= install_reg_set(p, INSTALL_APPS_KEY, L"DisplayName", L"clipback");
    ok &= install_reg_set(p, INSTALL_APPS_KEY, L"DisplayVersion", L"" CLIPBACK_VERSION);
    ok &= install_reg_set(p, INSTALL_APPS_KEY, L"Publisher", L"CalvBot");
    ok &= install_reg_set(p, INSTALL_APPS_KEY, L"DisplayIcon", p.exe.s);
    ok &= install_reg_set(p, INSTALL_APPS_KEY, L"InstallLocation", p.dir.s);
    ok &= install_reg_set(p, INSTALL_APPS_KEY, L"UninstallString", uninstall);
    ok &= install_reg_set(p, INSTALL_APPS_KEY, L"URLInfoAbout", L"https://github.com/CalvBot/clipback");
    ok &= install_reg_dword(p, INSTALL_APPS_KEY, L"NoModify", 1);
    ok &= install_reg_dword(p, INSTALL_APPS_KEY, L"NoRepair", 1);
    WIN32_FILE_ATTRIBUTE_DATA fa = {};
    if (GetFileAttributesExW(p.exe.s, GetFileExInfoStandard, &fa))
        ok &= install_reg_dword(p, INSTALL_APPS_KEY, L"EstimatedSize", (DWORD)(fa.nFileSizeLow / 1024 + 1));
    log_line("install: done%s", ok ? "" : ", but the shortcut or a registry value failed");
    if (p.test) log_line("install: a test install doesn't start %ls", p.exe.s); // it would open its window
    else install_start(p.exe.s, L"open");
    return 0;
}

// Moves a folder to the Recycle Bin, if it's there.
static void install_recycle(const wchar_t *dir)
{
    if (!dir[0] || GetFileAttributesW(dir) == INVALID_FILE_ATTRIBUTES) return;
    wchar_t from[MAX_PATH + 2] = {};
    wcscpy_s(from, MAX_PATH, dir); // double 0 at the end
    SHFILEOPSTRUCTW op = {};
    op.wFunc = FO_DELETE;
    op.pFrom = from;
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
    i32 r = SHFileOperationW(&op);
    log_line("install: %ls to the Recycle Bin%s", dir, r ? " failed" : "");
}

// The player's own files: clips and sessions (each year's Clips, Sessions and Data, not anything else they keep in
// that folder), the buffer, the settings and the local folder.
static void install_delete_data()
{
    static Paths paths;
    paths_init(&paths);
    static Config c;
    if (!config_load(&c, paths)) config_defaults(&c, paths);
    WIN32_FIND_DATAW fd;
    Path pattern = path_join(c.save_dir, L"*");
    HANDLE f = FindFirstFileW(pattern.s, &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            bool year = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && wcslen(fd.cFileName) == 4 &&
                        iswdigit(fd.cFileName[0]) && iswdigit(fd.cFileName[1]) && iswdigit(fd.cFileName[2]) &&
                        iswdigit(fd.cFileName[3]);
            if (!year) continue;
            Path y = path_join(c.save_dir, fd.cFileName);
            static const wchar_t *const parts[] = {L"Clips", L"Sessions", L"Data"};
            for (const wchar_t *part : parts) install_recycle(path_join(y, part).s);
            RemoveDirectoryW(y.s); // only if nothing else is in it
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    RemoveDirectoryW(c.save_dir.s);
    install_recycle(c.buffer_dir.s);
    install_recycle(paths.config_dir.s);
    log_line("install: deleting %ls last, it holds this log", paths.local_dir.s);
    install_recycle(paths.local_dir.s);
}

static i32 install_uninstall(const wchar_t *msg_class, ULONG_PTR copydata_command)
{
    InstallPlace p = install_place();
    if (!install_is_test_safe(p)) return 1;
    i32 answer = install_ask(p,
                             L"Uninstall clipback.\n\nAlso delete your clips, sessions, the replay buffer and your "
                             L"settings? They go to the Recycle Bin.\n\nYes: delete them too\nNo: keep them\n"
                             L"Cancel: don't uninstall",
                             L"Uninstall clipback", MB_YESNOCANCEL | MB_ICONQUESTION | MB_DEFBUTTON2);
    if (answer == IDCANCEL) return 0;
    log_line("install: uninstalling from %ls%s%s", p.dir.s, p.test ? " (test)" : "",
             answer == IDYES ? ", deleting the player's files" : "");
    if (!install_quit_running(msg_class, copydata_command)) {
        install_ask(p, L"clipback is still running. Quit it from its tray icon and uninstall again.",
                    L"Uninstall clipback", MB_ICONWARNING);
        return 1;
    }
    DeleteFileW(p.shortcut.s);
    install_reg_set(p, INSTALL_RUN_KEY, L"clipback", 0);
    install_reg_drop_apps(p);
    if (answer == IDYES) install_delete_data();
    // This exe may be the installed one, which can't delete itself while running: a hidden cmd does it once this
    // process has gone.
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(0, self, MAX_PATH);
    DeleteFileW(p.old_exe.s);
    if (_wcsicmp(self, p.exe.s)) {
        DeleteFileW(p.exe.s);
        RemoveDirectoryW(p.dir.s);
    } else {
        wchar_t cmd_exe[MAX_PATH], cmd[MAX_PATH * 3];
        GetEnvironmentVariableW(L"ComSpec", cmd_exe, MAX_PATH);
        swprintf(cmd, COUNT(cmd), L"\"%s\" /d /c ping -n 3 127.0.0.1 >nul & del /f /q \"%s\" & rmdir \"%s\"",
                 cmd_exe, p.exe.s, p.dir.s);
        STARTUPINFOW si = {sizeof si};
        PROCESS_INFORMATION pi = {};
        if (CreateProcessW(cmd_exe, cmd, 0, 0, FALSE, CREATE_NO_WINDOW, 0, 0, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
    }
    install_ask(p, answer == IDYES ? L"clipback and your files are gone (your files are in the Recycle Bin)."
                                   : L"clipback is uninstalled. Your clips, sessions and settings are still there.",
                L"Uninstall clipback", MB_ICONINFORMATION);
    return 0;
}

// The setup exe is clipback.exe named clipback-setup-<version>.exe
static bool install_is_setup_exe()
{
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(0, self, MAX_PATH);
    const wchar_t *name = wcsrchr(self, L'\\');
    name = name ? name + 1 : self;
    return !_wcsnicmp(name, L"clipback-setup", 14);
}

// For main: runs "install" and "uninstall" (and the setup exe with no command). True if it was one of them, and
// `*code` is then the exit code.
static bool install_command(const char *command, const wchar_t *msg_class, ULONG_PTR copydata_command, i32 *code)
{
    bool setup = !command[0] && install_is_setup_exe();
    if (setup || !strcmp(command, "install") || !strcmp(command, "uninstall")) {
        static Paths log_paths; // for the log
        paths_init(&log_paths);
    }
    if (setup || !strcmp(command, "install")) {
        *code = install_run(msg_class, copydata_command);
        return true;
    }
    if (!strcmp(command, "uninstall")) {
        *code = install_uninstall(msg_class, copydata_command);
        return true;
    }
    return false;
}
