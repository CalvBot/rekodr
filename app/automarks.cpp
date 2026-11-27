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
//   Minecraft          logs\latest.log in the running game's folder (mc_game_dir), read once a second.
//   Path of Exile 1, 2 logs\Client.txt in the game's folder (Steam, else Grinding Gear Games' install).
//   Team Fortress 2    tf\console.log. Turning it on adds a con_logfile line to tf\cfg\autoexec.cfg, read when the
//                      game starts; turning it off takes only that line out.
//
//   F1 25              Its UDP telemetry on 127.0.0.1:F1_PORT, which the player turns on in the game's settings.
//   iRacing            The sim's telemetry file, Local\IRSDKMemMapFileName, read once a second.
//   Assetto Corsa      Its shared memory, Local\acpmf_graphics, read once a second.
//   Competizione
//
//   Rocket League      Its Stats API, a JSON stream from 127.0.0.1:49123 (the Port in its ini). Turning it on sets
//                      PacketSendRate in TAGame\Config\DefaultStatsAPI.ini (and the player's TAStatsAPI.ini) when
//                      the API is off there, read when the game starts; turning it off puts back only what it set.
//   Hearthstone        Logs\Hearthstone_<started>\Power.log in its folder, the newest run's. Turning it on adds a
//                      [Power] section to %LOCALAPPDATA%\Blizzard\Hearthstone\log.config when there is none, read
//                      when the game starts; turning it off takes out only that section.
//   War Thunder        Its map server, http://127.0.0.1:8111, once a second while the game runs: the battle's log
//                      and the mission's state. The player's name is the one typed in Options.
//   Star Citizen       Game.log in the running game's folder (sclog.cpp), read once a second.
//   Escape from Tarkov Logs\log_<started>\ in its folder, the newest run's: its application, output, backend and
//                      push-notifications logs, only read. Nothing is written to the game or sent to it.
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
static const char *const AM_CS2_DATA[] = {"provider", "map", "round", "player_id", "player_state", "player_weapons",
                                          "player_match_stats", 0};
static const char *const AM_DOTA_DATA[] = {"provider", "map", "player", 0};
static const AmGsi AM_GSI[] = {
    {AMG_CS2, 730, L"Counter-Strike Global Offensive", L"game\\csgo\\cfg", false, AM_CS2_DATA},
    {AMG_DOTA2, 570, L"dota 2 beta", L"game\\dota\\cfg\\gamestate_integration", true, AM_DOTA_DATA},
};

// A marker to put in. `amend`: the note (and colour) of the last kill marker change instead, as when League says
// the kill was a double kill.
struct AutoMark {
    u32  event; // its AmEvent bit
    u8   colour;
    bool amend;
    char note[64];
    char detail[256]; // the marker's detail (marker_detail_add), "" for none
};

static void am_push(AutoMark *out, i32 cap, i32 *n, u32 event, u8 colour, const char *note, bool amend = false, const char *detail = 0)
{
    if (*n >= cap) return;
    AutoMark &m = out[(*n)++];
    m.event = event;
    m.colour = colour;
    m.amend = amend;
    str_set(m.note, note);
    str_set(m.detail, detail ? detail : "");
}

// ---------------------------------------------------------------------------------------------------------------
// Counter-Strike 2: what one state says, against the one before

// What the last states said. A state may leave a part out: what it leaves out stays as it was.
struct Cs2Seen {
    bool have;        // a match's state is known (false in the menus)
    char map[64];
    i32  kills, deaths, assists, mvps, score; // player.match_stats
    i32  round_kills, round_hs; // player.state: this round's kills, and those by headshot
    bool c4, c4_gone; // the player carries the bomb; they stopped carrying it in the last state
    char weapon[32];  // the weapon in hand, without its "weapon_"
    char team[8];     // player.team: CT or T
    i32  health;
    char round[16];   // round.phase: live, over, freezetime
    char bomb[16];    // round.bomb: planted, exploded, defused, or ""
    char phase[16];   // map.phase: warmup, live, intermission, gameover
    bool in_match;    // a match's start was marked and its end not yet
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

// Whether a state has a part at `path`, of any kind.
static bool j_has(const char *body, i64 len, const char *path)
{
    Json j = {body, body + len};
    return j_find(&j, path);
}

// The number at `path`, or `keep` when the state leaves it out.
static i32 j_int(const char *body, i64 len, const char *path, i32 keep)
{
    char v[32];
    return j_get(body, len, path, v, sizeof v) ? atoi(v) : keep;
}

// The markers a state brings, of the AmEvent bits in `events`. Each kill is a marker, however many one state brings.
// A kill that is the second or later of its round is a multi-kill: with AM_MULTI it is marked as one (Double kill,
// Ace), else as a Headshot (with AM_HEADSHOT, when round_killhs went up with it) or a Kill. Its detail holds the
// weapon in hand and, for a headshot, area=head. The bomb is the player's plant when they stopped carrying it as it
// was planted, or when their score went up then without a kill or assist; a defuse is theirs when they are a live
// CT whose score went up so. A state about another player (the one being watched after a death) changes nothing.
// A CS2 map's name as the game's menus show it, or its file name (de_dust2) for a map not listed
static const char *cs2_map_name(const char *map)
{
    static const char *const names[][2] = {
        {"de_dust2", "Dust II"},   {"de_mirage", "Mirage"},     {"de_inferno", "Inferno"}, {"de_nuke", "Nuke"},
        {"de_ancient", "Ancient"}, {"de_anubis", "Anubis"},     {"de_vertigo", "Vertigo"}, {"de_overpass", "Overpass"},
        {"de_train", "Train"},     {"cs_office", "Office"},     {"cs_italy", "Italy"},     {"ar_baggage", "Baggage"},
        {"ar_shoots", "Shoots"},
    };
    for (auto &n : names)
        if (!strcmp(map, n[0])) return n[1];
    return map;
}

// A match is a band: it starts (span=match) when the map's phase is first seen live, and ends (span_end=match) at
// gameover, on another map, or when a state has no map (the player left for the menus).
static i32 cs2_marks(Cs2Seen *s, u32 events, const char *body, i64 len, AutoMark *out, i32 cap)
{
    char me[32], player[32], map[64];
    i32 n = 0;
    bool on_map = j_get(body, len, "map.name", map, sizeof map);
    if (s->in_match && (!on_map || strcmp(s->map, map))) {
        if (events & AM_RAID_END) am_push(out, cap, &n, AM_RAID_END, 5, "Match end", false, "span_end=match");
        s->in_match = false;
    }
    if (!on_map) {
        s->have = false;
        return n;
    }
    if (!j_get(body, len, "provider.steamid", me, sizeof me) || !j_get(body, len, "player.steamid", player, sizeof player) ||
        strcmp(me, player))
        return n;
    bool same = s->have && !strcmp(s->map, map);
    bool in_match = s->in_match;
    Cs2Seen was = *s;
    if (!same) was = {};
    Cs2Seen now = was;
    now.kills = j_int(body, len, "player.match_stats.kills", was.kills);
    now.deaths = j_int(body, len, "player.match_stats.deaths", was.deaths);
    now.assists = j_int(body, len, "player.match_stats.assists", was.assists);
    now.mvps = j_int(body, len, "player.match_stats.mvps", was.mvps);
    now.score = j_int(body, len, "player.match_stats.score", was.score);
    now.round_kills = j_int(body, len, "player.state.round_kills", was.round_kills);
    now.round_hs = j_int(body, len, "player.state.round_killhs", was.round_hs);
    now.health = j_int(body, len, "player.state.health", was.health);
    char got[16];
    if (j_get(body, len, "player.team", got, sizeof got)) str_set(now.team, got);
    if (j_has(body, len, "player.weapons")) {
        now.c4 = false;
        now.weapon[0] = 0;
        for (i32 i = 0; i < 16; i++) {
            char path[48], name[48], state[16];
            snprintf(path, sizeof path, "player.weapons.weapon_%d.name", i);
            if (!j_get(body, len, path, name, sizeof name)) continue;
            snprintf(path, sizeof path, "player.weapons.weapon_%d.state", i);
            if (!strcmp(name, "weapon_c4")) now.c4 = true;
            if (j_get(body, len, path, state, sizeof state) && strcmp(state, "holstered"))
                str_set(now.weapon, starts_with(name, "weapon_") ? name + 7 : name);
        }
    }
    now.c4_gone = was.c4 && !now.c4;
    if (j_has(body, len, "round")) {
        now.round[0] = now.bomb[0] = 0;
        j_get(body, len, "round.phase", now.round, sizeof now.round);
        j_get(body, len, "round.bomb", now.bomb, sizeof now.bomb);
    }
    if (j_get(body, len, "map.phase", got, sizeof got)) str_set(now.phase, got);
    char win[8] = "";
    j_get(body, len, "round.win_team", win, sizeof win);

    if (!in_match && !strcmp(now.phase, "live") && (events & AM_RAID_START)) {
        char note[96], detail[256] = "";
        snprintf(note, sizeof note, "Match start (%s)", cs2_map_name(map));
        marker_detail_add(detail, "map", cs2_map_name(map));
        marker_detail_add(detail, "span", "match");
        am_push(out, cap, &n, AM_RAID_START, 5, note, false, detail);
        in_match = true;
    }
    if (same && strcmp(now.phase, "warmup")) {
        i32 k = now.kills - was.kills;
        k = k < 0 ? 0 : k > 5 ? 5 : k;
        // Headshots among them: round_killhs starts again at 0 with each round, as round_kills does
        i32 hs = now.round_kills < was.round_kills ? now.round_hs : now.round_hs - was.round_hs;
        hs = hs < 0 ? 0 : hs > k ? k : hs;
        for (i32 i = 0; i < k; i++) {
            i32 in_round = now.round_kills - (k - 1 - i);
            bool head = i >= k - hs;
            bool multi = in_round >= 2 && (events & AM_MULTI), shot = head && (events & AM_HEADSHOT);
            if (!multi && !shot && !(events & AM_KILL)) continue;
            char detail[256] = "";
            marker_detail_add(detail, "weapon", now.weapon);
            if (head) marker_detail_add(detail, "area", "head");
            am_push(out, cap, &n, multi ? AM_MULTI : AM_KILL, 4, multi ? am_kill_note(in_round, false) : shot ? "Headshot" : "Kill", false, detail);
        }
        if (now.deaths > was.deaths && (events & AM_DEATH)) am_push(out, cap, &n, AM_DEATH, 1, "Death");
        if (now.mvps > was.mvps && (events & AM_MVP)) am_push(out, cap, &n, AM_MVP, 3, "MVP");
        bool scored = now.score > was.score && now.kills == was.kills && now.assists == was.assists;
        if (!strcmp(now.bomb, "planted") && strcmp(was.bomb, "planted") && (events & AM_PLANT) &&
            (now.c4_gone || was.c4_gone || (scored && !strcmp(now.team, "T"))))
            am_push(out, cap, &n, AM_PLANT, 5, "Bomb planted");
        if (!strcmp(now.bomb, "defused") && strcmp(was.bomb, "defused") && (events & AM_DEFUSE) && scored &&
            !strcmp(now.team, "CT") && now.health > 0)
            am_push(out, cap, &n, AM_DEFUSE, 5, "Bomb defused");
        bool match_won = false;
        if (!strcmp(now.phase, "gameover") && strcmp(was.phase, "gameover") && now.team[0]) {
            i32 ct = j_int(body, len, "map.team_ct.score", 0), t = j_int(body, len, "map.team_t.score", 0);
            match_won = !strcmp(now.team, "CT") ? ct > t : t > ct;
            if (match_won && (events & AM_WIN)) am_push(out, cap, &n, AM_WIN, 3, "Match won");
        }
        if (!(match_won && (events & AM_WIN)) && (events & AM_ROUND) && !strcmp(now.round, "over") && strcmp(was.round, "over") &&
            now.team[0] && !strcmp(win, now.team))
            am_push(out, cap, &n, AM_ROUND, 3, "Round won");
    }
    if (in_match && !strcmp(now.phase, "gameover")) {
        if (events & AM_RAID_END) am_push(out, cap, &n, AM_RAID_END, 5, "Match end", false, "span_end=match");
        in_match = false;
    }
    now.in_match = in_match;
    now.have = true;
    str_set(now.map, map);
    *s = now;
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
        if (kills > s->kills && (events & AM_KILL)) am_push(out, cap, &n, AM_KILL, 4, "Kill");
        if (deaths > s->deaths && (events & AM_DEATH)) am_push(out, cap, &n, AM_DEATH, 1, "Death");
        if (assists > s->assists && (events & AM_ASSIST)) am_push(out, cap, &n, AM_ASSIST, 2, "Assist");
        if ((events & AM_WIN) && !strcmp(state, DOTA_POST_GAME) && strcmp(s->state, DOTA_POST_GAME) && team[0] &&
            !strcmp(win, team))
            am_push(out, cap, &n, AM_WIN, 3, "Win");
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
                if (mine && (events & AM_KILL)) am_push(out, cap, &n, AM_KILL, 4, "Kill");
                if ((events & AM_DEATH) && j_get(e.p, e.len, "VictimName", who, sizeof who) && lol_is_me(who, s->me))
                    am_push(out, cap, &n, AM_DEATH, 1, "Death");
            } else if (!strcmp(name, "Multikill")) {
                if (mine && (events & AM_MULTI)) {
                    i32 streak = j_get(e.p, e.len, "KillStreak", v, sizeof v) ? atoi(v) : 2;
                    am_push(out, cap, &n, AM_MULTI, 4, am_kill_note(streak, true), (events & AM_KILL) != 0);
                }
            } else if (!strcmp(name, "GameEnd")) {
                if ((events & AM_WIN) && j_get(e.p, e.len, "Result", v, sizeof v) && !strcmp(v, "Win")) am_push(out, cap, &n, AM_WIN, 3, "Win");
            } else if (mine && (events & AM_OBJECTIVE)) {
                static const char *const objectives[][2] = {{"DragonKill", "Dragon"}, {"HeraldKill", "Rift Herald"},
                                                            {"BaronKill", "Baron"}, {"TurretKilled", "Turret"},
                                                            {"InhibKilled", "Inhibitor"}};
                for (i32 o = 0; o < COUNT(objectives); o++)
                    if (!strcmp(name, objectives[o][0])) am_push(out, cap, &n, AM_OBJECTIVE, 5, objectives[o][1]);
            }
        }
    }
    s->have = true; // a list that went back (a new match) is where the new match is up to
    s->last_id = top;
    return n;
}

