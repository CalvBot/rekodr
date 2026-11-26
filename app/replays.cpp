// replays.cpp: markers from the match replays games save, read once the match is over. Only the player's own kills
// and deaths count. Each goes on the timeline at its moment, and into the layers file of any clip or session saved
// over it (markers_add_late), so it comes late but in its place.
//
//   Rainbow Six Siege  <game>\MatchReplay\<match>\*.rec, a file a round: a "dissect" header (the round's start in
//                      UTC, the recording player's id, the players' names and ids) and zstd frames holding the
//                      round's packets. The round clock and the kill feed are found by their byte patterns, as
//                      r6-dissect (github.com/redraskal/r6-dissect, MIT) finds them.
//
// The files are untrusted data: every read is checked against the end.

struct ReplayMark {
    i64  t_us; // when, on the wall clock
    u8   colour;
    char note[32];
};

static void replay_push(ReplayMark *out, i32 cap, i32 *n, i64 t_us, u8 colour, const char *note)
{
    if (*n >= cap) return;
    ReplayMark &m = out[(*n)++];
    m.t_us = t_us;
    m.colour = colour;
    str_set(m.note, note);
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

// "2024-05-04-02-14-08", UTC, in microseconds since 1970. 0 if it isn't one.
static i64 r6_datetime(const char *s)
{
    SYSTEMTIME st = {};
    int y, mo, d, h, mi, se;
    if (sscanf_s(s, "%d-%d-%d-%d-%d-%d", &y, &mo, &d, &h, &mi, &se) != 6) return 0;
    st.wYear = (WORD)y, st.wMonth = (WORD)mo, st.wDay = (WORD)d, st.wHour = (WORD)h, st.wMinute = (WORD)mi, st.wSecond = (WORD)se;
    FILETIME ft;
    if (y < 2015 || !SystemTimeToFileTime(&st, &ft)) return 0;
    i64 t = ((i64)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return (t - 116444736000000000LL) / 10;
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
    char k[64], v[256], me_id[32] = "", player_id[32] = "";
    for (i32 i = 0; i < 2000; i++) {
        if (!r6_header_string(d, len, &at, k, sizeof k) || !r6_header_string(d, len, &at, v, sizeof v)) return 0;
        if (!strcmp(k, "code")) r->code = atoi(v);
        else if (!strcmp(k, "datetime")) r->start_us = r6_datetime(v);
        else if (!strcmp(k, "recordingplayerid")) str_set(me_id, v);
        else if (!strcmp(k, "playerid")) str_set(player_id, v);
        else if (!strcmp(k, "playername") && player_id[0] && me_id[0] && !strcmp(player_id, me_id) && !r->me[0]) str_set(r->me, v);
        else if (!strcmp(k, "teamscore1")) return r->start_us && r->me[0] && r->code >= R6_Y9S1 ? at : 0;
    }
    return 0;
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
        if (!strcmp(target, r.me)) replay_push(out, cap, &n, t, 1, "Death");
        else if (!strcmp(killer, r.me)) replay_push(out, cap, &n, t, 4, "Kill");
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
