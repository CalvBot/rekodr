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
    T(b.folder_count == 1 && !strcmp(b.folders[0].path, "C:\\V\\*\\off") && b.folders[0].category == CAT_IGNORED);
    T(!wcscmp(a.save_dir.s, b.save_dir.s) && !wcscmp(a.buffer_dir.s, b.buffer_dir.s));

    DeleteFileW(paths.settings.s);
    RemoveDirectoryW(paths.config_dir.s);
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
    printf("%d checks, %d failed\n", g_checked, g_failed);
    return g_failed;
}
