// Folders and settings. Settings live in a plain `key = value` text file (settings.txt), written whenever Options
// changes something. Unknown keys are ignored, missing keys keep their defaults.
//
// REKODR_HOME (for development and tests) moves every folder Rekodr uses under that one folder, so a test copy
// never touches the real settings, buffer or clips.

// A test copy (REKODR_HOME) leaves the hotkeys and Windows' Run list to the real Rekodr.
static bool is_test_copy() { return test_home_hash() != 0; }

static void paths_init(Paths *p)
{
    wchar_t home[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"REKODR_HOME", home, MAX_PATH) && home[0]) {
        swprintf(p->config_dir.s, MAX_PATH, L"%s\\config", home);
        swprintf(p->local_dir.s, MAX_PATH, L"%s\\local", home);
        swprintf(p->share_dir.s, MAX_PATH, L"%s\\share", home);
        swprintf(p->link_dir.s, MAX_PATH, L"%s\\link", home);
    } else {
        wchar_t *roaming = 0, *local = 0;
        SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, 0, &roaming);
        SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, 0, &local);
        swprintf(p->config_dir.s, MAX_PATH, L"%s\\" APP_NAME, roaming ? roaming : L".");
        swprintf(p->local_dir.s, MAX_PATH, L"%s\\" APP_NAME, local ? local : L".");
        CoTaskMemFree(roaming);
        CoTaskMemFree(local);
        wchar_t temp[MAX_PATH];
        GetTempPathW(MAX_PATH, temp); // ends in a backslash
        swprintf(p->share_dir.s, MAX_PATH, L"%s" APP_NAME L"\\share", temp);
        swprintf(p->link_dir.s, MAX_PATH, L"%s" APP_NAME L"\\link", temp);
    }
    p->settings = path_join(p->config_dir, L"settings.txt");
    SHCreateDirectoryExW(0, p->config_dir.s, 0);
    SHCreateDirectoryExW(0, p->local_dir.s, 0);
    Path log = path_join(p->local_dir, L"rekodr-native.log");
    // Past 10 MB the log starts again; the previous one is kept as rekodr-native.old.log.
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetFileAttributesExW(log.s, GetFileExInfoStandard, &fa) && (fa.nFileSizeHigh || fa.nFileSizeLow > 10u << 20))
        MoveFileExW(log.s, path_join(p->local_dir, L"rekodr-native.old.log").s, MOVEFILE_REPLACE_EXISTING);
    wcscpy_s(g_log_path, log.s);
}

// The card styles, in r2.cpp.
static i32 embed_style_find(const char *id);
static const char *embed_style_id(i32 style);

static void config_defaults(Config *c, const Paths &paths)
{
    *c = {};
    wchar_t home[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"REKODR_HOME", home, MAX_PATH) && home[0]) {
        swprintf(c->save_dir.s, MAX_PATH, L"%s\\" APP_NAME, home);
        swprintf(c->buffer_dir.s, MAX_PATH, L"%s\\buffer", home);
    } else {
        wchar_t *videos = 0;
        SHGetKnownFolderPath(FOLDERID_Videos, 0, 0, &videos);
        swprintf(c->save_dir.s, MAX_PATH, L"%s\\" APP_NAME, videos ? videos : L".");
        CoTaskMemFree(videos);
        c->buffer_dir = path_join(paths.local_dir, L"buffer");
    }
    c->play_dir = paths.local_dir;
    c->buffer_hours = 24;
    c->popup_monitor = POPUP_RECORDED;
    c->popup_monitor_game = POPUP_AS_NORMAL;
    c->toast_clip = TOAST_CLIP_FULL;
    for (i32 i = 0; i < SOUND_EVENT_COUNT; i++) c->sounds[i] = {true, (u8)i};
    c->buffer_gb = 100;
    c->min_free_gb = 50;
    c->fps = 60;
    str_set(c->name_template, SAVE_NAME_DEFAULT);
    str_set(c->codec.s, "h264");
    str_set(c->encoder.s, "auto");
    c->bitrate_mbps = 20;
    c->desktop_bitrate_mbps = -1; // not in the file: the game bitrate (fixed up after loading)
    c->draw_mouse = true;
    str_set(c->capture.s, "auto");
    str_set(c->preset.s, "p4");
    str_set(c->rate_control.s, "vbr");
    c->cq = 23;
    c->peak_factor = 2;
    c->mic = true;
    c->session_gap_min = 5;
    c->session_min_length_min = 15;
    c->share_keep_days = 7;
    c->folder_warn_gb = 500;
    c->share_max_mb = 20;
    c->social_copy_mb = 150;
    c->share_min_quality = 67;
    c->render_edits = true;
    memcpy(c->share_kbps, SHARE_KBPS_DEFAULT, sizeof c->share_kbps);
    c->share_link_page = true;
    c->share_link_days = 0;
    c->share_copy_on_close = false;
    str_set(c->r2_bucket, "rekodr");
    c->r2_read_cap = 10000000;  // R2's free Class B requests a month
    c->r2_space_gb = -1;
    c->r2_read_rate = 1000000;
    c->wave_keep_days = 30;
    c->game_lookup = true;
    c->auto_update = true;
    c->tooltips = true;
    c->share_names = true; // "Help name games" and "Send usage stats": on, the setup asks
    c->send_stats = true;
    str_set(c->art_service.s, ART_SERVICE_HOST);
    c->voice_mono = true;
    wchar_t time_format[80] = L"";
    c->clock_24h = !GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_STIMEFORMAT, time_format, 80) ||
                   wcschr(time_format, L'H') != 0; // "HH:mm:ss" is 24-hour, "h:mm:ss tt" 12-hour
    c->accent = ACCENT_DEFAULT;
    c->card_width = 270;
    // One hotkey, on keys no other clip tool (NVIDIA, AMD, Medal, Steam, Game Bar) or common game binding uses.
    str_set(c->hotkeys[0].keys.s, "Alt+F6");
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

