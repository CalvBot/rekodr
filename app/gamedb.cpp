// The games database: which exe is which game, its proper name, and where its icon and cover are.
//
// clipback fetches it itself; nothing is shipped with it. The list is Discord's public list of the games it can
// detect (about 25,000, each with its exe names, the folders they sit in and, for an exe many programs share, the
// command line that tells them apart: javaw.exe is Minecraft only when it runs Minecraft's main class). Icons come
// from Discord's image server and covers from Steam's when the game is on Steam. All of it is kept under
// <local>\gamedb and the list is fetched again after a week:
//   detectable.json   the list as fetched
//   games.tsv         the same, one game a line, which is what is loaded:
//                     id, name, icon, cover, Steam id, other names (|), exes (|; "!" first marks a launcher,
//                     ">" a shared exe; the command line it needs follows a \x1f)
//   art\<id>-icon.png, art\<id>-art.jpg       fetched the first time something shows them; an empty file means
//                                             there is none
// One worker thread does the fetching and the loading. The index is never changed once built, only replaced, and
// readers copy what they find out of it under the lock, so any thread may ask.

struct GameInfo {
    char id[24];
    char name[128];
    char icon[40], cover[40]; // Discord's hashes; "" = none
    u32  steam;               // Steam's id for it, 0 = not on Steam
    bool launcher;            // matched by its launcher's exe, not the game's
};

struct GameRow {
    const char *id, *name, *icon, *cover, *aliases, *exes;
    u32 steam;
};
struct GameKey {
    u64 hash;
    i32 row;
};
struct GameIndex {
    char    *text; // games.tsv, cut into strings in place
    GameRow *rows;
    i32      row_count;
    GameKey *exes;  // by an exe's file name, sorted
    i32      exe_count;
    GameKey *names; // by the letters of a name or other name, sorted
    i32      name_count;
};

enum { GAMEDB_LOADED = 1, GAMEDB_ART = 2 };
enum GameArt { ART_ICON, ART_COVER };

struct GameDbShared {
    SRWLOCK    lock;
    GameIndex *index;
    HANDLE     thread, wake;
    volatile LONG stop;
    bool       online;      // may fetch
    struct { char id[24], hash[40]; u32 steam; GameArt kind; } want[64]; // art asked for, not fetched yet
    i32        want_count;
};
static GameDbShared g_gamedb;

static u64 gdb_hash(const char *s, size_t n)
{
    u64 h = 14695981039346656037ull;
    for (size_t i = 0; i < n; i++) h = (h ^ (u8)s[i]) * 1099511628211ull;
    return h;
}

// The letters and digits of a name, lower case: how names are compared ("Overwatch(R) 2" is "overwatch2").
static i32 gdb_letters(const char *in, char *out, i32 cap)
{
    i32 n = 0;
    for (; *in && n < cap - 1; in++)
        if (isalnum((u8)*in)) out[n++] = (char)tolower((u8)*in);
    out[n] = 0;
    return n;
}

static Path gamedb_dir()
{
    Path d = path_join(g_app.paths.local_dir, L"gamedb");
    SHCreateDirectoryExW(0, path_join(d, L"art").s, 0);
    return d;
}

static Path gamedb_art_file(const char *id, GameArt kind)
{
    wchar_t name[64];
    swprintf(name, COUNT(name), L"art\\%hs-%s", id, kind == ART_ICON ? L"icon.png" : L"art.jpg");
    return path_join(gamedb_dir(), name);
}

static char *gdb_read(const Path &file, i64 *size)
{
    FILE *f = _wfopen(file.s, L"rb");
    if (!f) return 0;
    _fseeki64(f, 0, SEEK_END);
    i64 n = _ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
    char *text = (char *)malloc((size_t)n + 1);
    if (text) {
        n = (i64)fread(text, 1, (size_t)n, f);
        text[n] = 0;
        *size = n;
    }
    fclose(f);
    return text;
}

// ---------------------------------------------------------------------------------------------------------------
// Fetching: one GET over https to a file (through a .part, so a half-fetched file is never read).

static bool http_get_file(const wchar_t *host, const wchar_t *path, const Path &out)
{
    bool ok = false;
    Path part = out;
    wcsncat(part.s, L".part", MAX_PATH - wcslen(part.s) - 1);
    HINTERNET session = WinHttpOpen(L"clipback", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, 0, 0, 0);
    HINTERNET conn = session ? WinHttpConnect(session, host, INTERNET_DEFAULT_HTTPS_PORT, 0) : 0;
    HINTERNET req = conn ? WinHttpOpenRequest(conn, L"GET", path, 0, 0, 0, WINHTTP_FLAG_SECURE) : 0;
    if (req) {
        DWORD unzip = WINHTTP_DECOMPRESSION_FLAG_ALL;
        WinHttpSetOption(req, WINHTTP_OPTION_DECOMPRESSION, &unzip, sizeof unzip);
        WinHttpSetTimeouts(req, 10000, 10000, 20000, 30000);
        DWORD status = 0, len = sizeof status;
        if (WinHttpSendRequest(req, 0, 0, 0, 0, 0, 0) && WinHttpReceiveResponse(req, 0) &&
            WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, 0, &status, &len, 0) && status == 200) {
            FILE *f = _wfopen(part.s, L"wb");
            if (f) {
                static thread_local char buf[65536];
                ok = true;
                for (;;) {
                    DWORD got = 0;
                    if (!WinHttpReadData(req, buf, sizeof buf, &got)) { ok = false; break; }
                    if (!got) break;
                    if (fwrite(buf, 1, got, f) != got) { ok = false; break; }
                    if (g_gamedb.stop) { ok = false; break; }
                }
                fclose(f);
            }
        } else {
            DEV_LOG("gamedb: %ls%ls answered %u (error %d)", host, path, status, (int)GetLastError());
        }
    }
    if (req) WinHttpCloseHandle(req);
    if (conn) WinHttpCloseHandle(conn);
    if (session) WinHttpCloseHandle(session);
    if (ok) ok = MoveFileExW(part.s, out.s, MOVEFILE_REPLACE_EXISTING) != 0;
    else DeleteFileW(part.s);
    return ok;
}

