// Tests for the parts of Rekodr that are plain logic: parsing, folder rules, the settings file. `test.bat` builds
// this into build\tests.exe and runs it; it exits with the number of failures.
//
// It is the whole program (rekodr.cpp) with its entry point renamed, so every function can be called as it is.
// Nothing here starts the recorder, opens a window or touches the real settings.

#define wWinMain rekodr_main
#include "rekodr.cpp"
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

// Names from the template in Options are shown as their game alone, no date or time; others as they are
static void test_shown_names()
{
    str_set(g_app.config.name_template, SAVE_NAME_DEFAULT);
    g_app.config.clock_24h = false;
    char name[300], out[300];
    lib_shown_name("Minecraft 15-00 20-11-2020", out, sizeof out);
    T(!strcmp(out, "Minecraft"));
    lib_shown_name("Minecraft 21-30 20-11-2020 (2)", out, sizeof out);
    T(!strcmp(out, "Minecraft (2)"));
    lib_shown_name("Clip 21-30 20-11-2020", out, sizeof out);
    T(!strcmp(out, GAME_DESKTOP));
    lib_shown_name("Minecraft 2026-11-19 21-30-00", out, sizeof out);
    T(!strcmp(out, "Minecraft 2026-11-19 21-30-00"));
    lib_shown_name("Minecraft 25-00 20-11-2020", out, sizeof out);
    T(!strcmp(out, "Minecraft 25-00 20-11-2020"));
    save_name_expand(SAVE_NAME_DEFAULT, "Minecraft", now_us(), name, sizeof name);
    lib_shown_name(name, out, sizeof out);
    T(!strcmp(out, "Minecraft"));
    lib_shown_name("Over 9 15-05 20-11-2020", out, sizeof out);
    T(!strcmp(out, "Over 9"));
    T(lib_name_matches("Clip 15-05 20-11-2020", "desktop"));
    // A card's own date and time, with the year when it isn't this one
    char when[48];
    fmt_when(lib_name_us(SAVE_NAME_DEFAULT, "Minecraft 17-32 07-10-2020", 0), when, sizeof when);
    T(!strcmp(when, "7 Oct 2020 5:32 pm"));
    g_app.config.clock_24h = true;
    fmt_when(lib_name_us(SAVE_NAME_DEFAULT, "Minecraft 17-32 07-10-2020", 0), when, sizeof when);
    T(!strcmp(when, "7 Oct 2020 17:32"));
    // Sorting and the Library's periods go by the name's time, or the file's when the name has none
    i64 at = now_us() - 3600ll * 1000000;
    save_name_expand(SAVE_NAME_DEFAULT, "Minecraft", at, name, sizeof name);
    i64 got = lib_name_us(SAVE_NAME_DEFAULT, name, 5);
    T(got <= at && at - got < 60ll * 1000000);
    T(lib_name_us(SAVE_NAME_DEFAULT, "Minecraft 2026-11-19 21-30-00", 5) == 5);
    str_set(g_app.config.name_template, "{YYYY}-{MM}-{DD} {hh}.{mm} {game}");
    lib_shown_name("2020-11-20 15.05 Overwatch 2", out, sizeof out);
    T(!strcmp(out, "Overwatch 2"));
}

