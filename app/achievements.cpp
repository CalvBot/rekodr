// Markers for rare Steam achievements (ach_marks, on by default; in Options > Auto markers, under its switch).
//
// Steam keeps each player's stats for a game in <Steam>\appcache\stats\UserGameStats_<account>_<app>.bin and
// rewrites the file when an achievement is earned; the achievement's unlock time is in it. A worker thread looks
// at those files' times every 2 s, reads one that changed and finds achievements unlocked since it was last read
// (and in the last 10 minutes). The achievement's name comes from UserGameStatsSchema_<app>.bin beside it, and how
// rare it is from Steam's public GetGlobalAchievementPercentagesForApp (no key, no account). One rarer than
// ach_rarer percent (unknown counts as rare) gets a yellow marker at its unlock, its note the achievement's name.
// Steam is never signed in to or written to.
//
// Both files are binary KeyValues: a type byte, a name ending in 0, then the value; type 0 opens a subtree and 8
// closes one.
//   UserGameStats:       cache / <stat> / AchievementTimes / <bit> = unix time
//   UserGameStatsSchema: <app> / stats / <stat> / bits / <bit> / name (the API name), display / name [/ english]

static const i64 ACH_RECENT_US = 10ll * 60 * 1000000; // an unlock older than this when seen is left alone

// Calls fn for each value: its path of names (path[0..depth-1], the last being the value's own name), its type
// and the value (s for a string, v for a number). False if the data isn't binary KeyValues.
typedef void KvFn(void *ctx, const char *const *path, i32 depth, const char *s, i64 v);
static bool kv_walk(const u8 *p, const u8 *end, KvFn *fn, void *ctx)
{
    const char *path[17];
    i32 depth = 0;
    while (p < end) {
        u8 t = *p++;
        if (t == 8 || t == 11) {
            if (depth == 0) return true;
            depth--;
            continue;
        }
        const u8 *z = (const u8 *)memchr(p, 0, (size_t)(end - p));
        if (!z || depth >= 16) return false;
        path[depth] = (const char *)p;
        p = z + 1;
        if (t == 0) {
            depth++;
            continue;
        }
        const char *s = 0;
        i64 v = 0;
        switch (t) {
        case 1:
            z = (const u8 *)memchr(p, 0, (size_t)(end - p));
            if (!z) return false;
            s = (const char *)p;
            p = z + 1;
            break;
        case 2: case 4: case 6: {
            if (end - p < 4) return false;
            i32 x;
            memcpy(&x, p, 4);
            v = x;
            p += 4;
            break;
        }
        case 3: {
            if (end - p < 4) return false;
            f32 x;
            memcpy(&x, p, 4);
            v = (i64)x;
            p += 4;
            break;
        }
        case 7: case 10:
            if (end - p < 8) return false;
            memcpy(&v, p, 8);
            p += 8;
            break;
        default: return false;
        }
        fn(ctx, path, depth + 1, s, v);
    }
    return depth == 0;
}

struct AchUnlock {
    i32 stat, bit;
    i64 time; // unix seconds
};

struct AchTimes {
    i64 after;          // unlocks after this count
    i64 newest;         // the newest unlock in the file
    AchUnlock found[16];
    i32 count;
};

static void ach_times_fn(void *ctx, const char *const *path, i32 depth, const char *s, i64 v)
{
    AchTimes &a = *(AchTimes *)ctx;
    if (depth != 4 || s || strcmp(path[0], "cache") || strcmp(path[2], "AchievementTimes") || v <= 0) return;
    if (v > a.newest) a.newest = v;
    if (v > a.after && a.count < COUNT(a.found)) a.found[a.count++] = {atoi(path[1]), atoi(path[3]), v};
}

// The unlocks in a UserGameStats file later than `after`. Sets *newest to the newest unlock in it.
static i32 ach_unlocks(const u8 *data, i64 len, i64 after, AchUnlock *out, i32 cap, i64 *newest)
{
    static AchTimes a;
    a = {};
    a.after = after;
    kv_walk(data, data + len, ach_times_fn, &a);
    i32 n = a.count < cap ? a.count : cap;
    memcpy(out, a.found, n * sizeof *out);
    *newest = a.newest;
    return n;
}

struct AchNames {
    const char *stat, *bit;
    char api[128], shown[128], english[128], icon[128];
};

static void ach_names_fn(void *ctx, const char *const *path, i32 depth, const char *s, i64)
{
    AchNames &a = *(AchNames *)ctx;
    if (!s || depth < 6 || strcmp(path[1], "stats") || strcmp(path[2], a.stat) || strcmp(path[3], "bits") || strcmp(path[4], a.bit))
        return;
    if (depth == 6 && !strcmp(path[5], "name")) str_set(a.api, s);
    else if (depth == 7 && !strcmp(path[5], "display") && !strcmp(path[6], "name")) str_set(a.shown, s);
    else if (depth == 8 && !strcmp(path[5], "display") && !strcmp(path[6], "name") && !strcmp(path[7], "english")) str_set(a.english, s);
    else if (depth == 7 && !strcmp(path[5], "display") && !strcmp(path[6], "icon")) str_set(a.icon, s);
}