// ---------------------------------------------------------------------------------------------------------------
// The fetched list is JSON: an array of games. A small reader, enough for it.

struct Json {
    const char *p, *end;
};
static void j_ws(Json *j) { while (j->p < j->end && (u8)*j->p <= ' ') j->p++; }
static bool j_eat(Json *j, char c)
{
    j_ws(j);
    if (j->p < j->end && *j->p == c) { j->p++; return true; }
    return false;
}

// A string into out (cut at cap), its escapes undone. False if there isn't one here.
static bool j_string(Json *j, char *out, i32 cap)
{
    j_ws(j);
    if (j->p >= j->end || *j->p != '"') return false;
    i32 n = 0;
    auto put = [&](char c) { if (out && n < cap - 1) out[n++] = c; };
    for (j->p++; j->p < j->end && *j->p != '"'; j->p++) {
        if (*j->p != '\\' || j->p + 1 >= j->end) { put(*j->p); continue; }
        char c = *++j->p;
        if (c == 'u' && j->p + 4 < j->end) {
            char hex[5] = {j->p[1], j->p[2], j->p[3], j->p[4], 0};
            u32 u = strtoul(hex, 0, 16);
            j->p += 4;
            if (u < 0x80) put((char)u);
            else if (u < 0x800) { put((char)(0xC0 | (u >> 6))); put((char)(0x80 | (u & 63))); }
            else { put((char)(0xE0 | (u >> 12))); put((char)(0x80 | ((u >> 6) & 63))); put((char)(0x80 | (u & 63))); }
        } else {
            put(c == 'n' || c == 't' || c == 'r' ? ' ' : c);
        }
    }
    if (out && cap > 0) out[n] = 0;
    if (j->p < j->end) j->p++;
    return true;
}

static void j_skip(Json *j)
{
    j_ws(j);
    if (j->p >= j->end) return;
    if (*j->p == '"') { j_string(j, 0, 0); return; }
    if (*j->p == '{' || *j->p == '[') {
        char open = *j->p, close = open == '{' ? '}' : ']';
        j->p++;
        while (j->p < j->end && !j_eat(j, close)) {
            if (open == '{') { j_string(j, 0, 0); j_eat(j, ':'); }
            j_skip(j);
            j_eat(j, ',');
        }
        return;
    }
    while (j->p < j->end && *j->p != ',' && *j->p != '}' && *j->p != ']') j->p++; // a number, true, false, null
}

// Nothing in a field may be one of the file's own separators.
static void gdb_clean(char *s)
{
    for (; *s; s++)
        if (*s == '\t' || *s == '|' || *s == '\x1f' || *s == '\n' || *s == '\r') *s = ' ';
}

