// sclog.cpp: Star Citizen's markers, from Game.log in the running game's channel folder (LIVE, PTU and the rest), the
// parent of the Bin64 folder StarCitizen.exe runs from. The game starts the file afresh at each launch, each line
// "<UTC time> [Notice] <tag> ...", and only lines added while Rekodr reads it make markers.
//
//   Player   "<Legacy login response> [CIG-net] User Login Success - Handle[<name>]", or the character's
//            "<AccountLoginCharacterStatus_Character> ... name <name> - state STATE_CURRENT".
//   Kills    "<Actor Death> CActor::Kill: '<victim>' [id] in zone '<zone>' killed by '<killer>' [id] using
//   Deaths   '<weapon>' [Class <class>] with damage type '<type>' ...". The game logs only deaths the player is
//            in: the player as killer is a kill, as victim a death. The other side, weapon, damage type and zone go
//            in the marker's detail, which stays on this PC.
//   Downed   "Logged an incap.! nickname: <name>, causes: [<cause> (<n> damage), ...]".
//   Spawns   "[CSessionManager::OnClientSpawned] Spawned!": into the game, and again after each death.
//   HUD      "<SHUDEvent_OnNotification> Added notification "<text>: " [n] to queue. ...": "Contract Accepted:
//            <title>" and "Contract Complete: <title>" (contracts); "Entering Armistice Zone", "Leaving Armistice
//            Zone" and "Entered <name> Jurisdiction" (zones).
//   Travel   "<Quantum Navtarget> ... Local client user <name>[id] received QT data for Entity:... to Target <place>"
//            (a quantum jump, once per target), "<Changing Solar System> ... changing system from <a> to <b>".

struct ScSeen {
    char me[64];          // the player's handle
    char qt_to[64];       // the last quantum jump's target
    i64  qt_at;           // and when (the line's time, seconds)
};

static const i64 SC_QT_AGAIN = 60; // the same target named again this soon is the same jump

// A line's time in seconds since 1601, from its "<YYYY-MM-DDTHH:MM:SS"; 0 when it starts with none.
static i64 sc_time(const char *s)
{
    int y, mo, d, h, mi, se;
    if (sscanf(s, "<%4d-%2d-%2dT%2d:%2d:%2d", &y, &mo, &d, &h, &mi, &se) != 6) return 0;
    SYSTEMTIME st = {(WORD)y, (WORD)mo, 0, (WORD)d, (WORD)h, (WORD)mi, (WORD)se, 0};
    FILETIME ft;
    if (!SystemTimeToFileTime(&st, &ft)) return 0;
    return filetime_to_us(ft) / 1000000;
}

// The text between `from` and `to` after `s`, into out; false when either isn't there.
static bool sc_between(const char *s, const char *from, const char *to, char *out, i32 cap)
{
    const char *a = strstr(s, from);
    if (!a) return false;
    a += strlen(from);
    const char *b = strstr(a, to);
    if (!b) return false;
    snprintf(out, cap, "%.*s", (int)(b - a), a);
    return true;
}

// An entity's name without the id the game puts on its end ("_123456789012"), as "PU_Human-...-Grunt_10".
static void sc_bare(char *s)
{
    size_t l = strlen(s), d = l;
    while (d && isdigit((u8)s[d - 1])) d--;
    if (l - d >= 9 && d > 1 && s[d - 1] == '_') s[d - 1] = 0;
}

// HUD text as shown: <EMn> tags out, runs of spaces made one, the ": " the game puts on its end and spaces around it
// taken off.
static void sc_hud_text(const char *in, char *out, i32 cap)
{
    i32 n = 0;
    for (const char *p = in; *p && n < cap - 1; p++) {
        if (*p == '<') {
            const char *e = strchr(p, '>');
            if (e && (!strncmp(p, "<EM", 3) || !strncmp(p, "</EM", 4))) {
                p = e;
                continue;
            }
        }
        if (*p == ' ' && (!n || out[n - 1] == ' ')) continue;
        out[n++] = *p;
    }
    while (n && (out[n - 1] == ' ' || out[n - 1] == ':')) n--;
    out[n] = 0;
}

