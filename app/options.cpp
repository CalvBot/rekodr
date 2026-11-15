// Options: every setting, and bringing the settings over from the Rust version on first run.
//
// Every change applies at once: it's written to settings.txt (config_save) and handed to whatever uses it
// (options_apply). There's no Save button. Text and number boxes count only when the edit is finished (Enter or
// clicking away), so a half-typed bitrate never restarts the capture.
//
// Layout rules (SPEC.md, "UI rules"): fixed-width controls, fixed-size paged lists whose order is frozen while
// Options is open, and settings that don't apply are disabled, never hidden.

#include <shobjidl.h>
#include <math.h>

// What a change touches, for options_apply.
enum : u32 {
    APPLY_RECORDER = 1, // recorder_settings_changed: capture, encoder, buffer folder and limits
    APPLY_AUDIO = 2,    // audio_config_changed: app categories, names, the mic
    APPLY_HOTKEYS = 4,  // hotkeys_apply
};

enum AppsFilter : i32 { FILTER_ALL, FILTER_NEW, FILTER_GAMES, FILTER_COMMUNICATION, FILTER_APPS, FILTER_IGNORED };

static f32 dp(f32 v) { return v * g_dpi_scale; }

// A test copy (CLIPBACK_HOME) leaves the hotkeys and Windows' Run list to the real clipback (see main.cpp).
static bool options_test_copy()
{
    wchar_t home[MAX_PATH] = {};
    return GetEnvironmentVariableW(L"CLIPBACK_HOME", home, MAX_PATH) && home[0];
}

static void options_apply(u32 what)
{
    config_save(g_app.config, g_app.paths);
    if (what & APPLY_RECORDER) recorder_settings_changed();
    if (what & APPLY_AUDIO) audio_config_changed();
    if ((what & APPLY_HOTKEYS) && !options_test_copy()) hotkeys_apply(g_app.config, g_app.msg_hwnd);
    ui_wake();
}

// ---------------------------------------------------------------------------------------------------------------
// Start with Windows: a "clipback" value under HKCU\...\Run, the same name the Rust version used, so turning it on
// here replaces the old one.

static const wchar_t *RUN_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

static void startup_command(wchar_t *out, i32 cap)
{
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(0, exe, MAX_PATH);
    swprintf(out, cap, L"\"%s\"", exe);
}

// The Run value as it is now. Returns false if there's none.
static bool startup_read(wchar_t *out, i32 cap)
{
    DWORD bytes = (DWORD)(cap * sizeof(wchar_t));
    out[0] = 0;
    return RegGetValueW(HKEY_CURRENT_USER, RUN_KEY, L"clipback", RRF_RT_REG_SZ, 0, out, &bytes) == ERROR_SUCCESS;
}

static void startup_set(bool on)
{
    wchar_t cmd[MAX_PATH + 8];
    startup_command(cmd, COUNT(cmd));
    if (options_test_copy()) {
        log_line("start with Windows %s; a test copy leaves the Run key alone (it would %s HKCU\\%ls\\clipback%s%ls)",
                 on ? "on" : "off", on ? "set" : "delete", RUN_KEY, on ? " = " : "", on ? cmd : L"");
        return;
    }
    HKEY key = 0;
    LSTATUS r = RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &key);
    if (r == ERROR_SUCCESS) {
        if (on) r = RegSetValueExW(key, L"clipback", 0, REG_SZ, (const BYTE *)cmd, (DWORD)((wcslen(cmd) + 1) * sizeof(wchar_t)));
        else {
            r = RegDeleteValueW(key, L"clipback");
            if (r == ERROR_FILE_NOT_FOUND) r = ERROR_SUCCESS; // already gone
        }
        RegCloseKey(key);
    }
    if (r == ERROR_SUCCESS) log_line("start with Windows %s", on ? "on" : "off");
    else log_line("start with Windows: updating the Run key failed (%d)", (int)r);
}

// ---------------------------------------------------------------------------------------------------------------
// A minimal TOML reader, for the Rust version's config.toml and lifelog's. It handles what those files hold:
// [tables], [[arrays of tables]], bare and quoted (dotted) keys, basic and literal strings, numbers, booleans and
// arrays of them, across lines, with comments. Inline tables and nested arrays are skipped. It calls `fn` once per
// value, and once per element of an array.

struct TomlItem {
    char table[64];   // "video", "video.game_bitrates", "hotkeys"; "" at the top
    i32  table_index; // which [[table]] this is (0, 1 …); 0 for a plain [table]
    char key[128];
    i32  array_index; // the element of an array value; -1 for a plain value
    bool is_string;
    char value[MAX_PATH * 3];
};

typedef void TomlFn(void *ctx, const TomlItem &item);

struct Toml {
    const char *p, *end;
    TomlItem    item;
    char        array_tables[16][64]; // names of [[tables]] seen, with how many of each
    i32         array_table_counts[16];
    i32         array_table_count;
};

static void toml_skip_space(Toml *t)
{
    while (t->p < t->end && (*t->p == ' ' || *t->p == '\t')) t->p++;
}

static void toml_skip_line(Toml *t)
{
    while (t->p < t->end && *t->p != '\n') t->p++;
    if (t->p < t->end) t->p++;
}

// Spaces, line ends and comments, as allowed inside an array.
static void toml_skip_blank(Toml *t)
{
    for (;;) {
        while (t->p < t->end && (*t->p == ' ' || *t->p == '\t' || *t->p == '\r' || *t->p == '\n')) t->p++;
        if (t->p < t->end && *t->p == '#') toml_skip_line(t);
        else return;
    }
}

static void utf8_put(char *out, i32 cap, i32 *n, u32 cp)
{
    char b[4];
    i32 len = 0;
    if (cp < 0x80) b[len++] = (char)cp;
    else if (cp < 0x800) { b[len++] = (char)(0xC0 | (cp >> 6)); b[len++] = (char)(0x80 | (cp & 63)); }
    else if (cp < 0x10000) {
        b[len++] = (char)(0xE0 | (cp >> 12)); b[len++] = (char)(0x80 | ((cp >> 6) & 63)); b[len++] = (char)(0x80 | (cp & 63));
    } else {
        b[len++] = (char)(0xF0 | (cp >> 18)); b[len++] = (char)(0x80 | ((cp >> 12) & 63));
        b[len++] = (char)(0x80 | ((cp >> 6) & 63)); b[len++] = (char)(0x80 | (cp & 63));
    }
    for (i32 i = 0; i < len && *n < cap - 1; i++) out[(*n)++] = b[i];
}

// A "basic", 'literal' or triple-quoted string at t->p. Returns false if there isn't one.
static bool toml_string(Toml *t, char *out, i32 cap)
{
    const char *p = t->p;
    char q = *p;
    if (q != '"' && q != '\'') return false;
    bool multi = t->end - p >= 3 && p[1] == q && p[2] == q;
    p += multi ? 3 : 1;
    if (multi && p < t->end && *p == '\r') p++;
    if (multi && p < t->end && *p == '\n') p++; // a line end straight after """ isn't part of the string
    i32 n = 0;
    while (p < t->end) {
        if (multi ? (t->end - p >= 3 && p[0] == q && p[1] == q && p[2] == q) : *p == q) {
            p += multi ? 3 : 1;
            break;
        }
        if (!multi && *p == '\n') break; // unterminated: stop at the line end
        char c = *p++;
        if (q == '"' && c == '\\' && p < t->end) {
            char e = *p++;
            switch (e) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'u':
            case 'U': {
                u32 cp = 0;
                for (i32 i = 0; i < (e == 'u' ? 4 : 8) && p < t->end && isxdigit((u8)*p); i++, p++)
                    cp = cp * 16 + (u32)(isdigit((u8)*p) ? *p - '0' : (tolower((u8)*p) - 'a' + 10));
                utf8_put(out, cap, &n, cp);
                continue;
            }
            default: c = e; break; // \" and \\ (and anything else) as the character itself
            }
        }
        if (n < cap - 1) out[n++] = c;
    }
    out[n] = 0;
    t->p = p;
    return true;
}

// A key or table name: bare or quoted parts joined by dots ("video.game_bitrates", "cs2.exe").
static void toml_name(Toml *t, char *out, i32 cap)
{
    i32 n = 0;
    out[0] = 0;
    for (;;) {
        toml_skip_space(t);
        char part[256];
        if (!toml_string(t, part, sizeof part)) {
            i32 k = 0;
            while (t->p < t->end && (isalnum((u8)*t->p) || *t->p == '_' || *t->p == '-') && k < (i32)sizeof part - 1)
                part[k++] = *t->p++;
            part[k] = 0;
        }
        n += snprintf(out + n, cap - n, "%s%s", n ? "." : "", part);
        if (n >= cap) n = cap - 1;
        toml_skip_space(t);
        if (t->p < t->end && *t->p == '.') t->p++;
        else return;
    }
}

// Skips an inline table or a nested array, minding strings.
static void toml_skip_nested(Toml *t)
{
    i32 depth = 0;
    char scratch[8];
    while (t->p < t->end) {
        char c = *t->p;
        if (c == '"' || c == '\'') { toml_string(t, scratch, sizeof scratch); continue; }
        if (c == '#') { toml_skip_line(t); continue; }
        t->p++;
        if (c == '[' || c == '{') depth++;
        else if ((c == ']' || c == '}') && --depth == 0) return;
    }
}

// One scalar at t->p into item.value. Returns false if there's nothing usable there.
static bool toml_scalar(Toml *t)
{
    t->item.is_string = toml_string(t, t->item.value, sizeof t->item.value);
    if (t->item.is_string) return true;
    i32 n = 0;
    while (t->p < t->end && !strchr(" \t\r\n,]}#", *t->p) && n < (i32)sizeof t->item.value - 1) t->item.value[n++] = *t->p++;
    t->item.value[n] = 0;
    return n > 0;
}

