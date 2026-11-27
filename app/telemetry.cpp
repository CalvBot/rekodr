// Usage stats and feedback, both to Rekodr's service (clipback-api) as signed POSTs, like /v1/lookup.
//   /v1/stats     once a day at most, only while "Send usage stats" is on: the version, Windows build, encoder, fps
//                 and height, and how many clips, saves, shares, links and slow-PC warnings since the last report
//                 that went through. The counts are kept in <local_dir>\stats.txt so they outlive a restart.
//   /v1/feedback  what the player writes in Options > General > Feedback, with its kind.
//   /v1/artreport a game whose icon, hero or cover looks wrong, from the same card: the game's service key, the
//                 name shown, the kind and a short note.
//   /v1/crash     after a crash or hang, the next start sends its report, while "Send crash reports" is on: see
//                 crash_body for what's in it.
// With "Look up games online" off, none is sent. The version is REKODR_VERSION (version.h).

struct StatCounts { i32 clips, saves, shares, links, strain; };

struct Telemetry {
    StatCounts counts;
    i64 sent;           // unix seconds of the last report that went through (or when counting began)
    i64 next_try;       // after a failure, not before this
    u32 r2_seen;        // the highest finished link job counted
    bool strained;      // recorder.strain was set last tick
    bool loaded;
    volatile LONG stats_busy;
    HANDLE stats_thread;
    StatCounts sending; // the counts the report in flight carries
    // Feedback and art reports, from the Options card: one in flight at a time
    volatile LONG fb_busy;
    HANDLE fb_thread;
    char fb_body[4400];
    char fb_result[160]; // "Sent", or why not; read once fb_busy is 0
    bool fb_ok;
    bool fb_art;     // the last one sent was an art report
    u64  fb_sent_ms; // GetTickCount64 when the last feedback went, for the wait after it (memory only); 0 = none yet
    u64  art_sent_ms; // the same for art reports
};
static Telemetry g_tel;

static const i64 STATS_EVERY_S = 24 * 3600;

static i64 tel_unix_now()
{
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    return (((i64)now.dwHighDateTime << 32 | now.dwLowDateTime) - 116444736000000000ll) / 10000000;
}

static Path tel_file() { return path_join(g_app.paths.local_dir, L"stats.txt"); }

static void tel_save()
{
    Path p = tel_file();
    FILE *f = _wfopen(p.s, L"wb");
    if (!f) return;
    const StatCounts &c = g_tel.counts;
    fprintf(f, "sent = %lld\nclips = %d\nsaves = %d\nshares = %d\nlinks = %d\nstrain = %d\n", g_tel.sent, c.clips,
            c.saves, c.shares, c.links, c.strain);
    fclose(f);
}

static void tel_load()
{
    g_tel.loaded = true;
    g_tel.sent = tel_unix_now();
    Path p = tel_file();
    FILE *f = _wfopen(p.s, L"rb");
    if (!f) return;
    char line[128];
    StatCounts &c = g_tel.counts;
    while (fgets(line, sizeof line, f)) {
        char key[32];
        long long v;
        if (sscanf(line, "%31s = %lld", key, &v) != 2) continue;
        if (!strcmp(key, "sent")) g_tel.sent = v;
        else if (!strcmp(key, "clips")) c.clips = (i32)v;
        else if (!strcmp(key, "saves")) c.saves = (i32)v;
        else if (!strcmp(key, "shares")) c.shares = (i32)v;
        else if (!strcmp(key, "links")) c.links = (i32)v;
        else if (!strcmp(key, "strain")) c.strain = (i32)v;
    }
    fclose(f);
}

static void tel_count(i32 StatCounts::*field)
{
    if (!g_tel.loaded) tel_load();
    g_tel.counts.*field += 1;
    tel_save();
}

// A save that finished: a clip, a session or a Keep, or a share copy.
static void telemetry_on_save(const Event &e)
{
    if (e.a != SAVE_EV_DONE) return;
    const SaveJobView *v = save_view((u32)(e.b & 0xffffffff));
    if (!v) return;
    if (v->kind == SAVE_CLIP) tel_count(&StatCounts::clips);
    else if (v->kind == SAVE_SESSION || v->kind == SAVE_KEEP) tel_count(&StatCounts::saves);
    else if (v->kind == SAVE_SHARE) tel_count(&StatCounts::shares);
}

static void telemetry_shared() { tel_count(&StatCounts::shares); } // a Share done: a file or a link

