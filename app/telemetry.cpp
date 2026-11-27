// Usage stats, feedback and crash reports, to the service art_service names as signed POSTs, like /v1/lookup.
//   /v1/stats     once a day at most, only while "Send usage stats" is on: the version, Windows build, encoder, fps
//                 and height, how many clips, saves, shares, links and slow-PC warnings since the last report that
//                 went through, and per game the service has a key for, the minutes played (to the nearest 15) and
//                 clips made. The counts are kept in <local_dir>\stats.txt so they outlive a restart.
//   /v1/settings  with the same switch: the settings on SETTINGS_SENT's list, as plain values (on/off, a choice, a
//                 whole number), and for the personal ones only whether they differ from the default. Sent once,
//                 then when they change, at most once a week (tel_settings_json).
//   /v1/feedback  what the player writes in Options > General > Feedback: a bug or an idea.
//   /v1/artreport a game whose art looks wrong, from the same card's Game art tab: the game's service key, the
//                 name shown and a short note.
//   /v1/crash     after a crash or hang, the next start sends its report, while "Send crash reports" is on: see
//                 crash_body for what's in it.
// With "Look up games online" off, or no service (art_service empty, or another host with no key), none is sent. The version is REKODR_VERSION (version.h).

struct StatCounts { i32 clips, saves, shares, links, strain; };

// A game's play time and clips since the last report, by the service's key for it.
struct TelGame { char key[48]; i32 secs, clips; };
enum { TEL_GAMES = 64, TEL_GAMES_SENT = 30 };

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
    TelGame games[TEL_GAMES];
    i32     game_count;
    TelGame games_sending[TEL_GAMES_SENT]; // the games the report in flight carries
    i32     games_sending_count;
    char    play_exe[64];  // the game being played as last seen, and the service's key for it ("" = none known)
    char    play_key[48];
    i64     play_key_next; // with no key yet, asked again from then
    // The settings report
    i64  settings_sent;    // unix seconds of the last one that went through, 0 = never
    u64  settings_hash;    // what it carried (fnv64 of its settings)
    i64  settings_next;    // not looked at again before this
    u64  settings_sending;
    volatile LONG settings_busy;
    HANDLE settings_thread;
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
    fprintf(f, "settings_sent = %lld\nsettings_hash = %llu\n", g_tel.settings_sent,
            (unsigned long long)g_tel.settings_hash);
    for (i32 i = 0; i < g_tel.game_count; i++)
        fprintf(f, "game = %s %d %d\n", g_tel.games[i].key, g_tel.games[i].secs, g_tel.games[i].clips);
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
        char key[48];
        long long v;
        i32 secs, clips;
        unsigned long long hash;
        if (sscanf(line, "game = %47s %d %d", key, &secs, &clips) == 3) {
            if (g_tel.game_count < TEL_GAMES) {
                TelGame &g = g_tel.games[g_tel.game_count++];
                g = {};
                str_set(g.key, key);
                g.secs = secs, g.clips = clips;
            }
            continue;
        }
        if (sscanf(line, "settings_hash = %llu", &hash) == 1) {
            g_tel.settings_hash = hash;
            continue;
        }
        if (sscanf(line, "%31s = %lld", key, &v) != 2) continue;
        if (!strcmp(key, "sent")) g_tel.sent = v;
        else if (!strcmp(key, "clips")) c.clips = (i32)v;
        else if (!strcmp(key, "saves")) c.saves = (i32)v;
        else if (!strcmp(key, "shares")) c.shares = (i32)v;
        else if (!strcmp(key, "links")) c.links = (i32)v;
        else if (!strcmp(key, "strain")) c.strain = (i32)v;
        else if (!strcmp(key, "settings_sent")) g_tel.settings_sent = v;
    }
    fclose(f);
}

static void tel_count(i32 StatCounts::*field)
{
    if (!g_tel.loaded) tel_load();
    g_tel.counts.*field += 1;
    tel_save();
}

// Whether play time and clips per game are counted: "Send usage stats" on, with a service to send them to.
static bool tel_games_on()
{
    const Config &c = g_app.config;
    return c.send_stats && c.game_lookup && g_gamedb.service[0];
}