static void toml_parse(const char *text, i64 size, TomlFn *fn, void *ctx)
{
    Toml *t = (Toml *)calloc(1, sizeof(Toml));
    if (!t) return;
    t->p = text;
    t->end = text + size;
    while (t->p < t->end) {
        toml_skip_blank(t);
        if (t->p >= t->end) break;
        if (*t->p == '[') {
            bool array_table = t->end - t->p >= 2 && t->p[1] == '[';
            t->p += array_table ? 2 : 1;
            toml_name(t, t->item.table, sizeof t->item.table);
            t->item.table_index = 0;
            if (array_table) {
                i32 k = 0;
                while (k < t->array_table_count && strcmp(t->array_tables[k], t->item.table)) k++;
                if (k == t->array_table_count && k < COUNT(t->array_tables)) {
                    str_set(t->array_tables[k], t->item.table);
                    t->array_table_counts[k] = 0;
                    t->array_table_count++;
                }
                if (k < t->array_table_count) t->item.table_index = t->array_table_counts[k]++;
            }
            toml_skip_line(t);
            continue;
        }
        toml_name(t, t->item.key, sizeof t->item.key);
        toml_skip_space(t);
        if (t->p >= t->end || *t->p != '=' || !t->item.key[0]) { // not a key = value line
            toml_skip_line(t);
            continue;
        }
        t->p++;
        toml_skip_space(t);
        if (t->p < t->end && *t->p == '[') {
            t->p++;
            for (i32 index = 0;;) {
                toml_skip_blank(t);
                if (t->p >= t->end) break;
                if (*t->p == ']') { t->p++; break; }
                if (*t->p == ',') { t->p++; continue; }
                if (*t->p == '[' || *t->p == '{') { toml_skip_nested(t); continue; }
                if (!toml_scalar(t)) { t->p++; continue; }
                t->item.array_index = index++;
                fn(ctx, t->item);
            }
        } else if (t->p < t->end && *t->p == '{') {
            toml_skip_nested(t);
        } else if (toml_scalar(t)) {
            t->item.array_index = -1;
            fn(ctx, t->item);
        }
        toml_skip_line(t); // the rest of the line is a comment (or junk)
    }
    free(t);
}

// Reads a whole file (null-terminated) into memory from malloc. Returns 0 if it can't be read.
static char *options_read_file(const Path &path, i64 *size)
{
    *size = 0;
    HANDLE f = CreateFileW(path.s, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING, 0, 0);
    if (f == INVALID_HANDLE_VALUE) return 0;
    LARGE_INTEGER len = {};
    char *data = 0;
    if (GetFileSizeEx(f, &len) && len.QuadPart < (16 << 20)) {
        data = (char *)malloc((size_t)len.QuadPart + 1);
        DWORD got = 0;
        if (data && ReadFile(f, data, (DWORD)len.QuadPart, &got, 0)) {
            data[got] = 0;
            *size = got;
        } else {
            free(data);
            data = 0;
        }
    }
    CloseHandle(f);
    return data;
}

// ---------------------------------------------------------------------------------------------------------------
// First run: settings from the Rust version (%APPDATA%\clipback\config.toml) and the game list from lifelog's
// config.toml ([processing] games / not_games). Both are only read. A test copy (CLIPBACK_HOME) reads copies of
// them from <home>\import instead: config.toml, lifelog-config.toml, seen_apps.txt.

// The Rust version's default communication apps, used when its config has none (or there's no config).
static const char *DEFAULT_COMMUNICATION[] = {
    "discord.exe", "discordptb.exe", "discordcanary.exe", "ms-teams.exe", "teams.exe", "zoom.exe", "slack.exe",
    "ts3client_win64.exe", "teamspeak.exe", "skype.exe", "telegram.exe", "whatsapp.exe", "signal.exe", "mumble.exe",
    "guilded.exe", "element.exe", "vesktop.exe",
};

enum ImportList { LIST_SORTED, LIST_NOT_GAMES, LIST_GAMES, LIST_COMMUNICATION, LIST_IGNORED, LIST_COUNT };

struct ImportGameRate { Str64 exe; f32 mbps; };

struct Import {
    Config        *c;
    Str64          lists[LIST_COUNT][512];
    i32            list_counts[LIST_COUNT];
    bool           communication_seen; // the file has communication_apps (an empty list means none)
    Hotkey         hotkeys[16];
    i32            hotkey_count;
    ImportGameRate rates[256];
    i32            rate_count;
    f64            max_buffer_gb;
    bool           have_buffer_dir, have_clips_dir;
    Path           buffer_dir, clips_dir;
};

static Import g_import; // big, so not on the stack; only used once, at startup

static void import_path(const wchar_t *test_name, const wchar_t *real, Path *out)
{
    wchar_t home[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"CLIPBACK_HOME", home, MAX_PATH) && home[0])
        swprintf(out->s, MAX_PATH, L"%s\\import\\%s", home, test_name);
    else wcscpy_s(out->s, real);
}

static void import_list_add(Import *im, i32 list, const char *exe)
{
    if (im->list_counts[list] == COUNT(im->lists[list]) || !exe[0]) return;
    Str64 *e = &im->lists[list][im->list_counts[list]++];
    str_set(e->s, exe);
    for (char *p = e->s; *p; p++) *p = (char)tolower((u8)*p);
}

static bool toml_true(const TomlItem &it) { return !strcmp(it.value, "true"); }

static void import_rust_item(void *ctx, const TomlItem &it)
{
    Import *im = (Import *)ctx;
    Config *c = im->c;
    const char *t = it.table, *k = it.key, *v = it.value;
    if (!t[0]) {
        if (!strcmp(k, "buffer_dir") || !strcmp(k, "ring_dir")) {
            utf8_to_wide(v, im->buffer_dir.s, MAX_PATH);
            im->have_buffer_dir = true;
        } else if (!strcmp(k, "clips_dir")) {
            utf8_to_wide(v, im->clips_dir.s, MAX_PATH);
            im->have_clips_dir = true;
        } else if (!strcmp(k, "max_buffer_gb") || !strcmp(k, "max_ring_gb")) im->max_buffer_gb = atof(v);
        else if (!strcmp(k, "ffmpeg")) utf8_to_wide(v, c->ffmpeg.s, MAX_PATH);
        else if (!strcmp(k, "start_with_windows")) c->start_with_windows = toml_true(it);
    } else if (!strcmp(t, "video")) {
        if (!strcmp(k, "output_idx")) c->output_idx = atoi(v);
        else if (!strcmp(k, "fps")) c->fps = atoi(v);
        else if (!strcmp(k, "codec")) str_set(c->codec.s, v);
        else if (!strcmp(k, "encoder")) str_set(c->encoder.s, v);
        else if (!strcmp(k, "bitrate_mbps")) c->bitrate_mbps = (f32)atof(v);
        else if (!strcmp(k, "draw_mouse")) c->draw_mouse = toml_true(it);
        else if (!strcmp(k, "advanced")) c->advanced = toml_true(it);
        else if (!strcmp(k, "preset")) str_set(c->preset.s, v);
        else if (!strcmp(k, "rate_control")) str_set(c->rate_control.s, v);
        else if (!strcmp(k, "cq")) c->cq = atoi(v);
        else if (!strcmp(k, "peak_factor")) c->peak_factor = (f32)atof(v);
        else if (!strcmp(k, "spatial_aq")) c->spatial_aq = toml_true(it);
    } else if (!strcmp(t, "video.game_bitrates")) {
        if (im->rate_count < COUNT(im->rates)) {
            ImportGameRate *r = &im->rates[im->rate_count++];
            str_set(r->exe.s, k);
            for (char *p = r->exe.s; *p; p++) *p = (char)tolower((u8)*p);
            r->mbps = (f32)atof(v);
        }
    } else if (!strcmp(t, "audio")) {
        if (!strcmp(k, "mic")) c->mic = toml_true(it);
        else if (!strcmp(k, "communication_apps")) import_list_add(im, LIST_COMMUNICATION, v);
        else if (!strcmp(k, "ignored_apps")) import_list_add(im, LIST_IGNORED, v);
        else if (!strcmp(k, "sorted_apps")) import_list_add(im, LIST_SORTED, v);
    } else if (!strcmp(t, "hotkeys") && it.table_index < COUNT(im->hotkeys)) {
        Hotkey *h = &im->hotkeys[it.table_index];
        if (!strcmp(k, "keys")) str_set(h->keys.s, v);
        else if (!strcmp(k, "action")) {
            // The Rust version's "pause" toggled; here "pause" only pauses.
            str_set(h->action.s, !_stricmp(v, "pause") ? "toggle" : v);
            for (char *p = h->action.s; *p; p++) *p = (char)tolower((u8)*p);
        }
        if (it.table_index + 1 > im->hotkey_count) im->hotkey_count = it.table_index + 1;
    }
}

// communication_apps = [] calls nothing, yet means "none", so the key is looked for in the text.
static bool toml_has_key(const char *text, const char *key)
{
    size_t n = strlen(key);
    for (const char *p = strstr(text, key); p; p = strstr(p + 1, key)) {
        bool line_start = p == text || p[-1] == '\n' || p[-1] == ' ' || p[-1] == '\t';
        const char *q = p + n;
        while (*q == ' ' || *q == '\t') q++;
        if (line_start && *q == '=') return true;
    }
    return false;
}

static void import_lifelog_item(void *ctx, const TomlItem &it)
{
    Import *im = (Import *)ctx;
    if (strcmp(it.table, "processing") || it.array_index < 0) return;
    if (!strcmp(it.key, "games")) import_list_add(im, LIST_GAMES, it.value);
    else if (!strcmp(it.key, "not_games")) import_list_add(im, LIST_NOT_GAMES, it.value);
}

// A folder from the Rust config, unless this is a test copy and it's outside CLIPBACK_HOME: a test copy never
// records into or saves to the real folders.
static void import_folder(const char *what, const Path &from, Path *to)
{
    char u[MAX_PATH * 3];
    wide_to_utf8(from.s, u, sizeof u);
    wchar_t home[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"CLIPBACK_HOME", home, MAX_PATH) && home[0] && _wcsnicmp(from.s, home, wcslen(home))) {
        log_line("import: %s would be %s; a test copy keeps its own", what, u);
        return;
    }
    *to = from;
    log_line("import: %s = %s", what, u);
}

static void import_seen_apps(Config *c, const Path &path)
{
    i64 size = 0;
    char *text = options_read_file(path, &size);
    if (!text) return;
    i32 n = 0;
    char *next = 0;
    for (char *line = strtok_s(text, "\r\n", &next); line; line = strtok_s(0, "\r\n", &next)) {
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        char *exe = trim(line);
        for (char *p = exe; *p; p++) *p = (char)tolower((u8)*p);
        AppInfo *a = config_app(c, exe); // new ones stay new (CAT_NONE), as in the Rust version
        if (!a) break;
        a->last_heard_us = _atoi64(tab + 1);
        n++;
    }
    free(text);
    log_line("import: %d apps heard by the Rust version", n);
}

