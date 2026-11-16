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
            // The Rust buffer was a size: keep it one. The length it would be is set too, for switching over.
            c->buffer_by_size = true;
            c->buffer_gb = (f32)im->max_buffer_gb;
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
        a->rec.bitrate_mbps = im->rates[i].mbps;
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
// Controls. A page is built from cards (opt_card_begin/end) holding rows (opt_row_begin/end): the setting's name on
// the left with at most one short dim line under it, its control right-aligned. Rows have fixed heights and
// controls fixed widths, so nothing moves when a value changes. The opt_card, opt_row, opt_combo, opt_keycaps,
// opt_pill and opt_pager helpers are generic and could move to the kit in ui.cpp.

static const f32 OPT_PAD = 20;     // a card's padding left and right
static const f32 OPT_FIELD_W = 84; // a number box
static const f32 OPT_UNIT_W = 44;  // the unit after it ("Mbps", "hours")

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

// Text in `font` at p, cut short with an ellipsis before max_x. Fades inside BeginDisabled like the widgets.
static void opt_text_at(ImDrawList *dl, ImFont *font, ImVec2 p, u32 col, const char *text, f32 max_x)
{
    if (max_x <= p.x) return;
    ImGui::PushFont(font);
    ImGui::PushStyleColor(ImGuiCol_Text, rgb(col));
    ImGui::RenderTextEllipsis(dl, p, ImVec2(max_x, p.y + ImGui::GetFontSize()), max_x, max_x, text, 0, 0);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

// The same, ending at right_x (and starting no further left than min_x).
static void opt_text_right(ImDrawList *dl, ImFont *font, f32 right_x, f32 y, u32 col, const char *text, f32 min_x)
{
    ImGui::PushFont(font);
    f32 w = ImGui::CalcTextSize(text).x;
    ImGui::PopFont();
    opt_text_at(dl, font, ImVec2(fmaxf(min_x, right_x - w), y), col, text, right_x + 1);
}

static f32 text_w(ImFont *font, const char *text)
{
    ImGui::PushFont(font);
    f32 w = ImGui::CalcTextSize(text).x;
    ImGui::PopFont();
    return w;
}

// A small rounded tag: a count on a tab, "new" on an app. p is its left end, vertically centred.
static f32 opt_pill_w(const char *text) { return fmaxf(dp(18), text_w(g_font_small, text) + dp(12)); }
static void opt_pill(ImDrawList *dl, ImVec2 p, const char *text, u32 fill, u32 ink)
{
    f32 h = dp(18), w = opt_pill_w(text);
    dl->AddRectFilled(ImVec2(p.x, p.y - h / 2), ImVec2(p.x + w, p.y + h / 2), col32(fill), h / 2);
    f32 tw = text_w(g_font_small, text);
    opt_text_at(dl, g_font_small, ImVec2(floorf(p.x + (w - tw) / 2), floorf(p.y - g_font_small->FontSize / 2) - 1), ink,
                text, p.x + w);
}

// Cards: a raised panel as tall as what's drawn in it. The draw list is split so the panel goes under its rows.
struct OptCard { ImVec2 a; f32 w; i32 rows; };
static OptCard g_card;
static f32 g_card_max_w = FLT_MAX; // settings pages keep their cards to a readable width; lists use the whole page

static void opt_card_begin()
{
    g_card = {};
    g_card.a = ImGui::GetCursorScreenPos();
    g_card.w = fminf(ImGui::GetContentRegionAvail().x, g_card_max_w);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->ChannelsSplit(2);
    dl->ChannelsSetCurrent(1);
    ImGui::SetCursorScreenPos(ImVec2(g_card.a.x, g_card.a.y + dp(4)));
}

static void opt_card_end(f32 space_after = 18)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    f32 y = ImGui::GetCursorScreenPos().y + dp(4);
    dl->ChannelsSetCurrent(0);
    dl->AddRectFilled(g_card.a, ImVec2(g_card.a.x + g_card.w, y), col32(COL_PANEL), dp(10));
    dl->ChannelsMerge();
    ImGui::SetCursorScreenPos(ImVec2(g_card.a.x, y));
    ImGui::Dummy(ImVec2(g_card.w, dp(space_after) - ImGui::GetStyle().ItemSpacing.y));
}

// A small caption over a card: "VIDEO", "BUFFER".
static void opt_section(const char *text)
{
    ImGui::PushFont(g_font_small);
    ImGui::PushStyleColor(ImGuiCol_Text, rgb(COL_DIM));
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + dp(4));
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - dp(4));
}

// Rows: begin draws the name (and the dim line) and puts the cursor where a control `control_w` wide ends at the
// right edge, vertically centred; end moves to the next row. Rows after the first get a hairline above them
// (unless `line` is false: the empty rows that keep a paged list's size).
struct OptRow { f32 x0, x1, y, h; };
static OptRow g_row;

static void opt_row_begin(const char *name, const char *sub, f32 control_w, f32 h = 0, bool line = true)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    g_row.x0 = g_card.a.x + dp(OPT_PAD);
    g_row.x1 = g_card.a.x + g_card.w - dp(OPT_PAD);
    g_row.y = ImGui::GetCursorScreenPos().y;
    g_row.h = h > 0 ? h : dp(sub ? 64.f : 54.f);
    if (g_card.rows++ && line) dl->AddLine(ImVec2(g_row.x0, g_row.y), ImVec2(g_row.x1, g_row.y), col32(COL_LINE));
    if (name) {
        f32 max_x = g_row.x1 - control_w - dp(20);
        f32 nh = g_font->FontSize, sh = g_font_small->FontSize, gap = dp(3);
        f32 ty = floorf(g_row.y + (g_row.h - nh - (sub ? gap + sh : 0)) / 2);
        opt_text_at(dl, g_font, ImVec2(g_row.x0, ty), COL_TEXT, name, max_x);
        if (sub) opt_text_at(dl, g_font_small, ImVec2(g_row.x0, ty + nh + gap), COL_DIM, sub, max_x);
    }
    ImGui::SetCursorScreenPos(ImVec2(g_row.x1 - control_w, g_row.y + floorf((g_row.h - ImGui::GetFrameHeight()) / 2)));
}

static void opt_row_end() { ImGui::SetCursorScreenPos(ImVec2(g_card.a.x, g_row.y + g_row.h)); }

// Where a list row's text sits: vertically centred in the row.
static f32 opt_row_text_y(ImFont *font) { return floorf(g_row.y + (g_row.h - font->FontSize) / 2); }
static f32 opt_row_frame_y() { return g_row.y + floorf((g_row.h - ImGui::GetFrameHeight()) / 2); }

static f32 opt_number_w() { return dp(OPT_FIELD_W + 8 + OPT_UNIT_W); }

// The unit after the number box just drawn, in the slot kept for it.
static void opt_unit(const char *unit)
{
    ImVec2 a = ImGui::GetItemRectMin(), z = ImGui::GetItemRectMax();
    opt_text_at(ImGui::GetWindowDrawList(), g_font, ImVec2(z.x + dp(8), floorf(a.y + (z.y - a.y - g_font->FontSize) / 2)),
                COL_DIM, unit, z.x + dp(8 + OPT_UNIT_W));
}

static bool opt_row_f32(const char *name, const char *sub, const char *id, f32 *v, f64 lo, f64 hi, const char *unit)
{
    opt_row_begin(name, sub, opt_number_w());
    bool changed = opt_f32(id, v, lo, hi, dp(OPT_FIELD_W));
    opt_unit(unit);
    opt_row_end();
    return changed;
}

static bool opt_row_i32(const char *name, const char *sub, const char *id, i32 *v, f64 lo, f64 hi, const char *unit)
{
    opt_row_begin(name, sub, opt_number_w());
    bool changed = opt_i32(id, v, lo, hi, dp(OPT_FIELD_W));
    opt_unit(unit);
    opt_row_end();
    return changed;
}

static bool opt_row_toggle(const char *name, const char *sub, const char *id, bool *v)
{
    opt_row_begin(name, sub, dp(36));
    bool changed = kit_toggle(id, v);
    opt_row_end();
    return changed;
}

// A row whose right side is a read-only value.
static void opt_row_value(const char *name, const char *sub, const char *value, u32 col = COL_TEXT)
{
    f32 w = fminf(dp(360), g_card.w * 0.5f);
    opt_row_begin(name, sub, w);
    opt_text_right(ImGui::GetWindowDrawList(), g_font, g_row.x1, opt_row_text_y(g_font), col, value, g_row.x1 - w);
    opt_row_end();
}

