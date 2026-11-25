// Backup: copies the save folder's sessions (and clips, if asked) to another drive or a NAS folder, on a thread.
// It mirrors <save_dir>\{Sessions\<year>,Data,Clips\<year>,Clips\<year>\Source} into the same folders under
// backup_dir. A file is copied when the backup has none or an older one; nothing at the backup is ever deleted.
// Files written in the last two minutes wait for the next run, so a save in progress isn't copied half done. It runs
// a minute after start, every 30 minutes, and on Back up now.

enum BackupState : LONG { BACKUP_IDLE, BACKUP_RUNNING, BACKUP_DONE, BACKUP_FAILED };

struct Backup {
    volatile LONG state;
    volatile LONG stop;
    volatile LONG wanted; // Back up now
    HANDLE thread;
    i64 next_us;          // the next run on the timer
    CRITICAL_SECTION lock; // guards the text below
    char current[128];    // the file being copied
    char error[200];      // the first failure of the last run
    i32 done, total, failed;
    u64 bytes;            // copied in the last run
    volatile LONG64 file_got, file_size;
    SYSTEMTIME finished;  // local time the last run ended
    // What the run works on, copied from the config when it starts
    Path from, to;
    bool clips;
};
static Backup g_backup;

static const i64 BACKUP_EVERY_US = 30ll * 60 * 1000000;
static const i64 BACKUP_SETTLE_100NS = 2ll * 60 * 10000000; // a file this fresh may still be being written

static void ui_wake(i32 frames); // ui.cpp

static DWORD CALLBACK backup_progress(LARGE_INTEGER total, LARGE_INTEGER got, LARGE_INTEGER, LARGE_INTEGER, DWORD,
                                      DWORD, HANDLE, HANDLE, LPVOID)
{
    InterlockedExchange64(&g_backup.file_size, total.QuadPart);
    InterlockedExchange64(&g_backup.file_got, got.QuadPart);
    return g_backup.stop ? PROGRESS_CANCEL : PROGRESS_CONTINUE;
}

static void backup_fail(const char *fmt, ...)
{
    EnterCriticalSection(&g_backup.lock);
    if (!g_backup.error[0]) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(g_backup.error, sizeof g_backup.error, fmt, ap);
        va_end(ap);
        log_line("backup: %s", g_backup.error);
    }
    LeaveCriticalSection(&g_backup.lock);
}

static bool backup_skip_name(const wchar_t *name)
{
    size_t n = wcslen(name);
    return name[0] == L'.' || (n > 5 && !_wcsicmp(name + n - 5, L".part")) || (n > 4 && !_wcsicmp(name + n - 4, L".tmp"));
}

static bool backup_is_year(const wchar_t *name)
{
    return wcslen(name) == 4 && iswdigit(name[0]) && iswdigit(name[1]) && iswdigit(name[2]) && iswdigit(name[3]);
}