// Builds the first settings: the defaults, then the Rust version's settings and lifelog's game list if they're
// there. Call when there's no settings.txt yet; the caller saves the result.
static void options_first_run(Config *c, const Paths &paths)
{
    config_defaults(c, paths);
    Import *im = &g_import;
    memset(im, 0, sizeof *im);
    im->c = c;

    wchar_t *roaming = 0, *local = 0;
    SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, 0, &roaming);
    SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, 0, &local);
    Path real = {}, rust = {}, lifelog = {}, seen = {};
    swprintf(real.s, MAX_PATH, L"%s\\clipback\\config.toml", roaming ? roaming : L".");
    import_path(L"config.toml", real.s, &rust);
    if (!GetEnvironmentVariableW(L"LIFELOG_CONFIG", real.s, MAX_PATH)) // lifelog's own override (its config.rs)
        swprintf(real.s, MAX_PATH, L"%s\\lifelog\\config.toml", roaming ? roaming : L".");
    import_path(L"lifelog-config.toml", real.s, &lifelog);
    swprintf(real.s, MAX_PATH, L"%s\\clipback\\seen_apps.txt", local ? local : L".");
    import_path(L"seen_apps.txt", real.s, &seen);
    CoTaskMemFree(roaming);
    CoTaskMemFree(local);

    i64 size = 0;
    char *text = options_read_file(rust, &size);
    if (text) {
        char u[MAX_PATH * 3];
        wide_to_utf8(rust.s, u, sizeof u);
        log_line("import: settings from %s", u);
        toml_parse(text, size, import_rust_item, im);
        im->communication_seen = toml_has_key(text, "communication_apps");
        free(text);
        if (im->have_buffer_dir) import_folder("buffer folder", im->buffer_dir, &c->buffer_dir);
        if (im->have_clips_dir) {
            // Rust saved to <clips_dir>\Clips\<year>; save_dir is that Clips folder (holding <year>\Clips …).
            Path save = {};
            swprintf(save.s, MAX_PATH, L"%s\\Clips", im->clips_dir.s);
            import_folder("save folder", save, &c->save_dir);
        }
        if (im->max_buffer_gb > 0) {
            // The Rust buffer was a size; here it's a length. The same space at the same bitrate:
            f64 mbps = c->bitrate_mbps > 0 ? c->bitrate_mbps : 20;
            c->buffer_hours = (f32)(floor(im->max_buffer_gb * 8000.0 / mbps / 3600.0 * 10 + 0.5) / 10);
            if (c->buffer_hours < 1) c->buffer_hours = 1;
            log_line("import: a %g GB buffer at %g Mbps keeps %g h", im->max_buffer_gb, mbps, c->buffer_hours);
        }
        if (im->hotkey_count) {
            c->hotkey_count = 0;
            for (i32 i = 0; i < im->hotkey_count && c->hotkey_count < COUNT(c->hotkeys); i++)
                if (im->hotkeys[i].keys.s[0] && im->hotkeys[i].action.s[0]) c->hotkeys[c->hotkey_count++] = im->hotkeys[i];
        }
    }

    text = options_read_file(lifelog, &size);
    if (text) {
        toml_parse(text, size, import_lifelog_item, im);
        free(text);
        log_line("import: lifelog's list has %d games and %d apps that aren't", im->list_counts[LIST_GAMES],
                 im->list_counts[LIST_NOT_GAMES]);
    }

    if (!im->communication_seen)
        for (i32 i = 0; i < COUNT(DEFAULT_COMMUNICATION); i++) import_list_add(im, LIST_COMMUNICATION, DEFAULT_COMMUNICATION[i]);

    // Later lists win, as in the Rust version's apps::category: ignored, then communication, then games.
    static const AppCategory list_category[LIST_COUNT] = {CAT_APP, CAT_APP, CAT_GAME, CAT_COMMUNICATION, CAT_IGNORED};
    for (i32 l = 0; l < LIST_COUNT; l++) {
        for (i32 i = 0; i < im->list_counts[l]; i++) {
            AppInfo *a = config_app(c, im->lists[l][i].s);
            if (a) a->category = list_category[l];
        }
    }
    for (i32 i = 0; i < im->rate_count; i++) {
        AppInfo *a = config_app(c, im->rates[i].exe.s);
        if (!a) continue;
        a->bitrate_mbps = im->rates[i].mbps;
        if (a->category == CAT_NONE) a->category = CAT_GAME; // it had a game bitrate, so it's a game
    }
    import_seen_apps(c, seen);
    log_line("import: %d apps, %d hotkeys", c->app_count, c->hotkey_count);
}

// ---------------------------------------------------------------------------------------------------------------
// Finding an app's exe for "Open file location": a running copy, else lifelog's record of where it saw it
// (%LOCALAPPDATA%\lifelog\paths.txt, "exe<TAB>path" lines; read only).

static bool app_find_path(const char *exe, Path *out)
{
    out->s[0] = 0;
    wchar_t want[MAX_PATH];
    utf8_to_wide(exe, want, MAX_PATH);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W e = {};
        e.dwSize = sizeof e;
        for (BOOL ok = Process32FirstW(snap, &e); ok && !out->s[0]; ok = Process32NextW(snap, &e)) {
            if (_wcsicmp(e.szExeFile, want)) continue;
            HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, e.th32ProcessID);
            if (!p) continue; // elevated, or gone
            DWORD len = MAX_PATH;
            if (!QueryFullProcessImageNameW(p, 0, out->s, &len)) out->s[0] = 0;
            CloseHandle(p);
        }
        CloseHandle(snap);
    }
    if (out->s[0]) return true;

    wchar_t *local = 0;
    SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, 0, &local);
    Path real = {}, paths = {};
    swprintf(real.s, MAX_PATH, L"%s\\lifelog\\paths.txt", local ? local : L".");
    CoTaskMemFree(local);
    import_path(L"lifelog-paths.txt", real.s, &paths);
    i64 size = 0;
    char *text = options_read_file(paths, &size);
    if (!text) return false;
    char *next = 0;
    for (char *line = strtok_s(text, "\r\n", &next); line && !out->s[0]; line = strtok_s(0, "\r\n", &next)) {
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        if (_stricmp(line, exe)) continue;
        utf8_to_wide(tab + 1, out->s, MAX_PATH);
        if (GetFileAttributesW(out->s) == INVALID_FILE_ATTRIBUTES) out->s[0] = 0; // moved or uninstalled
    }
    free(text);
    return out->s[0] != 0;
}

// explorer /select, as the Rust version did (SHOpenFolderAndSelectItems returned E_ABORT from here).
static void reveal_file(const Path &path)
{
    wchar_t args[MAX_PATH + 16];
    swprintf(args, COUNT(args), L"/select,\"%s\"", path.s);
    INT_PTR r = (INT_PTR)ShellExecuteW(0, L"open", L"explorer.exe", args, 0, SW_SHOWNORMAL);
    char u[MAX_PATH * 3];
    wide_to_utf8(path.s, u, sizeof u);
    if (r <= 32) log_line("couldn't show %s in Explorer (%d)", u, (int)r);
    else log_line("showing %s in Explorer", u);
}

// ---------------------------------------------------------------------------------------------------------------
// Monitors, in ddagrab's output_idx order (the outputs of the first GPU), with the names Display settings shows.

static void options_list_monitors()
{
    OptionsUi &o = g_app.options;
    o.monitor_count = 0;

    // GDI device name (\\.\DISPLAY1) → monitor name
    static DISPLAYCONFIG_PATH_INFO paths[32];
    static DISPLAYCONFIG_MODE_INFO modes[64];
    UINT32 np = COUNT(paths), nm = COUNT(modes);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths, &nm, modes, 0) != ERROR_SUCCESS) np = 0;

    IDXGIFactory1 *factory = 0;
    IDXGIAdapter1 *adapter = 0;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return;
    if (SUCCEEDED(factory->EnumAdapters1(0, &adapter))) {
        IDXGIOutput *out = 0;
        for (UINT i = 0; o.monitor_count < COUNT(o.monitors) && adapter->EnumOutputs(i, &out) == S_OK; i++) {
            DXGI_OUTPUT_DESC d = {};
            out->GetDesc(&d);
            out->Release();
            MonitorInfo *m = &o.monitors[o.monitor_count++];
            *m = {};
            m->width = d.DesktopCoordinates.right - d.DesktopCoordinates.left;
            m->height = d.DesktopCoordinates.bottom - d.DesktopCoordinates.top;
            m->primary = d.DesktopCoordinates.left == 0 && d.DesktopCoordinates.top == 0;
            DEVMODEW mode = {};
            mode.dmSize = sizeof mode;
            if (EnumDisplaySettingsW(d.DeviceName, ENUM_CURRENT_SETTINGS, &mode)) m->hz = (i32)mode.dmDisplayFrequency;
            wide_to_utf8(d.DeviceName[0] == '\\' ? d.DeviceName + 4 : d.DeviceName, m->name, sizeof m->name);
            for (UINT32 k = 0; k < np; k++) {
                DISPLAYCONFIG_SOURCE_DEVICE_NAME src = {};
                src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
                src.header.size = sizeof src;
                src.header.adapterId = paths[k].sourceInfo.adapterId;
                src.header.id = paths[k].sourceInfo.id;
                DISPLAYCONFIG_TARGET_DEVICE_NAME tgt = {};
                tgt.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
                tgt.header.size = sizeof tgt;
                tgt.header.adapterId = paths[k].targetInfo.adapterId;
                tgt.header.id = paths[k].targetInfo.id;
                if (DisplayConfigGetDeviceInfo(&src.header) || DisplayConfigGetDeviceInfo(&tgt.header)) continue;
                if (_wcsicmp(src.viewGdiDeviceName, d.DeviceName) || !tgt.monitorFriendlyDeviceName[0]) continue;
                wide_to_utf8(tgt.monitorFriendlyDeviceName, m->name, sizeof m->name);
                break;
            }
        }
        adapter->Release();
    }
    factory->Release();
}

// ---------------------------------------------------------------------------------------------------------------
// Controls

