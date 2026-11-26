// Markers from games: kills, deaths, wins and objectives put on the timeline by the games' own official local feeds.
// Options > Auto markers turns each game on and picks which of them it marks (AmEvent bits; all on by default); its
// switch at the top turns them all off. A test copy (REKODR_HOME) keeps its cfg file in its own local folder.
//
//   Counter-Strike 2   Game State Integration. Turning it on writes gamestate_integration_rekodr.cfg into the
//   Dota 2             game's GSI folder (found through Steam's libraries); the game reads it when it starts and
//                      then POSTs its state as JSON to a listener on 127.0.0.1:AM_GSI_PORT, which both games
//                      share. Turning a game off deletes its file; the listener closes when both are off. Dota 2
//                      reads the file only with -gamestateintegration in its launch options, which the player adds.
//   League of Legends  The Live Client Data API, https://127.0.0.1:2999, which the game serves while a match runs.
//                      Its event list is read once a second.
//
// Only what the player did counts: kills, deaths and objectives of the player themself, rounds and matches their
// team won.
// Markers go in at the moment Rekodr hears of them. Everything runs on one worker thread.

static const u16 AM_GSI_PORT = 29771;
static const wchar_t AM_GSI_CFG[] = L"gamestate_integration_rekodr.cfg";

// A Game State Integration game: its Steam app, its folder under steamapps\common, the folder in it for the cfg
// file (made when make_dir), and the parts of its state the file asks for.
struct AmGsi {
    i32 game;
    u32 app;
    const wchar_t *game_dir, *cfg_dir;
    bool make_dir;
    const char *const *data;
};
static const char *const AM_CS2_DATA[] = {"provider", "map", "round", "player_id", "player_state", "player_match_stats", 0};
static const char *const AM_DOTA_DATA[] = {"provider", "map", "player", 0};
static const AmGsi AM_GSI[] = {
    {AMG_CS2, 730, L"Counter-Strike Global Offensive", L"game\\csgo\\cfg", false, AM_CS2_DATA},
    {AMG_DOTA2, 570, L"dota 2 beta", L"game\\dota\\cfg\\gamestate_integration", true, AM_DOTA_DATA},
};

// A marker to put in. `amend`: the note (and colour) of the last kill marker change instead, as when League says
// the kill was a double kill.
struct AutoMark {
    u8   colour;
    bool amend;
    char note[32];
};

static void am_push(AutoMark *out, i32 cap, i32 *n, u8 colour, const char *note, bool amend = false)
{
    if (*n >= cap) return;
    AutoMark &m = out[(*n)++];
    m.colour = colour;
    m.amend = amend;
    str_set(m.note, note);
}

// ---------------------------------------------------------------------------------------------------------------
// Counter-Strike 2: what one state says, against the one before

struct Cs2Seen {
    bool have;      // a match's state is known (false in the menus)
    char map[64];
    i32  kills, deaths;
    char round[16]; // round.phase: live, over, freezetime
    char phase[16]; // map.phase: warmup, live, intermission, gameover
};

static const char *am_kill_note(i32 in_a_row, bool league)
{
    switch (in_a_row) {
    case 0:
    case 1: return "Kill";
    case 2: return "Double kill";
    case 3: return "Triple kill";
    case 4: return league ? "Quadra kill" : "Quad kill";
    default: return league ? "Penta kill" : "Ace";
    }
}

