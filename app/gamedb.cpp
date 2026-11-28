// The games database: which exe is which game, its proper name, and where its icon and hero are.
//
// Rekodr fetches it itself; nothing is shipped with it. With Rekodr's service on (ART_SERVICE_HOST; "art_service"
// in settings.txt names another, "" none), a game that starts is looked up there (POST /v1/lookup): the service
// names it and says where its icon and hero (a cover for a game with no hero) are. Only a game is asked about: one sorted as a Game, one
// Discord's list knows, or one a store installed (storeids.cpp). With the service off nothing goes to it: names
// come from Discord's public list of the games it can detect (about 25,000, each with its exe names, the folders
// they sit in and, for an exe many programs share, the command line that tells them apart), and art from
// SteamGridDB with the player's own key ("sgdb_key"), else Discord's image server. Discord's list is also what is
// used when the service can't be asked. All of it is kept under <local>\gamedb:
//   detectable.json   Discord's list as fetched, again after a day
//   games.tsv         the same, one game a line, which is what is loaded:
//                     id, name, icon, cover, Steam id, other names (|), exes (|; "!" first marks a launcher,
//                     ">" a shared exe; the command line it needs follows a \x1f)
//   lookups\<hash>.json   the service's answer to a lookup (its ETag "v" in it), asked again after a day
//   art\<id>-art.json     where a game's pictures are: the service's answer (a lookup's or /v1/art's), or with the
//                         service off, what SteamGridDB has. Asked again after a day (a week with it off)
//   art\<id>-icon.png, art\<id>-art.jpg   the pictures, each with a .url file beside it naming the address it came
//                         from ("none" = there is none); one whose address isn't the art file's any more is fetched
//                         again
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

enum { GAMEDB_LOADED = 1, GAMEDB_ART = 2, GAMEDB_RESOLVED = 3, GAMEDB_FOUND = 6 }; // FOUND: a search answered
enum GameArt { ART_ICON, ART_HERO };

