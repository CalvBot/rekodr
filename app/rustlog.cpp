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
//   Joins   "Loading custom map from <url>" as the player joins a server, at that moment, the server's name from
//           its welcome ("<size=45>...Welcome to</color> <color=...>Name</color></size>", tags taken out) and the map's
//           address in the detail. A join waits for the welcome until the log runs RUST_WELCOME_US past it. A
//           server without a custom map is joined at its welcome.
//   Account "SteamID: <SteamID64> (<name>)" at the game's start: the account played as, a marker kept on this PC
//           (local=1), its SteamID64 in the detail.
//
// The log is read whole whenever it changes: when Rekodr starts (so a session saved before is filled in), while the
// game runs and once it closes. Marks made are kept in <local_dir>\rust.tsv, so none is made twice. Each goes in at
// its moment, into the clips and sessions saved over it too (EV_MARKER, markers_add_late).
//
// The keys that print the table are the player's to turn on in Options (rust_binds), each on its own: Space, pressed
// often, and one Rekodr presses RUST_PRINT_AFTER after a death read as it happens, only while Rust is the window in
// front and only while that key is bound or its bind waits (F15, or a key the player bound to combatlog alone). Rekodr
// puts each bind in cfg\keys.cfg, adding combatlog to what the key does ("bind space +jump;combatlog") and changing
// nothing else in the file, and takes out only what it added when one is turned off. Rust writes keys.cfg as it quits,
// so while RustClient.exe runs the file is left alone: the change waits for it to close, and Options shows the bind
// line to paste in the game's console meanwhile. The keys Rekodr added are kept in <local_dir>\rust-binds.txt. A test
// copy (REKODR_HOME) reads and writes <home>\Rust and presses nothing.

static const i64 RUST_EVERY = 2000000;
static const i64 RUST_SAME_US = 500000;
static const i64 RUST_PRINT_AFTER = 11000000; // past the table's hidden last 10 seconds
static const i64 RUST_LIVE_US = 15000000;     // a death this recent is happening now
static const i64 RUST_WELCOME_US = 90000000;  // how long after a map's load its welcome may come
enum { RUST_SEEN_MAX = 4096, RUST_COLS = 20 };

struct RustMark {
    i64  t_us; // when, on the wall clock
    u32  event; // AM_KILL, AM_DEATH, AM_DOWNED, AM_SERVER or AM_ACCOUNT
    u64  key;   // its kind, target and weapon
    char note[64];
    char detail[256];
};

struct RustSeen { u64 key; i64 t_us; };