// An achievement's API name (what the percentages list goes by), the name players see and its icon's file name
// (<hash>.jpg, under Steam's community images for the app), from the schema.
static bool ach_names(const u8 *data, i64 len, i32 stat, i32 bit, char *api, i32 api_cap, char *shown, i32 shown_cap,
                      char *icon, i32 icon_cap)
{
    static AchNames a;
    a = {};
    char st[16], bt[16];
    snprintf(st, sizeof st, "%d", stat);
    snprintf(bt, sizeof bt, "%d", bit);
    a.stat = st;
    a.bit = bt;
    kv_walk(data, data + len, ach_names_fn, &a);
    snprintf(api, api_cap, "%s", a.api);
    snprintf(shown, shown_cap, "%s", a.english[0] ? a.english : a.shown[0] ? a.shown : a.api);
    snprintf(icon, icon_cap, "%s", a.icon);
    return a.api[0] != 0;
}

// The percent of players who have the achievement, from GetGlobalAchievementPercentagesForApp's answer; -1 if it
// isn't in it.
static f32 ach_percent(const char *body, i64 len, const char *api)
{
    Json j = {body, body + len};
    if (!j_find(&j, "achievementpercentages.achievements") || !j_eat(&j, '[')) return -1;
    while (j.p < j.end && !j_eat(&j, ']')) {
        j_ws(&j);
        const char *a = j.p;
        j_skip(&j);
        if (j.p == a) break;
        char name[128], pct[32];
        if (j_get(a, j.p - a, "name", name, sizeof name) && !strcmp(name, api) && j_get(a, j.p - a, "percent", pct, sizeof pct))
            return (f32)atof(pct);
        j_eat(&j, ',');
    }
    return -1;
}

// ---------------------------------------------------------------------------------------------------------------

struct AchFile {
    u32 app, account;
    i64 mtime;
    i64 newest; // the newest unlock when last read
};

static struct {
    HANDLE thread, wake;
    volatile bool stop, on;
    volatile i32 rarer; // Config.ach_rarer, copied by achievements_apply for the worker
    AchFile files[512]; // worker only
    i32 file_count;
    bool seen;          // the files were looked at once (what's in them then is old)
} g_ach;

static bool ach_read(const wchar_t *path, u8 *buf, i64 cap, i64 *len)
{
    FILE *f = _wfopen(path, L"rb");
    if (!f) return false;
    *len = (i64)fread(buf, 1, (size_t)cap, f);
    fclose(f);
    return *len > 0 && *len < cap;
}

// A game's name from its appmanifest. "" if it isn't found.
static void ach_game_name(u32 app, char *out, i32 cap)
{
    out[0] = 0;
    Path lib;
    if (!steam_app_library(app, &lib)) return;
    wchar_t acf[MAX_PATH];
    swprintf(acf, MAX_PATH, L"%s\\appmanifest_%u.acf", lib.s, app);
    static u8 text[64 * 1024];
    i64 len = 0;
    if (!ach_read(acf, text, sizeof text - 1, &len)) return;
    text[len] = 0;
    const char *k = strstr((const char *)text, "\"name\"");
    if (!k || !(k = strchr(k + 6, '"'))) return;
    i32 n = 0;
    for (k++; *k && *k != '"' && n < cap - 1; k++) out[n++] = *k;
    out[n] = 0;
}

// A marker's detail for an achievement: achievement (API name), name, icon, app and player (the SteamID64 of the
// account whose stats file it was).
static void ach_detail(char (&d)[256], u32 app, u32 account, const char *api, const char *shown, const char *icon)
{
    char num[24];
    d[0] = 0;
    marker_detail_add(d, "achievement", api);
    marker_detail_add(d, "name", shown);
    marker_detail_add(d, "icon", icon);
    snprintf(num, sizeof num, "%u", app);
    marker_detail_add(d, "app", num);
    snprintf(num, sizeof num, "%llu", 76561197960265728ull + account);
    marker_detail_add(d, "player", num);
}