// Drop-downs: a filled box of fixed width with its own chevron, and a popup that can be wider than the box so each
// item fits its note (a dim word or two on the right).
static bool opt_combo_begin(const char *id, const char *preview, f32 width, f32 popup_w = 0, u32 preview_col = COL_TEXT)
{
    ImDrawList *dl = ImGui::GetWindowDrawList(); // the box's window: once open, the current window is the popup
    ImVec2 a = ImGui::GetCursorScreenPos();
    f32 h = ImGui::GetFrameHeight(), pad = ImGui::GetStyle().FramePadding.x;
    ImGui::SetNextItemWidth(width);
    ImGui::SetNextWindowSizeConstraints(ImVec2(fmaxf(width, popup_w), 0), ImVec2(FLT_MAX, FLT_MAX));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(dp(6), dp(6)));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, rgb(0x202227));
    bool open = ImGui::BeginCombo(id, "", ImGuiComboFlags_NoArrowButton);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    f32 chev = text_w(g_font_small, ICON_DOWN);
    opt_text_at(dl, g_font_small, ImVec2(a.x + width - pad - chev, floorf(a.y + (h - g_font_small->FontSize) / 2) - dp(1)),
                COL_DIM, ICON_DOWN, a.x + width);
    opt_text_at(dl, g_font, ImVec2(a.x + pad, floorf(a.y + (h - g_font->FontSize) / 2)), preview_col, preview,
                a.x + width - pad - chev - dp(6));
    if (open) {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(dp(6), dp(2)));
        ImGui::PushStyleColor(ImGuiCol_Header, rgb(0x2e3137));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, rgb(0x34373e));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, rgb(0x34373e));
    }
    return open;
}

static bool opt_combo_item(const char *label, const char *note, bool selected, bool disabled = false)
{
    ImGui::PushID(label);
    bool clicked = ImGui::Selectable("##item", selected, disabled ? ImGuiSelectableFlags_Disabled : 0, ImVec2(0, dp(30)));
    ImGui::PopID();
    ImVec2 a = ImGui::GetItemRectMin(), z = ImGui::GetItemRectMax();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    f32 ty = floorf(a.y + (z.y - a.y - g_font->FontSize) / 2);
    u32 ink = disabled ? COL_FAINT : COL_TEXT;
    opt_text_at(dl, selected ? g_font_bold : g_font, ImVec2(a.x + dp(10), ty), ink, label, z.x - dp(10));
    if (note && note[0]) opt_text_right(dl, g_font, z.x - dp(10), ty, disabled ? COL_FAINT : COL_DIM, note, a.x + dp(10));
    if (selected && ImGui::IsWindowAppearing()) ImGui::SetScrollHereY();
    return clicked && !disabled;
}

static void opt_combo_end()
{
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar();
    ImGui::EndCombo();
}

// The popup width that fits the widest label and note.
static f32 opt_combo_popup_w(const char *const *items, const char *const *notes, i32 count)
{
    f32 lw = 0, nw = 0;
    for (i32 i = 0; i < count; i++) {
        lw = fmaxf(lw, text_w(g_font_bold, items[i]));
        if (notes && notes[i]) nw = fmaxf(nw, text_w(g_font, notes[i]));
    }
    return lw + (nw > 0 ? dp(32) + nw : 0) + dp(20 + 12);
}

// A whole drop-down. Items whose bit is set in `disabled` show greyed out and can't be picked.
static bool opt_combo(const char *id, const char *const *items, const char *const *notes, i32 count, i32 *index,
                      f32 width, u32 disabled = 0, const char *preview = 0, u32 preview_col = COL_TEXT)
{
    const char *shown = preview ? preview : (*index >= 0 && *index < count ? items[*index] : "");
    bool changed = false;
    if (opt_combo_begin(id, shown, width, opt_combo_popup_w(items, notes, count), preview_col)) {
        for (i32 i = 0; i < count; i++) {
            if (opt_combo_item(items[i], notes ? notes[i] : 0, i == *index, (disabled >> i) & 1) && i != *index) {
                *index = i;
                changed = true;
            }
        }
        opt_combo_end();
    }
    return changed;
}

static bool opt_row_combo(const char *name, const char *sub, const char *id, const char *const *items,
                          const char *const *notes, i32 count, i32 *index, f32 width, u32 disabled = 0,
                          const char *preview = 0)
{
    opt_row_begin(name, sub, width);
    bool changed = opt_combo(id, items, notes, count, index, width, disabled, preview);
    opt_row_end();
    return changed;
}

// Keys as caps: "Ctrl+Shift+S" draws [Ctrl] [Shift] [S] from p (top left), h high, stopping before max_x.
static void opt_keycaps(ImDrawList *dl, ImVec2 p, f32 h, const char *keys, f32 max_x)
{
    f32 x = p.x;
    for (const char *s = keys; *s;) {
        const char *e = strchr(s, '+');
        i32 n = e ? (i32)(e - s) : (i32)strlen(s);
        char part[32];
        snprintf(part, sizeof part, "%.*s", n < 31 ? n : 31, s);
        f32 tw = text_w(g_font_bold, part), w = fmaxf(h, tw + dp(18));
        if (x + w > max_x) break;
        dl->AddRectFilled(ImVec2(x, p.y + dp(2)), ImVec2(x + w, p.y + h), col32(0x101114), dp(6)); // the cap's edge
        dl->AddRectFilled(ImVec2(x, p.y), ImVec2(x + w, p.y + h - dp(2)), col32(0x2e3137), dp(6));
        opt_text_at(dl, g_font_bold, ImVec2(floorf(x + (w - tw) / 2), floorf(p.y + (h - dp(2) - g_font_bold->FontSize) / 2)),
                    COL_TEXT, part, x + w);
        x += w + dp(6);
        s = e ? e + 1 : s + n;
    }
}

static f32 opt_pager_h() { return ImGui::GetFrameHeight() + dp(14); }

// How many rows rh high fit in what's left of the page, leaving `below` for what follows (the pager).
static i32 opt_rows_fit(f32 rh, f32 below)
{
    f32 h = ImGui::GetContentRegionAvail().y - below - dp(8); // the card's own padding
    i32 rows = (i32)(h / rh);
    return rows < 3 ? 3 : rows > 30 ? 30 : rows;
}

// A footer line: `left` dim on the left, and  ‹  2 / 5  ›  on the right, all fixed sizes. The mouse wheel over the
// page turns pages too.
static void opt_pager(const char *id, i32 *page, i32 total, i32 per_page, const char *left)
{
    i32 pages = total > 0 ? (total + per_page - 1) / per_page : 1;
    if (ImGui::IsWindowHovered() && !ImGui::IsAnyItemActive()) {
        f32 wheel = ImGui::GetIO().MouseWheel;
        if (wheel < 0) (*page)++;
        else if (wheel > 0) (*page)--;
    }
    if (*page >= pages) *page = pages - 1;
    if (*page < 0) *page = 0;
    ImGui::PushID(id);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + dp(4));
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + dp(4));
    ImGui::AlignTextToFramePadding();
    kit_dim("%s", left);
    f32 bh = ImGui::GetFrameHeight(), slot = dp(64);
    kit_align_right(2 * bh + slot);
    if (kit_icon_button(ICON_LEFT, "##prev", KIT_GHOST, *page > 0)) (*page)--;
    ImGui::SameLine(0, 0);
    char text[32];
    snprintf(text, sizeof text, "%d / %d", *page + 1, pages);
    ImVec2 a = ImGui::GetCursorScreenPos();
    f32 tw = ImGui::CalcTextSize(text).x;
    opt_text_at(ImGui::GetWindowDrawList(), g_font, ImVec2(floorf(a.x + (slot - tw) / 2), floorf(a.y + (bh - g_font->FontSize) / 2)),
                COL_DIM, text, a.x + slot);
    ImGui::Dummy(ImVec2(slot, bh));
    ImGui::SameLine(0, 0);
    if (kit_icon_button(ICON_RIGHT, "##next", KIT_GHOST, *page < pages - 1)) (*page)++;
    ImGui::PopID();
}

// A list row's column caption (inside the card's first row).
static void opt_caption(f32 x, const char *text, bool right = false)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    f32 y = opt_row_text_y(g_font_small);
    if (right) opt_text_right(dl, g_font_small, x, y, COL_FAINT, text, x - dp(200));
    else opt_text_at(dl, g_font_small, ImVec2(x, y), COL_FAINT, text, x + dp(200));
}

// A list row's hover: a soft fill across the card. Returns whether the mouse is on the row.
static bool opt_row_hover()
{
    ImVec2 a(g_card.a.x + dp(6), g_row.y + 1), z(g_card.a.x + g_card.w - dp(6), g_row.y + g_row.h);
    bool hot = ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(a, z);
    if (hot) ImGui::GetWindowDrawList()->AddRectFilled(a, z, col32(0x1e2024), dp(6));
    return hot;
}

static void opt_empty_row(const char *text)
{
    opt_text_at(ImGui::GetWindowDrawList(), g_font, ImVec2(g_row.x0, opt_row_text_y(g_font)), COL_DIM, text, g_row.x1);
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

// Each category has a colour and a short name, used for its button on every app row and its filter chip.
static const char *CATEGORY_LABELS[] = {"New", "App", "Game", "Chat", "Ignored"};
static const u32 CATEGORY_COLOURS[] = {COL_WARN, 0x5b9cff, 0x3fbf74, 0xb07cff, 0x8e939c};

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
    options_apply(APPLY_AUDIO | (cat == CAT_GAME || a->rec.bitrate_mbps ? APPLY_RECORDER : 0));
}

// ---------------------------------------------------------------------------------------------------------------
// App icons: the exe's own icon, found where "Open file location" finds the exe (a running copy, or lifelog's
// record). One is looked up per frame; an app with none found gets a letter tile until Options is entered again.

