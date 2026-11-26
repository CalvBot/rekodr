// replays.cpp: markers from the match replays games save, read once the match is over. Only the player's own kills
// and deaths count. Each goes on the timeline at its moment, and into the layers file of any clip or session saved
// over it (markers_add_late), so it comes late but in its place.
//
//   Rainbow Six Siege  <game>\MatchReplay\<match>\*.rec, a file a round: a "dissect" header (the round's start in
//                      the PC's local time, the recording player's id, the players' names and ids) and zstd frames
//                      holding the round's packets. The round clock and the kill feed are found by their byte patterns, as
//                      r6-dissect (github.com/redraskal/r6-dissect, MIT) finds them.
//
// The files are untrusted data: every read is checked against the end.

struct ReplayMark {
    i64  t_us; // when, on the wall clock
    u32  event; // AM_KILL, AM_DEATH or AM_DOWNED
    u8   colour;
    char note[32];
};

static void replay_push(ReplayMark *out, i32 cap, i32 *n, i64 t_us, u32 event, u8 colour, const char *note)
{
    if (*n >= cap) return;
    ReplayMark &m = out[(*n)++];
    m.t_us = t_us;
    m.event = event;
    m.colour = colour;
    str_set(m.note, note);
}

// A time in the PC's local time zone, on the wall clock (UTC). 0 if it isn't one.
static i64 replay_local_us(const SYSTEMTIME &local)
{
    SYSTEMTIME us;
    FILETIME uf;
    if (!TzSpecificLocalTimeToSystemTime(0, &local, &us) || !SystemTimeToFileTime(&us, &uf)) return 0;
    i64 t = ((i64)uf.dwHighDateTime << 32) | uf.dwLowDateTime;
    return (t - 116444736000000000LL) / 10;
}

// ---------------------------------------------------------------------------------------------------------------
// Rainbow Six Siege

static const u8 R6_ZSTD_MAGIC[4] = {0x28, 0xB5, 0x2F, 0xFD};
static const i32 R6_Y9S1 = 8111697, R6_Y9S1_UPDATE3 = 8211379;
static const size_t R6_MAX_DATA = 512u << 20;

// The zstd frames in data[from, len) one after another, whatever is between them skipped, appended to *out (from
// malloc, *out_len long). False when nothing could be decoded.
static bool r6_unzstd(const u8 *data, size_t len, size_t from, u8 **out, size_t *out_len)
{
    ZSTD_DCtx *dc = ZSTD_createDCtx();
    if (!dc) return false;
    size_t cap = *out_len + (len - from) * 4 + (1 << 20);
    u8 *buf = (u8 *)realloc(*out, cap);
    if (!buf) {
        ZSTD_freeDCtx(dc);
        return false;
    }
    *out = buf;
    bool any = false;
    size_t at = from;
    while (at + 4 <= len) {
        if (memcmp(data + at, R6_ZSTD_MAGIC, 4)) {
            at++;
            continue;
        }
        ZSTD_DCtx_reset(dc, ZSTD_reset_session_only);
        ZSTD_inBuffer in = {data + at, len - at, 0};
        size_t r = 1;
        while (r) {
            if (cap - *out_len < (1 << 20)) {
                if (cap >= R6_MAX_DATA) break;
                cap *= 2;
                if (!(buf = (u8 *)realloc(*out, cap))) break;
                *out = buf;
            }
            ZSTD_outBuffer o = {*out + *out_len, cap - *out_len, 0};
            r = ZSTD_decompressStream(dc, &o, &in);
            *out_len += o.pos;
            if (ZSTD_isError(r) || (!o.pos && in.pos == in.size)) break;
        }
        if (!r) any = true;
        at += in.pos ? in.pos : 1;
        if (!buf || cap >= R6_MAX_DATA) break;
    }
    ZSTD_freeDCtx(dc);
    return any;
}

// One of the header's strings: u8 length, seven zero bytes, the bytes.
static bool r6_header_string(const u8 *d, size_t len, size_t *at, char *out, i32 cap)
{
    if (*at + 8 > len) return false;
    size_t n = d[*at];
    for (i32 i = 1; i < 8; i++)
        if (d[*at + i]) return false;
    *at += 8;
    if (*at + n > len) return false;
    size_t k = n < (size_t)cap - 1 ? n : (size_t)cap - 1;
    memcpy(out, d + *at, k);
    out[k] = 0;
    *at += n;
    return true;
}

