// Event icons: a small mono picture for each kind of game event (a skull for a death, a crosshair for a kill, a
// trophy for a win) and for a few weapon classes, drawn as paths so every game shows the same icon for the same
// event at any size. The timeline draws a marker as its icon, tinted with the marker's colour.
//
// Each icon is laid out on a 16 x 16 grid centred on the point it is drawn at; `hole` is the colour behind it, for
// the parts cut out of a shape (a skull's eyes).

static bool marker_detail_get(const char *d, const char *key, char *out, i32 cap); // markers.cpp

enum EvIcon {
    EVI_NONE, EVI_KILL, EVI_DEATH, EVI_HEADSHOT, EVI_MULTI, EVI_DOWNED, EVI_ASSIST, EVI_ROUND, EVI_WIN, EVI_OBJECTIVE,
    EVI_ADVANCEMENT, EVI_LEVEL, EVI_LAP, EVI_OVERTAKE, EVI_INCIDENT, EVI_GOAL, EVI_SAVE, EVI_RAID_START, EVI_RAID_END,
    EVI_QUEST, EVI_GROUP, EVI_CONTRACT, EVI_ZONE, EVI_TRAVEL, EVI_SPAWN, EVI_SERVER, EVI_ACCOUNT, EVI_MVP, EVI_PLANT,
    EVI_DEFUSE, EVI_ACHIEVEMENT,
    EVI_RIFLE, EVI_PISTOL, EVI_SMG, EVI_SNIPER, EVI_SHOTGUN, EVI_KNIFE, EVI_GRENADE,
    EVI_COUNT
};

// The icon of each AmEvent bit, in AM_EVENT_WORDS order.
static const u8 EVI_OF_EVENT[] = {EVI_KILL, EVI_MULTI, EVI_DEATH, EVI_ROUND, EVI_WIN, EVI_OBJECTIVE, EVI_ASSIST,
                                  EVI_ADVANCEMENT, EVI_LEVEL, EVI_LAP, EVI_OVERTAKE, EVI_INCIDENT, EVI_GOAL, EVI_SAVE,
                                  EVI_RAID_START, EVI_RAID_END, EVI_QUEST, EVI_GROUP, EVI_DOWNED, EVI_CONTRACT, EVI_ZONE,
                                  EVI_TRAVEL, EVI_SPAWN, EVI_SERVER, EVI_ACCOUNT, EVI_HEADSHOT, EVI_MVP, EVI_PLANT,
                                  EVI_DEFUSE};
static_assert(sizeof EVI_OF_EVENT == sizeof AM_EVENT_WORDS / sizeof *AM_EVENT_WORDS, "an icon for each event");