// ---------------------------------------------------------------------------------------------------------------
// Games that write what happens to a log: one line at a time. `old` is a line that was there before Rekodr looked,
// read only to learn who the player is.

// Whether `s` holds `name` as a whole word: after the start or a space, before the end, a space or . , !
static bool am_word(const char *s, const char *name)
{
    size_t n = strlen(name);
    if (!n) return false;
    for (const char *p = strstr(s, name); p; p = strstr(p + 1, name)) {
        char after = p[n];
        if ((p == s || p[-1] == ' ') && (!after || after == ' ' || after == '.' || after == ',' || after == '!')) return true;
    }
    return false;
}

struct McSeen { char me[32]; }; // the player's name, from "Setting user:"

// Whether the words after a player's name are a vanilla death message: "was slain by", "drowned", "fell from".
static bool mc_death_words(const char *rest)
{
    static const char *const first[] = {"was ", "drowned", "fell ", "burned ", "blew up", "hit the ground", "went ",
                                        "tried to swim", "starved", "suffocated", "walked into", "experienced kinetic",
                                        "froze to death", "withered away", "died", "discovered the floor",
                                        "didn't want to live", "left the confines"};
    for (i32 i = 0; i < COUNT(first); i++)
        if (!strncmp(rest, first[i], strlen(first[i]))) return true;
    return false;
}

// Whether a death message's killer is a mob or a thing ("Zombie", "Cave Spider", "a falling anvil"), not a player:
// a player's name is one word, so a killer of one word is kept only when it is a vanilla mob's.
static bool mc_not_player(const char *killer)
{
    if (strchr(killer, ' ')) return true;
    static const char *const mobs[] = {
        "Allay", "Armadillo", "Axolotl", "Bat", "Bee", "Blaze", "Bogged", "Breeze", "Camel", "Cat", "Chicken", "Cod",
        "Cow", "Creaking", "Creeper", "Dolphin", "Donkey", "Drowned", "Enderman", "Endermite", "Evoker", "Fireball",
        "Fox", "Frog", "Ghast", "Goat", "Guardian", "Hoglin", "Horse", "Husk", "Illusioner", "Llama", "Mooshroom",
        "Mule", "Ocelot", "Panda", "Parrot", "Phantom", "Pig", "Piglin", "Pillager", "Pufferfish", "Rabbit", "Ravager",
        "Salmon", "Sheep", "Shulker", "Silverfish", "Skeleton", "Slime", "Sniffer", "Spider", "Squid", "Stray",
        "Strider", "Tadpole", "Trident", "Turtle", "Vex", "Villager", "Vindicator", "Warden", "Witch", "Wither",
        "Wolf", "Zoglin", "Zombie", "Arrow", "TNT"};
    for (const char *m : mobs)
        if (!strcmp(killer, m)) return true;
    return false;
}

// A vanilla death message's detail, from the words after the dead player's name: killer (and weapon) when one is
// named ("was slain by Zombie using [Diamond Sword]", "drowned whilst trying to escape Zombie"), else the cause
// ("fell from a high place"). A killer that may be a player is left out, its weapon kept.
static void mc_death_detail(char (&d)[256], const char *rest)
{
    char words[160], killer[96] = "", weapon[96] = "";
    str_set(words, rest);
    size_t l = strlen(words);
    while (l && (words[l - 1] == '.' || words[l - 1] == ' ')) words[--l] = 0;
    static const char *const by[] = {" whilst trying to escape ", " whilst fighting ", " by "};
    for (i32 i = 0; i < COUNT(by) && !killer[0]; i++) {
        const char *k = strstr(words, by[i]);
        if (!k) continue;
        str_set(killer, k + strlen(by[i]));
        if (char *u = strstr(killer, " using ")) {
            str_set(weapon, u + 7);
            *u = 0;
        }
        size_t wl = strlen(weapon);
        if (wl >= 2 && weapon[0] == '[' && weapon[wl - 1] == ']') {
            weapon[wl - 1] = 0;
            memmove(weapon, weapon + 1, wl - 1);
        }
    }
    d[0] = 0;
    if (killer[0]) {
        if (mc_not_player(killer)) marker_detail_add(d, "killer", killer);
        marker_detail_add(d, "weapon", weapon);
    } else {
        marker_detail_add(d, "cause", words);
    }
}

// Minecraft's latest.log: the player's deaths and advancements, and the players whose death message names the
// player (a kill, its weapon kept and the other player's name left out). Chat lines (<name> ...) and other players'
// advancements are passed over.
static i32 mc_line(McSeen *s, u32 events, const char *line, bool old, AutoMark *out, i32 cap)
{
    if (const char *u = strstr(line, "]: Setting user: ")) {
        str_set(s->me, u + 17);
        return 0;
    }
    const char *c = strstr(line, "[CHAT] ");
    if (old || !s->me[0] || !c) return 0;
    c += 7;
    const char *sp = strchr(c, ' ');
    if (*c == '<' || *c == '[' || !sp) return 0;
    size_t me = strlen(s->me);
    bool mine = (size_t)(sp - c) == me && !strncmp(c, s->me, me);
    const char *rest = sp + 1;
    i32 n = 0;
    static const char *const adv[] = {"has made the advancement [", "has completed the challenge [", "has reached the goal ["};
    for (i32 i = 0; i < COUNT(adv); i++) {
        size_t l = strlen(adv[i]);
        if (strncmp(rest, adv[i], l)) continue;
        if (mine && (events & AM_ADVANCEMENT)) {
            char note[32];
            i32 k = 0;
            for (const char *p = rest + l; *p && *p != ']' && k < (i32)sizeof note - 1; p++) note[k++] = *p;
            note[k] = 0;
            am_push(out, cap, &n, AM_ADVANCEMENT, 6, note[0] ? note : "Advancement");
        }
        return n;
    }
    if (!mc_death_words(rest)) return 0;
    char d[256];
    if (mine) {
        mc_death_detail(d, rest);
        if (events & AM_DEATH) am_push(out, cap, &n, AM_DEATH, 1, "Death", false, d);
    } else if ((events & AM_KILL) && am_word(rest, s->me)) {
        char said[256], weapon[96] = "";
        mc_death_detail(said, rest); // the weapon the victim's message names is the player's
        marker_detail_get(said, "weapon", weapon, sizeof weapon);
        str_set(d, "");
        marker_detail_add(d, "weapon", weapon);
        am_push(out, cap, &n, AM_KILL, 4, "Kill", false, d);
    }
    return n;
}

struct PoeSeen { char me[64]; }; // the player's character, from its level-ups

// Path of Exile's Client.txt (both games): "<char> (<class>) is now level N" and "<char> has been slain.", after
// the line's "] : ".
static i32 poe_line(PoeSeen *s, u32 events, const char *line, bool old, AutoMark *out, i32 cap)
{
    const char *t = strstr(line, "] : ");
    if (!t) return 0;
    t += 4;
    i32 n = 0;
    if (const char *lv = strstr(t, ") is now level ")) {
        const char *open = strstr(t, " (");
        if (!open || open > lv) return 0;
        i32 k = (i32)(open - t) < (i32)sizeof s->me - 1 ? (i32)(open - t) : (i32)sizeof s->me - 1;
        memcpy(s->me, t, k);
        s->me[k] = 0;
        if (!old && (events & AM_LEVEL)) {
            char note[32];
            snprintf(note, sizeof note, "Level %d", atoi(lv + 15));
            am_push(out, cap, &n, AM_LEVEL, 6, note);
        }
        return n;
    }
    size_t me = strlen(s->me);
    if (!old && me && (events & AM_DEATH) && !strncmp(t, s->me, me) && !strcmp(t + me, " has been slain."))
        am_push(out, cap, &n, AM_DEATH, 1, "Death");
    return n;
}

struct Tf2Seen { char me[64]; }; // the player's name, from the name cvar in config.cfg

// Team Fortress 2's console.log: "<attacker> killed <victim> with <weapon>." (and " (crit)"), "<name> suicided.".
// A kill or death keeps the weapon, never the other player's name.
static i32 tf2_line(Tf2Seen *s, u32 events, const char *line, bool old, AutoMark *out, i32 cap)
{
    size_t me = strlen(s->me);
    if (old || !me) return 0;
    i32 n = 0;
    bool starts = !strncmp(line, s->me, me);
    if (starts && !strcmp(line + me, " suicided.")) {
        if (events & AM_DEATH) am_push(out, cap, &n, AM_DEATH, 1, "Death");
        return n;
    }
    const char *with = strstr(line, " with ");
    if (!with) return 0;
    char weapon[64], d[256] = "";
    snprintf(weapon, sizeof weapon, "%s", with + 6);
    if (char *dot = strchr(weapon, '.')) *dot = 0;
    if (starts && !strncmp(line + me, " killed ", 8)) {
        marker_detail_add(d, "weapon", weapon);
        if (events & AM_KILL) am_push(out, cap, &n, AM_KILL, 4, "Kill", false, d);
        return n;
    }
    const char *k = strstr(line, " killed ");
    if (k && k < with && (size_t)(with - (k + 8)) == me && !strncmp(k + 8, s->me, me) && (events & AM_DEATH)) {
        marker_detail_add(d, "weapon", weapon);
        am_push(out, cap, &n, AM_DEATH, 1, "Death", false, d);
    }
    return n;
}

// Hearthstone's Power.log. Both players' mulligans are logged ("DebugPrintEntityChoices() - id=<n> Player=<name>
// TaskList=... ChoiceType=MULLIGAN"), but only the player's own choice is sent from this PC ("SendChoices() -
// id=<n>"), which names them. A game won: "TAG_CHANGE Entity=<name> tag=PLAYSTATE value=WON", logged by both
// GameState and PowerTaskList, so it is said once per game (CREATE_GAME starts the next).
struct HsSeen {
    char me[64];
    i32 choice;           // the last mulligan choice's id
    char choice_who[64];  // and whose it was
    bool won_said;
};

static i32 hs_line(HsSeen *s, u32 events, const char *line, bool old, AutoMark *out, i32 cap)
{
    i32 n = 0;
    if (const char *c = strstr(line, "DebugPrintEntityChoices() - id=")) {
        const char *p = strstr(c, " Player="), *e = p ? strstr(p, " TaskList=") : 0;
        if (e && strstr(e, "ChoiceType=MULLIGAN") && e - (p + 8) < (i64)sizeof s->choice_who) {
            s->choice = atoi(c + 31);
            memcpy(s->choice_who, p + 8, e - (p + 8));
            s->choice_who[e - (p + 8)] = 0;
        }
    } else if (const char *sc = strstr(line, "SendChoices() - id=")) {
        if (s->choice_who[0] && atoi(sc + 19) == s->choice) str_set(s->me, s->choice_who);
    } else if (strstr(line, "DebugPrintPower() - CREATE_GAME")) {
        s->won_said = false;
    } else if (const char *t = strstr(line, "DebugPrintPower() -")) {
        t = strstr(t, "TAG_CHANGE Entity=");
        size_t me = strlen(s->me);
        const char *v = t && me && !strncmp(t + 18, s->me, me) ? t + 18 + me : 0;
        if (v && !strncmp(v, " tag=PLAYSTATE value=WON", 24) && (u8)v[24] <= ' ' && !s->won_said) {
            s->won_said = true;
            if (!old && (events & AM_WIN)) am_push(out, cap, &n, AM_WIN, 3, "Win");
        }
    }
    return n;
}