// A kill feed string: u8 length, the bytes.
static bool r6_string(const u8 *d, size_t len, size_t *at, char *out, i32 cap)
{
    if (*at >= len) return false;
    size_t n = d[(*at)++];
    if (*at + n > len) return false;
    size_t k = n < (size_t)cap - 1 ? n : (size_t)cap - 1;
    memcpy(out, d + *at, k);
    out[k] = 0;
    *at += n;
    return true;
}

// "2024-05-04-02-14-08", the PC's local time, in UTC microseconds since 1970. 0 if it isn't one.
static i64 r6_datetime(const char *s)
{
    SYSTEMTIME st = {};
    int y, mo, d, h, mi, se;
    if (sscanf_s(s, "%d-%d-%d-%d-%d-%d", &y, &mo, &d, &h, &mi, &se) != 6 || y < 2015) return 0;
    st.wYear = (WORD)y, st.wMonth = (WORD)mo, st.wDay = (WORD)d, st.wHour = (WORD)h, st.wMinute = (WORD)mi, st.wSecond = (WORD)se;
    return replay_local_us(st);
}

struct R6Round {
    i64  start_us; // the header's datetime
    i32  code;     // the game's code version
    char me[64];   // the recording player's name
};

// The header: "dissect", a version block that ends with the second run of seven zero bytes, then key and value
// strings up to teamscore1. Each player's block starts with its playerid. Returns where the header ends, 0 if the
// data isn't one or the recording player isn't named in it.
static size_t r6_header(const u8 *d, size_t len, R6Round *r)
{
    *r = {};
    if (len < 7 || memcmp(d, "dissect", 7)) return 0;
    size_t at = 7;
    for (i32 zeros = 0, runs = 0; runs < 2; at++) {
        if (at >= len) return 0;
        if (d[at]) zeros = 0;
        else if (++zeros == 7) zeros = 0, runs++;
    }
    char k[64], v[256], me_id[32] = "";
    struct { char id[32], name[64]; } players[16];
    i32 np = 0;
    for (i32 i = 0; i < 2000; i++) {
        if (!r6_header_string(d, len, &at, k, sizeof k) || !r6_header_string(d, len, &at, v, sizeof v)) return 0;
        if (!strcmp(k, "code")) r->code = atoi(v);
        else if (!strcmp(k, "datetime")) r->start_us = r6_datetime(v);
        else if (!strcmp(k, "recordingplayerid")) str_set(me_id, v);
        else if (!strcmp(k, "playerid") && np < COUNT(players)) str_set(players[np].id, v), players[np++].name[0] = 0;
        else if (!strcmp(k, "playername") && np && !players[np - 1].name[0]) str_set(players[np - 1].name, v);
        else if (!strcmp(k, "teamscore1")) break;
    }
    if (strcmp(k, "teamscore1")) return 0;
    for (i32 i = 0; i < np && me_id[0]; i++)
        if (!strcmp(players[i].id, me_id)) str_set(r->me, players[i].name);
    return r->start_us && r->me[0] && r->code >= R6_Y9S1 ? at : 0;
}

static const u8 R6_TIME[] = {0x1F, 0x07, 0xEF, 0xC9};
static const u8 R6_FEED[] = {0x59, 0x34, 0xE5, 0x8B, 0x04};
static const u8 R6_KILL[] = {0x22, 0xD9, 0x13, 0x3C, 0xBA};

