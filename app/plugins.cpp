// Plugins: small programs that feed layers. Each is a folder <config_dir>\Plugins\<folder>\ holding plugin.txt and
// the program it names. plugin.txt has one `key = value` per line:
//
//   name  = Heart rate                    what the player sees (the folder name when left out)
//   exe   = heart.exe                     the program, in the folder; started with no arguments
//   run   = always | game                 game: only while a game is being played (always when left out)
//   layer = <id> | <kind> | <label> | ... one per layer it sends, at most PLUGIN_LAYERS:
//           value adds `<unit> | <lo>-<hi>`, input adds the codes it may send, text and spans add nothing
//
// The layer's stored name is "<folder>/<id>". The program writes lines to its stdout, each stamped when it arrives:
//
//   v <id> <number>         a Value
//   t <id> <text>           a Text or Spans line that holds until the next one; no text clears it
//   k <id> <code> down|up   an Input key or button, one of the codes in plugin.txt
//
// A line that doesn't read, names an unknown id or code, or is over PLUGIN_LINE_MAX bytes is dropped; the first of
// each kind is logged. A Value layer keeps at most one value per PLUGIN_VALUE_US. stderr goes to the log, after
// the folder name. Plugins run in Rekodr's job object, so they end with it. One that ends by itself is started
// again after 1, 4 and 16 s; after that it stays off until Rekodr starts again.

enum { PLUGIN_MAX = 8, PLUGIN_LAYERS = 4, PLUGIN_CODES = 32, PLUGIN_ID_MAX = 13, PLUGIN_FOLDER_MAX = 18,
       PLUGIN_LINE_MAX = 512, PLUGIN_RESTARTS = 3 };
static const i64 PLUGIN_VALUE_US = 100000;

enum PluginDrop : u8 { PDROP_NONE, PDROP_BAD, PDROP_ID, PDROP_CODE, PDROP_LONG, PDROP_FAST, PDROP_KINDS };
static const char *const PDROP_WHY[PDROP_KINDS] = {"", "a line that doesn't read", "an unknown id",
                                                    "a code not in plugin.txt", "a line that is too long", ""};

enum PluginState : u8 {
    PLUGIN_OFF,     // switched off, or waiting for a game
    PLUGIN_RUNNING,
    PLUGIN_WAITING, // ended; starts again at next_ms
    PLUGIN_FAILED,  // ended too often; off until Rekodr starts again
    PLUGIN_BROKEN,  // plugin.txt doesn't read, or the program isn't there: `why`
};

struct PluginLayer {
    char      id[PLUGIN_ID_MAX];
    LayerKind kind;
    char      label[LAYER_NAME_MAX];
    char      unit[LAYER_UNIT_MAX];
    f64       lo, hi;
    u32       codes[PLUGIN_CODES];
    i32       code_count;
    i32       layer;   // in g_layers, -1 until registered
    i64       last_us; // Value: when the last kept value came
};

struct Plugin {
    char        folder[PLUGIN_FOLDER_MAX];
    char        name[LAYER_NAME_MAX];
    wchar_t     exe[64];
    bool        game_only;
    PluginLayer layers[PLUGIN_LAYERS];
    i32         layer_count;
    char        why[96]; // PLUGIN_BROKEN: what is wrong

    volatile PluginState state;
    Proc   proc;
    HANDLE out_thread, err_thread;
    i32    restarts;
    u64    next_ms;
    u32    logged; // the PluginDrop kinds already logged, as bits
};

static struct {
    bool   scanned;
    i32    count;
    Plugin p[PLUGIN_MAX];
} g_plugins;

// Whether `word` is one of the |-separated words in `list`.
static bool word_list_has(const char *list, const char *word)
{
    size_t n = strlen(word);
    if (!n) return false;
    for (const char *s = list; *s;) {
        const char *bar = strchr(s, '|');
        size_t len = bar ? (size_t)(bar - s) : strlen(s);
        if (len == n && !memcmp(s, word, n)) return true;
        if (!bar) break;
        s = bar + 1;
    }
    return false;
}