// What the service said to a lookup. `failed`: it couldn't be asked and nothing it said before is kept, so Discord's
// list decides.
struct LookAnswer {
    char name[128], key[48], how[16]; // how: its "matched_by"
    f32  confidence;
    char ask_args[1344];              // a shared exe: what to look for in its command line, one a line; "" = none
    bool software;                    // a Steam app Steam's store calls software, not a game ("steam_type")
    bool launcher;                    // the public files' row it came from is a launcher's
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
    char       service_key[72]; // what requests to it are signed with (gamedb_set_service)
    char       sgdb_key[72]; // the player's own SteamGridDB key; "" = none
    // Art asked for, not fetched yet, newest first. `paced`: it needs the service, which has been asked enough for
    // now. `quiet`: a picture is there, and only whether it is still the one to show is looked at.
    struct { char id[48], key[48], hash[40], name[128]; u32 steam; GameArt kind; bool paced, quiet; } want[64];
    i32        want_count;
    // Lookups and games the player picked (`pick`), by a hash of what is sent: being sent (body set) or answered.
    // The oldest answer makes room.
    struct { u64 hash; char *body; bool answered, pick; LookAnswer ans; } looks[64];
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
    // The icons searches gave, by key: a game picked from a search shows it without asking /v1/art.
    struct { char key[48], url[256]; } search_icons[32];
    i32        search_icon_next;
    // The service's public files (static_lookup), by number: to be fetched, and when one was last tried (seconds
    // of GetTickCount64, 0 = not this run).
    volatile u8 shard_want[4096];
    u32        shard_tried[4096];
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
    SHCreateDirectoryExW(0, path_join(d, L"g").s, 0);
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
// only kept for a 200, and with `etag` its ETag header too ("" for none).
// A host written "http://127.0.0.1:<port>" (or localhost) is a test service on this PC: plain http to that port.
// No other http address is asked.

// The port of a test service on this PC, from its host; 0 for any other host.
static u16 loopback_port(const wchar_t *host)
{
    const wchar_t *const loops[] = {L"http://127.0.0.1:", L"http://localhost:"};
    for (const wchar_t *l : loops) {
        size_t n = wcslen(l);
        if (wcsncmp(host, l, n)) continue;
        wchar_t *end = 0;
        long port = wcstol(host + n, &end, 10);
        return port > 0 && port < 65536 && end && !*end ? (u16)port : 0;
    }
    return 0;
}

static u32 http_get(const wchar_t *host, const wchar_t *path, const wchar_t *headers, const Path *file,
                    char **body = 0, i64 *body_len = 0, const char *post = 0, char *etag = 0, i32 etag_cap = 0)
{
    bool ok = false;
    u32 answer = 0;
    if (etag && etag_cap) etag[0] = 0;
    u16 port = loopback_port(host);
    if (!port && wcsstr(host, L"://")) return 0;
    Path part = {};
    if (file) {
        part = *file;
        wcsncat(part.s, L".part", MAX_PATH - wcslen(part.s) - 1);
    }
    char *mem = 0;
    i64 mem_len = 0, mem_cap = 0, file_len = 0;
    HINTERNET session = WinHttpOpen(L"rekodr", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, 0, 0, 0);
    HINTERNET conn = !session ? 0 : port ? WinHttpConnect(session, L"127.0.0.1", port, 0)
                                         : WinHttpConnect(session, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET req = conn ? WinHttpOpenRequest(conn, post ? L"POST" : L"GET", path, 0, 0, 0, port ? 0 : WINHTTP_FLAG_SECURE) : 0;
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
        if (answer == 200 && etag && etag_cap) {
            wchar_t w[128];
            DWORD wl = sizeof w;
            if (WinHttpQueryHeaders(req, WINHTTP_QUERY_ETAG, 0, w, &wl, 0)) wide_to_utf8(w, etag, etag_cap);
        }
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

// An exe name or path as the list is matched on: lower case, with spaces, '-' and '_' left out, so "Endless Sky.exe"
// is "endless sky/endlesssky.exe"'s file. Returns the length written to out.
static i32 gdb_squeeze(const char *s, i32 len, char *out, i32 cap)
{
    i32 n = 0;
    for (i32 i = 0; i < len && s[i] && n < cap - 1; i++) {
        char c = s[i] == '\\' ? '/' : (char)tolower((u8)s[i]);
        if (c != ' ' && c != '-' && c != '_') out[n++] = c;
    }
    out[n] = 0;
    return n;
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
            char key[96];
            fl = gdb_squeeze(file, fl, key, sizeof key);
            if (fl) ix->exes[ix->exe_count++] = {fnv64(key, fl), i};
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
// A copy unpacked somewhere else (an itch or DRM-free game in its own folder) still is the entry whose folders
// don't fit, when no other entry in the list has an exe of that file name: a name many share (game.exe) isn't one.
// Names are matched squeezed (gdb_squeeze); one that fits only squeezed is the game when no other entry's squeezes
// to the same name.
// The choosing, for Discord's list here and for the rows of the service's public files (static_lookup): start with
// what is known of the running exe, add each way a game's exe is written (`ent`, "folders/file" as the list spells
// it, the argument it needs, whether it is a launcher's and whether it must be told apart by its arguments, and the
// game: `row` and its name), then pick. pick gives the row, or -1, and with *shared that only the command line and
// the title could tell: ask again with them.
struct ExePick {
    char file[96], where[MAX_PATH * 2], title_letters[256], cmd_letters[2048];
    const char *exe, *cmd;
    i32 fl, wl, cl;
    i32 best, best_row, guess_row, file_row, moved_row;
    bool best_launcher, wants_more, file_many, moved_launcher, best_loose;

    void start(const char *exe_, const char *path, const char *cmdline, const char *title)
    {
        static thread_local char low[32768]; // a modded Minecraft's command line is long: every library is on it
        exe = exe_;
        fl = gdb_squeeze(exe, (i32)strlen(exe), file, sizeof file);
        wl = gdb_squeeze(path, (i32)strlen(path), where, sizeof where);
        for (cl = 0; cmdline[cl] && cl < (i32)sizeof low - 1; cl++) low[cl] = (char)tolower((u8)cmdline[cl]);
        low[cl] = 0;
        cmd = low;
        char seen[512];
        snprintf(seen, sizeof seen, "%s", title);
        gdb_letters(seen, title_letters, sizeof title_letters);
        gdb_letters(cmd, cmd_letters, sizeof cmd_letters);
        best = 0;
        best_row = guess_row = file_row = moved_row = -1;
        best_launcher = wants_more = file_many = moved_launcher = best_loose = false;
    }

    void add(const char *ent, i32 len, const char *want_args, bool launcher, bool strict, i32 row, const char *game)
    {
        char raw[200], name[200], args[200];
        snprintf(raw, sizeof raw, "%.*s", len, ent);
        gdb_squeeze(raw, len, name, sizeof name);
        snprintf(args, sizeof args, "%s", want_args);
        for (char *c = args; *c; c++) *c = (char)tolower((u8)*c);
        // The file name must be this exe's; folders named with it must be where it runs from
        const char *slash = strrchr(name, '/');
        if (strcmp(slash ? slash + 1 : name, file)) return;
        if (file_row < 0) file_row = row;
        else if (file_row != row) file_many = true;
        const char *raw_slash = strrchr(raw, '/');
        bool loose = _stricmp(raw_slash ? raw_slash + 1 : raw, exe) != 0; // fits only squeezed
        i32 score = loose ? 1 : 2;
        if (slash && wl) { // where it runs from isn't always known; then the file name has to do
            size_t n = strlen(name);
            if ((size_t)wl < n + 1 || strcmp(where + wl - n, name) || where[wl - n - 1] != '/') {
                if (!args[0] && !strict && moved_row < 0) moved_row = row, moved_launcher = launcher;
                return;
            }
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
                i32 n = gdb_letters(game, letters, sizeof letters);
                if (n >= 5 && guess_row < 0 && (strstr(title_letters, letters) || strstr(cmd_letters, letters)))
                    guess_row = row;
                if (!slash || !wl) return;
            }
        }
        if (score > best) { best = score; best_row = row; best_launcher = launcher; best_loose = loose; }
    }

    i32 pick(bool *launcher, bool *shared)
    {
        if (best_row >= 0 && best_loose && file_many) best_row = -1;
        if (best_row < 0 && guess_row >= 0) best_row = guess_row;
        if (best_row < 0 && moved_row >= 0 && !file_many) best_row = moved_row, best_launcher = moved_launcher;
        if (launcher) *launcher = best_row >= 0 && best_launcher;
        if (shared) *shared = best_row < 0 && wants_more;
        return best_row;
    }
};

// *shared is set when the exe is one that several programs share and what was given couldn't tell which it is:
// ask again with the command line and the title.
static bool gamedb_by_exe(const char *exe, const char *path, const char *cmdline, const char *title, GameInfo *out,
                          bool *shared = 0)
{
    ExePick ep;
    ep.start(exe, path, cmdline, title);
    u64 hash = fnv64(ep.file, ep.fl);
    AcquireSRWLockShared(&g_gamedb.lock);
    const GameIndex *ix = g_gamedb.index;
    i32 k = ix && ep.fl ? gdb_find(ix->exes, ix->exe_count, hash) : -1;
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
            char args[200] = "";
            if (nl < len) snprintf(args, sizeof args, "%.*s", len - nl - 1, ent + nl + 1);
            ep.add(ent, nl, args, launcher, strict, ix->exes[k].row, r.name);
        }
    }
    bool launcher = false;
    i32 row = ep.pick(&launcher, shared);
    if (row >= 0) {
        gdb_fill(ix->rows[row], out);
        out->launcher = launcher;
    }
    ReleaseSRWLockShared(&g_gamedb.lock);
    return row >= 0;
}

// ---------------------------------------------------------------------------------------------------------------
// A running copy of an exe, for the exes several games share: its command line, the title of its window, and when
// asked for, where it runs from (UTF-8) and the file name of the program that started it ("" when it has ended).

// The command line of process `p` (opened with PROCESS_QUERY_LIMITED_INFORMATION), "" when it can't be read.
// ProcessCommandLineInformation (60) gives it as a UNICODE_STRING, without reading the process's memory.
static void process_cmdline(HANDLE p, char *cmdline, i32 cap)
{
    cmdline[0] = 0;
    typedef LONG(WINAPI * QueryFn)(HANDLE, ULONG, void *, ULONG, ULONG *);
    static QueryFn query = (QueryFn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess");
    if (!p || !query) return;
    static thread_local u8 buf[65552]; // the most a UNICODE_STRING holds, and itself
    ULONG got = 0;
    if (query(p, 60, buf, sizeof buf, &got) < 0) return;
    struct { USHORT len, max; wchar_t *text; } *us = (decltype(us))buf;
    if (!us->text || !us->len) return;
    static thread_local wchar_t wide[32768];
    i32 n = us->len / 2 < COUNT(wide) - 1 ? us->len / 2 : COUNT(wide) - 1;
    memcpy(wide, us->text, n * sizeof(wchar_t));
    wide[n] = 0;
    wide_to_utf8(wide, cmdline, cap);
}

// Whether a java's command line starts a Minecraft client: its main class, a loader's or a launcher's wrapper around
// it, or the brand flag launchers set for the game.
static bool mc_client_cmdline(const char *cmd)
{
    static const char *const client[] = {
        "net.minecraft.client.main.Main", "cpw.mods.bootstraplauncher.BootstrapLauncher", "cpw.mods.modlauncher.Launcher",
        "net.fabricmc.loader.impl.launch.knot.KnotClient", "org.quiltmc.loader.impl.launch.knot.KnotClient",
        "net.minecraft.launchwrapper.Launch", "org.prismlauncher.EntryPoint", "org.multimc.EntryPoint",
        "-Dminecraft.launcher.brand=",
    };
    for (const char *c : client)
        if (strstr(cmd, c)) return true;
    return false;
}

// Of several running copies of one exe, which is the game: a Minecraft client (mc_client_cmdline) before one that
// isn't, then one with a window of its own. A modpack's helper (ATM10's crash assistant) and a launcher written in
// Java (ATLauncher) run as javaw.exe too. Ties go to the first; -1 if n is 0.
struct ProcessSeen { bool client, window; };
static i32 process_pick(const ProcessSeen *seen, i32 n)
{
    i32 best = -1, best_score = -1;
    for (i32 i = 0; i < n; i++) {
        i32 score = (seen[i].client ? 2 : 0) + (seen[i].window ? 1 : 0);
        if (score > best_score) best = i, best_score = score;
    }
    return best;
}

// The running copy of `exe` that is the game (process_pick) and the program that started it. 0 if none runs.
static DWORD process_find(const char *exe, DWORD *parent_pid = 0)
{
    wchar_t want[MAX_PATH];
    utf8_to_wide(exe, want, MAX_PATH);
    enum { MAX = 16 };
    DWORD pids[MAX], ppids[MAX];
    i32 n = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe = {};
    pe.dwSize = sizeof pe;
    for (BOOL ok = Process32FirstW(snap, &pe); ok && n < MAX; ok = Process32NextW(snap, &pe))
        if (!_wcsicmp(pe.szExeFile, want)) pids[n] = pe.th32ProcessID, ppids[n++] = pe.th32ParentProcessID;
    CloseHandle(snap);
    i32 at = n ? 0 : -1;
    if (n > 1) {
        struct Owners { DWORD *pids; i32 n; bool window[MAX]; } own = {pids, n, {}};
        EnumWindows([](HWND w, LPARAM lp) -> BOOL {
            Owners *o = (Owners *)lp;
            DWORD owner = 0;
            GetWindowThreadProcessId(w, &owner);
            if (!IsWindowVisible(w) || GetWindow(w, GW_OWNER)) return TRUE;
            for (i32 i = 0; i < o->n; i++)
                if (o->pids[i] == owner) o->window[i] = true;
            return TRUE;
        }, (LPARAM)&own);
        ProcessSeen seen[MAX];
        for (i32 i = 0; i < n; i++) {
            static thread_local char cmd[32768];
            HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pids[i]);
            process_cmdline(p, cmd, sizeof cmd);
            if (p) CloseHandle(p);
            seen[i] = {mc_client_cmdline(cmd), own.window[i]};
        }
        at = process_pick(seen, n);
    }
    if (at < 0) return 0;
    if (parent_pid) *parent_pid = ppids[at];
    return pids[at];
}

// The longest title of process pid's own visible windows (UTF-8), "" if it has none.
static void process_title(DWORD pid, char *title, i32 cap)
{
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
    wide_to_utf8(find.title, title, cap);
}

static bool process_facts(const char *exe, char *cmdline, i32 cmd_cap, char *title, i32 title_cap, char *image = 0,
                          i32 image_cap = 0, char *parent = 0, i32 parent_cap = 0)
{
    cmdline[0] = title[0] = 0;
    if (image) image[0] = 0;
    if (parent) parent[0] = 0;
    DWORD ppid = 0, pid = process_find(exe, &ppid);
    if (!pid) return false;
    process_title(pid, title, title_cap);
    if (parent && ppid) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        PROCESSENTRY32W pe = {};
        pe.dwSize = sizeof pe;
        if (snap != INVALID_HANDLE_VALUE) {
            for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
                if (pe.th32ProcessID == ppid) {
                    wide_to_utf8(pe.szExeFile, parent, parent_cap);
                    break;
                }
            CloseHandle(snap);
        }
    }

    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (p && image) {
        wchar_t full[MAX_PATH * 2];
        DWORD n = COUNT(full);
        if (QueryFullProcessImageNameW(p, 0, full, &n)) wide_to_utf8(full, image, image_cap);
    }
    process_cmdline(p, cmdline, cmd_cap);
    if (p) CloseHandle(p);
    return true;
}

// The folder a running copy of a java runs in (the one process_facts reads), from its process parameters, without a
// trailing slash. Read from its memory, so only ever asked of a java. False if it isn't running or can't be read.
static bool process_cwd(const char *exe, char *out, i32 cap)
{
    out[0] = 0;
    DWORD pid = process_find(exe);
    if (!pid) return false;
    typedef LONG(WINAPI * QueryFn)(HANDLE, ULONG, void *, ULONG, ULONG *);
    static QueryFn query = (QueryFn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess");
    HANDLE p = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!p) return false;
    // ProcessBasicInformation (0) gives the PEB; its ProcessParameters (+0x20) hold CurrentDirectory.DosPath (+0x38)
    struct { LONG exit; void *peb; ULONG_PTR affinity; LONG priority; ULONG_PTR pid, parent; } pbi = {};
    struct { USHORT len, max; wchar_t *text; } dir = {};
    void *params = 0;
    static thread_local wchar_t w[32768];
    bool ok = query && query(p, 0, &pbi, sizeof pbi, 0) >= 0 && pbi.peb &&
              ReadProcessMemory(p, (u8 *)pbi.peb + 0x20, &params, sizeof params, 0) && params &&
              ReadProcessMemory(p, (u8 *)params + 0x38, &dir, sizeof dir, 0) && dir.text && dir.len &&
              ReadProcessMemory(p, dir.text, w, dir.len, 0);
    CloseHandle(p);
    if (!ok) return false;
    i32 n = dir.len / 2;
    while (n > 3 && (w[n - 1] == '\\' || w[n - 1] == '/')) n--; // "C:\" keeps its slash
    w[n] = 0;
    wide_to_utf8(w, out, cap);
    return out[0] != 0;
}