// Escape from Tarkov's output log, each line "<date> <time>|<version>|<level>|output|<message>", some followed by
// lines of JSON or of a stack trace. It holds every line of the game's other logs too, in order, as
// "...|output|<channel>|<message>", so it is the only one read. Kills, deaths and extracts aren't logged. The mode:
// "Session mode: Pve" or "Session mode: Regular" (PvP). A raid's map: application's "scene preset
// path:maps/<scene>.bundle" as it loads, then "TRACE-NetworkGameCreate ... Location: <map>, ..." once a server holds
// it (online raids only); its time left: "GameTimer utcNow:<utc> ... escape:<utc>" (less than the whole raid for a
// Scav who joins late). "GameStarted:" starts it. It ends at the first of the OnGameSessionEnd stack line, "Reason:Stop
// game", backend's /client/match/local/end (PvE), the "UserMatchOver" notification and the "payloadType=userMatchOver"
// WebSocket message (PvP). The raid's side: "CompleteSelectedProfile ProfileId:<id>" names the PMC, and a PvP raid's
// profileStatus names the profile in it, a Scav when that isn't the PMC. A Scav raid's start and end are EFT_SCAV, a
// PMC's EFT_PMC. Where the start can't tell (PvE logs no profileStatus), "FinishScavSession" or
// /client/game/profile/savage/regenerate after the raid says it was a Scav's: an amend that gives the raid's start
// side=Scav and its start and end EFT_SCAV. A quest done: backend's response to /client/quest/complete (a chat message
// about a quest may be a reward mail, so it marks nothing). A group joined: "GroupMatchInviteAccept", someone taking
// the player's invite.
struct EftSeen {
    char map[32];   // the raid's map, "" if not known
    char mode[4];   // "PvE" or "PvP", "" if not known
    i32  minutes;   // the raid's time left from its GameTimer, 0 if not known
    char pmc[32];   // the PMC's profile id, "" if not known
    char profile[32]; // the profile in the raid being joined (PvP's profileStatus), "" if not known
    bool scav;      // the raid is a Scav's, as known at its start
    bool in_raid;   // started and not ended
    bool after;     // the raid has ended and its side wasn't known at its start
};

static const u8 EFT_PMC = 5, EFT_SCAV = 2; // the colours of a raid's start and end: blue, orange

// The id after `key` in a line, letters and digits only, into out; out unchanged when the line hasn't it.
static void eft_id(const char *line, const char *key, char (&out)[32])
{
    const char *p = strstr(line, key);
    if (!p) return;
    p += strlen(key);
    size_t l = 0;
    while (l < sizeof out - 1 && isalnum((u8)p[l])) l++;
    memcpy(out, p, l);
    out[l] = 0;
}

// The minutes left in a raid from its GameTimer line's "utcNow:<M/D/Y h:m:s> ... escape:<M/D/Y h:m:s>"; 0 if they
// aren't there or aren't 1 to 300 minutes.
static i32 eft_raid_minutes(const char *line)
{
    const char *a = strstr(line, "utcNow:"), *b = strstr(line, " escape:");
    int m1, d1, y1, h1, n1, s1, m2, d2, y2, h2, n2, s2;
    if (!a || !b || sscanf(a + 7, "%d/%d/%d %d:%d:%d", &m1, &d1, &y1, &h1, &n1, &s1) != 6 ||
        sscanf(b + 8, "%d/%d/%d %d:%d:%d", &m2, &d2, &y2, &h2, &n2, &s2) != 6)
        return 0;
    SYSTEMTIME t1 = {(WORD)y1, (WORD)m1, 0, (WORD)d1, (WORD)h1, (WORD)n1, (WORD)s1, 0};
    SYSTEMTIME t2 = {(WORD)y2, (WORD)m2, 0, (WORD)d2, (WORD)h2, (WORD)n2, (WORD)s2, 0};
    FILETIME f1, f2;
    if (!SystemTimeToFileTime(&t1, &f1) || !SystemTimeToFileTime(&t2, &f2)) return 0;
    i64 secs = (filetime_to_us(f2) - filetime_to_us(f1)) / 1000000;
    i32 min = (i32)((secs + 30) / 60);
    return min >= 1 && min <= 300 ? min : 0;
}

// A map's name from its location id or its scene's bundle name, 0 if not known.
static const char *eft_map(const char *id, size_t len)
{
    static const char *const maps[][2] = {
        {"bigmap", "Customs"}, {"customs", "Customs"}, {"factory", "Factory"}, {"interchange", "Interchange"},
        {"shopping_mall", "Interchange"}, {"laboratory", "The Lab"}, {"labyrinth", "The Labyrinth"},
        {"lighthouse", "Lighthouse"}, {"rezerv", "Reserve"}, {"shoreline", "Shoreline"}, {"woods", "Woods"},
        {"tarkovstreets", "Streets of Tarkov"}, {"city", "Streets of Tarkov"}, {"sandbox", "Ground Zero"}};
    char low[64];
    if (len >= sizeof low) len = sizeof low - 1;
    for (size_t i = 0; i < len; i++) low[i] = (char)tolower((u8)id[i]);
    low[len] = 0;
    for (auto &m : maps)
        if (strstr(low, m[0])) return m[1];
    return 0;
}

static i32 eft_line(EftSeen *s, u32 events, const char *line, bool old, AutoMark *out, i32 cap)
{
    i32 n = 0;
    auto end = [&]() {
        if (!s->in_raid) return;
        s->in_raid = false;
        if (!old && (events & AM_RAID_END)) am_push(out, cap, &n, AM_RAID_END, s->scav ? EFT_SCAV : EFT_PMC, "Raid end", false, "span_end=raid");
    };
    if (const char *g = strstr(line, "Got notification | ")) {
        g += 19;
        if (!strncmp(g, "GroupMatchInviteAccept", 22) && !old && (events & AM_GROUP)) am_push(out, cap, &n, AM_GROUP, 6, "Group joined");
        if (!strncmp(g, "UserMatchOver", 13)) end();
        return n;
    }
    bool head = isdigit((u8)line[0]) && isdigit((u8)line[3]) && line[4] == '-';
    if (!head) { // a line of JSON or of a stack trace
        if (strstr(line, "OnGameSessionEnd(")) end();
        return n;
    }
    if (const char *p = strstr(line, "|scene preset path:maps/")) {
        p += 24;
        size_t l = strcspn(p, ". ");
        const char *m = eft_map(p, l);
        str_set(s->map, m ? m : "");
        s->minutes = 0;
        s->profile[0] = 0;
        s->in_raid = s->after = false;
    } else if (strstr(line, "|CompleteSelectedProfile ProfileId:")) {
        eft_id(line, "ProfileId:", s->pmc);
    } else if (const char *sm = strstr(line, "|Session mode: ")) {
        str_set(s->mode, !strncmp(sm + 15, "Pve", 3) ? "PvE" : !strncmp(sm + 15, "Regular", 7) ? "PvP" : "");
    } else if (strstr(line, "|TRACE-NetworkGameCreate ")) {
        eft_id(line, "Profileid: ", s->profile);
        if (const char *at = strstr(line, "Location: ")) {
            at += 10;
            size_t l = strcspn(at, ",'");
            if (const char *m = eft_map(at, l)) str_set(s->map, m);
            else if (l && l < sizeof s->map) memcpy(s->map, at, l), s->map[l] = 0;
        }
    } else if (strstr(line, "|GameTimer ")) {
        if (i32 m = eft_raid_minutes(line)) s->minutes = m;
    } else if (strstr(line, "|GameStarted:")) {
        bool known = s->pmc[0] && s->profile[0];
        s->scav = known && strcmp(s->pmc, s->profile);
        s->in_raid = true;
        s->after = false;
        if (!old && (events & AM_RAID_START)) {
            char note[64], left[16] = "", detail[256] = "";
            if (s->map[0] && s->minutes) snprintf(note, sizeof note, "Raid start (%s, %d min)", s->map, s->minutes);
            else if (s->map[0]) snprintf(note, sizeof note, "Raid start (%s)", s->map);
            else if (s->minutes) snprintf(note, sizeof note, "Raid start (%d min)", s->minutes);
            else str_set(note, "Raid start");
            if (s->minutes) snprintf(left, sizeof left, "%d min", s->minutes);
            marker_detail_add(detail, "map", s->map);
            marker_detail_add(detail, "mode", s->mode);
            marker_detail_add(detail, "time left", left);
            if (known) marker_detail_add(detail, "side", s->scav ? "Scav" : "PMC");
            marker_detail_add(detail, "span", "raid");
            am_push(out, cap, &n, AM_RAID_START, s->scav ? EFT_SCAV : EFT_PMC, note, false, detail);
        }
        s->minutes = 0;
        s->after = !known; // the side is told after the raid
    } else if (strstr(line, "Reason:Stop game") || strstr(line, "/client/match/local/end") ||
               strstr(line, "payloadType=userMatchOver")) {
        end();
    } else if (strstr(line, "FinishScavSession") || strstr(line, "/client/game/profile/savage/regenerate")) {
        if (s->after) {
            s->after = false;
            if (!old && (events & (AM_RAID_START | AM_RAID_END))) am_push(out, cap, &n, AM_RAID_START, EFT_SCAV, "", true, "side=Scav");
        }
    } else if (strstr(line, "|<--- Response HTTPS, id [") && strstr(line, "/client/quest/complete")) {
        if (!old && (events & AM_QUEST)) am_push(out, cap, &n, AM_QUEST, 3, "Quest done");
    }
    return n;
}

// War Thunder's map server, http://127.0.0.1:8111, while a battle runs. /hudmsg's damage list is the battle's log,
// each {"id": n, "msg": text}: "<who> (<vehicle>) destroyed <who> (<vehicle>)", "... shot down ...", "<who>
// (<vehicle>) has crashed." and the like, a name maybe after a squadron tag ("=TAG= Name"). Nothing there says
// which name is the player's, so the player types it in Options. /mission.json's status goes from "running" to
// "success" or "fail".
struct WtSeen {
    char me[64];
    i32 last;       // the newest damage id read
    bool have;      // a battle's list has been read once (what was in it then is old)
    bool running;   // the mission was running
};

// Whether a "<tag> <name> (" side of a message is the player's: the name before " (" is theirs, after the start
// or a space.
static bool wt_is(const char *side, size_t len, const char *me)
{
    size_t m = strlen(me);
    if (!m || len < m || strncmp(side + len - m, me, m)) return false;
    return len == m || side[len - m - 1] == ' ';
}

static i32 wt_damage(WtSeen *s, u32 events, const char *msg, AutoMark *out, i32 cap)
{
    i32 n = 0;
    const char *open = strstr(msg, " (");
    const char *close = open ? strchr(open, ')') : 0;
    if (!close || close[1] != ' ') return 0;
    bool mine = wt_is(msg, open - msg, s->me);
    const char *rest = close + 2;
    static const char *const kills[] = {"destroyed ", "shot down "};
    for (const char *k : kills) {
        size_t kl = strlen(k);
        if (strncmp(rest, k, kl)) continue;
        const char *them = rest + kl, *t_open = strstr(them, " (");
        bool me_hit = wt_is(them, t_open ? (size_t)(t_open - them) : strlen(them), s->me);
        if (mine && !me_hit && (events & AM_KILL)) am_push(out, cap, &n, AM_KILL, 4, "Kill");
        else if (me_hit && !mine && (events & AM_DEATH)) am_push(out, cap, &n, AM_DEATH, 1, "Death");
        return n;
    }
    if (mine && (!strcmp(rest, "has crashed.") || !strcmp(rest, "has been wrecked.")) && (events & AM_DEATH))
        am_push(out, cap, &n, AM_DEATH, 1, "Death");
    return n;
}

// /hudmsg?lastEvt=0&lastDmg=<last>: the damage it hasn't read. A list from a new battle (ids lower than the last
// one read) is read from its start. The first list read is old: it was there before Rekodr looked.
static i32 wt_hudmsg(WtSeen *s, u32 events, const char *body, i64 len, AutoMark *out, i32 cap)
{
    Json j = {body, body + len};
    if (!j_find(&j, "damage") || !j_eat(&j, '[')) return 0;
    i32 n = 0, top = -1;
    bool fresh = s->have;
    while (j_eat(&j, '{')) {
        i32 id = -1;
        char key[16], msg[512] = "";
        while (!j_eat(&j, '}') && j.p < j.end) {
            if (!j_string(&j, key, sizeof key)) return n;
            j_eat(&j, ':');
            if (!strcmp(key, "msg")) j_string(&j, msg, sizeof msg) || (j_skip(&j), 0);
            else if (!strcmp(key, "id")) {
                j_ws(&j);
                id = j.p < j.end && *j.p >= '0' && *j.p <= '9' ? atoi(j.p) : -1;
                j_skip(&j);
            } else j_skip(&j);
            j_eat(&j, ',');
        }
        j_eat(&j, ',');
        if (id < 0) continue;
        if (top < 0 && id < s->last) s->last = -1; // a new battle's list
        if (id <= s->last) continue;
        if (id > top) top = id;
        if (fresh && msg[0] && s->me[0]) n += wt_damage(s, events, msg, out + n, cap - n);
    }
    if (top > s->last) s->last = top;
    s->have = true;
    return n;
}