// A text box whose edit counts only once it's finished. Returns true then, with the text in `out`.
static bool opt_text(const char *id, const char *current, f32 width, ImGuiInputTextFlags flags, const char *hint,
                     char *out, i32 cap)
{
    OptionsUi &o = g_app.options;
    bool active = ImGui::GetActiveID() == ImGui::GetID(id);
    char shown[MAX_PATH * 3];
    str_set(shown, current);
    char *buf = active ? o.edit : shown;
    ImGui::SetNextItemWidth(width);
    if (hint) ImGui::InputTextWithHint(id, hint, buf, sizeof o.edit, flags);
    else ImGui::InputText(id, buf, sizeof o.edit, flags);
    if (ImGui::IsItemActivated()) str_set(o.edit, current);
    if (!ImGui::IsItemDeactivatedAfterEdit()) return false;
    snprintf(out, cap, "%s", o.edit);
    return true;
}

// A number box. Returns true when an edit finished with a different number; it's clamped to lo..hi. With
// `blank_zero`, 0 shows as an empty box (and an empty box means 0).
static bool opt_number(const char *id, f64 *v, f64 lo, f64 hi, f32 width, bool blank_zero = false, const char *hint = 0)
{
    char cur[64] = "";
    if (!(blank_zero && *v == 0)) snprintf(cur, sizeof cur, "%g", *v);
    char text[64];
    if (!opt_text(id, cur, width, ImGuiInputTextFlags_CharsDecimal, hint, text, sizeof text)) return false;
    char *s = trim(text);
    f64 n = 0;
    if (*s) {
        char *end = 0;
        n = strtod(s, &end);
        if (end == s) return false;
        if (n < lo) n = lo;
        if (n > hi) n = hi;
    } else if (!blank_zero) {
        return false;
    }
    if (n == *v) return false;
    *v = n;
    return true;
}

static bool opt_f32(const char *id, f32 *v, f64 lo, f64 hi, f32 width)
{
    f64 d = *v;
    if (!opt_number(id, &d, lo, hi, width)) return false;
    *v = (f32)d;
    return true;
}

static bool opt_i32(const char *id, i32 *v, f64 lo, f64 hi, f32 width)
{
    f64 d = *v;
    if (!opt_number(id, &d, lo, hi, width)) return false;
    d = floor(d + 0.5);
    if ((i32)d == *v) return false;
    *v = (i32)d;
    return true;
}