// A signed POST of json to the service. Returns the status (0 = no answer).
static u32 tel_post(const char *path, const char *json)
{
    wchar_t host[128], wpath[64], header[320];
    utf8_to_wide(ART_SERVICE_HOST, host, COUNT(host));
    utf8_to_wide(path, wpath, COUNT(wpath));
    char sig[128];
    art_service_sign(path, tel_unix_now(), sig, sizeof sig, json);
    swprintf(header, COUNT(header), ART_SERVICE_SIG_HEADER L": %hs\r\ncontent-type: application/json\r\n", sig);
    char *body = 0;
    i64 len = 0;
    u32 status = http_get(host, wpath, header, 0, &body, &len, json);
    if (body) free(body);
    return status;
}

static void tel_windows_build(char *out, i32 cap)
{
    wchar_t w[32] = L"";
    DWORD size = sizeof w;
    RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"CurrentBuildNumber",
                 RRF_RT_REG_SZ, 0, w, &size);
    wide_to_utf8(w, out, cap);
}

// The report's JSON, with the counts in c.
static void tel_stats_json(const StatCounts &c, char *out, i32 cap)
{
    const Config &cfg = g_app.config;
    char win[32], enc[80];
    tel_windows_build(win, sizeof win);
    json_escape(g_app.recorder.encoder_used.s[0] ? g_app.recorder.encoder_used.s : cfg.encoder.s, enc, sizeof enc);
    snprintf(out, cap,
             "{\"version\":\"%s\",\"windows\":\"%s\",\"encoder\":\"%s\",\"fps\":%d,\"height\":%d,\"clips\":%d,"
             "\"saves\":%d,\"shares\":%d,\"links\":%d,\"strain\":%d}",
             REKODR_VERSION, win, enc, (int)cfg.fps, (int)cfg.height, c.clips, c.saves, c.shares, c.links, c.strain);
}

static DWORD WINAPI tel_stats_thread(void *arg)
{
    char *json = (char *)arg;
    u32 status = tel_post("/v1/stats", json);
    log_line("stats: report sent, the service answered %u", status);
    free(json);
    InterlockedExchange(&g_tel.stats_busy, status == 200 ? 2 : 3); // the main thread finishes up
    return 0;
}

// Crash reports: the pairs diag.cpp leaves in <local_dir>\reports, sent to /v1/crash one at a time, the newest
// first, at most one a day, while "Send crash reports" and "Look up games online" are on. The newest CRASH_KEEP wait;
// older ones are deleted.
static const i32 CRASH_KEEP = 3;
static const i64 CRASH_EVERY_S = 24 * 3600;
static const i64 CRASH_DUMP_MAX = 512 * 1024; // a bigger dump isn't sent

struct CrashSend {
    volatile LONG busy; // 1 sending, 2 sent, 3 failed: the main thread finishes up
    HANDLE thread;
    Path dmp, txt;
    i64 sent; // unix seconds of the last one that went through, from crash.txt
    i64 next_try;
    bool loaded;
};
static CrashSend g_crash;

// Log lines from the parts that write the names of clips, folders, links and players, or what the player typed.
// A report leaves them out.
static const char *const CRASH_DROP[] = {"library:", "r2:", "markers:", "marker:", "share", "toast:", "achievements:",
                                         "event ", "imgui:", "feedback", "eftkills", "rust:"};

static bool crash_is_exe(const char *s, i32 n) { return n > 4 && !_strnicmp(s + n - 4, ".exe", 4); }