struct AppIcon {
    char exe[64];
    ID3D11ShaderResourceView *srv;
    bool tried;
};
static AppIcon g_app_icons[192];
static i32 g_app_icon_count;
static i32 g_app_icon_frame; // the frame that last looked one up

static ID3D11ShaderResourceView *app_icon_load(const Path &path)
{
    const i32 S = 48;
    HICON icon = 0;
    if (SHDefExtractIconW(path.s, 0, 0, &icon, 0, S) != S_OK || !icon) return 0;
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = S;
    bi.bmiHeader.biHeight = -S; // top down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void *bits = 0;
    HDC dc = CreateCompatibleDC(0);
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, 0, 0);
    ID3D11ShaderResourceView *srv = 0;
    if (bmp && bits) {
        HGDIOBJ old = SelectObject(dc, bmp);
        memset(bits, 0, S * S * 4);
        DrawIconEx(dc, 0, 0, icon, S, S, 0, 0, DI_NORMAL);
        SelectObject(dc, old);
        u32 *px = (u32 *)bits;
        bool alpha = false;
        for (i32 i = 0; i < S * S && !alpha; i++) alpha = (px[i] >> 24) != 0;
        if (!alpha) // an old icon without alpha: whatever was drawn is opaque
            for (i32 i = 0; i < S * S; i++)
                if (px[i] & 0xffffff) px[i] |= 0xff000000u;
        srv = lib_texture(px, S, S);
    }
    if (bmp) DeleteObject(bmp);
    DeleteDC(dc);
    DestroyIcon(icon);
    return srv;
}

static ID3D11ShaderResourceView *app_icon(const char *exe)
{
    AppIcon *slot = 0;
    for (i32 i = 0; i < g_app_icon_count; i++)
        if (str_eq(g_app_icons[i].exe, exe)) { slot = &g_app_icons[i]; break; }
    if (!slot) {
        if (g_app_icon_count >= COUNT(g_app_icons)) return 0;
        slot = &g_app_icons[g_app_icon_count++];
        *slot = {};
        str_set(slot->exe, exe);
    }
    if (slot->srv || slot->tried) return slot->srv;
    i32 frame = ImGui::GetFrameCount();
    if (frame == g_app_icon_frame) { ui_wake(); return 0; } // one per frame: the next frame does the next
    g_app_icon_frame = frame;
    slot->tried = true;
    Path p = {};
    if (app_find_path(exe, &p)) slot->srv = app_icon_load(p);
    ui_wake();
    return slot->srv;
}

// Entering Options again tries the ones that had no icon (a game may be running now).
static void app_icons_retry()
{
    for (i32 i = 0; i < g_app_icon_count; i++)
        if (!g_app_icons[i].srv) g_app_icons[i].tried = false;
}

// The icon in a square at a, or a tile with the first letter in the category's colour.
static void app_icon_draw(ImDrawList *dl, ImVec2 a, f32 size, const AppInfo &info, const char *name)
{
    ID3D11ShaderResourceView *srv = app_icon(info.exe.s);
    if (srv) {
        dl->AddImage((ImTextureID)srv, a, ImVec2(a.x + size, a.y + size));
        return;
    }
    u32 col = CATEGORY_COLOURS[info.category];
    dl->AddRectFilled(a, ImVec2(a.x + size, a.y + size), col32(col, 0.18f), dp(8));
    char letter[2] = {(char)toupper((u8)name[0]), 0};
    ImGui::PushFont(g_font_big);
    ImVec2 ls = ImGui::CalcTextSize(letter);
    dl->AddText(ImVec2(floorf(a.x + (size - ls.x) / 2), floorf(a.y + (size - ls.y) / 2)), col32(col), letter);
    ImGui::PopFont();
}

// ---------------------------------------------------------------------------------------------------------------
// A game's own recording settings (the gear on its row). Empty fields use Recording's.

static const char *PRESET_LABELS[] = {"P1", "P2", "P3", "P4", "P5", "P6", "P7"};
static const char *PRESET_NOTES[] = {"fastest", 0, 0, "balanced", 0, 0, "best quality, more GPU"};
static const char *PRESET_KEYS[] = {"p1", "p2", "p3", "p4", "p5", "p6", "p7"};
static const char *RC_LABELS[] = {"VBR", "CBR", "Constant quality"};
static const char *RC_NOTES[] = {"peaks allowed", "fixed bitrate", "the bitrate is a cap"};
static const char *RC_KEYS[] = {"vbr", "cbr", "cq"};
static const char *SESSION_LABELS[] = {"Ask when it ends", "Always save", "Never"};

static i32 rc_index(const char *rc) { return !strcmp(rc, "cbr") ? 1 : !strcmp(rc, "cq") ? 2 : 0; }

// A line of the game popup: the name on the left, the cursor at x for the control.
static void game_line(const char *name, f32 x)
{
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(name);
    ImGui::SameLine(x);
}

static void game_settings_popup()
{
    OptionsUi &o = g_app.options;
    Config &c = g_app.config;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(dp(20), dp(18)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(dp(8), dp(10)));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, rgb(0x202227));
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    bool open = ImGui::BeginPopup("game settings");
    ImGui::PopStyleColor();
    if (!open) {
        ImGui::PopStyleVar(2);
        return;
    }
    if (o.menu_app < 0 || o.menu_app >= c.app_count) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        ImGui::PopStyleVar(2);
        return;
    }
    AppInfo &a = c.apps[o.menu_app];
    RecOverride &r = a.rec;
    char name[64];
    audio_display_name(c, a.exe.s, name, sizeof name);

    // Its icon and name
    f32 is = dp(36);
    ImVec2 p = ImGui::GetCursorScreenPos();
    app_icon_draw(ImGui::GetWindowDrawList(), p, is, a, name);
    ImGui::SetCursorScreenPos(ImVec2(p.x + is + dp(12), p.y + (is - g_font_big->FontSize - g_font_small->FontSize - dp(2)) / 2));
    ImGui::PushFont(g_font_big);
    ImGui::TextUnformatted(name);
    ImGui::PopFont();
    ImGui::SetCursorScreenPos(ImVec2(p.x + is + dp(12), ImGui::GetCursorScreenPos().y - dp(8)));
    ImGui::PushFont(g_font_small);
    kit_dim("%s", a.exe.s);
    ImGui::PopFont();
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + is + dp(14)));

    f32 x = dp(150), cw = dp(220);
    bool rec_changed = false;
    char hint[48];

    ImGui::PushStyleColor(ImGuiCol_FrameBg, rgb(0x2a2c32));
    game_line("Bitrate", x);
    f64 v = r.bitrate_mbps;
    snprintf(hint, sizeof hint, "%g", c.bitrate_mbps);
    if (opt_number("##rate", &v, 1, 300, dp(OPT_FIELD_W), true, hint)) {
        r.bitrate_mbps = (f32)v;
        rec_changed = true;
    }
    opt_unit("Mbps");

    game_line("Frame rate", x);
    v = r.fps;
    snprintf(hint, sizeof hint, "%d", c.fps);
    if (opt_number("##fps", &v, 1, 240, dp(OPT_FIELD_W), true, hint)) {
        r.fps = (i32)floor(v + 0.5);
        rec_changed = true;
    }
    opt_unit("fps");

    // Drop-downs: "Default" first, which shows what the default is
    const char *items[8], *notes[8];
    char def[48];
    game_line("Preset", x);
    snprintf(def, sizeof def, "Default \xC2\xB7 %s", PRESET_LABELS[preset_step(c.preset.s)]);
    items[0] = def;
    notes[0] = 0;
    for (i32 i = 0; i < COUNT(PRESET_LABELS); i++) {
        items[i + 1] = PRESET_LABELS[i];
        notes[i + 1] = PRESET_NOTES[i];
    }
    i32 sel = r.preset.s[0] ? preset_step(r.preset.s) + 1 : 0;
    if (opt_combo("##preset", items, notes, COUNT(PRESET_LABELS) + 1, &sel, cw)) {
        str_set(r.preset.s, sel ? PRESET_KEYS[sel - 1] : "");
        rec_changed = true;
    }

    game_line("Rate control", x);
    snprintf(def, sizeof def, "Default \xC2\xB7 %s", RC_LABELS[rc_index(c.rate_control.s)]);
    items[0] = def;
    for (i32 i = 0; i < COUNT(RC_LABELS); i++) {
        items[i + 1] = RC_LABELS[i];
        notes[i + 1] = RC_NOTES[i];
    }
    sel = r.rate_control.s[0] ? rc_index(r.rate_control.s) + 1 : 0;
    if (opt_combo("##rc", items, notes, COUNT(RC_LABELS) + 1, &sel, cw)) {
        str_set(r.rate_control.s, sel ? RC_KEYS[sel - 1] : "");
        rec_changed = true;
    }

    bool cq_on = !strcmp(r.rate_control.s[0] ? r.rate_control.s : c.rate_control.s, "cq");
    ImGui::BeginDisabled(!cq_on);
    game_line("Quality level", x);
    v = r.cq;
    snprintf(hint, sizeof hint, "%d", c.cq);
    if (opt_number("##cq", &v, 1, 51, dp(OPT_FIELD_W), true, hint)) {
        r.cq = (i32)floor(v + 0.5);
        rec_changed = true;
    }
    opt_unit("0\xE2\x80\x93" "51");
    ImGui::EndDisabled();

    game_line("Sessions", x);
    i32 m = a.session_mode;
    if (opt_combo("##mode", SESSION_LABELS, 0, COUNT(SESSION_LABELS), &m, cw)) {
        a.session_mode = (SessionMode)m;
        log_line("options: %s sessions: %s", a.exe.s, session_mode_name(a.session_mode));
        options_apply(0);
    }
    ImGui::PopStyleColor();

    if (rec_changed) {
        log_line("options: %s records at %g Mbps, %d fps, preset %s, %s, quality %d (0 or empty = the default)", a.exe.s,
                 r.bitrate_mbps, r.fps, r.preset.s, r.rate_control.s, r.cq);
        options_apply(APPLY_RECORDER);
    }

    ImGui::Dummy(ImVec2(0, dp(2)));
    ImGui::PushFont(g_font_small);
    kit_dim("Empty uses Recording's. The recording restarts when this game starts.");
    ImGui::PopFont();
    f32 bw = dp(84);
    bool custom = r.bitrate_mbps || r.fps || r.preset.s[0] || r.rate_control.s[0] || r.cq;
    ImGui::SetCursorPosX(x + cw - 2 * bw - dp(8));
    if (kit_button("Reset", KIT_GHOST, ImVec2(bw, 0), custom)) {
        r = {};
        log_line("options: %s records with the default settings", a.exe.s);
        options_apply(APPLY_RECORDER);
    }
    ImGui::SameLine(0, dp(8));
    if (kit_button("Done", KIT_PRIMARY, ImVec2(bw, 0))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    ImGui::PopStyleVar(2);
}

