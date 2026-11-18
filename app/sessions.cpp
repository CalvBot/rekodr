// Sessions (SPEC.md, "1. Whole sessions"): finding game sessions as they happen, the "save that session?" popup,
// the unsaved sessions, and sessions made by hand.
//
// Detection runs on the main thread, from the game being played (audio.cpp's watcher calls sessions_on_game when
// it changes) and the 1 s tick. A session starts when a game whose session mode isn't Never becomes the game being
// played. It ends once that game has been gone longer than the session gap, so a crash and relaunch stay one
// session. One shorter than the minimum length is dropped.
//
// On disk:
//   <local_dir>\sessions.txt                     open sessions and ones still in the buffer, rewritten whole, so a
//                                                restart of clipback carries on where it was
//   <save_dir>\<year>\Data\<name>.session.txt    the record of a saved or hand-made session (SPEC.md, "Folders")
// At start, sessions.txt is read and activity.tsv is replayed to add what the file doesn't know (it may be lost,
// or out of date after a crash).


static i64 sessions_gap_us()
{
    f32 m = g_app.config.session_gap_min;
    return (i64)((m > 0 ? m : 0) * 60e6);
}

static i64 sessions_min_us()
{
    f32 m = g_app.config.session_min_length_min;
    return (i64)((m > 0 ? m : 0) * 60e6);
}

static const AppInfo *sessions_app(const char *exe)
{
    const Config &c = g_app.config;
    for (i32 i = 0; i < c.app_count; i++)
        if (!strcmp(c.apps[i].exe.s, exe)) return &c.apps[i];
    return 0;
}

// A game that gets sessions: sorted as a game and not set to Never.
static bool sessions_game_ok(const char *exe)
{
    const AppInfo *a = exe[0] ? sessions_app(exe) : 0;
    return a && app_category(g_app.config, *a) == CAT_GAME && a->session_mode != SESSION_NEVER;
}

static void sessions_game_name(const char *exe, char (&out)[128])
{
    audio_display_name(g_app.config, exe, out, sizeof out);
    clean_name(out); // it names the file
}

static Session *session_find(u32 id)
{
    Sessions &ss = g_app.sessions;
    for (i32 i = 0; i < COUNT(ss.items); i++)
        if (id && ss.items[i].state != SES_FREE && ss.items[i].id == id) return &ss.items[i];
    return 0;
}

// A zeroed slot. When the table is full, the oldest saved or dismissed one makes room: those are only kept so
// they aren't found again in activity.tsv.
static Session *session_slot()
{
    Sessions &ss = g_app.sessions;
    Session *old = 0;
    for (i32 i = 0; i < COUNT(ss.items); i++) {
        Session *s = &ss.items[i];
        if (s->state == SES_FREE) { *s = {}; return s; }
        if (s->state == SES_SAVED && (!old || s->end_us < old->end_us)) old = s;
    }
    if (!old) {
        log_line("sessions: all %d slots hold sessions not saved yet; a new one is lost", COUNT(ss.items));
        return 0;
    }
    *old = {};
    return old;
}

static Session *session_new(const char *exe, const char *name, i64 start_us, i64 end_us)
{
    Session *s = session_slot();
    if (!s) return 0;
    s->id = ++g_app.sessions.next_id;
    s->state = SES_OPEN;
    str_set(s->exe, exe);
    str_set(s->name, name);
    s->custom = !exe[0];
    s->start_us = start_us;
    s->end_us = end_us;
    g_app.sessions.dirty = true;
    return s;
}

// ---------------------------------------------------------------------------------------------------------------
// Text

// "40 s", "25 min", "2 h 14 min"
static void fmt_length(i64 us, char *out, i32 cap)
{
    i64 s = us > 0 ? us / 1000000 : 0;
    if (s < 60) snprintf(out, cap, "%lld s", (long long)s);
    else if (s < 3600) snprintf(out, cap, "%lld min", (long long)(s / 60));
    else if ((s / 60) % 60 == 0) snprintf(out, cap, "%lld h", (long long)(s / 3600));
    else snprintf(out, cap, "%lld h %lld min", (long long)(s / 3600), (long long)((s / 60) % 60));
}

// "19:02", or "14 Nov 19:02" on another day than today (or 7:02 pm, by the clock setting)
static void fmt_clock(i64 us, char *out, i32 cap)
{
    SYSTEMTIME t = local_time(us), today = local_time(now_us());
    char hm[16];
    fmt_hm(t, hm, sizeof hm);
    if (t.wYear == today.wYear && t.wMonth == today.wMonth && t.wDay == today.wDay) snprintf(out, cap, "%s", hm);
    else snprintf(out, cap, "%d %.3s %s", t.wDay, month_name(t.wMonth), hm);
}