static const char *corner_name(PopupCorner c)
{
    return c == CORNER_BL ? "bl" : c == CORNER_TR ? "tr" : c == CORNER_TL ? "tl" : "br";
}

static PopupCorner corner_parse(const char *s)
{
    return !strcmp(s, "bl") ? CORNER_BL : !strcmp(s, "tr") ? CORNER_TR : !strcmp(s, "tl") ? CORNER_TL : CORNER_BR;
}

// "" is a game's "same as all games".
static const char *toast_clip_name(ToastClip t)
{
    return t == TOAST_CLIP_FULL ? "full" : t == TOAST_CLIP_SMALL ? "small" : t == TOAST_CLIP_OFF ? "off" : "";
}

static ToastClip toast_clip_parse(const char *s)
{
    return !strcmp(s, "full")    ? TOAST_CLIP_FULL
           : !strcmp(s, "small") ? TOAST_CLIP_SMALL
           : !strcmp(s, "off")   ? TOAST_CLIP_OFF
                                 : TOAST_CLIP_DEFAULT;
}

// A sound's name in settings.txt: its library key, or "own".
static const char *sound_key(u8 sound) { return sound < SOUND_COUNT ? SOUND_LIBRARY[sound].key : "own"; }

// "sound_<event> = on|off <sound>" and "sound_<event>_file = <path>". False when `key` isn't one of them.
static bool sound_setting(Config *c, const char *key, const char *value)
{
    if (strncmp(key, "sound_", 6)) return false;
    key += 6;
    for (i32 e = 0; e < SOUND_EVENT_COUNT; e++) {
        size_t n = strlen(SOUND_EVENTS[e].key);
        if (strncmp(key, SOUND_EVENTS[e].key, n)) continue;
        SoundPick &p = c->sounds[e];
        if (!strcmp(key + n, "_file")) {
            utf8_to_wide(value, p.own.s, MAX_PATH);
            return true;
        }
        if (key[n]) continue;
        p.on = strncmp(value, "off", 3) != 0;
        const char *name = strchr(value, ' ');
        name = name ? name + 1 : "";
        if (!strcmp(name, "own")) p.sound = SOUND_OWN;
        for (i32 i = 0; i < SOUND_COUNT; i++)
            if (!strcmp(name, SOUND_LIBRARY[i].key)) p.sound = (u8)i;
        return true;
    }
    return false;
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

// How Clip saved shows over a game (`game_exe`, lower case): its own choice, else the one for all games.
static ToastClip toast_clip_for(const Config &c, const char *game_exe)
{
    for (i32 i = 0; game_exe && game_exe[0] && i < c.app_count; i++)
        if (!strcmp(c.apps[i].exe.s, game_exe) && c.apps[i].toast_clip != TOAST_CLIP_DEFAULT)
            return c.apps[i].toast_clip;
    return c.toast_clip == TOAST_CLIP_DEFAULT ? TOAST_CLIP_FULL : c.toast_clip;
}

// The buffer for a drive with free_gb free: a quarter of it, in steps of 10 GB, between 10 and 200 GB. The space
// kept free is 50 GB, or a fifth of the free space on a small drive.
static void buffer_for_free(f64 free_gb, f32 *buffer_gb, f32 *min_free_gb)
{
    f64 gb = floor(free_gb / 4 / 10) * 10;
    *buffer_gb = (f32)(gb < 10 ? 10 : gb > 200 ? 200 : gb);
    *min_free_gb = (f32)fmin(50, floor(free_gb / 5));
}

// The first settings, when there's no settings.txt yet: the defaults, the buffer sized to the free space on its
// drive, and the usual chat apps sorted as Chat. The caller saves them.
static void config_first_run(Config *c, const Paths &paths)
{
    static const char *const chat[] = {
        "discord.exe", "discordptb.exe", "discordcanary.exe", "ms-teams.exe", "teams.exe", "zoom.exe", "slack.exe",
        "ts3client_win64.exe", "teamspeak.exe", "skype.exe", "telegram.exe", "whatsapp.exe", "signal.exe",
        "mumble.exe", "guilded.exe", "element.exe", "vesktop.exe",
    };
    config_defaults(c, paths);
    wchar_t root[4] = {c->buffer_dir.s[0], L':', L'\\', 0};
    ULARGE_INTEGER free_bytes;
    if (c->buffer_dir.s[1] == L':' && GetDiskFreeSpaceExW(root, &free_bytes, 0, 0)) {
        f64 free_gb = (f64)free_bytes.QuadPart / 1e9;
        c->buffer_by_size = true;
        buffer_for_free(free_gb, &c->buffer_gb, &c->min_free_gb);
        log_line("first start: %.0f GB free on %ls, buffer %.0f GB, keeping %.0f GB free", free_gb, root, c->buffer_gb,
                 c->min_free_gb);
    }
    for (i32 i = 0; i < COUNT(chat); i++) {
        AppInfo *a = config_app(c, chat[i]);
        if (a) a->category = CAT_COMMUNICATION;
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Folder rules. An exe's own category wins; with none (new), the deepest folder rule over its path
// decides. Paths compare without case, / and \ alike, and a trailing slash doesn't matter.

// The next part of a path from *p ("C:", "Users", ...), skipping separators. Returns its length; 0 at the end.
static i32 path_part(const char **p, const char **part)
{
    while (**p == '\\' || **p == '/') (*p)++;
    *part = *p;
    while (**p && **p != '\\' && **p != '/') (*p)++;
    return (i32)(*p - *part);
}

// How many parts deep `folder` is when `path` (an exe) is inside it or below it; 0 when it isn't. A `*` part of the
// folder matches any one folder name.
static i32 path_under(const char *path, const char *folder)
{
    const char *p = path, *f = folder, *pp, *fp;
    i32 depth = 0;
    for (;;) {
        i32 fl = path_part(&f, &fp);
        if (!fl) break;
        i32 pl = path_part(&p, &pp);
        if (!pl) return 0;
        bool any = fl == 1 && fp[0] == '*';
        if (!any && (fl != pl || _strnicmp(fp, pp, fl))) return 0;
        depth++;
    }
    const char *rest;
    return depth && path_part(&p, &rest) ? depth : 0; // something (the exe) must be left below it
}

// The folder rule for `path`: the deepest that holds it, ignored winning a tie. -1 = none.
static i32 folder_rule_for(const FolderRule *rules, i32 n, const char *path)
{
    i32 best = -1, best_depth = 0;
    if (!path || !path[0]) return -1;
    for (i32 i = 0; i < n; i++) {
        i32 d = path_under(path, rules[i].path);
        if (!d) continue;
        if (d > best_depth || (d == best_depth && rules[i].category == CAT_IGNORED)) {
            best = i;
            best_depth = d;
        }
    }
    return best;
}

// A configured input by endpoint id, ignoring case (Windows gives ids in lower case, Device Manager in upper).
static const InputDevice *config_input(const InputDevice *inputs, i32 count, const char *id)
{
    for (i32 i = 0; i < count; i++)
        if (!_stricmp(inputs[i].id, id)) return &inputs[i];
    return 0;
}

// A track title for an input: the device inside Windows' name, "RODE AI-1" for "Line (RODE AI-1)".
static void input_short_name(const char *name, char *out, i32 cap)
{
    const char *open = strchr(name, '(');
    const char *close = open ? strrchr(name, ')') : 0;
    if (open && close && close > open + 1) snprintf(out, cap, "%.*s", (int)(close - open - 1), open + 1);
    else snprintf(out, cap, "%s", name);
}

// One person in a Discord call, as vencord-voicestreams plays them: "discord-sam-1234...exe". Chat unless set
// otherwise, so each person gets a track of their own without sorting every new one by hand. (Discord's own exes
// have no dash: discordptb.exe, discordcanary.exe.)
static AppCategory voice_stream_category(const char *exe)
{
    return !strncmp(exe, "discord-", 8) ? CAT_COMMUNICATION : CAT_NONE;
}

// An app's category: its own, else its folder's (by `path`, or where it was last heard when that's empty), else new.
static AppCategory app_category_in(const AppInfo *apps, i32 app_count, const FolderRule *rules, i32 rule_count,
                                   const char *exe, const char *path)
{
    const AppInfo *a = 0;
    for (i32 i = 0; i < app_count && !a; i++)
        if (!strcmp(apps[i].exe.s, exe)) a = &apps[i];
    if (a && a->category != CAT_NONE) return a->category;
    if ((!path || !path[0]) && a) path = a->path;
    i32 r = folder_rule_for(rules, rule_count, path);
    return r >= 0 ? rules[r].category : voice_stream_category(exe);
}

// A known app's category: its own, else its folder's (by where it was last heard from).
static AppCategory app_category(const Config &c, const AppInfo &a)
{
    if (a.category != CAT_NONE) return a.category;
    i32 r = folder_rule_for(c.folders, c.folder_count, a.path);
    return r >= 0 ? c.folders[r].category : voice_stream_category(a.exe.s);
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

// The accent is a hue alone. Its lightness and chroma are the default teal's in OKLCH (lightness and colourfulness
// as the eye sees them), so every hue stands out from the dark UI as much as teal does; a hue the screen can't show
// at that chroma gives up chroma until it can. Hues go in ACCENT_STEP degree steps from teal's, so a colour snapped
// to one snaps to the same one again and the default stays exactly ACCENT_DEFAULT.
enum { ACCENT_STEP = 2, ACCENT_HUES = 360 / ACCENT_STEP };

static f64 srgb_linear(f64 v) { return v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4); }
static f64 linear_srgb(f64 v) { return v <= 0.0031308 ? v * 12.92 : 1.055 * pow(v, 1 / 2.4) - 0.055; }

// 0xRRGGBB as OKLab
static void rgb_oklab(u32 c, f64 out[3])
{
    f64 r = srgb_linear(((c >> 16) & 255) / 255.0), g = srgb_linear(((c >> 8) & 255) / 255.0), b = srgb_linear((c & 255) / 255.0);
    f64 l = cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b);
    f64 m = cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b);
    f64 s = cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b);
    out[0] = 0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s;
    out[1] = 1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s;
    out[2] = 0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s;
}