// /mission.json: the mission's end, once.
static i32 wt_mission(WtSeen *s, u32 events, const char *body, i64 len, AutoMark *out, i32 cap)
{
    char st[32];
    if (!j_get(body, len, "status", st, sizeof st)) return 0;
    i32 n = 0;
    if (!strcmp(st, "running")) s->running = true;
    else if (s->running && (!strcmp(st, "success") || !strcmp(st, "fail"))) {
        s->running = false, s->last = -1, s->have = false; // the next battle's log starts afresh
        if (events & AM_WIN) am_push(out, cap, &n, AM_WIN, st[0] == 's' ? 3 : 1, st[0] == 's' ? "Mission won" : "Mission lost");
    }
    return n;
}

// ---------------------------------------------------------------------------------------------------------------
// The racing games: what each says of the player's own car

static i32 am_i32(const u8 *p, i64 at)
{
    i32 v;
    memcpy(&v, p + at, 4);
    return v;
}
static f32 am_f32(const u8 *p, i64 at)
{
    f32 v;
    memcpy(&v, p + at, 4);
    return v;
}

// F1 25's UDP telemetry, packet format 2025, little-endian and packed: a 29-byte header (packetFormat u16 at 0,
// packetId u8 at 6, playerCarIndex u8 at 27), and for packetId 3 (Event) a 4-letter code at 29 and its details at
// 33. FTLP fastest lap {vehicleIdx}, OVTK overtake {overtakingVehicleIdx, beingOvertakenVehicleIdx}, COLL collision
// {vehicle1Idx, vehicle2Idx}, PENA penalty {penaltyType, infringementType, vehicleIdx, ...}, RCWN race winner
// {vehicleIdx}. Penalties marked: drive-through (0), stop-go (1), grid (2), time (4), disqualified (6).
static const u16 F1_PORT = 20777;
static i32 f1_packet(u32 events, const u8 *p, i32 len, AutoMark *out, i32 cap)
{
    if (len < 37 || (p[0] | p[1] << 8) != 2025 || p[6] != 3) return 0;
    u8 me = p[27];
    const char *code = (const char *)p + 29;
    const u8 *d = p + 33;
    i32 n = 0;
    if (!memcmp(code, "FTLP", 4) && d[0] == me && (events & AM_LAP)) am_push(out, cap, &n, AM_LAP, 6, "Fastest lap");
    else if (!memcmp(code, "OVTK", 4) && d[0] == me && (events & AM_OVERTAKE)) am_push(out, cap, &n, AM_OVERTAKE, 4, "Overtake");
    else if (!memcmp(code, "COLL", 4) && (d[0] == me || d[1] == me) && (events & AM_INCIDENT)) am_push(out, cap, &n, AM_INCIDENT, 1, "Contact");
    else if (!memcmp(code, "PENA", 4) && d[2] == me && (events & AM_INCIDENT)) {
        static const u8 marked[] = {0, 1, 2, 4, 6};
        for (u8 t : marked)
            if (d[0] == t) am_push(out, cap, &n, AM_INCIDENT, 2, "Penalty");
    } else if (!memcmp(code, "RCWN", 4) && d[0] == me && (events & AM_WIN)) am_push(out, cap, &n, AM_WIN, 3, "Race won");
    return n;
}

// iRacing's telemetry, the memory-mapped file Local\IRSDKMemMapFileName. Its header: status (bit 0: connected) at
// 4, sessionInfoLen 16, sessionInfoOffset 20, numVars 24, varHeaderOffset 28, numBuf 32, and from 48 up to 4
// buffers of {tickCount, bufOffset, pad[2]}; the newest buffer has the highest tickCount. Each var header is 144
// bytes: type 0 (2 int, 4 float), offset 4, name 16 (32 chars). The session info is YAML, its Sessions list holding
// each session's SessionNum and SessionType.
static const wchar_t IR_MAP[] = L"Local\\IRSDKMemMapFileName";
struct IrSeen {
    bool have;
    i32 session, uid;    // SessionNum, SessionUniqueID: another session starts over
    i32 incidents;       // PlayerCarMyIncidentCount
    f32 best;            // LapBestLapTime, s; 0 before a timed lap
    bool won_said;
};

// A var of the newest buffer, by name: false if there is none of that type.
static bool ir_var(const u8 *m, i64 size, i64 buf, const char *name, i32 type, void *out)
{
    i32 nv = am_i32(m, 24), vh = am_i32(m, 28);
    if (nv < 0 || nv > 4096 || vh < 0 || vh + (i64)nv * 144 > size) return false;
    for (i32 i = 0; i < nv; i++) {
        const u8 *h = m + vh + (i64)i * 144;
        if (strncmp((const char *)h + 16, name, 32)) continue;
        i64 at = buf + am_i32(h, 4);
        if (am_i32(h, 0) != type || at < 0 || at + 4 > size) return false;
        memcpy(out, m + at, 4);
        return true;
    }
    return false;
}

// SessionType of session `num` in the session info YAML ("" when not found).
static void ir_session_type(const u8 *m, i64 size, i32 num, char *out, i32 cap)
{
    out[0] = 0;
    i32 len = am_i32(m, 16), at = am_i32(m, 20);
    if (len <= 0 || at < 0 || at + (i64)len > size) return;
    const char *y = (const char *)m + at, *end = y + len;
    char want[32];
    snprintf(want, sizeof want, "SessionNum: %d\n", num);
    i32 wl = (i32)strlen(want);
    for (const char *s = y; s + wl <= end; s++) {
        if (memcmp(s, want, wl)) continue;
        for (const char *t = s + wl; t + 13 <= end && memcmp(t, "SessionNum: ", 12); t++) {
            if (memcmp(t, "SessionType: ", 13)) continue;
            i32 k = 0;
            for (t += 13; t < end && *t != '\n' && *t != '\r' && k < cap - 1; t++) out[k++] = *t;
            out[k] = 0;
            return;
        }
        return;
    }
}

// Incidents, a better best lap, and a race won (the checkered flag or after with the player first).
static i32 ir_marks(IrSeen *s, u32 events, const u8 *m, i64 size, AutoMark *out, i32 cap)
{
    if (size < 112 || !(am_i32(m, 4) & 1)) {
        s->have = false;
        return 0;
    }
    i32 nb = am_i32(m, 32), best_tick = -1;
    i64 buf = -1;
    for (i32 i = 0; i < nb && i < 4; i++)
        if (am_i32(m, 48 + i * 16) > best_tick) best_tick = am_i32(m, 48 + i * 16), buf = am_i32(m, 52 + i * 16);
    i32 session = 0, uid = 0, inc = 0, state = 0, pos = 0;
    f32 best = 0;
    if (buf < 0 || !ir_var(m, size, buf, "SessionNum", 2, &session) || !ir_var(m, size, buf, "PlayerCarMyIncidentCount", 2, &inc))
        return 0;
    ir_var(m, size, buf, "SessionUniqueID", 2, &uid);
    ir_var(m, size, buf, "SessionState", 2, &state);
    ir_var(m, size, buf, "PlayerCarPosition", 2, &pos);
    ir_var(m, size, buf, "LapBestLapTime", 4, &best);
    i32 n = 0;
    if (!s->have || s->session != session || s->uid != uid) {
        *s = {};
    } else {
        if (inc > s->incidents && (events & AM_INCIDENT)) {
            char note[32];
            snprintf(note, sizeof note, "Incident %dx", inc - s->incidents);
            am_push(out, cap, &n, AM_INCIDENT, 1, note);
        }
        if (best > 0 && s->best > 0 && best < s->best - 0.0005f && (events & AM_LAP)) am_push(out, cap, &n, AM_LAP, 6, "Best lap");
        if (state >= 5 && pos == 1 && !s->won_said) {
            char type[32];
            ir_session_type(m, size, session, type, sizeof type);
            if (!strcmp(type, "Race")) {
                s->won_said = true;
                if (events & AM_WIN) am_push(out, cap, &n, AM_WIN, 3, "Race won");
            }
        }
    }
    s->have = true;
    s->session = session, s->uid = uid, s->incidents = inc;
    if (best > 0 && (!s->best || best < s->best)) s->best = best;
    return n;
}

// Assetto Corsa Competizione's shared memory Local\acpmf_graphics: status at 4 (2 = live), session at 8 (2 = race),
// completedLaps 132, position 136, iBestTime 148 (ms).
static const wchar_t ACC_MAP[] = L"Local\\acpmf_graphics";
struct AccSeen {
    bool have;
    i32 session, laps, position, best;
};

// A better best lap, and places gained in a race.
static i32 acc_marks(AccSeen *s, u32 events, const u8 *m, i64 size, AutoMark *out, i32 cap)
{
    if (size < 152 || am_i32(m, 4) != 2) return 0; // paused, a replay or the menus: as it was
    i32 session = am_i32(m, 8), laps = am_i32(m, 132), pos = am_i32(m, 136), best = am_i32(m, 148);
    bool timed = best > 0 && best < 3600000;
    i32 n = 0;
    if (!s->have || session != s->session || laps < s->laps) {
        *s = {};
    } else {
        if (timed && s->best && best < s->best && (events & AM_LAP)) am_push(out, cap, &n, AM_LAP, 6, "Best lap");
        if (session == 2 && s->position > 0 && pos > 0 && pos < s->position && (events & AM_OVERTAKE)) {
            char note[32];
            snprintf(note, sizeof note, "Up to P%d", pos);
            am_push(out, cap, &n, AM_OVERTAKE, 4, note);
        }
    }
    s->have = true;
    s->session = session, s->laps = laps, s->position = pos;
    if (timed) s->best = best;
    return n;
}

// ---------------------------------------------------------------------------------------------------------------
// Rocket League: the Stats API

// The game streams JSON objects one after another over TCP with nothing between them, each
// {"Event": name, "Data": its data}, Data being the data's JSON as a string (or the object itself). The player is
// the one the game is showing: UpdateState's Data.Game.Target while Data.Game.bHasTarget. StatfeedEvent says what
// happened (EventName: Goal, AerialGoal, Save, EpicSave, Assist, Demolish, ...; Type: its label) to whom
// (MainTarget, and for a demolition the car demolished as SecondaryTarget); MatchEnded has WinnerTeamNum.
struct RlSeen {
    char me[64];
    i32 team;
    char part[256 * 1024]; // the object being read
    i32 part_n, depth;
    bool in_str, esc;
};

static i32 rl_message(RlSeen *s, u32 events, const char *msg, i64 len, AutoMark *out, i32 cap)
{
    char ev[32];
    if (!j_get(msg, len, "Event", ev, sizeof ev)) return 0;
    static char data[256 * 1024];
    const char *d = msg;
    i64 dl = len;
    const char *pre = "Data.";
    if (j_get(msg, len, "Data", data, sizeof data)) { // a string: the data's JSON
        d = data, dl = (i64)strlen(data), pre = "";
    }
    char key[64], v[64], name[64];
    auto get = [&](const char *path, char *o, i32 c) {
        snprintf(key, sizeof key, "%s%s", pre, path);
        return j_get(d, dl, key, o, c);
    };
    i32 n = 0;
    if (!strcmp(ev, "UpdateState")) {
        if (get("Game.bHasTarget", v, sizeof v) && !strcmp(v, "true") && get("Game.Target.Name", name, sizeof name)) {
            str_set(s->me, name);
            s->team = get("Game.Target.TeamNum", v, sizeof v) ? atoi(v) : -1;
        }
    } else if (!strcmp(ev, "StatfeedEvent") && s->me[0]) {
        char what[32] = "", label[32] = "", main[64] = "", second[64] = "";
        get("EventName", what, sizeof what);
        get("Type", label, sizeof label);
        get("MainTarget.Name", main, sizeof main);
        get("SecondaryTarget.Name", second, sizeof second);
        bool mine = !strcmp(main, s->me);
        if (!strcmp(what, "Demolish")) {
            if (mine && (events & AM_KILL)) am_push(out, cap, &n, AM_KILL, 4, "Demolition");
            else if (!strcmp(second, s->me) && (events & AM_DEATH)) am_push(out, cap, &n, AM_DEATH, 1, "Demolished");
        } else if (mine && strstr(what, "Goal") && (events & AM_GOAL)) am_push(out, cap, &n, AM_GOAL, 4, label[0] ? label : "Goal");
        else if (mine && strstr(what, "Save") && (events & AM_SAVE)) am_push(out, cap, &n, AM_SAVE, 5, label[0] ? label : "Save");
        else if (mine && !strcmp(what, "Assist") && (events & AM_ASSIST)) am_push(out, cap, &n, AM_ASSIST, 5, "Assist");
    } else if (!strcmp(ev, "MatchEnded")) {
        if (s->me[0] && s->team >= 0 && get("WinnerTeamNum", v, sizeof v) && atoi(v) == s->team && (events & AM_WIN))
            am_push(out, cap, &n, AM_WIN, 3, "Win");
        s->me[0] = 0;
    } else if (!strcmp(ev, "MatchDestroyed")) {
        s->me[0] = 0;
    }
    return n;
}