// The recording player's kills (green) and deaths (red) in a round's packets, from `from`. The round clock counts
// down a second at a time; each tick is a second since the round started, and a jump up is the next phase.
static i32 r6_feed(const u8 *d, size_t len, size_t from, const R6Round &r, ReplayMark *out, i32 cap)
{
    i32 n = 0;
    i64 elapsed = 0;
    i64 clock = -1;
    struct Seen { char killer[64], target[64]; };
    static thread_local Seen seen[64];
    i32 seen_n = 0;
    for (size_t i = from; i + 5 <= len; i++) {
        if (!memcmp(d + i, R6_TIME, 4)) {
            size_t at = i + 4 + 1; // past the pattern and a size byte
            if (at + 4 > len) break;
            i64 t = (i64)d[at] | (i64)d[at + 1] << 8 | (i64)d[at + 2] << 16 | (i64)d[at + 3] << 24;
            if (clock >= 0 && t != clock) elapsed += t < clock && clock - t <= 5 ? clock - t : 1;
            clock = t;
            i += 4;
            continue;
        }
        if (memcmp(d + i, R6_FEED, 5)) continue;
        size_t at = i + 5;
        if (r.code >= R6_Y9S1_UPDATE3) at += 38;
        else {
            at += 9;
            if (at >= len || d[at] != 4) continue;
            at += 1 + 24;
        }
        if (at + 6 > len || d[at] != 0 || memcmp(d + at + 1, R6_KILL, 5)) continue;
        at += 6;
        char killer[64], target[64];
        if (!r6_string(d, len, &at, killer, sizeof killer)) continue;
        at += 15;
        if (!r6_string(d, len, &at, target, sizeof target) || !target[0]) continue;
        bool dup = false;
        for (i32 s = 0; s < seen_n && !dup; s++) dup = !strcmp(seen[s].killer, killer) && !strcmp(seen[s].target, target);
        if (dup) continue;
        if (seen_n < COUNT(seen)) str_set(seen[seen_n].killer, killer), str_set(seen[seen_n].target, target), seen_n++;
        i64 t = r.start_us + elapsed * 1000000;
        if (!strcmp(target, r.me)) replay_push(out, cap, &n, t, AM_DEATH, 1, "Death");
        else if (!strcmp(killer, r.me)) replay_push(out, cap, &n, t, AM_KILL, 4, "Kill");
        i = at - 1;
    }
    return n;
}

// A round's file, whole: the recording player's kills and deaths in it, oldest first. How many; -1 if it isn't a
// round of a version this reads.
static i32 r6_marks(const u8 *file, size_t len, ReplayMark *out, i32 cap)
{
    u8 *data = 0;
    size_t data_len = 0;
    i32 n = -1;
    R6Round r;
    if (len >= 4 && !memcmp(file, R6_ZSTD_MAGIC, 4)) {
        // Before Y8S4 the whole file, header too, is one zstd stream
        size_t head;
        if (r6_unzstd(file, len, 0, &data, &data_len) && (head = r6_header(data, data_len, &r)))
            n = r6_feed(data, data_len, head, r, out, cap);
    } else if (size_t head = r6_header(file, len, &r)) {
        if (r6_unzstd(file, len, head, &data, &data_len)) n = r6_feed(data, data_len, 0, r, out, cap);
    }
    free(data);
    return n;
}

// ---------------------------------------------------------------------------------------------------------------
// Fortnite
//
// %LOCALAPPDATA%\FortniteGame\Saved\Demos\*.replay, an Unreal Engine replay: a header (when recording started, in
// the PC's local time, and the key its events are encrypted with, written once the match is over), then chunks.
// The event chunks carry every elimination of the match (playerElim: the eliminated player, the eliminator, and
// whether it was a knock) and the recording player's match stats (AthenaMatchStats). No event says who the
// recording player is: FortniteGame.log does, the Epic account that logged in (fn_log_owner). A player's own end
// (falling, the storm) is an elimination with themself as the eliminator: a death, not a kill, except in the
// match's first FN_START_MS.

// A little-endian reader over untrusted bytes: once a read runs past the end, `ok` is false and reads give 0.
struct FnReader {
    const u8 *d;
    size_t len, at;
    bool ok;
    bool take(void *out, size_t n)
    {
        if (!ok || n > len - at) {
            ok = false;
            memset(out, 0, n);
            return false;
        }
        memcpy(out, d + at, n), at += n;
        return true;
    }
    u8 byte() { u8 v; take(&v, 1); return v; }
    u32 u32le() { u32 v; take(&v, 4); return v; }
    i64 i64le() { i64 v; take(&v, 8); return v; }
    void skip(size_t n)
    {
        if (!ok || n > len - at) ok = false;
        else at += n;
    }
    // An FString: i32 length; above 0, that many bytes with the NUL; below 0, that many UTF-16 units. Only ASCII
    // is kept; anything else becomes '?'.
    void fstring(char *out, i32 cap)
    {
        i32 n = (i32)u32le(), k = 0;
        bool wide = n < 0;
        if (wide) n = -n;
        if (n > 65536) ok = false, n = 0;
        for (i32 i = 0; i < n && ok; i++) {
            u16 c = 0;
            take(&c, wide ? 2 : 1);
            if (c && k < cap - 1) out[k++] = c < 0x80 ? (char)c : '?';
        }
        out[k] = 0;
    }
};