// OKLab as 0xRRGGBB; false when the screen can't show it
static bool oklab_rgb(f64 L, f64 a, f64 b, u32 *out)
{
    f64 l = L + 0.3963377774 * a + 0.2158037573 * b, m = L - 0.1055613458 * a - 0.0638541728 * b,
        s = L - 0.0894841775 * a - 1.2914855480 * b;
    l = l * l * l, m = m * m * m, s = s * s * s;
    f64 v[3] = {4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s,
                -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s,
                -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s};
    u32 c = 0;
    for (i32 i = 0; i < 3; i++) {
        if (v[i] < -1e-4 || v[i] > 1 + 1e-4) return false;
        c = c << 8 | (u32)(linear_srgb(v[i] < 0 ? 0 : v[i] > 1 ? 1 : v[i]) * 255 + 0.5);
    }
    *out = c;
    return true;
}

// A colour's OKLCH hue in degrees, 0 to 360
static f64 rgb_hue(u32 c)
{
    f64 lab[3];
    rgb_oklab(c, lab);
    f64 h = atan2(lab[2], lab[1]) * 180 / 3.14159265358979;
    return h < 0 ? h + 360 : h;
}

// The accent of hue step k (0 = teal, ACCENT_HUES of them round the circle)
static u32 accent_at_step(i32 k)
{
    f64 base[3];
    rgb_oklab(ACCENT_DEFAULT, base);
    f64 chroma = sqrt(base[1] * base[1] + base[2] * base[2]);
    f64 h = (rgb_hue(ACCENT_DEFAULT) + (((k % ACCENT_HUES) + ACCENT_HUES) % ACCENT_HUES) * ACCENT_STEP) * 3.14159265358979 / 180;
    u32 out = ACCENT_DEFAULT;
    for (f64 ch = chroma; ch > 0.001; ch *= 0.98)
        if (oklab_rgb(base[0], ch * cos(h), ch * sin(h), &out)) break;
    return out;
}