// One log line made fit to send, into out: its time is dropped; the user's folder (profile, as UTF-8) becomes
// %USERPROFILE%; any other path, a link or anything in quotes but an exe's name becomes <path>, <url> or <text>; a
// run of 15 or more digits (an account ID) becomes <id>, and so does the part of an exe's name after a dash with
// such a run in it. False for a line that isn't sent at all.
static bool crash_scrub_line(const char *in, const char *profile, char *out, i32 cap)
{
    if (in[0] == '[') { // "[2026-11-20 22:45:37.574 t34340] "
        const char *e = strchr(in, ']');
        if (e) in = e + 1 + (e[1] == ' ');
    }
    for (const char *d : CRASH_DROP)
        if (!strncmp(in, d, strlen(d))) return false;
    i32 k = 0, pl = (i32)strlen(profile);
    auto put = [&](const char *s, i32 n) {
        for (i32 i = 0; i < n && k < cap - 1; i++) out[k++] = s[i];
    };
    for (const char *s = in; *s && *s != '\r' && *s != '\n';) {
        if (pl > 2 && !_strnicmp(s, profile, pl)) {
            put("%USERPROFILE%", 13);
            s += pl;
            continue;
        }
        if (!_strnicmp(s, "http://", 7) || !_strnicmp(s, "https://", 8)) {
            put("<url>", 5);
            while (*s && *s != ' ' && *s != '\r' && *s != '\n' && *s != '"' && *s != '\'' && *s != ')') s++;
            continue;
        }
        if (isalpha((u8)s[0]) && s[1] == ':' && s[2] == '\\' && (s == in || !isalnum((u8)s[-1]))) {
            // A path, to the end of the line or what ends it: ", ", " (", a quote or ": "
            const char *e = s + 3;
            while (*e && *e != '\r' && *e != '\n' && *e != '"' && *e != '\'' && *e != ')' &&
                   !((e[0] == ',' || e[0] == ':') && e[1] == ' ') && !(e[0] == ' ' && e[1] == '('))
                e++;
            put("<path>", 6);
            s = e;
            continue;
        }
        if (*s == '"' || *s == '\'') {
            const char *e = strchr(s + 1, *s);
            if (e) {
                if (crash_is_exe(s + 1, (i32)(e - s - 1))) put(s, (i32)(e - s + 1));
                else {
                    put(s, 1);
                    put("<text>", 6);
                    put(e, 1);
                }
                s = e + 1;
                continue;
            }
        }
        if (isdigit((u8)*s) && (s == in || !isalnum((u8)s[-1]))) {
            const char *e = s;
            while (isdigit((u8)*e)) e++;
            if (e - s >= 15) {
                put("<id>", 4);
                s = e;
                continue;
            }
        }
        if (*s == '-' && isalnum((u8)s[1])) { // "discord-name-450827036068544522.exe": the name and ID go
            const char *e = s + 1;
            while (*e && *e != ' ' && *e != '.' && *e != '"' && *e != '\'' && *e != ',' && *e != ')') e++;
            bool long_digits = false;
            for (const char *q = s + 1, *run = 0; q < e; q++) {
                if (isdigit((u8)*q)) {
                    if (!run) run = q;
                    if (q - run + 1 >= 15) long_digits = true;
                } else run = 0;
            }
            if (long_digits) {
                put("-<id>", 5);
                s = e;
                continue;
            }
        }
        put(s, 1);
        s++;
    }
    out[k] = 0;
    return true;
}

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// Standard base64 with padding; out needs 4 * ((n + 2) / 3) + 1 bytes.
static void crash_b64(const u8 *b, i64 n, char *out)
{
    i64 k = 0;
    for (i64 i = 0; i < n; i += 3) {
        u32 v = (u32)b[i] << 16 | (i + 1 < n ? (u32)b[i + 1] << 8 : 0) | (i + 2 < n ? b[i + 2] : 0);
        out[k++] = B64[v >> 18 & 63];
        out[k++] = B64[v >> 12 & 63];
        out[k++] = i + 1 < n ? B64[v >> 6 & 63] : '=';
        out[k++] = i + 2 < n ? B64[v & 63] : '=';
    }
    out[k] = 0;
}