// What a game records with, if it differs: "60 Mbps · 120 fps".
static void game_summary(const AppInfo &a, char *out, i32 cap)
{
    const RecOverride &r = a.rec;
    out[0] = 0;
    i32 n = 0;
    auto add = [&](const char *t) { n += snprintf(out + n, cap - n, "%s%s", n ? " \xC2\xB7 " : "", t); };
    char t[32];
    if (r.bitrate_mbps) { snprintf(t, sizeof t, "%g Mbps", r.bitrate_mbps); add(t); }
    if (r.fps) { snprintf(t, sizeof t, "%d fps", r.fps); add(t); }
    if (r.preset.s[0]) add(PRESET_LABELS[preset_step(r.preset.s)]);
    if (r.rate_control.s[0]) add(RC_LABELS[rc_index(r.rate_control.s)]);
    if (a.session_mode == SESSION_ALWAYS) add("sessions saved");
    else if (a.session_mode == SESSION_NEVER) add("no sessions");
}

// ---------------------------------------------------------------------------------------------------------------
// Apps

// "Add app": a small popup under its button with the exe name and its category.
static void apps_add_popup(ImVec2 at, i32 rows)
{
    OptionsUi &o = g_app.options;
    Config &c = g_app.config;
    static const char *add_cats[] = {"Game", "App", "Chat", "Ignored"};
    static const AppCategory add_cat_values[] = {CAT_GAME, CAT_APP, CAT_COMMUNICATION, CAT_IGNORED};
    ImGui::SetNextWindowPos(at, ImGuiCond_Appearing, ImVec2(1, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(dp(18), dp(16)));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, rgb(0x202227));
    bool open = ImGui::BeginPopup("add app");
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    if (!open) return;
    f32 w = dp(300);
    ImGui::PushFont(g_font_bold);
    ImGui::TextUnformatted("Add an app");
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, dp(2)));
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::PushStyleColor(ImGuiCol_FrameBg, rgb(0x2a2c32));
    ImGui::SetNextItemWidth(w);
    bool enter = ImGui::InputTextWithHint("##add", "name.exe", o.add_exe, sizeof o.add_exe, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::PopStyleColor();
    if (o.add_category < 0 || o.add_category >= COUNT(add_cats)) o.add_category = 0;
    kit_segmented("##addcat", add_cats, COUNT(add_cats), &o.add_category, (w - 3 * dp(4)) / 4);
    ImGui::Dummy(ImVec2(0, dp(2)));

    char exe[64];
    str_set(exe, o.add_exe);
    char *ex = trim(exe);
    for (char *p = ex; *p; p++) *p = (char)tolower((u8)*p);
    f32 bw = dp(84), gap = dp(8);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + w - 2 * bw - gap);
    if (kit_button("Cancel", KIT_GHOST, ImVec2(bw, 0))) ImGui::CloseCurrentPopup();
    ImGui::SameLine(0, gap);
    bool add = kit_button("Add", KIT_PRIMARY, ImVec2(bw, 0), ex[0] && c.app_count < COUNT(c.apps));
    if ((add || enter) && ex[0] && c.app_count < COUNT(c.apps)) {
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
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// Renaming an app (the right-click menu): the name it gets in files and on tracks.
static void apps_rename_popup()
{
    OptionsUi &o = g_app.options;
    Config &c = g_app.config;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(dp(18), dp(16)));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, rgb(0x202227));
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    bool open = ImGui::BeginPopup("rename app");
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    if (!open) return;
    if (o.menu_app < 0 || o.menu_app >= c.app_count) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    AppInfo &a = c.apps[o.menu_app];
    f32 w = dp(300);
    ImGui::PushFont(g_font_bold);
    ImGui::Text("Name for %s", a.exe.s);
    ImGui::PopFont();
    char hint[64];
    default_app_name(a.exe.s, hint, sizeof hint);
    if (ImGui::IsWindowAppearing()) {
        str_set(o.edit, a.name.s);
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::PushStyleColor(ImGuiCol_FrameBg, rgb(0x2a2c32));
    ImGui::SetNextItemWidth(w);
    bool enter = ImGui::InputTextWithHint("##name", hint, o.edit, 64, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::PopStyleColor();
    f32 bw = dp(84), gap = dp(8);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + w - 2 * bw - gap);
    if (kit_button("Cancel", KIT_GHOST, ImVec2(bw, 0))) ImGui::CloseCurrentPopup();
    ImGui::SameLine(0, gap);
    if (kit_button("Save", KIT_PRIMARY, ImVec2(bw, 0)) || enter) {
        char name[64];
        str_set(name, o.edit);
        for (char *p = name; *p; p++) if (*p == '|') *p = ' '; // | separates fields in settings.txt
        str_set(a.name.s, trim(name));
        log_line("options: %s is shown as \"%s\"", a.exe.s, a.name.s[0] ? a.name.s : hint);
        options_apply(APPLY_AUDIO);
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// The category buttons on an app row: four fixed-size pills, the app's own one filled in its colour.
static const AppCategory ROW_CATS[] = {CAT_GAME, CAT_APP, CAT_COMMUNICATION, CAT_IGNORED};
static f32 cat_pill_w() { return dp(74); }

static bool cat_pills(AppInfo *a, f32 x, f32 y)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    f32 w = cat_pill_w(), h = ImGui::GetFrameHeight(), gap = dp(4);
    bool changed = false;
    for (i32 i = 0; i < COUNT(ROW_CATS); i++) {
        AppCategory cat = ROW_CATS[i];
        ImVec2 p(x + i * (w + gap), y);
        ImGui::SetCursorScreenPos(p);
        ImGui::PushID(i);
        bool clicked = ImGui::InvisibleButton("##cat", ImVec2(w, h));
        ImGui::PopID();
        bool hot = ImGui::IsItemHovered(), on = a->category == cat;
        u32 col = CATEGORY_COLOURS[cat];
        if (on) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), col32(col, 0.2f), h / 2);
        else if (hot) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), col32(COL_PANEL2), h / 2);
        if (on) dl->AddRect(p, ImVec2(p.x + w, p.y + h), col32(col, 0.7f), h / 2, 0, dp(1));
        const char *label = CATEGORY_LABELS[cat];
        ImFont *font = on ? g_font_bold : g_font;
        f32 tw = text_w(font, label);
        opt_text_at(dl, font, ImVec2(floorf(p.x + (w - tw) / 2), floorf(p.y + (h - font->FontSize) / 2)),
                    on ? col : hot ? COL_TEXT : COL_DIM, label, p.x + w);
        if (clicked && !on) {
            app_set_category(a, cat);
            changed = true;
        }
    }
    return changed;
}