// The game with the service key `key` in the counts, added when it isn't there; 0 when they're full.
static TelGame *tel_game(const char *key)
{
    for (i32 i = 0; i < g_tel.game_count; i++)
        if (!strcmp(g_tel.games[i].key, key)) return &g_tel.games[i];
    if (g_tel.game_count >= TEL_GAMES) return 0;
    TelGame *g = &g_tel.games[g_tel.game_count++];
    *g = {};
    str_set(g->key, key);
    return g;
}

// A save that finished: a clip, a session or a Keep, or a share copy.
static void telemetry_on_save(const Event &e)
{
    if (e.a != SAVE_EV_DONE) return;
    const SaveJobView *v = save_view((u32)(e.b & 0xffffffff));
    if (!v) return;
    if (v->kind == SAVE_CLIP) {
        char key[48];
        TelGame *g = 0;
        if (tel_games_on() && v->game[0] && strcmp(v->game, GAME_DESKTOP) && gamedb_key_for(v->game, key, sizeof key))
            g = tel_game(key);
        if (g) g->clips++;
        tel_count(&StatCounts::clips);
    }
    else if (v->kind == SAVE_SESSION || v->kind == SAVE_KEEP) tel_count(&StatCounts::saves);
    else if (v->kind == SAVE_SHARE) tel_count(&StatCounts::shares);
}

static void telemetry_shared() { tel_count(&StatCounts::shares); } // a Share done: a file or a link

