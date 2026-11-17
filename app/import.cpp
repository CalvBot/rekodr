// First run: bringing the settings over from the Rust version, and the TOML reader that needs.

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
    char *text = read_file(path.s, &size);
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
    char *text = read_file(rust.s, &size);
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

    text = read_file(lifelog.s, &size);
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