// The markers a state brings, of the AmEvent bits in `events`. A kill that is the second or later of its round is a
// multi-kill: with AM_MULTI it is marked as one (Double kill, Ace), with only AM_KILL as a plain Kill. A state about
// another player (the one being watched after a death) changes nothing.
static i32 cs2_marks(Cs2Seen *s, u32 events, const char *body, i64 len, AutoMark *out, i32 cap)
{
    char me[32], player[32], map[64], v[32];
    if (!j_get(body, len, "map.name", map, sizeof map)) {
        s->have = false;
        return 0;
    }
    if (!j_get(body, len, "provider.steamid", me, sizeof me) || !j_get(body, len, "player.steamid", player, sizeof player) ||
        strcmp(me, player))
        return 0;
    if (!j_get(body, len, "player.match_stats.kills", v, sizeof v)) return 0;
    i32 kills = atoi(v);
    i32 deaths = j_get(body, len, "player.match_stats.deaths", v, sizeof v) ? atoi(v) : 0;
    char round[16] = "", phase[16] = "", team[8] = "", win[8] = "";
    j_get(body, len, "round.phase", round, sizeof round);
    j_get(body, len, "map.phase", phase, sizeof phase);
    j_get(body, len, "player.team", team, sizeof team);
    j_get(body, len, "round.win_team", win, sizeof win);

    i32 n = 0;
    if (s->have && !strcmp(s->map, map) && strcmp(phase, "warmup")) {
        if (kills > s->kills) {
            i32 in_round = j_get(body, len, "player.state.round_kills", v, sizeof v) ? atoi(v) : 1;
            bool multi = in_round >= 2 && (events & AM_MULTI);
            if (multi || (events & AM_KILL)) am_push(out, cap, &n, 4, am_kill_note(multi ? in_round : 1, false));
        }
        if (deaths > s->deaths && (events & AM_DEATH)) am_push(out, cap, &n, 1, "Death");
        bool match_won = false;
        if (!strcmp(phase, "gameover") && strcmp(s->phase, "gameover") && team[0]) {
            i32 ct = j_get(body, len, "map.team_ct.score", v, sizeof v) ? atoi(v) : 0;
            i32 t = j_get(body, len, "map.team_t.score", v, sizeof v) ? atoi(v) : 0;
            match_won = !strcmp(team, "CT") ? ct > t : t > ct;
            if (match_won && (events & AM_WIN)) am_push(out, cap, &n, 3, "Match won");
        }
        if (!(match_won && (events & AM_WIN)) && (events & AM_ROUND) && !strcmp(round, "over") && strcmp(s->round, "over") && team[0] && !strcmp(win, team))
            am_push(out, cap, &n, 3, "Round won");
    }
    s->have = true;
    str_set(s->map, map);
    s->kills = kills;
    s->deaths = deaths;
    str_set(s->round, round);
    str_set(s->phase, phase);
    return n;
}

// ---------------------------------------------------------------------------------------------------------------
// Dota 2: what one state says, against the one before

static const char DOTA_POST_GAME[] = "DOTA_GAMERULES_STATE_POST_GAME";

struct DotaSeen {
    bool have;      // a match's state is known
    char match[32];
    i32  kills, deaths, assists;
    char state[48]; // map.game_state
};

// The markers a state brings, of the AmEvent bits in `events`. Only a state of the player's own match counts: one
// watched as a spectator lists both teams' players instead of the player, and changes nothing.
static i32 dota_marks(DotaSeen *s, u32 events, const char *body, i64 len, AutoMark *out, i32 cap)
{
    char match[32], v[48];
    if (!j_get(body, len, "map.matchid", match, sizeof match) || !j_get(body, len, "player.kills", v, sizeof v)) {
        s->have = false;
        return 0;
    }
    i32 kills = atoi(v);
    i32 deaths = j_get(body, len, "player.deaths", v, sizeof v) ? atoi(v) : 0;
    i32 assists = j_get(body, len, "player.assists", v, sizeof v) ? atoi(v) : 0;
    char state[48] = "", team[16] = "", win[16] = "";
    j_get(body, len, "map.game_state", state, sizeof state);
    j_get(body, len, "player.team_name", team, sizeof team);
    j_get(body, len, "map.win_team", win, sizeof win);

    i32 n = 0;
    if (s->have && !strcmp(s->match, match)) {
        if (kills > s->kills && (events & AM_KILL)) am_push(out, cap, &n, 4, "Kill");
        if (deaths > s->deaths && (events & AM_DEATH)) am_push(out, cap, &n, 1, "Death");
        if (assists > s->assists && (events & AM_ASSIST)) am_push(out, cap, &n, 2, "Assist");
        if ((events & AM_WIN) && !strcmp(state, DOTA_POST_GAME) && strcmp(s->state, DOTA_POST_GAME) && team[0] &&
            !strcmp(win, team))
            am_push(out, cap, &n, 3, "Win");
    }
    s->have = true;
    str_set(s->match, match);
    s->kills = kills;
    s->deaths = deaths;
    s->assists = assists;
    str_set(s->state, state);
    return n;
}

// ---------------------------------------------------------------------------------------------------------------
// League of Legends: new events in the match's event list

struct LolSeen {
    bool have;    // the events up to last_id are known
    i32  last_id;
    char me[128]; // the player's Riot ID ("Name#TAG")
};