// A weapon's class from its name: Counter-Strike 2's ("ak47", "awp", "knife_karambit"), and the words other games
// use ("AK-74N", "Mosin rifle", "F-1 hand grenade"). EVI_NONE when the name says nothing of it.
static EvIcon evi_weapon_class(const char *weapon)
{
    char w[96];
    i32 n = 0;
    for (const char *c = weapon; *c && n < (i32)sizeof w - 1; c++) w[n++] = (char)tolower((u8)*c);
    w[n] = 0;
    if (!w[0]) return EVI_NONE;
    struct Word { const char *word; u8 icon; bool whole; };
    // Whole names first (CS2's, which are short and would match inside other words), then parts of names.
    static const Word words[] = {
        {"awp", EVI_SNIPER, true}, {"ssg08", EVI_SNIPER, true}, {"scar20", EVI_SNIPER, true}, {"g3sg1", EVI_SNIPER, true},
        {"ak47", EVI_RIFLE, true}, {"m4a1", EVI_RIFLE, true}, {"m4a1_silencer", EVI_RIFLE, true}, {"famas", EVI_RIFLE, true},
        {"galilar", EVI_RIFLE, true}, {"aug", EVI_RIFLE, true}, {"sg556", EVI_RIFLE, true}, {"m249", EVI_RIFLE, true},
        {"negev", EVI_RIFLE, true},
        {"mac10", EVI_SMG, true}, {"mp9", EVI_SMG, true}, {"mp7", EVI_SMG, true}, {"mp5sd", EVI_SMG, true},
        {"ump45", EVI_SMG, true}, {"p90", EVI_SMG, true}, {"bizon", EVI_SMG, true},
        {"nova", EVI_SHOTGUN, true}, {"xm1014", EVI_SHOTGUN, true}, {"sawedoff", EVI_SHOTGUN, true}, {"mag7", EVI_SHOTGUN, true},
        {"glock", EVI_PISTOL, true}, {"hkp2000", EVI_PISTOL, true}, {"usp_silencer", EVI_PISTOL, true}, {"p250", EVI_PISTOL, true},
        {"elite", EVI_PISTOL, true}, {"fiveseven", EVI_PISTOL, true}, {"tec9", EVI_PISTOL, true}, {"cz75a", EVI_PISTOL, true},
        {"deagle", EVI_PISTOL, true}, {"revolver", EVI_PISTOL, true}, {"taser", EVI_PISTOL, true},
        {"hegrenade", EVI_GRENADE, true}, {"molotov", EVI_GRENADE, true}, {"incgrenade", EVI_GRENADE, true},
        {"flashbang", EVI_GRENADE, true}, {"smokegrenade", EVI_GRENADE, true}, {"decoy", EVI_GRENADE, true},
        {"inferno", EVI_GRENADE, true},
        {"knife", EVI_KNIFE, false}, {"bayonet", EVI_KNIFE, false}, {"machete", EVI_KNIFE, false}, {"axe", EVI_KNIFE, false},
        {"hatchet", EVI_KNIFE, false}, {"melee", EVI_KNIFE, false}, {"sword", EVI_KNIFE, false}, {"spear", EVI_KNIFE, false},
        {"grenade", EVI_GRENADE, false}, {"explosive", EVI_GRENADE, false}, {"c4", EVI_GRENADE, false}, {"rocket", EVI_GRENADE, false},
        {"sniper", EVI_SNIPER, false}, {"mosin", EVI_SNIPER, false}, {"sv-98", EVI_SNIPER, false}, {"m700", EVI_SNIPER, false},
        {"t-5000", EVI_SNIPER, false}, {"dvl", EVI_SNIPER, false}, {"bolt action", EVI_SNIPER, false}, {"l96", EVI_SNIPER, false},
        {"shotgun", EVI_SHOTGUN, false}, {"mp-133", EVI_SHOTGUN, false}, {"mp-153", EVI_SHOTGUN, false}, {"saiga", EVI_SHOTGUN, false},
        {"toz", EVI_SHOTGUN, false}, {"m870", EVI_SHOTGUN, false}, {"eoka", EVI_SHOTGUN, false},
        {"smg", EVI_SMG, false}, {"mp5", EVI_SMG, false}, {"mp7", EVI_SMG, false}, {"mp9", EVI_SMG, false}, {"ump", EVI_SMG, false},
        {"vector", EVI_SMG, false}, {"kedr", EVI_SMG, false}, {"ppsh", EVI_SMG, false}, {"thompson", EVI_SMG, false},
        {"pistol", EVI_PISTOL, false}, {"glock", EVI_PISTOL, false}, {"makarov", EVI_PISTOL, false}, {"pm ", EVI_PISTOL, false},
        {"m1911", EVI_PISTOL, false}, {"revolver", EVI_PISTOL, false}, {"deagle", EVI_PISTOL, false}, {"apb", EVI_PISTOL, false},
        {"rifle", EVI_RIFLE, false}, {"ak-", EVI_RIFLE, false}, {"m4a1", EVI_RIFLE, false}, {"akm", EVI_RIFLE, false},
        {"carbine", EVI_RIFLE, false}, {"lmg", EVI_RIFLE, false}, {"hk 416", EVI_RIFLE, false}, {"mdr", EVI_RIFLE, false},
        {"sks", EVI_RIFLE, false}, {"vss", EVI_RIFLE, false}, {"m249", EVI_RIFLE, false},
        // Tarkov's
        {"mpx", EVI_SMG, false}, {"pp-19", EVI_SMG, false}, {"pp-91", EVI_SMG, false}, {"pp-9 ", EVI_SMG, false},
        {"saiga-9", EVI_SMG, false}, {"vityaz", EVI_SMG, false}, {"kriss", EVI_SMG, false}, {"stm-9", EVI_SMG, false},
        {"sr-2m", EVI_SMG, false}, {"p90", EVI_SMG, false}, {"mp-9", EVI_SMG, false},
        {"grach", EVI_PISTOL, false}, {"mp-443", EVI_PISTOL, false}, {"pl-15", EVI_PISTOL, false}, {"m9a3", EVI_PISTOL, false},
        {"p226", EVI_PISTOL, false}, {"tt-33", EVI_PISTOL, false}, {"five-seven", EVI_PISTOL, false}, {"usp", EVI_PISTOL, false},
        {"sr-1mp", EVI_PISTOL, false}, {"rsh-12", EVI_PISTOL, false}, {"m45", EVI_PISTOL, false}, {"aps", EVI_PISTOL, true},
        {"svd", EVI_SNIPER, false}, {"rsass", EVI_SNIPER, false}, {"sr-25", EVI_SNIPER, false}, {"m1a", EVI_SNIPER, false},
        {"axmc", EVI_SNIPER, false}, {"sa-58", EVI_RIFLE, false}, {"adar", EVI_RIFLE, false}, {"tx-15", EVI_RIFLE, false},
        {"mk47", EVI_RIFLE, false}, {"mcx", EVI_RIFLE, false}, {"scar", EVI_RIFLE, false}, {"ash-12", EVI_RIFLE, false},
        {"g36", EVI_RIFLE, false}, {"as val", EVI_RIFLE, false}, {"vpo", EVI_RIFLE, false}, {"rd-704", EVI_RIFLE, false},
        {"saiga-12", EVI_SHOTGUN, false}, {"mp-155", EVI_SHOTGUN, false}, {"590a1", EVI_SHOTGUN, false},
        {"ks-23", EVI_SHOTGUN, false}, {"aa-12", EVI_SHOTGUN, false}, {"ksg", EVI_SHOTGUN, false},
    };
    for (const Word &x : words)
        if (x.whole ? !strcmp(w, x.word) : strstr(w, x.word) != 0) return (EvIcon)x.icon;
    if (starts_with(w, "knife")) return EVI_KNIFE;
    return EVI_NONE;
}

