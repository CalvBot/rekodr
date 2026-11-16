// Folders and settings. Settings live in a plain `key = value` text file (settings.txt), written whenever Options
// changes something. Unknown keys are ignored, missing keys keep their defaults.
//
// CLIPBACK_HOME (for development and tests) moves every folder clipback uses under that one folder, so a test copy
// never touches the real settings, buffer or clips.

static void paths_init(Paths *p)
{
    wchar_t home[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"CLIPBACK_HOME", home, MAX_PATH) && home[0]) {
        swprintf(p->config_dir.s, MAX_PATH, L"%s\\config", home);
        swprintf(p->local_dir.s, MAX_PATH, L"%s\\local", home);
        swprintf(p->share_dir.s, MAX_PATH, L"%s\\share", home);
    } else {
        wchar_t *roaming = 0, *local = 0;
        SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, 0, &roaming);
        SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, 0, &local);
        swprintf(p->config_dir.s, MAX_PATH, L"%s\\clipback", roaming ? roaming : L".");
        swprintf(p->local_dir.s, MAX_PATH, L"%s\\clipback", local ? local : L".");
        CoTaskMemFree(roaming);
        CoTaskMemFree(local);
        wchar_t temp[MAX_PATH];
        GetTempPathW(MAX_PATH, temp); // ends in a backslash
        swprintf(p->share_dir.s, MAX_PATH, L"%sclipback\\share", temp);
    }
    p->settings = path_join(p->config_dir, L"settings.txt");
    SHCreateDirectoryExW(0, p->config_dir.s, 0);
    SHCreateDirectoryExW(0, p->local_dir.s, 0);
    Path log = path_join(p->local_dir, L"clipback-native.log");
    wcscpy_s(g_log_path, log.s);
}

static void config_defaults(Config *c, const Paths &paths)
{
    *c = {};
    wchar_t home[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"CLIPBACK_HOME", home, MAX_PATH) && home[0]) {
        swprintf(c->save_dir.s, MAX_PATH, L"%s\\Clips", home);
        swprintf(c->buffer_dir.s, MAX_PATH, L"%s\\buffer", home);
    } else {
        wchar_t *videos = 0;
        SHGetKnownFolderPath(FOLDERID_Videos, 0, 0, &videos);
        swprintf(c->save_dir.s, MAX_PATH, L"%s\\Clips", videos ? videos : L".");
        CoTaskMemFree(videos);
        c->buffer_dir = path_join(paths.local_dir, L"buffer");
    }
    c->buffer_hours = 24;
    c->buffer_gb = 100;
    c->min_free_gb = 50;
    c->fps = 60;
    str_set(c->codec.s, "h264");
    str_set(c->encoder.s, "auto");
    c->bitrate_mbps = 20;
    c->desktop_bitrate_mbps = -1; // not in the file: the game bitrate (fixed up after loading)
    c->draw_mouse = true;
    str_set(c->preset.s, "p4");
    str_set(c->rate_control.s, "vbr");
    c->cq = 23;
    c->peak_factor = 2;
    c->mic = true;
    c->session_gap_min = 5;
    c->session_min_length_min = 15;
    c->share_keep_days = 7;
    c->accent = 0;
    c->thumb_size = 1;
    // One hotkey, the same keys as NVIDIA's instant replay, so it's where hands already go.
    str_set(c->hotkeys[0].keys.s, "Alt+F10");
    str_set(c->hotkeys[0].action.s, "save 30s");
    c->hotkey_count = 1;
}

static const char *category_name(AppCategory c)
{
    switch (c) {
    case CAT_APP: return "app";
    case CAT_GAME: return "game";
    case CAT_COMMUNICATION: return "communication";
    case CAT_IGNORED: return "ignored";
    default: return "new";
    }
}

static AppCategory category_parse(const char *s)
{
    if (!strcmp(s, "app")) return CAT_APP;
    if (!strcmp(s, "game")) return CAT_GAME;
    if (!strcmp(s, "communication")) return CAT_COMMUNICATION;
    if (!strcmp(s, "ignored")) return CAT_IGNORED;
    return CAT_NONE;
}

static const char *session_mode_name(SessionMode m)
{
    return m == SESSION_ALWAYS ? "always" : m == SESSION_NEVER ? "never" : "ask";
}

static SessionMode session_mode_parse(const char *s)
{
    return !strcmp(s, "always") ? SESSION_ALWAYS : !strcmp(s, "never") ? SESSION_NEVER : SESSION_ASK;
}

// Finds an app by exe (lower case), adding it if it's new and there's room. Returns 0 only when the table is full.
static AppInfo *config_app(Config *c, const char *exe)
{
    for (i32 i = 0; i < c->app_count; i++)
        if (!strcmp(c->apps[i].exe.s, exe)) return &c->apps[i];
    if (c->app_count == COUNT(c->apps)) return 0;
    AppInfo *a = &c->apps[c->app_count++];
    *a = {};
    str_set(a->exe.s, exe);
    return a;
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
    return s;
}

