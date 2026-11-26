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

// Draws `icon` centred on `c`, `size` across, in `col`; `hole` is the colour behind it.
static void evi_draw(ImDrawList *dl, EvIcon icon, ImVec2 c, f32 size, ImU32 col, ImU32 hole)
{
    const f32 u = size / 16;
    auto P = [&](f32 x, f32 y) { return ImVec2(c.x + (x - 8) * u, c.y + (y - 8) * u); };
    const f32 th = 1.6f * u; // a stroke
    auto line = [&](f32 x0, f32 y0, f32 x1, f32 y1, f32 w = 0) { dl->AddLine(P(x0, y0), P(x1, y1), col, w ? w * u : th); };
    auto circle = [&](f32 x, f32 y, f32 r, f32 w = 0) { dl->AddCircle(P(x, y), r * u, col, 0, w ? w * u : th); };
    auto disc = [&](f32 x, f32 y, f32 r, ImU32 k) { dl->AddCircleFilled(P(x, y), r * u, k); };
    auto rect = [&](f32 x0, f32 y0, f32 x1, f32 y1, ImU32 k, f32 r = 0) { dl->AddRectFilled(P(x0, y0), P(x1, y1), k, r * u); };
    // A filled shape, or a line through points, from x, y pairs
    auto poly = [&](const f32 *v, i32 count, ImU32 k) {
        ImVec2 p[24];
        i32 n = 0;
        for (i32 i = 0; i + 1 < count && n < 24; i += 2) p[n++] = P(v[i], v[i + 1]);
        dl->AddConcavePolyFilled(p, n, k);
    };
    auto stroke = [&](const f32 *v, i32 count, bool closed, f32 w) {
        ImVec2 p[24];
        i32 n = 0;
        for (i32 i = 0; i + 1 < count && n < 24; i += 2) p[n++] = P(v[i], v[i + 1]);
        dl->AddPolyline(p, n, col, closed ? ImDrawFlags_Closed : 0, w ? w * u : th);
    };
#define EVI_POLY(k, ...) do { static const f32 v_[] = {__VA_ARGS__}; poly(v_, COUNT(v_), k); } while (0)
#define EVI_STROKE(closed, w, ...) do { static const f32 v_[] = {__VA_ARGS__}; stroke(v_, COUNT(v_), closed, w); } while (0)
    auto crosshair = [&](f32 x, f32 y, f32 r) {
        circle(x, y, r);
        line(x, y - r - 2, x, y - r + 2.5f), line(x, y + r - 2.5f, x, y + r + 2);
        line(x - r - 2, y, x - r + 2.5f, y), line(x + r - 2.5f, y, x + r + 2, y);
    };
    auto star = [&](f32 x, f32 y, f32 R, f32 r) {
        ImVec2 p[10];
        for (i32 i = 0; i < 10; i++) {
            f32 a = -IM_PI / 2 + i * IM_PI / 5, rr = (i & 1) ? r : R;
            p[i] = P(x + cosf(a) * rr, y + sinf(a) * rr);
        }
        dl->AddConcavePolyFilled(p, 10, col);
    };
    auto person = [&](f32 x, f32 y, f32 k) { // head and shoulders, `k` the scale
        disc(x, y - 3 * k, 2.6f * k, col);
        dl->PathArcTo(P(x, y + 5 * k), 5 * k * u, IM_PI, 2 * IM_PI);
        dl->PathFillConvex(col);
    };
    auto bomb = [&]() {
        disc(7, 9.5f, 5, col);
        rect(9.5f, 3.5f, 12, 6, col);
        line(11.5f, 4, 13.5f, 1.5f, 1.2f);
    };
    switch (icon) {
    case EVI_NONE: case EVI_COUNT: break;
    case EVI_KILL: crosshair(8, 8, 5); disc(8, 8, 1.3f, col); break;
    case EVI_MULTI: // a crosshair and a tally of kills
        crosshair(6, 8, 3.6f); disc(6, 8, 1.1f, col);
        rect(12.2f, 3.5f, 13.5f, 12.5f, col), rect(14.5f, 3.5f, 15.8f, 12.5f, col);
        break;
    case EVI_DEATH: // skull: dome, jaw, eye holes, nose
        disc(8, 7, 6, col);
        rect(4.5f, 9, 11.5f, 14, col, 1.2f);
        disc(5.8f, 7.8f, 1.7f, hole), disc(10.2f, 7.8f, 1.7f, hole);
        EVI_POLY(hole, 8, 9.6f, 7, 11.4f, 9, 11.4f);
        rect(6.6f, 12.4f, 7.2f, 14, hole), rect(8.8f, 12.4f, 9.4f, 14, hole);
        break;
    case EVI_HEADSHOT: // a head with a crosshair on it
        person(8, 9.5f, 1.1f);
        disc(8, 6.2f, 1.4f, hole);
        circle(8, 6.2f, 4.3f, 1.2f);
        line(8, 0.6f, 8, 2.6f, 1.2f), line(2.4f, 6.2f, 4.4f, 6.2f, 1.2f), line(11.6f, 6.2f, 13.6f, 6.2f, 1.2f);
        break;
    case EVI_DOWNED: // a person fallen, an arrow down over them
        disc(3.5f, 12, 2, col);
        rect(6, 10.5f, 14.5f, 13.5f, col, 1.5f);
        line(10, 1.5f, 10, 7);
        EVI_STROKE(false, 0, 7.5f, 4.8f, 10, 7.5f, 12.5f, 4.8f);
        break;
    case EVI_ASSIST: // a second person behind, a plus
        person(6.5f, 8.5f, 0.95f);
        rect(10.4f, 3.5f, 12.2f, 10.5f, col), rect(7.8f, 6.1f, 14.8f, 7.9f, col);
        break;
    case EVI_ROUND: // a flag
        rect(3, 1.5f, 4.6f, 15, col);
        EVI_POLY(col, 4.6f, 2, 13.5f, 2, 11, 5.5f, 13.5f, 9, 4.6f, 9);
        break;
    case EVI_WIN: // a trophy
        dl->PathArcTo(P(8, 3), 4.5f * u, 0, IM_PI);
        dl->PathFillConvex(col);
        rect(3.5f, 1.5f, 12.5f, 3.2f, col);
        EVI_STROKE(false, 1.3f, 3.6f, 3, 1.6f, 3, 1.8f, 5.5f, 4.4f, 7);
        EVI_STROKE(false, 1.3f, 12.4f, 3, 14.4f, 3, 14.2f, 5.5f, 11.6f, 7);
        rect(7.2f, 7, 8.8f, 11.5f, col);
        rect(4.5f, 11.5f, 11.5f, 14, col, 0.8f);
        break;
    case EVI_OBJECTIVE: // a tower
        EVI_POLY(col, 3, 2, 5, 2, 5, 4, 7, 4, 7, 2, 9, 2, 9, 4, 11, 4, 11, 2, 13, 2, 13, 6.5f, 11.5f, 7.5f, 11.5f, 14.5f,
              4.5f, 14.5f, 4.5f, 7.5f, 3, 6.5f);
        dl->PathArcTo(P(8, 12), 1.6f * u, IM_PI, 2 * IM_PI);
        dl->PathLineTo(P(9.6f, 14.5f));
        dl->PathLineTo(P(6.4f, 14.5f));
        dl->PathFillConvex(hole);
        break;
    case EVI_ADVANCEMENT: star(8, 8.6f, 7, 3); break;
    case EVI_LEVEL: // up chevrons
        EVI_STROKE(false, 2.2f, 3, 8, 8, 3, 13, 8);
        EVI_STROKE(false, 2.2f, 3, 13.5f, 8, 8.5f, 13, 13.5f);
        break;
    case EVI_LAP: // a stopwatch
        circle(8, 9.5f, 5.5f);
        rect(6.5f, 1.2f, 9.5f, 2.6f, col);
        line(8, 2.4f, 8, 4);
        line(8, 9.5f, 10.5f, 7, 1.4f);
        line(12.5f, 4.2f, 13.8f, 2.9f, 1.4f);
        break;
    case EVI_OVERTAKE: // one arrow past a bar
        rect(1.5f, 10, 6.5f, 13.5f, col, 1);
        line(2.5f, 6.5f, 12, 6.5f, 2);
        EVI_POLY(col, 10, 2.5f, 15, 6.5f, 10, 10.5f);
        break;
    case EVI_INCIDENT: // a warning sign
        EVI_POLY(col, 8, 1.5f, 15, 14, 1, 14);
        rect(7.2f, 5.5f, 8.8f, 10.2f, hole);
        rect(7.2f, 11.2f, 8.8f, 12.7f, hole);
        break;
    case EVI_GOAL: // a ball
        circle(8, 8, 6.2f);
        EVI_POLY(col, 8, 5.2f, 10.7f, 7.2f, 9.7f, 10.3f, 6.3f, 10.3f, 5.3f, 7.2f);
        line(8, 5.2f, 8, 1.8f, 1.2f), line(10.7f, 7.2f, 14, 6.2f, 1.2f), line(9.7f, 10.3f, 11.7f, 13, 1.2f);
        line(6.3f, 10.3f, 4.3f, 13, 1.2f), line(5.3f, 7.2f, 2, 6.2f, 1.2f);
        break;
    case EVI_SAVE: // a shield
        EVI_POLY(col, 8, 1.2f, 14, 3.4f, 13.6f, 8.5f, 11.8f, 12, 8, 14.8f, 4.2f, 12, 2.4f, 8.5f, 2, 3.4f);
        EVI_POLY(hole, 8, 3.4f, 11.8f, 4.8f, 11.5f, 8.3f, 10.2f, 10.8f, 8, 12.5f);
        break;
    case EVI_RAID_START: case EVI_RAID_END: { // a door frame, an arrow in or out
        EVI_STROKE(false, 1.8f, 9, 2, 14, 2, 14, 14, 9, 14);
        bool in = icon == EVI_RAID_START;
        line(in ? 1.5f : 5, 8, in ? 9 : 12.5f, 8, 2);
        if (in) EVI_POLY(col, 7, 4.5f, 11, 8, 7, 11.5f);
        else EVI_POLY(col, 4.5f, 4.5f, 0.5f, 8, 4.5f, 11.5f);
        break;
    }
    case EVI_QUEST: // a tick in a circle
        disc(8, 8, 7, col);
        { // the tick, cut out of it
            ImVec2 tick[3] = {P(4.6f, 8.2f), P(7, 10.6f), P(11.6f, 5.6f)};
            dl->AddPolyline(tick, 3, hole, 0, 2 * u);
        }
        break;
    case EVI_GROUP: person(5.5f, 8.5f, 0.85f); person(11, 9, 0.85f); break;
    case EVI_CONTRACT: // a page with lines
        EVI_POLY(col, 3, 1.5f, 10, 1.5f, 13, 4.5f, 13, 14.5f, 3, 14.5f);
        rect(5, 6.5f, 11, 7.6f, hole), rect(5, 9, 11, 10.1f, hole), rect(5, 11.5f, 9, 12.6f, hole);
        break;
    case EVI_ZONE: // a map pin
        disc(8, 6, 5, col);
        EVI_POLY(col, 3.6f, 7.8f, 12.4f, 7.8f, 8, 15);
        disc(8, 6, 1.9f, hole);
        break;
    case EVI_TRAVEL: // a planet and its ring
        disc(8, 8, 4.5f, col);
        dl->AddEllipse(P(8, 8), ImVec2(7.5f * u, 2.4f * u), hole, -0.45f, 0, 2.6f * u);
        dl->AddEllipse(P(8, 8), ImVec2(7.5f * u, 2.4f * u), col, -0.45f, 0, 1.2f * u);
        break;
    case EVI_SPAWN: person(8, 8.5f, 1.15f); break;
    case EVI_SERVER: // stacked boxes, a light each
        for (i32 k = 0; k < 3; k++) {
            f32 y = 2 + k * 4.4f;
            rect(2, y, 14, y + 3.4f, col, 0.8f);
            disc(4.2f, y + 1.7f, 0.8f, hole);
        }
        break;
    case EVI_ACCOUNT: // a card with a person on it
        rect(1, 3, 15, 13, col, 1.5f);
        disc(5.5f, 7, 1.8f, hole);
        dl->PathArcTo(P(5.5f, 12.4f), 3.2f * u, IM_PI, 2 * IM_PI);
        dl->PathFillConvex(hole);
        rect(9, 6, 13, 7.1f, hole), rect(9, 8.8f, 12, 9.9f, hole);
        break;
    case EVI_MVP: // a crown
        EVI_POLY(col, 1.5f, 4, 5, 8, 8, 2.5f, 11, 8, 14.5f, 4, 13, 12, 3, 12);
        rect(3, 13, 13, 14.6f, col);
        break;
    case EVI_PLANT: bomb(); break;
    case EVI_DEFUSE: // a bomb, a tick over it
        bomb();
        disc(7, 9.5f, 3.2f, hole);
        EVI_STROKE(false, 1.5f, 5.2f, 9.6f, 6.6f, 11, 9, 8.2f);
        break;
    case EVI_ACHIEVEMENT: // a medal on a ribbon
        EVI_POLY(col, 4, 1, 7, 1, 9, 6, 6, 6);
        EVI_POLY(col, 9, 1, 12, 1, 10, 6, 7.5f, 6);
        disc(8, 10, 4.8f, col);
        star(8, 10.2f, 3, 1.3f);
        dl->AddCircleFilled(P(8, 10), 3.4f * u, hole);
        star(8, 10.3f, 2.8f, 1.2f);
        break;
    // Weapons, pointing right
    case EVI_RIFLE:
        EVI_POLY(col, 0.5f, 6, 4, 6, 4, 5, 12, 5, 12, 5.8f, 15.5f, 5.8f, 15.5f, 7, 11, 7, 10, 8.5f, 9, 8.5f, 9.5f, 12, 7.8f, 12,
              7.2f, 8.5f, 6, 8.5f, 5.2f, 10.5f, 0.5f, 10);
        break;
    case EVI_PISTOL:
        EVI_POLY(col, 2, 4, 14, 4, 14, 7, 7.5f, 7, 7, 8, 7.8f, 8.5f, 6.5f, 13, 2.5f, 13, 4.2f, 7);
        break;
    case EVI_SMG:
        EVI_POLY(col, 1, 5.5f, 13, 5.5f, 13, 6.3f, 15, 6.3f, 15, 7.5f, 13, 7.5f, 11.5f, 8.8f, 10.5f, 8.8f, 11, 13.5f, 9.3f, 13.5f,
              8.6f, 8.8f, 6.8f, 8.8f, 6, 11, 4.5f, 11, 5, 8.8f, 1, 8.8f);
        break;
    case EVI_SNIPER:
        EVI_POLY(col, 0.5f, 7, 3.5f, 7, 4, 6.6f, 15.5f, 6.6f, 15.5f, 7.6f, 8, 7.6f, 7, 9, 5.8f, 9, 5, 11, 0.5f, 10.5f);
        rect(5, 3.4f, 10.5f, 5.2f, col, 0.9f);
        rect(7, 5, 8, 6.7f, col);
        break;
    case EVI_SHOTGUN:
        EVI_POLY(col, 0.5f, 6.5f, 4.5f, 6.2f, 15.5f, 6.2f, 15.5f, 7.6f, 8, 7.6f, 7, 9, 5, 9, 4.5f, 11, 0.5f, 10.5f);
        rect(9, 8.2f, 14, 9.6f, col, 0.6f);
        break;
    case EVI_KNIFE:
        EVI_POLY(col, 1, 9.5f, 6, 9.5f, 6, 7, 11, 7, 15.5f, 5, 13, 9.5f, 6, 11, 6, 10.5f, 1, 11);
        rect(5.5f, 6.4f, 6.6f, 11.8f, col);
        break;
    case EVI_GRENADE:
        disc(8, 10, 5, col);
        rect(6.2f, 3.2f, 9.8f, 5.6f, col);
        EVI_STROKE(false, 1.3f, 9.8f, 3.8f, 12.5f, 3.8f, 13, 6.5f);
        circle(5, 3.3f, 1.5f, 1.1f);
        break;
    }
}
#undef EVI_POLY
#undef EVI_STROKE
