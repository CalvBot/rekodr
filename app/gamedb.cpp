// The games database: which exe is which game, its proper name, and where its icon and cover are.
//
// clipback fetches it itself; nothing is shipped with it. The list is Discord's public list of the games it can
// detect (about 25,000, each with its exe names, the folders they sit in and, for an exe many programs share, the
// command line that tells them apart: javaw.exe is Minecraft only when it runs Minecraft's main class). Icons come
// from Discord's image server and covers from Steam's when the game is on Steam. A game with neither, or one not in
// the list at all, is looked up by name on clipback's art service (api.clipback.workers.dev, which asks SteamGridDB
// and IGDB; "art_service" in settings.txt names another, or none). All of it is kept under <local>\gamedb and the
// list is fetched again after a week:
//   detectable.json   the list as fetched
//   games.tsv         the same, one game a line, which is what is loaded:
//                     id, name, icon, cover, Steam id, other names (|), exes (|; "!" first marks a launcher,
//                     ">" a shared exe; the command line it needs follows a \x1f)
//   art\<id>-icon.png, art\<id>-art.jpg       fetched the first time something shows them; an empty file means
//                                             there is none, and it is asked for again after a week
//   art\<id>-found.json                       what the art service has for the game; empty = it has nothing
//   resolved\<hash>.json                      what the art service said a running Minecraft's modpack
//                                             (mcpack.cpp) or an exe the list doesn't know (storeids.cpp) is
// A game the list doesn't have goes by "n" and a hash of its name's letters as its id.
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

enum { GAMEDB_LOADED = 1, GAMEDB_ART = 2, GAMEDB_RESOLVED = 3 };
enum GameArt { ART_ICON, ART_COVER };

struct GameDbShared {
    SRWLOCK    lock;
    GameIndex *index;
    HANDLE     thread, wake;
    volatile LONG stop;
    bool       online;      // may fetch
    char       service[128]; // the art service's host; "" = don't ask it
    struct { char id[24], hash[40], name[128]; u32 steam; GameArt kind; } want[64]; // art asked for, not fetched yet
    i32        want_count;
    // What the art service was asked to name (a modpack, or an exe the list doesn't know), by a key for the
    // question: being asked (body set) or answered (name; "" = nothing it could name; sure = by a store id or the
    // exe table, not just a name). The oldest answer makes room for a new question.
    struct { u64 key; char *body; char name[128]; u8 kind; bool answered, sure; } asks[128];
    i32        ask_next;
};
static GameDbShared g_gamedb;

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
    SHCreateDirectoryExW(0, path_join(d, L"resolved").s, 0);
    return d;
}

static Path gamedb_art_file(const char *id, GameArt kind)
{
    wchar_t name[64];
    swprintf(name, COUNT(name), L"art\\%hs-%s", id, kind == ART_ICON ? L"icon.png" : L"art.jpg");
    return path_join(gamedb_dir(), name);
}

// ---------------------------------------------------------------------------------------------------------------
// Fetching: one GET over https, or a POST of `post` when it is given. The answer goes to a file (through a .part,
// so a half-fetched file is never read) or, with no file, into memory: *body, malloc'd and 0-terminated, which the
// caller frees. `headers` are sent with it ("" = none). Returns the status, 0 if there was no answer; the body is
// only kept for a 200.