// Splits "a | b | c" in place. Returns the number of fields.
static i32 split_fields(char *s, char **fields, i32 max)
{
    i32 n = 0;
    while (n < max) {
        char *bar = strchr(s, '|');
        if (bar) *bar = 0;
        fields[n++] = trim(s);
        if (!bar) break;
        s = bar + 1;
    }
    return n;
}

// Older files had an "advanced" switch: the encoder settings only applied while it was on.
static bool g_config_advanced_seen, g_config_advanced;

static void config_set(Config *c, const char *key, char *value)
{
    wchar_t wide[MAX_PATH];
    auto path = [&](Path *p) { utf8_to_wide(value, wide, MAX_PATH); wcscpy_s(p->s, wide); };
    auto boolean = [&](bool *b) { *b = !strcmp(value, "true") || !strcmp(value, "1"); };
    auto real = [&](f32 *f) { *f = (f32)atof(value); };

    if (!strcmp(key, "save_dir")) path(&c->save_dir);
    else if (!strcmp(key, "buffer_dir")) path(&c->buffer_dir);
    else if (!strcmp(key, "buffer_hours")) real(&c->buffer_hours);
    else if (!strcmp(key, "buffer_gb")) real(&c->buffer_gb);
    else if (!strcmp(key, "buffer_by_size")) boolean(&c->buffer_by_size);
    else if (!strcmp(key, "desktop_bitrate_mbps")) real(&c->desktop_bitrate_mbps);
    else if (!strcmp(key, "accent")) c->accent = atoi(value);
    else if (!strcmp(key, "thumb_size")) c->thumb_size = atoi(value);
    else if (!strcmp(key, "min_free_gb")) real(&c->min_free_gb);
    else if (!strcmp(key, "ffmpeg")) path(&c->ffmpeg);
    else if (!strcmp(key, "output_idx")) c->output_idx = atoi(value);
    else if (!strcmp(key, "fps")) c->fps = atoi(value);
    else if (!strcmp(key, "codec")) str_set(c->codec.s, value);
    else if (!strcmp(key, "encoder")) str_set(c->encoder.s, value);
    else if (!strcmp(key, "bitrate_mbps")) real(&c->bitrate_mbps);
    else if (!strcmp(key, "draw_mouse")) boolean(&c->draw_mouse);
    else if (!strcmp(key, "advanced")) {
        g_config_advanced_seen = true;
        g_config_advanced = !strcmp(value, "true") || !strcmp(value, "1");
    }
    else if (!strcmp(key, "preset")) str_set(c->preset.s, value);
    else if (!strcmp(key, "rate_control")) str_set(c->rate_control.s, value);
    else if (!strcmp(key, "cq")) c->cq = atoi(value);
    else if (!strcmp(key, "peak_factor")) real(&c->peak_factor);
    else if (!strcmp(key, "spatial_aq")) boolean(&c->spatial_aq);
    else if (!strcmp(key, "mic")) boolean(&c->mic);
    else if (!strcmp(key, "session_gap_min")) real(&c->session_gap_min);
    else if (!strcmp(key, "session_min_length_min")) real(&c->session_min_length_min);
    else if (!strcmp(key, "share_keep_days")) real(&c->share_keep_days);
    else if (!strcmp(key, "start_with_windows")) boolean(&c->start_with_windows);
    else if (!strcmp(key, "hotkey")) {
        // hotkey = Alt+F10 | save 30s
        char *f[2];
        if (split_fields(value, f, 2) == 2 && c->hotkey_count < COUNT(c->hotkeys)) {
            Hotkey *h = &c->hotkeys[c->hotkey_count++];
            str_set(h->keys.s, f[0]);
            str_set(h->action.s, f[1]);
        }
    } else if (!strcmp(key, "app")) {
        // app = overwatch.exe | Overwatch | game | ask | bitrate | fps | rate control | quality | preset
        char *f[9] = {};
        i32 n = split_fields(value, f, 9);
        if (n >= 3) {
            AppInfo *a = config_app(c, f[0]);
            if (!a) return;
            str_set(a->name.s, f[1]);
            a->category = category_parse(f[2]);
            if (n >= 4) a->session_mode = session_mode_parse(f[3]);
            if (n >= 5) a->rec.bitrate_mbps = (f32)atof(f[4]);
            if (n >= 6) a->rec.fps = atoi(f[5]);
            if (n >= 7) str_set(a->rec.rate_control.s, f[6]);
            if (n >= 8) a->rec.cq = atoi(f[7]);
            if (n >= 9) str_set(a->rec.preset.s, f[8]);
        }
    }
}