// A marker's icon from its detail: its event (event=<word>), a kill shown as its weapon's class when the detail
// names one and as a headshot when it was one, and an achievement's medal. EVI_NONE for a marker of the player's own.
static EvIcon evi_of_marker(const char *detail)
{
    char v[96];
    if (marker_detail_get(detail, "achievement", v, sizeof v) && v[0]) return EVI_ACHIEVEMENT;
    if (!marker_detail_get(detail, "event", v, sizeof v)) return EVI_NONE;
    i32 bit = -1;
    for (i32 i = 0; i < COUNT(AM_EVENT_WORDS); i++)
        if (!strcmp(v, AM_EVENT_WORDS[i])) bit = i;
    if (bit < 0) return EVI_NONE;
    EvIcon icon = (EvIcon)EVI_OF_EVENT[bit];
    if (icon == EVI_KILL || icon == EVI_MULTI) {
        if (marker_detail_get(detail, "area", v, sizeof v) && !_stricmp(v, "head")) return EVI_HEADSHOT;
        if (marker_detail_get(detail, "weapon", v, sizeof v))
            if (EvIcon w = evi_weapon_class(v)) return w;
    }
    return icon;
}

// What an icon is called, for the icon sheet.
static const char *const EVI_NAMES[EVI_COUNT] = {
    "", "Kill", "Death", "Headshot", "Multi-kill", "Downed", "Assist", "Round", "Win", "Objective", "Advancement",
    "Level", "Lap", "Overtake", "Incident", "Goal", "Save", "Raid start", "Raid end", "Quest", "Group", "Contract",
    "Zone", "Travel", "Spawn", "Server", "Account", "MVP", "Plant", "Defuse", "Achievement", "Rifle", "Pistol", "SMG",
    "Sniper", "Shotgun", "Knife", "Grenade"};

