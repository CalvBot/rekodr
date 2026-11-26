// hf2log.cpp: House Flipper 2's markers, from Player.log in %USERPROFILE%\AppData\LocalLow\Frozen District\House
// Flipper 2, which the game starts afresh at each launch (the last run's goes to Player-prev.log). Its lines have no
// time, so each marker goes in when Rekodr reads the line, once a second; lines there before Rekodr looked make none.
//
//   Achievements  "Steam achievement: <ApiName> unlocked", the name's words split at its capitals.
//   Jobs done     "CaptureAllPhotos for: <folder>\<n-n Name>[@<saved>].house": the recap's photos, taken as a job
//                 is finished.

struct Hf2Seen { char job[96]; }; // the last job done, as its photos may be taken again

// "TheFastAndTheCleanest" as "The Fast And The Cleanest".
static void hf2_words(const char *api, char *out, i32 cap)
{
    i32 n = 0;
    for (const char *p = api; *p && n < cap - 1; p++) {
        if (n && isupper((u8)*p) && islower((u8)p[1]) && out[n - 1] != ' ' && n < cap - 2) out[n++] = ' ';
        else if (n && isupper((u8)*p) && islower((u8)p[-1]) && n < cap - 2) out[n++] = ' ';
        out[n++] = *p;
    }
    out[n] = 0;
}

static i32 hf2_line(Hf2Seen *s, u32 events, const char *line, bool old, AutoMark *out, i32 cap)
{
    i32 n = 0;
    static const char ACH[] = "Steam achievement: ", UNLOCKED[] = " unlocked", PHOTOS[] = "CaptureAllPhotos for: ";
    const char *a = strstr(line, ACH);
    if (a) {
        a += sizeof ACH - 1;
        size_t l = strlen(a), u = sizeof UNLOCKED - 1;
        if (old || !(events & AM_ADVANCEMENT) || l <= u || strcmp(a + l - u, UNLOCKED)) return 0;
        char api[64], note[64], d[256] = "";
        snprintf(api, sizeof api, "%.*s", (int)(l - u), a);
        hf2_words(api, note, sizeof note);
        marker_detail_add(d, "achievement", api);
        am_push(out, cap, &n, 6, note, false, d);
        return n;
    }
    const char *p = strstr(line, PHOTOS);
    if (!p) return 0;
    p += sizeof PHOTOS - 1;
    const char *name = p;
    for (const char *c = p; *c; c++)
        if (*c == '\\' || *c == '/') name = c + 1;
    size_t l = strlen(name);
    if (l <= 6 || _stricmp(name + l - 6, ".house")) return 0;
    l -= 6;
    const char *at = (const char *)memchr(name, '@', l);
    if (at) l = (size_t)(at - name);
    const char *sp = (const char *)memchr(name, ' ', l); // "1-4 " is the job's place in its chapter
    if (sp && sp > name && isdigit((u8)name[0])) l -= (size_t)(sp + 1 - name), name = sp + 1;
    char job[96];
    snprintf(job, sizeof job, "%.*s", (int)l, name);
    if (!job[0] || old || !strcmp(job, s->job)) return 0;
    str_set(s->job, job);
    if (!(events & AM_QUEST)) return 0;
    char d[256] = "";
    marker_detail_add(d, "job", job);
    am_push(out, cap, &n, 3, "Job done", false, d);
    return n;
}

// Player.log's path; "" when the game's folder isn't there.
static void hf2_log_path(wchar_t (&out)[MAX_PATH])
{
    out[0] = 0;
    PWSTR low = 0;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppDataLow, 0, 0, &low))) {
        wchar_t dir[MAX_PATH];
        swprintf(dir, MAX_PATH, L"%s\\Frozen District\\House Flipper 2", low);
        if (GetFileAttributesW(dir) != INVALID_FILE_ATTRIBUTES) swprintf(out, MAX_PATH, L"%s\\Player.log", dir);
    }
    CoTaskMemFree(low);
}

static struct {
    Hf2Seen seen;
    AmTail  tail;
    wchar_t log[MAX_PATH]; // Player.log; "" while off or not installed (main thread sets it)
} g_hf2;

// Once a second from the worker: the lines Player.log has added.
static void hf2_poll()
{
    u32 ev = g_am.events[AMG_HOUSE_FLIPPER_2];
    if (!ev || !g_hf2.log[0]) return;
    AutoMark marks[8];
    am_tail(&g_hf2.tail, g_hf2.log, [&](const char *l, bool old) {
        am_put(marks, hf2_line(&g_hf2.seen, ev, l, old, marks, COUNT(marks)), AMG_HOUSE_FLIPPER_2);
    });
}

// Where Player.log is, and the game's state for Options. From automarks_apply. Main thread.
static void hf2_apply()
{
    wchar_t p[MAX_PATH] = L"";
    if (g_am.events[AMG_HOUSE_FLIPPER_2]) hf2_log_path(p);
    wcscpy_s(g_hf2.log, p); // the worker reads it whole or not at all
    InterlockedExchange(&g_am.state[AMG_HOUSE_FLIPPER_2], !g_am.events[AMG_HOUSE_FLIPPER_2] ? AM_OFF : p[0] ? AM_READY : AM_NOT_FOUND);
}