static const u32 FN_MAGIC = 0x1CA2E27F;

struct FnHeader {
    i64  start_us; // when recording started, on the wall clock
    bool live;     // still recording: no key yet
    bool encrypted;
    u8   key[32];
};

// Unreal's FDateTime (100 ns since 0001-01-01) in the PC's local time, on the wall clock. 0 if it isn't one.
static i64 fn_local_ticks_us(i64 ticks)
{
    const i64 to_1601 = 504911232000000000ll;
    if (ticks <= to_1601) return 0;
    i64 local = ticks - to_1601;
    FILETIME lf = {(DWORD)local, (DWORD)(local >> 32)};
    SYSTEMTIME ls;
    return FileTimeToSystemTime(&lf, &ls) ? replay_local_us(ls) : 0;
}

// The header. False if the bytes aren't a replay's.
static bool fn_header(FnReader *r, FnHeader *h)
{
    *h = {};
    if (r->u32le() != FN_MAGIC) return false;
    u32 version = r->u32le();
    if (version >= 7) {
        u32 n = r->u32le();
        if (n > 4096) return false;
        r->skip((size_t)n * 20); // custom versions: a GUID and a number each
    }
    r->skip(12); // its length, network version and changelist
    char name[256];
    r->fstring(name, sizeof name);
    h->live = r->u32le() != 0;
    if (version >= 3) h->start_us = fn_local_ticks_us(r->i64le());
    if (version >= 2) r->skip(4); // whether its data chunks are compressed
    if (version >= 6) {
        h->encrypted = r->u32le() != 0;
        u32 n = r->u32le();
        if (n == 32) r->take(h->key, 32);
        else if (n <= 1024) r->skip(n);
        else return false;
        if (h->encrypted && n != 32 && !h->live) return false;
    }
    return r->ok && h->start_us;
}

// An event's data, decrypted in place with AES-256-ECB and its padding taken off. Its length; -1 if it can't be.
static i32 fn_decrypt(const FnHeader &h, u8 *data, u32 len)
{
    if (!h.encrypted) return (i32)len;
    if (!len || len % 16) return -1;
    BCRYPT_ALG_HANDLE alg = 0;
    BCRYPT_KEY_HANDLE key = 0;
    i32 out = -1;
    ULONG got = 0;
    if (BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, 0, 0)) &&
        BCRYPT_SUCCESS(BCryptSetProperty(alg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_ECB, sizeof BCRYPT_CHAIN_MODE_ECB, 0)) &&
        BCRYPT_SUCCESS(BCryptGenerateSymmetricKey(alg, &key, 0, 0, (PUCHAR)h.key, 32, 0)) &&
        BCRYPT_SUCCESS(BCryptDecrypt(key, data, len, 0, 0, 0, data, len, &got, 0)) && got == len) {
        u8 pad = data[len - 1];
        if (pad >= 1 && pad <= 16) out = (i32)(len - pad);
    }
    if (key) BCryptDestroyKey(key);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return out;
}

// A player in an elimination: 0x03 a bot; 0x10 a named bot (an FString); 0x11 a player, u8 length and their Epic
// account id, written here as 32 lowercase hex digits ("" for a bot). False when it isn't one of those.
static bool fn_player(FnReader *r, char (&id)[40])
{
    id[0] = 0;
    u8 kind = r->byte();
    if (kind == 0x03) return r->ok;
    if (kind == 0x10) {
        char name[64];
        r->fstring(name, sizeof name);
        return r->ok;
    }
    u8 raw[16];
    if (kind != 0x11 || r->byte() != 16 || !r->take(raw, 16)) return false;
    for (i32 i = 0; i < 16; i++) snprintf(id + i * 2, 3, "%02x", raw[i]);
    return true;
}