// The body for /v1/crash from a report's .txt (text) and .dmp (dump, n bytes): kind, code, where, uptime, version
// and Windows build as fields, the scrubbed log lines as one string, the dump in base64. malloc'd; 0 if the .txt
// isn't a report.
static char *crash_body(const char *text, const u8 *dump, i64 n, const char *profile, const char *win)
{
    char kind[16] = "", where[96] = "";
    unsigned code = 0;
    long long up = 0;
    const char *log = strstr(text, "\nlog\n");
    if (!log || sscanf(text, "kind %15s", kind) != 1) return 0;
    const char *p;
    if ((p = strstr(text, "\ncode 0x")) && p < log) sscanf(p + 8, "%x", &code);
    if ((p = strstr(text, "\nwhere ")) && p < log) sscanf(p + 7, "%95[^\n]", where); // a hang's is empty
    if ((p = strstr(text, "\nuptime ")) && p < log) sscanf(p + 8, "%lld", &up);
    log += 5;
    i64 cap = 4 * ((n + 2) / 3) + strlen(log) * 2 + 1024;
    char *out = (char *)malloc(cap);
    if (!out) return 0;
    char ek[40], ew[200], ewin[64];
    json_escape(kind, ek, sizeof ek);
    json_escape(where, ew, sizeof ew);
    json_escape(win, ewin, sizeof ewin);
    i64 k = snprintf(out, cap, "{\"version\":\"%s\",\"windows\":\"%s\",\"kind\":\"%s\",\"code\":\"0x%08x\",\"where\":\"%s\","
                     "\"uptime\":%lld,\"log\":\"", REKODR_VERSION, ewin, ek, code, ew, up);
    char line[2200], clean[2400], esc[4800];
    for (const char *s = log; *s;) {
        const char *e = strchr(s, '\n');
        i64 len = e ? e - s : (i64)strlen(s);
        if (len > (i64)sizeof line - 1) len = sizeof line - 1;
        memcpy(line, s, len);
        line[len] = 0;
        s = e ? e + 1 : s + strlen(s);
        if (!line[0] || !crash_scrub_line(line, profile, clean, sizeof clean)) continue;
        strcat_s(clean, "\n");
        json_escape(clean, esc, sizeof esc);
        i64 el = (i64)strlen(esc);
        if (k + el + 64 + 4 * ((n + 2) / 3) >= cap) break;
        memcpy(out + k, esc, el);
        k += el;
    }
    k += snprintf(out + k, cap - k, "\",\"dump\":\"");
    crash_b64(dump, n, out + k);
    k += (i64)strlen(out + k);
    snprintf(out + k, cap - k, "\"}");
    return out;
}

// The reports waiting, newest first, as their paths without the extension; how many (at most cap).
static i32 crash_waiting(Path *out, i32 cap)
{
    Path dir = path_join(g_app.paths.local_dir, L"reports"), pat = path_join(dir, L"*.txt");
    struct Found { Path base; FILETIME t; };
    static Found all[64];
    i32 n = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat.s, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (n >= COUNT(all)) break;
            all[n].base = path_join(dir, fd.cFileName);
            wchar_t *dot = wcsrchr(all[n].base.s, L'.');
            if (dot) *dot = 0;
            all[n].t = fd.ftLastWriteTime;
            n++;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    for (i32 i = 1; i < n; i++) // newest first
        for (i32 j = i; j > 0 && CompareFileTime(&all[j].t, &all[j - 1].t) > 0; j--) {
            Found t = all[j];
            all[j] = all[j - 1];
            all[j - 1] = t;
        }
    for (i32 i = 0; i < n && i < cap; i++) out[i] = all[i].base;
    // Past CRASH_KEEP the older ones go
    for (i32 i = CRASH_KEEP; i < n; i++) {
        Path p = all[i].base;
        wcscat_s(p.s, L".txt");
        DeleteFileW(p.s);
        p = all[i].base;
        wcscat_s(p.s, L".dmp");
        DeleteFileW(p.s);
    }
    return n < cap ? n : cap;
}

static Path crash_file() { return path_join(g_app.paths.local_dir, L"crash.txt"); }

static DWORD WINAPI crash_thread(void *)
{
    i64 tn = 0, dn = 0;
    char *text = read_file(g_crash.txt.s, &tn);
    u8 *dump = (u8 *)read_file(g_crash.dmp.s, &dn);
    char *body = 0;
    if (text && dump && dn <= CRASH_DUMP_MAX) {
        char profile[MAX_PATH * 3] = "", win[32];
        wchar_t wp[MAX_PATH];
        if (GetEnvironmentVariableW(L"USERPROFILE", wp, MAX_PATH)) wide_to_utf8(wp, profile, sizeof profile);
        tel_windows_build(win, sizeof win);
        body = crash_body(text, dump, dn, profile, win);
    }
    u32 status = body ? tel_post("/v1/crash", body) : 0;
    log_line("crash report: %s, the service answered %u", body ? "sent" : "not readable", status);
    free(text);
    free(dump);
    free(body);
    bool gone = status == 200 || !body; // sent, or never sendable
    if (gone) {
        DeleteFileW(g_crash.txt.s);
        DeleteFileW(g_crash.dmp.s);
    }
    InterlockedExchange(&g_crash.busy, status == 200 ? 2 : 3);
    return 0;
}