// Each icon's shapes on a 14 by 14 grid of pixels at 100%, so straight edges fall on whole pixels. Upper case in
// the icon's colour, lower case in the colour behind it (holes): R x0 y0 x1 y1 a rectangle; D x y r a disc; O x y r w
// a ring, r to its outside; P n x y ... a convex shape of n points; S w n x y ... a line w wide through n points;
// T x y r1 r2 a five-point star.
static const char *const EVI_SHAPES[EVI_COUNT] = {
    "",
    /* kill */ "O 7 7 5 2 R 6 0 8 3 R 6 11 8 14 R 0 6 3 8 R 11 6 14 8 R 6 6 8 8",
    /* death */ "D 7 6 5.5 R 3 8 11 13 d 4.8 6.6 1.7 d 9.2 6.6 1.7 r 5 11 6 13 r 8 11 9 13",
    /* headshot */ "D 7 5 4.5 d 7 5 2.5 D 7 5 1.2 P 4 1 14 3 10 11 10 13 14",
    /* multi */ "P 8 7 0 11 5 12 9 10.5 12.5 7 14 3.5 12.5 2 9 3 5 p 5 7 6.5 9 10 8 12 6 12 5 10",
    /* downed */ "D 3 8.5 2.5 R 6 7 13 11 R 0 12 14 14",
    /* assist */ "D 5 4 3 P 5 0 14 1 9 5 8 9 9 10 14 R 10 2 12 10 R 7 5 15 7",
    /* round */ "R 2 1 4 14 R 4 1 9 8 P 3 9 1 13 1 9 8 P 3 13 8 9 8 9 4.5",
    /* win */ "R 3 1 11 5 D 7 5 4 R 6 8 8 11 R 4 11 10 13 R 1 2 3 3 R 1 2 2 6 R 11 2 13 3 R 12 2 13 6",
    /* objective */ "R 2 5 12 13 R 2 2 4 5 R 6 2 8 5 R 10 2 12 5 r 6 9 8 13",
    /* advancement */ "T 7 7.6 7 2.9",
    /* level */ "S 2.4 3 2 7.5 7 2.5 12 7.5 S 2.4 3 2 12.5 7 7.5 12 12.5",
    /* lap */ "O 7 8 6 2 R 5 0 9 2 R 6 4 8 9 R 11 2 13 4",
    /* overtake */ "S 2.4 3 2 2 7 7 2 12 S 2.4 3 7 2 12 7 7 12",
    /* incident */ "P 3 7 0.5 14 13.5 0 13.5 r 6 5 8 9 r 6 10 8 12",
    /* goal */ "R 0 2 14 4 R 0 2 2 14 R 12 2 14 14 D 7 10.5 2.5",
    /* save */ "P 5 1 1.5 13 1.5 13 7.5 7 13 1 7.5 r 6 4 8 10 r 4 6 10 8",
    /* raid_start */ "R 9 1 13 3 R 11 1 13 13 R 9 11 13 13 R 0 6 6 8 P 3 5 2.5 9.5 7 5 11.5",
    /* raid_end */ "R 1 1 5 3 R 1 1 3 13 R 1 11 5 13 R 4 6 10 8 P 3 9 2.5 13.5 7 9 11.5",
    /* quest */ "D 7 7 7 s 2.2 3 3.8 7.2 6 9.4 10.4 4.8",
    /* group */ "D 10 3.5 2.5 P 4 6 12 14 12 14 8 6 8 d 5 5 3.6 p 4 -1 14 11 14 11 9 -1 9 D 5 5 2.5 P 4 0 14 10 14 10 10 0 10",
    /* contract */ "P 5 2 0 9 0 12 3 12 14 2 14 r 4 5 10 6 r 4 8 10 9 r 4 11 8 12",
    /* zone */ "D 7 5.5 5 P 3 2.4 7.6 11.6 7.6 7 14 d 7 5.5 2",
    /* travel */ "P 3 0 6 14 0 6 8 P 3 6 8 14 0 9 14 s 1 2 6 8 13 1",
    /* spawn */ "D 7 4 3.2 P 6 1 14 2 10 5 8.5 9 8.5 12 10 13 14",
    /* server */ "R 1 1 13 4 R 1 5 13 8 R 1 9 13 12 r 9 2 11 3 r 9 6 11 7 r 9 10 11 11",
    /* account */ "R 0 2 14 12 d 4.5 6 1.8 r 2 8.5 7 10.5 r 8 5 12 6 r 8 8 12 9",
    /* mvp */ "R 1 9 13 12 P 3 1 3 1 9 5 9 P 3 7 1 3.5 9 10.5 9 P 3 13 3 13 9 9 9",
    /* plant */ "D 6 8.5 5 R 8 3 11 5 R 11 1 12 3 R 12 0 14 1",
    /* defuse */ "D 6 8.5 5 R 8 3 11 5 s 1.6 2 2.5 12 9.5 5",
    /* achievement */ "P 4 3 0 6 0 8 5 5 5 P 4 8 0 11 0 9 5 6 5 D 7 9 4.8 d 7 9 2.4",
    /* rifle */ "P 4 0 5 4 4 4 8 0 9 R 4 4 11 7 R 11 4 14 6 P 4 7 7 9 7 10 11 8 11 P 4 4 7 6 7 5 10 3 10",
    /* pistol */ "R 2 3 13 7 P 4 3 7 7 7 6 12 2 12",
    /* smg */ "R 0 4 2 6 R 2 3 11 7 R 11 4 14 6 R 6 7 8 12 P 4 3 7 5 7 4 10 2 10",
    /* sniper */ "P 4 0 6 3 5 3 9 0 10 R 3 5 9 8 R 9 5 14 7 R 4 2 9 4 P 4 4 8 6 8 5 11 3 11",
    /* shotgun */ "P 4 0 5 4 4 4 8 0 10 R 4 4 14 6 R 7 6 12 8 P 4 4 6 6 6 5 9 3 9",
    /* knife */ "P 4 1 13 8 4 11 7 3 13 S 3 2 10 5.5 13 2.5",
    /* grenade */ "D 7 9 4.8 R 5 2 9 5 R 9 2 12 3 O 3.5 3.5 2 1",
};