struct FnElim {
    i64  t_ms; // in the replay's time
    char victim[40], killer[40];
    bool knock;
};

// playerElim: i32 version, a byte, both players' places (160 bytes), the eliminated, the eliminator, a byte for
// the weapon, u32 knocked.
static bool fn_elim(const u8 *d, i32 len, FnElim *e)
{
    FnReader r = {d, (size_t)len, 0, true};
    r.skip(4 + 1 + 160);
    if (!fn_player(&r, e->victim) || !fn_player(&r, e->killer)) return false;
    r.skip(1);
    e->knock = r.u32le() != 0;
    return r.ok;
}

static const i64 FN_FINISH_MS = 65000;
static const i64 FN_START_MS = 10000; // a respawn mode takes every player out of the match as it starts

// The recording player's knocks and eliminations (green), and their being knocked or eliminated (red), oldest
// first. `owner` is their account id, from FortniteGame.log. A finish within FN_FINISH_MS of the same player's knock
// is the same moment and isn't marked again. How many; -1 if the file isn't a finished replay.
static i32 fn_marks(const u8 *file, size_t len, const char *owner, ReplayMark *out, i32 cap)
{
    FnReader r = {file, len, 0, true};
    FnHeader h;
    if (!fn_header(&r, &h) || h.live) return -1;
    static thread_local FnElim elims[2048];
    static thread_local u8 data[4096];
    i32 ne = 0;
    while (r.ok && len - r.at >= 8) {
        u32 type = r.u32le(), size = r.u32le();
        if (size > len - r.at) break;
        size_t end = r.at + size;
        if (type == 3) { // an event
            char id[128], group[64], meta[64];
            r.fstring(id, sizeof id), r.fstring(group, sizeof group), r.fstring(meta, sizeof meta);
            u32 start_ms = r.u32le();
            r.skip(4);
            u32 n = r.u32le();
            if (r.ok && n <= sizeof data && r.at + n <= end && r.take(data, n)) {
                i32 k = fn_decrypt(h, data, n);
                if (k > 0 && !strcmp(group, "playerElim") && ne < COUNT(elims) && fn_elim(data, k, &elims[ne]))
                    elims[ne++].t_ms = start_ms;
            }
        }
        r.ok = true;
        r.at = end;
    }
    char me[40] = "";
    for (i32 i = 0; owner && owner[i] && i < 39; i++) me[i] = (char)tolower((u8)owner[i]), me[i + 1] = 0;
    i32 n = 0;
    if (!me[0]) return 0;
    for (i32 i = 0; i < ne; i++) {
        const FnElim &e = elims[i];
        bool hit = !strcmp(e.victim, me), mine = !hit && !strcmp(e.killer, me);
        if ((!mine && !hit) || (hit && !strcmp(e.killer, me) && e.t_ms < FN_START_MS)) continue;
        bool finish = false;
        for (i32 j = i - 1; j >= 0 && !finish && !e.knock; j--)
            finish = elims[j].knock && !strcmp(elims[j].victim, e.victim) && e.t_ms - elims[j].t_ms <= FN_FINISH_MS;
        if (finish) continue;
        i64 t = h.start_us + e.t_ms * 1000;
        if (hit) replay_push(out, cap, &n, t, e.knock ? AM_DOWNED : AM_DEATH, 1, e.knock ? "Knocked" : "Death");
        else replay_push(out, cap, &n, t, AM_KILL, 4, e.knock ? "Knock" : "Kill");
    }
    return n;
}

// The account id in FortniteGame.log, the last "Successfully logged in user. UserId=[<32 hex digits>]".
static bool fn_log_owner(const char *log, char (&out)[40])
{
    out[0] = 0;
    static const char KEY[] = "Successfully logged in user. UserId=[";
    for (const char *p = strstr(log, KEY); p; p = strstr(p + 1, KEY)) {
        const char *id = p + sizeof KEY - 1;
        i32 k = 0;
        while (k < 32 && isxdigit((u8)id[k])) k++;
        if (k != 32 || id[32] != ']') continue;
        for (i32 i = 0; i < 32; i++) out[i] = (char)tolower((u8)id[i]);
        out[32] = 0;
    }
    return out[0] != 0;
}

