// rustlog.cpp: Rust's markers, from output_log.txt in the game's folder, which the game starts afresh at each
// launch. Each line the game writes begins "<UTC time>|<thread>|", as "2026-11-25T11:32:14.389Z|0x5c28|...".
//
//   Deaths  "You died: killed by <name> (<SteamID64>)", or "You died: <cause>". The killer's name and id go in the
//           marker's detail, which stays on this PC.
//   Kills   The combat log: a table the game writes each time the player prints it (the combatlog command), a
//           header "time attacker target weapon ammo area distance old_hp new_hp info ..." and a row a hit in
//           columns under it, the time being the seconds before the print. A row of the player's ("you") with
//           info "killed" is a kill, a headshot when the area is "head"; one against the player with "wounded" is
//           the player downed. The table holds the last 30 hits and leaves out the last 10 seconds', so a hit comes
//           again in later prints: it is known by its moment (within RUST_SAME_US), its kind, target and weapon.
//
// The log is read whole whenever it changes: when Rekodr starts (so a session saved before is filled in), while the
// game runs and once it closes. Marks made are kept in <local_dir>\rust.tsv, so none is made twice. Each goes in at
// its moment, into the clips and sessions saved over it too (EV_MARKER, markers_add_late).
//
// With rust_print_on_death on, a death read as it happens presses F15 RUST_PRINT_AFTER later, only while Rust is the
// window in front, so that the player's "bind f15 combatlog" prints the table. Rekodr never writes the game's files.
// A test copy (REKODR_HOME) reads <home>\Rust and presses nothing.

static const i64 RUST_EVERY = 2000000;
static const i64 RUST_SAME_US = 500000;
static const i64 RUST_PRINT_AFTER = 11000000; // past the table's hidden last 10 seconds
static const i64 RUST_LIVE_US = 15000000;     // a death this recent is happening now
enum { RUST_SEEN_MAX = 4096, RUST_COLS = 20 };

struct RustMark {
    i64  t_us; // when, on the wall clock
    u32  event; // AM_KILL, AM_DEATH or AM_DOWNED
    u64  key;   // its kind, target and weapon
    char note[32];
    char detail[256];
};

struct RustSeen { u64 key; i64 t_us; };

static struct {
    wchar_t dir[MAX_PATH]; // the game's folder; "" while off or not installed (main thread sets it)
    bool print;            // rust_print_on_death (main thread sets it)
    bool loaded;
    RustSeen seen[RUST_SEEN_MAX];
    i32 seen_count;
    i64 next, size, written, press_at;
} g_rust;

// The time a line starts with, on the wall clock; 0 if it starts with none.
static i64 rust_time(const char *s)
{
    int y, mo, d, h, mi, se, ms;
    if (sscanf(s, "%4d-%2d-%2dT%2d:%2d:%2d.%3d", &y, &mo, &d, &h, &mi, &se, &ms) != 7 || s[23] != 'Z' || s[24] != '|')
        return 0;
    SYSTEMTIME st = {(WORD)y, (WORD)mo, 0, (WORD)d, (WORD)h, (WORD)mi, (WORD)se, (WORD)ms};
    FILETIME ft;
    if (!SystemTimeToFileTime(&st, &ft)) return 0;
    return filetime_to_us(ft);
}

// A mark unless the same one is there already.
static void rust_push(RustMark *out, i32 cap, i32 *n, i64 t_us, u32 event, const char *what, const char *target,
                      const char *weapon, const char *note, const char *detail)
{
    u64 key = fnv64(target, strlen(target), fnv64(weapon, strlen(weapon), fnv64(what, strlen(what))));
    for (i32 i = 0; i < *n; i++)
        if (out[i].key == key && llabs(out[i].t_us - t_us) <= RUST_SAME_US) return;
    if (*n >= cap) return;
    RustMark &m = out[(*n)++];
    m = {};
    m.t_us = t_us;
    m.event = event;
    m.key = key;
    str_set(m.note, note);
    str_set(m.detail, detail);
}

// The combat log's columns: each header word and where it starts.
struct RustCols {
    i32  n;
    i32  at[RUST_COLS];
    char name[RUST_COLS][16];
};

static void rust_cols(const char *head, RustCols *c)
{
    c->n = 0;
    for (const char *p = head; *p && c->n < RUST_COLS;) {
        while (*p == ' ') p++;
        if (!*p) break;
        size_t w = strcspn(p, " ");
        c->at[c->n] = (i32)(p - head);
        snprintf(c->name[c->n++], sizeof c->name[0], "%.*s", (int)w, p);
        p += w;
    }
}