// Bytes from the socket, split into whole objects by their braces (strings skipped over). A message too big to
// keep is dropped whole.
template <typename F> static void rl_feed(RlSeen *s, const char *b, i32 len, F each)
{
    for (i32 i = 0; i < len; i++) {
        char c = b[i];
        if (!s->depth && c != '{') continue;
        if (s->part_n < (i32)sizeof s->part - 1) s->part[s->part_n] = c;
        s->part_n++;
        if (s->in_str) {
            if (s->esc) s->esc = false;
            else if (c == '\\') s->esc = true;
            else if (c == '"') s->in_str = false;
        } else if (c == '"') s->in_str = true;
        else if (c == '{') s->depth++;
        else if (c == '}' && !--s->depth) {
            if (s->part_n < (i32)sizeof s->part) {
                s->part[s->part_n] = 0;
                each(s->part, (i64)s->part_n);
            }
            s->part_n = 0;
        }
    }
}

// The Stats API's settings, section [TAGame.MatchStatsExporter_TA]: Port (49123) and PacketSendRate, 0 (the
// default) turning the API off. Rekodr sets a rate of RL_RATE when there is none and puts 0 back when it is
// turned off; a rate the player set is left as it is.
static const char RL_SECTION[] = "[TAGame.MatchStatsExporter_TA]";
static const i32 RL_RATE = 30;

// The ini's text with the rate on (none or 0 made RL_RATE) or off (RL_RATE made 0). Returns its length.
static i32 rl_ini(const char *text, bool on, char *out, i32 cap)
{
    const char *eol = strstr(text, "\r\n") ? "\r\n" : "\n";
    i32 n = 0;
    bool in = false, have_section = false, have_rate = false;
    auto put = [&](const char *p, size_t l) {
        if (n + (i32)l < cap) memcpy(out + n, p, l), n += (i32)l;
    };
    auto rate_line = [&](i32 r) {
        char l[40];
        snprintf(l, sizeof l, "PacketSendRate=%d%s", r, eol);
        put(l, strlen(l));
    };
    for (const char *p = text; *p;) {
        const char *e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) + 1 : strlen(p), body = len;
        while (body && (p[body - 1] == '\n' || p[body - 1] == '\r')) body--;
        if (body && p[0] == '[') {
            if (in && !have_rate && on) rate_line(RL_RATE), have_rate = true;
            in = body == strlen(RL_SECTION) && !_strnicmp(p, RL_SECTION, body);
            have_section |= in;
        } else if (in && body > 15 && !_strnicmp(p, "PacketSendRate=", 15)) {
            have_rate = true;
            f64 r = atof(p + 15);
            if (on && r <= 0) { rate_line(RL_RATE); p += len; continue; }
            if (!on && r == RL_RATE) { rate_line(0); p += len; continue; }
        }
        put(p, len);
        if (!e && on && !have_rate && in) put(eol, strlen(eol));
        p += len;
    }
    if (on && in && !have_rate) rate_line(RL_RATE);
    if (on && !have_section) {
        if (n && out[n - 1] != '\n') put(eol, strlen(eol));
        char s[128];
        snprintf(s, sizeof s, "%s%sPort=49123%sPacketSendRate=%d%s", RL_SECTION, eol, eol, RL_RATE, eol);
        put(s, strlen(s));
    }
    out[n < cap ? n : cap - 1] = 0;
    return n;
}

// The Port in the ini's section, 49123 when it doesn't say.
static u16 rl_port(const char *text)
{
    bool in = false;
    for (const char *p = text; p && *p; p = strchr(p, '\n') ? strchr(p, '\n') + 1 : 0) {
        if (*p == '[') in = !_strnicmp(p, RL_SECTION, strlen(RL_SECTION));
        else if (in && !_strnicmp(p, "Port=", 5) && atoi(p + 5) > 0 && atoi(p + 5) < 65536) return (u16)atoi(p + 5);
    }
    return 49123;
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

// A cfg file's text, with the token the game sends back so the listener knows the state is the game's. ASCII, no
// byte order mark. The heartbeat also sets how soon the game sends the player's state on joining a match.
static i32 gsi_cfg_text(const AmGsi &g, const char *token, char *out, i32 cap)
{
    i32 n = snprintf(out, cap,
                     "\"" APP_NAME "\"\n"
                     "{\n"
                     "    \"uri\"       \"http://127.0.0.1:%u/\"\n"
                     "    \"timeout\"   \"1.1\"\n"
                     "    \"buffer\"    \"0.1\"\n"
                     "    \"throttle\"  \"0.1\"\n"
                     "    \"heartbeat\" \"5.0\"\n"
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
// Following a log

// A log being followed. A new file (another path, another file at the same path, or one shorter than where it was
// read up to) has its last AM_TAIL_BACK bytes read as old lines, then is followed from there.
static const i64 AM_TAIL_BACK = 2 << 20;
struct AmTail {
    wchar_t path[MAX_PATH];
    DWORD index_hi, index_lo; // the file's id: a new latest.log at the same path is another file
    i64 at;                   // read up to here
    char part[1024];          // a line not finished yet
    i32 part_n;
};

// Calls each(line, old) for every whole line. False when the file isn't there.
template <typename F> static bool am_tail(AmTail *t, const wchar_t *path, F each)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0, OPEN_EXISTING, 0, 0);
    if (f == INVALID_HANDLE_VALUE) {
        t->path[0] = 0;
        return false;
    }
    BY_HANDLE_FILE_INFORMATION info = {};
    GetFileInformationByHandle(f, &info);
    i64 size = ((i64)info.nFileSizeHigh << 32) | info.nFileSizeLow;
    i64 old_end = -1;
    if (wcscmp(t->path, path) || info.nFileIndexHigh != t->index_hi || info.nFileIndexLow != t->index_lo || size < t->at) {
        wcscpy_s(t->path, path);
        t->index_hi = info.nFileIndexHigh;
        t->index_lo = info.nFileIndexLow;
        t->at = size > AM_TAIL_BACK ? size - AM_TAIL_BACK : 0;
        t->part_n = 0;
        old_end = size;
        if (t->at) t->part_n = -1; // the first line is cut: passed over
    }
    static char buf[64 * 1024];
    while (t->at < size) {
        LARGE_INTEGER pos;
        pos.QuadPart = t->at;
        SetFilePointerEx(f, pos, 0, FILE_BEGIN);
        DWORD want = (DWORD)(size - t->at < (i64)sizeof buf ? size - t->at : (i64)sizeof buf), got = 0;
        if (!ReadFile(f, buf, want, &got, 0) || !got) break;
        for (DWORD i = 0; i < got; i++) {
            char ch = buf[i];
            if (ch == '\n') {
                if (t->part_n > 0 && t->part[t->part_n - 1] == '\r') t->part_n--;
                if (t->part_n >= 0) {
                    t->part[t->part_n] = 0;
                    each(t->part, t->at + (i64)i < old_end);
                }
                t->part_n = 0;
            } else if (t->part_n >= 0 && t->part_n < (i32)sizeof t->part - 1) {
                t->part[t->part_n++] = ch;
            }
        }
        t->at += got;
    }
    CloseHandle(f);
    return true;
}

// Path of Exile's Client.txt: under its Steam folder, else Grinding Gear Games' own install. "" when neither is
// there.
static void poe_log_path(u32 app, const wchar_t *dir, wchar_t *out)
{
    out[0] = 0;
    Path lib;
    wchar_t p[MAX_PATH];
    if (steam_app_library(app, &lib)) {
        swprintf(p, MAX_PATH, L"%s\\common\\%s\\logs", lib.s, dir);
        if (GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES) {
            swprintf(out, MAX_PATH, L"%s\\Client.txt", p);
            return;
        }
    }
    wchar_t pf[MAX_PATH];
    if (!GetEnvironmentVariableW(L"ProgramFiles(x86)", pf, MAX_PATH)) return;
    swprintf(p, MAX_PATH, L"%s\\Grinding Gear Games\\%s\\logs", pf, dir);
    if (GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES) swprintf(out, MAX_PATH, L"%s\\Client.txt", p);
}

// Team Fortress 2's tf folder, "" when it isn't installed.
static void tf2_dir(wchar_t *out)
{
    out[0] = 0;
    Path lib;
    if (!steam_app_library(440, &lib)) return;
    wchar_t p[MAX_PATH];
    swprintf(p, MAX_PATH, L"%s\\common\\Team Fortress 2\\tf\\cfg", lib.s);
    if (GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES) swprintf(out, MAX_PATH, L"%s\\common\\Team Fortress 2\\tf", lib.s);
}

// The line Rekodr adds to Team Fortress 2's autoexec.cfg so the game writes its console to tf\console.log.
static const char TF2_LINE[] = "con_logfile console.log // " APP_NAME " reads it for markers";

// autoexec.cfg's text with Rekodr's line put in (on) or taken out, every other line as it was. Returns its length.
static i32 tf2_autoexec(const char *text, bool on, char *out, i32 cap)
{
    i32 n = 0;
    bool have = false;
    for (const char *p = text; *p;) {
        const char *e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) : strlen(p), body = len && p[len - 1] == '\r' ? len - 1 : len;
        bool ours = body == strlen(TF2_LINE) && !strncmp(p, TF2_LINE, body);
        have |= ours;
        if ((!ours || on) && n + (i32)len + 1 < cap) {
            memcpy(out + n, p, len);
            n += (i32)len;
            if (e) out[n++] = '\n';
        }
        p += len + (e ? 1 : 0);
    }
    if (on && !have) {
        if (n && out[n - 1] != '\n' && n + 1 < cap) out[n++] = '\n';
        n += snprintf(out + n, cap - n, "%s\n", TF2_LINE);
    }
    out[n] = 0;
    return n;
}

// Writes or takes out Rekodr's line in autoexec.cfg; a file left with nothing in it goes. A test copy keeps its file
// in its own home. False if the game isn't installed.
static bool am_tf2_cfg(const wchar_t *tf, bool on)
{
    Path cfg;
    if (is_test_copy()) {
        Path dir = path_join(g_app.paths.local_dir, L"gsicfg");
        CreateDirectoryW(dir.s, 0);
        cfg = path_join(dir, L"tf2_autoexec.cfg");
    } else {
        if (!tf[0]) return false;
        swprintf(cfg.s, COUNT(cfg.s), L"%s\\cfg\\autoexec.cfg", tf);
    }
    i64 size = 0;
    char *text = read_file(cfg.s, &size);
    if (!text && !on) return true;
    static char out[256 * 1024];
    if (size >= (i64)sizeof out - 256) {
        free(text);
        log_line("markers: Team Fortress 2's autoexec.cfg is too big to change");
        return true;
    }
    i32 n = tf2_autoexec(text ? text : "", on, out, sizeof out);
    bool same = text && n == size && !memcmp(text, out, n);
    free(text);
    if (same) return true;
    bool empty = true;
    for (i32 i = 0; i < n; i++) empty &= out[i] == ' ' || out[i] == '\t' || out[i] == '\r' || out[i] == '\n';
    if (empty) {
        DeleteFileW(cfg.s);
    } else if (FILE *f = _wfopen(cfg.s, L"wb")) {
        fwrite(out, 1, n, f);
        fclose(f);
    }
    log_line("markers: Team Fortress 2's autoexec.cfg %s", on ? "has Rekodr's line" : "no longer has Rekodr's line");
    return true;
}

// The name cvar in Team Fortress 2's config.cfg: name "<the player>".
static void tf2_name(const wchar_t *tf, char *out, i32 cap)
{
    out[0] = 0;
    wchar_t p[MAX_PATH];
    swprintf(p, MAX_PATH, L"%s\\cfg\\config.cfg", tf);
    char *text = read_file(p, 0);
    if (!text) return;
    for (const char *l = text; l; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : 0) {
        if (strncmp(l, "name \"", 6)) continue;
        i32 k = 0;
        for (const char *c = l + 6; *c && *c != '"' && k < cap - 1; c++) out[k++] = *c;
        out[k] = 0;
        break;
    }
    free(text);
}

// Rocket League's install folder, from Steam or the Epic launcher's manifests. "" if it isn't installed.
static void rl_dir(wchar_t *out)
{
    out[0] = 0;
    Path lib;
    if (steam_app_library(252950, &lib)) {
        swprintf(out, MAX_PATH, L"%s\\common\\rocketleague", lib.s);
        if (GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES) return;
        out[0] = 0;
    }
    wchar_t data[MAX_PATH];
    DWORD dn = GetEnvironmentVariableW(L"ProgramData", data, COUNT(data));
    if (!dn || dn >= COUNT(data)) return;
    Path dir = {};
    swprintf(dir.s, MAX_PATH, L"%ls\\Epic\\EpicGamesLauncher\\Data\\Manifests", data);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(path_join(dir, L"*.item").s, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.nFileSizeHigh || fd.nFileSizeLow > 1024 * 1024) continue;
        i64 len = 0;
        char *t = read_file(path_join(dir, fd.cFileName).s, &len);
        if (!t) continue;
        char name[128], at[MAX_PATH * 3];
        if (len > 0 && j_get(t, len, "DisplayName", name, sizeof name) && !strcmp(name, "Rocket League") &&
            j_get(t, len, "InstallLocation", at, sizeof at) && at[0]) {
            utf8_to_wide(at, out, MAX_PATH);
            if (GetFileAttributesW(out) == INVALID_FILE_ATTRIBUTES) out[0] = 0;
        }
        free(t);
    } while (!out[0] && FindNextFileW(h, &fd));
    FindClose(h);
}