static void options_apps()
{
    OptionsUi &o = g_app.options;
    Config &c = g_app.config;
    if (o.order_dirty) apps_order_make();
    apps_order_extend();

    // Filter chips with counts; search and Add on the right
    static const char *chips[] = {"All", "New", "Games", "Apps", "Chat", "Ignored"};
    static const i32 chip_filter[] = {FILTER_ALL, FILTER_NEW, FILTER_GAMES, FILTER_APPS, FILTER_COMMUNICATION, FILTER_IGNORED};
    static const i32 chip_cat[] = {-1, CAT_NONE, CAT_GAME, CAT_APP, CAT_COMMUNICATION, CAT_IGNORED};
    i32 counts[6] = {c.app_count};
    for (i32 i = 0; i < c.app_count; i++)
        for (i32 k = 1; k < 6; k++) counts[k] += c.apps[i].category == chip_cat[k];
    f32 fh = ImGui::GetFrameHeight();
    for (i32 i = 0; i < COUNT(chips); i++) {
        if (i) ImGui::SameLine(0, dp(6));
        char n[16];
        snprintf(n, sizeof n, "%d", counts[i]);
        // Wide enough for three digits, so a count going up never moves the chips after it
        f32 w = ImGui::CalcTextSize(chips[i]).x + ImGui::CalcTextSize("000").x + dp(8 + 28);
        char id[32];
        snprintf(id, sizeof id, "%s##chip", chips[i]);
        if (kit_chip(id, o.filter == chip_filter[i], n, w) && o.filter != chip_filter[i]) {
            o.filter = chip_filter[i];
            o.order_dirty = true;
            o.apps_page = 0;
        }
    }
    f32 search_w = dp(220), add_w = dp(118), right_w = search_w + dp(10) + add_w;
    if (ImGui::GetContentRegionAvail().x > right_w + dp(24)) kit_align_right(right_w);
    if (kit_search("##search", o.search, sizeof o.search, "Search", search_w)) {
        o.order_dirty = true;
        o.apps_page = 0;
    }
    ImGui::SameLine(0, dp(10));
    ImVec2 add_at = ImGui::GetCursorScreenPos();
    if (kit_button(ICON_ADD "  Add app", KIT_SECONDARY, ImVec2(add_w, 0), c.app_count < COUNT(c.apps))) {
        str_set(o.add_exe, o.search);
        ImGui::OpenPopup("add app");
    }
    ImGui::Dummy(ImVec2(0, dp(2)));

    f32 rh = dp(58);
    i32 rows = opt_rows_fit(rh, opt_pager_h());
    i32 open_popup = 0; // 1 the menu, 2 a game's settings, 3 rename: opened outside the rows' ID scopes
    opt_card_begin();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    f32 x0 = g_card.a.x + dp(OPT_PAD), x1 = g_card.a.x + g_card.w - dp(OPT_PAD);
    f32 more_x = x1 - fh, gear_x = more_x - dp(4) - fh;
    f32 pills_w = COUNT(ROW_CATS) * cat_pill_w() + (COUNT(ROW_CATS) - 1) * dp(4), pills_x = gear_x - dp(16) - pills_w;
    f32 is = dp(34), text_x = x0 + is + dp(14), text_max = pills_x - dp(16);

    for (i32 r = 0; r < rows; r++) {
        i32 k = o.apps_page * rows + r;
        opt_row_begin(0, 0, 0, rh, k < o.order_count);
        if (k >= o.order_count) {
            if (o.order_count == 0 && r == 0) {
                char msg[96];
                if (o.search[0]) snprintf(msg, sizeof msg, "No match. Add %s with Add app.", o.search);
                else snprintf(msg, sizeof msg, "%s", o.filter ? "None here" : "No apps have played sound yet");
                opt_empty_row(msg);
            }
            opt_row_end();
            continue;
        }
        i32 idx = o.order[k];
        AppInfo &a = c.apps[idx];
        ImGui::PushID(idx);
        bool hot = opt_row_hover();
        f32 cy = opt_row_frame_y();

        char name[64];
        audio_display_name(c, a.exe.s, name, sizeof name);
        app_icon_draw(dl, ImVec2(x0, floorf(g_row.y + (rh - is) / 2)), is, a, name);

        // Name, with a "new" tag; under it the exe, when it was heard, and what a game does differently
        bool is_new = a.category == CAT_NONE;
        f32 nh = g_font_bold->FontSize, sh = g_font_small->FontSize, ty = floorf(g_row.y + (rh - nh - dp(3) - sh) / 2);
        f32 tag_w = is_new ? opt_pill_w("new") + dp(8) : 0;
        opt_text_at(dl, g_font_bold, ImVec2(text_x, ty), COL_TEXT, name, text_max - tag_w);
        if (is_new) {
            f32 nw = fminf(text_w(g_font_bold, name), text_max - tag_w - text_x);
            opt_pill(dl, ImVec2(text_x + nw + dp(8), ty + nh / 2 + dp(1)), "new", 0x3a2e14, COL_WARN);
        }
        char line[192], when[32], summary[96] = "";
        bool playing = app_playing(a.exe.s);
        if (playing) str_set(when, "playing now");
        else ago_text(a.last_heard_us, when, sizeof when);
        if (a.category == CAT_GAME) game_summary(a, summary, sizeof summary);
        snprintf(line, sizeof line, "%s \xC2\xB7 %s%s%s", a.exe.s, when, summary[0] ? " \xC2\xB7 " : "", summary);
        opt_text_at(dl, g_font_small, ImVec2(text_x, ty + nh + dp(3)), playing ? COL_OK : COL_DIM, line, text_max);

        cat_pills(&a, pills_x, cy);

        // The gear (games only; the slot stays for the others) and the menu
        if (a.category == CAT_GAME) {
            ImGui::SetCursorScreenPos(ImVec2(gear_x, cy));
            if (kit_icon_button(ICON_SETTINGS, "##gear", KIT_GHOST, true, summary[0] != 0)) {
                o.menu_app = idx;
                open_popup = 2;
            }
        }
        ImGui::SetCursorScreenPos(ImVec2(more_x, cy));
        bool more = kit_icon_button(ICON_MORE, "##more");
        if (more || (hot && ImGui::IsMouseClicked(ImGuiMouseButton_Right))) {
            o.menu_app = idx;
            app_find_path(a.exe.s, &o.menu_path);
            open_popup = 1;
        }
        ImGui::PopID();
        opt_row_end();
    }
    opt_card_end(10);

    if (open_popup == 1) ImGui::OpenPopup("app menu");
    lib_popup_style_push(8);
    bool menu = ImGui::BeginPopup("app menu");
    lib_popup_style_pop();
    if (menu) {
        AppInfo *a = o.menu_app >= 0 && o.menu_app < c.app_count ? &c.apps[o.menu_app] : 0;
        if (a && a->category == CAT_GAME && lib_menu_item(ICON_SETTINGS, "Recording settings")) open_popup = 2;
        if (a && lib_menu_item(ICON_RENAME, "Rename")) open_popup = 3;
        ImGui::BeginDisabled(!o.menu_path.s[0]);
        if (lib_menu_item(ICON_FOLDER, "Open file location")) reveal_file(o.menu_path);
        ImGui::EndDisabled();
        if (!o.menu_path.s[0]) {
            ImGui::PushFont(g_font_small);
            kit_dim("  Not running, and lifelog hasn't seen where it is");
            ImGui::PopFont();
        }
        ImGui::EndPopup();
    }
    if (open_popup == 2) ImGui::OpenPopup("game settings");
    if (open_popup == 3) ImGui::OpenPopup("rename app");
    game_settings_popup();
    apps_rename_popup();
    apps_add_popup(ImVec2(add_at.x + add_w, add_at.y + fh + dp(6)), rows);

    char left[48];
    snprintf(left, sizeof left, "%d app%s", o.order_count, o.order_count == 1 ? "" : "s");
    opt_pager("apps", &o.apps_page, o.order_count, rows, left);
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
    capture_keys();

    f32 fh = ImGui::GetFrameHeight(), rh = dp(54), head_h = dp(34);
    i32 rows = opt_rows_fit(rh, head_h + opt_pager_h() + ImGui::GetFrameHeightWithSpacing() + dp(2));
    f32 add_w = dp(132);
    ImGui::AlignTextToFramePadding();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + dp(4));
    kit_dim("Keys that work anywhere, even in a game");
    kit_align_right(add_w);
    if (kit_button(ICON_ADD "  Add hotkey", KIT_SECONDARY, ImVec2(add_w, 0), c.hotkey_count < COUNT(c.hotkeys) && !o.capture_slot)) {
        Hotkey *h = &c.hotkeys[c.hotkey_count++];
        *h = {};
        str_set(h->action.s, "save 30s");
        o.hotkeys_page = (c.hotkey_count - 1) / rows;
        capture_start(c.hotkey_count - 1, true);
    }
    ImGui::Dummy(ImVec2(0, dp(2)));

    i32 remove = -1;
    opt_card_begin();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    f32 x0 = g_card.a.x + dp(OPT_PAD), x1 = g_card.a.x + g_card.w - dp(OPT_PAD);
    f32 keys_x = x0 - dp(6), keys_w = dp(230);
    f32 act_x = keys_x + keys_w + dp(14), act_w = dp(170);
    f32 amt_x = act_x + act_w + dp(10), amt_w = dp(64);
    f32 unit_x = amt_x + amt_w + dp(6), unit_w = dp(108);
    f32 warn_x = unit_x + unit_w + dp(16), del_x = x1 - fh + dp(6);
    f32 pad = ImGui::GetStyle().FramePadding.x;

    opt_row_begin(0, 0, 0, head_h);
    opt_caption(x0, "KEYS");
    opt_caption(act_x + pad, "ACTION");
    opt_caption(amt_x + pad, "LENGTH");
    opt_row_end();
    for (i32 r = 0; r < rows; r++) {
        i32 i = o.hotkeys_page * rows + r;
        opt_row_begin(0, 0, 0, rh, i < c.hotkey_count);
        if (i >= c.hotkey_count) {
            if (c.hotkey_count == 0 && r == 0) opt_empty_row("No hotkeys");
            opt_row_end();
            continue;
        }
        Hotkey *h = &c.hotkeys[i];
        ImGui::PushID(i);
        bool hot = opt_row_hover();
        f32 cy = opt_row_frame_y();
        bool capturing = o.capture_slot == i + 1;

        // The keys: caps in a fixed slot; click to press new ones ("Press keys…" in the same slot)
        ImGui::SetCursorScreenPos(ImVec2(keys_x, cy));
        ImGui::BeginDisabled(o.capture_slot && !capturing);
        if (ImGui::InvisibleButton("##keys", ImVec2(keys_w, fh))) {
            if (capturing) capture_end(true);
            else capture_start(i, false);
        }
        bool slot_hot = ImGui::IsItemHovered();
        f32 cap_h = dp(26), cap_y = cy + floorf((fh - cap_h) / 2);
        if (capturing) {
            dl->AddRectFilled(ImVec2(keys_x, cy), ImVec2(keys_x + keys_w, cy + fh), col32(COL_ACCENT_SOFT), ImGui::GetStyle().FrameRounding);
            opt_text_at(dl, g_font_bold, ImVec2(x0, floorf(cy + (fh - g_font_bold->FontSize) / 2)), 0xff8a92,
                        "Press keys\xE2\x80\xA6", keys_x + keys_w);
        } else {
            if (slot_hot) dl->AddRectFilled(ImVec2(keys_x, cy), ImVec2(keys_x + keys_w, cy + fh), col32(COL_PANEL2), ImGui::GetStyle().FrameRounding);
            if (h->keys.s[0]) opt_keycaps(dl, ImVec2(x0, cap_y), cap_h, h->keys.s, keys_x + keys_w - dp(6));
            else opt_text_at(dl, g_font, ImVec2(x0, floorf(cy + (fh - g_font->FontSize) / 2)), COL_DIM, "Set keys", keys_x + keys_w);
        }
        ImGui::EndDisabled();

        ImGui::SetCursorScreenPos(ImVec2(act_x, cy));
        i32 act = action_index(h->action.s);
        if (opt_combo("##action", ACTION_LABELS, 0, COUNT(ACTION_LABELS), &act, act_w, 0, act < 0 ? h->action.s : 0)) {
            if (act == 0) str_set(h->action.s, "save 30s");
            else str_set(h->action.s, ACTION_COMMANDS[act]);
            log_line("options: hotkey %s does %s", h->keys.s, h->action.s);
            options_apply(APPLY_HOTKEYS);
        }

        f64 amount = 30;
        i32 unit = 0;
        bool save = act == 0;
        if (save) save_length(h->action.s, &amount, &unit);
        ImGui::BeginDisabled(!save);
        ImGui::SetCursorScreenPos(ImVec2(amt_x, cy));
        if (opt_number("##amount", &amount, 0.1, 100000, amt_w)) {
            set_save_action(h, amount, unit);
            log_line("options: hotkey %s does %s", h->keys.s, h->action.s);
            options_apply(APPLY_HOTKEYS);
        }
        ImGui::SetCursorScreenPos(ImVec2(unit_x, cy));
        if (opt_combo("##unit", LENGTH_UNITS, 0, COUNT(LENGTH_UNITS), &unit, unit_w)) {
            set_save_action(h, amount, unit);
            log_line("options: hotkey %s does %s", h->keys.s, h->action.s);
            options_apply(APPLY_HOTKEYS);
        }
        ImGui::EndDisabled();

        u32 mods, vk;
        bool same = false;
        for (i32 k = 0; k < c.hotkey_count; k++) same |= k != i && h->keys.s[0] && !_stricmp(c.hotkeys[k].keys.s, h->keys.s);
        const char *warn = !capturing && !hotkey_parse(h->keys.s, &mods, &vk) ? "Not a key combination"
                           : same ? "Same keys as another hotkey" : 0;
        if (warn) opt_text_at(dl, g_font, ImVec2(warn_x, opt_row_text_y(g_font)), COL_WARN, warn, del_x - dp(8));

        if (hot && !o.capture_slot) {
            ImGui::SetCursorScreenPos(ImVec2(del_x, cy));
            if (kit_icon_button(ICON_DELETE, "##remove")) remove = i;
        }
        ImGui::PopID();
        opt_row_end();
    }
    opt_card_end(10);
    if (remove >= 0) {
        hotkey_remove(remove);
        options_apply(APPLY_HOTKEYS);
    }
    char left[96];
    if (options_test_copy()) snprintf(left, sizeof left, "Test copy: the real clipback keeps the hotkeys");
    else snprintf(left, sizeof left, "%d of %d", c.hotkey_count, COUNT(c.hotkeys));
    opt_pager("hotkeys", &o.hotkeys_page, c.hotkey_count, rows, left);
}

