// The games database: which exe is which game, its proper name, and where its icon and cover are.
//
// clipback fetches it itself; nothing is shipped with it. The list is Discord's public list of the games it can
// detect (about 25,000, each with its exe names, the folders they sit in and, for an exe many programs share, the
// command line that tells them apart: javaw.exe is Minecraft only when it runs Minecraft's main class). Icons come
// from Discord's image server and covers from Steam's when the game is on Steam. A game with neither, or one not in
// the list at all, is looked up by name on clipback's art service (api.clipback.workers.dev, which asks SteamGridDB
// and IGDB; "art_service" in settings.txt names another, or none). The service is always asked first. If it
// answers "not shared yet" (403), it only says which game an exe is, and the art comes from SteamGridDB with the
// player's own key ("sgdb_key" in settings.txt), if they set one. All of it is kept under <local>\gamedb and the
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
    HANDLE     find_thread, find_wake; // the searches a player types, on a thread of their own
    volatile LONG stop;
    bool       online;      // may fetch
    char       service[128]; // the art service's host; "" = don't ask it
    char       sgdb_key[72]; // the player's own SteamGridDB key; "" = none
    struct { char id[24], hash[40], name[128]; u32 steam; GameArt kind; } want[64]; // art asked for, not fetched yet
    i32        want_count;
    // What the art service was asked to name (a modpack, or an exe the list doesn't know), by a key for the
    // question: being asked (body set) or answered (name; "" = nothing it could name; sure = by a store id or the
    // exe table, not just a name). The oldest answer makes room for a new question.
    struct { u64 key; char *body; char name[128]; u8 kind; bool answered, sure; } asks[128];
    i32        ask_next;
    // A name typed when naming an app, looked for on the art service: what to look for (main thread), and what it
    // found for which (the worker).
    char       find_q[128], found_q[128];
    struct { char name[128]; i32 year; } found[8];
    i32        found_count;
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
    i64 mem_len = 0, mem_cap = 0, file_len = 0;
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
                        file_len += got;
                        if (file_len > (64 << 20)) { ok = false; break; } // no picture or list is that big: not kept
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

// How well a game's name answers a search, from its letters: 0 the same, 1 starts with it, 2 a word starts with it,
// 3 holds it, -1 not at all. `name` is the name as written, for its word starts.
static i32 gamedb_match_rank(const char *name, const char *own, const char *want, i32 wl)
{
    if (!strcmp(own, want)) return 0;
    if (!strncmp(own, want, wl)) return 1;
    for (const char *p = name; *p; p++) {
        if (isalnum((u8)*p) && (p == name || !isalnum((u8)p[-1]))) {
            char word[128];
            gdb_letters(p, word, sizeof word);
            if (!strncmp(word, want, wl)) return 2;
        }
    }
    return strstr(own, want) ? 3 : -1;
}