// Turns the Stats API on or back off: the game's DefaultStatsAPI.ini, and the player's own TAStatsAPI.ini (Documents\My
// Games), which the game reads first, when there is one. A file in local_dir says Rekodr turned it on, so only then
// is it turned off. A test copy changes a copy in its own home. Returns the port the game sends on, 0 if the game
// isn't installed. The game reads the files when it starts.
static u16 am_rl_cfg(const wchar_t *dir, bool on)
{
    Path files[2];
    i32 nf = 0;
    if (is_test_copy()) {
        Path d = path_join(g_app.paths.local_dir, L"gsicfg");
        CreateDirectoryW(d.s, 0);
        files[nf++] = path_join(d, L"rl_DefaultStatsAPI.ini");
    } else {
        if (!dir[0]) return 0;
        swprintf(files[nf].s, COUNT(files[nf].s), L"%s\\TAGame\\Config", dir);
        if (GetFileAttributesW(files[nf].s) == INVALID_FILE_ATTRIBUTES) return 0;
        wcscat_s(files[nf++].s, L"\\DefaultStatsAPI.ini");
        PWSTR docs = 0;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, 0, &docs))) {
            swprintf(files[nf].s, COUNT(files[nf].s), L"%s\\My Games\\Rocket League\\TAGame\\Config\\TAStatsAPI.ini", docs);
            if (GetFileAttributesW(files[nf].s) != INVALID_FILE_ATTRIBUTES) nf++;
        }
        CoTaskMemFree(docs);
    }
    Path flag = path_join(g_app.paths.local_dir, L"rl_statsapi_on");
    bool ours = GetFileAttributesW(flag.s) != INVALID_FILE_ATTRIBUTES;
    u16 port = 49123;
    bool changed = false;
    for (i32 i = 0; i < nf; i++) {
        i64 size = 0;
        char *text = read_file(files[i].s, &size);
        static char out[64 * 1024];
        if (size >= (i64)sizeof out - 256) {
            free(text);
            continue;
        }
        if (on || ours) {
            i32 n = rl_ini(text ? text : "", on, out, sizeof out);
            if (!(text && n == size && !memcmp(text, out, n)) && (text || on)) {
                if (FILE *f = _wfopen(files[i].s, L"wb")) {
                    fwrite(out, 1, n, f);
                    fclose(f);
                    changed = true;
                }
            }
            if (i == nf - 1) port = rl_port(out);
        } else if (text && i == nf - 1) port = rl_port(text);
        free(text);
    }
    if (on && changed && !ours) {
        if (FILE *f = _wfopen(flag.s, L"wb")) fclose(f);
        log_line("markers: Rocket League's Stats API turned on; the game reads it when it starts");
    } else if (!on && ours) {
        DeleteFileW(flag.s);
        log_line("markers: Rocket League's Stats API turned back off");
    }
    return port;
}

// Hearthstone writes its Power.log only with a [Power] section in %LOCALAPPDATA%\Blizzard\Hearthstone\log.config,
// read when it starts. Rekodr adds the section when there is none, leaving a file in local_dir that says so, and
// takes out only that section, only then.
static const char HS_SECTION[] = "[Power]";
static const char HS_KEYS[] = "LogLevel=1\nFilePrinting=true\nConsolePrinting=false\nScreenPrinting=false\nVerbose=true\n";

// log.config's text with Rekodr's [Power] section put in (on, when there is no [Power]) or taken out. Returns its
// length.
static i32 hs_log_config(const char *text, bool on, char *out, i32 cap)
{
    i32 n = 0;
    bool have = false, skip = false;
    for (const char *p = text; *p;) {
        const char *e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) + 1 : strlen(p), body = len;
        while (body && (p[body - 1] == '\n' || p[body - 1] == '\r')) body--;
        if (body && p[0] == '[') {
            bool power = body == strlen(HS_SECTION) && !_strnicmp(p, HS_SECTION, body);
            have |= power;
            skip = power && !on;
        }
        if (!skip && n + (i32)len < cap) memcpy(out + n, p, len), n += (i32)len;
        p += len;
    }
    if (on && !have) {
        if (n && out[n - 1] != '\n' && n + 1 < cap) out[n++] = '\n';
        n += snprintf(out + n, cap - n, "%s\n%s", HS_SECTION, HS_KEYS);
    }
    if (n >= cap) n = cap - 1;
    out[n] = 0;
    return n;
}

// Puts Rekodr's section in or takes it out (only when Rekodr put it there); a file left empty goes. A test copy
// keeps its file in its own home.
static void am_hs_cfg(bool on)
{
    Path cfg;
    if (is_test_copy()) {
        Path d = path_join(g_app.paths.local_dir, L"gsicfg");
        CreateDirectoryW(d.s, 0);
        cfg = path_join(d, L"hs_log.config");
    } else {
        PWSTR local = 0;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, 0, &local))) return;
        swprintf(cfg.s, COUNT(cfg.s), L"%s\\Blizzard\\Hearthstone", local);
        CoTaskMemFree(local);
        if (on) CreateDirectoryW(cfg.s, 0);
        wcscat_s(cfg.s, L"\\log.config");
    }
    Path flag = path_join(g_app.paths.local_dir, L"hs_logconfig_on");
    bool ours = GetFileAttributesW(flag.s) != INVALID_FILE_ATTRIBUTES;
    if (!on && !ours) return;
    i64 size = 0;
    char *text = read_file(cfg.s, &size);
    static char out[64 * 1024];
    if (size >= (i64)sizeof out - 256) {
        free(text);
        return;
    }
    i32 n = hs_log_config(text ? text : "", on, out, sizeof out);
    bool same = text && n == size && !memcmp(text, out, n);
    free(text);
    if (!same) {
        bool empty = true;
        for (i32 i = 0; i < n; i++) empty &= out[i] == ' ' || out[i] == '\t' || out[i] == '\r' || out[i] == '\n';
        if (empty) DeleteFileW(cfg.s);
        else if (FILE *f = _wfopen(cfg.s, L"wb")) fwrite(out, 1, n, f), fclose(f);
    }
    if (on && !same && !ours) {
        if (FILE *f = _wfopen(flag.s, L"wb")) fclose(f);
        log_line("markers: Hearthstone's log.config has Rekodr's [Power] section; the game reads it when it starts");
    } else if (!on) {
        DeleteFileW(flag.s);
        log_line("markers: Hearthstone's log.config no longer has Rekodr's [Power] section");
    }
}

// Hearthstone's install folder, from its uninstall entry (Battle.net's), else the usual place. "" if not there.
static void hs_dir(wchar_t *out)
{
    out[0] = 0;
    DWORD bytes = MAX_PATH * sizeof(wchar_t);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Hearthstone",
                     L"InstallLocation", RRF_RT_REG_SZ, 0, out, &bytes) != ERROR_SUCCESS || !out[0]) {
        wchar_t pf[MAX_PATH];
        if (!GetEnvironmentVariableW(L"ProgramFiles(x86)", pf, MAX_PATH)) return;
        swprintf(out, MAX_PATH, L"%s\\Hearthstone", pf);
    }
    size_t l = wcslen(out);
    while (l && out[l - 1] == L'\\') out[--l] = 0;
    if (GetFileAttributesW(out) == INVALID_FILE_ATTRIBUTES) out[0] = 0;
}

// The Power.log of the game's newest run: <install>\Logs\Hearthstone_<date and time>\Power.log. "" if none.
static void hs_log_path(const wchar_t *dir, wchar_t *out)
{
    out[0] = 0;
    wchar_t pat[MAX_PATH], best[MAX_PATH] = L"";
    swprintf(pat, MAX_PATH, L"%s\\Logs\\Hearthstone_*", dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && wcscmp(fd.cFileName, best) > 0) wcscpy_s(best, fd.cFileName);
    while (FindNextFileW(h, &fd));
    FindClose(h);
    if (best[0]) swprintf(out, MAX_PATH, L"%s\\Logs\\%s\\Power.log", dir, best);
}

// Escape from Tarkov's folder that holds its Logs: Battlestate Games' install (its uninstall entry), else Steam's
// <library>\common\<dir>\build. "" when neither is there.
static void eft_dir(wchar_t *out)
{
    out[0] = 0;
    DWORD bytes = MAX_PATH * sizeof(wchar_t);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\EscapeFromTarkov",
                     L"InstallLocation", RRF_RT_REG_SZ, 0, out, &bytes) == ERROR_SUCCESS && out[0]) {
        size_t l = wcslen(out);
        while (l && out[l - 1] == L'\\') out[--l] = 0;
        if (GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES) return;
    }
    out[0] = 0;
    Path lib;
    if (!steam_app_library(3932890, &lib)) return;
    wchar_t p[MAX_PATH];
    swprintf(p, MAX_PATH, L"%s\\appmanifest_3932890.acf", lib.s);
    char *text = read_file(p, 0), dir[MAX_PATH];
    bool ok = text && vdf_value(text, "installdir", dir, sizeof dir) && dir[0];
    free(text);
    if (!ok) return;
    wchar_t wd[MAX_PATH];
    utf8_to_wide(dir, wd, MAX_PATH);
    swprintf(out, MAX_PATH, L"%s\\common\\%s\\build", lib.s, wd);
    if (GetFileAttributesW(out) == INVALID_FILE_ATTRIBUTES) out[0] = 0;
}

// The logs of a run of Escape from Tarkov, by what each file's name ends in: "<name>.log" or "<name>_<n>.log",
// after the name's start, a space or a '_'.
static const char *const EFT_LOGS[] = {"output"}; // it holds the other logs' lines too (eft_line)
enum { EFT_LOG_COUNT = COUNT(EFT_LOGS) };

// Which of EFT_LOGS a file is, -1 for none.
static i32 eft_log_kind(const char *file)
{
    size_t l = strlen(file);
    if (l < 4 || _stricmp(file + l - 4, ".log")) return -1;
    l -= 4;
    size_t d = l;
    while (d && isdigit((u8)file[d - 1])) d--;
    if (d < l && d && file[d - 1] == '_') l = d - 1;
    for (i32 i = 0; i < EFT_LOG_COUNT; i++) {
        size_t k = strlen(EFT_LOGS[i]);
        if (l >= k && !_strnicmp(file + l - k, EFT_LOGS[i], k) && (l == k || file[l - k - 1] == ' ' || file[l - k - 1] == '_'))
            return i;
    }
    return -1;
}