// detectable.json -> games.tsv. Returns how many games were written.
static i32 gamedb_convert(const Path &json, const Path &tsv)
{
    i64 size = 0;
    char *text = gdb_read(json, &size);
    if (!text) return 0;
    Path part = tsv;
    wcsncat(part.s, L".part", MAX_PATH - wcslen(part.s) - 1);
    FILE *out = _wfopen(part.s, L"wb");
    i32 count = 0;
    Json j = {text, text + size};
    if (out && j_eat(&j, '[')) {
        static thread_local char aliases[2048], exes[4096];
        while (j_eat(&j, '{')) {
            char id[24] = "", name[128] = "", icon[40] = "", cover[40] = "", steam[16] = "", key[40];
            aliases[0] = exes[0] = 0;
            while (!j_eat(&j, '}') && j.p < j.end) {
                if (!j_string(&j, key, sizeof key)) break;
                j_eat(&j, ':');
                if (!strcmp(key, "id")) j_string(&j, id, sizeof id) || (j_skip(&j), 0);
                else if (!strcmp(key, "name")) j_string(&j, name, sizeof name) || (j_skip(&j), 0);
                else if (!strcmp(key, "icon_hash")) j_string(&j, icon, sizeof icon) || (j_skip(&j), 0);
                else if (!strcmp(key, "cover_image_hash")) j_string(&j, cover, sizeof cover) || (j_skip(&j), 0);
                else if (!strcmp(key, "aliases") && j_eat(&j, '[')) {
                    while (!j_eat(&j, ']') && j.p < j.end) {
                        char a[128];
                        if (!j_string(&j, a, sizeof a)) { j_skip(&j); j_eat(&j, ','); continue; }
                        gdb_clean(a);
                        size_t n = strlen(aliases);
                        if (a[0] && n + strlen(a) + 2 < sizeof aliases) snprintf(aliases + n, sizeof aliases - n, "%s%s", n ? "|" : "", a);
                        j_eat(&j, ',');
                    }
                } else if (!strcmp(key, "executables") && j_eat(&j, '[')) {
                    while (j_eat(&j, '{')) {
                        char exe[160] = "", os[16] = "", args[160] = "", k[24];
                        bool launcher = false;
                        while (!j_eat(&j, '}') && j.p < j.end) {
                            if (!j_string(&j, k, sizeof k)) break;
                            j_eat(&j, ':');
                            if (!strcmp(k, "name")) j_string(&j, exe, sizeof exe) || (j_skip(&j), 0);
                            else if (!strcmp(k, "os")) j_string(&j, os, sizeof os) || (j_skip(&j), 0);
                            else if (!strcmp(k, "arguments")) j_string(&j, args, sizeof args) || (j_skip(&j), 0);
                            else if (!strcmp(k, "is_launcher")) { j_ws(&j); launcher = j.p < j.end && *j.p == 't'; j_skip(&j); }
                            else j_skip(&j);
                            j_eat(&j, ',');
                        }
                        j_eat(&j, ',');
                        if (strcmp(os, "win32") || !exe[0]) continue;
                        gdb_clean(exe);
                        gdb_clean(args);
                        for (char *c = exe; *c; c++) *c = *c == '\\' ? '/' : (char)tolower((u8)*c);
                        size_t n = strlen(exes);
                        if (n + strlen(exe) + strlen(args) + 4 < sizeof exes)
                            snprintf(exes + n, sizeof exes - n, "%s%s%s%s%s", n ? "|" : "", launcher ? "!" : "", exe, args[0] ? "\x1f" : "", args);
                    }
                    j_eat(&j, ']');
                } else if (!strcmp(key, "third_party_skus") && j_eat(&j, '[')) {
                    while (j_eat(&j, '{')) {
                        char dist[24] = "", sid[32] = "", k[24];
                        while (!j_eat(&j, '}') && j.p < j.end) {
                            if (!j_string(&j, k, sizeof k)) break;
                            j_eat(&j, ':');
                            if (!strcmp(k, "distributor")) j_string(&j, dist, sizeof dist) || (j_skip(&j), 0);
                            else if (!strcmp(k, "id")) j_string(&j, sid, sizeof sid) || (j_skip(&j), 0);
                            else j_skip(&j);
                            j_eat(&j, ',');
                        }
                        j_eat(&j, ',');
                        if (!strcmp(dist, "steam") && !steam[0] && atoi(sid) > 0) snprintf(steam, sizeof steam, "%d", atoi(sid));
                    }
                    j_eat(&j, ']');
                } else {
                    j_skip(&j);
                }
                j_eat(&j, ',');
            }
            j_eat(&j, ',');
            gdb_clean(name);
            if (!id[0] || !name[0]) continue;
            fprintf(out, "%s\t%s\t%s\t%s\t%s\t%s\t%s\n", id, name, icon, cover, steam[0] ? steam : "0", aliases, exes);
            count++;
        }
    }
    free(text);
    bool ok = out && !ferror(out);
    if (out) fclose(out);
    if (ok && count > 1000) MoveFileExW(part.s, tsv.s, MOVEFILE_REPLACE_EXISTING); // fewer isn't the list
    else DeleteFileW(part.s);
    return ok ? count : 0;
}

// ---------------------------------------------------------------------------------------------------------------
// The index

static int gdb_key_cmp(const void *a, const void *b)
{
    u64 x = ((const GameKey *)a)->hash, y = ((const GameKey *)b)->hash;
    return x < y ? -1 : x > y ? 1 : 0;
}

// The file name of an exe entry ("!win64/cs2.exe\x1f-args" -> "cs2.exe"), and its length.
static const char *gdb_exe_file(const char *e, i32 len, i32 *file_len)
{
    while (len && (*e == '!' || *e == '>')) { e++; len--; }
    i32 n = 0;
    while (n < len && e[n] != '\x1f') n++;
    i32 start = n;
    while (start > 0 && e[start - 1] != '/') start--;
    *file_len = n - start;
    return e + start;
}