// ---------------------------------------------------------------------------------------------------------------
// The watcher. On the auto markers worker (replays_poll, from am_logs_poll), every REPLAYS_EVERY while Fortnite's
// or Siege's markers are on: each replay finished at least REPLAYS_SETTLE ago and newer than REPLAYS_KEEP is read
// once, and its marks go to the main thread as EV_MARKER events (a = when, b = colour, text = note). The replays
// read are kept in <local_dir>\replays.tsv (a key of the path, size and time it was written, and that time), so a
// restart doesn't mark them again.

static const i64 REPLAYS_EVERY = 30000000;
static const i64 REPLAYS_SETTLE = 60000000;
static const i64 REPLAYS_KEEP = 30ll * 86400 * 1000000;
enum { REPLAYS_SEEN_MAX = 4096 };

struct ReplaySeen { u64 key; i64 written_us; };

static struct {
    wchar_t fn_dir[MAX_PATH]; // Fortnite's Demos folder; "" while off or not installed (main thread sets it)
    wchar_t r6_dir[MAX_PATH]; // Siege's MatchReplay folder; the same
    bool loaded;
    ReplaySeen seen[REPLAYS_SEEN_MAX];
    i32 seen_count;
    i64 next;
} g_rp;

static u64 replay_key(const wchar_t *path, i64 size, i64 written_us)
{
    u64 h = 1469598103934665603ull;
    for (const wchar_t *c = path; *c; c++) h = (h ^ (u64)towlower(*c)) * 1099511628211ull;
    h = (h ^ (u64)size) * 1099511628211ull;
    return (h ^ (u64)written_us) * 1099511628211ull;
}

static Path replays_seen_path() { return path_join(g_app.paths.local_dir, L"replays.tsv"); }

static void replays_seen_load()
{
    g_rp.loaded = true;
    g_rp.seen_count = 0;
    char *text = read_file(replays_seen_path().s, 0);
    if (!text) return;
    for (char *line = text; *line && g_rp.seen_count < REPLAYS_SEEN_MAX;) {
        char *end = strchr(line, '\n');
        if (end) *end = 0;
        ReplaySeen s = {};
        if (sscanf(line, "%llx\t%lld", &s.key, &s.written_us) == 2) g_rp.seen[g_rp.seen_count++] = s;
        if (!end) break;
        line = end + 1;
    }
    free(text);
}

// Adds a replay read, dropping those older than REPLAYS_KEEP, and writes the list.
static void replays_seen_add(u64 key, i64 written_us)
{
    i64 old = now_us() - REPLAYS_KEEP;
    i32 n = 0;
    for (i32 i = 0; i < g_rp.seen_count; i++)
        if (g_rp.seen[i].written_us >= old) g_rp.seen[n++] = g_rp.seen[i];
    if (n == REPLAYS_SEEN_MAX) memmove(g_rp.seen, g_rp.seen + 1, sizeof g_rp.seen[0] * --n);
    g_rp.seen[n++] = {key, written_us};
    g_rp.seen_count = n;
    FILE *f = _wfopen(replays_seen_path().s, L"wb");
    if (!f) return;
    for (i32 i = 0; i < n; i++) fprintf(f, "%016llx\t%lld\n", g_rp.seen[i].key, g_rp.seen[i].written_us);
    fclose(f);
}

static bool replays_seen(u64 key)
{
    for (i32 i = 0; i < g_rp.seen_count; i++)
        if (g_rp.seen[i].key == key) return true;
    return false;
}

// A file mapped for reading; the game may still have it open. Replays run to hundreds of MB.
struct ReplayFile { HANDLE file, map; const u8 *p; size_t len; };

