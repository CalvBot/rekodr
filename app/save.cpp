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
    SAVE_EV_PROGRESS, // value: progress in 1/1000
    SAVE_EV_DONE,     // text: the file saved
    SAVE_EV_FAILED,   // text: why
    SAVE_EV_CANCELLED,
};

static const i64 SEGMENT_US = 2000000; // video segment length
static const char *DESKTOP = "Desktop";

// What the worker needs from Config and Paths, copied on the main thread with every job.
struct SaveSettings {
    Path    buffer_dir;
    Path    save_dir;
    Path    ffmpeg;   // empty = not found
    Path    work_dir; // temp lists and graphs (under local_dir, so test copies keep to CLIPBACK_HOME)
    char    codec[64];
    u8      vendor; // the H.264 encoder Keep and Share use when they re-encode (Vendor)
    bool    voice_mono;
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
    EditJob  edit;      // SAVE_KEEP and SAVE_SHARE
};

// The only data shared between the main thread and the worker, always under `lock`.
struct SaveQueue {
    SRWLOCK            lock;
    CONDITION_VARIABLE wake;
    SaveRequest        items[32];
    i32                count;
    SaveSettings       settings;  // from the newest save_range: a job runs with the settings of when it started
    u32                cancel_id; // the running job should stop
    bool               stop;
    HANDLE             thread;
};

static SaveQueue g_save_q;

// ---------------------------------------------------------------------------------------------------------------
// Worker side. There's one worker, so its data is plain statics.

struct Piece {
    i64 start_us, end_us; // wall clock
    i64 offset_us;        // where it starts in the saved file
};

struct Track {
    char title[128];
    char exe[64]; // app tracks: the exe; input tracks: the device's endpoint id
    bool input;   // an input other than Windows' default
    bool chat;    // a Chat app (each Discord person is an exe of their own)
    bool mono;    // kept in mono (Options: voices in mono, or the input's own setting)
    bool game;
    i32  rank;    // position in time played (most first); INT32_MAX for apps
};

struct SavePart { i64 from, to, delay; }; // one stretch of a stream used in one piece: µs into the stream, delay