// ---------------------------------------------------------------------------------------------------------------
// Recording: what's captured, how it's encoded, and how much the buffer keeps.

static const char *VENDOR_LABELS[VENDOR_COUNT] = {"NVIDIA NVENC", "AMD AMF", "Intel Quick Sync", "CPU"};

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
    snprintf(out, cap, "%d. %s", i + 1, g_app.options.monitors[i].name);
}

static void monitor_note(i32 i, char *out, i32 cap)
{
    const MonitorInfo &m = g_app.options.monitors[i];
    snprintf(out, cap, "%d\xC3\x97%d, %d Hz%s", m.width, m.height, m.hz, m.primary ? ", main" : "");
}

static void options_recording()
{
    OptionsUi &o = g_app.options;
    Config &c = g_app.config;
    const Recorder &rec = g_app.recorder;
    f32 wide = dp(320);

    opt_section("VIDEO");
    opt_card_begin();
    char preview[160], note[64];
    snprintf(preview, sizeof preview, "%d. not connected", c.output_idx + 1);
    if (c.output_idx >= 0 && c.output_idx < o.monitor_count) {
        monitor_label(c.output_idx, preview, sizeof preview);
        monitor_note(c.output_idx, note, sizeof note);
        size_t n = strlen(preview);
        snprintf(preview + n, sizeof preview - n, " \xC2\xB7 %s", note);
    }
    opt_row_begin("Monitor", 0, wide);
    f32 popup_w = 0;
    for (i32 i = 0; i < o.monitor_count; i++) {
        char label[96];
        monitor_label(i, label, sizeof label);
        monitor_note(i, note, sizeof note);
        popup_w = fmaxf(popup_w, text_w(g_font_bold, label) + dp(32) + text_w(g_font, note) + dp(32));
    }
    if (opt_combo_begin("##monitor", preview, wide, popup_w)) {
        if (ImGui::IsWindowAppearing()) options_list_monitors(); // monitors come and go
        for (i32 i = 0; i < o.monitor_count; i++) {
            char label[96];
            monitor_label(i, label, sizeof label);
            monitor_note(i, note, sizeof note);
            if (opt_combo_item(label, note, i == c.output_idx) && i != c.output_idx) {
                c.output_idx = i;
                log_line("options: monitor %s, %s", label, note);
                options_apply(APPLY_RECORDER);
            }
        }
        opt_combo_end();
    }
    opt_row_end();
    if (opt_row_i32("Frame rate", 0, "##fps", &c.fps, 1, 240, "fps")) {
        log_line("options: %d fps", c.fps);
        options_apply(APPLY_RECORDER);
    }
    if (opt_row_toggle("Show the mouse pointer", 0, "##mouse", &c.draw_mouse)) {
        log_line("options: mouse %s", c.draw_mouse ? "shown" : "hidden");
        options_apply(APPLY_RECORDER);
    }
    opt_card_end();

    opt_section("BITRATE");
    opt_card_begin();
    if (opt_row_f32("While playing a game", "A game can have its own: the gear on its row under Apps", "##bitrate",
                    &c.bitrate_mbps, 1, 300, "Mbps")) {
        log_line("options: game bitrate %g Mbps", c.bitrate_mbps);
        options_apply(APPLY_RECORDER);
    }
    if (opt_row_f32("On the desktop", "While no game is being played", "##desktop", &c.desktop_bitrate_mbps, 1, 300, "Mbps")) {
        log_line("options: desktop bitrate %g Mbps", c.desktop_bitrate_mbps);
        options_apply(APPLY_RECORDER);
    }
    opt_card_end();

    opt_section("ENCODER");
    opt_card_begin();
    static const char *codecs[CODEC_COUNT] = {"H.264", "HEVC", "AV1"};
    static const char *codec_notes[CODEC_COUNT] = {"plays everywhere", "smaller files", "smallest, not for Premiere"};
    u32 no_codec = 0;
    if (rec.encoders_known)
        for (i32 i = 0; i < CODEC_COUNT; i++)
            if (!rec.encoders_ok[i]) no_codec |= 1u << i;
    i32 codec = codec_index(c.codec.s);
    if (opt_row_combo("Format", 0, "##codec", codecs, codec_notes, CODEC_COUNT, &codec, wide, no_codec)) {
        str_set(c.codec.s, CODECS[codec]);
        log_line("options: format %s", c.codec.s);
        options_apply(APPLY_RECORDER);
    }

    // Encoders: Automatic, then each vendor; the ones whose test encode failed for this format are greyed out.
    u8 works = rec.encoders_ok[codec];
    i32 first = -1;
    for (i32 v = 0; v < VENDOR_COUNT && first < 0; v++)
        if (works & (1 << v)) first = v;
    const char *items[VENDOR_COUNT + 1], *notes[VENDOR_COUNT + 1];
    items[0] = "Automatic";
    notes[0] = !rec.encoders_known ? "testing\xE2\x80\xA6" : first < 0 ? "none works" : VENDOR_LABELS[first];
    u32 no_vendor = 0;
    for (i32 v = 0; v < VENDOR_COUNT; v++) {
        bool ok = !rec.encoders_known || (works & (1 << v));
        items[v + 1] = VENDOR_LABELS[v];
        notes[v + 1] = !ok ? "doesn't work here" : v == VENDOR_CPU ? "costs game frames" : 0;
        if (!ok) no_vendor |= 1u << (v + 1);
    }
    i32 enc = vendor_parse(c.encoder.s) + 1;
    char enc_preview[96];
    if (enc == 0) snprintf(enc_preview, sizeof enc_preview, "Automatic \xC2\xB7 %s", notes[0]);
    else snprintf(enc_preview, sizeof enc_preview, "%s", items[enc]);
    char status[160];
    if (rec.paused) snprintf(status, sizeof status, "Paused");
    else if (rec.recording) snprintf(status, sizeof status, "Recording with %s", rec.encoder_used.s);
    else if (rec.last_error.s[0]) snprintf(status, sizeof status, "Not recording: %s", rec.last_error.s);
    else snprintf(status, sizeof status, "Starting");
    if (opt_row_combo("Encoder", status, "##encoder", items, notes, VENDOR_COUNT + 1, &enc, wide, no_vendor, enc_preview)) {
        str_set(c.encoder.s, enc == 0 ? "auto" : VENDOR_KEYS[enc - 1]);
        log_line("options: encoder %s", c.encoder.s);
        options_apply(APPLY_RECORDER);
    }

    i32 vendor = options_vendor();
    static const char *vendor_notes[VENDOR_COUNT] = {"Speed against quality", "AMF has three speeds; P1 to P7 spread over them",
                                                     "Quick Sync's veryfast to veryslow", "CPU: fast presets only"};
    i32 preset = preset_step(c.preset.s);
    if (opt_row_combo("Preset", vendor >= 0 ? vendor_notes[vendor] : vendor_notes[0], "##preset", PRESET_LABELS, PRESET_NOTES,
                      COUNT(PRESET_LABELS), &preset, wide)) {
        str_set(c.preset.s, PRESET_KEYS[preset]);
        log_line("options: preset %s", c.preset.s);
        options_apply(APPLY_RECORDER);
    }
    i32 rc = rc_index(c.rate_control.s);
    if (opt_row_combo("Rate control", 0, "##rc", RC_LABELS, RC_NOTES, COUNT(RC_LABELS), &rc, wide)) {
        str_set(c.rate_control.s, RC_KEYS[rc]);
        log_line("options: rate control %s", c.rate_control.s);
        options_apply(APPLY_RECORDER);
    }

    // What record_args (recorder.cpp) does with each setting, per vendor
    bool cq_on = rc == 2;
    bool av1 = !strcmp(c.codec.s, "av1");
    bool peak_on = rc != 1 && !(vendor == VENDOR_AMF && rc == 2) && !(vendor == VENDOR_CPU && av1 && rc != 2);
    bool aq_on = vendor < 0 || vendor == VENDOR_NVENC || vendor == VENDOR_AMF;
    ImGui::BeginDisabled(!cq_on);
    if (opt_row_i32("Quality level", "0 to 51, lower is better. Constant quality only", "##cq", &c.cq, 0, 51, "")) {
        log_line("options: quality level %d", c.cq);
        options_apply(APPLY_RECORDER);
    }
    ImGui::EndDisabled();
    ImGui::BeginDisabled(!peak_on);
    if (opt_row_f32("Peak bitrate", "Times the bitrate, 1 to 4. Not with CBR", "##peak", &c.peak_factor, 1, 4, "\xC3\x97")) {
        log_line("options: peak %g times the bitrate", c.peak_factor);
        options_apply(APPLY_RECORDER);
    }
    ImGui::EndDisabled();
    ImGui::BeginDisabled(!aq_on);
    if (opt_row_toggle("Adaptive quantisation", "Fewer bands in flat areas. NVENC and AMF only", "##aq", &c.spatial_aq)) {
        log_line("options: adaptive quantisation %s", c.spatial_aq ? "on" : "off");
        options_apply(APPLY_RECORDER);
    }
    ImGui::EndDisabled();
    opt_card_end();

    // The buffer keeps either a length of time or an amount of space, whichever is chosen; the other is shown as
    // an estimate at the game bitrate (the highest a game records at, if one has its own).
    opt_section("BUFFER");
    opt_card_begin();
    static const char *limits[] = {"Length", "Size"};
    i32 by = c.buffer_by_size ? 1 : 0;
    opt_row_begin("Limit the buffer by", 0, dp(200));
    if (kit_segmented("##by", limits, 2, &by, dp(98))) {
        c.buffer_by_size = by == 1;
        log_line("options: buffer limited by %s", c.buffer_by_size ? "size" : "length");
        options_apply(APPLY_RECORDER);
    }
    opt_row_end();
    f32 rate = c.bitrate_mbps;
    for (i32 i = 0; i < c.app_count; i++)
        if (c.apps[i].category == CAT_GAME) rate = fmaxf(rate, c.apps[i].rec.bitrate_mbps);
    f64 bytes_per_hour = 3600.0 * rate / 8.0 * 1e6;
    char est[32], line[128];
    if (!c.buffer_by_size) {
        bytes_text((i64)(c.buffer_hours * bytes_per_hour), est, sizeof est);
        snprintf(line, sizeof line, "Up to about %s at %g Mbps", est, rate);
        if (opt_row_f32("Keep the last", line, "##hours", &c.buffer_hours, 0.1, 24 * 14, "hours")) {
            log_line("options: buffer keeps %g h", c.buffer_hours);
            options_apply(APPLY_RECORDER);
        }
    } else {
        f64 h = c.buffer_gb * 1e9 / bytes_per_hour;
        snprintf(line, sizeof line, "At least %.0f h %02.0f min at %g Mbps", floor(h), fmod(h * 60, 60), rate);
        if (opt_row_f32("Keep up to", line, "##gb", &c.buffer_gb, 1, 100000, "GB")) {
            log_line("options: buffer keeps %g GB", c.buffer_gb);
            options_apply(APPLY_RECORDER);
        }
    }
    if (opt_row_f32("Keep free on its drive", "The oldest footage goes early rather than fill the drive", "##free",
                    &c.min_free_gb, 0, 100000, "GB")) {
        log_line("options: keep %g GB free", c.min_free_gb);
        options_apply(APPLY_RECORDER);
    }
    char used[32], free_text[32], holds[96];
    bytes_text((i64)rec.buffer_bytes, used, sizeof used);
    bytes_text(rec.free_bytes, free_text, sizeof free_text);
    if (rec.oldest_us) {
        i64 mins = (now_us() - rec.oldest_us) / 60000000;
        snprintf(holds, sizeof holds, "%s, the last %lld h %02lld min", used, mins / 60, mins % 60);
    } else snprintf(holds, sizeof holds, "%s", rec.buffer_bytes ? used : "Nothing yet");
    opt_row_value("Holds now", 0, holds, rec.oldest_us ? COL_TEXT : COL_DIM);
    opt_row_value("Free on its drive", 0, free_text);
    opt_card_end();

    opt_section("AUDIO");
    opt_card_begin();
    if (opt_row_toggle("Record the microphone", "On its own track", "##mic", &c.mic)) {
        log_line("options: mic %s", c.mic ? "on" : "off");
        options_apply(APPLY_AUDIO);
    }
    opt_row_begin("ffmpeg.exe", 0, wide);
    char ff[MAX_PATH * 3], text[MAX_PATH * 3];
    wide_to_utf8(c.ffmpeg.s, ff, sizeof ff);
    if (opt_text("##ffmpeg", ff, wide, 0, "Found on the PATH", text, sizeof text)) {
        char *t = trim(text);
        utf8_to_wide(t, c.ffmpeg.s, MAX_PATH);
        log_line("options: ffmpeg %s", t[0] ? t : "found on the PATH");
        options_apply(APPLY_RECORDER);
    }
    opt_row_end();
    opt_card_end();
}