// Whether an event's name is the player: the whole Riot ID or the part before the #.
static bool lol_is_me(const char *name, const char *me)
{
    if (!name[0] || !me[0]) return false;
    if (!_stricmp(name, me)) return true;
    const char *hash = strchr(me, '#');
    return hash && strlen(name) == (size_t)(hash - me) && !_strnicmp(name, me, hash - me);
}

// The markers /liveclientdata/eventdata brings, of the AmEvent bits in `events`. The first list of a match only says
// where it is up to. A multi-kill renames the kill marker before it (Double kill); without AM_KILL it is a marker of
// its own.
static i32 lol_marks(LolSeen *s, u32 events, const char *body, i64 len, AutoMark *out, i32 cap)
{
    Json j = {body, body + len};
    if (!j_find(&j, "Events") || !j_eat(&j, '[')) return 0;
    struct Ev { i32 id; const char *p; i64 len; };
    static Ev evs[1024];
    i32 count = 0, top = -1;
    while (j.p < j.end && !j_eat(&j, ']')) {
        j_ws(&j);
        const char *a = j.p;
        j_skip(&j);
        if (j.p == a) break;
        char v[16];
        i32 id = j_get(a, j.p - a, "EventID", v, sizeof v) ? atoi(v) : -1;
        if (count < COUNT(evs)) evs[count++] = {id, a, j.p - a};
        if (id > top) top = id;
        j_eat(&j, ',');
    }
    i32 n = 0;
    if (s->have && top >= s->last_id) {
        for (i32 i = 0; i < count; i++) {
            const Ev &e = evs[i];
            if (e.id <= s->last_id) continue;
            char name[32] = "", who[128] = "", v[32] = "";
            j_get(e.p, e.len, "EventName", name, sizeof name);
            bool mine = j_get(e.p, e.len, "KillerName", who, sizeof who) && lol_is_me(who, s->me);
            if (!strcmp(name, "ChampionKill")) {
                if (mine && (events & AM_KILL)) am_push(out, cap, &n, 4, "Kill");
                if ((events & AM_DEATH) && j_get(e.p, e.len, "VictimName", who, sizeof who) && lol_is_me(who, s->me))
                    am_push(out, cap, &n, 1, "Death");
            } else if (!strcmp(name, "Multikill")) {
                if (mine && (events & AM_MULTI)) {
                    i32 streak = j_get(e.p, e.len, "KillStreak", v, sizeof v) ? atoi(v) : 2;
                    am_push(out, cap, &n, 4, am_kill_note(streak, true), (events & AM_KILL) != 0);
                }
            } else if (!strcmp(name, "GameEnd")) {
                if ((events & AM_WIN) && j_get(e.p, e.len, "Result", v, sizeof v) && !strcmp(v, "Win")) am_push(out, cap, &n, 3, "Win");
            } else if (mine && (events & AM_OBJECTIVE)) {
                static const char *const objectives[][2] = {{"DragonKill", "Dragon"}, {"HeraldKill", "Rift Herald"},
                                                            {"BaronKill", "Baron"}, {"TurretKilled", "Turret"},
                                                            {"InhibKilled", "Inhibitor"}};
                for (i32 o = 0; o < COUNT(objectives); o++)
                    if (!strcmp(name, objectives[o][0])) am_push(out, cap, &n, 5, objectives[o][1]);
            }
        }
    }
    s->have = true; // a list that went back (a new match) is where the new match is up to
    s->last_id = top;
    return n;
}

// ---------------------------------------------------------------------------------------------------------------
// Finding the Game State Integration games

// The steamapps folder of the library an app is installed in, from Steam's libraryfolders.vdf. Any thread (the
// main one and the achievements worker), so the file is read into a buffer of the call's own.
static bool steam_app_library(u32 app, Path *out)
{
    Path root;
    if (!steam_root(&root)) return false;
    char *text = read_file(path_join(root, L"steamapps\\libraryfolders.vdf").s, 0);
    if (!text) return false;
    char libs[16][MAX_PATH * 2];
    i32 n = steam_libraries(text, libs, COUNT(libs));
    free(text);
    for (i32 i = 0; i < n; i++) {
        wchar_t wlib[MAX_PATH], manifest[MAX_PATH];
        utf8_to_wide(libs[i], wlib, MAX_PATH);
        swprintf(manifest, MAX_PATH, L"%s\\steamapps\\appmanifest_%u.acf", wlib, app);
        if (GetFileAttributesW(manifest) != INVALID_FILE_ATTRIBUTES) {
            swprintf(out->s, COUNT(out->s), L"%s\\steamapps", wlib);
            return true;
        }
    }
    return false;
}