static u32 http_get(const wchar_t *host, const wchar_t *path, const wchar_t *headers, const Path *file,
                    char **body = 0, i64 *body_len = 0, const char *post = 0)
{
    bool ok = false;
    u32 answer = 0;
    Path part = {};
    if (file) {
        part = *file;
        wcsncat(part.s, L".part", MAX_PATH - wcslen(part.s) - 1);
    }
    char *mem = 0;
    i64 mem_len = 0, mem_cap = 0;
    HINTERNET session = WinHttpOpen(L"clipback", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, 0, 0, 0);
    HINTERNET conn = session ? WinHttpConnect(session, host, INTERNET_DEFAULT_HTTPS_PORT, 0) : 0;
    HINTERNET req = conn ? WinHttpOpenRequest(conn, post ? L"POST" : L"GET", path, 0, 0, 0, WINHTTP_FLAG_SECURE) : 0;
    DWORD post_len = post ? (DWORD)strlen(post) : 0;
    if (req) {
        DWORD unzip = WINHTTP_DECOMPRESSION_FLAG_ALL;
        WinHttpSetOption(req, WINHTTP_OPTION_DECOMPRESSION, &unzip, sizeof unzip);
        WinHttpSetTimeouts(req, 10000, 10000, 20000, 30000);
        DWORD status = 0, len = sizeof status;
        if (WinHttpSendRequest(req, headers && headers[0] ? headers : 0, headers && headers[0] ? (DWORD)-1L : 0,
                               (void *)post, post_len, post_len, 0) &&
            WinHttpReceiveResponse(req, 0) &&
            WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, 0, &status, &len, 0))
            answer = status;
        if (answer == 200) {
            FILE *f = file ? _wfopen(part.s, L"wb") : 0;
            if (f || !file) {
                static thread_local char buf[65536];
                ok = true;
                for (;;) {
                    DWORD got = 0;
                    if (!WinHttpReadData(req, buf, sizeof buf, &got)) { ok = false; break; }
                    if (!got) break;
                    if (f) {
                        if (fwrite(buf, 1, got, f) != got) { ok = false; break; }
                    } else {
                        if (mem_len + got + 1 > (1 << 20)) { ok = false; break; } // no answer here is that big
                        if (mem_len + got + 1 > mem_cap) {
                            mem_cap = (mem_len + got + 1) * 2;
                            mem = (char *)realloc(mem, (size_t)mem_cap);
                        }
                        memcpy(mem + mem_len, buf, got);
                        mem_len += got;
                    }
                    if (g_gamedb.stop) { ok = false; break; }
                }
                if (f) fclose(f);
            }
        } else {
            DEV_LOG("gamedb: %ls%ls answered %u (error %d)", host, path, answer, (int)GetLastError());
        }
    }
    if (req) WinHttpCloseHandle(req);
    if (conn) WinHttpCloseHandle(conn);
    if (session) WinHttpCloseHandle(session);
    if (file) {
        if (ok) ok = MoveFileExW(part.s, file->s, MOVEFILE_REPLACE_EXISTING) != 0;
        else DeleteFileW(part.s);
    }
    if (body && ok) {
        if (!mem) mem = (char *)malloc(1);
        mem[mem_len] = 0;
        *body = mem;
        if (body_len) *body_len = mem_len;
        mem = 0;
    }
    free(mem);
    return answer == 200 && !ok ? 0 : answer;
}