// ---------------------------------------------------------------------------------------------------------------
// General: sessions, folders, sharing, startup and the look.

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

// A folder row: the path in a box (edits count when finished) and a button that opens the folder picker.
static bool opt_row_folder(const char *name, const char *sub, const char *id, Path *p)
{
    f32 fh = ImGui::GetFrameHeight();
    f32 field_w = fmaxf(dp(220), fminf(dp(420), (g_card.w - 2 * dp(OPT_PAD)) * 0.55f));
    opt_row_begin(name, sub, field_w + dp(8) + fh);
    char cur[MAX_PATH * 3], text[MAX_PATH * 3];
    wide_to_utf8(p->s, cur, sizeof cur);
    bool changed = false;
    ImGui::PushID(id);
    if (opt_text("##path", cur, field_w, 0, 0, text, sizeof text) && trim(text)[0]) {
        wchar_t wide[MAX_PATH];
        utf8_to_wide(trim(text), wide, MAX_PATH);
        if (wcscmp(wide, p->s)) {
            wcscpy_s(p->s, wide);
            changed = true;
        }
    }
    ImGui::SameLine(0, dp(8));
    Path picked = {};
    if (kit_icon_button(ICON_FOLDER, "##choose", KIT_SECONDARY) && pick_folder(*p, &picked) && wcscmp(picked.s, p->s)) {
        *p = picked;
        changed = true;
    }
    ImGui::PopID();
    opt_row_end();
    if (changed) {
        wide_to_utf8(p->s, cur, sizeof cur);
        log_line("options: %s %s", name, cur);
    }
    return changed;
}

