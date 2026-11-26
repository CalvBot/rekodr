// The games database: which exe is which game, its proper name, and where its icon and cover are.
//
// Rekodr fetches it itself; nothing is shipped with it. With Rekodr's service on (ART_SERVICE_HOST; "art_service"
// in settings.txt names another, "" none), a game that starts is looked up there (POST /v1/lookup): the service
// names it and says where its icon, grid and hero are. Only a game is asked about: one sorted as a Game, one
// Discord's list knows, or one a store installed (storeids.cpp). With the service off nothing goes to it: names
// come from Discord's public list of the games it can detect (about 25,000, each with its exe names, the folders
// they sit in and, for an exe many programs share, the command line that tells them apart), and art from
// SteamGridDB with the player's own key ("sgdb_key"), else Discord's image server. Discord's list is also what is
// used when the service can't be asked. All of it is kept under <local>\gamedb:
//   detectable.json   Discord's list as fetched, again after a week
//   games.tsv         the same, one game a line, which is what is loaded:
//                     id, name, icon, cover, Steam id, other names (|), exes (|; "!" first marks a launcher,
//                     ">" a shared exe; the command line it needs follows a \x1f)
//   lookups\<hash>.json   the service's answer to a lookup (its ETag "v" in it), asked again after a day
//   art\<id>-art.json     where a game's pictures are: the service's answer (a lookup's or /v1/art's), or with the
//                         service off, what SteamGridDB has. Asked again after a day (a week with it off)
//   art\<id>-icon.png, art\<id>-art.jpg   the pictures, each with a .url file beside it naming the address it came
//                         from ("none" = there is none); one whose address isn't the art file's any more is fetched
//                         again
//   emojis.txt        the service's emoji for each game on Discord cards (/v1/emojis), one game a line: its name's
//                     key (emoji_key), a tab, the emoji. Fetched again after a day; offline, the one there is is used
// A game's id is its file name there: the service's key for it (":" made "_"), Discord's id with the service off,
// or "n" and a hash of its name's letters for one known only by its name.
// One worker thread does the fetching and the loading. The index is never changed once built, only replaced, and
// readers copy what they find out of it under the lock, so any thread may ask.