static bool replay_open(const wchar_t *path, ReplayFile *rf)
{
    *rf = {INVALID_HANDLE_VALUE, 0, 0, 0};
    rf->file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, 0);
    LARGE_INTEGER size = {};
    if (rf->file == INVALID_HANDLE_VALUE || !GetFileSizeEx(rf->file, &size) || !size.QuadPart) return false;
    rf->map = CreateFileMappingW(rf->file, 0, PAGE_READONLY, 0, 0, 0);
    if (rf->map) rf->p = (const u8 *)MapViewOfFile(rf->map, FILE_MAP_READ, 0, 0, 0);
    rf->len = (size_t)size.QuadPart;
    return rf->p != 0;
}

static void replay_close(ReplayFile *rf)
{
    if (rf->p) UnmapViewOfFile(rf->p);
    if (rf->map) CloseHandle(rf->map);
    if (rf->file != INVALID_HANDLE_VALUE) CloseHandle(rf->file);
    *rf = {INVALID_HANDLE_VALUE, 0, 0, 0};
}

// Sends the marks the game's events ask for to the main thread.
static void replays_post(i32 game, const ReplayMark *m, i32 n, u32 events)
{
    for (i32 i = 0; i < n; i++) {
        if (!(events & (m[i].colour == 1 ? AM_DEATH : AM_KILL))) continue;
        Event e = {};
        e.kind = EV_MARKER;
        e.a = m[i].t_us;
        e.b = m[i].colour;
        char d[256];
        marker_detail_event(d, m[i].event, 0);
        snprintf(e.text, sizeof e.text, "%s\t%s", m[i].note, d);
        event_post(e);
        log_line("markers: %s: %s, from its replay", AM_GAMES[game].name, m[i].note);
    }
}

