// Tests for the parts of clipback that are plain logic: parsing, folder rules, the settings file. `test.bat` builds
// this into build\tests.exe and runs it; it exits with the number of failures.
//
// It is the whole program (clipback.cpp) with its entry point renamed, so every function can be called as it is.
// Nothing here starts the recorder, opens a window or touches the real settings.

#define wWinMain clipback_main
#include "clipback.cpp"
#undef wWinMain

static i32 g_failed, g_checked;

#define T(cond) \
    do { \
        g_checked++; \
        if (!(cond)) { \
            g_failed++; \
            printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        } \
    } while (0)

// The game picker's rows, and which picks are custom games or may be told to the art service
static void test_game_pick()
{
    const char games[3][64] = {"Minecraft", "Desktop", "Other"};
    const char online[2][128] = {"Simon", "Simon Says"};
    const i32 years[2] = {1978, 1990};
    GamePickRow rows[7];
    // Typed, nothing from the list or the art service: the name as typed, alone
    i32 n = game_pick_rows("simon", games, 3, 0, 0, online, years, 0, rows, 7);
    T(n == 1 && rows[0].kind == PICK_TYPED && !strcmp(rows[0].name, "simon") && !game_pick_known(rows[0]));
    // The art service's answer: its rows; the typed row goes, as a row has that name
    n = game_pick_rows("simon", games, 3, 0, 0, online, years, 2, rows, 7);
    T(n == 2 && rows[0].kind == PICK_ONLINE && !strcmp(rows[0].name, "Simon") && rows[0].year == 1978);
    T(rows[1].kind == PICK_ONLINE && game_pick_known(rows[0]));
    // A library game that matches, then the name as typed
    n = game_pick_rows("mine", games, 3, 0, 0, online, years, 0, rows, 7);
    T(n == 2 && rows[0].kind == PICK_LIBRARY && !strcmp(rows[0].name, "Minecraft") && game_pick_known(rows[0]));
    T(rows[1].kind == PICK_TYPED && !strcmp(rows[1].name, "mine"));
    // Nothing typed: the library's games, Other left out; Desktop isn't a known game
    n = game_pick_rows("", games, 3, 0, 0, online, years, 0, rows, 7);
    T(n == 2 && !strcmp(rows[1].name, "Desktop") && !game_pick_known(rows[1]));
    // The games list's answer comes with the key: an exact name first, the library's next, then the list's, and the
    // name as typed last; list games are known
    const char lib2[2][64] = {"Terraria Mods", "Desktop"};
    const char local[3][128] = {"Terraria", "Terraria: Otherworld", "Terra Nil"};
    n = game_pick_rows("terraria", lib2, 2, local, 3, online, years, 0, rows, 7);
    T(n == 4 && rows[0].kind == PICK_LIST && !strcmp(rows[0].name, "Terraria") && game_pick_known(rows[0]));
    T(rows[1].kind == PICK_LIBRARY && !strcmp(rows[1].name, "Terraria Mods"));
    T(rows[2].kind == PICK_LIST && !strcmp(rows[2].name, "Terraria: Otherworld"));
    // ... the typed row isn't there, as a row has that name; with "terra" it is, last
    n = game_pick_rows("terra", lib2, 2, local, 3, online, years, 0, rows, 7);
    T(n == 5 && rows[0].kind == PICK_LIBRARY && rows[4].kind == PICK_TYPED && !strcmp(rows[4].name, "terra"));
    // The art service's answer goes under the list's, one row a name, and moves no row above it
    const char online2[3][128] = {"Terraria", "TERRA-RIA", "Terrarium"};
    const i32 years2[3] = {2011, 2012, 2020};
    GamePickRow before[7];
    i32 nb = game_pick_rows("terraria", lib2, 2, local, 3, online2, years2, 0, before, 7);
    n = game_pick_rows("terraria", lib2, 2, local, 3, online2, years2, 3, rows, 7);
    T(n == nb + 1 && rows[nb].kind == PICK_ONLINE && !strcmp(rows[nb].name, "Terrarium"));
    for (i32 i = 0; i < nb; i++) T(!strcmp(rows[i].name, before[i].name) && rows[i].kind == before[i].kind);
    // The rows never pass `max`, the typed row kept
    n = game_pick_rows("terr", lib2, 2, local, 3, online2, years2, 3, rows, 4);
    T(n == 4 && rows[3].kind == PICK_TYPED);
    GamePickRow listed = {"Terraria", 0, PICK_LIST};
    game_set_custom("Terraria", true);
    game_pick_filed(listed);
    T(!game_is_custom("Terraria"));
    // A typed pick is a custom game until it's picked from the art service
    game_set_custom("Simon", false);
    GamePickRow typed = {"Simon", 0, PICK_TYPED}, lib_row = {"Simon", 0, PICK_LIBRARY}, svc = {"Simon", 1978, PICK_ONLINE};
    game_pick_filed(typed);
    T(game_is_custom("simon") && !game_pick_known(lib_row));
    GameInfo g;
    T(!gamedb_for_art("Simon", &g));
    game_pick_filed(svc);
    T(!game_is_custom("Simon") && game_pick_known(lib_row));
}

static void test_select_range()
{
    const u64 keys[] = {10, 20, 30, 40, 50};
    i32 lo = -1, hi = -1;
    T(lib_range_bounds(keys, 5, 20, 40, &lo, &hi) && lo == 1 && hi == 3); // anchor before
    T(lib_range_bounds(keys, 5, 50, 20, &lo, &hi) && lo == 1 && hi == 4); // anchor after
    T(lib_range_bounds(keys, 5, 30, 30, &lo, &hi) && lo == 2 && hi == 2); // the anchor itself
    T(lib_range_bounds(keys, 5, 99, 40, &lo, &hi) && lo == 3 && hi == 3); // anchor not shown: that card alone
    T(lib_range_bounds(keys, 5, 0, 10, &lo, &hi) && lo == 0 && hi == 0);  // no anchor
    T(!lib_range_bounds(keys, 5, 20, 99, &lo, &hi));                      // the card isn't shown
}

// Tags: how a typed one is made into a tag, putting tags on and off a list, and searching for them
static void test_tags()
{
    char t[LIB_TAG_MAX];
    lib_tag_clean("  #Ace,|Shot ", t, sizeof t);
    T(!strcmp(t, "aceshot"));
    char tags[LIB_TAGS_MAX] = "";
    T(lib_tag_set(tags, sizeof tags, "ace", true) && !strcmp(tags, "ace"));
    T(lib_tag_set(tags, sizeof tags, "funny", true) && !strcmp(tags, "ace,funny"));
    T(lib_tag_set(tags, sizeof tags, "ace", true) && !strcmp(tags, "ace,funny")); // already on
    T(lib_tag_has(tags, "funny") && !lib_tag_has(tags, "fun") && lib_tag_has(tags, ""));
    T(!lib_tag_has("", ""));
    T(lib_tags_match(tags, "#ace") && !lib_tags_match(tags, "#fun") && lib_tags_match(tags, "fun"));
    T(lib_tag_set(tags, sizeof tags, "ace", false) && !strcmp(tags, "funny"));
    T(lib_tag_set(tags, sizeof tags, "funny", false) && !strcmp(tags, ""));
    char small_list[8] = "abc";
    T(!lib_tag_set(small_list, sizeof small_list, "defgh", true) && !strcmp(small_list, "abc")); // no room
}

// Clicks on cards: which select, which play, and where the anchor goes. Keys 1 to 10 in the grid's order.
static void test_card_clicks()
{
    Library &lib = g_app.library;
    lib.item_count = lib.shown_count = 10;
    for (i32 i = 0; i < 10; i++) {
        lib.items[i] = {};
        lib.items[i].key = 10 - i; // items in any order; the grid's order is lib.shown
        lib.shown[i] = 9 - i;
    }
    lib_sel_clear();
    T(!lib_card_click(4, false, false, false) && lib.selected_count == 0); // a plain click plays
    T(lib_card_click(3, false, false, true) && lib.selected_count == 1 && lib.select_anchor == 3); // the tick box
    T(lib_card_click(7, true, false, true) && lib.selected_count == 5); // Shift on a tick box: 3 to 7
    T(lib.select_anchor == 7 && lib_selected(5) && !lib_selected(8)); // the Shift-clicked card is the anchor
    T(lib_card_click(1, true, false, false) && lib.selected_count == 7 && lib.select_anchor == 1); // 1 to 7
    T(lib_card_click(4, true, false, false) && lib.selected_count == 3 && lib.select_anchor == 4); // a ticked one:
    T(!lib_selected(1) && !lib_selected(4) && lib_selected(5) && lib_selected(7));        // 1 to 4 off
    T(lib_card_click(6, true, false, false) && lib.selected_count == 1 && lib_selected(7)); // 4 to 6 off
    T(lib_card_click(9, true, false, false) && lib.selected_count == 4 && lib.select_anchor == 9); // 6 to 9 on
    T(lib_card_click(10, false, false, false) && lib_selected(10) && lib.select_anchor == 10); // a click while selecting
    T(lib_card_click(9, false, true, false) && !lib_selected(9) && lib.select_anchor == 9);   // Ctrl: off
    T(lib_card_click(9, false, false, false) && lib_selected(9) && lib.select_anchor == 9);   // and on again
    T(lib_card_click(9, false, true, false) && !lib_selected(9) && lib.select_anchor == 9); // Ctrl: off again
    lib_sel_clear();
    T(lib.select_anchor == 0);
    T(lib_card_click(6, true, false, false) && lib.selected_count == 1 && lib.select_anchor == 6); // Shift, none: alone
    T(lib_card_click(8, true, false, false) && lib.selected_count == 3); // 6 to 8
    lib_sel_clear();
    T(lib_card_click(2, false, true, false) && lib.select_anchor == 2); // Ctrl + click
    T(lib_card_click(10, true, false, false) && lib.selected_count == 9);
    lib_sel_clear();
    lib_sel_all();
    T(lib.selected_count == 10 && lib.select_anchor == 1);
    lib_sel_clear();
    lib.item_count = lib.shown_count = 0;
}

// Copies of a file (same size and time) each get a key of their own; the first keeps the plain one.
static void test_unique_keys()
{
    static u64 seen[LIB_MAX_ITEMS * 2];
    memset(seen, 0, sizeof seen);
    u64 a = lib_key_unique(seen, 42, "Clip");
    u64 b = lib_key_unique(seen, 42, "Clip - Copy");
    u64 c = lib_key_unique(seen, 42, "Clip"); // the same name again, in another folder
    T(a == 42 && b != 42 && c != 42 && c != b);
    T(lib_key_unique(seen, 7, "Other") == 7);
    T(lib_key_unique(seen, 42 + LIB_MAX_ITEMS * 2, "Same slot") == 42 + LIB_MAX_ITEMS * 2);
}

// The player's sound menu: all of a file's tracks ticked plays its Mix, any fewer plays the ticked ones.
static void test_sound_plan()
{
    i32 out[MEDIA_MAX_TRACKS];
    T(lib_sound_plan(true, 4, 0, out) == -1);                          // Mix + 3, all ticked: the Mix
    T(lib_sound_plan(true, 4, 0xe, out) == -1);                        // every bit set is the same
    T(lib_sound_plan(true, 4, 0xa, out) == 2 && out[0] == 1 && out[1] == 3); // two of three
    T(lib_sound_plan(true, 4, 0x4, out) == 1 && out[0] == 2);          // one
    T(lib_sound_plan(false, 3, 0, out) == 3 && out[0] == 0 && out[2] == 2); // no Mix: all of them, each
    T(lib_sound_plan(false, 3, 0x2, out) == 1 && out[0] == 1);
    T(lib_sound_plan(false, 1, 0, out) == 1 && out[0] == 0);           // one track
}

static void test_lengths()
{
    T(parse_length_secs("30s") == 30);
    T(parse_length_secs("2m") == 120);
    T(parse_length_secs("1h") == 3600);
    T(parse_length_secs("90") == 90);
    T(parse_length_secs("1.5m") == 90);
    T(parse_length_secs("") == 0);
    T(parse_length_secs("abc") == 0);
    T(parse_length_secs("-5s") == 0);
    T(parse_length_secs("10x") == 0);

    // An exact range in Unix seconds; one word is a length instead
    i64 rf = 0, rt = 0;
    T(parse_range_us("1794901560 1794901860", &rf, &rt) == 1 && rf == 1794901560000000LL && rt == 1794901860000000LL);
    T(parse_range_us("1794901560.250 1794901860.5", &rf, &rt) == 1 && rf == 1794901560250000LL &&
      rt == 1794901860500000LL);
    T(parse_range_us("1794901860 1794901560", &rf, &rt) == -1);
    T(parse_range_us("1794901560", &rf, &rt) == 0);
    T(parse_range_us("abc 1794901860", &rf, &rt) == -1);
    T(parse_range_us("1794901560 x", &rf, &rt) == -1);
    T(parse_range_us("30s", &rf, &rt) == 0);
}

// Names from the template in Options are shown with a readable date; others as they are
static void test_shown_names()
{
    str_set(g_app.config.name_template, SAVE_NAME_DEFAULT);
    g_app.config.clock_24h = false;
    char name[300], out[300];
    lib_shown_name("Minecraft 15-00 20-11-2020", out, sizeof out);
    T(!strcmp(out, "Minecraft - 20 Nov 2020 at 3 pm"));
    lib_shown_name("Minecraft 21-30 20-11-2020 (2)", out, sizeof out);
    T(!strcmp(out, "Minecraft - 20 Nov 2020 at 9:30 pm (2)"));
    lib_shown_name("Minecraft 2026-11-19 21-30-00", out, sizeof out);
    T(!strcmp(out, "Minecraft 2026-11-19 21-30-00"));
    lib_shown_name("Minecraft 25-00 20-11-2020", out, sizeof out);
    T(!strcmp(out, "Minecraft 25-00 20-11-2020"));
    save_name_expand(SAVE_NAME_DEFAULT, "Minecraft", now_us(), name, sizeof name);
    lib_shown_name(name, out, sizeof out);
    T(!strncmp(out, "Minecraft - today at ", 21));
    save_name_expand(SAVE_NAME_DEFAULT, "Minecraft", now_us() - 86400ll * 1000000, name, sizeof name);
    lib_shown_name(name, out, sizeof out);
    T(!strncmp(out, "Minecraft - yesterday at ", 25));
    g_app.config.clock_24h = true;
    lib_shown_name("Over 9 15-05 20-11-2020", out, sizeof out);
    T(!strcmp(out, "Over 9 - 20 Nov 2020 at 15:05"));
    T(lib_name_matches("Minecraft 15-05 20-11-2020", "nov 2020"));
    // Sorting and day headings go by the name's time, or the file's when the name has none
    i64 at = now_us() - 3600ll * 1000000;
    save_name_expand(SAVE_NAME_DEFAULT, "Minecraft", at, name, sizeof name);
    i64 got = lib_name_us(SAVE_NAME_DEFAULT, name, 5);
    T(got <= at && at - got < 60ll * 1000000);
    T(lib_name_us(SAVE_NAME_DEFAULT, "Minecraft 2026-11-19 21-30-00", 5) == 5);
    str_set(g_app.config.name_template, "{YYYY}-{MM}-{DD} {hh}.{mm} {game}");
    lib_shown_name("2020-11-20 15.05 Overwatch 2", out, sizeof out);
    T(!strcmp(out, "Overwatch 2 - 20 Nov 2020 at 15:05"));
}

// The game in a file name comes from the save template; a name it doesn't read isn't ours
static void test_game_from_name()
{
    char g[64];
    game_from_name(SAVE_NAME_DEFAULT, "Minecraft 15-00 20-11-2020", g, sizeof g);
    T(!strcmp(g, "Minecraft"));
    game_from_name(SAVE_NAME_DEFAULT, "Over 9 21-30 20-11-2020 (2)", g, sizeof g);
    T(!strcmp(g, "Over 9"));
    game_from_name(SAVE_NAME_DEFAULT, "Clip 15-00 20-11-2020", g, sizeof g);
    T(!strcmp(g, GAME_DESKTOP));
    game_from_name(SAVE_NAME_DEFAULT, "Desktop 15-00 20-11-2020", g, sizeof g);
    T(!strcmp(g, GAME_DESKTOP));
    game_from_name("{YYYY}-{MM}-{DD} {hh}.{mm} {game}", "2020-11-20 15.05 Overwatch 2", g, sizeof g);
    T(!strcmp(g, "Overwatch 2"));
    game_from_name("{YYYY}-{MM}-{DD} {hh}.{mm}", "2020-11-20 15.05", g, sizeof g);
    T(!strcmp(g, GAME_DESKTOP));
    game_from_name(SAVE_NAME_DEFAULT, "Holiday video", g, sizeof g);
    T(!strcmp(g, GAME_NOT_OURS));
    game_from_name(SAVE_NAME_DEFAULT, "Minecraft 2026-11-19 21-30-00", g, sizeof g);
    T(!strcmp(g, GAME_NOT_OURS));
}

// A made-up games list for game_guess: Overwatch, Overwatch 2 and Minecraft, by their letters
static bool test_known(const char *name, char *out, i32 cap)
{
    char l[128];
    gdb_letters(name, l, sizeof l);
    const char *g = !strcmp(l, "overwatch2") ? "Overwatch 2" : !strcmp(l, "overwatch") ? "Overwatch" : !strcmp(l, "minecraft") ? "Minecraft" : 0;
    if (g) snprintf(out, cap, "%s", g);
    return g != 0;
}