static bool http_get_file(const wchar_t *host, const wchar_t *path, const Path &out)
{
    return http_get(host, path, L"", &out) == 200;
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

// To the value at a path of keys through objects ("modpack.project_id"). False if there is none.
static bool j_find(Json *j, const char *path)
{
    for (const char *part = path;;) {
        const char *dot = strchr(part, '.');
        size_t pl = dot ? (size_t)(dot - part) : strlen(part);
        if (!j_eat(j, '{')) return false;
        bool found = false;
        while (!found && !j_eat(j, '}') && j->p < j->end) {
            char k[64];
            if (!j_string(j, k, sizeof k)) return false;
            j_eat(j, ':');
            if (strlen(k) == pl && !strncmp(k, part, pl)) found = true;
            else { j_skip(j); j_eat(j, ','); }
        }
        if (!found) return false;
        if (!dot) { j_ws(j); return true; }
        part = dot + 1;
    }
}

// A string or a number at a path, as text. False if there is none, or it is null or an object.
static bool j_get(const char *text, i64 len, const char *path, char *out, i32 cap)
{
    Json j = {text, text + len};
    out[0] = 0;
    if (!j_find(&j, path) || j.p >= j.end) return false;
    if (*j.p == '"') return j_string(&j, out, cap) && out[0];
    if (*j.p == '{' || *j.p == '[') return false;
    i32 n = 0;
    while (j.p < j.end && *j.p != ',' && *j.p != '}' && *j.p != ']' && (u8)*j.p > ' ' && n < cap - 1) out[n++] = *j.p++;
    out[n] = 0;
    return n > 0 && strcmp(out, "null");
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
    char *text = read_file(json.s, &size);
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
    char *text = read_file(tsv.s, &size);
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
        if (n) ix->names[ix->name_count++] = {fnv64(letters, n), i};
        for (const char *a = r.aliases; *a;) {
            const char *e = strchr(a, '|');
            char one[128];
            snprintf(one, sizeof one, "%.*s", (int)(e ? e - a : strlen(a)), a);
            n = gdb_letters(one, letters, sizeof letters);
            if (n) ix->names[ix->name_count++] = {fnv64(letters, n), i};
            a = e ? e + 1 : a + strlen(a);
        }
        for (const char *x = r.exes; *x;) {
            const char *e = strchr(x, '|');
            i32 len = (i32)(e ? e - x : strlen(x)), fl = 0;
            const char *file = gdb_exe_file(x, len, &fl);
            if (fl) ix->exes[ix->exe_count++] = {fnv64(file, fl), i};
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
    i32 k = ix && n ? gdb_find(ix->names, ix->name_count, fnv64(letters, n)) : -1;
    // The same letters can name a few entries (a game and its test server); the one named exactly so wins
    for (; k >= 0 && k < ix->name_count && ix->names[k].hash == fnv64(letters, n); k++) {
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
    char file[96], where[MAX_PATH * 2], seen[512];
    static thread_local char cmd[32768]; // a modded Minecraft's is long: every library is on it
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
    u64 hash = fnv64(file, fl);
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
        static thread_local u8 buf[65552]; // the most a UNICODE_STRING holds, and itself
        ULONG got = 0;
        if (query(p, 60, buf, sizeof buf, &got) >= 0) {
            struct { USHORT len, max; wchar_t *text; } *us = (decltype(us))buf;
            if (us->text && us->len) {
                static thread_local wchar_t wide[32768];
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

static void sessions_game_renamed(const char *exe); // sessions.cpp
static bool mc_clues(const char *cmdline, char *out, i32 cap); // mcpack.cpp

enum AskKind : u8 { ASK_PACK = 1, ASK_EXE = 2, ASK_CONFIRM = 3 };

// What the art service said to a question: 1 = it has answered (name "" if it named nothing), 0 = it is being
// asked, -1 = it hasn't been asked (or its answer made room for others since).
static i32 gamedb_asked(u64 key, char *name, i32 cap, bool *sure)
{
    name[0] = 0;
    *sure = false;
    i32 state = -1;
    AcquireSRWLockShared(&g_gamedb.lock);
    for (i32 i = 0; i < COUNT(g_gamedb.asks) && state < 0; i++) {
        auto &q = g_gamedb.asks[i];
        if (q.key != key || (!q.body && !q.answered)) continue;
        state = q.answered ? 1 : 0;
        if (q.answered) {
            snprintf(name, cap, "%s", q.name);
            *sure = q.sure;
        }
    }
    ReleaseSRWLockShared(&g_gamedb.lock);
    return state;
}

// Asks the art service a question (POST /v1/resolve with body), unless it is being asked already. With no body
// there is nothing to ask, and that is kept as its answer so the clues aren't looked for again.
static void gamedb_ask(u64 key, u8 kind, const char *body)
{
    AcquireSRWLockExclusive(&g_gamedb.lock);
    i32 at = -1;
    for (i32 i = 0; i < COUNT(g_gamedb.asks) && at < 0; i++)
        if (g_gamedb.asks[i].key == key && (g_gamedb.asks[i].body || g_gamedb.asks[i].answered)) at = i;
    bool ask = false;
    if (at < 0) {
        for (i32 tries = 0; tries < COUNT(g_gamedb.asks) && at < 0; tries++) { // one being asked keeps its place
            i32 i = g_gamedb.ask_next++ % COUNT(g_gamedb.asks);
            if (!g_gamedb.asks[i].body) at = i;
        }
        if (at >= 0) {
            auto &q = g_gamedb.asks[at];
            q.key = key;
            q.kind = kind;
            q.name[0] = 0;
            q.sure = false;
            q.body = body ? _strdup(body) : 0;
            q.answered = !q.body;
            ask = q.body != 0;
        }
    }
    ReleaseSRWLockExclusive(&g_gamedb.lock);
    if (ask) SetEvent(g_gamedb.wake);
}

// The modpack a running Minecraft is, from its command line: true with its name once the art service has named
// it. Until then it is asked (GAMEDB_RESOLVED says when it has answered) and this is false, as it is for a
// Minecraft with no pack the service could name. Main thread.
static bool gamedb_pack_name(const char *cmdline, char *name, i32 cap)
{
    name[0] = 0;
    if (!g_gamedb.online || !g_gamedb.service[0]) return false;
    static char body[12288];
    if (!mc_clues(cmdline, body, sizeof body)) return false;
    u64 key = fnv64(body, strlen(body));
    bool sure;
    i32 state = gamedb_asked(key, name, cap, &sure);
    if (state < 0) gamedb_ask(key, ASK_PACK, body);
    return state > 0 && name[0];
}

static bool exe_clues(const char *exe, const char *path, const char *name, bool game, char *out, i32 cap); // storeids.cpp

// The question asked about an exe where it is installed (and whether it was asked as a Game).
static u64 exe_ask_key(const AppInfo &a, bool game)
{
    char where[MAX_PATH + 8];
    i32 n = snprintf(where, sizeof where, "exe:%d:", game);
    for (const char *p = a.path; *p && n < (i32)sizeof where - 1; p++) where[n++] = (char)tolower((u8)*p);
    where[n] = 0;
    return fnv64(where, n);
}

// The game an exe the list doesn't know is, from the art service: true with its name once it has answered.
// *sure is set when it went by a store id or the exe table rather than a name. Only a game is asked about (one
// sorted as a Game, or installed by Steam or GOG), and only once for where it is installed. Main thread.
static bool gamedb_exe_name(const AppInfo &a, bool game, char *name, i32 cap, bool *sure)
{
    name[0] = 0;
    *sure = false;
    if (!g_gamedb.online || !g_gamedb.service[0] || !a.path[0]) return false;
    u64 key = exe_ask_key(a, game);
    i32 state = gamedb_asked(key, name, cap, sure);
    if (state < 0) {
        static char body[2048];
        // The name it was given by hand, else its exe's File description (a Unity game's is the game's name)
        const char *shown = a.name.s[0] && _stricmp(a.name.s, a.exe.s) ? a.name.s : a.desc.s;
        gamedb_ask(key, ASK_EXE, exe_clues(a.exe.s, a.path, shown, game, body, sizeof body) ? body : 0);
    }
    return state > 0 && name[0];
}

// "Help name games" (Options): a player named a Game the art service couldn't name by a store id or the exe table,
// so it is told (POST /v1/confirm), with the player's random voter id so they count once. When enough players name
// an exe in a folder the same game, the service names it so for everyone. Only with the player's say-so, and never
// for a game a store id decides. Main thread.
static void gamedb_confirm(const AppInfo &a, const char *name)
{
    Config &c = g_app.config;
    if (!c.share_names || !g_gamedb.online || !g_gamedb.service[0] || !a.path[0] || !name[0]) return;
    if (app_category(c, a) != CAT_GAME) return;
    GameInfo listed;
    if (gamedb_by_exe(a.exe.s, a.path, "", "", &listed)) return; // the games list has it
    char said[128];
    bool sure;
    if (gamedb_asked(exe_ask_key(a, true), said, sizeof said, &sure) == 1 && sure) return; // the service knows it
    static char body[2048];
    if (!exe_clues(a.exe.s, a.path, name, true, body, sizeof body) || strstr(body, "\"steam\":") || strstr(body, "\"gog\":"))
        return;
    bool hex = strlen(c.voter) == 32;
    for (const char *p = c.voter; *p && hex; p++) hex = (*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f');
    if (!hex) {
        u8 r[16];
        if (BCryptGenRandom(0, r, sizeof r, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) return;
        for (i32 i = 0; i < 16; i++) snprintf(c.voter + 2 * i, 3, "%02x", r[i]);
        config_save(c, g_app.paths);
    }
    size_t n = strlen(body);
    if (n < 2 || n + 48 >= sizeof body) return;
    snprintf(body + n - 1, sizeof body - n + 1, ",\"voter\":\"%s\"}", c.voter);
    gamedb_ask(fnv64(body, strlen(body)), ASK_CONFIRM, body);
    log_line("games: told the art service %s is %s", a.exe.s, name);
}

// Names an app from the database: its description becomes the game's name, and one that hasn't been sorted yet
// (and that no folder rule sorts) becomes a Game. An exe several games share is asked about while it runs: its
// command line decides, else its window naming a game in the list. One the list can't place goes by its exe's
// name (its File description is the runtime's, not a game's) until it is named by hand. Main thread. Returns true
// if something about it changed.
static bool gamedb_name_app(AppInfo *a)
{
    GameInfo g;
    bool shared = false, known = gamedb_by_exe(a->exe.s, a->path, "", "", &g, &shared);
    if (!known && shared) {
        static char cmd[32768];
        char title[256], pack[128];
        if (process_facts(a->exe.s, cmd, sizeof cmd, title, sizeof title)) {
            DEV_LOG("gamedb: %s is shared; its window is \"%s\", its command line ends \"%s\"", a->exe.s, title,
                    strlen(cmd) > 400 ? cmd + strlen(cmd) - 400 : cmd);
            known = gamedb_by_exe(a->exe.s, a->path, cmd, title, &g);
            // A Minecraft whose launcher installed a modpack is that pack, once the art service has named it
            if (gamedb_pack_name(cmd, pack, sizeof pack)) {
                g = {};
                str_set(g.name, pack);
                known = true;
            }
        }
        if (!known) {
            if (!a->desc.s[0]) return false;
            a->desc.s[0] = 0;
            sessions_game_renamed(a->exe.s);
            return true;
        }
    }
    // One the list doesn't have at all: the art service's name for it. What it found by a store id or the exe table
    // is a game; what it found only by a name is only used for one already sorted as a Game.
    if (!known && !shared) {
        bool game = app_category(g_app.config, *a) == CAT_GAME, sure;
        char named[128];
        if (gamedb_exe_name(*a, game, named, sizeof named, &sure) && (sure || game)) {
            g = {};
            str_set(g.name, named);
            known = true;
        }
    }
    if (!known) return false;
    bool changed = false;
    if (strcmp(a->desc.s, g.name)) {
        snprintf(a->desc.s, sizeof a->desc.s, "%s", g.name);
        changed = true;
        sessions_game_renamed(a->exe.s);
    }
    if (a->category == CAT_NONE && !g.launcher && app_category(g_app.config, *a) == CAT_NONE) {
        a->category = CAT_GAME;
        log_line("%s is %s: sorted as a game", a->exe.s, g.name);
        changed = true;
    }
    return changed;
}

// ---------------------------------------------------------------------------------------------------------------
// The art service: art for the games Discord's list has none for, from SteamGridDB and IGDB by way of clipback's
// own Worker (clipback-api), which holds the keys. Two calls, both GET and signed:
//   /v1/search?q=<name>  -> {"games":[{"id":"s5367951","name":"...","year":2017}, ...]}
//   /v1/art/<id>         -> {"id":..,"grid":{"url":..,"thumb":..,"w":..,"h":..},"hero":..,"logo":..,"icon":..},
//                           a kind it has none of being null
// The signature only keeps the service from being an open relay for its keys, so the key it is made with is no
// secret.

static const char ART_SERVICE_KEY[] = "REDACTED-OLD-SERVICE-KEY";

// HMAC-SHA256 of msg, in hex.
static bool hmac_sha256_hex(const char *key, const char *msg, char out[65])
{
    BCRYPT_ALG_HANDLE alg = 0;
    BCRYPT_HASH_HANDLE h = 0;
    u8 mac[32];
    bool ok = BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, 0, BCRYPT_ALG_HANDLE_HMAC_FLAG)) &&
              BCRYPT_SUCCESS(BCryptCreateHash(alg, &h, 0, 0, (PUCHAR)key, (ULONG)strlen(key), 0)) &&
              BCRYPT_SUCCESS(BCryptHashData(h, (PUCHAR)msg, (ULONG)strlen(msg), 0)) &&
              BCRYPT_SUCCESS(BCryptFinishHash(h, mac, sizeof mac, 0));
    if (h) BCryptDestroyHash(h);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    out[0] = 0;
    if (ok)
        for (i32 i = 0; i < 32; i++) snprintf(out + i * 2, 3, "%02x", mac[i]);
    return ok;
}

// The x-clipback header for a path (with its query, as sent): "<unix seconds>.<hmac of "seconds\npath">", and
// for a POST "seconds\npath\nbody".
static void art_service_sign(const char *path, i64 seconds, char *out, i32 cap, const char *post = 0)
{
    static thread_local char msg[16384];
    char mac[65];
    snprintf(msg, sizeof msg, post ? "%lld\n%s\n%s" : "%lld\n%s", seconds, path, post ? post : "");
    hmac_sha256_hex(ART_SERVICE_KEY, msg, mac);
    snprintf(out, cap, "%lld.%s", seconds, mac);
}

// A name made safe for a query: every byte but A-Z a-z 0-9 - _ . ~ as %XX.
static void url_encode(const char *in, char *out, i32 cap)
{
    i32 n = 0;
    for (; *in && n < cap - 4; in++) {
        u8 c = (u8)*in;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out[n++] = (char)c;
        else n += snprintf(out + n, cap - n, "%%%02X", c);
    }
    out[n] = 0;
}

// A signed GET to the service, or a POST of the JSON `post`. Returns the status (0 = no answer); the body, for a
// 200, in *body.
static u32 art_service_get(const char *path, char **body, i64 *len, const char *post = 0)
{
    wchar_t host[128], wpath[1024], header[320];
    utf8_to_wide(g_gamedb.service, host, COUNT(host));
    utf8_to_wide(path, wpath, COUNT(wpath));
    for (i32 attempt = 0;; attempt++) {
        char sig[128];
        FILETIME now;
        GetSystemTimeAsFileTime(&now);
        i64 unix_seconds = (((i64)now.dwHighDateTime << 32 | now.dwLowDateTime) - 116444736000000000ll) / 10000000;
        art_service_sign(path, unix_seconds, sig, sizeof sig, post);
        swprintf(header, COUNT(header), L"x-clipback: %hs\r\n%s", sig, post ? L"content-type: application/json\r\n" : L"");
        u32 status = http_get(host, wpath, header, 0, body, len, post);
        if (status != 429 || attempt || g_gamedb.stop) return status;
        // Asked too often (it allows 30 a minute): wait the minute out, once
        for (i32 s = 0; s < 61 && !g_gamedb.stop; s++) Sleep(1000);
    }
}

// From a search's answer, the id of the game that is this name: the one whose letters are the name's, else the
// first whose letters hold the name's or are held in them ("rustclient" finds "Rust"). False if none is.
static bool art_pick_search(const char *json, i64 len, const char *name, char *id, i32 cap)
{
    char want[128];
    i32 wl = gdb_letters(name, want, sizeof want);
    id[0] = 0;
    if (wl < 2) return false;
    bool exact = false;
    Json j = {json, json + len};
    if (!j_eat(&j, '{')) return false;
    while (!j_eat(&j, '}') && j.p < j.end) {
        char key[24];
        if (!j_string(&j, key, sizeof key)) break;
        j_eat(&j, ':');
        if (strcmp(key, "games") || !j_eat(&j, '[')) { j_skip(&j); j_eat(&j, ','); continue; }
        while (j_eat(&j, '{')) {
            char gid[24] = "", gname[128] = "", k[16];
            while (!j_eat(&j, '}') && j.p < j.end) {
                if (!j_string(&j, k, sizeof k)) break;
                j_eat(&j, ':');
                if (!strcmp(k, "id")) j_string(&j, gid, sizeof gid) || (j_skip(&j), 0);
                else if (!strcmp(k, "name")) j_string(&j, gname, sizeof gname) || (j_skip(&j), 0);
                else j_skip(&j);
                j_eat(&j, ',');
            }
            j_eat(&j, ',');
            char letters[128];
            i32 gl = gdb_letters(gname, letters, sizeof letters);
            if (!gid[0] || gl < 2 || exact) continue;
            if (!strcmp(letters, want)) { snprintf(id, cap, "%s", gid); exact = true; }
            else if (!id[0] && (strstr(letters, want) || strstr(want, letters))) snprintf(id, cap, "%s", gid);
        }
        j_eat(&j, ']');
        j_eat(&j, ',');
    }
    return id[0] != 0;
}

// From an art answer, the picture for an icon (its icon's thumb: a PNG, where the icon itself may be an .ico of
// many sizes) or a cover (its hero, a wide picture with no title on it, else its grid). False if it has neither.
static bool art_pick_url(const char *json, i64 len, GameArt kind, char *url, i32 cap)
{
    char found[2][512] = {"", ""}; // the first choice, the second
    Json j = {json, json + len};
    url[0] = 0;
    if (!j_eat(&j, '{')) return false;
    while (!j_eat(&j, '}') && j.p < j.end) {
        char key[16];
        if (!j_string(&j, key, sizeof key)) break;
        j_eat(&j, ':');
        i32 slot = kind == ART_ICON ? (!strcmp(key, "icon") ? 0 : -1)
                                    : (!strcmp(key, "hero") ? 0 : !strcmp(key, "grid") ? 1 : -1);
        if (slot < 0 || !j_eat(&j, '{')) { j_skip(&j); j_eat(&j, ','); continue; }
        while (!j_eat(&j, '}') && j.p < j.end) {
            char k[16];
            if (!j_string(&j, k, sizeof k)) break;
            j_eat(&j, ':');
            if (!strcmp(k, kind == ART_ICON ? "thumb" : "url")) j_string(&j, found[slot], sizeof found[slot]) || (j_skip(&j), 0);
            else j_skip(&j);
            j_eat(&j, ',');
        }
        j_eat(&j, ',');
    }
    const char *pick = found[0][0] ? found[0] : found[1];
    if (strncmp(pick, "https://", 8)) return false;
    snprintf(url, cap, "%s", pick);
    return true;
}

// An https address fetched to a file.
static bool http_get_url(const char *url, const Path &out)
{
    if (strncmp(url, "https://", 8)) return false;
    const char *host = url + 8, *slash = strchr(host, '/');
    if (!slash || slash == host) return false;
    wchar_t whost[128], wpath[1024];
    char h[128];
    snprintf(h, sizeof h, "%.*s", (int)(slash - host), host);
    utf8_to_wide(h, whost, COUNT(whost));
    utf8_to_wide(slash, wpath, COUNT(wpath));
    return http_get_file(whost, wpath, out);
}

static bool file_older_than(const WIN32_FILE_ATTRIBUTE_DATA &fa, i64 seconds)
{
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    i64 age = (((i64)now.dwHighDateTime << 32 | now.dwLowDateTime) -
               ((i64)fa.ftLastWriteTime.dwHighDateTime << 32 | fa.ftLastWriteTime.dwLowDateTime)) / 10000000;
    return age > seconds;
}

// Where the service has a game's icon or cover, looked up by its name. What it has is kept in <id>-found.json, so
// the icon and the cover take one lookup between them. False if it has none, or couldn't be asked.
static bool art_service_find(const char *id, const char *name, GameArt kind, char *url, i32 cap)
{
    if (!g_gamedb.service[0] || !name[0]) return false;
    wchar_t fname[64];
    swprintf(fname, COUNT(fname), L"art\\%hs-found.json", id);
    Path found = path_join(gamedb_dir(), fname);
    WIN32_FILE_ATTRIBUTE_DATA fa;
    // Nothing found is asked about again after a week, what was found after a month
    bool have = GetFileAttributesExW(found.s, GetFileExInfoStandard, &fa) &&
                !file_older_than(fa, (fa.nFileSizeLow ? 30 : 7) * 24 * 3600);
    if (!have) {
        // A game clipback only knows by its exe ("someapp.exe") is looked for without the ".exe"
        char q[128], path[512], enc[400], sid[24] = "";
        snprintf(q, sizeof q, "%s", name);
        size_t ql = strlen(q);
        if (ql > 4 && !_stricmp(q + ql - 4, ".exe")) q[ql - 4] = 0;
        url_encode(q, enc, sizeof enc);
        snprintf(path, sizeof path, "/v1/search?q=%s", enc);
        char *body = 0;
        i64 len = 0;
        u32 status = art_service_get(path, &body, &len);
        if (status != 200) { DEV_LOG("gamedb: art service search for %s answered %u", name, status); return false; }
        bool match = art_pick_search(body, len, q, sid, sizeof sid);
        free(body);
        body = 0;
        len = 0;
        if (match) {
            snprintf(path, sizeof path, "/v1/art/%s", sid);
            status = art_service_get(path, &body, &len);
            if (status != 200) { DEV_LOG("gamedb: art service art for %s answered %u", sid, status); return false; }
        }
        FILE *f = _wfopen(found.s, L"wb"); // empty: it has nothing by that name
        if (f) {
            if (body) fwrite(body, 1, (size_t)len, f);
            fclose(f);
        }
        DEV_LOG("gamedb: art service has %s for %s", match ? sid : "nothing", name);
        free(body);
    }
    i64 size = 0;
    char *text = read_file(found.s, &size);
    bool ok = text && size > 0 && art_pick_url(text, size, kind, url, cap);
    free(text);
    return ok;
}

// A game by its name, for its art: the list's entry, else one made up from the name, whose art only the art
// service can find.
static bool gamedb_for_art(const char *name, GameInfo *out)
{
    if (gamedb_by_name(name, out)) return true;
    AcquireSRWLockShared(&g_gamedb.lock);
    bool loaded = g_gamedb.index != 0;
    ReleaseSRWLockShared(&g_gamedb.lock);
    if (!loaded) return false; // it may be in the list once the list is there
    char letters[128];
    i32 n = gdb_letters(name, letters, sizeof letters);
    if (n < 2) return false;
    *out = {};
    snprintf(out->id, sizeof out->id, "n%016llx", (unsigned long long)fnv64(letters, n));
    str_set(out->name, name);
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// Art. A file that is there is used; one that isn't is asked for and arrives later (GAMEDB_ART).

// Where a game's icon or cover is, if it has been fetched. If it hasn't, it is asked for, *coming set and false
// returned. One found to have none is asked for again after a week.
static bool gamedb_art(const GameInfo &g, GameArt kind, Path *out, bool *coming = 0)
{
    if (coming) *coming = false;
    const char *hash = kind == ART_ICON ? g.icon : g.cover;
    if (!hash[0] && !(kind == ART_COVER && g.steam) && !g_gamedb.service[0]) return false;
    *out = gamedb_art_file(g.id, kind);
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetFileAttributesExW(out->s, GetFileExInfoStandard, &fa)) {
        if (fa.nFileSizeLow > 0) return true;
        if (!file_older_than(fa, 7 * 24 * 3600)) return false; // empty: there is none
    }
    if (!g_gamedb.online) return false;
    AcquireSRWLockExclusive(&g_gamedb.lock);
    bool queued = false;
    for (i32 i = 0; i < g_gamedb.want_count && !queued; i++)
        queued = g_gamedb.want[i].kind == kind && !strcmp(g_gamedb.want[i].id, g.id);
    if (!queued && g_gamedb.want_count < COUNT(g_gamedb.want)) {
        auto &w = g_gamedb.want[g_gamedb.want_count++];
        str_set(w.id, g.id);
        str_set(w.hash, hash);
        str_set(w.name, g.name);
        w.steam = g.steam;
        w.kind = kind;
    }
    ReleaseSRWLockExclusive(&g_gamedb.lock);
    SetEvent(g_gamedb.wake);
    if (coming) *coming = true;
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
        if (!ok && !g_gamedb.stop) {
            char found[512];
            if (art_service_find(w.id, w.name, w.kind, found, sizeof found)) ok = http_get_url(found, file);
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
// Naming by the art service: the clues mcpack.cpp gathers for a modpack, and storeids.cpp for an exe the list
// doesn't know, go to the service's POST /v1/resolve, which answers
//   {"game":{"id","name","year"}|null, "pack":{"platform","project","name","version"}?, "matched_by",
//    "confidence", "art":{"grid","hero","logo","icon"}}

// Where the art is in a resolve answer's text.
static void answer_art(const char *json, i64 len, const char **art, i64 *art_len)
{
    *art = 0;
    *art_len = 0;
    Json j = {json, json + len};
    if (j_find(&j, "art") && j.p < j.end && *j.p == '{') {
        const char *start = j.p;
        j_skip(&j);
        *art = start;
        *art_len = j.p - start;
    }
}

// From a resolve answer, the pack's name and where its art is in the text. False unless it names a pack its
// launcher installed: plain Minecraft stays Minecraft.
static bool pack_from_answer(const char *json, i64 len, char *name, i32 cap, const char **art, i64 *art_len)
{
    name[0] = 0;
    answer_art(json, len, art, art_len);
    char how[32];
    if (!j_get(json, len, "matched_by", how, sizeof how) || strcmp(how, "launcher_record")) return false;
    return j_get(json, len, "pack.name", name, cap);
}

// From a resolve answer about an exe, the game's name and where its art is. Sure if it went by a store id or the
// exe table; one found by a name is only taken when the name was the game's exactly. False if it named none.
static bool exe_from_answer(const char *json, i64 len, char *name, i32 cap, bool *sure, const char **art, i64 *art_len)
{
    name[0] = 0;
    *sure = false;
    answer_art(json, len, art, art_len);
    char how[32], conf[16];
    if (!j_get(json, len, "matched_by", how, sizeof how) || !j_get(json, len, "game.name", name, cap)) return false;
    *sure = !strcmp(how, "store_id") || !strcmp(how, "exe_table");
    if (*sure) return true;
    if (!strcmp(how, "name_search") && j_get(json, len, "confidence", conf, sizeof conf) && atof(conf) >= 0.9) return true;
    name[0] = 0;
    return false;
}

// Asks what gamedb_pack_name and gamedb_exe_name want named. What the service said is kept for a month, and the
// art goes where art_service_find looks for it, so the icon and the cover take no more questions.
static void gamedb_fetch_asks()
{
    for (;;) {
        AcquireSRWLockExclusive(&g_gamedb.lock);
        i32 at = -1;
        for (i32 i = 0; i < COUNT(g_gamedb.asks) && at < 0; i++)
            if (g_gamedb.asks[i].body && !g_gamedb.asks[i].answered) at = i;
        u64 key = at >= 0 ? g_gamedb.asks[at].key : 0;
        u8 kind = at >= 0 ? g_gamedb.asks[at].kind : 0;
        const char *body = at >= 0 ? g_gamedb.asks[at].body : 0; // its slot isn't reused while it has a body
        ReleaseSRWLockExclusive(&g_gamedb.lock);
        if (at < 0) return;

        if (kind == ASK_CONFIRM) { // a vote: nothing to keep from what it says
            char *said = 0;
            i64 said_len = 0;
            u32 status = art_service_get("/v1/confirm", &said, &said_len, body);
            DEV_LOG("gamedb: art service confirm answered %u: %.*s", status, said ? (i32)(said_len < 200 ? said_len : 200) : 0, said ? said : "");
            free(said);
            AcquireSRWLockExclusive(&g_gamedb.lock);
            auto &q = g_gamedb.asks[at];
            q.answered = true;
            free(q.body);
            q.body = 0;
            ReleaseSRWLockExclusive(&g_gamedb.lock);
            if (g_gamedb.stop) return;
            continue;
        }

        wchar_t fname[64];
        swprintf(fname, COUNT(fname), L"resolved\\%016llx.json", (unsigned long long)key);
        Path file = path_join(gamedb_dir(), fname);
        WIN32_FILE_ATTRIBUTE_DATA fa;
        char *text = 0;
        i64 len = 0;
        if (GetFileAttributesExW(file.s, GetFileExInfoStandard, &fa) && !file_older_than(fa, 30 * 24 * 3600))
            text = read_file(file.s, &len);
        bool asked = false;
        if (!text || len <= 0) {
            free(text);
            text = 0;
            u32 status = art_service_get("/v1/resolve", &text, &len, body);
            if (status == 200 && text) {
                asked = true;
                FILE *f = _wfopen(file.s, L"wb");
                if (f) {
                    fwrite(text, 1, (size_t)len, f);
                    fclose(f);
                }
            } else {
                DEV_LOG("gamedb: art service resolve answered %u", status);
                free(text);
                text = 0;
            }
        }
        char name[128] = "";
        const char *art = 0;
        i64 art_len = 0;
        bool sure = false, named = false;
        if (text) {
            named = kind == ASK_PACK ? pack_from_answer(text, len, name, sizeof name, &art, &art_len)
                                     : exe_from_answer(text, len, name, sizeof name, &sure, &art, &art_len);
            if (kind == ASK_PACK) sure = named;
        }
        if (named && art) {
            GameInfo g;
            if (gamedb_for_art(name, &g) && g.id[0] == 'n') { // one the games list has gets its art from there
                wchar_t fn[64];
                swprintf(fn, COUNT(fn), L"art\\%hs-found.json", g.id);
                Path found = path_join(gamedb_dir(), fn);
                if (asked || GetFileAttributesW(found.s) == INVALID_FILE_ATTRIBUTES) {
                    FILE *f = _wfopen(found.s, L"wb");
                    if (f) {
                        fwrite(art, 1, (size_t)art_len, f);
                        fclose(f);
                    }
                }
            }
        }
        DEV_LOG("gamedb: %s %016llx is %s%s", kind == ASK_PACK ? "modpack" : "exe", (unsigned long long)key,
                name[0] ? name : "none the service could name", name[0] && !sure ? " (by its name)" : "");
        free(text);

        AcquireSRWLockExclusive(&g_gamedb.lock);
        auto &p = g_gamedb.asks[at];
        str_set(p.name, name);
        p.sure = sure;
        p.answered = true;
        free(p.body);
        p.body = 0;
        ReleaseSRWLockExclusive(&g_gamedb.lock);
        if (g_gamedb.stop) return;
        Event e = {};
        e.kind = EV_GAMEDB;
        e.a = GAMEDB_RESOLVED;
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
        gamedb_fetch_asks();
        gamedb_fetch_art();
        WaitForSingleObject(g_gamedb.wake, INFINITE);
    }
    return 0;
}

static void gamedb_start(bool online)
{
    g_gamedb.online = online;
    str_set(g_gamedb.service, g_app.config.art_service.s);
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