static void fmt_stamp(i64 us, char *out, i32 cap)
{
    SYSTEMTIME t = local_time(us);
    snprintf(out, cap, "%04d-%02d-%02d %02d:%02d:%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
}

// ---------------------------------------------------------------------------------------------------------------
// The Data record: name, game or custom, the real start and end, and which part has footage (SPEC.md, "Folders").
// Named after the video once there is one, else after the name and start the way a session video would be.

static void session_write_record(Session *s, i32 video_year)
{
    i64 oldest = g_app.recorder.oldest_us;
    i64 foot_from = s->start_us, foot_to = s->end_us;
    if (oldest && foot_from < oldest) foot_from = oldest;
    if (foot_to <= foot_from) foot_from = foot_to = 0; // none of it is in the buffer: only declared

    // The year is the year the footage starts in.
    SYSTEMTIME t = local_time(foot_from ? foot_from : s->start_us);
    i32 year = video_year ? video_year : t.wYear;
    char stem[320];
    if (s->video[0]) {
        str_set(stem, s->video);
        size_t n = strlen(stem);
        if (n > 4 && !_stricmp(stem + n - 4, ".mp4")) stem[n - 4] = 0;
    } else {
        SYSTEMTIME st = local_time(s->start_us);
        snprintf(stem, sizeof stem, "%s %04d-%02d-%02d %02d-%02d", s->name, st.wYear, st.wMonth, st.wDay, st.wHour,
                 st.wMinute);
    }
    char rel[256];
    snprintf(rel, sizeof rel, "%d\\Data\\%s.session.txt", year, stem);

    wchar_t wrel[256];
    utf8_to_wide(rel, wrel, COUNT(wrel));
    Path file = path_join(g_app.config.save_dir, wrel), dir = file, tmp = file;
    *wcsrchr(dir.s, L'\\') = 0;
    wcscat_s(tmp.s, L".tmp");
    SHCreateDirectoryExW(0, dir.s, 0);
    FILE *f = _wfopen(tmp.s, L"wb");
    if (!f) {
        log_line("session %u: couldn't write its record %s (%d)", s->id, rel, (int)GetLastError());
        return;
    }
    char a[32], b[32];
    fmt_stamp(s->start_us, a, sizeof a);
    fmt_stamp(s->end_us, b, sizeof b);
    fprintf(f, "# clipback session record. Its video, if saved, has the same name with .mp4 in Sessions.\n");
    fprintf(f, "name = %s\nkind = %s\ngame = %s\n", s->name, s->custom ? "custom" : "game", s->exe);
    fprintf(f, "start = %s\nend = %s\nstart_us = %lld\nend_us = %lld\n", a, b, (long long)s->start_us,
            (long long)s->end_us);
    fprintf(f, "footage_start_us = %lld\nfootage_end_us = %lld\n", (long long)foot_from, (long long)foot_to);
    fprintf(f, "video = %s\n", s->video);
    if (s->comment[0]) fprintf(f, "comment = %s\n", s->comment);
    bool ok = fclose(f) == 0 && MoveFileExW(tmp.s, file.s, MOVEFILE_REPLACE_EXISTING);
    if (!ok) {
        log_line("session %u: couldn't write its record %s (%d)", s->id, rel, (int)GetLastError());
        return;
    }
    if (s->record[0] && strcmp(s->record, rel)) { // renamed (saved, or changed): the old one goes
        wchar_t old[256];
        utf8_to_wide(s->record, old, COUNT(old));
        DeleteFileW(path_join(g_app.config.save_dir, old).s);
    }
    str_set(s->record, rel);
    g_app.sessions.dirty = true;
}

// ---------------------------------------------------------------------------------------------------------------
// Saving

static bool session_save(Session *s)
{
    u32 job = save_range(s->start_us, s->end_us, SAVE_SESSION, s->name); // the buffer's own start clips it
    if (!job) {
        log_line("session %u (%s): couldn't queue the save", s->id, s->name);
        if (s->state == SES_ASKING) s->state = SES_NOT_SAVED;
        g_app.sessions.dirty = true;
        return false;
    }
    log_line("session %u (%s): saving as job %u", s->id, s->name, job);
    s->job = job;
    s->state = SES_SAVING;
    g_app.sessions.dirty = true;
    ui_wake();
    return true;
}

static bool library_set_comment(u64 key, const char *text, const char *name); // library.cpp

// Follows the save jobs of sessions being saved.
static void sessions_check_saves()
{
    Sessions &ss = g_app.sessions;
    for (i32 i = 0; i < COUNT(ss.items); i++) {
        Session *s = &ss.items[i];
        if (s->state != SES_SAVING) continue;
        const SaveJobView *v = save_view(s->job);
        if (v && v->state < SAVE_DONE) continue;
        if (v && v->state == SAVE_DONE) {
            s->state = SES_SAVED;
            const char *base = strrchr(v->path, '\\');
            str_set(s->video, base ? base + 1 : v->path);
            // The year folder the save chose: <save_dir>\<year>\Sessions\<file>
            i32 year = 0;
            const char *sessions_dir = base && base - v->path > 9 ? base - 9 : 0;
            if (sessions_dir && !strncmp(sessions_dir, "\\Sessions", 9)) {
                const char *y = sessions_dir;
                while (y > v->path && y[-1] != '\\') y--;
                year = atoi(y);
            }
            session_write_record(s, year);
            log_line("session %u (%s): saved", s->id, s->name);
            // Its comment goes with the file, keyed as the Library keys it (its size and time).
            wchar_t wp[MAX_PATH];
            WIN32_FILE_ATTRIBUTE_DATA fa;
            utf8_to_wide(v->path, wp, COUNT(wp));
            if (s->comment[0] && GetFileAttributesExW(wp, GetFileExInfoStandard, &fa)) {
                u64 size = ((u64)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
                i64 mt = filetime_us(fa.ftLastWriteTime);
                char stem[256];
                str_set(stem, s->video);
                if (char *dot = strrchr(stem, '.')) *dot = 0;
                library_set_comment(fnv64(&mt, sizeof mt, fnv64(&size, sizeof size)), s->comment, stem);
            }
        } else {
            s->state = SES_NOT_SAVED;
            log_line("session %u (%s): the save %s; it's under unsaved sessions", s->id, s->name,
                     !v ? "was lost" : v->state == SAVE_FAILED ? "failed" : "was cancelled");
        }
        s->job = 0;
        ss.dirty = true;
        ui_wake();
    }
}

// Drops sessions whose footage has all left the buffer (open ones end first, so they never get here early).
static void sessions_expire()
{
    Sessions &ss = g_app.sessions;
    i64 oldest = g_app.recorder.oldest_us;
    if (!oldest) return; // not known yet
    for (i32 i = 0; i < COUNT(ss.items); i++) {
        Session *s = &ss.items[i];
        if (s->state == SES_FREE || s->state == SES_OPEN || s->state == SES_SAVING || s->end_us >= oldest) continue;
        if (s->state == SES_ASKING || s->state == SES_NOT_SAVED)
            log_line("session %u (%s): not saved, and now gone from the buffer", s->id, s->name);
        *s = {};
        ss.dirty = true;
        ui_wake();
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Detection

static void session_end(Session *s)
{
    Sessions &ss = g_app.sessions;
    char len[32];
    fmt_length(s->end_us - s->start_us, len, sizeof len);
    ss.dirty = true;
    if (s->end_us - s->start_us < sessions_min_us()) {
        log_line("session %u (%s): ended after %s, shorter than the minimum; dropped", s->id, s->name, len);
        *s = {};
        return;
    }
    const AppInfo *a = sessions_app(s->exe);
    SessionMode mode = a ? a->session_mode : SESSION_ASK;
    log_line("session %u (%s): ended after %s", s->id, s->name, len);
    if (mode == SESSION_NEVER) { // changed to Never while it ran
        *s = {};
    } else if (mode == SESSION_ALWAYS) {
        session_save(s);
    } else {
        s->state = SES_ASKING;
        s->prompted = false;
        ui_wake();
    }
}

// The game being played at `now` (empty on the desktop). Extends its session or starts one, and ends sessions
// whose game has been gone longer than the gap.
static void sessions_observe(i64 now, const char *game)
{
    Sessions &ss = g_app.sessions;
    bool found = false;
    for (i32 i = 0; i < COUNT(ss.items); i++) {
        Session *s = &ss.items[i];
        if (s->state != SES_OPEN || s->custom || !game[0] || strcmp(s->exe, game)) continue;
        s->end_us = now;
        found = true;
    }
    i64 gap = sessions_gap_us();
    for (i32 i = 0; i < COUNT(ss.items); i++) {
        Session *s = &ss.items[i];
        if (s->state == SES_OPEN && now - s->end_us > gap) session_end(s);
    }
    if (!found && sessions_game_ok(game)) {
        char name[128];
        sessions_game_name(game, name);
        Session *s = session_new(game, name, now, now);
        if (s) log_line("session %u (%s): started", s->id, name);
    }
}

static const char *STATE_NAMES[] = {"free", "open", "asking", "not_saved", "saving", "saved"};

// sessions.txt is written by a thread of its own, so a slow disk never holds up the window. The main thread
// formats the file into a buffer and hands it over; a newer one replaces one not written yet.
static struct {
    SRWLOCK lock;
    HANDLE  wake, thread;
    char   *pending; // malloc'd text the thread hasn't taken yet; 0 = none
    i32     pending_len;
    bool    stop;
    Path    file;
} g_ses_writer = {SRWLOCK_INIT};

static DWORD WINAPI sessions_writer_main(void *)
{
    for (;;) {
        WaitForSingleObject(g_ses_writer.wake, INFINITE);
        AcquireSRWLockExclusive(&g_ses_writer.lock);
        char *text = g_ses_writer.pending;
        i32 len = g_ses_writer.pending_len;
        bool stop = g_ses_writer.stop;
        g_ses_writer.pending = 0;
        ReleaseSRWLockExclusive(&g_ses_writer.lock);
        if (text) {
            Path tmp = g_ses_writer.file;
            wcscat_s(tmp.s, L".tmp");
            FILE *f = _wfopen(tmp.s, L"wb");
            bool ok = f && fwrite(text, 1, len, f) == (size_t)len;
            if (f) ok = fclose(f) == 0 && ok;
            ok = ok && MoveFileExW(tmp.s, g_ses_writer.file.s, MOVEFILE_REPLACE_EXISTING);
            if (!ok) log_line("sessions: couldn't save sessions.txt (%d)", (int)GetLastError());
            free(text);
        }
        if (stop) return 0;
    }
}

static void sessions_write()
{
    Sessions &ss = g_app.sessions;
    i32 cap = 512 + COUNT(ss.items) * (64 + (i32)sizeof(Session)), len = 0;
    char *text = (char *)malloc(cap);
    if (!text) return;
    i64 now = now_us();
    len += snprintf(text + len, cap - len, "# clipback's open sessions and the ones still in the buffer. Rewritten by clipback.\n");
    len += snprintf(text + len, cap - len, "written_us\t%lld\nnext_id\t%u\n", (long long)now, ss.next_id);
    len += snprintf(text + len, cap - len, "# session  id  state  custom  start_us  end_us  exe  name  video  record  comment\n");
    for (i32 i = 0; i < COUNT(ss.items); i++) {
        const Session &s = ss.items[i];
        if (s.state == SES_FREE) continue;
        // A save in progress dies with clipback: it comes back as not saved.
        SessionState st = s.state == SES_SAVING ? SES_NOT_SAVED : s.state;
        len += snprintf(text + len, cap - len, "session\t%u\t%s\t%d\t%lld\t%lld\t%s\t%s\t%s\t%s\t%s\n", s.id,
                        STATE_NAMES[st], s.custom ? 1 : 0, (long long)s.start_us, (long long)s.end_us, s.exe, s.name,
                        s.video, s.record, s.comment);
    }
    CHECK(len < cap, "sessions.txt fits its buffer; %d of %d", len, cap);

    if (!g_ses_writer.thread) {
        g_ses_writer.file = path_join(g_app.paths.local_dir, L"sessions.txt");
        g_ses_writer.wake = CreateEventW(0, FALSE, FALSE, 0);
        g_ses_writer.thread = CreateThread(0, 0, sessions_writer_main, 0, 0, 0);
    }
    AcquireSRWLockExclusive(&g_ses_writer.lock);
    free(g_ses_writer.pending); // never written: this one is newer
    g_ses_writer.pending = text;
    g_ses_writer.pending_len = len;
    ReleaseSRWLockExclusive(&g_ses_writer.lock);
    SetEvent(g_ses_writer.wake);
    ss.dirty = false;
    ss.written_us = now;
}

// On quit: writes what changed since the last write and waits for the file to be on disk.
static void sessions_stop()
{
    if (g_app.sessions.loaded && g_app.sessions.dirty) sessions_write();
    if (!g_ses_writer.thread) return;
    AcquireSRWLockExclusive(&g_ses_writer.lock);
    g_ses_writer.stop = true;
    ReleaseSRWLockExclusive(&g_ses_writer.lock);
    SetEvent(g_ses_writer.wake);
    if (WaitForSingleObject(g_ses_writer.thread, 5000) != WAIT_OBJECT_0) log_line("sessions: sessions.txt still writing at quit");
    CloseHandle(g_ses_writer.thread);
    g_ses_writer.thread = 0;
}

// What activity.tsv says happened before this run, worked out with the same rules as live detection.
struct SessionGuess { char exe[64]; i64 start_us, end_us; bool open; };
static SessionGuess g_guess[256];

static void sessions_replay(i64 alive_until)
{
    Sessions &ss = g_app.sessions;
    // This run's own lines are watched live.
    FILETIME created, x, k, u;
    GetProcessTimes(GetCurrentProcess(), &created, &x, &k, &u);
    i64 cutoff = filetime_to_us(created);
    // Clipback (and the recorder with it) ran at least until the last video it wrote.
    i64 video_end = 0, written = 0;
    if (buffer_video_newest_end(g_app.config.buffer_dir, &video_end, &written) && video_end < cutoff &&
        video_end > alive_until)
        alive_until = video_end;

    i64 gap = sessions_gap_us(), min = sessions_min_us();
    i32 n = 0;
    char prev[64] = {};
    i64 last_t = 0;
    AcquireSRWLockShared(&g_activity_lock);
    FILE *f = _wfopen(path_join(g_app.config.buffer_dir, L"activity.tsv").s, L"rb");
    if (f) {
        char line[256];
        while (fgets(line, sizeof line, f)) {
            char *tab = strchr(line, '\t');
            if (!tab) continue;
            *tab = 0;
            char *end = 0;
            i64 t = strtoll(line, &end, 10);
            if (end == line) continue;
            if (t >= cutoff) break;
            const char *game = trim(tab + 1);
            bool found = false;
            for (i32 i = 0; i < n; i++) {
                SessionGuess &g = g_guess[i];
                if (!g.open) continue;
                if (!strcmp(g.exe, prev)) g.end_us = t; // played until this change
                if (t - g.end_us > gap) g.open = false;
                else if (game[0] && !strcmp(g.exe, game)) found = true;
            }
            if (!found && sessions_game_ok(game)) {
                if (n == COUNT(g_guess)) { // only the recent past matters
                    memmove(g_guess, g_guess + 1, (n - 1) * sizeof g_guess[0]);
                    n--;
                }
                SessionGuess &g = g_guess[n++];
                str_set(g.exe, game);
                g.start_us = g.end_us = t;
                g.open = true;
            }
            str_set(prev, game);
            last_t = t;
        }
        fclose(f);
    }
    ReleaseSRWLockShared(&g_activity_lock);
    if (alive_until < last_t) alive_until = last_t;

    i64 now = now_us();
    i32 added = 0;
    for (i32 i = 0; i < n; i++) {
        SessionGuess &g = g_guess[i];
        if (g.open && prev[0] && !strcmp(g.exe, prev)) g.end_us = alive_until; // still on when clipback stopped
        bool still_open = g.open && now - g.end_us <= gap;
        if (!still_open && g.end_us - g.start_us < min) continue;
        Session *known = 0;
        for (i32 j = 0; j < COUNT(ss.items) && !known; j++) {
            Session *s = &ss.items[j];
            if (s->state != SES_FREE && !s->custom && !strcmp(s->exe, g.exe) && s->start_us <= g.end_us + gap &&
                s->end_us + gap >= g.start_us)
                known = s;
        }
        if (known) {
            // An open session carries on to when its game was last seen; ended ones are already settled.
            if (known->state == SES_OPEN && g.end_us > known->end_us) {
                known->end_us = g.end_us;
                if (g.start_us < known->start_us) known->start_us = g.start_us;
            }
            continue;
        }
        char name[128];
        sessions_game_name(g.exe, name);
        Session *s = session_new(g.exe, name, g.start_us, g.end_us);
        if (!s) break;
        // One that ended while clipback wasn't watching isn't asked about now; it waits under unsaved sessions.
        if (!still_open) s->state = SES_NOT_SAVED;
        added++;
    }
    if (added) log_line("sessions: %d found in activity.tsv", added);
}

static void sessions_load()
{
    Sessions &ss = g_app.sessions;
    ss.loaded = true;
    i64 alive_until = 0; // clipback was watching at least until then
    i32 loaded = 0;
    FILE *f = _wfopen(path_join(g_app.paths.local_dir, L"sessions.txt").s, L"rb");
    if (f) {
        char line[1280];
        while (fgets(line, sizeof line, f)) {
            size_t len = strlen(line);
            while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = 0;
            char *fl[11];
            i32 n = split_tabs(line, fl, COUNT(fl));
            if (n == 2 && !strcmp(fl[0], "written_us")) {
                alive_until = strtoll(fl[1], 0, 10);
            } else if (n == 2 && !strcmp(fl[0], "next_id")) {
                ss.next_id = (u32)strtoul(fl[1], 0, 10);
            } else if ((n == 10 || n == 11) && !strcmp(fl[0], "session")) {
                SessionState st = SES_FREE;
                for (i32 i = 1; i < COUNT(STATE_NAMES); i++)
                    if (!strcmp(fl[2], STATE_NAMES[i])) st = (SessionState)i;
                if (st == SES_FREE || st == SES_SAVING) continue;
                Session *s = session_slot();
                if (!s) break;
                s->id = (u32)strtoul(fl[1], 0, 10);
                s->state = st;
                s->custom = atoi(fl[3]) != 0;
                s->start_us = strtoll(fl[4], 0, 10);
                s->end_us = strtoll(fl[5], 0, 10);
                str_set(s->exe, fl[6]);
                str_set(s->name, fl[7]);
                str_set(s->video, fl[8]);
                str_set(s->record, fl[9]);
                if (n == 11) str_set(s->comment, fl[10]);
                if (s->id > ss.next_id) ss.next_id = s->id;
                loaded++;
            }
        }
        fclose(f);
    }
    if (loaded) log_line("sessions: %d carried over from the last run", loaded);
    sessions_replay(alive_until);
    ss.dirty = true;
}

// Is a game (or anything else filling its monitor, such as an unsorted game or a video) in front? The popup waits
// until it isn't, so it never pulls the focus away mid-game.
static bool sessions_game_in_front()
{
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (pid == GetCurrentProcessId()) return false;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h) {
        wchar_t path[MAX_PATH];
        DWORD len = MAX_PATH;
        bool game = false;
        if (QueryFullProcessImageNameW(h, 0, path, &len)) {
            wchar_t *base = wcsrchr(path, L'\\');
            base = base ? base + 1 : path;
            CharLowerBuffW(base, (DWORD)wcslen(base));
            char exe[MAX_PATH * 3];
            wide_to_utf8(base, exe, sizeof exe);
            const AppInfo *a = sessions_app(exe);
            game = a && app_category(g_app.config, *a) == CAT_GAME;
        }
        CloseHandle(h);
        if (game) return true;
    }
    if (fg == GetShellWindow() || fg == GetDesktopWindow()) return false;
    wchar_t cls[64] = {};
    GetClassNameW(fg, cls, COUNT(cls));
    if (!wcscmp(cls, L"WorkerW") || !wcscmp(cls, L"Progman")) return false; // the desktop
    RECT r;
    MONITORINFO mi = {sizeof mi};
    if (!GetWindowRect(fg, &r) || !GetMonitorInfoW(MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST), &mi)) return false;
    return r.left <= mi.rcMonitor.left && r.top <= mi.rcMonitor.top && r.right >= mi.rcMonitor.right &&
           r.bottom >= mi.rcMonitor.bottom;
}

// The session the popup asks about: the one that ended first. `waiting` gets how many more are queued.
static Session *sessions_asking(i32 *waiting)
{
    Sessions &ss = g_app.sessions;
    Session *first = 0;
    i32 n = 0;
    for (i32 i = 0; i < COUNT(ss.items); i++) {
        Session *s = &ss.items[i];
        if (s->state != SES_ASKING) continue;
        n++;
        if (!first || s->end_us < first->end_us) first = s;
    }
    if (waiting) *waiting = n > 0 ? n - 1 : 0;
    return first;
}

// Brings the window up for the popup, once per session, unless a game is in front.
static void sessions_prompt()
{
    Sessions &ss = g_app.sessions;
    Session *s = sessions_asking(0);
    if (!s) return;
    if (s->prompted) {
        if (g_app.ui.shown) return;
        // The window was closed with the popup up: that's Not now, for every session waiting.
        for (i32 i = 0; i < COUNT(ss.items); i++) {
            if (ss.items[i].state != SES_ASKING) continue;
            ss.items[i].state = SES_NOT_SAVED;
            log_line("session %u (%s): window closed; unsaved", ss.items[i].id, ss.items[i].name);
        }
        ss.dirty = true;
        return;
    }
    if (sessions_game_in_front()) {
        if (ss.waiting_logged != s->id) log_line("session %u (%s): popup waits until no game is in front", s->id, s->name);
        ss.waiting_logged = s->id;
        return;
    }
    for (i32 i = 0; i < COUNT(ss.items); i++)
        if (ss.items[i].state == SES_ASKING) ss.items[i].prompted = true; // the queue shows in the same popup
    log_line("session %u (%s): asking", s->id, s->name);
    // Like ui_show, but on top without activating: the popup never takes the keyboard (SPEC.md), so whatever is
    // in front, such as a game just relaunched, keeps it.
    Ui &ui = g_app.ui;
    if (!ui.hwnd) return;
    if (!ui.shown) ShowWindow(ui.hwnd, SW_SHOWNA); // keeps it maximized if it was
    if (IsIconic(ui.hwnd)) ShowWindow(ui.hwnd, SW_SHOWNOACTIVATE);
    SetWindowPos(ui.hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    ui.shown = true;
    ui_wake(4);
}

// ---------------------------------------------------------------------------------------------------------------
// Main-thread entry points

static void sessions_tick()
{
    Sessions &ss = g_app.sessions;
    if (!ss.loaded) sessions_load();
    i64 now = now_us();
    // Until the watcher has spoken (2 s after start) every game looks gone, and a carried-over session would end.
    if (ss.game_known) sessions_observe(now, g_app.audio.game.s);
    sessions_check_saves();
    sessions_expire();
    sessions_prompt();
    bool open = false;
    for (i32 i = 0; i < COUNT(ss.items) && !open; i++) open = ss.items[i].state == SES_OPEN;
    if (ss.dirty || (open && now - ss.written_us > 10000000)) sessions_write(); // keeps the last-seen time fresh
    // The unsaved sessions' countdowns are in minutes: one redraw a minute keeps them right without drawing every second.
    if (g_app.ui.shown && now / 60000000 != ss.woke_minute) {
        ss.woke_minute = now / 60000000;
        ui_wake();
    }
}

// A game's name was found out after its session began (a modpack's window came up): the open session takes it.
static void sessions_game_renamed(const char *exe)
{
    Sessions &ss = g_app.sessions;
    for (i32 i = 0; i < COUNT(ss.items); i++) {
        Session *s = &ss.items[i];
        if (s->state != SES_OPEN || s->custom || strcmp(s->exe, exe)) continue;
        char name[128];
        sessions_game_name(exe, name);
        if (!strcmp(name, s->name)) continue;
        log_line("session %u: %s is %s", s->id, s->name, name);
        str_set(s->name, name);
        ss.dirty = true;
    }
}

static void sessions_on_game(const char *exe)
{
    Sessions &ss = g_app.sessions;
    if (!ss.loaded) sessions_load();
    ss.game_known = true;
    sessions_observe(now_us(), exe);
}

// Saves a session from the list or the library.
static void sessions_save(u32 id)
{
    Session *s = session_find(id);
    if (s && s->state != SES_OPEN && s->state != SES_SAVING) session_save(s);
}

// Picks a game by its exe or shown name, for sessions made by hand.
static const AppInfo *sessions_game_named(const char *name)
{
    const Config &c = g_app.config;
    for (i32 i = 0; i < c.app_count; i++) {
        const AppInfo &a = c.apps[i];
        if (app_category(c, a) != CAT_GAME) continue;
        char shown[128];
        sessions_game_name(a.exe.s, shown);
        if (!_stricmp(a.exe.s, name) || !_stricmp(shown, name)) return &a;
    }
    return 0;
}

// A session made by hand (timeline or library): in and out points and a game or a typed name. Its record goes to
// Data straight away, so it exists before or without its video, and one longer than the buffer keeps its real
// start and end. If any of it is still in the buffer it's also listed as unsaved, to save from there (or
// with sessions_save). Returns its id, or 0 if it's only a record (none of it is in the buffer).
static u32 sessions_add_manual(i64 from_us, i64 to_us, const char *name)
{
    if (to_us <= from_us) return 0;
    const AppInfo *game = sessions_game_named(name);
    char shown[128];
    if (game) {
        sessions_game_name(game->exe.s, shown);
    } else {
        str_set(shown, name);
        clean_name(shown);
        if (!shown[0]) str_set(shown, "Session");
    }
    Session *s = session_new(game ? game->exe.s : "", shown, from_us, to_us);
    if (!s) return 0;
    s->state = SES_NOT_SAVED;
    session_write_record(s, 0);
    char a[32], b[32];
    fmt_stamp(from_us, a, sizeof a);
    fmt_stamp(to_us, b, sizeof b);
    i64 oldest = g_app.recorder.oldest_us;
    if (oldest && to_us <= oldest) {
        log_line("session %u (%s): made by hand, %s to %s; none of it is in the buffer, so it's only a record", s->id,
                 shown, a, b);
        *s = {};
        g_app.sessions.dirty = true;
        return 0;
    }
    log_line("session %u (%s): made by hand, %s to %s", s->id, shown, a, b);
    ui_wake();
    return s->id;
}

// Changes a session's start, end or name, including an automatic one (SPEC.md: "can be changed later the same
// way"). Its record follows.
static void sessions_change(u32 id, i64 from_us, i64 to_us, const char *name)
{
    Session *s = session_find(id);
    if (!s || to_us <= from_us || s->state == SES_SAVING) return;
    s->start_us = from_us;
    s->end_us = to_us;
    if (name && name[0]) {
        const AppInfo *game = sessions_game_named(name);
        str_set(s->exe, game ? game->exe.s : "");
        s->custom = !game;
        if (game) {
            sessions_game_name(game->exe.s, s->name);
        } else {
            str_set(s->name, name);
            clean_name(s->name);
        }
    }
    if (s->state == SES_OPEN) s->state = SES_NOT_SAVED; // set by hand, it no longer follows the game
    if (s->record[0] || s->custom) session_write_record(s, 0);
    g_app.sessions.dirty = true;
    ui_wake();
}

// ---------------------------------------------------------------------------------------------------------------
// The popup: in the window's corner, over every screen. It doesn't take the keyboard (no nav or focus), and
// sessions that end while it's up wait behind it.

static void timeline_review(i64 from_us, i64 to_us, const char *name); // timeline.cpp

static void sessions_popup()
{
    i32 waiting = 0;
    Session *s = sessions_asking(&waiting);
    if (!s) return;
    f32 sc = g_dpi_scale;
    f32 pad = 18 * sc, tile = 48 * sc, button_w = 96 * sc, fh = ImGui::GetFrameHeight();
    // Fixed size: every line is always there, whatever it says, so the buttons never move
    ImVec2 size(400 * sc, pad + tile + 16 * sc + fh + pad);
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - size.x - 20 * sc,
                                   vp->WorkPos.y + vp->WorkSize.y - size.y - 20 * sc));
    ImGui::SetNextWindowSize(size);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, rgb(COL_PANEL));
    ImGui::PushStyleColor(ImGuiCol_Border, rgb(COL_LINE));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12 * sc);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));
    ImGui::Begin("##session_popup", 0,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    char len[32], from[32], to[32], when[128];
    fmt_length(s->end_us - s->start_us, len, sizeof len);
    fmt_clock(s->start_us, from, sizeof from);
    fmt_clock(s->end_us, to, sizeof to);
    snprintf(when, sizeof when, "%s  -  %s - %s", len, from, to);

    // The game's icon (a game tile when it has none), and beside it its name over how long and when
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 a = ImGui::GetCursorScreenPos();
    f32 inner_w = size.x - 2 * pad;
    ID3D11ShaderResourceView *icon = game_db_icon(s->name);
    if (!icon && s->exe[0]) icon = app_icon(s->exe);
    if (icon) {
        dl->AddImageRounded((ImTextureID)icon, a, ImVec2(a.x + tile, a.y + tile), ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, 8 * sc);
    } else {
        dl->AddRectFilled(a, ImVec2(a.x + tile, a.y + tile), col32(COL_ACCENT_SOFT), 10 * sc);
        ImGui::PushFont(g_font_icon);
        ImVec2 is = ImGui::CalcTextSize(ICON_GAME);
        dl->AddText(ImVec2(floorf(a.x + (tile - is.x) / 2), floorf(a.y + (tile - is.y) / 2)), col32(COL_ACCENT), ICON_GAME);
        ImGui::PopFont();
    }
    f32 tx = a.x + tile + 14 * sc, nh = g_font_big->FontSize, wh = g_font->FontSize;
    f32 ty = floorf(a.y + (tile - nh - 5 * sc - wh) / 2);
    dl->PushClipRect(ImVec2(tx, a.y), ImVec2(a.x + inner_w, a.y + tile), true);
    dl->AddText(g_font_big, nh, ImVec2(tx, ty), col32(COL_TEXT), s->name);
    dl->AddText(g_font, wh, ImVec2(tx, ty + nh + 5 * sc), col32(COL_DIM), when);
    dl->PopClipRect();

    // How many more are waiting, in the corner (the place is kept when there are none)
    if (waiting > 0) {
        char more[32];
        snprintf(more, sizeof more, "+%d waiting", waiting);
        f32 mw = g_font_small->CalcTextSizeA(g_font_small->FontSize, FLT_MAX, 0, more).x;
        dl->AddText(g_font_small, g_font_small->FontSize, ImVec2(a.x + inner_w - mw, a.y), col32(COL_FAINT), more);
    }

    // Under them: Review opens it on the timeline, to look before answering; the answer is on the right
    f32 by = a.y + tile + 16 * sc;
    u32 id = s->id;
    ImGui::SetCursorScreenPos(ImVec2(a.x, by));
    bool review = kit_button("Review", KIT_SECONDARY, ImVec2(button_w, 0));
    ImGui::SetCursorScreenPos(ImVec2(a.x + inner_w - 2 * button_w - 8 * sc, by));
    bool later = kit_button("Not now", KIT_GHOST, ImVec2(button_w, 0));
    ImGui::SameLine(0, 8 * sc);
    bool save = kit_button("Save", KIT_PRIMARY, ImVec2(button_w, 0));
    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);

    Session *cur = session_find(id);
    if (review && cur) timeline_review(cur->start_us, cur->end_us, cur->name);
    if (save && cur) session_save(cur);
    if (later && cur) {
        cur->state = SES_NOT_SAVED;
        g_app.sessions.dirty = true;
        log_line("session %u (%s): not now; unsaved", cur->id, cur->name);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Unsaved sessions, shown as a row at the top of the Library's Sessions. Its rows are taken when it comes on
// screen (or a new session joins), so Save and Dismiss change a tile in place instead of closing up the row
// under the pointer.

static bool session_listed(const Session &s)
{
    return s.state == SES_ASKING || s.state == SES_NOT_SAVED || s.state == SES_SAVING;
}

static void sessions_rows_rebuild()
{
    Sessions &ss = g_app.sessions;
    ss.row_count = 0;
    for (i32 i = 0; i < COUNT(ss.items); i++)
        if (session_listed(ss.items[i])) ss.rows[ss.row_count++] = ss.items[i].id;
    // Newest first
    for (i32 i = 1; i < ss.row_count; i++) {
        u32 id = ss.rows[i];
        i64 end = session_find(id)->end_us;
        i32 j = i;
        while (j > 0 && session_find(ss.rows[j - 1])->end_us < end) {
            ss.rows[j] = ss.rows[j - 1];
            j--;
        }
        ss.rows[j] = id;
    }
}

// How long a session stays whole in the buffer, or how long the rest stays once its start has gone.
static void session_time_left(const Session &s, char *out, i32 cap)
{
    i64 now = now_us(), oldest = g_app.recorder.oldest_us;
    // By length the buffer keeps buffer_hours; by size, about as long as it holds now.
    i64 keep = (i64)((g_app.config.buffer_hours > 0 ? g_app.config.buffer_hours : 24) * 3600e6);
    if (g_app.config.buffer_by_size) keep = oldest ? now - oldest : keep;
    bool start_gone = oldest && s.start_us < oldest;
    i64 us = (start_gone ? s.end_us : s.start_us) + keep - now;
    char left[32];
    if (us < 60000000) str_set(left, "under a minute");
    else fmt_length(us, left, sizeof left);
    if (start_gone) snprintf(out, cap, "Partly gone, %s left", left);
    else snprintf(out, cap, "%s left", left);
}

// Takes the rows again when the list comes on screen or a session joins it. The Library draws them.
static void sessions_unsaved_take()
{
    Sessions &ss = g_app.sessions;
    i32 frame = ImGui::GetFrameCount();
    bool rebuild = ss.drawn_frame != frame - 1;
    for (i32 i = 0; i < COUNT(ss.items) && !rebuild; i++) {
        if (!session_listed(ss.items[i])) continue;
        bool in = false;
        for (i32 r = 0; r < ss.row_count && !in; r++) in = ss.rows[r] == ss.items[i].id;
        rebuild = !in;
    }
    ss.drawn_frame = frame;
    if (rebuild) sessions_rows_rebuild();
}