// Once a second with telemetry_tick: sends the newest report when one's waiting and the day's one hasn't gone.
static void crash_tick(i64 now)
{
    const Config &c = g_app.config;
    if (!g_crash.loaded) {
        g_crash.loaded = true;
        if (FILE *f = _wfopen(crash_file().s, L"rb")) {
            long long v = 0;
            if (fscanf(f, "sent = %lld", &v) == 1) g_crash.sent = v;
            fclose(f);
        }
        g_crash.next_try = now + 60; // not in the busy first minute
    }
    LONG busy = g_crash.busy;
    if (busy >= 2) {
        WaitForSingleObject(g_crash.thread, INFINITE);
        CloseHandle(g_crash.thread);
        g_crash.thread = 0;
        if (busy == 2) {
            g_crash.sent = now;
            if (FILE *f = _wfopen(crash_file().s, L"wb")) {
                fprintf(f, "sent = %lld\n", (long long)now);
                fclose(f);
            }
        } else g_crash.next_try = now + 3600;
        InterlockedExchange(&g_crash.busy, 0);
        return;
    }
    if (busy || !c.send_crashes || !c.game_lookup || now < g_crash.next_try || now - g_crash.sent < CRASH_EVERY_S) return;
    g_crash.next_try = now + 600; // look again in a while
    Path base;
    if (!crash_waiting(&base, 1)) return;
    g_crash.txt = g_crash.dmp = base;
    wcscat_s(g_crash.txt.s, L".txt");
    wcscat_s(g_crash.dmp.s, L".dmp");
    InterlockedExchange(&g_crash.busy, 1);
    g_crash.thread = thread_start(crash_thread, 0);
}

// Once a second on the main thread: counts links and slow-PC warnings, and sends the day's report when one is due.
static void telemetry_tick()
{
    const Config &c = g_app.config;
    if (!g_tel.loaded) tel_load();
    crash_tick(tel_unix_now());

    bool strained = g_app.recorder.strain.s[0] != 0;
    if (strained && !g_tel.strained) tel_count(&StatCounts::strain);
    g_tel.strained = strained;

    u32 top = g_tel.r2_seen;
    i32 links = 0;
    AcquireSRWLockShared(&g_r2.lock);
    for (i32 i = 0; i < COUNT(g_r2.jobs); i++) {
        const R2Job &j = g_r2.jobs[i];
        if (j.id > g_tel.r2_seen && j.state == R2_DONE) {
            links++;
            if (j.id > top) top = j.id;
        }
    }
    ReleaseSRWLockShared(&g_r2.lock);
    g_tel.r2_seen = top;
    for (i32 i = 0; i < links; i++) tel_count(&StatCounts::links);

    i64 now = tel_unix_now();
    LONG busy = g_tel.stats_busy;
    if (busy >= 2) { // a report came back
        if (g_tel.stats_thread) {
            WaitForSingleObject(g_tel.stats_thread, INFINITE);
            CloseHandle(g_tel.stats_thread);
            g_tel.stats_thread = 0;
        }
        if (busy == 2) { // only what it carried is taken off; counts since keep going
            StatCounts &k = g_tel.counts;
            const StatCounts &s = g_tel.sending;
            k.clips -= s.clips, k.saves -= s.saves, k.shares -= s.shares, k.links -= s.links, k.strain -= s.strain;
            g_tel.sent = now;
            tel_save();
        } else g_tel.next_try = now + 3600;
        InterlockedExchange(&g_tel.stats_busy, 0);
        return;
    }
    if (busy || !c.send_stats || !c.game_lookup || now - g_tel.sent < STATS_EVERY_S || now < g_tel.next_try) return;
    g_tel.sending = g_tel.counts;
    char *json = (char *)malloc(512);
    tel_stats_json(g_tel.sending, json, 512);
    InterlockedExchange(&g_tel.stats_busy, 1);
    g_tel.stats_thread = thread_start(tel_stats_thread, json);
}

static const char *const FEEDBACK_KINDS[] = {"bug", "idea", "other"};
enum { FB_TEXT_MAX = 2000 };

// The feedback body: kind and text, the text cut to FB_TEXT_MAX bytes at a character's start.
static void tel_feedback_json(i32 kind, const char *text, char *out, i32 cap)
{
    char cut[FB_TEXT_MAX + 1], esc[FB_TEXT_MAX * 2 + 1];
    size_t len = strlen(text), n = len < FB_TEXT_MAX ? len : FB_TEXT_MAX;
    while (n > 0 && n < len && ((u8)text[n] & 0xc0) == 0x80) n--; // don't end inside a character
    memcpy(cut, text, n);
    cut[n] = 0;
    json_escape(trim(cut), esc, sizeof esc);
    snprintf(out, cap, "{\"version\":\"%s\",\"kind\":\"%s\",\"text\":\"%s\"}", REKODR_VERSION, FEEDBACK_KINDS[kind], esc);
}