// Games other than Minecraft that run as java.exe or javaw.exe. Which one a running java is goes by where it runs
// from (the Java each ships in its own folder: java_vm/ from Steam, runtime/ from a game's own installer), then by
// the program that started it, then by its command line; each test is tried on every game before the next. Lists
// are |-separated, lower case, with / for \. A java that is one of these is never taken for Minecraft.
struct JavaGame {
    const char *name;
    u32 steam;
    const char *folders, *parents, *args;
};
static const JavaGame g_java_games[] = {
    {"Starsector", 0, "/starsector/", "starsector.exe", "com.fs.starfarer."},
    {"Slay the Spire", 646570, "/slaythespire/jre/", "", "mts-launcher.jar|modthespire"}, // played with mods
    {"Spiral Knights", 99900, "/spiral knights/java_vm/|/spiral knights/runtime/", "spiral knights.exe", "com.threerings.projectx."},
    {"Puzzle Pirates", 99910, "/puzzle pirates/java_vm/|/puzzle pirates/runtime/", "puzzle pirates.exe", "com.threerings.yohoho."},
    {"Wakfu", 215080, "/wakfu/jre/", "ankama launcher.exe", ""},
    {"StarMade", 244770, "/starmade/", "", "starmade.jar"},
    {"Wurm Unlimited", 366220, "/wurm unlimited/", "", ""},
    {"Wurm Online", 1179680, "/wurm online/", "wurmlauncher64.exe|wurmlauncher.exe", ""},
};

static bool is_java_exe(const char *exe) { return !_stricmp(exe, "java.exe") || !_stricmp(exe, "javaw.exe"); }

// The game a shared exe (java) was named as, kept while its name is taken away: by a look that couldn't tell it
// apart (seen before its window is up), or by the exe running from another folder (another game's Java). The next
// name it gets is compared with this one, so another game ends the open session rather than renaming it.
static struct { char exe[64], name[128]; } g_untold[8];
static void gamedb_untold_put(const char *exe, const char *name)
{
    i32 k = -1; // its own slot, else a free one, else the first
    for (i32 i = 0; i < COUNT(g_untold) && k < 0; i++)
        if (!strcmp(g_untold[i].exe, exe)) k = i;
    for (i32 i = 0; i < COUNT(g_untold) && k < 0; i++)
        if (!g_untold[i].exe[0]) k = i;
    if (k < 0) k = 0;
    str_set(g_untold[k].exe, exe);
    str_set(g_untold[k].name, name);
}


// Whether any of a |-separated list is in `s` (`whole`: is all of it).
static bool java_list_has(const char *list, const char *s, bool whole)
{
    for (const char *p = list; *p;) {
        const char *e = strchr(p, '|');
        i32 n = (i32)(e ? e - p : strlen(p));
        char one[96];
        snprintf(one, sizeof one, "%.*s", n, p);
        if (one[0] && (whole ? !strcmp(s, one) : strstr(s, one) != 0)) return true;
        p = e ? e + 1 : p + n;
    }
    return false;
}

// The Java game a running java is, from where it runs from, its parent's file name and its command line (any may
// be ""). 0 if none of them.
static const JavaGame *java_game(const char *image, const char *parent, const char *cmdline)
{
    static thread_local char low[3][32768];
    const char *in[3] = {image, parent, cmdline};
    for (i32 k = 0; k < 3; k++) {
        i32 n = 0;
        for (; in[k][n] && n < (i32)sizeof low[k] - 1; n++) low[k][n] = in[k][n] == '\\' ? '/' : (char)tolower((u8)in[k][n]);
        low[k][n] = 0;
    }
    for (i32 k = 0; k < 3; k++)
        for (const JavaGame &g : g_java_games) {
            const char *list = k == 0 ? g.folders : k == 1 ? g.parents : g.args;
            if (low[k][0] && java_list_has(list, low[k], k == 1)) return &g;
        }
    return 0;
}

// A java that isn't Minecraft and isn't one of the Java games, named by its window: a game in Discord's list whose
// name is the whole title (Mindustry, whose entry is its Steam exe, played from its own Java). Any thread.
static bool java_titled_game(const char *cmdline, const char *title, GameInfo *out)
{
    return title[0] && !mc_client_cmdline(cmdline) && gamedb_by_name(title, out);
}

// Whether a running java counts as a game being played: a Minecraft client, one of the Java games, or one whose
// window names a game (java_titled_game). A modpack's helper (ATM10's crash assistant, which can outlive the game)
// or a launcher written in Java doesn't. One whose command line couldn't be read ("") counts, as nothing says it
// isn't the game.
static bool java_counts(const char *image, const char *parent, const char *cmdline, const char *title)
{
    GameInfo g;
    return !cmdline[0] || mc_client_cmdline(cmdline) || java_game(image, parent, cmdline) ||
           java_titled_game(cmdline, title, &g);
}

static void sessions_game_renamed(const char *exe, bool other_game); // sessions.cpp
static bool mc_clues(const char *cmdline, const char *cwd, char *out, i32 cap); // mcpack.cpp
static void mc_put(char *out, i32 cap, i32 *n, const char *s); // mcpack.cpp
static bool exe_clues(const char *exe, const char *path, const char *name, bool game, const char *more, char *out,
                      i32 cap); // storeids.cpp

enum StaticKind { STATIC_WAIT, STATIC_LOOKUP, STATIC_ANSWER, STATIC_SHARED };
static StaticKind static_lookup(const char *exe, const char *path, const char *cmdline, const char *title,
                                LookAnswer *ans);

// Whether games are looked up on the service: online, and it is on.
static bool gamedb_service_on() { return g_gamedb.online && g_gamedb.service[0]; }

static void seen_put(const char *key, const char *clues);

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