// Puts `word` on the end of the |-separated `list`, or takes it out when it's there.
static void word_list_toggle(char *list, i32 cap, const char *word)
{
    char out[1024] = "";
    bool had = false;
    for (const char *s = list; *s;) {
        const char *bar = strchr(s, '|');
        size_t len = bar ? (size_t)(bar - s) : strlen(s);
        if (len == strlen(word) && !memcmp(s, word, len)) had = true;
        else if (len) {
            if (out[0]) strncat_s(out, "|", _TRUNCATE);
            strncat_s(out, sizeof out, s, len);
        }
        if (!bar) break;
        s = bar + 1;
    }
    if (!had && word[0]) {
        if (out[0]) strncat_s(out, "|", _TRUNCATE);
        strncat_s(out, word, _TRUNCATE);
    }
    if ((i32)strlen(out) < cap) strcpy_s(list, cap, out);
}

// Cuts spaces and tabs off both ends, in place.
static char *plugin_trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) s[--n] = 0;
    return s;
}

static bool plugin_id_ok(const char *id)
{
    size_t n = strlen(id);
    if (!n || n >= PLUGIN_ID_MAX) return false;
    for (size_t i = 0; i < n; i++)
        if (!isalnum((u8)id[i]) && id[i] != '_' && id[i] != '-') return false;
    return true;
}

// Reads `<id> | <kind> | <label> | ...` into `l`.
static bool plugin_layer_parse(char *value, PluginLayer *l, char *why, i32 cap)
{
    char *f[PLUGIN_CODES + 4];
    i32 n = 0;
    for (char *s = value;;) {
        char *bar = strchr(s, '|');
        if (bar) *bar = 0;
        if (n < (i32)COUNT(f)) f[n++] = plugin_trim(s);
        if (!bar) break;
        s = bar + 1;
    }
    *l = {};
    l->layer = -1;
    if (!plugin_id_ok(f[0])) { snprintf(why, cap, "layer id \"%.16s\" isn't letters and digits", f[0]); return false; }
    str_set(l->id, f[0]);
    i32 kind = n > 1 ? layer_kind_named(f[1]) : -1;
    if (kind < 0) { snprintf(why, cap, "layer %s: no kind (value, text, spans or input)", l->id); return false; }
    l->kind = (LayerKind)kind;
    layer_word_set(l->label, sizeof l->label, n > 2 && f[2][0] ? f[2] : l->id);
    if (kind == LAYER_VALUE) {
        if (n > 3) layer_word_set(l->unit, sizeof l->unit, f[3]);
        if (n > 4 && f[4][0]) {
            char *end;
            f64 lo = strtod(f[4], &end);
            while (*end == ' ') end++;
            f64 hi = *end == '-' ? strtod(end + 1, &end) : 0;
            if (*end || !(hi > lo)) { snprintf(why, cap, "layer %s: the range isn't <lo>-<hi>", l->id); return false; }
            l->lo = lo, l->hi = hi;
        }
    } else if (kind == LAYER_INPUT) {
        for (i32 i = 3; i < n; i++)
            for (char *s = f[i]; *s;) {
                if (*s == ' ' || *s == ',') { s++; continue; }
                char *end;
                unsigned long c = strtoul(s, &end, 0);
                if (end == s || (*end && *end != ' ' && *end != ',')) {
                    snprintf(why, cap, "layer %s: a code isn't a number", l->id);
                    return false;
                }
                if (l->code_count < PLUGIN_CODES) l->codes[l->code_count++] = (u32)c;
                s = end;
            }
        if (!l->code_count) { snprintf(why, cap, "layer %s: no codes", l->id); return false; }
    }
    return true;
}