// Where a game's cfg file goes. False if the game isn't installed. Dota 2 reads its folder of GSI files only when
// it is there, so it is made; Counter-Strike 2's is its cfg folder.
static bool gsi_cfg_path(const AmGsi &g, Path *out)
{
    if (is_test_copy()) { // a test copy writes its cfg files into its own home, never into the game
        Path dir = path_join(g_app.paths.local_dir, L"gsicfg");
        CreateDirectoryW(dir.s, 0);
        wchar_t name[64];
        swprintf(name, COUNT(name), L"%hs.cfg", AM_GAMES[g.game].key);
        *out = path_join(dir, name);
        return true;
    }
    Path lib;
    if (!steam_app_library(g.app, &lib)) return false;
    wchar_t game[MAX_PATH], dir[MAX_PATH];
    swprintf(game, MAX_PATH, L"%s\\common\\%s", lib.s, g.game_dir);
    DWORD a = GetFileAttributesW(game);
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) return false;
    swprintf(dir, MAX_PATH, L"%s\\%s", game, g.cfg_dir);
    if (g.make_dir) CreateDirectoryW(dir, 0);
    a = GetFileAttributesW(dir);
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) return false;
    swprintf(out->s, COUNT(out->s), L"%s\\%s", dir, AM_GSI_CFG);
    return true;
}

// A cfg file's text, with the token the game sends back so the listener knows the state is the game's.
static i32 gsi_cfg_text(const AmGsi &g, const char *token, char *out, i32 cap)
{
    i32 n = snprintf(out, cap,
                     "\"" APP_NAME "\"\n"
                     "{\n"
                     "    \"uri\"       \"http://127.0.0.1:%u/\"\n"
                     "    \"timeout\"   \"1.0\"\n"
                     "    \"buffer\"    \"0.0\"\n"
                     "    \"throttle\"  \"0.1\"\n"
                     "    \"heartbeat\" \"30.0\"\n"
                     "    \"auth\"\n"
                     "    {\n"
                     "        \"token\" \"%s\"\n"
                     "    }\n"
                     "    \"data\"\n"
                     "    {\n",
                     AM_GSI_PORT, token);
    for (const char *const *d = g.data; *d && n < cap; d++) n += snprintf(out + n, cap - n, "        \"%s\" \"1\"\n", *d);
    if (n < cap) n += snprintf(out + n, cap - n, "    }\n}\n");
    return n < cap ? n : cap - 1;
}

// ---------------------------------------------------------------------------------------------------------------
// The worker

static struct {
    HANDLE thread;
    HANDLE wake;                     // set to stop or to look at the switches again
    volatile bool stop;
    volatile bool gsi_on;            // a GSI game is on: the listener runs
    volatile u32 events[AMG_COUNT];  // AmEvent bits each game marks; 0 while off
    volatile LONG state[AMG_COUNT];  // AmState (app.h), for Options
    bool gsi_applied[AMG_COUNT];     // whether automarks_apply last left the game's cfg file written
    char token[40];                  // the cfg files' token
    Cs2Seen cs2;
    DotaSeen dota;
    LolSeen lol;
    i64 last_kill[AMG_COUNT];        // the last kill marker of each (its time), for an amend
} g_am;

static void am_put(const AutoMark *marks, i32 n, i32 game)
{
    for (i32 i = 0; i < n; i++) {
        const AutoMark &m = marks[i];
        i64 *last_kill = &g_am.last_kill[game];
        if (m.amend && *last_kill) markers_set(*last_kill, m.colour, m.note);
        else if (!m.amend) {
            i64 t = markers_add(now_us(), m.colour, m.note);
            if (m.colour == 4) *last_kill = t;
        }
        log_line("markers: %s: %s", AM_GAMES[game].name, m.note);
    }
    if (n) event_post(Event{}); // the timeline shows it
}