struct GameInfo {
    char id[48];
    char key[48];             // the service's key for it ("d<discord id>", "s<sgdb id>", ...); "" = not known
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

// What the service said to a lookup. `failed`: it couldn't be asked and nothing it said before is kept, so Discord's
// list decides.
struct LookAnswer {
    char name[128], key[48], how[16]; // how: its "matched_by"
    f32  confidence;
    char ask_args[512];               // a shared exe: what to look for in its command line, one a line; "" = none
    bool failed;
};

struct GameDbShared {
    SRWLOCK    lock;
    GameIndex *index;
    HANDLE     thread, wake;
    HANDLE     find_thread, find_wake; // the searches a player types, on a thread of their own
    volatile LONG stop;
    bool       online;      // may fetch
    char       service[128]; // the service's host; "" = don't ask it
    char       sgdb_key[72]; // the player's own SteamGridDB key; "" = none
    // Art asked for, not fetched yet, newest first. `paced`: it needs the service, which has been asked enough for
    // now. `quiet`: a picture is there, and only whether it is still the one to show is looked at.
    struct { char id[48], key[48], hash[40], name[128]; u32 steam; GameArt kind; bool paced, quiet; } want[64];
    i32        want_count;
    // Lookups and names the player gave (`rename`), by a hash of what is sent: being sent (body set) or answered.
    // The oldest answer makes room.
    struct { u64 hash; char *body; bool answered, rename; LookAnswer ans; } looks[64];
    i32        look_next;
    // A name typed when naming an app, looked for on the service: what to look for (main thread), and what it
    // found for which (the worker).
    char       find_q[128], found_q[128];
    struct { char name[128]; i32 year; } found[8];
    i32        found_count;
    // The service's keys for names its lookups and searches gave, newest at named_next - 1, so a game's art is
    // asked for by its key.
    struct { char letters[128], key[48]; } named[128];
    i32        named_next;
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
    SHCreateDirectoryExW(0, path_join(d, L"lookups").s, 0);
    return d;
}

// A game's picture, or with `ext` the file beside it (".url").
static Path gamedb_art_file(const char *id, GameArt kind, const wchar_t *ext = L"")
{
    wchar_t name[128];
    swprintf(name, COUNT(name), L"art\\%hs-%s%s", id, kind == ART_ICON ? L"icon.png" : L"art.jpg", ext);
    return path_join(gamedb_dir(), name);
}

// Where the addresses of a game's pictures are kept.
static Path gamedb_art_json(const char *id)
{
    wchar_t name[128];
    swprintf(name, COUNT(name), L"art\\%hs-art.json", id);
    return path_join(gamedb_dir(), name);
}

// A service key as a file name: "p:modrinth:abc" -> "p_modrinth_abc". Anything but letters, digits and - is _.
static void key_file_id(const char *key, char *out, i32 cap)
{
    i32 n = 0;
    for (; key[n] && n < cap - 1; n++) out[n] = isalnum((u8)key[n]) || key[n] == '-' ? key[n] : '_';
    out[n] = 0;
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
    HINTERNET session = WinHttpOpen(L"rekodr", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, 0, 0, 0);
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
    snprintf(out->key, sizeof out->key, "d%s", r.id);
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
                    // Maybe not this one. If the window or the command line names the game, it is a fair guess
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
static void mc_put(char *out, i32 cap, i32 *n, const char *s); // mcpack.cpp
static bool exe_clues(const char *exe, const char *path, const char *name, bool game, const char *more, char *out,
                      i32 cap); // storeids.cpp

// Whether games are looked up on the service: online, and it is on.
static bool gamedb_service_on() { return g_gamedb.online && g_gamedb.service[0]; }

// Keeps the service's key for a name. Under the exclusive lock.
static void gamedb_named_put_locked(const char *name, const char *key)
{
    char letters[128];
    if (!key[0] || gdb_letters(name, letters, sizeof letters) < 1) return;
    for (auto &k : g_gamedb.named)
        if (!strcmp(k.letters, letters)) {
            str_set(k.key, key);
            return;
        }
    auto &k = g_gamedb.named[g_gamedb.named_next++ % COUNT(g_gamedb.named)];
    str_set(k.letters, letters);
    str_set(k.key, key);
}

// The service's key for a game's name: what its lookups and searches said, else Discord's id for a game in its list.
static bool gamedb_key_for(const char *name, char *key, i32 cap)
{
    char letters[128];
    key[0] = 0;
    if (gdb_letters(name, letters, sizeof letters) < 1) return false;
    AcquireSRWLockShared(&g_gamedb.lock);
    for (i32 i = 0; i < COUNT(g_gamedb.named) && !key[0]; i++)
        if (!strcmp(g_gamedb.named[i].letters, letters)) snprintf(key, cap, "%s", g_gamedb.named[i].key);
    ReleaseSRWLockShared(&g_gamedb.lock);
    GameInfo g;
    if (!key[0] && gamedb_by_name(name, &g)) snprintf(key, cap, "%s", g.key);
    return key[0] != 0;
}

// Sends `body` to the service: a lookup (POST /v1/lookup), or with `rename` a name the player gave (POST
// /v1/rename). The same body is sent once a run. A lookup returns 1 with *ans once it is answered, 0 while it is
// being asked. Main thread.
static i32 gamedb_lookup(const char *body, LookAnswer *ans, bool rename = false)
{
    u64 hash = fnv64(body, strlen(body)) ^ (rename ? 1 : 0);
    i32 state = -1;
    bool ask = false;
    AcquireSRWLockExclusive(&g_gamedb.lock);
    for (i32 i = 0; i < COUNT(g_gamedb.looks) && state < 0; i++) {
        auto &q = g_gamedb.looks[i];
        if (q.hash != hash || (!q.body && !q.answered)) continue;
        state = q.answered ? 1 : 0;
        if (q.answered && ans) *ans = q.ans;
    }
    if (state < 0) {
        state = 0;
        for (i32 tries = 0; tries < COUNT(g_gamedb.looks); tries++) { // one being asked keeps its place
            auto &q = g_gamedb.looks[g_gamedb.look_next++ % COUNT(g_gamedb.looks)];
            if (q.body) continue;
            q.hash = hash;
            q.answered = false;
            q.rename = rename;
            q.ans = {};
            q.body = _strdup(body);
            ask = q.body != 0;
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_gamedb.lock);
    if (ask) SetEvent(g_gamedb.wake);
    return state;
}

// Which of the strings the service asked about (one a line) the command line holds: a JSON array, lower case.
static void args_has_json(const char *ask_args, const char *cmdline, char *out, i32 cap)
{
    static char low[32768];
    i32 cl = 0;
    for (; cmdline[cl] && cl < (i32)sizeof low - 1; cl++) low[cl] = (char)tolower((u8)cmdline[cl]);
    low[cl] = 0;
    i32 n = snprintf(out, cap, "[");
    for (const char *p = ask_args; *p;) {
        const char *e = strchr(p, '\n');
        char one[128];
        snprintf(one, sizeof one, "%.*s", (int)(e ? e - p : strlen(p)), p);
        for (char *c = one; *c; c++) *c = (char)tolower((u8)*c);
        if (one[0] && strstr(low, one)) {
            if (n > 1 && n < cap - 1) out[n++] = ',';
            mc_put(out, cap, &n, one);
        }
        p = e ? e + 1 : p + strlen(p);
    }
    snprintf(out + n, cap - n, "]");
}

// What a lookup sends about an app: its clues (exe_clues), with its File description, a running Minecraft's
// (mc_clues, from `cmdline`) and, with `args_has`, which of the strings the service asked about its command line
// holds. False if it isn't to be asked about.
static bool lookup_body(const AppInfo &a, bool game, const char *cmdline, const char *args_has, char *out, i32 cap)
{
    static char more[16384], mc[12288];
    i32 n = 0;
    more[0] = 0;
    if (cmdline[0] && mc_clues(cmdline, mc, sizeof mc)) {
        size_t l = strlen(mc); // {"mc":{...}} -> ,"mc":{...}
        if (l > 2 && mc[0] == '{') n += snprintf(more + n, sizeof more - n, ",%.*s", (int)(l - 2), mc + 1);
    }
    if (args_has[0] && n < (i32)sizeof more) snprintf(more + n, sizeof more - n, ",\"args_has\":%s", args_has);
    char desc[128] = "";
    exe_description(a.path, desc, sizeof desc);
    return exe_clues(a.exe.s, a.path, desc, game, more, out, cap);
}

// The clues last sent in a lookup for each exe (its command line's among them), so a rename names the same exe the
// lookup did. Main thread.
static struct { char exe[64]; char *body; } g_last_looks[32];
static i32 g_last_look_next;

static void last_look_put(const char *exe, const char *body)
{
    i32 at = -1;
    for (i32 i = 0; i < COUNT(g_last_looks) && at < 0; i++)
        if (!strcmp(g_last_looks[i].exe, exe)) at = i;
    if (at < 0) at = g_last_look_next++ % COUNT(g_last_looks);
    if (g_last_looks[at].body && !strcmp(g_last_looks[at].body, body)) return;
    free(g_last_looks[at].body);
    str_set(g_last_looks[at].exe, exe);
    g_last_looks[at].body = _strdup(body);
}

static const char *last_look(const char *exe)
{
    for (auto &l : g_last_looks)
        if (l.body && !strcmp(l.exe, exe)) return l.body;
    return 0;
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

// "Help name games": the player named a game by hand, so the service is told that this exe, which was shown as
// `from` ("" = nothing), is `to` (POST /v1/rename), with the clues its lookup sent and the keys of both names where
// they are known (`to`'s only when it is a search hit). When enough players name an exe the same, the service names
// it so for everyone. Only with the service on and the player's say-so. Main thread.
static void gamedb_rename(const AppInfo &a, const char *from, const char *to)
{
    if (!g_app.config.share_names || !gamedb_service_on() || !to[0] || !strcmp(from, to)) return;
    static char more[1024], body[20 * 1024];
    i32 n = snprintf(more, sizeof more, ",\"from\":");
    mc_put(more, sizeof more, &n, from);
    n += snprintf(more + n, sizeof more - n, ",\"to\":");
    mc_put(more, sizeof more, &n, to);
    char key[48];
    if (from[0] && gamedb_key_for(from, key, sizeof key)) {
        n += snprintf(more + n, sizeof more - n, ",\"from_key\":");
        mc_put(more, sizeof more, &n, key);
    }
    if (gamedb_key_for(to, key, sizeof key) && (key[0] == 's' || key[0] == 'i')) {
        n += snprintf(more + n, sizeof more - n, ",\"to_key\":");
        mc_put(more, sizeof more, &n, key);
    }
    if (n >= (i32)sizeof more - 1) return;
    const char *clues = is_browser(a.exe.s) ? 0 : last_look(a.exe.s);
    if (is_browser(a.exe.s)) {
        i32 b = snprintf(body, sizeof body, "{\"exe\":");
        mc_put(body, sizeof body, &b, a.exe.s);
        if (snprintf(body + b, sizeof body - b, "%s}", more) >= (i32)sizeof body - b) return;
    } else if (clues) {
        size_t l = strlen(clues); // {...} -> {...,"from":...}
        if (l < 2 || (i32)(l + strlen(more)) >= (i32)sizeof body - 1) return;
        snprintf(body, sizeof body, "%.*s%s}", (int)(l - 1), clues, more);
    } else if (!exe_clues(a.exe.s, a.path, "", true, more, body, sizeof body)) {
        return;
    }
    gamedb_lookup(body, 0, true);
    log_line("games: told the service %s is %s", a.exe.s, to);
}

// The program longest in front between from_us and to_us, from focus.tsv text ("us\texe" lines, oldest first).
// Rekodr itself and "nothing in front" don't count. False if nothing was.
static bool focus_longest(char *text, i64 from_us, i64 to_us, char *exe, i32 cap)
{
    struct Spent { char exe[64]; i64 us; } spent[32];
    i32 n = 0;
    i64 at = 0;
    char cur[64] = "";
    auto add = [&](i64 until) {
        i64 a = at > from_us ? at : from_us, b = until < to_us ? until : to_us;
        if (!cur[0] || b <= a || !_stricmp(cur, "rekodr.exe")) return;
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
static Path lib_item_path(const LibItem &it);  // library.cpp

// A clip or session was moved from game `from` to `to` by hand: with "Help name games" on, the service is told the
// program that was in front during it is that game (gamedb_rename). Only for a file saved from this buffer while
// the buffer still covers it. Main thread.
static void gamedb_clip_game(const LibItem &it, const char *from, const char *to)
{
    Config &c = g_app.config;
    if (!c.share_names || !gamedb_service_on() || !to[0]) return;
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
    AppInfo *a = 0, browser = {};
    for (i32 i = 0; i < c.app_count && !a; i++)
        if (!strcmp(c.apps[i].exe.s, exe)) a = &c.apps[i];
    if (is_browser(exe)) { // only its exe's name is sent
        str_set(browser.exe.s, exe);
        a = &browser;
    }
    if (!a) return;
    AppCategory cat = app_category(c, *a);
    if (a != &browser && cat != CAT_GAME && cat != CAT_NONE) return; // a chat app or a tool in front says nothing
    gamedb_rename(*a, from, to);
}

// Names an app: its description becomes the game's name, and one that hasn't been sorted yet (and that no folder
// rule sorts) becomes a Game. With the service on, a game is looked up there; Discord's list names it while it is
// asked, and when the service can't be. An exe several games share is told apart by its command line: by the
// strings the service asks about, or by Discord's entries (else its window naming a game in the list). One neither
// knows goes by its exe's name until it is named by hand. Main thread. True if something about it changed.
static bool gamedb_name_app(AppInfo *a)
{
    GameInfo g;
    bool shared = false, known = gamedb_by_exe(a->exe.s, a->path, "", "", &g, &shared), facts = false;
    static char cmd[32768];
    char title[256];
    cmd[0] = title[0] = 0;
    if (!known && shared) {
        facts = process_facts(a->exe.s, cmd, sizeof cmd, title, sizeof title);
        if (facts) {
            DEV_LOG("gamedb: %s is shared; its window is \"%s\", its command line ends \"%s\"", a->exe.s, title,
                    strlen(cmd) > 400 ? cmd + strlen(cmd) - 400 : cmd);
            known = gamedb_by_exe(a->exe.s, a->path, cmd, title, &g);
        }
    }
    AppCategory cat = app_category(g_app.config, *a);
    if (gamedb_service_on() && a->path[0] && (cat == CAT_GAME || cat == CAT_NONE)) {
        static char body[16384], has[2048];
        LookAnswer ans = {};
        bool game = cat == CAT_GAME || known || shared;
        i32 state = lookup_body(*a, game, cmd, "", body, sizeof body) ? gamedb_lookup(body, &ans) : -1;
        if (state == 1 && !ans.failed && !ans.name[0] && ans.ask_args[0]) { // a shared exe: which one is it?
            if (!facts) facts = process_facts(a->exe.s, cmd, sizeof cmd, title, sizeof title);
            args_has_json(ans.ask_args, cmd, has, sizeof has);
            state = facts && lookup_body(*a, game, cmd, has, body, sizeof body) ? gamedb_lookup(body, &ans) : -1;
        }
        if (state >= 0) last_look_put(a->exe.s, body);
        if (state == 0 && !known) return false; // being asked
        // What it found only by a name is only taken for one sorted as a Game, or when it is sure
        if (state == 1 && !ans.failed && ans.name[0] &&
            (strcmp(ans.how, "name") || ans.confidence >= 0.9f || cat == CAT_GAME)) {
            bool launcher = known && g.launcher;
            g = {};
            str_set(g.name, ans.name);
            str_set(g.key, ans.key);
            g.launcher = launcher;
            known = true;
        }
    }
    if (!known && shared) {
        if (!a->desc.s[0]) return false;
        a->desc.s[0] = 0; // its File description is the runtime's, not a game's
        sessions_game_renamed(a->exe.s);
        return true;
    }
    if (!known) return false;
    bool changed = false;
    if (strcmp(a->desc.s, g.name)) {
        snprintf(a->desc.s, sizeof a->desc.s, "%s", g.name);
        changed = true;
        sessions_game_renamed(a->exe.s);
    }
    if (a->category == CAT_NONE && !g.launcher && cat == CAT_NONE) {
        a->category = CAT_GAME;
        log_line("%s is %s: sorted as a game", a->exe.s, g.name);
        changed = true;
    }
    return changed;
}

// ---------------------------------------------------------------------------------------------------------------
// The service (rekodr-api), which holds Discord's list and the SteamGridDB and IGDB keys. Its calls, all signed:
//   POST /v1/lookup  an exe's clues -> {"game":{"key","name",...}|null, "matched_by", "confidence",
//                    "art":{"icon":{"url","w","h","from"}|null,"grid":..,"hero":..}, "v", "ask_args"?}
//   GET /v1/art/<key>                -> {"key","art":{...},"v"}
//   GET /v1/search?q=<name>          -> {"games":[{"id":"s5367951","name":"...","year":2017}, ...]}
//   POST /v1/rename  a name the player gave -> 204
// "v" is the answer's ETag: asked again with If-None-Match, an answer that hasn't changed is a 304. A picture's
// address changes whenever the picture does. The signature only keeps the service from being an open relay for its
// keys, so the key it is made with is no secret.

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

// The signature header (ART_SERVICE_SIG_HEADER) for a path (with its query, as sent): "<unix seconds>.<hmac of "seconds\npath">", and
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

// A signed GET to the service, or a POST of the JSON `post`, with If-None-Match when `etag` is given. Returns the
// status (0 = no answer); the body, for a 200, in *body.
// A search being typed doesn't wait out a 429: the next key asks again.
static u32 art_service_get(const char *path, char **body, i64 *len, const char *post = 0, bool wait_out = true,
                           const char *etag = "")
{
    wchar_t host[128], wpath[1024], header[400];
    utf8_to_wide(g_gamedb.service, host, COUNT(host));
    utf8_to_wide(path, wpath, COUNT(wpath));
    for (i32 attempt = 0;; attempt++) {
        char sig[128];
        FILETIME now;
        GetSystemTimeAsFileTime(&now);
        i64 unix_seconds = (((i64)now.dwHighDateTime << 32 | now.dwLowDateTime) - 116444736000000000ll) / 10000000;
        art_service_sign(path, unix_seconds, sig, sizeof sig, post);
        i32 hl = swprintf(header, COUNT(header), ART_SERVICE_SIG_HEADER L": %hs\r\n%s", sig,
                          post ? L"content-type: application/json\r\n" : L"");
        if (etag[0] && hl > 0 && strlen(etag) < 64) swprintf(header + hl, COUNT(header) - hl, L"If-None-Match: \"%hs\"\r\n", etag);
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

// From an answer's "art", the address of a game's icon, or of its cover (its hero, a wide picture with no title on
// it, else its grid), and with `from` where the service had it. False if it has neither.
static bool art_pick_url(const char *json, i64 len, GameArt kind, char *url, i32 cap, char *from = 0, i32 from_cap = 0)
{
    url[0] = 0;
    if (from) from[0] = 0;
    const char *const kinds[2] = {kind == ART_ICON ? "art.icon" : "art.hero", kind == ART_ICON ? 0 : "art.grid"};
    for (const char *k : kinds) {
        char path[32];
        if (!k) break;
        snprintf(path, sizeof path, "%s.url", k);
        if (!j_get(json, len, path, url, cap) || strncmp(url, "https://", 8)) {
            url[0] = 0;
            continue;
        }
        snprintf(path, sizeof path, "%s.from", k);
        if (from) j_get(json, len, path, from, from_cap);
        return true;
    }
    return false;
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

// Writes text to a file through a .part, so a reader never sees half of it.
static bool write_whole(const Path &file, const char *text, i64 len)
{
    Path part = file;
    wcsncat(part.s, L".part", MAX_PATH - wcslen(part.s) - 1);
    FILE *f = _wfopen(part.s, L"wb");
    if (!f) return false;
    bool ok = fwrite(text, 1, (size_t)len, f) == (size_t)len;
    ok &= fclose(f) == 0;
    if (ok) ok = MoveFileExW(part.s, file.s, MOVEFILE_REPLACE_EXISTING) != 0;
    if (!ok) DeleteFileW(part.s);
    return ok;
}

// Marks a file as written now: what is in it was just found to be still so.
static void touch_file(const Path &file)
{
    HANDLE h = CreateFileW(file.s, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING, 0, 0);
    if (h == INVALID_HANDLE_VALUE) return;
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    SetFileTime(h, 0, 0, &now);
    CloseHandle(h);
}

// ---- SteamGridDB with the player's own key, for when the service is off

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

// SteamGridDB's search answer ({"data":[{"id":5,"name":"...","release_date":1500000000}]}) as the service gives
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

// The first picture's address in a SteamGridDB list answer ({"data":[{"url":"...","thumb":"..."}]}): its `field`.
// "" if there is none.
static void sgdb_first_url(const char *json, i64 len, const char *field, char *url, i32 cap)
{
    char k[16];
    url[0] = 0;
    Json j = {json, json + len};
    if (!json || !j_find(&j, "data") || !j_eat(&j, '[') || !j_eat(&j, '{')) return;
    while (!j_eat(&j, '}') && j.p < j.end) {
        if (!j_string(&j, k, sizeof k)) break;
        j_eat(&j, ':');
        if (!strcmp(k, field)) j_string(&j, url, cap) || (j_skip(&j), 0);
        else j_skip(&j);
        j_eat(&j, ',');
    }
    if (strncmp(url, "https://", 8) || strchr(url, '"') || strchr(url, '\\')) url[0] = 0;
}

// What SteamGridDB has for a game, found by its Steam id, else by its name, in the shape of the service's answers:
// {"art":{"icon":{"url","from":"sgdb"}|null,"grid":..,"hero":null}}. Its icon (as a PNG: an icon itself may be an
// .ico of many sizes) and its first wide grid. Into a malloc'd *out; false if it couldn't be asked.
static bool sgdb_art_json(u32 steam, const char *name, char **out, i64 *out_len)
{
    char path[400], *text = 0, sid[24] = "";
    i64 tl = 0;
    u32 status = 404;
    if (steam) {
        snprintf(path, sizeof path, "/games/steam/%u", steam);
        status = sgdb_get(path, &text, &tl);
        char id[24];
        if (status == 200 && j_get(text, tl, "data.id", id, sizeof id) && atoi(id) > 0) snprintf(sid, sizeof sid, "s%s", id);
        free(text);
        text = 0;
        if (status != 200 && status != 404) return false;
    }
    if (!sid[0] && name[0]) {
        char enc[400];
        url_encode(name, enc, sizeof enc);
        snprintf(path, sizeof path, "/search/autocomplete/%s", enc);
        status = sgdb_get(path, &text, &tl);
        if (status != 200 && status != 404) { free(text); return false; }
        static char games[8192];
        if (status == 200 && sgdb_games(text, tl, games, sizeof games))
            art_pick_search(games, (i64)strlen(games), name, sid, sizeof sid);
        free(text);
        text = 0;
    }
    char icon[512] = "", grid[512] = "";
    if (sid[0]) {
        static const char *const kinds[2][2] = {{"/icons/game/%s?nsfw=false&humor=false", "thumb"},
                                                {"/grids/game/%s?dimensions=920x430,460x215&nsfw=false&humor=false", "url"}};
        char *urls[2] = {icon, grid};
        for (i32 i = 0; i < 2; i++) {
            snprintf(path, sizeof path, kinds[i][0], sid + 1);
            status = sgdb_get(path, &text, &tl);
            if (status != 200 && status != 404) { free(text); return false; }
            if (status == 200) sgdb_first_url(text, tl, kinds[i][1], urls[i], 512);
            free(text);
            text = 0;
        }
    }
    i32 cap = 2048;
    *out = (char *)malloc(cap);
    if (!*out) return false;
    i32 n = snprintf(*out, cap, "{\"key\":\"%s\",\"art\":{\"icon\":", sid);
    n += icon[0] ? snprintf(*out + n, cap - n, "{\"url\":\"%s\",\"from\":\"sgdb\"}", icon) : snprintf(*out + n, cap - n, "null");
    n += snprintf(*out + n, cap - n, ",\"grid\":");
    n += grid[0] ? snprintf(*out + n, cap - n, "{\"url\":\"%s\",\"from\":\"sgdb\"}", grid) : snprintf(*out + n, cap - n, "null");
    n += snprintf(*out + n, cap - n, ",\"hero\":null}}");
    *out_len = n;
    return true;
}

// A search for a name: the service's, or with it off, SteamGridDB's with the player's own key. Returns the status
// (0: nothing to search with); the answer, as the service gives it, in *body. `typing`: the player is still typing
// it, so the service keeps it in its edge cache only, not its store.
static u32 art_search(const char *q, char **body, i64 *len, bool typing = false)
{
    char enc[400], path[512];
    url_encode(q, enc, sizeof enc);
    if (g_gamedb.service[0]) {
        snprintf(path, sizeof path, "/v1/search?q=%s%s", enc, typing ? "&typing=1" : "");
        return art_service_get(path, body, len, 0, !typing);
    }
    if (!g_gamedb.sgdb_key[0]) return 0;
    char *text = 0;
    i64 tl = 0;
    snprintf(path, sizeof path, "/search/autocomplete/%s", enc);
    u32 status = sgdb_get(path, &text, &tl);
    if (status == 200) {
        i32 cap = 8192;
        *body = (char *)malloc(cap);
        if (*body && sgdb_games(text, tl, *body, cap)) *len = (i64)strlen(*body);
        else { free(*body); *body = 0; status = 0; }
    }
    free(text);
    return status;
}

// The games in a /v1/search answer, in its order: their names, years (0 = none) and, with `ids`, ids. How many.
static i32 art_search_list(const char *json, i64 len, char (*names)[128], i32 *years, i32 max, char (*ids)[24] = 0)
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
            char gname[128] = "", gid[24] = "", k[16];
            i32 year = 0;
            while (!j_eat(&j, '}') && j.p < j.end) {
                if (!j_string(&j, k, sizeof k)) break;
                j_eat(&j, ':');
                if (!strcmp(k, "name")) j_string(&j, gname, sizeof gname) || (j_skip(&j), 0);
                else if (!strcmp(k, "id")) j_string(&j, gid, sizeof gid) || (j_skip(&j), 0);
                else if (!strcmp(k, "year")) { j_ws(&j); year = atoi(j.p); j_skip(&j); }
                else j_skip(&j);
                j_eat(&j, ',');
            }
            j_eat(&j, ',');
            if (gname[0] && n < max) {
                snprintf(names[n], 128, "%s", gname);
                if (ids) snprintf(ids[n], 24, "%s", gid);
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
    char names[8][128], ids[8][24];
    i32 years[8], n = status == 200 && body ? art_search_list(body, len, names, years, 8, ids) : 0;
    free(body);
    DEV_LOG("gamedb: art service search for \"%s\" answered %u, %d games", q, status, n);
    AcquireSRWLockExclusive(&g_gamedb.lock);
    for (i32 i = 0; i < n; i++) gamedb_named_put_locked(names[i], ids[i]);
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

// A game by its name, for its art. With the service on: by the service's key for it (gamedb_key_for). With it
// off: Discord's list's entry. Else one made up from the name, whose art a search finds. None for a custom game.
static bool gamedb_for_art(const char *name, GameInfo *out)
{
    if (game_is_custom(name)) return false;
    bool listed = gamedb_by_name(name, out);
    char key[48];
    if (g_gamedb.service[0] && gamedb_key_for(name, key, sizeof key)) {
        if (!listed) {
            *out = {};
            str_set(out->name, name);
        }
        str_set(out->key, key);
        key_file_id(key, out->id, sizeof out->id);
        return true;
    }
    if (listed) return true;
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
// Art. A picture that is there is used; one that isn't is asked for and arrives later (GAMEDB_ART). Where a game's
// pictures are is looked at again after a day (a week with the service off), and a picture whose address changed is
// fetched again; GAMEDB_ART with b = 1 then says the game called `text` has a new one.

// How long where a game's pictures are is trusted before it is asked about again.
static i64 art_json_days() { return g_gamedb.service[0] ? 1 : 7; }

// Where a game's icon or cover is in the queue of art asked for, or -1. Under the lock.
static i32 gamedb_want_at(const char *id, GameArt kind)
{
    for (i32 i = 0; i < g_gamedb.want_count; i++)
        if (g_gamedb.want[i].kind == kind && !strcmp(g_gamedb.want[i].id, id)) return i;
    return -1;
}

// Whether a game's picture is settled: its .url file was written since where its pictures are was last asked
// about, and that was lately.
static bool art_settled(const char *id, GameArt kind)
{
    WIN32_FILE_ATTRIBUTE_DATA fu, fj;
    if (!GetFileAttributesExW(gamedb_art_file(id, kind, L".url").s, GetFileExInfoStandard, &fu) ||
        !GetFileAttributesExW(gamedb_art_json(id).s, GetFileExInfoStandard, &fj))
        return false;
    return CompareFileTime(&fu.ftLastWriteTime, &fj.ftLastWriteTime) >= 0 && !file_older_than(fj, art_json_days() * 24 * 3600);
}

// Where a game's icon or cover is: true if there is a picture. One not settled (art_settled) is asked for; while
// it comes, one there from before is shown, and with none there *coming is set.
static bool gamedb_art(const GameInfo &g, GameArt kind, Path *out, bool *coming = 0)
{
    if (coming) *coming = false;
    const char *hash = kind == ART_ICON ? g.icon : g.cover;
    if (!hash[0] && !g_gamedb.service[0] && !g_gamedb.sgdb_key[0]) return false; // nowhere to get one
    *out = gamedb_art_file(g.id, kind);
    WIN32_FILE_ATTRIBUTE_DATA fa;
    bool have = GetFileAttributesExW(out->s, GetFileExInfoStandard, &fa) && fa.nFileSizeLow > 0;
    if (!g_gamedb.online || art_settled(g.id, kind)) return have;
    // The newest ask goes first, being what is on screen now; one asked again moves to the front, and when the
    // queue is full the oldest gives way. (The worker fetches want[0].)
    AcquireSRWLockExclusive(&g_gamedb.lock);
    i32 at = gamedb_want_at(g.id, kind);
    if (at < 0) at = g_gamedb.want_count < COUNT(g_gamedb.want) ? g_gamedb.want_count++ : g_gamedb.want_count - 1;
    memmove(&g_gamedb.want[1], &g_gamedb.want[0], at * sizeof g_gamedb.want[0]);
    auto &w = g_gamedb.want[0];
    str_set(w.id, g.id);
    str_set(w.key, g.key);
    str_set(w.hash, hash);
    str_set(w.name, g.name);
    w.steam = g.steam;
    w.kind = kind;
    w.paced = false;
    w.quiet = have;
    ReleaseSRWLockExclusive(&g_gamedb.lock);
    SetEvent(g_gamedb.wake);
    if (coming) *coming = !have;
    return have;
}

// Asks where a game's pictures are, into art\<id>-art.json, unless that was done lately. With the service on: by
// its key (/v1/art), or for one known only by its name, by a search; with it off, of SteamGridDB with the player's
// own key, and with no key "{}" (Discord's own pictures are all there is). True if the file is there to read, old
// or new. *paced: it needs the service, which has been asked enough for now, so nothing was asked.
static bool art_json_fetch(const char *id, const char *key, const char *name, u32 steam, bool *paced)
{
    *paced = false;
    Path file = gamedb_art_json(id);
    WIN32_FILE_ATTRIBUTE_DATA fa;
    bool there = GetFileAttributesExW(file.s, GetFileExInfoStandard, &fa) != 0;
    if (there && !file_older_than(fa, art_json_days() * 24 * 3600)) return true;
    if (!g_gamedb.online) return there;
    char *body = 0;
    i64 len = 0;
    if (!g_gamedb.service[0]) {
        bool ok = g_gamedb.sgdb_key[0] ? sgdb_art_json(steam, name, &body, &len) : (body = _strdup("{}")) != 0;
        if (ok && body && !g_gamedb.sgdb_key[0]) len = 2;
        if (ok) write_whole(file, body, len);
        free(body);
        return ok || there;
    }
    if (art_service_paced()) {
        *paced = true;
        return there;
    }
    char k[48];
    snprintf(k, sizeof k, "%s", key);
    if (!k[0]) {
        // Known only by its name: what the service finds by it ("someapp.exe" is looked for without the ".exe")
        char q[128];
        snprintf(q, sizeof q, "%s", name);
        size_t ql = strlen(q);
        if (ql > 4 && !_stricmp(q + ql - 4, ".exe")) q[ql - 4] = 0;
        u32 status = art_search(q, &body, &len);
        bool found = status == 200 && art_pick_search(body, len, q, k, sizeof k);
        free(body);
        body = 0;
        if (status != 200) return there;
        if (!found) return write_whole(file, "{}", 2);
    }
    char path[128], etag[64] = "";
    snprintf(path, sizeof path, "/v1/art/%s", k);
    if (there) {
        i64 ol = 0;
        char *old = read_file(file.s, &ol);
        if (old && !j_get(old, ol, "v", etag, sizeof etag)) etag[0] = 0;
        free(old);
    }
    u32 status = art_service_get(path, &body, &len, 0, true, etag);
    bool ok = status == 304 || (status == 200 && body && write_whole(file, body, len));
    if (status == 304) touch_file(file);
    if (!ok) DEV_LOG("gamedb: art for %s answered %u", k, status);
    free(body);
    return ok || there;
}

// Makes a game's icon or cover the one where its pictures are says (art_json_fetch first): fetched when its
// address changed, removed when there is none. Discord's own picture with the service off and no other.
// *replaced: a picture that was there was swapped for another. False if it couldn't be settled now (offline, or
// the picture didn't come); *paced when it waits its turn for the service.
static bool art_settle(const char *id, const char *key, const char *name, const char *hash, u32 steam, GameArt kind,
                       bool *replaced, bool *paced)
{
    *replaced = false;
    if (!art_json_fetch(id, key, name, steam, paced)) return false;
    Path json = gamedb_art_json(id), pic = gamedb_art_file(id, kind), side = gamedb_art_file(id, kind, L".url");
    i64 jl = 0, sl = 0;
    char *text = read_file(json.s, &jl), url[512] = "", was[512] = "";
    if (!art_pick_url(text ? text : "", text ? jl : 0, kind, url, sizeof url) && hash[0] && !g_gamedb.service[0])
        snprintf(url, sizeof url, "https://cdn.discordapp.com/app-icons/%s/%s.png?size=%d", id, hash,
                 kind == ART_ICON ? 128 : 512);
    free(text);
    char *s = read_file(side.s, &sl);
    if (s) snprintf(was, sizeof was, "%.*s", (int)(sl < (i64)sizeof was - 1 ? sl : sizeof was - 1), s);
    free(s);
    WIN32_FILE_ATTRIBUTE_DATA fa;
    bool have = GetFileAttributesExW(pic.s, GetFileExInfoStandard, &fa) && fa.nFileSizeLow > 0;
    const char *now = url[0] ? url : "none";
    if (strcmp(was, now) || (url[0] && !have)) {
        if (url[0] && !http_get_url(url, pic)) return false;
        if (!url[0]) DeleteFileW(pic.s);
        *replaced = have;
        DEV_LOG("gamedb: %s for %s %s", kind == ART_ICON ? "icon" : "cover", id, url[0] ? "fetched" : "none");
    }
    return write_whole(side, now, (i64)strlen(now));
}

// The game's SteamGridDB icon as a file, for a shared clip's page: its icon, when the service (or SteamGridDB itself,
// with the service off) had that from SteamGridDB or by hand. Waits on the network; not for the UI thread. False if
// there is none or it couldn't be had now.
static bool gamedb_sgdb_icon(const char *name, Path *out)
{
    GameInfo g;
    if (!name || !name[0] || !gamedb_for_art(name, &g)) return false;
    bool replaced, paced;
    if (!art_settled(g.id, ART_ICON) && !art_settle(g.id, g.key, g.name, "", g.steam, ART_ICON, &replaced, &paced))
        return false;
    i64 len = 0;
    char *text = read_file(gamedb_art_json(g.id).s, &len), url[512], from[16];
    bool sgdb = text && art_pick_url(text, len, ART_ICON, url, sizeof url, from, sizeof from) &&
                (!strcmp(from, "sgdb") || !strcmp(from, "override"));
    free(text);
    *out = gamedb_art_file(g.id, ART_ICON);
    WIN32_FILE_ATTRIBUTE_DATA fa;
    return sgdb && GetFileAttributesExW(out->s, GetFileExInfoStandard, &fa) && fa.nFileSizeLow > 0;
}

// Fetches the art asked for, newest first. One that needs the service while it has been asked enough for now
// waits, and those after it go ahead. False if some is left for later.
static bool gamedb_fetch_art()
{
    for (;;) {
        bool paced = art_service_paced();
        AcquireSRWLockExclusive(&g_gamedb.lock);
        i32 at = 0;
        while (at < g_gamedb.want_count && paced && g_gamedb.want[at].paced) at++;
        bool none = at == g_gamedb.want_count, done = !g_gamedb.want_count;
        auto w = none ? g_gamedb.want[0] : g_gamedb.want[at];
        ReleaseSRWLockExclusive(&g_gamedb.lock);
        if (none) return done;

        bool replaced = false, wait = false;
        bool ok = art_settle(w.id, w.key, w.name, w.hash, w.steam, w.kind, &replaced, &wait);
        AcquireSRWLockExclusive(&g_gamedb.lock);
        i32 i = gamedb_want_at(w.id, w.kind); // asks made meanwhile went in front of it
        if (i >= 0 && wait) g_gamedb.want[i].paced = true;
        else if (i >= 0) {
            memmove(&g_gamedb.want[i], &g_gamedb.want[i + 1], (g_gamedb.want_count - i - 1) * sizeof g_gamedb.want[0]);
            g_gamedb.want_count--;
        }
        ReleaseSRWLockExclusive(&g_gamedb.lock);
        // Nothing new to show, or it didn't come: what waits for it is tried again on the next GAMEDB event
        if (g_gamedb.stop) return true;
        if (wait || !ok || (w.quiet && !replaced)) continue;
        Event e = {};
        e.kind = EV_GAMEDB;
        e.a = GAMEDB_ART;
        e.b = replaced;
        str_set(e.text, w.name);
        event_post(e);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Lookups (gamedb_lookup): each answer is kept in lookups\<hash>.json and asked again after a day, with its "v" as
// If-None-Match. When the service can't be asked, the answer from before is used. A game's key and the addresses
// of its pictures go where gamedb_for_art and art_settle look for them.

// The parts of a lookup's answer. False if it isn't one.
static bool lookup_parse(const char *text, i64 len, LookAnswer *ans)
{
    *ans = {};
    char conf[16];
    if (!j_get(text, len, "matched_by", ans->how, sizeof ans->how) && !strstr(text, "\"game\"")) return false;
    j_get(text, len, "game.name", ans->name, sizeof ans->name);
    j_get(text, len, "game.key", ans->key, sizeof ans->key);
    if (j_get(text, len, "confidence", conf, sizeof conf)) ans->confidence = (f32)atof(conf);
    Json j = {text, text + len};
    if (j_find(&j, "ask_args") && j_eat(&j, '[')) {
        i32 n = 0;
        while (!j_eat(&j, ']') && j.p < j.end) {
            char one[128];
            if (!j_string(&j, one, sizeof one)) { j_skip(&j); j_eat(&j, ','); continue; }
            for (char *c = one; *c; c++) *c = *c == '\n' ? ' ' : *c;
            if (one[0] && n + (i32)strlen(one) + 2 < (i32)sizeof ans->ask_args)
                n += snprintf(ans->ask_args + n, sizeof ans->ask_args - n, "%s%s", n ? "\n" : "", one);
            j_eat(&j, ',');
        }
    }
    return true;
}

// Where an answer's "art" is in its text. False if it has none.
static bool answer_art(const char *text, i64 len, const char **art, i64 *art_len)
{
    Json j = {text, text + len};
    if (!j_find(&j, "art") || j.p >= j.end || *j.p != '{') return false;
    *art = j.p;
    j_skip(&j);
    *art_len = j.p - *art;
    return true;
}

// A lookup's answer about a game: its key for the name, and where its pictures are, as {"key","art"} (no "v": that
// is the lookup's, not /v1/art's). GAMEDB_ART (b = 1) when those changed, so what shows them looks again.
static void lookup_keep_art(const char *text, i64 len, const LookAnswer &ans)
{
    const char *art;
    i64 al;
    if (!ans.key[0] || !ans.name[0] || !answer_art(text, len, &art, &al)) return;
    AcquireSRWLockExclusive(&g_gamedb.lock);
    gamedb_named_put_locked(ans.name, ans.key);
    ReleaseSRWLockExclusive(&g_gamedb.lock);
    char id[48];
    key_file_id(ans.key, id, sizeof id);
    Path file = gamedb_art_json(id);
    i64 ol = 0, oal = 0;
    const char *oart;
    char *old = read_file(file.s, &ol);
    bool had = old != 0, same = old && answer_art(old, ol, &oart, &oal) && oal == al && !memcmp(oart, art, (size_t)al);
    free(old);
    if (same) {
        touch_file(file);
        return;
    }
    static char keep[16384];
    i32 n = snprintf(keep, sizeof keep, "{\"key\":");
    mc_put(keep, sizeof keep, &n, ans.key);
    if (n + al + 10 >= (i64)sizeof keep) return;
    n += snprintf(keep + n, sizeof keep - n, ",\"art\":%.*s}", (int)al, art);
    write_whole(file, keep, n);
    if (!had) return;
    Event e = {};
    e.kind = EV_GAMEDB;
    e.a = GAMEDB_ART;
    e.b = 1;
    str_set(e.text, ans.name);
    event_post(e);
}

// The keys of the games looked up before, from lookups\, so their art is found by name from the start.
static void lookups_load()
{
    Path dir = path_join(gamedb_dir(), L"lookups");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(path_join(dir, L"*.json").s, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        i64 len = 0;
        char *text = fd.nFileSizeLow < 256 * 1024 ? read_file(path_join(dir, fd.cFileName).s, &len) : 0;
        LookAnswer ans;
        if (text && lookup_parse(text, len, &ans) && ans.key[0] && ans.name[0]) {
            AcquireSRWLockExclusive(&g_gamedb.lock);
            gamedb_named_put_locked(ans.name, ans.key);
            ReleaseSRWLockExclusive(&g_gamedb.lock);
        }
        free(text);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// Sends what gamedb_lookup was given. False if some are left for later, the service having been asked enough for
// now.
static bool gamedb_fetch_looks()
{
    for (;;) {
        AcquireSRWLockExclusive(&g_gamedb.lock);
        i32 at = -1;
        for (i32 i = 0; i < COUNT(g_gamedb.looks) && at < 0; i++)
            if (g_gamedb.looks[i].body && !g_gamedb.looks[i].answered) at = i;
        u64 hash = at >= 0 ? g_gamedb.looks[at].hash : 0;
        bool rename = at >= 0 && g_gamedb.looks[at].rename;
        const char *body = at >= 0 ? g_gamedb.looks[at].body : 0; // its slot isn't reused while it has a body
        ReleaseSRWLockExclusive(&g_gamedb.lock);
        if (at < 0) return true;
        if (!g_gamedb.service[0]) { // turned off meanwhile: nothing is sent
            AcquireSRWLockExclusive(&g_gamedb.lock);
            auto &q = g_gamedb.looks[at];
            q.answered = true;
            q.ans.failed = true;
            free(q.body);
            q.body = 0;
            ReleaseSRWLockExclusive(&g_gamedb.lock);
            continue;
        }
        if (art_service_paced()) return false;

        LookAnswer ans = {};
        if (rename) {
            u32 status = art_service_get("/v1/rename", 0, 0, body);
            DEV_LOG("gamedb: service rename answered %u", status);
        } else {
            wchar_t fname[64];
            swprintf(fname, COUNT(fname), L"lookups\\%016llx.json", (unsigned long long)hash);
            Path file = path_join(gamedb_dir(), fname);
            WIN32_FILE_ATTRIBUTE_DATA fa;
            i64 len = 0, new_len = 0;
            char *text = GetFileAttributesExW(file.s, GetFileExInfoStandard, &fa) ? read_file(file.s, &len) : 0;
            u32 status = 304;
            if (!text || file_older_than(fa, 24 * 3600)) {
                char etag[64] = "", *got = 0;
                if (text && !j_get(text, len, "v", etag, sizeof etag)) etag[0] = 0;
                status = art_service_get("/v1/lookup", &got, &new_len, body, true, etag);
                if (status == 200 && got) {
                    free(text);
                    text = got;
                    len = new_len;
                    write_whole(file, text, len);
                } else {
                    free(got);
                    if (status == 304) touch_file(file);
                    else DEV_LOG("gamedb: service lookup answered %u%s", status, text ? "; using the answer from before" : "");
                }
            }
            if (!text || !lookup_parse(text, len, &ans)) ans.failed = true;
            else lookup_keep_art(text, len, ans);
            DEV_LOG("gamedb: lookup %016llx is %s (%s %.2f)", (unsigned long long)hash,
                    ans.name[0] ? ans.name : ans.ask_args[0] ? "a shared exe" : "none", ans.how, ans.confidence);
            free(text);
        }
        AcquireSRWLockExclusive(&g_gamedb.lock);
        auto &q = g_gamedb.looks[at];
        q.ans = ans;
        q.answered = true;
        free(q.body);
        q.body = 0;
        ReleaseSRWLockExclusive(&g_gamedb.lock);
        if (g_gamedb.stop) return true;
        if (rename) continue;
        Event e = {};
        e.kind = EV_GAMEDB;
        e.a = GAMEDB_RESOLVED;
        event_post(e);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// The worker: load what is on disk, fetch the list if there is none or it is a week old, then look up games and
// fetch art as asked.

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

// ---------------------------------------------------------------------------------------------------------------
// Game emojis: what a Discord card puts before the game's name. The art service keeps the list by hand
// (data/emojis.mjs in clipback-api) and sends it whole: {"emojis":[["overwatch","<:ow:123>"], ...]}.

enum { EMOJI_MAX = 4096 };
struct EmojiRow {
    char key[64];   // emoji_key of the game's name
    char emoji[64]; // "<:name:id>" or "<a:name:id>"
};
static struct {
    SRWLOCK   lock;
    EmojiRow *rows; // sorted by key
    i32       count;
} g_emoji;

// A Discord custom emoji as a message writes it: "<:name:id>", or "<a:name:id>" for an animated one; the name 2 to
// 32 letters, digits or _.
static bool emoji_ok(const char *e)
{
    if (!e || e[0] != '<') return false;
    const char *p = e + 1;
    if (*p == 'a') p++;
    if (*p++ != ':') return false;
    i32 n = 0;
    while (isalnum((u8)*p) || *p == '_') p++, n++;
    if (n < 2 || n > 32 || *p++ != ':') return false;
    n = 0;
    while (*p >= '0' && *p <= '9') p++, n++;
    return n >= 1 && n <= 20 && p[0] == '>' && !p[1];
}

// A game's name as the emoji list matches it: its ASCII letters and digits, lower case. "Counter-Strike 2" ->
// "counterstrike2", which stays apart from "Counter-Strike".
static void emoji_key(const char *name, char *out, i32 cap)
{
    i32 n = 0;
    for (const char *p = name ? name : ""; *p && n < cap - 1; p++)
        if (*p >= 'A' && *p <= 'Z') out[n++] = (char)(*p - 'A' + 'a');
        else if ((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9')) out[n++] = *p;
    out[n] = 0;
}

static int emoji_cmp(const void *a, const void *b)
{
    return strcmp(((const EmojiRow *)a)->key, ((const EmojiRow *)b)->key);
}

// The rows of /v1/emojis's answer, or of emojis.txt (`tsv`: "key\temoji" lines), into out (at most max), sorted.
// A row with a key that isn't a key or an emoji that isn't one is skipped. Returns how many; -1 when `text` isn't
// the answer at all.
static i32 emoji_parse(const char *text, bool tsv, EmojiRow *out, i32 max)
{
    i32 n = 0;
    const char *p = text;
    if (!tsv) {
        p = strstr(text, "\"emojis\":[");
        if (!p) return -1;
        p += 10;
    }
    while (*p && n < max) {
        char key[96], emoji[96];
        i32 used = 0;
        if (tsv) {
            const char *end = p + strcspn(p, "\r\n"), *tab = (const char *)memchr(p, '\t', end - p);
            if (tab && tab - p < (i64)sizeof key && end - tab - 1 < (i64)sizeof emoji) {
                memcpy(key, p, tab - p);
                key[tab - p] = 0;
                memcpy(emoji, tab + 1, end - tab - 1);
                emoji[end - tab - 1] = 0;
                used = (i32)(end - p);
            }
        } else {
            while (*p == ' ' || *p == ',' || *p == '\r' || *p == '\n') p++;
            if (*p != '[') break;
            if (sscanf(p, "[\"%95[^\"]\",\"%95[^\"]\"]%n", key, emoji, &used) != 2 || !used) break;
        }
        if (used) {
            char check[96];
            emoji_key(key, check, sizeof check);
            if (key[0] && !strcmp(check, key) && strlen(key) < sizeof out->key && emoji_ok(emoji)) {
                str_set(out[n].key, key);
                str_set(out[n].emoji, emoji);
                n++;
            }
            p += used;
        }
        if (tsv) {
            while (*p && *p != '\n') p++;
            if (*p) p++;
        }
    }
    qsort(out, n, sizeof *out, emoji_cmp);
    return n;
}

// Puts `rows` in place of the list there was.
static void emoji_publish(EmojiRow *rows, i32 n)
{
    AcquireSRWLockExclusive(&g_emoji.lock);
    EmojiRow *old = g_emoji.rows;
    g_emoji.rows = rows;
    g_emoji.count = n;
    ReleaseSRWLockExclusive(&g_emoji.lock);
    free(old);
}

// The emoji for the game called `name` into out ("" and false for none). Any thread.
static bool gamedb_emoji(const char *name, char *out, i32 cap)
{
    EmojiRow want;
    emoji_key(name, want.key, sizeof want.key);
    out[0] = 0;
    if (!want.key[0]) return false;
    AcquireSRWLockShared(&g_emoji.lock);
    const EmojiRow *r =
        g_emoji.rows ? (const EmojiRow *)bsearch(&want, g_emoji.rows, g_emoji.count, sizeof want, emoji_cmp) : 0;
    if (r) snprintf(out, cap, "%s", r->emoji);
    ReleaseSRWLockShared(&g_emoji.lock);
    return out[0] != 0;
}

// Loads emojis.txt, then, online and when it is a day old or missing, fetches the list again and keeps it there.
static void gamedb_emojis(bool fetch)
{
    Path file = path_join(gamedb_dir(), L"emojis.txt");
    EmojiRow *rows = (EmojiRow *)calloc(EMOJI_MAX, sizeof(EmojiRow));
    if (!rows) return;
    if (char *text = read_file(file.s, 0)) {
        i32 n = emoji_parse(text, true, rows, EMOJI_MAX);
        free(text);
        emoji_publish(rows, n);
        rows = (EmojiRow *)calloc(EMOJI_MAX, sizeof(EmojiRow));
        if (!rows) return;
    }
    if (!fetch || !g_gamedb.service[0]) {
        free(rows);
        return;
    }
    char *body = 0;
    i64 len = 0;
    u32 status = art_service_get("/v1/emojis", &body, &len);
    i32 n = status == 200 && body ? emoji_parse(body, false, rows, EMOJI_MAX) : -1;
    free(body);
    if (n < 0) {
        log_line("game emojis: couldn't fetch them (%u)", status);
        free(rows);
        return;
    }
    Path part = file;
    wcsncat(part.s, L".part", MAX_PATH - wcslen(part.s) - 1);
    FILE *f = _wfopen(part.s, L"wb");
    bool ok = f != 0;
    for (i32 i = 0; f && i < n; i++) ok &= fprintf(f, "%s\t%s\n", rows[i].key, rows[i].emoji) > 0;
    if (f) ok &= fclose(f) == 0;
    if (!ok || !MoveFileExW(part.s, file.s, MOVEFILE_REPLACE_EXISTING)) log_line("game emojis: couldn't keep them");
    emoji_publish(rows, n);
    log_line("game emojis: %d", n);
}

// Whether the emoji list is due to be fetched again: missing, or a day old.
static bool gamedb_emojis_due()
{
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(path_join(gamedb_dir(), L"emojis.txt").s, GetFileExInfoStandard, &fa)) return true;
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    i64 at = (i64)fa.ftLastWriteTime.dwHighDateTime << 32 | fa.ftLastWriteTime.dwLowDateTime;
    return ((i64)now.dwHighDateTime << 32 | now.dwLowDateTime) - at > 24 * 3600 * 10000000ll;
}

static DWORD WINAPI gamedb_thread(void *)
{
    Path dir = gamedb_dir(), json = path_join(dir, L"detectable.json"), tsv = path_join(dir, L"games.tsv");
    lookups_load();
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
    gamedb_emojis(g_gamedb.online && gamedb_emojis_due());
    while (!g_gamedb.stop) {
        bool done = gamedb_fetch_looks();
        done &= gamedb_fetch_art();
        if (g_gamedb.online && gamedb_emojis_due()) gamedb_emojis(true);
        // some waiting their turn: look again soon; else within the hour, for the emojis
        WaitForSingleObject(g_gamedb.wake, done ? 3600 * 1000 : 2000);
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

// Points the lookups at another art service host, or at none with "", while they run. Workers read the host
// without a lock, so a host is written in behind an empty first byte and the first byte goes in last.
static void gamedb_set_service(const char *host)
{
    if (!strcmp(g_gamedb.service, host)) return;
    g_gamedb.service[0] = 0;
    MemoryBarrier();
    if (!host[0]) return;
    snprintf(g_gamedb.service + 1, sizeof g_gamedb.service - 1, "%s", host + 1);
    MemoryBarrier();
    g_gamedb.service[0] = host[0];
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
