// Backup: keeps a copy of the save folder's clips (and sessions, if asked) on another drive or a NAS folder, on a
// thread. It mirrors <save_dir>\{Data,Clips\<year>,Clips\<year>\Source,Sessions\<year>} into the same folders under
// backup_dir. Each run lists both sides and plans every file (backup_plan): Data is always kept, then the newest
// clips, then the newest sessions, up to the space cap; what's past the cap is deleted at the backup or not copied.
// A clip or Data file deleted from the save folder stays at the backup for 30 days from the run that first finds it
// gone; Deleted.txt at the backup's root holds those times, and its time is the last backup's. Sessions the save
// folder let go stay at the backup until the cap needs their room. Files written in the last two minutes wait for the
// next run. It runs a minute after start, then on a timer, and on Back up now. A measure-only run (Options) lists the
// backup without changing it.

enum BackupState : LONG { BACKUP_IDLE, BACKUP_RUNNING, BACKUP_DONE, BACKUP_FAILED };
enum BackupKind : u8 { BK_DATA, BK_CLIP, BK_SESSION, BK_KINDS };
enum BackupAct : u8 { BK_KEEP, BK_COPY, BK_DROP, BK_SKIP };

// One file, from either side or both
struct BackupFile {
    wchar_t rel[MAX_PATH]; // the same under the save folder and the backup folder
    u64 size;              // the save folder's, else the backup's
    u64 bk_size;           // the backup's
    i64 written;           // 100 ns since 1601: the save folder's, else the backup's
    i64 gone_since;        // when a run first found it missing from the save folder; 0 = not gone
    u8 kind, act;
    bool at_src, at_bk;
    bool newer;            // the save folder's is newer than the backup's, or the backup has none
    bool fresh;            // written in the last two minutes, so maybe still being written: not copied this run
};

struct Backup {
    volatile LONG state;
    volatile LONG stop;
    volatile LONG wanted;  // Back up now
    volatile LONG measure; // a measure-only run, for Options
    LONG before;           // the state before a measure-only run, which it puts back
    HANDLE thread;
    i64 next_us;           // the next run on the timer
    CRITICAL_SECTION lock; // guards what's below
    char current[128];     // the file being copied
    char error[200];       // the first failure of the last run
    i32 done, total, failed, dropped;
    u64 bytes;             // copied in the last run
    volatile LONG64 file_got, file_size;
    i64 ended_us;          // when the last run ended
    // What the backup holds by kind, and its room: the drive's free space plus what the backup holds
    bool measured;
    u64 used[BK_KINDS], room;
    i64 last_us;           // the last finished backup (Deleted.txt's time); 0 = none
    // What the run works on, copied from the config when it starts
    Path from, to;
    bool sessions, measure_only;
    f32 cap_gb;
};
static Backup g_backup;

static const i64 BACKUP_EVERY_US = 30ll * 60 * 1000000;
static const i64 BACKUP_SETTLE_100NS = 2ll * 60 * 10000000; // a file this fresh may still be being written
static const i64 BACKUP_KEEP_GONE_100NS = 30ll * 24 * 3600 * 10000000; // a deleted clip stays this long
static const u64 BACKUP_GB = 1000000000ull;
static const wchar_t BACKUP_LEDGER[] = L"Deleted.txt";

static void ui_wake(i32 frames); // ui.cpp

static i64 backup_ft(FILETIME ft) { return (i64)(((u64)ft.dwHighDateTime << 32) | ft.dwLowDateTime); }

static i64 backup_now_100ns()
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return backup_ft(ft);
}

// The cap when none is set: half the room, in whole GB, at least 1 GB
static u64 backup_default_cap(u64 room)
{
    u64 gb = room / 2 / BACKUP_GB;
    return (gb ? gb : 1) * BACKUP_GB;
}

static u64 backup_cap(f32 cap_gb, u64 room) { return cap_gb >= 0 ? (u64)((f64)cap_gb * BACKUP_GB) : backup_default_cap(room); }

static int backup_rank_cmp(const void *pa, const void *pb)
{
    const BackupFile &a = *(const BackupFile *)pa, &b = *(const BackupFile *)pb;
    if (a.kind != b.kind) return a.kind < b.kind ? -1 : 1;
    return a.written > b.written ? -1 : a.written < b.written;
}