// Each of EFT_LOGS in the newest run's folder, <dir>\Logs\log_<date and time>_<version>, the last written of each
// kind; "" for one not there. The newest folder is the last made, as its name's hour has no leading 0.
static void eft_log_paths(const wchar_t *dir, wchar_t out[][MAX_PATH])
{
    for (i32 i = 0; i < EFT_LOG_COUNT; i++) out[i][0] = 0;
    wchar_t pat[MAX_PATH], run[MAX_PATH] = L"";
    FILETIME made = {};
    swprintf(pat, MAX_PATH, L"%s\\Logs\\log_*", dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && CompareFileTime(&fd.ftCreationTime, &made) > 0) {
            made = fd.ftCreationTime;
            wcscpy_s(run, fd.cFileName);
        }
    while (FindNextFileW(h, &fd));
    FindClose(h);
    if (!run[0]) return;
    swprintf(pat, MAX_PATH, L"%s\\Logs\\%s\\*.log", dir, run);
    FILETIME written[EFT_LOG_COUNT] = {};
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        char name[MAX_PATH];
        wide_to_utf8(fd.cFileName, name, sizeof name);
        i32 k = eft_log_kind(name);
        if (k < 0 || (out[k][0] && CompareFileTime(&fd.ftLastWriteTime, &written[k]) < 0)) continue;
        written[k] = fd.ftLastWriteTime;
        swprintf(out[k], MAX_PATH, L"%s\\Logs\\%s\\%s", dir, run, fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
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
    volatile i64 gsi_dump_until;     // until then each state a game sends is kept in gsi-dump.txt ("gsi-dump")
    Cs2Seen cs2;
    DotaSeen dota;
    LolSeen lol;
    McSeen mc;
    PoeSeen poe[2];                  // Path of Exile, Path of Exile 2
    Tf2Seen tf2;
    IrSeen ir;
    AccSeen acc;
    RlSeen rl;
    WtSeen wt;
    char wt_me[64];                  // the War Thunder name typed in Options (main thread sets it)
    bool wt_running;
    i64 wt_next;
    HsSeen hs;
    AmTail hs_tail;
    wchar_t hs_dir[MAX_PATH];        // Hearthstone's install; "" when it isn't installed (main thread sets it)
    wchar_t hs_log[MAX_PATH];        // the newest run's Power.log; "" until found
    i64 hs_next;                     // when to look for a newer one
    EftSeen eft;
    AmTail eft_tail[EFT_LOG_COUNT];
    wchar_t eft_dir[MAX_PATH];       // Escape from Tarkov's folder; "" when it isn't installed (main thread sets it)
    wchar_t eft_log[EFT_LOG_COUNT][MAX_PATH]; // the newest run's logs; "" until found
    i64 eft_next;                    // when to look for newer ones
    volatile LONG rl_port;           // the port Rocket League's Stats API sends on (main thread sets it)
    AmTail mc_tail, poe_tail[2], tf2_tail;
    wchar_t mc_log[MAX_PATH];        // the running Minecraft's latest.log; "" until found
    i64 mc_next;                     // when to look for it again
    wchar_t poe_log[2][MAX_PATH];    // each Client.txt; "" when the game isn't installed (main thread sets them)
    wchar_t tf2_dir[MAX_PATH];       // the tf folder; "" when it isn't installed (main thread sets it)
    i64 last_kill[AMG_COUNT];        // the last kill marker of each (its time), for an amend
    i64 last_start[AMG_COUNT];       // the last marker of each that starts a band (span=), for an amend with a detail
    i64 last_end[AMG_COUNT];         // the last marker of each that ends one (span_end=)
} g_am;

static void am_put(const AutoMark *marks, i32 n, i32 game)
{
    for (i32 i = 0; i < n; i++) {
        const AutoMark &m = marks[i];
        i64 *last_kill = &g_am.last_kill[game];
        char k[32], v[64];
        if (m.amend && m.detail[0]) { // the last band: its start's detail, its start's and end's colour
            if (i64 t = g_am.last_start[game]) {
                for (const char *at = m.detail; marker_detail_next(&at, k, sizeof k, v, sizeof v);) markers_detail_put(t, k, v);
                markers_colour_set(t, m.colour);
                if (g_am.last_end[game] > t) markers_colour_set(g_am.last_end[game], m.colour);
            }
        } else if (m.amend && *last_kill) { // a kill that turns out to be a multi-kill
            markers_set(*last_kill, m.colour, m.note);
            for (i32 b = 0; b < COUNT(AM_EVENT_WORDS); b++)
                if (m.event == 1u << b) markers_detail_put(*last_kill, "event", AM_EVENT_WORDS[b]);
        }
        else if (!m.amend) {
            char d[256];
            marker_detail_event(d, m.event, m.detail);
            i64 t = markers_add(now_us(), m.colour, m.note, d);
            if (m.colour == 4) *last_kill = t;
            if (marker_detail_get(m.detail, "span", v, sizeof v)) g_am.last_start[game] = t;
            if (marker_detail_get(m.detail, "span_end", v, sizeof v)) g_am.last_end[game] = t;
        }
        log_line("markers: %s: %s", AM_GAMES[game].name, m.note[0] ? m.note : m.detail);
    }
    if (n) event_post(Event{}); // the timeline shows it
}

// A state as it came, at the end of gsi-dump.txt in local_dir, after a line with the time.
static void am_gsi_dump(const char *body, i32 len)
{
    Path p = path_join(g_app.paths.local_dir, L"gsi-dump.txt");
    FILE *f = _wfopen(p.s, L"ab");
    if (!f) return;
    fprintf(f, "--- %lld\n", (long long)now_us());
    fwrite(body, 1, (size_t)len, f);
    fputs("\n", f);
    fclose(f);
}

// The pipe's "gsi-dump": the states games send in the next ten minutes, kept as they come (am_gsi_dump).
static void automarks_gsi_dump()
{
    g_am.gsi_dump_until = now_us() + 600000000;
    log_line("markers: keeping game states in gsi-dump.txt for ten minutes");
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
    if (now_us() < g_am.gsi_dump_until) am_gsi_dump(body, body_len);
    AutoMark marks[16];
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
static bool am_local_get(HINTERNET session, u16 port, bool secure, const wchar_t *path, char *out, i32 cap, i32 *len)
{
    *len = 0;
    HINTERNET conn = WinHttpConnect(session, L"127.0.0.1", port, 0);
    HINTERNET req = conn ? WinHttpOpenRequest(conn, L"GET", path, 0, 0, 0, secure ? WINHTTP_FLAG_SECURE : 0) : 0;
    bool ok = false;
    if (req) {
        if (secure) {
            DWORD flags = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_CN_INVALID | SECURITY_FLAG_IGNORE_CERT_DATE_INVALID;
            WinHttpSetOption(req, WINHTTP_OPTION_SECURITY_FLAGS, &flags, sizeof flags);
        }
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
        if (!am_local_get(session, 2999, true, L"/liveclientdata/activeplayername", buf, sizeof buf, &len)) {
            g_am.lol.have = false;
            return;
        }
        Json j = {buf, buf + len};
        if (!j_string(&j, g_am.lol.me, sizeof g_am.lol.me)) return;
        log_line("markers: League match as %s", g_am.lol.me);
    }
    if (!am_local_get(session, 2999, true, L"/liveclientdata/eventdata", buf, sizeof buf, &len)) {
        g_am.lol = {};
        return;
    }
    AutoMark marks[16];
    i32 n = lol_marks(&g_am.lol, g_am.events[AMG_LOL], buf, len, marks, COUNT(marks));
    am_put(marks, n, AMG_LOL);
}

// War Thunder's battle, once a second while aces.exe runs (looked for every 5 s).
static void am_wt_poll(HINTERNET session)
{
    WtSeen &s = g_am.wt;
    i64 now = now_us();
    if (now >= g_am.wt_next) {
        static char cmd[4096];
        char title[8];
        g_am.wt_running = process_facts("aces.exe", cmd, sizeof cmd, title, sizeof title);
        g_am.wt_next = now + 5000000;
    }
    str_set(s.me, (const char *)g_am.wt_me);
    if (!g_am.wt_running || !s.me[0]) {
        s.have = s.running = false, s.last = -1;
        return;
    }
    static char buf[256 * 1024];
    i32 len = 0;
    AutoMark marks[16];
    u32 ev = g_am.events[AMG_WAR_THUNDER];
    wchar_t path[64];
    swprintf(path, COUNT(path), L"/hudmsg?lastEvt=0&lastDmg=%d", s.last < 0 ? 0 : s.last);
    if (!am_local_get(session, 8111, false, path, buf, sizeof buf, &len)) {
        s.have = s.running = false, s.last = -1;
        return;
    }
    am_put(marks, wt_hudmsg(&s, ev, buf, len, marks, COUNT(marks)), AMG_WAR_THUNDER);
    if (am_local_get(session, 8111, false, L"/mission.json", buf, sizeof buf, &len))
        am_put(marks, wt_mission(&s, ev, buf, len, marks, COUNT(marks)), AMG_WAR_THUNDER);
}

// Minecraft's latest.log: in the running game's folder (mc_game_dir). Looked for every 5 s, as another instance
// started is another folder.
static void mc_log_path(wchar_t *out)
{
    out[0] = 0;
    static char cmd[32768];
    char title[8], dir[MAX_PATH * 2], cwd[MAX_PATH * 2];
    static const char *const exes[] = {"javaw.exe", "java.exe"};
    for (const char *exe : exes) {
        if (!process_facts(exe, cmd, sizeof cmd, title, sizeof title) || !strstr(cmd, "minecraft")) continue;
        process_cwd(exe, cwd, sizeof cwd);
        if (!mc_game_dir(cmd, cwd, dir, sizeof dir)) continue;
        wchar_t w[MAX_PATH];
        utf8_to_wide(dir, w, MAX_PATH);
        swprintf(out, MAX_PATH, L"%s\\logs\\latest.log", w);
        return;
    }
}

// The player's name from the start of a latest.log, where "Setting user:" is: a modded game's log grows past
// AM_TAIL_BACK, so one found late (Rekodr started while it ran) has it before the part read.
static void mc_me_from_head(const wchar_t *path, McSeen *s)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0, OPEN_EXISTING, 0, 0);
    if (f == INVALID_HANDLE_VALUE) return;
    static char buf[1 << 20];
    DWORD got = 0;
    if (!ReadFile(f, buf, sizeof buf - 1, &got, 0)) got = 0;
    CloseHandle(f);
    buf[got] = 0;
    for (DWORD i = 0; i < got; i++) if (!buf[i]) buf[i] = ' ';
    const char *u = strstr(buf, "]: Setting user: ");
    if (!u) return;
    u += 17;
    i32 n = 0;
    while (u[n] && u[n] != '\r' && u[n] != '\n' && n < (i32)sizeof s->me - 1) n++;
    snprintf(s->me, sizeof s->me, "%.*s", n, u);
}

static void replays_poll(); // replays.cpp
static void replays_apply();
static void rust_poll(); // rustlog.cpp
static void rust_apply();
static void sc_poll(); // sclog.cpp
static void eftkills_poll(); // eftkills.cpp
static void eftkills_apply(const wchar_t *eft_dir);

// Once a second: the lines each log game on has added, and the replays games have finished.
static void am_logs_poll()
{
    AutoMark marks[8];
    auto feed = [&](i32 game, i32 n) { am_put(marks, n, game); };
    if (u32 ev = g_am.events[AMG_MINECRAFT]) {
        i64 now = now_us();
        if (now >= g_am.mc_next) {
            wchar_t p[MAX_PATH];
            mc_log_path(p);
            if (p[0] && wcscmp(p, g_am.mc_log)) {
                g_am.mc = {}, g_am.mc_tail = {}, wcscpy_s(g_am.mc_log, p);
                mc_me_from_head(p, &g_am.mc);
            }
            g_am.mc_next = now + 5000000;
        }
        if (g_am.mc_log[0] && !am_tail(&g_am.mc_tail, g_am.mc_log, [&](const char *l, bool old) {
                feed(AMG_MINECRAFT, mc_line(&g_am.mc, ev, l, old, marks, COUNT(marks)));
            }))
            g_am.mc_log[0] = 0;
    }
    static const i32 poe_games[] = {AMG_POE, AMG_POE2};
    for (i32 i = 0; i < 2; i++) {
        i32 g = poe_games[i];
        u32 ev = g_am.events[g];
        if (ev && g_am.poe_log[i][0])
            am_tail(&g_am.poe_tail[i], g_am.poe_log[i], [&](const char *l, bool old) {
                feed(g, poe_line(&g_am.poe[i], ev, l, old, marks, COUNT(marks)));
            });
    }
    if (u32 ev = g_am.events[AMG_TF2]) {
        if (g_am.tf2_dir[0]) {
            wchar_t p[MAX_PATH];
            swprintf(p, MAX_PATH, L"%s\\console.log", g_am.tf2_dir);
            bool was = g_am.tf2_tail.path[0] != 0;
            am_tail(&g_am.tf2_tail, p, [&](const char *l, bool old) {
                if (!was && !g_am.tf2.me[0]) tf2_name(g_am.tf2_dir, g_am.tf2.me, sizeof g_am.tf2.me);
                was = true;
                feed(AMG_TF2, tf2_line(&g_am.tf2, ev, l, old, marks, COUNT(marks)));
            });
        }
    }
    if (u32 ev = g_am.events[AMG_HEARTHSTONE]) {
        if (g_am.hs_dir[0] && now_us() >= g_am.hs_next) {
            hs_log_path(g_am.hs_dir, g_am.hs_log);
            g_am.hs_next = now_us() + 5000000;
        }
        if (g_am.hs_log[0])
            am_tail(&g_am.hs_tail, g_am.hs_log, [&](const char *l, bool old) {
                feed(AMG_HEARTHSTONE, hs_line(&g_am.hs, ev, l, old, marks, COUNT(marks)));
            });
    }
    if (u32 ev = g_am.events[AMG_TARKOV]) {
        if (g_am.eft_dir[0] && now_us() >= g_am.eft_next) {
            eft_log_paths(g_am.eft_dir, g_am.eft_log);
            g_am.eft_next = now_us() + 5000000;
        }
        for (i32 i = 0; i < EFT_LOG_COUNT; i++)
            if (g_am.eft_log[i][0])
                am_tail(&g_am.eft_tail[i], g_am.eft_log[i], [&](const char *l, bool old) {
                    feed(AMG_TARKOV, eft_line(&g_am.eft, ev, l, old, marks, COUNT(marks)));
                });
    }
    replays_poll();
    rust_poll();
    sc_poll();
    eftkills_poll();
}

// Once a second: iRacing's and Assetto Corsa Competizione's shared memory, while the sim has it open.
static void am_sims_poll()
{
    AutoMark marks[8];
    static const i32 games[] = {AMG_IRACING, AMG_ACC};
    static const wchar_t *const maps[] = {IR_MAP, ACC_MAP};
    for (i32 i = 0; i < 2; i++) {
        i32 g = games[i];
        u32 ev = g_am.events[g];
        if (!ev) continue;
        HANDLE h = OpenFileMappingW(FILE_MAP_READ, FALSE, maps[i]);
        if (!h) {
            if (g == AMG_IRACING) g_am.ir.have = false;
            continue;
        }
        const u8 *m = (const u8 *)MapViewOfFile(h, FILE_MAP_READ, 0, 0, 0);
        MEMORY_BASIC_INFORMATION mi = {};
        i32 n = 0;
        if (m && VirtualQuery(m, &mi, sizeof mi)) {
            i64 size = (i64)mi.RegionSize;
            n = g == AMG_IRACING ? ir_marks(&g_am.ir, ev, m, size, marks, COUNT(marks)) : acc_marks(&g_am.acc, ev, m, size, marks, COUNT(marks));
        }
        if (m) UnmapViewOfFile(m);
        CloseHandle(h);
        am_put(marks, n, g);
    }
}