// A signed POST of json to the service art_service names (gamedb_set_service). Returns the status (0 = no answer,
// or no service to send to).
static u32 tel_post(const char *path, const char *json)
{
    if (!g_gamedb.service[0]) return 0;
    wchar_t host[128], wpath[64], header[320];
    utf8_to_wide(g_gamedb.service, host, COUNT(host));
    utf8_to_wide(path, wpath, COUNT(wpath));
    char sig[128];
    art_service_sign(g_gamedb.service_key, path, tel_unix_now(), sig, sizeof sig, json);
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

// Minutes played to the nearest 15.
static i32 tel_minutes(i32 secs) { return (secs + 450) / 900 * 15; }

// The games a report carries, into out (at most TEL_GAMES_SENT): the most played of those with any rounded minutes
// or clips. The rest wait for a later one. Returns how many.
static i32 tel_games_pick(const TelGame *games, i32 n, TelGame *out)
{
    i32 k = 0;
    for (i32 i = 0; i < n; i++) {
        if (!tel_minutes(games[i].secs) && !games[i].clips) continue;
        i32 at = k < TEL_GAMES_SENT ? k++ : TEL_GAMES_SENT;
        while (at > 0 && out[at - 1].secs < games[i].secs) {
            if (at < TEL_GAMES_SENT) out[at] = out[at - 1];
            at--;
        }
        if (at < TEL_GAMES_SENT) out[at] = games[i];
    }
    return k;
}

// The report's JSON, with the counts in c and the games in games.
static void tel_stats_json(const StatCounts &c, const TelGame *games, i32 game_count, char *out, i32 cap)
{
    const Config &cfg = g_app.config;
    char win[32], enc[80];
    tel_windows_build(win, sizeof win);
    json_escape(g_app.recorder.encoder_used.s[0] ? g_app.recorder.encoder_used.s : cfg.encoder.s, enc, sizeof enc);
    i32 k = snprintf(out, cap,
                     "{\"version\":\"%s\",\"windows\":\"%s\",\"encoder\":\"%s\",\"fps\":%d,\"height\":%d,\"clips\":%d,"
                     "\"saves\":%d,\"shares\":%d,\"links\":%d,\"strain\":%d,\"games\":[",
                     REKODR_VERSION, win, enc, (int)cfg.fps, (int)cfg.height, c.clips, c.saves, c.shares, c.links, c.strain);
    for (i32 i = 0; i < game_count && k < cap; i++) {
        char key[100];
        json_escape(games[i].key, key, sizeof key);
        k += snprintf(out + k, cap - k, "%s{\"key\":\"%s\",\"minutes\":%d,\"clips\":%d}", i ? "," : "", key,
                      tel_minutes(games[i].secs), games[i].clips);
    }
    if (k < cap) snprintf(out + k, cap - k, "]}");
}

// The settings report's "settings" object, from c, with d the defaults: SETTINGS_SENT's names only, each a plain
// value. Paths, keys, hosts, names and anything typed are never in it; for those only whether they differ from d.
static void tel_settings_json(const Config &c, const Config &d, char *out, i32 cap)
{
    i32 k = snprintf(out, cap, "{");
    auto put = [&](const char *name, const char *value) {
        if (k < cap) k += snprintf(out + k, cap - k, "%s\"%s\":%s", k > 1 ? "," : "", name, value);
    };
    auto flag = [&](const char *name, bool v) { put(name, v ? "true" : "false"); };
    auto num = [&](const char *name, f64 v) {
        char s[32];
        snprintf(s, sizeof s, "%lld", (long long)floor(v + 0.5));
        put(name, s);
    };
    // A choice is sent only when it is one of the list's, so nothing typed into settings.txt goes
    auto choice = [&](const char *name, const char *v, const char *list) { // list: "a|b|c"
        size_t n = strlen(v);
        for (const char *one = list; n && *one;) {
            const char *e = strchr(one, '|');
            size_t len = e ? (size_t)(e - one) : strlen(one);
            if (len == n && !strncmp(one, v, n)) {
                char s[40];
                snprintf(s, sizeof s, "\"%s\"", v);
                put(name, s);
                return;
            }
            one = e ? e + 1 : one + len;
        }
    };
    flag("draw_mouse", c.draw_mouse);
    flag("window_capture", c.window_capture);
    flag("spatial_aq", c.spatial_aq);
    flag("mic", c.mic);
    flag("buffer_by_size", c.buffer_by_size);
    flag("cam_on", c.cam_on);
    flag("mic_raw", c.mic_raw);
    flag("voice_mono", c.voice_mono);
    flag("share_names", c.share_names);
    flag("steam_type_check", c.steam_type_check);
    flag("send_crashes", c.send_crashes);
    flag("auto_update", c.auto_update);
    flag("render_edits", c.render_edits);
    flag("share_kbps_custom", c.share_kbps_custom);
    flag("share_link_page", c.share_link_page);
    flag("share_copy_on_close", c.share_copy_on_close);
    flag("auto_marks", c.auto_marks);
    flag("ach_marks", c.ach_marks);
    flag("backup_on", c.backup_on);
    flag("backup_sessions", c.backup_sessions);
    flag("tooltips", c.tooltips);
    flag("flat_look", c.flat_look);
    flag("front_window", c.front_window);
    flag("input_busy", c.input_busy);
    flag("timeline_simple", c.timeline_simple);
    flag("clock_24h", c.clock_24h);
    choice("codec", c.codec.s, "h264|hevc|av1");
    choice("encoder", c.encoder.s, "auto|nvenc|amf|qsv|cpu");
    choice("capture", c.capture.s, "auto|wgc|dxgi");
    choice("preset", c.preset.s, "p1|p2|p3|p4|p5|p6|p7");
    choice("rate_control", c.rate_control.s, "vbr|cbr|cq");
    choice("toast_clip", toast_clip_name(c.toast_clip), "full|small|off");
    choice("popup_corner", corner_name(c.popup_corner), "br|bl|tr|tl");
    choice("editor_zoom_view", c.editor_zoom_view ? "footage" : "crop", "crop|footage");
    num("fps", c.fps);
    num("height", c.height);
    num("bitrate_mbps", c.bitrate_mbps);
    num("desktop_bitrate_mbps", c.desktop_bitrate_mbps);
    num("cq", c.cq);
    num("buffer_hours", c.buffer_hours);
    num("buffer_gb", c.buffer_gb);
    num("min_free_gb", c.min_free_gb);
    num("session_gap_min", c.session_gap_min);
    num("session_min_length_min", c.session_min_length_min);
    num("share_max_mb", c.share_max_mb);
    num("social_copy_mb", c.social_copy_mb);
    num("share_min_quality", c.share_min_quality);
    num("share_link_days", c.share_link_days);
    num("ach_rarer", c.ach_rarer);
    num("folder_warn_gb", c.folder_warn_gb);
    num("share_keep_days", c.share_keep_days);
    num("wave_keep_days", c.wave_keep_days);
    num("card_width", c.card_width);
    num("mics_added", c.input_count);
    num("hotkeys", c.hotkey_count);
    num("watch_folders", c.watch_count);
    bool watch = c.watch_count != d.watch_count;
    for (i32 i = 0; i < c.watch_count && !watch; i++) watch = _wcsicmp(c.watch_dirs[i].s, d.watch_dirs[i].s) != 0;
    bool hotkey = c.hotkey_count != d.hotkey_count;
    for (i32 i = 0; i < c.hotkey_count && !hotkey; i++) {
        const Hotkey &a = c.hotkeys[i], &b = d.hotkeys[i];
        hotkey = strcmp(a.keys.s, b.keys.s) || strcmp(a.action.s, b.action.s) || strcmp(a.hold.s, b.hold.s) ||
                 strcmp(a.game.s, b.game.s);
    }
    bool sounds = false;
    for (i32 i = 0; i < SOUND_EVENT_COUNT && !sounds; i++)
        sounds = c.sounds[i].on != d.sounds[i].on || c.sounds[i].sound != d.sounds[i].sound ||
                 wcscmp(c.sounds[i].own.s, d.sounds[i].own.s);
    flag("save_dir_changed", _wcsicmp(c.save_dir.s, d.save_dir.s) != 0);
    flag("buffer_dir_changed", _wcsicmp(c.buffer_dir.s, d.buffer_dir.s) != 0);
    flag("play_dir_changed", _wcsicmp(c.play_dir.s, d.play_dir.s) != 0);
    flag("backup_dir_set", c.backup_dir.s[0] != 0);
    flag("discord_dir_changed", _wcsicmp(c.discord_dir.s, d.discord_dir.s) != 0);
    flag("watch_dirs_changed", watch);
    flag("ffmpeg_changed", c.ffmpeg.s[0] != 0);
    flag("hotkey_changed", hotkey);
    flag("name_template_changed", strcmp(c.name_template, d.name_template) != 0);
    flag("accent_changed", c.accent != d.accent);
    flag("sounds_changed", sounds);
    flag("popup_monitor_changed", c.popup_monitor != d.popup_monitor || c.popup_monitor_game != d.popup_monitor_game);
    flag("encoder_args_set", c.encoder_args[0] != 0);
    flag("art_service_changed", strcmp(c.art_service.s, d.art_service.s) != 0);
    flag("sgdb_key_set", c.sgdb_key[0] != 0);
    flag("r2_set", c.r2_account[0] != 0);
    flag("uploaders_set", c.link_uploaders[0] != 0);
    if (k < cap) snprintf(out + k, cap - k, "}");
}

static DWORD WINAPI tel_settings_thread(void *arg)
{
    char *json = (char *)arg;
    u32 status = tel_post("/v1/settings", json);
    log_line("stats: settings sent, the service answered %u", status);
    free(json);
    InterlockedExchange(&g_tel.settings_busy, status == 200 ? 2 : 3);
    return 0;
}

static const i64 SETTINGS_EVERY_S = 7 * 24 * 3600;

// Once a second from telemetry_tick: sends the settings when they haven't gone yet, or changed and a week has
// passed since they last went.
static void tel_settings_tick(i64 now)
{
    LONG busy = g_tel.settings_busy;
    if (busy >= 2) {
        WaitForSingleObject(g_tel.settings_thread, INFINITE);
        CloseHandle(g_tel.settings_thread);
        g_tel.settings_thread = 0;
        if (busy == 2) {
            g_tel.settings_sent = now;
            g_tel.settings_hash = g_tel.settings_sending;
            tel_save();
        }
        g_tel.settings_next = now + 3600;
        InterlockedExchange(&g_tel.settings_busy, 0);
        return;
    }
    if (busy || !tel_games_on() || now < g_tel.settings_next) return;
    if (g_tel.settings_sent && now - g_tel.settings_sent < SETTINGS_EVERY_S) return;
    g_tel.settings_next = now + 3600;
    static Config defaults;
    config_defaults(&defaults, g_app.paths);
    static char settings[8192];
    tel_settings_json(g_app.config, defaults, settings, sizeof settings);
    u64 hash = fnv64(settings, strlen(settings));
    if (g_tel.settings_sent && hash == g_tel.settings_hash) return;
    i32 cap = (i32)strlen(settings) + 64;
    char *json = (char *)malloc(cap);
    snprintf(json, cap, "{\"version\":\"%s\",\"settings\":%s}", REKODR_VERSION, settings);
    g_tel.settings_sending = hash;
    InterlockedExchange(&g_tel.settings_busy, 1);
    g_tel.settings_thread = thread_start(tel_settings_thread, json);
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
    if (busy || !c.send_crashes || !c.game_lookup || !g_gamedb.service[0] || now < g_crash.next_try || now - g_crash.sent < CRASH_EVERY_S) return;
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
    // A second of the game being played, on the service's key for it
    const char *exe = g_app.audio.game.s;
    if (!exe[0] || !tel_games_on()) g_tel.play_exe[0] = 0;
    else {
        if (strcmp(exe, g_tel.play_exe) || (!g_tel.play_key[0] && now >= g_tel.play_key_next)) {
            char name[128];
            audio_display_name(c, exe, name, sizeof name);
            if (!gamedb_key_for(name, g_tel.play_key, sizeof g_tel.play_key)) g_tel.play_key[0] = 0;
            str_set(g_tel.play_exe, exe);
            g_tel.play_key_next = now + 60; // the lookup may not have answered yet
        }
        TelGame *g = g_tel.play_key[0] ? tel_game(g_tel.play_key) : 0;
        if (g && ++g->secs % 60 == 0) tel_save();
    }
    tel_settings_tick(now);
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
            for (i32 i = 0; i < g_tel.games_sending_count; i++) {
                TelGame *g = tel_game(g_tel.games_sending[i].key);
                if (g) g->secs -= g_tel.games_sending[i].secs, g->clips -= g_tel.games_sending[i].clips;
            }
            i32 kept = 0;
            for (i32 i = 0; i < g_tel.game_count; i++)
                if (g_tel.games[i].secs > 0 || g_tel.games[i].clips > 0) g_tel.games[kept++] = g_tel.games[i];
            g_tel.game_count = kept;
            g_tel.sent = now;
            tel_save();
        } else g_tel.next_try = now + 3600;
        InterlockedExchange(&g_tel.stats_busy, 0);
        return;
    }
    if (busy || !c.send_stats || !c.game_lookup || !g_gamedb.service[0] || now - g_tel.sent < STATS_EVERY_S || now < g_tel.next_try) return;
    g_tel.sending = g_tel.counts;
    g_tel.games_sending_count = tel_games_pick(g_tel.games, g_tel.game_count, g_tel.games_sending);
    char *json = (char *)malloc(4096);
    tel_stats_json(g_tel.sending, g_tel.games_sending, g_tel.games_sending_count, json, 4096);
    InterlockedExchange(&g_tel.stats_busy, 1);
    g_tel.stats_thread = thread_start(tel_stats_thread, json);
}