// Decides each file's act, and sorts the files in the order they're kept: Data, then clips newest first, then
// sessions newest first. Data is always kept; the rest is kept until one doesn't fit under the cap, and from there on
// goes: deleted at the backup (BK_DROP) or not copied (BK_SKIP). Clip and Data files gone from the save folder for
// 30 days go first of all. A file only at the save folder isn't copied while fresh, nor a session unless sessions are
// wanted. Returns the bytes the backup holds after the run.
static u64 backup_plan(BackupFile *f, i32 n, u64 cap, i64 now, bool sessions)
{
    qsort(f, n, sizeof *f, backup_rank_cmp);
    u64 total = 0;
    bool full = false;
    for (i32 i = 0; i < n; i++) {
        BackupFile &e = f[i];
        if (!e.at_src && e.kind != BK_SESSION && e.gone_since && now - e.gone_since >= BACKUP_KEEP_GONE_100NS) {
            e.act = BK_DROP;
            continue;
        }
        bool wanted = e.kind != BK_SESSION || sessions;
        if (!e.at_bk && (e.fresh || !wanted)) {
            e.act = BK_SKIP;
            continue;
        }
        if (e.kind != BK_DATA && (full || total + e.size > cap)) {
            full = true;
            e.act = e.at_bk ? BK_DROP : BK_SKIP;
            continue;
        }
        bool copy = e.at_src && e.newer && !e.fresh && wanted;
        total += copy || !e.at_bk ? e.size : e.bk_size;
        e.act = copy ? BK_COPY : BK_KEEP;
    }
    return total;
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

struct BackupList {
    BackupFile *f;
    i32 n, cap;
};

static BackupFile *backup_add(BackupList *l)
{
    if (l->n == l->cap) {
        i32 cap = l->cap ? l->cap * 2 : 256;
        BackupFile *f = (BackupFile *)realloc(l->f, sizeof *f * (size_t)cap);
        if (!f) return 0;
        l->f = f;
        l->cap = cap;
    }
    BackupFile *e = &l->f[l->n++];
    memset(e, 0, sizeof *e);
    return e;
}

// The files of one mirrored folder on both sides (the save folder's only when `src`)
static void backup_list_dir(BackupList *l, const wchar_t *rel_dir, u8 kind, bool src, i64 now)
{
    i32 first = l->n;
    size_t rl = wcslen(rel_dir) + 1;
    for (i32 side = src ? 0 : 1; side < 2 && !g_backup.stop; side++) {
        WIN32_FIND_DATAW fd;
        Path pattern = path_join(path_join(side ? g_backup.to : g_backup.from, rel_dir), L"*");
        HANDLE h = FindFirstFileW(pattern.s, &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || backup_skip_name(fd.cFileName)) continue;
            i64 written = backup_ft(fd.ftLastWriteTime);
            u64 size = ((u64)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            BackupFile *e = 0;
            if (side)
                for (i32 i = first; i < l->n && !e; i++)
                    if (!_wcsicmp(l->f[i].rel + rl, fd.cFileName)) e = &l->f[i];
            if (!e) {
                if (!(e = backup_add(l))) break;
                swprintf(e->rel, MAX_PATH, L"%s\\%s", rel_dir, fd.cFileName);
                e->kind = kind;
                e->size = size;
                e->written = written;
            }
            if (!side) {
                e->at_src = true;
                e->fresh = now - written < BACKUP_SETTLE_100NS;
                e->newer = true;
            } else {
                e->at_bk = true;
                e->bk_size = size;
                if (e->at_src) e->newer = e->written > written;
            }
        } while (!g_backup.stop && FindNextFileW(h, &fd));
        FindClose(h);
    }
}

// The year folders under <side>\<top> on either side, ascending
static i32 backup_years(const wchar_t *top, i32 *out, i32 cap)
{
    i32 n = 0;
    for (i32 side = 0; side < 2; side++) {
        WIN32_FIND_DATAW fd;
        Path pattern = path_join(path_join(side ? g_backup.to : g_backup.from, top), L"*");
        HANDLE h = FindFirstFileW(pattern.s, &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !backup_is_year(fd.cFileName)) continue;
            i32 y = _wtoi(fd.cFileName), k = 0;
            while (k < n && out[k] != y) k++;
            if (k == n && n < cap) out[n++] = y;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    for (i32 i = 1; i < n; i++)
        for (i32 j = i; j > 0 && out[j - 1] > out[j]; j--) {
            i32 t = out[j];
            out[j] = out[j - 1];
            out[j - 1] = t;
        }
    return n;
}

static void backup_list(BackupList *l, i64 now)
{
    backup_list_dir(l, L"Data", BK_DATA, true, now);
    i32 years[64];
    wchar_t rel[64];
    i32 n = backup_years(L"Clips", years, COUNT(years));
    for (i32 i = 0; i < n; i++) {
        swprintf(rel, COUNT(rel), L"Clips\\%d", years[i]);
        backup_list_dir(l, rel, BK_CLIP, true, now);
        swprintf(rel, COUNT(rel), L"Clips\\%d\\Source", years[i]);
        backup_list_dir(l, rel, BK_CLIP, true, now);
    }
    n = backup_years(L"Sessions", years, COUNT(years));
    for (i32 i = 0; i < n; i++) {
        swprintf(rel, COUNT(rel), L"Sessions\\%d", years[i]);
        backup_list_dir(l, rel, BK_SESSION, g_backup.sessions, now);
    }
}

static u64 backup_rel_hash(const wchar_t *rel)
{
    wchar_t low[MAX_PATH];
    size_t n = 0;
    for (; rel[n] && n < MAX_PATH - 1; n++) low[n] = towlower(rel[n]);
    return fnv64(low, n * sizeof(wchar_t));
}

// Deleted.txt: a line per clip or Data file gone from the save folder, "<100 ns since 1601> <path under the
// backup>". Sets each listed file's gone_since, and now for one newly gone. Returns the file's time, 0 if none.
static i64 backup_ledger_read(BackupList *l, i64 now)
{
    struct Gone { u64 hash; i64 since; };
    Gone *gone = 0;
    i32 n = 0, cap = 0;
    Path file = path_join(g_backup.to, BACKUP_LEDGER);
    i64 at = 0;
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetFileAttributesExW(file.s, GetFileExInfoStandard, &fa)) at = backup_ft(fa.ftLastWriteTime);
    if (FILE *f = _wfopen(file.s, L"rb")) {
        char line[MAX_PATH * 3 + 32];
        while (fgets(line, sizeof line, f)) {
            char *end = 0;
            i64 since = _strtoi64(line, &end, 10);
            if (end == line || *end != ' ' || since <= 0) continue;
            char *rel = end + 1;
            rel[strcspn(rel, "\r\n")] = 0;
            wchar_t w[MAX_PATH];
            utf8_to_wide(rel, w, MAX_PATH);
            if (n == cap) {
                cap = cap ? cap * 2 : 64;
                Gone *g = (Gone *)realloc(gone, sizeof *g * (size_t)cap);
                if (!g) break;
                gone = g;
            }
            gone[n++] = {backup_rel_hash(w), since};
        }
        fclose(f);
    }
    for (i32 i = 0; i < l->n; i++) {
        BackupFile &e = l->f[i];
        if (e.at_src || !e.at_bk || e.kind == BK_SESSION) continue;
        u64 h = backup_rel_hash(e.rel);
        e.gone_since = now;
        for (i32 k = 0; k < n; k++)
            if (gone[k].hash == h) {
                e.gone_since = gone[k].since;
                break;
            }
    }
    free(gone);
    return at;
}

static void backup_ledger_write(const BackupList &l)
{
    Path file = path_join(g_backup.to, BACKUP_LEDGER);
    FILE *f = _wfopen(file.s, L"wb");
    if (!f) {
        backup_fail("%ls couldn't be written (%d)", BACKUP_LEDGER, (int)GetLastError());
        return;
    }
    for (i32 i = 0; i < l.n; i++) {
        const BackupFile &e = l.f[i];
        if (!e.gone_since || e.act == BK_DROP) continue;
        char rel[MAX_PATH * 3];
        wide_to_utf8(e.rel, rel, sizeof rel);
        fprintf(f, "%lld %s\n", (long long)e.gone_since, rel);
    }
    fclose(f);
}

// The free space where the backup folder is, or would be made
static u64 backup_free(const Path &to)
{
    Path p = to;
    for (;;) {
        ULARGE_INTEGER avail;
        if (GetDiskFreeSpaceExW(p.s, &avail, 0, 0)) return avail.QuadPart;
        wchar_t *slash = wcsrchr(p.s, L'\\');
        if (!slash || slash - p.s < 2) return 0;
        *slash = 0;
    }
}

static DWORD CALLBACK backup_progress(LARGE_INTEGER total, LARGE_INTEGER got, LARGE_INTEGER, LARGE_INTEGER, DWORD,
                                      DWORD, HANDLE, HANDLE, LPVOID)
{
    InterlockedExchange64(&g_backup.file_size, total.QuadPart);
    InterlockedExchange64(&g_backup.file_got, got.QuadPart);
    return g_backup.stop ? PROGRESS_CANCEL : PROGRESS_CONTINUE;
}

static bool backup_copy(const BackupFile &e)
{
    Path src = path_join(g_backup.from, e.rel), dst = path_join(g_backup.to, e.rel);
    const wchar_t *name = wcsrchr(e.rel, L'\\');
    char u[128];
    wide_to_utf8(name ? name + 1 : e.rel, u, sizeof u);
    EnterCriticalSection(&g_backup.lock);
    str_set(g_backup.current, u);
    LeaveCriticalSection(&g_backup.lock);
    InterlockedExchange64(&g_backup.file_got, 0);
    InterlockedExchange64(&g_backup.file_size, (LONG64)e.size);

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
        return false;
    }
    g_backup.done++;
    g_backup.bytes += e.size;
    return true;
}