// A drop-down of fixed width. Items whose bit is set in `disabled` show greyed out and can't be picked.
static bool opt_combo(const char *id, const char *const *items, i32 count, i32 *index, f32 width, u32 disabled = 0,
                      const char *preview = 0)
{
    bool changed = false;
    ImGui::SetNextItemWidth(width);
    const char *shown = preview ? preview : (*index >= 0 && *index < count ? items[*index] : "");
    if (ImGui::BeginCombo(id, shown)) {
        for (i32 i = 0; i < count; i++) {
            bool off = (disabled >> i) & 1;
            if (ImGui::Selectable(items[i], i == *index, off ? ImGuiSelectableFlags_Disabled : 0) && i != *index) {
                *index = i;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

// The label column of a form row.
static void opt_label(const char *label)
{
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(dp(230));
}

static void opt_unit(const char *text)
{
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", text);
}

static void opt_title(const char *title, const char *about)
{
    ImGui::PushFont(g_font_big);
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, rgb(COL_DIM));
    ImGui::TextWrapped("%s", about);
    ImGui::PopStyleColor();
    ImGui::Spacing();
}

static f32 opt_row_height()
{
    return ImGui::GetFrameHeight() + 2 * ImGui::GetStyle().CellPadding.y;
}

// How many rows of a paged list fit in what's left of the page, leaving `below` for the pager.
static i32 opt_rows_fit(f32 below)
{
    f32 h = ImGui::GetContentRegionAvail().y - below - opt_row_height(); // the header row
    i32 rows = (i32)(h / opt_row_height());
    return rows < 3 ? 3 : rows > 30 ? 30 : rows;
}

static f32 opt_pager_height() { return ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y; }

// Previous · Page 2 of 5 · Next, all fixed widths.
static void opt_pager(const char *id, i32 *page, i32 total, i32 per_page)
{
    i32 pages = total > 0 ? (total + per_page - 1) / per_page : 1;
    if (*page >= pages) *page = pages - 1;
    if (*page < 0) *page = 0;
    ImGui::PushID(id);
    ImGui::BeginDisabled(*page == 0);
    if (ImGui::Button("Previous", ImVec2(dp(100), 0))) (*page)--;
    ImGui::EndDisabled();
    ImGui::SameLine();
    char text[64];
    snprintf(text, sizeof text, "Page %d of %d", *page + 1, pages);
    f32 w = dp(130), tw = ImGui::CalcTextSize(text).x;
    f32 x = ImGui::GetCursorPosX();
    ImGui::SetCursorPosX(x + (w - tw) / 2);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(text);
    ImGui::SameLine(x + w);
    ImGui::BeginDisabled(*page >= pages - 1);
    if (ImGui::Button("Next", ImVec2(dp(100), 0))) (*page)++;
    ImGui::EndDisabled();
    ImGui::PopID();
}

static void bytes_text(i64 bytes, char *out, i32 cap)
{
    if (bytes < 0) snprintf(out, cap, "unknown");
    else if (bytes >= 1000000000000LL) snprintf(out, cap, "%.2f TB", bytes / 1e12);
    else if (bytes >= 1000000000LL) snprintf(out, cap, "%.1f GB", bytes / 1e9);
    else snprintf(out, cap, "%.0f MB", bytes / 1e6);
}

static void ago_text(i64 us, char *out, i32 cap)
{
    if (!us) { snprintf(out, cap, "\xE2\x80\x94"); return; } // —: not since clipback started
    i64 s = (now_us() - us) / 1000000;
    if (s < 90) snprintf(out, cap, "just now");
    else if (s < 90 * 60) snprintf(out, cap, "%lld min ago", s / 60);
    else if (s < 36 * 3600) snprintf(out, cap, "%lld h ago", s / 3600);
    else snprintf(out, cap, "%lld days ago", s / 86400);
}

// ---------------------------------------------------------------------------------------------------------------
// Apps

static const char *CATEGORY_LABELS[] = {
    "New: pick a category", "App: its own track", "Game: own track, sessions", "Communication track",
    "Ignored: never recorded",
};

// The name an app gets when it has none of its own: "Overwatch" for overwatch.exe (as audio_display_name).
static void default_app_name(const char *exe, char *out, i32 cap)
{
    snprintf(out, cap, "%s", exe);
    size_t n = strlen(out);
    if (n > 4 && !strcmp(out + n - 4, ".exe")) out[n - 4] = 0;
    out[0] = (char)toupper((u8)out[0]);
}

static bool app_matches(const AppInfo &a, i32 filter, const char *search)
{
    switch (filter) {
    case FILTER_NEW: if (a.category != CAT_NONE) return false; break;
    case FILTER_GAMES: if (a.category != CAT_GAME) return false; break;
    case FILTER_COMMUNICATION: if (a.category != CAT_COMMUNICATION) return false; break;
    case FILTER_APPS: if (a.category != CAT_APP) return false; break;
    case FILTER_IGNORED: if (a.category != CAT_IGNORED) return false; break;
    default: break;
    }
    char needle[64];
    str_set(needle, search);
    char *n = trim(needle);
    if (!n[0]) return true;
    for (char *p = n; *p; p++) *p = (char)tolower((u8)*p);
    char hay[160];
    snprintf(hay, sizeof hay, "%s %s", a.exe.s, a.name.s);
    for (char *p = hay; *p; p++) *p = (char)tolower((u8)*p);
    return strstr(hay, n) != 0;
}

// New apps first (sorting them is like clearing an inbox), then the most recently heard, then by name.
static int app_order_cmp(const void *pa, const void *pb)
{
    const AppInfo &a = g_app.config.apps[*(const i16 *)pa], &b = g_app.config.apps[*(const i16 *)pb];
    bool na = a.category == CAT_NONE, nb = b.category == CAT_NONE;
    if (na != nb) return na ? -1 : 1;
    if (a.last_heard_us != b.last_heard_us) return a.last_heard_us > b.last_heard_us ? -1 : 1;
    return strcmp(a.exe.s, b.exe.s);
}

static void apps_order_make()
{
    OptionsUi &o = g_app.options;
    const Config &c = g_app.config;
    o.order_count = 0;
    for (i32 i = 0; i < c.app_count; i++)
        if (app_matches(c.apps[i], o.filter, o.search)) o.order[o.order_count++] = (i16)i;
    qsort(o.order, o.order_count, sizeof o.order[0], app_order_cmp);
    o.order_app_count = c.app_count;
    o.order_dirty = false;
}

// Apps heard (or added) since the order was made go on the end, so nothing already shown moves.
static void apps_order_extend()
{
    OptionsUi &o = g_app.options;
    const Config &c = g_app.config;
    for (i32 i = o.order_app_count; i < c.app_count; i++)
        if (app_matches(c.apps[i], o.filter, o.search) && o.order_count < COUNT(o.order)) o.order[o.order_count++] = (i16)i;
    o.order_app_count = c.app_count;
}

static bool app_playing(const char *exe)
{
    for (i32 i = 0; i < g_app.audio.stream_count; i++)
        if (str_eq(g_app.audio.streams[i].exe.s, exe)) return true;
    return false;
}

static void app_set_category(AppInfo *a, AppCategory cat)
{
    if (a->category == cat) return;
    log_line("options: %s is now %s", a->exe.s, category_name(cat));
    a->category = cat;
    options_apply(APPLY_AUDIO);
}

static void options_apps()
{
    OptionsUi &o = g_app.options;
    Config &c = g_app.config;
    opt_title("Apps", "Every app that has played sound. Each gets its own audio track in saved files, games first; "
                      "communication apps share one track, and ignored apps are never recorded. New apps are listed "
                      "first. Right-click an app to open its file location.");

    if (o.order_dirty) apps_order_make();
    apps_order_extend();

    static const char *filters[] = {"All apps", "New apps", "Games", "Communication", "Other apps", "Ignored"};
    opt_label("Show");
    if (opt_combo("##filter", filters, COUNT(filters), &o.filter, dp(200))) {
        o.order_dirty = true;
        o.apps_page = 0;
    }
    ImGui::SameLine(0, dp(24));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Search");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(dp(220));
    if (ImGui::InputText("##search", o.search, sizeof o.search)) {
        o.order_dirty = true;
        o.apps_page = 0;
    }

    opt_label("Add an app by its exe name");
    ImGui::SetNextItemWidth(dp(200));
    bool enter = ImGui::InputText("##add", o.add_exe, sizeof o.add_exe, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    static const char *add_cats[] = {"as communication", "as a game", "as an app", "as ignored"};
    static const AppCategory add_cat_values[] = {CAT_COMMUNICATION, CAT_GAME, CAT_APP, CAT_IGNORED};
    opt_combo("##addcat", add_cats, COUNT(add_cats), &o.add_category, dp(170));
    ImGui::SameLine();
    char exe[64];
    str_set(exe, o.add_exe);
    char *ex = trim(exe);
    for (char *p = ex; *p; p++) *p = (char)tolower((u8)*p);
    ImGui::BeginDisabled(!ex[0] || c.app_count == COUNT(c.apps));
    bool add = ImGui::Button("Add", ImVec2(dp(80), 0)) || (enter && ex[0]);
    ImGui::EndDisabled();
    i32 rows = opt_rows_fit(opt_pager_height() + ImGui::GetStyle().ItemSpacing.y);
    if (add && ex[0]) {
        char name[64];
        size_t n = strlen(ex);
        snprintf(name, sizeof name, "%s%s", ex, n < 4 || strcmp(ex + n - 4, ".exe") ? ".exe" : "");
        AppInfo *a = config_app(&c, name);
        if (a) {
            app_set_category(a, add_cat_values[o.add_category]);
            o.add_exe[0] = 0;
            apps_order_extend();
            for (i32 i = 0; i < o.order_count; i++) // turn to its page
                if (&c.apps[o.order[i]] == a) o.apps_page = i / rows;
        }
    }
    ImGui::Spacing();

    f32 rh = opt_row_height();
    bool open_menu = false;
    ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingFixedFit;
    if (ImGui::BeginTable("apps", 4, flags, ImVec2(0, rh * (rows + 1)))) {
        ImGui::TableSetupColumn("App", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Name shown in files", ImGuiTableColumnFlags_WidthFixed, dp(210));
        ImGui::TableSetupColumn("Category", ImGuiTableColumnFlags_WidthFixed, dp(230));
        ImGui::TableSetupColumn("Last played sound", ImGuiTableColumnFlags_WidthFixed, dp(140));
        ImGui::TableHeadersRow();
        for (i32 r = 0; r < rows; r++) {
            ImGui::TableNextRow(0, rh);
            i32 k = o.apps_page * rows + r;
            if (k >= o.order_count) {
                ImGui::TableSetColumnIndex(0);
                ImGui::Dummy(ImVec2(0, ImGui::GetFrameHeight()));
                continue;
            }
            i32 idx = o.order[k];
            AppInfo &a = c.apps[idx];
            ImGui::PushID(idx);
            ImGui::TableSetColumnIndex(0);
            // Spans the row, so a right-click anywhere on it (outside the boxes) opens the menu. Centred in a
            // frame-high box rather than AlignTextToFramePadding, which would push it down and make the row taller.
            ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0, 0.5f));
            ImGui::Selectable(a.exe.s, false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap,
                              ImVec2(0, ImGui::GetFrameHeight()));
            ImGui::PopStyleVar();
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                o.menu_app = idx;
                app_find_path(a.exe.s, &o.menu_path);
                open_menu = true; // opened outside the row's ID scope, where BeginPopup looks for it
            }
            if (a.category == CAT_NONE) {
                ImGui::SameLine();
                ImGui::AlignTextToFramePadding();
                ImGui::PushStyleColor(ImGuiCol_Text, rgb(COL_BLUE));
                ImGui::TextUnformatted("new");
                ImGui::PopStyleColor();
            }

            ImGui::TableSetColumnIndex(1);
            char hint[64], name[64];
            default_app_name(a.exe.s, hint, sizeof hint);
            if (opt_text("##name", a.name.s, -FLT_MIN, 0, hint, name, sizeof name)) {
                for (char *p = name; *p; p++) if (*p == '|') *p = ' '; // | separates fields in settings.txt
                str_set(a.name.s, trim(name));
                log_line("options: %s is shown as \"%s\"", a.exe.s, a.name.s[0] ? a.name.s : hint);
                options_apply(APPLY_AUDIO);
            }

            ImGui::TableSetColumnIndex(2);
            i32 cat = a.category;
            if (opt_combo("##cat", CATEGORY_LABELS, COUNT(CATEGORY_LABELS), &cat, -FLT_MIN, 1u << CAT_NONE))
                app_set_category(&a, (AppCategory)cat);

            ImGui::TableSetColumnIndex(3);
            ImGui::AlignTextToFramePadding();
            char ago[32];
            if (app_playing(a.exe.s)) {
                ImGui::PushStyleColor(ImGuiCol_Text, rgb(COL_OK));
                ImGui::TextUnformatted("playing now");
                ImGui::PopStyleColor();
            } else {
                ago_text(a.last_heard_us, ago, sizeof ago);
                ImGui::TextDisabled("%s", ago);
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (open_menu) ImGui::OpenPopup("app menu");
    if (ImGui::BeginPopup("app menu")) {
        ImGui::BeginDisabled(!o.menu_path.s[0]);
        if (ImGui::MenuItem("Open file location")) reveal_file(o.menu_path);
        ImGui::EndDisabled();
        if (!o.menu_path.s[0]) ImGui::TextDisabled("Not running, and lifelog hasn't seen where it is");
        ImGui::EndPopup();
    }
    opt_pager("apps", &o.apps_page, o.order_count, rows);
}

// ---------------------------------------------------------------------------------------------------------------
// Hotkeys

static const char *ACTION_LABELS[] = {"Save the last", "Open the window", "Open the timeline", "Pause or resume",
                                      "Pause", "Resume"};
static const char *ACTION_COMMANDS[] = {"save", "open", "timeline", "toggle", "pause", "resume"};
static const char *LENGTH_UNITS[] = {"seconds", "minutes", "hours"};

static i32 action_index(const char *action)
{
    char cmd[64];
    str_set(cmd, action);
    char *sp = strchr(cmd, ' ');
    if (sp) *sp = 0;
    for (i32 i = 0; i < COUNT(ACTION_COMMANDS); i++)
        if (!strcmp(cmd, ACTION_COMMANDS[i])) return i;
    return -1;
}

// "save 90s" → 90 and seconds; "save 2m" → 2 and minutes.
static void save_length(const char *action, f64 *amount, i32 *unit)
{
    const char *arg = strchr(action, ' ');
    *amount = 30;
    *unit = 0;
    if (!arg) return;
    char *end = 0;
    f64 v = strtod(arg + 1, &end);
    if (end == arg + 1 || v <= 0) return;
    *amount = v;
    *unit = *end == 'm' ? 1 : *end == 'h' ? 2 : 0;
}

static void set_save_action(Hotkey *h, f64 amount, i32 unit)
{
    snprintf(h->action.s, sizeof h->action.s, "save %g%c", amount, "smh"[unit]);
}

// The name hotkey_parse knows for an ImGui key, or 0.
static const char *key_name(ImGuiKey k)
{
    static char buf[8];
    if (k >= ImGuiKey_A && k <= ImGuiKey_Z) { snprintf(buf, sizeof buf, "%c", 'A' + (k - ImGuiKey_A)); return buf; }
    if (k >= ImGuiKey_0 && k <= ImGuiKey_9) { snprintf(buf, sizeof buf, "%c", '0' + (k - ImGuiKey_0)); return buf; }
    if (k >= ImGuiKey_F1 && k <= ImGuiKey_F24) { snprintf(buf, sizeof buf, "F%d", 1 + (k - ImGuiKey_F1)); return buf; }
    if (k >= ImGuiKey_Keypad0 && k <= ImGuiKey_Keypad9) { snprintf(buf, sizeof buf, "Num%d", k - ImGuiKey_Keypad0); return buf; }
    switch (k) {
    case ImGuiKey_Space: return "Space";
    case ImGuiKey_Enter: return "Enter";
    case ImGuiKey_Tab: return "Tab";
    case ImGuiKey_Escape: return "Esc";
    case ImGuiKey_Backspace: return "Backspace";
    case ImGuiKey_Insert: return "Insert";
    case ImGuiKey_Delete: return "Delete";
    case ImGuiKey_Home: return "Home";
    case ImGuiKey_End: return "End";
    case ImGuiKey_PageUp: return "PageUp";
    case ImGuiKey_PageDown: return "PageDown";
    case ImGuiKey_UpArrow: return "Up";
    case ImGuiKey_DownArrow: return "Down";
    case ImGuiKey_LeftArrow: return "Left";
    case ImGuiKey_RightArrow: return "Right";
    case ImGuiKey_PrintScreen: return "PrintScreen";
    case ImGuiKey_Pause: return "Pause";
    case ImGuiKey_ScrollLock: return "ScrollLock";
    default: return 0;
    }
}

static void hotkeys_unregister_all()
{
    for (i32 i = 0; i < g_hotkeys_registered; i++) UnregisterHotKey(g_app.msg_hwnd, i + 1);
    g_hotkeys_registered = 0;
}

static void hotkey_remove(i32 i)
{
    Config &c = g_app.config;
    log_line("options: hotkey %s (%s) removed", c.hotkeys[i].keys.s[0] ? c.hotkeys[i].keys.s : "with no keys",
             c.hotkeys[i].action.s);
    memmove(&c.hotkeys[i], &c.hotkeys[i + 1], (c.hotkey_count - i - 1) * sizeof c.hotkeys[0]);
    c.hotkey_count--;
}

static void capture_start(i32 i, bool added)
{
    OptionsUi &o = g_app.options;
    o.capture_slot = i + 1;
    o.capture_added = added;
    // Our own hotkeys would swallow the keys being pressed (RegisterHotKey takes them before the window does).
    if (!options_test_copy()) hotkeys_unregister_all();
    // Nothing keeps the keyboard focus, so Enter or Space set the hotkey rather than press a button.
    ImGui::SetWindowFocus(0);
}

static void capture_end(bool cancelled)
{
    OptionsUi &o = g_app.options;
    i32 i = o.capture_slot - 1;
    o.capture_slot = 0;
    if (cancelled && o.capture_added && i < g_app.config.hotkey_count) hotkey_remove(i);
    options_apply(APPLY_HOTKEYS);
}

// While a hotkey's keys are being set: the first key pressed with whatever modifiers are held.
static void capture_keys()
{
    OptionsUi &o = g_app.options;
    if (!o.capture_slot) return;
    ImGuiIO &io = ImGui::GetIO();
    for (i32 k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; k++) {
        if (!ImGui::IsKeyPressed((ImGuiKey)k, false)) continue;
        const char *name = key_name((ImGuiKey)k);
        if (!name) continue; // modifiers, mouse buttons, keys hotkey_parse doesn't know
        bool any_mod = io.KeyCtrl || io.KeyAlt || io.KeyShift || io.KeySuper;
        if (k == ImGuiKey_Escape && !any_mod) {
            capture_end(true);
            return;
        }
        char keys[64];
        snprintf(keys, sizeof keys, "%s%s%s%s%s", io.KeyCtrl ? "Ctrl+" : "", io.KeyAlt ? "Alt+" : "",
                 io.KeyShift ? "Shift+" : "", io.KeySuper ? "Win+" : "", name);
        u32 mods, vk;
        if (!hotkey_parse(keys, &mods, &vk)) continue;
        Hotkey *h = &g_app.config.hotkeys[o.capture_slot - 1];
        str_set(h->keys.s, keys);
        log_line("options: hotkey %s does %s", h->keys.s, h->action.s);
        capture_end(false);
        return;
    }
}

static void options_hotkeys()
{
    OptionsUi &o = g_app.options;
    Config &c = g_app.config;
    opt_title("Hotkeys", "Keys that work anywhere, even in a game. Click a hotkey's keys, then press the new ones. "
                         "Stream Deck keys can run commands instead; Help lists them.");
    capture_keys();

    bool test = options_test_copy();
    i32 rows = opt_rows_fit(opt_pager_height() + ImGui::GetFrameHeightWithSpacing());
    ImGui::BeginDisabled(c.hotkey_count == COUNT(c.hotkeys) || o.capture_slot);
    if (ImGui::Button("Add a hotkey", ImVec2(dp(160), 0))) {
        Hotkey *h = &c.hotkeys[c.hotkey_count++];
        *h = {};
        str_set(h->action.s, "save 30s");
        o.hotkeys_page = (c.hotkey_count - 1) / rows;
        capture_start(c.hotkey_count - 1, true);
    }
    ImGui::EndDisabled();
    ImGui::SameLine(0, dp(16));
    ImGui::AlignTextToFramePadding();
    if (test) ImGui::TextDisabled("This is a test copy (CLIPBACK_HOME): the real clipback keeps the hotkeys.");
    else ImGui::TextDisabled("%d of %d", c.hotkey_count, COUNT(c.hotkeys));

    f32 rh = opt_row_height();
    i32 remove = -1;
    if (ImGui::BeginTable("hotkeys", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingFixedFit,
                          ImVec2(0, rh * (rows + 1)))) {
        ImGui::TableSetupColumn("Keys", ImGuiTableColumnFlags_WidthFixed, dp(190));
        ImGui::TableSetupColumn("Does", ImGuiTableColumnFlags_WidthFixed, dp(190));
        ImGui::TableSetupColumn("How much", ImGuiTableColumnFlags_WidthFixed, dp(200));
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, dp(100));
        ImGui::TableHeadersRow();
        for (i32 r = 0; r < rows; r++) {
            ImGui::TableNextRow(0, rh);
            i32 i = o.hotkeys_page * rows + r;
            if (i >= c.hotkey_count) {
                ImGui::TableSetColumnIndex(0);
                ImGui::Dummy(ImVec2(0, ImGui::GetFrameHeight()));
                continue;
            }
            Hotkey *h = &c.hotkeys[i];
            ImGui::PushID(i);
            ImGui::TableSetColumnIndex(0);
            bool capturing = o.capture_slot == i + 1;
            if (capturing) ImGui::PushStyleColor(ImGuiCol_Button, rgb(COL_BLUE, 0.35f));
            const char *label = capturing ? "Press the keys" : h->keys.s[0] ? h->keys.s : "Set keys";
            ImGui::BeginDisabled(o.capture_slot && !capturing);
            if (ImGui::Button(label, ImVec2(-FLT_MIN, 0))) {
                if (capturing) capture_end(true);
                else capture_start(i, false);
            }
            ImGui::EndDisabled();
            if (capturing) ImGui::PopStyleColor();

            ImGui::TableSetColumnIndex(1);
            i32 act = action_index(h->action.s);
            if (opt_combo("##action", ACTION_LABELS, COUNT(ACTION_LABELS), &act, -FLT_MIN, 0, act < 0 ? h->action.s : 0)) {
                if (act == 0) str_set(h->action.s, "save 30s");
                else str_set(h->action.s, ACTION_COMMANDS[act]);
                log_line("options: hotkey %s does %s", h->keys.s, h->action.s);
                options_apply(APPLY_HOTKEYS);
            }

            ImGui::TableSetColumnIndex(2);
            f64 amount = 30;
            i32 unit = 0;
            bool save = act == 0;
            if (save) save_length(h->action.s, &amount, &unit);
            ImGui::BeginDisabled(!save);
            if (opt_number("##amount", &amount, 0.1, 100000, dp(70))) {
                set_save_action(h, amount, unit);
                log_line("options: hotkey %s does %s", h->keys.s, h->action.s);
                options_apply(APPLY_HOTKEYS);
            }
            ImGui::SameLine();
            if (opt_combo("##unit", LENGTH_UNITS, COUNT(LENGTH_UNITS), &unit, -FLT_MIN)) {
                set_save_action(h, amount, unit);
                log_line("options: hotkey %s does %s", h->keys.s, h->action.s);
                options_apply(APPLY_HOTKEYS);
            }
            ImGui::EndDisabled();

            ImGui::TableSetColumnIndex(3);
            ImGui::AlignTextToFramePadding();
            u32 mods, vk;
            bool same = false;
            for (i32 k = 0; k < c.hotkey_count; k++) same |= k != i && h->keys.s[0] && !_stricmp(c.hotkeys[k].keys.s, h->keys.s);
            if (!capturing && !hotkey_parse(h->keys.s, &mods, &vk)) ImGui::TextDisabled("Not a key combination");
            else if (same) ImGui::TextDisabled("Same keys as another hotkey");

            ImGui::TableSetColumnIndex(4);
            ImGui::BeginDisabled(o.capture_slot != 0);
            if (ImGui::Button("Remove", ImVec2(-FLT_MIN, 0))) remove = i;
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (remove >= 0) {
        hotkey_remove(remove);
        options_apply(APPLY_HOTKEYS);
    }
    opt_pager("hotkeys", &o.hotkeys_page, c.hotkey_count, rows);
}

// ---------------------------------------------------------------------------------------------------------------
// Recording

static const char *VENDOR_LABELS[VENDOR_COUNT] = {"NVIDIA NVENC", "AMD AMF", "Intel Quick Sync", "CPU (costs game frames)"};

// The vendor recording now, or the one that would record with these settings; -1 = not known yet.
static i32 options_vendor()
{
    const Recorder &r = g_app.recorder;
    if (r.recording) return r.vendor;
    i32 forced = vendor_parse(g_app.config.encoder.s);
    if (forced >= 0) return forced;
    if (!r.encoders_known) return -1;
    u8 works = r.encoders_ok[codec_index(g_app.config.codec.s)];
    for (i32 v = 0; v < VENDOR_COUNT; v++)
        if (works & (1 << v)) return v;
    return -1;
}

static void monitor_label(i32 i, char *out, i32 cap)
{
    const MonitorInfo &m = g_app.options.monitors[i];
    snprintf(out, cap, "%d. %s, %d\xC3\x97%d at %d Hz%s", i + 1, m.name, m.width, m.height, m.hz, m.primary ? " (main)" : "");
}

static void options_recording()
{
    OptionsUi &o = g_app.options;
    Config &c = g_app.config;
    const Recorder &rec = g_app.recorder;
    opt_title("Recording", "What goes into the buffer. Changes apply at once; the capture restarts when it needs to, "
                           "which loses about a second of footage.");

    opt_label("Record the microphone");
    if (ImGui::Checkbox("##mic", &c.mic)) {
        log_line("options: mic %s", c.mic ? "on" : "off");
        options_apply(APPLY_AUDIO);
    }

    opt_label("Monitor");
    char preview[128];
    snprintf(preview, sizeof preview, "%d. not connected", c.output_idx + 1);
    if (c.output_idx >= 0 && c.output_idx < o.monitor_count) monitor_label(c.output_idx, preview, sizeof preview);
    ImGui::SetNextItemWidth(dp(420));
    if (ImGui::BeginCombo("##monitor", preview)) {
        if (ImGui::IsWindowAppearing()) options_list_monitors(); // monitors come and go
        for (i32 i = 0; i < o.monitor_count; i++) {
            char label[128];
            monitor_label(i, label, sizeof label);
            if (ImGui::Selectable(label, i == c.output_idx) && i != c.output_idx) {
                c.output_idx = i;
                log_line("options: monitor %s", label);
                options_apply(APPLY_RECORDER);
            }
        }
        ImGui::EndCombo();
    }

    opt_label("Frame rate");
    if (opt_i32("##fps", &c.fps, 1, 240, dp(90))) {
        log_line("options: %d fps", c.fps);
        options_apply(APPLY_RECORDER);
    }
    opt_unit("frames per second");

    opt_label("Bitrate");
    if (opt_f32("##bitrate", &c.bitrate_mbps, 1, 300, dp(90))) {
        log_line("options: bitrate %g Mbps", c.bitrate_mbps);
        options_apply(APPLY_RECORDER);
    }
    opt_unit("Mbps (20 is sharp at 1440p; games can have their own)");

    static const char *codecs[CODEC_COUNT] = {"H.264: plays and edits everywhere", "HEVC: smaller files",
                                              "AV1: smallest, not for Premiere"};
    u32 no_codec = 0;
    if (rec.encoders_known)
        for (i32 i = 0; i < CODEC_COUNT; i++)
            if (!rec.encoders_ok[i]) no_codec |= 1u << i;
    opt_label("Format");
    i32 codec = codec_index(c.codec.s);
    if (opt_combo("##codec", codecs, CODEC_COUNT, &codec, dp(420), no_codec)) {
        str_set(c.codec.s, CODECS[codec]);
        log_line("options: format %s", c.codec.s);
        options_apply(APPLY_RECORDER);
    }

    // Encoders: Automatic, then each vendor; the ones whose test encode failed for this format are greyed out.
    u8 works = rec.encoders_ok[codec];
    char labels[VENDOR_COUNT + 1][64];
    const char *items[VENDOR_COUNT + 1];
    i32 first = -1;
    for (i32 v = 0; v < VENDOR_COUNT && first < 0; v++)
        if (works & (1 << v)) first = v;
    if (!rec.encoders_known) snprintf(labels[0], sizeof labels[0], "Automatic (testing the encoders)");
    else if (first < 0) snprintf(labels[0], sizeof labels[0], "Automatic (none works)");
    else snprintf(labels[0], sizeof labels[0], "Automatic: %s", VENDOR_LABELS[first]);
    u32 no_vendor = 0;
    for (i32 v = 0; v < VENDOR_COUNT; v++) {
        bool ok = !rec.encoders_known || (works & (1 << v));
        snprintf(labels[v + 1], sizeof labels[v + 1], "%s%s", VENDOR_LABELS[v], ok ? "" : " (doesn't work here)");
        if (!ok) no_vendor |= 1u << (v + 1);
    }
    for (i32 i = 0; i <= VENDOR_COUNT; i++) items[i] = labels[i];
    opt_label("Encoder");
    i32 enc = vendor_parse(c.encoder.s) + 1;
    if (opt_combo("##encoder", items, VENDOR_COUNT + 1, &enc, dp(420), no_vendor)) {
        str_set(c.encoder.s, enc == 0 ? "auto" : VENDOR_KEYS[enc - 1]);
        log_line("options: encoder %s", c.encoder.s);
        options_apply(APPLY_RECORDER);
    }

    opt_label("Recording now");
    ImGui::AlignTextToFramePadding();
    if (rec.paused) ImGui::TextDisabled("Paused");
    else if (rec.recording) ImGui::TextUnformatted(rec.encoder_used.s);
    else if (rec.last_error.s[0]) ImGui::TextDisabled("Not recording: %s", rec.last_error.s);
    else ImGui::TextDisabled("Starting");

    opt_label("Show the mouse pointer");
    if (ImGui::Checkbox("##mouse", &c.draw_mouse)) {
        log_line("options: mouse %s", c.draw_mouse ? "shown" : "hidden");
        options_apply(APPLY_RECORDER);
    }

    opt_label("ffmpeg.exe");
    char ff[MAX_PATH * 3], text[MAX_PATH * 3];
    wide_to_utf8(c.ffmpeg.s, ff, sizeof ff);
    if (opt_text("##ffmpeg", ff, dp(420), 0, "found on the PATH", text, sizeof text)) {
        char *t = trim(text);
        utf8_to_wide(t, c.ffmpeg.s, MAX_PATH);
        log_line("options: ffmpeg %s", t[0] ? t : "found on the PATH");
        options_apply(APPLY_RECORDER);
    }
}

// The games list behind Game bitrates and Sessions: every app sorted as a game, by exe. Neither page changes
// what's a game, so the order holds while it's shown.
static i32 options_games(i16 *out, i32 cap)
{
    const Config &c = g_app.config;
    i32 n = 0;
    for (i32 i = 0; i < c.app_count && n < cap; i++)
        if (c.apps[i].category == CAT_GAME) out[n++] = (i16)i;
    for (i32 i = 1; i < n; i++) // insertion sort by exe: short lists
        for (i32 k = i; k > 0 && strcmp(c.apps[out[k - 1]].exe.s, c.apps[out[k]].exe.s) > 0; k--) {
            i16 t = out[k];
            out[k] = out[k - 1];
            out[k - 1] = t;
        }
    return n;
}

static void game_name_cell(const AppInfo &a)
{
    char name[64];
    audio_display_name(g_app.config, a.exe.s, name, sizeof name);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(name);
    ImGui::SameLine();
    ImGui::TextDisabled("%s", a.exe.s);
}

static void options_game_bitrates()
{
    OptionsUi &o = g_app.options;
    Config &c = g_app.config;
    char about[256];
    snprintf(about, sizeof about,
             "A game can record at its own bitrate while it's the game being played. An empty box uses the normal "
             "bitrate (%g Mbps). Games are the apps sorted as Game under Apps.", c.bitrate_mbps);
    opt_title("Game bitrates", about);

    static i16 games[512];
    i32 n = options_games(games, COUNT(games));
    i32 rows = opt_rows_fit(opt_pager_height());
    f32 rh = opt_row_height();
    if (ImGui::BeginTable("rates", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingFixedFit,
                          ImVec2(0, rh * (rows + 1)))) {
        ImGui::TableSetupColumn("Game", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Bitrate while playing it", ImGuiTableColumnFlags_WidthFixed, dp(260));
        ImGui::TableHeadersRow();
        for (i32 r = 0; r < rows; r++) {
            ImGui::TableNextRow(0, rh);
            i32 k = o.bitrates_page * rows + r;
            ImGui::TableSetColumnIndex(0);
            if (k >= n) {
                if (n == 0 && r == 0) {
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextDisabled("No games yet. Sort apps as Game under Apps.");
                } else ImGui::Dummy(ImVec2(0, ImGui::GetFrameHeight()));
                continue;
            }
            AppInfo &a = c.apps[games[k]];
            ImGui::PushID(games[k]);
            game_name_cell(a);
            ImGui::TableSetColumnIndex(1);
            f64 v = a.bitrate_mbps;
            char hint[32];
            snprintf(hint, sizeof hint, "%g", c.bitrate_mbps);
            if (opt_number("##rate", &v, 1, 300, dp(90), true, hint)) {
                a.bitrate_mbps = (f32)v;
                if (v) log_line("options: %s records at %g Mbps", a.exe.s, v);
                else log_line("options: %s records at the normal bitrate", a.exe.s);
                options_apply(APPLY_RECORDER);
            }
            opt_unit("Mbps");
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    opt_pager("rates", &o.bitrates_page, n, rows);
}

static void options_advanced()
{
    Config &c = g_app.config;
    i32 vendor = options_vendor();
    opt_title("Advanced encoding", "Encoder settings for people who want them. Off, clipback uses its defaults. "
                                   "Settings the encoder in use doesn't support are greyed out.");

    opt_label("Use these settings");
    if (ImGui::Checkbox("##advanced", &c.advanced)) {
        log_line("options: advanced encoding %s", c.advanced ? "on" : "off");
        options_apply(APPLY_RECORDER);
    }
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    if (vendor < 0) ImGui::TextDisabled("Encoder in use: not known yet");
    else ImGui::TextDisabled("Encoder in use: %s", VENDOR_LABELS[vendor]);

    ImGui::BeginDisabled(!c.advanced);
    static const char *presets[] = {"P1: fastest", "P2", "P3", "P4: balanced", "P5", "P6", "P7: best quality, more GPU"};
    static const char *preset_keys[] = {"p1", "p2", "p3", "p4", "p5", "p6", "p7"};
    opt_label("Preset");
    i32 preset = preset_step(c.preset.s);
    if (opt_combo("##preset", presets, COUNT(presets), &preset, dp(300))) {
        str_set(c.preset.s, preset_keys[preset]);
        log_line("options: preset %s", c.preset.s);
        options_apply(APPLY_RECORDER);
    }
    static const char *preset_notes[VENDOR_COUNT] = {"", "AMF has three speeds; P1 to P7 spread over them",
                                                     "Quick Sync's veryfast to veryslow", "CPU: fast presets only"};
    opt_unit(vendor >= 0 ? preset_notes[vendor] : "");

    static const char *rcs[] = {"VBR: target bitrate, peaks allowed", "CBR: fixed bitrate",
                                "Constant quality: the bitrate is a cap"};
    static const char *rc_keys[] = {"vbr", "cbr", "cq"};
    i32 rc = !strcmp(c.rate_control.s, "cbr") ? 1 : !strcmp(c.rate_control.s, "cq") ? 2 : 0;
    opt_label("Rate control");
    if (opt_combo("##rc", rcs, COUNT(rcs), &rc, dp(300))) {
        str_set(c.rate_control.s, rc_keys[rc]);
        log_line("options: rate control %s", c.rate_control.s);
        options_apply(APPLY_RECORDER);
    }

    // What record_args (recorder.cpp) does with each setting, per vendor
    bool cq_on = rc == 2;
    bool av1 = !strcmp(c.codec.s, "av1");
    bool peak_on = rc != 1 && !(vendor == VENDOR_AMF && rc == 2) && !(vendor == VENDOR_CPU && av1 && rc != 2);
    bool aq_on = vendor < 0 || vendor == VENDOR_NVENC || vendor == VENDOR_AMF;

    opt_label("Quality level");
    ImGui::BeginDisabled(!cq_on);
    if (opt_i32("##cq", &c.cq, 0, 51, dp(90))) {
        log_line("options: quality level %d", c.cq);
        options_apply(APPLY_RECORDER);
    }
    opt_unit("0 to 51, lower is better (constant quality only)");
    ImGui::EndDisabled();

    opt_label("Peak bitrate");
    ImGui::BeginDisabled(!peak_on);
    if (opt_f32("##peak", &c.peak_factor, 1, 4, dp(90))) {
        log_line("options: peak %g times the bitrate", c.peak_factor);
        options_apply(APPLY_RECORDER);
    }
    opt_unit("\xC3\x97 the bitrate, 1 to 4 (not with CBR)");
    ImGui::EndDisabled();

    opt_label("Adaptive quantisation");
    ImGui::BeginDisabled(!aq_on);
    if (ImGui::Checkbox("##aq", &c.spatial_aq)) {
        log_line("options: adaptive quantisation %s", c.spatial_aq ? "on" : "off");
        options_apply(APPLY_RECORDER);
    }
    opt_unit("spends bits on flat areas that would band (NVENC and AMF only)");
    ImGui::EndDisabled();
    ImGui::EndDisabled();
}

// ---------------------------------------------------------------------------------------------------------------
// Sessions

static void options_sessions()
{
    OptionsUi &o = g_app.options;
    Config &c = g_app.config;
    opt_title("Sessions", "A game session lasts from when the game starts being played until it has been gone longer "
                          "than the gap. When a long enough session ends, clipback asks whether to save it.");

    opt_label("Gap that ends a session");
    if (opt_f32("##gap", &c.session_gap_min, 0.5, 600, dp(90))) {
        log_line("options: session gap %g min", c.session_gap_min);
        options_apply(0);
    }
    opt_unit("minutes");
    opt_label("Shortest session to offer");
    if (opt_f32("##minlen", &c.session_min_length_min, 0, 1440, dp(90))) {
        log_line("options: shortest session %g min", c.session_min_length_min);
        options_apply(0);
    }
    opt_unit("minutes");
    ImGui::Spacing();

    static const char *modes[] = {"Ask when it ends", "Always save it", "Never: no sessions"};
    static i16 games[512];
    i32 n = options_games(games, COUNT(games));
    i32 rows = opt_rows_fit(opt_pager_height());
    f32 rh = opt_row_height();
    if (ImGui::BeginTable("modes", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingFixedFit,
                          ImVec2(0, rh * (rows + 1)))) {
        ImGui::TableSetupColumn("Game", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Its sessions", ImGuiTableColumnFlags_WidthFixed, dp(260));
        ImGui::TableHeadersRow();
        for (i32 r = 0; r < rows; r++) {
            ImGui::TableNextRow(0, rh);
            i32 k = o.sessions_page * rows + r;
            ImGui::TableSetColumnIndex(0);
            if (k >= n) {
                if (n == 0 && r == 0) {
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextDisabled("No games yet. Sort apps as Game under Apps.");
                } else ImGui::Dummy(ImVec2(0, ImGui::GetFrameHeight()));
                continue;
            }
            AppInfo &a = c.apps[games[k]];
            ImGui::PushID(games[k]);
            game_name_cell(a);
            ImGui::TableSetColumnIndex(1);
            i32 m = a.session_mode;
            if (opt_combo("##mode", modes, COUNT(modes), &m, -FLT_MIN)) {
                a.session_mode = (SessionMode)m;
                log_line("options: %s sessions: %s", a.exe.s, session_mode_name(a.session_mode));
                options_apply(0);
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    opt_pager("modes", &o.sessions_page, n, rows);
}

// ---------------------------------------------------------------------------------------------------------------
// Storage

static bool pick_folder(const Path &start, Path *out)
{
    IFileOpenDialog *d = 0;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, 0, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return false;
    DWORD opts = 0;
    d->GetOptions(&opts);
    d->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    IShellItem *folder = 0;
    if (SUCCEEDED(SHCreateItemFromParsingName(start.s, 0, IID_PPV_ARGS(&folder)))) {
        d->SetFolder(folder);
        folder->Release();
    }
    bool ok = false;
    if (SUCCEEDED(d->Show(g_app.ui.hwnd))) {
        IShellItem *item = 0;
        if (SUCCEEDED(d->GetResult(&item))) {
            wchar_t *p = 0;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                wcscpy_s(out->s, p);
                CoTaskMemFree(p);
                ok = true;
            }
            item->Release();
        }
    }
    d->Release();
    // The dialog took the button-up, so ImGui would think the mouse is still down.
    ImGui::GetIO().AddMouseButtonEvent(0, false);
    return ok;
}

// A folder row: the path in a box (edits count when finished) and a Choose button.
static bool folder_row(const char *label, const char *id, Path *p)
{
    opt_label(label);
    char cur[MAX_PATH * 3], text[MAX_PATH * 3];
    wide_to_utf8(p->s, cur, sizeof cur);
    bool changed = false;
    ImGui::PushID(id);
    if (opt_text("##path", cur, dp(440), 0, 0, text, sizeof text) && trim(text)[0]) {
        wchar_t wide[MAX_PATH];
        utf8_to_wide(trim(text), wide, MAX_PATH);
        if (wcscmp(wide, p->s)) {
            wcscpy_s(p->s, wide);
            changed = true;
        }
    }
    ImGui::SameLine();
    Path picked = {};
    if (ImGui::Button("Choose", ImVec2(dp(90), 0)) && pick_folder(*p, &picked) && wcscmp(picked.s, p->s)) {
        *p = picked;
        changed = true;
    }
    ImGui::PopID();
    if (changed) {
        wide_to_utf8(p->s, cur, sizeof cur);
        log_line("options: %s %s", label, cur);
    }
    return changed;
}

static void options_storage()
{
    Config &c = g_app.config;
    const Recorder &rec = g_app.recorder;
    opt_title("Storage", "Where clips and the buffer go, and how much the buffer keeps. Clips and sessions are saved "
                         "in a folder per year inside the save folder.");

    if (folder_row("Save clips and sessions in", "save", &c.save_dir)) options_apply(0);
    if (folder_row("Buffer folder", "buffer", &c.buffer_dir)) options_apply(APPLY_RECORDER);

    opt_label("Keep in the buffer");
    if (opt_f32("##hours", &c.buffer_hours, 0.1, 24 * 14, dp(90))) {
        log_line("options: buffer keeps %g h", c.buffer_hours);
        options_apply(APPLY_RECORDER);
    }
    char size[32], line[128];
    bytes_text((i64)(c.buffer_hours * 3600.0 * c.bitrate_mbps / 8.0 * 1e6), size, sizeof size);
    snprintf(line, sizeof line, "hours of footage (about %s at %g Mbps)", size, c.bitrate_mbps);
    opt_unit(line);

    opt_label("Keep free on the buffer's drive");
    if (opt_f32("##free", &c.min_free_gb, 0, 100000, dp(90))) {
        log_line("options: keep %g GB free", c.min_free_gb);
        options_apply(APPLY_RECORDER);
    }
    opt_unit("GB (the oldest footage goes early rather than fill the drive)");

    opt_label("Keep share copies for");
    if (opt_f32("##share", &c.share_keep_days, 0.1, 365, dp(90))) {
        log_line("options: share copies kept %g days", c.share_keep_days);
        options_apply(0);
    }
    opt_unit("days");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    char used[32], free_text[32];
    bytes_text((i64)rec.buffer_bytes, used, sizeof used);
    bytes_text(rec.free_bytes, free_text, sizeof free_text);
    opt_label("The buffer now holds");
    ImGui::AlignTextToFramePadding();
    if (rec.oldest_us) {
        i64 mins = (now_us() - rec.oldest_us) / 60000000;
        ImGui::Text("%s, from %lld h %02lld min ago", used, mins / 60, mins % 60);
    } else {
        ImGui::TextDisabled("%s", rec.buffer_bytes ? used : "nothing yet");
    }
    opt_label("Free on the buffer's drive");
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(free_text);
}

// ---------------------------------------------------------------------------------------------------------------
// Start with Windows

static void options_startup()
{
    Config &c = g_app.config;
    opt_title("Start with Windows", "clipback starts hidden in the tray when you sign in, so the buffer is always "
                                    "recording.");
    wchar_t now_value[MAX_PATH + 8], cmd[MAX_PATH + 8];
    bool set = startup_read(now_value, COUNT(now_value));
    startup_command(cmd, COUNT(cmd));
    if (!g_app.options.startup_logged) {
        log_line("start with Windows: the Run value is %ls; on, it would be %ls", set ? now_value : L"not set", cmd);
        g_app.options.startup_logged = true;
    }

    opt_label("Start clipback with Windows");
    if (ImGui::Checkbox("##startup", &c.start_with_windows)) {
        startup_set(c.start_with_windows);
        options_apply(0);
    }
    opt_label("Windows runs at sign-in");
    ImGui::AlignTextToFramePadding();
    char u[MAX_PATH * 3];
    wide_to_utf8(now_value, u, sizeof u);
    if (!set) ImGui::TextDisabled("nothing (clipback isn't in the Run list)");
    else if (!_wcsicmp(now_value, cmd)) ImGui::TextUnformatted("this clipback");
    else ImGui::TextDisabled("%s", u);
    if (options_test_copy()) {
        ImGui::Spacing();
        ImGui::TextDisabled("This is a test copy (CLIPBACK_HOME): the setting is saved, but the Run list is left alone.");
    }
}

// ---------------------------------------------------------------------------------------------------------------

static void screen_options()
{
    OptionsUi &o = g_app.options;
    i32 frame = ImGui::GetFrameCount();
    if (!o.last_frame || frame - o.last_frame > 1) { // entered again: new apps may have arrived, monitors may have changed
        o.order_dirty = true;
        options_list_monitors();
        if (o.capture_slot) capture_end(true);
    }
    o.last_frame = frame;

    static const char *tabs[OPT_TAB_COUNT] = {"Apps", "Hotkeys", "Recording", "Game bitrates", "Advanced",
                                              "Sessions", "Storage", "Start with Windows"};
    static const bool sub[OPT_TAB_COUNT] = {false, false, false, true, true, false, false, false};
    i32 new_apps = 0;
    for (i32 i = 0; i < g_app.config.app_count; i++) new_apps += g_app.config.apps[i].category == CAT_NONE;

    ImGui::BeginChild("nav", ImVec2(dp(190), 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    for (i32 i = 0; i < OPT_TAB_COUNT; i++) {
        bool active = o.tab == i;
        char label[64];
        if (i == OPT_APPS && new_apps) snprintf(label, sizeof label, "Apps (%d new)###tab%d", new_apps, i);
        else snprintf(label, sizeof label, "%s###tab%d", tabs[i], i);
        if (sub[i]) ImGui::Indent(dp(16));
        ImGui::PushStyleColor(ImGuiCol_Button, active ? rgb(COL_PANEL2) : ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Text, active ? rgb(COL_TEXT) : rgb(COL_DIM));
        ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0, 0.5f));
        ImGui::BeginDisabled(o.capture_slot != 0 && !active); // finish setting the hotkey first
        if (ImGui::Button(label, ImVec2(-FLT_MIN, 0))) o.tab = i;
        ImGui::EndDisabled();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);
        if (sub[i]) ImGui::Unindent(dp(16));
    }
    ImGui::EndChild();
    ImGui::SameLine(0, dp(16));

    ImGui::BeginChild("page", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    switch (o.tab) {
    case OPT_APPS: options_apps(); break;
    case OPT_HOTKEYS: options_hotkeys(); break;
    case OPT_RECORDING: options_recording(); break;
    case OPT_GAME_BITRATES: options_game_bitrates(); break;
    case OPT_ADVANCED: options_advanced(); break;
    case OPT_SESSIONS: options_sessions(); break;
    case OPT_STORAGE: options_storage(); break;
    case OPT_STARTUP: options_startup(); break;
    default: o.tab = OPT_APPS; break;
    }
    ImGui::EndChild();
    if (o.capture_slot) ui_wake(); // keep drawing while waiting for keys
}
