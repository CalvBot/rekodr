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
    }
    T(b.clock_24h == a.clock_24h);
    T(!b.voice_mono);
    T(b.timeline_top == 0.4f);
    T(b.timeline_names == 220);
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
    // Finding games by part of a name
    GameInfo hits[4];
    T(gamedb_search("count", hits, 4) == 1 && !strcmp(hits[0].name, "Counter-Strike 2"));
    T(gamedb_search("r", hits, 4) == 4); // every one of the four has an r
    T(gamedb_search("ru", hits, 4) == 1 && !strcmp(hits[0].name, "Rust"));
    T(gamedb_search("", hits, 4) == 0 && gamedb_search("zzz", hits, 4) == 0);
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

int main()
{
    InitializeSRWLock(&g_log_lock);
    test_lengths();
    test_buffer_names();
    test_folder_rules();
    test_fields();
    test_hotkeys();
    test_settings_round_trip();
    test_gamedb();
    printf("%d checks, %d failed\n", g_checked, g_failed);
    return g_failed;
}