static struct {
    wchar_t dir[MAX_PATH]; // the game's folder; "" while off or not installed (main thread sets it)
    u32 want;              // the binds asked for, 1 << RUST_BIND_* (main thread sets it)
    u16 press_vk;          // the key pressed after a death, 0 for none (rust_binds_sync sets it)
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
    char detail[256] = "", note[64];
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

// Text with its <...> tags taken out and the spaces at its ends trimmed.
static void rust_untag(const char *s, char *out, i32 cap)
{
    i32 n = 0;
    for (; *s && n < cap - 1; s++) {
        if (*s == '<' && strchr(s, '>')) s = strchr(s, '>');
        else out[n++] = *s;
    }
    out[n] = 0;
    char *t = trim(out);
    memmove(out, t, strlen(t) + 1);
}

// A server joined at `t`, from the map at `map` ("" for none), named `server` ("" when it gave no welcome).
static void rust_join(i64 t, const char *map, const char *server, RustMark *out, i32 cap, i32 *n)
{
    char detail[256] = "";
    marker_detail_add(detail, "server", server);
    marker_detail_add(detail, "map", map);
    rust_push(out, cap, n, t, AM_SERVER, "server", map[0] ? map : server, "", "Joined server", detail);
}

// "SteamID: ..." after its "SteamID: ".
static void rust_account(const char *r, i64 t, RustMark *out, i32 cap, i32 *n)
{
    char id[24] = "", name[128] = "";
    i32 l = 0;
    while (r[l] >= '0' && r[l] <= '9' && l < 20) l++;
    if (l < 15 || r[l] != ' ' || r[l + 1] != '(') return;
    snprintf(id, sizeof id, "%.*s", l, r);
    str_set(name, r + l + 2);
    size_t nl = strlen(name);
    if (!nl || name[nl - 1] != ')') return;
    name[nl - 1] = 0;
    char detail[256] = "", note[64];
    marker_detail_add(detail, "steam_id", id);
    marker_detail_add(detail, "local", "1");
    snprintf(note, sizeof note, "Playing as %s", name);
    rust_push(out, cap, n, t, AM_ACCOUNT, "account", id, "", note, detail);
}

// The marks in a whole output_log.txt, each once, in the order read. How many.
static i32 rust_marks(const char *text, RustMark *out, i32 cap)
{
    i32 n = 0;
    RustCols cols = {};
    i64 printed = 0; // the table's print time while its rows run
    i64 join_t = 0, last_t = 0; // a map loaded at join_t waits for its welcome
    char map[256] = "";
    char line[1024];
    for (const char *p = text; *p;) {
        size_t len = strcspn(p, "\r\n");
        snprintf(line, sizeof line, "%.*s", (int)(len < sizeof line ? len : sizeof line - 1), p);
        p += len;
        while (*p == '\r' || *p == '\n') p++;
        i64 t = rust_time(line);
        if (t) {
            printed = 0;
            last_t = t;
            if (join_t && t - join_t > RUST_WELCOME_US) {
                rust_join(join_t, map, "", out, cap, &n);
                join_t = 0;
            }
            const char *msg = strchr(line + 25, '|');
            if (!msg) continue;
            msg++;
            char plain[256];
            if (!strncmp(msg, "You died: ", 10)) rust_death(msg + 10, t, out, cap, &n);
            else if (!strncmp(msg, "SteamID: ", 9)) rust_account(msg + 9, t, out, cap, &n);
            else if (!strncmp(msg, "Loading custom map from ", 24)) {
                if (join_t) rust_join(join_t, map, "", out, cap, &n);
                join_t = t;
                str_set(map, trim(line + (msg - line) + 24));
            } else if (msg[0] == '<' && (rust_untag(msg, plain, sizeof plain), !strncmp(plain, "Welcome to ", 11))) {
                char *server = trim(plain + 11);
                rust_join(join_t ? join_t : t, join_t ? map : "", server, out, cap, &n);
                join_t = 0;
            }
            else if (!strncmp(msg, "time ", 5) && strstr(msg, " attacker ") && strstr(msg, " target ")) {
                rust_cols(msg, &cols);
                printed = t;
            }
        } else if (printed && line[0] >= '0' && line[0] <= '9' && strchr(line, 's')) rust_row(line, cols, printed, out, cap, &n);
        else printed = 0;
    }
    if (join_t && last_t - join_t > RUST_WELCOME_US) rust_join(join_t, map, "", out, cap, &n);
    return n;
}

// keys.cfg as Rust writes it: a line a key, "bind <key> <command>[;<command>...]", the commands at times in quotes.
// Where a key's last bind line is in it, by offset: the line, its end, the end of its line break, its commands' span
// (trailing spaces left out), and whether one of them is combatlog. `line` is -1 when the key has none.
struct RustBindAt { i32 line, end, next, cmd, cmd_end; bool found; };

// Each command in cfg[from, to), split at ';': f(start, a, b, sep) with [a, b) the command without spaces or
// quotes around it and sep the ';' after it (or `to`).
template <typename F> static void rust_cmds(const char *cfg, i32 from, i32 to, F &&f)
{
    for (i32 s = from; s <= to;) {
        i32 e = s;
        while (e < to && cfg[e] != ';') e++;
        i32 a = s, b = e;
        while (a < b && (cfg[a] == ' ' || cfg[a] == '"')) a++;
        while (b > a && (cfg[b - 1] == ' ' || cfg[b - 1] == '"')) b--;
        f(s, a, b, e);
        s = e + 1;
    }
}

static bool rust_is_combatlog(const char *cfg, i32 a, i32 b) { return b - a == 9 && !_strnicmp(cfg + a, "combatlog", 9); }

static RustBindAt rust_bind_find(const char *cfg, const char *key)
{
    RustBindAt at = {-1, 0, 0, 0, 0, false};
    char want[32];
    i32 wl = snprintf(want, sizeof want, "bind %s ", key);
    for (i32 p = 0; cfg[p];) {
        i32 end = p + (i32)strcspn(cfg + p, "\r\n"), next = end;
        if (cfg[next] == '\r') next++;
        if (cfg[next] == '\n') next++;
        i32 s = p;
        while (cfg[s] == ' ') s++;
        if (!_strnicmp(cfg + s, want, wl)) {
            at = {p, end, next, s + wl, end, false};
            while (at.cmd_end > at.cmd && cfg[at.cmd_end - 1] == ' ') at.cmd_end--;
        }
        p = next;
    }
    if (at.line >= 0)
        rust_cmds(cfg, at.cmd, at.cmd_end, [&](i32, i32 a, i32 b, i32) { at.found |= rust_is_combatlog(cfg, a, b); });
    return at;
}

// Whether a bind's commands are only spaces and quotes.
static bool rust_cmds_empty(const char *cfg, const RustBindAt &at)
{
    for (i32 i = at.cmd; i < at.cmd_end; i++)
        if (cfg[i] != ' ' && cfg[i] != '"') return false;
    return true;
}

// The bind line that makes `key` print the combat log: the key's own commands kept and combatlog added, as "bind space
// +jump;combatlog". True when the key prints it already (the line is then the bind as it is).
static bool rust_bind_line(const char *cfg, const char *key, char *out, i32 cap)
{
    RustBindAt at = rust_bind_find(cfg, key);
    i32 n = at.cmd_end - at.cmd;
    if (at.line < 0 || rust_cmds_empty(cfg, at)) snprintf(out, cap, "bind %s combatlog", key);
    else if (at.found) snprintf(out, cap, "bind %s %.*s", key, n, cfg + at.cmd);
    else if (n >= 2 && cfg[at.cmd] == '"' && cfg[at.cmd_end - 1] == '"')
        snprintf(out, cap, "bind %s %.*s;combatlog\"", key, n - 1, cfg + at.cmd);
    else snprintf(out, cap, "bind %s %.*s;combatlog", key, n, cfg + at.cmd);
    return at.found;
}

// keys.cfg with combatlog added to `key`'s bind (on) or taken out of it, the rest of the file as it was: a new line
// goes at the end with the file's own line break, and a bind left with no commands goes. 0 when nothing changes.
static char *rust_cfg_set(const char *cfg, const char *key, bool on)
{
    RustBindAt at = rust_bind_find(cfg, key);
    if (on == at.found) return 0;
    i32 len = (i32)strlen(cfg), cap = len + 64;
    char *out = (char *)malloc((size_t)cap);
    if (!out) return 0;
    if (on && at.line < 0) {
        const char *eol = strstr(cfg, "\r\n") ? "\r\n" : "\n";
        snprintf(out, cap, "%s%sbind %s combatlog%s", cfg, len && cfg[len - 1] != '\n' ? eol : "", key, eol);
    } else if (on) {
        bool quoted = at.cmd_end - at.cmd >= 2 && cfg[at.cmd] == '"' && cfg[at.cmd_end - 1] == '"';
        i32 ins = quoted ? at.cmd_end - 1 : at.cmd_end;
        snprintf(out, cap, "%.*s%s%s", ins, cfg, rust_cmds_empty(cfg, at) ? "combatlog" : ";combatlog", cfg + ins);
    } else {
        // The first combatlog goes with the ';' before it, or the one after it when it comes first; then the next
        i32 cut = -1, cut_end = -1;
        rust_cmds(cfg, at.cmd, at.cmd_end, [&](i32 s, i32 a, i32 b, i32 sep) {
            if (cut >= 0 || !rust_is_combatlog(cfg, a, b)) return;
            if (s > at.cmd) cut = s - 1, cut_end = b;
            else if (sep < at.cmd_end) cut = a, cut_end = sep + 1;
            else cut = a, cut_end = b;
        });
        snprintf(out, cap, "%.*s%s", cut, cfg, cfg + cut_end);
        RustBindAt left = rust_bind_find(out, key);
        if (left.found) {
            char *again = rust_cfg_set(out, key, false);
            free(out);
            return again;
        }
        if (left.line >= 0 && rust_cmds_empty(out, left)) memmove(out + left.line, out + left.next, strlen(out + left.next) + 1);
    }
    return out;
}

// A key's virtual key code by its keys.cfg name (f1 to f24, a letter or a digit), 0 for others.
static u16 rust_key_vk(const char *key)
{
    if ((key[0] == 'f' || key[0] == 'F') && key[1] >= '1' && key[1] <= '9') {
        char *end;
        long n = strtol(key + 1, &end, 10);
        return !*end && n >= 1 && n <= 24 ? (u16)(VK_F1 + n - 1) : 0;
    }
    if (key[0] && !key[1] && ((key[0] >= 'a' && key[0] <= 'z') || (key[0] >= 'A' && key[0] <= 'Z'))) return (u16)toupper(key[0]);
    if (key[0] >= '0' && key[0] <= '9' && !key[1]) return (u16)key[0];
    return 0;
}

// The key Rekodr presses after a death: F15 when it prints the combat log, else a key bound to combatlog alone that it
// can press, else F15, which is the one it binds. True when that key prints it.
static bool rust_death_key(const char *cfg, char (&key)[16])
{
    str_set(key, "f15");
    if (rust_bind_find(cfg, "f15").found) return true;
    for (const char *p = cfg; *p;) {
        size_t len = strcspn(p, "\r\n");
        const char *s = p;
        p += len;
        while (*p == '\r' || *p == '\n') p++;
        while (*s == ' ') s++;
        if (_strnicmp(s, "bind ", 5)) continue;
        char k[16];
        size_t kl = strcspn(s + 5, " \r\n");
        if (kl >= sizeof k) continue;
        snprintf(k, sizeof k, "%.*s", (int)kl, s + 5);
        if (!rust_key_vk(k)) continue;
        RustBindAt at = rust_bind_find(cfg, k);
        i32 cmds = 0;
        bool only = at.found;
        rust_cmds(cfg, at.cmd, at.cmd_end, [&](i32, i32 a, i32 b, i32) {
            cmds++;
            only &= rust_is_combatlog(cfg, a, b);
        });
        if (only && cmds == 1) {
            str_set(key, k);
            return true;
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

// The binds' state, from rust_binds_sync for Options, under g_rust_lock; and the binds Rekodr added (1 << RUST_BIND_*).
static SRWLOCK g_rust_lock = SRWLOCK_INIT;
static RustBindView g_rust_view[RUST_BIND_COUNT];
static u32 g_rust_ours;
static bool g_rust_ours_loaded;

static Path rust_ours_path() { return path_join(g_app.paths.local_dir, L"rust-binds.txt"); }

static void rust_ours_load()
{
    g_rust_ours_loaded = true;
    g_rust_ours = 0;
    char *text = read_file(rust_ours_path().s, 0);
    for (i32 i = 0; text && i < RUST_BIND_COUNT; i++) {
        char w[24];
        snprintf(w, sizeof w, "%s\n", RUST_BIND_WORDS[i]);
        if (strstr(text, w)) g_rust_ours |= 1u << i;
    }
    free(text);
}

static void rust_ours_save()
{
    Path p = rust_ours_path();
    if (!g_rust_ours) {
        DeleteFileW(p.s);
        return;
    }
    FILE *f = _wfopen(p.s, L"wb");
    if (!f) return;
    for (i32 i = 0; i < RUST_BIND_COUNT; i++)
        if (g_rust_ours & (1u << i)) fprintf(f, "%s\n", RUST_BIND_WORDS[i]);
    fclose(f);
}

// Whether RustClient.exe runs.
static bool rust_running()
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W e = {sizeof e};
    bool found = false;
    for (BOOL ok = Process32FirstW(snap, &e); ok && !found; ok = Process32NextW(snap, &e)) found = !_wcsicmp(e.szExeFile, L"RustClient.exe");
    CloseHandle(snap);
    return found;
}

// Writes keys.cfg whole: into a file beside it, then over it.
static bool rust_cfg_write(const wchar_t *path, const char *text)
{
    wchar_t tmp[MAX_PATH];
    swprintf(tmp, MAX_PATH, L"%s.rekodr", path);
    FILE *f = _wfopen(tmp, L"wb");
    if (!f) return false;
    size_t n = strlen(text);
    bool ok = fwrite(text, 1, n, f) == n;
    ok &= fclose(f) == 0;
    ok = ok && MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING);
    if (!ok) DeleteFileW(tmp);
    return ok;
}

// Brings keys.cfg in line with the binds asked for (g_rust.want): adds each one on, and takes out each one off that
// Rekodr added, unless Rust runs, when the change waits. Sets the binds' state and the key pressed after a death. From
// the worker every RUST_EVERY, and from Options when a bind is turned on or off.
static void rust_binds_sync()
{
    AcquireSRWLockExclusive(&g_rust_lock);
    if (!g_rust_ours_loaded) rust_ours_load();
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    wcscpy_s(dir, g_rust.dir);
    swprintf(path, MAX_PATH, L"%s\\cfg\\keys.cfg", dir);
    u32 want = g_rust.want, ours = g_rust_ours;
    char *text = dir[0] ? read_file(path, 0) : 0;
    bool running = text && rust_running();
    RustBindView view[RUST_BIND_COUNT];
    u32 changed = 0;
    u16 vk = 0;
    for (i32 i = 0; i < RUST_BIND_COUNT; i++) {
        u32 bit = 1u << i;
        const char *own = i == RUST_BIND_DEATH ? "f15" : "space"; // the key Rekodr binds
        char key[16];
        str_set(key, own);
        bool found = false;
        if (text) found = i == RUST_BIND_DEATH ? rust_death_key(text, key) : rust_bind_find(text, own).found;
        bool on = want & bit, mine = !_stricmp(key, own);
        if (on && !found && text) ours |= bit; // asked for now, so Rekodr's to take out, pasted or written
        if (text && !running && on != found && (on || ((ours & bit) && mine))) {
            if (char *next = rust_cfg_set(text, key, on)) {
                free(text);
                text = next;
                changed |= bit;
                found = on;
            }
        }
        if (!on && !found) ours &= ~bit;
        RustBindView &v = view[i];
        v.show = on ? (found ? RUST_SHOW_BOUND : running ? RUST_SHOW_PENDING : RUST_SHOW_NOT_BOUND)
                    : (found && (ours & bit) && mine) ? RUST_SHOW_UNBIND : RUST_SHOW_OFF;
        str_set(v.key, key);
        if (i == RUST_BIND_DEATH && on && (found || running)) vk = rust_key_vk(key);
    }
    if (changed && !rust_cfg_write(path, text)) {
        log_line("markers: Rust: keys.cfg not written (%lu)", GetLastError());
        for (i32 i = 0; i < RUST_BIND_COUNT; i++) {
            if (!(changed & (1u << i))) continue;
            bool on = want & (1u << i);
            view[i].show = on ? RUST_SHOW_NOT_BOUND : RUST_SHOW_OFF;
            if (on && i == RUST_BIND_DEATH) vk = 0;
            if (!on) ours |= 1u << i; // still there, still Rekodr's
        }
        free(text);
        text = dir[0] ? read_file(path, 0) : 0;
    } else if (changed) log_line("markers: Rust: keys.cfg binds written");
    for (i32 i = 0; i < RUST_BIND_COUNT; i++) rust_bind_line(text ? text : "", view[i].key, view[i].line, sizeof view[i].line);
    memcpy(g_rust_view, view, sizeof view);
    g_rust.press_vk = vk;
    if (ours != g_rust_ours) {
        g_rust_ours = ours;
        rust_ours_save();
    }
    free(text);
    ReleaseSRWLockExclusive(&g_rust_lock);
}

// A bind's state, for Options. Main thread.
static void rust_bind_view(i32 bind, RustBindView *v)
{
    AcquireSRWLockShared(&g_rust_lock);
    *v = g_rust_view[bind];
    ReleaseSRWLockShared(&g_rust_lock);
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

// A key's press: down and up, by virtual key and scan code.
static void rust_key_inputs(u16 vk, INPUT (&in)[2])
{
    memset(in, 0, sizeof in);
    in[0].type = INPUT_KEYBOARD;
    in[0].ki.wVk = vk;
    in[0].ki.wScan = (WORD)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
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
    i32 bit = 0;
    while (bit < 31 && !(m.event & (1u << bit))) bit++;
    e.b = AM_EVENT_COLOURS[bit];
    snprintf(e.text, sizeof e.text, "%s%s%s", m.note, m.detail[0] ? "\t" : "", m.detail);
    event_post(e);
}

// The worker's look at the log and at the binds, and the press after a death when one is due.
static void rust_poll()
{
    u32 ev = g_am.events[AMG_RUST];
    i64 now = now_us();
    if (g_rust.press_at && rust_press_due(now, g_rust.press_at)) {
        g_rust.press_at = 0;
        u16 vk = g_rust.press_vk;
        if (vk && !is_test_copy() && rust_in_front()) {
            INPUT in[2];
            rust_key_inputs(vk, in);
            SendInput(2, in, sizeof in[0]);
            log_line("markers: Rust: key %u pressed for the combat log", vk);
        }
    } else if (g_rust.press_at && now > g_rust.press_at) g_rust.press_at = 0;
    if (now < g_rust.next) return;
    g_rust.next = now + RUST_EVERY;
    if (g_rust.dir[0] && (g_rust.want || g_rust_ours)) rust_binds_sync();
    if (!ev || !g_rust.dir[0]) return;
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
        if (m.event == AM_DEATH && g_rust.press_vk && llabs(now - m.t_us) <= RUST_LIVE_US) g_rust.press_at = m.t_us + RUST_PRINT_AFTER;
    }
    if (made) {
        rust_seen_write();
        log_line("markers: Rust: %d from its log", made);
    }
}

// The game's folder, the binds asked for (none while Rust's markers are off) and the game's state for Options. From
// automarks_apply. Main thread.
static void rust_apply()
{
    if (!g_rust_ours_loaded) {
        AcquireSRWLockExclusive(&g_rust_lock);
        if (!g_rust_ours_loaded) rust_ours_load();
        ReleaseSRWLockExclusive(&g_rust_lock);
    }
    u32 want = g_am.events[AMG_RUST] ? g_app.config.rust_binds : 0;
    wchar_t d[MAX_PATH] = L"";
    if (g_am.events[AMG_RUST] || g_rust_ours) rust_dir(d);
    wcscpy_s(g_rust.dir, d); // the worker reads it whole or not at all
    g_rust.want = want;
    rust_binds_sync();
    InterlockedExchange(&g_am.state[AMG_RUST], !g_am.events[AMG_RUST] ? AM_OFF : d[0] ? AM_READY : AM_NOT_FOUND);
}