// The hue step nearest colour c's
static i32 accent_step(u32 c)
{
    f64 d = rgb_hue(c & 0xffffff) - rgb_hue(ACCENT_DEFAULT);
    if (d < 0) d += 360;
    return (i32)floor(d / (f64)ACCENT_STEP + 0.5) % ACCENT_HUES;
}

// Colour c as an accent: the nearest hue, at teal's lightness and chroma
static u32 accent_lock(u32 c) { return accent_at_step(accent_step(c)); }

static void config_set(Config *c, const char *key, char *value)
{
    wchar_t wide[MAX_PATH];
    auto path = [&](Path *p) { utf8_to_wide(value, wide, MAX_PATH); wcscpy_s(p->s, wide); };
    auto boolean = [&](bool *b) { *b = !strcmp(value, "true") || !strcmp(value, "1"); };
    auto real = [&](f32 *f) { *f = (f32)atof(value); };

    if (!strcmp(key, "save_dir")) path(&c->save_dir);
    else if (!strcmp(key, "buffer_dir")) path(&c->buffer_dir);
    else if (!strcmp(key, "play_dir")) path(&c->play_dir);
    else if (!strcmp(key, "backup_on")) boolean(&c->backup_on);
    else if (!strcmp(key, "backup_dir")) path(&c->backup_dir);
    else if (!strcmp(key, "backup_clips")) boolean(&c->backup_clips);
    else if (!strcmp(key, "name_template")) str_set(c->name_template, value);
    else if (!strcmp(key, "buffer_hours")) real(&c->buffer_hours);
    else if (!strcmp(key, "buffer_gb")) real(&c->buffer_gb);
    else if (!strcmp(key, "buffer_by_size")) boolean(&c->buffer_by_size);
    else if (!strcmp(key, "desktop_bitrate_mbps")) real(&c->desktop_bitrate_mbps);
    else if (!strcmp(key, "accent")) { // six hex digits, "00897b", taken as its hue (accent_lock); anything else
                                         // keeps the default
        bool hex = strlen(value) == 6;
        for (i32 i = 0; hex && i < 6; i++) hex = isxdigit((u8)value[i]) != 0;
        if (hex) c->accent = accent_lock((u32)strtoul(value, 0, 16));
    }
    else if (!strcmp(key, "card_width")) c->card_width = atoi(value);
    else if (!strcmp(key, "games_collapsed")) c->games_collapsed = atoi(value) != 0;
    else if (!strcmp(key, "unsaved_collapsed")) c->unsaved_collapsed = atoi(value) != 0;
    else if (!strcmp(key, "side_folders")) c->side_folders = atoi(value) != 0;
    else if (!strcmp(key, "timeline_top")) real(&c->timeline_top);
    else if (!strcmp(key, "timeline_names")) c->timeline_names = atoi(value);
    else if (!strcmp(key, "timeline_lane")) c->timeline_lane = atoi(value);
    else if (!strcmp(key, "timeline_simple")) c->timeline_simple = atoi(value) != 0;
    else if (!strcmp(key, "editor_top")) real(&c->editor_top);
    else if (!strcmp(key, "editor_side")) real(&c->editor_side);
    else if (!strcmp(key, "editor_lane")) c->editor_lane = atoi(value);
    else if (!strcmp(key, "editor_zoom_view")) c->editor_zoom_view = atoi(value) == 1;
    else if (!strcmp(key, "min_free_gb")) real(&c->min_free_gb);
    else if (!strcmp(key, "ffmpeg")) path(&c->ffmpeg);
    else if (!strcmp(key, "output_idx")) c->output_idx = atoi(value);
    else if (!strcmp(key, "popup_monitor")) c->popup_monitor = atoi(value);
    else if (!strcmp(key, "popup_corner")) c->popup_corner = corner_parse(value);
    else if (!strcmp(key, "popup_monitor_game")) c->popup_monitor_game = atoi(value);
    else if (!strcmp(key, "popup_corner_game")) c->popup_corner_game = corner_parse(value);
    else if (!strcmp(key, "toast_clip")) {
        c->toast_clip = toast_clip_parse(value);
        if (c->toast_clip == TOAST_CLIP_DEFAULT) c->toast_clip = TOAST_CLIP_FULL;
    }
    else if (sound_setting(c, key, value)) {}
    else if (!strcmp(key, "fps")) c->fps = atoi(value);
    else if (!strcmp(key, "height")) c->height = atoi(value);
    else if (!strcmp(key, "codec")) str_set(c->codec.s, value);
    else if (!strcmp(key, "encoder")) str_set(c->encoder.s, value);
    else if (!strcmp(key, "bitrate_mbps")) real(&c->bitrate_mbps);
    else if (!strcmp(key, "draw_mouse")) boolean(&c->draw_mouse);
    else if (!strcmp(key, "capture")) str_set(c->capture.s, value);
    else if (!strcmp(key, "window_capture")) boolean(&c->window_capture);
    else if (!strcmp(key, "encoder_args")) str_set(c->encoder_args, value);
    else if (!strcmp(key, "premiere")) path(&c->premiere);
    else if (!strcmp(key, "resolve")) path(&c->resolve);
    else if (!strcmp(key, "preset")) str_set(c->preset.s, value);
    else if (!strcmp(key, "rate_control")) str_set(c->rate_control.s, value);
    else if (!strcmp(key, "cq")) c->cq = atoi(value);
    else if (!strcmp(key, "peak_factor")) real(&c->peak_factor);
    else if (!strcmp(key, "spatial_aq")) boolean(&c->spatial_aq);
    else if (!strcmp(key, "mic")) boolean(&c->mic);
    else if (!strcmp(key, "session_gap_min")) real(&c->session_gap_min);
    else if (!strcmp(key, "session_min_length_min")) real(&c->session_min_length_min);
    else if (!strcmp(key, "share_keep_days")) real(&c->share_keep_days);
    else if (!strcmp(key, "folder_warn_gb")) real(&c->folder_warn_gb);
    else if (!strcmp(key, "wave_keep_days")) real(&c->wave_keep_days);
    else if (!strcmp(key, "game_lookup")) c->game_lookup = atoi(value) != 0;
    else if (!strcmp(key, "share_names")) boolean(&c->share_names);
    else if (!strcmp(key, "send_stats")) boolean(&c->send_stats);
    else if (!strcmp(key, "auto_update")) boolean(&c->auto_update);
    else if (!strcmp(key, "tooltips")) boolean(&c->tooltips);
    else if (!strcmp(key, "flat_look")) boolean(&c->flat_look);
    else if (!strcmp(key, "setup_done")) boolean(&c->setup_done);
    else if (!strcmp(key, "sgdb_key")) str_set(c->sgdb_key, value);
    else if (!strcmp(key, "art_service")) str_set(c->art_service.s, value);
    else if (!strcmp(key, "r2_account")) str_set(c->r2_account, value);
    else if (!strcmp(key, "r2_access_key")) str_set(c->r2_access_key, value);
    else if (!strcmp(key, "r2_bucket")) {
        if (value[0]) str_set(c->r2_bucket, value); // empty keeps the default
    }
    else if (!strcmp(key, "r2_public_url")) str_set(c->r2_public_url, value);
    else if (!strcmp(key, "r2_budget")) real(&c->r2_budget);
    else if (!strcmp(key, "r2_space_gb")) real(&c->r2_space_gb);
    else if (!strcmp(key, "r2_read_cap")) c->r2_read_cap = atoi(value);
    else if (!strcmp(key, "r2_read_rate")) c->r2_read_rate = atoi(value);
    else if (!strcmp(key, "link_uploaders")) str_set(c->link_uploaders, value);
    else if (!strcmp(key, "share_max_mb")) real(&c->share_max_mb);
    else if (!strcmp(key, "social_copy_mb")) real(&c->social_copy_mb);
    else if (!strcmp(key, "share_min_quality")) c->share_min_quality = atoi(value);
    else if (!strcmp(key, "render_edits")) boolean(&c->render_edits);
    else if (!strcmp(key, "share_kbps_custom")) boolean(&c->share_kbps_custom);
    else if (!strncmp(key, "share_kbps_", 11)) { // share_kbps_1080_60 = 12000
        i32 height = 0, fps = 0;
        if (sscanf(key + 11, "%d_%d", &height, &fps) == 2)
            for (i32 i = 0; i < 6; i++)
                if (SHARE_KBPS_HEIGHTS[i] == height && (fps == 30 || fps == 60))
                    c->share_kbps[i][fps == 60] = atoi(value);
    }
    else if (!strcmp(key, "share_link_page")) boolean(&c->share_link_page);
    else if (!strcmp(key, "share_copy_on_close")) boolean(&c->share_copy_on_close);
    else if (!strcmp(key, "share_link_days")) { i32 d = atoi(value); c->share_link_days = d == 1 || d == 7 ? d : 0; }
    else if (!strcmp(key, "embed_style")) { i32 i = embed_style_find(value); c->embed_style = (u8)(i < 0 ? 0 : i); }
    else if (!strcmp(key, "clock_24h")) boolean(&c->clock_24h);
    else if (!strcmp(key, "voice_mono")) boolean(&c->voice_mono);
    else if (!strcmp(key, "dev_log")) boolean(&c->dev_log);
    else if (!strcmp(key, "hotkey")) {
        // hotkey = Alt+F7 | save 30s | save 3m (held)
        char *f[3] = {};
        if (split_fields(value, f, 3) >= 2 && c->hotkey_count < COUNT(c->hotkeys)) {
            Hotkey *h = &c->hotkeys[c->hotkey_count++];
            *h = {};
            str_set(h->keys.s, f[0]);
            str_set(h->action.s, f[1]);
            if (f[2]) str_set(h->hold.s, f[2]);
        }
    } else if (!strcmp(key, "app")) {
        // app = overwatch.exe | Overwatch | game | ask | bitrate | fps | rate control | quality | preset | height |
        //       path | window | clip saved
        char *f[13] = {};
        i32 n = split_fields(value, f, 13);
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
            if (n >= 10) a->rec.height = atoi(f[9]);
            if (n >= 11) str_set(a->path, f[10]);
            if (n >= 12) a->rec.window = atoi(f[11]);
            if (n >= 13) a->toast_clip = toast_clip_parse(f[12]);
        }
    } else if (!strcmp(key, "input")) {
        // input = mono | Line (RODE AI-1) | {0.0.1.00000000}.{...}
        char *f[3] = {};
        if (split_fields(value, f, 3) == 3 && f[2][0] && c->input_count < COUNT(c->inputs)) {
            InputDevice *d = &c->inputs[c->input_count++];
            d->mode = !strcmp(f[0], "stereo") ? INPUT_STEREO : !strcmp(f[0], "off") ? INPUT_OFF : INPUT_MONO;
            str_set(d->name, f[1]);
            str_set(d->id, f[2]);
        }
    } else if (!strcmp(key, "folder")) {
        // folder = ignored | C:\Users\me\AppData\Local\VoiceStreams\*\clips-off
        char *f[2] = {};
        if (split_fields(value, f, 2) == 2 && f[1][0] && c->folder_count < COUNT(c->folders)) {
            FolderRule *r = &c->folders[c->folder_count++];
            r->category = category_parse(f[0]);
            str_set(r->path, f[1]);
        }
    }
}