static GameIndex *gamedb_load(const Path &tsv)
{
    i64 size = 0;
    char *text = gdb_read(tsv, &size);
    if (!text) return 0;
    i32 lines = 0;
    for (i64 i = 0; i < size; i++) lines += text[i] == '\n';
    GameIndex *ix = (GameIndex *)calloc(1, sizeof *ix);
    ix->text = text;
    ix->rows = (GameRow *)calloc(lines + 1, sizeof ix->rows[0]);
    i32 exe_cap = 0, name_cap = 0;
    char *next = 0;
    for (char *line = strtok_s(text, "\n", &next); line; line = strtok_s(0, "\n", &next)) {
        char *f[7];
        i32 n = 0;
        f[n++] = line;
        for (char *c = line; *c && n < 7; c++)
            if (*c == '\t') { *c = 0; f[n++] = c + 1; }
        if (n != 7) continue;
        GameRow *r = &ix->rows[ix->row_count++];
        r->id = f[0]; r->name = f[1]; r->icon = f[2]; r->cover = f[3]; r->steam = (u32)atoi(f[4]); r->aliases = f[5]; r->exes = f[6];
        name_cap += 1;
        for (const char *c = r->aliases; *c; c++) name_cap += *c == '|';
        if (r->aliases[0]) name_cap++;
        for (const char *c = r->exes; *c; c++) exe_cap += *c == '|';
        if (r->exes[0]) exe_cap++;
    }
    ix->exes = (GameKey *)calloc(exe_cap + 1, sizeof ix->exes[0]);
    ix->names = (GameKey *)calloc(name_cap + 1, sizeof ix->names[0]);
    for (i32 i = 0; i < ix->row_count; i++) {
        const GameRow &r = ix->rows[i];
        char letters[128];
        i32 n = gdb_letters(r.name, letters, sizeof letters);
        if (n) ix->names[ix->name_count++] = {gdb_hash(letters, n), i};
        for (const char *a = r.aliases; *a;) {
            const char *e = strchr(a, '|');
            char one[128];
            snprintf(one, sizeof one, "%.*s", (int)(e ? e - a : strlen(a)), a);
            n = gdb_letters(one, letters, sizeof letters);
            if (n) ix->names[ix->name_count++] = {gdb_hash(letters, n), i};
            a = e ? e + 1 : a + strlen(a);
        }
        for (const char *x = r.exes; *x;) {
            const char *e = strchr(x, '|');
            i32 len = (i32)(e ? e - x : strlen(x)), fl = 0;
            const char *file = gdb_exe_file(x, len, &fl);
            if (fl) ix->exes[ix->exe_count++] = {gdb_hash(file, fl), i};
            x = e ? e + 1 : x + len;
        }
    }
    qsort(ix->exes, ix->exe_count, sizeof ix->exes[0], gdb_key_cmp);
    qsort(ix->names, ix->name_count, sizeof ix->names[0], gdb_key_cmp);
    return ix;
}

static void gamedb_free(GameIndex *ix)
{
    if (!ix) return;
    free(ix->text);
    free(ix->rows);
    free(ix->exes);
    free(ix->names);
    free(ix);
}

static void gdb_fill(const GameRow &r, GameInfo *out)
{
    *out = {};
    str_set(out->id, r.id);
    str_set(out->name, r.name);
    str_set(out->icon, r.icon);
    str_set(out->cover, r.cover);
    out->steam = r.steam;
}

// The first key with this hash, or -1.
static i32 gdb_find(const GameKey *keys, i32 count, u64 hash)
{
    i32 lo = 0, hi = count;
    while (lo < hi) {
        i32 mid = (lo + hi) / 2;
        if (keys[mid].hash < hash) lo = mid + 1;
        else hi = mid;
    }
    return lo < count && keys[lo].hash == hash ? lo : -1;
}

// A game by its name or one of its other names, letters only. Any thread.
static bool gamedb_by_name(const char *name, GameInfo *out)
{
    char letters[128];
    i32 n = gdb_letters(name, letters, sizeof letters);
    bool found = false;
    AcquireSRWLockShared(&g_gamedb.lock);
    const GameIndex *ix = g_gamedb.index;
    i32 k = ix && n ? gdb_find(ix->names, ix->name_count, gdb_hash(letters, n)) : -1;
    // The same letters can name a few entries (a game and its test server); the one named exactly so wins
    for (; k >= 0 && k < ix->name_count && ix->names[k].hash == gdb_hash(letters, n); k++) {
        const GameRow &r = ix->rows[ix->names[k].row];
        char own[128];
        gdb_letters(r.name, own, sizeof own);
        bool exact = !strcmp(own, letters);
        if (found && !exact) continue;
        gdb_fill(r, out);
        found = true;
        if (exact) break;
    }
    ReleaseSRWLockShared(&g_gamedb.lock);
    return found;
}