struct SaveWork {
    BufferIdx    buf;
    SaveSettings cfg;
    SaveRequest  req;
    Piece        pieces[512];
    i32          piece_count;
    i32          streams[128];      // indexes into buf.streams of those overlapping the clip
    i32          stream_track[128]; // the track each goes on, -1 = none (ignored apps)
    i32          stream_parts[128]; // stretches of it used (0 = it only overlaps a gap)
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

// A readable name: the one set in Options, else "Overwatch" from overwatch.exe.
static void display_name(const SaveSettings &c, const char *exe, char *out, i32 cap)
{
    const AppInfo *a = save_app(c, exe);
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

static SYSTEMTIME local_time(i64 us)
{
    i64 t = us * 10 + 116444736000000000LL;
    FILETIME ft = {(DWORD)t, (DWORD)(t >> 32)};
    SYSTEMTIME utc, local;
    FileTimeToSystemTime(&ft, &utc);
    if (!SystemTimeToTzSpecificLocalTime(0, &utc, &local)) local = utc;
    return local;
}

// A time of day the way Options › General sets it: "9:04 pm" or "21:04" ("9:04:05 pm", "21:04:05" with seconds).
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

// Mic (Windows' default input), then one track per other input, then per app: chat apps and people (in order of
// first sound), games (the one played most first), then the rest in order of first sound. Every stream of one
// device or exe goes on its track.
static void make_tracks(SaveWork *w)
{
    w->track_count = 1;
    w->tracks[0] = {};
    str_set(w->tracks[0].title, "Mic");
    w->tracks[0].mono = w->cfg.voice_mono;
    for (i32 i = 0; i < w->stream_count; i++) {
        const AudioStreamInfo &st = w->buf.streams[w->streams[i]];
        w->stream_track[i] = -1;
        if (st.mic && !st.path[0]) { w->stream_track[i] = 0; continue; }
        AppCategory cat = st.mic ? CAT_NONE : save_category(w->cfg, st.exe, st.path);
        if (cat == CAT_IGNORED) continue;
        const char *key = st.mic ? st.path : st.exe;
        i32 t = 1;
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
    for (i32 i = 2; i < w->track_count; i++) {
        Track tr = w->tracks[i];
        i32 old = order[i];
        i32 j = i;
        for (; j > 1 && track_before(tr, w->tracks[j - 1]); j--) {
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

// The filter graph for the audio tracks. Input 0 is the video; stream i is input i + 1. A stream's concat input
// starts at its first remaining segment, and each stretch of it is trimmed and delayed to its place in the file.
static void make_graph(SaveWork *w, i64 total_us)
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
        i64 base, avail_end;
        stream_span(w->buf, w->streams[i], &base, &avail_end);
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
    for (i32 t = 0; t < w->track_count; t++) {
        i32 inputs = 0;
        for (i32 i = 0; i < w->stream_count; i++)
            if (w->stream_track[i] == t) inputs += w->stream_parts[i];
        if (inputs == 0) {
            graph_add(w, "anullsrc=r=48000:cl=stereo,atrim=duration=%s[t%d];\n", total, t);
        } else {
            for (i32 i = 0; i < w->stream_count; i++)
                if (w->stream_track[i] == t)
                    for (i32 j = 0; j < w->stream_parts[i]; j++) graph_add(w, "[p%d_%d]", i, j);
            if (inputs > 1) graph_add(w, "amix=inputs=%d:normalize=0:dropout_transition=0,", inputs);
            graph_add(w, "apad=whole_dur=%s,atrim=duration=%s[t%d];\n", total, total, t);
        }
        // The Mix takes the track as it is; a mono track is only its own copy, both channels averaged.
        if (w->tracks[t].mono)
            graph_add(w, "[t%d]asplit=2[c%d][m%d];\n[c%d]pan=mono|c0=0.5*c0+0.5*c1[o%d];\n", t, t, t, t, t);
        else graph_add(w, "[t%d]asplit=2[o%d][m%d];\n", t, t, t);
    }
    for (i32 t = 0; t < w->track_count; t++) graph_add(w, "[m%d]", t);
    graph_add(w,
              "amix=inputs=%d:normalize=0:dropout_transition=0,alimiter=limit=0.98:level=0:latency=1,"
              "atrim=duration=%s[mix]",
              w->track_count, total);
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
            } else if (starts_with(line, "out_time_us=") && total_us > 0) {
                i64 t = strtoll(line + 12, 0, 10);
                i64 permille = t <= 0 ? 0 : t >= total_us ? 1000 : t * 1000 / total_us;
                if (permille != last_permille) {
                    save_post(SAVE_EV_PROGRESS, w->req.id, permille, 0);
                    last_permille = permille;
                }
            }
        }
    }
    i32 code = proc_wait(&p, INFINITE);
    proc_close(&p);
    return cancelled ? -2 : code;
}

static void remove_work_files(const Path &dir, i32 streams)
{
    DeleteFileW(path_join(dir, L"video.txt").s);
    DeleteFileW(path_join(dir, L"graph.txt").s);
    for (i32 i = 0; i < streams; i++) DeleteFileW(work_file(dir, L"audio%d.txt", i).s);
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
static i64 edit_run(SaveWork *w, char *out, i32 out_cap); // Keep and Share, below

static i64 save_run(SaveWork *w, char *out, i32 out_cap)
{
    const SaveRequest &req = w->req;
    out[0] = 0;
    if (!w->cfg.ffmpeg.s[0]) {
        snprintf(out, out_cap, "ffmpeg not found (install it with winget install Gyan.FFmpeg, or set it in Options)");
        return SAVE_EV_FAILED;
    }
    if (req.kind == SAVE_KEEP || req.kind == SAVE_SHARE) return edit_run(w, out, out_cap);

    // The segment covering `to` is still being written: wait for it to finish (a couple of seconds at most), so the
    // clip ends at the moment the save was asked for. "Being written" is the recorder saying it's capturing, or a
    // list.csv written moments ago (a recorder outside this process, such as the Rust version).
    u64 deadline = GetTickCount64() + SEGMENT_US / 1000 + 2500;
    for (bool wait = req.kind != SAVE_PREVIEW; wait;) { // a preview takes what's there
        i64 end = 0, written = 0;
        bool known = buffer_video_newest_end(w->cfg.buffer_dir, &end, &written);
        bool live = req.recording || (known && now_us() - written < SEGMENT_US + 3000000);
        if (!live || end >= req.to_us - 100000 || GetTickCount64() > deadline) break;
        if (save_cancelled(req.id)) return SAVE_EV_CANCELLED;
        Sleep(150);
    }

    if (!buffer_read(w->cfg.buffer_dir, &w->buf)) {
        snprintf(out, out_cap, "out of memory reading the buffer");
        return SAVE_EV_FAILED;
    }
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
        w->streams[w->stream_count++] = s;
    }

    // The game played most names and tags it; no game at all is "Desktop".
    w->game_count = games_in(b, clip_start, clip_end, w->games, COUNT(w->games));
    make_tracks(w);
    char game[96], names[1024] = {}, exes[1024] = {};
    if (w->game_count) display_name(w->cfg, w->games[0].exe, game, sizeof game);
    else str_set(game, DESKTOP);
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

    // <save_dir>\<year>\Clips\<label> 2026-11-15 21-04-10.mp4, or ...\Sessions\<label> 2026-11-15 19-02.mp4. The
    // year is the year the footage starts in.
    // A preview goes in the local folder as preview-<id>.mp4.
    SYSTEMTIME t = local_time(clip_start);
    bool session = req.kind == SAVE_SESSION, preview = req.kind == SAVE_PREVIEW;
    Path dir = {};
    if (preview) dir = w->cfg.work_dir;
    else swprintf(dir.s, MAX_PATH, L"%s\\%d\\%s", w->cfg.save_dir.s, t.wYear, session ? L"Sessions" : L"Clips");
    int made = SHCreateDirectoryExW(0, dir.s, 0);
    if (made != ERROR_SUCCESS && made != ERROR_ALREADY_EXISTS && made != ERROR_FILE_EXISTS) {
        char d[MAX_PATH * 3];
        wide_to_utf8(dir.s, d, sizeof d);
        snprintf(out, out_cap, "couldn't create %s (%d)", d, made);
        return SAVE_EV_FAILED;
    }
    char stem[400];
    if (preview) snprintf(stem, sizeof stem, "preview-%u", req.id);
    else if (session)
        snprintf(stem, sizeof stem, "%s %04d-%02d-%02d %02d-%02d", label, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute);
    else
        snprintf(stem, sizeof stem, "%s %04d-%02d-%02d %02d-%02d-%02d", label, t.wYear, t.wMonth, t.wDay, t.wHour,
                 t.wMinute, t.wSecond);
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
        for (i32 k = 0; k < st.seg_count; k++) {
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
    make_graph(w, total_us);
    f = w->graph_full ? 0 : _wfopen(path_join(work, L"graph.txt").s, L"wb");
    if (!f) {
        snprintf(out, out_cap, w->graph_full ? "the audio graph is too big" : "couldn't write the save's work files");
        remove_work_files(work, w->stream_count);
        return SAVE_EV_FAILED;
    }
    fwrite(w->graph, 1, w->graph_len, f);
    fclose(f);

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
    cmd_arg8(a, cap, "-/filter_complex");
    cmd_arg(a, cap, path_join(work, L"graph.txt").s);
    arg_pair(a, cap, "-map", "0:v");
    arg_pair(a, cap, "-c:v", "copy");
    if (!strcmp(w->cfg.codec, "hevc")) arg_pair(a, cap, "-tag:v", "hvc1"); // so Apple and browser players take it
    arg_pair(a, cap, "-map", "[mix]");
    for (i32 tr = 0; tr < w->track_count; tr++) arg_pair(a, cap, "-map", "[o%d]", tr);
    arg_pair(a, cap, "-c:a", "aac");
    arg_pair(a, cap, "-b:a", "192k");
    arg_pair(a, cap, "-ar", "48000");
    for (i32 i = 0; i <= w->track_count; i++) {
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
    arg_pair(a, cap, "-metadata", "title=%s \xC2\xB7 %d %.3s %d %s", label, t.wDay, month_name(t.wMonth), t.wYear, hm);
    arg_pair(a, cap, "-metadata", "genre=%s", game);
    arg_pair(a, cap, "-metadata", "description=%s", names[0] ? names : DESKTOP);
    arg_pair(a, cap, "-metadata", "comment=clipback; games=%s", exes);
    arg_pair(a, cap, "-movflags", "+faststart");
    arg_pair(a, cap, "-f", "mp4");
    cmd_arg(a, cap, part_path.s);
    // proc_start's command line holds 32767 characters with the exe; cut short, ffmpeg would get nonsense.
    if (wcslen(a) + MAX_PATH + 8 >= 32767) {
        snprintf(out, out_cap, "too many audio streams for one command line");
        remove_work_files(work, w->stream_count);
        return SAVE_EV_FAILED;
    }

    log_line("save %u: %.1f s from %lld, %d video segments in %d pieces, %d audio streams on %d tracks -> %s",
             req.id, (f64)total_us / 1e6, clip_start, end - first, w->piece_count, w->stream_count,
             w->track_count + 1, final8);
    i32 code = run_ffmpeg(w, total_us, out, out_cap);
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
            // The graph is the usual suspect: keep it for a look.
            CopyFileW(path_join(work, L"graph.txt").s, path_join(w->cfg.work_dir, L"last-failed-graph.txt").s, FALSE);
            result = SAVE_EV_FAILED;
        }
    }
    remove_work_files(work, w->stream_count);
    return result;
}

// ---------------------------------------------------------------------------------------------------------------
// Keep and Share: an edit of a saved file (ported from the Rust version's render.rs).
//
// Keep is the edit at full quality into <year>\Clips, every track kept: the video is stream-copied when there's no
// zoom (the trim is then cut at the keyframe before the start), else re-encoded at a constant quality; the audio is
// copied as it is when no volume was touched, else the Mix is rebuilt from the edited tracks and the separate
// tracks are copied as recorded. Share is a small file in the share folder: zoomed, scaled down and encoded to the
// size asked for, with one stereo mix of the edited tracks.

// Whether a file has the clipback layout: track 1 is a Mix of the tracks after it.
static bool edit_has_mix(const MediaInfo &m) { return m.track_count >= 2 && !strcmp(m.tracks[0], "Mix"); }

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

// A track's volume (or the mix's, at EDIT_MASTER) at `t`, before muting: its keys, holding before the first and
// after the last, or its one level when it has none.
static f64 vol_at(const EditRecord &e, i32 track, f64 t)
{
    i32 n = e.vol_count[track];
    const VolKey *k = e.vol[track];
    if (!n) return 1.0 + e.boost[track];
    if (t <= k[0].t) return k[0].v;
    for (i32 i = 1; i < n; i++)
        if (t < k[i].t) {
            f64 span = k[i].t - k[i - 1].t;
            return k[i - 1].v + (k[i].v - k[i - 1].v) * ease_curve(k[i - 1].ease, k[i].ease, (t - k[i - 1].t) / (span > 0.001 ? span : 0.001));
        }
    return k[n - 1].v;
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

// One of a keyframe's values (0 z, 1 x, 2 y) at time `t` (an ffmpeg expression), eased between keys (smoothstep).
static void smooth_expr(SaveWork *w, const EditRecord &e, i32 field, const char *t)
{
    auto val = [&](i32 k) { return field == 0 ? e.keys[k].z : field == 1 ? e.keys[k].x : e.keys[k].y; };
    i32 n = e.key_count;
    for (i32 k = 0; k + 1 < n; k++) {
        f64 ta = e.keys[k].t, tb = e.keys[k + 1].t, a = val(k), b = val(k + 1);
        f64 span = tb - ta > 0.001 ? tb - ta : 0.001;
        char u[160], c[560];
        snprintf(u, sizeof u, "clip((%s-%.4f)/%.4f,0,1)", t, ta, span);
        if (ease_expr(e.keys[k].ease, e.keys[k + 1].ease, u, c, sizeof c))
            graph_add(w, "if(lt(%s,%.4f),(%.5f+(%.5f)*(%s)),", t, tb, a, b - a, c);
        else graph_add(w, "if(lt(%s,%.4f),%.5f,", t, tb, a);
    }
    graph_add(w, "%.5f", val(n - 1));
    for (i32 k = 0; k + 1 < n; k++) graph_add(w, ")");
}

// A volume filter for a track (or the mix at EDIT_MASTER) cut from `start` seconds: a plain level, or a ramp
// between its keys evaluated every frame (ffmpeg's t starts at 0 at the cut).
static void vol_filter(SaveWork *w, const EditRecord &e, i32 track, f64 start)
{
    i32 n = e.vol_count[track];
    const VolKey *k = e.vol[track];
    if (!n) {
        graph_add(w, "volume=%.4f", 1.0 + e.boost[track]);
        return;
    }
    char t[48];
    snprintf(t, sizeof t, "(t+%.4f)", start);
    graph_add(w, "volume='if(lt(%s,%.4f),%.4f,", t, k[0].t, k[0].v);
    for (i32 i = 1; i < n; i++) {
        f64 span = k[i].t - k[i - 1].t;
        char u[128], c[480];
        snprintf(u, sizeof u, "clip((%s-%.4f)/%.4f,0,1)", t, k[i - 1].t, span > 0.001 ? span : 0.001);
        if (span < 0.001 || !ease_expr(k[i - 1].ease, k[i].ease, u, c, sizeof c))
            graph_add(w, "if(lt(%s,%.4f),%.4f,", t, k[i].t, span < 0.001 ? k[i].v : k[i - 1].v);
        else graph_add(w, "if(lt(%s,%.4f),%.4f+(%.4f)*(%s),", t, k[i].t, k[i - 1].v, k[i].v - k[i - 1].v, c);
    }
    graph_add(w, "%.4f", k[n - 1].v);
    for (i32 i = 0; i < n; i++) graph_add(w, ")");
    graph_add(w, "':eval=frame");
}

// The zoom as a perspective filter that crops the view and stretches it to the whole frame, each frame.
static void zoom_filter(SaveWork *w, const EditRecord &e, f64 fps, f64 start)
{
    char t[64];
    snprintf(t, sizeof t, "(in/%.4f+%.4f)", fps > 1 ? fps : 30, start);
    // The view's top-left corner, kept inside the picture: x = clip(cx*W - W/(2z), 0, W - W/z).
    auto z = [&]() { graph_add(w, "max(1,"); smooth_expr(w, e, 0, t); graph_add(w, ")"); };
    auto corner = [&](const char *dim, i32 field, bool far_edge) {
        graph_add(w, "clip(");
        smooth_expr(w, e, field, t);
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

// The audio of a share, or Keep's rebuilt Mix, as [a]. Returns false if the file has no audio.
static bool edit_audio_graph(SaveWork *w, const EditJob &j)
{
    const MediaInfo &m = j.info;
    if (m.track_count <= 0) return false;
    if (!edit_audio_touched(j.edit, m)) {
        graph_add(w, "[0:a:0]anull[a];\n");
        return true;
    }
    i32 first = edit_has_mix(m) ? 1 : 0, n = 0;
    f64 start = j.edit.in_s > 0 ? j.edit.in_s : 0;
    for (i32 t = first; t < m.track_count && t < MEDIA_MAX_TRACKS; t++) {
        if (j.edit.muted[t]) continue;
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

// How Share fits a size (ported from Compress for Discord's Planner). The size is in MiB, which is what Discord
// means by MB, less a 3% margin and ~2% for the mp4 container. Audio gets a tier by the bitrate there is; the video
// gets the rest, never more than the source has (that only makes a bigger file). The picture steps down with the
// bitrate (1080p, 720p, 480p, and 30 fps under 3 Mbps), and further when there are too few bits for its pixels:
// below 0.009 bits per pixel the encoder is at its coarsest and ignores a lower bitrate, so the file comes out over.
enum { SHARE_MIN_VIDEO_KBPS = 80, SHARE_MIN_SIDE = 144, SHARE_ATTEMPTS = 4 };
static const f64 SHARE_MIB = 1024.0 * 1024.0, SHARE_MIN_BPP = 0.009, SHARE_MARGIN = 0.03;

struct SharePlan {
    i64  limit_bytes, target_bytes; // what was asked, and what's aimed for
    i32  video_kbps, audio_kbps;
    i32  side;           // the picture's short side
    f64  fps;            // its frame rate
    bool shrunk;         // smaller than the bitrate tier, for want of bits
    bool capped;         // held to the source's own bitrate
    bool fits;           // false: too long for the size even at the lowest quality
    f64  max_duration;   // the longest that fits at that lowest quality
    f64  estimate_bytes; // what it should come out at
};

// `src_kbps` is the source file's whole bitrate (0 = unknown); `w`, `h` and `fps` are its picture.
static SharePlan share_plan(f64 duration, f64 mb, bool audio, i32 w, i32 h, f64 fps, f64 src_kbps)
{
    SharePlan p = {};
    p.limit_bytes = (i64)(mb * SHARE_MIB);
    p.target_bytes = (i64)(p.limit_bytes * (1 - SHARE_MARGIN));
    f64 budget = p.target_bytes * 8 * 0.98 / 1000; // kbits
    f64 total = budget / (duration > 0.1 ? duration : 0.1);
    p.audio_kbps = !audio ? 0 : total >= 1500 ? 160 : total >= 600 ? 128 : total >= 250 ? 96 : 64;
    f64 video = total - p.audio_kbps;
    if (video > 200000) video = 200000;
    p.video_kbps = (i32)video;
    f64 src_video = src_kbps - p.audio_kbps;
    p.capped = src_video > 0 && p.video_kbps > src_video;
    if (p.capped) p.video_kbps = (i32)ceil(src_video);

    i32 short_side = w < h ? w : h, long_side = w < h ? h : w;
    if (short_side <= 0) short_side = 1080, long_side = 1920;
    f64 aspect = (f64)long_side / short_side;
    p.fps = fps > 0 ? fps : 30;
    p.side = short_side;
    if (!p.capped) {
        i32 tier = p.video_kbps < 1200 ? 480 : p.video_kbps < 3500 ? 720 : 1080;
        if (tier < p.side) p.side = tier;
        if (p.video_kbps < 3000 && p.fps > 31) p.fps = 30;
        if (p.video_kbps * 1000.0 / ((f64)p.side * p.side * aspect * p.fps) < SHARE_MIN_BPP) {
            i32 fit = (i32)sqrt(p.video_kbps * 1000.0 / (SHARE_MIN_BPP * aspect * p.fps)) & ~1;
            if (fit < SHARE_MIN_SIDE) fit = SHARE_MIN_SIDE;
            if (fit < p.side) { p.side = fit; p.shrunk = true; }
        }
    }
    p.fits = p.capped || p.video_kbps >= SHARE_MIN_VIDEO_KBPS;
    p.max_duration = budget / (SHARE_MIN_VIDEO_KBPS + (audio ? 64 : 0));
    p.estimate_bytes = (p.video_kbps + p.audio_kbps) * 125.0 * duration;
    if (p.estimate_bytes > p.target_bytes) p.estimate_bytes = (f64)p.target_bytes;
    return p;
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

static i64 edit_run(SaveWork *w, char *out, i32 out_cap)
{
    const SaveRequest &req = w->req;
    const EditJob &j = req.edit;
    const MediaInfo &m = j.info;
    bool share = req.kind == SAVE_SHARE;
    f64 in_s = j.edit.in_s > 0 ? j.edit.in_s : 0;
    f64 out_s = j.edit.out_s > in_s ? j.edit.out_s : m.duration;
    f64 dur = out_s - in_s;
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

    bool zoom = edit_zoomed(j.edit);
    bool touched = edit_audio_touched(j.edit, m);
    bool reencode = share || zoom;
    SharePlan plan = {};
    i32 video_kbps = 0, side = 0;
    bool reduce_fps = false;
    if (share) {
        WIN32_FILE_ATTRIBUTE_DATA fa = {};
        f64 src_kbps = 0;
        if (GetFileAttributesExW(j.src.s, GetFileExInfoStandard, &fa) && m.duration > 0)
            src_kbps = (f64)(((u64)fa.nFileSizeHigh << 32) | fa.nFileSizeLow) * 8 / 1000 / m.duration;
        plan = share_plan(dur, j.share_mb > 1 ? j.share_mb : 1, m.track_count > 0, m.width, m.height, m.fps, src_kbps);
        if (!plan.fits) {
            i32 most = (i32)plan.max_duration;
            snprintf(out, out_cap, "too long for %g MB: trim it to under %d:%02d", j.share_mb, most / 60, most % 60);
            return SAVE_EV_FAILED;
        }
        video_kbps = plan.video_kbps;
        side = plan.side;
        reduce_fps = plan.fps < m.fps - 0.5;
    }
    bool portrait = m.height > m.width;
    Path work = work_file(w->cfg.work_dir, L"edit-%d", (i32)req.id);
    SHCreateDirectoryExW(0, work.s, 0);
    Path graph = path_join(work, L"graph.txt");
    i32 code = 0;
    i64 last_kbps = 0, last_size = 0;
    f64 audio_bytes = plan.audio_kbps * 125.0 * dur;
    for (i32 attempt = 0;; attempt++) {
        // The graph: video (when re-encoded) as [v], audio (when re-mixed, or for a share) as [a]. Built again for each
        // attempt at a share, since a retry can make the picture smaller.
        w->graph_len = 0;
        w->graph_full = false;
        w->graph[0] = 0;
        if (reencode) {
            graph_add(w, "[0:v]");
            if (zoom) zoom_filter(w, j.edit, m.fps, in_s);
            if (share) {
                if (portrait) graph_add(w, "scale='min(%d,iw)':-2:flags=lanczos,", side);
                else graph_add(w, "scale=-2:'min(%d,ih)':flags=lanczos,", side);
                if (reduce_fps) graph_add(w, "fps=%g,", plan.fps);
            }
            graph_add(w, "format=%s[v];\n", w->cfg.vendor == VENDOR_QSV ? "nv12" : "yuv420p");
        }
        bool mix = (share || touched) && edit_audio_graph(w, j);
        if (w->graph_full) {
            snprintf(out, out_cap, "the zoom has too many keyframes");
            RemoveDirectoryW(work.s);
            return SAVE_EV_FAILED;
        }
        if (w->graph_len) {
            FILE *f = _wfopen(graph.s, L"wb");
            if (!f) {
                snprintf(out, out_cap, "couldn't write the edit's work files");
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
        arg_pair(a, cap, "-ss", "%.3f", in_s);
        cmd_arg8(a, cap, "-i");
        cmd_arg(a, cap, j.src.s);
        arg_pair(a, cap, "-t", "%.3f", dur);
        if (w->graph_len) {
            cmd_arg8(a, cap, "-/filter_complex");
            cmd_arg(a, cap, graph.s);
        }
        if (reencode) {
            arg_pair(a, cap, "-map", "[v]");
            edit_encoder_args(a, cap, w->cfg.vendor, share ? video_kbps : 0);
        } else {
            arg_pair(a, cap, "-map", "0:v:0");
            arg_pair(a, cap, "-c:v", "copy");
        }
        i32 outs = 0; // audio streams written
        if (share) {
            if (mix) {
                arg_pair(a, cap, "-map", "[a]");
                arg_pair(a, cap, "-c:a", "aac");
                arg_pair(a, cap, "-b:a", "%dk", plan.audio_kbps);
                arg_pair(a, cap, "-ar", "48000");
            }
        } else if (!touched) {
            if (m.track_count > 0) {
                arg_pair(a, cap, "-map", "0:a");
                arg_pair(a, cap, "-c:a", "copy");
            }
        } else {
            // The rebuilt Mix first, then the original tracks as recorded (a clipback file's old Mix is dropped).
            arg_pair(a, cap, "-map", "[a]");
            arg_pair(a, cap, "-c:a:0", "aac");
            arg_pair(a, cap, "-b:a:0", "192k");
            arg_pair(a, cap, "-ar:a:0", "48000");
            outs = 1;
            for (i32 t = edit_has_mix(m) ? 1 : 0; t < m.track_count && t < MEDIA_MAX_TRACKS; t++) {
                arg_pair(a, cap, "-map", "0:a:%d", t);
                char key[32];
                snprintf(key, sizeof key, "-c:a:%d", outs);
                arg_pair(a, cap, key, "copy");
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
        arg_pair(a, cap, "-movflags", "+faststart");
        arg_pair(a, cap, "-f", "mp4");
        cmd_arg(a, cap, part_path.s);

        log_line("%s %u: %.1f s from %.2f s, zoom %d, audio %s, %s -> %s", share ? "share" : "keep", req.id, dur, in_s,
                 zoom ? j.edit.key_count : 0, touched ? "re-mixed" : "as recorded",
                 share ? "re-encoded" : reencode ? "re-encoded" : "copied", final8);
        if (share)
            log_line("share %u: %g MB asked, attempt %d: video %d kbps at %dp%g%s%s, audio %d kbps", req.id,
                     j.share_mb, attempt + 1, video_kbps, side, plan.fps, plan.shrunk ? " (shrunk)" : "",
                     plan.capped ? " (source's rate)" : "", plan.audio_kbps);
        code = run_ffmpeg(w, (i64)(dur * 1e6), out, out_cap);
        if (code != 0 || !share) break;

        // Over the size: again at a lower bitrate, by as much as it was over and a little more (ported from Compress for
        // Discord). When a cut stops helping, or it's far over, the encoder is at its coarsest for this many pixels and
        // only a smaller picture gets it down; at that floor the size goes with the pixel count.
        WIN32_FILE_ATTRIBUTE_DATA fa = {};
        GetFileAttributesExW(part_path.s, GetFileExInfoStandard, &fa);
        i64 size = (i64)(((u64)fa.nFileSizeHigh << 32) | fa.nFileSizeLow);
        log_line("share %u: attempt %d made %.2f MB, aiming for %.2f", req.id, attempt + 1, size / SHARE_MIB,
                 plan.target_bytes / SHARE_MIB);
        if (size <= plan.target_bytes) break;
        if (attempt == SHARE_ATTEMPTS - 1) {
            if (size <= plan.limit_bytes) break;
            snprintf(out, out_cap, "couldn't get it under %g MB: trim it shorter", j.share_mb);
            code = -3;
            break;
        }
        f64 video_made = size - audio_bytes > 1 ? size - audio_bytes : 1;
        f64 room = plan.target_bytes - audio_bytes > 1 ? plan.target_bytes - audio_bytes : 1;
        bool stuck = last_kbps > 0 &&
                     last_size - size < (last_size - audio_bytes) * (1 - (f64)video_kbps / last_kbps) * 0.5;
        i32 next = (i32)floor(video_kbps * (room / video_made) * (w->cfg.vendor == VENDOR_NVENC ? 0.97 : 0.92));
        if (next < 50) next = 50;
        if ((size > plan.target_bytes * 1.4 || stuck || next >= video_kbps) && side > SHARE_MIN_SIDE) {
            i32 smaller = (i32)(side * sqrt(room / video_made) * 0.9) / 2 * 2;
            side = smaller > SHARE_MIN_SIDE ? smaller : SHARE_MIN_SIDE;
            last_kbps = 0;
        } else {
            if (next >= video_kbps) {
                snprintf(out, out_cap, "couldn't get it under %g MB: trim it shorter", j.share_mb);
                code = -3;
                break;
            }
            last_kbps = video_kbps;
            last_size = size;
            video_kbps = next;
        }
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
        ReleaseSRWLockExclusive(&g_save_q.lock);

        save_post(SAVE_EV_STARTED, w->req.id, 0, 0);
        char out[512];
        u64 started = GetTickCount64();
        i64 result = save_run(w, out, sizeof out);
        if (result == SAVE_EV_DONE) log_line("save %u: took %.1f s", w->req.id, (GetTickCount64() - started) / 1000.0);
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
static bool save_job_held(u32 id)
{
    if (id == g_app.editor.keep_job || id == g_app.editor.share_job) return true;
    if (id == g_app.timeline.job || id == g_app.timeline.play_job) return true;
    const Sessions &ss = g_app.sessions;
    for (i32 i = 0; i < COUNT(ss.items); i++)
        if (ss.items[i].state == SES_SAVING && ss.items[i].job == id) return true;
    return false;
}

// Queues a save of the wall-clock range [from_us, to_us]: a clip into <year>\Clips, or a session into
// <year>\Sessions. An empty `name` names it after the game played most. Returns the job id, or 0 if it couldn't be
// queued (no free slot).
static u32 save_queue(SaveKind kind, i64 from_us, i64 to_us, const char *name, const EditJob *edit)
{
    Saving &sv = g_app.saving;
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
    if (edit) r->edit = *edit;
    SaveSettings *s = &g_save_q.settings;
    s->buffer_dir = c.buffer_dir;
    s->save_dir = c.save_dir;
    s->ffmpeg = ffmpeg;
    s->work_dir = g_app.paths.local_dir;
    str_set(s->codec, c.codec.s);
    s->voice_mono = c.voice_mono;
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

// Saves the last `secs` seconds of the buffer as a clip in <save_dir>\<year>\Clips. A start before the oldest
// footage is moved up to it, so the saved line says what the clip holds.
static void save_last(i64 secs)
{
    i64 to = now_us(), from = to - secs * 1000000, oldest = g_app.recorder.oldest_us;
    bool trimmed = oldest && from < oldest && oldest < to;
    u32 id = save_range(trimmed ? oldest : from, to, SAVE_CLIP, "");
    if (!id) return;
    save_view(id)->trimmed = trimmed;
    log_line("save %u: last %lld s queued", id, secs);
}

// Saves the wall-clock stretch [from_us, to_us] as a clip: "save <from> <to>" from the pipe or the command line.
// An end in the future is cut to now, and a start before the oldest footage is moved up to it (trimmed).
static void save_exact(i64 from_us, i64 to_us)
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
    u32 id = save_range(from_us, to_us, SAVE_CLIP, "");
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

static void save_on_event(const Event &e)
{
    u32 id = (u32)(e.b & 0xffffffff);
    i64 value = e.b >> 32;
    SaveJobView *v = save_view(id);
    if (!v) return;
    switch (e.a) {
    case SAVE_EV_STARTED: v->state = SAVE_RUNNING; break;
    case SAVE_EV_PATH: str_set(v->path, e.text); break;
    case SAVE_EV_PROGRESS: v->progress = (f32)value / 1000.0f; break;
    case SAVE_EV_DONE:
        v->state = SAVE_DONE;
        v->progress = 1;
        str_set(v->path, e.text);
        if (v->kind != SAVE_PREVIEW) str_set(g_app.saving.last_saved, e.text);
        g_app.saving.jobs_running--;
        log_line("save %u: saved %s", id, e.text);
        saved_add(v->kind, v->from_us, v->to_us, e.text);
        pipe_saved(v->kind, true, e.text, v);
        break;
    case SAVE_EV_FAILED:
        v->state = SAVE_FAILED;
        str_set(v->error, e.text);
        g_app.saving.jobs_running--;
        log_line("save %u: failed: %s", id, e.text);
        pipe_saved(v->kind, false, e.text, v);
        break;
    case SAVE_EV_CANCELLED:
        v->state = SAVE_CANCELLED;
        g_app.saving.jobs_running--;
        log_line("save %u: cancelled", id);
        break;
    }
}