// Backup: new and newer files are copied, fresh ones and ones the backup already has are left, clips only when asked,
// and nothing at the backup is deleted.
static void test_backup()
{
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    Path root = {};
    swprintf(root.s, MAX_PATH, L"%sclipback-test-backup-%u", temp, GetCurrentProcessId());
    Path from = path_join(root, L"from"), to = path_join(root, L"to");
    auto put = [](const Path &dir, const wchar_t *name, const char *text, i64 age_s) {
        SHCreateDirectoryExW(0, dir.s, 0);
        Path p = path_join(dir, name);
        FILE *f = _wfopen(p.s, L"wb");
        if (f) { fputs(text, f); fclose(f); }
        HANDLE h = CreateFileW(p.s, FILE_WRITE_ATTRIBUTES, 0, 0, OPEN_EXISTING, 0, 0);
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        u64 t = (((u64)ft.dwHighDateTime << 32) | ft.dwLowDateTime) - (u64)age_s * 10000000;
        ft.dwLowDateTime = (DWORD)t;
        ft.dwHighDateTime = (DWORD)(t >> 32);
        SetFileTime(h, 0, 0, &ft);
        CloseHandle(h);
    };
    auto has = [](const Path &p) { return GetFileAttributesW(p.s) != INVALID_FILE_ATTRIBUTES; };
    Path ses = path_join(path_join(from, L"2026"), L"Sessions"), clips = path_join(path_join(from, L"2026"), L"Clips");
    Path bses = path_join(path_join(to, L"2026"), L"Sessions"), bclips = path_join(path_join(to, L"2026"), L"Clips");
    put(ses, L"a.mp4", "a", 3600);
    put(ses, L"fresh.mp4", "f", 10); // still being written, maybe
    put(ses, L"b.mp4.part", "p", 3600);
    put(clips, L"c.mp4", "c", 3600);
    put(bses, L"old.mp4", "o", 3600); // only at the backup: stays

    g_backup.from = from;
    g_backup.to = to;
    g_backup.clips = false;
    g_backup.done = g_backup.total = g_backup.failed = 0;
    g_backup.error[0] = 0;
    InitializeCriticalSection(&g_backup.lock);
    backup_thread(0);
    T(g_backup.state == BACKUP_DONE && g_backup.done == 1 && g_backup.total == 1);
    T(has(path_join(bses, L"a.mp4")) && !has(path_join(bses, L"fresh.mp4")) && !has(path_join(bses, L"b.mp4.part")));
    T(!has(path_join(bclips, L"c.mp4")) && has(path_join(bses, L"old.mp4")));

    g_backup.clips = true;
    g_backup.done = g_backup.total = 0;
    backup_thread(0);
    T(g_backup.done == 1 && has(path_join(bclips, L"c.mp4"))); // a.mp4 is there already

    put(ses, L"a.mp4", "a2", 600); // changed since
    g_backup.done = g_backup.total = 0;
    backup_thread(0);
    T(g_backup.done == 1);
    T(backup_same_folder(from, from) && !backup_same_folder(from, to));
    DeleteCriticalSection(&g_backup.lock);
    g_backup.state = BACKUP_IDLE;

    SHFILEOPSTRUCTW op = {};
    wchar_t dbl[MAX_PATH + 2] = {};
    wcscpy_s(dbl, MAX_PATH, root.s);
    op.wFunc = FO_DELETE;
    op.pFrom = dbl;
    op.fFlags = FOF_NO_UI;
    SHFileOperationW(&op);
}

// Telemetry: the report and feedback bodies are the JSON the service takes, and feedback is cut to its limit.
static void test_telemetry()
{
    static char out[8192];
    StatCounts c = {3, 1, 2, 4, 5};
    tel_stats_json(c, out, sizeof out);
    T(strstr(out, "\"version\":\"" CLIPBACK_VERSION "\"") && strstr(out, "\"clips\":3,\"saves\":1,\"shares\":2,\"links\":4,\"strain\":5}"));
    tel_feedback_json(1, "  it \"works\"\n", out, sizeof out);
    T(!strcmp(out, "{\"version\":\"" CLIPBACK_VERSION "\",\"kind\":\"idea\",\"text\":\"it \\\"works\\\"\"}"));
    static char longer[3000];
    memset(longer, 'a', sizeof longer - 1);
    longer[FB_TEXT_MAX - 1] = (char)0xc3; // a two-byte character across the limit is dropped whole
    longer[FB_TEXT_MAX] = (char)0xa9;
    tel_feedback_json(2, longer, out, sizeof out);
    const char *t = strstr(out, "\"text\":\"") + 8;
    T((i32)(strrchr(out, '"') - t) == FB_TEXT_MAX - 1);
}

static void test_first_run()
{
    f32 gb, keep;
    buffer_for_free(1000, &gb, &keep); // a big drive: the most
    T(gb == 200 && keep == 50);
    buffer_for_free(500, &gb, &keep); // a quarter, in tens
    T(gb == 120 && keep == 50);
    buffer_for_free(60, &gb, &keep); // a small drive keeps less free
    T(gb == 10 && keep == 12);
    buffer_for_free(5, &gb, &keep);
    T(gb == 10 && keep == 1);

    Config c = {};
    char keys[64];
    i32 secs = -1;
    T(!setup_clip_key(c, keys, sizeof keys, &secs));
    str_set(c.hotkeys[0].keys.s, "Alt+F9");
    str_set(c.hotkeys[0].action.s, "timeline");
    str_set(c.hotkeys[1].keys.s, "Alt+F10");
    str_set(c.hotkeys[1].action.s, "save 45s");
    c.hotkey_count = 2;
    T(setup_clip_key(c, keys, sizeof keys, &secs) && !strcmp(keys, "Alt+F10") && secs == 45);
}

static void test_game_guess()
{
    char g[64];
    T(game_guess("Overwatch 2 2024.01.15 - 20.14.03.02.DVR", test_known, g, sizeof g) && !strcmp(g, "Overwatch 2"));
    T(game_guess("Minecraft 2024-01-15 20-14-03", test_known, g, sizeof g) && !strcmp(g, "Minecraft"));
    T(game_guess("Overwatch_20240115_201403", test_known, g, sizeof g) && !strcmp(g, "Overwatch"));
    T(!game_guess("Minecraft castle tour 2024", test_known, g, sizeof g)); // words after the game: not sure
    T(!game_guess("Minecraft", test_known, g, sizeof g));                  // no date or time after it
    T(!game_guess("2024-01-15 20-14-03", test_known, g, sizeof g));        // OBS: no game in it
    T(!game_guess("Holiday 2024-01-15", test_known, g, sizeof g));
}

static void test_buffer_names()
{
    u32 n = 0;
    T(seg_number("v_00012.mkv", &n) && n == 12);
    T(!seg_number("list.csv", &n));
    T(!seg_number("v_x.mkv", &n));

    const char *json = "{\"game\": \"Over\\\"watch\", \"start_us\": 123, \"paused\":true}";
    char out[64];
    T(json_field(json, "game", out, sizeof out) && !strcmp(out, "Over\"watch"));
    T(json_field(json, "start_us", out, sizeof out) && !strcmp(out, "123"));
    T(json_field(json, "paused", out, sizeof out) && !strcmp(out, "true"));
    T(!json_field(json, "missing", out, sizeof out));
}

static void test_folder_rules()
{
    // How deep the folder is when the exe is inside it; 0 when it isn't
    T(path_under("C:\\A\\B\\x.exe", "C:\\A") == 2);
    T(path_under("C:\\A\\B\\x.exe", "c:/a/b/") == 3); // case, slashes and a trailing slash make no difference
    T(path_under("C:\\A", "C:\\A") == 0);             // the folder itself is not inside it
    T(path_under("C:\\AB\\x.exe", "C:\\A") == 0);     // a name that only starts the same
    T(path_under("C:\\A\\x.exe", "") == 0);
    T(path_under("C:\\V\\user1\\off\\x.exe", "C:\\V\\*\\off") == 4); // * is any one folder
    T(path_under("C:\\V\\off\\x.exe", "C:\\V\\*\\off") == 0);

    static FolderRule rules[4];
    str_set(rules[0].path, "C:\\V");
    rules[0].category = CAT_APP;
    str_set(rules[1].path, "C:\\V\\off");
    rules[1].category = CAT_IGNORED;
    str_set(rules[2].path, "C:\\V\\on");
    rules[2].category = CAT_COMMUNICATION;
    str_set(rules[3].path, "C:\\V\\on");
    rules[3].category = CAT_IGNORED;
    T(folder_rule_for(rules, 3, "C:\\V\\x.exe") == 0);
    T(folder_rule_for(rules, 3, "C:\\V\\off\\x.exe") == 1); // the deepest wins
    T(folder_rule_for(rules, 3, "C:\\V\\on\\x.exe") == 2);
    T(folder_rule_for(rules, 4, "C:\\V\\on\\x.exe") == 3);  // a tie goes to ignored
    T(folder_rule_for(rules, 3, "D:\\x.exe") == -1);
    T(folder_rule_for(rules, 3, "") == -1);
    T(folder_rule_for(rules, 3, 0) == -1);

    static AppInfo apps[2];
    str_set(apps[0].exe.s, "own.exe"); // has its own category: the folder leaves it alone
    apps[0].category = CAT_GAME;
    str_set(apps[0].path, "C:\\V\\off\\own.exe");
    str_set(apps[1].exe.s, "new.exe"); // has none: the folder it was last heard from decides
    str_set(apps[1].path, "C:\\V\\off\\new.exe");
    T(app_category_in(apps, 2, rules, 3, "own.exe", "") == CAT_GAME);
    T(app_category_in(apps, 2, rules, 3, "new.exe", "") == CAT_IGNORED);
    T(app_category_in(apps, 2, rules, 3, "new.exe", "C:\\V\\on\\new.exe") == CAT_COMMUNICATION); // where it runs now
    T(app_category_in(apps, 2, rules, 3, "unknown.exe", "C:\\V\\unknown.exe") == CAT_APP);
    T(app_category_in(apps, 2, rules, 3, "unknown.exe", "D:\\unknown.exe") == CAT_NONE);
}

static void test_fields()
{
    char a[] = "  a | b |c \r\n", *f[4];
    T(split_fields(a, f, 4) == 3 && !strcmp(f[0], "a") && !strcmp(f[1], "b") && !strcmp(f[2], "c"));
    char b[] = "a||c";
    T(split_fields(b, f, 4) == 3 && !strcmp(f[1], ""));
    char c[] = "a|b|c";
    T(split_fields(c, f, 2) == 2 && !strcmp(f[1], "b"));

    for (i32 i = CAT_NONE; i <= CAT_IGNORED; i++) T(category_parse(category_name((AppCategory)i)) == i);
    T(session_mode_parse(session_mode_name(SESSION_ALWAYS)) == SESSION_ALWAYS);
    T(session_mode_parse(session_mode_name(SESSION_NEVER)) == SESSION_NEVER);
    T(session_mode_parse("nonsense") == SESSION_ASK);
}

static void test_hotkeys()
{
    u32 mods = 0, vk = 0;
    T(hotkey_parse("Alt+F10", &mods, &vk) && mods == (MOD_NOREPEAT | MOD_ALT) && vk == VK_F10);
    T(hotkey_parse("ctrl + shift + s", &mods, &vk) && mods == (MOD_NOREPEAT | MOD_CONTROL | MOD_SHIFT) && vk == 'S');
    T(hotkey_parse("Win+Num5", &mods, &vk) && vk == VK_NUMPAD5);
    T(hotkey_parse("F24", &mods, &vk) && vk == VK_F24);
    T(!hotkey_parse("F25", &mods, &vk));
    T(!hotkey_parse("Ctrl", &mods, &vk));     // no key
    T(!hotkey_parse("Ctrl+A+B", &mods, &vk)); // two keys
    T(!hotkey_parse("Alt+Nope", &mods, &vk));
    T(!hotkey_parse("", &mods, &vk));
}

// What is saved comes back the same.
static void test_settings_round_trip()
{
    static Paths paths;
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    swprintf(paths.config_dir.s, MAX_PATH, L"%sclipback-tests-%lu", temp, GetCurrentProcessId());
    paths.local_dir = paths.config_dir;
    paths.settings = path_join(paths.config_dir, L"settings.txt");
    CreateDirectoryW(paths.config_dir.s, 0);

    static Config a, b;
    T(!config_load(&b, paths)); // no file yet
    config_defaults(&a, paths);
    a.bitrate_mbps = 42.5f;
    a.fps = 120;
    a.buffer_by_size = true;
    a.dev_log = true;
    a.clock_24h = !a.clock_24h; // the other way from Windows' format, so loading it can't pass by default
    a.voice_mono = false;        // the default is on
    a.timeline_top = 0.4f;
    a.timeline_names = 220;
    a.editor_top = 0.6f;
    a.editor_lane = 60;
    a.editor_zoom_view = 1;
    str_set(a.codec.s, "hevc");
    a.hotkey_count = 1;
    str_set(a.hotkeys[0].keys.s, "Alt+F10");
    str_set(a.hotkeys[0].action.s, "save 30s");
    a.app_count = 0;
    AppInfo *app = config_app(&a, "game.exe");
    str_set(app->name.s, "The Game");
    app->category = CAT_GAME;
    app->session_mode = SESSION_ALWAYS;
    app->rec.fps = 60;
    app->rec.window = -1;
    a.window_capture = true;
    str_set(app->path, "C:\\Games\\The Game\\game.exe");
    a.folder_count = 1;
    str_set(a.folders[0].path, "C:\\V\\*\\off");
    a.folders[0].category = CAT_IGNORED;
    a.input_count = 2;
    a.inputs[0] = {"{0.0.1.00000000}.{a}", "Line (RODE AI-1)", INPUT_STEREO};
    a.inputs[1] = {"{0.0.1.00000000}.{b}", "Off one", INPUT_OFF}; // off isn't saved

    T(config_save(a, paths));
    T(config_load(&b, paths));
    T(b.bitrate_mbps == 42.5f && b.fps == 120 && b.buffer_by_size && b.dev_log && !strcmp(b.codec.s, "hevc"));
    T(b.hotkey_count == 1 && !strcmp(b.hotkeys[0].keys.s, "Alt+F10") && !strcmp(b.hotkeys[0].action.s, "save 30s"));
    T(b.app_count == 1);
    if (b.app_count == 1) {
        const AppInfo &g = b.apps[0];
        T(!strcmp(g.exe.s, "game.exe") && !strcmp(g.name.s, "The Game") && g.category == CAT_GAME);
        T(g.session_mode == SESSION_ALWAYS && g.rec.fps == 60 && !strcmp(g.path, "C:\\Games\\The Game\\game.exe"));
        T(g.rec.window == -1 && b.window_capture);
        // capture window: a game's own setting wins over the default, and only a game has a window to record
        T(!window_capture_for(b, "game.exe", g.path));
        b.apps[0].rec.window = 0;
        T(window_capture_for(b, "game.exe", g.path));
        b.window_capture = false;
        T(!window_capture_for(b, "game.exe", g.path));
        b.apps[0].rec.window = 1;
        T(window_capture_for(b, "game.exe", g.path) && !window_capture_for(b, "notepad.exe", "C:\\Windows\\notepad.exe"));
        // a window is recorded through Windows Graphics Capture by its handle, fitted into a fixed size
        static Args args;
        RecSettings rs = {};
        rs.fps = 60;
        rs.hmonitor = 7;
        str_set(rs.codec.s, "h264");
        capture_args(&args, VENDOR_CPU, rs, true);
        T(wcsstr(args.s, L"gfxcapture=hmonitor=7:") && !wcsstr(args.s, L"hwnd="));
        rs.hwnd = 1234;
        rs.width = 2560;
        rs.height = 1440;
        capture_args(&args, VENDOR_CPU, rs, true);
        T(wcsstr(args.s, L"gfxcapture=hwnd=1234:") && wcsstr(args.s, L":width=2560:height=1440:resize_mode=scale_aspect"));
    }
    T(b.clock_24h == a.clock_24h);
    T(!b.voice_mono);
    T(b.timeline_top == 0.4f);
    T(b.timeline_names == 220);
    T(b.editor_top == 0.6f);
    T(b.editor_lane == 60);
    T(b.editor_zoom_view == 1);
    T(b.input_count == 1 && !strcmp(b.inputs[0].id, "{0.0.1.00000000}.{a}") &&
      !strcmp(b.inputs[0].name, "Line (RODE AI-1)") && b.inputs[0].mode == INPUT_STEREO);
    char short_name[64];
    input_short_name("Line (RODE AI-1)", short_name, sizeof short_name);
    T(!strcmp(short_name, "RODE AI-1"));
    input_short_name("Stereo Mix", short_name, sizeof short_name);
    T(!strcmp(short_name, "Stereo Mix"));
    T(b.folder_count == 1 && !strcmp(b.folders[0].path, "C:\\V\\*\\off") && b.folders[0].category == CAT_IGNORED);
    T(!wcscmp(a.save_dir.s, b.save_dir.s) && !wcscmp(a.buffer_dir.s, b.buffer_dir.s));

    DeleteFileW(paths.settings.s);
    RemoveDirectoryW(paths.config_dir.s);
}