#pragma comment(lib, "version.lib")

// An exe's File description (what Task Manager and the volume mixer show), "" if it has none.
static void exe_description(const char *path, char *out, i32 cap)
{
    out[0] = 0;
    if (!path || !path[0]) return;
    wchar_t w[MAX_PATH];
    utf8_to_wide(path, w, MAX_PATH);
    DWORD size = GetFileVersionInfoSizeW(w, 0);
    if (!size || size > 1 << 20) return;
    void *info = malloc(size);
    if (!info) return;
    struct Lang { WORD lang, code; } *langs = 0;
    UINT n = 0;
    if (GetFileVersionInfoW(w, 0, size, info) && VerQueryValueW(info, L"\\VarFileInfo\\Translation", (void **)&langs, &n) &&
        n >= sizeof *langs) {
        wchar_t key[64];
        swprintf(key, COUNT(key), L"\\StringFileInfo\\%04x%04x\\FileDescription", langs[0].lang, langs[0].code);
        wchar_t *v = 0;
        UINT len = 0;
        if (VerQueryValueW(info, key, (void **)&v, &len) && v && len > 1) {
            wide_to_utf8(v, out, cap);
            // trim
            char *e = out + strlen(out);
            while (e > out && (e[-1] == ' ' || e[-1] == '|')) *--e = 0;
        }
    }
    free(info);
}