// The accent colours as a row of swatches; the one in use has a ring.
static bool accent_swatches(i32 *sel)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    f32 d = ImGui::GetFrameHeight(), gap = dp(8), r = d / 2 - dp(4);
    ImVec2 a = ImGui::GetCursorScreenPos();
    bool changed = false;
    for (i32 i = 0; i < ACCENT_COUNT; i++) {
        ImVec2 p(a.x + i * (d + gap), a.y);
        ImGui::SetCursorScreenPos(p);
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##accent", ImVec2(d, d)) && *sel != i) {
            *sel = i;
            changed = true;
        }
        bool hot = ImGui::IsItemHovered();
        ImGui::PopID();
        ImVec2 c(p.x + d / 2, p.y + d / 2);
        dl->AddCircleFilled(c, hot ? r + dp(1) : r, col32(ACCENTS[i].base));
        if (*sel == i) dl->AddCircle(c, d / 2 - dp(0.5f), col32(COL_TEXT), 0, dp(2));
    }
    return changed;
}

static f32 swatches_w() { return (f32)ACCENT_COUNT * ImGui::GetFrameHeight() + (f32)(ACCENT_COUNT - 1) * dp(8); }

static void options_general()
{
    Config &c = g_app.config;

    opt_section("LOOK");
    opt_card_begin();
    opt_row_begin("Accent colour", 0, swatches_w());
    if (accent_swatches(&c.accent)) {
        ui_apply_accent(c.accent);
        log_line("options: accent %s", ACCENTS[c.accent].name);
        options_apply(0);
    }
    opt_row_end();
    static const char *sizes[] = {"Small", "Medium", "Large"};
    opt_row_begin("Thumbnails in the Library", 0, 3 * dp(84) + 2 * dp(4));
    if (kit_segmented("##thumbs", sizes, 3, &c.thumb_size, dp(84))) {
        log_line("options: %s thumbnails", sizes[c.thumb_size]);
        options_apply(0);
    }
    opt_row_end();
    opt_card_end();

    opt_section("SESSIONS");
    opt_card_begin();
    if (opt_row_f32("Gap that ends a session", "How long a game can be away before its session ends", "##gap",
                    &c.session_gap_min, 0.5, 600, "min")) {
        log_line("options: session gap %g min", c.session_gap_min);
        options_apply(0);
    }
    if (opt_row_f32("Shortest session to offer", "Each game asks, always saves or never: its gear under Apps", "##minlen",
                    &c.session_min_length_min, 0, 1440, "min")) {
        log_line("options: shortest session %g min", c.session_min_length_min);
        options_apply(0);
    }
    opt_card_end();

    opt_section("FOLDERS");
    opt_card_begin();
    if (opt_row_folder("Save folder", "Clips and sessions, in a folder per year", "save", &c.save_dir)) options_apply(0);
    if (opt_row_folder("Buffer folder", 0, "buffer", &c.buffer_dir)) options_apply(APPLY_RECORDER);
    if (opt_row_f32("Keep share copies for", 0, "##share", &c.share_keep_days, 0.1, 365, "days")) {
        log_line("options: share copies kept %g days", c.share_keep_days);
        options_apply(0);
    }
    opt_card_end();

    opt_section("STARTUP");
    wchar_t now_value[MAX_PATH + 8], cmd[MAX_PATH + 8];
    bool set = startup_read(now_value, COUNT(now_value));
    startup_command(cmd, COUNT(cmd));
    if (!g_app.options.startup_logged) {
        log_line("start with Windows: the Run value is %ls; on, it would be %ls", set ? now_value : L"not set", cmd);
        g_app.options.startup_logged = true;
    }
    char u[MAX_PATH * 3];
    wide_to_utf8(now_value, u, sizeof u);
    const char *runs = !set ? "Nothing runs at sign-in now" : !_wcsicmp(now_value, cmd) ? "This clipback runs at sign-in" : u;
    if (options_test_copy()) runs = "Test copy: the Run list is left alone";
    opt_card_begin();
    if (opt_row_toggle("Start with Windows", runs, "##startup", &c.start_with_windows)) {
        startup_set(c.start_with_windows);
        options_apply(0);
    }
    opt_card_end();
}

// ---------------------------------------------------------------------------------------------------------------

static void screen_options()
{
    OptionsUi &o = g_app.options;
    i32 frame = ImGui::GetFrameCount();
    if (!o.last_frame || frame - o.last_frame > 1) { // entered again: new apps may have arrived, monitors may have changed
        o.order_dirty = true;
        options_list_monitors();
        app_icons_retry();
        if (o.capture_slot) capture_end(true);
    }
    o.last_frame = frame;

    // The sections as text tabs over a hairline, the current one underlined in the accent. Each tab's width is
    // fixed (its bold label, and room for the new-apps count on Apps), so none moves.
    static const char *names[OPT_TAB_COUNT] = {"Apps", "Recording", "Hotkeys", "General"};
    if (o.tab < 0 || o.tab >= OPT_TAB_COUNT) o.tab = OPT_APPS;
    i32 new_apps = 0;
    for (i32 i = 0; i < g_app.config.app_count; i++) new_apps += g_app.config.apps[i].category == CAT_NONE;
    char count[8];
    snprintf(count, sizeof count, new_apps > 99 ? "99+" : "%d", new_apps);
    f32 pill_room = opt_pill_w("99") + dp(8);

    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 a = ImGui::GetCursorScreenPos();
    f32 avail = ImGui::GetContentRegionAvail().x, tab_h = dp(40), gap = dp(30);
    dl->AddLine(ImVec2(a.x, a.y + tab_h - 0.5f), ImVec2(a.x + avail, a.y + tab_h - 0.5f), col32(COL_LINE));
    f32 x = a.x;
    for (i32 t = 0; t < OPT_TAB_COUNT; t++) {
        f32 w = text_w(g_font_bold, names[t]) + (t == OPT_APPS ? pill_room : 0);
        bool active = o.tab == t;
        ImGui::SetCursorScreenPos(ImVec2(x - gap / 2, a.y));
        ImGui::PushID(t);
        ImGui::BeginDisabled(o.capture_slot != 0 && !active); // finish setting the hotkey first
        if (ImGui::InvisibleButton("##tab", ImVec2(w + gap, tab_h))) o.tab = t;
        bool hot = ImGui::IsItemHovered();
        ImFont *font = active ? g_font_bold : g_font;
        f32 ty = floorf(a.y + (tab_h - dp(4) - font->FontSize) / 2);
        opt_text_at(dl, font, ImVec2(x, ty), active ? COL_TEXT : hot ? 0xd4d7dc : COL_DIM, names[t], x + w + 1);
        f32 line_w = text_w(font, names[t]);
        if (t == OPT_APPS && new_apps) {
            f32 px = x + line_w + dp(8);
            opt_pill(dl, ImVec2(px, ty + font->FontSize / 2 + dp(1)), count, COL_ACCENT, 0xffffff);
            line_w += dp(8) + opt_pill_w(count);
        }
        if (active) dl->AddRectFilled(ImVec2(x, a.y + tab_h - dp(2)), ImVec2(x + line_w, a.y + tab_h), col32(COL_ACCENT), dp(1));
        ImGui::EndDisabled();
        ImGui::PopID();
        x += w + gap;
    }
    ImGui::SetCursorScreenPos(ImVec2(a.x, a.y + tab_h));
    ImGui::Dummy(ImVec2(avail, dp(14)));

    // List pages fit their page and turn pages; the others scroll if the window is short.
    bool list = o.tab == OPT_APPS || o.tab == OPT_HOTKEYS;
    g_card_max_w = list ? FLT_MAX : dp(860);
    ImGui::BeginChild("page", ImVec2(0, 0), ImGuiChildFlags_None,
                      list ? ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse : 0);
    switch (o.tab) {
    case OPT_APPS: options_apps(); break;
    case OPT_RECORDING: options_recording(); break;
    case OPT_HOTKEYS: options_hotkeys(); break;
    case OPT_GENERAL: options_general(); break;
    default: break;
    }
    ImGui::EndChild();
    if (o.capture_slot) ui_wake(); // keep drawing while waiting for keys
}
