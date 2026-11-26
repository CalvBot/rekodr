// Saving: turning a wall-clock range of the buffer into an MP4 (clips and sessions), and the editor's Keep and
// Share, which render an edit of a saved file (edit_run).
//
// The main thread queues jobs (save_range, save_last) and owns the jobs table in g_app.saving. One worker thread
// runs them in order, one ffmpeg at a time so a long save never fights a short one for the disk. The worker gets a
// copy of the settings it needs with each job and reports back with EV_SAVE events; it never touches g_app.
//
// A save stream-copies the video segments covering the range (gaps such as the lock screen are cut out) and lays
// every audio stream overlapping it onto the SPEC's tracks, cut to match:
//   1. Mix (everything)   2. Mic   3+. one per other input, then per chat app or person, per game, per other app
// Tracks 1 and 2 are always there, silent if nothing played. A file has at most MEDIA_MAX_TRACKS tracks (what the
// editor reads); apps past that share a last "Other" track. The Mic and chat tracks are mono when Options says so.

// EV_SAVE: `a` is one of these, `b` is the job id in the low 32 bits and a value in the high 32.
enum SaveEventKind : i64 {
    SAVE_EV_STARTED,  // the worker took the job
    SAVE_EV_PATH,     // text: the file it's writing
    SAVE_EV_GAME,     // text: the game played most in it ("" for none)
    SAVE_EV_PROGRESS, // value: progress in 1/1000
    SAVE_EV_DONE,     // text: the file saved
    SAVE_EV_FAILED,   // text: why
    SAVE_EV_CANCELLED,
};

static const i64 SEGMENT_US = 2000000; // video segment length
static const i32 CAM_SAVE_GROUPS = 32;  // stretches of webcam footage one save takes; any after them stay black

// What the worker needs from Config and Paths, copied on the main thread with every job.
struct SaveSettings {
    Path    buffer_dir;
    Path    save_dir;
    Path    ffmpeg;   // empty = not found
    Path    work_dir; // temp lists and graphs (under local_dir, so test copies keep to REKODR_HOME)
    Path    play_dir; // previews (Config::play_dir)
    char    name_template[128];
    char    codec[64];
    u8      vendor; // the H.264 encoder Keep and Share use when they re-encode (Vendor)
    bool    voice_mono;
    bool    mic_clean, mic_raw;
    InputDevice inputs[16];
    i32     input_count;
    AppInfo apps[512];
    i32     app_count;
    FolderRule folders[32];
    i32     folder_count;
};

struct SaveRequest {
    u32      id;
    SaveKind kind;
    i64      from_us, to_us;
    char     name[256];
    bool     recording; // the recorder says it's capturing, so the newest segment is still being written
    u64      queued_ms; // GetTickCount64 when it was queued
    EditJob  edit;      // SAVE_KEEP, SAVE_SHARE and SAVE_LINK
};

// The only data shared between the main thread and the worker, always under `lock`.
struct SaveQueue {
    SRWLOCK            lock;
    CONDITION_VARIABLE wake;
    SaveRequest        items[32];
    i32                count;
    SaveSettings       settings;  // from the newest save_range: a job runs with the settings of when it started
    u32                cancel_id; // the running job should stop
    bool               running;   // the worker has a job
    bool               stop;
    HANDLE             thread;
};

static SaveQueue g_save_q;

// ---------------------------------------------------------------------------------------------------------------
// Worker side. There's one worker, so its data is plain statics.

struct Track {
    char title[128];
    char exe[64]; // app tracks: the exe; input tracks: the device's endpoint id
    bool input;   // an input other than Windows' default
    bool chat;    // a Chat app (each Discord person is an exe of their own)
    bool mono;    // kept in mono (Options: voices in mono, or the input's own setting)
    bool game;
    bool raw;     // the Mic as recorded, beside the cleaned one (Config.mic_raw): no streams of its own, in no Mix
    i32  rank;    // position in time played (most first); INT32_MAX for apps
};

struct SavePart { i64 from, to, delay; }; // one stretch of a stream used in one piece: us into the stream, delay

struct SaveWork {
    BufferIdx    buf;
    SaveSettings cfg;
    SaveRequest  req;
    Piece        pieces[512];
    i32          piece_count;
    i32          streams[128];      // indexes into buf.streams of those overlapping the clip
    i32          stream_track[128]; // the track each goes on, -1 = none (ignored apps)
    i32          stream_parts[128]; // stretches of it used (0 = it only overlaps a gap)
    i32          stream_segs[128][2]; // its segments that reach into the clip, [first, end) within the stream
    i32          stream_count;
    Track        tracks[130];
    i32          track_count;
    GameTime     games[64];
    i32          game_count;
    SavePart     parts[512];
    char         graph[1 << 20];
    i32          graph_len;
    bool         graph_full;
    wchar_t      args[32768];
    // How the last ffmpeg run went, for the save's timing line: ms from its start to its first progress, ms it ran,
    // and the last speed it reported ("1.23x")
    u64          ff_first_ms, ff_ms;
    char         ff_speed[24];
    // Where run_ffmpeg's progress lands in the save's 1000: from ff_from over ff_span (0 = all of it)
    i32          ff_from, ff_span;
};

static SaveWork g_save_work;

static void save_post(i64 kind, u32 id, i64 value, const char *text)
{
    Event e = {};
    e.kind = EV_SAVE;
    e.a = kind;
    e.b = (i64)id | (value << 32);
    if (text) str_set(e.text, text);
    event_post(e);
}

static bool save_cancelled(u32 id)
{
    AcquireSRWLockShared(&g_save_q.lock);
    bool c = g_save_q.cancel_id == id || g_save_q.stop;
    ReleaseSRWLockShared(&g_save_q.lock);
    return c;
}

static const AppInfo *save_app(const SaveSettings &c, const char *exe)
{
    for (i32 i = 0; i < c.app_count; i++)
        if (!strcmp(c.apps[i].exe.s, exe)) return &c.apps[i];
    return 0;
}

// By the path it was recorded from, so a folder rule covers it.
static AppCategory save_category(const SaveSettings &c, const char *exe, const char *path)
{
    return app_category_in(c.apps, c.app_count, c.folders, c.folder_count, exe, path);
}

// Drops what a file name can't hold.
static void clean_name(char *s)
{
    char *w = s;
    for (char *r = s; *r; r++)
        if (!strchr("<>:\"/\\|?*", *r) && (u8)*r >= 32) *w++ = *r;
    *w = 0;
    char *t = trim(s);
    if (t != s) memmove(s, t, strlen(t) + 1);
    for (size_t n = strlen(s); n && s[n - 1] == '.'; n--) s[n - 1] = 0; // Windows drops trailing dots
}

// A readable name: the one set in Options (`a`, its entry, may be 0), else "Overwatch" from overwatch.exe.
static void display_name_of(const AppInfo *a, const char *exe, char *out, i32 cap)
{
    if (a && (a->name.s[0] || a->desc.s[0])) {
        snprintf(out, cap, "%s", a->name.s[0] ? a->name.s : a->desc.s);
    } else {
        snprintf(out, cap, "%s", exe);
        size_t n = strlen(out);
        if (n > 4 && !_stricmp(out + n - 4, ".exe")) out[n - 4] = 0;
        out[0] = (char)toupper((u8)out[0]);
    }
    clean_name(out);
}

static void display_name(const SaveSettings &c, const char *exe, char *out, i32 cap)
{
    display_name_of(save_app(c, exe), exe, out, cap);
}

static SYSTEMTIME local_time(i64 us)
{
    i64 t = us * 10 + 116444736000000000LL;
    FILETIME ft = {(DWORD)t, (DWORD)(t >> 32)};
    SYSTEMTIME utc, local;
    FileTimeToSystemTime(&ft, &utc);
    if (!SystemTimeToTzSpecificLocalTime(0, &utc, &local)) local = utc;
    return local;
}

// A clip's or session's file name (no folder, " (2)" or ".mp4") from a template: {game} is `game`, and {YYYY} {YY}
// {MM} {DD} {hh} {mm} {ss} the local time `start_us` (24-hour). What a file name can't hold is dropped; a template
// that leaves nothing gives the game.
static void save_name_expand(const char *tmpl, const char *game, i64 start_us, char *out, i32 cap)
{
    static const char *const tokens[] = {"{game}", "{YYYY}", "{YY}", "{MM}", "{DD}", "{hh}", "{mm}", "{ss}"};
    SYSTEMTIME t = local_time(start_us);
    const i32 nums[] = {0, t.wYear, t.wYear % 100, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond};
    i32 n = 0;
    for (const char *p = tmpl; *p && n < cap - 1;) {
        i32 k = 0;
        while (k < COUNT(tokens) && strncmp(p, tokens[k], strlen(tokens[k]))) k++;
        if (k == COUNT(tokens)) {
            out[n++] = *p++;
            continue;
        }
        char v[128];
        if (k == 0) snprintf(v, sizeof v, "%s", game);
        else snprintf(v, sizeof v, k == 1 ? "%04d" : "%02d", nums[k]);
        n += snprintf(out + n, cap - n, "%s", v);
        if (n > cap - 1) n = cap - 1;
        p += strlen(tokens[k]);
    }
    out[n] = 0;
    clean_name(out);
    if (!out[0]) snprintf(out, cap, "%s", game);
}

// The file name a clip or session of footage from `start_us` gets with the template in Options: `game_exe` is the
// game played most in it ("" = the desktop), `name` one typed for it, which stands in for the game ("" = none).
static void save_file_name(const char *game_exe, const char *name, i64 start_us, char *out, i32 cap)
{
    char game[256] = "";
    if (name && name[0]) {
        str_set(game, name);
        clean_name(game);
    }
    if (!game[0] && game_exe && game_exe[0]) {
        audio_display_name(g_app.config, game_exe, game, sizeof game);
        clean_name(game);
    }
    if (!game[0]) str_set(game, GAME_DESKTOP);
    save_name_expand(g_app.config.name_template, game, start_us, out, cap);
}

// A time of day the way Options > General sets it: "9:04 pm" or "21:04" ("9:04:05 pm", "21:04:05" with seconds).
static void fmt_hm(const SYSTEMTIME &t, char *out, i32 cap, bool secs = false)
{
    char sec[8] = "";
    if (secs) snprintf(sec, sizeof sec, ":%02d", t.wSecond);
    if (g_app.config.clock_24h) snprintf(out, cap, "%02d:%02d%s", t.wHour, t.wMinute, sec);
    else snprintf(out, cap, "%d:%02d%s %s", t.wHour % 12 ? t.wHour % 12 : 12, t.wMinute, sec, t.wHour < 12 ? "am" : "pm");
}

static bool file_exists(const wchar_t *p) { return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES; }

static void graph_add(SaveWork *w, const char *fmt, ...)
{
    if (w->graph_full) return;
    va_list args;
    va_start(args, fmt);
    i32 room = (i32)sizeof w->graph - w->graph_len;
    i32 n = vsnprintf(w->graph + w->graph_len, room, fmt, args);
    va_end(args);
    if (n < 0 || n >= room) { w->graph_full = true; return; }
    w->graph_len += n;
}

static void us_secs(i64 us, char (&out)[32]) { snprintf(out, sizeof out, "%.6f", (f64)us / 1e6); }

// Segments that join up (within 50 ms) are one piece; a recording gap starts the next.
static void make_pieces(SaveWork *w, i32 first, i32 end)
{
    w->piece_count = 0;
    i64 offset = 0;
    for (i32 i = first; i < end; i++) {
        const VideoSeg &s = w->buf.segs[i];
        Piece *last = w->piece_count ? &w->pieces[w->piece_count - 1] : 0;
        i64 gap = last ? s.start_us - last->end_us : 0;
        if (last && gap <= 50000 && gap >= -50000) {
            offset += s.end_us - last->end_us;
            last->end_us = s.end_us;
        } else if (w->piece_count < COUNT(w->pieces)) {
            w->pieces[w->piece_count++] = {s.start_us, s.end_us, offset};
            offset += s.end_us - s.start_us;
        }
    }
}

// A chapter of a saved file: a marker, or (in a session) where a game starts
struct Chapter { i64 t_us; char title[256]; }; // t_us is wall clock

// Where a wall-clock moment lands in the saved file; -1 when it falls in a recording gap
static i64 file_time(const Piece *pieces, i32 piece_count, i64 t_us)
{
    for (i32 k = 0; k < piece_count; k++)
        if (t_us >= pieces[k].start_us && t_us < pieces[k].end_us) return pieces[k].offset_us + t_us - pieces[k].start_us;
    return -1;
}

// The clip's layers, [from, to) of the buffer's, written beside its edit as Data\<video's name>.layers.txt with
// its pieces. None is written when no layer has anything in it.
static void save_layers(const SaveWork *w, i64 from, i64 to, const Path &video)
{
    static LayerSet set;
    layers_slice(from, to, &set);
    if (set.count) {
        wchar_t stem[MAX_PATH];
        const wchar_t *name = wcsrchr(video.s, L'\\');
        wcscpy_s(stem, name ? name + 1 : video.s);
        wchar_t *dot = wcsrchr(stem, L'.');
        if (dot) *dot = 0;
        if (!layers_write(layers_clip_path(w->cfg.save_dir, stem), set, w->pieces, w->piece_count)) log_line("save %u: couldn't write its layers", w->req.id);
    }
    layer_set_free(&set);
}

// The chapters as an ffmetadata file, each running to the next one (the last to the end). Chapters in a gap and
// ones on the same millisecond as the one before are left out. MP4 players start the first chapter at 0, so when
// the first is later, a chapter titled `lead` comes before it. Returns how many were written; 0 = none.
static i32 chapters_text(const Piece *pieces, i32 piece_count, i64 total_us, Chapter *ch, i32 n, const char *lead,
                         char *out, i32 cap)
{
    for (i32 i = 1; i < n; i++) { // oldest first
        Chapter c = ch[i];
        i32 j = i;
        while (j > 0 && ch[j - 1].t_us > c.t_us) { ch[j] = ch[j - 1]; j--; }
        ch[j] = c;
    }
    i64 at[257];
    i32 kept[257], m = 0;
    for (i32 i = 0; i < n && m < COUNT(at) - 1; i++) { // room for the lead
        i64 t = file_time(pieces, piece_count, ch[i].t_us);
        if (t < 0 || t / 1000 >= total_us / 1000 || (m && t / 1000 <= at[m - 1] / 1000)) continue;
        at[m] = t;
        kept[m++] = i;
    }
    if (m && at[0] / 1000 > 0) {
        memmove(at + 1, at, m * sizeof at[0]);
        memmove(kept + 1, kept, m * sizeof kept[0]);
        at[0] = 0;
        kept[0] = -1;
        m++;
    }
    i32 len = snprintf(out, cap, ";FFMETADATA1\n");
    for (i32 i = 0; i < m && len < cap; i++) {
        len += snprintf(out + len, cap - len, "[CHAPTER]\nTIMEBASE=1/1000\nSTART=%lld\nEND=%lld\ntitle=", at[i] / 1000,
                        (i + 1 < m ? at[i + 1] : total_us) / 1000);
        for (const char *p = kept[i] < 0 ? lead : ch[kept[i]].title; *p && len + 3 < cap; p++) { // ffmetadata escapes these with a backslash
            if (*p == '\r' || *p == '\n') { out[len++] = ' '; continue; }
            if (strchr("=;#\\", *p)) out[len++] = '\\';
            out[len++] = *p;
        }
        if (len + 1 < cap) out[len++] = '\n';
        out[len < cap ? len : cap - 1] = 0;
    }
    return len < cap ? m : 0;
}

// The wall-clock span of a stream's remaining segments; the end is INT64_MAX while it's still recording.
static void stream_span(const BufferIdx &b, i32 s, i64 *start, i64 *end)
{
    const AudioStreamInfo &st = b.streams[s];
    const AudioSeg &first = b.audio_segs[st.first_seg];
    const AudioSeg &last = b.audio_segs[st.first_seg + st.seg_count - 1];
    *start = first.start_us;
    *end = last.open ? INT64_MAX : last.end_us;
}

static bool track_before(const Track &a, const Track &b)
{
    if (a.input != b.input) return a.input;
    if (a.chat != b.chat) return a.chat;
    if (a.game != b.game) return a.game;
    return a.rank < b.rank;
}

// Mic (Windows' default input) and, when it's cleaned and heard, Mic (raw); then one track per other input, then per
// app: chat apps and people (in order of first sound), games (the one played most first), then the rest in order of
// first sound. Every stream of one device or exe goes on its track.
static void make_tracks(SaveWork *w)
{
    w->track_count = 1;
    w->tracks[0] = {};
    str_set(w->tracks[0].title, "Mic");
    w->tracks[0].mono = w->cfg.voice_mono;
    bool heard = false;
    for (i32 i = 0; i < w->stream_count; i++) {
        const AudioStreamInfo &st = w->buf.streams[w->streams[i]];
        heard |= st.mic && !st.path[0];
    }
    if (heard && w->cfg.mic_clean && w->cfg.mic_raw) {
        Track *raw = &w->tracks[w->track_count++];
        *raw = w->tracks[0];
        str_set(raw->title, MIC_RAW_TITLE);
        raw->raw = true;
    }
    i32 first = w->track_count; // the first track an input or app may take
    for (i32 i = 0; i < w->stream_count; i++) {
        const AudioStreamInfo &st = w->buf.streams[w->streams[i]];
        w->stream_track[i] = -1;
        if (st.mic && !st.path[0]) { w->stream_track[i] = 0; continue; }
        AppCategory cat = st.mic ? CAT_NONE : save_category(w->cfg, st.exe, st.path);
        if (cat == CAT_IGNORED) continue;
        const char *key = st.mic ? st.path : st.exe;
        i32 t = first;
        while (t < w->track_count && (w->tracks[t].input != st.mic || strcmp(w->tracks[t].exe, key))) t++;
        if (t == w->track_count) {
            if (t == COUNT(w->tracks)) continue;
            Track *tr = &w->tracks[w->track_count++];
            *tr = {};
            str_set(tr->exe, key);
            tr->input = st.mic;
            tr->chat = cat == CAT_COMMUNICATION;
            tr->game = cat == CAT_GAME;
            char name[96];
            if (tr->input) {
                const InputDevice *d = config_input(w->cfg.inputs, w->cfg.input_count, st.path);
                input_short_name(d ? d->name : st.exe, name, sizeof name);
                tr->mono = !d || d->mode != INPUT_STEREO;
            } else {
                display_name(w->cfg, st.exe, name, sizeof name);
                tr->mono = tr->chat && w->cfg.voice_mono;
            }
            snprintf(tr->title, sizeof tr->title, tr->game ? "%s (game)" : tr->chat ? "%s (chat)" : "%s", name);
            tr->rank = INT32_MAX;
            for (i32 g = 0; g < w->game_count; g++)
                if (!strcmp(w->games[g].exe, st.exe)) { tr->rank = g; break; }
        }
        w->stream_track[i] = t;
    }
    // Insertion sort (stable) of the app tracks, remembering where each one went.
    i32 order[COUNT(w->tracks)]; // order[new] = old
    for (i32 t = 0; t < w->track_count; t++) order[t] = t;
    for (i32 i = first + 1; i < w->track_count; i++) {
        Track tr = w->tracks[i];
        i32 old = order[i];
        i32 j = i;
        for (; j > first && track_before(tr, w->tracks[j - 1]); j--) {
            w->tracks[j] = w->tracks[j - 1];
            order[j] = order[j - 1];
        }
        w->tracks[j] = tr;
        order[j] = old;
    }
    i32 moved_to[COUNT(w->tracks)];
    for (i32 t = 0; t < w->track_count; t++) moved_to[order[t]] = t;
    for (i32 i = 0; i < w->stream_count; i++)
        if (w->stream_track[i] >= 0) w->stream_track[i] = moved_to[w->stream_track[i]];
    // Past what the editor reads (the Mix is one of them), the last tracks share one: other apps go first.
    const i32 max = MEDIA_MAX_TRACKS - 1;
    if (w->track_count > max) {
        for (i32 i = 0; i < w->stream_count; i++)
            if (w->stream_track[i] > max - 1) w->stream_track[i] = max - 1;
        Track *last = &w->tracks[max - 1];
        *last = {};
        str_set(last->title, "Other");
        w->track_count = max;
    }
}

// The Mic's cleaning (Config.mic_clean): rumble below a voice cut, then the steady noise floor (fans, hiss, hum),
// which afftdn learns as it goes, taken down by 12 dB. Speech and short sounds are left as they are.
static const char MIC_CLEAN[] = "highpass=f=80,afftdn=nr=12:nf=-45:tn=1";