// F1 25's telemetry socket, 127.0.0.1:F1_PORT, open while the game runs (looked for every 5 s) so no other
// program's use of the port is in the way the rest of the time. Its packets are read as they come.
static void am_f1_socket(SOCKET *s, i64 *next)
{
    bool want = false;
    if (g_am.events[AMG_F1_25]) {
        if (now_us() < *next) return;
        *next = now_us() + 5000000;
        static char cmd[4096];
        char title[8];
        want = process_facts("F1_25.exe", cmd, sizeof cmd, title, sizeof title);
    }
    if (!want) {
        if (*s != INVALID_SOCKET) closesocket(*s), *s = INVALID_SOCKET;
        if (!g_am.events[AMG_F1_25]) *next = 0;
        return;
    }
    if (*s != INVALID_SOCKET) return;
    SOCKET u = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons(F1_PORT);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    BOOL excl = TRUE;
    if (u != INVALID_SOCKET) setsockopt(u, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&excl, sizeof excl);
    if (u == INVALID_SOCKET || bind(u, (sockaddr *)&a, sizeof a)) {
        if (u != INVALID_SOCKET) closesocket(u);
        if (InterlockedExchange(&g_am.state[AMG_F1_25], AM_PORT_BUSY) != AM_PORT_BUSY)
            log_line("markers: port %u is taken; F1 25 can't reach " APP_NAME, F1_PORT);
        return;
    }
    *s = u;
    InterlockedExchange(&g_am.state[AMG_F1_25], AM_READY);
    log_line("markers: listening for F1 25 on port %u", F1_PORT);
}

// Rocket League's Stats API: a connection to 127.0.0.1:rl_port, tried every 5 s while the game runs. A closed one
// starts a new stream.
static void am_rl_socket(SOCKET *s, i64 *next)
{
    if (!g_am.events[AMG_ROCKET_LEAGUE]) {
        if (*s != INVALID_SOCKET) closesocket(*s), *s = INVALID_SOCKET;
        *next = 0;
        return;
    }
    if (*s != INVALID_SOCKET || now_us() < *next) return;
    *next = now_us() + 5000000;
    static char cmd[4096];
    char title[8];
    if (!process_facts("RocketLeague.exe", cmd, sizeof cmd, title, sizeof title)) return;
    SOCKET c = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons((u16)g_am.rl_port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (c == INVALID_SOCKET || connect(c, (sockaddr *)&a, sizeof a)) {
        if (c != INVALID_SOCKET) closesocket(c);
        return;
    }
    RlSeen &r = g_am.rl;
    r.part_n = r.depth = 0, r.in_str = r.esc = false;
    *s = c;
    log_line("markers: reading Rocket League's Stats API on port %d", (int)g_am.rl_port);
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
    SOCKET ls = INVALID_SOCKET, f1 = INVALID_SOCKET, rl = INVALID_SOCKET;
    i64 f1_next = 0, rl_next = 0;
    HINTERNET session = WinHttpOpen(L"rekodr", WINHTTP_ACCESS_TYPE_NO_PROXY, 0, 0, 0);
    i64 lol_next = 0, gsi_retry = 0, logs_next = 0, wt_next = 0;
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
        if (!g_am.events[AMG_MINECRAFT]) g_am.mc = {}, g_am.mc_tail = {}, g_am.mc_log[0] = 0;
        for (i32 i = 0; i < 2; i++)
            if (!g_am.events[i ? AMG_POE2 : AMG_POE]) g_am.poe[i] = {}, g_am.poe_tail[i] = {};
        if (!g_am.events[AMG_TF2]) g_am.tf2 = {}, g_am.tf2_tail = {};
        if (!g_am.events[AMG_IRACING]) g_am.ir = {};
        if (!g_am.events[AMG_ACC]) g_am.acc = {};
        if (!g_am.events[AMG_ROCKET_LEAGUE]) g_am.rl.me[0] = 0;
        if (!g_am.events[AMG_WAR_THUNDER]) g_am.wt = {}, g_am.wt.last = -1, g_am.wt_running = false, g_am.wt_next = 0;
        if (!g_am.events[AMG_HEARTHSTONE]) g_am.hs = {}, g_am.hs_tail = {}, g_am.hs_log[0] = 0, g_am.hs_next = 0;
        if (!g_am.events[AMG_TARKOV]) {
            g_am.eft = {}, g_am.eft_next = 0;
            for (i32 i = 0; i < EFT_LOG_COUNT; i++) g_am.eft_tail[i] = {}, g_am.eft_log[i][0] = 0;
        }

        i64 now = now_us();
        i64 wait_ms = 1000;
        bool lol_on = g_am.events[AMG_LOL] && session;
        if (lol_on) {
            i64 until = (lol_next - now) / 1000;
            if (until < wait_ms) wait_ms = until < 0 ? 0 : until;
        }
        am_f1_socket(&f1, &f1_next);
        am_rl_socket(&rl, &rl_next);
        if (ls != INVALID_SOCKET || f1 != INVALID_SOCKET || rl != INVALID_SOCKET) {
            fd_set fds;
            FD_ZERO(&fds);
            if (ls != INVALID_SOCKET) FD_SET(ls, &fds);
            if (f1 != INVALID_SOCKET) FD_SET(f1, &fds);
            if (rl != INVALID_SOCKET) FD_SET(rl, &fds);
            timeval tv = {(long)(wait_ms / 1000), (long)(wait_ms % 1000) * 1000};
            if (select(0, &fds, 0, 0, &tv) > 0) {
                if (ls != INVALID_SOCKET && FD_ISSET(ls, &fds)) {
                    SOCKET c = accept(ls, 0, 0);
                    if (c != INVALID_SOCKET) am_gsi_client(c);
                }
                if (f1 != INVALID_SOCKET && FD_ISSET(f1, &fds)) {
                    static u8 pkt[2048];
                    i32 got = recv(f1, (char *)pkt, sizeof pkt, 0);
                    AutoMark marks[4];
                    if (got > 0) am_put(marks, f1_packet(g_am.events[AMG_F1_25], pkt, got, marks, COUNT(marks)), AMG_F1_25);
                }
                if (rl != INVALID_SOCKET && FD_ISSET(rl, &fds)) {
                    static char buf[16 * 1024];
                    i32 got = recv(rl, buf, sizeof buf, 0);
                    if (got <= 0) {
                        closesocket(rl), rl = INVALID_SOCKET;
                        log_line("markers: Rocket League's Stats API closed");
                    } else {
                        u32 ev = g_am.events[AMG_ROCKET_LEAGUE];
                        rl_feed(&g_am.rl, buf, got, [&](const char *msg, i64 len) {
                            AutoMark marks[4];
                            am_put(marks, rl_message(&g_am.rl, ev, msg, len, marks, COUNT(marks)), AMG_ROCKET_LEAGUE);
                        });
                    }
                }
            }
        } else {
            WaitForSingleObject(g_am.wake, (DWORD)wait_ms);
        }
        if (g_am.events[AMG_WAR_THUNDER] && session && now_us() >= wt_next) {
            am_wt_poll(session);
            wt_next = now_us() + 1000000;
        }
        if (lol_on && now_us() >= lol_next) {
            am_lol_poll(session);
            lol_next = now_us() + (g_am.lol.have ? 1000000 : 3000000);
        }
        if (now_us() >= logs_next) {
            am_logs_poll();
            am_sims_poll();
            logs_next = now_us() + 1000000;
        }
    }
    if (ls != INVALID_SOCKET) closesocket(ls);
    if (f1 != INVALID_SOCKET) closesocket(f1);
    if (rl != INVALID_SOCKET) closesocket(rl);
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

    // The log games: where their logs are, and Team Fortress 2's autoexec line
    static const i32 poe_games[] = {AMG_POE, AMG_POE2};
    static const u32 poe_apps[] = {238960, 2694490};
    static const wchar_t *const poe_dirs[] = {L"Path of Exile", L"Path of Exile 2"};
    for (i32 i = 0; i < 2; i++) {
        i32 g = poe_games[i];
        wchar_t p[MAX_PATH] = L"";
        if (g_am.events[g]) poe_log_path(poe_apps[i], poe_dirs[i], p);
        if (wcscmp(p, g_am.poe_log[i])) wcscpy_s(g_am.poe_log[i], p); // the worker reads it whole or not at all
        InterlockedExchange(&g_am.state[g], !g_am.events[g] ? AM_OFF : p[0] ? AM_READY : AM_NOT_FOUND);
    }
    bool tf2 = g_am.events[AMG_TF2] != 0;
    wchar_t tf[MAX_PATH];
    tf2_dir(tf);
    if (tf2 || g_am.gsi_applied[AMG_TF2] || !c.auto_marks_on[AMG_TF2]) {
        bool found = am_tf2_cfg(tf, tf2);
        g_am.gsi_applied[AMG_TF2] = tf2;
        InterlockedExchange(&g_am.state[AMG_TF2], !tf2 ? AM_OFF : found ? AM_READY : AM_NOT_FOUND);
    }
    wcscpy_s(g_am.tf2_dir, tf2 ? tf : L"");
    bool rl_on = g_am.events[AMG_ROCKET_LEAGUE] != 0;
    if (rl_on || g_am.gsi_applied[AMG_ROCKET_LEAGUE] || !c.auto_marks_on[AMG_ROCKET_LEAGUE]) {
        wchar_t rd[MAX_PATH];
        rl_dir(rd);
        u16 port = am_rl_cfg(rd, rl_on);
        if (port) InterlockedExchange(&g_am.rl_port, port);
        g_am.gsi_applied[AMG_ROCKET_LEAGUE] = rl_on;
        InterlockedExchange(&g_am.state[AMG_ROCKET_LEAGUE], !rl_on ? AM_OFF : port ? AM_READY : AM_NOT_FOUND);
    }
    bool hs = g_am.events[AMG_HEARTHSTONE] != 0;
    wchar_t hd[MAX_PATH] = L"";
    if (hs) hs_dir(hd);
    if (hs || g_am.gsi_applied[AMG_HEARTHSTONE] || !c.auto_marks_on[AMG_HEARTHSTONE]) {
        if (hd[0] || !hs) am_hs_cfg(hs);
        g_am.gsi_applied[AMG_HEARTHSTONE] = hs;
        InterlockedExchange(&g_am.state[AMG_HEARTHSTONE], !hs ? AM_OFF : hd[0] ? AM_READY : AM_NOT_FOUND);
    }
    wcscpy_s(g_am.hs_dir, hd);
    wchar_t ed[MAX_PATH] = L"";
    if (g_am.events[AMG_TARKOV]) eft_dir(ed);
    wcscpy_s(g_am.eft_dir, ed);
    eftkills_apply(ed);
    InterlockedExchange(&g_am.state[AMG_TARKOV], !g_am.events[AMG_TARKOV] ? AM_OFF : ed[0] ? AM_READY : AM_NOT_FOUND);
    str_set(g_am.wt_me, c.wt_name);
    InterlockedExchange(&g_am.state[AMG_WAR_THUNDER], g_am.events[AMG_WAR_THUNDER] ? AM_READY : AM_OFF);
    static const i32 waiting[] = {AMG_MINECRAFT, AMG_IRACING, AMG_ACC, AMG_STAR_CITIZEN};
    for (i32 g : waiting) InterlockedExchange(&g_am.state[g], g_am.events[g] ? AM_READY : AM_OFF);
    if (!g_am.events[AMG_F1_25]) InterlockedExchange(&g_am.state[AMG_F1_25], AM_OFF);
    else if (g_am.state[AMG_F1_25] == AM_OFF) InterlockedExchange(&g_am.state[AMG_F1_25], AM_READY);

    bool logs = false;
    replays_apply();
    rust_apply();
    static const i32 log_games[] = {AMG_MINECRAFT, AMG_POE, AMG_POE2, AMG_TF2, AMG_F1_25, AMG_IRACING, AMG_ACC, AMG_FORTNITE, AMG_SIEGE, AMG_ROCKET_LEAGUE, AMG_HEARTHSTONE, AMG_WAR_THUNDER,
                                     AMG_TARKOV, AMG_RUST, AMG_STAR_CITIZEN};
    for (i32 g : log_games) logs |= g_am.events[g] != 0;
    if ((g_am.gsi_on || g_am.events[AMG_LOL] || logs) && !g_am.thread) {
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

// Uninstall: the cfg files and Team Fortress 2's autoexec line go, and Rocket League's Stats API and Hearthstone's
// Power log are turned back off if Rekodr turned them on, whatever the switches say.
static void automarks_uninstall()
{
    for (i32 i = 0; i < COUNT(AM_GSI); i++) am_gsi_cfg(AM_GSI[i], false);
    wchar_t tf[MAX_PATH];
    tf2_dir(tf);
    am_tf2_cfg(tf, false);
    wchar_t rd[MAX_PATH];
    rl_dir(rd);
    am_rl_cfg(rd, false);
    am_hs_cfg(false);
}