static i32 sc_line(ScSeen *s, u32 events, const char *line, bool old, AutoMark *out, i32 cap)
{
    char a[128], b[128];
    if (sc_between(line, "User Login Success - Handle[", "]", a, sizeof a) && a[0]) {
        str_set(s->me, a);
        return 0;
    }
    if (strstr(line, "<AccountLoginCharacterStatus_Character>") && strstr(line, " - state STATE_CURRENT") &&
        sc_between(line, " - name ", " - state", a, sizeof a) && a[0]) {
        str_set(s->me, a);
        return 0;
    }
    if (!s->me[0] && sc_between(line, "Local client user ", "[", a, sizeof a) && a[0]) str_set(s->me, a);
    if (old) return 0;
    i32 n = 0;
    char d[256] = "";

    if (strstr(line, "<Actor Death> CActor::Kill: '")) {
        char victim[128], zone[128], killer[128], weapon[128], cls[128], type[64];
        if (!s->me[0] || !sc_between(line, "CActor::Kill: '", "'", victim, sizeof victim) ||
            !sc_between(line, "] in zone '", "'", zone, sizeof zone) || !sc_between(line, "' killed by '", "'", killer, sizeof killer))
            return 0;
        if (!sc_between(line, "] using '", "'", weapon, sizeof weapon)) weapon[0] = 0;
        if (!sc_between(line, " [Class ", "]", cls, sizeof cls) || !strcmp(cls, "unknown")) cls[0] = 0;
        if (!sc_between(line, "with damage type '", "'", type, sizeof type)) type[0] = 0;
        sc_bare(victim), sc_bare(zone), sc_bare(killer), sc_bare(weapon);
        bool died = !strcmp(victim, s->me), killed = !strcmp(killer, s->me);
        if (died && (events & AM_DEATH)) {
            if (!killed) marker_detail_add(d, "killer", killer);
            marker_detail_add(d, "weapon", cls[0] ? cls : weapon);
            marker_detail_add(d, "damage", type);
            marker_detail_add(d, "zone", zone);
            am_push(out, cap, &n, AM_DEATH, 1, killed ? "Death: suicide" : "Death", false, d);
        } else if (killed && !died && (events & AM_KILL)) {
            marker_detail_add(d, "target", victim);
            marker_detail_add(d, "weapon", cls[0] ? cls : weapon);
            marker_detail_add(d, "damage", type);
            marker_detail_add(d, "zone", zone);
            am_push(out, cap, &n, AM_KILL, 4, "Kill", false, d);
        }
        return n;
    }
    if (strstr(line, "Logged an incap.! nickname: ")) {
        if (!(events & AM_DOWNED) || !sc_between(line, "nickname: ", ",", a, sizeof a) || strcmp(a, s->me)) return 0;
        if (sc_between(line, "causes: [", "]", b, sizeof b)) {
            char *paren = strstr(b, " (");
            if (paren) *paren = 0;
            marker_detail_add(d, "cause", b);
        }
        am_push(out, cap, &n, AM_DOWNED, 2, "Downed", false, d);
        return n;
    }
    if (strstr(line, "[CSessionManager::OnClientSpawned] Spawned!")) {
        if (events & AM_SPAWN) am_push(out, cap, &n, AM_SPAWN, 3, "Spawned");
        return n;
    }
    if (strstr(line, "<SHUDEvent_OnNotification> Added notification \"")) {
        char raw[256], text[256];
        if (!sc_between(line, "Added notification \"", "\" [", raw, sizeof raw)) return 0;
        sc_hud_text(raw, text, sizeof text);
        static const char ACCEPTED[] = "Contract Accepted:", DONE[] = "Contract Complete:";
        const char *title = 0, *note = 0;
        u32 ev = 0;
        if (!strncmp(text, ACCEPTED, sizeof ACCEPTED - 1)) title = text + sizeof ACCEPTED - 1, note = "Contract accepted", ev = AM_CONTRACT;
        else if (!strncmp(text, DONE, sizeof DONE - 1)) title = text + sizeof DONE - 1, note = "Contract complete", ev = AM_QUEST;
        if (title) {
            while (*title == ' ') title++;
            marker_detail_add(d, "contract", title);
            if (events & ev) am_push(out, cap, &n, ev, ev == AM_QUEST ? 3 : 5, note, false, d);
            return n;
        }
        if (!(events & AM_ZONE)) return 0;
        size_t l = strlen(text);
        static const char JUR[] = " Jurisdiction";
        if (!strcmp(text, "Entering Armistice Zone")) am_push(out, cap, &n, AM_ZONE, 6, "Armistice zone");
        else if (!strcmp(text, "Leaving Armistice Zone")) am_push(out, cap, &n, AM_ZONE, 6, "Left armistice zone");
        else if (!strncmp(text, "Entered ", 8) && l > 8 + sizeof JUR - 1 && !strcmp(text + l - (sizeof JUR - 1), JUR))
            am_push(out, cap, &n, AM_ZONE, 6, text);
        return n;
    }
    if (strstr(line, "<Quantum Navtarget>") && strstr(line, "received QT data for Entity:")) {
        if (!(events & AM_TRAVEL) || !sc_between(line, "Local client user ", "[", a, sizeof a) || strcmp(a, s->me)) return 0;
        const char *t = strstr(line, " to Target ");
        if (!t) return 0;
        str_set(b, t + 11);
        b[strcspn(b, " ")] = 0;
        i64 at = sc_time(line);
        if (!b[0] || (!strcmp(b, s->qt_to) && at && at - s->qt_at <= SC_QT_AGAIN)) return 0;
        str_set(s->qt_to, b);
        s->qt_at = at;
        sc_bare(b);
        marker_detail_add(d, "to", b);
        am_push(out, cap, &n, AM_TRAVEL, 5, "Quantum jump", false, d);
        return n;
    }
    if (strstr(line, "<Changing Solar System>") && sc_between(line, " changing system from ", " ", a, sizeof a)) {
        const char *t = strstr(line, " changing system from ");
        t = t ? strstr(t + 22, " to ") : 0;
        if (!t || !(events & AM_TRAVEL)) return 0;
        str_set(b, t + 4);
        b[strcspn(b, " ")] = 0;
        if (!b[0]) return 0;
        marker_detail_add(d, "from", a);
        marker_detail_add(d, "to", b);
        char note[64];
        snprintf(note, sizeof note, "Jump to %s", b);
        am_push(out, cap, &n, AM_TRAVEL, 5, note, false, d);
    }
    return n;
}