// Reads plugin.txt's `text` for the plugin in `folder`. False with `p->why` when it doesn't read.
static bool plugin_manifest_parse(const char *folder, const char *text, Plugin *p)
{
    *p = {};
    str_set(p->folder, folder);
    char name[LAYER_NAME_MAX + 4];
    snprintf(name, sizeof name, "%s/x", folder);
    if (strlen(folder) >= PLUGIN_FOLDER_MAX || !layer_name_ok(name)) {
        snprintf(p->why, sizeof p->why, "the folder's name is too long");
        return false;
    }
    char line[1024];
    for (const char *s = text; *s;) {
        const char *nl = strchr(s, '\n');
        size_t len = nl ? (size_t)(nl - s) : strlen(s);
        snprintf(line, sizeof line, "%.*s", (int)len, s);
        s = nl ? nl + 1 : s + len;
        char *t = plugin_trim(line);
        if (!*t || *t == '#') continue;
        char *eq = strchr(t, '=');
        if (!eq) { snprintf(p->why, sizeof p->why, "\"%.24s\" has no =", t); return false; }
        *eq = 0;
        char *key = plugin_trim(t), *value = plugin_trim(eq + 1);
        if (!strcmp(key, "name")) layer_word_set(p->name, sizeof p->name, value);
        else if (!strcmp(key, "exe")) {
            size_t n = strlen(value);
            if (n < 5 || n >= COUNT(p->exe) || _stricmp(value + n - 4, ".exe") || strpbrk(value, "\\/:")) {
                snprintf(p->why, sizeof p->why, "exe isn't a program in the folder");
                return false;
            }
            utf8_to_wide(value, p->exe, COUNT(p->exe));
        } else if (!strcmp(key, "run")) {
            if (!strcmp(value, "game")) p->game_only = true;
            else if (strcmp(value, "always")) { snprintf(p->why, sizeof p->why, "run is always or game"); return false; }
        } else if (!strcmp(key, "layer")) {
            if (p->layer_count == PLUGIN_LAYERS) {
                snprintf(p->why, sizeof p->why, "more than %d layers", PLUGIN_LAYERS);
                return false;
            }
            PluginLayer *l = &p->layers[p->layer_count];
            if (!plugin_layer_parse(value, l, p->why, sizeof p->why)) return false;
            for (i32 i = 0; i < p->layer_count; i++)
                if (!strcmp(p->layers[i].id, l->id)) {
                    snprintf(p->why, sizeof p->why, "layer %s is there twice", l->id);
                    return false;
                }
            p->layer_count++;
        }
        // Other keys are ignored.
    }
    if (!p->exe[0]) { snprintf(p->why, sizeof p->why, "no exe"); return false; }
    if (!p->layer_count) { snprintf(p->why, sizeof p->why, "no layers"); return false; }
    if (!p->name[0]) layer_word_set(p->name, sizeof p->name, folder);
    return true;
}

// Registers the plugin's layers in g_layers. False if one couldn't be (the store is full, or a layer of another
// kind has its name).
static bool plugin_register(Plugin *p)
{
    bool ok = true;
    for (i32 i = 0; i < p->layer_count; i++) {
        PluginLayer &l = p->layers[i];
        char name[LAYER_NAME_MAX];
        snprintf(name, sizeof name, "%s/%s", p->folder, l.id);
        l.layer = layer_register(name, l.kind, l.label, l.unit, l.lo, l.hi);
        ok &= l.layer >= 0;
    }
    return ok;
}