// The files of one folder that the backup lacks or has older. Calls fn(src, dst, name, size) for each
// and stops when it returns false. Returns the count.
template <typename F> static i32 backup_walk_dir(const Path &src_dir, const Path &dst_dir, F fn)
{
    WIN32_FIND_DATAW fd;
    Path pattern = path_join(src_dir, L"*");
    HANDLE h = FindFirstFileW(pattern.s, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    FILETIME now_ft;
    GetSystemTimeAsFileTime(&now_ft);
    i64 now = (i64)(((u64)now_ft.dwHighDateTime << 32) | now_ft.dwLowDateTime);
    i32 n = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (backup_skip_name(fd.cFileName)) continue;
        i64 written = (i64)(((u64)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime);
        if (now - written < BACKUP_SETTLE_100NS) continue;
        Path src = path_join(src_dir, fd.cFileName), dst = path_join(dst_dir, fd.cFileName);
        WIN32_FILE_ATTRIBUTE_DATA have;
        if (GetFileAttributesExW(dst.s, GetFileExInfoStandard, &have) &&
            CompareFileTime(&have.ftLastWriteTime, &fd.ftLastWriteTime) >= 0)
            continue; // the backup's is as new
        n++;
        if (!fn(src, dst, fd.cFileName, ((u64)fd.nFileSizeHigh << 32) | fd.nFileSizeLow)) break;
    } while (!g_backup.stop && FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}

// Every folder to back up, in order: Sessions\<year>, Data, then Clips\<year> and Clips\<year>\Source.
template <typename F> static i32 backup_walk(F fn)
{
    i32 n = 0;
    for (i32 top = 0; top < 3 && !g_backup.stop; top++) {
        if (top == 1) {
            n += backup_walk_dir(save_data_dir(g_backup.from), save_data_dir(g_backup.to), fn);
            continue;
        }
        if (top == 2 && !g_backup.clips) continue;
        WIN32_FIND_DATAW fd;
        Path pattern = path_join(path_join(g_backup.from, top ? L"Clips" : L"Sessions"), L"*");
        HANDLE h = FindFirstFileW(pattern.s, &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !backup_is_year(fd.cFileName)) continue;
            i32 year = _wtoi(fd.cFileName);
            if (!top) {
                n += backup_walk_dir(save_sessions_dir(g_backup.from, year), save_sessions_dir(g_backup.to, year), fn);
                continue;
            }
            n += backup_walk_dir(save_clips_dir(g_backup.from, year), save_clips_dir(g_backup.to, year), fn);
            if (!g_backup.stop)
                n += backup_walk_dir(save_source_dir(g_backup.from, year), save_source_dir(g_backup.to, year), fn);
        } while (!g_backup.stop && FindNextFileW(h, &fd));
        FindClose(h);
    }
    return n;
}

static bool backup_copy(const Path &src, const Path &dst, const wchar_t *name, u64 size)
{
    char u[128];
    wide_to_utf8(name, u, sizeof u);
    EnterCriticalSection(&g_backup.lock);
    str_set(g_backup.current, u);
    LeaveCriticalSection(&g_backup.lock);
    InterlockedExchange64(&g_backup.file_got, 0);
    InterlockedExchange64(&g_backup.file_size, (LONG64)size);

    Path dir = dst;
    wchar_t *slash = wcsrchr(dir.s, L'\\');
    if (slash) *slash = 0;
    SHCreateDirectoryExW(0, dir.s, 0);
    Path part = dst;
    wcscat_s(part.s, L".part");
    BOOL cancel = FALSE;
    bool ok = CopyFileExW(src.s, part.s, backup_progress, 0, &cancel, 0) &&
              MoveFileExW(part.s, dst.s, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (!ok) {
        DWORD err = GetLastError();
        DeleteFileW(part.s); // only the half-copied file of this run
        if (g_backup.stop) return false;
        g_backup.failed++;
        backup_fail("%s couldn't be copied (%d)", u, (int)err);
        return true; // try the rest
    }
    g_backup.done++;
    g_backup.bytes += size;
    return true;
}

static DWORD WINAPI backup_thread(void *)
{
    char to[MAX_PATH * 3];
    wide_to_utf8(g_backup.to.s, to, sizeof to);
    log_line("backup: to %s%s", to, g_backup.clips ? ", clips too" : "");
    SHCreateDirectoryExW(0, g_backup.to.s, 0);
    DWORD attr = GetFileAttributesW(g_backup.to.s);
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        backup_fail("%s can't be reached (%d)", to, (int)GetLastError());
    } else {
        g_backup.total = backup_walk([](const Path &, const Path &, const wchar_t *, u64) { return true; });
        backup_walk(backup_copy);
    }
    EnterCriticalSection(&g_backup.lock);
    GetLocalTime(&g_backup.finished);
    g_backup.current[0] = 0;
    LeaveCriticalSection(&g_backup.lock);
    log_line("backup: %d of %d files copied (%llu bytes), %d failed%s", g_backup.done, g_backup.total,
             (unsigned long long)g_backup.bytes, g_backup.failed, g_backup.stop ? ", stopped" : "");
    InterlockedExchange(&g_backup.state, g_backup.error[0] ? BACKUP_FAILED : BACKUP_DONE);
    ui_wake(3);
    return 0;
}

static bool backup_same_folder(const Path &a, const Path &b)
{
    wchar_t fa[MAX_PATH], fb[MAX_PATH];
    if (!GetFullPathNameW(a.s, MAX_PATH, fa, 0) || !GetFullPathNameW(b.s, MAX_PATH, fb, 0)) return false;
    wchar_t *both[] = {fa, fb};
    for (wchar_t *p : both) {
        size_t n = wcslen(p);
        while (n > 3 && (p[n - 1] == L'\\' || p[n - 1] == L'/')) p[--n] = 0;
    }
    return !_wcsicmp(fa, fb);
}

static void backup_start()
{
    const Config &c = g_app.config;
    if (g_backup.state == BACKUP_RUNNING) return;
    if (g_backup.thread) {
        WaitForSingleObject(g_backup.thread, INFINITE);
        CloseHandle(g_backup.thread);
        g_backup.thread = 0;
    }
    g_backup.from = c.save_dir;
    g_backup.to = c.backup_dir;
    g_backup.clips = c.backup_clips;
    g_backup.error[0] = 0;
    g_backup.done = g_backup.total = g_backup.failed = 0;
    g_backup.bytes = 0;
    if (backup_same_folder(c.save_dir, c.backup_dir)) {
        backup_fail("the backup folder is the save folder");
        GetLocalTime(&g_backup.finished);
        InterlockedExchange(&g_backup.state, BACKUP_FAILED);
        return;
    }
    InterlockedExchange(&g_backup.state, BACKUP_RUNNING);
    g_backup.thread = thread_start(backup_thread, 0);
}

static void backup_init()
{
    InitializeCriticalSection(&g_backup.lock);
    g_backup.next_us = now_us() + 60ll * 1000000; // after start has settled
}

// Once a second on the main thread: starts a run when one is due.
static void backup_tick()
{
    const Config &c = g_app.config;
    if (g_backup.state == BACKUP_RUNNING) {
        ui_wake(2); // the status line moves
        return;
    }
    if (!c.backup_on || !c.backup_dir.s[0]) return;
    i64 now = now_us();
    if (!InterlockedExchange(&g_backup.wanted, 0) && now < g_backup.next_us) return;
    g_backup.next_us = now + BACKUP_EVERY_US;
    backup_start();
}

static void backup_now() { InterlockedExchange(&g_backup.wanted, 1); }

static void backup_stop()
{
    InterlockedExchange(&g_backup.stop, 1);
    if (g_backup.thread) {
        WaitForSingleObject(g_backup.thread, 5000);
        CloseHandle(g_backup.thread);
        g_backup.thread = 0;
    }
}

// For the pipe's state line (Stream Deck)
static const char *backup_state_name()
{
    const Config &c = g_app.config;
    LONG s = g_backup.state;
    if (s == BACKUP_RUNNING) return "running";
    if (!c.backup_on || !c.backup_dir.s[0]) return "off";
    if (s == BACKUP_FAILED) return "failed";
    if (s == BACKUP_DONE) return "ok";
    return "waiting";
}

static void backup_size(u64 bytes, char *out, i32 cap)
{
    if (bytes >= 1000000000ull) snprintf(out, cap, "%.1f GB", bytes / 1e9);
    else snprintf(out, cap, "%.0f MB", bytes / 1e6);
}

// One line for Options: what the backup is doing or last did.
static void backup_status(char *out, i32 cap)
{
    const Config &c = g_app.config;
    LONG s = g_backup.state;
    EnterCriticalSection(&g_backup.lock);
    char at[16];
    snprintf(at, sizeof at, "%02d:%02d", g_backup.finished.wHour, g_backup.finished.wMinute);
    if (s == BACKUP_RUNNING) {
        LONG64 got = g_backup.file_got, size = g_backup.file_size;
        if (g_backup.current[0])
            snprintf(out, cap, "Copying %s, %d of %d, %d%%", g_backup.current, g_backup.done + g_backup.failed + 1,
                     g_backup.total, size > 0 ? (int)(got * 100 / size) : 0);
        else snprintf(out, cap, "Looking for new files");
    } else if (s == BACKUP_FAILED) {
        snprintf(out, cap, "At %s: %s", at, g_backup.error);
    } else if (s == BACKUP_DONE) {
        char size[24];
        backup_size(g_backup.bytes, size, sizeof size);
        if (g_backup.done) snprintf(out, cap, "Up to date at %s, %d file%s copied (%s)", at, g_backup.done,
                                    g_backup.done == 1 ? "" : "s", size);
        else snprintf(out, cap, "Up to date at %s", at);
    } else if (!c.backup_dir.s[0]) {
        snprintf(out, cap, "Choose a folder on another drive or a NAS");
    } else if (!c.backup_on) {
        snprintf(out, cap, "Off");
    } else {
        snprintf(out, cap, "Starts a minute after " APP_NAME " does, then every 30 minutes");
    }
    LeaveCriticalSection(&g_backup.lock);
}

// ---------------------------------------------------------------------------------------------------------------
// Places found on this PC for the backup, offered as picks in Options: the Google Drive, OneDrive and Dropbox
// folders their desktop apps sync, from those apps' own registry keys and files (nothing is signed in to), and
// mapped network drives (a NAS). A pick backs up into an APP_NAME folder there.

#pragma comment(lib, "mpr.lib")

struct BackupPlace {
    char name[64];
    Path dir;
};

static bool backup_dir_exists(const wchar_t *p)
{
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static void backup_place_add(BackupPlace *out, i32 cap, i32 *n, const char *name, const wchar_t *dir)
{
    if (*n >= cap || !dir[0]) return;
    for (i32 i = 0; i < *n; i++)
        if (!_wcsicmp(out[i].dir.s, dir)) return;
    str_set(out[*n].name, name);
    swprintf(out[*n].dir.s, COUNT(out[*n].dir.s), L"%s", dir);
    size_t l = wcslen(out[*n].dir.s);
    if (l > 3 && out[*n].dir.s[l - 1] == '\\') out[*n].dir.s[l - 1] = 0;
    (*n)++;
}

// Google Drive's mount point as its setting holds it ("G", "G:" or a folder) as a folder: "G:\".
static void backup_mount_dir(const wchar_t *v, wchar_t *out, i32 cap)
{
    if (iswalpha(v[0]) && (!v[1] || (v[1] == ':' && (!v[2] || (v[2] == '\\' && !v[3]))))) swprintf(out, cap, L"%c:\\", towupper(v[0]));
    else swprintf(out, cap, L"%s", v);
}

// Dropbox's info.json: {"personal": {"path": ...}, "business": {"path": ...}}. Returns how many it holds.
static i32 backup_dropbox_paths(const char *json, i64 len, char (*out)[MAX_PATH * 3], const char **names)
{
    static const char *const keys[2] = {"personal.path", "business.path"};
    static const char *const shown[2] = {"Dropbox", "Dropbox (work)"};
    i32 n = 0;
    for (i32 i = 0; i < 2; i++)
        if (j_get(json, len, keys[i], out[n], MAX_PATH * 3)) names[n++] = shown[i];
    return n;
}

static i32 backup_places(BackupPlace *out, i32 cap)
{
    i32 n = 0;
    wchar_t v[MAX_PATH], dir[MAX_PATH];

    // Google Drive for desktop: its mount point setting, else a drive it has mounted (named "Google Drive")
    bool drive = false;
    static const HKEY roots[2] = {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE};
    for (i32 r = 0; r < 2 && !drive; r++) {
        DWORD bytes = sizeof v;
        if (RegGetValueW(roots[r], L"Software\\Google\\DriveFS", L"DefaultMountPoint", RRF_RT_REG_SZ, 0, v, &bytes) == ERROR_SUCCESS && v[0]) {
            backup_mount_dir(v, dir, MAX_PATH);
            drive = backup_dir_exists(dir);
        }
    }
    DWORD letters = GetLogicalDrives();
    DWORD old_mode = 0;
    SetThreadErrorMode(SEM_FAILCRITICALERRORS, &old_mode); // an empty card reader fails quietly, no "no disk" box
    for (i32 d = 0; d < 26 && !drive; d++) {
        if (!(letters & (1u << d))) continue;
        wchar_t root[4] = {(wchar_t)('A' + d), ':', '\\', 0};
        UINT type = GetDriveTypeW(root);
        if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE) continue;
        wchar_t label[64] = L"";
        if (GetVolumeInformationW(root, label, COUNT(label), 0, 0, 0, 0, 0) && !_wcsicmp(label, L"Google Drive")) {
            wcscpy_s(dir, root);
            drive = true;
        }
    }
    SetThreadErrorMode(old_mode, 0);
    if (drive) {
        wchar_t mine[MAX_PATH];
        swprintf(mine, MAX_PATH, L"%s%sMy Drive", dir, dir[wcslen(dir) - 1] == '\\' ? L"" : L"\\");
        backup_place_add(out, cap, &n, "Google Drive", backup_dir_exists(mine) ? mine : dir);
    }

    // OneDrive: each account's folder
    HKEY accounts;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\OneDrive\\Accounts", 0, KEY_READ, &accounts) == ERROR_SUCCESS) {
        wchar_t sub[128];
        for (DWORD i = 0; RegEnumKeyW(accounts, i, sub, COUNT(sub)) == ERROR_SUCCESS; i++) {
            DWORD bytes = sizeof v;
            if (RegGetValueW(accounts, sub, L"UserFolder", RRF_RT_REG_SZ, 0, v, &bytes) != ERROR_SUCCESS || !backup_dir_exists(v)) continue;
            backup_place_add(out, cap, &n, _wcsnicmp(sub, L"Business", 8) ? "OneDrive" : "OneDrive (work)", v);
        }
        RegCloseKey(accounts);
    }

    // Dropbox: info.json in either AppData folder
    static const wchar_t *const appdata[2] = {L"LOCALAPPDATA", L"APPDATA"};
    for (i32 a = 0; a < 2; a++) {
        wchar_t base[MAX_PATH], info[MAX_PATH];
        if (!GetEnvironmentVariableW(appdata[a], base, MAX_PATH)) continue;
        swprintf(info, MAX_PATH, L"%s\\Dropbox\\info.json", base);
        FILE *f = _wfopen(info, L"rb");
        if (!f) continue;
        char json[8192];
        size_t len = fread(json, 1, sizeof json - 1, f);
        fclose(f);
        json[len] = 0;
        static char paths[2][MAX_PATH * 3];
        const char *names[2];
        i32 k = backup_dropbox_paths(json, (i64)len, paths, names);
        for (i32 i = 0; i < k; i++) {
            utf8_to_wide(paths[i], v, MAX_PATH);
            if (backup_dir_exists(v)) backup_place_add(out, cap, &n, names[i], v);
        }
    }

    // Mapped network drives, named after their share; not touched (one that's offline would hang a moment)
    for (i32 d = 0; d < 26; d++) {
        if (!(letters & (1u << d))) continue;
        wchar_t root[4] = {(wchar_t)('A' + d), ':', '\\', 0}, local[3] = {(wchar_t)('A' + d), ':', 0};
        if (GetDriveTypeW(root) != DRIVE_REMOTE) continue;
        wchar_t unc[MAX_PATH] = L"";
        DWORD ul = MAX_PATH;
        char name[64];
        if (WNetGetConnectionW(local, unc, &ul) == NO_ERROR && unc[0]) {
            const wchar_t *share = wcsrchr(unc, '\\');
            snprintf(name, sizeof name, "%ls (%c:)", share && share[1] ? share + 1 : unc, 'A' + d);
        } else {
            snprintf(name, sizeof name, "%c:", 'A' + d);
        }
        backup_place_add(out, cap, &n, name, root);
    }
    return n;
}

// The folder a pick backs up into: <place>\APP_NAME.
static Path backup_place_target(const BackupPlace &p)
{
    Path t;
    size_t l = wcslen(p.dir.s);
    swprintf(t.s, COUNT(t.s), L"%s%s%hs", p.dir.s, l && p.dir.s[l - 1] == '\\' ? L"" : L"\\", APP_NAME);
    return t;
}