// The editor's volume keys and eases: the curve, the volume at a time, and the edit file round trip.
// The editor's lanes: keys go in by time, and a selection stays on the same keys through inserts, erases and sorts.
// Share: the recommended rate, the bitrates for a size, and what one click does with a clip.
static void test_share()
{
    // Recommended video rates at 60 fps, between them along the pixel count, 0.65 at 30 fps
    T(share_reco_kbps(1280, 720, 60) == 5000 && share_reco_kbps(1920, 1080, 60) == 9000);
    T(share_reco_kbps(2560, 1440, 60) == 15000 && share_reco_kbps(3840, 2160, 60) == 30000);
    T(fabs(share_reco_kbps(1920, 1080, 30) - 5850) < 1 && fabs(share_reco_kbps(3840, 2160, 30) - 19500) < 1);
    T(fabs(share_reco_kbps(640, 360, 60) - 1250) < 1 && share_reco_kbps(0, 0, 0) == 9000);
    f64 mid = share_reco_kbps(2240, 1260, 60); // between 1080p and 1440p
    T(mid > 9000 && mid < 15000);
    T(share_reco_kbps(1920, 1080, 45) > 5850 && share_reco_kbps(1920, 1080, 45) < 9000);
    // 20 s of 1080p60 with sound: (9000 + 128) kbit/s
    T(share_reco_bytes(20, 1920, 1080, 60, true) == 22820000);
    T(share_reco_bytes(20, 1920, 1080, 60, false) == 22500000);

    // Bitrates for 20 s squeezed to 20 MB: the top sound tier, the video the rest, under the ceiling
    const i64 mb = (i64)SHARE_MIB;
    ShareRate r;
    T(share_rate(20, true, 20 * mb, 20 * mb, 0, &r));
    T(r.audio_kbps == 128 && r.video_kbps > 7800 && r.video_kbps < 7900 && r.ceiling < 20 * mb && !r.capped);
    // Never more than the source has
    T(share_rate(20, true, 50 * mb, 0, 3000, &r) && r.capped && r.video_kbps == 3000 && !r.ceiling);
    // An hour into 1 MB: too few bits
    T(!share_rate(3600, true, mb, mb, 0, &r));
    // Little room: a smaller sound tier
    T(share_rate(60, true, 3 * mb, 3 * mb, 0, &r) && r.audio_kbps == 96);

    // One click: a file as is under the max, squeezed to it up to 1.5 times over, a link further over
    ShareHosts free = {false, true, true, 72, ""}, bucket = {true, true, false, 72, ""}, none = {};
    ShareDecision d = share_decide_for(15 * mb, 20, 1.5, free);
    T(d.kind == SHARE_AS_IS && d.file_ok && !d.file_squeezed && d.file_bytes == 15 * mb && !strcmp(d.why, "15 MB, as is"));
    d = share_decide_for(25 * mb, 20, 1.5, free);
    T(d.kind == SHARE_SQUEEZED && d.file_ok && d.file_squeezed && d.file_bytes < 20 * mb);
    T(!strcmp(d.why, "25 MB squeezed to 20 MB"));
    d = share_decide_for(31 * mb, 20, 1.5, free);
    T(d.kind == SHARE_LINK && !d.file_ok && d.link_ok && d.host == HOST_CATBOX && !d.link_squeezed);
    T(d.link_bytes == 31 * mb && !strcmp(d.host_label, "Catbox"));
    d = share_decide_for(21 * mb, 20, 1.0, free); // squeeze off
    T(d.kind == SHARE_LINK);
    // No bucket: Catbox as is to 200 MB, squeezed to 300 MB; Litterbox as is to 1 GB, squeezed to 1.5 GB
    d = share_decide_for(250000000, 20, 1.5, free);
    T(d.host == HOST_CATBOX && d.link_squeezed && d.link_bytes < 200000000);
    d = share_decide_for(400000000, 20, 1.5, free);
    T(d.host == HOST_LITTERBOX && !d.link_squeezed && d.link_bytes == 400000000);
    T(!strcmp(d.host_label, "Litterbox, 72 h"));
    d = share_decide_for(1200000000, 20, 1.5, free);
    T(d.host == HOST_LITTERBOX && d.link_squeezed && d.link_bytes < 1000000000);
    d = share_decide_for(2000000000, 20, 1.5, free);
    T(!d.link_ok && starts_with(d.link_why, "Needs your own Cloudflare"));
    d = share_decide_for(2000000000, 20, 1.5, none);
    T(!d.link_ok && starts_with(d.link_why, "Turn on a free host"));
    ShareHosts up = {false, false, false, 0, "MyUploader"};
    d = share_decide_for(2000000000, 20, 1.5, up);
    T(d.link_ok && d.host == HOST_CUSTOM && !d.link_squeezed && !strcmp(d.host_label, "MyUploader"));
    // A bucket: Catbox (when on) for what it takes, else the bucket as is, however big
    d = share_decide_for(250000000, 20, 1.5, bucket);
    T(d.host == HOST_CATBOX && d.link_squeezed);
    d = share_decide_for(400000000, 20, 1.5, bucket);
    T(d.host == HOST_R2 && !d.link_squeezed && d.link_bytes == 400000000 && !strcmp(d.host_label, "your bucket"));
    bucket.catbox = false;
    d = share_decide_for(50 * mb, 20, 1.5, bucket);
    T(d.host == HOST_R2 && !d.link_squeezed);
    d = share_decide_for(5000000000, 20, 1.5, bucket);
    T(d.link_ok && d.host == HOST_R2);
    // Sizes in the menu: whole MB from 10, one decimal under, GB from 1000
    char t[24];
    share_size_text(5 * mb + mb / 2, t, sizeof t);
    T(!strcmp(t, "5.5 MB"));
    share_size_text(2000 * mb, t, sizeof t);
    T(!strcmp(t, "2 GB"));
}

// The editor's lanes: a track's kind from its title, and waveforms read only where a chunk is measured.
static void test_wave_chunks()
{
    T(ed_track_kind("sam (chat)", 2) == KIND_CHAT && ed_track_kind("Overwatch (game)", 3) == KIND_GAME);
    T(ed_track_kind("Mic", 1) == KIND_MIC && ed_track_kind("Brave Browser", 4) == KIND_APP);
    T(ed_track_kind("Mix", EDIT_MASTER) == KIND_MIX);

    // 25 s of one track: three chunks (10, 10 and 5 s), the middle one measured, loud at 15 s.
    static WaveData w;
    w = {};
    InitializeSRWLock(&w.lock);
    w.refs = 1;
    w.count = 1;
    w.slices = 25 * WAVE_RATE;
    w.chunks = 3;
    static u8 level_data[2 * 25 * WAVE_RATE + 64];
    memset(level_data, 0, sizeof level_data);
    u8 *at = level_data;
    for (i32 s = w.slices;; s = (s + 1) / 2) {
        w.size[w.levels] = s;
        w.lvl[0][w.levels++] = at;
        at += s;
        if (s == 1) break;
    }
    static volatile u8 known[3];
    known[0] = known[1] = known[2] = WAVE_UNKNOWN;
    w.known = known;
    w.lvl[0][0][15 * WAVE_RATE] = 255;
    f32 pk;
    T(!wave_peak(&w, 0, 14, 16, &pk));
    wave_chunk_done(&w, 1);
    T(wave_peak(&w, 0, 14.9, 15.1, &pk) && pk > 0.99f);
    T(wave_peak(&w, 0, 0, 25, &pk) && pk > 0.99f); // the coarse levels see it too
    T(!wave_peak(&w, 0, 1, 9, &pk));               // chunk 0 isn't measured
    T(wave_peak(&w, 0, 11, 12, &pk) && pk == 0);
    T(w.loudest[0] == 255 && w.known_count == 1);

    // Claiming: the view's chunks first, then outwards, never one already taken.
    known[0] = known[1] = known[2] = WAVE_UNKNOWN;
    w.view_from = 21;
    w.view_to = 24;
    i32 first, end;
    T(wave_claim(&w, &first, &end) && first == 2 && end == 3);
    T(wave_claim(&w, &first, &end) && first == 1 && end == 2);
    T(wave_claim(&w, &first, &end) && first == 0 && end == 1);
    T(!wave_claim(&w, &first, &end));
}

static void test_lanes()
{
    T(bits_insert(0b101, 1) == 0b1001);
    T(bits_insert(0b1, 0) == 0b10);
    T(bits_erase(0b1101, 1) == 0b111);
    T(bits_erase(0b1, 0) == 0);
    T(bits_all(3) == 0b111 && bits_all(64) == ~0ull);
    static EditRecord e;
    e = {};
    u64 sel = 0;
    LaneKey q = {};
    q.t = 2; q.v = 0.5; T(ln_insert(e, 3, q, &sel) == 0);
    q.t = 1; q.v = 1.5; T(ln_insert(e, 3, q, &sel) == 0);
    q.t = 3; q.v = 1.0; T(ln_insert(e, 3, q, &sel) == 2);
    T(e.vol_count[2] == 3 && e.vol[2][0].t == 1 && e.vol[2][1].t == 2);
    sel = 0b100; // the key at 3 s
    e.vol[2][2].t = 0.5; // dragged to the front
    ln_sort(e, 3, &sel);
    T(e.vol[2][0].t == 0.5 && e.vol[2][0].v == 1.0f && sel == 0b1);
    ln_erase(e, 3, 0, &sel);
    T(e.vol_count[2] == 2 && sel == 0);
    T(fabs(ln_value_at(e, 3, 1.5) - 1.0) < 1e-6); // linear from 1.5 to 0.5
    // A zoom key keeps its aim through the lane.
    q = {};
    q.t = 4; q.v = 2; q.x = 0.25; q.y = 0.75; q.ease = EASE_HOLD;
    T(ln_insert(e, 0, q, 0) == 0);
    LaneKey g = ln_get(e, 0, 0);
    T(g.v == 2 && g.x == 0.25 && g.y == 0.75 && g.ease == EASE_HOLD);
}

// Pieces: output time and source time both ways, split, ripple delete (with the keys in what's removed), trim held
// by the neighbours, and reorder.
static void test_pieces()
{
    static EditRecord e;
    e = {};
    const f64 dur = 10;
    EditPiece p[EDIT_MAX_PIECES];
    auto same = [](f64 a, f64 b) { return fabs(a - b) < 1e-9; };
    T(pieces_of(e, dur, p) == 1 && p[0].from == 0 && p[0].to == dur);
    T(same(pieces_len(e, dur), 10));

    // Split at 4, then at 7: 0-4, 4-7, 7-10.
    T(pieces_split(&e, dur, 4) && e.piece_count == 2);
    T(pieces_split(&e, dur, 7) && e.piece_count == 3);
    T(!pieces_split(&e, dur, 7.05)); // too close to an edge
    T(same(e.pieces[1].from, 4) && same(e.pieces[1].to, 7));
    T(same(pieces_len(e, dur), 10));

    // Ripple delete the middle: 0-4, 7-10, 7 s long; the keys at 5 go, the ones at 3 and 4 (an edge kept) stay.
    e.keys[0] = {3.0, 2.0, 0.5, 0.5, EASE_LINEAR};
    e.keys[1] = {5.0, 2.0, 0.5, 0.5, EASE_LINEAR};
    e.key_count = 2;
    e.vol[1][0] = {4.0, 0.5f, EASE_LINEAR};
    e.vol[1][1] = {5.5, 0.5f, EASE_LINEAR};
    e.vol_count[1] = 2;
    static EditRecord before;
    before = e;
    T(pieces_delete(&e, dur, 0b010));
    T(e.piece_count == 2 && same(pieces_len(e, dur), 7));
    T(e.key_count == 1 && e.keys[0].t == 3.0);
    T(e.vol_count[1] == 1 && e.vol[1][0].t == 4.0);
    T(!pieces_delete(&e, dur, 0b11)); // never all of them
    T(!pieces_delete(&e, dur, 0));

    // Output and source time: 5 s out is 8 s in; 8 s in is 5 s out; 5 s in is cut.
    i32 at = -1;
    f64 u = 0;
    T(same(piece_src(e, dur, 5, &at), 8) && at == 1);
    T(piece_out(e, dur, 8, &u, &at) && same(u, 5) && at == 1);
    T(!piece_out(e, dur, 5, &u));
    T(piece_out(e, dur, 10, &u) && same(u, 7)); // the end is the last piece's
    T(same(piece_src(e, dur, 99), 10) && same(piece_src(e, dur, -1), 0));
    T(same(piece_start(e, dur, 1), 4));

    // Trim: the first piece's end stops at the second's start once they're in the same place.
    T(same(pieces_trim(&e, dur, 0, true, 5.5), 5.5));
    T(same(pieces_len(e, dur), 8.5));
    T(same(pieces_trim(&e, dur, 1, false, 2), 5.5)); // held by the first piece's end
    T(same(pieces_trim(&e, dur, 1, true, 9.95), 9.95));
    T(same(pieces_trim(&e, dur, 1, true, 3), 5.5 + PIECE_LEAST)); // never shorter than the least
    pieces_trim(&e, dur, 1, true, 10);

    // Reorder: 5.5-10 first, then 0-5.5; keys stay on their moment, so the key at 3 s plays at 7.5.
    pieces_move(&e, dur, 1, 0);
    T(same(e.pieces[0].from, 5.5) && same(e.pieces[1].to, 5.5));
    T(piece_out(e, dur, 3, &u, &at) && same(u, 7.5) && at == 1);
    pieces_move(&e, dur, 0, 1);
    T(same(e.pieces[0].from, 0));

    // Joined back into one whole piece it's stored as none; undo is a copy of the record, keys and all.
    pieces_delete(&e, dur, 0b10);
    pieces_trim(&e, dur, 0, true, 10);
    T(e.piece_count == 0);
    e = before;
    T(e.piece_count == 3 && e.key_count == 2 && e.vol_count[1] == 2);
}