// One POST from a game: the request is read, answered and the state looked at. provider.appid says whose it is.
static void am_gsi_client(SOCKET c)
{
    DWORD wait = 2000;
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char *)&wait, sizeof wait);
    static char buf[256 * 1024];
    i32 used = 0, body_at = -1, body_len = 0;
    u64 until = GetTickCount64() + 3000; // the whole request, however slowly it trickles in
    for (;;) {
        if (used >= (i32)sizeof buf - 1 || GetTickCount64() > until) break;
        i32 got = recv(c, buf + used, (i32)sizeof buf - 1 - used, 0);
        if (got <= 0) break;
        used += got;
        buf[used] = 0;
        if (body_at < 0) {
            char *end = strstr(buf, "\r\n\r\n");
            if (!end) continue;
            body_at = (i32)(end + 4 - buf);
            *end = 0;
            for (char *line = buf; line && *line;) {
                char *next = strstr(line, "\r\n");
                if (next) *next = 0;
                if (!_strnicmp(line, "Content-Length:", 15)) body_len = atoi(line + 15);
                line = next ? next + 2 : 0;
                if (next) *next = '\r';
            }
            *end = '\r';
            if (body_len > (i32)sizeof buf - 1 - body_at) break; // too big to read: not a game's
        }
        if (used - body_at >= body_len) break;
    }
    static const char ok[] = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    send(c, ok, (i32)sizeof ok - 1, 0);
    closesocket(c);
    if (body_at < 0 || used - body_at < body_len || body_len <= 0) return;
    const char *body = buf + body_at;
    char token[64], app[16];
    if (!j_get(body, body_len, "auth.token", token, sizeof token) || strcmp(token, g_am.token)) return;
    if (!j_get(body, body_len, "provider.appid", app, sizeof app)) return;
    AutoMark marks[8];
    for (i32 i = 0; i < COUNT(AM_GSI); i++) {
        const AmGsi &g = AM_GSI[i];
        u32 events = g_am.events[g.game];
        if ((u32)atoi(app) != g.app || !events) continue;
        i32 n = g.game == AMG_CS2 ? cs2_marks(&g_am.cs2, events, body, body_len, marks, COUNT(marks))
                                  : dota_marks(&g_am.dota, events, body, body_len, marks, COUNT(marks));
        am_put(marks, n, g.game);
    }
}

// A GET to League's local API. The game serves it with its own certificate, which Windows doesn't know, so the
// certificate isn't checked; it is only ever 127.0.0.1.
static bool am_lol_get(HINTERNET session, const wchar_t *path, char *out, i32 cap, i32 *len)
{
    *len = 0;
    HINTERNET conn = WinHttpConnect(session, L"127.0.0.1", 2999, 0);
    HINTERNET req = conn ? WinHttpOpenRequest(conn, L"GET", path, 0, 0, 0, WINHTTP_FLAG_SECURE) : 0;
    bool ok = false;
    if (req) {
        DWORD flags = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_CN_INVALID | SECURITY_FLAG_IGNORE_CERT_DATE_INVALID;
        WinHttpSetOption(req, WINHTTP_OPTION_SECURITY_FLAGS, &flags, sizeof flags);
        WinHttpSetTimeouts(req, 1000, 1000, 2000, 3000);
        DWORD status = 0, sl = sizeof status;
        if (WinHttpSendRequest(req, 0, 0, 0, 0, 0, 0) && WinHttpReceiveResponse(req, 0) &&
            WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, 0, &status, &sl, 0) && status == 200) {
            ok = true;
            for (;;) {
                DWORD got = 0;
                if (*len >= cap - 1) { ok = false; break; }
                if (!WinHttpReadData(req, out + *len, (DWORD)(cap - 1 - *len), &got)) { ok = false; break; }
                if (!got) break;
                *len += (i32)got;
            }
        }
    }
    if (req) WinHttpCloseHandle(req);
    if (conn) WinHttpCloseHandle(conn);
    out[ok ? *len : 0] = 0;
    return ok;
}

// Once a second while League's API answers, every 3 s while it doesn't (no match).
static void am_lol_poll(HINTERNET session)
{
    static char buf[512 * 1024];
    i32 len = 0;
    if (!g_am.lol.me[0]) {
        if (!am_lol_get(session, L"/liveclientdata/activeplayername", buf, sizeof buf, &len)) {
            g_am.lol.have = false;
            return;
        }
        Json j = {buf, buf + len};
        if (!j_string(&j, g_am.lol.me, sizeof g_am.lol.me)) return;
        log_line("markers: League match as %s", g_am.lol.me);
    }
    if (!am_lol_get(session, L"/liveclientdata/eventdata", buf, sizeof buf, &len)) {
        g_am.lol = {};
        return;
    }
    AutoMark marks[16];
    i32 n = lol_marks(&g_am.lol, g_am.events[AMG_LOL], buf, len, marks, COUNT(marks));
    am_put(marks, n, AMG_LOL);
}