// The Library's headings: periods back from today, then months, each period one run of days
static void test_lib_periods()
{
    char out[32];
    i64 now = now_us(), day_us = 86400ll * 1000000;
    lib_period(now, out, sizeof out);
    T(!strcmp(out, "Today"));
    lib_period(now - day_us, out, sizeof out);
    T(!strcmp(out, "Yesterday"));
    lib_period(lib_name_us(SAVE_NAME_DEFAULT, "Minecraft 12-00 15-05-2020", 0), out, sizeof out);
    T(!strcmp(out, "May 2020"));
    // Going back a day at a time, the headings come in order and none comes back once left
    static const char *const order[] = {"Today", "Yesterday", "This week", "Last week", "This month", "Last month"};
    i32 at = 0;
    i64 last = lib_period(now, 0, 0), changes = 0;
    bool ordered = true, seen_month = false;
    for (i32 d = 1; d < 120; d++) {
        i64 key = lib_period(now - d * day_us, out, sizeof out);
        if (key == last) continue;
        changes++;
        last = key;
        i32 k = 0;
        while (k < COUNT(order) && strcmp(out, order[k])) k++;
        if (k < COUNT(order)) {
            ordered = ordered && !seen_month && k > at;
            at = k;
        } else seen_month = true;
    }
    T(ordered && seen_month && changes >= 4);
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
// A buffer with more stream folders and segments than any fixed size: the index holds all of them, the newest too,
// and a read since the last one keeps them.
static void test_buffer_many()
{
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    Path root = {};
    swprintf(root.s, MAX_PATH, L"%srekodr-test-buffer-%u", temp, GetCurrentProcessId());
    Path audio = path_join(root, L"audio");
    SHCreateDirectoryExW(0, audio.s, 0);
    const i32 streams = 1100, segs = 70; // 77000 segments
    auto put = [](const Path &p, const char *text) {
        FILE *f = _wfopen(p.s, L"wb");
        if (f) { fputs(text, f); fclose(f); }
    };
    for (i32 i = 0; i < streams; i++) {
        wchar_t name[64];
        swprintf(name, 64, L"2026%04d_mic_0", i);
        Path dir = path_join(audio, name);
        CreateDirectoryW(dir.s, 0);
        char meta[128];
        snprintf(meta, sizeof meta, "{\"kind\":\"mic\",\"exe\":\"mic\",\"start_us\":%lld}", 1000000000ll * (i + 1));
        put(path_join(dir, L"meta.json"), meta);
        static char list[8192];
        list[0] = 0;
        for (i32 k = 0; k < segs; k++) {
            wchar_t seg[32];
            swprintf(seg, 32, L"a_%05d.mka", k);
            put(path_join(dir, seg), "x");
            char line[64];
            snprintf(line, sizeof line, "a_%05d.mka,%d,%d\n", k, k * 10, k * 10 + 10);
            strcat(list, line);
        }
        put(path_join(dir, L"list.csv"), list);
    }
    BufferIdx *b = (BufferIdx *)calloc(1, sizeof(BufferIdx)), *again = (BufferIdx *)calloc(1, sizeof(BufferIdx));
    bool full = false;
    T(buffer_read(root, b, 0, &full) && full);
    T(b->stream_count == streams && b->audio_seg_count == streams * segs);
    T(!strcmp(b->streams[streams - 1].dir, "20261099_mic_0") && b->streams[streams - 1].seg_count == segs);
    T(b->audio_segs[b->streams[streams - 1].first_seg + segs - 1].end_us == 1000000000ll * streams + 700000000ll);
    T(buffer_read(root, again, b, &full) && !full); // nothing changed: taken from the last read
    T(again->stream_count == streams && again->audio_seg_count == streams * segs);
    BufferIdx *copy = (BufferIdx *)calloc(1, sizeof(BufferIdx));
    buffer_idx_copy(copy, b);
    T(copy->audio_seg_count == streams * segs && copy->audio_segs != b->audio_segs &&
      !strcmp(copy->streams[streams - 1].dir, "20261099_mic_0"));
    BufferIdx *all[] = {b, again, copy};
    for (BufferIdx *x : all) {
        buffer_idx_free(x);
        free(x);
    }
    SHFILEOPSTRUCTW op = {};
    static wchar_t from[MAX_PATH + 2];
    memset(from, 0, sizeof from);
    wcscpy(from, root.s);
    op.wFunc = FO_DELETE;
    op.pFrom = from;
    op.fFlags = FOF_NO_UI;
    T(SHFileOperationW(&op) == 0);
}

// The preview page's picture (r2.cpp): a short clip gives a JPEG of its size.
static void test_r2_thumb()
{
    Path ffmpeg = {};
    if (!ffmpeg_find(Path{}, &ffmpeg)) {
        printf("test_r2_thumb: no ffmpeg, skipped\n");
        return;
    }
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    Path clip = {}, jpg = {};
    swprintf(clip.s, MAX_PATH, L"%srekodr-test-thumb-%u.mp4", temp, GetCurrentProcessId());
    swprintf(jpg.s, MAX_PATH, L"%s.jpg", clip.s);
    wchar_t args[2048] = {};
    static const char *const gen[] = {"-hide_banner", "-loglevel", "error", "-nostdin", "-y", "-f", "lavfi", "-i",
                                      "testsrc=size=320x180:rate=30:duration=2", "-pix_fmt", "yuv420p"};
    arg_list(args, COUNT(args), gen, COUNT(gen));
    cmd_arg(args, COUNT(args), clip.s);
    T(proc_run(ffmpeg.s, args, 60000, 0, 0) == 0);
    i32 w = 0, h = 0;
    T(r2_make_thumb(clip, ffmpeg, jpg, &w, &h));
    T(w == 320 && h == 180);
    i64 len = 0;
    char *pic = read_file(jpg.s, &len);
    T(pic && len > 2 && (u8)pic[0] == 0xFF && (u8)pic[1] == 0xD8);
    free(pic);
    DeleteFileW(jpg.s);
    DeleteFileW(clip.s);
}

// Delete footage (erase.cpp): video segments over the stretch go, sound over it is silenced exactly, the rest stays.
static void test_erase()
{
    Path ffmpeg = {};
    if (!ffmpeg_find(Path{}, &ffmpeg)) {
        printf("test_erase: no ffmpeg, skipped\n");
        return;
    }
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    Path root = {};
    swprintf(root.s, MAX_PATH, L"%srekodr-test-erase-%u", temp, GetCurrentProcessId());
    Path run = path_join(path_join(root, L"video"), L"run1"), stream = path_join(path_join(root, L"audio"), L"s1_mic_0");
    Path posters = path_join(root, L"posters");
    SHCreateDirectoryExW(0, run.s, 0);
    SHCreateDirectoryExW(0, stream.s, 0);
    CreateDirectoryW(posters.s, 0);
    auto put = [](const Path &p, const char *text) {
        FILE *f = _wfopen(p.s, L"wb");
        if (f) { fputs(text, f); fclose(f); }
    };
    // 30 s of a tone, as the recorder writes it: 10 s Opus segments whose timestamps carry on from the start
    const i64 t0 = 1700000000000000ll;
    char text[256];
    snprintf(text, sizeof text, "{\"kind\":\"mic\",\"exe\":\"mic\",\"start_us\":%lld}", t0);
    put(path_join(stream, L"meta.json"), text);
    wchar_t args[2048] = {};
    static const char *const gen[] = {"-hide_banner", "-loglevel", "error", "-nostdin", "-f", "lavfi", "-i",
                                      "sine=frequency=440:duration=30:sample_rate=48000", "-ac", "2", "-c:a", "libopus",
                                      "-b:a", "128k", "-f", "segment", "-segment_time", "10", "-segment_format",
                                      "matroska", "-reset_timestamps", "0", "-segment_list_type", "csv", "-segment_list"};
    arg_list(args, COUNT(args), gen, COUNT(gen));
    cmd_arg(args, COUNT(args), path_join(stream, L"list.csv").s);
    cmd_arg(args, COUNT(args), path_join(stream, L"a_%05d.mka").s);
    T(proc_run(ffmpeg.s, args, 60000, 0, 0) == 0);
    u8 peaks[300];
    memset(peaks, 200, sizeof peaks);
    FILE *pf = _wfopen(path_join(stream, L"peaks.bin").s, L"wb");
    if (pf) { fwrite(peaks, 1, sizeof peaks, pf); fclose(pf); }
    // 15 video segments of 2 s (their content isn't read)
    snprintf(text, sizeof text, "%lld", t0);
    put(path_join(run, L"anchor.txt"), text);
    static char list[2048];
    list[0] = 0;
    for (i32 k = 0; k < 15; k++) {
        wchar_t seg[32];
        swprintf(seg, 32, L"v_%05d.mkv", k);
        put(path_join(run, seg), "x");
        char line[64];
        snprintf(line, sizeof line, "v_%05d.mkv,%d.000000,%d.000000\n", k, k * 2, k * 2 + 2);
        strcat(list, line);
    }
    put(path_join(run, L"list.csv"), list);
    wchar_t pname[64];
    swprintf(pname, COUNT(pname), L"p-%016llx.jpg", (u64)(t0 + 12000000));
    put(path_join(posters, pname), "x");

    // 13 s to 15.5 s: video segments 6 and 7 (12-16 s) go; sound segment 1 (10-20 s) is silent from 13 s to 15.5 s
    EraseJob j = {};
    j.from_us = t0 + 13000000;
    j.to_us = t0 + 15500000;
    j.buffer_dir = root;
    j.ffmpeg = ffmpeg;
    j.posters = posters;
    BufferIdx *b = (BufferIdx *)calloc(1, sizeof(BufferIdx));
    EraseDone *done = (EraseDone *)malloc(ERASE_DONE_MAX * sizeof(EraseDone));
    i32 done_n = 0, videos = 0;
    bool open = true;
    u64 before0 = 0, before1 = 0;
    {
        WIN32_FILE_ATTRIBUTE_DATA a = {};
        if (GetFileAttributesExW(path_join(stream, L"a_00000.mka").s, GetFileExInfoStandard, &a)) before0 = a.nFileSizeLow;
        if (GetFileAttributesExW(path_join(stream, L"a_00001.mka").s, GetFileExInfoStandard, &a)) before1 = a.nFileSizeLow;
    }
    T(erase_pass(j, b, done, &done_n, &videos, &open));
    T(!open && videos == 2 && done_n == 1 && !strcmp(done[0].name, "a_00001.mka"));
    T(!file_exists(path_join(run, L"v_00006.mkv").s) && !file_exists(path_join(run, L"v_00007.mkv").s));
    T(file_exists(path_join(run, L"v_00005.mkv").s) && file_exists(path_join(run, L"v_00008.mkv").s));
    T(!file_exists(path_join(posters, pname).s));
    T(buffer_read(root, b) && b->seg_count == 13 && b->audio_seg_count >= 3);
    {
        WIN32_FILE_ATTRIBUTE_DATA a = {};
        T(GetFileAttributesExW(path_join(stream, L"a_00000.mka").s, GetFileExInfoStandard, &a) && a.nFileSizeLow == before0);
        T(GetFileAttributesExW(path_join(stream, L"a_00001.mka").s, GetFileExInfoStandard, &a) && a.nFileSizeLow != before1);
    }
    // The loudest sound in the stretch, and either side of it
    auto loudest = [&](f64 a, f64 z) {
        wchar_t c[2048] = {};
        static const char *const head[] = {"-hide_banner", "-nostdin", "-copyts", "-i"};
        arg_list(c, COUNT(c), head, COUNT(head));
        cmd_arg(c, COUNT(c), path_join(stream, L"a_00001.mka").s);
        arg_pair(c, COUNT(c), "-af", "atrim=start=%.3f:end=%.3f,volumedetect", a, z);
        cmd_arg8(c, COUNT(c), "-f");
        cmd_arg8(c, COUNT(c), "null");
        cmd_arg8(c, COUNT(c), "-");
        static char out[16384];
        proc_run(ffmpeg.s, c, 60000, out, sizeof out);
        const char *m = strstr(out, "max_volume:");
        return m ? atof(m + 11) : 0.0;
    };
    T(loudest(13.1, 15.4) < -80);
    T(loudest(11.0, 12.8) > -40);
    T(loudest(15.7, 19.0) > -40);
    FILE *rf = _wfopen(path_join(stream, L"peaks.bin").s, L"rb");
    if (rf) { fread(peaks, 1, sizeof peaks, rf); fclose(rf); }
    T(peaks[129] == 200 && peaks[130] == 0 && peaks[154] == 0 && peaks[155] == 200);
    // Again: nothing more to do
    T(!erase_pass(j, b, done, &done_n, &videos, &open) && done_n == 1);
    buffer_idx_free(b);
    free(b);
    free(done);
    SHFILEOPSTRUCTW op = {};
    static wchar_t from[MAX_PATH + 2];
    memset(from, 0, sizeof from);
    wcscpy(from, root.s);
    op.wFunc = FO_DELETE;
    op.pFrom = from;
    op.fFlags = FOF_NO_UI;
    T(SHFileOperationW(&op) == 0);
}

// Other apps' folders: a watched folder's videos and those a folder down are listed, the game named by that folder,
// and Rekodr's own save folder inside it isn't listed twice.
static void test_watch_scan()
{
    wchar_t t1[MAX_PATH] = L"C:/Videos/OBS/ ", t2[MAX_PATH] = L"D:\\";
    watch_tidy(t1);
    watch_tidy(t2);
    T(!wcscmp(t1, L"C:\\Videos\\OBS") && !wcscmp(t2, L"D:\\"));
    T(path_within(L"C:\\Videos\\Rekodr\\Clips", L"C:\\Videos") && path_within(L"C:\\Videos", L"C:\\Videos\\"));
    T(!path_within(L"C:\\Videos2", L"C:\\Videos") && !path_within(L"C:\\Videos", L"C:\\Videos\\Rekodr"));
    {
        // A drive's root gets no second separator, so a folder in it compares equal to the same folder typed out
        Path root = {}, dir = {};
        wcscpy_s(root.s, L"D:\\");
        wcscpy_s(dir.s, L"D:\\Videos");
        T(!wcscmp(path_join(root, L"OBS").s, L"D:\\OBS") && !wcscmp(path_join(dir, L"OBS").s, L"D:\\Videos\\OBS"));
    }
    T(display_hz(59) == 60 && display_hz(119) == 120 && display_hz(144) == 144 && display_hz(60) == 60);
    { // an own sound: 12 s of 8 kHz 8-bit mono, cut to its first 10 s
        static u8 wav[44 + 96000];
        u32 v;
        memcpy(wav, "RIFF", 4), v = sizeof wav - 8, memcpy(wav + 4, &v, 4), memcpy(wav + 8, "WAVEfmt ", 8);
        v = 16, memcpy(wav + 16, &v, 4);
        u16 fmt[2] = {1, 1};
        memcpy(wav + 20, fmt, 4);
        v = 8000, memcpy(wav + 24, &v, 4), memcpy(wav + 28, &v, 4);
        fmt[0] = 1, fmt[1] = 8;
        memcpy(wav + 32, fmt, 4), memcpy(wav + 36, "data", 4);
        v = 96000, memcpy(wav + 40, &v, 4);
        u32 len = 0, data, riff;
        f64 secs = 0;
        T(sound_wav_cut(wav, sizeof wav, &len, &secs) && secs == 12.0 && len == 44 + 80000);
        memcpy(&data, wav + 40, 4), memcpy(&riff, wav + 4, 4);
        T(data == 80000 && riff == len - 8);
        T(!sound_wav_cut(wav + 1, 40, &len, &secs));
    }

    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    Path root = {};
    swprintf(root.s, MAX_PATH, L"%srekodr-test-watch-%u", temp, GetCurrentProcessId());
    Path watch = path_join(root, L"Videos"), save = path_join(watch, L"Rekodr"), game = path_join(watch, L"Some Game");
    auto put = [](const Path &dir, const wchar_t *name) {
        SHCreateDirectoryExW(0, dir.s, 0);
        FILE *f = _wfopen(path_join(dir, name).s, L"wb");
        if (f) { fputs("x", f); fclose(f); }
    };
    put(save_clips_dir(save, 2026), L"ours.mp4");
    put(watch, L"obs.mkv");
    put(watch, L"notes.txt");
    put(game, L"sp.mp4");
    Path empty = path_join(watch, L"Empty Game");
    SHCreateDirectoryExW(0, empty.s, 0);
    lib_scan(save, path_join(root, L"cache"), Path{}, SAVE_NAME_DEFAULT, path_join(root, L"buffer"), &watch, 1);
    { // a folder with no videos yet is watched too
        bool held = false;
        for (i32 i = 0; i < (i32)g_lib_dir_count; i++) held |= !_wcsicmp(g_lib_dirs[i].path.s, empty.s) && g_lib_dirs[i].live;
        T(held);
    }
    i32 n = g_lib.item_count, ours = 0, obs = -1, sp = -1;
    for (i32 i = 0; i < n; i++) {
        const LibItem &it = g_lib.items[i];
        if (!strcmp(it.name, "ours")) ours += !it.dir;
        if (!strcmp(it.name, "obs")) obs = i;
        if (!strcmp(it.name, "sp")) sp = i;
    }
    T(n == 3 && ours == 1 && obs >= 0 && sp >= 0);
    if (obs >= 0 && sp >= 0) {
        T(!wcscmp(lib_file_path(save, g_lib.items[sp]).s, path_join(game, L"sp.mp4").s));
        T(!wcscmp(lib_file_path(save, g_lib.items[obs]).s, path_join(watch, L"obs.mkv").s));
        T(!strcmp(g_lib.items[sp].info.game, "Some Game") && g_lib.items[sp].kind == LIB_CLIPS);
        // another app's clips keep their edits apart: from a Rekodr clip of the same name, and from each other
        LibItem own = {}, other = g_lib.items[sp];
        str_set(own.name, "sp");
        other.dir = g_lib.items[obs].dir;
        Path e_own = lib_edit_path(own), e_sp = lib_edit_path(g_lib.items[sp]), e_other = lib_edit_path(other);
        T(wcscmp(e_own.s, e_sp.s) && wcscmp(e_sp.s, e_other.s) && wcscmp(e_own.s, e_other.s));
        T(wcsstr(e_sp.s, L"\\sp.") && !wcscmp(e_sp.s + wcslen(e_sp.s) - 9, L".edit.txt"));
    }
    g_lib.item_count = 0;
    { // the watch thread sees a file added in a watched folder, then closes its watches at the end
        if (!g_lib_watch.wake) g_lib_watch.wake = CreateEventW(0, FALSE, FALSE, 0);
        if (!g_lib_watch.thread) g_lib_watch.thread = thread_start(lib_watch_thread, 0);
        InterlockedExchange(&g_lib_watch.fired, 0);
        InterlockedExchange(&g_lib_watch.sync, 1);
        SetEvent(g_lib_watch.wake);
        bool fired = false;
        for (i32 i = 0; i < 40 && !fired; i++) {
            wchar_t name[32];
            swprintf(name, COUNT(name), L"new%d.mp4", i);
            put(empty, name);
            SetEvent(g_lib_watch.wake);
            Sleep(50);
            fired = InterlockedExchange(&g_lib_watch.fired, 0) != 0;
        }
        T(fired);
        library_stop();
        T(!g_lib_watch.thread);
        g_lib_watch.quit = 0;
    }

    SHFILEOPSTRUCTW op = {};
    wchar_t dbl[MAX_PATH + 2] = {};
    wcscpy_s(dbl, MAX_PATH, root.s);
    op.wFunc = FO_DELETE;
    op.pFrom = dbl;
    op.fFlags = FOF_NO_UI;
    SHFileOperationW(&op);
}

static void test_webcam()
{
    i32 sw = 0, sh = 0;
    T(cam_parse_size("  Stream #1:0: Video: h264 (Main), nv12(tv, progressive), 1280x720 [SAR 1:1 DAR 16:9], q=2-31, 30 fps", &sw,
                     &sh) && sw == 1280 && sh == 720);
    T(cam_parse_size("  Stream #1:0: Video: h264 (avc1 / 0x31637661), yuv420p, 640x480, 30 fps", &sw, &sh) && sw == 640 && sh == 480);
    T(!cam_parse_size("  Stream #0:0: Video: rawvideo, bgra, 256x144, 10 fps", &sw, &sh));
    const char *list =
        "[in#0 @ 0000026dd2097140] \"Pixel 10 Pro XL (Windows Virtual Camera)\" (video)\r\n"
        "[in#0 @ 0000026dd2097140]   Alternative name \"@device_pnp_\\\\?\\swd#vcamdevapi#92de\"\r\n"
        "I2026-11-26 03:30:47.009207 (20168) [INFO] [VCAMDS] ffmpeg.exe\r\n"
        "[in#0 @ 0000026dd2097140] \"Iriun Webcam\" (video)\n"
        "[in#0 @ 0000026dd2097140] \"Line (RODE AI-1)\" (audio)\n"
        "[in#0 @ 0000026dd2097140] \"OBS Virtual Camera\" (video)";
    static char devs[CAM_MAX_DEVICES][CAM_NAME];
    T(cam_parse_devices(list, devs, CAM_MAX_DEVICES) == 3 && !strcmp(devs[0], "Pixel 10 Pro XL (Windows Virtual Camera)") &&
      !strcmp(devs[1], "Iriun Webcam") && !strcmp(devs[2], "OBS Virtual Camera"));
    T(cam_parse_devices(list, devs, 1) == 1);
    T(cam_parse_devices("Error opening input file dummy.", devs, CAM_MAX_DEVICES) == 0);

    static Args a;
    char s[8192];
    cam_args(&a, "Iriun Webcam", true, VENDOR_NVENC);
    wide_to_utf8(a.s, s, sizeof s);
    T(strstr(s, "-f dshow") && strstr(s, "\"video=Iriun Webcam\"") && strstr(s, "print(RTCTIME)") && strstr(s, "h264_nvenc") &&
      strstr(s, "-g 60") && strstr(s, "-segment_time 2") && strstr(s, "-f rawvideo pipe:1") && !strcmp(s + strlen(s) - 13, "-segment_list"));
    cam_args(&a, "Iriun Webcam", false, VENDOR_CPU);
    wide_to_utf8(a.s, s, sizeof s);
    T(strstr(s, "scale=256:144") && strstr(s, "pipe:1") && !strstr(s, "segment") && !strstr(s, "libx264") && !strstr(s, "RTCTIME"));
    cam_args(&a, "Cam", true, VENDOR_CPU);
    wide_to_utf8(a.s, s, sizeof s);
    T(strstr(s, "libx264") && strstr(s, "-sc_threshold 0"));
}

static void test_backup_places()
{
    const char *info = "{\"personal\": {\"path\": \"C:\\\\Users\\\\me\\\\Dropbox\", \"host\": 1, \"is_team\": false},"
                       " \"business\": {\"path\": \"D:\\\\Dropbox (Work)\"}}";
    static char paths[2][MAX_PATH * 3];
    const char *names[2];
    T(backup_dropbox_paths(info, (i64)strlen(info), paths, names) == 2 && !strcmp(paths[0], "C:\\Users\\me\\Dropbox") &&
      !strcmp(names[0], "Dropbox") && !strcmp(paths[1], "D:\\Dropbox (Work)") && !strcmp(names[1], "Dropbox (work)"));
    const char *one = "{\"business\": {\"path\": \"E:\\\\Box\"}}";
    T(backup_dropbox_paths(one, (i64)strlen(one), paths, names) == 1 && !strcmp(names[0], "Dropbox (work)"));
    T(backup_dropbox_paths("{}", 2, paths, names) == 0);

    wchar_t d[MAX_PATH];
    backup_mount_dir(L"g", d, MAX_PATH);
    T(!wcscmp(d, L"G:\\"));
    backup_mount_dir(L"G:", d, MAX_PATH);
    T(!wcscmp(d, L"G:\\"));
    backup_mount_dir(L"G:\\", d, MAX_PATH);
    T(!wcscmp(d, L"G:\\"));
    backup_mount_dir(L"D:\\Google Drive", d, MAX_PATH);
    T(!wcscmp(d, L"D:\\Google Drive"));

    BackupPlace p = {};
    wcscpy_s(p.dir.s, L"G:\\My Drive");
    T(!wcscmp(backup_place_target(p).s, L"G:\\My Drive\\" APP_NAME));
    wcscpy_s(p.dir.s, L"N:\\");
    T(!wcscmp(backup_place_target(p).s, L"N:\\" APP_NAME));
}

static void test_backup()
{
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    Path root = {};
    swprintf(root.s, MAX_PATH, L"%srekodr-test-backup-%u", temp, GetCurrentProcessId());
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
    Path ses = save_sessions_dir(from, 2026), clips = save_clips_dir(from, 2026);
    Path bses = save_sessions_dir(to, 2026), bclips = save_clips_dir(to, 2026);
    put(ses, L"a.mp4", "a", 3600);
    put(ses, L"fresh.mp4", "f", 10); // still being written, maybe
    put(ses, L"b.mp4.part", "p", 3600);
    put(clips, L"c.mp4", "c", 3600);
    put(save_source_dir(from, 2026), L"d.mp4", "d", 3600);
    put(save_data_dir(from), L"a.session.txt", "r", 3600);
    put(bses, L"old.mp4", "o", 3600); // only at the backup: stays

    g_backup.from = from;
    g_backup.to = to;
    g_backup.clips = false;
    g_backup.done = g_backup.total = g_backup.failed = 0;
    g_backup.error[0] = 0;
    InitializeCriticalSection(&g_backup.lock);
    backup_thread(0);
    T(g_backup.state == BACKUP_DONE && g_backup.done == 2 && g_backup.total == 2);
    T(has(path_join(bses, L"a.mp4")) && !has(path_join(bses, L"fresh.mp4")) && !has(path_join(bses, L"b.mp4.part")));
    T(has(path_join(save_data_dir(to), L"a.session.txt")));
    T(!has(path_join(bclips, L"c.mp4")) && has(path_join(bses, L"old.mp4")));

    g_backup.clips = true;
    g_backup.done = g_backup.total = 0;
    backup_thread(0);
    T(g_backup.done == 2 && has(path_join(bclips, L"c.mp4"))); // a.mp4 is there already
    T(has(path_join(save_source_dir(to, 2026), L"d.mp4")));

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
    T(strstr(out, "\"version\":\"" REKODR_VERSION "\"") && strstr(out, "\"clips\":3,\"saves\":1,\"shares\":2,\"links\":4,\"strain\":5}"));
    tel_feedback_json(1, "  it \"works\"\n", out, sizeof out);
    T(!strcmp(out, "{\"version\":\"" REKODR_VERSION "\",\"kind\":\"idea\",\"text\":\"it \\\"works\\\"\"}"));
    static char longer[3000];
    memset(longer, 'a', sizeof longer - 1);
    ((u8 *)longer)[FB_TEXT_MAX - 1] = 0xc3; // a two-byte character across the limit is dropped whole
    ((u8 *)longer)[FB_TEXT_MAX] = 0xa9;
    tel_feedback_json(2, longer, out, sizeof out);
    const char *t = strstr(out, "\"text\":\"") + 8;
    T((i32)(strrchr(out, '"') - t) == FB_TEXT_MAX - 1);
    // Send waits a minute after a send
    T(feedback_wait_left(0, 5000) == 0);                           // nothing sent yet
    T(feedback_wait_left(1000, 1000) == 1);                        // just sent
    T(fabsf(feedback_wait_left(1000, 31000) - 0.5f) < 0.001f);     // half way
    T(feedback_wait_left(1000, 60999) > 0 && feedback_wait_left(1000, 61000) == 0);
    T(feedback_wait_left(5000, 1000) == 0);                        // a clock before the send
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
    str_set(c.hotkeys[1].keys.s, "Alt+F7");
    str_set(c.hotkeys[1].action.s, "save 45s");
    c.hotkey_count = 2;
    T(setup_clip_key(c, keys, sizeof keys, &secs) && !strcmp(keys, "Alt+F7") && secs == 45);
}

static bool test_has_discord_steam(const FirstApp &a) { return strstr(a.where, "Discord") || strstr(a.where, "Valve"); }
static bool test_has_none(const FirstApp &) { return false; }

static void test_ffmpeg_check()
{
    char v[32], why[192];
    T(ffmpeg_version("ffmpeg version 8.1.1-full_build-www.gyan.dev Copyright (c) 2000-2026", v, sizeof v) == 8);
    T(!strcmp(v, "8.1.1"));
    T(ffmpeg_version("ffmpeg version n7.1-12-gabc Copyright", v, sizeof v) == 7 && !strcmp(v, "7.1"));
    T(ffmpeg_version("ffmpeg version N-121000-g1a2b3c4d5e-20260101 Copyright", v, sizeof v) == 0);
    T(!strcmp(v, "N-121000-g1a2b3c4d5e"));
    T(ffmpeg_version("ffmpeg version 2026-01-01-git-abc-full_build Copyright", v, sizeof v) == 0);
    T(ffmpeg_version("not ffmpeg at all", v, sizeof v) == -1);

    // The list lines are flags then the name; a name inside another or in a description doesn't count
    const char *filters_ok = "Filters:\n  T.. = Timeline support\n  ------\n"
                             " .. amix              N->A       Audio mixing.\n"
                             " .. afftdn            A->A       Denoise.\n .. loudnorm A->A x\n"
                             " .S xfade             VV->V      Cross fade.\n .. subtitles V->V x\n .. drawtext V->V x\n"
                             " .. hwupload V->V x\n .. ddagrab           |->V       Desktop Duplication\r\n"
                             " .. gfxcapture        |->V       Capture graphics\r\n";
    const char *encoders = " V....D libx264              libx264 H.264\n A....D aac   AAC\n A....D libopus  Opus\n";
    T(ffmpeg_lists(filters_ok, "gfxcapture") && ffmpeg_lists(filters_ok, "ddagrab"));
    T(!ffmpeg_lists(filters_ok, "mix") && !ffmpeg_lists(filters_ok, "Desktop") && !ffmpeg_lists(encoders, "libx26"));
    const char *ver = "ffmpeg version 8.1.1-full_build-www.gyan.dev Copyright";
    T(ffmpeg_judge(ver, filters_ok, encoders, true, v, sizeof v, why, sizeof why) && !why[0]);
    T(!ffmpeg_judge(ver, filters_ok, encoders, false, v, sizeof v, why, sizeof why) && strstr(why, "ffprobe"));
    T(!ffmpeg_judge("ffmpeg version 7.1 Copyright", filters_ok, encoders, true, v, sizeof v, why, sizeof why));
    T(strstr(why, "too old") && !strcmp(v, "7.1"));
    T(!ffmpeg_judge("", filters_ok, encoders, true, v, sizeof v, why, sizeof why) && strstr(why, "didn't answer"));
    const char *no_wgc = " .. amix N->A x\n .. afftdn A->A x\n .. loudnorm A->A x\n .. xfade VV->V x\n"
                         " .. subtitles V->V x\n .. drawtext V->V x\n .. hwupload V->V x\n .. ddagrab |->V x\n";
    T(!ffmpeg_judge(ver, no_wgc, " A....D aac AAC\n", true, v, sizeof v, why, sizeof why));
    T(!strcmp(why, "it has no gfxcapture, libx264, libopus"));
}

static void test_ffmpeg_tools_ignored()
{
    // ffplay sorted as a Game in Options is still never one, nor are ffmpeg and ffprobe; javaw keeps its category
    Config c = {};
    AppInfo *ffplay = config_app(&c, "ffplay.exe");
    ffplay->category = CAT_GAME;
    config_app(&c, "javaw.exe")->category = CAT_GAME;
    T(app_category(c, *ffplay) == CAT_IGNORED);
    T(app_category_in(c.apps, c.app_count, c.folders, c.folder_count, "ffplay.exe", "") == CAT_IGNORED);
    T(app_category_in(c.apps, c.app_count, c.folders, c.folder_count, "FFmpeg.exe", "") == CAT_IGNORED);
    T(app_category_in(c.apps, c.app_count, c.folders, c.folder_count, "ffprobe.exe", "") == CAT_IGNORED);
    T(app_category_in(c.apps, c.app_count, c.folders, c.folder_count, "javaw.exe", "") == CAT_GAME);
}

static void test_first_apps()
{
    // Only what is installed is added, with its category and name
    Config c = {};
    config_first_apps(&c, test_has_none);
    T(c.app_count == 0);
    config_first_apps(&c, test_has_discord_steam);
    T(c.app_count == 3);
    const AppInfo *discord = 0, *steam = 0, *helper = 0;
    for (i32 i = 0; i < c.app_count; i++) {
        if (!strcmp(c.apps[i].exe.s, "discord.exe")) discord = &c.apps[i];
        if (!strcmp(c.apps[i].exe.s, "steam.exe")) steam = &c.apps[i];
        if (!strcmp(c.apps[i].exe.s, "steamwebhelper.exe")) helper = &c.apps[i];
    }
    T(discord && discord->category == CAT_COMMUNICATION && !strcmp(discord->name.s, "Discord"));
    T(steam && steam->category == CAT_IGNORED && helper && helper->category == CAT_IGNORED);
    // An exe named like Discord's is New like any other
    T(app_category_in(c.apps, c.app_count, c.folders, c.folder_count, "discord-sam-1234.exe", "") == CAT_NONE);

    // The list is only Chat and Ignored: an app that isn't listed is sorted by the player
    for (i32 i = 0; i < COUNT(FIRST_APPS); i++)
        T(FIRST_APPS[i].category == CAT_COMMUNICATION || FIRST_APPS[i].category == CAT_IGNORED);

    // Every entry: a lower-case exe, listed once, with at least one place to look
    for (i32 i = 0; i < COUNT(FIRST_APPS); i++) {
        const FirstApp &f = FIRST_APPS[i];
        bool lower = true;
        for (const char *p = f.exe; *p; p++) lower &= !(*p >= 'A' && *p <= 'Z');
        size_t n = strlen(f.exe);
        T(lower && n > 4 && !strcmp(f.exe + n - 4, ".exe") && f.name[0] && f.where[0]);
        for (i32 k = 0; k < i; k++) T(strcmp(FIRST_APPS[k].exe, f.exe) != 0);
    }

    // The places: a file after expanding variables, a Store package, a registry value
    wchar_t temp[MAX_PATH], file[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    swprintf(file, MAX_PATH, L"%srekodr_first_app.exe", temp);
    DeleteFileW(file);
    SetEnvironmentVariableW(L"REKODR_TEST_DIR", temp);
    FirstApp probe = {"rekodr_first_app.exe", "Probe", CAT_APP, "%REKODR_TEST_DIR%\\rekodr_first_app.exe"};
    T(!first_app_installed(probe));
    FILE *f = _wfopen(file, L"wb");
    T(f != 0);
    if (f) fclose(f);
    T(first_app_installed(probe));
    DeleteFileW(file);
    T(!first_app_installed(probe));
    T(!first_app_place("%REKODR_NO_SUCH_VAR%\\x.exe")); // an unknown variable is never a match
    T(!first_app_place("pkg:Rekodr.NoSuchPackage_0000000000000"));
    T(!first_app_place("reg:Software\\Rekodr\\NoSuchKey|Path"));
    T(first_app_place("%WINDIR%\\explorer.exe"));
    SetEnvironmentVariableW(L"REKODR_TEST_DIR", 0);
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
    for (i32 i = TOAST_CLIP_DEFAULT; i <= TOAST_CLIP_OFF; i++) T(toast_clip_parse(toast_clip_name((ToastClip)i)) == i);
    T(toast_clip_parse("nonsense") == TOAST_CLIP_DEFAULT);
}

// The monitor and corner the toasts pick, in games too; from a top corner they stack downward. Small is small only
// on the game's monitor.
static void test_popup_place()
{
    RECT work = {0, 0, 1920, 1040};
    POINT a = toast_spot(work, CORNER_TR, 400, 100, 16, 0, 0);
    T(a.x == 1920 - 400 - 16 && a.y == 16);
    RECT up[] = {{a.x, a.y, a.x + 400, a.y + 100}};
    POINT b = toast_spot(work, CORNER_TR, 400, 80, 16, up, 1);
    T(b.x == a.x && b.y == 16 + 100 + 16);
    RECT two[] = {{b.x, b.y, b.x + 400, b.y + 80}, up[0]}; // either order
    POINT c = toast_spot(work, CORNER_TR, 400, 60, 16, two, 2);
    T(c.y == b.y + 80 + 16);
    POINT d = toast_spot(work, CORNER_TL, 400, 100, 16, up, 1); // the other side: not in its way
    T(d.x == 16 && d.y == 16);
    POINT e = toast_spot(work, CORNER_BR, 400, 100, 16, 0, 0);
    T(e.x == a.x && e.y == 1040 - 16 - 100);
    RECT low[] = {{e.x, e.y, e.x + 400, e.y + 100}};
    POINT f = toast_spot(work, CORNER_BR, 400, 50, 16, low, 1);
    T(f.y == e.y - 16 - 50);
    POINT g = toast_spot(work, CORNER_BL, 300, 50, 16, 0, 0);
    T(g.x == 16 && g.y == 1040 - 16 - 50);

    // The monitor picked: fake monitors 1, 2 and 3 (left to right), outputs 0 to 2 on them
    static Config pc;
    pc = {};
    pc.popup_monitor = POPUP_RECORDED;
    pc.output_idx = 1;
    pc.popup_corner = CORNER_TR;
    pc.popup_monitor_game = 2;
    pc.popup_corner_game = CORNER_BL;
    HMONITOR (*outputs)(i32) = [](i32 out) -> HMONITOR {
        return out >= 0 && out < 3 ? (HMONITOR)(intptr_t)(out + 1) : 0;
    };
    HMONITOR (*no_outputs)(i32) = [](i32) -> HMONITOR { return 0; };
    PopupCorner pcorner;
    T(popup_monitor_for(pc, false, (HMONITOR)3, outputs, &pcorner) == (HMONITOR)2 && pcorner == CORNER_TR);
    T(popup_monitor_for(pc, true, (HMONITOR)1, outputs, &pcorner) == (HMONITOR)3 && pcorner == CORNER_BL);
    pc.popup_monitor_game = POPUP_RECORDED;
    T(popup_monitor_for(pc, true, (HMONITOR)1, outputs, &pcorner) == (HMONITOR)2 && pcorner == CORNER_BL);
    pc.popup_monitor_game = 5; // an output that's gone: the game's monitor
    HMONITOR gone = popup_monitor_for(pc, true, (HMONITOR)1, outputs, &pcorner);
    T(gone == (HMONITOR)1 && toast_clip_on(TOAST_CLIP_SMALL, (u64)gone, 1) == TOAST_CLIP_SMALL);
    T(popup_monitor_for(pc, true, (HMONITOR)1, no_outputs, &pcorner) == (HMONITOR)1); // none: the game's too
    pc.popup_monitor_game = POPUP_AS_NORMAL;
    T(popup_monitor_for(pc, true, (HMONITOR)3, outputs, &pcorner) == (HMONITOR)2 && pcorner == CORNER_TR);

    T(toast_clip_on(TOAST_CLIP_SMALL, 1, 1) == TOAST_CLIP_SMALL); // game on 1, popups on 1
    T(toast_clip_on(TOAST_CLIP_SMALL, 2, 1) == TOAST_CLIP_FULL);  // popups on 2
    T(toast_clip_on(TOAST_CLIP_OFF, 2, 1) == TOAST_CLIP_OFF && toast_clip_on(TOAST_CLIP_FULL, 1, 1) == TOAST_CLIP_FULL);
}

// Clip saved over a game: the game's own choice, else the one for all games.
static void test_toast_clip()
{
    static Config c;
    c = {};
    T(toast_clip_for(c, "") == TOAST_CLIP_FULL); // nothing set: the whole popup
    c.toast_clip = TOAST_CLIP_SMALL;
    T(toast_clip_for(c, "") == TOAST_CLIP_SMALL && toast_clip_for(c, 0) == TOAST_CLIP_SMALL);
    AppInfo *same = config_app(&c, "same.exe"), *off = config_app(&c, "off.exe"), *full = config_app(&c, "full.exe");
    off->toast_clip = TOAST_CLIP_OFF;
    full->toast_clip = TOAST_CLIP_FULL;
    T(same->toast_clip == TOAST_CLIP_DEFAULT && toast_clip_for(c, "same.exe") == TOAST_CLIP_SMALL);
    T(toast_clip_for(c, "off.exe") == TOAST_CLIP_OFF && toast_clip_for(c, "full.exe") == TOAST_CLIP_FULL);
    T(toast_clip_for(c, "unknown.exe") == TOAST_CLIP_SMALL);
    c.toast_clip = TOAST_CLIP_OFF;
    T(toast_clip_for(c, "same.exe") == TOAST_CLIP_OFF && toast_clip_for(c, "full.exe") == TOAST_CLIP_FULL);
}

// An unsaved session is new until session_seen, and stays seen through sessions.txt.
static void test_sessions_seen()
{
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    Paths keep = g_app.paths;
    Path buffer = g_app.config.buffer_dir;
    swprintf(g_app.paths.local_dir.s, MAX_PATH, L"%srekodr-tests-ses-%lu", temp, GetCurrentProcessId());
    CreateDirectoryW(g_app.paths.local_dir.s, 0);
    g_app.config.buffer_dir = g_app.paths.local_dir; // no activity.tsv: only the file comes back
    Sessions &ss = g_app.sessions;
    ss = {};
    ss.loaded = true;
    SessionState states[] = {SES_NOT_SAVED, SES_ASKING, SES_SAVED, SES_NOT_SAVED};
    for (i32 i = 0; i < COUNT(states); i++) {
        Session &s = ss.items[i];
        s.id = 1 + i;
        s.state = states[i];
        s.start_us = 1000000ll * (i + 1);
        s.end_us = s.start_us + 600000000;
        snprintf(s.name, sizeof s.name, "game %d", i);
    }
    ss.next_id = 4;
    T(sessions_unseen_count() == 3); // the saved one isn't in the list
    session_seen(1);
    session_seen(1); // once is enough
    session_seen(99); // no such session
    T(sessions_unseen_count() == 2 && ss.items[0].seen && ss.dirty);
    session_seen(2);
    sessions_stop(); // writes sessions.txt and waits for it
    ss = {};
    sessions_load();
    i32 seen = 0, unseen = 0;
    for (const Session &s : ss.items) {
        if (s.state == SES_FREE) continue;
        if (s.seen) seen++;
        else unseen++;
        T(s.seen == (s.id == 1 || s.id == 2));
    }
    T(seen == 2 && unseen == 2 && sessions_unseen_count() == 1);
    DeleteFileW(path_join(g_app.paths.local_dir, L"sessions.txt").s);
    RemoveDirectoryW(g_app.paths.local_dir.s);
    ss = {};
    g_app.paths = keep;
    g_app.config.buffer_dir = buffer;
}

static void test_hotkeys()
{
    HotkeyKeys k;
    T(hotkey_parse("Alt+F7", &k) && k.mods == (MOD_NOREPEAT | MOD_ALT) && k.vk == VK_F7 && !k.pad);
    T(hotkey_parse("ctrl + shift + s", &k) && k.mods == (MOD_NOREPEAT | MOD_CONTROL | MOD_SHIFT) && k.vk == 'S');
    T(hotkey_parse("Win+Num5", &k) && k.vk == VK_NUMPAD5);
    T(hotkey_parse("F24", &k) && k.vk == VK_F24);
    T(!hotkey_parse("F25", &k));
    T(!hotkey_parse("Ctrl", &k));     // no key
    T(!hotkey_parse("Ctrl+A+B", &k)); // two keys
    T(!hotkey_parse("Alt+Nope", &k));
    T(!hotkey_parse("", &k));

    // Mouse buttons, with modifiers like a key
    T(hotkey_parse("Mouse4", &k) && k.vk == VK_XBUTTON1 && k.mods == MOD_NOREPEAT && hotkey_is_mouse(k));
    T(hotkey_parse("Ctrl+Mouse5", &k) && k.vk == VK_XBUTTON2 && k.mods == (MOD_NOREPEAT | MOD_CONTROL));
    T(hotkey_parse("MouseMiddle", &k) && k.vk == VK_MBUTTON);
    T(!hotkey_parse("Mouse4+A", &k));
    T(hotkey_parse("Alt+F7", &k) && !hotkey_is_mouse(k));

    // Controller buttons, alone or held together, never with keys
    T(hotkey_parse("PadA", &k) && k.pad == XINPUT_GAMEPAD_A && !k.vk && !hotkey_is_mouse(k));
    T(hotkey_parse("PadLB+PadRT", &k) && k.pad == (XINPUT_GAMEPAD_LEFT_SHOULDER | PAD_RT));
    T(!hotkey_parse("Ctrl+PadA", &k));
    T(!hotkey_parse("PadA+F7", &k));
    T(!hotkey_parse("PadZ", &k));
    char name[64];
    hotkey_pad_name(XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_BACK | PAD_LT, name, sizeof name);
    T(!strcmp(name, "PadA+PadLT+PadBack") && hotkey_parse(name, &k) &&
      k.pad == (XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_BACK | PAD_LT));

    // A controller hotkey fires once, when its last button goes down
    u32 lb = XINPUT_GAMEPAD_LEFT_SHOULDER, a = XINPUT_GAMEPAD_A, mask = lb | a;
    T(!hotkey_pad_fired(0, lb, mask));
    T(hotkey_pad_fired(lb, lb | a, mask));
    T(hotkey_pad_fired(0, lb | a | XINPUT_GAMEPAD_B, mask)); // other buttons held too
    T(!hotkey_pad_fired(lb | a, lb | a, mask));               // held: once
    T(!hotkey_pad_fired(lb | a, lb, mask));
    T(!hotkey_pad_fired(0, a, 0));

    // Tap or hold: let go first and it's a tap; still down at HOTKEY_HOLD_MS and it's a hold
    T(hotkey_hold_verdict(true, 0) == HOLD_WAIT);
    T(hotkey_hold_verdict(true, HOTKEY_HOLD_MS - 1) == HOLD_WAIT);
    T(hotkey_hold_verdict(false, 100) == HOLD_TAP);
    T(hotkey_hold_verdict(true, HOTKEY_HOLD_MS) == HOLD_HELD);
    T(hotkey_hold_verdict(false, HOTKEY_HOLD_MS + 500) == HOLD_TAP); // released before the check saw it held

    // The tray's Save clip: the first save hotkey's length, else 30 s
    static Config c;
    char action[64];
    wchar_t label[64];
    c.hotkey_count = 0;
    tray_save(c, action, sizeof action, label, COUNT(label));
    T(!strcmp(action, "save 30s") && !wcscmp(label, L"Save clip (30 s)"));
    c.hotkey_count = 2;
    str_set(c.hotkeys[0].action.s, "open");
    str_set(c.hotkeys[1].action.s, "save 2m");
    tray_save(c, action, sizeof action, label, COUNT(label));
    T(!strcmp(action, "save 2m") && !wcscmp(label, L"Save clip (2 min)"));
    // The default hotkey isn't any other clip tool's
    config_defaults(&c, g_app.paths);
    T(c.hotkey_count == 1 && !strcmp(c.hotkeys[0].keys.s, "Alt+F6"));
    // A game's own hotkey works only in it and wins there over an every-game one on the same keys
    c.hotkey_count = 3;
    str_set(c.hotkeys[1].keys.s, "alt+f6");
    str_set(c.hotkeys[1].game.s, "game.exe");
    str_set(c.hotkeys[2].keys.s, "Alt+F8");
    str_set(c.hotkeys[2].game.s, "other.exe");
    T(hotkey_active(c, 0, "") && hotkey_active(c, 0, "notepad.exe") && !hotkey_active(c, 0, "game.exe"));
    T(hotkey_active(c, 1, "game.exe") && !hotkey_active(c, 1, "") && !hotkey_active(c, 1, "other.exe"));
    T(hotkey_active(c, 2, "other.exe") && !hotkey_active(c, 2, "game.exe"));
    T(hotkey_active(c, 0, "other.exe")); // other.exe's own is on different keys
    // A game's own Mic choice wins over the default while it's played
    c.mic = true;
    c.app_count = 1;
    str_set(c.apps[0].exe.s, "game.exe");
    c.apps[0].mic = -1;
    T(!mic_for(c, "game.exe") && mic_for(c, "") && mic_for(c, "other.exe"));
    c.apps[0].mic = 0;
    T(mic_for(c, "game.exe"));
    c.mic = false;
    c.apps[0].mic = 1;
    T(mic_for(c, "game.exe") && !mic_for(c, ""));
}

// What's new: versions compare as numbers, and the notes after a version are its newer ones only
static void test_news()
{
    T(version_cmp("0.2.0", "0.2.0") == 0 && version_cmp("0.2", "0.2.0") == 0);
    T(version_cmp("0.10.0", "0.9.9") > 0 && version_cmp("0.2.0", "0.2.1") < 0 && version_cmp("1.0", "0.99.99") > 0);
    static const char notes[] = "About the file\r\n\r\n# 0.3.0\r\nThree a  \r\n\r\nThree b\r\n#0.2.1\nTwo\n# 0.1.0\nOne";
    NewsLine l[8];
    i32 n = news_parse(notes, (i32)strlen(notes), "", l, 8);
    T(n == 4 && !strcmp(l[0].ver, "0.3.0") && l[0].len == 7 && !memcmp(l[0].text, "Three a", 7));
    T(!strcmp(l[2].ver, "0.2.1") && l[2].len == 3 && !strcmp(l[3].ver, "0.1.0") && l[3].len == 3);
    n = news_parse(notes, (i32)strlen(notes), "0.2.0", l, 8);
    T(n == 3 && !strcmp(l[2].ver, "0.2.1"));
    T(news_parse(notes, (i32)strlen(notes), "0.3.0", l, 8) == 0);
    T(news_parse(notes, (i32)strlen(notes), "", l, 2) == 2);
}

// The quality warning: what's too much for the encoder, the monitor or the disk, and what keeps up
static void test_quality()
{
    Config c = {};
    str_set(c.codec.s, "h264");
    str_set(c.preset.s, "p4");
    c.height = 1080;
    c.fps = 60;
    c.bitrate_mbps = 50;
    c.desktop_bitrate_mbps = 10;
    QualityHw hw = {1440, 144, 16, VENDOR_NVENC, false};
    QualityWarn w;
    quality_check(c, hw, &w);
    for (i32 i = 0; i < Q_COUNT; i++) T(!w.why[i][0]); // a usual setup on a GPU: nothing
    // A 4K monitor at 120 on the GPU: too much; 1440p keeps up at 120, 60 fps at 4K
    c.height = 0;
    c.fps = 120;
    hw.mon_h = 2160;
    quality_check(c, hw, &w);
    T(strstr(w.why[Q_RES], "1440p keeps up") && strstr(w.why[Q_FPS], "60 fps keeps up") && !w.why[Q_ENCODER][0]);
    // Above the monitor's refresh: said once, and checked at the refresh
    c.height = 1080;
    c.fps = 240;
    quality_check(c, hw, &w);
    T(strstr(w.why[Q_FPS], "144 Hz") && !w.why[Q_RES][0]);
    // The CPU with 6 threads at 1080p60: names the CPU and the encoder row
    c.fps = 60;
    hw.vendor = VENDOR_CPU;
    hw.threads = 6;
    quality_check(c, hw, &w);
    T(strstr(w.why[Q_RES], "the CPU") && strstr(w.why[Q_RES], "720p keeps up") && w.why[Q_ENCODER][0]);
    // A slow preset at 4K60 on the GPU: the preset is named
    hw = {2160, 60, 16, VENDOR_NVENC, false};
    c.height = 0;
    str_set(c.preset.s, "p7");
    quality_check(c, hw, &w);
    T(strstr(w.why[Q_PRESET], "P5") && w.why[Q_RES][0]);
    // A hard disk: only the bitrate over its limit
    str_set(c.preset.s, "p4");
    c.bitrate_mbps = 200;
    hw.hdd = true;
    quality_check(c, hw, &w);
    T(w.why[Q_BITRATE][0] && !w.why[Q_DESKTOP_BITRATE][0] && !w.why[Q_RES][0]);
    // Unknown encoder: no pixel-rate guess
    hw = {2160, 240, 2, -1, false, -1};
    c.fps = 240;
    quality_check(c, hw, &w);
    T(!w.why[Q_RES][0] && !w.why[Q_FPS][0]);

    // Measured: under a minute of play says nothing; then lag or drops, whichever lost more, from the last 10 min
    g_pace_count = g_pace_next = 0;
    u64 t = 100000000;
    quality_pace(t, "10000|9900|594|0");
    T(quality_lost(t) == -1);
    for (i32 i = 1; i < 6; i++) quality_pace(t + i * 10000, "10000|9900|594|0");
    T(quality_lost(t + 50000) == 1);
    quality_pace(t + 60000, "10000|10000|500|100"); // 100 dropped: 2.4% of all frames, more than the lag
    T(quality_lost(t + 60000) == 2);
    T(quality_lost(t + 60000 + QUALITY_SPAN_MS) == -1); // all older than 10 min
    quality_pace(t, "bad");
    T(g_pace_count == 7);
    for (i32 i = 0; i < 100; i++) quality_pace(t + i * 10000, "10000|9000|540|0"); // the ring keeps the newest
    T(g_pace_count == COUNT(g_pace) && quality_lost(t + 990000) == 10);
    hw.lost_pct = 10;
    c.fps = 60;
    quality_check(c, hw, &w);
    T(!strcmp(w.why[Q_FPS], "Lost 10% of frames in the last 10 min of play"));
    hw.lost_pct = 2;
    quality_check(c, hw, &w);
    T(!w.why[Q_FPS][0]);
    g_pace_count = g_pace_next = 0;
}

// The Mic is cleaned before the Mix takes it; Mic (raw) is the Mic as recorded, on its own, in no Mix, and left out
// of the editor's tracks.
static void test_mic_clean()
{
    static AudioStreamInfo st[2];
    static AudioSeg segs[2];
    st[0] = {}, st[1] = {};
    st[0].mic = true, str_set(st[0].exe, "mic"), st[0].first_seg = 0, st[0].seg_count = 1;
    str_set(st[1].exe, "game.exe"), st[1].first_seg = 1, st[1].seg_count = 1;
    segs[0] = {}, segs[1] = {};
    segs[0].end_us = segs[1].end_us = 10000000;
    SaveWork *w = &g_save_work;
    w->buf = {};
    w->buf.streams = st, w->buf.stream_count = 2;
    w->buf.audio_segs = segs, w->buf.audio_seg_count = 2;
    w->cfg = {};
    w->cfg.voice_mono = true;
    w->stream_count = 2;
    for (i32 i = 0; i < 2; i++) w->streams[i] = i, w->stream_segs[i][0] = 0, w->stream_segs[i][1] = 1;
    w->pieces[0] = {0, 10000000, 0};
    w->piece_count = 1;
    w->game_count = 0;

    w->cfg.mic_clean = true, w->cfg.mic_raw = true;
    make_tracks(w);
    T(w->track_count == 3 && !strcmp(w->tracks[1].title, MIC_RAW_TITLE) && w->tracks[1].raw && w->tracks[1].mono);
    T(w->stream_track[0] == 0 && w->stream_track[1] == 2);
    make_graph(w, 10000000, false);
    T(strstr(w->graph, "[u0]asplit=2[v0][r0];\n[v0]highpass=f=80,afftdn=") != 0);
    T(strstr(w->graph, "[r0]pan=mono|c0=0.5*c0+0.5*c1[o1]") != 0);
    T(strstr(w->graph, "[m0][m2]amix=inputs=2:") != 0 && strstr(w->graph, "[m1]") == 0);
    make_graph(w, 10000000, true); // a preview: the Mix only, nothing left unused
    T(strstr(w->graph, "[r0]") == 0 && strstr(w->graph, "[u0]highpass") != 0 && strstr(w->graph, "[m0][m2]amix") != 0);

    w->cfg.mic_raw = false;
    make_tracks(w);
    T(w->track_count == 2 && w->stream_track[1] == 1);
    make_graph(w, 10000000, false);
    T(strstr(w->graph, "[u0]highpass=f=80,afftdn=") != 0 && strstr(w->graph, "[r0]") == 0);

    w->cfg.mic_clean = false, w->cfg.mic_raw = true; // a raw track only beside a cleaned one
    make_tracks(w);
    T(w->track_count == 2);
    make_graph(w, 10000000, false);
    T(strstr(w->graph, "afftdn") == 0 && strstr(w->graph, "[u0]") == 0);

    w->cfg.mic_clean = true; // no Mic heard: nothing to clean, no raw track
    st[0].mic = false, str_set(st[0].exe, "app.exe");
    make_tracks(w);
    T(w->track_count == 3 && !w->tracks[1].raw);
    make_graph(w, 10000000, false);
    T(strstr(w->graph, "afftdn") == 0);
    w->buf = {};

    static MediaInfo m;
    m = {};
    m.track_count = 4;
    str_set(m.tracks[0], "Mix"), str_set(m.tracks[1], "Mic"), str_set(m.tracks[2], MIC_RAW_TITLE);
    str_set(m.tracks[3], "Game (game)");
    i32 tracks[MEDIA_MAX_TRACKS];
    T(ed_tracks(m, tracks, MEDIA_MAX_TRACKS) == 2 && tracks[0] == 1 && tracks[1] == 3);
    T(track_is_spare(m, 2) && !track_is_spare(m, 1));
}

// What is saved comes back the same.
static void test_settings_round_trip()
{
    static Paths paths;
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    swprintf(paths.config_dir.s, MAX_PATH, L"%srekodr-tests-%lu", temp, GetCurrentProcessId());
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
    a.accent = 0x0a1b2c;         // a colour with leading zeros
    a.timeline_top = 0.4f;
    a.timeline_names = 220;
    a.editor_top = 0.6f;
    a.editor_lane = 60;
    a.editor_zoom_view = 1;
    str_set(a.codec.s, "hevc");
    a.hotkey_count = 1;
    str_set(a.hotkeys[0].keys.s, "Alt+F7");
    str_set(a.hotkeys[0].action.s, "save 30s");
    a.app_count = 0;
    AppInfo *app = config_app(&a, "game.exe");
    str_set(app->name.s, "The Game");
    app->category = CAT_GAME;
    app->session_mode = SESSION_ALWAYS;
    app->rec.fps = 60;
    app->rec.window = -1;
    app->toast_clip = TOAST_CLIP_OFF;
    config_app(&a, "other.exe")->category = CAT_GAME; // same as all games
    a.toast_clip = TOAST_CLIP_SMALL;
    a.popup_corner = CORNER_TL;
    a.popup_monitor_game = 1;
    a.popup_corner_game = CORNER_BL;
    a.window_capture = true;
    str_set(app->path, "C:\\Games\\The Game\\game.exe");
    a.folder_count = 1;
    str_set(a.folders[0].path, "C:\\V\\*\\off");
    a.folders[0].category = CAT_IGNORED;
    a.input_count = 2;
    a.inputs[0] = {"{0.0.1.00000000}.{a}", "Line (RODE AI-1)", INPUT_STEREO};
    a.inputs[1] = {"{0.0.1.00000000}.{b}", "Off one", INPUT_OFF}; // off isn't saved

    str_set(a.link_uploaders, "mine"); // one uploader on
    T(config_save(a, paths));
    T(config_load(&b, paths));
    T(!strcmp(b.link_uploaders, "mine"));
    {
        // An empty bucket name reads as the default
        static Config e;
        config_defaults(&e, paths);
        char empty[] = "";
        config_set(&e, "r2_bucket", empty);
        T(!strcmp(e.r2_bucket, "rekodr"));
        // The accent: six hex digits, taken as their hue; anything else keeps the default
        T(e.accent == ACCENT_DEFAULT && accent_lock(ACCENT_DEFAULT) == ACCENT_DEFAULT && accent_at_step(0) == ACCENT_DEFAULT);
        bool steady = true, same_light = true;
        f64 teal[3];
        rgb_oklab(ACCENT_DEFAULT, teal);
        for (i32 k = 0; k < ACCENT_HUES; k++) {
            u32 v = accent_at_step(k);
            f64 lab[3];
            rgb_oklab(v, lab);
            steady = steady && accent_lock(v) == v && accent_step(v) == k;
            same_light = same_light && fabs(lab[0] - teal[0]) < 0.01;
        }
        T(steady && same_light);
        T(accent_lock(0xff0000) == accent_lock(0x400000) && accent_lock(0x0000ff) != accent_lock(0xff0000));
        char acc[16];
        static const char *const bad[] = {"0", "+12345", "12345g", "1234567", " 12345", ""};
        for (const char *v : bad) {
            str_set(acc, v);
            config_set(&e, "accent", acc);
            T(e.accent == ACCENT_DEFAULT);
        }
        str_set(acc, "FFe066");
        config_set(&e, "accent", acc);
        T(e.accent == accent_lock(0xffe066) && e.accent != 0xffe066);
    }
    T(b.accent == accent_lock(0x0a1b2c));
    T(b.bitrate_mbps == 42.5f && b.fps == 120 && b.buffer_by_size && b.dev_log && !strcmp(b.codec.s, "hevc"));
    T(b.hotkey_count == 1 && !strcmp(b.hotkeys[0].keys.s, "Alt+F7") && !strcmp(b.hotkeys[0].action.s, "save 30s"));
    {
        // Every hotkey removed stays removed: the default one doesn't come back
        static Config none, back;
        none = a;
        none.hotkey_count = 0;
        T(config_save(none, paths) && config_load(&back, paths) && back.hotkey_count == 0);
        T(config_save(a, paths) && config_load(&back, paths) && back.hotkey_count == 1); // and one comes back as one
        T(!back.hotkeys[0].hold.s[0]);
        // A hold action comes back with its hotkey
        none = a;
        str_set(none.hotkeys[0].hold.s, "save 3m");
        T(config_save(none, paths) && config_load(&back, paths) && back.hotkey_count == 1 &&
          !strcmp(back.hotkeys[0].action.s, "save 30s") && !strcmp(back.hotkeys[0].hold.s, "save 3m"));
        // A hotkey for one game comes back for it, with or without a hold action, and so does a game's Mic
        str_set(none.hotkeys[0].game.s, "game.exe");
        T(config_save(none, paths) && config_load(&back, paths) && !strcmp(back.hotkeys[0].game.s, "game.exe") &&
          !strcmp(back.hotkeys[0].hold.s, "save 3m"));
        none.hotkeys[0].hold = {};
        none.apps[0].mic = -1;
        none.apps[1].mic = 1;
        T(config_save(none, paths) && config_load(&back, paths) && !strcmp(back.hotkeys[0].game.s, "game.exe") &&
          !back.hotkeys[0].hold.s[0] && !strcmp(back.hotkeys[0].action.s, "save 30s"));
        T(back.app_count == 2 && back.apps[0].mic == -1 && back.apps[1].mic == 1);
        // Other apps' folders: the ones saved come back in place of those found on the PC, and none stays none
        none = a;
        none.watch_count = 2;
        wcscpy_s(none.watch_dirs[0].s, L"D:\\Videos\\NVIDIA");
        wcscpy_s(none.watch_dirs[1].s, L"E:\\");
        T(config_save(none, paths) && config_load(&back, paths) && back.watch_count == 2 &&
          !wcscmp(back.watch_dirs[0].s, L"D:\\Videos\\NVIDIA") && !wcscmp(back.watch_dirs[1].s, L"E:\\"));
        none.watch_count = 0;
        T(config_save(none, paths) && config_load(&back, paths) && back.watch_count == 0);
        // Discord's folder comes back, and none stays none
        wcscpy_s(none.discord_dir.s, L"D:\\Discord clips");
        T(config_save(none, paths) && config_load(&back, paths) && !wcscmp(back.discord_dir.s, L"D:\\Discord clips"));
        none.discord_dir = {};
        T(config_save(none, paths) && config_load(&back, paths) && !back.discord_dir.s[0]);
        // The version whose notes were seen comes back
        str_set(none.seen_version, "0.3.1");
        T(config_save(none, paths) && config_load(&back, paths) && !strcmp(back.seen_version, "0.3.1"));
        T(config_save(a, paths));
    }
    T(b.app_count == 2 && b.toast_clip == TOAST_CLIP_SMALL);
    T(b.popup_corner == CORNER_TL && b.popup_monitor_game == 1 && b.popup_corner_game == CORNER_BL);
    for (i32 i = CORNER_BR; i <= CORNER_TL; i++) T(corner_parse(corner_name((PopupCorner)i)) == i);
    T(b.apps[0].toast_clip == TOAST_CLIP_OFF && b.apps[1].toast_clip == TOAST_CLIP_DEFAULT);
    if (b.app_count == 2) {
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
// Playable copies (render.cpp): where a rendered clip's files are, what the copies come to, when one is made or put
// back, and the swap into Clips and back on real files.
static void test_render()
{
    LibItem items[3] = {};
    items[0].rendered = true;
    items[0].render_size = 1000;
    items[1].render_size = 0;
    items[2].rendered = true; // its render not in Clips: nothing to free
    i64 bytes = -1;
    T(render_copies_in(items, 3, &bytes) == 1 && bytes == 1000);
    T(render_copies_in(items, 0, &bytes) == 0 && bytes == 0);

    T(render_need(true, false, true, false) == RENDER_MAKE);
    T(render_need(true, true, true, false) == RENDER_MAKE);   // the edit changed
    T(render_need(true, true, true, true) == RENDER_NOTHING);
    T(render_need(true, true, false, false) == RENDER_RESTORE); // reset to nothing
    T(render_need(false, true, true, true) == RENDER_RESTORE);  // rendering off
    T(render_need(false, false, true, false) == RENDER_NOTHING);
    T(render_need(true, false, false, false) == RENDER_NOTHING);

    T(render_settled(false, false, 0, 1));
    T(!render_settled(true, false, 0, RENDER_SETTLE_US * 10)); // the editor holds the original in Clips
    T(!render_settled(true, true, 0, RENDER_SETTLE_US - 1));
    T(render_settled(true, true, 0, RENDER_SETTLE_US));

    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    Path keep = g_app.config.save_dir;
    swprintf(g_app.config.save_dir.s, MAX_PATH, L"%srekodr-test-render-%u", temp, GetCurrentProcessId());
    Path root = g_app.config.save_dir;
    Path clips = path_join(path_join(root, L"Clips"), L"2026"), data = path_join(root, L"Data");
    Path source = path_join(clips, L"Source");
    LibItem it = {};
    str_set(it.name, "Clip 1");
    str_set(it.ext, ".mp4");
    it.year = 2026;
    it.kind = LIB_CLIPS;
    T(!wcscmp(lib_item_path(it).s, path_join(clips, L"Clip 1.mp4").s));
    T(!wcscmp(lib_playable_path(it).s, lib_item_path(it).s));
    T(!wcscmp(lib_render_note_path(it).s, path_join(data, L"Clip 1.render.txt").s));
    LibItem done = it;
    done.rendered = true;
    T(!wcscmp(lib_item_path(done).s, path_join(source, L"Clip 1.mp4").s));
    T(!wcscmp(lib_playable_path(done).s, path_join(clips, L"Clip 1.mp4").s));

    auto put = [](const Path &p, const char *text) {
        Path dir = p;
        *wcsrchr(dir.s, L'\\') = 0;
        SHCreateDirectoryExW(0, dir.s, 0);
        FILE *f = _wfopen(p.s, L"wb");
        if (f) { fputs(text, f); fclose(f); }
    };
    auto text = [](const Path &p, char *out, i32 cap) {
        out[0] = 0;
        FILE *f = _wfopen(p.s, L"rb");
        if (!f) return false;
        size_t n = fread(out, 1, cap - 1, f);
        out[n] = 0;
        fclose(f);
        return true;
    };
    char got[64];
    Path clip = path_join(clips, L"Clip 1.mp4"), orig = path_join(source, L"Clip 1.mp4");
    Path made = path_join(render_dir(), L"Clip 1.mp4");
    put(clip, "original");
    put(made, "render 1");
    T(render_swap(it, made));
    T(text(clip, got, sizeof got) && !strcmp(got, "render 1"));
    T(text(orig, got, sizeof got) && !strcmp(got, "original"));
    T(!file_exists(made.s));
    put(made, "render 2"); // the edit changed: the render is replaced, the original stays
    T(render_swap(done, made));
    T(text(clip, got, sizeof got) && !strcmp(got, "render 2"));
    T(text(orig, got, sizeof got) && !strcmp(got, "original"));
    put(made, "render 3");
    T(!render_swap(it, made)); // as if not rendered while an original is in Source: left alone
    T(text(orig, got, sizeof got) && !strcmp(got, "original"));
    DeleteFileW(made.s);

    RenderNote note = {123456789, 0xabcdef0123456789ull}, back;
    render_note_write(done, note);
    T(render_note_read(done, &back) && back.edit_mtime_us == note.edit_mtime_us && back.edit_hash == note.edit_hash);
    put(lib_edit_path(done), "edit");
    Path files[LIB_ITEM_FILES];
    T(lib_item_files(done, files) == 4 && !wcscmp(files[0].s, orig.s) && !wcscmp(files[1].s, clip.s));
    i64 mtime = 0;
    u64 h1 = 0, h2 = 0;
    T(render_edit_stamp(done, &mtime, &h1) && mtime > 0);
    put(lib_edit_path(done), "edit 2");
    T(render_edit_stamp(done, &mtime, &h2) && h1 != h2);

    T(render_restore(done));
    T(text(clip, got, sizeof got) && !strcmp(got, "original"));
    T(!file_exists(orig.s) && !file_exists(lib_render_note_path(done).s));
    T(render_restore(done)); // nothing left to put back
    T(lib_item_files(it, files) == 2); // the clip and its edit

    DeleteFileW(clip.s);
    DeleteFileW(lib_edit_path(it).s);
    RemoveDirectoryW(source.s);
    RemoveDirectoryW(clips.s);
    RemoveDirectoryW(render_dir().s);
    RemoveDirectoryW(data.s);
    RemoveDirectoryW(path_join(root, L"Clips").s);
    T(RemoveDirectoryW(root.s));
    g_app.config.save_dir = keep;
}

static void test_share()
{
    // Recommended video rates: the table's rows at 30 and 60 fps, along the pixel count between them
    Config &cf = g_app.config;
    Config saved_cf = cf;
    share_kbps_reset();
    T(share_kbps_is_default());
    T(share_reco_kbps(1920, 1080, 60) == 12000 && share_reco_kbps(1920, 1080, 30) == 8000);
    T(share_reco_kbps(1280, 720, 60) == 7500 && share_reco_kbps(3840, 2160, 60) == 53000);
    T(share_reco_kbps(640, 360, 60) == 1500 && share_reco_kbps(0, 0, 0) == 12000);
    f64 mid = share_reco_kbps(2240, 1260, 60); // between 1080p and 1440p
    T(mid > 12000 && mid < 24000);
    // Linear between 30 and 60 fps, the 30 fps column below, in proportion above 60, and below 360p and above 2160p
    T(share_reco_kbps(1920, 1080, 45) == 10000 && share_reco_kbps(1920, 1080, 24) == 8000);
    T(share_reco_kbps(1920, 1080, 120) == 24000 && share_reco_kbps(7680, 4320, 60) == 4 * 53000.0);
    T(share_reco_kbps(320, 180, 60) == 375);
    // The lowest a copy goes at 67%, and the table set and reset
    T(fabs(share_lowest_kbps(1920, 1080, 60, 67) - 8040) < 0.01);
    share_kbps_set(3, true, 20000);
    cf.share_kbps_custom = false; // Use recommended: the table's kept but not used
    T(share_reco_kbps(1920, 1080, 60) == 12000 && !share_kbps_is_default());
    cf.share_kbps_custom = true;
    T(share_reco_kbps(1920, 1080, 60) == 20000);
    share_kbps_set(3, true, 5);
    T(cf.share_kbps[3][1] == 100);
    share_kbps_set(9, true, 5000); // no such row
    share_kbps_reset();
    T(share_kbps_is_default() && share_floor(67) == 0.67 && share_floor(10) == 0.5 && share_floor(200) == 1);
    // 20 s of 1080p60 with sound: (12000 + 128) kbit/s
    T(share_reco_bytes(20, 1920, 1080, 60, true) == 30320000);
    T(share_reco_bytes(20, 1920, 1080, 60, false) == 30000000);
    cf = saved_cf;

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
    // A session's four tracks: each stream's sound comes out of the video's share, and all of it fits the ceiling
    ShareRate r1;
    T(share_rate(20, 1, 20 * mb, 20 * mb, 0, &r1) && share_rate(20, 4, 20 * mb, 20 * mb, 0, &r));
    T(r.audio_kbps == r1.audio_kbps && r.video_kbps == r1.video_kbps - 3 * r1.audio_kbps);
    T((r.video_kbps + 4.0 * r.audio_kbps) * 125 * 20 <= r.ceiling);

    // One click: a file as is under the max, squeezed to it when 67% of it fits, a link further over
    ShareHosts bucket = {true, ""}, none = {};
    // Copies at game close: clips written while the game was in front, or just after it closed
    {
        i64 from = 1000000000, to = from + 3600ll * 1000000;
        T(share_prep_in(from, from, to) && share_prep_in(to + SHARE_PREP_SETTLE_US, from, to));
        T(!share_prep_in(from - 1, from, to) && !share_prep_in(to + SHARE_PREP_SETTLE_US + 1, from, to));
    }

    ShareDecision d = share_decide_for(15 * mb, 20, 0.67, bucket);
    T(d.kind == SHARE_AS_IS && d.file_ok && !d.file_squeezed && d.file_bytes == 15 * mb && !strcmp(d.why, "15 MB, as is"));
    d = share_decide_for(25 * mb, 20, 0.67, bucket);
    T(d.kind == SHARE_SQUEEZED && d.file_ok && d.file_squeezed && d.file_bytes < 20 * mb);
    T(!strcmp(d.why, "25 MB squeezed to 20 MB"));
    d = share_decide_for(31 * mb, 20, 0.67, bucket);
    T(d.kind == SHARE_LINK && !d.file_ok && d.link_ok && d.host == HOST_R2 && d.link_bytes == 31 * mb);
    T(!strcmp(d.host_label, "your bucket"));
    d = share_decide_for(21 * mb, 20, 1.0, bucket); // never lower
    T(d.kind == SHARE_LINK);
    d = share_decide_for(5000000000, 20, 0.67, bucket); // the bucket takes it as is, however big
    T(d.link_ok && d.host == HOST_R2 && d.link_bytes == 5000000000);
    // No bucket: the first uploader as is; with none, the way to sign in
    d = share_decide_for(2000000000, 20, 0.67, none);
    T(!d.link_ok && !strcmp(d.link_why, SHARE_NO_BUCKET));
    ShareHosts up = {false, "MyUploader"};
    d = share_decide_for(2000000000, 20, 0.67, up);
    T(d.link_ok && d.host == HOST_CUSTOM && d.link_bytes == 2000000000 && !strcmp(d.host_label, "MyUploader"));
    // A bucket: the uploaders are left out by the rule even when on
    ShareHosts all = {true, "Up1,Up2", 3000000000};
    d = share_decide_for(1200000000, 20, 0.67, all);
    T(d.host == HOST_R2);
    // The hosts to pick from: each that's on, with whether a clip fits
    ShareHostChoice ch[8];
    T(share_hosts_for(250000000, all, ch, 8) == 3);
    T(!strcmp(ch[0].name, "bucket") && ch[0].host == HOST_R2 && ch[0].fits && ch[0].bytes == 250000000);
    T(!strcmp(ch[1].name, "Up1") && !strcmp(ch[2].name, "Up2") && ch[2].host == HOST_CUSTOM);
    T(share_hosts_for(4000000000, all, ch, 8) == 3);
    T(!ch[0].fits && starts_with(ch[0].why, "Bigger than your bucket's") && !ch[0].bytes);
    T(ch[1].fits && ch[1].bytes == 4000000000);
    T(share_hosts_for(mb, none, ch, 8) == 0 && share_hosts_for(mb, all, ch, 2) == 2);
    // A picked host: there when it fits, or no link with why
    d = share_decide_on(250000000, 20, 0.67, all, "r2");
    T(d.link_ok && d.host == HOST_R2 && !strcmp(d.host_label, "your bucket"));
    d = share_decide_on(250000000, 20, 0.67, all, "up2");
    T(d.link_ok && d.host == HOST_CUSTOM && !strcmp(d.host_label, "Up2"));
    d = share_decide_on(4000000000, 20, 0.67, all, "bucket");
    T(!d.link_ok && starts_with(d.link_why, "Bigger than your bucket's"));
    d = share_decide_on(mb, 20, 0.67, up, "bucket");
    T(!d.link_ok && !strcmp(d.link_why, SHARE_NO_BUCKET));
    d = share_decide_on(mb, 20, 0.67, up, "other");
    T(!d.link_ok && !strcmp(d.link_why, "other isn't switched on in Options"));
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
    swprintf(file.s, MAX_PATH, L"%srekodr-test-%u.edit.txt", dir, GetCurrentProcessId());
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
    swprintf(dir.s, MAX_PATH, L"%srekodr-test-gamedb-%u", temp, GetCurrentProcessId());
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
    // A Java game: by the Java in its folder, then by what started it, then by its command line
    {
        auto jg = [](const char *image, const char *parent, const char *cmd) {
            const JavaGame *j = java_game(image, parent, cmd);
            return j ? j->name : "";
        };
        T(!strcmp(jg("D:\\SteamLibrary\\steamapps\\common\\Spiral Knights\\java_vm\\bin\\javaw.exe", "steam.exe", ""), "Spiral Knights"));
        T(!strcmp(jg("C:\\Games\\SteamApps\\common\\SlayTheSpire\\jre\\bin\\javaw.exe", "", "javaw -jar mts-launcher.jar"), "Slay the Spire"));
        T(!strcmp(jg("E:\\Steam\\steamapps\\common\\Wurm Unlimited\\jre\\bin\\java.exe", "WurmLauncher64.exe", ""), "Wurm Unlimited"));
        T(!strcmp(jg("C:\\Program Files\\Java\\bin\\java.exe", "WurmLauncher64.exe", ""), "Wurm Online"));
        T(!strcmp(jg("C:\\Program Files\\Java\\bin\\java.exe", "Starsector.exe", ""), "Starsector"));
        T(!strcmp(jg("C:\\Program Files\\Java\\bin\\java.exe", "", "java -cp x com.fs.starfarer.StarfarerLauncher"), "Starsector"));
        T(!strcmp(jg("C:\\Java\\bin\\javaw.exe", "cmd.exe", "javaw -jar StarMade.jar -force"), "StarMade"));
        T(!strcmp(jg("C:\\Users\\x\\AppData\\Local\\Ankama\\Wakfu\\jre\\bin\\java.exe", "", ""), "Wakfu"));
        T(!strcmp(jg("", "", "javaw com.threerings.yohoho.client.YoClient"), "Puzzle Pirates"));
        // Minecraft, and a parent's name only as a whole file name
        T(!strcmp(jg("C:\\java\\bin\\javaw.exe", "MinecraftLauncher.exe", "javaw net.minecraft.client.main.Main"), ""));
        T(!strcmp(jg("C:\\java\\bin\\javaw.exe", "notstarsector.exe", ""), ""));
        T(!strcmp(jg("", "", ""), ""));
    }
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
        char pic[512];
        const char grids[] = "{\"success\":true,\"data\":[{\"id\":1,\"url\":\"https://cdn2.steamgriddb.com/a.png\",\"thumb\":\"https://cdn2.steamgriddb.com/t.png\"},{\"url\":\"https://x\"}]}";
        sgdb_first_url(grids, sizeof grids - 1, "url", pic, sizeof pic);
        T(!strcmp(pic, "https://cdn2.steamgriddb.com/a.png"));
        sgdb_first_url(grids, sizeof grids - 1, "thumb", pic, sizeof pic);
        T(!strcmp(pic, "https://cdn2.steamgriddb.com/t.png"));
        sgdb_first_url("{\"success\":true,\"data\":[]}", 26, "url", pic, sizeof pic);
        T(!pic[0]);
        sgdb_first_url(0, 0, "url", pic, sizeof pic);
        T(!pic[0]);
    }
    // Its art: the icon for an icon; the hero, else the cover sent in its place; nothing that isn't https
    static const char art[] = "{\"key\":\"s2\",\"art\":{\"icon\":{\"url\":\"http://x/i.png\",\"from\":\"sgdb\"},\"hero\":null,"
                              "\"cover\":{\"url\":\"https://cdn2.steamgriddb.com/grid/g.png\",\"w\":920,\"h\":430,\"from\":\"sgdb\"}}}";
    char url[512], from[16];
    T(art_pick_url(art, sizeof art - 1, ART_HERO, url, sizeof url, from, sizeof from) &&
      !strcmp(url, "https://cdn2.steamgriddb.com/grid/g.png") && !strcmp(from, "sgdb"));
    T(!art_pick_url(art, sizeof art - 1, ART_ICON, url, sizeof url));
    static const char art2[] = "{\"art\":{\"cover\":{\"url\":\"https://a/g.png\"},\"hero\":{\"url\":\"https://a/h.png\",\"w\":1920},"
                               "\"icon\":{\"url\":\"https://a/i.png\",\"from\":\"discord\"}}}";
    T(art_pick_url(art2, sizeof art2 - 1, ART_HERO, url, sizeof url) && !strcmp(url, "https://a/h.png"));
    static const char art3[] = "{\"art\":{\"grid\":{\"url\":\"https://a/g.png\"},\"hero\":null}}";
    T(!art_pick_url(art3, sizeof art3 - 1, ART_HERO, url, sizeof url));
    T(art_pick_url(art2, sizeof art2 - 1, ART_ICON, url, sizeof url, from, sizeof from) && !strcmp(url, "https://a/i.png") &&
      !strcmp(from, "discord"));
    T(!art_pick_url("{}", 2, ART_HERO, url, sizeof url));
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
    { // connecting a bucket fills in its settings
        static Config c;
        c = {};
        R2Conf f = {"acct", "key", "", "bucket", "https://clips.example"};
        r2_connected(c, f);
        T(!strcmp(c.r2_account, "acct") && !strcmp(c.r2_bucket, "bucket"));
        T(!strcmp(c.r2_public_url, "https://clips.example"));
    }
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

    // Names: rekodr/<16 letters and digits>/clip.mp4, never the same twice
    char a[R2_KEY_MAX], b[R2_KEY_MAX];
    r2_new_key(a);
    r2_new_key(b);
    T(strlen(a) == 32 && starts_with(a, "rekodr/") && !strcmp(a + 23, "/clip.mp4") && strcmp(a, b));
    bool plain = true;
    for (i32 i = 7; i < 23; i++) plain &= isalnum((u8)a[i]) != 0;
    T(plain);
    char url[128];
    r2_make_url("https://pub-x.r2.dev/", "k.mp4", 1, url, sizeof url);
    T(!strcmp(url, "https://pub-x.r2.dev/k.mp4"));
    r2_make_url("https://clips.example.com", "k.mp4", 3, url, sizeof url);
    T(!strcmp(url, "https://clips.example.com/k.mp4?v=3"));

    // Space: the free 10 GB less the other buckets less 5%, 2 GB when the account's total can't be read, nothing
    // when the other buckets already hold it all; the player's value wins. Then $0.015 a GB-month more.
    T(r2_space_default(3 * R2_GB) == 6500000000ll);
    T(r2_space_default(0) == 9500000000ll);
    T(r2_space_default(-1) == 2 * R2_GB);
    T(r2_space_default(12 * R2_GB) == 0 && r2_space_default(9600000000ll) == 0);
    T(r2_cap_for(0, -1, 3 * R2_GB) == 6500000000ll);
    T(r2_cap_for(0, -1, -1) == 2 * R2_GB);
    T(r2_cap_for(0, 4, 3 * R2_GB) == 4 * R2_GB && r2_cap_for(0, 4, -1) == 4 * R2_GB && r2_cap_for(0, 0, 0) == 0);
    T(r2_cap_for(1.35, 10, -1) / R2_GB == 100 || r2_cap_for(1.35, 10, -1) / R2_GB == 99);
    T(r2_cap_for(1.5, -1, 12 * R2_GB) / R2_GB == 100 || r2_cap_for(1.5, -1, 12 * R2_GB) / R2_GB == 99);
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
    s[1].host = HOST_CUSTOM; // an uploader's clip isn't in the bucket
    T(r2_plan_cleanup(s, 4, 8 * R2_GB, 0, -1, plan, 4) == 0);

    // Before an upload: the bucket full to the limit, a link of 3 GB clears the oldest unpinned until it fits; one
    // that exactly fills the limit fits; the link being shared again is never cleared; bigger than the limit, or
    // only pinned links left, nothing is cleared and it's refused
    for (i32 i = 0; i < 4; i++) {
        s[i] = {};
        s[i].bytes = 2 * R2_GB;
        s[i].uploaded_us = 100 + i; // the first is the oldest
    }
    s[1].pinned = true;
    T(r2_plan_cleanup(s, 4, 8 * R2_GB, 0, -1, plan, 4) == 0);
    n = r2_plan_cleanup(s, 4, 8 * R2_GB, 3 * R2_GB, -1, plan, 4);
    T(n == 2 && plan[0] == 0 && plan[1] == 2);
    T(r2_plan_cleanup(s, 4, 10 * R2_GB, 2 * R2_GB, -1, plan, 4) == 0);
    n = r2_plan_cleanup(s, 4, 10 * R2_GB, 2 * R2_GB + 1, 0, plan, 4);
    T(n == 1 && plan[0] == 2);
    T(r2_plan_cleanup(s, 4, 8 * R2_GB, 9 * R2_GB, -1, plan, 4) == -1);
    T(r2_plan_cleanup(s, 4, 8 * R2_GB, 7 * R2_GB, -1, plan, 4) == -1); // pinned 2 + 7
    n = r2_plan_cleanup(s, 4, 8 * R2_GB, 6 * R2_GB, -1, plan, 4); // pinned 2 + 6: all three others go
    T(n == 3 && plan[0] == 0 && plan[1] == 2 && plan[2] == 3);

    // Expiry: a bucket link past its end goes, pinned or not; one with no end, a removed one or an uploader's stays
    {
        static R2Share e[4];
        for (i32 i = 0; i < 4; i++) {
            e[i] = {};
            e[i].host = HOST_R2;
        }
        i64 day = 86400ll * 1000000, t0 = 1700000000000000;
        e[0].expires_us = r2_expiry(t0, 1);
        e[0].pinned = true;
        e[1].expires_us = r2_expiry(t0, 7);
        e[2].expires_us = r2_expiry(t0, 1);
        e[2].removed = true;
        e[3].expires_us = r2_expiry(t0, 1);
        e[3].host = HOST_CUSTOM;
        T(r2_expiry(t0, 0) == 0 && e[0].expires_us == t0 + day && e[1].expires_us == t0 + 7 * day);
        T(r2_plan_expired(e, 4, t0 + day - 1, plan, 4) == 0 && r2_share_live(e[0], t0 + day - 1));
        n = r2_plan_expired(e, 4, t0 + day, plan, 4);
        T(n == 1 && plan[0] == 0 && !r2_share_live(e[0], t0 + day) && r2_share_live(e[1], t0 + day));
        n = r2_plan_expired(e, 4, t0 + 8 * day, plan, 4);
        T(n == 2 && plan[0] == 0 && plan[1] == 1);
    }

    // The Space bar: the bucket's links oldest first, an uploader's left out, and which go for a limit
    {
        static R2Share shown[4];
        u8 goes[4];
        s[3].host = HOST_CUSTOM;
        i32 m = r2_evict_plan(s, 4, 4 * R2_GB, 0, 0, goes, shown, 4);
        T(m == 3 && shown[0].uploaded_us == 100 && shown[1].uploaded_us == 101 && shown[2].uploaded_us == 102);
        T(goes[0] && !goes[1] && !goes[2]); // pinned 2 + 2 + 2 over 4: the oldest unpinned goes
        m = r2_evict_plan(s, 4, 6 * R2_GB, 0, 0, goes, shown, 4);
        T(m == 3 && !goes[0] && !goes[1] && !goes[2]);
        m = r2_evict_plan(s, 4, 6 * R2_GB, 0, R2_GB, goes, shown, 4); // untracked files count too
        T(goes[0] && !goes[1] && !goes[2]);
        m = r2_evict_plan(s, 4, 6 * R2_GB, 4 * R2_GB, 0, goes, shown, 4); // and a link to come
        T(goes[0] && !goes[1] && goes[2]);
        m = r2_evict_plan(s, 4, R2_GB, 0, 0, goes, shown, 4); // pinned alone over: none go
        T(m == 3 && !goes[0] && !goes[1] && !goes[2]);
        T(r2_evict_plan(s, 4, R2_GB, 0, 0, goes, shown, 2) == 2);
        s[3].host = HOST_R2;
    }

    {
        // Uploaders switched on and off by name, once each
        char kept[256];
        str_set(kept, g_app.config.link_uploaders);
        g_app.config.link_uploaders[0] = 0;
        link_uploader_set("mine", true);
        link_uploader_set("other", true);
        link_uploader_set("mine", true);
        T(!strcmp(g_app.config.link_uploaders, "mine,other") && link_host_on("other") && !link_host_on("x"));
        link_uploader_set("mine", false);
        link_uploader_set("mine", false);
        T(!strcmp(g_app.config.link_uploaders, "other") && !link_host_on("mine"));
        T(!uploader_remove("") && !uploader_remove("../x"));
        str_set(g_app.config.link_uploaders, kept);
    }
    R2Share live = {};
    T(r2_share_live(live));
    live.paused = 1;
    T(!r2_share_live(live));

    // The month's cap: reached at 90%, off at 0, never before the reads are known
    T(!r2_capped(8999999, 10000000) && r2_capped(9000000, 10000000) && r2_capped(20000000, 10000000));
    T(!r2_capped(20000000, 0) && !r2_capped(-1, 10000000));

    // A bucket that can't take a link says why; the link isn't passed to a free host
    T(!r2_refuses(true, false));
    T(r2_refuses(false, false) && strstr(r2_refuses(false, false), "keys"));
    T(r2_refuses(true, true) && strstr(r2_refuses(true, true), "until the 1st"));

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
    ap.host = HOST_CUSTOM;
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

    // History file round trip, a tab in a title made a space
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    Path file = {};
    swprintf(file.s, MAX_PATH, L"%srekodr-tests-%lu-shares.tsv", temp, GetCurrentProcessId());
    static R2Share w[2], r[4];
    w[0] = {};
    str_set(w[0].key, "rekodr/abcdefghijklmnop/clip.mp4");
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
    w[1].host = HOST_CUSTOM;
    w[0].paused = 2;
    str_set(w[0].hidden, "rekodr/qrstuvwxyzabcdef/clip.mp4");
    w[0].pause_day = 20261121;
    w[0].page_w = 1920;
    w[0].page_h = 1080;
    w[0].social = 1;
    w[0].social_w = 1280;
    w[0].social_h = 720;
    w[0].seconds = 125.5f;
    w[0].preview = 60;
    w[0].expires_us = 1700604800000000;
    snprintf(w[0].marks, sizeof w[0].marks, "12.400 First kill%c17.000 Ace", R2_MARK_SEP);
    w[1].social = -1;
    T(r2_history_write(file, w, 2));
    T(r2_history_read(file, r, 4) == 2);
    T(!strcmp(r[0].key, w[0].key) && !strcmp(r[0].url, w[0].url) && !strcmp(r[0].title, "ace round"));
    T(!strcmp(r[0].game, "Valorant") && !strcmp(r[0].path, w[0].path) && r[0].bytes == w[0].bytes);
    T(r[0].uploaded_us == w[0].uploaded_us && r[0].pinned && !r[0].removed && r[0].version == 2);
    T(r[0].lib_key == w[0].lib_key && r[1].removed && !r[1].pinned && !strcmp(r[1].key, "def.mp4"));
    T(r[0].host == HOST_R2 && r[1].host == HOST_CUSTOM);
    T(r[0].paused == 2 && !strcmp(r[0].hidden, w[0].hidden) && r[0].pause_day == 20261121 && !r[1].paused);
    T(r[0].plays_today == -1 && r[0].plays_month == -1 && r[0].plays_24h == -1);
    T(r[0].page_w == 1920 && r[0].page_h == 1080 && !r[1].page_w && !r[1].page_h);
    T(!strcmp(r2_object_key(r[0]), w[0].hidden) && !strcmp(r2_object_key(r[1]), "def.mp4"));
    T(r[0].social == 1 && r[0].social_w == 1280 && r[0].social_h == 720 && r[0].seconds == 125.5f);
    T(r[0].preview == 60 && !strcmp(r[0].marks, w[0].marks) && !r[1].marks[0]);
    T(r[1].social == -1 && !r[1].social_w);
    T(r[0].expires_us == w[0].expires_us && !r[1].expires_us);
    DeleteFileW(file.s);

    // Preview pages: the page beside the clip's folder, the picture in it, at most 1280 wide and even
    char pkey[R2_KEY_MAX];
    r2_page_key("rekodr/abcdefghijklmnop/clip.mp4", pkey);
    T(!strcmp(pkey, "rekodr/abcdefghijklmnop"));
    r2_thumb_key("rekodr/qrstuvwxyzabcdef/clip.mp4", pkey);
    T(!strcmp(pkey, "rekodr/qrstuvwxyzabcdef/thumb.jpg"));
    i32 tw, th;
    r2_thumb_size(1920, 1080, &tw, &th);
    T(tw == 1280 && th == 720);
    r2_thumb_size(2560, 1440, &tw, &th);
    T(tw == 1280 && th == 720);
    r2_thumb_size(853, 480, &tw, &th);
    T(tw == 852 && th == 480);
    r2_thumb_size(0, 1080, &tw, &th);
    T(!tw && !th);

    // The page: the tags for a card that plays, the name, the game line and the player; the title made safe and
    // ASCII; paused, no clip and nothing that plays it
    static char html[65536];
    static const u8 past_ascii[] = {'c', 'a', 'f', 0xc3, 0xa9, ' ', 0xf0, 0x9f, 0x8e, 0xae, 0x01, 0}; // e-acute, a pad, a control
    char title[96];
    snprintf(title, sizeof title, "ace <b>\"round\"</b> & %s", (const char *)past_ascii);
    R2PageParts pp = {title, "https://x.dev/rekodr/abc?v=2", "https://x.dev/rekodr/abc/clip.mp4?v=2",
                      "https://x.dev/rekodr/abc/social.mp4?v=2", "https://x.dev/rekodr/abc/thumb.jpg?v=2", 2560,
                      1440, 1280, 720, 1280, 720};
    i32 hn = r2_page_html(pp, false, html, sizeof html);
    T(hn > 0 && hn == (i32)strlen(html));
    bool ascii = true;
    for (i32 i = 0; i < hn; i++) ascii = ascii && (u8)html[i] < 0x80;
    T(ascii);
    T(strstr(html, "<title>ace &lt;b&gt;&quot;round&quot;&lt;/b&gt; &amp; caf&#233; &#127918;</title>") != 0);
    T(strstr(html, "<h1>ace &lt;b&gt;&quot;round&quot;&lt;/b&gt; &amp; caf&#233; &#127918;</h1>") != 0);
    T(strstr(html, "<meta property=\"og:type\" content=\"video.other\">") != 0);
    T(strstr(html, "<meta property=\"og:site_name\" content=\"Rekodr\">") != 0);
    T(strstr(html, "<meta property=\"og:image\" content=\"https://x.dev/rekodr/abc/thumb.jpg?v=2\">") != 0);
    T(strstr(html, "og:image:width\" content=\"1280\"") != 0 && strstr(html, "og:image:height\" content=\"720\"") != 0);
    // cards play the small copy at its size; the page plays the clip
    const char *copy = "https://x.dev/rekodr/abc/social.mp4?v=2";
    char tag[160];
    static const char *const og_video[] = {"og:video", "og:video:url", "og:video:secure_url"};
    for (const char *name : og_video) {
        snprintf(tag, sizeof tag, "<meta property=\"%s\" content=\"%s\">", name, copy);
        T(strstr(html, tag) != 0);
    }
    T(strstr(html, "og:video:type\" content=\"video/mp4\"") != 0);
    T(strstr(html, "og:video:width\" content=\"1280\"") != 0);
    T(strstr(html, "<meta name=\"twitter:card\" content=\"player\">") != 0);
    snprintf(tag, sizeof tag, "<meta name=\"twitter:player\" content=\"%s\">", copy);
    T(strstr(html, tag) != 0);
    snprintf(tag, sizeof tag, "<meta name=\"twitter:player:stream\" content=\"%s\">", copy);
    T(strstr(html, tag) != 0 && strstr(html, "twitter:player:stream:content_type\" content=\"video/mp4\"") != 0);
    T(strstr(html, "twitter:player:height\" content=\"720\"") != 0 && strstr(html, "twitter:image\"") != 0);
    T(strstr(html, "noindex") != 0 && !strstr(html, "og:video:width\" content=\"2560\""));
    T(strstr(html, "<video src=\"https://x.dev/rekodr/abc/clip.mp4?v=2\" poster=\"https://x.dev/rekodr/abc/thumb.jpg?v=2\"") != 0);
    T(strstr(html, "width=\"2560\" height=\"1440\" controls playsinline") != 0 && strstr(html, "aspect-ratio:2560/1440"));
    T(strstr(html, "<div class=\"bg\" style=\"background-image:url('https://x.dev/rekodr/abc/thumb.jpg?v=2')\">"));
    // Onest is in the page: no font, nor anything else, from another site
    T(strstr(html, "@font-face{font-family:Onest;") && strstr(html, "src:url(data:font/woff2;base64,d09GMg"));
    T(!strstr(html, "fonts.g") && strstr(html, "font:14px/1.5 Onest,\"Segoe UI\",system-ui,sans-serif"));
    // Download (the white pill), Copy link, and "via Rekodr" linking to the project; no game line without one
    T(strstr(html, "<a class=\"btn p\" href=\"https://x.dev/rekodr/abc/clip.mp4?v=2\" download>"));
    T(strstr(html, "<a class=\"via\" href=\"https://github.com/CalvBot/rekodr\"><img src=\"https://cdn.discordapp.com/"
                   "emojis/1574532567124803645.webp\" alt=\"\">via <b>Rekodr</b></a>"));
    T(strstr(html, "Copy link") && !strstr(html, "class=\"game\""));
    // the accent, in the CSS and as the theme colour: the default, or the one picked
    T(strstr(html, "--accent:#00897b;") && strstr(html, "background:var(--accent)") &&
      strstr(html, "<meta name=\"theme-color\" content=\"#00897b\">"));
    pp.accent = 0xffe066;
    hn = r2_page_html(pp, false, html, sizeof html);
    T(hn > 0 && strstr(html, "--accent:#ffe066;") && strstr(html, "content=\"#ffe066\"") && !strstr(html, "#00897b"));
    pp.accent = 0;
    // #t= starts the clip there; no preview, no markers
    T(strstr(html, "/[#&]t=([0-9:.]+)/.exec(location.hash)") && strstr(html, "var CB={pv:0.000,m:[]};"));
    // a preview and markers go to the player, the notes made safe for the script tag
    pp.preview = 210;
    char marks[96];
    snprintf(marks, sizeof marks, "12.400 First kill%c17.000 </script>%c25.5 ", R2_MARK_SEP, R2_MARK_SEP);
    pp.marks = marks;
    hn = r2_page_html(pp, false, html, sizeof html);
    T(hn > 0 && strstr(html, "var CB={pv:210.000,m:[[12.400,\"First kill\"],[17.000,\"\\u003c/script\\u003e\"],"
                             "[25.500,\"\"]]};</script>"));
    pp.preview = 0;
    pp.marks = 0;
    // the heading and the game line with its emoji
    pp.heading = "Ace";
    pp.line = "Valorant";
    pp.emoji = "<a:val:123>";
    hn = r2_page_html(pp, false, html, sizeof html);
    T(hn > 0 && strstr(html, "<h1>Ace</h1>\n<div class=\"game\"><img src=\"https://cdn.discordapp.com/emojis/123.gif\" "
                             "alt=\"\">Valorant</div>"));
    // the game's icon goes before the line instead of the emoji
    pp.icon = "data:image/png;base64,iVBORw0K";
    hn = r2_page_html(pp, false, html, sizeof html);
    T(hn > 0 && strstr(html, "<div class=\"game\"><img class=\"icon\" src=\"data:image/png;base64,iVBORw0K\" "
                             "alt=\"\">Valorant</div>") && !strstr(html, "emojis/123"));
    pp.icon = "";
    pp.emoji = "nope";
    hn = r2_page_html(pp, false, html, sizeof html);
    T(hn > 0 && strstr(html, "<div class=\"game\">Valorant</div>"));
    pp.heading = pp.line = pp.emoji = pp.icon = 0;
    // the game's art goes behind the page instead of the clip's picture
    pp.art = "data:image/png;base64,iVBORw0K";
    hn = r2_page_html(pp, false, html, sizeof html);
    T(hn > 0 && strstr(html, "<div class=\"bg\" style=\"background-image:url('data:image/png;base64,iVBORw0K')\">")
      && !strstr(html, "url('https://x.dev/rekodr/abc/thumb.jpg"));
    pp.art = 0;
    // an icon for the page: a PNG scaled to R2_ICON_PX, in base64
    {
        static char data[32768];
        Path png = {};
        GetFullPathNameW(L"..\\assets\\icons\\rekodr.png", MAX_PATH, png.s, 0);
        T(r2_icon_data(png, data, sizeof data) && starts_with(data, "data:image/png;base64,iVBORw0KGgo"));
        T(strlen(data) < 24000 && !strchr(data, '"'));
        T(!r2_icon_data(png, data, 64) && !data[0]);
        // the art behind a page: small and grey
        T(r2_icon_data(png, data, sizeof data, R2_ART_PX, true) && starts_with(data, "data:image/png;base64,iVBORw0KGgo"));
        Path none = {};
        GetFullPathNameW(L"..\\assets\\icons\\none.png", MAX_PATH, none.s, 0);
        T(!r2_icon_data(none, data, sizeof data));
    }
    // The accent's ink: white on the default and darker picks, the page's near-black on light ones
    T(ink_on(ACCENT_DEFAULT) == 0xffffff && ink_on(0x4f8cff) == 0xffffff && ink_on(0xf0434f) == 0xffffff);
    T(ink_on(0xffe066) == COL_BG && ink_on(0xffffff) == COL_BG && ink_on(0x7fffd4) == COL_BG);
    ui_apply_accent(0xffe066);
    T(COL_ACCENT == 0xffe066 && COL_ACCENT_INK == COL_BG && COL_ACCENT_HOVER != COL_ACCENT);
    ui_apply_accent(ACCENT_DEFAULT);
    T(COL_ACCENT == 0x00897b && COL_ACCENT_DOWN == 0x00574b && COL_ACCENT_INK == 0xffffff);
    // nothing small enough for cards: a picture card, the page still plays the clip
    pp.embed = "";
    hn = r2_page_html(pp, false, html, sizeof html);
    T(hn > 0 && !strstr(html, "og:video") && !strstr(html, "twitter:player") && !strstr(html, "social.mp4"));
    T(strstr(html, "twitter:card\" content=\"summary_large_image\"") && strstr(html, "og:image\""));
    T(strstr(html, "<video src=\"https://x.dev/rekodr/abc/clip.mp4?v=2\"") != 0);
    pp = {"ace", "https://x.dev/rekodr/abc", "https://x.dev/rekodr/abc/clip.mp4",
          "https://x.dev/rekodr/abc/clip.mp4", "https://x.dev/rekodr/abc/thumb.jpg", 1920, 1080, 1920, 1080, 1280,
          720};
    hn = r2_page_html(pp, true, html, sizeof html);
    T(hn > 0 && strstr(html, "<title>ace</title>") && strstr(html, "This clip isn't playing right now."));
    T(!strstr(html, "<video") && !strstr(html, "og:video") && !strstr(html, "clip.mp4") && !strstr(html, "twitter:player"));
    T(!strstr(html, "thumb.jpg") && !strstr(html, "<script") && strstr(html, "via <b>Rekodr</b></a>"));
    hn = r2_page_html(pp, false, html, sizeof html);
    T(hn > 0 && strstr(html, "via <b>Rekodr</b></a>") > strstr(html, "<video"));
    T(r2_page_html(pp, false, html, 64) == 0);

    // Discord's card: title, line, the video and the via line; names escaped for markdown, JSON and the script tag
    EmbedCard card;
    EmbedInput ein = {"ace <\"round\"> *1*", "Valorant", 42, 0, 0, true, 0};
    embed_card(ein, &card);
    T(!strcmp(card.title, ein.title) && !strcmp(card.line, "Valorant") && card.player && card.via
      && !card.accent && card.icon);
    ein.title = "Valorant 2026-11-24 21-04-33";
    embed_card(ein, &card);
    T(!strcmp(card.title, "Valorant") && !strcmp(card.line, "Valorant"));
    ein.title = "ace";
    ein.game = "";
    embed_card(ein, &card);
    T(!strcmp(card.title, "ace") && !card.line[0]);
    char named[64];
    snprintf(named, sizeof named, "ace <\"round\"> *1* %s", (const char *)past_ascii); // e-acute, a pad, a control
    ein = {named, "Valorant", 42, 0, 0, true, 0x112233}; // the accent is the card's edge
    embed_card(ein, &card);
    static char cj[R2_CARD_MAX + 1];
    i32 cn = r2_card_json(card, "https://x.dev/rekodr/abc/social.mp4?v=2", "https://x.dev/rekodr/abc/thumb.jpg",
                          "", "", "", cj, sizeof cj);
    T(cn > 0 && cn == (i32)strlen(cj) && !strchr(cj, '<') && !strchr(cj, '>') && !strchr(cj, '&'));
    T(starts_with(cj, "{\"component\":{\"type\":17,\"accent_color\":1122867,\"components\":[{\"type\":10,"));
    T(strstr(cj, "\"content\":\"**ace \\u003c\\\"round\\\"\\\\\\u003e \\\\*1\\\\* caf\\u00e9 \\ud83c\\udfae**\"}") != 0);
    T(strstr(cj, "{\"type\":10,\"content\":\"Valorant\"}") != 0);
    T(strstr(cj, "{\"type\":12,\"items\":[{\"media\":{\"url\":\"https://x.dev/rekodr/abc/social.mp4?v=2\"}}]}") != 0);
    T(strstr(cj, "\"-# \\u003c:rekodr:1574532567124803645\\u003e [via Rekodr](\\u003chttps://github.com/CalvBot/"
                 "rekodr\\u003e)\"}]}}") != 0);
    T(!strstr(cj, "thumb.jpg") && !strstr(cj, "\"type\":9"));
    card.player = false; // nothing small enough: the picture
    cn = r2_card_json(card, "https://x.dev/v.mp4", "https://x.dev/t.jpg", "", "", "", cj, sizeof cj);
    T(cn > 0 && strstr(cj, "https://x.dev/t.jpg") && !strstr(cj, "v.mp4"));
    T(r2_card_json(card, "a", "b", "", "", "", cj, 100) == 0);
    // a game's emoji, when there is one, starts the line; one that isn't an emoji is left out, the line kept
    T(card.icon);
    cn = r2_card_json(card, "https://x.dev/v.mp4", "https://x.dev/t.jpg", "<:g_s5367951:1574532569876406393>", "", "",
                      cj, sizeof cj);
    T(cn > 0 && strstr(cj, "{\"type\":10,\"content\":\"\\u003c:g_s5367951:1574532569876406393\\u003e Valorant\"}"));
    cn = r2_card_json(card, "https://x.dev/v.mp4", "https://x.dev/t.jpg", "<:g:1>", "", "", cj, sizeof cj);
    T(cn > 0 && strstr(cj, "{\"type\":10,\"content\":\"Valorant\"}"));
    card.icon = false;
    cn = r2_card_json(card, "https://x.dev/v.mp4", "https://x.dev/t.jpg", "<:g_s5:1>", "", "", cj, sizeof cj);
    T(cn > 0 && strstr(cj, "{\"type\":10,\"content\":\"Valorant\"}"));
    T(emoji_ok("<:rekodr:1574532567124803645>") && emoji_ok("<:a_1:9>") && emoji_ok("<a:spin:9>"));
    T(!emoji_ok("") && !emoji_ok(0) && !emoji_ok("<:a:1>") && !emoji_ok("<b:ab:1>") && !emoji_ok("<a:a:1>"));
    T(!emoji_ok("<:ab:>") && !emoji_ok("<:ab:1> ") && !emoji_ok("<:a b:1>") && !emoji_ok("<:ab:x1>"));
    T(!emoji_ok("<:abcdefghijabcdefghijabcdefghijabc:1>") && !emoji_ok("<:ab:123456789012345678901>"));

    // The game emoji list: a game's letters and digits are its key, so Counter-Strike and Counter-Strike 2 differ
    char ekey[64];
    emoji_key("Counter-Strike 2", ekey, sizeof ekey);
    T(!strcmp(ekey, "counterstrike2"));
    emoji_key("Counter-Strike", ekey, sizeof ekey);
    T(!strcmp(ekey, "counterstrike"));
    static EmojiRow erows[8];
    static const char eans[] = "{\"emojis\":[[\"overwatch\",\"<:ow:77>\"],[\"counterstrike2\",\"<:cs2:2>\"],"
                               "[\"Bad Key\",\"<:x1:1>\"],[\"counterstrike\",\"<a:cs:1>\"],[\"nope\",\":nope:\"]]}";
    T(emoji_parse(eans, false, erows, 8) == 3);
    T(!strcmp(erows[0].key, "counterstrike") && !strcmp(erows[1].key, "counterstrike2"));
    T(!strcmp(erows[2].emoji, "<:ow:77>"));
    T(emoji_parse("{\"emojis\":[]}", false, erows, 8) == 0 && emoji_parse("<html>", false, erows, 8) == -1);
    T(emoji_parse("overwatch\t<:ow:77>\r\nbad\nhades\t<:hades:3>\n", true, erows, 8) == 2);
    T(!strcmp(erows[0].key, "hades") && !strcmp(erows[1].emoji, "<:ow:77>"));
    EmojiRow *elist = (EmojiRow *)calloc(8, sizeof(EmojiRow));
    T(emoji_parse(eans, false, elist, 8) == 3);
    emoji_publish(elist, 3);
    char eout[64];
    T(gamedb_emoji("Counter-Strike 2", eout, sizeof eout) && !strcmp(eout, "<:cs2:2>"));
    T(gamedb_emoji("COUNTER-STRIKE", eout, sizeof eout) && !strcmp(eout, "<a:cs:1>"));
    T(!gamedb_emoji("Valorant", eout, sizeof eout) && !eout[0] && !gamedb_emoji("", eout, sizeof eout));
    emoji_publish(0, 0);
    // the style: found by its id, the first when the id is unknown; a preview tags the line
    T(embed_style_count() == 1 && !strcmp(embed_style_id(0), "rekodr") && embed_style_find("rekodr") == 0);
    T(embed_style_find("nope") == -1 && !strcmp(embed_style_name(5), "Rekodr"));
    ein = {"ace", "Valorant", 1500, 210, 0, true, 0};
    embed_card(ein, &card, 0);
    T(!strcmp(card.line, "Valorant") && !strcmp(card.tag, "Preview, first 3:30 of 25:00") && card.player);
    cn = r2_card_json(card, "https://x.dev/s.mp4", "https://x.dev/t.jpg", "", "", "", cj, sizeof cj);
    T(cn > 0 && strstr(cj, "{\"type\":10,\"content\":\"Valorant - Preview, first 3:30 of 25:00\"}"));
    // with the page, the tag links to it where the preview stops
    T(card.tag_at == 210);
    cn = r2_card_json(card, "https://x.dev/s.mp4", "https://x.dev/t.jpg", "", "https://x.dev/p", "", cj, sizeof cj);
    T(cn > 0 && strstr(cj, "\"Valorant - [Preview, first 3:30 of 25:00](\\u003chttps://x.dev/p#t=210"
                           "\\u003e)\"}"));
    // a session: Watch and Download after the video, before the via line; its page links the download; the file
    // downloads by its name
    ein.session = true;
    embed_card(ein, &card, 0);
    T(card.buttons);
    cn = r2_card_json(card, "https://x.dev/s.mp4", "https://x.dev/t.jpg", "", "https://x.dev/p", "https://x.dev/c.mp4",
                      cj, sizeof cj);
    T(cn > 0 && strstr(cj, "\"}}]},{\"type\":1,\"components\":[{\"type\":2,\"style\":5,\"label\":\"Watch\","
                           "\"url\":\"https://x.dev/p\"},{\"type\":2,\"style\":5,\"label\":\"Download\","
                           "\"url\":\"https://x.dev/c.mp4\"}]},{\"type\":10,\"content\":\"-# \\u003c:rekodr"));
    char dtype[256];
    r2_download_type("", dtype, sizeof dtype);
    T(!strcmp(dtype, "video/mp4"));
    r2_download_type("Overwatch \"late\" 2026-11-24 ", dtype, sizeof dtype);
    T(!strcmp(dtype, "video/mp4\r\nContent-Disposition: attachment; filename=\"Overwatch late 2026-11-24.mp4\""));
    pp.card = "{\"component\":{}}";
    hn = r2_page_html(pp, false, html, sizeof html);
    T(strstr(html, "<script id=\"discord:component-embed\" type=\"application/json\">{\"component\":{}}</script>"));
    T(strstr(html, "og:image\"") != 0); // the Open Graph tags stay for other apps
    hn = r2_page_html(pp, true, html, sizeof html);
    T(hn > 0 && !strstr(html, "component-embed"));

    // The page's title: the game and how long it plays; a name of its own goes first
    char pt[128];
    r2_page_title("Overwatch 2026-11-24 21-04-33", "Overwatch", 125, pt, sizeof pt);
    T(!strcmp(pt, "Overwatch, 2 minutes"));
    r2_page_title("", "Overwatch", 1, pt, sizeof pt);
    T(!strcmp(pt, "Overwatch, 1 second"));
    r2_page_title("ace round", "Valorant", 42.4, pt, sizeof pt);
    T(!strcmp(pt, "ace round - Valorant, 42 seconds"));
    r2_page_title("ace round", "", 3900, pt, sizeof pt);
    T(!strcmp(pt, "ace round, 1 hour 5 minutes"));
    r2_page_title("", "", 0, pt, sizeof pt);
    T(!strcmp(pt, "Clip"));
    r2_page_title("", "", 7200, pt, sizeof pt);
    T(!strcmp(pt, "Clip, 2 hours"));

    // The social copy: none when the clip fits cards; else the whole clip at 1080p when it fits at the floor, or a
    // preview
    const i64 mb = 1000 * 1000;
    T(r2_social_height(30, 1920, 1080, 60, true, 50 * mb, 150 * mb) == 0);     // ~13 Mbps, under SOCIAL_MAX_KBPS
    T(r2_social_height(30, 1920, 1080, 60, true, 60 * mb, 150 * mb) == 1080);  // ~16 Mbps, over SOCIAL_MAX_KBPS
    T(r2_social_height(30, 2560, 1440, 60, true, 50 * mb, 150 * mb) == 1080);  // too big a picture
    T(r2_social_height(200, 1920, 1080, 60, true, 300 * mb, 150 * mb) == 1080); // ~5.5 Mbps fits
    T(r2_social_height(300, 1920, 1080, 60, true, 900 * mb, 150 * mb) == -1);  // ~3.6 Mbps, under the floor
    T(r2_social_height(300, 640, 360, 30, true, 900 * mb, 150 * mb) == 1080);  // a small picture's floor is lower
    f64 target, least;
    social_kbps(1920, 1080, 60, &target, &least);
    T(target == 12000 && least == 5000);
    social_kbps(2560, 1440, 60, &target, &least); // made at 1080p
    T(target == 12000 && least == 5000);
    social_kbps(1920, 1080, 120, &target, &least); // never over SOCIAL_KBPS
    T(target == 12000 && least == 5000);
    social_kbps(1920, 1080, 30, &target, &least);
    T(target == 8000 && least == 5000);
    social_kbps(640, 360, 30, &target, &least);
    T(target == 1000 && least == 1000);
    char sk[R2_KEY_MAX];
    r2_social_key("rekodr/abcdefghijklmnop/clip.mp4", sk);
    T(!strcmp(sk, "rekodr/abcdefghijklmnop/social.mp4"));

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
    T(r2_parse_new_token("{\"result\":{\"id\":\"0123456789abcdef0123456789abcd02\",\"name\":\"rekodr\","
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
    // No "total" in the answer: the account's storage isn't known
    T(f.account_bytes == -1);
    // The account's storage: each bucket's newest count (rows come newest first), added up
    T(r2_parse_usage("{\"data\":{\"viewer\":{\"accounts\":[{\"storage\":[{\"max\":{\"payloadSize\":1000,\"metadataSize\":0}}],"
                     "\"ops\":[],\"total\":["
                     "{\"max\":{\"payloadSize\":1000,\"metadataSize\":0},\"dimensions\":{\"bucketName\":\"rekodr\"}},"
                     "{\"max\":{\"payloadSize\":3000,\"metadataSize\":5},\"dimensions\":{\"bucketName\":\"music\"}},"
                     "{\"max\":{\"payloadSize\":900,\"metadataSize\":0},\"dimensions\":{\"bucketName\":\"rekodr\"}},"
                     "{\"max\":{\"payloadSize\":2000,\"metadataSize\":0},\"dimensions\":{\"bucketName\":\"music\"}}]}]}}}",
                     &f));
    T(f.bytes == 1000 && f.account_bytes == 4005);
    T(r2_parse_usage("{\"data\":{\"viewer\":{\"accounts\":[{\"storage\":[],\"ops\":[],\"total\":[]}]}}}", &f));
    T(f.account_bytes == 0);
    // The cap in force uses the other buckets from the last answer: 4005 - 1000 of them here
    {
        R2Fetched keep_f = g_r2.fetched;
        i64 keep_us = g_r2.usage_us;
        f64 keep_budget = g_r2.budget;
        f32 keep_space = g_r2.space_gb;
        bool keep_known = g_r2.budget_known;
        g_r2.fetched = {};
        g_r2.fetched.bytes = 1000;
        g_r2.fetched.account_bytes = 4005;
        g_r2.usage_us = 1;
        g_r2.budget = 0;
        g_r2.space_gb = -1;
        g_r2.budget_known = true;
        T(r2_others_now() == 3005 && r2_cap_now() == r2_space_default(3005));
        g_r2.space_gb = 3;
        T(r2_cap_now() == 3 * R2_GB);
        g_r2.space_gb = -1;
        g_r2.fetched.account_bytes = -1;
        T(r2_others_now() == -1 && r2_cap_now() == 2 * R2_GB);
        g_r2.fetched.account_bytes = 4005;
        g_r2.usage_us = 0; // not asked yet
        T(r2_cap_now() == 2 * R2_GB);
        // Links are removed only for a space that's the player's or comes from a known total, never the 2 GB stand-in
        T(!r2_may_clear(-1, -1) && r2_may_clear(3, -1) && r2_may_clear(-1, 0) && r2_may_clear(-1, 5 * R2_GB));
        AcquireSRWLockExclusive(&g_r2.lock);
        T(!r2_may_clear_locked());
        g_r2.usage_us = 1;
        T(r2_may_clear_locked());
        g_r2.usage_us = 0;
        g_r2.space_gb = 3;
        T(r2_may_clear_locked());
        g_r2.space_gb = -1;
        ReleaseSRWLockExclusive(&g_r2.lock);
        // A cleanup while the space isn't known lists nothing, removes nothing and waits for the usage
        {
            i32 keep_count = g_r2.count;
            g_r2.cleanup_waiting = false;
            R2Task ct = {};
            ct.kind = R2_T_CLEANUP;
            r2_run_cleanup(ct);
            T(g_r2.cleanup_waiting && g_r2.count == keep_count);
            g_r2.cleanup_waiting = false;
        }
        g_r2.fetched = keep_f;
        g_r2.usage_us = keep_us;
        g_r2.budget = keep_budget;
        g_r2.space_gb = keep_space;
        g_r2.budget_known = keep_known;
    }
    T(r2_op_class("UploadPart") == 'A' && r2_op_class("HeadObject") == 'B' && r2_op_class("DeleteObject") == 0);
    char q[3200];
    r2_usage_query("acc1", "rekodr", "2026-11-01T00:00:00Z", "2026-11-21T10:00:00Z", "2026-11-20T23:00:00Z",
                   "2026-11-20T10:00:00Z", q, sizeof q);
    T(strstr(q, "objectName") && strstr(q, "\"get\":\"GetObject\"") && strstr(q, "\"dayStart\":\"2026-11-20T23:00:00Z\""));
    T(strstr(q, "\"since\":\"2026-11-20T10:00:00Z\"") && strstr(q, "day:r2Op") &&
      strstr(q, "account:r2OperationsAdaptiveGroups(limit:1000,filter:{datetime_geq:$startDate,datetime_leq:$endDate})"));
    T(strstr(q, "\"accountTag\":\"acc1\"") && strstr(q, "\"bucketName\":\"rekodr\"") &&
      strstr(q, "r2OperationsAdaptiveGroups") && strstr(q, "\"startDate\":\"2026-11-01T00:00:00Z\""));
    // Cloudflare orders only by a field the query asks for
    T(strstr(q, "orderBy:[datetime_DESC]){max{payloadSize metadataSize} dimensions{datetime}}"));
    // The account's storage: every bucket, newest first, by bucket
    T(strstr(q, "total:r2StorageAdaptiveGroups(limit:1000,filter:{datetime_geq:$startDate,datetime_leq:$endDate},"
                "orderBy:[datetime_DESC]){max{payloadSize metadataSize} dimensions{datetime bucketName}}"));

    // Plays per file
    const char *pl = "{\"data\":{\"viewer\":{\"accounts\":[{\"month\":[{\"sum\":{\"requests\":12},\"dimensions\":"
                     "{\"objectName\":\"rekodr/a/x.mp4\"}},{\"sum\":{\"requests\":3},\"dimensions\":{\"objectName\":"
                     "\"b.mp4\"}}],\"today\":[{\"sum\":{\"requests\":4},\"dimensions\":{\"objectName\":\"b.mp4\"}}]}]}}}";
    static char pk[4][R2_KEY_MAX];
    i64 pc[4];
    T(r2_parse_plays(pl, "month", pk, pc, 4) == 2 && !strcmp(pk[0], "rekodr/a/x.mp4") && pc[0] == 12 && pc[1] == 3);
    T(r2_parse_plays(pl, "today", pk, pc, 4) == 1 && !strcmp(pk[0], "b.mp4") && pc[0] == 4);
    T(r2_parse_plays(pl, "month", pk, pc, 1) == 1);

    // A month's cost past the free allowances
    T(r2_month_cost(5 * R2_GB, 100, 100) == 0);
    T(fabs(r2_month_cost(20 * R2_GB, 3000000, 20000000) - (0.15 + 9.0 + 3.6)) < 1e-9);

    T(r2_bucket_name_ok("rekodr") && r2_bucket_name_ok("my-clips-2"));
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
                        cb_code, sizeof cb_code, state, sizeof state) == R2_CB_CANCELLED);
    T(!strcmp(cb_code, "The user said no") && !strcmp(state, "s1"));
    T(r2_callback_parse("GET /favicon.ico HTTP/1.1", cb_code, sizeof cb_code, state, sizeof state) == R2_CB_OTHER);
    // Another error fails with Cloudflare's reason, on the page as text
    T(r2_callback_parse("GET /oauth/callback?error=invalid_scope&error_description=The+scope+%3Cx%3E+is+invalid&state=s1 "
                        "HTTP/1.1", cb_code, sizeof cb_code, state, sizeof state) == R2_CB_REFUSED);
    T(!strcmp(cb_code, "The scope <x> is invalid"));
    static char done[2048];
    r2_done_page(R2_CB_REFUSED, cb_code, done, sizeof done);
    T(strstr(done, "200 OK") && strstr(done, "The sign-in failed. Cloudflare said: The scope &lt;x&gt; is invalid<br>"));
    T(!strstr(done, "cancelled"));
    r2_done_page(R2_CB_CANCELLED, "The user said no", done, sizeof done);
    T(strstr(done, "The sign-in was cancelled.") && !strstr(done, "said no"));
    r2_done_page(R2_CB_CODE, "", done, sizeof done);
    T(strstr(done, "Signed in.") != 0);
    r2_done_page(R2_CB_OTHER, "", done, sizeof done);
    T(strstr(done, "404 Not Found") && strstr(done, "Content-Length: 0"));
    T(r2_callback_parse("GET /oauth/callback?state=s1 HTTP/1.1", cb_code, sizeof cb_code, state, sizeof state) == R2_CB_OTHER);
    static R2OAuth o;
    T(r2_parse_oauth("{\"access_token\":\"at1\",\"expires_in\":3599,\"refresh_token\":\"rt1\",\"scope\":\"account.read\","
                     "\"token_type\":\"bearer\"}",
                     &o));
    T(!strcmp(o.access, "at1") && !strcmp(o.refresh, "rt1") && o.expires_in == 3599 && !o.error[0]);
    T(!strcmp(o.scope, "account.read"));
    // The scopes the player allowed: whole words only
    T(r2_scope_has("account-settings.read workers-r2.write user-api-tokens.write", "user-api-tokens.write"));
    T(r2_scope_has("user-api-tokens.write", "user-api-tokens.write"));
    T(!r2_scope_has("account-settings.read workers-r2.write", "user-api-tokens.write"));
    T(!r2_scope_has("user-api-tokens.write2 xuser-api-tokens.write", "user-api-tokens.write"));
    T(!r2_scope_has("", "user-api-tokens.write"));
    T(!r2_parse_oauth("{\"error\":\"invalid_grant\",\"error_description\":\"expired\"}", &o));
    T(!strcmp(o.error, "invalid_grant"));
}

// The network as a cancelled share sees it: each request is noted, and the one named cancels the job as it goes.
static struct {
    char        log[32][200];
    i32         n;
    const char *cancel_on; // the method whose request cancels the job, 0 = none
    u32         job;
    i32         fail;      // this many requests from now answer fail_status
    u32         fail_status;
    const char *list;      // the bucket listing's answer, 0 = an empty bucket
} g_net;

static u32 net_fake(const char *method, const char *where, const char *query, R2Progress *, char *resp, i32 cap,
                    char *etag, i32 etag_cap)
{
    if (g_net.n < COUNT(g_net.log))
        snprintf(g_net.log[g_net.n++], sizeof g_net.log[0], "%s %s?%s", method, where, query);
    if (g_net.cancel_on && !strcmp(method, g_net.cancel_on)) {
        AcquireSRWLockExclusive(&g_r2.lock);
        for (i32 i = 0; i < COUNT(g_r2.jobs); i++)
            if (g_r2.jobs[i].id == g_net.job) g_r2.cancel[i] = 1;
        ReleaseSRWLockExclusive(&g_r2.lock);
    }
    if (g_net.fail > 0) {
        g_net.fail--;
        if (cap) resp[0] = 0;
        return g_net.fail_status;
    }
    const char *body = "";
    if (!strcmp(method, "POST") && !strcmp(query, "uploads=")) body = "<UploadId>u1</UploadId>";
    else if (!strcmp(method, "POST") && strstr(where, "up.example"))
        body = "{\"url\":\"https://up.example/f/1.mp4\",\"del\":\"https://up.example/del/1\"}";
    else if (!strcmp(method, "GET") && strstr(query, "list-type"))
        body = g_net.list ? g_net.list : "<ListBucketResult></ListBucketResult>";
    if (cap) snprintf(resp, cap, "%s", body);
    if (etag_cap) snprintf(etag, etag_cap, "\"e\"");
    return 200;
}

static i32 net_count(const char *prefix)
{
    i32 n = 0;
    for (i32 i = 0; i < g_net.n; i++) n += starts_with(g_net.log[i], prefix);
    return n;
}

// Cancelling a link: the upload stops and nothing it made is left, on this PC or on the host.
// Which links count as ready to share: one that plays for the clip as it is. A link cleanup removed, one gone from
// the bucket, an uploader's past its time, or one made before the clip changed is not.
static void test_share_ready()
{
    Config &cf = g_app.config;
    f32 keep_days = cf.share_keep_days;
    cf.share_keep_days = 7;
    i64 now = now_us(), day = 86400ll * 1000000;

    // Live: an R2 link plays until removed or paused; an uploader's only for share_keep_days
    R2Share s = {};
    s.host = HOST_R2;
    s.uploaded_us = now - 30 * day;
    T(r2_share_live(s, now));
    s.removed = true;
    T(!r2_share_live(s, now));
    s.removed = false;
    s.paused = 1;
    T(!r2_share_live(s, now));
    s.paused = 0;
    s.host = HOST_CUSTOM;
    T(!r2_share_live(s, now));
    s.uploaded_us = now - day;
    T(r2_share_live(s, now));

    // A full listing: a link whose file isn't there is marked removed; one listed, one moving, one put up after
    // the listing began and an uploader's are left alone. A paused link is looked for under its hidden name.
    static R2Share sh[6];
    memset(sh, 0, sizeof sh);
    for (i32 i = 0; i < COUNT(sh); i++) {
        sh[i].host = HOST_R2;
        snprintf(sh[i].key, sizeof sh[i].key, "rekodr/k%d/clip.mp4", i);
        sh[i].uploaded_us = now - day;
    }
    sh[2].moving = true;
    sh[3].uploaded_us = now + 1;
    sh[4].host = HOST_CUSTOM;
    sh[5].paused = 1;
    str_set(sh[5].hidden, "rekodr/hidden5/clip.mp4");
    u64 listed[] = {fnv64(sh[0].key, strlen(sh[0].key)), fnv64(sh[5].key, strlen(sh[5].key))};
    qsort(listed, COUNT(listed), sizeof listed[0], r2_cmp_u64);
    T(r2_mark_missing(sh, COUNT(sh), listed, COUNT(listed), now) == 2);
    T(!sh[0].removed && sh[1].removed && !sh[2].removed && !sh[3].removed && !sh[4].removed);
    T(sh[5].removed && !sh[5].paused && !sh[5].hidden[0]); // its hidden name wasn't listed

    // Through the made list and the history
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    Paths keep_paths = g_app.paths;
    swprintf(g_app.paths.local_dir.s, MAX_PATH, L"%srekodr-tests-ready-%lu", temp, GetCurrentProcessId());
    CreateDirectoryW(g_app.paths.local_dir.s, 0);
    static R2Share keep_shares[R2_MAX_SHARES];
    memcpy(keep_shares, g_r2.shares, sizeof keep_shares);
    i32 keep_count = g_r2.count;
    bool keep_loaded = g_r2.loaded;
    g_r2.loaded = true;
    g_r2.count = 3;
    R2Share *h = g_r2.shares;
    memset(h, 0, 3 * sizeof *h);
    const char *a = "https://clips.example/a", *b = "https://clips.example/b", *c = "https://up.example/c";
    h[0].host = HOST_R2, h[0].lib_key = 5, h[0].uploaded_us = now - 30 * day, str_set(h[0].url, a);
    h[1].host = HOST_R2, h[1].lib_key = 6, h[1].uploaded_us = now, h[1].removed = true, str_set(h[1].url, b);
    h[2].host = HOST_CUSTOM, h[2].lib_key = 7, h[2].uploaded_us = now - 30 * day, str_set(h[2].url, c);
    u64 src = 0x1234;
    g_made = {};
    g_made.loaded = true;
    auto made = [&](u64 lib_key, i64 at, const char *what) {
        ShareMade &m = g_made.items[g_made.count++];
        m = {};
        m.hash = g_made.count;
        m.lib_key = lib_key;
        m.made_us = at;
        m.source = src;
        str_set(m.what, what);
    };
    made(5, now - 30 * day, a); // old, but the link plays: kept
    made(6, now, b);            // removed by cleanup: dropped
    made(7, now - 30 * day, c); // an uploader's, past its time: dropped
    share_made_load();
    T(g_made.count == 1 && !strcmp(g_made.items[0].what, a));
    char url[512];
    T(share_ready_link(5, src, url, sizeof url) && !strcmp(url, a));
    T(!share_ready_link(5, src + 1, url, sizeof url)); // the clip changed since
    T(!share_ready_link(0, src, url, sizeof url));     // not a library clip
    T(!share_ready_link(6, src, url, sizeof url));
    h[0].paused = 1;
    T(!share_ready_link(5, src, url, sizeof url) && r2_url_state(a) == R2_URL_PAUSED);
    h[0].paused = 0;
    h[0].removed = true; // cleanup took it
    T(!share_ready_link(5, src, url, sizeof url) && r2_url_state(a) == R2_URL_GONE);
    T(r2_url_state("https://clips.example/unknown") == R2_URL_GONE);

    g_made = {};
    memcpy(g_r2.shares, keep_shares, sizeof keep_shares);
    g_r2.count = keep_count;
    g_r2.loaded = keep_loaded;
    DeleteFileW(path_join(g_app.paths.local_dir, L"made-shares.tsv").s);
    RemoveDirectoryW(g_app.paths.local_dir.s);
    g_app.paths = keep_paths;
    cf.share_keep_days = keep_days;
}

static void test_share_cancel()
{
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    Paths keep = g_app.paths;
    swprintf(g_app.paths.config_dir.s, MAX_PATH, L"%srekodr-tests-cancel-%lu", temp, GetCurrentProcessId());
    CreateDirectoryW(g_app.paths.config_dir.s, 0);
    g_r2.count = 0;
    g_r2.loaded = false;
    r2_net_fake = net_fake;
    R2Conf conf = {"acct", "key", "secret", "bucket", "https://clips.example"};
    // A cap far above these files: uploads here never clear room
    f32 keep_space = g_r2.space_gb;
    bool keep_known = g_r2.budget_known;
    g_r2.space_gb = 1000;
    g_r2.budget_known = true;
    Path file = path_join(g_app.paths.config_dir, L"copy.mp4");
    auto make = [&](i64 bytes) { // sized without writing it all
        HANDLE h = CreateFileW(file.s, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0, 0);
        LARGE_INTEGER at;
        at.QuadPart = bytes;
        SetFilePointerEx(h, at, 0, FILE_BEGIN);
        SetEndOfFile(h);
        CloseHandle(h);
    };
    auto job = [&](i32 *slot) {
        *slot = r2_new_job();
        g_net = {};
        g_net.job = g_r2.jobs[*slot].id;
        return g_net.job;
    };
    i32 slot;
    char err[256];

    // In parts: cancelled during the first, the rest never go and the upload is aborted, its parts dropped
    u32 id = job(&slot);
    make(R2_SINGLE_MAX + R2_PART);
    g_net.cancel_on = "PUT";
    T(!r2_put_file(conf, "rekodr/k/clip.mp4", file, id, err, sizeof err) && !strcmp(err, "Cancelled"));
    T(net_count("PUT ") == 1 && net_count("DELETE rekodr/k/clip.mp4?uploadId=u1") == 1);
    T(net_count("POST rekodr/k/clip.mp4?uploadId=") == 0); // never completed

    // To the bucket, cancelled as the file went up: it's deleted there, the copy here, and no link is kept
    R2Task t = {};
    t.kind = R2_T_UPLOAD;
    t.conf = conf;
    t.file = file;
    t.slot = slot;
    id = job(&slot);
    t.slot = slot;
    make(1000);
    g_net.cancel_on = "PUT";
    r2_run_upload(t);
    R2Job r = r2_job(id);
    T(r.state == R2_FAILED && !strcmp(r.error, "Cancelled") && !r.url[0]);
    T(!file_exists(file.s) && g_r2.count == 0);
    T(net_count("PUT rekodr/") == 1 && net_count("DELETE rekodr/") == 1);

    // Cancelled while waiting its turn: nothing is sent and the copy goes, with its small copy
    id = job(&slot);
    t.slot = slot;
    make(1000);
    t.social = path_join(g_app.paths.config_dir, L"social.mp4");
    CloseHandle(CreateFileW(t.social.s, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0, 0));
    g_r2.cancel[slot] = 1;
    r2_run_upload(t);
    T(g_net.n == 0 && !file_exists(file.s) && !file_exists(t.social.s) && r2_job(id).state == R2_FAILED);
    t.social = {};

    // A share's set: the clip, its small copy, the picture and the page
    g_net = {};
    T(r2_delete_share(conf, "rekodr/k/clip.mp4", "rekodr/k/clip.mp4", true, true));
    T(net_count("DELETE rekodr/k/clip.mp4") == 1 && net_count("DELETE rekodr/k/social.mp4") == 1);
    T(net_count("DELETE rekodr/k/thumb.jpg") == 1 && net_count("DELETE rekodr/k") == 4);

    // A custom uploader with a DeletionURL, cancelled as it answered: what went up is deleted there
    Path dir = path_join(g_app.paths.config_dir, L"uploaders"), sxcu = path_join(dir, L"mine.sxcu");
    CreateDirectoryW(dir.s, 0);
    FILE *f = _wfopen(sxcu.s, L"wb");
    fputs("{\"Name\":\"mine\",\"RequestMethod\":\"POST\",\"RequestURL\":\"https://up.example/upload\","
          "\"Body\":\"MultipartFormData\",\"FileFormName\":\"file\",\"URL\":\"{json:url}\","
          "\"DeletionURL\":\"{json:del}\"}", f);
    fclose(f);
    t.kind = R2_T_UPLOADER;
    str_set(t.hosts, "mine");
    id = job(&slot);
    t.slot = slot;
    make(1000);
    g_net.cancel_on = "POST";
    r2_run_uploader(t);
    r = r2_job(id);
    T(r.state == R2_FAILED && !strcmp(r.error, "Cancelled") && !file_exists(file.s) && g_r2.count == 0);
    T(net_count("GET https://up.example/del/1") == 1);

    // Quiet retries: a server error or no answer goes again, up to three more times; a refusal doesn't
    g_net.cancel_on = 0;
    g_net.n = 0;
    g_net.fail = 2;
    g_net.fail_status = 503;
    T(r2_put_bytes(conf, "rekodr/r/thumb.jpg", "x", 1, "image/jpeg") && net_count("PUT rekodr/r/thumb.jpg") == 3);
    g_net.n = 0;
    g_net.fail = 4;
    g_net.fail_status = 0;
    T(!r2_put_bytes(conf, "rekodr/r/thumb.jpg", "x", 1, "image/jpeg") && net_count("PUT rekodr/r/thumb.jpg") == 4);
    g_net.n = 0;
    g_net.fail = 1;
    g_net.fail_status = 403;
    T(!r2_put_bytes(conf, "rekodr/r/thumb.jpg", "x", 1, "image/jpeg") && net_count("PUT rekodr/r/thumb.jpg") == 1);
    id = job(&slot);
    g_net.fail = 2;
    g_net.fail_status = 502;
    t.slot = slot;
    make(1000);
    r2_run_uploader(t);
    T(r2_job(id).state == R2_DONE && net_count("POST https://up.example/upload") == 3);
    g_net.fail = 0;
    g_r2.count = 0;

    // Done just before the cancel: the job hands over what to delete and ends as cancelled
    id = job(&slot);
    T(r2_job_end(id, true, "https://x/1", "", "rekodr/abc", ""));
    T(r2_job(id).state == R2_DONE && !strcmp(r2_job(id).key, "rekodr/abc"));
    id = job(&slot);
    g_r2.cancel[slot] = 1;
    T(!r2_job_end(id, true, "https://x/2", "", "rekodr/def", "") && r2_job(id).state == R2_FAILED &&
      !r2_job(id).key[0]);

    // Cleanup over a 3 GB link: with the space unknown and none saved nothing is listed or removed; a saved space
    // or a known total clears it as before
    auto old_link = [&]() {
        g_r2.count = 1;
        g_r2.shares[0] = {};
        str_set(g_r2.shares[0].key, "rekodr/old/clip.mp4");
        g_r2.shares[0].host = HOST_R2;
        g_r2.shares[0].bytes = 3 * R2_GB;
        g_r2.shares[0].uploaded_us = 1;
        g_net = {};
        g_net.list = "<ListBucketResult><Contents><Key>rekodr/old/clip.mp4</Key><Size>3000000000</Size></Contents>"
                     "</ListBucketResult>";
    };
    R2Fetched keep_fetched = g_r2.fetched;
    i64 keep_usage = g_r2.usage_us;
    R2Task ct = {};
    ct.kind = R2_T_CLEANUP;
    ct.conf = conf;
    old_link();
    g_r2.space_gb = -1;
    g_r2.usage_us = 0;
    r2_run_cleanup(ct);
    T(g_net.n == 0 && !g_r2.shares[0].removed && g_r2.cleanup_waiting);
    old_link();
    g_r2.space_gb = 1;
    r2_run_cleanup(ct);
    T(net_count("DELETE rekodr/old/clip.mp4") == 1 && g_r2.shares[0].removed && !g_r2.cleanup_waiting);
    old_link();
    g_r2.space_gb = -1;
    g_r2.usage_us = 1;
    g_r2.fetched = {};
    g_r2.fetched.bytes = 3 * R2_GB;
    g_r2.fetched.account_bytes = 11 * R2_GB; // 8 GB in other buckets: 1.5 GB for Rekodr
    r2_run_cleanup(ct);
    T(net_count("DELETE rekodr/old/clip.mp4") == 1 && g_r2.shares[0].removed);
    g_r2.fetched = keep_fetched;
    g_r2.usage_us = keep_usage;
    g_r2.count = 0;

    r2_net_fake = 0;
    DeleteFileW(sxcu.s);
    RemoveDirectoryW(dir.s);
    DeleteFileW(path_join(g_app.paths.config_dir, L"shares.tsv").s);
    RemoveDirectoryW(g_app.paths.config_dir.s);
    T(GetFileAttributesW(g_app.paths.config_dir.s) == INVALID_FILE_ATTRIBUTES); // nothing else was left in it
    g_r2.count = 0;
    g_r2.loaded = false;
    g_r2.space_gb = keep_space;
    g_r2.budget_known = keep_known;
    g_app.paths = keep;
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
    swprintf(root.s, MAX_PATH, L"%srekodr-mcpack-%lu", tmp, GetCurrentProcessId());
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
    T(!strstr(body, "My Own") && !strstr(body, "rekodr-mcpack"));
    T(!mc_clues("javaw -jar notagame.jar --gameDir C:/x", body, sizeof body));

    // The service's answer for a pack its launcher named: the pack, keyed by its platform's project
    static const char answer[] = "{\"game\":{\"key\":\"p:modrinth:1KVo5zza\",\"name\":\"Fabulously Optimized\"},\"matched_by\":\"mc_pack\","
                                 "\"confidence\":0.95,\"art\":{\"icon\":null,\"hero\":null,\"cover\":{\"url\":\"https://a/g.png\"}},\"v\":\"3\"}";
    LookAnswer ans;
    T(lookup_parse(answer, sizeof answer - 1, &ans) && !strcmp(ans.name, "Fabulously Optimized") &&
      !strcmp(ans.key, "p:modrinth:1KVo5zza") && !strcmp(ans.how, "mc_pack"));
    char kf[48];
    key_file_id(ans.key, kf, sizeof kf);
    T(!strcmp(kf, "p_modrinth_1KVo5zza"));

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
// A Counter-Strike 2 state as the game sends it, cut down to what cs2_marks reads.
static i32 cs2_feed(Cs2Seen *s, const char *watched, const char *map_phase, const char *round_phase, const char *win,
                    i32 kills, i32 deaths, i32 round_kills, i32 ct, i32 t, AutoMark *out, u32 events = AM_GAMES[AMG_CS2].events)
{
    char body[2048];
    snprintf(body, sizeof body,
             "{\"provider\":{\"name\":\"Counter-Strike 2\",\"appid\":730,\"steamid\":\"7656\"},"
             "\"map\":{\"name\":\"de_dust2\",\"phase\":\"%s\",\"team_ct\":{\"score\":%d},\"team_t\":{\"score\":%d}},"
             "\"round\":{\"phase\":\"%s\"%s%s%s},"
             "\"player\":{\"steamid\":\"%s\",\"team\":\"CT\",\"state\":{\"health\":100,\"round_kills\":%d},"
             "\"match_stats\":{\"kills\":%d,\"assists\":0,\"deaths\":%d}},"
             "\"auth\":{\"token\":\"x\"}}",
             map_phase, ct, t, round_phase, win[0] ? ",\"win_team\":\"" : "", win, win[0] ? "\"" : "", watched,
             round_kills, kills, deaths);
    return cs2_marks(s, events, body, (i64)strlen(body), out, 8);
}

static void test_automarks()
{
    AutoMark m[16];
    Cs2Seen s = {};
    T(cs2_feed(&s, "7656", "live", "live", "", 3, 1, 0, 0, 0, m) == 0); // the first state only says where it is
    T(cs2_feed(&s, "7656", "live", "live", "", 4, 1, 1, 0, 0, m) == 1 && m[0].colour == 4 && !strcmp(m[0].note, "Kill"));
    T(cs2_feed(&s, "7656", "live", "live", "", 5, 1, 2, 0, 0, m) == 1 && !strcmp(m[0].note, "Double kill"));
    T(cs2_feed(&s, "7656", "live", "live", "", 5, 2, 2, 0, 0, m) == 1 && m[0].colour == 1 && !strcmp(m[0].note, "Death"));
    T(cs2_feed(&s, "7657", "live", "live", "", 9, 0, 3, 0, 0, m) == 0); // watching a teammate: nothing
    T(cs2_feed(&s, "7656", "live", "over", "CT", 5, 2, 2, 1, 0, m) == 1 && m[0].colour == 3 && !strcmp(m[0].note, "Round won"));
    T(cs2_feed(&s, "7656", "live", "over", "CT", 5, 2, 2, 1, 0, m) == 0); // the same round again
    T(cs2_feed(&s, "7656", "live", "live", "", 5, 2, 0, 1, 0, m) == 0);
    T(cs2_feed(&s, "7656", "live", "over", "T", 5, 2, 0, 1, 1, m) == 0); // the other team's round
    T(cs2_feed(&s, "7656", "gameover", "over", "CT", 5, 2, 0, 13, 9, m) == 1 && !strcmp(m[0].note, "Match won"));
    Cs2Seen w = {};
    T(cs2_feed(&w, "7656", "warmup", "live", "", 0, 0, 0, 0, 0, m) == 0);
    T(cs2_feed(&w, "7656", "warmup", "live", "", 2, 1, 2, 0, 0, m) == 0); // warmup kills don't count
    T(cs2_marks(&w, AM_GAMES[AMG_CS2].events, "{\"provider\":{\"steamid\":\"7656\"}}", 33, m, 8) == 0 && !w.have); // in the menus
    // Only some events: multi-kills alone skip plain kills; kills alone mark a double as a Kill; no deaths, no rounds
    Cs2Seen e = {};
    T(cs2_feed(&e, "7656", "live", "live", "", 0, 0, 0, 0, 0, m, AM_MULTI) == 0);
    T(cs2_feed(&e, "7656", "live", "live", "", 1, 0, 1, 0, 0, m, AM_MULTI) == 0);
    T(cs2_feed(&e, "7656", "live", "live", "", 2, 0, 2, 0, 0, m, AM_MULTI) == 1 && !strcmp(m[0].note, "Double kill"));
    T(cs2_feed(&e, "7656", "live", "live", "", 3, 1, 3, 0, 0, m, AM_KILL) == 1 && !strcmp(m[0].note, "Kill"));
    T(cs2_feed(&e, "7656", "live", "over", "CT", 3, 1, 3, 1, 0, m, AM_WIN) == 0);
    T(cs2_feed(&e, "7656", "live", "live", "", 3, 1, 0, 1, 0, m, AM_ROUND) == 0);
    T(cs2_feed(&e, "7656", "gameover", "over", "CT", 3, 1, 0, 13, 0, m, AM_ROUND) == 1 && !strcmp(m[0].note, "Round won"));

    LolSeen l = {};
    str_set(l.me, "Sam#EUW");
    const char *start = "{\"Events\":[{\"EventID\":0,\"EventName\":\"GameStart\",\"EventTime\":0.02}]}";
    T(lol_marks(&l, AM_GAMES[AMG_LOL].events, start, (i64)strlen(start), m, 16) == 0 && l.have && l.last_id == 0);
    const char *more =
        "{\"Events\":[{\"EventID\":0,\"EventName\":\"GameStart\",\"EventTime\":0.02},"
        "{\"EventID\":1,\"EventName\":\"ChampionKill\",\"EventTime\":300.1,\"VictimName\":\"Other\",\"KillerName\":\"Sam\",\"Assisters\":[\"A\",\"B\"]},"
        "{\"EventID\":2,\"EventName\":\"ChampionKill\",\"EventTime\":302.5,\"VictimName\":\"Third\",\"KillerName\":\"sam\",\"Assisters\":[]},"
        "{\"EventID\":3,\"EventName\":\"Multikill\",\"EventTime\":302.5,\"KillerName\":\"Sam\",\"KillStreak\":2},"
        "{\"EventID\":4,\"EventName\":\"ChampionKill\",\"EventTime\":400,\"VictimName\":\"Sam#EUW\",\"KillerName\":\"Other\",\"Assisters\":[]},"
        "{\"EventID\":5,\"EventName\":\"ChampionKill\",\"EventTime\":401,\"VictimName\":\"Samuel\",\"KillerName\":\"Other\",\"Assisters\":[]}]}";
    i32 n = lol_marks(&l, AM_GAMES[AMG_LOL].events, more, (i64)strlen(more), m, 16);
    T(n == 4 && !strcmp(m[0].note, "Kill") && !m[1].amend && m[2].amend && !strcmp(m[2].note, "Double kill") &&
      m[3].colour == 1 && !strcmp(m[3].note, "Death"));
    T(lol_marks(&l, AM_GAMES[AMG_LOL].events, more, (i64)strlen(more), m, 16) == 0 && l.last_id == 5); // nothing new
    const char *end = "{\"Events\":[{\"EventID\":6,\"EventName\":\"GameEnd\",\"EventTime\":1500,\"Result\":\"Win\"}]}";
    T(lol_marks(&l, AM_GAMES[AMG_LOL].events, end, (i64)strlen(end), m, 16) == 1 && m[0].colour == 3 && !strcmp(m[0].note, "Win"));
    T(lol_marks(&l, AM_GAMES[AMG_LOL].events, start, (i64)strlen(start), m, 16) == 0 && l.last_id == 0); // a new match
    LolSeen lm = {};
    str_set(lm.me, "Sam#EUW");
    lol_marks(&lm, AM_MULTI, start, (i64)strlen(start), m, 16);
    T(lol_marks(&lm, AM_MULTI, more, (i64)strlen(more), m, 16) == 1 && !m[0].amend && !strcmp(m[0].note, "Double kill"));
    const char *obj =
        "{\"Events\":[{\"EventID\":6,\"EventName\":\"DragonKill\",\"EventTime\":600,\"DragonType\":\"Fire\",\"Stolen\":\"False\",\"KillerName\":\"Sam\",\"Assisters\":[]},"
        "{\"EventID\":7,\"EventName\":\"TurretKilled\",\"EventTime\":610,\"TurretKilled\":\"Turret_T2_L_03_A\",\"KillerName\":\"Minion_T100L0S0N0001\",\"Assisters\":[]},"
        "{\"EventID\":8,\"EventName\":\"BaronKill\",\"EventTime\":1300,\"Stolen\":\"False\",\"KillerName\":\"Sam\",\"Assisters\":[]}]}";
    LolSeen lo = lm;
    T(lol_marks(&lo, AM_GAMES[AMG_LOL].events, obj, (i64)strlen(obj), m, 16) == 2 && m[0].colour == 5 && !strcmp(m[0].note, "Dragon") &&
      !strcmp(m[1].note, "Baron"));
    lo = lm;
    T(lol_marks(&lo, AM_GAMES[AMG_LOL].events & ~AM_OBJECTIVE, obj, (i64)strlen(obj), m, 16) == 0);
    // Saved as words; words a game doesn't have are dropped
    T(am_events_parse("kill, death,round,bogus", AM_GAMES[AMG_LOL].events) == (AM_KILL | AM_DEATH) && am_events_parse("", AM_GAMES[AMG_CS2].events) == 0);
    char words[96];
    am_events_text(AM_KILL | AM_WIN, words, sizeof words);
    T(!strcmp(words, "kill,win") && am_events_parse(words, AM_GAMES[AMG_CS2].events) == (AM_KILL | AM_WIN));

    char cfg[2048];
    gsi_cfg_text(AM_GSI[0], "abc123", cfg, sizeof cfg);
    T(strstr(cfg, "\"uri\"       \"http://127.0.0.1:29771/\"") && strstr(cfg, "\"token\" \"abc123\"") && strstr(cfg, "\"player_match_stats\" \"1\"") &&
      !strcmp(cfg + strlen(cfg) - 8, "    }\n}\n"));
    gsi_cfg_text(AM_GSI[1], "abc123", cfg, sizeof cfg);
    T(AM_GSI[1].game == AMG_DOTA2 && strstr(cfg, "\"player\" \"1\"") && !strstr(cfg, "\"round\""));

    // Dota 2: the player's own kills, deaths and assists, and a won match; a spectated match has no player kills
    auto dota = [](DotaSeen *s, const char *match, const char *state, i32 k, i32 d, i32 a, const char *win, AutoMark *out,
                   u32 events = AM_GAMES[AMG_DOTA2].events) {
        char body[512];
        snprintf(body, sizeof body, "{\"provider\":{\"appid\":570},\"map\":{\"matchid\":\"%s\",\"game_state\":\"%s\","
                 "\"win_team\":\"%s\"},\"player\":{\"team_name\":\"radiant\",\"kills\":%d,\"deaths\":%d,\"assists\":%d}}",
                 match, state, win, k, d, a);
        return dota_marks(s, events, body, (i64)strlen(body), out, 8);
    };
    DotaSeen ds = {};
    const char *live = "DOTA_GAMERULES_STATE_GAME_IN_PROGRESS";
    T(dota(&ds, "77", live, 0, 0, 0, "none", m) == 0 && ds.have);
    T(dota(&ds, "77", live, 1, 0, 1, "none", m) == 2 && !strcmp(m[0].note, "Kill") && m[0].colour == 4 && !strcmp(m[1].note, "Assist"));
    T(dota(&ds, "77", live, 1, 1, 1, "none", m) == 1 && !strcmp(m[0].note, "Death") && m[0].colour == 1);
    T(dota(&ds, "77", "DOTA_GAMERULES_STATE_POST_GAME", 1, 1, 1, "radiant", m) == 1 && !strcmp(m[0].note, "Win") && m[0].colour == 3);
    T(dota(&ds, "77", "DOTA_GAMERULES_STATE_POST_GAME", 1, 1, 1, "radiant", m) == 0); // said once
    T(dota(&ds, "78", live, 5, 5, 5, "none", m) == 0); // a new match only says where it is up to
    T(dota(&ds, "78", live, 6, 5, 5, "none", m, AM_DEATH) == 0);
    const char *watch = "{\"provider\":{\"appid\":570},\"map\":{\"matchid\":\"9\"},\"player\":{\"team2\":{}}}";
    T(dota_marks(&ds, AM_GAMES[AMG_DOTA2].events, watch, (i64)strlen(watch), m, 8) == 0 && !ds.have);
    T(am_events_parse("assist", AM_GAMES[AMG_DOTA2].events) == AM_ASSIST && am_events_parse("assist", AM_GAMES[AMG_CS2].events) == 0);

    // Minecraft's latest.log
    u32 mce = AM_GAMES[AMG_MINECRAFT].events;
    McSeen mc = {};
    T(mc_line(&mc, mce, "[12:00:00] [Render thread/INFO]: Setting user: Steve", true, m, 8) == 0 && !strcmp(mc.me, "Steve"));
    T(mc_line(&mc, mce, "[12:00:01] [Render thread/INFO]: [System] [CHAT] Steve was slain by Zombie", true, m, 8) == 0);
    T(mc_line(&mc, mce, "[12:00:02] [Render thread/INFO]: [System] [CHAT] Steve was slain by Zombie", false, m, 8) == 1 &&
      !strcmp(m[0].note, "Death") && m[0].colour == 1);
    T(mc_line(&mc, mce, "[12:00:03] [Render thread/INFO]: [System] [CHAT] Steve has made the advancement [Stone Age]", false, m, 8) == 1 &&
      !strcmp(m[0].note, "Stone Age") && m[0].colour == 6);
    T(mc_line(&mc, mce, "[12:00:04] [Render thread/INFO]: [System] [CHAT] Alex has made the advancement [Stone Age]", false, m, 8) == 0);
    T(mc_line(&mc, mce, "[12:00:05] [Render thread/INFO]: [System] [CHAT] Alex was shot by Steve", false, m, 8) == 1 && !strcmp(m[0].note, "Kill"));
    T(mc_line(&mc, mce, "[12:00:06] [Render thread/INFO]: [System] [CHAT] Alex was shot by Steven", false, m, 8) == 0);
    T(mc_line(&mc, mce, "[12:00:07] [Render thread/INFO]: [System] [CHAT] <Alex> Steve was slain by me", false, m, 8) == 0);
    T(mc_line(&mc, AM_KILL, "[12:00:08] [Render thread/INFO]: [System] [CHAT] Steve drowned", false, m, 8) == 0);

    // Path of Exile's Client.txt
    u32 poee = AM_GAMES[AMG_POE].events;
    PoeSeen poe = {};
    T(poe_line(&poe, poee, "2026/11/26 12:00:00 1 abc [INFO Client 1] : Grub (Marauder) is now level 4", true, m, 8) == 0 &&
      !strcmp(poe.me, "Grub"));
    T(poe_line(&poe, poee, "2026/11/26 12:00:01 1 abc [INFO Client 1] : Grub (Marauder) is now level 5", false, m, 8) == 1 &&
      !strcmp(m[0].note, "Level 5"));
    T(poe_line(&poe, poee, "2026/11/26 12:00:02 1 abc [INFO Client 1] : Grub has been slain.", false, m, 8) == 1 && !strcmp(m[0].note, "Death"));
    T(poe_line(&poe, poee, "2026/11/26 12:00:03 1 abc [INFO Client 1] : Grubby has been slain.", false, m, 8) == 0);
    T(poe_line(&poe, poee, "2026/11/26 12:00:04 1 abc [INFO Client 1] : #Grub: has been slain.", false, m, 8) == 0);

    // Team Fortress 2's console.log
    u32 tfe = AM_GAMES[AMG_TF2].events;
    Tf2Seen tf = {};
    str_set(tf.me, "Pyro Main");
    T(tf2_line(&tf, tfe, "Pyro Main killed Scout Guy with flamethrower.", false, m, 8) == 1 && !strcmp(m[0].note, "Kill"));
    T(tf2_line(&tf, tfe, "Pyro Main killed Scout Guy with flamethrower. (crit)", false, m, 8) == 1);
    T(tf2_line(&tf, tfe, "Scout Guy killed Pyro Main with scattergun.", false, m, 8) == 1 && !strcmp(m[0].note, "Death"));
    T(tf2_line(&tf, tfe, "Pyro Main suicided.", false, m, 8) == 1 && !strcmp(m[0].note, "Death"));
    T(tf2_line(&tf, tfe, "Scout Guy killed Pyro Mainer with scattergun.", false, m, 8) == 0);
    T(tf2_line(&tf, tfe, "Pyro Main killed Scout Guy with flamethrower.", true, m, 8) == 0);

    // F1 25: only the player's car's events, of packet format 2025
    u32 f1e = AM_GAMES[AMG_F1_25].events;
    auto f1 = [&](const char *code, u8 a, u8 b, u8 c, AutoMark *out, u32 ev) {
        u8 p[45] = {};
        p[0] = 2025 & 255, p[1] = 2025 >> 8, p[6] = 3, p[27] = 7;
        memcpy(p + 29, code, 4);
        p[33] = a, p[34] = b, p[35] = c;
        return f1_packet(ev, p, sizeof p, out, 8);
    };
    T(f1("FTLP", 7, 0, 0, m, f1e) == 1 && !strcmp(m[0].note, "Fastest lap") && m[0].colour == 6);
    T(f1("FTLP", 3, 0, 0, m, f1e) == 0);
    T(f1("OVTK", 7, 2, 0, m, f1e) == 1 && !strcmp(m[0].note, "Overtake"));
    T(f1("OVTK", 2, 7, 0, m, f1e) == 0); // overtaken
    T(f1("COLL", 1, 7, 0, m, f1e) == 1 && !strcmp(m[0].note, "Contact"));
    T(f1("PENA", 4, 0, 7, m, f1e) == 1 && !strcmp(m[0].note, "Penalty"));
    T(f1("PENA", 5, 0, 7, m, f1e) == 0); // a warning
    T(f1("RCWN", 7, 0, 0, m, f1e) == 1 && !strcmp(m[0].note, "Race won"));
    T(f1("RCWN", 7, 0, 0, m, f1e & ~AM_WIN) == 0);
    {
        u8 p[45] = {};
        p[0] = 2024 & 255, p[1] = 2024 >> 8, p[6] = 3, p[27] = 7;
        memcpy(p + 29, "RCWN", 4);
        p[33] = 7;
        T(f1_packet(f1e, p, sizeof p, m, 8) == 0); // another year's format
    }

    // iRacing: a telemetry file with one buffer and five vars, and a YAML session list
    {
        static u8 ir[8192];
        memset(ir, 0, sizeof ir);
        auto put = [&](i64 at, i32 v) { memcpy(ir + at, &v, 4); };
        const char *yaml = "SessionInfo:\n Sessions:\n - SessionNum: 0\n   SessionType: Practice\n - SessionNum: 1\n   SessionType: Race\n";
        put(4, 1), put(16, (i32)strlen(yaml)), put(20, 7000), memcpy(ir + 7000, yaml, strlen(yaml));
        const char *names[] = {"SessionNum", "SessionUniqueID", "PlayerCarMyIncidentCount", "SessionState", "PlayerCarPosition", "LapBestLapTime"};
        put(24, 6), put(28, 200), put(32, 1), put(48, 5), put(52, 4000);
        for (i32 i = 0; i < 6; i++) {
            put(200 + i * 144, i == 5 ? 4 : 2), put(204 + i * 144, i * 4);
            strcpy((char *)ir + 216 + i * 144, names[i]);
        }
        auto var = [&](i32 i, i32 v) { put(4000 + i * 4, v); };
        auto lap = [&](f32 v) { memcpy(ir + 4000 + 20, &v, 4); };
        u32 ire = AM_GAMES[AMG_IRACING].events;
        IrSeen is = {};
        var(0, 1), var(1, 9), var(2, 0), var(3, 4), var(4, 3), lap(0);
        T(ir_marks(&is, ire, ir, sizeof ir, m, 8) == 0 && is.have);
        lap(92.5f);
        T(ir_marks(&is, ire, ir, sizeof ir, m, 8) == 0); // the first timed lap
        var(2, 4), lap(91.f);
        T(ir_marks(&is, ire, ir, sizeof ir, m, 8) == 2 && !strcmp(m[0].note, "Incident 4x") && !strcmp(m[1].note, "Best lap"));
        lap(93.f);
        T(ir_marks(&is, ire, ir, sizeof ir, m, 8) == 0 && is.best == 91.f);
        var(3, 5), var(4, 1);
        T(ir_marks(&is, ire, ir, sizeof ir, m, 8) == 1 && !strcmp(m[0].note, "Race won"));
        T(ir_marks(&is, ire, ir, sizeof ir, m, 8) == 0); // said once
        var(0, 0), var(3, 4);
        T(ir_marks(&is, ire, ir, sizeof ir, m, 8) == 0 && is.session == 0 && !is.won_said); // another session
        var(3, 5);
        T(ir_marks(&is, ire, ir, sizeof ir, m, 8) == 0); // first in practice
        put(4, 0);
        T(ir_marks(&is, ire, ir, sizeof ir, m, 8) == 0 && !is.have); // the sim closed
    }

    // Assetto Corsa Competizione: a better best lap, and places gained in a race only
    {
        u8 g[256] = {};
        auto put = [&](i64 at, i32 v) { memcpy(g + at, &v, 4); };
        u32 acce = AM_GAMES[AMG_ACC].events;
        AccSeen as = {};
        put(4, 2), put(8, 2), put(132, 0), put(136, 6), put(148, 2147483647);
        T(acc_marks(&as, acce, g, sizeof g, m, 8) == 0 && as.have);
        put(136, 4);
        T(acc_marks(&as, acce, g, sizeof g, m, 8) == 1 && !strcmp(m[0].note, "Up to P4") && m[0].colour == 4);
        put(136, 5), put(132, 1), put(148, 101000);
        T(acc_marks(&as, acce, g, sizeof g, m, 8) == 0);
        put(132, 2), put(148, 100500);
        T(acc_marks(&as, acce, g, sizeof g, m, 8) == 1 && !strcmp(m[0].note, "Best lap"));
        put(4, 3), put(136, 1);
        T(acc_marks(&as, acce, g, sizeof g, m, 8) == 0 && as.position == 5); // paused: as it was
        put(4, 2), put(8, 0), put(136, 1);
        T(acc_marks(&as, acce, g, sizeof g, m, 8) == 0); // another session
        put(136, 0);
        T(acc_marks(&as, acce, g, sizeof g, m, 8) == 0); // practice: no places
    }

    // Rocket League: the Stats API's stream, split into its messages, and what they mark
    {
        static RlSeen rs;
        rs = {};
        u32 rle = AM_GAMES[AMG_ROCKET_LEAGUE].events;
        AutoMark rm[8];
        i32 got = 0, msgs = 0;
        auto feed = [&](const char *text) {
            rl_feed(&rs, text, (i32)strlen(text), [&](const char *msg, i64 len) {
                msgs++;
                got += rl_message(&rs, rle, msg, len, rm + got, 8 - got);
            });
        };
        // Data as a string, the way the game sends it, and two messages in one read
        feed("{\"Event\":\"UpdateState\",\"Data\":\"{\\\"Game\\\":{\\\"bHasTarget\\\":true,\\\"Target\\\":{\\\"Name\\\":\\\"Me {1}\\\","
             "\\\"Shortcut\\\":3,\\\"TeamNum\\\":1}}}\"}{\"Event\":\"StatfeedEvent\",\"Data\":\"{\\\"EventName\\\":\\\"Goal\\\","
             "\\\"Type\\\":\\\"Goal\\\",\\\"MainTarget\\\":{\\\"Name\\\":\\\"Me {1}\\\",\\\"TeamNum\\\":1}}\"}");
        T(msgs == 2 && !strcmp(rs.me, "Me {1}") && rs.team == 1);
        T(got == 1 && rm[0].colour == 4 && !strcmp(rm[0].note, "Goal"));
        // Data as an object, split across reads
        got = 0;
        feed("{\"Event\":\"StatfeedEvent\",\"Data\":{\"EventName\":\"Demolish\",\"Type\":\"Demolition\",\"MainTarget\":{\"Na");
        T(got == 0);
        feed("me\":\"Me {1}\"},\"SecondaryTarget\":{\"Name\":\"Them\"}}}");
        T(got == 1 && !strcmp(rm[0].note, "Demolition"));
        got = 0;
        feed("{\"Event\":\"StatfeedEvent\",\"Data\":{\"EventName\":\"Demolish\",\"MainTarget\":{\"Name\":\"Them\"},"
             "\"SecondaryTarget\":{\"Name\":\"Me {1}\"}}}");
        T(got == 1 && rm[0].colour == 1 && !strcmp(rm[0].note, "Demolished"));
        got = 0;
        feed("{\"Event\":\"StatfeedEvent\",\"Data\":{\"EventName\":\"EpicSave\",\"Type\":\"Epic Save\",\"MainTarget\":{\"Name\":\"Me {1}\"}}}"
             "{\"Event\":\"StatfeedEvent\",\"Data\":{\"EventName\":\"AerialGoal\",\"Type\":\"Aerial Goal\",\"MainTarget\":{\"Name\":\"Them\"}}}"
             "{\"Event\":\"StatfeedEvent\",\"Data\":{\"EventName\":\"Assist\",\"MainTarget\":{\"Name\":\"Me {1}\"}}}");
        T(got == 2 && !strcmp(rm[0].note, "Epic Save") && !strcmp(rm[1].note, "Assist")); // the other's goal isn't mine
        got = 0;
        const char *goal = "{\"Event\":\"StatfeedEvent\",\"Data\":{\"EventName\":\"Goal\",\"MainTarget\":{\"Name\":\"Me {1}\"}}}";
        rl_feed(&rs, goal, (i32)strlen(goal), [&](const char *msg, i64 len) { got += rl_message(&rs, rle & ~AM_GOAL, msg, len, rm, 8); });
        T(got == 0); // goals unticked
        feed("{\"Event\":\"MatchEnded\",\"Data\":{\"WinnerTeamNum\":1}}");
        T(got == 1 && rm[0].colour == 3 && !strcmp(rm[0].note, "Win") && !rs.me[0]);
        got = 0;
        feed("{\"Event\":\"MatchEnded\",\"Data\":{\"WinnerTeamNum\":1}}");
        T(got == 0); // no player seen since

        // Its ini: the rate set when there is none or it is 0, put back only from Rekodr's rate
        char ini[1024], back[1024];
        rl_ini("", true, ini, sizeof ini);
        T(!strcmp(ini, "[TAGame.MatchStatsExporter_TA]\nPort=49123\nPacketSendRate=30\n"));
        rl_ini(ini, false, back, sizeof back);
        T(!strcmp(back, "[TAGame.MatchStatsExporter_TA]\nPort=49123\nPacketSendRate=0\n"));
        rl_ini("[TAGame.MatchStatsExporter_TA]\r\nPort=49200\r\nPacketSendRate=0\r\n[Other]\r\nX=1\r\n", true, ini, sizeof ini);
        T(!strcmp(ini, "[TAGame.MatchStatsExporter_TA]\r\nPort=49200\r\nPacketSendRate=30\r\n[Other]\r\nX=1\r\n"));
        T(rl_port(ini) == 49200);
        rl_ini("[TAGame.MatchStatsExporter_TA]\nPort=49123\n[Other]\nX=1", true, ini, sizeof ini);
        T(!strcmp(ini, "[TAGame.MatchStatsExporter_TA]\nPort=49123\nPacketSendRate=30\n[Other]\nX=1"));
        rl_ini("[TAGame.MatchStatsExporter_TA]\nPacketSendRate=60\n", true, ini, sizeof ini);
        T(!strcmp(ini, "[TAGame.MatchStatsExporter_TA]\nPacketSendRate=60\n")); // the player's own rate
        rl_ini(ini, false, back, sizeof back);
        T(!strcmp(back, ini));
        T(rl_port("[Other]\nPort=1\n") == 49123);
    }

    // Hearthstone: the player is whoever this PC sent the mulligan for; their win is said once a game
    {
        HsSeen hs = {};
        AutoMark hm[4];
        u32 hse = AM_GAMES[AMG_HEARTHSTONE].events;
        const char *pre = "D 20:01:02.0000000 ";
        auto line = [&](const char *l, bool old = false) {
            char b[512];
            snprintf(b, sizeof b, "%s%s", pre, l);
            return hs_line(&hs, hse, b, old, hm, 4);
        };
        T(line("GameState.DebugPrintPower() - CREATE_GAME") == 0);
        line("GameState.DebugPrintEntityChoices() - id=1 Player=Them#2222 TaskList= ChoiceType=MULLIGAN CountMin=0 CountMax=3");
        line("GameState.DebugPrintEntityChoices() - id=2 Player=Me#1234 TaskList= ChoiceType=MULLIGAN CountMin=0 CountMax=4");
        T(!hs.me[0]);
        line("GameState.SendChoices() - id=2 ChoiceType=MULLIGAN");
        T(!strcmp(hs.me, "Me#1234"));
        T(line("GameState.DebugPrintPower() - TAG_CHANGE Entity=Them#2222 tag=PLAYSTATE value=WON") == 0);
        T(line("GameState.DebugPrintPower() - TAG_CHANGE Entity=Me#1234 tag=PLAYSTATE value=WON") == 1 && hm[0].colour == 3);
        T(line("PowerTaskList.DebugPrintPower() -     TAG_CHANGE Entity=Me#1234 tag=PLAYSTATE value=WON") == 0); // the same win
        line("PowerTaskList.DebugPrintPower() - CREATE_GAME");
        T(line("GameState.DebugPrintPower() - TAG_CHANGE Entity=Me#1234 tag=PLAYSTATE value=WON", true) == 0); // an old line
        line("GameState.DebugPrintPower() - CREATE_GAME");
        T(line("GameState.DebugPrintPower() - TAG_CHANGE Entity=Me#1234 tag=PLAYSTATE value=WONDER") == 0);
        T(line("GameState.DebugPrintPower() - TAG_CHANGE Entity=Me#1234 tag=PLAYSTATE value=LOST") == 0);

        // log.config: Rekodr's [Power] in and out, a [Power] of the player's own left as it is
        char hcfg[1024], back[1024];
        hs_log_config("", true, hcfg, sizeof hcfg);
        T(!strncmp(hcfg, "[Power]\nLogLevel=1\nFilePrinting=true\n", 37));
        T(hs_log_config(hcfg, false, back, sizeof back) == 0);
        hs_log_config("[Zone]\r\nLogLevel=1", true, hcfg, sizeof hcfg);
        T(!strncmp(hcfg, "[Zone]\r\nLogLevel=1\n[Power]\n", 27));
        hs_log_config(hcfg, false, back, sizeof back);
        T(!strcmp(back, "[Zone]\r\nLogLevel=1\n"));
        const char *own = "[Power]\nLogLevel=1\nFilePrinting=true\n[Zone]\nLogLevel=1\n";
        hs_log_config(own, true, hcfg, sizeof hcfg);
        T(!strcmp(hcfg, own));
    }

    // autoexec.cfg: Rekodr's line in and out, the player's lines as they were
    char ax[512];
    T(tf2_autoexec("", true, ax, sizeof ax) > 0 && !strcmp(ax, "con_logfile console.log // " APP_NAME " reads it for markers\n"));
    T(tf2_autoexec(ax, false, ax + 256, 256) == 0);
    const char *own = "bind f \"say hi\"\r\nfov_desired 90";
    tf2_autoexec(own, true, ax, sizeof ax);
    T(!strncmp(ax, "bind f \"say hi\"\r\nfov_desired 90\ncon_logfile", 43));
    char back[512];
    tf2_autoexec(ax, true, back, sizeof back);
    T(!strcmp(ax, back)); // on twice: one line
    tf2_autoexec(ax, false, back, sizeof back);
    T(!strcmp(back, "bind f \"say hi\"\r\nfov_desired 90\n"));
}

// Binary KeyValues, as Steam writes them, for the achievement tests.
struct KvOut {
    u8 b[4096];
    i32 n;
    void tag(u8 t, const char *name) { b[n++] = t; size_t l = strlen(name) + 1; memcpy(b + n, name, l); n += (i32)l; }
    void open(const char *name) { tag(0, name); }
    void close() { b[n++] = 8; }
    void num(const char *name, i32 v) { tag(2, name); memcpy(b + n, &v, 4); n += 4; }
    void str(const char *name, const char *v) { tag(1, name); size_t l = strlen(v) + 1; memcpy(b + n, v, l); n += (i32)l; }
};

static void test_achievements()
{
    static KvOut u;
    u = {};
    u.open("cache");
    u.num("crc", 5);
    u.open("67");
    u.num("data", 7);
    u.open("AchievementTimes");
    u.num("0", 1688115185);
    u.num("1", 1700000000);
    u.num("2", 1600000000);
    u.close();
    u.close();
    u.open("93");
    u.open("AchievementTimes");
    u.num("4", 1700000100);
    u.close();
    u.close();
    u.close();
    u.close();
    AchUnlock found[16];
    i64 newest = 0;
    T(ach_unlocks(u.b, u.n, 0, found, 16, &newest) == 4 && newest == 1700000100);
    i32 n = ach_unlocks(u.b, u.n, 1690000000, found, 16, &newest);
    T(n == 2 && found[0].stat == 67 && found[0].bit == 1 && found[0].time == 1700000000 && found[1].stat == 93 && found[1].bit == 4);
    T(ach_unlocks(u.b, u.n, 1700000100, found, 16, &newest) == 0);
    T(ach_unlocks(u.b, 7, 0, found, 16, &newest) == 0); // cut short

    static KvOut s;
    s = {};
    s.open("252490");
    s.open("stats");
    s.open("67");
    s.str("type", "ACHIEVEMENTS");
    s.open("bits");
    s.open("0");
    s.str("name", "PLACE_CAMPFIRE");
    s.open("display");
    s.open("name");
    s.str("english", "Place Camp Fire");
    s.str("token", "NEW_ACHIEVEMENT_67_0_NAME");
    s.close();
    s.close();
    s.close();
    s.open("1");
    s.str("name", "CRAFT_CAMPFIRE");
    s.open("display");
    s.str("name", "Craft Camp Fire");
    s.close();
    s.close();
    s.close();
    s.close();
    s.close();
    s.close();
    char api[128], shown[128];
    T(ach_names(s.b, s.n, 67, 0, api, sizeof api, shown, sizeof shown) && !strcmp(api, "PLACE_CAMPFIRE") && !strcmp(shown, "Place Camp Fire"));
    T(ach_names(s.b, s.n, 67, 1, api, sizeof api, shown, sizeof shown) && !strcmp(api, "CRAFT_CAMPFIRE") && !strcmp(shown, "Craft Camp Fire"));
    T(!ach_names(s.b, s.n, 67, 2, api, sizeof api, shown, sizeof shown));

    const char *pct = "{\"achievementpercentages\":{\"achievements\":[{\"name\":\"PLACE_CAMPFIRE\",\"percent\":\"73.5\"},"
                      "{\"name\":\"CRAFT_CAMPFIRE\",\"percent\":0.8}]}}";
    T(fabsf(ach_percent(pct, (i64)strlen(pct), "PLACE_CAMPFIRE") - 73.5f) < 0.01f);
    T(fabsf(ach_percent(pct, (i64)strlen(pct), "CRAFT_CAMPFIRE") - 0.8f) < 0.01f);
    T(ach_percent(pct, (i64)strlen(pct), "NOPE") < 0);
}

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
    snprintf(root + strlen(root), sizeof root - strlen(root), "rekodr-stores-%lu", GetCurrentProcessId());
    snprintf(p, sizeof p, "%s/Lib/steamapps/common/Hollow Knight", root);
    make_dirs(p);
    snprintf(p, sizeof p, "%s/Lib/steamapps/appmanifest_367520.acf", root);
    put_text(p, "\"AppState\"\n{\n\t\"appid\"\t\t\"367520\"\n\t\"name\"\t\t\"Hollow Knight\"\n\t\"installdir\"\t\t\"Hollow Knight\"\n}\n");
    snprintf(p, sizeof p, "%s/Lib/steamapps/appmanifest_10.acf", root);
    put_text(p, "\"AppState\"\n{\n\t\"installdir\"\t\t\"Hollow\"\n}\n");
    snprintf(p, sizeof p, "%s/Lib/steamapps/common/Hollow Knight/hollow_knight.exe", root);
    T(steam_app_for(p, "Hollow Knight") == 367520 && steam_app_for(p, "hollow knight") == 367520 && !steam_app_for(p, "Silksong"));
    T(exe_clues("hollow_knight.exe", p, "Hollow Knight", false, 0, body, sizeof body) &&
      !strcmp(body, "{\"exe\":\"hollow_knight.exe\",\"folder\":\"Hollow Knight\",\"dirs\":[\"steamapps\",\"common\",\"Hollow Knight\"],"
                    "\"name\":\"Hollow Knight\",\"steam\":367520}"));
    T(!strstr(body, "rekodr-stores"));

    snprintf(p, sizeof p, "%s/GOG/Witcher 3/bin/x64", root);
    make_dirs(p);
    snprintf(p, sizeof p, "%s/GOG/Witcher 3/goggame-1207664643.info", root);
    put_text(p, "{}");
    snprintf(p, sizeof p, "%s/GOG/Witcher 3/bin/x64/witcher3.exe", root);
    T(gog_game_for(p) == 1207664643ull);
    T(exe_clues("witcher3.exe", p, "", false, ",\"args_has\":[]", body, sizeof body) &&
      !strcmp(body, "{\"exe\":\"witcher3.exe\",\"folder\":\"Witcher 3\",\"dirs\":[\"Witcher 3\",\"bin\",\"x64\"],"
                    "\"gog\":\"1207664643\",\"args_has\":[]}"));

    // Epic: the AppName of the launcher's manifest whose install folder holds the exe
    {
        wchar_t keep[MAX_PATH] = L"";
        DWORD had = GetEnvironmentVariableW(L"ProgramData", keep, MAX_PATH);
        snprintf(p, sizeof p, "%s/PD/Epic/EpicGamesLauncher/Data/Manifests", root);
        make_dirs(p);
        snprintf(p, sizeof p, "%s/Epic/Fall Guys", root);
        make_dirs(p);
        char item[1024], at[MAX_PATH * 3];
        i32 an = 0;
        for (const char *c = root; *c && an < (i32)sizeof at - 2; c++) {
            if (*c == '/') { at[an++] = '\\'; at[an++] = '\\'; }
            else at[an++] = *c;
        }
        at[an] = 0;
        snprintf(item, sizeof item, "{\"AppName\":\"Fall\",\"InstallLocation\":\"%s\\\\Epic\\\\Fall Guys\"}", at);
        snprintf(p, sizeof p, "%s/PD/Epic/EpicGamesLauncher/Data/Manifests/1.item", root);
        put_text(p, item);
        snprintf(p, sizeof p, "%s/PD", root);
        Path pd = {};
        utf8_to_wide(p, pd.s, MAX_PATH);
        SetEnvironmentVariableW(L"ProgramData", pd.s);
        char epic[128];
        snprintf(p, sizeof p, "%s/Epic/Fall Guys/FallGuys_client.exe", root);
        T(epic_app_for(p, epic, sizeof epic) && !strcmp(epic, "Fall"));
        T(exe_clues("FallGuys_client.exe", p, "", false, 0, body, sizeof body) && strstr(body, ",\"epic\":\"Fall\"}"));
        snprintf(p, sizeof p, "%s/Epic/Fall Guys Two/x.exe", root);
        T(!epic_app_for(p, epic, sizeof epic));
        SetEnvironmentVariableW(L"ProgramData", had ? keep : 0);
        snprintf(p, sizeof p, "%s/PD/Epic/EpicGamesLauncher/Data/Manifests/1.item", root);
        Path w = {};
        utf8_to_wide(p, w.s, MAX_PATH);
        DeleteFileW(w.s);
        const char *dirs[] = {"PD/Epic/EpicGamesLauncher/Data/Manifests", "PD/Epic/EpicGamesLauncher/Data", "PD/Epic/EpicGamesLauncher",
                              "PD/Epic", "PD", "Epic/Fall Guys", "Epic"};
        for (auto d : dirs) {
            snprintf(p, sizeof p, "%s/%s", root, d);
            utf8_to_wide(p, w.s, MAX_PATH);
            RemoveDirectoryW(w.s);
        }
    }

    // Not a game and not from a store: not asked about. Sorted as a Game: asked about with what there is.
    snprintf(p, sizeof p, "%s/Tools/Thing/thing.exe", root);
    T(!exe_clues("thing.exe", p, "Thing", false, 0, body, sizeof body));
    T(exe_clues("thing.exe", p, "Thing", true, 0, body, sizeof body) &&
      !strncmp(body, "{\"exe\":\"thing.exe\",\"folder\":\"Thing\",\"dirs\":[", 38) &&
      strstr(body, "\"Tools\",\"Thing\"],\"name\":\"Thing\"}"));
    // Folders that could name the player are never among them
    {
        char dirs[512];
        T(exe_dirs("C:\\Users\\someone\\Desktop\\Games\\Thing\\thing.exe", dirs, sizeof dirs) &&
          !strcmp(dirs, "[\"Games\",\"Thing\"]"));
        T(!exe_dirs("C:\\thing.exe", dirs, sizeof dirs));
    }

    // A lookup's answer: the game's name and key, how it was matched, and for a shared exe what to look for
    LookAnswer ans;
    static const char by_id[] = "{\"game\":{\"key\":\"d356875221482618880\",\"name\":\"Hollow Knight\"},\"matched_by\":\"store_id\","
                                "\"confidence\":0.99,\"art\":{\"icon\":null,\"hero\":null,\"cover\":null},\"v\":\"7\"}";
    T(lookup_parse(by_id, sizeof by_id - 1, &ans) && !strcmp(ans.name, "Hollow Knight") && !strcmp(ans.key, "d356875221482618880") &&
      !strcmp(ans.how, "store_id") && ans.confidence > 0.98f && !ans.ask_args[0] && !ans.software);
    static const char software[] = "{\"game\":{\"key\":\"s1\",\"name\":\"Wallpaper Engine\"},\"matched_by\":\"store_id\","
                                   "\"confidence\":0.99,\"art\":{},\"steam_type\":\"software\",\"v\":\"7\"}";
    T(lookup_parse(software, sizeof software - 1, &ans) && ans.software);
    const char *art_at;
    i64 art_len;
    T(answer_art(by_id, sizeof by_id - 1, &art_at, &art_len) && art_len == 38 && art_at[0] == '{');
    static const char shared_exe[] = "{\"game\":null,\"ask_args\":[\"-game garrysmod\",\"-game cstrike\"],\"v\":\"7\"}";
    T(lookup_parse(shared_exe, sizeof shared_exe - 1, &ans) && !ans.name[0] && !strcmp(ans.ask_args, "-game garrysmod\n-game cstrike"));
    char has[256];
    args_has_json(ans.ask_args, "hl2.exe -GAME garrysmod -windowed", has, sizeof has);
    T(!strcmp(has, "[\"-game garrysmod\"]"));
    static const char none[] = "{\"game\":null,\"matched_by\":\"none\",\"confidence\":0,\"art\":{}}";
    T(lookup_parse(none, sizeof none - 1, &ans) && !ans.name[0] && !strcmp(ans.how, "none"));
    T(!lookup_parse("{\"error\":1}", 11, &ans));

    // "Help name games": a game named by hand is told to the service, what it was shown as and what it is now;
    // nothing is sent without the player's say-so, or when the name didn't change
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
        HANDLE wake = g_gamedb.wake;
        g_gamedb.wake = 0;
        auto renames = [](const char **body) {
            i32 n = 0;
            for (auto &q : g_gamedb.looks)
                if (q.rename && q.body) { n++; *body = q.body; }
            return n;
        };
        const char *sent = 0;
        g_app.config.share_names = false;
        gamedb_rename(app, "thing", "Thing");
        T(renames(&sent) == 0);
        g_app.config.share_names = true;
        gamedb_rename(app, "Thing", "Thing");
        T(renames(&sent) == 0);
        gamedb_rename(app, "thing", "Thing");
        T(renames(&sent) == 1 && strstr(sent, "{\"exe\":\"thing.exe\",\"folder\":\"Thing\",\"dirs\":[") &&
          strstr(sent, "],\"from\":\"thing\",\"to\":\"Thing\"}"));
        // with the clues its lookup sent, when there was one
        last_look_put("thing.exe", "{\"exe\":\"thing.exe\",\"folder\":\"Thing\",\"args_has\":[\"-x\"]}");
        gamedb_rename(app, "thing", "Thing 2");
        T(renames(&sent) == 2 && !strcmp(sent, "{\"exe\":\"thing.exe\",\"folder\":\"Thing\",\"args_has\":[\"-x\"],\"from\":\"thing\",\"to\":\"Thing 2\"}"));

        // A clip given a game by hand: the program longest in front during it is voted for, and a browser
        // only counts the game
        char focus[] = "100\tgame.exe\n200\trekodr.exe\n900\tfirefox.exe\n1000\tgame.exe\n";
        char exe[64];
        T(focus_longest(focus, 150, 1020, exe, sizeof exe) && !strcmp(exe, "firefox.exe"));
        char focus2[] = "100\tgame.exe\n200\trekodr.exe\n900\tfirefox.exe\n1000\tgame.exe\n";
        T(focus_longest(focus2, 0, 1500, exe, sizeof exe) && !strcmp(exe, "game.exe"));
        char focus3[] = "100\trekodr.exe\n200\t\n";
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
            gamedb_clip_game(it, "", "Chess");
            T(renames(&sent) == 3 && !strcmp(sent, "{\"exe\":\"chrome.exe\",\"from\":\"\",\"to\":\"Chess\"}"));
            str_set(it.name, "Not saved here");
            gamedb_clip_game(it, "", "Chess");
            T(renames(&sent) == 3);
            g_app.saving = keep;
            g_app.config.buffer_dir = buffer;
            g_app.config.save_dir = save_dir;
        }
        for (auto &q : g_gamedb.looks) {
            free(q.body);
            q = {};
        }
        g_gamedb.wake = wake;
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

static void test_store_folders()
{
    // Steam's libraryfolders.vdf: each library's path, unescaped
    static const char vdf[] = "\"libraryfolders\"\n{\n\t\"0\"\n\t{\n\t\t\"path\"\t\t\"C:\\\\Program Files (x86)\\\\Steam\"\n"
                              "\t\t\"apps\"\n\t\t{\n\t\t\t\"228980\"\t\t\"0\"\n\t\t}\n\t}\n\t\"1\"\n\t{\n\t\t\"path\"\t\t\"E:\\\\SteamLibrary\"\n\t}\n}\n";
    char libs[4][MAX_PATH * 2];
    T(steam_libraries(vdf, libs, 4) == 2 && !strcmp(libs[0], "C:\\Program Files (x86)\\Steam") && !strcmp(libs[1], "E:\\SteamLibrary"));
    T(steam_libraries(vdf, libs, 1) == 1);
    T(steam_libraries("", libs, 4) == 0);
    char v[64];
    T(vdf_value("\"AppState\"\n{\n\t\"appid\"\t\t\"431960\"\n\t\"installdir\"\t\t\"wallpaper_engine\"\n}", "installdir", v, sizeof v) &&
      !strcmp(v, "wallpaper_engine"));
    T(!vdf_value("\"AppState\" { }", "installdir", v, sizeof v));

    // A drive's .GamingRoot: "RGBX", a count, then UTF-16 names
    static const u8 root1[] = {'R', 'G', 'B', 'X', 1, 0, 0, 0, 'X', 0, 'b', 0, 'o', 0, 'x', 0, 'G', 0, 'a', 0, 'm', 0, 'e', 0, 's', 0, 0, 0};
    char names[4][MAX_PATH * 2];
    T(gaming_root_folders(root1, sizeof root1, names, 4) == 1 && !strcmp(names[0], "XboxGames"));
    static const u8 root2[] = {'R', 'G', 'B', 'X', 2, 0, 0, 0, 'A', 0, 0, 0, 'B', 0, '\\', 0, 'C', 0, 0, 0};
    T(gaming_root_folders(root2, sizeof root2, names, 4) == 2 && !strcmp(names[0], "A") && !strcmp(names[1], "B\\C"));
    T(gaming_root_folders(root1, 6, names, 4) == 0);
    static const u8 cut[] = {'R', 'G', 'B', 'X', 9, 0, 0, 0, 'X', 0};
    T(gaming_root_folders(cut, sizeof cut, names, 4) == 1 && !strcmp(names[0], "X")); // a file cut short ends where it ends
    static const u8 other[] = {'N', 'O', 'P', 'E', 1, 0, 0, 0, 'X', 0, 0, 0};
    T(gaming_root_folders(other, sizeof other, names, 4) == 0);

    // An Epic manifest: a game's InstallLocation, never Unreal Engine's
    char at[MAX_PATH * 2];
    T(epic_item_folder("{\"AppCategories\": [\"public\", \"games\", \"applications\"], \"InstallLocation\": \"D:\\\\Epic\\\\Fortnite\"}", at,
                       sizeof at) && !strcmp(at, "D:\\Epic\\Fortnite"));
    T(!epic_item_folder("{\"AppCategories\": [\"engines/ue5\", \"engines\"], \"InstallLocation\": \"C:\\\\UE_5.6\"}", at, sizeof at));
    T(!epic_item_folder("{\"AppCategories\": [\"plugins\"], \"Note\": \"games\", \"InstallLocation\": \"C:\\\\X\"}", at, sizeof at));
    T(!epic_item_folder("{\"InstallLocation\": \"C:\\\\X\"}", at, sizeof at));

    // Found folders become rules once: a rule the player removed or changed stays that way
    static Config c;
    c = {};
    static StoreFolder found[3];
    i32 n = 0;
    store_folder_add(found, &n, 3, "E:/SteamLibrary/steamapps/common/", CAT_GAME);
    store_folder_add(found, &n, 3, "E:\\SteamLibrary\\steamapps\\common\\wallpaper_engine", CAT_APP);
    store_folder_add(found, &n, 3, "C:\\XboxGames", CAT_GAME);
    T(n == 3 && !strcmp(found[0].path, "E:\\SteamLibrary\\steamapps\\common"));
    T(store_folders_merge(&c, found, n) && c.folder_count == 3 && c.store_seen_count == 3);
    T(app_category_in(c.apps, c.app_count, c.folders, c.folder_count, "wallpaper64.exe",
                      "E:\\SteamLibrary\\steamapps\\common\\wallpaper_engine\\wallpaper64.exe") == CAT_APP);
    T(app_category_in(c.apps, c.app_count, c.folders, c.folder_count, "cs2.exe",
                      "E:\\SteamLibrary\\steamapps\\common\\Counter-Strike Global Offensive\\game\\bin\\win64\\cs2.exe") == CAT_GAME);
    T(!store_folders_merge(&c, found, n)); // the next start: nothing new
    c.folders[2].category = CAT_IGNORED;    // the player changes one
    memmove(&c.folders[0], &c.folders[1], 2 * sizeof c.folders[0]); // and removes another
    c.folder_count = 2;
    T(!store_folders_merge(&c, found, n) && c.folder_count == 2 && c.folders[1].category == CAT_IGNORED);
    // a rule the player made before the store folder was found is left as it is
    static Config d;
    d = {};
    d.folder_count = 1;
    str_set(d.folders[0].path, "c:\\xboxgames");
    d.folders[0].category = CAT_APP;
    T(store_folders_merge(&d, &found[2], 1) && d.folder_count == 1 && d.folders[0].category == CAT_APP && d.store_seen_count == 1);
    // the seen list is saved and read back
    char line[] = "E:\\SteamLibrary\\steamapps\\common";
    config_set(&d, "store_folder", line);
    T(d.store_seen_count == 2 && !store_folders_merge(&d, found, 1) && d.folder_count == 1);
    // with the rules full, a folder waits for room instead of being taken as seen
    d.folder_count = FOLDER_MAX;
    T(!store_folders_merge(&d, &found[1], 1) && d.store_seen_count == 2);

    // Every built-in exception is a Steam app with an id and a name
    for (const SteamNotGame &s : STEAM_NOT_GAMES) T(s.id && s.name[0]);
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
    swprintf(file.s, MAX_PATH, L"%srekodr-test-adv-%u.edit.txt", dir, GetCurrentProcessId());
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

    // A dragged key stays inside its item and in its lane's range, the keys stay in order, and it's still selected
    ed.edit = ed.drag_from = a;
    ed.adv_u = 0;
    ed.adv_drag_key = 0;
    const ItemKey k0 = a.ikeys[0];
    const EditItem &ki = a.items[item_find(a, k0.item)];
    i32 kl = k0.what == IKEY_ZOOM ? 0 : 1;
    adv_drag_key(1000, 10, 100, 100, false, false);
    T(ed.edit.ikey_count == a.ikey_count && adv_key_ok());
    const ItemKey &kd = ed.edit.ikeys[ed.adv_key];
    T(kd.item == k0.item && kd.what == k0.what && near(kd.t, ki.to) && near(kd.v, ln_hi(kl)));
    bool ordered = true;
    for (i32 k = 1; k < ed.edit.ikey_count; k++) ordered = ordered && !ikey_before(ed.edit.ikeys[k], ed.edit.ikeys[k - 1]);
    T(ordered);
    // With Ctrl only the value moves; Delete takes the key out and leaves its slot clear
    adv_drag_key(1000, -10, 100, 100, false, true);
    T(near(ed.edit.ikeys[ed.adv_key].t, k0.t) && near(ed.edit.ikeys[ed.adv_key].v, ln_lo(kl)));
    adv_delete_key();
    T(ed.edit.ikey_count == a.ikey_count - 1 && ed.adv_key == -1 && !ed.edit.ikeys[ed.edit.ikey_count].item);
    ed.edit = ed.drag_from = {};
}

// The webcam in an Advanced edit: its items, where they put it, the file they're kept in, and the graph that lays
// them over the picture; and how the library finds a clip's webcam
static void test_cam_layer()
{
    static MediaInfo m;
    m = {};
    m.duration = 10, m.width = 1920, m.height = 1080, m.fps = 30, m.track_count = 3;
    str_set(m.tracks[0], "Mix"), str_set(m.tracks[1], "Mic"), str_set(m.tracks[2], "Game");
    m.channels[0] = 2, m.channels[1] = 1, m.channels[2] = 2;
    m.cam_w = 1280, m.cam_h = 720;

    // Simple 1-4 then 6-9 with a 1 s crossfade and a fade in: a cam item per piece, linked with its picture, in the
    // bottom right corner a quarter of the picture wide, 38 px (2% of the width) in from both edges
    static EditRecord s, a, b;
    s = {};
    s.pieces[0] = {1, 4};
    s.pieces[1] = {6, 9, TRANS_CROSS, 1.0f};
    s.piece_count = 2;
    s.fade_in = 0.5f;
    a = s;
    edit_to_advanced(&a, m);
    T(a.item_count == 8);
    i32 pic[EDIT_MAX_ITEMS], cam[EDIT_MAX_ITEMS];
    T(track_items(a, 0, pic) == 2 && track_items(a, EDIT_CAM_TRACK, cam) == 2);
    const EditItem &c0 = a.items[cam[0]], &c1 = a.items[cam[1]];
    T(c0.link == a.items[pic[0]].link && c1.link == a.items[pic[1]].link);
    T(near(c1.start, 3) && near(c1.from, 6) && near(c1.to, 9) && near(c0.fade_in, 0.5) && c1.cross == 0);
    T(near(c0.cam_w, CAM_DEFAULT_W) && cam_corner_of(c0, 1920, 1080, 1280, 720) == CAM_BOTTOM_RIGHT);
    CamRect r = cam_rect(c0, 1920, 1080, 1280, 720);
    T(r.w == 480 && r.h == 270 && r.x == 1402 && r.y == 772 && 1920 - r.x - r.w == 38 && 1080 - r.y - r.h == 38);
    // No webcam, no cam items
    m.cam_w = m.cam_h = 0;
    b = s;
    edit_to_advanced(&b, m);
    T(b.item_count == 6 && track_items(b, EDIT_CAM_TRACK, cam) == 0);
    m.cam_w = 1280, m.cam_h = 720;
    track_items(a, EDIT_CAM_TRACK, cam);

    // The layer maths: corners, held inside the picture, the cam's own shape, the least and most width
    EditItem it = c0;
    cam_corner(&it, CAM_TOP_LEFT, 1920, 1080, 1280, 720);
    r = cam_rect(it, 1920, 1080, 1280, 720);
    T(r.x == 38 && r.y == 38 && cam_corner_of(it, 1920, 1080, 1280, 720) == CAM_TOP_LEFT);
    cam_place(&it, 0.5, 0.5, 0.25, 1920, 1080, 1280, 720);
    r = cam_rect(it, 1920, 1080, 1280, 720);
    T(r.x == 720 && r.y == 405 && cam_corner_of(it, 1920, 1080, 1280, 720) == -1);
    cam_place(&it, 1.2, -0.5, 0.25, 1920, 1080, 1280, 720);
    r = cam_rect(it, 1920, 1080, 1280, 720);
    T(r.x + r.w == 1920 && r.y == 0 && near(it.cam_x, 0.875) && near(it.cam_y, 0.125));
    r = cam_rect(it, 1920, 1080, 640, 480); // 4:3: as wide, taller
    T(r.w == 480 && r.h == 360 && r.x + r.w == 1920 && r.y == 0);
    cam_place(&it, 0.5, 0.5, 1, 1920, 1080, 720, 1280); // a phone held upright: as tall as the picture, no more
    r = cam_rect(it, 1920, 1080, 720, 1280);
    T(r.h == 1080 && r.w == 608 && r.y == 0 && near(it.cam_w, 0.31640625));
    cam_place(&it, 0.5, 0.5, 0.01, 1920, 1080, 1280, 720);
    T(near(it.cam_w, CAM_MIN_W));
    it.cam_x = 2, it.cam_y = 2, it.cam_w = 0.5f; // out of place on disk: the render still keeps it inside
    r = cam_rect(it, 1280, 720, 1280, 720);
    T(r.w == 640 && r.h == 360 && r.x == 640 && r.y == 360);

    // Back to Simple loses a moved webcam, so it asks; one left where it was put goes back as it was
    T(edit_round_trips(a, m));
    b = a;
    cam_corner(&b.items[cam[1]], CAM_TOP_LEFT, 1920, 1080, 1280, 720);
    T(!edit_round_trips(b, m));
    b = a;
    b.items[cam[0]].cam_w = 0.4f;
    T(!edit_round_trips(b, m) && !adv_same(a, b, m));
    b = a;
    edit_to_simple(&b, m);
    T(b.piece_count == 2 && !b.item_count);

    // The record on disk keeps the webcam's place
    wchar_t dir[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    Path file = {};
    swprintf(file.s, MAX_PATH, L"%srekodr-test-cam-%u.edit.txt", dir, GetCurrentProcessId());
    b = a;
    cam_place(&b.items[cam[1]], 0.3, 0.4, 0.33, 1920, 1080, 1280, 720);
    edit_write(file, b);
    static EditRecord rd;
    edit_read(file, &rd);
    DeleteFileW(file.s);
    i32 rc[EDIT_MAX_ITEMS];
    T(rd.item_count == 8 && track_items(rd, EDIT_CAM_TRACK, rc) == 2 && adv_same(rd, b, m));
    T(fabs(rd.items[rc[1]].cam_x - 0.3) < 1e-4 && fabs(rd.items[rc[1]].cam_w - 0.33) < 1e-4);

    // The graph: each cam item cut from the webcam stream, sized, faded by its alpha, moved to its start and laid over
    // the picture in turn; the picture's format last
    static EditJob j;
    j = {};
    j.info = m;
    j.edit = a;
    SaveWork *w = &g_save_work;
    w->graph_len = 0, w->graph_full = false, w->graph[0] = 0;
    i32 streams[MEDIA_MAX_TRACKS + 1];
    adv_graph(w, j, 0.5, "", "yuv420p", false, streams);
    T(strstr(w->graph, "[0:v:1]split=2[cm0][cm1];") != 0);
    T(strstr(w->graph, "[cm0]trim=start=0.5000:end=3.5000,setpts=PTS-STARTPTS,fps=30.0000,scale=480:270,setsar=1,"
                       "format=yuva420p,fade=t=in:st=0:d=0.5000:alpha=1,settb=AVTB,setpts=PTS+0.0000/TB[cq0];") != 0);
    T(strstr(w->graph, "[cm1]trim=start=5.5000:end=8.5000,") != 0 && strstr(w->graph, "setpts=PTS+3.0000/TB[cq1];") != 0);
    T(strstr(w->graph, "[cq0]overlay=x=1402:y=772:eof_action=pass:repeatlast=0[cv0];") != 0);
    T(strstr(w->graph, "[cv0][cq1]overlay=x=1402:y=772:eof_action=pass:repeatlast=0[cv1];") != 0);
    T(strstr(w->graph, "[cv1]format=yuv420p[v];") != 0);
    // A clip without a webcam: the picture alone
    j.info.cam_w = j.info.cam_h = 0;
    w->graph_len = 0, w->graph[0] = 0;
    adv_graph(w, j, 0.5, "", "yuv420p", false, streams);
    T(strstr(w->graph, "[0:v:1]") == 0 && strstr(w->graph, "overlay") == 0);

    // The library: the picture is the first video stream that isn't the webcam, and the webcam's size is kept
    static char out[2048];
    snprintf(out, sizeof out, "%s",
             "[STREAM]\ncodec_type=audio\nchannels=2\nTAG:title=Mix\n[/STREAM]\n"
             "[STREAM]\ncodec_type=video\nwidth=1920\nheight=1080\navg_frame_rate=60/1\nTAG:handler_name=VideoHandler\n[/STREAM]\n"
             "[STREAM]\ncodec_type=video\nwidth=1280\nheight=720\navg_frame_rate=30/1\nTAG:title=Webcam\n"
             "TAG:handler_name=Webcam\n[/STREAM]\n"
             "[FORMAT]\nduration=12.500\nTAG:genre=Overwatch\nTAG:comment=rekodr 1.0\n[/FORMAT]\n");
    MediaInfo pm;
    char tagged[64];
    T(lib_probe_parse(out, &pm, tagged, sizeof tagged));
    T(pm.width == 1920 && pm.height == 1080 && near(pm.fps, 60) && pm.cam_w == 1280 && pm.cam_h == 720);
    T(pm.track_count == 1 && near(pm.duration, 12.5) && !strcmp(tagged, "Overwatch"));
    snprintf(out, sizeof out, "%s",
             "[STREAM]\ncodec_type=video\nwidth=2560\nheight=1440\navg_frame_rate=30/1\n[/STREAM]\n"
             "[FORMAT]\nduration=5\n[/FORMAT]\n");
    T(lib_probe_parse(out, &pm, tagged, sizeof tagged) && pm.width == 2560 && pm.cam_w == 0 && !tagged[0]);
    // And the probe cache keeps it; a cache file without it is probed again
    Path cache = {};
    swprintf(cache.s, MAX_PATH, L"%srekodr-test-cam-cache-%u", dir, GetCurrentProcessId());
    CreateDirectoryW(cache.s, 0);
    MediaInfo cm;
    lib_cache_write(cache, 7, m, "Overwatch");
    T(lib_cache_read(cache, 7, &cm, tagged, sizeof tagged) && cm.cam_w == 1280 && cm.cam_h == 720 && cm.width == 1920);
    Path cf = lib_cache_file(cache, 7, L".txt");
    FILE *f = _wfopen(cf.s, L"wb");
    if (f) fputs("duration = 5.000\nwidth = 1920\nheight = 1080\nfps = 30.000\ntagged = \n", f), fclose(f);
    T(!lib_cache_read(cache, 7, &cm, tagged, sizeof tagged));
    DeleteFileW(cf.s);
    RemoveDirectoryW(cache.s);
}

static const Layer *test_layer_named(const LayerSet &set, const char *name)
{
    for (i32 k = 0; k < set.count; k++)
        if (!strcmp(set.layers[k].name, name)) return &set.layers[k];
    return 0;
}

static void test_remove_dir(const Path &dir)
{
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(path_join(dir, L"*").s, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) DeleteFileW(path_join(dir, fd.cFileName).s);
        while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.s);
}

// Layers, through a test source: one of each kind registered, pushed to and a span replaced; the store read back
// from its files and pruned; a save's slice in the clip's time through its pieces, read back by the library; an
// edit's switches kept; and the render's .ass drawing what is switched on where the pieces play it
// A Siege round built by hand: its header, then the packets in zstd frames of raw blocks, with junk between them
struct TestBytes {
    u8 b[4096];
    size_t n;
    void put(const void *p, size_t k) { memcpy(b + n, p, k), n += k; }
    void byte(u8 v) { b[n++] = v; }
    void zeros(size_t k) { memset(b + n, 0, k), n += k; }
    void hstr(const char *s) { byte((u8)strlen(s)), zeros(7), put(s, strlen(s)); }
    void str(const char *s) { byte((u8)strlen(s)), put(s, strlen(s)); }
    void clock(u32 left) { put(R6_TIME, 4), byte(4), put(&left, 4); }
    void feed(const char *killer, const char *target)
    {
        put(R6_FEED, 5), zeros(38), byte(0), put(R6_KILL, 5), str(killer), zeros(15), str(target), zeros(56), byte(1);
    }
    void frame(const TestBytes &in) // a single-segment frame, one raw block
    {
        put(R6_ZSTD_MAGIC, 4);
        if (in.n < 256) byte(0x20), byte((u8)in.n); // its size in a byte
        else byte(0x60), byte((u8)(in.n - 256)), byte((u8)((in.n - 256) >> 8)); // in two, less 256
        u32 h = (u32)in.n << 3 | 1;
        byte((u8)h), byte((u8)(h >> 8)), byte((u8)(h >> 16));
        put(in.b, in.n);
    }
};

static void test_r6_replay()
{
    static TestBytes f, p1, p2;
    f = {}, p1 = {}, p2 = {};
    f.put("dissect", 7), f.byte(8), f.zeros(7), f.put("UNKNOWN", 7), f.zeros(7);
    const char *head[] = {"version", "Y9S1", "code", "8211379", "datetime", "2024-05-04-02-14-08", "recordingplayerid", "7",
                          "playerid", "5", "playername", "Other", "playerid", "7", "playername", "Me", "teamscore1", "0"};
    for (const char *h : head) f.hstr(h);
    // The prep clock, then the action phase: a kill 3 s in, a kill of someone else's, the same kill again, a death
    p1.clock(45), p1.clock(44), p1.clock(180), p1.clock(179), p1.feed("Me", "Other");
    p2.feed("Other", "Someone"), p2.feed("Me", "Other"), p2.clock(178), p2.clock(177), p2.feed("Other", "Me");
    f.frame(p1), f.put("junk", 4), f.frame(p2);
    ReplayMark m[8];
    const i64 start = r6_datetime("2024-05-04-02-14-08");
    // The PC's local time: the same moment as Unreal's local ticks for it, a whole quarter hour off UTC
    const i64 off = start - 1714788848ll * 1000000;
    T(start == fn_local_ticks_us((1714788848ll + 62135596800ll) * 10000000));
    T(off % (900ll * 1000000) == 0 && llabs(off) <= 14 * 3600ll * 1000000);
    T(r6_marks(f.b, f.n, m, 8) == 2);
    T(m[0].t_us == start + 3 * 1000000ll && m[0].colour == 4 && !strcmp(m[0].note, "Kill"));
    T(m[1].t_us == start + 5 * 1000000ll && m[1].colour == 1 && !strcmp(m[1].note, "Death"));
    // Cut short: what is whole is read; not a round, -1
    T(r6_marks(f.b, f.n - 20, m, 8) == 2 && r6_marks(f.b, f.n - 70, m, 8) == 1);
    T(r6_marks(f.b, 300, m, 8) == -1 && r6_marks((const u8 *)"dissect", 7, m, 8) == -1 && r6_marks(f.b, 0, m, 8) == -1);
}

// A Fortnite replay built by hand: its header and key, an event chunk per elimination, encrypted as the game does
struct TestReplay : TestBytes {
    u8 key[32];
    void u32le(u32 v) { put(&v, 4); }
    void fstring(const char *s) { u32le((u32)strlen(s) + 1), put(s, strlen(s) + 1); }
    void header(i64 ticks, bool live)
    {
        for (i32 i = 0; i < 32; i++) key[i] = (u8)(i * 7 + 1);
        u32le(FN_MAGIC), u32le(7), u32le(0), zeros(12), fstring(""), u32le(live), put(&ticks, 8), u32le(0);
        u32le(1), u32le(32), put(key, 32);
    }
    void elim(u32 at_ms, char victim, char killer, bool knock)
    {
        u8 d[256] = {};
        u32 dn = 4 + 1 + 160;
        d[0] = 10;
        const char who[2] = {victim, killer};
        for (char c : who) {
            d[dn++] = 0x11, d[dn++] = 16;
            memset(d + dn, (u8)c, 16), dn += 16;
        }
        dn++; // the weapon
        d[dn] = knock, dn += 4 + 4;
        u8 pad = (u8)(16 - dn % 16);
        memset(d + dn, pad, pad), dn += pad;
        BCRYPT_ALG_HANDLE alg;
        BCRYPT_KEY_HANDLE k;
        ULONG got;
        BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, 0, 0);
        BCryptSetProperty(alg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_ECB, sizeof BCRYPT_CHAIN_MODE_ECB, 0);
        BCryptGenerateSymmetricKey(alg, &k, 0, 0, key, 32, 0);
        BCryptEncrypt(k, d, dn, 0, 0, 0, d, dn, &got, 0);
        BCryptDestroyKey(k), BCryptCloseAlgorithmProvider(alg, 0);
        u32le(3);
        size_t size_at = n;
        u32le(0), fstring("event"), fstring("playerElim"), fstring(""), u32le(at_ms), u32le(at_ms), u32le(dn), put(d, dn);
        u32 size = (u32)(n - size_at - 4);
        memcpy(b + size_at, &size, 4);
    }
};

static void test_fn_replay()
{
    static TestReplay f;
    f = {};
    const i64 ticks = 638705738100000000ll; // 2024-12-23 23:43:30 on this PC's clock
    f.header(ticks, false);
    // A data chunk, skipped; then A knocks B and finishes them, takes C out, is knocked by B and finished by C, is
    // back and taken out by D; and E and F's own fight
    f.u32le(1), f.u32le(4), f.u32le(0);
    f.elim(1000, 'A', 'A', false); // the start of a respawn mode
    f.elim(10000, 'B', 'A', true), f.elim(20000, 'B', 'A', false), f.elim(30000, 'C', 'A', false);
    f.elim(40000, 'A', 'B', true), f.elim(50000, 'A', 'C', false), f.elim(60000, 'E', 'F', false);
    f.elim(200000, 'A', 'A', false);
    char owner[40];
    memset(owner, 'A', 32), owner[32] = 0;
    for (i32 i = 0; i < 32; i += 2) owner[i] = '4', owner[i + 1] = '1'; // 'A' is 0x41
    ReplayMark m[8];
    const i64 start = fn_local_ticks_us(ticks);
    T(start > 1734900000ll * 1000000 && start < 1735100000ll * 1000000);
    T(fn_marks(f.b, f.n, owner, m, 8) == 4);
    T(m[0].t_us == start + 10000000 && m[0].colour == 4 && !strcmp(m[0].note, "Knock"));
    T(m[1].t_us == start + 30000000 && m[1].colour == 4 && !strcmp(m[1].note, "Kill"));
    T(m[2].t_us == start + 40000000 && m[2].colour == 1 && !strcmp(m[2].note, "Knocked"));
    T(m[3].t_us == start + 200000000 && m[3].colour == 1 && !strcmp(m[3].note, "Death"));
    // Whose it is comes from the log; without that, nothing. Still recording, or not a replay: -1
    T(fn_marks(f.b, f.n, 0, m, 8) == 0 && fn_marks(f.b, f.n - 5, owner, m, 8) == 3);
    static TestReplay live;
    live = {};
    live.header(ticks, true);
    T(fn_marks(live.b, live.n, owner, m, 8) == -1 && fn_marks(f.b, 10, owner, m, 8) == -1);
    char id[40];
    const char *log = "[2024.12.23-23.40.01:000][  0]LogOnlineAccount: Successfully logged in user. UserId=[41414141414141414141414141414141]\n"
                      "LogOnline: Successfully logged in user. UserId=[0123456789ABCDEF0123456789abcdef] DisplayName=[x]\n"
                      "Successfully logged in user. UserId=[123]\n";
    T(fn_log_owner(log, id) && !strcmp(id, "0123456789abcdef0123456789abcdef"));
    T(!fn_log_owner("LogOnline: nothing", id) && !id[0]);

    // The watcher: a finished replay in Demos is read once, even after a restart, and sends the events asked for
    Path keep = g_app.paths.local_dir, home;
    wchar_t temp[MAX_PATH], demos[MAX_PATH], logs[MAX_PATH], file[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    swprintf(home.s, MAX_PATH, L"%srekodr-tests-rp-%lu", temp, GetCurrentProcessId());
    swprintf(demos, MAX_PATH, L"%s\\Saved\\Demos", home.s);
    swprintf(logs, MAX_PATH, L"%s\\Saved\\Logs", home.s);
    SHCreateDirectoryExW(0, demos, 0), SHCreateDirectoryExW(0, logs, 0);
    g_app.paths.local_dir = home;
    auto put = [](const wchar_t *path, const void *data, size_t n, i64 ago_us) {
        FILE *o = _wfopen(path, L"wb");
        fwrite(data, 1, n, o);
        fclose(o);
        HANDLE h = CreateFileW(path, FILE_WRITE_ATTRIBUTES, 0, 0, OPEN_EXISTING, 0, 0);
        i64 t = (now_us() - ago_us) * 10 + 116444736000000000LL;
        FILETIME ft = {(DWORD)t, (DWORD)(t >> 32)};
        SetFileTime(h, 0, 0, &ft), CloseHandle(h);
    };
    swprintf(file, MAX_PATH, L"%s\\FortniteGame.log", logs);
    put(file, log, strlen(log), 0); // the log says whose; its last login is 0123..., then A's below
    const char *mine = "LogOnline: Successfully logged in user. UserId=[41414141414141414141414141414141]\n";
    put(file, mine, strlen(mine), 0);
    swprintf(file, MAX_PATH, L"%s\\done.replay", demos);
    put(file, f.b, f.n, 3600000000ll);
    swprintf(file, MAX_PATH, L"%s\\now.replay", demos);
    put(file, f.b, f.n, 0);
    swprintf(file, MAX_PATH, L"%s\\old.replay", demos);
    put(file, f.b, f.n, 40ll * 86400 * 1000000);
    Event e;
    while (event_take(&e)) {}
    auto poll = [&]() {
        g_rp.next = 0;
        replays_poll();
        i32 n = 0;
        while (event_take(&e)) n += e.kind == EV_MARKER && e.b == 4 && e.a >= start;
        return n;
    };
    g_rp = {};
    wcscpy_s(g_rp.fn_dir, demos);
    g_am.events[AMG_FORTNITE] = AM_KILL;
    T(poll() == 2 && poll() == 0);
    g_rp.loaded = false; // a restart: replays.tsv says it was read
    T(poll() == 0);
    g_am.events[AMG_FORTNITE] = 0;
    g_rp = {};
    g_app.paths.local_dir = keep;
    SHFILEOPSTRUCTW op = {};
    wchar_t from[MAX_PATH + 1] = {};
    wcscpy_s(from, home.s);
    op.wFunc = FO_DELETE, op.pFrom = from, op.fFlags = FOF_NO_UI;
    SHFileOperationW(&op);
}

static void test_layers()
{
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    Path dir = {}, save_dir = {};
    swprintf(dir.s, MAX_PATH, L"%srekodr-test-layers-%u", tmp, GetCurrentProcessId());
    swprintf(save_dir.s, MAX_PATH, L"%srekodr-test-layers-save-%u", tmp, GetCurrentProcessId());
    auto fresh_store = [&]() {
        for (i32 k = 0; k < g_layers.set.count; k++) layer_free_samples(&g_layers.set.layers[k]);
        g_layers = {};
        g_layers.dir = dir;
    };
    fresh_store();
    const i64 T0 = 1000000000000ll, S = 1000000;
    i32 hr = layer_register("test heart", LAYER_VALUE, "Heart rate", "bpm", 50, 190),
        words = layer_register("test words", LAYER_TEXT, 0, 0, 0, 0), keys = layer_register("test keys", LAYER_INPUT, 0, 0, 0, 0);
    T(hr == 0 && words == 1 && keys == 2);
    T(layer_register("test heart", LAYER_VALUE, "Heart rate", "bpm", 50, 190) == hr &&
      layer_register("test heart", LAYER_TEXT, 0, 0, 0, 0) == -1);
    T(layer_register("", LAYER_VALUE, 0, 0, 0, 0) == -1 && layer_register(" x", LAYER_VALUE, 0, 0, 0, 0) == -1 &&
      layer_register("a\tb", LAYER_VALUE, 0, 0, 0, 0) == -1 && layer_register("x", LAYER_KINDS, 0, 0, 0, 0) == -1 &&
      layer_register("a|b", LAYER_VALUE, 0, 0, 0, 0) == -1);
    // The label is the name when none is given; a range only on a Value, and only when it is one
    T(!strcmp(g_layers.set.layers[words].label, "test words") && g_layers.set.layers[words].hi == 0);
    T(!strcmp(g_layers.set.layers[hr].label, "Heart rate") && !strcmp(g_layers.set.layers[hr].unit, "bpm"));
    // A minute: the heart every 10 s from 80, a line every 20 s, W held from 5 s to 25 s and Space tapped at 40 s
    for (i32 i = 0; i < 6; i++) T(layer_value(hr, T0 + i * 10 * S, 80 + i));
    layer_text(words, T0, T0 + 4 * S, "first words");
    layer_text(words, T0 + 20 * S, T0 + 24 * S, "rough guess");
    layer_text(words, T0 + 40 * S, T0 + 44 * S, "last\twords");
    layer_input(keys, T0 + 5 * S, 'W', true);
    layer_input(keys, T0 + 25 * S, 'W', false);
    layer_input(keys, T0 + 40 * S, VK_SPACE, true);
    layer_input(keys, T0 + 40 * S + S / 10, VK_SPACE, false);
    // The source's better guess at 15-30 s, in place of its own
    LayerSample better[2] = {};
    better[0].t_us = T0 + 26 * S, better[0].end_us = T0 + 28 * S, better[0].text = (char *)"and more";
    better[1].t_us = T0 + 21 * S, better[1].end_us = T0 + 23 * S, better[1].text = (char *)"better words";
    T(layer_replace(words, T0 + 15 * S, T0 + 30 * S, better, 2));
    T(!layer_replace(7, 0, 1, 0, 0) && !layer_value(-1, T0, 1) && !layer_value(hr, 0, 1));

    // Read again from its files, as after a restart: the same samples, each layer under its own name
    fresh_store();
    layers_load_locked(&g_layers); // the header as read back, before a register could set it again
    const Layer *heart = test_layer_named(g_layers.set, "test heart");
    T(g_layers.set.count == 3 && heart && heart->lo == 50 && heart->hi == 190 && !strcmp(heart->label, "Heart rate") &&
      !strcmp(heart->unit, "bpm"));
    hr = layer_register("test heart", LAYER_VALUE, "Heart rate", "bpm", 50, 190);
    words = layer_register("test words", LAYER_TEXT, 0, 0, 0, 0);
    keys = layer_register("test keys", LAYER_INPUT, 0, 0, 0, 0);
    T(g_layers.set.count == 3 && hr >= 0 && words >= 0 && keys >= 0);
    const Layer &w0 = g_layers.set.layers[words];
    T(g_layers.set.layers[hr].count == 6 && w0.count == 4 && g_layers.set.layers[keys].count == 4);
    T(w0.s[1].t_us == T0 + 21 * S && !strcmp(w0.s[1].text, "better words") && !strcmp(w0.s[2].text, "and more"));
    T(!strcmp(w0.s[3].text, "last words") && w0.s[3].end_us == T0 + 44 * S);

    // A save of 8-50 s with a gap at 20-30 s: the heart from its last before, the lines that overlap, W held from
    // the start; then in the file's time, the gap closed up
    static LayerSet set;
    layers_slice(T0 + 8 * S, T0 + 50 * S, &set);
    const Layer *h = test_layer_named(set, "test heart"), *t = test_layer_named(set, "test words"),
                *k = test_layer_named(set, "test keys");
    T(set.count == 3 && h && t && k);
    if (h && t && k) {
        T(h->count == 5 && h->s[0].t_us == T0 && h->s[0].value == 80 && h->s[4].value == 84);
        T(t->count == 3 && !strcmp(t->s[0].text, "better words"));
        T(k->count == 4 && k->s[0].t_us == T0 + 8 * S && k->s[0].code == 'W' && k->s[0].value == 1);
    }
    layer_set_free(&set);
    SaveWork *w = &g_save_work;
    Path keep_save = w->cfg.save_dir, keep_lib = g_app.config.save_dir;
    w->cfg.save_dir = g_app.config.save_dir = save_dir;
    w->piece_count = 2;
    w->pieces[0] = {T0 + 8 * S, T0 + 20 * S, 0};
    w->pieces[1] = {T0 + 30 * S, T0 + 50 * S, 12 * S};
    Path video = {};
    swprintf(video.s, MAX_PATH, L"%s\\Clips\\2026\\Test clip.mp4", save_dir.s);
    save_layers(w, T0 + 8 * S, T0 + 50 * S, video);
    LibItem it = {};
    str_set(it.name, "Test clip");
    Path data = save_data_dir(save_dir);
    T(!wcscmp(lib_layers_path(it).s, path_join(data, L"Test clip.layers.txt").s));
    { // on disk: the wall clock in UTC microseconds, and the pieces that place it in the video
        static char text[8192];
        FILE *lf = _wfopen(lib_layers_path(it).s, L"rb");
        text[0] = 0;
        if (lf) text[fread(text, 1, sizeof text - 1, lf)] = 0, fclose(lf);
        T(strstr(text, "piece = 1000008000000 1000020000000 0\n") && strstr(text, "piece = 1000030000000 1000050000000 12000000\n"));
        T(strstr(text, "v = 1000000000000 80\n") && strstr(text, "i = 1000025000000 87 0\n"));
    }
    static LayerSet clip;
    T(lib_layers_read(it, &clip) && clip.count == 3);
    Path files[LIB_ITEM_FILES];
    T(lib_item_files(it, files) == 2 && !wcscmp(files[1].s, lib_layers_path(it).s));
    h = test_layer_named(clip, "test heart"), t = test_layer_named(clip, "test words"), k = test_layer_named(clip, "test keys");
    T(h && t && k);
    if (h && t && k) {
        T(h->count == 5 && h->s[0].t_us == 0 && h->s[1].t_us == 2 * S && h->s[2].t_us == 12 * S && h->s[3].t_us == 12 * S);
        T(h->s[4].t_us == 22 * S && h->s[4].value == 84);
        T(t->count == 1 && t->s[0].t_us == 22 * S && t->s[0].end_us == 26 * S && !strcmp(t->s[0].text, "last words"));
        T(k->count == 4 && k->s[0].t_us == 0 && k->s[1].t_us == 12 * S && k->s[1].value == 0 && k->s[2].code == VK_SPACE);
        T(k->s[3].t_us == 22 * S + S / 10);
        T(!strcmp(h->label, "Heart rate") && !strcmp(h->unit, "bpm") && h->lo == 50 && h->hi == 190);
    }
    // Nothing in a slice (before the first sample): the file still has the pieces, for markers that come later
    swprintf(video.s, MAX_PATH, L"%s\\Clips\\2026\\Empty clip.mp4", save_dir.s);
    save_layers(w, T0 - 100 * S, T0 - 80 * S, video);
    Path late_file = layers_video_path(save_dir, video);
    static LayerSet none;
    T(!wcscmp(late_file.s, path_join(data, L"Empty clip.layers.txt").s) && !layers_read(late_file, &none) && GetFileAttributesW(late_file.s) != INVALID_FILE_ATTRIBUTES);
    layer_set_free(&none);
    // Late markers: in video time, oldest first; one in the gap between pieces, or a second of the same, isn't added
    T(layers_mark_add(late_file, T0 + 40 * S, 4, "Kill") && layers_mark_add(late_file, T0 + 10 * S, 1, "Death"));
    T(!layers_mark_add(late_file, T0 + 25 * S, 4, "Gap") && !layers_mark_add(late_file, T0 + 60 * S, 4, "After"));
    T(!layers_mark_add(late_file, T0 + 40 * S, 4, "Kill"));
    Marker late[8];
    T(layers_marks_read(late_file, late, 8) == 2);
    T(late[0].t_us == 2 * S && late[0].colour == 1 && !strcmp(late[0].note, "Death"));
    T(late[1].t_us == 22 * S && late[1].colour == 4 && !strcmp(late[1].note, "Kill"));
    T(!layers_read(late_file, &none));
    layer_set_free(&none);
    T(layers_marks_read(path_join(data, L"No such clip.layers.txt"), late, 8) == 0);
    w->piece_count = 0;

    // An edit's switches: kept on disk, kept through Advanced and back
    static EditRecord e, er;
    static MediaInfo m;
    e = {}, m = {};
    m.duration = 32, m.width = 1920, m.height = 1080, m.fps = 30;
    str_set(e.layer_on[0], "test heart"), str_set(e.layer_on[1], "test words"), str_set(e.layer_on[2], "test keys");
    e.layer_on_count = 3;
    e.pieces[0] = {10, 30};
    e.piece_count = 1;
    T(!edit_is_default(e));
    Path ef = path_join(data, L"Test clip.edit.txt");
    edit_write(ef, e);
    edit_read(ef, &er);
    T(er.layer_on_count == 3 && !strcmp(er.layer_on[1], "test words") && er.piece_count == 1);
    DeleteFileW(ef.s);
    er = e;
    edit_to_advanced(&er, m);
    T(er.mode == EDIT_ADVANCED && er.layer_on_count == 3 && !strcmp(er.layer_on[2], "test keys"));
    edit_to_simple(&er, m);
    T(er.layer_on_count == 3);

    // The render's layers: 10-30 s of the clip played from the start; the heart's number at the top right while it
    // holds, the line at the bottom, the keys held at the bottom left
    LayerSpan spans[] = {{10, 30, 0}};
    T(layers_ass(0, clip, e.layer_on, 0, spans, 1, 20, 1920, 1080) == 0);
    T(layers_ass(0, clip, e.layer_on, 3, spans, 1, 20, 1920, 1080) == 6);
    T(layers_ass(0, clip, e.layer_on + 1, 1, spans, 1, 20, 1920, 1080) == 1);
    Path ass = path_join(data, L"test.ass");
    FILE *f = _wfopen(ass.s, L"wb");
    if (f) layers_ass(f, clip, e.layer_on, 3, spans, 1, 20, 1920, 1080), fclose(f);
    static char text[8192];
    text[0] = 0;
    f = _wfopen(ass.s, L"rb");
    if (f) text[fread(text, 1, sizeof text - 1, f)] = 0, fclose(f);
    DeleteFileW(ass.s);
    T(strstr(text, "PlayResX: 1920\nPlayResY: 1080\n") != 0);
    T(strstr(text, "Style: Value,Segoe UI,49,") != 0 && strstr(text, ",9,36,36,36,1\n") != 0);
    T(strstr(text, "Dialogue: 0,0:00:00.00,0:00:02.00,Value,,0,0,36,,81\n") != 0);
    T(strstr(text, "Dialogue: 0,0:00:02.00,0:00:12.00,Value,,0,0,36,,83\n") != 0);
    T(strstr(text, "Dialogue: 0,0:00:12.00,0:00:20.00,Value,,0,0,36,,84\n") != 0);
    T(strstr(text, ",82\n") == 0);
    T(strstr(text, "Dialogue: 0,0:00:12.00,0:00:16.00,Text,,0,0,36,,last words\n") != 0);
    T(strstr(text, "Dialogue: 0,0:00:00.00,0:00:02.00,Input,,0,0,36,,W\n") != 0);
    T(strstr(text, "Dialogue: 0,0:00:12.00,0:00:12.10,Input,,0,0,36,,#32\n") != 0);
    // Two of a kind stack, and a line's braces and backslashes stay text
    static LayerSet two;
    two = {};
    two.count = 2;
    str_set(two.layers[0].name, "a"), str_set(two.layers[1].name, "b");
    two.layers[0].kind = two.layers[1].kind = LAYER_TEXT;
    LayerSample line = {};
    line.t_us = 11 * S, line.end_us = 12 * S, line.text = (char *)"x {y} \\n";
    layer_insert(&two.layers[0], layer_sample_clean(LAYER_TEXT, line));
    layer_insert(&two.layers[1], layer_sample_clean(LAYER_TEXT, line));
    char on2[2][LAYER_NAME_MAX] = {"a", "b"};
    f = _wfopen(ass.s, L"wb");
    i32 events = 0;
    if (f) events = layers_ass(f, two, on2, 2, spans, 1, 20, 1920, 1080), fclose(f);
    T(events == 2);
    f = _wfopen(ass.s, L"rb");
    text[0] = 0;
    if (f) text[fread(text, 1, sizeof text - 1, f)] = 0, fclose(f);
    DeleteFileW(ass.s);
    T(strstr(text, "Text,,0,0,36,,x \\{y\\} \\") != 0 && strstr(text, "Text,,0,0,97,,x") != 0);
    T(strstr(text, "\\n\n") == 0);
    layer_set_free(&two);
    layer_set_free(&clip);

    // Spans held until the next: Discord 0-10 s, a 1 s flick to Steam, Discord on to 20 s, nothing in front 20-30 s,
    // then the game from 30 s on. Shown steady: Discord 0-20 s whole, a gap, the game from 30 s.
    i32 front = layer_register("test front", LAYER_SPANS, "Front window", 0, 0, 0);
    T(front >= 0);
    layer_hold(front, T0, "Discord");
    layer_hold(front, T0 + 10 * S, "Steam");
    layer_hold(front, T0 + 11 * S, "Discord");
    layer_hold(front, T0 + 20 * S, "");
    layer_hold(front, T0 + 30 * S, "Overwatch");
    const Layer &fl = g_layers.set.layers[front];
    T(fl.count == 5 && fl.s[0].end_us == 0 && layer_end_of(fl, 0, 0) == T0 + 10 * S && layer_end_of(fl, 4, 7) == 7);
    LayerRun runs[8];
    i32 nr = layer_steady_runs(fl, T0, T0 + 60 * S, T0 + 60 * S, LAYER_STEADY_US, runs, COUNT(runs));
    T(nr == 2 && runs[0].a == T0 && runs[0].z == T0 + 20 * S && !strcmp(runs[0].label, "Discord"));
    T(runs[1].a == T0 + 30 * S && runs[1].z == T0 + 60 * S && !strcmp(runs[1].label, "Overwatch"));
    // Unsteady, every switch shows; a view starting mid-run keeps the run's start
    T(layer_steady_runs(fl, T0, T0 + 60 * S, T0 + 60 * S, 0, runs, COUNT(runs)) == 4);
    T(layer_steady_runs(fl, T0 + 15 * S, T0 + 16 * S, T0 + 60 * S, LAYER_STEADY_US, runs, COUNT(runs)) == 1 &&
      runs[0].a == T0 && runs[0].z == T0 + 20 * S);
    // A save of 5-40 s: the held lines get their ends, the last one the save's end; in the clip they are spans again
    layers_slice(T0 + 5 * S, T0 + 40 * S, &set);
    const Layer *fs = test_layer_named(set, "test front");
    T(fs && fs->kind == LAYER_SPANS && fs->count == 5 && fs->s[0].end_us == T0 + 10 * S && fs->s[4].end_us == T0 + 40 * S);
    T(fs && !strcmp(fs->label, "Front window"));
    layer_set_free(&set);
    swprintf(video.s, MAX_PATH, L"%s\\Clips\\2026\\Front clip.mp4", save_dir.s);
    w->piece_count = 1;
    w->pieces[0] = {T0 + 5 * S, T0 + 40 * S, 0};
    save_layers(w, T0 + 5 * S, T0 + 40 * S, video);
    w->piece_count = 0;
    str_set(it.name, "Front clip");
    T(lib_layers_read(it, &clip));
    fs = test_layer_named(clip, "test front");
    T(fs && fs->kind == LAYER_SPANS && fs->count == 5 && fs->s[0].t_us == 0 && fs->s[0].end_us == 5 * S);
    T(fs && fs->s[3].text && !fs->s[3].text[0] && !strcmp(fs->s[4].text, "Overwatch") && fs->s[4].end_us == 35 * S);
    // Burned in at the top left, steady
    char on_front[1][LAYER_NAME_MAX] = {"test front"};
    LayerSpan whole[] = {{0, 35, 0}};
    f = _wfopen(ass.s, L"wb");
    if (f) layers_ass(f, clip, on_front, 1, whole, 1, 35, 1920, 1080), fclose(f);
    f = _wfopen(ass.s, L"rb");
    text[0] = 0;
    if (f) text[fread(text, 1, sizeof text - 1, f)] = 0, fclose(f);
    DeleteFileW(ass.s);
    T(strstr(text, "Style: Spans,") != 0 && strstr(text, "0:00:00.00,0:00:15.00,Spans,,0,0,36,,Discord\n") != 0);
    T(strstr(text, "0:00:25.00,0:00:35.00,Spans,,0,0,36,,Overwatch\n") != 0 && strstr(text, "Steam") == 0);
    layer_set_free(&clip);

    // Pruned as the buffer moves on: not until the oldest is well behind, then all but the heart's last
    layers_prune(T0 + 45 * S);
    T(g_layers.set.layers[hr].count == 6 && g_layers.set.layers[keys].count == 4);
    layers_prune(T0 + 700 * S);
    fresh_store();
    hr = layer_register("test heart", LAYER_VALUE, "Heart rate", "bpm", 50, 190), words = layer_register("test words", LAYER_TEXT, 0, 0, 0, 0);
    keys = layer_register("test keys", LAYER_INPUT, 0, 0, 0, 0);
    T(g_layers.set.layers[hr].count == 1 && g_layers.set.layers[hr].s[0].value == 85);
    T(g_layers.set.layers[words].count == 0 && g_layers.set.layers[keys].count == 0);

    fresh_store();
    g_layers.dir = {};
    test_remove_dir(dir);
    test_remove_dir(data);
    RemoveDirectoryW(save_dir.s);
    w->cfg.save_dir = keep_save, g_app.config.save_dir = keep_lib;
}

// Plugins: plugin.txt read and refused, and the lines a plugin sends kept or dropped
static void test_plugins()
{
    // The timeline's value colours run grey to red, through green
    T(tl_value_colour(0) == 0x96a0a8 && tl_value_colour(1) == 0xe82014 && tl_value_colour(-3) == 0x96a0a8 &&
      tl_value_colour(0.42) == 0x3cbe5a);
    // Data rows: only those in timeline_show, never Input
    {
        Config rc = {};
        Layer in = {}, val = {};
        in.kind = LAYER_INPUT, val.kind = LAYER_VALUE;
        str_set(in.name, "p/keys");
        str_set(val.name, "p/heart");
        str_set(rc.timeline_show, "Clips|p/keys|p/heart");
        T(tl_data_shown(rc, val) && !tl_data_shown(rc, in));
        str_set(rc.timeline_show, "Clips");
        T(!tl_data_shown(rc, val));
    }

    char list[64] = "";
    word_list_toggle(list, sizeof list, "heart");
    word_list_toggle(list, sizeof list, "keys");
    T(!strcmp(list, "heart|keys") && word_list_has(list, "keys") && !word_list_has(list, "key"));
    word_list_toggle(list, sizeof list, "heart");
    T(!strcmp(list, "keys") && !word_list_has(list, "heart") && !word_list_has(list, ""));

    Plugin p;
    T(plugin_manifest_parse("heart", "# a test\r\nname = Heart\r\nexe = heart.exe\r\nrun = game\r\n"
                                     "layer = bpm | value | Heart rate | bpm | 50-190\r\n"
                                     "layer = song | text\nlayer = app | spans | Window\n"
                                     "layer = keys | input | Keys | 0x57 65, 32\n", &p));
    T(!strcmp(p.name, "Heart") && !wcscmp(p.exe, L"heart.exe") && p.game_only && p.layer_count == 4);
    T(p.layers[0].kind == LAYER_VALUE && !strcmp(p.layers[0].unit, "bpm") && p.layers[0].lo == 50 &&
      p.layers[0].hi == 190 && !strcmp(p.layers[0].label, "Heart rate"));
    T(p.layers[1].kind == LAYER_TEXT && !strcmp(p.layers[1].label, "song") && p.layers[2].kind == LAYER_SPANS);
    T(p.layers[3].code_count == 3 && p.layers[3].codes[0] == 'W' && p.layers[3].codes[1] == 'A' &&
      p.layers[3].codes[2] == ' ');
    T(plugin_manifest_parse("x", "exe = x.exe\nlayer = a | value\n", &p) && !strcmp(p.name, "x") && !p.game_only);
    const char *bad[] = {
        "layer = a | value\n",                                  // no exe
        "exe = x.exe\n",                                        // no layers
        "exe = ..\\x.exe\nlayer = a | value\n",                 // not in the folder
        "exe = x.bat\nlayer = a | value\n",                     // not a program
        "exe = x.exe\nrun = sometimes\nlayer = a | value\n",
        "exe = x.exe\nlayer = a b | value\n",                   // the id has a space
        "exe = x.exe\nlayer = a | sound\n",
        "exe = x.exe\nlayer = a | value | A | u | 9-3\n",       // the range runs backwards
        "exe = x.exe\nlayer = a | input | A\n",                 // no codes
        "exe = x.exe\nlayer = a | input | A | W\n",             // a code that isn't a number
        "exe = x.exe\nlayer = a | value\nlayer = a | text\n",   // twice
        "exe = x.exe\nlayer = a | value\nlayer = b | value\nlayer = c | value\nlayer = d | value\nlayer = e | value\n",
        "exe = x.exe\nwhat\nlayer = a | value\n",
    };
    for (const char *b : bad) T(!plugin_manifest_parse("x", b, &p) && p.why[0]);
    T(!plugin_manifest_parse("a folder name far too long", "exe = x.exe\nlayer = a | value\n", &p));

    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    Path dir = {};
    swprintf(dir.s, MAX_PATH, L"%srekodr-test-plugins-%u", tmp, GetCurrentProcessId());
    for (i32 k = 0; k < g_layers.set.count; k++) layer_free_samples(&g_layers.set.layers[k]);
    g_layers = {};
    g_layers.dir = dir;
    T(plugin_manifest_parse("heart", "exe = heart.exe\nlayer = bpm | value | Heart rate | bpm | 50-190\n"
                                     "layer = song | text\nlayer = app | spans\nlayer = keys | input | Keys | 87 65\n",
                            &p));
    T(plugin_register(&p));
    const Layer *bpm = test_layer_named(g_layers.set, "heart/bpm");
    T(bpm && bpm->kind == LAYER_VALUE && bpm->lo == 50 && bpm->hi == 190 && !strcmp(bpm->label, "Heart rate"));
    T(test_layer_named(g_layers.set, "heart/keys") != 0);
    const i64 T0 = 1000000000000ll, MS = 1000;
    auto line = [&](const char *s, i64 t) { return plugin_line(&p, s, (i32)strlen(s), t); };
    T(line("v bpm 80", T0) == PDROP_NONE && line("v bpm 81.5 ", T0 + 150 * MS) == PDROP_NONE);
    T(line("v bpm 99", T0 + 200 * MS) == PDROP_FAST);          // under 100 ms after the last one kept
    T(line("v bpm x", T0 + 400 * MS) == PDROP_BAD && line("v bpm", T0 + 400 * MS) == PDROP_BAD &&
      line("v bpm 1e999", T0 + 400 * MS) == PDROP_BAD && line("v song 3", T0 + 400 * MS) == PDROP_BAD);
    T(line("v pulse 80", T0) == PDROP_ID && line("x bpm 80", T0) == PDROP_BAD && line("v  80", T0) == PDROP_BAD);
    T(line("t song Some song", T0) == PDROP_NONE && line("t app Discord", T0) == PDROP_NONE &&
      line("t song", T0 + 5000 * MS) == PDROP_NONE && line("t bpm 3", T0) == PDROP_BAD);
    T(line("k keys 87 down", T0) == PDROP_NONE && line("k keys 0x57 up", T0 + 50 * MS) == PDROP_NONE);
    T(line("k keys 32 down", T0) == PDROP_CODE && line("k keys 87 held", T0) == PDROP_BAD &&
      line("k keys down", T0) == PDROP_BAD);
    char lng[700];
    memset(lng, 'a', sizeof lng);
    memcpy(lng, "t song ", 7);
    lng[sizeof lng - 1] = 0;
    T(line(lng, T0) == PDROP_LONG);
    bpm = test_layer_named(g_layers.set, "heart/bpm");
    const Layer *song = test_layer_named(g_layers.set, "heart/song"), *keys = test_layer_named(g_layers.set, "heart/keys");
    T(bpm->count == 2 && bpm->s[1].value == 81.5 && bpm->s[1].t_us == T0 + 150 * MS);
    T(song->count == 2 && !strcmp(song->s[0].text, "Some song") && song->s[0].end_us == 0 && (!song->s[1].text || !song->s[1].text[0]));
    T(keys->count == 2 && keys->s[0].code == 87 && keys->s[0].value == 1 && keys->s[1].value == 0);

    // Spikes on 50-190: a jump of 18% of the range (25.2) over the mean of the samples 3-6 before, held while 9%
    // (12.6) over it. 80 steady, up to 110 and 120, 100 (still 20 over), then 85: one spike, its peak 120.
    {
        Layer v = {};
        v.kind = LAYER_VALUE, v.lo = 50, v.hi = 190;
        f64 vals[] = {80, 80, 80, 80, 80, 80, 104, 110, 120, 100, 85, 80, 80, 80, 80, 80, 80, 80};
        for (i32 i = 0; i < COUNT(vals); i++) {
            LayerSample sm = {};
            sm.t_us = T0 + i * 1000 * MS, sm.value = vals[i];
            layer_insert(&v, sm);
        }
        LayerSpike sp[4];
        f64 lo, hi;
        layer_range(v, &lo, &hi);
        T(lo == 50 && hi == 190);
        T(layer_spikes(v, lo, hi, sp, COUNT(sp)) == 1 && sp[0].a == T0 + 7000 * MS && sp[0].z == T0 + 9000 * MS &&
          sp[0].peak == 120 && sp[0].peak_us == T0 + 8000 * MS);
        v.lo = v.hi = 0; // no range of its own: the samples' 80-120
        layer_range(v, &lo, &hi);
        T(lo == 80 && hi == 120);
        layer_free_samples(&v);
    }

    // Lines as they come down a pipe: split at \n, \r\n taken off, a long one whole in length, the last unended
    HANDLE r, w;
    CreatePipe(&r, &w, 0, 1 << 16);
    char big[1200];
    memset(big, 'b', sizeof big);
    DWORD put;
    WriteFile(w, "v bpm 80\r\nt song x\n\n", 20, &put, 0);
    WriteFile(w, big, sizeof big, &put, 0);
    WriteFile(w, "\nk keys 87 up", 13, &put, 0);
    CloseHandle(w);
    char got[4][40] = {};
    i32 lens[4] = {}, n = 0;
    plugin_read_lines(r, [&](const char *s, i32 len) {
        if (n < 4) snprintf(got[n], sizeof got[n], "%s", s), lens[n] = len;
        n++;
    });
    CloseHandle(r);
    T(n == 4 && !strcmp(got[0], "v bpm 80") && lens[0] == 8 && !strcmp(got[1], "t song x"));
    T(lens[2] == 1200 && !strcmp(got[3], "k keys 87 up"));

    for (i32 k = 0; k < g_layers.set.count; k++) layer_free_samples(&g_layers.set.layers[k]);
    g_layers = {};
    test_remove_dir(dir);
}

// The social copy: whole when it fits, else a preview as long as the budget allows; the edit cut to its first part
static void test_social_preview()
{
    const i64 MB = 1024 * 1024;
    T(social_preview_len(60, true, 150 * MB, 1080, 5000) == 0); // a minute fits at well over the floor
    f64 p = social_preview_len(25 * 60, true, 150 * MB, 1080, 5000);
    T(!SOCIAL_PREVIEW || (p > 200 && p < 240 && p == floor(p)));   // about 3.8 minutes at 5 Mbps
    T(!SOCIAL_PREVIEW || social_preview_len(25 * 60, true, 150 * MB, -1, 5000) == p);
    T(social_preview_len(20, true, 150 * MB, -1, 5000) == 0);      // shorter than the preview would be
    T(social_whole_fits(200, true, 150 * MB, 5000) && !social_whole_fits(300, true, 150 * MB, 5000));

    EditRecord e = {};
    e.pieces[0] = {10, 40};
    e.pieces[1] = {100, 200};
    e.piece_count = 2;
    e.fade_out = 1;
    edit_first(&e, 600, 50);
    T(e.piece_count == 2 && e.pieces[0].to == 40 && e.pieces[1].from == 100 && e.pieces[1].to == 120);
    T(pieces_len(e, 600) == 50 && e.fade_out == 0);
    EditRecord w = {};
    edit_first(&w, 600, 90);
    T(w.piece_count == 1 && w.pieces[0].from == 0 && w.pieces[0].to == 90);

    EditRecord a = {};
    a.mode = EDIT_ADVANCED;
    a.items[0] = {0, 0, 30, 0, 0, 0, 1, 0, 0};
    a.items[1] = {30, 50, 100, 0, 2, 0, 2, 0, 0};
    a.items[2] = {90, 200, 210, 0, 0, 0, 3, 0, 0};
    a.item_count = 3;
    a.ikeys[0].item = 2;
    a.ikeys[1].item = 3;
    a.ikey_count = 2;
    edit_first(&a, 600, 60);
    T(a.item_count == 2 && a.items[1].to == 80 && a.items[1].fade_out == 0 && adv_len(a) == 60);
    T(a.ikey_count == 1 && a.ikeys[0].item == 2);

    // a session keeps its tracks in the bucket: a file in a year folder of a Sessions folder
    T(path_is_session(L"D:\\Videos\\Rekodr\\Sessions\\2026\\Overwatch 2026-11-24.mp4"));
    T(path_is_session(L"D:\\Videos\\Rekodr\\sessions\\2026\\a.mp4"));
    T(!path_is_session(L"D:\\Videos\\Rekodr\\Clips\\2026\\Sessions.mp4"));
    T(!path_is_session(L"D:\\Videos\\Rekodr\\Sessions\\a.mp4") && !path_is_session(L"D:\\Sessions\\20x6\\a.mp4"));
    T(!path_is_session(L"D:\\Videos\\Rekodr\\Clips\\2026\\a.mp4") && !path_is_session(L"a.mp4"));
}

static void test_edit_xml()
{
    Editor &ed = g_app.editor;
    static const Editor blank = {};
    ed = blank;
    wchar_t dir[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    swprintf(ed.path.s, MAX_PATH, L"%srekodr test %u & co.mp4", dir, GetCurrentProcessId());
    str_set(ed.item.name, "rekodr test & co");
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
    T(strstr(text, "<name>rekodr test &amp; co</name>"));
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

    // The same as an Advanced edit with the Mic's second item moved 5 s later: each item a clip where it was put, the
    // sequence as long as the last one's end, a clip per item per channel track
    edit_to_advanced(&e, m);
    i32 idx[EDIT_MAX_ITEMS], nv = track_items(e, 0, idx), nm = track_items(e, 2, idx), ng = track_items(e, 3, idx);
    track_items(e, 2, idx);
    EditItem &mic = e.items[idx[nm - 1]];
    mic.start += 5;
    T(ed_export_xml(&xml));
    f = _wfopen(xml.s, L"rb");
    n = f ? fread(text, 1, sizeof text - 1, f) : 0;
    if (f) fclose(f);
    text[n] = 0;
    DeleteFileW(xml.s);
    T(xml_balanced(text) && nv == 2 && nm == 2 && ng == 2);
    char want_s[96];
    i64 ms = (i64)(mic.start * 60 + 0.5), mi = (i64)(mic.from * 60 + 0.5), mo = (i64)(mic.to * 60 + 0.5);
    snprintf(want_s, sizeof want_s, "<start>%lld</start><end>%lld</end><in>%lld</in><out>%lld</out>", ms, ms + mo - mi, mi, mo);
    T(strstr(text, want_s));
    snprintf(want_s, sizeof want_s, "<duration>%lld</duration>", ms + mo - mi);
    T(strstr(text, want_s));
    items = 0;
    for (const char *p = text; (p = strstr(p, "<clipitem ")); p++) items++;
    T(items == nv + nm + 2 * ng);
    T(strstr(text, "<effectid>basic</effectid>") && strstr(text, "<value>200.00</value>")); // the item's zoom keys
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
    test_first_apps();
    test_ffmpeg_tools_ignored();
    test_ffmpeg_check();
    test_telemetry();
    test_backup();
    test_backup_places();
    test_webcam();
    test_buffer_many();
    test_erase();
    test_r2_thumb();
    test_save_names();
    test_buffer_names();
    test_shown_names();
    test_lib_periods();
    test_folder_rules();
    test_fields();
    test_hotkeys();
    test_mic_clean();
    test_sessions_seen();
    test_toast_clip();
    test_popup_place();
    test_settings_round_trip();
    test_watch_scan();
    test_news();
    test_quality();
    test_pieces();
    test_edit_volumes();
    test_edit_xml();
    test_social_preview();
    test_edit_advanced();
    test_cam_layer();
    test_layers();
    test_r6_replay();
    test_fn_replay();
    test_plugins();
    test_lanes();
    test_share();
    test_render();
    test_chapters();
    test_wave_chunks();
    test_gamedb();
    test_mcpack();
    test_r2();
    test_share_cancel();
    test_share_ready();
    test_storeids();
    test_store_folders();
    test_markers();
    test_automarks();
    test_achievements();
    printf("%d checks, %d failed\n", g_checked, g_failed);
    return g_failed;
}