// A row's value in the named column, trimmed; "" when the row has none.
static void rust_field(const char *row, const RustCols &c, const char *name, char *out, i32 cap)
{
    out[0] = 0;
    i32 len = (i32)strlen(row);
    for (i32 k = 0; k < c.n; k++) {
        if (strcmp(c.name[k], name)) continue;
        i32 from = c.at[k], to = k + 1 < c.n ? c.at[k + 1] : len;
        if (from >= len) return;
        if (to > len) to = len;
        while (from < to && row[from] == ' ') from++;
        while (to > from && row[to - 1] == ' ') to--;
        snprintf(out, cap, "%.*s", to - from, row + from);
        return;
    }
}

// "You died: ..." after its "You died: ".
static void rust_death(const char *r, i64 t, RustMark *out, i32 cap, i32 *n)
{
    char detail[256] = "", note[32];
    if (!strncmp(r, "killed by ", 10)) {
        char name[128];
        str_set(name, r + 10);
        char *open = strrchr(name, '('), id[24] = "";
        size_t l = strlen(name);
        if (open && open > name && open[-1] == ' ' && l && name[l - 1] == ')') {
            name[l - 1] = 0;
            bool digits = open[1] != 0;
            for (const char *c = open + 1; *c; c++) digits &= *c >= '0' && *c <= '9';
            if (digits) {
                str_set(id, open + 1);
                open[-1] = 0;
            } else name[l - 1] = ')';
        }
        marker_detail_add(detail, "killer", name);
        marker_detail_add(detail, "steam_id", id);
        str_set(note, "Death");
    } else snprintf(note, sizeof note, "Death: %s", r);
    rust_push(out, cap, n, t, AM_DEATH, "death", "", "", note, detail);
}

// A combat log row under the header printed at `printed`.
static void rust_row(const char *row, const RustCols &c, i64 printed, RustMark *out, i32 cap, i32 *n)
{
    char attacker[64], target[64], weapon[64], area[32], distance[16], info[16];
    rust_field(row, c, "attacker", attacker, sizeof attacker);
    rust_field(row, c, "target", target, sizeof target);
    rust_field(row, c, "info", info, sizeof info);
    bool kill = !strcmp(attacker, "you") && strcmp(target, "you") && !strcmp(info, "killed");
    bool down = !strcmp(target, "you") && !strcmp(info, "wounded");
    if (!kill && !down) return;
    rust_field(row, c, "weapon", weapon, sizeof weapon);
    rust_field(row, c, "area", area, sizeof area);
    rust_field(row, c, "distance", distance, sizeof distance);
    size_t wl = strlen(weapon);
    if (wl > 7 && !strcmp(weapon + wl - 7, ".entity")) weapon[wl - 7] = 0;
    i64 t = printed - (i64)(atof(row) * 1e6);
    char detail[256] = "";
    marker_detail_add(detail, kill ? "target" : "attacker", kill ? target : attacker);
    marker_detail_add(detail, "weapon", weapon);
    marker_detail_add(detail, "area", area);
    marker_detail_add(detail, "distance", distance);
    if (kill) rust_push(out, cap, n, t, AM_KILL, "kill", target, weapon, !strcmp(area, "head") ? "Headshot" : "Kill", detail);
    else rust_push(out, cap, n, t, AM_DOWNED, "downed", attacker, weapon, "Downed", detail);
}

// The marks in a whole output_log.txt, each once, in the order read. How many.
static i32 rust_marks(const char *text, RustMark *out, i32 cap)
{
    i32 n = 0;
    RustCols cols = {};
    i64 printed = 0; // the table's print time while its rows run
    char line[1024];
    for (const char *p = text; *p;) {
        size_t len = strcspn(p, "\r\n");
        snprintf(line, sizeof line, "%.*s", (int)(len < sizeof line ? len : sizeof line - 1), p);
        p += len;
        while (*p == '\r' || *p == '\n') p++;
        i64 t = rust_time(line);
        if (t) {
            printed = 0;
            const char *msg = strchr(line + 25, '|');
            if (!msg) continue;
            msg++;
            if (!strncmp(msg, "You died: ", 10)) rust_death(msg + 10, t, out, cap, &n);
            else if (!strncmp(msg, "time ", 5) && strstr(msg, " attacker ") && strstr(msg, " target ")) {
                rust_cols(msg, &cols);
                printed = t;
            }
        } else if (printed && line[0] >= '0' && line[0] <= '9' && strchr(line, 's')) rust_row(line, cols, printed, out, cap, &n);
        else printed = 0;
    }
    return n;
}