// Sets the state of each GSI game that is on.
static void am_gsi_state(AmState st)
{
    for (i32 i = 0; i < COUNT(AM_GSI); i++)
        if (g_am.gsi_applied[AM_GSI[i].game] && g_am.state[AM_GSI[i].game] != AM_NOT_FOUND)
            InterlockedExchange(&g_am.state[AM_GSI[i].game], st);
}

static DWORD WINAPI am_thread(void *)
{
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    SOCKET ls = INVALID_SOCKET;
    HINTERNET session = WinHttpOpen(L"rekodr", WINHTTP_ACCESS_TYPE_NO_PROXY, 0, 0, 0);
    i64 lol_next = 0, gsi_retry = 0;
    bool busy_said = false;
    while (!g_am.stop) {
        if (g_am.gsi_on && ls == INVALID_SOCKET && now_us() >= gsi_retry) {
            ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            sockaddr_in a = {};
            a.sin_family = AF_INET;
            a.sin_port = htons(AM_GSI_PORT);
            a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            BOOL excl = TRUE;
            if (ls != INVALID_SOCKET) setsockopt(ls, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&excl, sizeof excl);
            if (ls == INVALID_SOCKET || bind(ls, (sockaddr *)&a, sizeof a) || listen(ls, 4)) {
                if (ls != INVALID_SOCKET) closesocket(ls);
                ls = INVALID_SOCKET;
                am_gsi_state(AM_PORT_BUSY);
                if (!busy_said) log_line("markers: port %u is taken; games can't reach " APP_NAME, AM_GSI_PORT);
                busy_said = true;
                gsi_retry = now_us() + 10000000;
            } else {
                am_gsi_state(AM_READY);
                busy_said = false;
                log_line("markers: listening for games on port %u", AM_GSI_PORT);
            }
        }
        if (!g_am.gsi_on && ls != INVALID_SOCKET) {
            closesocket(ls);
            ls = INVALID_SOCKET;
        }
        if (!g_am.events[AMG_CS2]) g_am.cs2 = {};
        if (!g_am.events[AMG_DOTA2]) g_am.dota = {};
        if (!g_am.events[AMG_LOL]) g_am.lol = {};

        i64 now = now_us();
        i64 wait_ms = 1000;
        bool lol_on = g_am.events[AMG_LOL] && session;
        if (lol_on) {
            i64 until = (lol_next - now) / 1000;
            if (until < wait_ms) wait_ms = until < 0 ? 0 : until;
        }
        if (ls != INVALID_SOCKET) {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(ls, &fds);
            timeval tv = {(long)(wait_ms / 1000), (long)(wait_ms % 1000) * 1000};
            if (select(0, &fds, 0, 0, &tv) > 0) {
                SOCKET c = accept(ls, 0, 0);
                if (c != INVALID_SOCKET) am_gsi_client(c);
            }
        } else {
            WaitForSingleObject(g_am.wake, (DWORD)wait_ms);
        }
        if (lol_on && now_us() >= lol_next) {
            am_lol_poll(session);
            lol_next = now_us() + (g_am.lol.have ? 1000000 : 3000000);
        }
    }
    if (ls != INVALID_SOCKET) closesocket(ls);
    if (session) WinHttpCloseHandle(session);
    WSACleanup();
    return 0;
}