// The game an exe is. `path` is where it runs from (UTF-8, either slash; "" = not known), `cmdline` and `title`
// its command line and window title when the caller has them ("" otherwise). An entry that names folders wants the
// path, when there is one, to end with them; one that names a command line wants it there. With several that fit, the most exact wins.
// *shared is set when the exe is one that several programs share and what was given couldn't tell which it is:
// ask again with the command line and the title.
static bool gamedb_by_exe(const char *exe, const char *path, const char *cmdline, const char *title, GameInfo *out,
                          bool *shared = 0)
{
    char file[96], where[MAX_PATH * 2], cmd[4096], seen[512];
    i32 fl = 0;
    for (; exe[fl] && fl < (i32)sizeof file - 1; fl++) file[fl] = (char)tolower((u8)exe[fl]);
    file[fl] = 0;
    i32 wl = 0;
    for (; path[wl] && wl < (i32)sizeof where - 1; wl++) where[wl] = path[wl] == '\\' ? '/' : (char)tolower((u8)path[wl]);
    where[wl] = 0;
    i32 cl = 0;
    for (; cmdline[cl] && cl < (i32)sizeof cmd - 1; cl++) cmd[cl] = (char)tolower((u8)cmdline[cl]);
    cmd[cl] = 0;
    snprintf(seen, sizeof seen, "%s", title);
    char title_letters[256], cmd_letters[2048];
    gdb_letters(seen, title_letters, sizeof title_letters);
    gdb_letters(cmd, cmd_letters, sizeof cmd_letters);

    i32 best = 0, best_row = -1, guess_row = -1;
    bool best_launcher = false, wants_more = false;
    u64 hash = gdb_hash(file, fl);
    AcquireSRWLockShared(&g_gamedb.lock);
    const GameIndex *ix = g_gamedb.index;
    i32 k = ix && fl ? gdb_find(ix->exes, ix->exe_count, hash) : -1;
    for (; k >= 0 && k < ix->exe_count && ix->exes[k].hash == hash; k++) {
        const GameRow &r = ix->rows[ix->exes[k].row];
        for (const char *x = r.exes; *x;) {
            const char *e = strchr(x, '|');
            i32 len = (i32)(e ? e - x : strlen(x));
            const char *ent = x;
            x = e ? e + 1 : x + len;
            bool launcher = false, strict = false;
            while (len && (*ent == '!' || *ent == '>')) { launcher |= *ent == '!'; strict |= *ent == '>'; ent++; len--; }
            i32 nl = 0;
            while (nl < len && ent[nl] != '\x1f') nl++;
            char name[200], args[200] = "";
            snprintf(name, sizeof name, "%.*s", nl, ent);
            if (nl < len) snprintf(args, sizeof args, "%.*s", len - nl - 1, ent + nl + 1);
            for (char *c = args; *c; c++) *c = (char)tolower((u8)*c);
            // The file name must be this exe's; folders named with it must be where it runs from
            const char *slash = strrchr(name, '/');
            if (strcmp(slash ? slash + 1 : name, file)) continue;
            i32 score = 1;
            if (slash && wl) { // where it runs from isn't always known; then the file name has to do
                size_t n = strlen(name);
                if ((size_t)wl < n + 1 || strcmp(where + wl - n, name) || where[wl - n - 1] != '/') continue;
                score += 2;
                for (const char *c = name; *c; c++) score += *c == '/';
            }
            if (args[0] || strict) {
                if (args[0] && cl && strstr(cmd, args)) {
                    score += 10;
                } else {
                    // Not provably this one. If the window or the command line names the game, it is a fair guess
                    wants_more = true;
                    char letters[128];
                    i32 n = gdb_letters(r.name, letters, sizeof letters);
                    if (n >= 5 && guess_row < 0 && (strstr(title_letters, letters) || strstr(cmd_letters, letters)))
                        guess_row = ix->exes[k].row;
                    if (!slash || !wl) continue;
                }
            }
            if (score > best) { best = score; best_row = ix->exes[k].row; best_launcher = launcher; }
        }
    }
    if (best_row < 0 && guess_row >= 0) best_row = guess_row;
    if (best_row >= 0) {
        gdb_fill(ix->rows[best_row], out);
        out->launcher = best_launcher;
    }
    ReleaseSRWLockShared(&g_gamedb.lock);
    if (shared) *shared = best_row < 0 && wants_more;
    return best_row >= 0;
}

// ---------------------------------------------------------------------------------------------------------------
// A running copy of an exe: its command line and the title of its window, for the exes several games share.