// Games in the list whose name answers a search, best first: by how well it matches, then the ones on Steam (the
// list's better-known games), then the shortest name. Fast enough to run on every key. Main thread.
static i32 gamedb_find_local(const char *q, char (*names)[128], i32 max)
{
    char want[128];
    i32 wl = gdb_letters(q, want, sizeof want);
    if (wl < 1 || max <= 0) return 0;
    struct Hit { i32 row, rank, len; bool steam; } hits[16];
    i32 n = 0, cap = max < (i32)COUNT(hits) ? max : (i32)COUNT(hits);
    auto better = [](const Hit &a, const Hit &b) {
        if (a.rank != b.rank) return a.rank < b.rank;
        if (a.steam != b.steam) return a.steam;
        return a.len < b.len;
    };
    AcquireSRWLockShared(&g_gamedb.lock);
    const GameIndex *ix = g_gamedb.index;
    for (i32 i = 0; ix && i < ix->row_count; i++) {
        const GameRow &r = ix->rows[i];
        char own[128];
        i32 ol = gdb_letters(r.name, own, sizeof own);
        if (ol < wl) continue;
        i32 rank = gamedb_match_rank(r.name, own, want, wl);
        if (rank < 0) continue;
        Hit h = {i, rank, ol, r.steam != 0};
        if (n == cap && !better(h, hits[n - 1])) continue;
        i32 at = n < cap ? n++ : n - 1;
        while (at > 0 && better(h, hits[at - 1])) {
            hits[at] = hits[at - 1];
            at--;
        }
        hits[at] = h;
    }
    for (i32 i = 0; i < n; i++) snprintf(names[i], 128, "%s", ix->rows[hits[i].row].name);
    ReleaseSRWLockShared(&g_gamedb.lock);
    return n;
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
// so it is told (POST /v1/confirm). When enough players name
// an exe in a folder the same game, the service names it so for everyone. Only with the player's say-so, and never
// for a game a store id decides. Main thread.
static void gamedb_vote(const AppInfo &a, const char *name);

static void gamedb_confirm(const AppInfo &a, const char *name)
{
    if (app_category(g_app.config, a) != CAT_GAME) return;
    gamedb_vote(a, name);
}

// Sends the vote that exe `a` is game `name`, under the same rules as gamedb_confirm, whatever it is sorted as.
static void gamedb_vote(const AppInfo &a, const char *name)
{
    Config &c = g_app.config;
    if (!c.share_names || !g_gamedb.online || !g_gamedb.service[0] || !a.path[0] || !name[0]) return;
    GameInfo listed;
    if (gamedb_by_exe(a.exe.s, a.path, "", "", &listed)) return; // the games list has it
    char said[128];
    bool sure;
    if (gamedb_asked(exe_ask_key(a, true), said, sizeof said, &sure) == 1 && sure) return; // the service knows it
    static char body[2048];
    if (!exe_clues(a.exe.s, a.path, name, true, body, sizeof body) || strstr(body, "\"steam\":") || strstr(body, "\"gog\":"))
        return;
    gamedb_ask(fnv64(body, strlen(body)), ASK_CONFIRM, body);
    log_line("games: told the art service %s is %s", a.exe.s, name);
}

// Browsers: a game played in one can't be told by its exe, so the service only counts the game.
static bool is_browser(const char *exe)
{
    static const char *const browsers[] = {"chrome.exe", "msedge.exe", "firefox.exe", "brave.exe", "opera.exe",
                                           "vivaldi.exe", "arc.exe", "zen.exe", "librewolf.exe", "waterfox.exe",
                                           "floorp.exe", "chromium.exe", "iexplore.exe", "thorium.exe"};
    for (auto b : browsers)
        if (!_stricmp(exe, b)) return true;
    return false;
}

// The program longest in front between from_us and to_us, from focus.tsv text ("us\texe" lines, oldest first).
// clipback itself and "nothing in front" don't count. False if nothing was.
static bool focus_longest(char *text, i64 from_us, i64 to_us, char *exe, i32 cap)
{
    struct Spent { char exe[64]; i64 us; } spent[32];
    i32 n = 0;
    i64 at = 0;
    char cur[64] = "";
    auto add = [&](i64 until) {
        i64 a = at > from_us ? at : from_us, b = until < to_us ? until : to_us;
        if (!cur[0] || b <= a || !_stricmp(cur, "clipback.exe")) return;
        i32 i = 0;
        while (i < n && strcmp(spent[i].exe, cur)) i++;
        if (i == n) {
            if (n == COUNT(spent)) return;
            str_set(spent[n].exe, cur);
            spent[n++].us = 0;
        }
        spent[i].us += b - a;
    };
    for (char *line = text; line && *line;) {
        char *next = strchr(line, '\n');
        if (next) *next++ = 0;
        char *tab = strchr(line, '\t');
        if (tab) {
            *tab = 0;
            i64 t = strtoll(line, 0, 10);
            char *e = tab + 1;
            size_t len = strlen(e);
            while (len && (e[len - 1] == '\r' || e[len - 1] == ' ')) e[--len] = 0;
            if (t > 0) {
                add(t);
                at = t;
                str_set(cur, e);
            }
        }
        line = next;
    }
    add(to_us);
    i32 best = -1;
    for (i32 i = 0; i < n; i++)
        if (best < 0 || spent[i].us > spent[best].us) best = i;
    if (best < 0) return false;
    snprintf(exe, cap, "%s", spent[best].exe);
    return true;
}

static void saved_load();                      // save.cpp
static void mc_put(char *out, i32 cap, i32 *n, const char *s); // mcpack.cpp
static Path lib_item_path(const LibItem &it);  // library.cpp

// A clip or session was given a game by hand: with "Help name games" on, the service is told the program that was
// in front during it is that game. Only for a file saved from this buffer while the buffer still covers it. Main
// thread.
static void gamedb_clip_game(const LibItem &it, const char *game)
{
    Config &c = g_app.config;
    if (!c.share_names || !g_gamedb.online || !g_gamedb.service[0] || !game[0]) return;
    char path[1024];
    wide_to_utf8(lib_item_path(it).s, path, sizeof path);
    saved_load();
    const SavedRange *r = 0;
    for (i32 i = g_app.saving.saved_count - 1; i >= 0 && !r; i--)
        if (!_stricmp(g_app.saving.saved[i].path, path)) r = &g_app.saving.saved[i];
    if (!r) return;
    FILE *f = _wfopen(path_join(c.buffer_dir, L"focus.tsv").s, L"rb");
    if (!f) return;
    static char text[256 * 1024];
    size_t got = fread(text, 1, sizeof text - 1, f);
    fclose(f);
    text[got] = 0;
    char exe[64];
    if (!focus_longest(text, r->from_us, r->to_us, exe, sizeof exe)) return;
    if (is_browser(exe)) {
        static char body[512];
        i32 n = snprintf(body, sizeof body, "{\"exe\":");
        mc_put(body, sizeof body, &n, exe);
        n += snprintf(body + n, sizeof body - n, ",\"name\":");
        mc_put(body, sizeof body, &n, game);
        n += snprintf(body + n, sizeof body - n, "}");
        if (n <= 0 || n >= (i32)sizeof body - 1) return;
        gamedb_ask(fnv64(body, strlen(body)), ASK_CONFIRM, body);
        log_line("games: told the art service a clip in %s is %s", exe, game);
        return;
    }
    AppInfo *a = 0;
    for (i32 i = 0; i < c.app_count && !a; i++)
        if (!strcmp(c.apps[i].exe.s, exe)) a = &c.apps[i];
    if (!a) return;
    AppCategory cat = app_category(c, *a);
    if (cat != CAT_GAME && cat != CAT_NONE) return; // a chat app or a tool in front says nothing of the game
    gamedb_vote(*a, game);
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

// The service allows an address 30 requests a minute. Art and names are asked for at 20 a minute at most, which
// leaves room for the searches a player types; past that they wait their turn (gamedb_thread looks again shortly).
// The worker and the search thread both count their requests here.
static u64 g_service_calls[20]; // when the last 20 requests were made (GetTickCount64)
static volatile LONG g_service_call_n;
static bool art_service_paced()
{
    u64 oldest = g_service_calls[(u32)g_service_call_n % COUNT(g_service_calls)];
    return g_gamedb.service[0] && oldest && GetTickCount64() - oldest < 60000;
}

// Whether a status says "couldn't be asked just now" (no answer, turned away, or the server's own trouble) rather
// than a plain no. 404 is one: the service gives it to a request whose clock is out.
static bool http_later(u32 status)
{
    return status == 0 || status == 403 || status == 404 || status == 429 || status >= 500;
}

// A signed GET to the service, or a POST of the JSON `post`. Returns the status (0 = no answer); the body, for a
// 200, in *body.
// A search being typed doesn't wait out a 429: the next key asks again.
static u32 art_service_get(const char *path, char **body, i64 *len, const char *post = 0, bool wait_out = true)
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
        g_service_calls[(u32)(InterlockedIncrement(&g_service_call_n) - 1) % COUNT(g_service_calls)] = GetTickCount64();
        u32 status = http_get(host, wpath, header, 0, body, len, post);
        if (status != 429 || attempt || !wait_out || g_gamedb.stop) return status;
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

// ---- SteamGridDB with the player's own key, for when the art service doesn't hand out art (403 "not shared yet")

// SteamGridDB's answer to a GET of /api/v2<path>, in *body. Returns the status (0 = none, or no key).
static u32 sgdb_get(const char *path, char **body, i64 *len)
{
    if (!g_gamedb.sgdb_key[0] || !g_gamedb.online) return 0;
    wchar_t wpath[1024], header[128];
    char full[1024];
    snprintf(full, sizeof full, "/api/v2%s", path);
    utf8_to_wide(full, wpath, COUNT(wpath));
    swprintf(header, COUNT(header), L"Authorization: Bearer %hs\r\n", g_gamedb.sgdb_key);
    return http_get(L"www.steamgriddb.com", wpath, header, 0, body, len);
}

// SteamGridDB's search answer ({"data":[{"id":5,"name":"...","release_date":1500000000}]}) as the art service gives
// it: {"games":[{"id":"s5","name":"...","year":2017}]}. Into out; false if it has no games list.
static bool sgdb_games(const char *json, i64 len, char *out, i32 cap)
{
    Json j = {json, json + len};
    if (!j_find(&j, "data") || !j_eat(&j, '[')) return false;
    i32 n = snprintf(out, cap, "{\"games\":[");
    bool first = true;
    while (j_eat(&j, '{') && n < cap - 300) {
        char name[128] = "", k[24];
        i64 id = 0, released = 0;
        while (!j_eat(&j, '}') && j.p < j.end) {
            if (!j_string(&j, k, sizeof k)) break;
            j_eat(&j, ':');
            j_ws(&j);
            if (!strcmp(k, "id")) id = _atoi64(j.p);
            else if (!strcmp(k, "release_date")) released = _atoi64(j.p);
            if (!strcmp(k, "name")) j_string(&j, name, sizeof name) || (j_skip(&j), 0);
            else j_skip(&j);
            j_eat(&j, ',');
        }
        j_eat(&j, ',');
        if (id <= 0 || !name[0]) continue;
        char esc[260];
        i32 e = 0;
        for (const char *p = name; *p && e < (i32)sizeof esc - 3; p++) {
            if (*p == '"' || *p == '\\') esc[e++] = '\\';
            esc[e++] = *p;
        }
        esc[e] = 0;
        i32 year = released > 0 ? 1970 + (i32)(released / 31556952) : 0;
        n += snprintf(out + n, cap - n, "%s{\"id\":\"s%lld\",\"name\":\"%s\",\"year\":%d}", first ? "" : ",", id, esc, year);
        first = false;
    }
    snprintf(out + n, cap - n, "]}");
    return true;
}

// The first picture in a SteamGridDB list answer ({"data":[{"url":"...","thumb":"..."}]}) as the art service gives
// one: {"url":"...","thumb":"..."}, or null.
static void sgdb_first(const char *json, i64 len, char *out, i32 cap)
{
    char url[512] = "", thumb[512] = "", k[16];
    Json j = {json, json + len};
    if (json && j_find(&j, "data") && j_eat(&j, '[') && j_eat(&j, '{')) {
        while (!j_eat(&j, '}') && j.p < j.end) {
            if (!j_string(&j, k, sizeof k)) break;
            j_eat(&j, ':');
            if (!strcmp(k, "url")) j_string(&j, url, sizeof url) || (j_skip(&j), 0);
            else if (!strcmp(k, "thumb")) j_string(&j, thumb, sizeof thumb) || (j_skip(&j), 0);
            else j_skip(&j);
            j_eat(&j, ',');
        }
    }
    if (strncmp(url, "https://", 8) || strchr(url, '"') || strchr(thumb, '"')) snprintf(out, cap, "null");
    else snprintf(out, cap, "{\"url\":\"%s\",\"thumb\":\"%s\"}", url, thumb[0] ? thumb : url);
}

// What SteamGridDB has for a game ("s5"), as the art service's /v1/art answers: {"id","grid","hero","icon"}, each
// SteamGridDB's first pick. Into a malloc'd *body; false if it couldn't be asked.
static bool sgdb_art(const char *sid, char **body, i64 *len)
{
    static const char *const kinds[3][2] = {{"grid", "grids"}, {"hero", "heroes"}, {"icon", "icons"}};
    char parts[3][1100];
    for (i32 i = 0; i < 3; i++) {
        char path[96], *text = 0;
        i64 tl = 0;
        snprintf(path, sizeof path, "/%s/game/%s", kinds[i][1], sid + 1);
        u32 status = sgdb_get(path, &text, &tl);
        if (status != 200 && status != 404) { free(text); return false; }
        sgdb_first(status == 200 ? text : 0, tl, parts[i], sizeof parts[i]);
        free(text);
    }
    i32 cap = 4096;
    *body = (char *)malloc(cap);
    if (!*body) return false;
    *len = snprintf(*body, cap, "{\"id\":\"%s\",\"grid\":%s,\"hero\":%s,\"icon\":%s}", sid, parts[0], parts[1], parts[2]);
    return true;
}

// A search for a name: the art service's, else, when it doesn't hand out art, SteamGridDB's with the player's key.
// Returns the status (403 with no key: nothing to search with); the answer, as the service gives it, in *body.
// `typing`: the player is still typing it, so the service keeps it in its edge cache only, not its store.
static u32 art_search(const char *q, char **body, i64 *len, bool typing = false)
{
    char enc[400], path[512];
    url_encode(q, enc, sizeof enc);
    snprintf(path, sizeof path, "/v1/search?q=%s%s", enc, typing ? "&typing=1" : "");
    u32 status = g_gamedb.service[0] ? art_service_get(path, body, len, 0, !typing) : 403;
    if (status != 403 || !g_gamedb.sgdb_key[0]) return status;
    free(*body);
    *body = 0;
    char *text = 0;
    i64 tl = 0;
    snprintf(path, sizeof path, "/search/autocomplete/%s", enc);
    status = sgdb_get(path, &text, &tl);
    if (status == 200) {
        i32 cap = 8192;
        *body = (char *)malloc(cap);
        if (*body && sgdb_games(text, tl, *body, cap)) *len = (i64)strlen(*body);
        else { free(*body); *body = 0; status = 0; }
    }
    free(text);
    return status;
}

// Where the service has a game's icon or cover, looked up by its name. What it has is kept in <id>-found.json, so
// the icon and the cover take one lookup between them. False if it has none, or couldn't be asked; *later is set
// when it couldn't be (the service is down, busy or not handing out art, or the PC is offline), so it is asked again
// next time rather than taken as none.
static bool art_service_find(const char *id, const char *name, GameArt kind, char *url, i32 cap, bool *later)
{
    *later = false;
    if ((!g_gamedb.service[0] && !g_gamedb.sgdb_key[0]) || !name[0]) return false;
    wchar_t fname[64];
    swprintf(fname, COUNT(fname), L"art\\%hs-found.json", id);
    Path found = path_join(gamedb_dir(), fname);
    WIN32_FILE_ATTRIBUTE_DATA fa;
    // Nothing found is asked about again after a week, what was found after a month
    bool have = GetFileAttributesExW(found.s, GetFileExInfoStandard, &fa) &&
                !file_older_than(fa, (fa.nFileSizeLow ? 30 : 7) * 24 * 3600);
    if (!have) {
        // A game clipback only knows by its exe ("someapp.exe") is looked for without the ".exe"
        char q[128], path[512], sid[24] = "";
        snprintf(q, sizeof q, "%s", name);
        size_t ql = strlen(q);
        if (ql > 4 && !_stricmp(q + ql - 4, ".exe")) q[ql - 4] = 0;
        char *body = 0;
        i64 len = 0;
        u32 status = art_search(q, &body, &len);
        *later = http_later(status);
        if (status != 200) { DEV_LOG("gamedb: art search for %s answered %u", name, status); free(body); return false; }
        bool match = art_pick_search(body, len, q, sid, sizeof sid);
        free(body);
        body = 0;
        len = 0;
        if (match) {
            snprintf(path, sizeof path, "/v1/art/%s", sid);
            status = g_gamedb.service[0] ? art_service_get(path, &body, &len) : 403;
            if (status == 403 && sid[0] == 's') { // not handed out: SteamGridDB's own, with the player's key
                free(body);
                body = 0;
                status = sgdb_art(sid, &body, &len) ? 200 : 0;
            }
            if (status != 200) {
                *later = http_later(status);
                DEV_LOG("gamedb: art for %s answered %u", sid, status);
                free(body);
                return false;
            }
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

// The games in a /v1/search answer, in its order: their names and years (0 = none). How many.
static i32 art_search_list(const char *json, i64 len, char (*names)[128], i32 *years, i32 max)
{
    i32 n = 0;
    Json j = {json, json + len};
    if (!j_eat(&j, '{')) return 0;
    while (!j_eat(&j, '}') && j.p < j.end) {
        char key[24];
        if (!j_string(&j, key, sizeof key)) break;
        j_eat(&j, ':');
        if (strcmp(key, "games") || !j_eat(&j, '[')) { j_skip(&j); j_eat(&j, ','); continue; }
        while (j_eat(&j, '{')) {
            char gname[128] = "", k[16];
            i32 year = 0;
            while (!j_eat(&j, '}') && j.p < j.end) {
                if (!j_string(&j, k, sizeof k)) break;
                j_eat(&j, ':');
                if (!strcmp(k, "name")) j_string(&j, gname, sizeof gname) || (j_skip(&j), 0);
                else if (!strcmp(k, "year")) { j_ws(&j); year = atoi(j.p); j_skip(&j); }
                else j_skip(&j);
                j_eat(&j, ',');
            }
            j_eat(&j, ',');
            if (gname[0] && n < max) {
                snprintf(names[n], 128, "%s", gname);
                years[n++] = year > 0 ? year : 0;
            }
        }
        j_eat(&j, ']');
        j_eat(&j, ',');
    }
    return n;
}

// Looks for the name typed when naming an app (gamedb_find_online asked for it) on the art service. Search thread.
static void gamedb_fetch_find()
{
    char q[128];
    AcquireSRWLockShared(&g_gamedb.lock);
    bool same = !strcmp(g_gamedb.find_q, g_gamedb.found_q);
    snprintf(q, sizeof q, "%s", g_gamedb.find_q);
    ReleaseSRWLockShared(&g_gamedb.lock);
    if (same || !q[0]) return;
    char *body = 0;
    i64 len = 0;
    u32 status = g_gamedb.online ? art_search(q, &body, &len, true) : 0;
    char names[8][128];
    i32 years[8], n = status == 200 && body ? art_search_list(body, len, names, years, 8) : 0;
    free(body);
    DEV_LOG("gamedb: art service search for \"%s\" answered %u, %d games", q, status, n);
    AcquireSRWLockExclusive(&g_gamedb.lock);
    snprintf(g_gamedb.found_q, sizeof g_gamedb.found_q, "%s", q);
    g_gamedb.found_count = n;
    for (i32 i = 0; i < n; i++) {
        snprintf(g_gamedb.found[i].name, sizeof g_gamedb.found[i].name, "%s", names[i]);
        g_gamedb.found[i].year = years[i];
    }
    ReleaseSRWLockExclusive(&g_gamedb.lock);
}

// Games the art service has by a name, for naming an app: asks for them (the worker looks while this is called
// again), and once it has them, copies them out. -1 = still looking, else how many (0 when it couldn't be asked).
// Main thread.
static i32 gamedb_find_online(const char *q, char (*names)[128], i32 *years, i32 max)
{
    if (!q[0] || !g_gamedb.online || (!g_gamedb.service[0] && !g_gamedb.sgdb_key[0]) || !g_gamedb.find_wake) return 0;
    i32 n = -1;
    bool ask = false;
    AcquireSRWLockExclusive(&g_gamedb.lock);
    if (strcmp(g_gamedb.find_q, q)) {
        snprintf(g_gamedb.find_q, sizeof g_gamedb.find_q, "%s", q);
        ask = true;
    }
    if (!strcmp(g_gamedb.found_q, q)) {
        n = 0;
        for (; n < g_gamedb.found_count && n < max; n++) {
            snprintf(names[n], 128, "%s", g_gamedb.found[n].name);
            years[n] = g_gamedb.found[n].year;
        }
    }
    ReleaseSRWLockExclusive(&g_gamedb.lock);
    if (ask) SetEvent(g_gamedb.find_wake);
    return n;
}

// Custom games: names typed in Change game rather than picked from the library's or the art service's games. Nothing
// looks up art or an icon for one. <config_dir>\custom_games.txt holds one name a line. Any thread.
static struct {
    SRWLOCK lock;
    char    names[256][64];
    i32     count;
    bool    loaded;
} g_custom_games = {SRWLOCK_INIT};

static void custom_games_load_locked()
{
    if (g_custom_games.loaded) return;
    g_custom_games.loaded = true;
    if (!g_app.paths.config_dir.s[0]) return;
    FILE *f = _wfopen(path_join(g_app.paths.config_dir, L"custom_games.txt").s, L"rb");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof line, f) && g_custom_games.count < COUNT(g_custom_games.names)) {
        char *t = trim(line);
        if (*t) str_set(g_custom_games.names[g_custom_games.count++], t);
    }
    fclose(f);
}

static i32 custom_games_find_locked(const char *name)
{
    for (i32 i = 0; i < g_custom_games.count; i++)
        if (!_stricmp(g_custom_games.names[i], name)) return i;
    return -1;
}

static bool game_is_custom(const char *name)
{
    AcquireSRWLockExclusive(&g_custom_games.lock);
    custom_games_load_locked();
    bool found = custom_games_find_locked(name) >= 0;
    ReleaseSRWLockExclusive(&g_custom_games.lock);
    return found;
}

// Marks a name a custom game, or not.
static void game_set_custom(const char *name, bool custom)
{
    AcquireSRWLockExclusive(&g_custom_games.lock);
    custom_games_load_locked();
    i32 at = custom_games_find_locked(name);
    bool change = custom != (at >= 0) && (!custom || g_custom_games.count < COUNT(g_custom_games.names));
    if (change) {
        if (custom) str_set(g_custom_games.names[g_custom_games.count++], name);
        else {
            memmove(&g_custom_games.names[at], &g_custom_games.names[at + 1],
                    (g_custom_games.count - at - 1) * sizeof g_custom_games.names[0]);
            g_custom_games.count--;
        }
        if (g_app.paths.config_dir.s[0]) {
            FILE *f = _wfopen(path_join(g_app.paths.config_dir, L"custom_games.txt").s, L"wb");
            if (f) {
                for (i32 i = 0; i < g_custom_games.count; i++) fprintf(f, "%s\r\n", g_custom_games.names[i]);
                fclose(f);
            }
        }
    }
    ReleaseSRWLockExclusive(&g_custom_games.lock);
    if (change) log_line("games: %s is %s", name, custom ? "a custom game" : "not a custom game");
}

// A game by its name, for its art: the list's entry, else one made up from the name, whose art only the art
// service can find. None for a custom game.
static bool gamedb_for_art(const char *name, GameInfo *out)
{
    if (game_is_custom(name)) return false;
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
    if (!hash[0] && !(kind == ART_COVER && g.steam) && !g_gamedb.service[0] && !g_gamedb.sgdb_key[0]) return false;
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

// False if some is left for later, the service having been asked enough for now.
static bool gamedb_fetch_art()
{
    for (;;) {
        AcquireSRWLockExclusive(&g_gamedb.lock);
        if (!g_gamedb.want_count) { ReleaseSRWLockExclusive(&g_gamedb.lock); return true; }
        auto w = g_gamedb.want[0];
        ReleaseSRWLockExclusive(&g_gamedb.lock);
        if (art_service_paced()) return false;

        Path file = gamedb_art_file(w.id, w.kind);
        wchar_t url[256];
        bool ok = false, later = false;
        if (w.kind == ART_COVER && w.steam) {
            // Steam's library art: a wide picture of the game with no title or season's promotion written across
            // it. Its store banner has both, so that is only the second choice.
            static const wchar_t *const kinds[] = {L"library_hero.jpg", L"header.jpg"};
            for (i32 k = 0; k < COUNT(kinds) && !ok && !g_gamedb.stop; k++) {
                swprintf(url, COUNT(url), L"/store_item_assets/steam/apps/%u/%s", w.steam, kinds[k]);
                u32 status = http_get(L"shared.steamstatic.com", url, L"", &file);
                ok = status == 200;
                later |= status == 0 || status == 429 || status >= 500;
            }
        }
        if (!ok && w.hash[0]) {
            swprintf(url, COUNT(url), L"/app-icons/%hs/%hs.png?size=%d", w.id, w.hash, w.kind == ART_ICON ? 128 : 512);
            u32 status = http_get(L"cdn.discordapp.com", url, L"", &file);
            ok = status == 200;
            later |= status == 0 || status == 429 || status >= 500;
        }
        if (!ok && !g_gamedb.stop) {
            char found[512];
            bool unasked = false;
            if (art_service_find(w.id, w.name, w.kind, found, sizeof found, &unasked)) {
                ok = http_get_url(found, file);
                later |= !ok; // the picture is there but didn't come
            }
            later |= unasked;
        }
        // None to be had: an empty file says so, and it isn't asked for again for a week. Not when it only couldn't
        // be asked (offline, or a server down or busy): then it is asked for again the next time clipback runs.
        if (!ok && !later && !g_gamedb.stop) {
            FILE *f = _wfopen(file.s, L"wb");
            if (f) fclose(f);
        }
        DEV_LOG("gamedb: %s for %s %s", w.kind == ART_ICON ? "icon" : "cover", w.id, ok ? "fetched" : "not there");

        AcquireSRWLockExclusive(&g_gamedb.lock);
        memmove(&g_gamedb.want[0], &g_gamedb.want[1], (g_gamedb.want_count - 1) * sizeof g_gamedb.want[0]);
        g_gamedb.want_count--;
        ReleaseSRWLockExclusive(&g_gamedb.lock);
        if (g_gamedb.stop) return true;
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

// Whether text[0..len) holds what.
static bool has_text(const char *text, i64 len, const char *what)
{
    i64 n = (i64)strlen(what);
    for (i64 i = 0; i + n <= len; i++)
        if (!memcmp(text + i, what, (size_t)n)) return true;
    return false;
}

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
// art goes where art_service_find looks for it, so the icon and the cover take no more questions. False if some are
// left for later, the service having been asked enough for now.
static bool gamedb_fetch_asks()
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
        if (at < 0) return true;
        if (art_service_paced()) return false;

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
            if (g_gamedb.stop) return true;
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
        if (named && art && strstr(text, "\"shared\":false") && !has_text(art, art_len, "https://")) art = 0; // none handed out
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
        if (g_gamedb.stop) return true;
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
        bool done = gamedb_fetch_asks();
        done &= gamedb_fetch_art();
        WaitForSingleObject(g_gamedb.wake, done ? INFINITE : 2000); // some waiting their turn: look again soon
    }
    return 0;
}

// The searches a player types, apart from the worker, so they never wait behind art.
static DWORD WINAPI gamedb_find_thread(void *)
{
    while (!g_gamedb.stop) {
        gamedb_fetch_find();
        WaitForSingleObject(g_gamedb.find_wake, INFINITE);
    }
    return 0;
}

static void gamedb_start(bool online)
{
    g_gamedb.online = online;
    str_set(g_gamedb.service, g_app.config.art_service.s);
    str_set(g_gamedb.sgdb_key, g_app.config.sgdb_key);
    g_gamedb.wake = CreateEventW(0, FALSE, FALSE, 0);
    g_gamedb.thread = thread_start(gamedb_thread, 0);
    g_gamedb.find_wake = CreateEventW(0, FALSE, FALSE, 0);
    g_gamedb.find_thread = thread_start(gamedb_find_thread, 0);
}

static void gamedb_stop()
{
    if (!g_gamedb.thread) return;
    InterlockedExchange(&g_gamedb.stop, 1);
    SetEvent(g_gamedb.wake);
    SetEvent(g_gamedb.find_wake);
    WaitForSingleObject(g_gamedb.thread, 3000);
    WaitForSingleObject(g_gamedb.find_thread, 3000);
}