// Sends `body` to the service: a lookup (POST /v1/lookup), or with `pick` a game the player picked (POST
// /v1/pick). The same body is sent once a run. A lookup returns 1 with *ans once it is answered, 0 while it is
// being asked. Main thread.
static i32 gamedb_lookup(const char *body, LookAnswer *ans, bool pick = false)
{
    u64 hash = fnv64(body, strlen(body)) ^ (pick ? 1 : 0);
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
            q.pick = pick;
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

// Which of the arguments the service asked about (one a line) the command line holds: a JSON array, lower case.
// One counts only as whole arguments in a row ("-game garrysmod"), never inside one, so an answer can't tell
// anything about the rest of the line, such as a launcher's sign-in token. lookup_parse keeps only short asks.
static void args_has_json(const char *ask_args, const char *cmdline, char *out, i32 cap)
{
    // The command line's arguments, lower case, quotes taken off
    static char low[32768];
    static const char *arg[1024];
    i32 cl = 0, na = 0;
    for (const char *c = cmdline; *c && cl < (i32)sizeof low - 2 && na < (i32)COUNT(arg);) {
        while (*c == ' ' || *c == '\t') c++;
        if (!*c) break;
        arg[na++] = low + cl;
        for (bool quoted = false; *c && (quoted || (*c != ' ' && *c != '\t')) && cl < (i32)sizeof low - 2; c++)
            if (*c == '"') quoted = !quoted;
            else low[cl++] = (char)tolower((u8)*c);
        low[cl++] = 0;
    }
    i32 n = snprintf(out, cap, "[");
    for (const char *p = ask_args; *p;) {
        const char *e = strchr(p, '\n');
        char one[128], words[128];
        snprintf(one, sizeof one, "%.*s", (int)(e ? e - p : strlen(p)), p);
        for (char *c = one; *c; c++) *c = (char)tolower((u8)*c);
        const char *w[4];
        i32 nw = 0;
        str_set(words, one);
        for (char *t = strtok(words, " "); t && nw < (i32)COUNT(w); t = strtok(0, " ")) w[nw++] = t;
        bool found = false;
        for (i32 a = 0; nw && a + nw <= na && !found; a++) {
            found = true;
            for (i32 k = 0; k < nw && found; k++) found = !strcmp(arg[a + k], w[k]);
        }
        if (found) {
            if (n > 1 && n < cap - 1) out[n++] = ',';
            mc_put(out, cap, &n, one);
        }
        p = e ? e + 1 : p + strlen(p);
    }
    snprintf(out + n, cap - n, "]");
}

// An argument the service may ask about: 1 to 3 words of letters, digits and "-_.=", 64 bytes at most, one space
// between words.
static bool ask_arg_ok(const char *s)
{
    i32 len = (i32)strlen(s), words = 1;
    if (!len || len > 64 || s[0] == ' ' || s[len - 1] == ' ') return false;
    for (const char *c = s; *c; c++) {
        if (*c == ' ') {
            if (c[1] == ' ' || ++words > 3) return false;
        } else if (!isalnum((u8)*c) && !strchr("-_.=", *c)) return false;
    }
    return true;
}

// What a lookup sends about an app: its clues (exe_clues), with its File description, a running Minecraft's
// (mc_clues, from `cmdline` and the folder it runs in) and, with `args_has`, which of the strings the service asked about its command line
// holds. False if it isn't to be asked about.
static bool lookup_body(const AppInfo &a, bool game, const char *cmdline, const char *args_has, char *out, i32 cap)
{
    static char more[16384], mc[12288];
    char cwd[MAX_PATH * 2] = "";
    i32 n = 0;
    more[0] = 0;
    if (cmdline[0] && is_java_exe(a.exe.s)) process_cwd(a.exe.s, cwd, sizeof cwd);
    if (cmdline[0] && mc_clues(cmdline, cwd, mc, sizeof mc)) {
        size_t l = strlen(mc); // {"mc":{...}} -> ,"mc":{...}
        if (l > 2 && mc[0] == '{') n += snprintf(more + n, sizeof more - n, ",%.*s", (int)(l - 2), mc + 1);
    }
    if (args_has[0] && n < (i32)sizeof more) snprintf(more + n, sizeof more - n, ",\"args_has\":%s", args_has);
    char desc[128] = "";
    exe_description(a.path, desc, sizeof desc);
    return exe_clues(a.exe.s, a.path, desc, game, more, out, cap);
}

// The clues last sent in a lookup for each exe (its command line's among them), so a pick names the same exe the
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

// The player picked a game for an exe, which was shown as `from` ("" = nothing): the service is told it is `to`
// (POST /v1/pick), with the clues its lookup sent and the keys of both games ("" for `from` when it has none).
// Only a game with a key is sent, so a name the player typed stays on this PC. When enough players pick the same
// game for an exe, the service names it so for everyone. Only with the service on. Main thread.
static void gamedb_pick(const AppInfo &a, const char *from, const char *to)
{
    char to_key[48], from_key[48] = "";
    if (!gamedb_service_on() || !to[0] || !strcmp(from, to) || !gamedb_key_for(to, to_key, sizeof to_key)) return;
    if (from[0]) gamedb_key_for(from, from_key, sizeof from_key);
    static char more[1024], body[20 * 1024];
    i32 n = snprintf(more, sizeof more, ",\"to_key\":");
    mc_put(more, sizeof more, &n, to_key);
    n += snprintf(more + n, sizeof more - n, ",\"to\":");
    mc_put(more, sizeof more, &n, to);
    n += snprintf(more + n, sizeof more - n, ",\"from_key\":");
    mc_put(more, sizeof more, &n, from_key);
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
    log_line("games: told the service %s is %s (%s)", a.exe.s, to, to_key);
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

// A clip or session was moved from game `from` to `to` by hand: the service is told the program that was in front
// during it is that game (gamedb_pick, so only when `to` has a key). Only for a file saved from this buffer while
// the buffer still covers it. Main thread.
static void gamedb_clip_game(const LibItem &it, const char *from, const char *to)
{
    Config &c = g_app.config;
    char key[48];
    if (!gamedb_service_on() || !to[0] || !gamedb_key_for(to, key, sizeof key)) return;
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
    gamedb_pick(*a, from, to);
}

// Names an app: its description becomes the game's name, and one that hasn't been sorted yet (and that no folder
// rule sorts) becomes a Game. A Steam app Steam's store calls software (Wallpaper Engine) is never made a Game, and
// one only a Game folder rule sorts becomes an App (steam_type_check). With the service on, a game is looked up
// there; Discord's list names it while it is asked, and when the service can't be. An exe several games share is
// told apart by its command line: by the strings the service asks about, or by Discord's entries (else its window
// naming a game in the list). A java that is one of the Java games (java_game), or whose window is a game's name
// (java_titled_game), is that game, without asking. One neither knows goes by its exe's name until it is named by hand. Main thread. True if something about it changed.
static bool gamedb_name_app(AppInfo *a)
{
    GameInfo g;
    bool shared = false, known = gamedb_by_exe(a->exe.s, a->path, "", "", &g, &shared), facts = false;
    static char cmd[32768];
    char title[256];
    cmd[0] = title[0] = 0;
    bool java = false;
    if (!known && is_java_exe(a->exe.s)) {
        char image[MAX_PATH * 2], parent[MAX_PATH];
        facts = process_facts(a->exe.s, cmd, sizeof cmd, title, sizeof title, image, sizeof image, parent, sizeof parent);
        const JavaGame *j = facts ? java_game(image, parent, cmd) : 0;
        if (j) {
            if (!gamedb_by_name(j->name, &g)) {
                g = {};
                str_set(g.name, j->name);
            }
            if (!g.steam) g.steam = j->steam;
            known = java = true;
        } else if (facts && java_titled_game(cmd, title, &g)) {
            known = java = true;
        }
    }
    if (!known && shared) {
        if (!facts) facts = process_facts(a->exe.s, cmd, sizeof cmd, title, sizeof title);
        if (facts) {
            // Never the command line itself: launchers put sign-in tokens and the player's paths on it.
            DEV_LOG("gamedb: %s is shared; its window is \"%s\"", a->exe.s, title);
            known = gamedb_by_exe(a->exe.s, a->path, cmd, title, &g);
        }
    }
    AppCategory cat = app_category(g_app.config, *a);
    bool software = false, sorted = false, minecraft = false;
    if (!java && gamedb_service_on() && a->path[0] && (cat == CAT_GAME || cat == CAT_NONE)) {
        static char body[16384], has[2048];
        LookAnswer ans = {};
        bool game = cat == CAT_GAME || known || shared;
        i32 state = -1;
        if (lookup_body(*a, game, cmd, "", body, sizeof body)) {
            // The service's public files first; a lookup only for what they can't tell
            StaticKind k = static_lookup(a->exe.s, a->path, cmd, title, &ans);
            if (k == STATIC_SHARED) { // its rows want the command line and the window
                if (!facts) facts = process_facts(a->exe.s, cmd, sizeof cmd, title, sizeof title);
                k = facts ? static_lookup(a->exe.s, a->path, cmd, title, &ans) : STATIC_ANSWER;
                if (k == STATIC_SHARED) k = STATIC_ANSWER; // none of them, then
            }
            if (k == STATIC_ANSWER && ans.key[0]) seen_put(ans.key, body); // counted in the day's note
            state = k == STATIC_WAIT ? 0 : k == STATIC_ANSWER ? 1 : gamedb_lookup(body, &ans);
        }
        if (state == 1 && !ans.failed && !ans.name[0] && ans.ask_args[0]) { // a shared exe: which one is it?
            if (!facts) facts = process_facts(a->exe.s, cmd, sizeof cmd, title, sizeof title);
            args_has_json(ans.ask_args, cmd, has, sizeof has);
            state = facts && lookup_body(*a, game, cmd, has, body, sizeof body) ? gamedb_lookup(body, &ans) : -1;
        }
        if (state >= 0) last_look_put(a->exe.s, body);
        // Being asked. A shared exe already named waits for the answer too: its window coming up can match a game
        // here ("Minecraft") that the service names more closely (its modpack), and that isn't another game.
        if (state == 0 && (!known || (shared && a->desc.s[0]))) return false;
        software = state == 1 && !ans.failed && ans.software && g_app.config.steam_type_check;
        if (software && a->category == CAT_NONE && cat == CAT_GAME) {
            a->category = CAT_APP;
            log_line("%s: Steam's store calls it software, sorted as an app", a->exe.s);
            sorted = true;
        }
        // What it found only by a name is only taken for one sorted as a Game, or when it is sure
        if (state == 1 && !ans.failed && ans.name[0] &&
            (strcmp(ans.how, "name") || ans.confidence >= 0.9f || cat == CAT_GAME)) {
            bool launcher = (known && g.launcher) || ans.launcher;
            g = {};
            str_set(g.name, ans.name);
            str_set(g.key, ans.key);
            g.launcher = launcher;
            known = true;
            minecraft = !strcmp(ans.how, "mc") || !strcmp(ans.how, "mc_pack");
        }
    }
    // An exe several games share (java among them) that was named as one game and is now something else is
    // another program: its open session ends rather than taking the new name. It was a game when its description
    // is a name, not the exe's own File description (the runtime's). Minecraft named as one of its modpacks is
    // the same game, found out more about.
    // Its name before: the description, else the game's name taken away since (gamedb_untold_put).
    char own[128] = "";
    bool shared_exe = java || shared || is_java_exe(a->exe.s);
    if (shared_exe) exe_description(a->path, own, sizeof own);
    const char *old = a->desc.s;
    for (auto &u : g_untold)
        if ((!old[0] || !strcmp(old, own)) && !strcmp(u.exe, a->exe.s)) old = u.name;
    bool was_game = shared_exe && old[0] && strcmp(old, own) && !(minecraft && !strcmp(old, "Minecraft"));
    if (!known && shared) {
        if (!a->desc.s[0]) return sorted;
        if (was_game) gamedb_untold_put(a->exe.s, old);
        a->desc.s[0] = 0; // its File description is the runtime's, not a game's
        sessions_game_renamed(a->exe.s, was_game);
        return true;
    }
    // Still being asked: the name before is kept for when the answer comes
    if (!known) return sorted;
    for (auto &u : g_untold)
        if (!strcmp(u.exe, a->exe.s)) u.exe[0] = 0;
    bool changed = sorted;
    if (strcmp(a->desc.s, g.name)) {
        snprintf(a->desc.s, sizeof a->desc.s, "%s", g.name);
        changed = true;
        sessions_game_renamed(a->exe.s, was_game);
    }
    if (a->category == CAT_NONE && !g.launcher && cat == CAT_NONE && !software) {
        a->category = CAT_GAME;
        log_line("%s is %s: sorted as a game", a->exe.s, g.name);
        changed = true;
    }
    return changed;
}

// An exe several games share (java), played in a new process: the game it was named as is put away
// (gamedb_untold_put), as this one may be another game. It has no session until gamedb_name_app names it again.
static void gamedb_new_run(AppInfo *a)
{
    GameInfo g;
    bool shared = false;
    gamedb_by_exe(a->exe.s, a->path, "", "", &g, &shared);
    if (!shared && !is_java_exe(a->exe.s)) return;
    char own[128] = "";
    exe_description(a->path, own, sizeof own);
    if (!a->desc.s[0] || !strcmp(a->desc.s, own)) return;
    gamedb_untold_put(a->exe.s, a->desc.s);
    a->desc.s[0] = 0;
}

// ---------------------------------------------------------------------------------------------------------------
// The game and art service. Its calls, all signed:
//   POST /v1/lookup  an exe's clues -> {"game":{"key","name",...}|null, "matched_by", "confidence",
//                    "art":{"icon":{"url","w","h","from"}|null,"hero":..,"cover":..}, "v", "ask_args"?}
//                    (a cover only for a game with no hero)
//   GET /v1/art/<key>                -> {"key","art":{...},"v"}
//   GET /v1/search?q=<name>          -> {"games":[{"id":"s5367951","name":"...","year":2017,"icon":"<url>"|null},
//                                       ...]}: up to 8, "id" any key; with no icon, /v1/art has it
//   POST /v1/pick    a game the player picked: the clues + "to_key", "to", "from_key" -> 204
//   POST /v1/seen  {"version","games":[{"key","clues":{<a lookup's body>}}]} -> 2xx: once a day, the games the
//                  public files named (seen_send)
//   POST /v1/broken  {"version","key","kind":"icon"|"hero"|"cover","url"}: a picture that is gone -> {"ok":true}
//   GET /g/<nnn>.json  unsigned, the public files (static_lookup)
// "v" is the answer's ETag: asked again with If-None-Match, an answer that hasn't changed is a 304. A picture's
// address changes whenever the picture does.

// The signing key comes from the build: REKODR_SERVICE_KEY (build.bat reads it from the environment or
// ~\.secrets\rekodr-service-key).
#ifndef REKODR_SERVICE_KEY
#error REKODR_SERVICE_KEY is not defined
#endif
static const char ART_SERVICE_KEY[] = REKODR_SERVICE_KEY;

// A service of the player's own (art_service set to another host) has its own signing key: Options takes it and it
// is kept in Windows' Credential Manager, not settings.txt. A test copy (REKODR_HOME) keeps its own.
static void service_key_target(wchar_t *out, i32 cap)
{
    u32 h = test_home_hash();
    if (h) swprintf(out, cap, L"rekodr-%08x/service-key", h);
    else swprintf(out, cap, L"rekodr/service-key");
}

static void service_key_get(char *out, i32 cap)
{
    out[0] = 0;
    wchar_t target[64];
    service_key_target(target, COUNT(target));
    CREDENTIALW *cred = 0;
    if (!CredReadW(target, CRED_TYPE_GENERIC, 0, &cred)) return;
    i32 n = (i32)cred->CredentialBlobSize;
    if (n >= cap) n = cap - 1;
    memcpy(out, cred->CredentialBlob, n);
    out[n] = 0;
    CredFree(cred);
}

// Stores the key; "" removes it.
static void service_key_store(const char *s)
{
    wchar_t target[64];
    service_key_target(target, COUNT(target));
    if (!s[0]) {
        CredDeleteW(target, CRED_TYPE_GENERIC, 0);
        return;
    }
    CREDENTIALW cred = {};
    cred.Type = CRED_TYPE_GENERIC;
    cred.TargetName = target;
    cred.CredentialBlobSize = (DWORD)strlen(s);
    cred.CredentialBlob = (LPBYTE)s;
    cred.Persist = CRED_PERSIST_LOCAL_MACHINE;
    cred.UserName = (wchar_t *)L"rekodr";
    if (!CredWriteW(&cred, 0)) log_line("gamedb: couldn't store the service key (%d)", (int)GetLastError());
}

// The key requests to `host` are signed with: the player's own if one is set, else the built-in one, which only
// Rekodr's own host gets. "" = none, and then nothing is sent there.
static const char *service_key_for(const char *host, const char *own)
{
    if (!host[0]) return "";
    if (own[0]) return own;
    return strcmp(host, ART_SERVICE_HOST) ? "" : ART_SERVICE_KEY;
}

// HMAC-SHA256 of the parts, one after another, in hex.
static bool hmac_sha256_hex(const char *key, const char *const *parts, i32 n, char out[65])
{
    BCRYPT_ALG_HANDLE alg = 0;
    BCRYPT_HASH_HANDLE h = 0;
    u8 mac[32];
    bool ok = BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, 0, BCRYPT_ALG_HANDLE_HMAC_FLAG)) &&
              BCRYPT_SUCCESS(BCryptCreateHash(alg, &h, 0, 0, (PUCHAR)key, (ULONG)strlen(key), 0));
    for (i32 i = 0; ok && i < n; i++) ok = BCRYPT_SUCCESS(BCryptHashData(h, (PUCHAR)parts[i], (ULONG)strlen(parts[i]), 0));
    ok = ok && BCRYPT_SUCCESS(BCryptFinishHash(h, mac, sizeof mac, 0));
    if (h) BCryptDestroyHash(h);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    out[0] = 0;
    if (ok)
        for (i32 i = 0; i < 32; i++) snprintf(out + i * 2, 3, "%02x", mac[i]);
    return ok;
}