static bool backup_dir_exists(const wchar_t *p)
{
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static DWORD WINAPI backup_thread(void *)
{
    char to[MAX_PATH * 3];
    wide_to_utf8(g_backup.to.s, to, sizeof to);
    bool measure = g_backup.measure_only;
    if (!measure) log_line("backup: to %s%s", to, g_backup.sessions ? ", sessions too" : "");
    if (!measure) SHCreateDirectoryExW(0, g_backup.to.s, 0);
    if (!backup_dir_exists(g_backup.from.s)) {
        if (!measure) backup_fail("the save folder can't be reached");
    } else if (!measure && !backup_dir_exists(g_backup.to.s)) {
        backup_fail("%s can't be reached (%d)", to, (int)GetLastError());
    } else {
        i64 now = backup_now_100ns();
        BackupList l = {};
        backup_list(&l, now);
        i64 last = backup_ledger_read(&l, now);
        u64 used[BK_KINDS] = {}, held = 0;
        for (i32 i = 0; i < l.n; i++) used[l.f[i].kind] += l.f[i].bk_size;
        for (u64 u : used) held += u;
        EnterCriticalSection(&g_backup.lock);
        memcpy(g_backup.used, used, sizeof used);
        g_backup.room = backup_free(g_backup.to) + held;
        g_backup.last_us = last ? (last - 116444736000000000LL) / 10 : 0;
        g_backup.measured = true;
        LeaveCriticalSection(&g_backup.lock);
        if (!measure && !g_backup.stop) {
            backup_plan(l.f, l.n, backup_cap(g_backup.cap_gb, g_backup.room), now, g_backup.sessions);
            for (i32 i = 0; i < l.n; i++) g_backup.total += l.f[i].act == BK_COPY;
            for (i32 i = 0; i < l.n && !g_backup.stop; i++) {
                BackupFile &e = l.f[i];
                if (e.act != BK_DROP) continue;
                Path p = path_join(g_backup.to, e.rel);
                if (DeleteFileW(p.s)) g_backup.dropped++;
                else e.act = BK_KEEP;
            }
            for (i32 i = 0; i < l.n && !g_backup.stop; i++)
                if (l.f[i].act == BK_COPY && !backup_copy(l.f[i])) l.f[i].act = l.f[i].at_bk ? BK_KEEP : BK_SKIP;
            if (!g_backup.stop) backup_ledger_write(l);
            u64 now_used[BK_KINDS] = {};
            for (i32 i = 0; i < l.n; i++) {
                const BackupFile &e = l.f[i];
                if (e.act == BK_COPY) now_used[e.kind] += e.size;
                else if (e.act == BK_KEEP && e.at_bk) now_used[e.kind] += e.bk_size;
            }
            EnterCriticalSection(&g_backup.lock);
            memcpy(g_backup.used, now_used, sizeof now_used);
            if (!g_backup.stop) g_backup.last_us = now_us();
            LeaveCriticalSection(&g_backup.lock);
        }
        free(l.f);
    }
    if (measure) {
        InterlockedExchange(&g_backup.state, g_backup.before);
        ui_wake(3);
        return 0;
    }
    EnterCriticalSection(&g_backup.lock);
    g_backup.ended_us = now_us();
    g_backup.current[0] = 0;
    LeaveCriticalSection(&g_backup.lock);
    log_line("backup: %d of %d files copied (%llu bytes), %d deleted, %d failed%s", g_backup.done, g_backup.total,
             (unsigned long long)g_backup.bytes, g_backup.dropped, g_backup.failed, g_backup.stop ? ", stopped" : "");
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

static void backup_start(bool measure)
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
    g_backup.sessions = c.backup_sessions;
    g_backup.cap_gb = c.backup_cap_gb;
    g_backup.measure_only = measure;
    g_backup.before = g_backup.state;
    bool same = backup_same_folder(c.save_dir, c.backup_dir);
    if (measure && same) return;
    if (!measure) {
        g_backup.error[0] = 0;
        g_backup.done = g_backup.total = g_backup.failed = g_backup.dropped = 0;
        g_backup.bytes = 0;
    }
    if (same) {
        backup_fail("the backup folder is the save folder");
        g_backup.ended_us = now_us();
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

// Once a second on the main thread: starts a run when one is due, or a measure Options asked for.
static void backup_tick()
{
    const Config &c = g_app.config;
    if (g_backup.state == BACKUP_RUNNING) {
        ui_wake(2); // the status line moves
        return;
    }
    if (!c.backup_dir.s[0]) return;
    i64 now = now_us();
    bool due = c.backup_on && (InterlockedExchange(&g_backup.wanted, 0) || now >= g_backup.next_us);
    if (due) {
        g_backup.next_us = now + BACKUP_EVERY_US;
        InterlockedExchange(&g_backup.measure, 0);
        backup_start(false);
    } else if (InterlockedExchange(&g_backup.measure, 0)) {
        backup_start(true);
    }
}

static void backup_now() { InterlockedExchange(&g_backup.wanted, 1); }

// Options wants the backup's size: after the folder changed, or when it isn't known yet
static void backup_measure()
{
    EnterCriticalSection(&g_backup.lock);
    g_backup.measured = false;
    LeaveCriticalSection(&g_backup.lock);
    InterlockedExchange(&g_backup.measure, 1);
}

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
    if (s == BACKUP_RUNNING && !g_backup.measure_only) return "running";
    if (!c.backup_on || !c.backup_dir.s[0]) return "off";
    if (s == BACKUP_FAILED) return "failed";
    if (s == BACKUP_DONE || g_backup.last_us) return "ok";
    return "waiting";
}

// A time for the status line: "14:05" today, else "26 Nov 14:05"
static void backup_when(i64 us, char *out, i32 cap)
{
    static const char *const months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    auto local = [](i64 t, SYSTEMTIME *st) {
        u64 ft = (u64)(t * 10 + 116444736000000000LL);
        FILETIME f = {(DWORD)ft, (DWORD)(ft >> 32)};
        SYSTEMTIME utc;
        FileTimeToSystemTime(&f, &utc);
        SystemTimeToTzSpecificLocalTime(0, &utc, st);
    };
    SYSTEMTIME t, n;
    local(us, &t);
    local(now_us(), &n);
    if (t.wYear == n.wYear && t.wMonth == n.wMonth && t.wDay == n.wDay) snprintf(out, cap, "%02d:%02d", t.wHour, t.wMinute);
    else snprintf(out, cap, "%d %s %02d:%02d", t.wDay, months[(t.wMonth + 11) % 12], t.wHour, t.wMinute);
}

// One line for Options: what the backup is doing or last did.
static void backup_status(char *out, i32 cap)
{
    const Config &c = g_app.config;
    LONG s = g_backup.state;
    EnterCriticalSection(&g_backup.lock);
    char at[32] = "", last[32] = "";
    if (g_backup.ended_us) backup_when(g_backup.ended_us, at, sizeof at);
    if (g_backup.last_us) backup_when(g_backup.last_us, last, sizeof last);
    if (!c.backup_dir.s[0]) {
        snprintf(out, cap, "Choose a folder on another drive or a NAS");
    } else if (s == BACKUP_RUNNING && !g_backup.measure_only) {
        LONG64 got = g_backup.file_got, size = g_backup.file_size;
        if (g_backup.current[0])
            snprintf(out, cap, "Copying %s, %d of %d, %d%%", g_backup.current, g_backup.done + g_backup.failed + 1,
                     g_backup.total, size > 0 ? (int)(got * 100 / size) : 0);
        else snprintf(out, cap, "Looking for new files");
    } else if (s == BACKUP_FAILED) {
        snprintf(out, cap, "Failed at %s: %s", at, g_backup.error);
    } else if (!c.backup_on) {
        snprintf(out, cap, "Off");
    } else if (last[0]) {
        snprintf(out, cap, "%s", last);
    } else {
        snprintf(out, cap, "Not yet");
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