// The filter graph for the audio tracks. Input 0 is the video; stream i is input i + 1. A stream's concat input
// starts at its first remaining segment, and each stretch of it is trimmed and delayed to its place in the file.
// The Mic is cleaned before anything takes it, and Mic (raw) is the Mic before that.
// `mix_only` (a preview): the Mix and nothing else, so no track is written on its own.
static void make_graph(SaveWork *w, i64 total_us, bool mix_only)
{
    w->graph_len = 0;
    w->graph_full = false;
    w->graph[0] = 0;
    char a[32], b[32], total[32];
    us_secs(total_us, total);
    for (i32 i = 0; i < w->stream_count; i++) {
        // An ignored app's stream is on no track: no pieces, or ffmpeg fails on outputs that go nowhere.
        if (w->stream_track[i] < 0) {
            w->stream_parts[i] = 0;
            continue;
        }
        // Its input is only the segments that reach into the clip, so it starts at the first one's start
        i64 base, avail_end;
        stream_span(w->buf, w->streams[i], &base, &avail_end);
        base = w->buf.audio_segs[w->buf.streams[w->streams[i]].first_seg + w->stream_segs[i][0]].start_us;
        i32 n = 0;
        for (i32 k = 0; k < w->piece_count; k++) {
            const Piece &p = w->pieces[k];
            i64 from = p.start_us > base ? p.start_us : base;
            i64 to = p.end_us < avail_end ? p.end_us : avail_end;
            if (to - from > 1000) w->parts[n++] = {from - base, to - base, p.offset_us + (from - p.start_us)};
        }
        w->stream_parts[i] = n;
        if (n > 1) {
            graph_add(w, "[%d:a]asplit=%d", i + 1, n);
            for (i32 j = 0; j < n; j++) graph_add(w, "[s%d_%d]", i, j);
            graph_add(w, ";\n");
        }
        for (i32 j = 0; j < n; j++) {
            us_secs(w->parts[j].from, a);
            us_secs(w->parts[j].to, b);
            if (n > 1) graph_add(w, "[s%d_%d]", i, j);
            else graph_add(w, "[%d:a]", i + 1);
            graph_add(w,
                      "atrim=start=%s:end=%s,asetpts=PTS-STARTPTS,aformat=sample_fmts=fltp:sample_rates=48000:"
                      "channel_layouts=stereo,adelay=%lldS:all=1[p%d_%d];\n",
                      a, b, w->parts[j].delay * 48000 / 1000000, i, j);
        }
    }
    bool raw = w->track_count > 1 && w->tracks[1].raw && !mix_only;
    for (i32 t = 0; t < w->track_count; t++) {
        if (w->tracks[t].raw) { // the Mic as it was, on its own: [r0] from below
            if (!raw) continue;
            if (w->tracks[t].mono) graph_add(w, "[r0]pan=mono|c0=0.5*c0+0.5*c1[o%d];\n", t);
            else graph_add(w, "[r0]anull[o%d];\n", t);
            continue;
        }
        i32 inputs = 0;
        for (i32 i = 0; i < w->stream_count; i++)
            if (w->stream_track[i] == t) inputs += w->stream_parts[i];
        bool clean = t == 0 && inputs && w->cfg.mic_clean;
        char lab[16];
        snprintf(lab, sizeof lab, "%s%d", clean ? "u" : "t", t);
        if (inputs == 0) {
            graph_add(w, "anullsrc=r=48000:cl=stereo,atrim=duration=%s[%s];\n", total, lab);
        } else {
            for (i32 i = 0; i < w->stream_count; i++)
                if (w->stream_track[i] == t)
                    for (i32 j = 0; j < w->stream_parts[i]; j++) graph_add(w, "[p%d_%d]", i, j);
            if (inputs > 1) graph_add(w, "amix=inputs=%d:normalize=0:dropout_transition=0,", inputs);
            graph_add(w, "apad=whole_dur=%s,atrim=duration=%s[%s];\n", total, total, lab);
        }
        if (clean && raw) graph_add(w, "[u0]asplit=2[v0][r0];\n[v0]%s[t0];\n", MIC_CLEAN);
        else if (clean) graph_add(w, "[u0]%s[t0];\n", MIC_CLEAN);
        // The Mix takes the track as it is; a mono track is only its own copy, both channels averaged.
        if (mix_only) graph_add(w, "[t%d]anull[m%d];\n", t, t);
        else if (w->tracks[t].mono)
            graph_add(w, "[t%d]asplit=2[c%d][m%d];\n[c%d]pan=mono|c0=0.5*c0+0.5*c1[o%d];\n", t, t, t, t, t);
        else graph_add(w, "[t%d]asplit=2[o%d][m%d];\n", t, t, t);
    }
    i32 mixed = 0;
    for (i32 t = 0; t < w->track_count; t++)
        if (!w->tracks[t].raw) graph_add(w, "[m%d]", t), mixed++;
    graph_add(w,
              "amix=inputs=%d:normalize=0:dropout_transition=0,alimiter=limit=0.98:level=0:latency=1,"
              "atrim=duration=%s[mix]",
              mixed, total);
}

// One concat-list line. A single quote in the path is closed, escaped and reopened, as the concat demuxer wants.
static void concat_line(FILE *f, const wchar_t *path)
{
    char p[MAX_PATH * 3];
    wide_to_utf8(path, p, sizeof p);
    fputs("file '", f);
    for (char *c = p; *c; c++) {
        if (*c == '\\') fputc('/', f);
        else if (*c == '\'') fputs("'\\''", f);
        else fputc(*c, f);
    }
    fputs("'\n", f);
}

static Path work_file(const Path &dir, const wchar_t *fmt, i32 n)
{
    wchar_t name[64];
    swprintf(name, COUNT(name), fmt, n);
    return path_join(dir, name);
}

// Whether a line of ffmpeg's output is a -progress "key=value" line rather than a message.
static bool progress_line(const char *s)
{
    const char *eq = strchr(s, '=');
    if (!eq || eq == s) return false;
    for (const char *c = s; c < eq; c++)
        if (!(islower((u8)*c) || isdigit((u8)*c) || *c == '_')) return false;
    return true;
}

// Runs the export ffmpeg, reporting progress. Returns its exit code, or -2 if cancelled. Messages go to `error`.
static i32 run_ffmpeg(SaveWork *w, i64 total_us, char *error, i32 error_cap)
{
    error[0] = 0;
    Proc p;
    if (!proc_start(w->cfg.ffmpeg.s, w->args, PROC_MERGE, &p)) {
        snprintf(error, error_cap, "couldn't start ffmpeg");
        return -1;
    }
    // A preview may be built while the playhead rests and a render whenever an edit settles, so they give way to
    // everything else on the PC
    if (w->req.kind == SAVE_PREVIEW || w->req.kind == SAVE_RENDER)
        SetPriorityClass(p.process, BELOW_NORMAL_PRIORITY_CLASS);
    u64 started = GetTickCount64();
    w->ff_first_ms = w->ff_ms = 0;
    str_set(w->ff_speed, "?");
    char line[1024];
    i32 n = 0;
    i64 last_permille = -1;
    bool cancelled = false;
    // Polled rather than a blocking read, so a cancel is seen within 30 ms even while ffmpeg is quiet.
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(p.out, 0, 0, 0, &avail, 0)) break; // ffmpeg exited and the pipe ended
        if (avail == 0) {
            if (save_cancelled(w->req.id)) { proc_kill(&p); cancelled = true; break; }
            if (proc_wait(&p, 30) >= 0 && (!PeekNamedPipe(p.out, 0, 0, 0, &avail, 0) || avail == 0)) break;
            continue;
        }
        char buf[4096];
        DWORD got = 0;
        if (!ReadFile(p.out, buf, avail < sizeof buf ? avail : sizeof buf, &got, 0) || got == 0) break;
        for (DWORD k = 0; k < got; k++) {
            char c = buf[k];
            if (c != '\n' && c != '\r') { if (n + 1 < (i32)sizeof line) line[n++] = c; continue; }
            if (!n) continue;
            line[n] = 0;
            n = 0;
            if (!progress_line(line)) {
                // A message (with -loglevel error, only errors): keep the newest that fit.
                log_line("save %u: ffmpeg: %s", w->req.id, line);
                size_t have = strlen(error), add = strlen(line);
                if (have + add + 3 >= (size_t)error_cap) have = 0;
                snprintf(error + have, error_cap - have, "%s%s", have ? "; " : "", line);
            } else if (starts_with(line, "speed=")) {
                str_set(w->ff_speed, trim(line + 6));
            } else if (starts_with(line, "out_time_us=") && total_us > 0) {
                i64 t = strtoll(line + 12, 0, 10);
                if (!w->ff_first_ms && t > 0) w->ff_first_ms = GetTickCount64() - started;
                i64 permille = t <= 0 ? 0 : t >= total_us ? 1000 : t * 1000 / total_us;
                if (permille != last_permille) {
                    save_post(SAVE_EV_PROGRESS, w->req.id, w->ff_span ? w->ff_from + permille * w->ff_span / 1000 : permille, 0);
                    last_permille = permille;
                }
            }
        }
    }
    i32 code = proc_wait(&p, INFINITE);
    proc_close(&p);
    w->ff_ms = GetTickCount64() - started;
    return cancelled ? -2 : code;
}

