// Usage stats and feedback, both to clipback's service (clipback-api) as signed POSTs, like /v1/resolve.
//   /v1/stats     once a day at most, only while "Send usage stats" is on: the version, Windows build, encoder, fps
//                 and height, and how many clips, saves, shares, links and slow-PC warnings since the last report
//                 that went through. The counts are kept in <local_dir>\stats.txt so they outlive a restart.
//   /v1/feedback  what the player writes in Options > General > Feedback, with its kind.
// With "Look up games online" off, neither is sent. The version is CLIPBACK_VERSION (version.h).

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
    // Feedback, from the Options box
    volatile LONG fb_busy;
    HANDLE fb_thread;
    char fb_body[4400];
    char fb_result[160]; // "Sent", or why not; read once fb_busy is 0
    bool fb_ok;
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
    swprintf(header, COUNT(header), L"x-clipback: %hs\r\ncontent-type: application/json\r\n", sig);
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
             CLIPBACK_VERSION, win, enc, (int)cfg.fps, (int)cfg.height, c.clips, c.saves, c.shares, c.links, c.strain);
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

// Once a second on the main thread: counts links and slow-PC warnings, and sends the day's report when one is due.
static void telemetry_tick()
{
    const Config &c = g_app.config;
    if (!g_tel.loaded) tel_load();

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
    snprintf(out, cap, "{\"version\":\"%s\",\"kind\":\"%s\",\"text\":\"%s\"}", CLIPBACK_VERSION, FEEDBACK_KINDS[kind], esc);
}

static DWORD WINAPI tel_feedback_thread(void *)
{
    u32 status = tel_post("/v1/feedback", g_tel.fb_body);
    log_line("feedback: sent, the service answered %u", status);
    g_tel.fb_ok = status == 200;
    if (status == 200) str_set(g_tel.fb_result, "Sent. Thank you");
    else if (status == 0) str_set(g_tel.fb_result, "Couldn't reach clipback's service");
    else if (status == 429) str_set(g_tel.fb_result, "Sent too often. Try again in a minute");
    else snprintf(g_tel.fb_result, sizeof g_tel.fb_result, "The service turned it down (%u)", status);
    InterlockedExchange(&g_tel.fb_busy, 0);
    ui_wake(3);
    return 0;
}

static bool feedback_send(i32 kind, const char *text)
{
    if (g_tel.fb_busy) return false;
    if (g_tel.fb_thread) {
        WaitForSingleObject(g_tel.fb_thread, INFINITE);
        CloseHandle(g_tel.fb_thread);
    }
    tel_feedback_json(kind, text, g_tel.fb_body, sizeof g_tel.fb_body);
    g_tel.fb_result[0] = 0;
    InterlockedExchange(&g_tel.fb_busy, 1);
    g_tel.fb_thread = thread_start(tel_feedback_thread, 0);
    return true;
}

static void telemetry_stop()
{
    HANDLE both[] = {g_tel.stats_thread, g_tel.fb_thread};
    for (HANDLE h : both)
        if (h) WaitForSingleObject(h, 5000);
}