// One line from the plugin, `len` bytes long (more than fit in `line` when it's too long), arrived at `t_us`.
static PluginDrop plugin_line(Plugin *p, const char *line, i32 len, i64 t_us)
{
    if (len > PLUGIN_LINE_MAX) return PDROP_LONG;
    char op = line[0];
    if ((op != 'v' && op != 't' && op != 'k') || line[1] != ' ') return PDROP_BAD;
    const char *id = line + 2, *sp = strchr(id, ' ');
    size_t id_len = sp ? (size_t)(sp - id) : strlen(id);
    PluginLayer *l = 0;
    for (i32 i = 0; i < p->layer_count && !l; i++)
        if (strlen(p->layers[i].id) == id_len && !memcmp(p->layers[i].id, id, id_len)) l = &p->layers[i];
    if (!l || l->layer < 0) return id_len ? PDROP_ID : PDROP_BAD;
    const char *rest = sp ? sp + 1 : "";
    if (op == 'v') {
        if (l->kind != LAYER_VALUE) return PDROP_BAD;
        char *end;
        f64 v = strtod(rest, &end);
        while (*end == ' ') end++;
        if (end == rest || *end || !isfinite(v)) return PDROP_BAD;
        if (l->last_us && t_us - l->last_us < PLUGIN_VALUE_US) return PDROP_FAST;
        l->last_us = t_us;
        return layer_value(l->layer, t_us, v) ? PDROP_NONE : PDROP_BAD;
    }
    if (op == 't') {
        if (l->kind != LAYER_TEXT && l->kind != LAYER_SPANS) return PDROP_BAD;
        return layer_hold(l->layer, t_us, rest) ? PDROP_NONE : PDROP_BAD;
    }
    if (l->kind != LAYER_INPUT) return PDROP_BAD;
    char *end;
    unsigned long code = strtoul(rest, &end, 0);
    if (end == rest || *end != ' ') return PDROP_BAD;
    const char *how = end + 1;
    bool down = !strcmp(how, "down");
    if (!down && strcmp(how, "up")) return PDROP_BAD;
    bool known = false;
    for (i32 i = 0; i < l->code_count; i++) known |= l->codes[i] == (u32)code;
    if (!known) return PDROP_CODE;
    return layer_input(l->layer, t_us, (u32)code, down) ? PDROP_NONE : PDROP_BAD;
}

// Calls `fn` with each line from `pipe` (without its line ending), until the pipe ends. A line too long to keep
// comes with its whole length and only its start.
template <typename F> static void plugin_read_lines(HANDLE pipe, F fn)
{
    char line[PLUGIN_LINE_MAX + 2], buf[4096];
    i32 n = 0, len = 0;
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(pipe, buf, sizeof buf, &got, 0) || got == 0) break;
        for (DWORD i = 0; i < got; i++) {
            char c = buf[i];
            if (c == '\n') {
                if (n && line[n - 1] == '\r') n--, len--;
                line[n] = 0;
                if (len) fn(line, len);
                n = len = 0;
            } else {
                if (n < PLUGIN_LINE_MAX + 1) line[n++] = c;
                len++;
            }
        }
    }
    if (len) { line[n] = 0; fn(line, len); }
}

static DWORD WINAPI plugin_out_thread(void *arg)
{
    Plugin *p = (Plugin *)arg;
    plugin_read_lines(p->proc.out, [&](const char *line, i32 len) {
        PluginDrop d = plugin_line(p, line, len, now_us());
        if (d != PDROP_NONE && d != PDROP_FAST && !(p->logged & (1u << d))) {
            p->logged |= 1u << d;
            log_line("plugin %s: dropped %s: %.80s", p->folder, PDROP_WHY[d], line);
        }
    });
    return 0;
}

static DWORD WINAPI plugin_err_thread(void *arg)
{
    Plugin *p = (Plugin *)arg;
    plugin_read_lines(p->proc.err, [&](const char *line, i32) { log_line("%s: %.400s", p->folder, line); });
    return 0;
}

static Path plugins_dir() { return path_join(g_app.paths.config_dir, L"Plugins"); }

// Finds the plugins, once.
static void plugins_scan()
{
    g_plugins.scanned = true;
    Path dir = plugins_dir();
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW(path_join(dir, L"*").s, &fd);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
        if (g_plugins.count == PLUGIN_MAX) { log_line("plugins: more than %d, the rest are left out", PLUGIN_MAX); break; }
        Plugin *p = &g_plugins.p[g_plugins.count];
        char folder[MAX_PATH];
        wide_to_utf8(fd.cFileName, folder, sizeof folder);
        Path pd = path_join(dir, fd.cFileName);
        char *text = read_file(path_join(pd, L"plugin.txt").s, 0);
        if (!text) continue; // a folder without plugin.txt isn't a plugin
        bool ok = plugin_manifest_parse(folder, text, p);
        free(text);
        if (ok && GetFileAttributesW(path_join(pd, p->exe).s) == INVALID_FILE_ATTRIBUTES) {
            ok = false;
            snprintf(p->why, sizeof p->why, "its program isn't in the folder");
        }
        p->state = ok ? PLUGIN_OFF : PLUGIN_BROKEN;
        if (!ok) {
            str_set(p->folder, folder);
            if (!p->name[0]) layer_word_set(p->name, sizeof p->name, folder);
            log_line("plugin %s: %s", folder, p->why);
        }
        g_plugins.count++;
    } while (FindNextFileW(find, &fd));
    FindClose(find);
}