static bool process_facts(const char *exe, char *cmdline, i32 cmd_cap, char *title, i32 title_cap)
{
    cmdline[0] = title[0] = 0;
    wchar_t want[MAX_PATH];
    utf8_to_wide(exe, want, MAX_PATH);
    DWORD pid = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe = {};
    pe.dwSize = sizeof pe;
    for (BOOL ok = Process32FirstW(snap, &pe); ok && !pid; ok = Process32NextW(snap, &pe))
        if (!_wcsicmp(pe.szExeFile, want)) pid = pe.th32ProcessID;
    CloseHandle(snap);
    if (!pid) return false;

    // ProcessCommandLineInformation (60): the command line as a UNICODE_STRING, without reading the process's memory
    typedef LONG(WINAPI * QueryFn)(HANDLE, ULONG, void *, ULONG, ULONG *);
    static QueryFn query = (QueryFn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess");
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (p && query) {
        static thread_local u8 buf[16384];
        ULONG got = 0;
        if (query(p, 60, buf, sizeof buf, &got) >= 0) {
            struct { USHORT len, max; wchar_t *text; } *us = (decltype(us))buf;
            if (us->text && us->len) {
                static thread_local wchar_t wide[8192];
                i32 n = us->len / 2 < COUNT(wide) - 1 ? us->len / 2 : COUNT(wide) - 1;
                memcpy(wide, us->text, n * sizeof(wchar_t));
                wide[n] = 0;
                wide_to_utf8(wide, cmdline, cmd_cap);
            }
        }
    }
    if (p) CloseHandle(p);

    struct Find { DWORD pid; wchar_t title[256]; } find = {pid, L""};
    EnumWindows([](HWND w, LPARAM lp) -> BOOL {
        Find *f = (Find *)lp;
        DWORD owner = 0;
        GetWindowThreadProcessId(w, &owner);
        if (owner != f->pid || !IsWindowVisible(w) || GetWindow(w, GW_OWNER)) return TRUE;
        wchar_t t[256];
        if (GetWindowTextW(w, t, COUNT(t)) > (int)wcslen(f->title)) wcscpy(f->title, t);
        return TRUE;
    }, (LPARAM)&find);
    wide_to_utf8(find.title, title, title_cap);
    return true;
}

// The one game an exe belongs to in the list, whichever of its entries that is: javaw.exe is only ever listed for
// Minecraft, so a modpack that names itself something else is still Minecraft underneath. False if the list has
// the exe under several games, or not at all.
static bool gamedb_family(const char *exe, GameInfo *out)
{
    char file[96];
    i32 fl = 0;
    for (; exe[fl] && fl < (i32)sizeof file - 1; fl++) file[fl] = (char)tolower((u8)exe[fl]);
    file[fl] = 0;
    u64 hash = gdb_hash(file, fl);
    i32 row = -1;
    bool one = true;
    AcquireSRWLockShared(&g_gamedb.lock);
    const GameIndex *ix = g_gamedb.index;
    i32 k = ix && fl ? gdb_find(ix->exes, ix->exe_count, hash) : -1;
    for (; k >= 0 && k < ix->exe_count && ix->exes[k].hash == hash; k++) {
        if (row >= 0 && ix->exes[k].row != row) one = false;
        row = ix->exes[k].row;
    }
    if (row >= 0 && one) gdb_fill(ix->rows[row], out);
    ReleaseSRWLockShared(&g_gamedb.lock);
    return row >= 0 && one;
}

// Games whose name has these letters in it, best first: a name that starts with them before one that only has
// them, one with an icon and a Windows exe before a bare entry (the list has thousands), and of those the
// shortest name, which is the nearest to what was typed. Returns how many were put in `out`.
static i32 gamedb_search(const char *query, GameInfo *out, i32 max)
{
    char want[128];
    i32 wl = gdb_letters(query, want, sizeof want), n = 0;
    if (!wl || max <= 0 || max > 16) return 0;
    i32 rows[16], scores[16];
    AcquireSRWLockShared(&g_gamedb.lock);
    const GameIndex *ix = g_gamedb.index;
    for (i32 i = 0; ix && i < ix->row_count; i++) {
        const GameRow &r = ix->rows[i];
        char letters[128];
        i32 len = gdb_letters(r.name, letters, sizeof letters);
        const char *at = strstr(letters, want);
        if (!at) continue;
        i32 score = (at == letters ? 0 : 4000) + (r.icon[0] ? 0 : 2000) + (r.exes[0] ? 0 : 1000) + len;
        i32 k = n;
        while (k > 0 && scores[k - 1] > score) k--;
        bool dup = false;
        for (i32 d = 0; d < n && !dup; d++) dup = !_stricmp(ix->rows[rows[d]].name, r.name);
        if (dup || k >= max) continue;
        if (n < max) n++;
        for (i32 m = n - 1; m > k; m--) { rows[m] = rows[m - 1]; scores[m] = scores[m - 1]; }
        rows[k] = i;
        scores[k] = score;
    }
    for (i32 i = 0; i < n; i++) gdb_fill(ix->rows[rows[i]], &out[i]);
    ReleaseSRWLockShared(&g_gamedb.lock);
    return n;
}

// Names that stand for a game in the list: "GT New Horizons" is Minecraft, for its icon and cover. Kept in
// <config>\game-aliases.txt, a line each of "name<TAB>game". Written when a shared exe names itself by its window,
// and when a name is typed for an app the list knows as something else. Main thread only.
struct GameAlias {
    char name[96], game[128];
};
static GameAlias g_game_aliases[256];
static i32       g_game_alias_count = -1; // -1: not read yet

static void game_aliases_load()
{
    g_game_alias_count = 0;
    FILE *f = _wfopen(path_join(g_app.paths.config_dir, L"game-aliases.txt").s, L"rb");
    if (!f) return;
    char line[300];
    while (fgets(line, sizeof line, f) && g_game_alias_count < COUNT(g_game_aliases)) {
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        char *game = trim(tab + 1), *name = trim(line);
        if (!*name || !*game) continue;
        GameAlias *a = &g_game_aliases[g_game_alias_count++];
        snprintf(a->name, sizeof a->name, "%s", name);
        snprintf(a->game, sizeof a->game, "%s", game);
    }
    fclose(f);
}

static const char *game_alias(const char *name)
{
    if (g_game_alias_count < 0) game_aliases_load();
    char want[128], have[128];
    if (!gdb_letters(name, want, sizeof want)) return 0;
    for (i32 i = 0; i < g_game_alias_count; i++) {
        gdb_letters(g_game_aliases[i].name, have, sizeof have);
        if (!strcmp(want, have)) return g_game_aliases[i].game;
    }
    return 0;
}

static void game_alias_set(const char *name, const char *game)
{
    const char *now = game_alias(name);
    if (now && !strcmp(now, game)) return;
    GameAlias *a = 0;
    char want[128], have[128];
    gdb_letters(name, want, sizeof want);
    for (i32 i = 0; i < g_game_alias_count && !a; i++) {
        gdb_letters(g_game_aliases[i].name, have, sizeof have);
        if (!strcmp(want, have)) a = &g_game_aliases[i];
    }
    if (!a && g_game_alias_count < COUNT(g_game_aliases)) a = &g_game_aliases[g_game_alias_count++];
    if (!a) return;
    snprintf(a->name, sizeof a->name, "%s", name);
    snprintf(a->game, sizeof a->game, "%s", game);
    FILE *f = _wfopen(path_join(g_app.paths.config_dir, L"game-aliases.txt").s, L"wb");
    if (!f) return;
    for (i32 i = 0; i < g_game_alias_count; i++) fprintf(f, "%s\t%s\n", g_game_aliases[i].name, g_game_aliases[i].game);
    fclose(f);
    log_line("%s counts as %s for its icon and cover", name, game);
}

// A game by a name: the list's own, else the game that name stands for. Main thread.
static bool game_lookup(const char *name, GameInfo *out)
{
    if (gamedb_by_name(name, out)) return true;
    const char *alias = game_alias(name);
    return alias && gamedb_by_name(alias, out);
}

// What a window's title says the game is: the title up to its version number or a " - ", so "GT: New Horizons
// 2.9.0" is "GT: New Horizons" from one release to the next. False if that leaves nothing to call it.
static bool title_game_name(const char *title, char *out, i32 cap)
{
    char t[128];
    snprintf(t, sizeof t, "%s", title);
    bool word = false;
    for (char *c = t; *c; c++) {
        bool start = c == t || c[-1] == ' ';
        bool version = start && word && (isdigit((u8)c[0]) || ((c[0] == 'v' || c[0] == 'V') && isdigit((u8)c[1])));
        bool dash = c[0] == ' ' && (c[1] == '-' || c[1] == '|') && c[2] == ' ';
        if (version || dash) { *c = 0; break; }
        if (isalpha((u8)*c)) word = true;
    }
    size_t n = strlen(t);
    while (n && (t[n - 1] == ' ' || t[n - 1] == '-' || t[n - 1] == ':' || t[n - 1] == '*' || t[n - 1] == '|')) t[--n] = 0;
    i32 alpha = 0;
    for (char *c = t; *c; c++) {
        alpha += isalpha((u8)*c) != 0;
        if (*c == '|') *c = ' '; // | separates fields in settings.txt
    }
    if (alpha < 3) return false; // a bare number, or nothing
    snprintf(out, cap, "%s", t);
    return true;
}

static void sessions_game_renamed(const char *exe); // sessions.cpp

// Names an app from the database: its description becomes the game's name, and one that hasn't been sorted yet
// (and that no folder rule sorts) becomes a Game. An exe several games share is asked about while it runs: its
// command line decides, else its window naming a game in the list, else its window's own title is its name (a
// modpack's, say) and it counts as the one game the list has that exe for. Main thread. Returns true if something
// about it changed.
static bool gamedb_name_app(AppInfo *a)
{
    GameInfo g;
    bool shared = false, known = gamedb_by_exe(a->exe.s, a->path, "", "", &g, &shared);
    char named[128] = "";
    a->db_pending = false;
    if (!known && shared) {
        a->db_tried_us = now_us();
        char cmd[4096], title[256];
        bool running = process_facts(a->exe.s, cmd, sizeof cmd, title, sizeof title);
        if (running) {
            DEV_LOG("gamedb: %s is shared; its window is \"%s\", its command line ends \"%s\"", a->exe.s, title,
                    strlen(cmd) > 400 ? cmd + strlen(cmd) - 400 : cmd);
            known = gamedb_by_exe(a->exe.s, a->path, cmd, title, &g);
        }
        if (!known) {
            GameInfo family;
            if (running && title_game_name(title, named, sizeof named)) {
                if (gamedb_family(a->exe.s, &family)) game_alias_set(named, family.name);
            } else {
                // No window yet (or it isn't running). Its File description is the runtime's, not a game's, so it
                // goes by its exe's name until it says more.
                a->db_pending = running;
                if (a->desc.s[0]) {
                    a->desc.s[0] = 0;
                    return true;
                }
                return false;
            }
        }
    }
    if (!known && !named[0]) return false;
    const char *name = known ? g.name : named;
    bool changed = false;
    if (strcmp(a->desc.s, name)) {
        snprintf(a->desc.s, sizeof a->desc.s, "%s", name);
        changed = true;
        sessions_game_renamed(a->exe.s);
    }
    if (known && a->category == CAT_NONE && !g.launcher && app_category(g_app.config, *a) == CAT_NONE) {
        a->category = CAT_GAME;
        log_line("%s is %s: sorted as a game", a->exe.s, g.name);
        changed = true;
    }
    return changed;
}

// ---------------------------------------------------------------------------------------------------------------
// Art. A file that is there is used; one that isn't is asked for and arrives later (GAMEDB_ART).

// Where a game's icon or cover is, if it has been fetched. If it hasn't, it is asked for and false returned.
static bool gamedb_art(const GameInfo &g, GameArt kind, Path *out)
{
    const char *hash = kind == ART_ICON ? g.icon : g.cover;
    if (!hash[0] && !(kind == ART_COVER && g.steam)) return false;
    *out = gamedb_art_file(g.id, kind);
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetFileAttributesExW(out->s, GetFileExInfoStandard, &fa)) return fa.nFileSizeLow > 0; // empty: there is none
    if (!g_gamedb.online) return false;
    AcquireSRWLockExclusive(&g_gamedb.lock);
    bool queued = false;
    for (i32 i = 0; i < g_gamedb.want_count && !queued; i++)
        queued = g_gamedb.want[i].kind == kind && !strcmp(g_gamedb.want[i].id, g.id);
    if (!queued && g_gamedb.want_count < COUNT(g_gamedb.want)) {
        auto &w = g_gamedb.want[g_gamedb.want_count++];
        str_set(w.id, g.id);
        str_set(w.hash, hash);
        w.steam = g.steam;
        w.kind = kind;
    }
    ReleaseSRWLockExclusive(&g_gamedb.lock);
    SetEvent(g_gamedb.wake);
    return false;
}