// Reads one replay unless it was read before or isn't finished. Fortnite's needs whose it is (owner).
static void replays_file(i32 game, const wchar_t *path, const WIN32_FIND_DATAW &fd, const char *owner, u32 events)
{
    i64 written = filetime_to_us(fd.ftLastWriteTime), now = now_us();
    i64 size = ((i64)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
    if (written < now - REPLAYS_KEEP || written > now - REPLAYS_SETTLE) return;
    u64 key = replay_key(path, size, written);
    if (replays_seen(key)) return;
    ReplayFile rf;
    bool open = replay_open(path, &rf);
    static ReplayMark marks[256];
    i32 n = !open ? -2
            : game == AMG_FORTNITE ? fn_marks(rf.p, rf.len, owner, marks, COUNT(marks))
                                   : r6_marks(rf.p, rf.len, marks, COUNT(marks));
    replay_close(&rf);
    if (n == -2 || (n < 0 && game == AMG_FORTNITE)) return; // can't be read yet, or still recording
    replays_seen_add(key, written);
    if (n > 0) replays_post(game, marks, n, events);
}

static void replays_poll()
{
    u32 fn_ev = g_am.events[AMG_FORTNITE], r6_ev = g_am.events[AMG_SIEGE];
    bool fn = fn_ev && g_rp.fn_dir[0], r6 = r6_ev && g_rp.r6_dir[0];
    if ((!fn && !r6) || now_us() < g_rp.next) return;
    g_rp.next = now_us() + REPLAYS_EVERY;
    if (!g_rp.loaded) replays_seen_load();
    WIN32_FIND_DATAW fd = {};
    wchar_t pat[MAX_PATH], path[MAX_PATH];
    if (fn) {
        char owner[40] = "";
        wchar_t log[MAX_PATH];
        swprintf(log, MAX_PATH, L"%s\\..\\Logs\\FortniteGame.log", g_rp.fn_dir);
        if (char *text = read_file(log, 0)) {
            fn_log_owner(text, owner);
            free(text);
        }
        swprintf(pat, MAX_PATH, L"%s\\*.replay", g_rp.fn_dir);
        HANDLE h = owner[0] ? FindFirstFileW(pat, &fd) : INVALID_HANDLE_VALUE;
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                swprintf(path, MAX_PATH, L"%s\\%s", g_rp.fn_dir, fd.cFileName);
                replays_file(AMG_FORTNITE, path, fd, owner, fn_ev);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    if (r6) {
        swprintf(pat, MAX_PATH, L"%s\\*", g_rp.r6_dir);
        HANDLE h = FindFirstFileW(pat, &fd);
        if (h == INVALID_HANDLE_VALUE) return;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.') continue;
            if (filetime_to_us(fd.ftLastWriteTime) < now_us() - REPLAYS_KEEP) continue;
            wchar_t match[MAX_PATH];
            swprintf(match, MAX_PATH, L"%s\\%s", g_rp.r6_dir, fd.cFileName);
            swprintf(pat, MAX_PATH, L"%s\\*.rec", match);
            WIN32_FIND_DATAW rd;
            HANDLE r = FindFirstFileW(pat, &rd);
            if (r == INVALID_HANDLE_VALUE) continue;
            do {
                swprintf(path, MAX_PATH, L"%s\\%s", match, rd.cFileName);
                replays_file(AMG_SIEGE, path, rd, 0, r6_ev);
            } while (FindNextFileW(r, &rd));
            FindClose(r);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
}

// Siege's MatchReplay folder: in each Steam library's common folder (the game's app manifest may be missing) or
// Ubisoft Connect's; of several, the one written last. "" if there is none.
static void r6_replay_dir(wchar_t (&out)[MAX_PATH])
{
    out[0] = 0;
    wchar_t games[17][MAX_PATH];
    i32 n = 0;
    Path root;
    if (steam_root(&root)) {
        char *vdf = read_file(path_join(root, L"steamapps\\libraryfolders.vdf").s, 0);
        char libs[16][MAX_PATH * 2];
        i32 k = vdf ? steam_libraries(vdf, libs, COUNT(libs)) : 0;
        free(vdf);
        for (i32 i = 0; i < k; i++) {
            wchar_t lib[MAX_PATH];
            utf8_to_wide(libs[i], lib, MAX_PATH);
            swprintf(games[n++], MAX_PATH, L"%s\\steamapps\\common\\Tom Clancy's Rainbow Six Siege", lib);
        }
    }
    DWORD bytes = sizeof games[n];
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Ubisoft\\Launcher\\Installs\\635", L"InstallDir",
                     RRF_RT_REG_SZ, 0, games[n], &bytes) == ERROR_SUCCESS && games[n][0]) {
        for (wchar_t *c = games[n]; *c; c++)
            if (*c == '/') *c = '\\';
        size_t k = wcslen(games[n]);
        if (games[n][k - 1] == '\\') games[n][k - 1] = 0;
        n++;
    }
    i64 newest = -1;
    for (i32 i = 0; i < n; i++) {
        wchar_t dir[MAX_PATH];
        swprintf(dir, MAX_PATH, L"%s\\MatchReplay", games[i]);
        WIN32_FILE_ATTRIBUTE_DATA a;
        bool is_dir = GetFileAttributesExW(dir, GetFileExInfoStandard, &a) && (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
        if (!is_dir) continue;
        i64 t = filetime_to_us(a.ftLastWriteTime);
        if (t > newest) newest = t, wcscpy_s(out, dir);
    }
}

// Where the replays are, and each game's state for Options. From automarks_apply. Main thread.
static void replays_apply()
{
    wchar_t fn[MAX_PATH] = L"", r6[MAX_PATH] = L"";
    if (g_am.events[AMG_FORTNITE]) {
        wchar_t local[MAX_PATH], game[MAX_PATH];
        DWORD k = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
        if (k && k < MAX_PATH) {
            swprintf(game, MAX_PATH, L"%s\\FortniteGame", local);
            if (GetFileAttributesW(game) != INVALID_FILE_ATTRIBUTES) swprintf(fn, MAX_PATH, L"%s\\Saved\\Demos", game);
        }
    }
    if (g_am.events[AMG_SIEGE]) r6_replay_dir(r6);
    if (wcscmp(fn, g_rp.fn_dir)) wcscpy_s(g_rp.fn_dir, fn); // the worker reads them whole or not at all
    if (wcscmp(r6, g_rp.r6_dir)) wcscpy_s(g_rp.r6_dir, r6);
    InterlockedExchange(&g_am.state[AMG_FORTNITE], !g_am.events[AMG_FORTNITE] ? AM_OFF : fn[0] ? AM_READY : AM_NOT_FOUND);
    InterlockedExchange(&g_am.state[AMG_SIEGE], !g_am.events[AMG_SIEGE] ? AM_OFF : r6[0] ? AM_READY : AM_NOT_FOUND);
}