// Draws `icon` centred on `c`, `size` across (14 * the UI's scale keeps edges on whole pixels), in `col`; `hole` is
// the colour behind it.
static void evi_draw(ImDrawList *dl, EvIcon icon, ImVec2 c, f32 size, ImU32 col, ImU32 hole)
{
    if (icon <= EVI_NONE || icon >= EVI_COUNT) return;
    const f32 u = size / 14, ox = floorf(c.x - size / 2 + 0.5f), oy = floorf(c.y - size / 2 + 0.5f);
    auto P = [&](f32 x, f32 y) { return ImVec2(ox + x * u, oy + y * u); };
    const char *s = EVI_SHAPES[icon];
    f32 v[40];
    auto num = [&]() { char *e; f32 f = strtof(s, &e); s = e; return f; };
    while (*s) {
        while (*s == ' ') s++;
        char op = *s;
        if (!op) break;
        s++;
        ImU32 k = isupper((u8)op) ? col : hole;
        switch (toupper((u8)op)) {
        case 'R': // whole pixels, no smoothing: crisp
            for (i32 i = 0; i < 4; i++) v[i] = num();
            dl->AddRectFilled(ImVec2(roundf(ox + v[0] * u), roundf(oy + v[1] * u)), ImVec2(roundf(ox + v[2] * u), roundf(oy + v[3] * u)), k);
            break;
        case 'D':
            for (i32 i = 0; i < 3; i++) v[i] = num();
            dl->AddCircleFilled(P(v[0], v[1]), v[2] * u, k);
            break;
        case 'O':
            for (i32 i = 0; i < 4; i++) v[i] = num();
            dl->AddCircle(P(v[0], v[1]), (v[2] - v[3] / 2) * u, k, 0, v[3] * u);
            break;
        case 'P': case 'S': {
            f32 w = toupper((u8)op) == 'S' ? num() : 0;
            i32 n = (i32)num();
            ImVec2 p[16];
            n = n > 16 ? 16 : n;
            for (i32 i = 0; i < n; i++) { f32 x = num(); p[i] = P(x, num()); }
            if (!w) dl->AddConvexPolyFilled(p, n, k);
            else {
                dl->AddPolyline(p, n, k, 0, w * u);
                for (i32 i = 1; i + 1 < n; i++) dl->AddCircleFilled(p[i], w * u / 2, k); // round joints
            }
            break;
        }
        case 'T': { // a star: its middle and five points, each convex
            for (i32 i = 0; i < 4; i++) v[i] = num();
            ImVec2 p[10];
            for (i32 i = 0; i < 10; i++) {
                f32 a = -IM_PI / 2 + i * IM_PI / 5, r = (i & 1) ? v[3] : v[2];
                p[i] = P(v[0] + r * cosf(a), v[1] + r * sinf(a));
            }
            ImVec2 mid[5] = {p[1], p[3], p[5], p[7], p[9]};
            dl->AddConvexPolyFilled(mid, 5, k);
            for (i32 i = 0; i < 10; i += 2) dl->AddTriangleFilled(p[(i + 9) % 10], p[i], p[i + 1], k);
            break;
        }
        default: return;
        }
    }
}