// Whether a keys.cfg binds F15 to combatlog, as Rust writes a bind: "bind f15 <command>[;<command>...]".
static bool rust_bind_found(const char *cfg)
{
    char line[1024];
    for (const char *p = cfg; *p;) {
        size_t len = strcspn(p, "\r\n");
        snprintf(line, sizeof line, "%.*s", (int)(len < sizeof line ? len : sizeof line - 1), p);
        p += len;
        while (*p == '\r' || *p == '\n') p++;
        char *s = line;
        while (*s == ' ') s++;
        if (_strnicmp(s, "bind f15 ", 9)) continue;
        for (char *cmd = s + 9; *cmd;) {
            size_t w = strcspn(cmd, ";");
            char one[64];
            snprintf(one, sizeof one, "%.*s", (int)(w < sizeof one ? w : sizeof one - 1), cmd);
            char *a = one;
            while (*a == ' ' || *a == '"') a++;
            size_t l = strlen(a);
            while (l && (a[l - 1] == ' ' || a[l - 1] == '"')) a[--l] = 0;
            if (!_stricmp(a, "combatlog")) return true;
            cmd += w;
            if (*cmd) cmd++;
        }
    }
    return false;
}

// The game's folder: a test copy's <home>\Rust, else Steam's <library>\common\<installdir>. "" when it isn't there.
static void rust_dir(wchar_t (&out)[MAX_PATH])
{
    out[0] = 0;
    wchar_t home[MAX_PATH];
    if (is_test_copy()) {
        if (GetEnvironmentVariableW(L"REKODR_HOME", home, MAX_PATH)) swprintf(out, MAX_PATH, L"%s\\Rust", home);
    } else {
        Path lib;
        if (!steam_app_library(252490, &lib)) return;
        wchar_t p[MAX_PATH];
        swprintf(p, MAX_PATH, L"%s\\appmanifest_252490.acf", lib.s);
        char *text = read_file(p, 0), dir[MAX_PATH];
        bool ok = text && vdf_value(text, "installdir", dir, sizeof dir) && dir[0];
        free(text);
        if (!ok) return;
        wchar_t wd[MAX_PATH];
        utf8_to_wide(dir, wd, MAX_PATH);
        swprintf(out, MAX_PATH, L"%s\\common\\%s", lib.s, wd);
    }
    if (GetFileAttributesW(out) == INVALID_FILE_ATTRIBUTES) out[0] = 0;
}

// Whether the game's cfg\keys.cfg binds F15 to combatlog: 1 yes, 0 no, -1 no keys.cfg (or no game). Main thread.
static i32 rust_bind_state()
{
    if (!g_rust.dir[0]) return -1;
    wchar_t p[MAX_PATH];
    swprintf(p, MAX_PATH, L"%s\\cfg\\keys.cfg", g_rust.dir);
    char *text = read_file(p, 0);
    if (!text) return -1;
    bool found = rust_bind_found(text);
    free(text);
    return found;
}

static Path rust_seen_path() { return path_join(g_app.paths.local_dir, L"rust.tsv"); }

static void rust_seen_load()
{
    g_rust.loaded = true;
    g_rust.seen_count = 0;
    char *text = read_file(rust_seen_path().s, 0);
    if (!text) return;
    for (char *line = text; *line && g_rust.seen_count < RUST_SEEN_MAX;) {
        char *end = strchr(line, '\n');
        if (end) *end = 0;
        RustSeen s = {};
        if (sscanf(line, "%llx\t%lld", &s.key, &s.t_us) == 2) g_rust.seen[g_rust.seen_count++] = s;
        if (!end) break;
        line = end + 1;
    }
    free(text);
}

static bool rust_seen(const RustMark &m)
{
    for (i32 i = 0; i < g_rust.seen_count; i++)
        if (g_rust.seen[i].key == m.key && llabs(g_rust.seen[i].t_us - m.t_us) <= RUST_SAME_US) return true;
    return false;
}

// Adds a mark made, dropping the oldest when full.
static void rust_seen_add(const RustMark &m)
{
    if (g_rust.seen_count == RUST_SEEN_MAX) memmove(g_rust.seen, g_rust.seen + 1, sizeof g_rust.seen[0] * --g_rust.seen_count);
    g_rust.seen[g_rust.seen_count++] = {m.key, m.t_us};
}