static void test_edit_volumes()
{
    T(ease_curve(EASE_LINEAR, EASE_LINEAR, 0.25) == 0.25);
    T(ease_curve(EASE_HOLD, EASE_LINEAR, 0.9) == 0);
    T(fabs(ease_curve(EASE_SMOOTH, EASE_SMOOTH, 0.5) - 0.5) < 1e-9);
    T(ease_curve(EASE_OUT, EASE_LINEAR, 0.5) == 0.25);   // slow leaving: f^2
    T(ease_curve(EASE_LINEAR, EASE_IN, 0.5) == 0.75);    // slow arriving: f(2 - f)

    static EditRecord e;
    e = {};
    T(vol_at(e, 2, 5) == 1);
    e.boost[2] = 0.5f;
    T(fabs(vol_at(e, 2, 5) - 1.5) < 1e-6);
    e.vol[2][0] = {1.0, 0.0f, EASE_LINEAR};
    e.vol[2][1] = {3.0, 2.0f, EASE_LINEAR};
    e.vol_count[2] = 2;
    T(vol_at(e, 2, 0) == 0);               // holds before the first key
    T(fabs(vol_at(e, 2, 2) - 1) < 1e-6);   // halfway
    T(vol_at(e, 2, 9) == 2);               // and after the last
    e.muted[2] = true;
    T(vol_heard(e, 2, 2) == 0);
    e.muted[2] = false;
    e.vol[EDIT_MASTER][0] = {4.0, 0.5f, EASE_HOLD};
    e.vol_count[EDIT_MASTER] = 1;
    e.keys[0] = {2.0, 2.0, 0.25, 0.75, EASE_OUT};
    e.key_count = 1;
    e.pieces[0] = {1.5, 6};
    e.pieces[1] = {0.25, 1};
    e.piece_count = 2;
    // Colour: as recorded without keys; eased between keys; brightness given to eq as an offset
    T(color_at(e, COLOR_SATURATION, 3) == 1 && !edit_colored(e));
    e.color[COLOR_SATURATION][0] = {2.0, 0.0f, EASE_LINEAR};
    e.color[COLOR_SATURATION][1] = {4.0, 2.0f, EASE_LINEAR};
    e.color_count[COLOR_SATURATION] = 2;
    T(fabs(color_at(e, COLOR_SATURATION, 3) - 1) < 1e-6 && edit_colored(e));
    T(color_eq(COLOR_BRIGHTNESS, 1) == 0 && fabs(color_eq(COLOR_BRIGHTNESS, 2) - EQ_BRIGHTNESS) < 1e-9);
    T(color_eq(COLOR_CONTRAST, 1.5) == 1.5);
    // Transitions: held to the pieces and, for a crossfade, to the footage past the cut; fades scale towards black
    e.pieces[1].trans = TRANS_CROSS;
    e.pieces[1].trans_len = 2;
    e.fade_in = e.fade_out = 1;
    {
        EditPiece pc[EDIT_MAX_PIECES];
        i32 np = pieces_of(e, 10, pc);
        Transitions tr = transitions_of(e, pc, np, 10);
        T(tr.into[1] == TRANS_CROSS && fabs(tr.len[1] - 0.5) < 1e-9); // 0.25 s before the second piece's start
        T(tr.fade_in == 1 && fabs(tr.fade_out - 0.375) < 1e-9);         // half the 0.75 s last piece
        T(fabs(fade_gain(e, 10, 0, 2.0) - 0.5) < 1e-6 && fade_gain(e, 10, 0, 4) == 1);
        T(fabs(fade_gain(e, 10, 1, 1 - 0.1875) - 0.5) < 1e-6);
        e.pieces[1].trans = TRANS_FADE;
        np = pieces_of(e, 10, pc);
        tr = transitions_of(e, pc, np, 10);
        T(tr.into[1] == TRANS_FADE && fabs(tr.len[1] - 0.75) < 1e-9 && transitions_any(tr, np));
        T(fabs(fade_gain(e, 10, 1, 0.25 + 0.1875) - 0.5) < 1e-6);
        e.pieces[1].trans = TRANS_CROSS;
    }

    wchar_t dir[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    Path file = {};
    swprintf(file.s, MAX_PATH, L"%sclipback-test-%u.edit.txt", dir, GetCurrentProcessId());
    edit_write(file, e);
    static EditRecord r;
    edit_read(file, &r);
    DeleteFileW(file.s);
    T(r.piece_count == 2 && r.pieces[0].from == 1.5 && r.pieces[0].to == 6 && r.pieces[1].from == 0.25);
    T(r.vol_count[2] == 2 && r.vol[2][1].t == 3.0 && r.vol[2][1].v == 2.0f);
    T(fabs(r.boost[2] - 0.5f) < 1e-4f);
    T(r.vol_count[EDIT_MASTER] == 1 && r.vol[EDIT_MASTER][0].ease == EASE_HOLD && r.vol[EDIT_MASTER][0].v == 0.5f);
    T(r.key_count == 1 && r.keys[0].ease == EASE_OUT && r.keys[0].x == 0.25);
    T(r.color_count[COLOR_SATURATION] == 2 && r.color[COLOR_SATURATION][1].v == 2.0f &&
      r.color[COLOR_SATURATION][1].ease == EASE_LINEAR && r.color_count[COLOR_BRIGHTNESS] == 0);
    T(r.pieces[1].trans == TRANS_CROSS && r.pieces[1].trans_len == 2.0f && r.pieces[0].trans == TRANS_CUT);
    T(r.fade_in == 1.0f && r.fade_out == 1.0f);
    T(edit_is_default(EditRecord{}));
}

// The games list: the fetched JSON to the file that is loaded, and finding a game by its exe and by its name.
static void test_gamedb()
{
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    Path dir = {};
    swprintf(dir.s, MAX_PATH, L"%sclipback-test-gamedb-%u", temp, GetCurrentProcessId());
    CreateDirectoryW(dir.s, 0);
    Path json = path_join(dir, L"d.json"), tsv = path_join(dir, L"g.tsv");
    static const char *list =
        "[{\"id\":\"1\",\"name\":\"Minecraft\",\"aliases\":[\"Minecraft Launcher\"],\"icon_hash\":\"aa\",\"cover_image_hash\":null,"
        "\"executables\":[{\"is_launcher\":false,\"name\":\">javaw.exe\",\"os\":\"win32\",\"arguments\":\"net.minecraft.client.main.Main\"},"
        "{\"name\":\">java\",\"os\":\"darwin\"}],\"third_party_skus\":[],\"hook\":true},"
        "{\"id\":\"2\",\"name\":\"Counter-Strike 2\",\"aliases\":[],\"icon_hash\":\"bb\",\"cover_image_hash\":\"cc\","
        "\"executables\":[{\"is_launcher\":false,\"name\":\"win64/cs2.exe\",\"os\":\"win32\"}],"
        "\"third_party_skus\":[{\"distributor\":\"xbox\",\"id\":\"9X\"},{\"distributor\":\"steam\",\"id\":\"730\"}]},"
        "{\"id\":\"3\",\"name\":\"Rust\",\"aliases\":[\"Rust\"],\"icon_hash\":null,\"cover_image_hash\":null,"
        "\"executables\":[{\"is_launcher\":true,\"name\":\"rust.exe\",\"os\":\"win32\"},{\"is_launcher\":false,\"name\":\"rustclient.exe\",\"os\":\"win32\"}],"
        "\"third_party_skus\":[{\"distributor\":\"steam\",\"id\":\"252490\"}]},"
        "{\"id\":\"4\",\"name\":\"Garry\u0027s Mod\",\"aliases\":[],\"icon_hash\":null,\"cover_image_hash\":null,"
        "\"executables\":[{\"name\":\"garrysmod/hl2.exe\",\"os\":\"win32\",\"arguments\":\"-game garrysmod\"},{\"name\":\">hl2.exe\",\"os\":\"win32\",\"arguments\":\"-game garrysmod\"}],"
        "\"third_party_skus\":[]}]";
    FILE *f = _wfopen(json.s, L"wb");
    T(f != 0);
    if (!f) return;
    fwrite(list, 1, strlen(list), f);
    fclose(f);
    // gamedb_convert keeps only a list of over 1000 (anything less isn't the real one), so read its .part
    gamedb_convert(json, tsv);
    Path part = tsv;
    wcsncat(part.s, L".part", MAX_PATH - wcslen(part.s) - 1);
    T(!file_exists(tsv.s) && !file_exists(part.s));
    // So write the four lines the way it does, by converting with the limit out of the way: load what it wrote
    f = _wfopen(tsv.s, L"wb");
    if (f) {
        fprintf(f, "1\tMinecraft\taa\t\t0\tMinecraft Launcher\t>javaw.exe\x1fnet.minecraft.client.main.Main\n");
        fprintf(f, "2\tCounter-Strike 2\tbb\tcc\t730\t\twin64/cs2.exe\n");
        fprintf(f, "3\tRust\t\t\t252490\tRust\t!rust.exe|rustclient.exe\n");
        fprintf(f, "4\tGarry's Mod\t\t\t0\t\tgarrysmod/hl2.exe\x1f-game garrysmod|>hl2.exe\x1f-game garrysmod\n");
        fclose(f);
    }
    InitializeSRWLock(&g_gamedb.lock);
    g_gamedb.index = gamedb_load(tsv);
    T(g_gamedb.index && g_gamedb.index->row_count == 4);
    GameInfo g;
    bool shared = false;
    // A name, whatever its case and punctuation, and another name of the same game
    T(gamedb_by_name("counter strike 2", &g) && !strcmp(g.id, "2") && g.steam == 730 && !strcmp(g.cover, "cc"));
    T(gamedb_by_name("Minecraft Launcher", &g) && !strcmp(g.id, "1"));
    T(!gamedb_by_name("Counter-Strike", &g));
    // A search from the list, as typed: the same name first, then a start, then a word's start, then inside
    {
        char found[4][128];
        T(gamedb_find_local("minecraft", found, 4) == 1 && !strcmp(found[0], "Minecraft"));
        T(gamedb_find_local("Mine", found, 4) == 1 && !strcmp(found[0], "Minecraft"));
        T(gamedb_find_local("strike", found, 4) == 1 && !strcmp(found[0], "Counter-Strike 2"));
        T(gamedb_find_local("u", found, 4) == 2 && !strcmp(found[0], "Rust") && !strcmp(found[1], "Counter-Strike 2"));
        T(gamedb_find_local("zzz", found, 4) == 0 && gamedb_find_local("", found, 4) == 0);
        T(gamedb_find_local("u", found, 1) == 1 && !strcmp(found[0], "Rust"));
    }
    // An exe: with no path the file name is enough; with one, the folders must be right
    T(gamedb_by_exe("cs2.exe", "", "", "", &g) && !strcmp(g.name, "Counter-Strike 2"));
    T(gamedb_by_exe("CS2.exe", "E:\\Steam\\game\\bin\\win64\\cs2.exe", "", "", &g));
    T(!gamedb_by_exe("cs2.exe", "E:\\Other\\cs2.exe", "", "", &g));
    T(gamedb_by_exe("rust.exe", "", "", "", &g) && g.launcher);
    T(gamedb_by_exe("rustclient.exe", "", "", "", &g) && !g.launcher);
    T(!gamedb_by_exe("notepad.exe", "", "", "", &g, &shared) && !shared);
    // A shared exe: not without its command line, and then only the game whose command line it is
    T(!gamedb_by_exe("javaw.exe", "C:\\java\\bin\\javaw.exe", "", "", &g, &shared) && shared);
    T(gamedb_by_exe("javaw.exe", "C:\\java\\bin\\javaw.exe", "javaw -cp x net.minecraft.client.main.Main --demo", "", &g) && !strcmp(g.id, "1"));
    T(!gamedb_by_exe("javaw.exe", "C:\\java\\bin\\javaw.exe", "javaw -jar other.jar", "Some Tool", &g));
    // Its window naming the game is a fair guess when the command line doesn't say
    T(gamedb_by_exe("javaw.exe", "C:\\java\\bin\\javaw.exe", "javaw org.launcher.EntryPoint", "Minecraft 1.7.10", &g) && !strcmp(g.id, "1"));
    T(gamedb_by_exe("hl2.exe", "D:\\steamapps\\common\\GarrysMod\\hl2.exe", "hl2.exe -game garrysmod", "", &g) && !strcmp(g.id, "4"));
    // A game the list doesn't have still gets an id for its art, the same for the same letters
    GameInfo h;
    T(gamedb_for_art("Hollow Knight", &g) && g.id[0] == 'n' && !strcmp(g.name, "Hollow Knight"));
    T(gamedb_for_art("hollow-knight", &h) && !strcmp(g.id, h.id));
    T(gamedb_for_art("Counter-Strike 2", &g) && !strcmp(g.id, "2"));

    // The art service: signed as clipback-api checks it (this one worked out with node's crypto)
    char sig[128], enc[64];
    art_service_sign("/v1/search?q=Hollow%20Knight", 1763500000, sig, sizeof sig);
    T(!strcmp(sig, "1763500000.0e4bec4a258ac83ea7eba9a7894e6108f90447c9686e2c93ba2305e3e0d1efef"));
    url_encode("Garry's Mod: 2 & co~", enc, sizeof enc);
    T(!strcmp(enc, "Garry%27s%20Mod%3A%202%20%26%20co~"));
    // Its search: the game named so, else one whose name holds the name or is held in it, else none
    static const char search[] = "{\"games\":[{\"id\":\"s1\",\"name\":\"Hollow Knight: Silksong\",\"year\":2025},"
                                 "{\"id\":\"s2\",\"name\":\"Hollow Knight\",\"year\":2017},{\"id\":\"s3\",\"name\":\"X\"}]}";
    char id[24];
    T(art_pick_search(search, sizeof search - 1, "hollow knight", id, sizeof id) && !strcmp(id, "s2"));
    T(art_pick_search(search, sizeof search - 1, "Hollow", id, sizeof id) && !strcmp(id, "s1"));
    T(!art_pick_search(search, sizeof search - 1, "Celeste", id, sizeof id));
    T(!art_pick_search("{\"games\":[]}", 12, "Celeste", id, sizeof id));
    {
        char names[8][128];
        i32 years[8];
        const char games[] = "{\"games\":[{\"id\":\"s9\",\"name\":\"GregTech: New Horizons\",\"year\":2016},{\"id\":\"s8\",\"name\":\"Greg\",\"year\":null}]}";
        T(art_search_list(games, sizeof games - 1, names, years, 8) == 2);
        T(!strcmp(names[0], "GregTech: New Horizons") && years[0] == 2016 && !strcmp(names[1], "Greg") && years[1] == 0);
        T(art_search_list(games, sizeof games - 1, names, years, 1) == 1);
        T(art_search_list("{\"games\":[]}", 12, names, years, 8) == 0);
        // SteamGridDB's own answers, with the player's key, made into the service's
        const char sg[] = "{\"success\":true,\"data\":[{\"id\":5,\"name\":\"Say \\\"Hi\\\"\",\"release_date\":1500000000,\"types\":[\"steam\"]},{\"id\":6,\"name\":\"B\"}]}";
        static char conv[1024];
        T(sgdb_games(sg, sizeof sg - 1, conv, sizeof conv));
        T(!strcmp(conv, "{\"games\":[{\"id\":\"s5\",\"name\":\"Say \\\"Hi\\\"\",\"year\":2017},{\"id\":\"s6\",\"name\":\"B\",\"year\":0}]}"));
        T(art_search_list(conv, (i64)strlen(conv), names, years, 8) == 2 && !strcmp(names[0], "Say \"Hi\"") && years[0] == 2017);
        T(!sgdb_games("{\"success\":false}", 17, conv, sizeof conv));
        char pic[1100];
        const char grids[] = "{\"success\":true,\"data\":[{\"id\":1,\"url\":\"https://cdn2.steamgriddb.com/a.png\",\"thumb\":\"https://cdn2.steamgriddb.com/t.png\"},{\"url\":\"https://x\"}]}";
        sgdb_first(grids, sizeof grids - 1, pic, sizeof pic);
        T(!strcmp(pic, "{\"url\":\"https://cdn2.steamgriddb.com/a.png\",\"thumb\":\"https://cdn2.steamgriddb.com/t.png\"}"));
        sgdb_first("{\"success\":true,\"data\":[]}", 26, pic, sizeof pic);
        T(!strcmp(pic, "null"));
        sgdb_first(0, 0, pic, sizeof pic);
        T(!strcmp(pic, "null"));
        T(has_text("abc", 2, "ab") && !has_text("abc", 2, "bc"));
        // only a plain no is kept as "none": no answer, a busy or broken server, or a refusal is asked again later
        T(http_later(0) && http_later(429) && http_later(503) && http_later(403) && http_later(404));
        T(!http_later(200) && !http_later(400));
    }
    // Its art: the icon for an icon; the hero for a cover, else the grid; nothing that isn't https
    static const char art[] = "{\"id\":\"s2\",\"grid\":{\"url\":\"https://cdn2.steamgriddb.com/grid/g.png\",\"thumb\":\"t\",\"w\":920,\"h\":430},"
                              "\"hero\":null,\"logo\":{\"url\":\"https://x/l.png\"},\"icon\":{\"thumb\":\"http://x/i.png\"}}";
    char url[512];
    T(art_pick_url(art, sizeof art - 1, ART_COVER, url, sizeof url) && !strcmp(url, "https://cdn2.steamgriddb.com/grid/g.png"));
    T(!art_pick_url(art, sizeof art - 1, ART_ICON, url, sizeof url));
    static const char art2[] = "{\"grid\":{\"url\":\"https://a/g.png\"},\"hero\":{\"url\":\"https://a/h.png\",\"w\":1920},\"icon\":{\"url\":\"https://a/i.ico\",\"thumb\":\"https://a/i.png\"}}";
    T(art_pick_url(art2, sizeof art2 - 1, ART_COVER, url, sizeof url) && !strcmp(url, "https://a/h.png"));
    T(art_pick_url(art2, sizeof art2 - 1, ART_ICON, url, sizeof url) && !strcmp(url, "https://a/i.png"));
    gamedb_free(g_gamedb.index);
    g_gamedb.index = 0;
    DeleteFileW(json.s);
    DeleteFileW(tsv.s);
    RemoveDirectoryW(dir.s);
}

static void put_file(const Path &p, const char *text)
{
    FILE *f = _wfopen(p.s, L"wb");
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

// Minecraft's modpacks: what is read from a game's folder, and what goes to the art service
// Links (r2.cpp): request signing, names, the history file, limits and what cleanup removes
static void test_r2()
{
    // AWS's own example for Signature V4 (GET Object, S3 docs)
    SigHeader h[] = {{"host", "examplebucket.s3.amazonaws.com"},
                     {"range", "bytes=0-9"},
                     {"x-amz-content-sha256", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
                     {"x-amz-date", "20130524T000000Z"}};
    char auth[512];
    sigv4_auth("GET", "/test.txt", "", h, COUNT(h), h[2].value, "AKIAIOSFODNN7EXAMPLE",
               "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY", "20130524T000000Z", "us-east-1", "s3", auth, sizeof auth);
    T(strstr(auth, "Credential=AKIAIOSFODNN7EXAMPLE/20130524/us-east-1/s3/aws4_request") != 0);
    T(strstr(auth, "SignedHeaders=host;range;x-amz-content-sha256;x-amz-date") != 0);
    T(strstr(auth, "Signature=f0e8bdb87c964420e857bd35b5d6ed310bd44f0170aba48dd91039c6036bdb41") != 0);

    char enc[64];
    r2_uri_encode("a+b/c=d~e", enc, sizeof enc);
    T(!strcmp(enc, "a%2Bb%2Fc%3Dd~e"));
    char code[32];
    T(r2_xml("<Error><Code>NoSuchBucket</Code></Error>", "Code", code, sizeof code) && !strcmp(code, "NoSuchBucket"));

    // Names: clipback/<16 letters and digits>/clip.mp4, never the same twice
    char a[R2_KEY_MAX], b[R2_KEY_MAX];
    r2_new_key(a);
    r2_new_key(b);
    T(strlen(a) == 34 && starts_with(a, "clipback/") && !strcmp(a + 25, "/clip.mp4") && strcmp(a, b));
    bool plain = true;
    for (i32 i = 9; i < 25; i++) plain &= isalnum((u8)a[i]) != 0;
    T(plain);
    char url[128];
    r2_make_url("https://pub-x.r2.dev/", "k.mp4", 1, url, sizeof url);
    T(!strcmp(url, "https://pub-x.r2.dev/k.mp4"));
    r2_make_url("https://clips.example.com", "k.mp4", 3, url, sizeof url);
    T(!strcmp(url, "https://clips.example.com/k.mp4?v=3"));

    // Limits: free 10 GB, then $0.015 a GB-month, of which 80% is used
    T(r2_limit_for_budget(0) == 8 * R2_GB);
    T(r2_limit_for_budget(1.35) / R2_GB == 80 || r2_limit_for_budget(1.35) / R2_GB == 79);
    T(fabs(r2_cost_for(100 * R2_GB) - 1.35) < 1e-9 && r2_cost_for(5 * R2_GB) == 0);

    // Cleanup: oldest unpinned first, until it fits
    static R2Share s[4];
    for (i32 i = 0; i < 4; i++) {
        s[i] = {};
        s[i].bytes = 4 * R2_GB;
        s[i].uploaded_us = 100 - i; // the last is the oldest
    }
    s[3].pinned = true;
    i32 plan[4];
    T(r2_plan_cleanup(s, 4, 16 * R2_GB, 0, -1, plan, 4) == 0);
    i32 n = r2_plan_cleanup(s, 4, 10 * R2_GB, 0, 0, plan, 4); // 16 GB, keep 0, 3 pinned: the oldest of 1 and 2 goes
    T(n == 2 && plan[0] == 2 && plan[1] == 1);
    s[2].removed = true;
    n = r2_plan_cleanup(s, 4, 12 * R2_GB, 4 * R2_GB, -1, plan, 4); // 12 + 4 incoming: 1 goes, then 0
    T(n == 1 && plan[0] == 1);
    T(r2_plan_cleanup(s, 4, 6 * R2_GB, 4 * R2_GB, -1, plan, 4) == -1); // pinned 4 + incoming 4 > 6
    s[1].host = HOST_LITTERBOX; // a free host's clip isn't in the bucket
    T(r2_plan_cleanup(s, 4, 8 * R2_GB, 0, -1, plan, 4) == 0);

    // Free hosts: link_hosts' order, unknown and repeated names skipped; Litterbox's hours
    const FreeHost *order[HOST_COUNT];
    T(free_order("litterbox, catbox", order, HOST_COUNT) == 1 && order[0]->host == HOST_LITTERBOX);
    T(free_order("Catbox,nope,litterbox,catbox", order, HOST_COUNT) == 2 && order[0]->host == HOST_CATBOX &&
      order[1]->host == HOST_LITTERBOX);
    T(free_order("", order, HOST_COUNT) == 0 && free_max_bytes("") == 0);
    T(free_max_bytes("catbox") == 200000000 && free_max_bytes("catbox,litterbox") == 1000000000);
    T(free_hours(0) == 1 && free_hours(5) == 12 && free_hours(72) == 72 && free_hours(500) == 72);
    R2Share live = {};
    live.expires_us = 1000;
    T(r2_share_live(live, 999) && !r2_share_live(live, 1000));
    live.expires_us = 0;
    T(r2_share_live(live, 1ll << 60));
    live.paused = 1;
    T(!r2_share_live(live, 1));

    // The month's cap: reached at 90%, off at 0, never before the reads are known
    T(!r2_capped(8999999, 10000000) && r2_capped(9000000, 10000000) && r2_capped(20000000, 10000000));
    T(!r2_capped(20000000, 0) && !r2_capped(-1, 10000000));

    // Options' budget label: what clipback keeps for a step and the most it costs
    {
        char t[64];
        r2_budget_label(10, t, sizeof t);
        T(!strcmp(t, "Free: 8 GB"));
        r2_budget_label(25, t, sizeof t);
        T(!strcmp(t, "20 GB for at most $0.23 a month"));
        r2_budget_label(50, t, sizeof t);
        T(!strcmp(t, "40 GB for at most $0.60 a month"));
    }

    // A bucket that can't take a link says why; the link isn't passed to a free host
    T(!r2_refuses(true, false));
    T(r2_refuses(false, false) && strstr(r2_refuses(false, false), "keys"));
    T(r2_refuses(true, true) && strstr(r2_refuses(true, true), "until the 1st"));

    // Upload speed: the mean of the first 10, then each new one counts a tenth
    {
        f64 avg = 0;
        i32 cnt_n = 0;
        r2_rate_add(&avg, &cnt_n, 2e6);
        r2_rate_add(&avg, &cnt_n, 4e6);
        T(cnt_n == 2 && fabs(avg - 3e6) < 1);
        for (i32 i = 0; i < 20; i++) r2_rate_add(&avg, &cnt_n, 3e6);
        T(cnt_n == 10 && fabs(avg - 3e6) < 1);
        r2_rate_add(&avg, &cnt_n, 13e6);
        T(cnt_n == 10 && fabs(avg - 4e6) < 1);
        f64 rate[3] = {11.4e6, 2.1e6, 0};
        i32 cnt[3] = {3, 2, 0};
        char t[160];
        r2_rates_tip(rate, cnt, true, t, sizeof t);
        T(!strcmp(t, "Upload speed from your last uploads: Catbox 2.1 MB/s, your bucket 11.4 MB/s"));
        cnt[HOST_R2] = 0;
        r2_rates_tip(rate, cnt, true, t, sizeof t);
        T(!strcmp(t, "Upload speed from your last uploads: Catbox 2.1 MB/s, your bucket not measured yet"));
        cnt[HOST_CATBOX] = 0;
        r2_rates_tip(rate, cnt, false, t, sizeof t);
        T(!strcmp(t, "Upload speed from your last uploads: Catbox not measured yet"));
    }

    // The bucket's custom domains: only those enabled, owned and with a certificate
    {
        const char *resp = "{\"success\":true,\"result\":{\"domains\":["
                           "{\"domain\":\"clips.example.com\",\"enabled\":true,\"status\":{\"ownership\":\"active\",\"ssl\":\"active\"}},"
                           "{\"domain\":\"off.example.com\",\"enabled\":false,\"status\":{\"ownership\":\"active\",\"ssl\":\"active\"}},"
                           "{\"domain\":\"new.example.com\",\"enabled\":true,\"status\":{\"ownership\":\"pending\",\"ssl\":\"pending\"}},"
                           "{\"domain\":\"v.example.net\",\"enabled\":true,\"status\":{\"ownership\":\"active\",\"ssl\":\"active\"}}]}}";
        char got[8][128];
        T(r2_parse_custom_domains(resp, got, 8) == 2 && !strcmp(got[0], "clips.example.com") && !strcmp(got[1], "v.example.net"));
        T(r2_parse_custom_domains("{\"success\":true,\"result\":{\"domains\":[]}}", got, 8) == 0);
    }

    // Automatic pause: a link read too often in 24 hours, every link near the cap; not one resumed by hand today
    R2Share ap = {};
    ap.host = HOST_R2;
    ap.plays_24h = 50;
    T(r2_auto_action(ap, 50, false, 20261121) == 2 && r2_auto_action(ap, 51, false, 20261121) == 0);
    T(r2_auto_action(ap, 0, false, 20261121) == 0);
    T(r2_auto_action(ap, 0, true, 20261121) == 3 && r2_auto_action(ap, 50, true, 20261121) == 3);
    ap.pause_day = 20261121; // resumed by hand today
    T(r2_auto_action(ap, 50, true, 20261121) == 0 && r2_auto_action(ap, 50, false, 20261122) == 2);
    ap.paused = 2; // paused for its rate today: back tomorrow, unless the cap holds it
    T(r2_auto_action(ap, 50, false, 20261121) == 0 && r2_auto_action(ap, 50, false, 20261122) == -1);
    T(r2_auto_action(ap, 50, true, 20261122) == 0);
    ap.paused = 3; // paused for the cap: back when the month's reads are under it
    T(r2_auto_action(ap, 50, true, 20261122) == 0 && r2_auto_action(ap, 50, false, 20261121) == -1);
    ap.paused = 1;
    T(r2_auto_action(ap, 50, false, 20261122) == 0 && r2_auto_action(ap, 50, true, 20261122) == 0); // by hand: stays
    ap.paused = 0;
    ap.moving = true;
    T(r2_auto_action(ap, 1, true, 1) == 0);
    ap.moving = false;
    ap.host = HOST_CATBOX;
    T(r2_auto_action(ap, 1, true, 1) == 0);

    char rd[24];
    r2_fmt_reads(950, rd, sizeof rd);
    T(!strcmp(rd, "950"));
    r2_fmt_reads(12345, rd, sizeof rd);
    T(!strcmp(rd, "12 K"));
    r2_fmt_reads(1234567, rd, sizeof rd);
    T(!strcmp(rd, "1.2 M"));
    r2_fmt_reads(10000000, rd, sizeof rd);
    T(!strcmp(rd, "10 M"));

    // Custom uploaders (.sxcu): what's read, what's refused, and links made from answers
    const char *sx = "{\"Version\":\"16.0.1\",\"Name\":\"My host\",\"DestinationType\":\"ImageUploader, FileUploader\","
                     "\"RequestMethod\":\"POST\",\"RequestURL\":\"https://up.example.com/api\","
                     "\"Parameters\":{\"key\":\"a b\"},\"Headers\":{\"Authorization\":\"Bearer x\"},"
                     "\"Body\":\"MultipartFormData\",\"Arguments\":{\"name\":\"{filename}\",\"n\":5},"
                     "\"FileFormName\":\"file\",\"URL\":\"{json:files[1].url}\"}";
    static Uploader up;
    char why[256];
    T(uploader_parse(sx, (i64)strlen(sx), &up, why, sizeof why));
    T(!strcmp(up.name, "My host") && !strcmp(up.method, "POST") && !strcmp(up.form_name, "file"));
    T(!strcmp(up.url, "https://up.example.com/api?key=a%20b"));
    T(up.count[0] == 1 && !strcmp(up.pairs[0][0][0], "Authorization") && !strcmp(up.pairs[0][0][1], "Bearer x"));
    T(up.count[1] == 1 && !strcmp(up.pairs[1][0][1], "clip.mp4")); // the number isn't a string: left out
    const char *bin = "{\"RequestMethod\":\"PUT\",\"RequestURL\":\"https://x.example/{filename}\",\"Body\":\"Binary\"}";
    T(uploader_parse(bin, (i64)strlen(bin), &up, why, sizeof why) && !up.form_name[0] && !strcmp(up.method, "PUT"));
    const char *insecure = "{\"RequestURL\":\"http://x.example/\",\"FileFormName\":\"f\"}";
    T(!uploader_parse(insecure, (i64)strlen(insecure), &up, why, sizeof why) && strstr(why, "https"));
    const char *rx = "{\"RequestURL\":\"https://x.example/\",\"FileFormName\":\"f\",\"URL\":\"{regex:(.*)}\"}";
    T(!uploader_parse(rx, (i64)strlen(rx), &up, why, sizeof why));
    const char *js = "{\"Body\":\"JSON\",\"RequestURL\":\"https://x.example/\"}";
    T(!uploader_parse(js, (i64)strlen(js), &up, why, sizeof why));

    char link[256];
    const char *ans = "{\"ok\":true,\"files\":[{\"url\":\"https://a/1\"},{\"url\":\"https://a/2.mp4\",\"n\":[1,2]}]}";
    T(uploader_result("{json:files[1].url}", ans, link, sizeof link) && !strcmp(link, "https://a/2.mp4"));
    T(uploader_result("$json:files[0].url$?x", ans, link, sizeof link) && !strcmp(link, "https://a/1?x"));
    T(!uploader_result("{json:files[2].url}", ans, link, sizeof link));
    T(uploader_result("", "  https://h/x.mp4\n", link, sizeof link) && !strcmp(link, "https://h/x.mp4"));
    T(uploader_result("https://h/{response}", "abc\n", link, sizeof link) && !strcmp(link, "https://h/abc"));
    T(json_path("[{\"link\":\"q\"}]", 15, "[0].link", link, sizeof link) && !strcmp(link, "q"));
    T(up_plain_link("https://files.catbox.moe/ab12cd.mp4", link, sizeof link) && !up_plain_link("<html>", link, sizeof link));

    // History file round trip, a tab in a title made a space
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    Path file = {};
    swprintf(file.s, MAX_PATH, L"%sclipback-tests-%lu-shares.tsv", temp, GetCurrentProcessId());
    static R2Share w[2], r[4];
    w[0] = {};
    str_set(w[0].key, "clipback/abcdefghijklmnop/clip.mp4");
    str_set(w[0].url, "https://x/abc.mp4?v=2");
    str_set(w[0].title, "ace\tround");
    str_set(w[0].game, "Valorant");
    str_set(w[0].path, "C:\\Clips\\ace round.mp4");
    w[0].bytes = 123456789012;
    w[0].uploaded_us = 1700000000000000;
    w[0].pinned = true;
    w[0].version = 2;
    w[0].lib_key = 18446744073709551615ull;
    w[1] = {};
    str_set(w[1].key, "def.mp4");
    w[1].removed = true;
    w[1].version = 1;
    w[1].host = HOST_LITTERBOX;
    w[1].expires_us = 1700000259200000;
    w[0].paused = 2;
    str_set(w[0].hidden, "clipback/qrstuvwxyzabcdef/clip.mp4");
    w[0].pause_day = 20261121;
    w[0].page_w = 1920;
    w[0].page_h = 1080;
    T(r2_history_write(file, w, 2));
    T(r2_history_read(file, r, 4) == 2);
    T(!strcmp(r[0].key, w[0].key) && !strcmp(r[0].url, w[0].url) && !strcmp(r[0].title, "ace round"));
    T(!strcmp(r[0].game, "Valorant") && !strcmp(r[0].path, w[0].path) && r[0].bytes == w[0].bytes);
    T(r[0].uploaded_us == w[0].uploaded_us && r[0].pinned && !r[0].removed && r[0].version == 2);
    T(r[0].lib_key == w[0].lib_key && r[1].removed && !r[1].pinned && !strcmp(r[1].key, "def.mp4"));
    T(r[0].host == HOST_R2 && !r[0].expires_us && r[1].host == HOST_LITTERBOX && r[1].expires_us == w[1].expires_us);
    T(r[0].paused == 2 && !strcmp(r[0].hidden, w[0].hidden) && r[0].pause_day == 20261121 && !r[1].paused);
    T(r[0].plays_today == -1 && r[0].plays_month == -1 && r[0].plays_24h == -1);
    T(r[0].page_w == 1920 && r[0].page_h == 1080 && !r[1].page_w && !r[1].page_h);
    T(!strcmp(r2_object_key(r[0]), w[0].hidden) && !strcmp(r2_object_key(r[1]), "def.mp4"));
    DeleteFileW(file.s);

    // Preview pages: the page beside the clip's folder, the picture in it, at most 1280 wide and even
    char pkey[R2_KEY_MAX];
    r2_page_key("clipback/abcdefghijklmnop/clip.mp4", pkey);
    T(!strcmp(pkey, "clipback/abcdefghijklmnop"));
    r2_thumb_key("clipback/qrstuvwxyzabcdef/clip.mp4", pkey);
    T(!strcmp(pkey, "clipback/qrstuvwxyzabcdef/thumb.jpg"));
    i32 tw, th;
    r2_thumb_size(1920, 1080, &tw, &th);
    T(tw == 1280 && th == 720);
    r2_thumb_size(2560, 1440, &tw, &th);
    T(tw == 1280 && th == 720);
    r2_thumb_size(853, 480, &tw, &th);
    T(tw == 852 && th == 480);
    r2_thumb_size(0, 1080, &tw, &th);
    T(!tw && !th);

    // The page: the tags for a card that plays, the clip full width, the word under it; the title made safe and
    // ASCII; paused, no clip and nothing that plays it
    static char html[8192];
    static const u8 past_ascii[] = {'c', 'a', 'f', 0xc3, 0xa9, ' ', 0xf0, 0x9f, 0x8e, 0xae, 0x01, 0}; // e-acute, a pad, a control
    char title[96];
    snprintf(title, sizeof title, "ace <b>\"round\"</b> & %s", (const char *)past_ascii);
    i32 hn = r2_page_html(title, "https://x.dev/clipback/abc?v=2",
                          "https://x.dev/clipback/abc/clip.mp4?v=2", "https://x.dev/clipback/abc/thumb.jpg?v=2", 1920,
                          1080, 1280, 720, false, html, sizeof html);
    T(hn > 0 && hn == (i32)strlen(html));
    bool ascii = true;
    for (i32 i = 0; i < hn; i++) ascii = ascii && (u8)html[i] < 0x80;
    T(ascii);
    T(strstr(html, "<title>ace &lt;b&gt;&quot;round&quot;&lt;/b&gt; &amp; caf&#233; &#127918;</title>") != 0);
    T(strstr(html, "<meta property=\"og:type\" content=\"video.other\">") != 0);
    T(strstr(html, "<meta property=\"og:site_name\" content=\"clipback\">") != 0);
    T(strstr(html, "<meta property=\"og:image\" content=\"https://x.dev/clipback/abc/thumb.jpg?v=2\">") != 0);
    T(strstr(html, "og:image:width\" content=\"1280\"") != 0 && strstr(html, "og:image:height\" content=\"720\"") != 0);
    T(strstr(html, "<meta property=\"og:video:secure_url\" content=\"https://x.dev/clipback/abc/clip.mp4?v=2\">") != 0);
    T(strstr(html, "og:video:type\" content=\"video/mp4\"") != 0 && strstr(html, "og:video:width\" content=\"1920\"") != 0);
    T(strstr(html, "<meta name=\"twitter:card\" content=\"player\">") != 0);
    T(strstr(html, "<meta name=\"twitter:player\" content=\"https://x.dev/clipback/abc?v=2\">") != 0);
    T(strstr(html, "twitter:player:height\" content=\"1080\"") != 0 && strstr(html, "twitter:image\"") != 0);
    T(strstr(html, "<video src=\"https://x.dev/clipback/abc/clip.mp4?v=2\" poster=\"https://x.dev/clipback/abc/thumb.jpg?v=2\"") != 0);
    T(strstr(html, " controls playsinline ") != 0 && strstr(html, "<p>clipback</p>") != 0);
    hn = r2_page_html("ace", "https://x.dev/clipback/abc", "https://x.dev/clipback/abc/clip.mp4",
                      "https://x.dev/clipback/abc/thumb.jpg", 1920, 1080, 1280, 720, true, html, sizeof html);
    T(hn > 0 && strstr(html, "<title>ace</title>") && strstr(html, "<p>clipback</p>"));
    T(!strstr(html, "<video") && !strstr(html, "og:video") && !strstr(html, "clip.mp4") && !strstr(html, "twitter:player"));
    T(!strstr(html, "thumb.jpg"));
    T(r2_page_html("ace", "a", "b", "c", 1, 1, 1, 1, false, html, 64) == 0);

    // The bucket's S3 secret from an API token
    char secret[65];
    r2_token_secret("abc", secret);
    T(!strcmp(secret, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));

    // Cloudflare's answers
    char id[72], msg[128];
    T(r2_parse_verify("{\"result\":{\"id\":\"0123456789abcdef0123456789abcd01\",\"status\":\"active\"},\"success\":true,"
                      "\"errors\":[],\"messages\":[]}",
                      id, sizeof id));
    T(!strcmp(id, "0123456789abcdef0123456789abcd01"));
    T(!r2_parse_verify("{\"result\":{\"id\":\"f267\",\"status\":\"disabled\"}}", id, sizeof id));
    T(r2_cf_error("{\"success\":false,\"errors\":[{\"code\":10006,\"message\":\"The specified bucket does not exist.\"}]}",
                  msg, sizeof msg) == CF_NO_SUCH_BUCKET);
    T(!strcmp(msg, "The specified bucket does not exist."));
    T(r2_cf_error("{\"success\":true,\"errors\":[],\"result\":{}}", msg, sizeof msg) == 0 && !msg[0]);
    char ids[4][40], names[4][128];
    T(r2_parse_accounts("{\"result\":[{\"id\":\"a1\",\"name\":\"Main\",\"settings\":{\"x\":1}},{\"id\":\"b2\",\"name\":\"Other\"}],"
                        "\"success\":true}",
                        ids, names, 4) == 2);
    T(!strcmp(ids[0], "a1") && !strcmp(names[0], "Main") && !strcmp(ids[1], "b2") && !strcmp(names[1], "Other"));
    T(r2_parse_accounts("{\"result\":[],\"success\":true}", ids, names, 4) == 0);
    char domain[128];
    bool on = true;
    T(r2_parse_domain("{\"result\":{\"bucketId\":\"x\",\"domain\":\"pub-0123abcd.r2.dev\",\"enabled\":false}}", domain,
                      sizeof domain, &on));
    T(!strcmp(domain, "pub-0123abcd.r2.dev") && !on);
    T(r2_parse_domain("{\"result\":{\"domain\":\"pub-0123abcd.r2.dev\",\"enabled\":true}}", domain, sizeof domain, &on) && on);
    char value[128];
    T(r2_parse_new_token("{\"result\":{\"id\":\"0123456789abcdef0123456789abcd02\",\"name\":\"clipback\","
                         "\"value\":\"test-token-value-00000000000000000000000\"},\"success\":true}",
                         id, sizeof id, value, sizeof value));
    T(!strcmp(id, "0123456789abcdef0123456789abcd02") && !strcmp(value, "test-token-value-00000000000000000000000"));
    char group[40];
    T(r2_parse_perm_group("{\"result\":[{\"id\":\"6a01\",\"name\":\"Workers R2 Storage Bucket Item Read\"},"
                          "{\"id\":\"2efd\",\"name\":\"Workers R2 Storage Bucket Item Write\",\"scopes\":[\"a\"]}]}",
                          CF_PERM_BUCKET_ITEMS_NAME, group, sizeof group));
    T(!strcmp(group, "2efd"));
    T(!r2_parse_perm_group("{\"result\":[{\"id\":\"6a01\",\"name\":\"Other\"}]}", CF_PERM_BUCKET_ITEMS_NAME, group,
                           sizeof group));

    // Usage: the latest storage count, and requests by class (DeleteObject is free)
    R2Fetched f;
    T(r2_parse_usage("{\"data\":{\"viewer\":{\"accounts\":[{\"storage\":[{\"max\":{\"payloadSize\":1000,\"metadataSize\":24}}],"
                     "\"ops\":[{\"sum\":{\"requests\":7},\"dimensions\":{\"actionType\":\"PutObject\"}},"
                     "{\"sum\":{\"requests\":3},\"dimensions\":{\"actionType\":\"ListObjects\"}},"
                     "{\"sum\":{\"requests\":40},\"dimensions\":{\"actionType\":\"GetObject\"}},"
                     "{\"sum\":{\"requests\":5},\"dimensions\":{\"actionType\":\"DeleteObject\"}}]}]}},\"errors\":null}",
                     &f));
    T(f.bytes == 1024 && f.class_a == 10 && f.class_b == 40);
    T(r2_parse_usage("{\"data\":{\"viewer\":{\"accounts\":[{\"storage\":[],\"ops\":[]}]}},\"errors\":null}", &f));
    T(f.bytes == 0 && f.class_a == 0 && f.class_b == 0);
    T(!r2_parse_usage("{\"data\":null,\"errors\":[{\"message\":\"not authorized\"}]}", &f));
    // The account's reads: Class B requests to every bucket
    T(r2_parse_usage("{\"data\":{\"viewer\":{\"accounts\":[{\"storage\":[],\"ops\":[],\"account\":["
                     "{\"sum\":{\"requests\":900},\"dimensions\":{\"actionType\":\"GetObject\"}},"
                     "{\"sum\":{\"requests\":100},\"dimensions\":{\"actionType\":\"HeadObject\"}},"
                     "{\"sum\":{\"requests\":7},\"dimensions\":{\"actionType\":\"PutObject\"}}]}]}}}",
                     &f));
    T(f.account_reads == 1000 && f.class_b == 0);
    T(r2_op_class("UploadPart") == 'A' && r2_op_class("HeadObject") == 'B' && r2_op_class("DeleteObject") == 0);
    char q[3200];
    r2_usage_query("acc1", "clipback", "2026-11-01T00:00:00Z", "2026-11-21T10:00:00Z", "2026-11-20T23:00:00Z",
                   "2026-11-20T10:00:00Z", q, sizeof q);
    T(strstr(q, "objectName") && strstr(q, "\"get\":\"GetObject\"") && strstr(q, "\"dayStart\":\"2026-11-20T23:00:00Z\""));
    T(strstr(q, "\"since\":\"2026-11-20T10:00:00Z\"") && strstr(q, "day:r2Op") &&
      strstr(q, "account:r2OperationsAdaptiveGroups(limit:1000,filter:{datetime_geq:$startDate,datetime_leq:$endDate})"));
    T(strstr(q, "\"accountTag\":\"acc1\"") && strstr(q, "\"bucketName\":\"clipback\"") &&
      strstr(q, "r2OperationsAdaptiveGroups") && strstr(q, "\"startDate\":\"2026-11-01T00:00:00Z\""));

    // Plays per file
    const char *pl = "{\"data\":{\"viewer\":{\"accounts\":[{\"month\":[{\"sum\":{\"requests\":12},\"dimensions\":"
                     "{\"objectName\":\"clipback/a/x.mp4\"}},{\"sum\":{\"requests\":3},\"dimensions\":{\"objectName\":"
                     "\"b.mp4\"}}],\"today\":[{\"sum\":{\"requests\":4},\"dimensions\":{\"objectName\":\"b.mp4\"}}]}]}}}";
    static char pk[4][R2_KEY_MAX];
    i64 pc[4];
    T(r2_parse_plays(pl, "month", pk, pc, 4) == 2 && !strcmp(pk[0], "clipback/a/x.mp4") && pc[0] == 12 && pc[1] == 3);
    T(r2_parse_plays(pl, "today", pk, pc, 4) == 1 && !strcmp(pk[0], "b.mp4") && pc[0] == 4);
    T(r2_parse_plays(pl, "month", pk, pc, 1) == 1);

    // A month's cost past the free allowances
    T(r2_month_cost(5 * R2_GB, 100, 100) == 0);
    T(fabs(r2_month_cost(20 * R2_GB, 3000000, 20000000) - (0.15 + 9.0 + 3.6)) < 1e-9);

    T(r2_bucket_name_ok("clipback") && r2_bucket_name_ok("my-clips-2"));
    T(!r2_bucket_name_ok("ab") && !r2_bucket_name_ok("-clips") && !r2_bucket_name_ok("Clips") && !r2_bucket_name_ok("a_b"));
    T(strstr(r2_token_link(), "permissionGroupKeys=%5B%7B%22key%22%3A%22bf7481a1826f439697cb59a20b22293e%22") != 0);

    // Sign-in: PKCE (RFC 7636's example), the browser's callback, and the token answer
    char challenge[44];
    r2_pkce_challenge("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk", challenge);
    T(!strcmp(challenge, "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM"));
    char cb_code[256], state[64];
    T(r2_callback_parse("GET /oauth/callback?code=ab%2Fc+d&scope=x&state=s123 HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n", cb_code,
                        sizeof cb_code, state, sizeof state) == R2_CB_CODE);
    T(!strcmp(cb_code, "ab/c d") && !strcmp(state, "s123"));
    T(r2_callback_parse("GET /oauth/callback?error=access_denied&error_description=The+user+said+no&state=s1 HTTP/1.1",
                        cb_code, sizeof cb_code, state, sizeof state) == R2_CB_REFUSED);
    T(!strcmp(cb_code, "The user said no") && !strcmp(state, "s1"));
    T(r2_callback_parse("GET /favicon.ico HTTP/1.1", cb_code, sizeof cb_code, state, sizeof state) == R2_CB_OTHER);
    T(r2_callback_parse("GET /oauth/callback?state=s1 HTTP/1.1", cb_code, sizeof cb_code, state, sizeof state) == R2_CB_OTHER);
    static R2OAuth o;
    T(r2_parse_oauth("{\"access_token\":\"at1\",\"expires_in\":3599,\"refresh_token\":\"rt1\",\"scope\":\"account.read\","
                     "\"token_type\":\"bearer\"}",
                     &o));
    T(!strcmp(o.access, "at1") && !strcmp(o.refresh, "rt1") && o.expires_in == 3599 && !o.error[0]);
    T(!r2_parse_oauth("{\"error\":\"invalid_grant\",\"error_description\":\"expired\"}", &o));
    T(!strcmp(o.error, "invalid_grant"));
}

static void test_mcpack()
{
    char v[256];
    T(mc_arg("java -cp x --gameDir \"C:/My Packs/inst/.minecraft\" --version 1.20.1", "--gameDir", v, sizeof v) &&
      !strcmp(v, "C:/My Packs/inst/.minecraft"));
    T(mc_arg("java --gameDirX no --gameDir D:/mc --width 854", "--gameDir", v, sizeof v) && !strcmp(v, "D:/mc"));
    T(!mc_arg("java --version 1.20.1", "--gameDir", v, sizeof v));

    char m[48];
    mc_mod_name("jei-1.20.1-forge-15.2.0.27.jar", m, sizeof m);
    T(!strcmp(m, "jei"));
    mc_mod_name("[1.12.2]Botania r1.10.jar", m, sizeof m);
    T(!strcmp(m, "botania"));
    mc_mod_name("sodium-fabric-mc1.20.1-0.5.3.jar", m, sizeof m);
    T(!strcmp(m, "sodium-fabric"));
    mc_mod_name("fabric-api-0.92.0+1.20.1.jar", m, sizeof m);
    T(!strcmp(m, "fabric-api"));
    mc_mod_name("Xaeros_Minimap_23.9.7_Forge_1.20.jar", m, sizeof m);
    T(!strcmp(m, "xaeros_minimap"));

    T(ini_get("a=1\r\nManagedPackID=abc\r\nb=2", "ManagedPackID", v, sizeof v) && !strcmp(v, "abc"));
    T(!ini_get("XManagedPackID=abc\n", "ManagedPackID", v, sizeof v));
    static const char nested[] = "{\"a\":{\"b\":[1,2],\"c\":{\"d\":42}},\"e\":\"x\"}";
    T(j_get(nested, sizeof nested - 1, "a.c.d", v, sizeof v) && !strcmp(v, "42"));
    T(j_get(nested, sizeof nested - 1, "e", v, sizeof v) && !strcmp(v, "x"));
    T(!j_get(nested, sizeof nested - 1, "a.b", v, sizeof v) && !j_get(nested, sizeof nested - 1, "a.z", v, sizeof v));

    // Each launcher's record, in folders made here
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    Path root = {};
    swprintf(root.s, MAX_PATH, L"%sclipback-mcpack-%lu", tmp, GetCurrentProcessId());
    CreateDirectoryW(root.s, 0);
    Path prism = path_join(root, L"prism"), pgame = path_join(prism, L".minecraft"), pmods = path_join(pgame, L"mods");
    CreateDirectoryW(prism.s, 0);
    CreateDirectoryW(pgame.s, 0);
    CreateDirectoryW(pmods.s, 0);
    put_file(path_join(prism, L"instance.cfg"), "name=My Own Pack Name\r\nManagedPack=true\r\nManagedPackType=modrinth\r\n"
                                                "ManagedPackID=1KVo5zza\r\nManagedPackVersionID=vXyZ\r\n");
    put_file(path_join(pmods, L"jei-1.20.1-forge-15.2.0.27.jar"), "");
    put_file(path_join(pmods, L"create-1.20.1-0.5.1.f.jar"), "");
    put_file(path_join(pmods, L"notes.txt"), "");
    char dir[MAX_PATH * 2];
    wide_to_utf8(pgame.s, dir, sizeof dir);
    static McClues c;
    T(mc_read_record(dir, &c) && !strcmp(c.launcher, "prism") && !strcmp(c.platform, "modrinth") &&
      !strcmp(c.project, "1KVo5zza") && !strcmp(c.version, "vXyZ") && c.mod_count == 2);

    Path cf = path_join(root, L"cf");
    CreateDirectoryW(cf.s, 0);
    put_file(path_join(cf, L"minecraftinstance.json"), "{\"name\":\"x\",\"projectID\":285109,\"fileID\":4567}");
    wide_to_utf8(cf.s, dir, sizeof dir);
    T(mc_read_record(dir, &c) && !strcmp(c.launcher, "curseforge") && !strcmp(c.platform, "curseforge") &&
      !strcmp(c.project, "285109") && !strcmp(c.version, "4567"));
    put_file(path_join(cf, L"minecraftinstance.json"), "{\"projectID\":0,\"fileID\":0}"); // imported from a zip
    T(mc_read_record(dir, &c) && !strcmp(c.launcher, "curseforge") && !c.platform[0] && !c.project[0]);

    Path ftb = path_join(root, L"ftb");
    CreateDirectoryW(ftb.s, 0);
    put_file(path_join(ftb, L"instance.json"), "{\"id\":79,\"versionId\":12345,\"name\":\"x\"}");
    wide_to_utf8(ftb.s, dir, sizeof dir);
    T(mc_read_record(dir, &c) && !strcmp(c.launcher, "ftb") && !strcmp(c.project, "79") && !strcmp(c.version, "12345"));
    T(!mc_read_record("Z:/no/such/folder", &c));

    // The clues: launcher, versions, loader and mods; never the path or the instance's name
    char cmd[1024], body[4096];
    wide_to_utf8(pgame.s, dir, sizeof dir);
    snprintf(cmd, sizeof cmd, "javaw -cp net/minecraftforge/x.jar cpw.mods.bootstraplauncher.BootstrapLauncher "
                              "--gameDir \"%s\" --version 1.20.1-forge-47.2.0", dir);
    T(mc_clues(cmd, body, sizeof body));
    T(!strcmp(body, "{\"mc\":{\"launcher\":\"prism\",\"mc\":\"1.20.1\",\"loader\":\"forge\",\"pack\":{\"platform\":\"modrinth\","
                    "\"project\":\"1KVo5zza\",\"version\":\"vXyZ\"},\"mods\":[\"create\",\"jei\"]}}") ||
      !strcmp(body, "{\"mc\":{\"launcher\":\"prism\",\"mc\":\"1.20.1\",\"loader\":\"forge\",\"pack\":{\"platform\":\"modrinth\","
                    "\"project\":\"1KVo5zza\",\"version\":\"vXyZ\"},\"mods\":[\"jei\",\"create\"]}}"));
    T(!strstr(body, "My Own") && !strstr(body, "clipback-mcpack"));
    T(!mc_clues("javaw -jar notagame.jar --gameDir C:/x", body, sizeof body));

    // The service's answer: a pack its launcher named is that pack, with its art; plain Minecraft is not a pack
    static const char answer[] = "{\"game\":{\"id\":\"s5\",\"name\":\"Minecraft\"},\"pack\":{\"platform\":\"modrinth\",\"project\":\"1KVo5zza\","
                                 "\"name\":\"Fabulously Optimized\",\"version\":\"5.0\"},\"matched_by\":\"launcher_record\",\"confidence\":0.95,"
                                 "\"art\":{\"grid\":{\"url\":\"https://a/g.png\"},\"icon\":null}}";
    const char *art;
    i64 art_len;
    char name[128];
    T(pack_from_answer(answer, sizeof answer - 1, name, sizeof name, &art, &art_len) && !strcmp(name, "Fabulously Optimized") &&
      art && art_len == 46 && !strncmp(art, "{\"grid\"", 7) && art[art_len - 1] == '}');
    static const char plain[] = "{\"game\":{\"id\":\"s5\",\"name\":\"Minecraft\"},\"matched_by\":\"store_id\",\"art\":{}}";
    T(!pack_from_answer(plain, sizeof plain - 1, name, sizeof name, &art, &art_len) && !name[0]);

    // A POST is signed with its body (worked out with Python's hmac)
    char sig[128];
    art_service_sign("/v1/resolve", 1763500000, sig, sizeof sig, "{\"mc\":{\"launcher\":\"prism\"}}");
    T(!strcmp(sig, "1763500000.2aa819f4a85e9589f05f8bc1abf247b9027dc889fa57f073bb1926bb86d4905e"));

    const wchar_t *files[] = {L"prism/.minecraft/mods/jei-1.20.1-forge-15.2.0.27.jar", L"prism/.minecraft/mods/create-1.20.1-0.5.1.f.jar",
                              L"prism/.minecraft/mods/notes.txt", L"prism/instance.cfg", L"cf/minecraftinstance.json", L"ftb/instance.json"};
    for (auto f : files) DeleteFileW(path_join(root, f).s);
    const wchar_t *dirs[] = {L"prism/.minecraft/mods", L"prism/.minecraft", L"prism", L"cf", L"ftb"};
    for (auto d : dirs) RemoveDirectoryW(path_join(root, d).s);
    RemoveDirectoryW(root.s);
}

static void make_dirs(const char *utf8)
{
    Path p = {};
    utf8_to_wide(utf8, p.s, MAX_PATH);
    for (wchar_t *c = p.s + 3; *c; c++)
        if (*c == L'/') {
            *c = 0;
            CreateDirectoryW(p.s, 0);
            *c = L'/';
        }
    CreateDirectoryW(p.s, 0);
}

static void put_text(const char *utf8, const char *text)
{
    Path p = {};
    utf8_to_wide(utf8, p.s, MAX_PATH);
    put_file(p, text);
}

// Markers: what follows `mark`, their line in markers.tsv, and the list kept in time order
static void test_markers()
{
    u8 colour;
    char note[256];
    marker_args("", &colour, note);
    T(colour == 0 && !note[0]);
    marker_args("nice kill", &colour, note);
    T(colour == 0 && !strcmp(note, "nice kill"));
    marker_args("Blue  boss fight\tphase 2 ", &colour, note);
    T(colour == 5 && !strcmp(note, "boss fight phase 2"));
    marker_args("red", &colour, note);
    T(colour == 1 && !note[0]);
    marker_args("reddish thing", &colour, note); // a word that only starts like a colour is part of the note
    T(colour == 0 && !strcmp(note, "reddish thing"));

    Marker m = {1763500000000000, 4, "clutch"}, back;
    char line[512];
    T(marker_line(m, line, sizeof line) > 0 && !strcmp(line, "1763500000000000\tgreen\tclutch\n"));
    T(marker_from_line(line, &back) && back.t_us == m.t_us && back.colour == 4 && !strcmp(back.note, "clutch"));
    char plain[] = "1763500000000001\t\t\r\n", bad[] = "not a marker";
    T(marker_from_line(plain, &back) && back.colour == 0 && !back.note[0]);
    T(!marker_from_line(bad, &back));

    static Markers ms;
    ms = {};
    Marker a = {300}, b = {100}, c = {200};
    T(markers_insert(&ms, a) == 300 && markers_insert(&ms, b) == 100 && markers_insert(&ms, c) == 200);
    T(ms.count == 3 && ms.items[0].t_us == 100 && ms.items[1].t_us == 200 && ms.items[2].t_us == 300);
    T(markers_insert(&ms, c) == 201 && ms.items[2].t_us == 201); // two at one moment: the second is moved on
    Marker got[8];
    T(markers_range(&ms, 150, 250, got, 8) == 2 && got[0].t_us == 200 && got[1].t_us == 201);
    T(markers_range(&ms, 0, 1000, got, 2) == 2);
    for (i32 i = 0; i < MARKER_MAX; i++) { Marker x = {1000 + i}; markers_insert(&ms, x); }
    T(ms.count == MARKER_MAX && ms.items[0].t_us == 1000 && ms.items[MARKER_MAX - 1].t_us == 1000 + MARKER_MAX - 1); // the oldest went
}

// Chapters: wall clock to file time across a gap, sorted, titles escaped
static void test_chapters()
{
    Piece pieces[2] = {{1000000, 5000000, 0}, {9000000, 12000000, 4000000}}; // a 4 s gap; 7 s in all
    Chapter ch[5] = {{10000000, "second; part=2"}, {2000000, "Marker"}, {6000000, "in the gap"},
                     {2000000, "same ms"}, {1000000, "a\nb"}};
    char out[1024];
    T(chapters_text(pieces, 2, 7000000, ch, 5, "Game", out, sizeof out) == 3);
    T(!strcmp(out, ";FFMETADATA1\n"
                   "[CHAPTER]\nTIMEBASE=1/1000\nSTART=0\nEND=1000\ntitle=a b\n"
                   "[CHAPTER]\nTIMEBASE=1/1000\nSTART=1000\nEND=5000\ntitle=Marker\n"
                   "[CHAPTER]\nTIMEBASE=1/1000\nSTART=5000\nEND=7000\ntitle=second\\; part\\=2\n"));
    T(chapters_text(pieces, 2, 7000000, ch, 5, "Game", out, 40) == 0); // doesn't fit: none
    Chapter late[1] = {{3000000, "Late"}}; // the first after the start: a lead chapter from 0
    T(chapters_text(pieces, 2, 7000000, late, 1, "Game", out, sizeof out) == 2);
    T(!strcmp(out, ";FFMETADATA1\n"
                   "[CHAPTER]\nTIMEBASE=1/1000\nSTART=0\nEND=2000\ntitle=Game\n"
                   "[CHAPTER]\nTIMEBASE=1/1000\nSTART=2000\nEND=7000\ntitle=Late\n"));
}

// Games the list doesn't know: their folder, the store id their launcher left, and what goes to the art service
static void test_storeids()
{
    char f[MAX_PATH];
    bool steam;
    T(game_folder("D:/Lib/steamapps/common/Satisfactory/FactoryGame/Binaries/Win64/FactoryGame-Win64-Shipping.exe", f, sizeof f, &steam) &&
      !strcmp(f, "Satisfactory") && steam);
    T(game_folder("C:/Games/Celeste/Celeste.exe", f, sizeof f, &steam) && !strcmp(f, "Celeste") && !steam);
    T(game_folder("C:/Games/Foo Bar/bin/x64/foo.exe", f, sizeof f, &steam) && !strcmp(f, "Foo Bar"));
    T(!game_folder("C:/foo.exe", f, sizeof f, &steam) && !game_folder("C:/bin/foo.exe", f, sizeof f, &steam));
    T(!game_folder("D:/Lib/steamapps/common/x.exe", f, sizeof f, &steam));
    // a folder that could name the player is never sent
    T(!game_folder("C:/Users/Sam/game.exe", f, sizeof f, &steam));
    T(!game_folder("C:/Users/Sam/Downloads/game.exe", f, sizeof f, &steam));
    T(!game_folder("C:/Users/Sam/OneDrive - Acme/Desktop/bin/game.exe", f, sizeof f, &steam));
    T(game_folder("C:/Users/Sam/AppData/Local/Programs/Foo/foo.exe", f, sizeof f, &steam) && !strcmp(f, "Foo"));
    {
        wchar_t me[128];
        char path[400], mine[384];
        if (GetEnvironmentVariableW(L"USERNAME", me, COUNT(me))) {
            wide_to_utf8(me, mine, sizeof mine);
            snprintf(path, sizeof path, "D:/%s/game.exe", mine);
            T(!game_folder(path, f, sizeof f, &steam));
        }
    }

    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    char root[MAX_PATH * 2], p[MAX_PATH * 3], body[2048];
    wide_to_utf8(tmp, root, sizeof root);
    for (char *c = root; *c; c++)
        if (*c == '\\') *c = '/';
    snprintf(root + strlen(root), sizeof root - strlen(root), "clipback-stores-%lu", GetCurrentProcessId());
    snprintf(p, sizeof p, "%s/Lib/steamapps/common/Hollow Knight", root);
    make_dirs(p);
    snprintf(p, sizeof p, "%s/Lib/steamapps/appmanifest_367520.acf", root);
    put_text(p, "\"AppState\"\n{\n\t\"appid\"\t\t\"367520\"\n\t\"name\"\t\t\"Hollow Knight\"\n\t\"installdir\"\t\t\"Hollow Knight\"\n}\n");
    snprintf(p, sizeof p, "%s/Lib/steamapps/appmanifest_10.acf", root);
    put_text(p, "\"AppState\"\n{\n\t\"installdir\"\t\t\"Hollow\"\n}\n");
    snprintf(p, sizeof p, "%s/Lib/steamapps/common/Hollow Knight/hollow_knight.exe", root);
    T(steam_app_for(p, "Hollow Knight") == 367520 && steam_app_for(p, "hollow knight") == 367520 && !steam_app_for(p, "Silksong"));
    T(exe_clues("hollow_knight.exe", p, "Hollow Knight", false, body, sizeof body) &&
      !strcmp(body, "{\"exe\":\"hollow_knight.exe\",\"folder\":\"Hollow Knight\",\"name\":\"Hollow Knight\",\"steam\":367520}"));
    T(!strstr(body, "clipback-stores"));

    snprintf(p, sizeof p, "%s/GOG/Witcher 3/bin/x64", root);
    make_dirs(p);
    snprintf(p, sizeof p, "%s/GOG/Witcher 3/goggame-1207664643.info", root);
    put_text(p, "{}");
    snprintf(p, sizeof p, "%s/GOG/Witcher 3/bin/x64/witcher3.exe", root);
    T(gog_game_for(p) == 1207664643ull);
    T(exe_clues("witcher3.exe", p, "", false, body, sizeof body) &&
      !strcmp(body, "{\"exe\":\"witcher3.exe\",\"folder\":\"Witcher 3\",\"gog\":\"1207664643\"}"));

    // Not a game and not from a store: not asked about. Sorted as a Game: asked about with what there is.
    snprintf(p, sizeof p, "%s/Tools/Thing/thing.exe", root);
    T(!exe_clues("thing.exe", p, "Thing", false, body, sizeof body));
    T(exe_clues("thing.exe", p, "Thing", true, body, sizeof body) && !strcmp(body, "{\"exe\":\"thing.exe\",\"folder\":\"Thing\",\"name\":\"Thing\"}"));

    // The answer: a store id or the exe table is sure; a name only when it was the game's exactly
    char name[128];
    bool sure;
    const char *art;
    i64 art_len;
    static const char by_id[] = "{\"game\":{\"id\":\"s1\",\"name\":\"Hollow Knight\",\"year\":2017},\"matched_by\":\"store_id\",\"confidence\":0.99,\"art\":{\"grid\":null}}";
    T(exe_from_answer(by_id, sizeof by_id - 1, name, sizeof name, &sure, &art, &art_len) && sure && !strcmp(name, "Hollow Knight") &&
      art_len == 13);
    static const char by_name[] = "{\"game\":{\"id\":\"s2\",\"name\":\"Thing\"},\"matched_by\":\"name_search\",\"confidence\":0.9,\"art\":{}}";
    T(exe_from_answer(by_name, sizeof by_name - 1, name, sizeof name, &sure, &art, &art_len) && !sure && !strcmp(name, "Thing"));
    static const char close_by[] = "{\"game\":{\"id\":\"s3\",\"name\":\"Thing 2\"},\"matched_by\":\"name_search\",\"confidence\":0.6,\"art\":{}}";
    T(!exe_from_answer(close_by, sizeof close_by - 1, name, sizeof name, &sure, &art, &art_len) && !name[0]);
    static const char none[] = "{\"game\":null,\"matched_by\":\"none\",\"confidence\":0,\"art\":{}}";
    T(!exe_from_answer(none, sizeof none - 1, name, sizeof name, &sure, &art, &art_len));

    // "Help name games": a Game the service couldn't place is told what it was named; nothing
    // is sent without the player's say-so, or for a game a store id decides
    {
        static Config saved;
        saved = g_app.config;
        bool online = g_gamedb.online;
        char service[128];
        str_set(service, g_gamedb.service);
        g_gamedb.online = true;
        str_set(g_gamedb.service, "test.invalid");
        AppInfo app = {};
        str_set(app.exe.s, "thing.exe");
        snprintf(app.path, sizeof app.path, "%s/Tools/Thing/thing.exe", root);
        app.category = CAT_GAME;
        auto confirms = [](const char **body) {
            i32 n = 0;
            for (auto &q : g_gamedb.asks)
                if (q.kind == ASK_CONFIRM && q.body) { n++; *body = q.body; }
            return n;
        };
        const char *sent = 0;
        g_app.config.share_names = false;
        gamedb_confirm(app, "Thing");
        T(confirms(&sent) == 0);
        g_app.config.share_names = true;
        gamedb_confirm(app, "Thing");
        T(confirms(&sent) == 1 && !strcmp(sent, "{\"exe\":\"thing.exe\",\"folder\":\"Thing\",\"name\":\"Thing\"}"));
        snprintf(app.path, sizeof app.path, "%s/Lib/steamapps/common/Hollow Knight/hollow_knight.exe", root);
        str_set(app.exe.s, "hollow_knight.exe");
        gamedb_confirm(app, "Hollow Knight");
        T(confirms(&sent) == 1);

        // A clip given a game by hand: the program longest in front during it is voted for, and a browser
        // only counts the game
        char focus[] = "100\tgame.exe\n200\tclipback.exe\n900\tfirefox.exe\n1000\tgame.exe\n";
        char exe[64];
        T(focus_longest(focus, 150, 1020, exe, sizeof exe) && !strcmp(exe, "firefox.exe"));
        char focus2[] = "100\tgame.exe\n200\tclipback.exe\n900\tfirefox.exe\n1000\tgame.exe\n";
        T(focus_longest(focus2, 0, 1500, exe, sizeof exe) && !strcmp(exe, "game.exe"));
        char focus3[] = "100\tclipback.exe\n200\t\n";
        T(!focus_longest(focus3, 0, 300, exe, sizeof exe));
        T(is_browser("Chrome.exe") && !is_browser("game.exe"));
        {
            Saving keep = g_app.saving;
            Path buffer = g_app.config.buffer_dir, save_dir = g_app.config.save_dir;
            swprintf(g_app.config.buffer_dir.s, MAX_PATH, L"%hs/clipbuf", root);
            swprintf(g_app.config.save_dir.s, MAX_PATH, L"%hs/clipsave", root);
            CreateDirectoryW(g_app.config.buffer_dir.s, 0);
            LibItem it = {};
            str_set(it.name, "Clip 1");
            str_set(it.ext, ".mp4");
            it.year = 2026;
            g_app.saving = {};
            g_app.saving.saved_loaded = true;
            g_app.saving.saved_count = 1;
            g_app.saving.saved[0].from_us = 1000;
            g_app.saving.saved[0].to_us = 5000;
            wide_to_utf8(lib_item_path(it).s, g_app.saving.saved[0].path, sizeof g_app.saving.saved[0].path);
            FILE *ff = _wfopen(path_join(g_app.config.buffer_dir, L"focus.tsv").s, L"wb");
            fputs("500\tdiscord.exe\n1500\tchrome.exe\n4800\tdiscord.exe\n", ff);
            fclose(ff);
            gamedb_clip_game(it, "Chess");
            T(confirms(&sent) == 2 && !strcmp(sent, "{\"exe\":\"chrome.exe\",\"name\":\"Chess\"}"));
            str_set(it.name, "Not saved here");
            gamedb_clip_game(it, "Chess");
            T(confirms(&sent) == 2);
            g_app.saving = keep;
            g_app.config.buffer_dir = buffer;
            g_app.config.save_dir = save_dir;
        }
        for (auto &q : g_gamedb.asks) {
            free(q.body);
            q = {};
        }
        g_gamedb.online = online;
        str_set(g_gamedb.service, service);
        g_app.config = saved;
    }

    const char *files[] = {"Lib/steamapps/appmanifest_367520.acf", "Lib/steamapps/appmanifest_10.acf", "GOG/Witcher 3/goggame-1207664643.info"};
    for (auto file : files) {
        snprintf(p, sizeof p, "%s/%s", root, file);
        Path w = {};
        utf8_to_wide(p, w.s, MAX_PATH);
        DeleteFileW(w.s);
    }
    const char *dirs[] = {"Lib/steamapps/common/Hollow Knight", "Lib/steamapps/common", "Lib/steamapps", "Lib", "GOG/Witcher 3/bin/x64",
                          "GOG/Witcher 3/bin", "GOG/Witcher 3", "GOG", ""};
    for (auto d : dirs) {
        snprintf(p, sizeof p, "%s/%s", root, d);
        Path w = {};
        utf8_to_wide(p, w.s, MAX_PATH);
        RemoveDirectoryW(w.s);
    }
}

// True when every <tag> in the text is closed in order (self-closing and <? ... ?> / <! ... > tags aside)
static bool xml_balanced(const char *s)
{
    static char stack[64][64];
    i32 depth = 0;
    for (const char *p = strchr(s, '<'); p; p = strchr(p + 1, '<')) {
        if (p[1] == '?' || p[1] == '!') continue;
        const char *end = strchr(p, '>');
        if (!end) return false;
        if (end[-1] == '/') continue;
        bool closing = p[1] == '/';
        const char *n = p + (closing ? 2 : 1);
        i32 len = (i32)strcspn(n, " >");
        if (len >= 64) return false;
        if (closing) {
            if (!depth || strncmp(stack[depth - 1], n, len) || stack[depth - 1][len]) return false;
            depth--;
        } else {
            if (depth == 64) return false;
            memcpy(stack[depth], n, len);
            stack[depth++][len] = 0;
        }
    }
    return depth == 0;
}

// The editor's XML for Premiere and DaVinci: well formed, the trim in frames, a track per recorded track
// Advanced edits: Simple to Advanced and back, the record on disk, settling overlaps, and the render's graph.
static void test_edit_advanced()
{
    static MediaInfo m;
    m = {};
    m.duration = 10, m.width = 1920, m.height = 1080, m.fps = 30, m.track_count = 3;
    str_set(m.tracks[0], "Mix"), str_set(m.tracks[1], "Mic"), str_set(m.tracks[2], "Game");
    m.channels[0] = 2, m.channels[1] = 1, m.channels[2] = 2;
    auto about = [](f64 a, f64 b) { return fabs(a - b) < 1e-4; };

    // Simple: 1-4 then 6-9 with a 1 s crossfade, a fade in, zoom keys at 2 and 7, the Mic at 150%, the mix at 50%
    static EditRecord s;
    s = {};
    s.pieces[0] = {1, 4};
    s.pieces[1] = {6, 9, TRANS_CROSS, 1.0f};
    s.piece_count = 2;
    s.fade_in = 0.5f;
    s.keys[0] = {2, 2, 0.25, 0.5, EASE_LINEAR};
    s.keys[1] = {7, 3, 0.75, 0.5, EASE_LINEAR};
    s.key_count = 2;
    s.boost[1] = 0.5f;
    s.boost[EDIT_MASTER] = -0.5f;
    s.muted[2] = true;
    static EditRecord a;
    a = s;
    edit_to_advanced(&a, m);
    T(a.mode == EDIT_ADVANCED && a.item_count == 6 && !a.piece_count && !a.key_count);
    i32 idx[EDIT_MAX_ITEMS];
    T(track_items(a, 0, idx) == 2 && about(a.items[idx[1]].start, 3) && about(a.items[idx[1]].cross, 1));
    T(about(a.items[idx[0]].fade_in, 0.5) && a.items[idx[0]].link != a.items[idx[1]].link);
    T(track_items(a, 1, idx) == 0 && track_items(a, 2, idx) == 2 && track_items(a, 3, idx) == 2);
    T(a.muted[2] && !a.muted[1]);
    // The Mic's 150% times the mix's 50%, flat: one key
    i32 first, mic = idx[0];
    track_items(a, 2, idx);
    mic = idx[0];
    T(ikey_range(a, a.items[mic].id, IKEY_VOLUME, &first) == 1 && about(a.ikeys[first].v, 0.75));
    // The zoom split at the cuts: 1 (held at 2x), 2, and 4 (a fifth of the way from 2x to 3x)
    track_items(a, 0, idx);
    u16 v0 = a.items[idx[0]].id, v1 = a.items[idx[1]].id;
    T(ikey_range(a, v0, IKEY_ZOOM, &first) == 3 && about(a.ikeys[first].t, 1) && about(a.ikeys[first].v, 2) &&
      about(a.ikeys[first + 2].t, 4) && about(a.ikeys[first + 2].v, 2.4));
    f64 z, x, y;
    item_zoom_at(a, v1, 8, &z, &x, &y);
    T(about(z, 3) && about(x, 0.75));
    T(item_at(a, 0, 3.5) == idx[1] && item_at(a, 0, 2.99) == idx[0] && item_at(a, 0, 6.5) == -1);
    T(about(adv_len(a), 6));
    T(about(item_fade_gain(a, m.duration, idx[0], 1.25), 0.5));

    // Back to Simple and again gives the same edit, so going back needs no asking
    T(edit_round_trips(a, m));
    static EditRecord b;
    b = a;
    edit_to_simple(&b, m);
    T(b.mode == EDIT_SIMPLE && b.piece_count == 2 && b.pieces[1].trans == TRANS_CROSS && about(b.fade_in, 0.5));
    T(!b.item_count && b.vol_count[1] == 2 && about(b.vol[1][0].v, 0.75) && b.muted[2]);
    // An unlinked move of the Mic, a gap or a changed key does not
    b = a;
    track_items(b, 2, idx);
    b.items[idx[1]].start += 0.5;
    T(!edit_round_trips(b, m));
    b = a;
    track_items(b, 0, idx);
    b.items[idx[1]].start += 1;
    T(!edit_round_trips(b, m));

    // The record on disk
    wchar_t dir[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    Path file = {};
    swprintf(file.s, MAX_PATH, L"%sclipback-test-adv-%u.edit.txt", dir, GetCurrentProcessId());
    edit_write(file, a);
    T(edit_is_advanced(file));
    static EditRecord r;
    edit_read(file, &r);
    DeleteFileW(file.s);
    T(r.mode == EDIT_ADVANCED && r.item_count == a.item_count && r.ikey_count == a.ikey_count && adv_same(r, a, m));
    edit_write(file, s);
    T(!edit_is_advanced(file));
    DeleteFileW(file.s);

    // An item run over the next one on its track is cut back to it; one left too short goes
    b = a;
    track_items(b, 0, idx);
    b.items[idx[0]].to += 2; // now ends at 5 on the sequence, over the next item at 3
    adv_settle(&b, m.duration);
    track_items(b, 0, idx);
    T(about(item_end(b.items[idx[0]]), 3));
    b.items[idx[1]].start = 0.05; // over all but 0.05 s of the first
    adv_settle(&b, m.duration);
    T(track_items(b, 0, idx) == 1 && b.item_count == 5);
    T(item_find(b, v0) < 0 && ikey_range(b, v0, IKEY_ZOOM, &first) == 0);

    // The graph: the crossfade overlaps the first item's 3.5 s (half the crossfade past its end) by 1 s; with the
    // second item moved on 1 s there's a black second between them instead.
    static EditJob j;
    j = {};
    j.info = m;
    j.edit = a;
    SaveWork *w = &g_save_work;
    w->graph_len = 0, w->graph_full = false, w->graph[0] = 0;
    i32 streams[MEDIA_MAX_TRACKS + 1];
    i32 ns = adv_graph(w, j, 0.5, "", "yuv420p", true, streams);
    T(ns == 3 && streams[0] == -1 && streams[1] == 1 && streams[2] == 2);
    T(strstr(w->graph, "xfade=transition=fade:duration=1.0000:offset=2.5000") != 0);
    T(strstr(w->graph, "acrossfade=d=1.0000") != 0);
    T(strstr(w->graph, "[c0_0]trim=start=0.5000:end=4.0000") != 0); // 1 to 4.5 in the video, read from 0.5
    T(strstr(w->graph, "fade=t=in:st=0:d=0.5000") != 0);
    T(strstr(w->graph, "amix") == 0 && strstr(w->graph, "[x1]alimiter") != 0); // the Game is muted: the Mic alone
    j.edit.items[item_find(j.edit, v1)].start = 4;
    w->graph_len = 0, w->graph[0] = 0;
    adv_graph(w, j, 0.5, "", "yuv420p", false, streams);
    T(strstr(w->graph, "color=c=black:s=1920x1080:r=30.0000:d=1.0000") != 0 && strstr(w->graph, "xfade") == 0);
    T(strstr(w->graph, "[o1]") == 0);

    // Share's graph: the picture zoomed and coloured per item, black in the gap, and one sound stream, the Mic and
    // the Game (unmuted now) at their item volumes, faded and mixed
    j.edit = a;
    j.edit.muted[2] = false;
    track_items(j.edit, 0, idx);
    ItemKey ck = {};
    ck.item = j.edit.items[idx[1]].id, ck.what = IKEY_COLOR + COLOR_SATURATION, ck.t = 7, ck.v = 1.5;
    ikey_add(&j.edit, ck);
    for (i32 i = 0; i < j.edit.item_count; i++) // the second picture item and its sound, a second later
        if (j.edit.items[i].link == j.edit.items[idx[1]].link) j.edit.items[i].start = 4;
    w->graph_len = 0, w->graph[0] = 0;
    ns = adv_graph(w, j, 0.5, "", "yuv420p", false, streams);
    T(ns == 1 && streams[0] == -1);
    T(strstr(w->graph, "perspective=") != 0 && strstr(w->graph, "eq=") != 0 && strstr(w->graph, "color=c=black") != 0);
    T(strstr(w->graph, "amix=inputs=2") != 0 && strstr(w->graph, "volume='") != 0 && strstr(w->graph, "[o0]") != 0);
    T(strstr(w->graph, "afade=t=in:st=0:d=0.5000") != 0);

    // A Share made for an Advanced edit isn't reused once the edit changes, nor for the same cuts in Simple
    u64 h_adv = share_hash(L"C:\\clip.mp4", &a, false, 1000, 2000, HOST_R2, "");
    b = a;
    T(share_hash(L"C:\\clip.mp4", &b, false, 1000, 2000, HOST_R2, "") == h_adv);
    track_items(b, 2, idx);
    b.items[idx[1]].start += 0.5;
    T(share_hash(L"C:\\clip.mp4", &b, false, 1000, 2000, HOST_R2, "") != h_adv);
    b = a;
    b.ikeys[0].v += 0.1;
    T(share_hash(L"C:\\clip.mp4", &b, false, 1000, 2000, HOST_R2, "") != h_adv);
    T(share_hash(L"C:\\clip.mp4", &s, false, 1000, 2000, HOST_R2, "") != h_adv);
    // A deleted item leaves its slot and its keys' slots clear, so the record hashes as if it had never been there
    b = a;
    item_remove(&b, b.item_count - 1);
    static const EditItem clear_item = {};
    T(!memcmp(&b.items[b.item_count], &clear_item, sizeof clear_item) && !b.ikeys[b.ikey_count].item);

    // Split at 1.5 with nothing selected: the picture and both sound items there, the second halves linked to each
    // other and not to the first halves, each with the keys of the item it came from
    Editor &ed = g_app.editor;
    ed.item.info = m;
    ed.edit = a;
    ed.adv_sel_count = 0;
    adv_split(1.5);
    T(ed.edit.item_count == 9 && track_items(ed.edit, 0, idx) == 3);
    const EditItem &h0 = ed.edit.items[idx[0]], &h1 = ed.edit.items[idx[1]];
    T(near(item_end(h0), 1.5) && near(h1.start, 1.5) && near(h1.from, 2.5) && near(h1.to, 4));
    T(h1.link && h1.link != h0.link && ikey_range(ed.edit, h1.id, IKEY_ZOOM, &first) == 3);
    track_items(ed.edit, 2, idx);
    T(ed.edit.items[idx[1]].link == h1.link && near(ed.edit.items[idx[1]].start, 1.5));
    ed.edit = {};
}

static void test_edit_xml()
{
    Editor &ed = g_app.editor;
    static const Editor blank = {};
    ed = blank;
    wchar_t dir[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    swprintf(ed.path.s, MAX_PATH, L"%sclipback test %u & co.mp4", dir, GetCurrentProcessId());
    str_set(ed.item.name, "clipback test & co");
    MediaInfo &m = ed.item.info;
    m.duration = 10;
    m.width = 1920;
    m.height = 1080;
    m.fps = 59.94;
    m.track_count = 3;
    str_set(m.tracks[0], "Mix");
    str_set(m.tracks[1], "Mic");
    str_set(m.tracks[2], "Overwatch (game)");
    m.channels[0] = 2;
    m.channels[1] = 1;
    m.channels[2] = 2;
    EditRecord &e = ed.edit;
    e.pieces[0] = {1, 9};
    e.piece_count = 1;
    e.keys[0] = {2.0, 1.0, 0.5, 0.5, EASE_LINEAR};
    e.keys[1] = {4.0, 2.0, 0.25, 0.75, EASE_SMOOTH};
    e.key_count = 2;
    e.muted[1] = true;
    e.vol[2][0] = {3.0, 0.5f, EASE_LINEAR};
    e.vol_count[2] = 1;

    Path xml = {};
    T(ed_export_xml(&xml));
    FILE *f = _wfopen(xml.s, L"rb");
    static char text[1 << 20];
    size_t n = f ? fread(text, 1, sizeof text - 1, f) : 0;
    if (f) fclose(f);
    text[n] = 0;
    DeleteFileW(xml.s);
    T(n > 0 && wcsstr(xml.s, L".xml"));
    T(xml_balanced(text));
    T(strstr(text, "<name>clipback test &amp; co</name>"));
    T(strstr(text, "<timebase>60</timebase>"));
    T(strstr(text, "<duration>480</duration>"));                      // 8 s at 60 fps
    T(strstr(text, "<in>60</in><out>540</out>"));
    T(strstr(text, "%20%26%20co.mp4</pathurl>"));                     // the space and & escaped in the URL
    T(strstr(text, "<width>1920</width><height>1080</height>"));
    T(strstr(text, "<effectid>basic</effectid>") && strstr(text, "<value>200.00</value>")); // the zoom, in percent
    i32 tracks = 0, disabled = 0;
    for (const char *p = text; (p = strstr(p, "<sourcetrack>")); p++) tracks++;
    for (const char *p = text; (p = strstr(p, "<enabled>FALSE</enabled>")); p++) disabled++;
    i32 want[MEDIA_MAX_TRACKS];
    T(tracks == 3 && ed_tracks(m, want, MEDIA_MAX_TRACKS) == 2);      // Mic and the game's two; the Mix left out
    T(disabled == 1);                                                  // the muted Mic
    // Two slots per track: Mix 1-2, Mic 3, the game 5-6 as a linked stereo pair
    T(strstr(text, "premiereTrackType=\"Mono\" currentExplodedTrackIndex=\"0\" totalExplodedTrackCount=\"1\""));
    T(strstr(text, "<trackindex>3</trackindex></sourcetrack>"));
    T(strstr(text, "premiereTrackType=\"Stereo\" currentExplodedTrackIndex=\"1\" totalExplodedTrackCount=\"2\""));
    T(strstr(text, "<trackindex>5</trackindex></sourcetrack>") && strstr(text, "<trackindex>6</trackindex></sourcetrack>"));
    T(!strstr(text, "<trackindex>4</trackindex></sourcetrack>"));
    T(strstr(text, "premiereChannelType=\"mono\"") && strstr(text, "premiereChannelType=\"stereo\""));
    T(strstr(text, "<linkclipref>clipitem-4</linkclipref>"));
    T(strstr(text, "<channelcount>1</channelcount>"));

    // Two pieces, the second first: each a clipitem one after the other on every track.
    e.pieces[0] = {6, 8};
    e.pieces[1] = {1, 3};
    e.piece_count = 2;
    T(ed_export_xml(&xml));
    f = _wfopen(xml.s, L"rb");
    n = f ? fread(text, 1, sizeof text - 1, f) : 0;
    if (f) fclose(f);
    text[n] = 0;
    DeleteFileW(xml.s);
    T(xml_balanced(text));
    T(strstr(text, "<duration>240</duration>"));                      // 4 s at 60 fps
    T(strstr(text, "<start>0</start><end>120</end><in>360</in><out>480</out>"));
    T(strstr(text, "<start>120</start><end>240</end><in>60</in><out>180</out>"));
    i32 items = 0;
    for (const char *p = text; (p = strstr(p, "<clipitem ")); p++) items++;
    T(items == 8);                                                     // video, Mic, the game's two; twice
    T(strstr(text, "<clipindex>2</clipindex>"));

    // A crossfade into the second piece and a fade in: transitionitems on every track (video, Mic, the game's two)
    e.pieces[1].trans = TRANS_CROSS;
    e.pieces[1].trans_len = 1;
    e.fade_in = 0.5f;
    T(ed_export_xml(&xml));
    f = _wfopen(xml.s, L"rb");
    n = f ? fread(text, 1, sizeof text - 1, f) : 0;
    if (f) fclose(f);
    text[n] = 0;
    DeleteFileW(xml.s);
    T(xml_balanced(text));
    T(strstr(text, "<start>90</start><end>150</end><alignment>center</alignment>"));
    T(strstr(text, "<start>0</start><end>30</end><alignment>start-black</alignment>"));
    i32 trans = 0;
    for (const char *p = text; (p = strstr(p, "<transitionitem>")); p++) trans++;
    T(trans == 8);
    ed = blank;
}

static void test_save_names()
{
    // 2026-11-15 21:04:05 local, whatever the time zone
    SYSTEMTIME lt = {2026, 11, 0, 15, 21, 4, 5, 0}, utc;
    TzSpecificLocalTimeToSystemTime(0, &lt, &utc);
    FILETIME ft;
    SystemTimeToFileTime(&utc, &ft);
    i64 us = ((((i64)ft.dwHighDateTime << 32) | ft.dwLowDateTime) - 116444736000000000LL) / 10;
    char out[256];
    save_name_expand(SAVE_NAME_DEFAULT, "Overwatch", us, out, sizeof out);
    T(!strcmp(out, "Overwatch 21-04 15-11-2026"));
    save_name_expand("{YY}{MM}{DD}_{hh}{mm}{ss} {game}", "Desktop", us, out, sizeof out);
    T(!strcmp(out, "261115_210405 Desktop"));
    save_name_expand("{game}: {hh}:{mm}?", "A/B", us, out, sizeof out);
    T(!strcmp(out, "AB 2104"));
    save_name_expand("{nope} {game}", "X", us, out, sizeof out);
    T(!strcmp(out, "{nope} X"));
    save_name_expand(":/?", "X", us, out, sizeof out);
    T(!strcmp(out, "X"));
}

int main()
{
    InitializeSRWLock(&g_log_lock);
    test_lengths();
    test_select_range();
    test_tags();
    test_card_clicks();
    test_sound_plan();
    test_unique_keys();
    test_game_pick();
    test_game_from_name();
    test_game_guess();
    test_first_run();
    test_telemetry();
    test_backup();
    test_save_names();
    test_buffer_names();
    test_shown_names();
    test_folder_rules();
    test_fields();
    test_hotkeys();
    test_settings_round_trip();
    test_pieces();
    test_edit_volumes();
    test_edit_xml();
    test_edit_advanced();
    test_lanes();
    test_share();
    test_chapters();
    test_wave_chunks();
    test_gamedb();
    test_mcpack();
    test_r2();
    test_storeids();
    test_markers();
    printf("%d checks, %d failed\n", g_checked, g_failed);
    return g_failed;
}