// Waits for the plugin's readers to finish (its pipes end with it) and closes everything.
static void plugin_reap(Plugin *p)
{
    HANDLE threads[2] = {p->out_thread, p->err_thread};
    for (HANDLE t : threads) {
        if (!t) continue;
        // A program the plugin started may still hold the pipe; then the read is cut off.
        if (WaitForSingleObject(t, 1000) != WAIT_OBJECT_0) {
            CancelSynchronousIo(t);
            WaitForSingleObject(t, 1000);
        }
        CloseHandle(t);
    }
    p->out_thread = p->err_thread = 0;
    proc_close(&p->proc);
}

static void plugin_start(Plugin *p)
{
    if (!plugin_register(p)) {
        p->state = PLUGIN_BROKEN;
        snprintf(p->why, sizeof p->why, "its layers don't fit");
        log_line("plugin %s: %s", p->folder, p->why);
        return;
    }
    wchar_t folder[64];
    utf8_to_wide(p->folder, folder, COUNT(folder));
    Path exe = path_join(path_join(plugins_dir(), folder), p->exe);
    if (!proc_start(exe.s, 0, PROC_STDOUT | PROC_STDERR, &p->proc)) {
        p->state = PLUGIN_FAILED;
        return;
    }
    for (i32 i = 0; i < p->layer_count; i++) p->layers[i].last_us = 0;
    p->out_thread = thread_start(plugin_out_thread, p);
    p->err_thread = thread_start(plugin_err_thread, p);
    p->state = PLUGIN_RUNNING;
    log_line("plugin %s: started", p->folder);
}

// Starts and stops plugins as they're switched and as games come and go; main thread, once a second.
static void plugins_tick()
{
    if (!g_plugins.scanned) plugins_scan();
    bool game = g_app.audio.game.s[0] != 0;
    u64 now = GetTickCount64();
    for (i32 i = 0; i < g_plugins.count; i++) {
        Plugin *p = &g_plugins.p[i];
        if (p->state == PLUGIN_BROKEN || p->state == PLUGIN_FAILED) continue;
        bool want = !word_list_has(g_app.config.plugins_off, p->folder) && (!p->game_only || game);
        if (p->state == PLUGIN_RUNNING) {
            if (!want) {
                proc_kill(&p->proc);
                proc_wait(&p->proc, 1000);
                plugin_reap(p);
                p->state = PLUGIN_OFF;
                log_line("plugin %s: stopped", p->folder);
            } else if (proc_wait(&p->proc, 0) >= 0) {
                i32 code = proc_wait(&p->proc, 0);
                plugin_reap(p);
                if (++p->restarts > PLUGIN_RESTARTS) {
                    p->state = PLUGIN_FAILED;
                    log_line("plugin %s: ended (%d) %d times, off until Rekodr starts again", p->folder, code,
                             p->restarts);
                } else {
                    p->state = PLUGIN_WAITING;
                    p->next_ms = now + (1000ull << (2 * (p->restarts - 1))); // 1, 4, 16 s
                    log_line("plugin %s: ended (%d)", p->folder, code);
                }
            }
        } else if (!want) {
            if (p->state == PLUGIN_WAITING) p->state = PLUGIN_OFF;
        } else if (p->state == PLUGIN_OFF || now >= p->next_ms) {
            plugin_start(p);
        }
    }
}