// What to do about one unlock: its names, its rarity, and a marker if it's rare enough, its detail from ach_detail.
static void ach_found(const Path &dir, u32 app, u32 account, const AchUnlock &u)
{
    static u8 schema[4 << 20];
    i64 len = 0;
    char api[128] = "", shown[128] = "", icon[128] = "";
    wchar_t sp[MAX_PATH];
    swprintf(sp, MAX_PATH, L"%s\\UserGameStatsSchema_%u.bin", dir.s, app);
    if (ach_read(sp, schema, sizeof schema, &len)) ach_names(schema, len, u.stat, u.bit, api, sizeof api, shown, sizeof shown, icon, sizeof icon);
    if (!shown[0]) snprintf(shown, sizeof shown, "Achievement");

    f32 pct = -1;
    if (api[0]) {
        wchar_t path[160];
        swprintf(path, COUNT(path), L"/ISteamUserStats/GetGlobalAchievementPercentagesForApp/v0002/?gameid=%u&format=json", app);
        char *body = 0;
        i64 blen = 0;
        if (http_get(L"api.steampowered.com", path, L"", 0, &body, &blen) == 200 && body) pct = ach_percent(body, blen, api);
        free(body);
    }
    i32 rarer = g_ach.rarer;
    bool rare = pct < 0 || rarer >= 100 || pct < rarer;
    char game[128];
    ach_game_name(app, game, sizeof game);
    log_line("achievements: %s%s%s (%s%.1f%% of players)%s", game, game[0] ? ": " : "", shown, pct < 0 ? "unknown, " : "",
             pct < 0 ? 0.0 : pct, rare ? "" : ", not rare enough for a marker");
    if (!rare) return;
    char detail[256];
    ach_detail(detail, app, account, api, shown, icon);
    markers_add(u.time * 1000000, 3, shown, detail);
    event_post(Event{}); // the timeline shows it
}

// Looks at the stats files' times; reads the ones that changed.
static void ach_scan(const Path &dir)
{
    wchar_t pat[MAX_PATH];
    swprintf(pat, MAX_PATH, L"%s\\UserGameStats_*_*.bin", dir.s);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    static u8 data[1 << 20];
    do {
        u32 account = 0, app = 0;
        if (swscanf(fd.cFileName, L"UserGameStats_%u_%u.bin", &account, &app) != 2) continue;
        i64 mtime = filetime_to_us(fd.ftLastWriteTime);
        AchFile *f = 0;
        for (i32 i = 0; i < g_ach.file_count && !f; i++)
            if (g_ach.files[i].app == app && g_ach.files[i].account == account) f = &g_ach.files[i];
        if (f && f->mtime == mtime) continue;
        if (!f) {
            if (g_ach.file_count >= COUNT(g_ach.files)) continue;
            f = &g_ach.files[g_ach.file_count++];
            *f = {app, account, 0, 0};
        }
        wchar_t path[MAX_PATH];
        swprintf(path, MAX_PATH, L"%s\\%s", dir.s, fd.cFileName);
        i64 len = 0;
        if (!ach_read(path, data, sizeof data, &len)) continue; // Steam has it open: read on the next look
        f->mtime = mtime;
        AchUnlock found[16];
        i64 newest = 0;
        i32 n = ach_unlocks(data, len, f->newest, found, COUNT(found), &newest);
        bool first = !g_ach.seen;
        if (newest > f->newest) f->newest = newest;
        if (first) continue; // what was there before Rekodr looked is old
        i64 now = now_us();
        for (i32 i = 0; i < n && !g_ach.stop; i++)
            if (found[i].time * 1000000 >= now - ACH_RECENT_US) ach_found(dir, app, account, found[i]);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    g_ach.seen = true;
}

static DWORD WINAPI ach_thread(void *)
{
    Path root;
    while (!g_ach.stop) {
        if (g_ach.on && steam_root(&root)) {
            Path dir;
            swprintf(dir.s, COUNT(dir.s), L"%s\\appcache\\stats", root.s);
            ach_scan(dir);
        } else {
            g_ach.seen = false;
            g_ach.file_count = 0;
        }
        WaitForSingleObject(g_ach.wake, 2000);
    }
    return 0;
}

// At startup and when the switch changes. Main thread.
static void achievements_apply()
{
    g_ach.on = g_app.config.auto_marks && g_app.config.ach_marks;
    g_ach.rarer = g_app.config.ach_rarer;
    if (g_ach.on && !g_ach.thread) {
        g_ach.wake = CreateEventW(0, FALSE, FALSE, 0);
        g_ach.thread = CreateThread(0, 0, ach_thread, 0, 0, 0);
    }
    if (g_ach.wake) SetEvent(g_ach.wake);
}

static void achievements_stop()
{
    if (!g_ach.thread) return;
    g_ach.stop = true;
    SetEvent(g_ach.wake);
    // A worker still in a web request after 5 s keeps its event: it waits on it again once the request ends
    if (WaitForSingleObject(g_ach.thread, 5000) == WAIT_OBJECT_0) CloseHandle(g_ach.wake);
    CloseHandle(g_ach.thread);
    g_ach.thread = 0;
}