static bool config_load(Config *c, const Paths &paths)
{
    config_defaults(c, paths);
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
        // The file's hotkeys replace the default one; "hotkeys = none" says the player removed them all
        bool none = !strcmp(key, "hotkeys") && !strcmp(value, "none");
        if ((none || !strcmp(key, "hotkey")) && !hotkeys_seen) {
            c->hotkey_count = 0;
            hotkeys_seen = true;
        }
        if (none) continue;
        config_set(c, key, value);
    }
    fclose(f);
    if (c->desktop_bitrate_mbps < 0) c->desktop_bitrate_mbps = c->bitrate_mbps;
    for (i32 i = 0; i < c->app_count; i++) exe_description(c->apps[i].path, c->apps[i].desc.s, sizeof c->apps[i].desc.s);
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

    fprintf(f, "# " APP_NAME " settings. Options writes this file; it's safe to edit while " APP_NAME " is closed.\n\n");
    path("save_dir", c.save_dir);
    path("buffer_dir", c.buffer_dir);
    path("play_dir", c.play_dir);
    boolean("backup_on", c.backup_on);
    path("backup_dir", c.backup_dir);
    boolean("backup_clips", c.backup_clips);
    fprintf(f, "name_template = %s\n", c.name_template);
    boolean("buffer_by_size", c.buffer_by_size);
    fprintf(f, "buffer_hours = %g\nbuffer_gb = %g\n", c.buffer_hours, c.buffer_gb);
    fprintf(f, "min_free_gb = %g\n", c.min_free_gb);
    path("ffmpeg", c.ffmpeg);
    fprintf(f, "\noutput_idx = %d\nfps = %d\nheight = %d\ncodec = %s\nencoder = %s\nbitrate_mbps = %g\n"
               "desktop_bitrate_mbps = %g\n",
            c.output_idx, c.fps, c.height, c.codec.s, c.encoder.s, c.bitrate_mbps, c.desktop_bitrate_mbps);
    boolean("draw_mouse", c.draw_mouse);
    fprintf(f, "capture = %s\n", c.capture.s);
    boolean("window_capture", c.window_capture);
    fprintf(f, "preset = %s\nrate_control = %s\ncq = %d\npeak_factor = %g\n", c.preset.s, c.rate_control.s, c.cq,
            c.peak_factor);
    boolean("spatial_aq", c.spatial_aq);
    fprintf(f, "encoder_args = %s\n", c.encoder_args);
    path("premiere", c.premiere);
    path("resolve", c.resolve);
    fprintf(f, "\n");
    boolean("mic", c.mic);
    fprintf(f, "\nsession_gap_min = %g\nsession_min_length_min = %g\nshare_keep_days = %g\n", c.session_gap_min,
            c.session_min_length_min, c.share_keep_days);
    fprintf(f, "wave_keep_days = %g\n", c.wave_keep_days);
    fprintf(f, "folder_warn_gb = %g\n", c.folder_warn_gb);
    fprintf(f, "game_lookup = %d\n", c.game_lookup ? 1 : 0);
    boolean("share_names", c.share_names);
    boolean("send_stats", c.send_stats);
    boolean("auto_update", c.auto_update);
    boolean("tooltips", c.tooltips);
    boolean("flat_look", c.flat_look);
    boolean("setup_done", c.setup_done);
    if (c.sgdb_key[0]) fprintf(f, "sgdb_key = %s\n", c.sgdb_key);
    if (strcmp(c.art_service.s, ART_SERVICE_HOST)) fprintf(f, "art_service = %s\n", c.art_service.s); // once changed
    fprintf(f, "r2_account = %s\nr2_access_key = %s\nr2_bucket = %s\nr2_public_url = %s\nr2_budget = %g\n",
            c.r2_account, c.r2_access_key, c.r2_bucket, c.r2_public_url, c.r2_budget);
    if (c.r2_space_gb >= 0) fprintf(f, "r2_space_gb = %g\n", c.r2_space_gb);
    fprintf(f, "r2_read_cap = %d\nr2_read_rate = %d\n", c.r2_read_cap, c.r2_read_rate);
    fprintf(f, "link_uploaders = %s\n", c.link_uploaders);
    fprintf(f, "share_max_mb = %g\nshare_min_quality = %d\n", c.share_max_mb, c.share_min_quality);
    fprintf(f, "social_copy_mb = %g\n", c.social_copy_mb);
    for (i32 i = 0; i < 6; i++)
        fprintf(f, "share_kbps_%d_30 = %d\nshare_kbps_%d_60 = %d\n", SHARE_KBPS_HEIGHTS[i], c.share_kbps[i][0],
                SHARE_KBPS_HEIGHTS[i], c.share_kbps[i][1]);
    boolean("share_kbps_custom", c.share_kbps_custom);
    boolean("share_link_page", c.share_link_page);
    boolean("share_copy_on_close", c.share_copy_on_close);
    fprintf(f, "share_link_days = %d\n", c.share_link_days);
    fprintf(f, "embed_style = %s\n", embed_style_id(c.embed_style));
    boolean("render_edits", c.render_edits);
    boolean("clock_24h", c.clock_24h);
    fprintf(f, "popup_monitor = %d\npopup_corner = %s\n", c.popup_monitor, corner_name(c.popup_corner));
    fprintf(f, "popup_monitor_game = %d\npopup_corner_game = %s\n", c.popup_monitor_game,
            corner_name(c.popup_corner_game));
    fprintf(f, "toast_clip = %s\n", toast_clip_name(c.toast_clip));
    for (i32 e = 0; e < SOUND_EVENT_COUNT; e++) {
        const SoundPick &p = c.sounds[e];
        fprintf(f, "sound_%s = %s %s\n", SOUND_EVENTS[e].key, p.on ? "on" : "off", sound_key(p.sound));
        if (p.own.s[0]) {
            char k[64];
            snprintf(k, sizeof k, "sound_%s_file", SOUND_EVENTS[e].key);
            path(k, p.own);
        }
    }
    boolean("voice_mono", c.voice_mono);
    boolean("dev_log", c.dev_log);
    fprintf(f, "accent = %06x\ncard_width = %d\ngames_collapsed = %d\n", c.accent, c.card_width, c.games_collapsed ? 1 : 0);
    fprintf(f, "unsaved_collapsed = %d\nside_folders = %d\n", c.unsaved_collapsed ? 1 : 0, c.side_folders ? 1 : 0);
    fprintf(f, "timeline_lane = %d\n", c.timeline_lane);
    fprintf(f, "timeline_top = %g\ntimeline_names = %d\ntimeline_simple = %d\n", c.timeline_top, c.timeline_names,
            c.timeline_simple ? 1 : 0);
    fprintf(f, "editor_top = %g\neditor_side = %g\neditor_lane = %d\neditor_zoom_view = %d\n", c.editor_top, c.editor_side,
            c.editor_lane, c.editor_zoom_view);
    fprintf(f, "\n# hotkey = keys | action | action when held\n");
    if (!c.hotkey_count) fprintf(f, "hotkeys = none\n");
    for (i32 i = 0; i < c.hotkey_count; i++) {
        const Hotkey &h = c.hotkeys[i];
        fprintf(f, "hotkey = %s | %s%s%s\n", h.keys.s, h.action.s, h.hold.s[0] ? " | " : "", h.hold.s);
    }
    fprintf(f, "\n# app = exe | name | category | session mode | bitrate (Mbps) | fps | rate control | quality | preset"
               " | height | where it was last heard | capture window | clip saved\n"
               "# (a game's own settings; 0 or empty = the default. capture window: 1 = yes, -1 = no. clip saved:"
               " full, small or off)\n");
    for (i32 i = 0; i < c.app_count; i++) {
        const AppInfo &a = c.apps[i];
        fprintf(f, "app = %s | %s | %s | %s | %g | %d | %s | %d | %s | %d | %s | %d | %s\n", a.exe.s, a.name.s,
                category_name(a.category), session_mode_name(a.session_mode), a.rec.bitrate_mbps, a.rec.fps,
                a.rec.rate_control.s, a.rec.cq, a.rec.preset.s, a.rec.height, a.path, a.rec.window,
                toast_clip_name(a.toast_clip));
    }
    fprintf(f, "\n# input = mono | stereo, name | endpoint id (a recording device on a track of its own)\n");
    for (i32 i = 0; i < c.input_count; i++)
        if (c.inputs[i].mode != INPUT_OFF)
            fprintf(f, "input = %s | %s | %s\n", c.inputs[i].mode == INPUT_STEREO ? "stereo" : "mono", c.inputs[i].name,
                    c.inputs[i].id);
    fprintf(f, "\n# folder = category | path (every app in it or below it, unless the app has its own category)\n");
    for (i32 i = 0; i < c.folder_count; i++)
        fprintf(f, "folder = %s | %s\n", category_name(c.folders[i].category), c.folders[i].path);
    bool ok = fclose(f) == 0;
    ok = ok && MoveFileExW(tmp.s, paths.settings.s, MOVEFILE_REPLACE_EXISTING);
    if (!ok) log_line("couldn't save settings (%d)", (int)GetLastError());
    return ok;
}