static void gamedb_fetch_art()
{
    for (;;) {
        AcquireSRWLockExclusive(&g_gamedb.lock);
        if (!g_gamedb.want_count) { ReleaseSRWLockExclusive(&g_gamedb.lock); return; }
        auto w = g_gamedb.want[0];
        ReleaseSRWLockExclusive(&g_gamedb.lock);

        Path file = gamedb_art_file(w.id, w.kind);
        wchar_t url[256];
        bool ok = false;
        if (w.kind == ART_COVER && w.steam) {
            // Steam's library art: a wide picture of the game with no title or season's promotion written across
            // it. Its store banner has both, so that is only the second choice.
            static const wchar_t *const kinds[] = {L"library_hero.jpg", L"header.jpg"};
            for (i32 k = 0; k < COUNT(kinds) && !ok && !g_gamedb.stop; k++) {
                swprintf(url, COUNT(url), L"/store_item_assets/steam/apps/%u/%s", w.steam, kinds[k]);
                ok = http_get_file(L"shared.steamstatic.com", url, file);
            }
        }
        if (!ok && w.hash[0]) {
            swprintf(url, COUNT(url), L"/app-icons/%hs/%hs.png?size=%d", w.id, w.hash, w.kind == ART_ICON ? 128 : 512);
            ok = http_get_file(L"cdn.discordapp.com", url, file);
        }
        if (!ok && !g_gamedb.stop) { // none to be had: an empty file says so, and it isn't asked for again
            FILE *f = _wfopen(file.s, L"wb");
            if (f) fclose(f);
        }
        DEV_LOG("gamedb: %s for %s %s", w.kind == ART_ICON ? "icon" : "cover", w.id, ok ? "fetched" : "not there");

        AcquireSRWLockExclusive(&g_gamedb.lock);
        memmove(&g_gamedb.want[0], &g_gamedb.want[1], (g_gamedb.want_count - 1) * sizeof g_gamedb.want[0]);
        g_gamedb.want_count--;
        ReleaseSRWLockExclusive(&g_gamedb.lock);
        if (g_gamedb.stop) return;
        Event e = {};
        e.kind = EV_GAMEDB;
        e.a = GAMEDB_ART;
        event_post(e);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// The worker: load what is on disk, fetch the list if there is none or it is a week old, then fetch art as asked.

static void gamedb_publish(GameIndex *ix)
{
    if (!ix) return;
    AcquireSRWLockExclusive(&g_gamedb.lock);
    GameIndex *old = g_gamedb.index;
    g_gamedb.index = ix;
    ReleaseSRWLockExclusive(&g_gamedb.lock);
    gamedb_free(old);
    Event e = {};
    e.kind = EV_GAMEDB;
    e.a = GAMEDB_LOADED;
    e.b = ix->row_count;
    event_post(e);
}

static DWORD WINAPI gamedb_thread(void *)
{
    Path dir = gamedb_dir(), json = path_join(dir, L"detectable.json"), tsv = path_join(dir, L"games.tsv");
    gamedb_publish(gamedb_load(tsv));
    WIN32_FILE_ATTRIBUTE_DATA fa;
    bool have = GetFileAttributesExW(tsv.s, GetFileExInfoStandard, &fa) != 0;
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    i64 age = have ? (((i64)now.dwHighDateTime << 32 | now.dwLowDateTime) - ((i64)fa.ftLastWriteTime.dwHighDateTime << 32 | fa.ftLastWriteTime.dwLowDateTime)) / 10000000 : 0;
    if (g_gamedb.online && (!have || age > 7 * 24 * 3600)) {
        if (http_get_file(L"discord.com", L"/api/v10/applications/detectable", json)) {
            i32 n = gamedb_convert(json, tsv);
            log_line("games list fetched: %d games", n);
            if (n) gamedb_publish(gamedb_load(tsv));
        } else if (!g_gamedb.stop) {
            log_line("games list: couldn't fetch it%s", have ? "; using the one from before" : "");
        }
    }
    while (!g_gamedb.stop) {
        gamedb_fetch_art();
        WaitForSingleObject(g_gamedb.wake, INFINITE);
    }
    return 0;
}

static void gamedb_start(bool online)
{
    g_gamedb.online = online;
    g_gamedb.wake = CreateEventW(0, FALSE, FALSE, 0);
    g_gamedb.thread = thread_start(gamedb_thread, 0);
}

static void gamedb_stop()
{
    if (!g_gamedb.thread) return;
    InterlockedExchange(&g_gamedb.stop, 1);
    SetEvent(g_gamedb.wake);
    WaitForSingleObject(g_gamedb.thread, 3000);
}