static void rust_seen_write()
{
    FILE *f = _wfopen(rust_seen_path().s, L"wb");
    if (!f) return;
    for (i32 i = 0; i < g_rust.seen_count; i++) fprintf(f, "%016llx\t%lld\n", g_rust.seen[i].key, g_rust.seen[i].t_us);
    fclose(f);
}

// The F15 press: down and up, by virtual key and scan code.
static void rust_f15_inputs(INPUT (&in)[2])
{
    memset(in, 0, sizeof in);
    in[0].type = INPUT_KEYBOARD;
    in[0].ki.wVk = VK_F15;
    in[0].ki.wScan = (WORD)MapVirtualKeyW(VK_F15, MAPVK_VK_TO_VSC);
    in[1] = in[0];
    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
}

// Whether a press set for `at` is due at `now`: not before it, and not once it's RUST_EVERY stale.
static bool rust_press_due(i64 now, i64 at) { return at && now >= at && now - at <= RUST_EVERY; }

// Whether the window in front is the game's (RustClient.exe).
static bool rust_in_front()
{
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    HANDLE h = pid ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid) : 0;
    if (!h) return false;
    wchar_t path[MAX_PATH];
    DWORD len = MAX_PATH;
    bool ok = QueryFullProcessImageNameW(h, 0, path, &len);
    CloseHandle(h);
    const wchar_t *name = ok ? wcsrchr(path, L'\\') : 0;
    return name && !_wcsicmp(name + 1, L"RustClient.exe");
}

static void rust_post(const RustMark &m)
{
    Event e = {};
    e.kind = EV_MARKER;
    e.a = m.t_us;
    e.b = AM_EVENT_COLOURS[m.event == AM_KILL ? 0 : m.event == AM_DEATH ? 2 : 18];
    snprintf(e.text, sizeof e.text, "%s%s%s", m.note, m.detail[0] ? "\t" : "", m.detail);
    event_post(e);
}

// The worker's look at the log, and the F15 press when one is due.
static void rust_poll()
{
    u32 ev = g_am.events[AMG_RUST];
    i64 now = now_us();
    if (g_rust.press_at && rust_press_due(now, g_rust.press_at)) {
        g_rust.press_at = 0;
        if (g_rust.print && !is_test_copy() && rust_in_front()) {
            INPUT in[2];
            rust_f15_inputs(in);
            SendInput(2, in, sizeof in[0]);
            log_line("markers: Rust: F15 pressed for the combat log");
        }
    } else if (g_rust.press_at && now > g_rust.press_at) g_rust.press_at = 0;
    if (!ev || !g_rust.dir[0] || now < g_rust.next) return;
    g_rust.next = now + RUST_EVERY;
    wchar_t log[MAX_PATH];
    swprintf(log, MAX_PATH, L"%s\\output_log.txt", g_rust.dir);
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(log, GetFileExInfoStandard, &a)) return;
    i64 written = filetime_to_us(a.ftLastWriteTime), size = ((i64)a.nFileSizeHigh << 32) | a.nFileSizeLow;
    if (written == g_rust.written && size == g_rust.size) return;
    char *text = read_file(log, 0);
    if (!text) return;
    g_rust.written = written, g_rust.size = size;
    static RustMark marks[1024];
    i32 n = rust_marks(text, marks, COUNT(marks));
    free(text);
    if (!g_rust.loaded) rust_seen_load();
    i32 made = 0;
    for (i32 i = 0; i < n; i++) {
        const RustMark &m = marks[i];
        if (!(ev & m.event) || rust_seen(m)) continue;
        rust_post(m);
        rust_seen_add(m);
        made++;
        if (m.event == AM_DEATH && g_rust.print && llabs(now - m.t_us) <= RUST_LIVE_US) g_rust.press_at = m.t_us + RUST_PRINT_AFTER;
    }
    if (made) {
        rust_seen_write();
        log_line("markers: Rust: %d from its log", made);
    }
}

// The game's folder and its state for Options. From automarks_apply. Main thread.
static void rust_apply()
{
    wchar_t d[MAX_PATH] = L"";
    if (g_am.events[AMG_RUST]) rust_dir(d);
    wcscpy_s(g_rust.dir, d); // the worker reads it whole or not at all
    g_rust.print = g_app.config.rust_print_on_death;
    InterlockedExchange(&g_am.state[AMG_RUST], !g_am.events[AMG_RUST] ? AM_OFF : d[0] ? AM_READY : AM_NOT_FOUND);
}