static const char *const FEEDBACK_KINDS[] = {"bug", "idea"};
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

enum { ART_NOTE_MAX = 120 };

// The art report body: the game's service key and shown name and the note, cut to ART_NOTE_MAX bytes at a
// character's start.
static void tel_art_json(const char *key, const char *name, const char *note, char *out, i32 cap)
{
    char cut[ART_NOTE_MAX + 1], esc_note[ART_NOTE_MAX * 2 + 1], esc_key[160], esc_name[260];
    size_t len = strlen(note), n = len < ART_NOTE_MAX ? len : ART_NOTE_MAX;
    while (n > 0 && n < len && ((u8)note[n] & 0xc0) == 0x80) n--;
    memcpy(cut, note, n);
    cut[n] = 0;
    json_escape(trim(cut), esc_note, sizeof esc_note);
    json_escape(key, esc_key, sizeof esc_key);
    json_escape(name, esc_name, sizeof esc_name);
    snprintf(out, cap, "{\"version\":\"%s\",\"key\":\"%s\",\"name\":\"%s\",\"note\":\"%s\"}", REKODR_VERSION,
             esc_key, esc_name, esc_note);
}

static DWORD WINAPI tel_feedback_thread(void *)
{
    u32 status = tel_post(g_tel.fb_art ? "/v1/artreport" : "/v1/feedback", g_tel.fb_body);
    log_line("%s: sent, the service answered %u", g_tel.fb_art ? "art report" : "feedback", status);
    g_tel.fb_ok = status == 200;
    if (status == 200) str_set(g_tel.fb_result, "Sent. Thank you");
    else if (status == 0) str_set(g_tel.fb_result, "Couldn't reach the service");
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

static bool art_report_send(const char *key, const char *name, const char *note)
{
    if (g_tel.fb_busy || feedback_wait_left(g_tel.art_sent_ms, GetTickCount64()) > 0) return false;
    tel_art_json(key, name, note, g_tel.fb_body, sizeof g_tel.fb_body);
    tel_feedback_start(true);
    return true;
}

static void telemetry_stop()
{
    HANDLE both[] = {g_tel.stats_thread, g_tel.fb_thread, g_tel.settings_thread};
    for (HANDLE h : both)
        if (h) WaitForSingleObject(h, 5000);
}