// The running game's Game.log: <channel>\Game.log beside its Bin64 folder. "" when the game isn't running.
static void sc_log_path(wchar_t *out)
{
    out[0] = 0;
    static char cmd[32768];
    char title[8], image[MAX_PATH * 3];
    if (!process_facts("StarCitizen.exe", cmd, sizeof cmd, title, sizeof title, image, sizeof image) || !image[0]) return;
    wchar_t w[MAX_PATH];
    utf8_to_wide(image, w, MAX_PATH);
    wchar_t *slash = wcsrchr(w, L'\\');
    if (!slash) return;
    *slash = 0; // ...\Bin64
    slash = wcsrchr(w, L'\\');
    if (!slash || _wcsicmp(slash + 1, L"Bin64")) return;
    *slash = 0;
    swprintf(out, MAX_PATH, L"%s\\Game.log", w);
}

static struct {
    ScSeen  seen;
    AmTail  tail;
    wchar_t log[MAX_PATH]; // the running game's Game.log; "" until found
    i64     next;          // when to look for it again
} g_sc;

// Once a second from the worker: the lines Game.log has added.
static void sc_poll()
{
    u32 ev = g_am.events[AMG_STAR_CITIZEN];
    if (!ev) return;
    i64 now = now_us();
    if (!g_sc.tail.path[0] && now >= g_sc.next) {
        sc_log_path(g_sc.log);
        g_sc.next = now + 5000000;
    }
    AutoMark marks[8];
    if (g_sc.log[0] && !am_tail(&g_sc.tail, g_sc.log, [&](const char *l, bool old) {
            am_put(marks, sc_line(&g_sc.seen, ev, l, old, marks, COUNT(marks)), AMG_STAR_CITIZEN);
        }))
        g_sc.log[0] = 0;
}