static bool config_load(Config *c, const Paths &paths)
{
    config_defaults(c, paths);
    g_config_advanced_seen = g_config_advanced = false;
    FILE *f = _wfopen(paths.settings.s, L"rb");
    if (!f) {
        c->desktop_bitrate_mbps = c->bitrate_mbps;
        return false;
    }
    bool hotkeys_seen = false;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        char *s = trim(line);
        if (!*s || *s == '#') continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = trim(s), *value = trim(eq + 1);
        if (!strcmp(key, "hotkey") && !hotkeys_seen) {
            c->hotkey_count = 0; // the file's hotkeys replace the default one
            hotkeys_seen = true;
        }
        config_set(c, key, value);
    }
    fclose(f);
    if (c->desktop_bitrate_mbps < 0) c->desktop_bitrate_mbps = c->bitrate_mbps;
    if (g_config_advanced_seen && !g_config_advanced) { // they weren't in use: back to the defaults
        static Config d; // big (the app table)
        config_defaults(&d, paths);
        c->preset = d.preset;
        c->rate_control = d.rate_control;
        c->cq = d.cq;
        c->peak_factor = d.peak_factor;
        c->spatial_aq = d.spatial_aq;
    }
    return true;
}

// Writes settings.txt (through a temp file, so a crash mid-write never leaves half a file).
static bool config_save(const Config &c, const Paths &paths)
{
    Path tmp = paths.settings;
    wcscat_s(tmp.s, L".tmp");
    FILE *f = _wfopen(tmp.s, L"wb");
    if (!f) {
        log_line("couldn't write settings (%d)", (int)GetLastError());
        return false;
    }
    char u[MAX_PATH * 3];
    auto path = [&](const char *k, const Path &p) { wide_to_utf8(p.s, u, sizeof u); fprintf(f, "%s = %s\n", k, u); };
    auto boolean = [&](const char *k, bool b) { fprintf(f, "%s = %s\n", k, b ? "true" : "false"); };

    fprintf(f, "# clipback settings. Options writes this file; it's safe to edit while clipback is closed.\n\n");
    path("save_dir", c.save_dir);
    path("buffer_dir", c.buffer_dir);
    boolean("buffer_by_size", c.buffer_by_size);
    fprintf(f, "buffer_hours = %g\nbuffer_gb = %g\n", c.buffer_hours, c.buffer_gb);
    fprintf(f, "min_free_gb = %g\n", c.min_free_gb);
    path("ffmpeg", c.ffmpeg);
    fprintf(f, "\noutput_idx = %d\nfps = %d\ncodec = %s\nencoder = %s\nbitrate_mbps = %g\ndesktop_bitrate_mbps = %g\n",
            c.output_idx, c.fps, c.codec.s, c.encoder.s, c.bitrate_mbps, c.desktop_bitrate_mbps);
    boolean("draw_mouse", c.draw_mouse);
    fprintf(f, "preset = %s\nrate_control = %s\ncq = %d\npeak_factor = %g\n", c.preset.s, c.rate_control.s, c.cq,
            c.peak_factor);
    boolean("spatial_aq", c.spatial_aq);
    fprintf(f, "\n");
    boolean("mic", c.mic);
    fprintf(f, "\nsession_gap_min = %g\nsession_min_length_min = %g\nshare_keep_days = %g\n", c.session_gap_min,
            c.session_min_length_min, c.share_keep_days);
    boolean("start_with_windows", c.start_with_windows);
    fprintf(f, "accent = %d\nthumb_size = %d\n", c.accent, c.thumb_size);
    fprintf(f, "\n# hotkey = keys | action\n");
    for (i32 i = 0; i < c.hotkey_count; i++) fprintf(f, "hotkey = %s | %s\n", c.hotkeys[i].keys.s, c.hotkeys[i].action.s);
    fprintf(f, "\n# app = exe | name | category | session mode | bitrate (Mbps) | fps | rate control | quality | preset\n"
               "# (a game's own recording settings; 0 or empty = the default)\n");
    for (i32 i = 0; i < c.app_count; i++) {
        const AppInfo &a = c.apps[i];
        fprintf(f, "app = %s | %s | %s | %s | %g | %d | %s | %d | %s\n", a.exe.s, a.name.s, category_name(a.category),
                session_mode_name(a.session_mode), a.rec.bitrate_mbps, a.rec.fps, a.rec.rate_control.s, a.rec.cq,
                a.rec.preset.s);
    }
    bool ok = fclose(f) == 0;
    ok = ok && MoveFileExW(tmp.s, paths.settings.s, MOVEFILE_REPLACE_EXISTING);
    if (!ok) log_line("couldn't save settings (%d)", (int)GetLastError());
    return ok;
}