// After a send, Send waits FEEDBACK_WAIT_MS. How much of the wait is left: 1 just sent, 0 = it may send again.
static const u64 FEEDBACK_WAIT_MS = 60000;
static f32 feedback_wait_left(u64 sent_ms, u64 now_ms)
{
    if (!sent_ms || now_ms < sent_ms || now_ms - sent_ms >= FEEDBACK_WAIT_MS) return 0;
    return 1 - (f32)(now_ms - sent_ms) / FEEDBACK_WAIT_MS;
}

static const char *const ART_KINDS[] = {"icon", "hero", "cover"};
enum { ART_NOTE_MAX = 120 };

// The art report body: the game's service key and shown name, the kind of picture and the note, cut to
// ART_NOTE_MAX bytes at a character's start.
static void tel_art_json(const char *key, const char *name, i32 kind, const char *note, char *out, i32 cap)
{
    char cut[ART_NOTE_MAX + 1], esc_note[ART_NOTE_MAX * 2 + 1], esc_key[160], esc_name[260];
    size_t len = strlen(note), n = len < ART_NOTE_MAX ? len : ART_NOTE_MAX;
    while (n > 0 && n < len && ((u8)note[n] & 0xc0) == 0x80) n--;
    memcpy(cut, note, n);
    cut[n] = 0;
    json_escape(trim(cut), esc_note, sizeof esc_note);
    json_escape(key, esc_key, sizeof esc_key);
    json_escape(name, esc_name, sizeof esc_name);
    snprintf(out, cap, "{\"version\":\"%s\",\"key\":\"%s\",\"name\":\"%s\",\"kind\":\"%s\",\"note\":\"%s\"}", REKODR_VERSION,
             esc_key, esc_name, ART_KINDS[kind], esc_note);
}

static DWORD WINAPI tel_feedback_thread(void *)
{
    u32 status = tel_post(g_tel.fb_art ? "/v1/artreport" : "/v1/feedback", g_tel.fb_body);
    log_line("%s: sent, the service answered %u", g_tel.fb_art ? "art report" : "feedback", status);
    g_tel.fb_ok = status == 200;
    if (status == 200) str_set(g_tel.fb_result, "Sent. Thank you");
    else if (status == 0) str_set(g_tel.fb_result, "Couldn't reach " APP_NAME "'s service");
    else if (status == 429) str_set(g_tel.fb_result, "Sent too often. Try again in a minute");
    else snprintf(g_tel.fb_result, sizeof g_tel.fb_result, "The service turned it down (%u)", status);
    InterlockedExchange(&g_tel.fb_busy, 0);
    ui_wake(3);
    return 0;
}

// Starts the send of what fb_body holds, an art report when `art`.
static void tel_feedback_start(bool art)
{
    if (g_tel.fb_thread) {
        WaitForSingleObject(g_tel.fb_thread, INFINITE);
        CloseHandle(g_tel.fb_thread);
    }
    g_tel.fb_art = art;
    g_tel.fb_result[0] = 0;
    (art ? g_tel.art_sent_ms : g_tel.fb_sent_ms) = GetTickCount64();
    InterlockedExchange(&g_tel.fb_busy, 1);
    g_tel.fb_thread = thread_start(tel_feedback_thread, 0);
}

static bool feedback_send(i32 kind, const char *text)
{
    if (g_tel.fb_busy || feedback_wait_left(g_tel.fb_sent_ms, GetTickCount64()) > 0) return false;
    tel_feedback_json(kind, text, g_tel.fb_body, sizeof g_tel.fb_body);
    tel_feedback_start(false);
    return true;
}

static bool art_report_send(const char *key, const char *name, i32 kind, const char *note)
{
    if (g_tel.fb_busy || feedback_wait_left(g_tel.art_sent_ms, GetTickCount64()) > 0) return false;
    tel_art_json(key, name, kind, note, g_tel.fb_body, sizeof g_tel.fb_body);
    tel_feedback_start(true);
    return true;
}

static void telemetry_stop()
{
    HANDLE both[] = {g_tel.stats_thread, g_tel.fb_thread};
    for (HANDLE h : both)
        if (h) WaitForSingleObject(h, 5000);
}