// The cfg files' token: the one in a game's file if there is one, else the one already in use (a switch turned
// off and on again), else a new one. A game already running keeps reaching Rekodr with the token it read when it
// started. g_am.token only changes when the token does (the worker compares with it).
static void am_gsi_token()
{
    char tok[sizeof g_am.token] = {};
    i32 k = 0;
    for (i32 i = 0; i < COUNT(AM_GSI) && k < 16; i++) {
        Path cfg;
        if (!gsi_cfg_path(AM_GSI[i], &cfg)) continue;
        FILE *f = _wfopen(cfg.s, L"rb");
        if (!f) continue;
        char text[4096];
        size_t n = fread(text, 1, sizeof text - 1, f);
        fclose(f);
        text[n] = 0;
        k = 0;
        if (const char *t = strstr(text, "\"token\"")) {
            t = strchr(t + 7, '"');
            if (t)
                for (t++; *t && *t != '"' && k < (i32)sizeof tok - 1; t++) tok[k++] = *t;
        }
        tok[k] = 0;
    }
    if (k < 16) {
        if (strlen(g_am.token) >= 16) return;
        u8 raw[16];
        BCryptGenRandom(0, raw, sizeof raw, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        for (i32 i = 0; i < 16; i++) snprintf(tok + i * 2, 3, "%02x", raw[i]);
    }
    if (strcmp(tok, g_am.token)) memcpy(g_am.token, tok, sizeof tok);
}

// Writes or deletes a game's cfg file as its switch says. False if the game wasn't found.
static bool am_gsi_cfg(const AmGsi &g, bool on)
{
    const char *name = AM_GAMES[g.game].name;
    Path cfg;
    if (!gsi_cfg_path(g, &cfg)) return false;
    if (!on) {
        if (DeleteFileW(cfg.s)) log_line("markers: %s's cfg file removed", name);
        return true;
    }
    char text[2048];
    i32 n = gsi_cfg_text(g, g_am.token, text, sizeof text);
    char old[2048];
    size_t old_n = 0;
    if (FILE *f = _wfopen(cfg.s, L"rb")) {
        old_n = fread(old, 1, sizeof old, f);
        fclose(f);
    }
    if (old_n == (size_t)n && !memcmp(old, text, (size_t)n)) return true;
    FILE *f = _wfopen(cfg.s, L"wb");
    if (!f) {
        log_line("markers: couldn't write %s's cfg file (%d)", name, (int)GetLastError());
        return true;
    }
    fwrite(text, 1, (size_t)n, f);
    fclose(f);
    log_line("markers: %s's cfg file written; the game reads it when it starts", name);
    return true;
}

// At startup and when a switch changes. Main thread.
static void automarks_apply()
{
    const Config &c = g_app.config;
    for (i32 g = 0; g < AMG_COUNT; g++) g_am.events[g] = c.auto_marks && c.auto_marks_on[g] ? c.auto_marks_events[g] : 0;
    bool any_gsi = false;
    for (i32 i = 0; i < COUNT(AM_GSI); i++) any_gsi |= g_am.events[AM_GSI[i].game] != 0;
    if (any_gsi) am_gsi_token();
    bool gsi = false;
    for (i32 i = 0; i < COUNT(AM_GSI); i++) {
        const AmGsi &g = AM_GSI[i];
        bool on = g_am.events[g.game] != 0;
        if (on != g_am.gsi_applied[g.game] || on) {
            bool found = am_gsi_cfg(g, on);
            if (on && !found) {
                if (InterlockedExchange(&g_am.state[g.game], AM_NOT_FOUND) != AM_NOT_FOUND)
                    log_line("markers: %s isn't installed", AM_GAMES[g.game].name);
            } else if (!on) InterlockedExchange(&g_am.state[g.game], AM_OFF);
            else if (g_am.state[g.game] == AM_OFF || g_am.state[g.game] == AM_NOT_FOUND)
                InterlockedExchange(&g_am.state[g.game], AM_READY);
            g_am.gsi_applied[g.game] = on;
            gsi |= on && found;
        }
    }
    g_am.gsi_on = gsi;
    if ((g_am.gsi_on || g_am.events[AMG_LOL]) && !g_am.thread) {
        g_am.wake = CreateEventW(0, FALSE, FALSE, 0);
        g_am.thread = CreateThread(0, 0, am_thread, 0, 0, 0);
    }
    if (g_am.wake) SetEvent(g_am.wake);
}

static AmState automarks_state(i32 game) { return (AmState)g_am.state[game]; }

static void automarks_stop()
{
    if (!g_am.thread) return;
    g_am.stop = true;
    SetEvent(g_am.wake);
    // A worker still in a request after 5 s keeps its event: it waits on it again once the request ends
    if (WaitForSingleObject(g_am.thread, 5000) == WAIT_OBJECT_0) CloseHandle(g_am.wake);
    CloseHandle(g_am.thread);
    g_am.thread = 0;
}

// Uninstall: the cfg files go, whatever the switches say.
static void automarks_uninstall()
{
    for (i32 i = 0; i < COUNT(AM_GSI); i++) am_gsi_cfg(AM_GSI[i], false);
}