// Deletes the work folders (save-<number>) left by saves cut short by a crash or a restart. Only while no save is
// queued or running, as the folder could be its own; false when one is, to be tried again later.
static bool save_sweep_work()
{
    AcquireSRWLockShared(&g_save_q.lock);
    bool busy = g_save_q.count || g_save_q.running;
    ReleaseSRWLockShared(&g_save_q.lock);
    if (busy) return false;
    const Path &local = g_app.paths.local_dir;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(path_join(local, L"save-*").s, &fd);
    if (h == INVALID_HANDLE_VALUE) return true;
    do {
        const wchar_t *n = fd.cFileName + 5;
        bool digits = *n != 0;
        for (const wchar_t *c = n; *c; c++) digits &= *c >= L'0' && *c <= L'9';
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !digits) continue;
        Path dir = path_join(local, fd.cFileName);
        WIN32_FIND_DATAW in;
        HANDLE hi = FindFirstFileW(path_join(dir, L"*").s, &in);
        if (hi != INVALID_HANDLE_VALUE) {
            do {
                if (!(in.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) DeleteFileW(path_join(dir, in.cFileName).s);
            } while (FindNextFileW(hi, &in));
            FindClose(hi);
        }
        if (RemoveDirectoryW(dir.s)) log_line("save: deleted the work folder %ls, left by a save cut short", fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return true;
}

static void remove_work_files(const Path &dir, i32 streams)
{
    DeleteFileW(path_join(dir, L"video.txt").s);
    DeleteFileW(path_join(dir, L"graph.txt").s);
    DeleteFileW(path_join(dir, L"chapters.txt").s);
    for (i32 i = 0; i < streams; i++) DeleteFileW(work_file(dir, L"audio%d.txt", i).s);
    DeleteFileW(path_join(dir, L"cam.mkv").s);
    DeleteFileW(path_join(dir, L"camgraph.txt").s);
    for (i32 i = 0; i < CAM_SAVE_GROUPS; i++) DeleteFileW(work_file(dir, L"cam%d.txt", i).s);
    RemoveDirectoryW(dir.s);
}

static void arg_list(wchar_t *a, i32 cap, const char *const *args, i32 n)
{
    for (i32 i = 0; i < n; i++) cmd_arg8(a, cap, args[i]);
}

static void arg_pair(wchar_t *a, i32 cap, const char *key, const char *fmt, ...)
{
    char value[1200];
    va_list args;
    va_start(args, fmt);
    vsnprintf(value, sizeof value, fmt, args);
    va_end(args);
    cmd_arg8(a, cap, key);
    cmd_arg8(a, cap, value);
}

// Runs one job. Returns the event to finish with; `out` gets the path saved or the error.
static i64 edit_run(SaveWork *w, char *out, i32 out_cap); // Keep, Share and Link, below

static void edit_encoder_args(wchar_t *a, i32 cap, u8 vendor, i32 kbps);

static BufferIdx g_save_cam; // the worker's index of the webcam's footage

// The webcam over the clip's pieces as one H.264 file, <work>\cam.mkv, the clip's whole length: each stretch of
// footage at its place in the clip, black where the camera has none, all at the size of the first stretch's run.
// Reads g_save_cam. 1 = made; 0 = the clip has no webcam footage or the pass failed (the clip goes without it);
// -2 = cancelled.
static i32 save_cam(SaveWork *w, const Path &work, i64 total_us)
{
    const BufferIdx &c = g_save_cam;
    struct Group { i64 out_us, len_us; i32 run; } groups[CAM_SAVE_GROUPS];
    i32 n = 0;
    FILE *f = 0;
    bool full = false;
    Path cam_dir = path_join(w->cfg.buffer_dir, L"cam");
    for (i32 k = 0; k < w->piece_count && !full; k++) {
        const Piece &p = w->pieces[k];
        for (i32 i = 0; i < c.seg_count && !full; i++) {
            const VideoSeg &s = c.segs[i];
            i64 from = s.start_us > p.start_us ? s.start_us : p.start_us;
            i64 to = s.end_us < p.end_us ? s.end_us : p.end_us;
            if (to - from < 20000) continue;
            i64 at = p.offset_us + from - p.start_us; // in the clip
            Group *g = n ? &groups[n - 1] : 0;
            // A stretch carries on while the run does and lands where the last one ended
            if (!g || g->run != s.run || llabs(g->out_us + g->len_us - at) > 50000) {
                if (f) fclose(f);
                f = n < CAM_SAVE_GROUPS ? _wfopen(work_file(work, L"cam%d.txt", n).s, L"wb") : 0;
                if (!f) { full = true; break; }
                g = &groups[n++];
                *g = {at, 0, s.run};
            }
            wchar_t run[64], name[32];
            utf8_to_wide(c.runs[s.run].name, run, COUNT(run));
            utf8_to_wide(s.name, name, COUNT(name));
            concat_line(f, path_join(path_join(cam_dir, run), name).s);
            fprintf(f, "inpoint %.6f\noutpoint %.6f\n", s.inpoint + (f64)(from - s.start_us) / 1e6,
                    s.inpoint + (f64)(to - s.start_us) / 1e6);
            g->len_us = at + to - from - g->out_us;
        }
    }
    if (f) fclose(f);
    if (!n) return 0;

    // The size the camera's ffmpeg wrote down for the first run; a later run of another size is fitted inside it.
    i32 cw = 1280, ch = 720;
    {
        wchar_t run[64];
        utf8_to_wide(c.runs[groups[0].run].name, run, COUNT(run));
        char text[32];
        i32 x = 0, y = 0;
        if (read_small(path_join(path_join(cam_dir, run), L"size.txt"), text, sizeof text) &&
            sscanf(text, "%d %d", &x, &y) == 2 && x >= 16 && y >= 16 && x <= 4096 && y <= 4096) {
            cw = x & ~1;
            ch = y & ~1;
        }
    }
    const char *pf = w->cfg.vendor == VENDOR_QSV ? "nv12" : "yuv420p";
    w->graph_len = 0;
    w->graph_full = false;
    i32 parts = 0;
    i64 t = 0;
    for (i32 i = 0; i <= n; i++) {
        i64 next = i < n ? groups[i].out_us : total_us;
        if (next - t > 40000)
            graph_add(w, "color=c=black:s=%dx%d:r=30:d=%.6f,setsar=1,format=%s[p%d];\n", cw, ch, (f64)(next - t) / 1e6, pf,
                      parts++);
        if (i == n) break;
        // Each stretch at 30 fps, held on its last frame to exactly its length
        graph_add(w,
                  "[%d:v]setpts=PTS-STARTPTS,scale=%d:%d:force_original_aspect_ratio=decrease,pad=%d:%d:-1:-1,setsar=1,"
                  "fps=30,format=%s,tpad=stop_mode=clone:stop_duration=1,trim=duration=%.6f,setpts=PTS-STARTPTS[p%d];\n",
                  i, cw, ch, cw, ch, pf, (f64)groups[i].len_us / 1e6, parts++);
        t = groups[i].out_us + groups[i].len_us;
    }
    for (i32 i = 0; i < parts; i++) graph_add(w, "[p%d]", i);
    graph_add(w, "concat=n=%d:v=1:a=0[cam]\n", parts);
    f = w->graph_full ? 0 : _wfopen(path_join(work, L"camgraph.txt").s, L"wb");
    if (!f) return 0;
    fwrite(w->graph, 1, w->graph_len, f);
    fclose(f);

    wchar_t *a = w->args;
    i32 cap = COUNT(w->args);
    a[0] = 0;
    static const char *const head[] = {"-hide_banner", "-loglevel", "error", "-nostats", "-progress", "pipe:1", "-y"};
    static const char *const concat[] = {"-f", "concat", "-safe", "0", "-i"};
    arg_list(a, cap, head, COUNT(head));
    for (i32 i = 0; i < n; i++) {
        arg_list(a, cap, concat, COUNT(concat));
        cmd_arg(a, cap, work_file(work, L"cam%d.txt", i).s);
    }
    cmd_arg8(a, cap, "-/filter_complex");
    cmd_arg(a, cap, path_join(work, L"camgraph.txt").s);
    arg_pair(a, cap, "-map", "[cam]");
    edit_encoder_args(a, cap, w->cfg.vendor, 0);
    arg_pair(a, cap, "-f", "matroska");
    cmd_arg(a, cap, path_join(work, L"cam.mkv").s);
    w->ff_from = 0;
    w->ff_span = 300;
    char err[512];
    i32 code = run_ffmpeg(w, total_us, err, sizeof err);
    w->ff_from = 300;
    w->ff_span = 700;
    if (code == -2) return -2;
    if (code) {
        log_line("save %u: the webcam is left out: %s", w->req.id, err[0] ? err : "ffmpeg failed");
        w->ff_span = 0;
        return 0;
    }
    log_line("save %u: webcam %dx%d in %d stretches, %.1f s", w->req.id, cw, ch, n, (f64)w->ff_ms / 1000);
    return 1;
}

static i64 save_run(SaveWork *w, char *out, i32 out_cap)
{
    const SaveRequest &req = w->req;
    out[0] = 0;
    w->ff_span = 0;
    if (!w->cfg.ffmpeg.s[0]) {
        snprintf(out, out_cap, "ffmpeg isn't ready yet (" APP_NAME " is getting it, or set its path in Options)");
        return SAVE_EV_FAILED;
    }
    if (req.kind == SAVE_KEEP || req.kind == SAVE_SHARE || req.kind == SAVE_LINK || req.kind == SAVE_RENDER)
        return edit_run(w, out, out_cap);

    // The segment covering `to` is still being written: wait for it to finish (a couple of seconds at most), so the
    // clip ends at the moment the save was asked for. "Being written" is the recorder saying it's capturing, or a
    // list.csv written moments ago (a recorder outside this process).
    u64 t_start = GetTickCount64(); // each stage is timed, logged when the save ends
    u64 deadline = t_start + SEGMENT_US / 1000 + 2500;
    for (bool wait = req.kind != SAVE_PREVIEW; wait;) { // a preview takes what's there
        i64 end = 0, written = 0;
        bool known = buffer_video_newest_end(w->cfg.buffer_dir, &end, &written);
        bool live = req.recording || (known && now_us() - written < SEGMENT_US + 3000000);
        if (!live || end >= req.to_us - 100000 || GetTickCount64() > deadline) break;
        if (save_cancelled(req.id)) return SAVE_EV_CANCELLED;
        Sleep(150);
    }

    // The webcam's newest segment, when it's still recording, gets a moment more to cover `to` too.
    for (bool wait = req.kind != SAVE_PREVIEW; wait;) {
        buffer_read_cam(w->cfg.buffer_dir, &g_save_cam);
        const BufferIdx &c = g_save_cam;
        i64 end = c.seg_count ? c.segs[c.seg_count - 1].end_us : 0;
        if (!end || end >= req.to_us - 100000 || now_us() - end > SEGMENT_US + 3000000 || GetTickCount64() > deadline + 2500)
            break;
        if (save_cancelled(req.id)) return SAVE_EV_CANCELLED;
        Sleep(150);
    }

    u64 t_waited = GetTickCount64();
    if (!buffer_read(w->cfg.buffer_dir, &w->buf)) {
        snprintf(out, out_cap, "out of memory reading the buffer");
        return SAVE_EV_FAILED;
    }
    u64 t_read = GetTickCount64();
    BufferIdx &b = w->buf;
    i32 first = 0;
    while (first < b.seg_count && b.segs[first].end_us <= req.from_us) first++;
    i32 end = first;
    while (end < b.seg_count && b.segs[end].start_us < req.to_us) end++;
    if (end == first) {
        snprintf(out, out_cap, "nothing recorded in that time; is recording paused?");
        return SAVE_EV_FAILED;
    }
    make_pieces(w, first, end);
    i64 total_us = 0;
    for (i32 k = 0; k < w->piece_count; k++) total_us += w->pieces[k].end_us - w->pieces[k].start_us;
    i64 clip_start = b.segs[first].start_us, clip_end = b.segs[end - 1].end_us;

    w->stream_count = 0;
    for (i32 s = 0; s < b.stream_count; s++) {
        if (!b.streams[s].seg_count) continue;
        i64 start, stop;
        stream_span(b, s, &start, &stop);
        if (start >= clip_end || stop <= clip_start) continue;
        if (w->stream_count == COUNT(w->streams)) {
            log_line("save %u: more than %d audio streams; the rest left out", req.id, COUNT(w->streams));
            break;
        }
        // Only the segments that reach into the clip go to ffmpeg: atrim decodes everything before its start, which
        // hours into a long stream (the mic) is most of the save's time.
        const AudioStreamInfo &st = b.streams[s];
        i32 k0 = 0, k1 = st.seg_count;
        while (k0 < st.seg_count - 1) {
            const AudioSeg &g = b.audio_segs[st.first_seg + k0];
            if (g.open || g.end_us > clip_start) break;
            k0++;
        }
        while (k1 > k0 + 1 && b.audio_segs[st.first_seg + k1 - 1].start_us >= clip_end) k1--;
        w->stream_segs[w->stream_count][0] = k0;
        w->stream_segs[w->stream_count][1] = k1;
        w->streams[w->stream_count++] = s;
    }

    // The game played most names and tags it; no game at all is "Desktop".
    w->game_count = games_in(b, clip_start, clip_end, w->games, COUNT(w->games));
    make_tracks(w);
    char game[96], names[1024] = {}, exes[1024] = {};
    if (w->game_count) display_name(w->cfg, w->games[0].exe, game, sizeof game);
    else str_set(game, GAME_DESKTOP);
    for (i32 g = 0; g < w->game_count; g++) {
        char n[96];
        display_name(w->cfg, w->games[g].exe, n, sizeof n);
        size_t ln = strlen(names), le = strlen(exes);
        snprintf(names + ln, sizeof names - ln, "%s%s", g ? ", " : "", n);
        snprintf(exes + le, sizeof exes - le, "%s%s", g ? "," : "", w->games[g].exe);
    }
    char label[256];
    str_set(label, req.name[0] ? req.name : game);
    clean_name(label);
    if (!label[0]) str_set(label, game);

    // <save_dir>\Clips\<year>\<name>.mp4 or <save_dir>\Sessions\<year>\<name>.mp4, the name from the template in
    // Options with the label as {game}. The year is the year the footage starts in.
    // A preview goes in the playback cache as preview-<id>.mp4.
    SYSTEMTIME t = local_time(clip_start);
    bool session = req.kind == SAVE_SESSION, preview = req.kind == SAVE_PREVIEW;
    Path dir = {};
    if (preview) dir = w->cfg.play_dir;
    else dir = session ? save_sessions_dir(w->cfg.save_dir, t.wYear) : save_clips_dir(w->cfg.save_dir, t.wYear);
    int made = SHCreateDirectoryExW(0, dir.s, 0);
    if (made != ERROR_SUCCESS && made != ERROR_ALREADY_EXISTS && made != ERROR_FILE_EXISTS) {
        char d[MAX_PATH * 3];
        wide_to_utf8(dir.s, d, sizeof d);
        snprintf(out, out_cap, "couldn't create %s (%d)", d, made);
        return SAVE_EV_FAILED;
    }
    char stem[400];
    if (preview) snprintf(stem, sizeof stem, "preview-%u", req.id);
    else save_name_expand(w->cfg.name_template, label, clip_start, stem, sizeof stem);
    wchar_t wstem[400];
    utf8_to_wide(stem, wstem, COUNT(wstem));
    // ffmpeg writes "<name>.mp4.part", renamed when complete, so the library never sees half a file.
    Path final_path = {}, part_path = {};
    for (i32 n = 1; n < 1000; n++) {
        if (n == 1) swprintf(final_path.s, MAX_PATH, L"%s\\%s.mp4", dir.s, wstem);
        else swprintf(final_path.s, MAX_PATH, L"%s\\%s (%d).mp4", dir.s, wstem, n);
        swprintf(part_path.s, MAX_PATH, L"%s.part", final_path.s);
        if (!file_exists(final_path.s) && !file_exists(part_path.s)) break;
    }
    char final8[MAX_PATH * 3];
    wide_to_utf8(final_path.s, final8, sizeof final8);
    save_post(SAVE_EV_PATH, req.id, 0, final8);
    save_post(SAVE_EV_GAME, req.id, 0, w->game_count ? game : "");

    // Lists and the filter graph go in files (graphs get long).
    Path work = work_file(w->cfg.work_dir, L"save-%d", (i32)req.id);
    SHCreateDirectoryExW(0, work.s, 0);
    FILE *f = _wfopen(path_join(work, L"video.txt").s, L"wb");
    if (!f) { snprintf(out, out_cap, "couldn't write the save's work files"); return SAVE_EV_FAILED; }
    Path video_dir = path_join(w->cfg.buffer_dir, L"video");
    for (i32 i = first; i < end; i++) {
        const VideoSeg &s = b.segs[i];
        wchar_t run[64], name[32];
        utf8_to_wide(b.runs[s.run].name, run, COUNT(run));
        utf8_to_wide(s.name, name, COUNT(name));
        // Segments keep the run's timestamps; in and out points let concat line them up end to end.
        concat_line(f, path_join(path_join(video_dir, run), name).s);
        fprintf(f, "inpoint %.6f\noutpoint %.6f\n", s.inpoint, s.outpoint);
    }
    fclose(f);
    Path audio_dir = path_join(w->cfg.buffer_dir, L"audio");
    for (i32 i = 0; i < w->stream_count; i++) {
        const AudioStreamInfo &st = b.streams[w->streams[i]];
        f = _wfopen(work_file(work, L"audio%d.txt", i).s, L"wb");
        if (!f) { snprintf(out, out_cap, "couldn't write the save's work files"); remove_work_files(work, i); return SAVE_EV_FAILED; }
        wchar_t sdir[128];
        utf8_to_wide(st.dir, sdir, COUNT(sdir));
        Path stream_dir = path_join(audio_dir, sdir);
        for (i32 k = w->stream_segs[i][0]; k < w->stream_segs[i][1]; k++) {
            const AudioSeg &seg = b.audio_segs[st.first_seg + k];
            wchar_t name[32];
            utf8_to_wide(seg.name, name, COUNT(name));
            concat_line(f, path_join(stream_dir, name).s);
            // Each segment's timestamps carry on from the stream's start, and its Matroska duration is the absolute
            // end, so concat left to itself opens a growing gap after every file. The listed length keeps them end
            // to end. (The open last segment has none, and needs none.)
            if (!seg.open) fprintf(f, "duration %.6f\n", (f64)(seg.end_us - seg.start_us) / 1e6);
        }
        fclose(f);
    }
    // Chapters: the markers in the clip, titled by their notes, and in a session where each game starts.
    static Chapter chapters[256];
    static char chapter_meta[1 << 17];
    i32 chapter_count = 0;
    {
        static Marker marks[COUNT(chapters)];
        i32 mark_count = markers_in(clip_start, clip_end, marks, COUNT(marks));
        for (i32 i = 0; i < mark_count; i++) {
            chapters[chapter_count].t_us = marks[i].t_us;
            str_set(chapters[chapter_count++].title, marks[i].note[0] ? marks[i].note : "Marker");
        }
        const char *prev = "";
        for (i32 i = 0; session && i < b.activity_count && chapter_count < COUNT(chapters); i++) {
            const ActivityEntry &e = b.activity[i];
            bool ends_before = i + 1 < b.activity_count && b.activity[i + 1].us <= clip_start;
            if (e.us >= clip_end) break;
            if (e.exe[0] && strcmp(e.exe, prev) && !ends_before) {
                chapters[chapter_count].t_us = e.us > clip_start ? e.us : clip_start;
                display_name(w->cfg, e.exe, chapters[chapter_count].title, sizeof chapters[0].title);
                chapter_count++;
            }
            prev = e.exe;
        }
    }
    if (preview) chapter_count = 0; // nothing to jump between while it plays
    else chapter_count = chapters_text(w->pieces, w->piece_count, total_us, chapters, chapter_count, game, chapter_meta,
                                  sizeof chapter_meta);
    if (chapter_count) {
        f = _wfopen(path_join(work, L"chapters.txt").s, L"wb");
        if (f) { fwrite(chapter_meta, 1, strlen(chapter_meta), f); fclose(f); }
        else chapter_count = 0;
    }

    make_graph(w, total_us, preview);
    f = w->graph_full ? 0 : _wfopen(path_join(work, L"graph.txt").s, L"wb");
    if (!f) {
        snprintf(out, out_cap, w->graph_full ? "the audio graph is too big" : "couldn't write the save's work files");
        remove_work_files(work, w->stream_count);
        return SAVE_EV_FAILED;
    }
    fwrite(w->graph, 1, w->graph_len, f);
    fclose(f);

    // The webcam goes beside the game as a second video stream, made first in a pass of its own.
    i32 cam = preview ? 0 : save_cam(w, work, total_us);
    if (cam == -2) {
        remove_work_files(work, w->stream_count);
        return SAVE_EV_CANCELLED;
    }

    // The command line.
    wchar_t *a = w->args;
    i32 cap = COUNT(w->args);
    a[0] = 0;
    static const char *const head[] = {"-hide_banner", "-loglevel", "error", "-nostats", "-progress", "pipe:1", "-y"};
    static const char *const concat[] = {"-f", "concat", "-safe", "0", "-i"};
    arg_list(a, cap, head, COUNT(head));
    arg_list(a, cap, concat, COUNT(concat));
    cmd_arg(a, cap, path_join(work, L"video.txt").s);
    for (i32 i = 0; i < w->stream_count; i++) {
        arg_list(a, cap, concat, COUNT(concat));
        cmd_arg(a, cap, work_file(work, L"audio%d.txt", i).s);
    }
    if (chapter_count) {
        arg_pair(a, cap, "-f", "ffmetadata");
        cmd_arg8(a, cap, "-i");
        cmd_arg(a, cap, path_join(work, L"chapters.txt").s);
        arg_pair(a, cap, "-map_chapters", "%d", w->stream_count + 1);
    }
    if (cam) {
        cmd_arg8(a, cap, "-i");
        cmd_arg(a, cap, path_join(work, L"cam.mkv").s);
    }
    cmd_arg8(a, cap, "-/filter_complex");
    cmd_arg(a, cap, path_join(work, L"graph.txt").s);
    arg_pair(a, cap, "-map", "0:v");
    if (cam) arg_pair(a, cap, "-map", "%d:v", w->stream_count + 1 + (chapter_count ? 1 : 0));
    arg_pair(a, cap, "-c:v", "copy");
    if (!strcmp(w->cfg.codec, "hevc")) arg_pair(a, cap, "-tag:v:0", "hvc1"); // so Apple and browser players take it
    if (cam) { // players show the game; the webcam is there for editors
        arg_pair(a, cap, "-disposition:v:0", "default");
        arg_pair(a, cap, "-disposition:v:1", "0");
        arg_pair(a, cap, "-metadata:s:v:1", "title=" CAM_STREAM_TITLE);
        arg_pair(a, cap, "-metadata:s:v:1", "handler_name=" CAM_STREAM_TITLE);
    }
    arg_pair(a, cap, "-map", "[mix]");
    i32 own_tracks = preview ? 0 : w->track_count; // a preview is only played here: its Mix is enough
    for (i32 tr = 0; tr < own_tracks; tr++) arg_pair(a, cap, "-map", "[o%d]", tr);
    arg_pair(a, cap, "-c:a", "aac");
    arg_pair(a, cap, "-b:a", "192k");
    arg_pair(a, cap, "-ar", "48000");
    for (i32 i = 0; i <= own_tracks; i++) {
        const char *title = i == 0 ? "Mix" : w->tracks[i - 1].title;
        char key[48];
        if (i > 0 && w->tracks[i - 1].mono) {
            snprintf(key, sizeof key, "-b:a:%d", i);
            arg_pair(a, cap, key, "96k");
        }
        snprintf(key, sizeof key, "-metadata:s:a:%d", i);
        arg_pair(a, cap, key, "title=%s", title);
        arg_pair(a, cap, key, "handler_name=%s", title);
        snprintf(key, sizeof key, "-disposition:a:%d", i);
        arg_pair(a, cap, key, i == 0 ? "default" : "0");
    }
    // Genre = the game (Explorer can show, sort and group by it); the exes go in the comment, for icons.
    char hm[16];
    fmt_hm(t, hm, sizeof hm);
    arg_pair(a, cap, "-metadata", "title=%s - %d %.3s %d %s", label, t.wDay, month_name(t.wMonth), t.wYear, hm);
    arg_pair(a, cap, "-metadata", "genre=%s", game);
    arg_pair(a, cap, "-metadata", "description=%s", names[0] ? names : GAME_DESKTOP);
    arg_pair(a, cap, "-metadata", "comment=rekodr; games=%s", exes);
    arg_pair(a, cap, "-movflags", "+faststart");
    arg_pair(a, cap, "-f", "mp4");
    cmd_arg(a, cap, part_path.s);
    // proc_start's command line holds 32767 characters with the exe; cut short, ffmpeg would get nonsense.
    if (wcslen(a) + MAX_PATH + 8 >= 32767) {
        snprintf(out, out_cap, "too many audio streams for one command line");
        remove_work_files(work, w->stream_count);
        return SAVE_EV_FAILED;
    }

    log_line("save %u: %.1f s from %lld, %d video segments in %d pieces, %d audio streams on %d tracks, %d chapters%s -> %s",
             req.id, (f64)total_us / 1e6, clip_start, end - first, w->piece_count, w->stream_count,
             own_tracks + 1, chapter_count, cam ? ", webcam" : "", final8);
    u64 t_ready = GetTickCount64();
    i32 code = run_ffmpeg(w, total_us, out, out_cap);
    u64 t_ran = GetTickCount64();
    i64 result;
    if (code == 0 && MoveFileExW(part_path.s, final_path.s, 0)) {
        snprintf(out, out_cap, "%s", final8);
        result = SAVE_EV_DONE;
        if (!preview) save_layers(w, clip_start, clip_end, final_path);
    } else {
        DeleteFileW(part_path.s);
        if (code == -2) {
            result = SAVE_EV_CANCELLED;
        } else {
            if (code == 0) snprintf(out, out_cap, "couldn't rename the finished file (%d)", (int)GetLastError());
            else if (!out[0]) snprintf(out, out_cap, "ffmpeg failed (exit code %d)", code);
            // The graph is the usual suspect: keep it for a look.
            CopyFileW(path_join(work, L"graph.txt").s, path_join(w->cfg.work_dir, L"last-failed-graph.txt").s, FALSE);
            result = SAVE_EV_FAILED;
        }
    }
    remove_work_files(work, w->stream_count);
    u64 t_end = GetTickCount64();
    log_line("save %u: times: queued %.2f s, waiting for footage %.2f s, buffer read %.2f s, lists and graph %.2f s, ffmpeg %.2f s "
             "(first progress after %.2f s, speed %s), finishing %.2f s",
             req.id, (t_start - req.queued_ms) / 1000.0, (t_waited - t_start) / 1000.0, (t_read - t_waited) / 1000.0, (t_ready - t_read) / 1000.0,
             (t_ran - t_ready) / 1000.0, w->ff_first_ms / 1000.0, w->ff_speed, (t_end - t_ran) / 1000.0);
    return result;
}

// ---------------------------------------------------------------------------------------------------------------
// Keep and Share: an edit of a saved file.
//
// Keep is the edit at full quality into Clips\<year>, every track kept. With one piece the video is stream-copied when
// there's no zoom or colour (cut at the keyframe before the start), else re-encoded at a constant quality; the audio is
// copied as it is when no volume was touched, else the Mix is rebuilt from the edited tracks and the separate tracks
// are copied as recorded. With several pieces everything is re-encoded, each stream cut into the pieces and joined,
// since a copy can only cut at keyframes. Share is a small file in the share folder: the pieces joined, zoomed,
// coloured, scaled down and encoded to the size asked for, with one stereo mix of the edited tracks.


// Pieces: the parts of a video an edit keeps, played one after another. Times on the joined result ("output time")
// run from 0 at the first piece's start; times in the file ("source time") are what keys and pieces are stored in,
// so a key stays on its moment whatever is cut around it. `dur` is the video's length.
static const f64 PIECE_LEAST = 0.1; // the shortest a piece gets

// The pieces as they play: the whole video when there are none, each held inside the video, an end within 0.01 s of
// the video's end taken as the end.
static i32 pieces_of(const EditRecord &e, f64 dur, EditPiece *out)
{
    i32 n = 0;
    for (i32 i = 0; i < e.piece_count && i < EDIT_MAX_PIECES; i++) {
        EditPiece p = e.pieces[i];
        if (p.from < 0) p.from = 0;
        if (dur > 0 && p.to >= dur - 0.01) p.to = dur;
        if (p.to > p.from) out[n++] = p;
    }
    if (!n) out[n++] = {0, dur};
    return n;
}

// Stores pieces; one that is the whole video is stored as none.
static void pieces_store(EditRecord *e, const EditPiece *p, i32 n, f64 dur)
{
    if (n > EDIT_MAX_PIECES) n = EDIT_MAX_PIECES;
    if (n == 1 && p[0].from <= 0.0005 && p[0].to >= dur - 0.01) n = 0;
    memmove(e->pieces, p, n * sizeof *p);
    e->piece_count = n;
}

static f64 pieces_len(const EditRecord &e, f64 dur)
{
    EditPiece p[EDIT_MAX_PIECES];
    i32 n = pieces_of(e, dur, p);
    f64 len = 0;
    for (i32 i = 0; i < n; i++) len += p[i].to - p[i].from;
    return len;
}

// Output time to source time, and the piece it's in. Before the start is the first piece's start, past the end the
// last piece's end.
static f64 piece_src(const EditRecord &e, f64 dur, f64 u, i32 *at = 0)
{
    EditPiece p[EDIT_MAX_PIECES];
    i32 n = pieces_of(e, dur, p);
    f64 o = 0;
    for (i32 i = 0; i < n; i++) {
        f64 len = p[i].to - p[i].from;
        if (u < o + len || i == n - 1) {
            if (at) *at = i;
            f64 d = u - o;
            return p[i].from + (d < 0 ? 0 : d > len ? len : d);
        }
        o += len;
    }
    if (at) *at = 0;
    return 0;
}

// Source time to output time; false when no piece holds it. A moment that is one piece's end and the next one's
// start is the next one's.
static bool piece_out(const EditRecord &e, f64 dur, f64 s, f64 *u, i32 *at = 0)
{
    EditPiece p[EDIT_MAX_PIECES];
    i32 n = pieces_of(e, dur, p);
    for (i32 pass = 0; pass < 2; pass++) {
        f64 o = 0;
        for (i32 i = 0; i < n; i++) {
            if (s >= p[i].from && (s < p[i].to || (pass && s <= p[i].to))) {
                *u = o + (s - p[i].from);
                if (at) *at = i;
                return true;
            }
            o += p[i].to - p[i].from;
        }
    }
    return false;
}

// Where piece i starts in output time.
static f64 piece_start(const EditRecord &e, f64 dur, i32 i)
{
    EditPiece p[EDIT_MAX_PIECES];
    i32 n = pieces_of(e, dur, p);
    f64 o = 0;
    for (i32 k = 0; k < i && k < n; k++) o += p[k].to - p[k].from;
    return o;
}

// Splits the piece under output time u in two there; false when u is too close to its edges or there's no room.
static bool pieces_split(EditRecord *e, f64 dur, f64 u)
{
    EditPiece p[EDIT_MAX_PIECES + 1];
    i32 n = pieces_of(*e, dur, p), i = 0;
    if (n >= EDIT_MAX_PIECES) return false;
    f64 s = piece_src(*e, dur, u, &i);
    if (s - p[i].from < PIECE_LEAST || p[i].to - s < PIECE_LEAST) return false;
    memmove(p + i + 2, p + i + 1, (n - i - 1) * sizeof *p);
    p[i + 1] = {s, p[i].to};
    p[i].to = s;
    pieces_store(e, p, n + 1, dur);
    return true;
}

// Removes the pieces in `mask` (bit i for piece i) and the rest close up. Keys that were only in removed pieces go
// with them. False when that would remove them all.
static bool pieces_delete(EditRecord *e, f64 dur, u64 mask)
{
    EditPiece p[EDIT_MAX_PIECES], gone[EDIT_MAX_PIECES];
    i32 n = pieces_of(*e, dur, p), kept = 0, ng = 0;
    for (i32 i = 0; i < n; i++) {
        if (mask >> i & 1) gone[ng++] = p[i];
        else p[kept++] = p[i];
    }
    if (!ng || !kept) return false;
    auto inside = [](const EditPiece *q, i32 count, f64 t) {
        for (i32 i = 0; i < count; i++)
            if (t >= q[i].from && t <= q[i].to) return true;
        return false;
    };
    auto drop = [&](f64 t) { return inside(gone, ng, t) && !inside(p, kept, t); };
    i32 nk = 0;
    for (i32 k = 0; k < e->key_count; k++)
        if (!drop(e->keys[k].t)) e->keys[nk++] = e->keys[k];
    e->key_count = nk;
    for (i32 t = 0; t <= EDIT_MASTER; t++) {
        i32 nv = 0;
        for (i32 k = 0; k < e->vol_count[t]; k++)
            if (!drop(e->vol[t][k].t)) e->vol[t][nv++] = e->vol[t][k];
        e->vol_count[t] = nv;
    }
    for (i32 c = 0; c < EDIT_COLORS; c++) {
        i32 nc = 0;
        for (i32 k = 0; k < e->color_count[c]; k++)
            if (!drop(e->color[c][k].t)) e->color[c][nc++] = e->color[c][k];
        e->color_count[c] = nc;
    }
    pieces_store(e, p, kept, dur);
    return true;
}

// The furthest one edge of piece i can go: as far as the video, or the edge of another piece in the way, and never
// closer than PIECE_LEAST to its other edge.
static void piece_edge_range(const EditPiece *p, i32 n, f64 dur, i32 i, bool end, f64 *lo, f64 *hi)
{
    if (end) {
        *lo = p[i].from + PIECE_LEAST;
        *hi = dur;
        for (i32 k = 0; k < n; k++)
            if (k != i && p[k].from >= p[i].to - 0.0005 && p[k].from < *hi) *hi = p[k].from;
    } else {
        *lo = 0;
        *hi = p[i].to - PIECE_LEAST;
        for (i32 k = 0; k < n; k++)
            if (k != i && p[k].to <= p[i].from + 0.0005 && p[k].to > *lo) *lo = p[k].to;
    }
}

// Moves the start or end of piece i to source time s, held in its range. Returns where it went.
static f64 pieces_trim(EditRecord *e, f64 dur, i32 i, bool end, f64 s)
{
    EditPiece p[EDIT_MAX_PIECES];
    i32 n = pieces_of(*e, dur, p);
    if (i < 0 || i >= n) return s;
    f64 lo, hi;
    piece_edge_range(p, n, dur, i, end, &lo, &hi);
    s = s < lo ? lo : s > hi ? hi : s;
    (end ? p[i].to : p[i].from) = s;
    pieces_store(e, p, n, dur);
    return s;
}

// Moves piece i to place `to` in the order.
static void pieces_move(EditRecord *e, f64 dur, i32 i, i32 to)
{
    EditPiece p[EDIT_MAX_PIECES];
    i32 n = pieces_of(*e, dur, p);
    if (i < 0 || i >= n) return;
    to = to < 0 ? 0 : to >= n ? n - 1 : to;
    EditPiece m = p[i];
    if (to > i) memmove(p + i, p + i + 1, (to - i) * sizeof *p);
    else memmove(p + to + 1, p + to, (i - to) * sizeof *p);
    p[to] = m;
    pieces_store(e, p, n, dur);
}

// Transitions as they play, for pieces p[0..n) of a video `dur` long. A transition sits on its cut, half before and
// half after, so the joined length stays the pieces' own. A fade dips each side to black over its half; a crossfade
// blends the two, so it also plays the footage past the first piece's end and before the second one's start, and is
// held to what the video has there. Each half and each end fade is held to half its piece; anything under 0.05 s
// is a cut. into[i] is the transition into piece i (into[0] none), with its length in len[i].
struct Transitions {
    u8  into[EDIT_MAX_PIECES];
    f64 len[EDIT_MAX_PIECES];
    f64 fade_in, fade_out;
};

static const f64 TRANS_LEAST = 0.05;

static Transitions transitions_of(const EditRecord &e, const EditPiece *p, i32 n, f64 dur)
{
    Transitions t = {};
    for (i32 i = 1; i < n; i++) {
        f64 l = p[i].trans_len, a = p[i - 1].to - p[i - 1].from, b = p[i].to - p[i].from;
        u8 k = p[i].trans < TRANS_COUNT ? p[i].trans : TRANS_CUT;
        if (l > a) l = a;
        if (l > b) l = b;
        if (k == TRANS_CROSS) {
            f64 after = dur - p[i - 1].to, before = p[i].from;
            if (l > 2 * after) l = 2 * after;
            if (l > 2 * before) l = 2 * before;
        }
        if (k == TRANS_CUT || l < TRANS_LEAST) k = TRANS_CUT, l = 0;
        t.into[i] = k;
        t.len[i] = l;
    }
    f64 first = p[0].to - p[0].from, last = p[n - 1].to - p[n - 1].from;
    t.fade_in = e.fade_in < first / 2 ? e.fade_in : first / 2;
    t.fade_out = e.fade_out < last / 2 ? e.fade_out : last / 2;
    if (t.fade_in < TRANS_LEAST) t.fade_in = 0;
    if (t.fade_out < TRANS_LEAST) t.fade_out = 0;
    return t;
}

static bool transitions_any(const Transitions &t, i32 n)
{
    for (i32 i = 1; i < n; i++)
        if (t.into[i] != TRANS_CUT) return true;
    return t.fade_in > 0 || t.fade_out > 0;
}

// The fades into and out of black on piece i: from its start over `in` seconds and to its end over `out`.
static void piece_fades(const Transitions &t, i32 n, i32 i, f64 *in, f64 *out)
{
    *in = !i ? t.fade_in : t.into[i] == TRANS_FADE ? t.len[i] / 2 : 0;
    *out = i == n - 1 ? t.fade_out : t.into[i + 1] == TRANS_FADE ? t.len[i + 1] / 2 : 0;
}

// How much of the picture and sound is left by the fades at source time s in piece i (1 = all, 0 = black and
// silent). A crossfade isn't a fade, so it leaves 1.
static f64 fade_gain(const EditRecord &e, f64 dur, i32 i, f64 s)
{
    EditPiece p[EDIT_MAX_PIECES];
    i32 n = pieces_of(e, dur, p);
    if (i < 0 || i >= n) return 1;
    Transitions t = transitions_of(e, p, n, dur);
    f64 in, out, g = 1;
    piece_fades(t, n, i, &in, &out);
    if (in > 0 && s < p[i].from + in) g = (s - p[i].from) / in;
    if (out > 0 && s > p[i].to - out && (p[i].to - s) / out < g) g = (p[i].to - s) / out;
    if (g < 0) g = 0;
    return g;
}

// Whether a file has the Rekodr layout: track 1 is a Mix of the tracks after it.
static bool edit_has_mix(const MediaInfo &m) { return m.track_count >= 2 && !strcmp(m.tracks[0], "Mix"); }

// Whether track t of a Rekodr file is Mic (raw): kept for other editors, never in a Mix or the editor's tracks.
static bool track_is_spare(const MediaInfo &m, i32 t) { return edit_has_mix(m) && !strcmp(m.tracks[t], MIC_RAW_TITLE); }

// Eases (Ease in app.h): how far along (0..1) a value is, a fraction f of the way in time from one key to the next.
static bool ease_slow_leaving(u8 e) { return e == EASE_SMOOTH || e == EASE_OUT; }
static bool ease_slow_arriving(u8 e) { return e == EASE_SMOOTH || e == EASE_IN; }

static f64 ease_curve(u8 from, u8 to, f64 f)
{
    f = f < 0 ? 0 : f > 1 ? 1 : f;
    if (from == EASE_HOLD) return 0;
    bool l = ease_slow_leaving(from), a = ease_slow_arriving(to);
    if (l && a) return f * f * (3 - 2 * f);
    if (l) return f * f;
    if (a) return f * (2 - f);
    return f;
}

// The same curve as an ffmpeg expression of u (already 0..1); false for Hold (the value stays put).
static bool ease_expr(u8 from, u8 to, const char *u, char *out, i32 cap)
{
    if (from == EASE_HOLD) return false;
    bool l = ease_slow_leaving(from), a = ease_slow_arriving(to);
    if (l && a) snprintf(out, cap, "%s*%s*(3-2*%s)", u, u, u);
    else if (l) snprintf(out, cap, "%s*%s", u, u);
    else if (a) snprintf(out, cap, "%s*(2-%s)", u, u);
    else snprintf(out, cap, "%s", u);
    return true;
}

// A level's keys at `t`: eased between them, holding before the first and after the last, `rest` when there are none.
static f64 level_at(const VolKey *k, i32 n, f64 rest, f64 t)
{
    if (!n) return rest;
    if (t <= k[0].t) return k[0].v;
    for (i32 i = 1; i < n; i++)
        if (t < k[i].t) {
            f64 span = k[i].t - k[i - 1].t;
            return k[i - 1].v + (k[i].v - k[i - 1].v) * ease_curve(k[i - 1].ease, k[i].ease, (t - k[i - 1].t) / (span > 0.001 ? span : 0.001));
        }
    return k[n - 1].v;
}

// A track's volume (or the mix's, at EDIT_MASTER) at `t`, before muting: its keys, or its one level when it has none.
static f64 vol_at(const EditRecord &e, i32 track, f64 t)
{
    return level_at(e.vol[track], e.vol_count[track], 1.0 + e.boost[track], t);
}

// A colour value at `t`, 1 = as recorded.
static f64 color_at(const EditRecord &e, i32 c, f64 t) { return level_at(e.color[c], e.color_count[c], 1, t); }

// What ffmpeg's eq filter is given for a colour value v (0..2, 1 = as recorded): brightness is added (-0.3..0.3),
// contrast and saturation multiply. The editor's preview applies the same.
static const f64 EQ_BRIGHTNESS = 0.3;
static f64 color_eq(i32 c, f64 v) { return c == COLOR_BRIGHTNESS ? (v - 1) * EQ_BRIGHTNESS : v; }

// Whether the edit changes the picture's colour anywhere.
static bool edit_colored(const EditRecord &e)
{
    for (i32 c = 0; c < EDIT_COLORS; c++)
        for (i32 k = 0; k < e.color_count[c]; k++)
            if (fabs(e.color[c][k].v - 1) > 0.001) return true;
    return false;
}

// What's heard of a track at `t`: its volume, 0 when muted.
static f64 vol_heard(const EditRecord &e, i32 track, f64 t) { return e.muted[track] ? 0 : vol_at(e, track, t); }

// Whether a track's volume is anything but as recorded.
static bool vol_touched(const EditRecord &e, i32 track)
{
    if (e.muted[track] || e.vol_count[track]) return true;
    return e.boost[track] > 0.005f || e.boost[track] < -0.005f;
}

// Whether the edit changes what's heard.
static bool edit_audio_touched(const EditRecord &e, const MediaInfo &m)
{
    for (i32 t = 0; t < m.track_count && t < MEDIA_MAX_TRACKS; t++)
        if (vol_touched(e, t)) return true;
    return vol_touched(e, EDIT_MASTER);
}

static bool edit_zoomed(const EditRecord &e)
{
    for (i32 k = 0; k < e.key_count; k++)
        if (e.keys[k].z > 1.001) return true;
    return false;
}

// The zoom at `t` from keys k[0..n) (sorted by t): eased between them, the same curve the render uses; none is no
// zoom.
static void zoom_keys_at(const ZoomKey *k, i32 n, f64 t, f64 *z, f64 *x, f64 *y)
{
    *z = 1;
    *x = *y = 0.5;
    if (!n) return;
    const ZoomKey *a = &k[0], *b = a;
    if (t >= k[n - 1].t) a = b = &k[n - 1];
    else if (t > k[0].t)
        for (i32 i = 0; i + 1 < n; i++)
            if (t < k[i + 1].t) { a = &k[i]; b = &k[i + 1]; break; }
    f64 u = b->t > a->t ? ease_curve(a->ease, b->ease, (t - a->t) / (b->t - a->t)) : 0;
    *z = a->z + (b->z - a->z) * u;
    if (*z < 1) *z = 1;
    *x = a->x + (b->x - a->x) * u;
    *y = a->y + (b->y - a->y) * u;
}

// ---- Advanced edits (EditItem in app.h) ----
// `start` is on the sequence; from, to and the keys are in the video, so an item at sequence time u shows the video
// at from + (u - start).

static f64 item_end(const EditItem &i) { return i.start + (i.to - i.from); }

// The items on one track by start, as indexes into e.items.
static i32 track_items(const EditRecord &e, i32 track, i32 *out)
{
    i32 n = 0;
    for (i32 i = 0; i < e.item_count; i++) {
        if (e.items[i].track != track) continue;
        i32 at = n++;
        while (at > 0 && e.items[out[at - 1]].start > e.items[i].start) {
            out[at] = out[at - 1];
            at--;
        }
        out[at] = i;
    }
    return n;
}

// The sequence's length: to the end of its last item.
static f64 adv_len(const EditRecord &e)
{
    f64 len = 0;
    for (i32 i = 0; i < e.item_count; i++) len = ImMax(len, item_end(e.items[i]));
    return len;
}

static i32 item_find(const EditRecord &e, u16 id)
{
    for (i32 i = 0; i < e.item_count; i++)
        if (e.items[i].id == id) return i;
    return -1;
}

static u16 item_new_id(const EditRecord &e)
{
    u16 id = 1;
    for (i32 i = 0; i < e.item_count; i++)
        if (e.items[i].id >= id) id = (u16)(e.items[i].id + 1);
    return id;
}

// An item's keys of one kind: where they start in e.ikeys, and how many.
static i32 ikey_range(const EditRecord &e, u16 id, i32 what, i32 *first)
{
    i32 n = 0;
    *first = 0;
    for (i32 k = 0; k < e.ikey_count; k++)
        if (e.ikeys[k].item == id && e.ikeys[k].what == what) {
            if (!n) *first = k;
            n++;
        }
    return n;
}

// The same keys as levels or as zoom keys, for the curves and filters that take those.
static i32 item_levels(const EditRecord &e, u16 id, i32 what, VolKey *out)
{
    i32 first, n = ikey_range(e, id, what, &first);
    for (i32 k = 0; k < n; k++) {
        const ItemKey &q = e.ikeys[first + k];
        out[k] = {q.t, (f32)q.v, q.ease};
    }
    return n;
}

static i32 item_zooms(const EditRecord &e, u16 id, ZoomKey *out)
{
    i32 first, n = ikey_range(e, id, IKEY_ZOOM, &first);
    for (i32 k = 0; k < n; k++) {
        const ItemKey &q = e.ikeys[first + k];
        out[k] = {q.t, q.v, q.x, q.y, q.ease};
    }
    return n;
}

// An item's level (a colour or the volume, 1 when it has no keys) or zoom at source time t.
static f64 item_level_at(const EditRecord &e, u16 id, i32 what, f64 t)
{
    VolKey k[EDIT_MAX_ITEM_KEYS];
    return level_at(k, item_levels(e, id, what, k), 1, t);
}

static void item_zoom_at(const EditRecord &e, u16 id, f64 t, f64 *z, f64 *x, f64 *y)
{
    ZoomKey k[EDIT_MAX_ITEM_KEYS];
    zoom_keys_at(k, item_zooms(e, id, k), t, z, x, y);
}

static bool ikey_before(const ItemKey &a, const ItemKey &b)
{
    if (a.item != b.item) return a.item < b.item;
    if (a.what != b.what) return a.what < b.what;
    return a.t < b.t;
}

// Adds a key in order (after any at the same time); false when there's no room.
static bool ikey_add(EditRecord *e, const ItemKey &k)
{
    if (e->ikey_count >= EDIT_MAX_ITEM_KEYS) return false;
    i32 at = e->ikey_count;
    while (at > 0 && ikey_before(k, e->ikeys[at - 1])) at--;
    memmove(e->ikeys + at + 1, e->ikeys + at, (e->ikey_count - at) * sizeof k);
    e->ikeys[at] = k;
    e->ikey_count++;
    return true;
}

// Removes item i and its keys.
static void item_remove(EditRecord *e, i32 i)
{
    u16 id = e->items[i].id;
    memmove(e->items + i, e->items + i + 1, (e->item_count - i - 1) * sizeof *e->items);
    e->item_count--;
    e->items[e->item_count] = {}; // the slots past the end stay clear, so the same edit hashes the same (share_hash)
    i32 n = 0;
    for (i32 k = 0; k < e->ikey_count; k++)
        if (e->ikeys[k].item != id) e->ikeys[n++] = e->ikeys[k];
    memset(e->ikeys + n, 0, (e->ikey_count - n) * sizeof *e->ikeys);
    e->ikey_count = n;
}

// Keeps an edit playable after a change: items held inside the video and at or after the sequence's start, an item
// that runs over the start of the next one on its track cut back to it (dropped if that leaves under PIECE_LEAST),
// and no keys left for items that are gone.
static void adv_settle(EditRecord *e, f64 dur)
{
    for (i32 i = e->item_count - 1; i >= 0; i--) {
        EditItem &it = e->items[i];
        if (it.from < 0) it.from = 0;
        if (dur > 0 && it.to > dur) it.to = dur;
        if (it.start < 0) it.start = 0;
        if (it.to - it.from < PIECE_LEAST - 0.0005) item_remove(e, i);
    }
    for (i32 tr = 0; tr <= EDIT_CAM_TRACK; tr++) {
        i32 idx[EDIT_MAX_ITEMS], n = track_items(*e, tr, idx);
        u16 gone[EDIT_MAX_ITEMS];
        i32 ng = 0;
        for (i32 k = 0; k + 1 < n; k++) {
            EditItem &a = e->items[idx[k]];
            const EditItem &b = e->items[idx[k + 1]];
            if (item_end(a) <= b.start + 0.0005) continue;
            a.to = a.from + (b.start - a.start);
            if (a.to - a.from < PIECE_LEAST - 0.0005) gone[ng++] = a.id;
        }
        for (i32 g = 0; g < ng; g++) item_remove(e, item_find(*e, gone[g]));
    }
    i32 n = 0;
    for (i32 k = 0; k < e->ikey_count; k++)
        if (item_find(*e, e->ikeys[k].item) >= 0) e->ikeys[n++] = e->ikeys[k];
    e->ikey_count = n;
}

// An item's transitions as they play: each fade held to half the item; a crossfade (which plays half its length
// past the cut on each side) held to both items and to the footage there, and only where the item before ends
// where this one starts. It takes the place of the fades at its cut. Anything under TRANS_LEAST is none. The
// webcam's items only cut.
struct ItemFades {
    f64 in, out, cross; // cross: from the item before
};

static void track_fades(const EditRecord &e, f64 dur, const i32 *idx, i32 n, ItemFades *out)
{
    for (i32 k = 0; k < n; k++) {
        const EditItem &it = e.items[idx[k]];
        f64 len = it.to - it.from;
        out[k].in = ImMin((f64)it.fade_in, len / 2);
        out[k].out = ImMin((f64)it.fade_out, len / 2);
        out[k].cross = 0;
        if (k && it.cross > 0 && it.track != EDIT_CAM_TRACK) {
            const EditItem &p = e.items[idx[k - 1]];
            if (fabs(item_end(p) - it.start) < 0.001) {
                f64 l = ImMin((f64)it.cross, ImMin(p.to - p.from, len));
                l = ImMin(l, ImMin(2 * (dur - p.to), 2 * it.from));
                out[k].cross = l >= TRANS_LEAST ? l : 0;
            }
        }
        if (out[k].in < TRANS_LEAST) out[k].in = 0;
        if (out[k].out < TRANS_LEAST) out[k].out = 0;
    }
    for (i32 k = 1; k < n; k++)
        if (out[k].cross > 0) out[k].in = out[k - 1].out = 0;
}

// How much of item i is left by its fades at source time s (1 = all, 0 = black or silent).
static f64 item_fade_gain(const EditRecord &e, f64 dur, i32 i, f64 s)
{
    if (i < 0 || i >= e.item_count) return 1;
    i32 idx[EDIT_MAX_ITEMS], n = track_items(e, e.items[i].track, idx);
    ItemFades f[EDIT_MAX_ITEMS];
    track_fades(e, dur, idx, n, f);
    for (i32 k = 0; k < n; k++) {
        if (idx[k] != i) continue;
        const EditItem &it = e.items[i];
        f64 g = 1;
        if (f[k].in > 0 && s < it.from + f[k].in) g = ImMax(0.0, (s - it.from) / f[k].in);
        if (f[k].out > 0 && s > it.to - f[k].out) g = ImMin(g, ImMax(0.0, (it.to - s) / f[k].out));
        return g;
    }
    return 1;
}

// The item on a track at sequence time u, -1 for none (a gap). Where one item ends and the next starts, the next.
static i32 item_at(const EditRecord &e, i32 track, f64 u)
{
    i32 best = -1;
    for (i32 i = 0; i < e.item_count; i++) {
        const EditItem &it = e.items[i];
        if (it.track == track && u >= it.start - 0.0005 && u < item_end(it) - 0.0005 &&
            (best < 0 || it.start > e.items[best].start))
            best = i;
    }
    return best;
}

// The webcam over the picture. A cam item's width is a part of the picture's width; its height follows from the
// cam's own shape (cw x ch) and is never more than the picture's; the whole cam stays inside the picture.
static const f32 CAM_MIN_W = 0.08f, CAM_DEFAULT_W = 0.25f, CAM_MARGIN = 0.02f;
enum { CAM_TOP_LEFT, CAM_TOP_RIGHT, CAM_BOTTOM_LEFT, CAM_BOTTOM_RIGHT, CAM_CORNERS };

// A cam's size as parts of a fw x fh picture, from the width asked for.
static void cam_size(f64 want_w, f64 fw, f64 fh, i32 cw, i32 ch, f64 *w, f64 *h)
{
    f64 pa = fw > 0 && fh > 0 ? fw / fh : 16.0 / 9, ca = cw > 0 && ch > 0 ? (f64)cw / ch : 16.0 / 9;
    *w = ImClamp(want_w, (f64)CAM_MIN_W, 1.0);
    *h = *w * pa / ca;
    if (*h > 1) *h = 1, *w = ca / pa;
}

// Puts a cam item's centre at (x, y) and its width at w, held so the whole cam is inside the picture.
static void cam_place(EditItem *it, f64 x, f64 y, f64 w, i32 fw, i32 fh, i32 cw, i32 ch)
{
    f64 sw, sh;
    cam_size(w, fw, fh, cw, ch, &sw, &sh);
    it->cam_w = (f32)sw;
    it->cam_x = (f32)ImClamp(x, sw / 2, 1 - sw / 2);
    it->cam_y = (f32)ImClamp(y, sh / 2, 1 - sh / 2);
}

// Where a cam item puts the cam in corner c (CAM_*), CAM_MARGIN of the picture's width in from both edges.
static void cam_corner_at(const EditItem &it, i32 c, i32 fw, i32 fh, i32 cw, i32 ch, f64 *x, f64 *y)
{
    f64 sw, sh, mx = CAM_MARGIN, my = fh > 0 ? CAM_MARGIN * fw / fh : CAM_MARGIN;
    cam_size(it.cam_w, fw, fh, cw, ch, &sw, &sh);
    *x = c & 1 ? 1 - mx - sw / 2 : mx + sw / 2;
    *y = c & 2 ? 1 - my - sh / 2 : my + sh / 2;
}

static void cam_corner(EditItem *it, i32 c, i32 fw, i32 fh, i32 cw, i32 ch)
{
    f64 x, y;
    cam_corner_at(*it, c, fw, fh, cw, ch, &x, &y);
    cam_place(it, x, y, it->cam_w, fw, fh, cw, ch);
}

// The corner a cam item is in (as cam_corner puts it), -1 for none.
static i32 cam_corner_of(const EditItem &it, i32 fw, i32 fh, i32 cw, i32 ch)
{
    for (i32 c = 0; c < CAM_CORNERS; c++) {
        f64 x, y;
        cam_corner_at(it, c, fw, fh, cw, ch, &x, &y);
        if (fabs(it.cam_x - x) < 0.002 && fabs(it.cam_y - y) < 0.002) return c;
    }
    return -1;
}

// A cam item's place in a fw x fh picture, in pixels: even sizes for the encoder, inside the picture.
struct CamRect {
    i32 x, y, w, h;
};

static CamRect cam_rect(const EditItem &it, i32 fw, i32 fh, i32 cw, i32 ch)
{
    CamRect r = {};
    if (fw < 2 || fh < 2) return r;
    f64 sw, sh;
    cam_size(it.cam_w, fw, fh, cw, ch, &sw, &sh);
    r.w = ImMin(ImMax(2, (i32)(sw * fw + 0.5) & ~1), fw & ~1);
    r.h = ImMin(ImMax(2, (i32)(sh * fh + 0.5) & ~1), fh & ~1);
    r.x = ImClamp((i32)floor(it.cam_x * fw - r.w / 2.0 + 0.5), 0, fw - r.w);
    r.y = ImClamp((i32)floor(it.cam_y * fh - r.h / 2.0 + 0.5), 0, fh - r.h);
    return r;
}

// Keys for an item over [a, b] of the video from a curve with keys at kt[0..n) (eases ke) and values val(t, v, x,
// y): the keys inside the span, and one at each end with the curve's value there (its ease the one of the stretch it
// cuts into). A curve flat over the span is one key at a, or none when that's `rest` (as recorded).
template <typename F>
static void span_keys(EditRecord *e, u16 id, u8 what, f64 a, f64 b, const f64 *kt, const u8 *ke, i32 n, f64 rest,
                      F val)
{
    i32 lo = 0, hi; // the keys inside: [lo, hi)
    while (lo < n && kt[lo] < a - 0.0005) lo++;
    hi = lo;
    while (hi < n && kt[hi] <= b + 0.0005) hi++;
    ItemKey k = {};
    k.item = id;
    k.what = what;
    if (lo == hi) {
        f64 va, xa, ya, vb, xb, yb;
        val(a, &va, &xa, &ya);
        val(b, &vb, &xb, &yb);
        if (fabs(va - vb) < 1e-4 && fabs(xa - xb) < 1e-4 && fabs(ya - yb) < 1e-4) {
            if (fabs(va - rest) < 1e-4) return;
            k.t = a;
            k.v = va, k.x = xa, k.y = ya;
            k.ease = EASE_LINEAR;
            ikey_add(e, k);
            return;
        }
    }
    if (lo == hi || kt[lo] > a + 0.0005) {
        k.t = a;
        val(a, &k.v, &k.x, &k.y);
        k.ease = lo > 0 ? ke[lo - 1] : (u8)EASE_LINEAR;
        ikey_add(e, k);
    }
    for (i32 i = lo; i < hi; i++) {
        k.t = kt[i];
        val(kt[i], &k.v, &k.x, &k.y);
        k.ease = ke[i];
        ikey_add(e, k);
    }
    if (lo == hi || kt[hi - 1] < b - 0.0005) {
        k.t = b;
        val(b, &k.v, &k.x, &k.y);
        k.ease = hi < n ? ke[hi] : (u8)EASE_LINEAR;
        ikey_add(e, k);
    }
}

// Simple to Advanced: each piece becomes a linked set of items, the picture, every audio track and the webcam (in the
// bottom right corner, with the picture's fades), end to end. The keys are split at the cuts, and each track's
// volume times the whole mix's goes into its items' volume keys (held to 0..2); the whole mix muted mutes every
// track.
static void edit_to_advanced(EditRecord *e, const MediaInfo &m)
{
    if (e->mode == EDIT_ADVANCED) return;
    static EditRecord a;
    a = {};
    a.mode = EDIT_ADVANCED;
    f64 dur = m.duration;
    EditPiece pc[EDIT_MAX_PIECES];
    i32 np = pieces_of(*e, dur, pc);
    Transitions tr = transitions_of(*e, pc, np, dur);
    i32 first = edit_has_mix(m) ? 1 : 0;
    f64 kt[EDIT_MAX_KEYS + 2 * EDIT_MAX_VOL_KEYS];
    u8 ke[EDIT_MAX_KEYS + 2 * EDIT_MAX_VOL_KEYS];
    f64 at = 0;
    u16 id = 1;
    for (i32 i = 0; i < np; i++) {
        f64 fin, fout;
        piece_fades(tr, np, i, &fin, &fout);
        for (i32 t = -1; t < m.track_count && t < MEDIA_MAX_TRACKS && a.item_count < EDIT_MAX_ITEMS; t++) {
            if (t >= 0 && t < first) continue;
            EditItem it = {};
            it.start = at;
            it.from = pc[i].from;
            it.to = pc[i].to;
            it.fade_in = (f32)fin;
            it.fade_out = (f32)fout;
            it.cross = tr.into[i] == TRANS_CROSS ? (f32)tr.len[i] : 0;
            it.id = id++;
            it.link = (u16)(i + 1);
            it.track = (u8)(t + 1);
            a.items[a.item_count++] = it;
            if (t < 0) {
                for (i32 k = 0; k < e->key_count; k++) kt[k] = e->keys[k].t, ke[k] = e->keys[k].ease;
                span_keys(&a, it.id, IKEY_ZOOM, it.from, it.to, kt, ke, e->key_count, 1,
                          [&](f64 s, f64 *v, f64 *x, f64 *y) { zoom_keys_at(e->keys, e->key_count, s, v, x, y); });
                for (i32 c = 0; c < EDIT_COLORS; c++) {
                    const VolKey *ck = e->color[c];
                    i32 nc = e->color_count[c];
                    for (i32 k = 0; k < nc; k++) kt[k] = ck[k].t, ke[k] = ck[k].ease;
                    span_keys(&a, it.id, (u8)(IKEY_COLOR + c), it.from, it.to, kt, ke, nc, 1,
                              [&](f64 s, f64 *v, f64 *x, f64 *y) { *v = level_at(ck, nc, 1, s), *x = *y = 0; });
                }
            } else {
                // The track's keys and the mix's, merged in time; at the same time, the track's ease
                const VolKey *tk = e->vol[t], *mk = e->vol[EDIT_MASTER];
                i32 nt = e->vol_count[t], nm = e->vol_count[EDIT_MASTER], n = 0, p = 0, q = 0;
                while (p < nt || q < nm) {
                    bool take_t = q >= nm || (p < nt && tk[p].t <= mk[q].t + 0.0005);
                    const VolKey &k = take_t ? tk[p] : mk[q];
                    if (take_t && q < nm && fabs(tk[p].t - mk[q].t) <= 0.0005) q++;
                    if (take_t) p++;
                    else q++;
                    kt[n] = k.t, ke[n] = k.ease, n++;
                }
                span_keys(&a, it.id, IKEY_VOLUME, it.from, it.to, kt, ke, n, 1, [&](f64 s, f64 *v, f64 *x, f64 *y) {
                    f64 g = vol_at(*e, t, s) * vol_at(*e, EDIT_MASTER, s);
                    *v = g > 2 ? 2 : g, *x = *y = 0;
                });
            }
        }
        if (m.cam_w > 0 && a.item_count < EDIT_MAX_ITEMS) {
            EditItem it = {};
            it.start = at;
            it.from = pc[i].from;
            it.to = pc[i].to;
            it.fade_in = (f32)fin;
            it.fade_out = (f32)fout;
            it.id = id++;
            it.link = (u16)(i + 1);
            it.track = EDIT_CAM_TRACK;
            it.cam_w = CAM_DEFAULT_W;
            cam_corner(&it, CAM_BOTTOM_RIGHT, m.width, m.height, m.cam_w, m.cam_h);
            a.items[a.item_count++] = it;
        }
        at += pc[i].to - pc[i].from;
    }
    for (i32 t = 0; t < MEDIA_MAX_TRACKS; t++) a.muted[t] = e->muted[t] || e->muted[EDIT_MASTER];
    memcpy(a.layer_on, e->layer_on, sizeof a.layer_on);
    a.layer_on_count = e->layer_on_count;
    *e = a;
}

// Advanced to Simple, as near as Simple can say it: the picture's items become the pieces in their order (gaps
// closed up), their crossfades and fades the transitions at the cuts, and each kind of key the keys of all the
// items on its track, merged in time. Anything else is lost.
static void edit_to_simple(EditRecord *e, const MediaInfo &m)
{
    if (e->mode != EDIT_ADVANCED) return;
    static EditRecord s;
    s = {};
    i32 idx[EDIT_MAX_ITEMS], n = track_items(*e, 0, idx);
    EditPiece p[EDIT_MAX_PIECES];
    i32 np = 0;
    for (i32 k = 0; k < n && np < EDIT_MAX_PIECES; k++) {
        const EditItem &it = e->items[idx[k]];
        EditPiece q = {it.from, it.to};
        if (k) {
            const EditItem &prev = e->items[idx[k - 1]];
            if (it.cross > 0) q.trans = TRANS_CROSS, q.trans_len = it.cross;
            else if (prev.fade_out > 0 || it.fade_in > 0) q.trans = TRANS_FADE, q.trans_len = prev.fade_out + it.fade_in;
        }
        p[np++] = q;
    }
    if (np) {
        s.fade_in = e->items[idx[0]].fade_in;
        s.fade_out = e->items[idx[n - 1]].fade_out;
        pieces_store(&s, p, np, m.duration);
    }
    // Every key of one kind on the items of a track, by time, one per moment
    static ItemKey all[EDIT_MAX_ITEM_KEYS];
    auto merged = [&](i32 track, i32 what, auto put) {
        i32 ids[EDIT_MAX_ITEMS], ni = track_items(*e, track, ids), na = 0;
        for (i32 i = 0; i < ni; i++) {
            i32 first, c = ikey_range(*e, e->items[ids[i]].id, what, &first);
            for (i32 k = 0; k < c; k++) {
                const ItemKey &q = e->ikeys[first + k];
                i32 at = na++;
                while (at > 0 && all[at - 1].t > q.t) {
                    all[at] = all[at - 1];
                    at--;
                }
                all[at] = q;
            }
        }
        for (i32 k = 0; k < na; k++)
            if (!k || all[k].t - all[k - 1].t > 0.0005) put(all[k]);
    };
    merged(0, IKEY_ZOOM, [&](const ItemKey &q) {
        if (s.key_count < EDIT_MAX_KEYS) s.keys[s.key_count++] = {q.t, q.v, q.x, q.y, q.ease};
    });
    for (i32 c = 0; c < EDIT_COLORS; c++)
        merged(0, IKEY_COLOR + c, [&](const ItemKey &q) {
            if (s.color_count[c] < EDIT_MAX_VOL_KEYS) s.color[c][s.color_count[c]++] = {q.t, (f32)q.v, q.ease};
        });
    for (i32 t = 0; t < m.track_count && t < MEDIA_MAX_TRACKS; t++) {
        merged(t + 1, IKEY_VOLUME, [&](const ItemKey &q) {
            if (s.vol_count[t] < EDIT_MAX_VOL_KEYS) s.vol[t][s.vol_count[t]++] = {q.t, (f32)q.v, q.ease};
        });
        s.muted[t] = e->muted[t];
    }
    memcpy(s.layer_on, e->layer_on, sizeof s.layer_on);
    s.layer_on_count = e->layer_on_count;
    *e = s;
}

// Whether two Advanced edits play the same: the same items on each track (place, span and transitions as they
// play), linked the same way, the webcam in the same place, and the same zoom, colour and volume through each.
static bool adv_same(const EditRecord &a, const EditRecord &b, const MediaInfo &m)
{
    if (a.item_count != b.item_count) return false;
    f64 dur = m.duration;
    static u16 ab[65536]; // a's link to b's
    memset(ab, 0, sizeof ab);
    for (i32 t = 0; t < MEDIA_MAX_TRACKS; t++)
        if (a.muted[t] != b.muted[t]) return false;
    for (i32 tr = 0; tr <= EDIT_CAM_TRACK; tr++) {
        i32 ia[EDIT_MAX_ITEMS], ib[EDIT_MAX_ITEMS];
        i32 n = track_items(a, tr, ia);
        if (track_items(b, tr, ib) != n) return false;
        ItemFades fa[EDIT_MAX_ITEMS], fb[EDIT_MAX_ITEMS];
        track_fades(a, dur, ia, n, fa);
        track_fades(b, dur, ib, n, fb);
        for (i32 k = 0; k < n; k++) {
            const EditItem &x = a.items[ia[k]], &y = b.items[ib[k]];
            if (fabs(x.start - y.start) > 0.001 || fabs(x.from - y.from) > 0.001 || fabs(x.to - y.to) > 0.001) return false;
            if (fabs(fa[k].in - fb[k].in) > 0.001 || fabs(fa[k].out - fb[k].out) > 0.001 ||
                fabs(fa[k].cross - fb[k].cross) > 0.001)
                return false;
            if (!x.link != !y.link) return false;
            if (x.link) {
                if (!ab[x.link]) ab[x.link] = y.link;
                else if (ab[x.link] != y.link) return false;
            }
            if (tr == EDIT_CAM_TRACK) {
                if (fabs(x.cam_x - y.cam_x) > 0.001 || fabs(x.cam_y - y.cam_y) > 0.001 || fabs(x.cam_w - y.cam_w) > 0.001)
                    return false;
                continue;
            }
            // The curves, at both edits' keys and at even steps through the item
            for (i32 what = 0; what < IKEY_KINDS; what++) {
                if ((what == IKEY_VOLUME) != (tr > 0)) continue;
                static f64 at[2 * EDIT_MAX_ITEM_KEYS + 17];
                i32 na = 0;
                for (i32 s = 0; s <= 16; s++) at[na++] = x.from + (x.to - x.from) * s / 16;
                for (i32 side = 0; side < 2; side++) {
                    const EditRecord &r = side ? b : a;
                    i32 first, c = ikey_range(r, side ? y.id : x.id, what, &first);
                    for (i32 q = 0; q < c; q++)
                        if (r.ikeys[first + q].t > x.from && r.ikeys[first + q].t < x.to) at[na++] = r.ikeys[first + q].t;
                }
                for (i32 s = 0; s < na; s++) {
                    if (what == IKEY_ZOOM) {
                        f64 z1, x1, y1, z2, x2, y2;
                        item_zoom_at(a, x.id, at[s], &z1, &x1, &y1);
                        item_zoom_at(b, y.id, at[s], &z2, &x2, &y2);
                        if (fabs(z1 - z2) > 0.002) return false;
                        if (z1 > 1.001 && (fabs(x1 - x2) > 0.002 || fabs(y1 - y2) > 0.002)) return false;
                    } else if (fabs(item_level_at(a, x.id, what, at[s]) - item_level_at(b, y.id, what, at[s])) > 0.002)
                        return false;
                }
            }
        }
    }
    return true;
}

// Whether going back to Simple keeps an Advanced edit as it is: to Simple and back again plays the same.
static bool edit_round_trips(const EditRecord &e, const MediaInfo &m)
{
    if (e.mode != EDIT_ADVANCED) return true;
    static EditRecord c;
    c = e;
    edit_to_simple(&c, m);
    edit_to_advanced(&c, m);
    return adv_same(e, c, m);
}

// One of a keyframe's values (0 z, 1 x, 2 y) of keys[0..n) at time `t` (an ffmpeg expression), eased between keys.
static void smooth_expr(SaveWork *w, const ZoomKey *keys, i32 n, i32 field, const char *t)
{
    auto val = [&](i32 k) { return field == 0 ? keys[k].z : field == 1 ? keys[k].x : keys[k].y; };
    for (i32 k = 0; k + 1 < n; k++) {
        f64 ta = keys[k].t, tb = keys[k + 1].t, a = val(k), b = val(k + 1);
        f64 span = tb - ta > 0.001 ? tb - ta : 0.001;
        char u[160], c[560];
        snprintf(u, sizeof u, "clip((%s-%.4f)/%.4f,0,1)", t, ta, span);
        if (ease_expr(keys[k].ease, keys[k + 1].ease, u, c, sizeof c))
            graph_add(w, "if(lt(%s,%.4f),(%.5f+(%.5f)*(%s)),", t, tb, a, b - a, c);
        else graph_add(w, "if(lt(%s,%.4f),%.5f,", t, tb, a);
    }
    graph_add(w, "%.5f", val(n - 1));
    for (i32 k = 0; k + 1 < n; k++) graph_add(w, ")");
}

// A volume filter for a track (or the mix at EDIT_MASTER) cut from `start` seconds: a plain level, or a ramp
// between its keys evaluated every frame (ffmpeg's t starts at 0 at the cut).
// A level's keys (n > 0) as an expression of the time `t`, each value v written as v * mul + add.
static void level_expr(SaveWork *w, const VolKey *k, i32 n, const char *t, f64 mul, f64 add)
{
    graph_add(w, "if(lt(%s,%.4f),%.4f,", t, k[0].t, k[0].v * mul + add);
    for (i32 i = 1; i < n; i++) {
        f64 span = k[i].t - k[i - 1].t, a = k[i - 1].v * mul + add, b = k[i].v * mul + add;
        char u[128], c[480];
        snprintf(u, sizeof u, "clip((%s-%.4f)/%.4f,0,1)", t, k[i - 1].t, span > 0.001 ? span : 0.001);
        if (span < 0.001 || !ease_expr(k[i - 1].ease, k[i].ease, u, c, sizeof c))
            graph_add(w, "if(lt(%s,%.4f),%.4f,", t, k[i].t, span < 0.001 ? b : a);
        else graph_add(w, "if(lt(%s,%.4f),%.4f+(%.4f)*(%s),", t, k[i].t, a, b - a, c);
    }
    graph_add(w, "%.4f", k[n - 1].v * mul + add);
    for (i32 i = 0; i < n; i++) graph_add(w, ")");
}

static void vol_filter(SaveWork *w, const EditRecord &e, i32 track, f64 start)
{
    i32 n = e.vol_count[track];
    if (!n) {
        graph_add(w, "volume=%.4f", 1.0 + e.boost[track]);
        return;
    }
    char t[48];
    snprintf(t, sizeof t, "(t+%.4f)", start);
    graph_add(w, "volume='");
    level_expr(w, e.vol[track], n, t, 1, 0);
    graph_add(w, "':eval=frame");
}

// The colour as an eq filter, each value (keys k[c][0..n[c]), none = as recorded) a plain number or an expression of
// the frame's time, for input cut from `start` seconds. eq works on planar YUV, so the picture is put in that first.
static void color_filter(SaveWork *w, const VolKey *const *k, const i32 *n, f64 fps, f64 start)
{
    static const char *const names[EDIT_COLORS] = {"brightness", "contrast", "saturation"};
    char t[64];
    snprintf(t, sizeof t, "(n/%.4f+%.4f)", fps > 1 ? fps : 30, start);
    graph_add(w, "format=yuv420p,eq=");
    for (i32 c = 0; c < EDIT_COLORS; c++) {
        graph_add(w, "%s%s=", c ? ":" : "", names[c]);
        if (!n[c]) {
            graph_add(w, "%.4f", color_eq(c, 1));
            continue;
        }
        graph_add(w, "'");
        if (c == COLOR_BRIGHTNESS) level_expr(w, k[c], n[c], t, EQ_BRIGHTNESS, -EQ_BRIGHTNESS);
        else level_expr(w, k[c], n[c], t, 1, 0);
        graph_add(w, "'");
    }
    graph_add(w, ":eval=frame,");
}

// Joins the pieces cut out of the input: the video's [v<i>] into [v] (stream -1), or audio stream s's [e<s>_<i>]
// into [o<s>]. A crossfade overlaps the two pieces by its length (xfade, acrossfade); anything else follows on.
static void seq_join(SaveWork *w, const Transitions &tr, const f64 *sa, const f64 *sb, i32 np, i32 s)
{
    bool audio = s >= 0;
    auto piece = [&](i32 i) {
        if (audio) graph_add(w, "[e%d_%d]", s, i);
        else graph_add(w, "[v%d]", i);
    };
    auto result = [&]() {
        if (audio) graph_add(w, "[o%d];\n", s);
        else graph_add(w, "[v];\n");
    };
    bool cross = false;
    for (i32 i = 1; i < np; i++) cross |= tr.into[i] == TRANS_CROSS;
    if (!cross) {
        for (i32 i = 0; i < np; i++) piece(i);
        if (np == 1) graph_add(w, audio ? "anull" : "null");
        else graph_add(w, "concat=n=%d:v=%d:a=%d", np, audio ? 0 : 1, audio ? 1 : 0);
        result();
        return;
    }
    f64 len = sb[0] - sa[0]; // the joined length so far
    for (i32 i = 1; i < np; i++) {
        if (i == 1) piece(0);
        else if (audio) graph_add(w, "[ja%d_%d]", s, i - 1);
        else graph_add(w, "[jv%d]", i - 1);
        piece(i);
        f64 l = tr.len[i];
        if (tr.into[i] == TRANS_CROSS) {
            if (audio) graph_add(w, "acrossfade=d=%.4f:c1=tri:c2=tri", l);
            else graph_add(w, "xfade=transition=fade:duration=%.4f:offset=%.4f", l, len - l);
            len += sb[i] - sa[i] - l;
        } else {
            graph_add(w, "concat=n=2:v=%d:a=%d", audio ? 0 : 1, audio ? 1 : 0);
            len += sb[i] - sa[i];
        }
        if (i == np - 1) result();
        else if (audio) graph_add(w, "[ja%d_%d];\n", s, i);
        else graph_add(w, "[jv%d];\n", i);
    }
}

// The zoom (keys[0..n)) as a perspective filter that crops the view and stretches it to the whole frame, each frame,
// for input cut from `start` seconds.
static void zoom_filter(SaveWork *w, const ZoomKey *keys, i32 n, f64 fps, f64 start)
{
    char t[64];
    snprintf(t, sizeof t, "(in/%.4f+%.4f)", fps > 1 ? fps : 30, start);
    // The view's top-left corner, kept inside the picture: x = clip(cx*W - W/(2z), 0, W - W/z).
    auto z = [&]() { graph_add(w, "max(1,"); smooth_expr(w, keys, n, 0, t); graph_add(w, ")"); };
    auto corner = [&](const char *dim, i32 field, bool far_edge) {
        graph_add(w, "clip(");
        smooth_expr(w, keys, n, field, t);
        graph_add(w, "*%s-%s/(2*", dim, dim);
        z();
        graph_add(w, "),0,%s-%s/", dim, dim);
        z();
        graph_add(w, ")");
        if (far_edge) { graph_add(w, "+%s/", dim); z(); }
    };
    graph_add(w, "perspective=");
    for (i32 c = 0; c < 4; c++) {
        bool right = c & 1, down = c & 2;
        graph_add(w, "%sx%d='", c ? ":" : "", c);
        corner("W", 1, right);
        graph_add(w, "':y%d='", c);
        corner("H", 2, down);
        graph_add(w, "'");
    }
    graph_add(w, ":interpolation=linear:eval=frame,");
}

// The audio of a share, or Keep's rebuilt Mix, as [a], for input cut from `start` seconds. Returns false if the
// file has no audio.
static bool edit_audio_graph(SaveWork *w, const EditJob &j, f64 start)
{
    const MediaInfo &m = j.info;
    if (m.track_count <= 0) return false;
    if (!edit_audio_touched(j.edit, m)) {
        graph_add(w, "[0:a:0]anull[a];\n");
        return true;
    }
    i32 first = edit_has_mix(m) ? 1 : 0, n = 0;
    for (i32 t = first; t < m.track_count && t < MEDIA_MAX_TRACKS; t++) {
        if (j.edit.muted[t] || track_is_spare(m, t)) continue;
        graph_add(w, "[0:a:%d]", t);
        vol_filter(w, j.edit, t, start);
        graph_add(w, ",aformat=sample_fmts=fltp:sample_rates=48000:channel_layouts=stereo[s%d];\n", n);
        n++;
    }
    if (!n || j.edit.muted[EDIT_MASTER]) {
        graph_add(w, "anullsrc=r=48000:cl=stereo[a];\n");
        return true;
    }
    for (i32 i = 0; i < n; i++) graph_add(w, "[s%d]", i);
    // The whole mix's volume goes on after the tracks are mixed, before the limiter.
    if (n > 1) graph_add(w, "amix=inputs=%d:normalize=0:dropout_transition=0,", n);
    else graph_add(w, "anull,");
    vol_filter(w, j.edit, EDIT_MASTER, start);
    graph_add(w, ",alimiter=limit=0.98:level=0[a];\n");
    return true;
}

// An Advanced edit's graph, for input cut from `from` seconds. Each track's items are cut out of the input (with half
// of any crossfade past each end), zoomed and coloured or set to their volume, faded, and joined with black or
// silence between them and up to the sequence's end. The picture then goes through `post` (filters, each ending in a
// comma) into [v], with the webcam's items laid over it first when the file has a webcam (cam_graph). The sound is
// written as [o<s>]: the mix of the unmuted tracks first, then for Keep (`tracks`)
// every track as edited; streams[s] is -1 for the mix, else the file's track. Returns how many.
// The webcam's items over the picture `v` (a label, changed to the result's): each cut from the file's webcam stream
// read from `from`, sized and placed as the item says, faded by its alpha, and laid over from its start until it
// ends. Nothing without a webcam.
static void cam_graph(SaveWork *w, const EditRecord &e, const MediaInfo &m, f64 from, f64 fps, char (&v)[24])
{
    i32 idx[EDIT_MAX_ITEMS], n = m.cam_w > 0 ? track_items(e, EDIT_CAM_TRACK, idx) : 0;
    if (!n) return;
    ItemFades fd[EDIT_MAX_ITEMS];
    track_fades(e, m.duration, idx, n, fd);
    graph_add(w, "[0:v:1]split=%d", n);
    for (i32 k = 0; k < n; k++) graph_add(w, "[cm%d]", k);
    graph_add(w, ";\n");
    for (i32 k = 0; k < n; k++) {
        const EditItem &it = e.items[idx[k]];
        CamRect r = cam_rect(it, m.width, m.height, m.cam_w, m.cam_h);
        graph_add(w, "[cm%d]trim=start=%.4f:end=%.4f,setpts=PTS-STARTPTS,fps=%.4f,scale=%d:%d,setsar=1,format=yuva420p", k,
                  it.from - from, it.to - from, fps, r.w, r.h);
        if (fd[k].in > 0) graph_add(w, ",fade=t=in:st=0:d=%.4f:alpha=1", fd[k].in);
        if (fd[k].out > 0) graph_add(w, ",fade=t=out:st=%.4f:d=%.4f:alpha=1", it.to - it.from - fd[k].out, fd[k].out);
        graph_add(w, ",settb=AVTB,setpts=PTS+%.4f/TB[cq%d];\n", it.start, k);
        graph_add(w, "%s[cq%d]overlay=x=%d:y=%d:eof_action=pass:repeatlast=0[cv%d];\n", v, k, r.x, r.y, k);
        snprintf(v, sizeof v, "[cv%d]", k);
    }
}

static i32 adv_graph(SaveWork *w, const EditJob &j, f64 from, const char *post, const char *vfmt, bool tracks,
                     i32 *streams)
{
    const EditRecord &e = j.edit;
    const MediaInfo &m = j.info;
    f64 dur = m.duration, len = adv_len(e), fps = m.fps > 1 ? m.fps : 30;
    static const char *const AFMT = "aformat=sample_fmts=fltp:sample_rates=48000:channel_layouts=stereo";
    static VolKey lv[EDIT_MAX_ITEM_KEYS], cks[EDIT_COLORS][EDIT_MAX_ITEM_KEYS];
    static ZoomKey zk[EDIT_MAX_ITEM_KEYS];
    // Track tr (0 the picture) from input `src`, its result's label put in `out`.
    auto track = [&](i32 tr, const char *src, char (&out)[24]) {
        bool audio = tr > 0;
        const char *a = audio ? "a" : "";
        i32 idx[EDIT_MAX_ITEMS], n = track_items(e, tr, idx);
        ItemFades fd[EDIT_MAX_ITEMS];
        track_fades(e, dur, idx, n, fd);
        if (n) {
            graph_add(w, "%s%ssplit=%d", src, a, n);
            for (i32 k = 0; k < n; k++) graph_add(w, "[c%d_%d]", tr, k);
            graph_add(w, ";\n");
        }
        f64 joined = 0;
        i32 joins = 0, gaps = 0;
        out[0] = 0;
        auto join = [&](const char *next, f64 next_len, f64 cross) {
            if (!out[0]) {
                snprintf(out, sizeof out, "%s", next);
                joined = next_len;
                return;
            }
            char lab[24];
            snprintf(lab, sizeof lab, "[j%d_%d]", tr, joins++);
            if (cross > 0 && audio) graph_add(w, "%s%sacrossfade=d=%.4f:c1=tri:c2=tri%s;\n", out, next, cross, lab);
            else if (cross > 0)
                graph_add(w, "%s%sxfade=transition=fade:duration=%.4f:offset=%.4f%s;\n", out, next, cross, joined - cross, lab);
            else graph_add(w, "%s%sconcat=n=2:v=%d:a=%d%s;\n", out, next, audio ? 0 : 1, audio ? 1 : 0, lab);
            joined += next_len - cross;
            snprintf(out, sizeof out, "%s", lab);
        };
        auto gap = [&](f64 d) {
            char g[24];
            snprintf(g, sizeof g, "[g%d_%d]", tr, gaps++);
            if (audio) graph_add(w, "anullsrc=r=48000:cl=stereo,atrim=duration=%.4f,%s%s;\n", d, AFMT, g);
            else
                graph_add(w, "color=c=black:s=%dx%d:r=%.4f:d=%.4f,format=yuv420p,setsar=1,settb=AVTB%s;\n", m.width,
                          m.height, fps, d, g);
            join(g, d, 0);
        };
        for (i32 k = 0; k < n; k++) {
            const EditItem &it = e.items[idx[k]];
            f64 sa = it.from - fd[k].cross / 2, sb = it.to + (k + 1 < n ? fd[k + 1].cross / 2 : 0);
            graph_add(w, "[c%d_%d]%strim=start=%.4f:end=%.4f,%ssetpts=PTS-STARTPTS,", tr, k, a, sa - from, sb - from, a);
            if (!audio) {
                i32 nz = item_zooms(e, it.id, zk);
                bool zoomed = false, colored = false;
                for (i32 q = 0; q < nz; q++) zoomed |= zk[q].z > 1.001;
                if (zoomed) zoom_filter(w, zk, nz, fps, sa);
                const VolKey *ck[EDIT_COLORS];
                i32 nc[EDIT_COLORS];
                for (i32 c = 0; c < EDIT_COLORS; c++) {
                    nc[c] = item_levels(e, it.id, IKEY_COLOR + c, cks[c]);
                    ck[c] = cks[c];
                    for (i32 q = 0; q < nc[c]; q++) colored |= fabs(cks[c][q].v - 1) > 0.001;
                }
                if (colored) color_filter(w, ck, nc, fps, sa);
                graph_add(w, "format=yuv420p,setsar=1,fps=%.4f,settb=AVTB", fps);
            } else {
                i32 nl = item_levels(e, it.id, IKEY_VOLUME, lv);
                if (nl) {
                    char t[48];
                    snprintf(t, sizeof t, "(t+%.4f)", sa);
                    graph_add(w, "volume='");
                    level_expr(w, lv, nl, t, 1, 0);
                    graph_add(w, "':eval=frame,");
                }
                graph_add(w, "%s", AFMT);
            }
            if (fd[k].in > 0) graph_add(w, ",%sfade=t=in:st=0:d=%.4f", a, fd[k].in);
            if (fd[k].out > 0) graph_add(w, ",%sfade=t=out:st=%.4f:d=%.4f", a, sb - sa - fd[k].out, fd[k].out);
            char p[24];
            snprintf(p, sizeof p, "[p%d_%d]", tr, k);
            graph_add(w, "%s;\n", p);
            if (!fd[k].cross && it.start > joined + 0.0005) gap(it.start - joined);
            join(p, sb - sa, fd[k].cross);
        }
        if (len > joined + 0.0005) gap(len - joined);
    };
    char v[24];
    track(0, "[0:v]", v);
    cam_graph(w, e, m, from, fps, v);
    graph_add(w, "%s%sformat=%s[v];\n", v, post, vfmt);
    if (m.track_count <= 0) return 0;
    // The tracks: in the mix when unmuted and with something on them, and each written alone for Keep
    i32 first = edit_has_mix(m) ? 1 : 0, mixed[MEDIA_MAX_TRACKS], nm = 0, ns = 1;
    char lab[MEDIA_MAX_TRACKS][24];
    streams[0] = -1;
    for (i32 t = first; t < m.track_count && t < MEDIA_MAX_TRACKS; t++) {
        i32 idx[EDIT_MAX_ITEMS];
        bool in_mix = !e.muted[t] && !track_is_spare(m, t) && track_items(e, t + 1, idx) > 0;
        if (!in_mix && !tracks) continue;
        char src[24];
        snprintf(src, sizeof src, "[0:a:%d]", t);
        track(t + 1, src, lab[t]);
        if (in_mix && tracks) {
            graph_add(w, "%sasplit=2[o%d][x%d];\n", lab[t], ns, t);
            snprintf(lab[t], sizeof lab[t], "[x%d]", t);
        } else if (tracks) graph_add(w, "%sanull[o%d];\n", lab[t], ns);
        if (tracks) streams[ns++] = t;
        if (in_mix) mixed[nm++] = t;
    }
    if (!nm) graph_add(w, "anullsrc=r=48000:cl=stereo,atrim=duration=%.4f,%s[o0];\n", len, AFMT);
    else {
        for (i32 i = 0; i < nm; i++) graph_add(w, "%s", lab[mixed[i]]);
        if (nm > 1) graph_add(w, "amix=inputs=%d:normalize=0:dropout_transition=0,", nm);
        graph_add(w, "alimiter=limit=0.98:level=0[o0];\n");
    }
    return ns;
}

// Share's sizes. A file is aimed 3% under the size it must fit, as the encoder lands a little either side of the
// bitrate asked for, with ~2% more for the mp4 container. Sizes the player sets are in MiB, which is what Discord
// means by MB.
enum { SHARE_MIN_VIDEO_KBPS = 80, SHARE_ATTEMPTS = 4, SHARE_AUDIO_KBPS = 128 };
static const f64 SHARE_MIB = 1024.0 * 1024.0, SHARE_MARGIN = 0.03;

// The video bitrate a clip needs to look right, in kbit/s, from a bitrate table (share_kbps; SHARE_KBPS_DEFAULT
// is YouTube's recommended upload bitrates): along the pixel count between its rows (16:9 pictures of their
// heights) and in proportion to it below 360p and above 2160p. Up to 30 fps the 30 fps column, from 60 the 60 fps
// one and in proportion to the frame rate above 60, in between along the frame rate. An unknown picture counts as
// 1080p, an unknown frame rate as 60.
static f64 share_reco_kbps_in(const i32 (*table)[2], i32 w, i32 h, f64 fps)
{
    f64 px[6];
    for (i32 i = 0; i < 6; i++) px[i] = SHARE_KBPS_HEIGHTS[i] * SHARE_KBPS_HEIGHTS[i] * 16.0 / 9;
    f64 p = w > 0 && h > 0 ? (f64)w * h : px[3];
    auto column = [&](i32 c) {
        if (p <= px[0]) return table[0][c] * p / px[0];
        if (p >= px[5]) return table[5][c] * p / px[5];
        i32 i = 0;
        while (p > px[i + 1]) i++;
        return table[i][c] + (table[i + 1][c] - table[i][c]) * (p - px[i]) / (px[i + 1] - px[i]);
    };
    if (fps <= 0) fps = 60;
    if (fps <= 30) return column(0);
    if (fps >= 60) return column(1) * fps / 60;
    return column(0) + (column(1) - column(0)) * (fps - 30) / 30;
}

static f64 share_reco_kbps(i32 w, i32 h, f64 fps)
{
    const Config &c = g_app.config;
    return share_reco_kbps_in(c.share_kbps_custom ? c.share_kbps : SHARE_KBPS_DEFAULT, w, h, fps);
}

// Options' line under Lower quality to fit: the lowest video bitrate, in kbit/s, a w x h clip at `fps` may be made
// at with `min_quality` (a percent, as share_min_quality).
static f64 share_lowest_kbps(i32 w, i32 h, f64 fps, i32 min_quality)
{
    return share_reco_kbps(w, h, fps) * min_quality / 100;
}

// Options' table: sets one cell (row of SHARE_KBPS_HEIGHTS, the 60 fps column when `high`), kept to 100 kbit/s to
// 500 Mbit/s. The caller saves the settings.
static void share_kbps_set(i32 row, bool high, i32 kbps)
{
    if (row < 0 || row >= 6) return;
    g_app.config.share_kbps[row][high] = kbps < 100 ? 100 : kbps > 500000 ? 500000 : kbps;
}

// Options' Reset: the table back to SHARE_KBPS_DEFAULT. The caller saves the settings.
static void share_kbps_reset() { memcpy(g_app.config.share_kbps, SHARE_KBPS_DEFAULT, sizeof g_app.config.share_kbps); }

// Whether the table is as it started (Reset has nothing to do).
static bool share_kbps_is_default()
{
    return !memcmp(g_app.config.share_kbps, SHARE_KBPS_DEFAULT, sizeof SHARE_KBPS_DEFAULT);
}

// A clip's recommended size: what it needs to look right, `seconds` long, with 128 kbit/s of sound when it has any.
static i64 share_reco_bytes(f64 seconds, i32 w, i32 h, f64 fps, bool audio)
{
    return (i64)((share_reco_kbps(w, h, fps) + (audio ? SHARE_AUDIO_KBPS : 0)) * 125 * (seconds > 0 ? seconds : 0));
}

// The bitrates for a copy `seconds` long with `audio` sound streams, aimed at `aim` bytes and never over `limit` (0 =
// no limit). Each sound stream gets a tier by the bitrate there is, the video the rest, never more than the source's
// own (`src_video_kbps`, 0 = unknown), which would only make a bigger file. False when even the lowest video bitrate
// doesn't fit.
struct ShareRate {
    i32 video_kbps, audio_kbps; // audio_kbps for each sound stream
    i64 ceiling; // what a made file must come in under, 0 = anything
    bool capped; // held to the source's own bitrate
};
static bool share_rate(f64 seconds, i32 audio, i64 aim, i64 limit, f64 src_video_kbps, ShareRate *out)
{
    *out = {};
    out->ceiling = limit > 0 ? (i64)(limit * (1 - SHARE_MARGIN)) : 0;
    if (out->ceiling && aim > out->ceiling) aim = out->ceiling;
    f64 total = aim * 8 * 0.98 / 1000 / (seconds > 0.1 ? seconds : 0.1);
    out->audio_kbps = !audio ? 0 : total >= 600 ? SHARE_AUDIO_KBPS : total >= 250 ? 96 : 64;
    f64 video = total - out->audio_kbps * (audio > 0 ? audio : 0);
    if (video > 200000) video = 200000;
    out->capped = src_video_kbps > 0 && video > src_video_kbps;
    if (out->capped) video = ceil(src_video_kbps);
    out->video_kbps = (i32)video;
    return out->capped || out->video_kbps >= SHARE_MIN_VIDEO_KBPS;
}

// Encoder settings for H.264 on `vendor`: a target bitrate for Share, else a constant quality for Keep.
static void edit_encoder_args(wchar_t *a, i32 cap, u8 vendor, i32 kbps)
{
    arg_pair(a, cap, "-c:v", "%s", encoder_name(vendor, "h264"));
    switch (vendor) {
    case VENDOR_NVENC:
        // For a size, constant bitrate with a one-second buffer: Compress for Discord measured it landing 0-2% over
        // what's asked at 1440p60, where variable bitrate landed anywhere from 21% under to 58% over.
        if (kbps) {
            arg_pair(a, cap, "-preset", "p6");
            arg_pair(a, cap, "-tune", "hq");
            arg_pair(a, cap, "-multipass", "fullres");
            arg_pair(a, cap, "-rc", "cbr");
            arg_pair(a, cap, "-b:v", "%dk", kbps);
            arg_pair(a, cap, "-maxrate", "%dk", kbps);
            arg_pair(a, cap, "-bufsize", "%dk", kbps);
            return;
        }
        arg_pair(a, cap, "-preset", "p5");
        arg_pair(a, cap, "-rc", "vbr");
        arg_pair(a, cap, "-cq", "19");
        arg_pair(a, cap, "-b:v", "0");
        break;
    case VENDOR_AMF:
        arg_pair(a, cap, "-quality", "quality");
        if (kbps) arg_pair(a, cap, "-rc", "vbr_peak");
        else { arg_pair(a, cap, "-rc", "cqp"); arg_pair(a, cap, "-qp_i", "19"); arg_pair(a, cap, "-qp_p", "19"); }
        break;
    case VENDOR_QSV:
        arg_pair(a, cap, "-preset", "slower");
        if (!kbps) arg_pair(a, cap, "-global_quality", "19");
        break;
    default:
        arg_pair(a, cap, "-preset", "medium");
        if (!kbps) arg_pair(a, cap, "-crf", "19");
        break;
    }
    if (kbps) {
        arg_pair(a, cap, "-b:v", "%dk", kbps);
        arg_pair(a, cap, "-maxrate", "%dk", kbps * 3 / 2);
        arg_pair(a, cap, "-bufsize", "%dk", kbps * 2);
    }
}

// The codecs of a file's video and first audio stream ("h264", "aac"), by ffprobe; empty where there's none.
static void file_codecs(const Path &ffmpeg, const Path &file, char (&video)[16], char (&audio)[16])
{
    static char out[4096];
    static wchar_t args[1024];
    video[0] = audio[0] = 0;
    args[0] = 0;
    static const char *const head[] = {"-v", "error", "-show_entries", "stream=codec_type,codec_name", "-of", "default"};
    arg_list(args, COUNT(args), head, COUNT(head));
    cmd_arg(args, COUNT(args), file.s);
    if (proc_run(ffprobe_for(ffmpeg).s, args, 20000, out, sizeof out) != 0) return;
    char name[16] = {};
    char *ctx = 0;
    for (char *line = strtok_s(out, "\r\n", &ctx); line; line = strtok_s(0, "\r\n", &ctx)) {
        if (!strncmp(line, "codec_name=", 11)) snprintf(name, sizeof name, "%s", line + 11);
        else if (!strcmp(line, "codec_type=video") && !video[0]) snprintf(video, sizeof video, "%s", name);
        else if (!strcmp(line, "codec_type=audio") && !audio[0]) snprintf(audio, sizeof audio, "%s", name);
    }
}

// The picture size a w x h video is scaled down to so it's at most `max_h` tall and 1920 wide, both even; 0 x 0 when
// it fits already. Chat apps halve a video wider than 1920.
static void link_fit_size(i32 w, i32 h, i32 max_h, i32 *fw, i32 *fh)
{
    *fw = *fh = 0;
    if (w <= 0 || h <= 0 || max_h <= 0) return;
    f64 s = (f64)max_h / h;
    if (1920.0 / w < s) s = 1920.0 / w;
    if (s >= 1) return;
    *fw = (i32)(w * s + 0.5) & ~1;
    *fh = (i32)(h * s + 0.5) & ~1;
}

// The end card a preview ends on (EditJob.end_card): its text centred on black, in Segoe UI.
#define END_CARD_TEXT "Continues on the page"
#define END_CARD_FONT "C\\:/Windows/Fonts/segoeui.ttf"
static const f32 END_CARD_S = 1.5f;

// How loud Share and Link make the mix (loudnorm): integrated loudness in LUFS and the true peak in dBTP, the level
// streaming sites and chat apps play at.
enum { LOUD_LUFS = -14 };
static const f64 LOUD_PEAK = -1.5;

// The layers an edit switches on, for drawing into its render: the clip's own (a Rekodr clip in the save folder), and
// where each stretch of the source plays (the pieces in turn, or the picture's items). False when there are none.
static bool edit_layers(const SaveWork *w, const EditJob &j, const EditPiece *pc, i32 np, LayerSet *set,
                        LayerSpan *spans, i32 *n_spans)
{
    *n_spans = 0;
    *set = {};
    size_t root = wcslen(w->cfg.save_dir.s);
    if (!j.edit.layer_on_count || !root || _wcsnicmp(j.src.s, w->cfg.save_dir.s, root) || j.src.s[root] != L'\\')
        return false;
    wchar_t stem[MAX_PATH];
    const wchar_t *name = wcsrchr(j.src.s, L'\\');
    wcscpy_s(stem, name + 1);
    wchar_t *dot = wcsrchr(stem, L'.');
    if (dot) *dot = 0;
    if (!layers_read(layers_clip_path(w->cfg.save_dir, stem), set)) return false;
    if (j.edit.mode == EDIT_ADVANCED) {
        i32 idx[EDIT_MAX_ITEMS], n = track_items(j.edit, 0, idx);
        for (i32 k = 0; k < n; k++) {
            const EditItem &it = j.edit.items[idx[k]];
            spans[(*n_spans)++] = {it.from, it.to, it.start};
        }
    } else {
        f64 at = 0;
        for (i32 i = 0; i < np; i++) {
            spans[(*n_spans)++] = {pc[i].from, pc[i].to, at};
            at += pc[i].to - pc[i].from;
        }
    }
    return true;
}

// A path as a filter's quoted option: forward slashes, the drive's colon escaped. False for one that can't be quoted.
static bool filter_path(const Path &p, char *out, i32 cap)
{
    char u[MAX_PATH * 3];
    wide_to_utf8(p.s, u, sizeof u);
    i32 n = 0;
    for (const char *c = u; *c; c++) {
        if (*c == '\'' || n + 3 >= cap) return false;
        if (*c == ':') out[n++] = '\\';
        out[n++] = *c == '\\' ? '/' : *c;
    }
    out[n] = 0;
    return true;
}

static i64 edit_run(SaveWork *w, char *out, i32 out_cap)
{
    const SaveRequest &req = w->req;
    const EditJob &j = req.edit;
    const MediaInfo &m = j.info;
    bool share = req.kind == SAVE_SHARE;
    // A link is played by Discord's player: H.264 and one track of sound. The picture is copied as it is when it's
    // H.264 already and the edit leaves it alone, else made at about the source's quality. The sound is the mix made
    // evenly loud (loudnorm), as for Share (`mixed`); a link with `tracks` keeps every track as Keep does.
    bool link = req.kind == SAVE_LINK, mixed = share || (link && !j.tracks);
    char vcodec[16] = {}, acodec[16] = {};
    if (link) file_codecs(w->cfg.ffmpeg, j.src, vcodec, acodec);
    bool link_video_ok = !strcmp(vcodec, "h264");
    // Each piece is read from sa to sb: its own span, and half a crossfade past each cut that has one. The input is
    // read from the earliest of those to the latest (`from`, `span`); `dur` is the joined length. With cuts or
    // transitions (`seq`) every piece is cut out of the input and they're joined. An Advanced edit (`adv`) is always
    // cut up and joined, every track on its own (adv_graph).
    bool adv = j.edit.mode == EDIT_ADVANCED;
    EditPiece pc[EDIT_MAX_PIECES];
    i32 np = pieces_of(j.edit, m.duration, pc);
    Transitions tr = transitions_of(j.edit, pc, np, m.duration);
    bool cuts = np > 1 || adv, seq = cuts || transitions_any(tr, np);
    f64 sa[EDIT_MAX_PIECES], sb[EDIT_MAX_PIECES];
    for (i32 i = 0; i < np; i++) {
        sa[i] = pc[i].from - (tr.into[i] == TRANS_CROSS ? tr.len[i] / 2 : 0);
        sb[i] = pc[i].to + (i + 1 < np && tr.into[i + 1] == TRANS_CROSS ? tr.len[i + 1] / 2 : 0);
    }
    f64 from = sa[0], to = sb[0];
    for (i32 i = 1; i < np; i++) {
        if (sa[i] < from) from = sa[i];
        if (sb[i] > to) to = sb[i];
    }
    if (adv) {
        from = m.duration, to = 0;
        for (i32 t = 0; t <= EDIT_CAM_TRACK; t++) {
            i32 idx[EDIT_MAX_ITEMS], n = track_items(j.edit, t, idx);
            ItemFades fd[EDIT_MAX_ITEMS];
            track_fades(j.edit, m.duration, idx, n, fd);
            for (i32 k = 0; k < n; k++) {
                const EditItem &it = j.edit.items[idx[k]];
                from = ImMin(from, it.from - fd[k].cross / 2);
                to = ImMax(to, it.to + (k + 1 < n ? fd[k + 1].cross / 2 : 0));
            }
        }
        if (to < from) from = to = 0;
    }
    f64 span = to - from, dur = adv ? adv_len(j.edit) : pieces_len(j.edit, m.duration);
    if (dur <= 0.05) {
        snprintf(out, out_cap, "the trim leaves nothing to save");
        return SAVE_EV_FAILED;
    }
    int made = SHCreateDirectoryExW(0, j.out_dir.s, 0);
    if (made != ERROR_SUCCESS && made != ERROR_ALREADY_EXISTS && made != ERROR_FILE_EXISTS) {
        snprintf(out, out_cap, "couldn't create the folder to save in (%d)", made);
        return SAVE_EV_FAILED;
    }
    wchar_t wstem[256];
    utf8_to_wide(j.stem, wstem, COUNT(wstem));
    Path final_path = {}, part_path = {};
    for (i32 n = 1; n < 1000; n++) {
        if (n == 1) swprintf(final_path.s, MAX_PATH, L"%s\\%s.mp4", j.out_dir.s, wstem);
        else swprintf(final_path.s, MAX_PATH, L"%s\\%s (%d).mp4", j.out_dir.s, wstem, n);
        swprintf(part_path.s, MAX_PATH, L"%s.part", final_path.s);
        if (!file_exists(final_path.s) && !file_exists(part_path.s)) break;
    }
    char final8[MAX_PATH * 3];
    wide_to_utf8(final_path.s, final8, sizeof final8);
    save_post(SAVE_EV_PATH, req.id, 0, final8);

    bool zoom = edit_zoomed(j.edit), colored = edit_colored(j.edit);
    bool touched = edit_audio_touched(j.edit, m);
    // Share and a sized Link are made at a bitrate for aim_bytes, under limit_bytes; the picture and frame rate stay.
    // A Link copy is stream-copied when the edit leaves the picture alone, it's H.264 already and the copy wouldn't
    // be much over the aim.
    i64 aim = j.aim_bytes, limit = j.limit_bytes;
    WIN32_FILE_ATTRIBUTE_DATA sfa = {};
    i64 src_bytes = GetFileAttributesExW(j.src.s, GetFileExInfoStandard, &sfa)
                        ? (i64)(((u64)sfa.nFileSizeHigh << 32) | sfa.nFileSizeLow) : 0;
    f64 src_kbps = m.duration > 0 ? src_bytes * 8 / 1000.0 / m.duration : 0;
    f64 copy_bytes = m.duration > 0 ? src_bytes * (span / m.duration) : 0;
    bool link_big = link && ((aim > 0 && copy_bytes > aim * 1.1) || (limit > 0 && copy_bytes > limit * (1 - SHARE_MARGIN)));
    i32 fit_w = 0, fit_h = 0; // a Link made smaller (max_h)
    if (link) link_fit_size(m.width, m.height, j.max_h, &fit_w, &fit_h);
    // Layers switched on are drawn into the picture, from layers.ass in the work folder
    static LayerSet lays;
    static LayerSpan lay_spans[EDIT_MAX_ITEMS + EDIT_MAX_PIECES];
    i32 n_lay_spans = 0;
    bool lay = edit_layers(w, j, pc, np, &lays, lay_spans, &n_lay_spans) &&
               layers_ass(0, lays, j.edit.layer_on, j.edit.layer_on_count, lay_spans, n_lay_spans, dur, m.width, m.height) > 0;
    if (!lay) layer_set_free(&lays);
    bool reencode = lay || share || zoom || colored || seq || j.end_card > 0 || (link && (!link_video_ok || link_big || fit_w));
    bool sized = reencode && (share || link) && aim > 0;
    // The sound streams written: the mix alone, or (a link with `tracks`) the tracks, after a rebuilt mix when there
    // is one; a sized copy encodes every one at rate.audio_kbps
    i32 first_track = edit_has_mix(m) ? 1 : 0, tc = m.track_count < MEDIA_MAX_TRACKS ? m.track_count : MEDIA_MAX_TRACKS;
    i32 n_audio = tc <= 0 ? 0 : mixed ? 1 : adv || touched ? 1 + tc - first_track : tc;
    ShareRate rate = {};
    i32 video_kbps = 0;
    if (sized) {
        bool audio = n_audio > 0;
        if (!share_rate(dur + j.end_card, n_audio, aim, limit, src_kbps - (audio ? SHARE_AUDIO_KBPS : 0), &rate)) {
            snprintf(out, out_cap, "too long for %.0f MB: trim it shorter", (limit ? limit : aim) / SHARE_MIB);
            return SAVE_EV_FAILED;
        }
        video_kbps = rate.video_kbps;
    }
    Path work = work_file(w->cfg.work_dir, L"edit-%d", (i32)req.id);
    SHCreateDirectoryExW(0, work.s, 0);
    Path graph = path_join(work, L"graph.txt"), ass = path_join(work, L"layers.ass");
    char ass_arg[MAX_PATH * 3] = {};
    if (lay) {
        FILE *f = filter_path(ass, ass_arg, sizeof ass_arg) ? _wfopen(ass.s, L"wb") : 0;
        if (f) layers_ass(f, lays, j.edit.layer_on, j.edit.layer_on_count, lay_spans, n_lay_spans, dur, m.width, m.height), fclose(f);
        layer_set_free(&lays);
        if (!f) {
            snprintf(out, out_cap, "couldn't write the layers for the picture");
            RemoveDirectoryW(work.s);
            return SAVE_EV_FAILED;
        }
    }
    i32 code = 0;
    f64 audio_bytes = rate.audio_kbps * 125.0 * dur * n_audio;
    for (i32 attempt = 0;; attempt++) {
        // The graph: video (when re-encoded) as [v], audio (when re-mixed, or for a share) as [a]
        w->graph_len = 0;
        w->graph_full = false;
        w->graph[0] = 0;
        // The audio streams written when the pieces are joined, in order: -1 for the rebuilt mix, else a track
        i32 streams[MEDIA_MAX_TRACKS + 1], ns = 0;
        if (adv) {
            ns = adv_graph(w, j, from, "", w->cfg.vendor == VENDOR_QSV ? "nv12" : "yuv420p", !mixed, streams);
        } else if (reencode) {
            graph_add(w, "[0:v]");
            if (zoom) zoom_filter(w, j.edit.keys, j.edit.key_count, m.fps, from);
            if (colored) {
                const VolKey *ck[EDIT_COLORS] = {j.edit.color[0], j.edit.color[1], j.edit.color[2]};
                color_filter(w, ck, j.edit.color_count, m.fps, from);
            }
            graph_add(w, "format=%s", w->cfg.vendor == VENDOR_QSV ? "nv12" : "yuv420p");
            if (seq) {
                graph_add(w, ",split=%d", np);
                for (i32 i = 0; i < np; i++) graph_add(w, "[c%d]", i);
                graph_add(w, ";\n");
                for (i32 i = 0; i < np; i++) {
                    f64 in, fo;
                    piece_fades(tr, np, i, &in, &fo);
                    graph_add(w, "[c%d]trim=start=%.4f:end=%.4f,setpts=PTS-STARTPTS", i, sa[i] - from, sb[i] - from);
                    if (in > 0) graph_add(w, ",fade=t=in:st=0:d=%.4f", in);
                    if (fo > 0) graph_add(w, ",fade=t=out:st=%.4f:d=%.4f", sb[i] - sa[i] - fo, fo);
                    graph_add(w, "[v%d];\n", i);
                }
                seq_join(w, tr, sa, sb, np, -1);
            } else graph_add(w, "[v];\n");
        }
        bool mix = !adv && (mixed || touched) && edit_audio_graph(w, j, from);
        // With cuts or transitions each stream (the rebuilt mix [a], or a track as recorded) is cut into the pieces,
        // faded, and joined into [o<n>].
        if (seq && !adv) {
            if (mixed) {
                if (mix) streams[ns++] = -1;
            } else {
                i32 first = 0;
                if (touched) { streams[ns++] = -1; first = edit_has_mix(m) ? 1 : 0; }
                for (i32 t = first; t < m.track_count && t < MEDIA_MAX_TRACKS; t++) streams[ns++] = t;
            }
            for (i32 s = 0; s < ns; s++) {
                if (streams[s] < 0) graph_add(w, "[a]");
                else graph_add(w, "[0:a:%d]", streams[s]);
                graph_add(w, "asplit=%d", np);
                for (i32 i = 0; i < np; i++) graph_add(w, "[d%d_%d]", s, i);
                graph_add(w, ";\n");
                for (i32 i = 0; i < np; i++) {
                    f64 in, fo;
                    piece_fades(tr, np, i, &in, &fo);
                    graph_add(w, "[d%d_%d]atrim=start=%.4f:end=%.4f,asetpts=PTS-STARTPTS", s, i, sa[i] - from, sb[i] - from);
                    if (in > 0) graph_add(w, ",afade=t=in:st=0:d=%.4f", in);
                    if (fo > 0) graph_add(w, ",afade=t=out:st=%.4f:d=%.4f", sb[i] - sa[i] - fo, fo);
                    graph_add(w, "[e%d_%d];\n", s, i);
                }
                seq_join(w, tr, sa, sb, np, s);
            }
        }
        const char *pic = "v"; // the finished picture's label
        if (lay) { // the layers drawn on [v], as [vl]
            i32 end = w->graph_len;
            while (end > 0 && (w->graph[end - 1] == '\n' || w->graph[end - 1] == ' ')) end--;
            graph_add(w, "%s[v]subtitles=filename='%s':fontsdir='C\\:/Windows/Fonts'[vl];\n",
                      end > 0 && w->graph[end - 1] != ';' ? ";\n" : "", ass_arg);
            pic = "vl";
        }
        if (fit_w) { // the finished picture made smaller, as [vfit]
            i32 end = w->graph_len;
            while (end > 0 && (w->graph[end - 1] == '\n' || w->graph[end - 1] == ' ')) end--;
            graph_add(w, "%s[%s]scale=%d:%d:flags=lanczos[vfit];\n", end > 0 && w->graph[end - 1] != ';' ? ";\n" : "",
                      pic, fit_w, fit_h);
        }
        // Share and Link: the mix (the one sound stream, [o0] when joined, else [a]) made evenly loud, as [loud]
        const char *loud_from = !mixed ? 0 : seq ? (ns && streams[0] == -1 ? "o0" : 0) : mix ? "a" : 0;
        if (loud_from) {
            i32 end = w->graph_len;
            while (end > 0 && (w->graph[end - 1] == '\n' || w->graph[end - 1] == ' ')) end--;
            graph_add(w, "%s[%s]loudnorm=I=%d:TP=%.1f:LRA=11,aresample=48000[loud];\n",
                      end > 0 && w->graph[end - 1] != ';' ? ";\n" : "", loud_from, LOUD_LUFS, LOUD_PEAK);
        }
        if (j.end_card > 0) { // the card after the finished picture, as [vend]; the sound just ends before it
            i32 end = w->graph_len, cw = fit_w ? fit_w : m.width, ch = fit_w ? fit_h : m.height;
            while (end > 0 && (w->graph[end - 1] == '\n' || w->graph[end - 1] == ' ')) end--;
            graph_add(w, "%scolor=c=black:s=%dx%d:r=%.3f:d=%.3f,format=%s,drawtext=fontfile='%s':text='%s'"
                         ":fontcolor=0xd6d9de:fontsize=%d:x=(w-text_w)/2:y=(h-text_h)/2,setsar=1[card];\n",
                      end > 0 && w->graph[end - 1] != ';' ? ";\n" : "", cw, ch, m.fps > 0 ? m.fps : 60.0, j.end_card,
                      w->cfg.vendor == VENDOR_QSV ? "nv12" : "yuv420p", END_CARD_FONT, END_CARD_TEXT, ch / 18 > 8 ? ch / 18 : 8);
            graph_add(w, "[%s]setsar=1[vcut];\n[vcut][card]concat=n=2:v=1:a=0[vend];\n", fit_w ? "vfit" : pic);
        }
        if (w->graph_full) {
            snprintf(out, out_cap, "the zoom or colour has too many keyframes");
            DeleteFileW(ass.s);
            RemoveDirectoryW(work.s);
            return SAVE_EV_FAILED;
        }
        if (w->graph_len) {
            FILE *f = _wfopen(graph.s, L"wb");
            if (!f) {
                snprintf(out, out_cap, "couldn't write the edit's work files");
                DeleteFileW(ass.s);
                RemoveDirectoryW(work.s);
                return SAVE_EV_FAILED;
            }
            fwrite(w->graph, 1, w->graph_len, f);
            fclose(f);
        }

        wchar_t *a = w->args;
        i32 cap = COUNT(w->args);
        a[0] = 0;
        static const char *const head[] = {"-hide_banner", "-loglevel", "error", "-nostats", "-progress", "pipe:1", "-y"};
        arg_list(a, cap, head, COUNT(head));
        arg_pair(a, cap, "-ss", "%.3f", from);
        if (seq) arg_pair(a, cap, "-t", "%.3f", span); // the input's end; the output is the joined pieces
        cmd_arg8(a, cap, "-i");
        cmd_arg(a, cap, j.src.s);
        if (!seq) arg_pair(a, cap, "-t", "%.3f", span);
        if (w->graph_len) {
            cmd_arg8(a, cap, "-/filter_complex");
            cmd_arg(a, cap, graph.s);
        }
        if (reencode) {
            arg_pair(a, cap, "-map", "[%s]", j.end_card > 0 ? "vend" : fit_w ? "vfit" : pic);
            edit_encoder_args(a, cap, w->cfg.vendor, sized ? video_kbps : 0);
        } else {
            arg_pair(a, cap, "-map", "0:v:0");
            if (!link) arg_pair(a, cap, "-map", "0:v:1?"); // the webcam stays with a kept copy, title and all
            arg_pair(a, cap, "-c:v", "copy");
        }
        i32 outs = 0; // audio streams written
        if (seq) {
            for (i32 s = 0; s < ns; s++) {
                char key[32];
                if (s == 0 && loud_from) arg_pair(a, cap, "-map", "[loud]");
                else arg_pair(a, cap, "-map", "[o%d]", s);
                snprintf(key, sizeof key, "-c:a:%d", s);
                arg_pair(a, cap, key, "aac");
                snprintf(key, sizeof key, "-b:a:%d", s);
                arg_pair(a, cap, key, "%dk", sized && rate.audio_kbps ? rate.audio_kbps : 192);
                snprintf(key, sizeof key, "-ar:a:%d", s);
                arg_pair(a, cap, key, "48000");
                const char *title = streams[s] < 0 ? "Mix" : m.tracks[streams[s]];
                snprintf(key, sizeof key, "-metadata:s:a:%d", s);
                arg_pair(a, cap, key, "title=%s", title);
                arg_pair(a, cap, key, "handler_name=%s", title);
                snprintf(key, sizeof key, "-disposition:a:%d", s);
                arg_pair(a, cap, key, s ? "0" : "default");
            }
        } else if (mixed) {
            if (mix) {
                arg_pair(a, cap, "-map", loud_from ? "[loud]" : "[a]");
                arg_pair(a, cap, "-c:a", "aac");
                arg_pair(a, cap, "-b:a", "%dk", sized && rate.audio_kbps ? rate.audio_kbps : 192);
                arg_pair(a, cap, "-ar", "48000");
            }
        } else if (!touched) {
            if (m.track_count > 0) { // as recorded; a sized link's at its rate
                arg_pair(a, cap, "-map", "0:a");
                if (sized && rate.audio_kbps) {
                    arg_pair(a, cap, "-c:a", "aac");
                    arg_pair(a, cap, "-b:a", "%dk", rate.audio_kbps);
                    arg_pair(a, cap, "-ar", "48000");
                } else arg_pair(a, cap, "-c:a", "copy");
            }
        } else {
            // The rebuilt Mix first, then the original tracks as recorded (a Rekodr file's old Mix is dropped); a
            // sized link's all at its rate.
            i32 akbps = sized && rate.audio_kbps ? rate.audio_kbps : 0;
            arg_pair(a, cap, "-map", "[a]");
            arg_pair(a, cap, "-c:a:0", "aac");
            arg_pair(a, cap, "-b:a:0", "%dk", akbps ? akbps : 192);
            arg_pair(a, cap, "-ar:a:0", "48000");
            outs = 1;
            for (i32 t = edit_has_mix(m) ? 1 : 0; t < m.track_count && t < MEDIA_MAX_TRACKS; t++) {
                arg_pair(a, cap, "-map", "0:a:%d", t);
                char key[32];
                snprintf(key, sizeof key, "-c:a:%d", outs);
                arg_pair(a, cap, key, akbps ? "aac" : "copy");
                if (akbps) {
                    snprintf(key, sizeof key, "-b:a:%d", outs);
                    arg_pair(a, cap, key, "%dk", akbps);
                    snprintf(key, sizeof key, "-ar:a:%d", outs);
                    arg_pair(a, cap, key, "48000");
                }
                snprintf(key, sizeof key, "-metadata:s:a:%d", outs);
                arg_pair(a, cap, key, "title=%s", m.tracks[t]);
                arg_pair(a, cap, key, "handler_name=%s", m.tracks[t]);
                snprintf(key, sizeof key, "-disposition:a:%d", outs);
                arg_pair(a, cap, key, "0");
                outs++;
            }
            arg_pair(a, cap, "-metadata:s:a:0", "title=Mix");
            arg_pair(a, cap, "-metadata:s:a:0", "handler_name=Mix");
            arg_pair(a, cap, "-disposition:a:0", "default");
        }
        // Keep with one piece carries the original's chapters; Share, Link and cuts have none
        if (share || link || cuts) arg_pair(a, cap, "-map_chapters", "-1");
        arg_pair(a, cap, "-movflags", "+faststart");
        arg_pair(a, cap, "-f", "mp4");
        cmd_arg(a, cap, part_path.s);

        log_line("%s %u: %.1f s in %d piece%s from %.2f s, transitions %s, zoom %d, colour %s, audio %s, %s -> %s",
                 share ? "share" : link ? "link" : "keep", req.id, dur, np, np == 1 ? "" : "s", from,
                 transitions_any(tr, np) ? "yes" : "no",
                 zoom ? j.edit.key_count : 0, colored ? "changed" : "as recorded", mix ? "re-mixed" : "as recorded", reencode ? "re-encoded" : "copied", final8);
        if (adv) log_line("%s %u: advanced, %d items", share ? "share" : link ? "link" : "keep", req.id, j.edit.item_count);
        if (link) log_line("link %u: source %s / %s%s", req.id, vcodec[0] ? vcodec : "-", acodec[0] ? acodec : "-",
                           fit_w ? ", made smaller" : "");
        if (sized)
            log_line("%s %u: aiming at %.2f MB under %.2f, attempt %d: video %d kbps%s, audio %d kbps",
                     share ? "share" : "link", req.id, aim / SHARE_MIB, limit / SHARE_MIB, attempt + 1, video_kbps,
                     rate.capped ? " (source's rate)" : "", rate.audio_kbps);
        code = run_ffmpeg(w, (i64)(dur * 1e6), out, out_cap);
        if (code != 0 || !sized || !rate.ceiling) break;

        // Over the size: again at a lower bitrate, by as much as it was over and a little more
        WIN32_FILE_ATTRIBUTE_DATA fa = {};
        GetFileAttributesExW(part_path.s, GetFileExInfoStandard, &fa);
        i64 size = (i64)(((u64)fa.nFileSizeHigh << 32) | fa.nFileSizeLow);
        log_line("%s %u: attempt %d made %.2f MB, under %.2f wanted", share ? "share" : "link", req.id, attempt + 1,
                 size / SHARE_MIB, rate.ceiling / SHARE_MIB);
        if (size <= rate.ceiling) break;
        if (attempt == SHARE_ATTEMPTS - 1) {
            if (size <= limit) break;
            snprintf(out, out_cap, "couldn't get it under %.0f MB: trim it shorter", limit / SHARE_MIB);
            code = -3;
            break;
        }
        f64 video_made = size - audio_bytes > 1 ? size - audio_bytes : 1;
        f64 room = rate.ceiling - audio_bytes > 1 ? rate.ceiling - audio_bytes : 1;
        i32 next = (i32)floor(video_kbps * (room / video_made) * (w->cfg.vendor == VENDOR_NVENC ? 0.97 : 0.92));
        if (next >= video_kbps) next = video_kbps * 9 / 10;
        if (next < SHARE_MIN_VIDEO_KBPS) {
            snprintf(out, out_cap, "couldn't get it under %.0f MB: trim it shorter", limit / SHARE_MIB);
            code = -3;
            break;
        }
        video_kbps = next;
    }
    i64 result;
    if (code == 0 && MoveFileExW(part_path.s, final_path.s, 0)) {
        snprintf(out, out_cap, "%s", final8);
        result = SAVE_EV_DONE;
    } else {
        DeleteFileW(part_path.s);
        if (code == -2) {
            result = SAVE_EV_CANCELLED;
        } else {
            if (code == 0) snprintf(out, out_cap, "couldn't rename the finished file (%d)", (int)GetLastError());
            else if (!out[0]) snprintf(out, out_cap, "ffmpeg failed (exit code %d)", code);
            if (w->graph_len) CopyFileW(graph.s, path_join(w->cfg.work_dir, L"last-failed-graph.txt").s, FALSE);
            result = SAVE_EV_FAILED;
        }
    }
    DeleteFileW(graph.s);
    DeleteFileW(ass.s);
    RemoveDirectoryW(work.s);
    return result;
}

static DWORD WINAPI save_thread(void *)
{
    SaveWork *w = &g_save_work;
    for (;;) {
        AcquireSRWLockExclusive(&g_save_q.lock);
        while (!g_save_q.count && !g_save_q.stop) SleepConditionVariableSRW(&g_save_q.wake, &g_save_q.lock, INFINITE, 0);
        if (g_save_q.stop) {
            ReleaseSRWLockExclusive(&g_save_q.lock);
            break;
        }
        w->req = g_save_q.items[0];
        g_save_q.count--;
        memmove(g_save_q.items, g_save_q.items + 1, g_save_q.count * sizeof g_save_q.items[0]);
        w->cfg = g_save_q.settings;
        g_save_q.cancel_id = 0;
        g_save_q.running = true;
        ReleaseSRWLockExclusive(&g_save_q.lock);

        save_post(SAVE_EV_STARTED, w->req.id, 0, 0);
        char out[512];
        u64 started = GetTickCount64();
        i64 result = save_run(w, out, sizeof out);
        if (result == SAVE_EV_DONE) log_line("save %u: took %.1f s", w->req.id, (GetTickCount64() - started) / 1000.0);
        AcquireSRWLockExclusive(&g_save_q.lock);
        g_save_q.running = false;
        ReleaseSRWLockExclusive(&g_save_q.lock);
        save_post(result, w->req.id, 0, out);
    }
    return 0;
}

// ---------------------------------------------------------------------------------------------------------------
// Main thread side.

static SaveJobView *save_view(u32 id)
{
    for (i32 i = 0; i < COUNT(g_app.saving.jobs); i++)
        if (id && g_app.saving.jobs[i].id == id) return &g_app.saving.jobs[i];
    return 0;
}

// A finished job whose result something on screen or a session still follows (by id). Its slot isn't reused, or
// that reader would find the job gone and take it as failed.
static bool r2_holds_save(u32 id);
static bool share_holds_save(u32 id); // share.cpp
static bool save_job_held(u32 id)
{
    if (r2_holds_save(id) || share_holds_save(id)) return true;
    if (id == g_app.editor.keep_job || id == g_app.editor.share_job) return true;
    const Timeline &tl = g_app.timeline;
    if (id == tl.job || id == tl.play.job || id == tl.next.job || id == tl.rest.job) return true;
    const Sessions &ss = g_app.sessions;
    for (i32 i = 0; i < COUNT(ss.items); i++)
        if (ss.items[i].state == SES_SAVING && ss.items[i].job == id) return true;
    return false;
}

static void save_cancel(u32 id);

// Queues a save of the wall-clock range [from_us, to_us]: a clip into Clips\<year>, or a session into
// Sessions\<year>. An empty `name` names it after the game played most. Returns the job id, or 0 if it couldn't be
// queued (no free slot).
static u32 save_queue(SaveKind kind, i64 from_us, i64 to_us, const char *name, const EditJob *edit)
{
    Saving &sv = g_app.saving;
    // Anything else goes before a render: one under way stops and is made again later (render.cpp)
    if (kind != SAVE_RENDER)
        for (i32 i = 0; i < COUNT(sv.jobs); i++)
            if (sv.jobs[i].id && sv.jobs[i].kind == SAVE_RENDER && sv.jobs[i].state < SAVE_DONE)
                save_cancel(sv.jobs[i].id);
    // A free slot, else the oldest finished job's.
    SaveJobView *v = 0;
    for (i32 i = 0; i < COUNT(sv.jobs); i++) {
        SaveJobView *j = &sv.jobs[i];
        if (!j->id) { v = j; break; }
        if (j->state >= SAVE_DONE && (!v || j->id < v->id) && !save_job_held(j->id)) v = j;
    }
    if (!v) {
        log_line("save: no free save slot (%d saves waiting or still followed); not queued", COUNT(sv.jobs));
        return 0;
    }
    *v = {};
    v->id = ++sv.next_id;
    v->kind = kind;
    v->state = SAVE_QUEUED;
    v->from_us = from_us;
    v->to_us = to_us;
    v->quiet = edit && edit->quiet;
    str_set(v->name, name);
    sv.jobs_running++;

    Path ffmpeg = {};
    const Config &c = g_app.config;
    if (!ffmpeg_find(c.ffmpeg, &ffmpeg)) ffmpeg = {};
    AcquireSRWLockExclusive(&g_save_q.lock);
    CHECK(g_save_q.count < COUNT(g_save_q.items), "a free job slot means room in the queue; found %d queued",
          g_save_q.count);
    SaveRequest *r = &g_save_q.items[g_save_q.count++];
    *r = {};
    r->id = v->id;
    r->kind = kind;
    r->from_us = from_us;
    r->to_us = to_us;
    str_set(r->name, name);
    r->recording = g_app.recorder.recording;
    r->queued_ms = GetTickCount64();
    if (edit) r->edit = *edit;
    SaveSettings *s = &g_save_q.settings;
    s->buffer_dir = c.buffer_dir;
    s->save_dir = c.save_dir;
    s->ffmpeg = ffmpeg;
    s->work_dir = g_app.paths.local_dir;
    s->play_dir = c.play_dir;
    str_set(s->name_template, c.name_template);
    str_set(s->codec, c.codec.s);
    s->voice_mono = c.voice_mono;
    s->mic_clean = c.mic_clean;
    s->mic_raw = c.mic_raw;
    memcpy(s->inputs, c.inputs, sizeof s->inputs);
    s->input_count = c.input_count;
    // H.264 for edits: the encoder forced in Options if it's set, else the first whose test encode worked.
    i32 forced = vendor_parse(c.encoder.s);
    u8 ok = g_app.recorder.encoders_ok[0];
    s->vendor = VENDOR_CPU;
    if (forced >= 0) s->vendor = (u8)forced;
    else
        for (i32 vi = 0; vi < VENDOR_COUNT; vi++)
            if (ok & (1 << vi)) { s->vendor = (u8)vi; break; }
    memcpy(s->apps, c.apps, c.app_count * sizeof c.apps[0]);
    s->app_count = c.app_count;
    memcpy(s->folders, c.folders, c.folder_count * sizeof c.folders[0]);
    s->folder_count = c.folder_count;
    if (!g_save_q.thread) g_save_q.thread = thread_start(save_thread, 0);
    WakeConditionVariable(&g_save_q.wake);
    ReleaseSRWLockExclusive(&g_save_q.lock);
    return v->id;
}

static u32 save_range(i64 from_us, i64 to_us, SaveKind kind, const char *name)
{
    if (to_us <= from_us) {
        log_line("save: empty range");
        return 0;
    }
    return save_queue(kind, from_us, to_us, name, 0);
}

// Queues the editor's Keep or Share of `job`. Returns the job id, or 0 if it couldn't be queued.
static u32 save_edit(SaveKind kind, const EditJob &job)
{
    return save_queue(kind, 0, 0, job.stem, &job);
}

// Queues a copy of `src` for a link, with `edit` applied (an empty record for the file as it is): H.264 with the
// first (Mix) track only, made evenly loud (with `tracks`, every track as Keep keeps them), +faststart, in the link folder as <stem>.mp4. The picture is copied when
// it's already H.264, the edit only trims and the copy isn't much over `aim` bytes; anything else is encoded at the
// bitrate for `aim`, under `limit` (0 = no limit). With no aim it's encoded at about the source's quality. Follow it
// with save_view(id) like any save; the caller deletes the file once it's done with it. Returns the job id, or 0 if
// it couldn't be queued. With `max_h` the picture is made at most that tall and 1920 wide (link_fit_size), and
// with `end_card` it ends on that many seconds of END_CARD_TEXT.
static u32 save_link(const Path &src, const MediaInfo &info, const EditRecord &edit, const char *stem, i64 aim = 0,
                     i64 limit = 0, i32 max_h = 0, f32 end_card = 0, bool tracks = false)
{
    static EditJob j;
    j = {};
    j.tracks = tracks;
    j.aim_bytes = aim;
    j.limit_bytes = limit;
    j.max_h = max_h;
    j.end_card = end_card;
    j.src = src;
    j.out_dir = g_app.paths.link_dir;
    j.info = info;
    j.edit = edit;
    str_set(j.stem, stem);
    clean_name(j.stem);
    if (!j.stem[0]) str_set(j.stem, "clip");
    return save_queue(SAVE_LINK, 0, 0, j.stem, &j);
}

// Whether `path` is a session: a file in a year folder of a Sessions folder (Sessions\<year>).
static bool path_is_session(const wchar_t *path)
{
    const wchar_t *name = wcsrchr(path, L'\\');
    if (!name || name - path < 14 || name[-5] != L'\\') return false;
    for (int i = 1; i <= 4; i++)
        if (name[-i] < L'0' || name[-i] > L'9') return false;
    return !_wcsnicmp(name - 14, L"\\Sessions", 9);
}

// ---------------------------------------------------------------------------------------------------------------
// The social copy: the H.264 copy a link's page gives chat apps to play, at most SOCIAL_H tall and 1920 wide. It's
// made at social_kbps' target (12 Mbps at 1080p60) when that fits the budget, lower down to the floor
// (5 Mbps at 1080p) when it doesn't; when not even the floor fits the whole clip, with SOCIAL_PREVIEW on, it's a
// preview: the first part at the floor, as long as the budget allows, then END_CARD_S of "Continues on the page".
// The budget (social_copy_mb, 150 MB) is under what Discord's proxy was seen to embed (250 MB) and play; the copy is
// made SOCIAL_MARGIN under it. A copy is needed over SOCIAL_MAX_KBPS (14 Mbps) too.

#define SOCIAL_PREVIEW 1 // experimental: 0 makes every social copy the whole clip
enum { SOCIAL_H = 1080, SOCIAL_KBPS = 12000, SOCIAL_MIN_KBPS = 5000, SOCIAL_MAX_KBPS = 14000 };
static const f64 SOCIAL_MARGIN = 0.05;

// The social copy's video bitrates, in kbit/s, for a w x h clip at `fps`: `*target` what it's made at (the default
// bitrate table at its size after link_fit_size, at most SOCIAL_KBPS), `*least` the floor (SOCIAL_MIN_KBPS, or the
// target when that's lower).
static void social_kbps(i32 w, i32 h, f64 fps, f64 *target, f64 *least)
{
    i32 fw, fh;
    link_fit_size(w, h, SOCIAL_H, &fw, &fh);
    if (!fw) fw = w, fh = h;
    f64 t = share_reco_kbps_in(SHARE_KBPS_DEFAULT, fw, fh, fps);
    *target = t < (f64)SOCIAL_KBPS ? t : (f64)SOCIAL_KBPS;
    *least = *target < (f64)SOCIAL_MIN_KBPS ? *target : (f64)SOCIAL_MIN_KBPS;
}

// Whether the whole `full`-second clip fits `budget` bytes at `floor_kbps` of video or better.
static bool social_whole_fits(f64 full, bool audio, i64 budget, f64 floor_kbps)
{
    ShareRate r;
    return share_rate(full, audio, (i64)(budget * (1 - SOCIAL_MARGIN)), budget, 0, &r) && r.video_kbps >= floor_kbps;
}

// The edit cut to its first `secs` seconds of output: Simple pieces in order, Advanced items by where they start.
// Keys stay on their moments in the video, so they need no change; an item's keys go with it.
static void edit_first(EditRecord *e, f64 video_len, f64 secs)
{
    if (e->mode == EDIT_ADVANCED) {
        i32 n = 0;
        for (i32 i = 0; i < e->item_count; i++) {
            EditItem it = e->items[i];
            if (it.start >= secs - 0.01) continue;
            if (item_end(it) > secs) {
                it.to = it.from + (secs - it.start);
                it.fade_out = 0;
            }
            e->items[n++] = it;
        }
        e->item_count = n;
        n = 0;
        for (i32 k = 0; k < e->ikey_count; k++)
            if (item_find(*e, e->ikeys[k].item) >= 0) e->ikeys[n++] = e->ikeys[k];
        e->ikey_count = n;
        return;
    }
    EditPiece p[EDIT_MAX_PIECES];
    i32 n = pieces_of(*e, video_len, p), kept = 0;
    f64 at = 0;
    for (; kept < n && at < secs - 0.01; kept++) {
        f64 len = p[kept].to - p[kept].from;
        if (at + len > secs + 0.01) {
            p[kept].to = p[kept].from + (secs - at);
            e->fade_out = 0; // the end card follows the cut
        }
        at += len;
    }
    if (kept < n) e->fade_out = 0;
    pieces_store(e, p, kept, video_len);
}

// How long the preview of a `full`-second clip is in `budget` bytes: whole seconds at `floor_kbps` (and the sound)
// with room for the end card. 0 when the whole clip is the copy (it fits at `floor_kbps` and `max_h` > 0, or
// SOCIAL_PREVIEW is off) or there's no room for a preview.
static f64 social_preview_len(f64 full, bool audio, i64 budget, i32 max_h, f64 floor_kbps)
{
    if (!SOCIAL_PREVIEW || (max_h > 0 && social_whole_fits(full, audio, budget, floor_kbps))) return 0;
    f64 kbit = budget * (1 - SOCIAL_MARGIN) * 8 * 0.98 / 1000;
    f64 secs = floor(kbit / (floor_kbps + (audio ? SHARE_AUDIO_KBPS : 0)) - END_CARD_S);
    return secs >= 5 && secs < full ? secs : 0;
}

// Queues the social copy of `src` with `edit` applied, at most `budget` bytes (save_link): the whole clip at most
// `max_h` tall at social_kbps' target, or as much under it as the budget needs down to the floor; else the preview
// (social_preview_len) at the floor. `*preview_s` is the preview's length (0: the whole clip), `*full_s` the clip's.
// Follow it with save_view(id); 0 when it couldn't be queued or neither fits.
static u32 save_social_copy(const Path &src, const MediaInfo &info, const EditRecord &edit, const char *stem,
                            i64 budget, i32 max_h, f64 *preview_s, f64 *full_s)
{
    f64 full = edit.mode == EDIT_ADVANCED ? adv_len(edit) : pieces_len(edit, info.duration);
    bool audio = info.track_count > 0;
    f64 target, low;
    social_kbps(info.width, info.height, info.fps, &target, &low);
    f64 preview = social_preview_len(full, audio, budget, max_h, low);
    *preview_s = preview;
    *full_s = full;
    i64 most = (i64)(budget * (1 - SOCIAL_MARGIN));
    if (!preview) {
        if (max_h <= 0 || !social_whole_fits(full, audio, budget, low)) return 0;
        i64 aim = (i64)((target + (audio ? SHARE_AUDIO_KBPS : 0)) * 125 / 0.98 * full);
        return save_link(src, info, edit, stem, aim < most ? aim : most, budget, max_h);
    }
    static EditRecord cut;
    cut = edit;
    edit_first(&cut, info.duration, preview);
    i64 aim = (i64)((low + (audio ? SHARE_AUDIO_KBPS : 0)) * 125 / 0.98 * (preview + END_CARD_S));
    u32 id = save_link(src, info, cut, stem, aim < most ? aim : most, budget, SOCIAL_H, END_CARD_S);
    if (id) log_line("social %u: preview, first %.0f of %.0f s", id, preview, full);
    return id;
}

static void toast_save_failed(SaveKind kind, const char *why); // toast.cpp

// Saves the last `secs` seconds of the buffer as a clip in <save_dir>\Clips\<year>. A start before the oldest
// footage is moved up to it, so the saved line says what the clip holds. Refused, with the Failed sound, while
// recording is paused or stopped by an error: the clip would only hold footage from before.
static void save_last(i64 secs)
{
    const Recorder &rec = g_app.recorder;
    if (rec.paused || (!rec.recording && rec.last_error.s[0])) {
        const char *why = rec.paused ? "recording is paused" : "recording has stopped";
        log_line("save: not saved, %s", why);
        pipe_saved(SAVE_CLIP, false, why);
        sound_event(SOUND_FAILED);
        toast_save_failed(SAVE_CLIP, why);
        return;
    }
    i64 to = now_us(), from = to - secs * 1000000, oldest = g_app.recorder.oldest_us;
    bool trimmed = oldest && from < oldest && oldest < to;
    u32 id = save_range(trimmed ? oldest : from, to, SAVE_CLIP, "");
    if (!id) return;
    save_view(id)->trimmed = trimmed;
    log_line("save %u: last %lld s queued", id, secs);
}

// Saves the wall-clock stretch [from_us, to_us] as a clip: "save <from> <to>" from the pipe or the command line,
// and achievement clips (`name` for its {game}; "" = the game played most). An end in the future is cut to now,
// and a start before the oldest footage is moved up to it (trimmed).
static void save_exact(i64 from_us, i64 to_us, const char *name = "")
{
    i64 now = now_us(), oldest = g_app.recorder.oldest_us;
    if (to_us > now) {
        log_line("save: the range ends %.1f s from now; it ends now instead", (to_us - now) / 1e6);
        to_us = now;
    }
    if (to_us <= from_us || (oldest && to_us <= oldest)) {
        log_line("save: range is no longer in the buffer");
        pipe_saved(SAVE_CLIP, false, "range is no longer in the buffer");
        return;
    }
    bool trimmed = oldest && from_us < oldest;
    if (trimmed) from_us = oldest;
    u32 id = save_range(from_us, to_us, SAVE_CLIP, name);
    if (!id) {
        pipe_saved(SAVE_CLIP, false, "the clip couldn't be queued");
        return;
    }
    save_view(id)->trimmed = trimmed;
    SYSTEMTIME a = local_time(from_us), b = local_time(to_us);
    log_line("save %u: range %02d:%02d:%02d.%03d to %02d:%02d:%02d.%03d queued%s", id, a.wHour, a.wMinute, a.wSecond,
             a.wMilliseconds, b.wHour, b.wMinute, b.wSecond, b.wMilliseconds,
             trimmed ? " (the buffer starts later, so from there)" : "");
}

// Cancels a queued or running job. A cancelled save leaves no file behind.
static void save_cancel(u32 id)
{
    SaveJobView *v = save_view(id);
    if (!v || v->state >= SAVE_DONE) return;
    AcquireSRWLockExclusive(&g_save_q.lock);
    bool queued = false;
    for (i32 i = 0; i < g_save_q.count; i++) {
        if (g_save_q.items[i].id != id) continue;
        g_save_q.count--;
        memmove(g_save_q.items + i, g_save_q.items + i + 1, (g_save_q.count - i) * sizeof g_save_q.items[0]);
        queued = true;
        break;
    }
    if (!queued) g_save_q.cancel_id = id; // running (or just taken): the worker stops it and reports
    ReleaseSRWLockExclusive(&g_save_q.lock);
    if (queued) {
        v->state = SAVE_CANCELLED;
        g_app.saving.jobs_running--;
        log_line("save %u: cancelled", id);
    }
}

// At exit: a running save is cancelled (ffmpeg killed, its part file removed). Queued ones are dropped.
static void save_stop()
{
    if (!g_save_q.thread) return;
    AcquireSRWLockExclusive(&g_save_q.lock);
    g_save_q.stop = true;
    WakeConditionVariable(&g_save_q.wake);
    ReleaseSRWLockExclusive(&g_save_q.lock);
    if (WaitForSingleObject(g_save_q.thread, 5000) != WAIT_OBJECT_0) log_line("save: the worker didn't stop in time");
    CloseHandle(g_save_q.thread);
    g_save_q.thread = 0;
}

// Splits on tabs, keeping empty fields (saved.tsv here, sessions.txt in sessions.cpp). Returns how many.
static i32 split_tabs(char *s, char **fields, i32 max)
{
    i32 n = 0;
    fields[n++] = s;
    while (n < max && (s = strchr(s, '	')) != 0) {
        *s++ = 0;
        fields[n++] = s;
    }
    return n;
}

// What's been saved from the buffer, for the timeline: <local_dir>\saved.tsv, a line per clip or session,
// "from_us <tab> to_us <tab> clip|session <tab> path". Appended as saves finish; lines older than 30 days are dropped
// when it's read.
static const i64 SAVED_KEEP_US = 30LL * 86400 * 1000000;

static void saved_write_all()
{
    const Saving &sv = g_app.saving;
    FILE *f = _wfopen(path_join(g_app.paths.local_dir, L"saved.tsv").s, L"wb");
    if (!f) return;
    for (i32 i = 0; i < sv.saved_count; i++) {
        const SavedRange &r = sv.saved[i];
        fprintf(f, "%lld\t%lld\t%s\t%s\n", (long long)r.from_us, (long long)r.to_us,
                r.kind == SAVE_SESSION ? "session" : "clip", r.path);
    }
    fclose(f);
}

static void saved_load()
{
    Saving &sv = g_app.saving;
    if (sv.saved_loaded) return;
    sv.saved_loaded = true;
    FILE *f = _wfopen(path_join(g_app.paths.local_dir, L"saved.tsv").s, L"rb");
    if (!f) return;
    i64 oldest = now_us() - SAVED_KEEP_US;
    bool dropped = false;
    char line[700];
    while (fgets(line, sizeof line, f)) {
        size_t len = strlen(line);
        while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = 0;
        char *fl[4];
        if (split_tabs(line, fl, COUNT(fl)) != 4) continue;
        SavedRange r = {};
        r.from_us = strtoll(fl[0], 0, 10);
        r.to_us = strtoll(fl[1], 0, 10);
        r.kind = !strcmp(fl[2], "session") ? SAVE_SESSION : SAVE_CLIP;
        str_set(r.path, fl[3]);
        if (r.to_us <= r.from_us || r.to_us < oldest) {
            dropped = true;
            continue;
        }
        if (sv.saved_count == COUNT(sv.saved)) { // full: the oldest goes
            memmove(sv.saved, sv.saved + 1, (COUNT(sv.saved) - 1) * sizeof sv.saved[0]);
            sv.saved_count--;
            dropped = true;
        }
        sv.saved[sv.saved_count++] = r;
    }
    fclose(f);
    if (dropped) saved_write_all();
}

static void saved_add(SaveKind kind, i64 from_us, i64 to_us, const char *path)
{
    if (kind != SAVE_CLIP && kind != SAVE_SESSION) return;
    Saving &sv = g_app.saving;
    saved_load();
    bool full = sv.saved_count == COUNT(sv.saved);
    if (full) {
        memmove(sv.saved, sv.saved + 1, (COUNT(sv.saved) - 1) * sizeof sv.saved[0]);
        sv.saved_count--;
    }
    SavedRange &r = sv.saved[sv.saved_count++];
    r = {};
    r.from_us = from_us;
    r.to_us = to_us;
    r.kind = kind;
    str_set(r.path, path);
    if (full) {
        saved_write_all();
        return;
    }
    FILE *f = _wfopen(path_join(g_app.paths.local_dir, L"saved.tsv").s, L"ab");
    if (!f) return;
    fprintf(f, "%lld\t%lld\t%s\t%s\n", (long long)from_us, (long long)to_us, kind == SAVE_SESSION ? "session" : "clip", path);
    fclose(f);
}

// Marks the saves whose file has been deleted or renamed, at most every few seconds (the timeline calls it).
static void saved_check()
{
    Saving &sv = g_app.saving;
    saved_load();
    u64 t = GetTickCount64();
    if (sv.saved_checked && t - sv.saved_checked < 3000) return;
    sv.saved_checked = t;
    for (i32 i = 0; i < sv.saved_count; i++) {
        wchar_t w[MAX_PATH * 2];
        utf8_to_wide(sv.saved[i].path, w, COUNT(w));
        sv.saved[i].gone = GetFileAttributesW(w) == INVALID_FILE_ATTRIBUTES;
    }
}

static void toast_saved(SaveKind kind, const char *path, const char *game); // toast.cpp
static void toast_save_failed(SaveKind kind, const char *why);
static void render_on_save(u32 id, i64 what, const char *text); // render.cpp

static void save_on_event(const Event &e)
{
    u32 id = (u32)(e.b & 0xffffffff);
    i64 value = e.b >> 32;
    SaveJobView *v = save_view(id);
    if (!v) return;
    if (v->kind == SAVE_RENDER && e.a >= SAVE_EV_DONE) {
        // Not a saved file: render.cpp swaps it in, nothing is announced
        v->state = e.a == SAVE_EV_DONE ? SAVE_DONE : e.a == SAVE_EV_FAILED ? SAVE_FAILED : SAVE_CANCELLED;
        g_app.saving.jobs_running--;
        if (e.a == SAVE_EV_FAILED) log_line("save %u: render failed: %s", id, e.text);
        render_on_save(id, e.a, e.text);
        return;
    }
    switch (e.a) {
    case SAVE_EV_STARTED: v->state = SAVE_RUNNING; break;
    case SAVE_EV_PATH: str_set(v->path, e.text); break;
    case SAVE_EV_GAME: str_set(v->game, e.text); break;
    case SAVE_EV_PROGRESS: v->progress = (f32)value / 1000.0f; break;
    case SAVE_EV_DONE:
        v->state = SAVE_DONE;
        v->progress = 1;
        str_set(v->path, e.text);
        if (v->kind != SAVE_PREVIEW && v->kind != SAVE_LINK) str_set(g_app.saving.last_saved, e.text);
        g_app.saving.jobs_running--;
        log_line("save %u: saved %s", id, e.text);
        saved_add(v->kind, v->from_us, v->to_us, e.text);
        pipe_saved(v->kind, true, e.text, v);
        if (v->quiet) break;
        if (v->kind == SAVE_SESSION) sound_event(SOUND_SESSION_SAVED);
        else if (v->kind != SAVE_PREVIEW && v->kind != SAVE_LINK) sound_event(SOUND_CLIP_SAVED);
        toast_saved(v->kind, e.text, v->game);
        break;
    case SAVE_EV_FAILED:
        v->state = SAVE_FAILED;
        str_set(v->error, e.text);
        g_app.saving.jobs_running--;
        log_line("save %u: failed: %s", id, e.text);
        pipe_saved(v->kind, false, e.text, v);
        if (v->quiet) break;
        if (v->kind != SAVE_PREVIEW && v->kind != SAVE_LINK) sound_event(SOUND_FAILED);
        toast_save_failed(v->kind, e.text);
        break;
    case SAVE_EV_CANCELLED:
        v->state = SAVE_CANCELLED;
        g_app.saving.jobs_running--;
        log_line("save %u: cancelled", id);
        break;
    }
}