// The signature header (ART_SERVICE_SIG_HEADER) for a path (with its query, as sent): "<unix seconds>.<hmac of "seconds\npath">", and
// for a POST "seconds\npath\nbody". The body is hashed as it is, whatever its size.
static void art_service_sign(const char *key, const char *path, i64 seconds, char *out, i32 cap, const char *post = 0)
{
    char secs[24], mac[65];
    snprintf(secs, sizeof secs, "%lld\n", seconds);
    const char *parts[] = {secs, path, "\n", post};
    hmac_sha256_hex(key, parts, post ? 4 : 2, mac);
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
        art_service_sign(g_gamedb.service_key, path, unix_seconds, sig, sizeof sig, post);
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
            char gid[48] = "", gname[128] = "", k[16];
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

// Whether a picture's address is https on one of the hosts game pictures come from. No other is fetched, so an
// answer can't send every player to another address.
static bool image_url_ok(const char *url)
{
    static const char *const hosts[] = {"cdn2.steamgriddb.com", "cdn.steamgriddb.com", "cdn.discordapp.com",
                                        "shared.steamstatic.com", "images.igdb.com", "cdn.modrinth.com",
                                        "media.forgecdn.net", "cdn.feed-the-beast.com", "apps.modpacks.ch"};
    if (strncmp(url, "https://", 8)) return false;
    const char *host = url + 8;
    size_t hl = strcspn(host, "/?#");
    if (!host[hl] || host[hl] != '/') return false;
    for (const char *h : hosts)
        if (strlen(h) == hl && !_strnicmp(host, h, hl)) return true;
    return false;
}

// From an answer's "art", the address of a game's icon, or of its hero (a wide picture with no title on it, else
// the cover sent in its place), with `from` where the service had it and `which` which of the three it is. Only
// an address image_url_ok allows. False if it has neither.
static bool art_pick_url(const char *json, i64 len, GameArt kind, char *url, i32 cap, char *from = 0, i32 from_cap = 0,
                         const char **which = 0)
{
    url[0] = 0;
    if (from) from[0] = 0;
    const char *const kinds[2] = {kind == ART_ICON ? "icon" : "hero", kind == ART_ICON ? 0 : "cover"};
    for (const char *k : kinds) {
        char path[32];
        if (!k) break;
        snprintf(path, sizeof path, "art.%s.url", k);
        if (!j_get(json, len, path, url, cap) || !image_url_ok(url)) {
            url[0] = 0;
            continue;
        }
        snprintf(path, sizeof path, "art.%s.from", k);
        if (from) j_get(json, len, path, from, from_cap);
        if (which) *which = k;
        return true;
    }
    return false;
}

// Whether a file's first bytes are a picture's: PNG, JPEG, GIF or WebP.
static bool is_image_file(const Path &file)
{
    u8 b[12] = {};
    FILE *f = _wfopen(file.s, L"rb");
    size_t n = f ? fread(b, 1, sizeof b, f) : 0;
    if (f) fclose(f);
    return (n >= 8 && !memcmp(b, "\x89PNG\r\n\x1a\n", 8)) || (n >= 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF) ||
           (n >= 6 && (!memcmp(b, "GIF87a", 6) || !memcmp(b, "GIF89a", 6))) ||
           (n >= 12 && !memcmp(b, "RIFF", 4) && !memcmp(b + 8, "WEBP", 4));
}

// A picture fetched to a file, from an address image_url_ok allows. Returns the status (0 = not asked, or no
// answer); a 200 that isn't a picture (is_image_file) is 415, and nothing is kept.
static u32 http_get_url(const char *url, const Path &out)
{
    if (!image_url_ok(url)) return 0;
    const char *host = url + 8, *slash = strchr(host, '/');
    wchar_t whost[128], wpath[1024];
    char h[128];
    snprintf(h, sizeof h, "%.*s", (int)(slash - host), host);
    utf8_to_wide(h, whost, COUNT(whost));
    utf8_to_wide(slash, wpath, COUNT(wpath));
    Path got = out;
    wcsncat(got.s, L".new", MAX_PATH - wcslen(got.s) - 1);
    u32 status = http_get(whost, wpath, L"", &got);
    if (status == 200 && !is_image_file(got)) status = 415;
    if (status != 200 || !MoveFileExW(got.s, out.s, MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(got.s);
        return status == 200 ? 0 : status;
    }
    return 200;
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

// The games the public files named (static_lookup), each with the clues a lookup would have sent: the service is
// told them in one note a day (POST /v1/seen), as it counts the lookups it answers itself. Up to 30 go in a note;
// the rest wait for the next. <gamedb>\seen.txt keeps them, and the day a note last went, across starts: a "sent
// <yyyymmdd>" line, then "key<TAB>clues" lines. Only with the service on. Any thread.
static struct {
    SRWLOCK lock;
    bool    loaded;
    u32     sent_day, tried_day; // local yyyymmdd; tried: this run, a note that didn't go (asked again next start)
    i32     count;
    struct { char key[48]; u64 hash; char *clues; } items[64];
} g_seen_games;

static u32 seen_today()
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    return t.wYear * 10000u + t.wMonth * 100u + t.wDay;
}

static Path seen_file() { return path_join(gamedb_dir(), L"seen.txt"); }

static u64 seen_hash(const char *key, const char *clues)
{
    return fnv64(key, strlen(key)) * 31 ^ fnv64(clues, strlen(clues));
}

static void seen_load_locked()
{
    if (g_seen_games.loaded) return;
    g_seen_games.loaded = true;
    char *text = read_file(seen_file().s, 0);
    if (!text) return;
    char *save = 0;
    for (char *line = strtok_s(text, "\r\n", &save); line; line = strtok_s(0, "\r\n", &save)) {
        if (!strncmp(line, "sent ", 5)) {
            g_seen_games.sent_day = (u32)strtoul(line + 5, 0, 10);
            continue;
        }
        char *tab = strchr(line, '\t');
        if (!tab || tab == line || tab - line >= 48 || !tab[1] || g_seen_games.count >= COUNT(g_seen_games.items)) continue;
        auto &it = g_seen_games.items[g_seen_games.count];
        snprintf(it.key, sizeof it.key, "%.*s", (int)(tab - line), line);
        it.clues = _strdup(tab + 1);
        it.hash = seen_hash(it.key, tab + 1);
        if (it.clues) g_seen_games.count++;
    }
    free(text);
}

static void seen_save_locked()
{
    size_t cap = 32;
    for (i32 i = 0; i < g_seen_games.count; i++) cap += strlen(g_seen_games.items[i].key) + strlen(g_seen_games.items[i].clues) + 2;
    char *text = (char *)malloc(cap);
    if (!text) return;
    i64 n = snprintf(text, cap, "sent %u\n", g_seen_games.sent_day);
    for (i32 i = 0; i < g_seen_games.count; i++)
        n += snprintf(text + n, cap - (size_t)n, "%s\t%s\n", g_seen_games.items[i].key, g_seen_games.items[i].clues);
    write_whole(seen_file(), text, n);
    free(text);
}

// A game the public files named for an exe, with the clues (a /v1/lookup body). Once per game and clues until
// they are sent; none past 64 waiting. Main thread.
static void seen_put(const char *key, const char *clues)
{
    if (!gamedb_service_on() || !key[0] || !clues[0] || strlen(key) >= 48 || strpbrk(clues, "\t\r\n")) return;
    u64 h = seen_hash(key, clues);
    AcquireSRWLockExclusive(&g_seen_games.lock);
    seen_load_locked();
    bool have = false;
    for (i32 i = 0; i < g_seen_games.count && !have; i++) have = g_seen_games.items[i].hash == h;
    if (!have && g_seen_games.count < COUNT(g_seen_games.items)) {
        auto &it = g_seen_games.items[g_seen_games.count];
        str_set(it.key, key);
        it.hash = h;
        it.clues = _strdup(clues);
        if (it.clues) {
            g_seen_games.count++;
            seen_save_locked();
        }
    }
    ReleaseSRWLockExclusive(&g_seen_games.lock);
}

// The day's note: {"version":..,"games":[{"key":..,"clues":{..}},..]}, the first ones waiting, up to 30 and 32 KB.
// How many went in. Under the lock.
static i32 seen_note_locked(char *out, i32 cap)
{
    i32 n = snprintf(out, cap, "{\"version\":\"%s\",\"games\":[", REKODR_VERSION), took = 0;
    for (; took < g_seen_games.count && took < 30; took++) {
        auto &it = g_seen_games.items[took];
        if (n + (i64)strlen(it.key) * 2 + (i64)strlen(it.clues) + 32 >= cap) break;
        n += snprintf(out + n, cap - n, "%s{\"key\":", took ? "," : "");
        mc_put(out, cap, &n, it.key);
        n += snprintf(out + n, cap - n, ",\"clues\":%s}", it.clues);
    }
    snprintf(out + n, cap - n, "]}");
    return took;
}

// What an answer to a note does with the `took` games in it: any 2xx, they were told (the day's note has gone);
// 400, nothing in them was usable, so they go too; else they wait for the next start or day. Under the lock.
static void seen_answered_locked(u32 status, i32 took, u32 today)
{
    if (status / 100 != 2 && status != 400) {
        g_seen_games.tried_day = today;
        return;
    }
    for (i32 i = 0; i < took; i++) free(g_seen_games.items[i].clues);
    memmove(g_seen_games.items, g_seen_games.items + took, (g_seen_games.count - took) * sizeof g_seen_games.items[0]);
    g_seen_games.count -= took;
    if (status / 100 == 2) g_seen_games.sent_day = today;
    seen_save_locked();
}

// Sends the day's note when one is due: games waiting, none sent today, and none tried today this run. Worker
// thread; seen_put only adds after the ones being sent, so they stay where they are meanwhile.
static void seen_send()
{
    if (!gamedb_service_on()) return;
    static char note[32 * 1024];
    u32 today = seen_today();
    AcquireSRWLockExclusive(&g_seen_games.lock);
    seen_load_locked();
    bool due = g_seen_games.count && g_seen_games.sent_day != today && g_seen_games.tried_day != today;
    i32 took = due ? seen_note_locked(note, sizeof note) : 0;
    ReleaseSRWLockExclusive(&g_seen_games.lock);
    if (!took) return;
    u32 status = art_service_get("/v1/seen", 0, 0, note);
    AcquireSRWLockExclusive(&g_seen_games.lock);
    seen_answered_locked(status, took, today);
    ReleaseSRWLockExclusive(&g_seen_games.lock);
    log_line("games: the day's note of %d games named by the public files went to the service (%u)", took, status);
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
// {"art":{"icon":{"url","from":"sgdb"}|null,"hero":..,"cover":..}}. Its icon (as a PNG: an icon itself may be an
// .ico of many sizes), its first hero, and only with no hero its first wide grid as the cover. Into a malloc'd *out;
// false if it couldn't be asked.
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
    char icon[512] = "", hero[512] = "", cover[512] = "";
    if (sid[0]) {
        static const char *const kinds[3][2] = {{"/icons/game/%s?nsfw=false&humor=false", "thumb"},
                                                {"/heroes/game/%s?nsfw=false&humor=false", "url"},
                                                {"/grids/game/%s?dimensions=920x430,460x215&nsfw=false&humor=false", "url"}};
        char *urls[3] = {icon, hero, cover};
        for (i32 i = 0; i < 3 && !(i == 2 && hero[0]); i++) {
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
    for (i32 i = 1; i < 3; i++) {
        const char *u = i == 1 ? hero : cover;
        n += snprintf(*out + n, cap - n, ",\"%s\":", i == 1 ? "hero" : "cover");
        n += u[0] ? snprintf(*out + n, cap - n, "{\"url\":\"%s\",\"from\":\"sgdb\"}", u) : snprintf(*out + n, cap - n, "null");
    }
    n += snprintf(*out + n, cap - n, "}}");
    *out_len = n;
    return true;
}

// A search for a name: the service's, or with it off, SteamGridDB's with the player's own key. Returns the status
// (0: nothing to search with); the answer, as the service gives it, in *body. `typing`: the player is still typing
// it, so a 429 isn't waited out.
static u32 art_search(const char *q, char **body, i64 *len, bool typing = false)
{
    char enc[400], path[512];
    url_encode(q, enc, sizeof enc);
    if (g_gamedb.service[0]) {
        snprintf(path, sizeof path, "/v1/search?q=%s", enc);
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

// The games in a /v1/search answer, in its order: their names, years (0 = none), with `ids`
// their ids and with `icons` their icons' addresses ("" = none). How many.
static i32 art_search_list(const char *json, i64 len, char (*names)[128], i32 *years, i32 max, char (*ids)[48] = 0,
                           char (*icons)[256] = 0)
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
            char gname[128] = "", gid[48] = "", icon[256] = "", k[16];
            i32 year = 0;
            while (!j_eat(&j, '}') && j.p < j.end) {
                if (!j_string(&j, k, sizeof k)) break;
                j_eat(&j, ':');
                if (!strcmp(k, "name")) j_string(&j, gname, sizeof gname) || (j_skip(&j), 0);
                else if (!strcmp(k, "id")) j_string(&j, gid, sizeof gid) || (j_skip(&j), 0);
                else if (!strcmp(k, "icon")) j_string(&j, icon, sizeof icon) || (j_skip(&j), 0);
                else if (!strcmp(k, "year")) { j_ws(&j); year = atoi(j.p); j_skip(&j); }
                else j_skip(&j);
                j_eat(&j, ',');
            }
            j_eat(&j, ',');
            if (gname[0] && n < max) {
                snprintf(names[n], 128, "%s", gname);
                if (ids) snprintf(ids[n], 48, "%s", gid);
                if (icons) snprintf(icons[n], 256, "%s", image_url_ok(icon) ? icon : "");
                years[n++] = year > 0 ? year : 0;
            }
        }
        j_eat(&j, ']');
        j_eat(&j, ',');
    }
    return n;
}

// A search's icon for a key, kept (search_icons) or looked up (search_icon: false if none). Under the lock.
static void search_icon_put_locked(const char *key, const char *url)
{
    i32 at = -1;
    for (i32 i = 0; i < COUNT(g_gamedb.search_icons) && at < 0; i++)
        if (!strcmp(g_gamedb.search_icons[i].key, key)) at = i;
    if (at < 0) at = g_gamedb.search_icon_next++ % COUNT(g_gamedb.search_icons);
    str_set(g_gamedb.search_icons[at].key, key);
    str_set(g_gamedb.search_icons[at].url, url);
}

static bool search_icon(const char *key, char *url, i32 cap)
{
    url[0] = 0;
    AcquireSRWLockShared(&g_gamedb.lock);
    for (auto &si : g_gamedb.search_icons)
        if (key[0] && !strcmp(si.key, key)) snprintf(url, cap, "%s", si.url);
    ReleaseSRWLockShared(&g_gamedb.lock);
    return url[0] != 0;
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
    static char names[8][128], ids[8][48], icons[8][256];
    i32 years[8], n = status == 200 && body ? art_search_list(body, len, names, years, 8, ids, icons) : 0;
    free(body);
    DEV_LOG("gamedb: art service search for \"%s\" answered %u, %d games", q, status, n);
    AcquireSRWLockExclusive(&g_gamedb.lock);
    for (i32 i = 0; i < n; i++) {
        gamedb_named_put_locked(names[i], ids[i]);
        if (ids[i][0] && icons[i][0]) search_icon_put_locked(ids[i], icons[i]);
    }
    snprintf(g_gamedb.found_q, sizeof g_gamedb.found_q, "%s", q);
    g_gamedb.found_count = n;
    for (i32 i = 0; i < n; i++) {
        snprintf(g_gamedb.found[i].name, sizeof g_gamedb.found[i].name, "%s", names[i]);
        g_gamedb.found[i].year = years[i];
    }
    ReleaseSRWLockExclusive(&g_gamedb.lock);
    Event e = {}; // the name box shows them now, not at its next input
    e.kind = EV_GAMEDB;
    e.a = GAMEDB_FOUND;
    event_post(e);
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

// Where a game's icon or hero is: true if there is a picture. One not settled (art_settled) is asked for; while
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

// A picture from the service's answer that is gone or isn't one (POST /v1/broken): the game's key, which of its
// pictures (icon, hero or cover) and the address. Once a day for each address. Worker.
static void art_broken(const char *key, const char *which, const char *url)
{
    if (!g_gamedb.service[0] || !key[0] || !which) return;
    static struct { u64 hash; u32 day; } sent[64];
    static i32 next;
    u32 day = (u32)(GetTickCount64() / (24 * 3600 * 1000ull)) + 1;
    u64 h = fnv64(url, strlen(url));
    for (auto &b : sent)
        if (b.hash == h && b.day == day) return;
    sent[next++ % COUNT(sent)] = {h, day};
    static char body[1024];
    i32 n = snprintf(body, sizeof body, "{\"version\":\"%s\",\"key\":", REKODR_VERSION);
    mc_put(body, sizeof body, &n, key);
    n += snprintf(body + n, sizeof body - n, ",\"kind\":\"%s\",\"url\":", which);
    mc_put(body, sizeof body, &n, url);
    if (n >= (i32)sizeof body - 2) return;
    snprintf(body + n, sizeof body - n, "}");
    u32 status = art_service_get("/v1/broken", 0, 0, body);
    log_line("games: a picture of %s is broken; the service was told (%u)", key, status);
}

// Makes a game's icon or cover the one where its pictures are says (art_json_fetch first): fetched when its
// address changed, removed when there is none. Discord's own picture with the service off and no other. An icon a
// search gave (search_icon) is used while where the game's pictures are hasn't been asked.
// *replaced: a picture that was there was swapped for another. False if it couldn't be settled now (offline, or
// the picture didn't come); *paced when it waits its turn for the service.
static bool art_settle(const char *id, const char *key, const char *name, const char *hash, u32 steam, GameArt kind,
                       bool *replaced, bool *paced)
{
    *replaced = false;
    *paced = false;
    Path json = gamedb_art_json(id), pic = gamedb_art_file(id, kind), side = gamedb_art_file(id, kind, L".url");
    char url[512] = "", was[512] = "";
    const char *which = 0;
    bool searched = kind == ART_ICON && g_gamedb.service[0] && GetFileAttributesW(json.s) == INVALID_FILE_ATTRIBUTES &&
                    search_icon(key, url, sizeof url);
    if (searched) which = "icon";
    else if (!art_json_fetch(id, key, name, steam, paced)) return false;
    i64 jl = 0, sl = 0;
    char *text = searched ? 0 : read_file(json.s, &jl);
    if (!searched && !art_pick_url(text ? text : "", text ? jl : 0, kind, url, sizeof url, 0, 0, &which) && hash[0] &&
        !g_gamedb.service[0])
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
        u32 status = url[0] ? http_get_url(url, pic) : 200;
        if (status == 404 || status == 410 || status == 415) {
            // Gone, or not a picture: the service is told, and the game has none until its pictures are asked
            // about again
            art_broken(key, which, url);
            DeleteFileW(pic.s);
            *replaced = have;
            return write_whole(side, now, (i64)strlen(now));
        }
        if (status != 200) return false;
        if (!url[0]) DeleteFileW(pic.s);
        *replaced = have;
        DEV_LOG("gamedb: %s for %s %s", kind == ART_ICON ? "icon" : "hero", id, url[0] ? "fetched" : "none");
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
    char type[16];
    ans->software = j_get(text, len, "steam_type", type, sizeof type) && !strcmp(type, "software");
    Json j = {text, text + len};
    // At most 20 asks, each one ask_arg_ok allows; the rest go unanswered
    if (j_find(&j, "ask_args") && j_eat(&j, '[')) {
        i32 n = 0, asks = 0;
        while (!j_eat(&j, ']') && j.p < j.end) {
            char one[128];
            if (!j_string(&j, one, sizeof one)) { j_skip(&j); j_eat(&j, ','); continue; }
            if (ask_arg_ok(one) && asks < 20 && n + (i32)strlen(one) + 2 < (i32)sizeof ans->ask_args && ++asks)
                n += snprintf(ans->ask_args + n, sizeof ans->ask_args - n, "%s%s", n ? "\n" : "", one);
            j_eat(&j, ',');
        }
    }
    return true;
}

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

// An answer's "emoji" (a lookup's, /v1/art's or the art file's) into out when it is one (emoji_ok); "" and false
// for null or none.
static bool art_emoji(const char *text, i64 len, char *out, i32 cap)
{
    char e[64];
    out[0] = 0;
    if (!j_get(text, len, "emoji", e, sizeof e) || !emoji_ok(e)) return false;
    snprintf(out, cap, "%s", e);
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

// A lookup's answer about a game: its key for the name, where its pictures are and its emoji, as
// {"key","art","emoji"} (no "v": that is the lookup's, not /v1/art's). GAMEDB_ART (b = 1) when those changed, so
// what shows them looks again.
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
    char *old = read_file(file.s, &ol), emoji[64], was[64] = "";
    art_emoji(text, len, emoji, sizeof emoji);
    if (old) art_emoji(old, ol, was, sizeof was);
    bool had = old != 0, same = old && answer_art(old, ol, &oart, &oal) && oal == al && !memcmp(oart, art, (size_t)al) &&
                               !strcmp(was, emoji);
    free(old);
    if (same) {
        touch_file(file);
        return;
    }
    static char keep[16384];
    i32 n = snprintf(keep, sizeof keep, "{\"key\":");
    mc_put(keep, sizeof keep, &n, ans.key);
    if (n + al + 100 >= (i64)sizeof keep) return;
    n += snprintf(keep + n, sizeof keep - n, ",\"art\":%.*s,\"emoji\":", (int)al, art);
    if (emoji[0]) n += snprintf(keep + n, sizeof keep - n, "\"%s\"}", emoji);
    else n += snprintf(keep + n, sizeof keep - n, "null}");
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

// ---------------------------------------------------------------------------------------------------------------
// The service's public files: GET /g/<nnn>.json, unsigned, {"<name>": entry} for the exe names in it (static_name).
// An entry is one of
//   {"rows":[[folders, argument, flags, answer, file], ...], "answers":[answer, ...]}: the ways Discord writes the
//     exe, told apart here as Discord's own list is (ExePick; flags & 1: a launcher's, 0.6 sure; flags & 2: told
//     apart by its arguments), each with its game's place in "answers"
//   a whole answer, as a lookup gives it: the exe is that game wherever it is
//   {"ask":[...]}: only a lookup can tell
// A name with no entry is one only a lookup can tell too. Each file is kept in gamedb\g\ and checked again when it
// is a day old, with its ETag; one the service has none for is kept as {}.

// An exe's name as the public files have it: lower case (Windows' lower case, which covers accented letters),
// with no spaces, '-' or '_', in UTF-8, and its file's number: FNV-1a of its UTF-16 units, mod 4096. -1 for no
// name.
static i32 static_name(const char *exe, char *out, i32 cap)
{
    wchar_t w[128], q[128];
    utf8_to_wide(exe, w, COUNT(w));
    CharLowerBuffW(w, (DWORD)wcslen(w));
    i32 n = 0;
    for (const wchar_t *c = w; *c && n < COUNT(q) - 1; c++)
        if (*c != L' ' && *c != L'-' && *c != L'_') q[n++] = *c;
    q[n] = 0;
    u32 h = 0x811c9dc5;
    for (i32 i = 0; i < n; i++) h = (h ^ q[i]) * 0x01000193;
    wide_to_utf8(q, out, cap);
    return n ? (i32)(h % 4096) : -1;
}

static Path static_file(i32 shard, const wchar_t *ext)
{
    wchar_t name[32];
    swprintf(name, COUNT(name), L"g\\%03x%s", shard, ext);
    return path_join(gamedb_dir(), name);
}

// A whole number in JSON. False if there isn't one here.
static bool j_int(Json *j, i64 *out)
{
    j_ws(j);
    char num[24];
    i32 n = 0;
    while (j->p < j->end && n < (i32)sizeof num - 1 && (isdigit((u8)*j->p) || *j->p == '-')) num[n++] = *j->p++;
    num[n] = 0;
    *out = strtoll(num, 0, 10);
    return n > 0;
}

// An answer from the public files, kept as a lookup's is (lookup_keep_art), once a run for each.
static void static_keep(const char *text, i64 len, const LookAnswer &ans)
{
    static u64 kept[64];
    static i32 next;
    u64 h = fnv64(text, (size_t)len);
    for (u64 k : kept)
        if (k == h) return;
    kept[next++ % COUNT(kept)] = h;
    lookup_keep_art(text, len, ans);
}

// The answer an entry's rows give for the running exe. STATIC_SHARED when only its command line and window could
// tell; an answer with no game when none of them is it.
static StaticKind static_rows(const char *e, i64 el, const char *exe, const char *path, const char *cmdline,
                              const char *title, LookAnswer *ans)
{
    const char *at[32];
    i64 at_len[32];
    i32 an = 0;
    Json j = {e, e + el};
    if (j_find(&j, "answers") && j_eat(&j, '[')) {
        while (!j_eat(&j, ']') && j.p < j.end) {
            j_ws(&j);
            const char *v = j.p;
            j_skip(&j);
            if (an < COUNT(at)) at[an] = v, at_len[an++] = j.p - v;
            j_eat(&j, ',');
        }
    }
    ExePick ep;
    ep.start(exe, path, cmdline, title);
    j = {e, e + el};
    if (j_find(&j, "rows") && j_eat(&j, '[')) {
        while (j_eat(&j, '[')) {
            char folders[160] = "", args[200] = "", file[96] = "", game[128] = "";
            i64 flags = 0, n = -1;
            bool ok = j_string(&j, folders, sizeof folders) && j_eat(&j, ',') && j_string(&j, args, sizeof args) &&
                      j_eat(&j, ',') && j_int(&j, &flags) && j_eat(&j, ',') && j_int(&j, &n) && j_eat(&j, ',') &&
                      j_string(&j, file, sizeof file);
            while (!j_eat(&j, ']') && j.p < j.end) { j_skip(&j); j_eat(&j, ','); }
            j_eat(&j, ',');
            if (!ok || n < 0 || n >= an || !file[0]) continue;
            char ent[260];
            i32 len = snprintf(ent, sizeof ent, "%s%s%s", folders, folders[0] ? "/" : "", file);
            if (len <= 0 || len >= (i32)sizeof ent) continue;
            j_get(at[n], at_len[n], "game.name", game, sizeof game);
            ep.add(ent, len, args, (flags & 1) != 0, (flags & 2) != 0, (i32)n, game);
        }
    }
    bool launcher = false, shared = false;
    i32 row = ep.pick(&launcher, &shared);
    *ans = {};
    if (row < 0) {
        str_set(ans->how, "none");
        return shared && !cmdline[0] ? STATIC_SHARED : STATIC_ANSWER;
    }
    if (!lookup_parse(at[row], at_len[row], ans)) return STATIC_LOOKUP;
    ans->launcher = launcher;
    if (launcher) ans->confidence = 0.6f;
    static_keep(at[row], at_len[row], *ans);
    return STATIC_ANSWER;
}

// What the public files say about an exe: STATIC_ANSWER with *ans (no game in it when they say it is none),
// STATIC_LOOKUP when only a lookup can tell, STATIC_WAIT while its file is being fetched, STATIC_SHARED when the
// command line and the window are wanted (ask again with them). A file a day old is used while it is checked
// again. Main thread.
static StaticKind static_lookup(const char *exe, const char *path, const char *cmdline, const char *title,
                                LookAnswer *ans)
{
    *ans = {};
    char name[256];
    i32 shard = static_name(exe, name, sizeof name);
    if (shard < 0) return STATIC_LOOKUP;
    Path file = static_file(shard, L".json");
    WIN32_FILE_ATTRIBUTE_DATA fa;
    bool have = GetFileAttributesExW(file.s, GetFileExInfoStandard, &fa) != 0;
    u32 now = (u32)(GetTickCount64() / 1000) + 1, tried = g_gamedb.shard_tried[shard];
    if ((!have || file_older_than(fa, 24 * 3600)) && !g_gamedb.shard_want[shard] && (!tried || now - tried > 600)) {
        AcquireSRWLockExclusive(&g_gamedb.lock);
        g_gamedb.shard_want[shard] = 1;
        g_gamedb.shard_tried[shard] = tried = now;
        ReleaseSRWLockExclusive(&g_gamedb.lock);
        SetEvent(g_gamedb.wake);
    }
    if (!have) return g_gamedb.shard_want[shard] ? STATIC_WAIT : STATIC_LOOKUP;
    i64 len = 0;
    char *text = read_file(file.s, &len);
    if (!text) return STATIC_LOOKUP;
    const char *e = 0;
    i64 el = 0;
    Json j = {text, text + len};
    if (j_eat(&j, '{')) {
        while (!j_eat(&j, '}') && j.p < j.end) {
            char key[256];
            if (!j_string(&j, key, sizeof key)) break;
            j_eat(&j, ':');
            j_ws(&j);
            const char *v = j.p;
            j_skip(&j);
            if (!strcmp(key, name)) {
                e = v;
                el = j.p - v;
                break;
            }
            j_eat(&j, ',');
        }
    }
    StaticKind kind = STATIC_LOOKUP;
    if (e && *e == '{') {
        Json f = {e, e + el}, g = {e, e + el};
        if (j_find(&f, "rows")) kind = static_rows(e, el, exe, path, cmdline, title, ans);
        else if (j_find(&g, "ask")) kind = STATIC_LOOKUP;
        else if (lookup_parse(e, el, ans)) {
            static_keep(e, el, *ans);
            kind = STATIC_ANSWER;
        }
    }
    free(text);
    return kind;
}

// Fetches the public files static_lookup wants, each with the ETag it came with. GAMEDB_RESOLVED when one is new
// or changed, or couldn't be had, so the apps waiting for it are named.
static void gamedb_fetch_shards()
{
    for (i32 i = 0; i < COUNT(g_gamedb.shard_want) && !g_gamedb.stop; i++) {
        if (!g_gamedb.shard_want[i]) continue;
        Path file = static_file(i, L".json"), tag = static_file(i, L".etag");
        bool have = GetFileAttributesW(file.s) != INVALID_FILE_ATTRIBUTES;
        u32 status = 0;
        if (g_gamedb.service[0]) {
            char etag[128] = "", got[128] = "";
            i64 tl = 0;
            char *t = have ? read_file(tag.s, &tl) : 0;
            if (t && tl > 0 && tl < (i64)sizeof etag && !strpbrk(t, "\r\n")) snprintf(etag, sizeof etag, "%s", t);
            free(t);
            wchar_t host[128], path[32], header[200] = L"";
            utf8_to_wide(g_gamedb.service, host, COUNT(host));
            swprintf(path, COUNT(path), L"/g/%03x.json", i);
            if (etag[0]) swprintf(header, COUNT(header), L"If-None-Match: %hs\r\n", etag);
            status = http_get(host, path, header, &file, 0, 0, 0, got, sizeof got);
            if (status == 200) {
                if (got[0]) write_whole(tag, got, (i64)strlen(got));
                else DeleteFileW(tag.s);
            } else if (status == 304) {
                touch_file(file);
            } else if (status == 404) { // no exe name of the service's is in it
                write_whole(file, "{}", 2);
                DeleteFileW(tag.s);
            }
            DEV_LOG("gamedb: public file %03x answered %u", i, status);
        }
        AcquireSRWLockExclusive(&g_gamedb.lock);
        g_gamedb.shard_want[i] = 0;
        ReleaseSRWLockExclusive(&g_gamedb.lock);
        if (status == 304 || (have && status != 200 && status != 404)) continue;
        Event e = {};
        e.kind = EV_GAMEDB;
        e.a = GAMEDB_RESOLVED;
        event_post(e);
    }
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
        bool pick = at >= 0 && g_gamedb.looks[at].pick;
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
        if (pick) {
            u32 status = art_service_get("/v1/pick", 0, 0, body);
            DEV_LOG("gamedb: service pick answered %u", status);
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
        if (pick) continue;
        Event e = {};
        e.kind = EV_GAMEDB;
        e.a = GAMEDB_RESOLVED;
        event_post(e);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// The worker: load what is on disk, fetch the list if there is none or it is a day old, then look up games and
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
// Game emojis: what a Discord card puts before the game's name. Only some games have one: the service says which
// in a lookup's and /v1/art's "emoji" (null for none), kept with where the game's pictures are.

// The game's emoji for Discord cards into out ("" and false for none). Asks the service where the game's pictures
// are when that wasn't asked lately: waits on the network, not for the UI thread.
static bool gamedb_emoji(const char *name, char *out, i32 cap)
{
    out[0] = 0;
    GameInfo g;
    if (!g_gamedb.service[0] || !name || !name[0] || !gamedb_for_art(name, &g)) return false;
    bool paced;
    if (!art_json_fetch(g.id, g.key, g.name, g.steam, &paced)) return false;
    i64 len = 0;
    char *text = read_file(gamedb_art_json(g.id).s, &len);
    bool ok = text && art_emoji(text, len, out, cap);
    free(text);
    return ok;
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
    if (g_gamedb.online && (!have || age > 24 * 3600)) {
        if (http_get_file(L"discord.com", L"/api/v10/applications/detectable", json)) {
            i32 n = gamedb_convert(json, tsv);
            log_line("games list fetched: %d games", n);
            if (n) gamedb_publish(gamedb_load(tsv));
        } else if (!g_gamedb.stop) {
            log_line("games list: couldn't fetch it%s", have ? "; using the one from before" : "");
        }
    }
    while (!g_gamedb.stop) {
        gamedb_fetch_shards();
        bool done = gamedb_fetch_looks();
        done &= gamedb_fetch_art();
        seen_send();
        // some waiting their turn: look again soon
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

// Points the lookups and reports at another service host, or at none with "", while they run, signed with the key
// service_key_for picks; a host with no key to sign with counts as none. Workers read the host without a lock, so
// the key and the host are written in behind an empty first byte and the first byte goes in last.
static void gamedb_set_service(const char *want)
{
    char own[72];
    service_key_get(own, sizeof own);
    const char *key = service_key_for(want, own);
    const char *host = key[0] ? want : "";
    bool same = !strcmp(g_gamedb.service, host) && !strcmp(g_gamedb.service_key, key);
    if (!same) {
        g_gamedb.service[0] = 0;
        MemoryBarrier();
        str_set(g_gamedb.service_key, key);
    }
    SecureZeroMemory(own, sizeof own);
    if (same || !host[0]) return;
    snprintf(g_gamedb.service + 1, sizeof g_gamedb.service - 1, "%s", host + 1);
    MemoryBarrier();
    g_gamedb.service[0] = host[0];
}

static void gamedb_start(bool online)
{
    g_gamedb.online = online;
    gamedb_set_service(g_app.config.art_service.s);
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
