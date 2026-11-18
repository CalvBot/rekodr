// Timeline: the buffer on a zoomable, pannable bar, a range picked on it, and Save / Edit / Session. The preview
// sits top left: a still frame at the playhead, or the footage playing with its sound after Play.
//
// Two workers, neither touching g_app:
// - the index worker reads the buffer (buffer_read) every 3 s while the screen is shown. There are two heap blocks
//   of BufferIdx (about 8 MB each): the main thread draws from one while the worker fills the other, and they swap
//   on EV_TIMELINE. Both are freed while the screen isn't shown.
// - the preview worker runs ffmpeg for one small frame of the segment under the wanted time (the newest request
//   wins) and hands back BGRA pixels. The main thread uploads them as D3D11 textures and keeps the last few.
//
// Rows, top to bottom: the time ruler, video (gaps in recording are gaps), the game being played (a band per game,
// from activity.tsv), what has been saved (saved.tsv; a click selects it), then one row per app that made sound (every stream of one exe on one row; a stream lasts
// while the app plays sound, plus 30 s).

enum TimelineEventKind : i64 {
    TL_EV_INDEX,   // b = how long the read took, ms
    TL_EV_PREVIEW, // a frame (or a failure) is waiting in g_tl
};

enum { TL_FRAME_W = 640, TL_FRAME_H = 360, TL_FRAME_BYTES = TL_FRAME_W * TL_FRAME_H * 4 };

static const i64 TL_SEC = 1000000;
static const i64 TL_MIN_SPAN = 10 * TL_SEC;
static const i64 TL_DAY = 24 * 3600 * TL_SEC;
static const i64 TL_DEFAULT_SPAN = 3600 * TL_SEC;

struct TlPreviewReq {
    i64  key;
    bool exact;  // the frame at `offset`; otherwise the segment's first keyframe, which is much quicker
    f64  offset; // seconds into the segment file
    Path file;
    Path ffmpeg;
};

// The only data shared between the main thread and the workers, always under `lock`.
struct TlShared {
    SRWLOCK            lock;
    CONDITION_VARIABLE index_wake, preview_wake;
    HANDLE             index_thread, preview_thread; // started on first use; main thread only
    bool               index_want;
    Path               buffer_dir;
    BufferIdx         *spare; // the block the worker fills next; 0 while it's filling it
    BufferIdx         *ready; // filled, not taken by the main thread yet
    bool               preview_want;
    TlPreviewReq       req;
    bool               result_ready, result_ok;
    i64                result_key;
    u8                 result_px[TL_FRAME_BYTES];
};

static TlShared g_tl;

// ---------------------------------------------------------------------------------------------------------------
// Workers

static DWORD WINAPI tl_index_thread(void *)
{
    static Path dir;
    for (;;) {
        AcquireSRWLockExclusive(&g_tl.lock);
        while (!g_tl.index_want || !g_tl.spare) SleepConditionVariableSRW(&g_tl.index_wake, &g_tl.lock, INFINITE, 0);
        BufferIdx *b = g_tl.spare;
        g_tl.spare = 0;
        g_tl.index_want = false;
        dir = g_tl.buffer_dir;
        ReleaseSRWLockExclusive(&g_tl.lock);

        u64 started = GetTickCount64();
        buffer_read(dir, b);
        AcquireSRWLockExclusive(&g_tl.lock);
        g_tl.ready = b;
        ReleaseSRWLockExclusive(&g_tl.lock);
        Event e = {};
        e.kind = EV_TIMELINE;
        e.a = TL_EV_INDEX;
        e.b = (i64)(GetTickCount64() - started);
        event_post(e);
    }
}

// One frame, scaled and letterboxed to TL_FRAME_W x TL_FRAME_H, as raw BGRA on ffmpeg's stdout.
static bool tl_extract(const TlPreviewReq &r, u8 *px)
{
    static wchar_t args[4096];
    args[0] = 0;
    i32 cap = COUNT(args);
    static const char *const head[] = {"-hide_banner", "-loglevel", "error", "-nostdin"};
    arg_list(args, cap, head, COUNT(head));
    if (r.exact) arg_pair(args, cap, "-ss", "%.3f", r.offset);
    else arg_pair(args, cap, "-skip_frame", "nokey");
    cmd_arg8(args, cap, "-i");
    cmd_arg(args, cap, r.file.s);
    arg_pair(args, cap, "-vf", "scale=%d:%d:force_original_aspect_ratio=decrease,pad=%d:%d:-1:-1", TL_FRAME_W,
             TL_FRAME_H, TL_FRAME_W, TL_FRAME_H);
    static const char *const tail[] = {"-an", "-sn", "-frames:v", "1", "-pix_fmt", "bgra", "-f", "rawvideo", "pipe:1"};
    arg_list(args, cap, tail, COUNT(tail));

    Proc p;
    if (!proc_start(r.ffmpeg.s, args, PROC_STDOUT, &p)) return false;
    i32 got_total = 0;
    u64 deadline = GetTickCount64() + 8000;
    // Polled so a stuck ffmpeg can't hang the worker: it's killed at the deadline, which ends the pipe.
    while (got_total < TL_FRAME_BYTES) {
        DWORD avail = 0;
        if (!PeekNamedPipe(p.out, 0, 0, 0, &avail, 0)) break; // ffmpeg exited and the pipe ended
        if (avail == 0) {
            if (GetTickCount64() > deadline) { proc_kill(&p); break; }
            Sleep(5);
            continue;
        }
        DWORD want = (DWORD)(TL_FRAME_BYTES - got_total), got = 0;
        if (want > avail) want = avail;
        if (!ReadFile(p.out, px + got_total, want, &got, 0) || got == 0) break;
        got_total += (i32)got;
    }
    i32 code = proc_wait(&p, 2000);
    if (code < 0) proc_kill(&p);
    proc_close(&p);
    if (got_total < TL_FRAME_BYTES) {
        char f8[MAX_PATH * 3];
        wide_to_utf8(r.file.s, f8, sizeof f8);
        log_line("timeline: no preview frame from %s at %.3f s (ffmpeg exit %d, %d bytes)", f8, r.offset, code, got_total);
        return false;
    }
    return true;
}

static DWORD WINAPI tl_preview_thread(void *)
{
    static TlPreviewReq req;
    static u8 px[TL_FRAME_BYTES];
    for (;;) {
        AcquireSRWLockExclusive(&g_tl.lock);
        while (!g_tl.preview_want) SleepConditionVariableSRW(&g_tl.preview_wake, &g_tl.lock, INFINITE, 0);
        req = g_tl.req;
        g_tl.preview_want = false;
        ReleaseSRWLockExclusive(&g_tl.lock);

        bool ok = tl_extract(req, px);
        AcquireSRWLockExclusive(&g_tl.lock);
        if (ok) memcpy(g_tl.result_px, px, TL_FRAME_BYTES);
        g_tl.result_ok = ok;
        g_tl.result_key = req.key;
        g_tl.result_ready = true;
        ReleaseSRWLockExclusive(&g_tl.lock);
        Event e = {};
        e.kind = EV_TIMELINE;
        e.a = TL_EV_PREVIEW;
        event_post(e);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Main thread: the index

static BufferIdx *tl_alloc_idx()
{
    return (BufferIdx *)VirtualAlloc(0, sizeof(BufferIdx), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
}

static void tl_ask_index()
{
    Timeline &tl = g_app.timeline;
    if (!tl.idx) {
        BufferIdx *a = tl_alloc_idx(), *b = tl_alloc_idx();
        if (!a || !b) {
            log_line("timeline: out of memory for the buffer index");
            if (a) VirtualFree(a, 0, MEM_RELEASE);
            if (b) VirtualFree(b, 0, MEM_RELEASE);
            return;
        }
        tl.idx = a; // zeroed: an empty index until the first read lands
        tl.loaded = false;
        AcquireSRWLockExclusive(&g_tl.lock);
        g_tl.spare = b;
        ReleaseSRWLockExclusive(&g_tl.lock);
    }
    AcquireSRWLockExclusive(&g_tl.lock);
    g_tl.index_want = true;
    g_tl.buffer_dir = g_app.config.buffer_dir;
    WakeConditionVariable(&g_tl.index_wake);
    ReleaseSRWLockExclusive(&g_tl.lock);
    if (!g_tl.index_thread) g_tl.index_thread = thread_start(tl_index_thread, 0);
}

// Sound rows are grouped: the mic, then games, then other apps.
static i32 tl_row_group(const TimelineRow &r)
{
    if (!strcmp(r.exe, "mic")) return 0;
    return audio_category_at(g_app.config, r.exe, 0) == CAT_GAME ? 1 : 2;
}

// One row per exe, by group, and within a group in the order they were first heard.
static void tl_make_rows()
{
    Timeline &tl = g_app.timeline;
    const BufferIdx &b = *tl.idx;
    tl.row_count = 0;
    for (i32 s = 0; s < b.stream_count && s < COUNT(tl.stream_row); s++) {
        const AudioStreamInfo &st = b.streams[s];
        const char *exe = st.mic ? "mic" : st.exe;
        tl.stream_row[s] = -1;
        if (!st.seg_count) continue;
        if (!st.mic && audio_category_at(g_app.config, exe, st.path) == CAT_IGNORED) continue;
        i32 r = 0;
        while (r < tl.row_count && strcmp(tl.rows[r].exe, exe)) r++;
        if (r == tl.row_count) {
            if (r == COUNT(tl.rows)) continue;
            TimelineRow *row = &tl.rows[tl.row_count++];
            str_set(row->exe, exe);
            if (st.mic) str_set(row->name, "Mic");
            else audio_display_name(g_app.config, exe, row->name, sizeof row->name);
            AppCategory cat = st.mic ? CAT_NONE : audio_category_at(g_app.config, exe, st.path);
            row->rgb = st.mic ? COL_TRACK_MIC : cat == CAT_GAME ? COL_TRACK_GAME
                     : cat == CAT_COMMUNICATION ? COL_TRACK_CHAT : COL_TRACK_APP;
        }
        tl.stream_row[s] = (i16)r;
    }
    // Stable insertion sort by group, remapping stream_row as rows move.
    i32 order[COUNT(tl.rows)];
    for (i32 i = 0; i < tl.row_count; i++) order[i] = i;
    for (i32 i = 1; i < tl.row_count; i++) {
        TimelineRow row = tl.rows[i];
        i32 old = order[i], j = i;
        for (; j > 0 && tl_row_group(row) < tl_row_group(tl.rows[j - 1]); j--) {
            tl.rows[j] = tl.rows[j - 1];
            order[j] = order[j - 1];
        }
        tl.rows[j] = row;
        order[j] = old;
    }
    i32 moved_to[COUNT(tl.rows)];
    for (i32 i = 0; i < tl.row_count; i++) moved_to[order[i]] = i;
    for (i32 s = 0; s < b.stream_count && s < COUNT(tl.stream_row); s++)
        if (tl.stream_row[s] >= 0) tl.stream_row[s] = (i16)moved_to[tl.stream_row[s]];
}

static void tl_take_index(i64 ms)
{
    Timeline &tl = g_app.timeline;
    AcquireSRWLockExclusive(&g_tl.lock);
    BufferIdx *fresh = g_tl.ready;
    if (fresh) {
        g_tl.ready = 0;
        g_tl.spare = tl.idx; // the one drawn so far is filled next
    }
    ReleaseSRWLockExclusive(&g_tl.lock);
    if (!fresh) return;
    if (!tl.loaded) {
        log_line("timeline: buffer read in %lld ms: %d video segments in %d runs, %d audio streams, %d activity lines",
                 ms, fresh->seg_count, fresh->run_count, fresh->stream_count, fresh->activity_count);
    }
    tl.idx = fresh;
    tl.loaded = true;
    tl_make_rows();
}

static void tl_play_stop();

// Opens the timeline on a stretch of the buffer, to look at it before saving: the stretch is selected and fills
// the view, the playhead is at its start, and the session box has its name. The session popup's Review, and the
// unsaved sessions in the Library.
static void timeline_review(i64 from_us, i64 to_us, const char *name)
{
    Timeline &tl = g_app.timeline;
    if (to_us <= from_us) return;
    tl_play_stop();
    i64 span = to_us - from_us;
    tl.sel_in = from_us;
    tl.sel_out = to_us;
    tl.follow = false;
    tl.view_span_us = span + span / 5; // a tenth of room each side
    if (tl.view_span_us < TL_MIN_SPAN) tl.view_span_us = TL_MIN_SPAN;
    tl.view_end_us = to_us + (tl.view_span_us - span) / 2;
    tl.playhead_us = from_us;
    str_set(tl.session_name, name);
    log_line("timeline: reviewing %s", name);
    g_app.ui.screen = SCREEN_TIMELINE;
    ui_wake(4);
}

// Frees the index blocks and preview textures while the screen isn't shown (SPEC: hidden, the window holds nothing).
static void tl_release()
{
    Timeline &tl = g_app.timeline;
    tl_play_stop();
    for (i32 i = 0; i < COUNT(tl.frames); i++) {
        if (tl.frames[i].srv) tl.frames[i].srv->Release();
        tl.frames[i] = {};
    }
    tl.pending_key = tl.shown_key = tl.failed_key = 0;
    if (!tl.idx) return;
    AcquireSRWLockExclusive(&g_tl.lock);
    BufferIdx *spare = g_tl.spare, *ready = g_tl.ready;
    bool idle = spare || ready; // otherwise the worker is reading into the second block
    if (idle) {
        g_tl.spare = g_tl.ready = 0;
        g_tl.index_want = false;
    }
    ReleaseSRWLockExclusive(&g_tl.lock);
    if (!idle) return; // next tick
    VirtualFree(tl.idx, 0, MEM_RELEASE);
    if (spare) VirtualFree(spare, 0, MEM_RELEASE);
    if (ready) VirtualFree(ready, 0, MEM_RELEASE);
    tl.idx = 0;
    tl.loaded = false;
    tl.row_count = 0;
}

// ---------------------------------------------------------------------------------------------------------------
// Main thread: preview frames

static ID3D11ShaderResourceView *tl_upload(const u8 *px)
{
    if (!g_gpu.device) return 0;
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = TL_FRAME_W;
    d.Height = TL_FRAME_H;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_IMMUTABLE;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init = {px, TL_FRAME_W * 4, 0};
    ID3D11Texture2D *tex = 0;
    HRESULT hr = g_gpu.device->CreateTexture2D(&d, &init, &tex);
    if (FAILED(hr)) {
        log_line("timeline: preview texture failed (0x%08x)", (u32)hr);
        return 0;
    }
    ID3D11ShaderResourceView *srv = 0;
    g_gpu.device->CreateShaderResourceView(tex, 0, &srv);
    tex->Release(); // the view keeps it
    return srv;
}

static TimelineFrame *tl_frame_find(i64 key)
{
    Timeline &tl = g_app.timeline;
    for (i32 i = 0; i < COUNT(tl.frames); i++)
        if (key && tl.frames[i].key == key) return &tl.frames[i];
    return 0;
}

static void tl_take_preview()
{
    Timeline &tl = g_app.timeline;
    AcquireSRWLockExclusive(&g_tl.lock);
    bool have = g_tl.result_ready, ok = g_tl.result_ok;
    i64 key = g_tl.result_key;
    ID3D11ShaderResourceView *srv = have && ok ? tl_upload(g_tl.result_px) : 0;
    g_tl.result_ready = false;
    ReleaseSRWLockExclusive(&g_tl.lock);
    if (!have) return;
    if (key == tl.pending_key) tl.pending_key = 0;
    if (!srv) {
        tl.failed_key = key; // not asked again until something else is wanted
        return;
    }
    // The least recently shown slot goes (free ones first: their `used` is 0).
    TimelineFrame *slot = &tl.frames[0];
    for (i32 i = 1; i < COUNT(tl.frames); i++)
        if (tl.frames[i].used < slot->used) slot = &tl.frames[i];
    if (slot->srv) slot->srv->Release();
    slot->key = key;
    slot->srv = srv;
    slot->used = ++tl.frame_clock;
}

// A preview's cache key: the time it shows, doubled, plus 1 for an exact frame. A keyframe preview is keyed by its
// segment's start, so hovering anywhere in one segment reuses it.
static i64 tl_frame_key(i64 t, bool exact) { return t * 2 + (exact ? 1 : 0); }

static i32 tl_seg_at(const BufferIdx &b, i64 t)
{
    i32 lo = 0, hi = b.seg_count - 1, found = -1;
    while (lo <= hi) {
        i32 mid = (lo + hi) / 2;
        if (b.segs[mid].start_us <= t) { found = mid; lo = mid + 1; }
        else hi = mid - 1;
    }
    if (found >= 0 && t < b.segs[found].end_us) return found;
    return -1;
}

// The first segment ending after `t`.
static i32 tl_seg_after(const BufferIdx &b, i64 t)
{
    i32 lo = 0, hi = b.seg_count;
    while (lo < hi) {
        i32 mid = (lo + hi) / 2;
        if (b.segs[mid].end_us <= t) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

// Asks for the frame at `t` unless it's cached or already asked for. Returns its key, 0 if there's no footage there.
static i64 tl_want_frame(i64 t, bool exact)
{
    Timeline &tl = g_app.timeline;
    const BufferIdx &b = *tl.idx;
    i32 s = tl_seg_at(b, t);
    if (s < 0) return 0;
    const VideoSeg &seg = b.segs[s];
    // Exact frames to a tenth of a second: closer than that isn't worth another ffmpeg.
    i64 shown_t = exact ? t / 100000 * 100000 : seg.start_us;
    if (shown_t < seg.start_us) shown_t = seg.start_us;
    i64 key = tl_frame_key(shown_t, exact);
    TimelineFrame *f = tl_frame_find(key);
    if (f) {
        f->used = ++tl.frame_clock;
        return key;
    }
    if (key == tl.pending_key || key == tl.failed_key) return key;

    Path ffmpeg = {};
    if (!ffmpeg_find(g_app.config.ffmpeg, &ffmpeg)) {
        log_line("timeline: ffmpeg not found, so no preview frames");
        tl.failed_key = key;
        return key;
    }
    wchar_t run[64], name[32];
    utf8_to_wide(b.runs[seg.run].name, run, COUNT(run));
    utf8_to_wide(seg.name, name, COUNT(name));
    AcquireSRWLockExclusive(&g_tl.lock);
    g_tl.req.key = key;
    g_tl.req.exact = exact;
    g_tl.req.offset = (f64)(shown_t - seg.start_us) / 1e6;
    g_tl.req.file = path_join(path_join(path_join(g_app.config.buffer_dir, L"video"), run), name);
    g_tl.req.ffmpeg = ffmpeg;
    g_tl.preview_want = true;
    WakeConditionVariable(&g_tl.preview_wake);
    ReleaseSRWLockExclusive(&g_tl.lock);
    if (!g_tl.preview_thread) g_tl.preview_thread = thread_start(tl_preview_thread, 0);
    tl.pending_key = key;
    return key;
}

// ---------------------------------------------------------------------------------------------------------------
// Main thread: events, the tick, saving

static void timeline_on_event(const Event &e)
{
    if (e.a == TL_EV_INDEX) tl_take_index(e.b);
    else if (e.a == TL_EV_PREVIEW) tl_take_preview();
}

// After "Save and edit": open the editor once the file is there.
static void tl_check_job()
{
    Timeline &tl = g_app.timeline;
    if (!tl.job_edit) return;
    SaveJobView *v = save_view(tl.job);
    if (v && v->state < SAVE_DONE) return;
    tl.job_edit = false;
    if (v && v->state == SAVE_DONE) {
        log_line("timeline: opening the editor on %s", v->path);
        ui_show(SCREEN_EDITOR);
        editor_open_file(v->path);
    }
}

static bool tl_visible() { return g_app.ui.shown && g_app.ui.screen == SCREEN_TIMELINE && !IsIconic(g_app.ui.hwnd); }

// The Library's unsaved sessions show the buffer's frames too, so the index is kept while they're on screen.
static bool tl_wanted_by_library()
{
    const Library &lib = g_app.library;
    return g_app.ui.shown && g_app.ui.screen == SCREEN_LIBRARY && !IsIconic(g_app.ui.hwnd) && lib.view == LIB_SESSIONS &&
           !lib.player.open && g_app.sessions.row_count > 0;
}

// A keyframe of the buffer at `t` (the start of its segment, or the next one if `t` falls in a gap) for the
// Library's session previews. Null until it's been read; `ask` reads it (one at a time).
static ID3D11ShaderResourceView *timeline_keyframe(i64 t, bool ask)
{
    Timeline &tl = g_app.timeline;
    if (!tl.idx) {
        tl_ask_index();
        return 0;
    }
    const BufferIdx &b = *tl.idx;
    i32 s = tl_seg_at(b, t);
    if (s < 0) {
        s = tl_seg_after(b, t);
        if (s >= b.seg_count) return 0;
        t = b.segs[s].start_us;
    }
    TimelineFrame *f = tl_frame_find(tl_frame_key(b.segs[s].start_us, false));
    if (f) {
        f->used = ++tl.frame_clock;
        return f->srv;
    }
    if (ask && !tl.pending_key) tl_want_frame(t, false);
    return 0;
}

// Main thread, once a second.
static void timeline_tick()
{
    Timeline &tl = g_app.timeline;
    tl_check_job();
    if (!tl_visible()) {
        if (tl_wanted_by_library()) {
            if (tl.ticks++ % 3 == 0) tl_ask_index();
            return;
        }
        tl.ticks = 0;
        tl_release();
        return;
    }
    if (tl.ticks++ % 3 == 0) tl_ask_index();
    ui_wake(); // the now edge and a view that follows now move with the clock
}

// Whether any video was recorded between a and z: a range that falls in a gap has nothing to save.
static bool tl_has_video(const BufferIdx &b, i64 a, i64 z)
{
    for (i32 i = 0; i < b.seg_count; i++)
        if (b.segs[i].end_us > a && b.segs[i].start_us < z) return true;
    return false;
}

// The names the session box suggests: the games in the selection first, then the rest of the buffer, the games in
// Options and the games in the library, so a game that was only watched (not played here) can still be picked.
// Made when the box opens.
static char g_tl_games[200][64];
static i32  g_tl_game_count;

static void tl_games_add(const char *name)
{
    if (!name[0] || g_tl_game_count >= (i32)COUNT(g_tl_games)) return;
    if (!strcmp(name, "Desktop") || !strcmp(name, "Other")) return;
    for (i32 i = 0; i < g_tl_game_count; i++)
        if (!_stricmp(g_tl_games[i], name)) return;
    str_set(g_tl_games[g_tl_game_count++], name);
}

static void tl_games_make(const BufferIdx &b, i64 now)
{
    Timeline &tl = g_app.timeline;
    const Config &c = g_app.config;
    g_tl_game_count = 0;
    GameTime g[64];
    char name[64];
    for (i32 pass = 0; pass < 2; pass++) {
        i32 n = !b.seg_count ? 0 : pass ? games_in(b, b.oldest_us, now, g, COUNT(g))
                                        : games_in(b, tl.sel_in, tl.sel_out, g, COUNT(g));
        for (i32 i = 0; i < n; i++) {
            audio_display_name(c, g[i].exe, name, sizeof name);
            tl_games_add(name);
        }
    }
    for (i32 i = 0; i < c.app_count; i++)
        if (app_category(c, c.apps[i]) == CAT_GAME) {
            audio_display_name(c, c.apps[i].exe.s, name, sizeof name);
            tl_games_add(name);
        }
    const Library &lib = g_app.library;
    for (i32 i = 0; i < lib.game_count; i++) tl_games_add(lib.games[i]);
}

// Whether `name` holds `part`, ignoring case.
static bool tl_has_text(const char *name, const char *part)
{
    for (; *name; name++) {
        i32 k = 0;
        while (part[k] && name[k] && tolower((u8)name[k]) == tolower((u8)part[k])) k++;
        if (!part[k]) return true;
    }
    return !part[0];
}

static void tl_save(SaveKind kind, bool edit)
{
    Timeline &tl = g_app.timeline;
    if (!tl.sel_in || tl.sel_out <= tl.sel_in || tl.job_edit) return;
    if (!tl_has_video(*tl.idx, tl.sel_in, tl.sel_out)) return;
    u32 id = 0;
    if (kind == SAVE_SESSION) {
        // Through sessions.cpp, so the session gets its record in Data and shows under unsaved sessions if the save fails.
        // No name typed: the game played most in the range, as an automatic session would be named.
        char name[128];
        str_set(name, tl.session_name);
        const char *n = trim(name);
        GameTime top[1];
        if (!n[0] && games_in(*tl.idx, tl.sel_in, tl.sel_out, top, 1)) n = top[0].exe;
        u32 sid = sessions_add_manual(tl.sel_in, tl.sel_out, n);
        if (!sid) return;
        sessions_save(sid);
        const Session *ses = session_find(sid);
        if (ses && ses->state == SES_SAVING) id = ses->job;
    } else {
        id = save_range(tl.sel_in, tl.sel_out, kind, "");
    }
    if (!id) return;
    tl.job = id;
    tl.job_edit = edit;
    log_line("timeline: save %u queued (%s%s, %.1f s)", id, kind == SAVE_SESSION ? "session" : "clip",
             edit ? ", then edit" : "", (f64)(tl.sel_out - tl.sel_in) / 1e6);
}

// ---------------------------------------------------------------------------------------------------------------
// Play: the buffer's segments have no sound of their own and are 2 s each, so Play renders a stretch (up to a minute,
// or to the end of the selection) with its sound mixed, the way a save does, then plays that file. It takes a
// second or two to start.

static i64 tl_playhead();

static const i64 TL_PLAY_CHUNK = 60 * TL_SEC, TL_PLAY_MAX = 300 * TL_SEC;

// Deletes preview files left in the local folder, except the one being played.
static void tl_play_sweep()
{
    Timeline &tl = g_app.timeline;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(path_join(g_app.paths.local_dir, L"preview-*.mp4").s, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        Path f = path_join(g_app.paths.local_dir, fd.cFileName);
        if (tl.player.open && !_wcsicmp(f.s, tl.play_path.s)) continue;
        DeleteFileW(f.s);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// Stops playing and the render, and deletes the file.
static void tl_play_stop()
{
    Timeline &tl = g_app.timeline;
    if (tl.play_job) save_cancel(tl.play_job);
    tl.play_job = 0;
    bool had = tl.player.open || tl.play_path.s[0];
    player_close(&tl.player);
    if (tl.play_path.s[0]) DeleteFileW(tl.play_path.s);
    tl.play_path = {};
    if (had) tl_play_sweep();
}

// Where the file's `pos` seconds are on the wall clock: its segments end to end from play_file_start.
static i64 tl_play_wall(f64 pos)
{
    Timeline &tl = g_app.timeline;
    const BufferIdx &b = *tl.idx;
    i64 left = (i64)(pos * 1e6);
    for (i32 i = tl_seg_after(b, tl.play_file_start); i < b.seg_count && b.segs[i].start_us < tl.play_to; i++) {
        i64 len = b.segs[i].end_us - b.segs[i].start_us;
        if (left < len) return b.segs[i].start_us + left;
        left -= len;
    }
    return tl.play_to;
}

// The other way: the file's time at `t`, the next footage when `t` is in a gap; -1 when the file doesn't reach it.
static f64 tl_play_pos(i64 t)
{
    Timeline &tl = g_app.timeline;
    const BufferIdx &b = *tl.idx;
    if (t < tl.play_file_start) return -1;
    i64 acc = 0;
    for (i32 i = tl_seg_after(b, tl.play_file_start); i < b.seg_count && b.segs[i].start_us < tl.play_to; i++) {
        const VideoSeg &g = b.segs[i];
        if (t < g.start_us) return (f64)acc / 1e6;
        if (t < g.end_us) return (f64)(acc + t - g.start_us) / 1e6;
        acc += g.end_us - g.start_us;
    }
    return -1;
}

// Renders from `at`: to the selection's end when `at` is in it (5 min at most), else a minute.
static void tl_play_render(i64 at)
{
    Timeline &tl = g_app.timeline;
    const BufferIdx &b = *tl.idx;
    tl_play_stop();
    tl.play_error[0] = 0;
    if (!b.seg_count) return;
    if (at >= b.newest_us - TL_SEC) at = b.newest_us - 30 * TL_SEC; // at the live edge: the last 30 s
    if (at < b.oldest_us) at = b.oldest_us;
    i32 first = tl_seg_after(b, at);
    if (first >= b.seg_count) return;
    if (at < b.segs[first].start_us) at = b.segs[first].start_us; // in a gap: from the footage after it
    bool in_sel = tl.sel_in && at >= tl.sel_in && at < tl.sel_out;
    i64 to = in_sel ? ImMin(tl.sel_out, at + TL_PLAY_MAX) : at + TL_PLAY_CHUNK;
    if (to > b.newest_us) to = b.newest_us;
    if (to - at < TL_SEC) return;
    tl.play_job = save_range(at, to, SAVE_PREVIEW, "");
    if (!tl.play_job) {
        str_set(tl.play_error, "Couldn't start the preview");
        return;
    }
    tl.play_file_start = b.segs[first].start_us;
    tl.play_at = at;
    tl.play_to = to;
    tl.playhead_us = tl.play_set_us = at;
}

// Play or Pause.
static void tl_play_toggle()
{
    Timeline &tl = g_app.timeline;
    Player &p = tl.player;
    if (tl.play_job) { // still rendering: Pause gives up on it
        tl_play_stop();
        return;
    }
    if (p.open && !p.paused) {
        player_pause(&p);
        return;
    }
    i64 at = tl_playhead();
    f64 pos = p.open && p.loaded ? tl_play_pos(at) : -1;
    if (pos >= 0 && pos < p.duration - 0.2) { // the file has it: carry on from there
        if (fabs(pos - p.pos) > 0.3) player_seek(&p, pos);
        player_play(&p);
        return;
    }
    tl_play_render(at);
}

// Every frame: open the render once it's done, and keep the playhead on the playing video.
static void tl_play_update()
{
    Timeline &tl = g_app.timeline;
    Player &p = tl.player;
    if (tl.play_job) {
        SaveJobView *v = save_view(tl.play_job);
        if (v && v->state < SAVE_DONE) return;
        tl.play_job = 0;
        if (!v || v->state != SAVE_DONE) {
            snprintf(tl.play_error, sizeof tl.play_error, "%s", v && v->state == SAVE_FAILED ? v->error : "The preview stopped");
            return;
        }
        utf8_to_wide(v->path, tl.play_path.s, MAX_PATH);
        p.id = 2;
        f64 pos = tl_play_pos(tl.play_at);
        player_open(&p, tl.play_path, pos > 0 ? pos : 0, true);
        return;
    }
    if (!p.open) return;
    player_update(&p);
    if (!p.loaded) return;
    if (tl.playhead_us != tl.play_set_us) { // moved by hand
        f64 pos = tl_play_pos(tl.playhead_us);
        if (pos >= 0 && pos < p.duration) {
            player_seek(&p, pos);
            tl.play_set_us = tl.playhead_us;
        } else {
            bool was_playing = !p.paused;
            i64 at = tl.playhead_us;
            tl_play_stop(); // the still frame shows there instead
            if (was_playing) tl_play_render(at);
            else tl.playhead_us = at;
        }
        return;
    }
    tl.playhead_us = tl.play_set_us = tl_play_wall(p.pos);
    // At the end of a minute's stretch, the next one (not past a selection's end, and not at the live edge).
    if (p.ended && p.autoplay) {
        bool sel_end = tl.sel_in && tl.play_at >= tl.sel_in && tl.play_to >= tl.sel_out;
        const BufferIdx &b = *tl.idx;
        if (!sel_end && tl.play_to < b.newest_us - 2 * TL_SEC) tl_play_render(tl.play_to);
        else p.autoplay = false;
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Drawing

static void tl_clock(i64 us, bool secs, char *out, i32 cap)
{
    fmt_hm(local_time(us), out, cap, secs);
}

static void tl_length(i64 us, char *out, i32 cap)
{
    i64 s = (us + 500000) / TL_SEC;
    if (s < 60) snprintf(out, cap, "%lld s", s);
    else if (s < 3600) snprintf(out, cap, "%lld min %02lld s", s / 60, s % 60);
    else snprintf(out, cap, "%lld h %02lld min", s / 3600, s / 60 % 60);
}

// Local time minus UTC, in us, so ruler ticks fall on round local times.
static i64 tl_utc_offset(i64 now)
{
    SYSTEMTIME local = local_time(now);
    FILETIME ft;
    SystemTimeToFileTime(&local, &ft);
    i64 t = ((i64)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return (t - 116444736000000000LL) / 10 - now;
}

static u32 tl_game_colour(const char *exe)
{
    static const u32 cols[] = {0x7a5cff, 0x2fa9a0, 0xd08a2e, 0xc9475d, 0x5b8f3a, 0x3d7fd1, 0xb05cc0, 0x8f7a4a};
    u32 h = 2166136261u;
    for (const char *c = exe; *c; c++) h = (h ^ (u8)*c) * 16777619u;
    return cols[h % COUNT(cols)];
}


static i64 tl_max_span(const BufferIdx &b, i64 now)
{
    i64 have = b.oldest_us ? now - b.oldest_us : 0;
    have += have / 20;
    return have > TL_DAY ? have : TL_DAY;
}

// Keeps the view within the buffer, and follows now once its right edge reaches it.
static void tl_clamp_view(i64 now)
{
    Timeline &tl = g_app.timeline;
    const BufferIdx &b = *tl.idx;
    i64 max_span = tl_max_span(b, now);
    if (tl.view_span_us < TL_MIN_SPAN) tl.view_span_us = TL_MIN_SPAN;
    if (tl.view_span_us > max_span) tl.view_span_us = max_span;
    i64 margin = tl.view_span_us / 30; // room to see the now edge
    i64 latest = now + margin;
    if (tl.follow || tl.view_end_us >= latest) {
        tl.follow = true;
        tl.view_end_us = latest;
    }
    i64 earliest = (b.oldest_us ? b.oldest_us : now - TL_DAY) - margin + tl.view_span_us;
    if (tl.view_end_us < earliest) tl.view_end_us = earliest;
    if (tl.view_end_us > latest) tl.view_end_us = latest;
}

static void tl_zoom(f64 factor, i64 around, i64 now)
{
    Timeline &tl = g_app.timeline;
    i64 start = tl.view_end_us - tl.view_span_us;
    f64 frac = tl.view_span_us ? (f64)(around - start) / (f64)tl.view_span_us : 1;
    i64 span = (i64)((f64)tl.view_span_us * factor);
    i64 max_span = tl_max_span(*tl.idx, now);
    if (span < TL_MIN_SPAN) span = TL_MIN_SPAN;
    if (span > max_span) span = max_span;
    tl.view_span_us = span;
    // A view following now keeps following; otherwise the time under the mouse stays put.
    if (!tl.follow) tl.view_end_us = around + (i64)((1 - frac) * (f64)span);
    tl_clamp_view(now);
}

static void tl_set_span(i64 span, i64 now)
{
    Timeline &tl = g_app.timeline;
    i64 centre = tl.view_end_us - tl.view_span_us / 2;
    tl.view_span_us = span;
    if (!tl.follow) tl.view_end_us = centre + span / 2;
    tl_clamp_view(now);
}

// Keeps the selection on footage times, at least a second long.
static void tl_clamp_selection()
{
    Timeline &tl = g_app.timeline;
    const BufferIdx &b = *tl.idx;
    if (!tl.sel_in || !b.seg_count) return;
    i64 lo = b.oldest_us, hi = b.newest_us;
    if (tl.sel_in < lo) tl.sel_in = lo;
    if (tl.sel_out > hi) tl.sel_out = hi;
    if (tl.sel_out - tl.sel_in < TL_SEC) {
        if (tl.sel_in + TL_SEC <= hi) tl.sel_out = tl.sel_in + TL_SEC;
        else tl.sel_in = tl.sel_out - TL_SEC;
    }
}

static void tl_move_selection(i64 by)
{
    Timeline &tl = g_app.timeline;
    const BufferIdx &b = *tl.idx;
    if (!tl.sel_in) return;
    i64 len = tl.sel_out - tl.sel_in;
    if (tl.sel_in + by < b.oldest_us) by = b.oldest_us - tl.sel_in;
    if (tl.sel_out + by > b.newest_us) by = b.newest_us - tl.sel_out;
    tl.sel_in += by;
    tl.sel_out = tl.sel_in + len;
}

// Sets the start (or end) of the selection at `t`, making a 30 s one if there's none.
// Selection edges sit on whole seconds, so the times shown (to the second) add up to the length shown.
static i64 tl_whole_sec(i64 t) { return (t + TL_SEC / 2) / TL_SEC * TL_SEC; }

static void tl_set_edge(bool in, i64 t)
{
    Timeline &tl = g_app.timeline;
    t = tl_whole_sec(t);
    const BufferIdx &b = *tl.idx;
    if (!b.seg_count) return;
    if (!tl.sel_in) {
        tl.sel_in = in ? t : t - 30 * TL_SEC;
        tl.sel_out = in ? t + 30 * TL_SEC : t;
    } else if (in) {
        tl.sel_in = t;
        if (tl.sel_out <= t) tl.sel_out = t + 30 * TL_SEC;
    } else {
        tl.sel_out = t;
        if (tl.sel_in >= t) tl.sel_in = t - 30 * TL_SEC;
    }
    tl_clamp_selection();
}

static i64 tl_playhead()
{
    Timeline &tl = g_app.timeline;
    if (tl.playhead_us) return tl.playhead_us;
    const BufferIdx &b = *tl.idx;
    return b.seg_count ? b.segs[b.seg_count - 1].start_us : 0; // the newest footage, at its keyframe
}

static void tl_keys(i64 now)
{
    Timeline &tl = g_app.timeline;
    ImGuiIO &io = ImGui::GetIO();
    if (io.WantTextInput || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) return;
    // Keyboard navigation would act on these too (Enter pressing the focused button), so the timeline owns them.
    static const ImGuiKey owned[] = {ImGuiKey_LeftArrow, ImGuiKey_RightArrow, ImGuiKey_UpArrow, ImGuiKey_DownArrow,
                                     ImGuiKey_Enter, ImGuiKey_KeypadEnter, ImGuiKey_Space};
    ImGuiID owner = ImGui::GetID("timeline keys");
    for (ImGuiKey k : owned) ImGui::SetKeyOwner(k, owner);

    i64 step = io.KeyShift ? 10 * TL_SEC : TL_SEC;
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) tl_move_selection(-step);
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) tl_move_selection(step);
    i64 at = tl.hovering ? tl.hover_us : tl_playhead();
    if (ImGui::IsKeyPressed(ImGuiKey_I, false)) tl_set_edge(true, at);
    if (ImGui::IsKeyPressed(ImGuiKey_O, false)) tl_set_edge(false, at);
    if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false))
        tl_save(SAVE_CLIP, false);
    if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) tl_play_toggle();
    i64 centre = tl.view_end_us - tl.view_span_us / 2;
    if (ImGui::IsKeyPressed(ImGuiKey_Equal) || ImGui::IsKeyPressed(ImGuiKey_KeypadAdd)) tl_zoom(0.5, centre, now);
    if (ImGui::IsKeyPressed(ImGuiKey_Minus) || ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract)) tl_zoom(2, centre, now);
    if (ImGui::IsKeyPressed(ImGuiKey_Home, false)) {
        tl.follow = true;
        tl_clamp_view(now);
    }
}

// The ruler's tick step: the smallest that leaves `min_px` between labels.
static i64 tl_tick_step(f64 px_per_us, f32 min_px)
{
    static const i64 steps[] = {1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 900, 1800, 3600, 7200, 10800, 21600, 43200};
    for (i64 s : steps)
        if ((f64)(s * TL_SEC) * px_per_us >= min_px) return s * TL_SEC;
    return 86400 * TL_SEC;
}

struct TlCanvas {
    ImDrawList *dl;
    f32         x0, x1;      // the bar area, left of it are the row labels
    i64         start, span; // the view
    f64         px_per_us;
};

static f32 tl_x(const TlCanvas &c, i64 t) { return c.x0 + (f32)((f64)(t - c.start) * c.px_per_us); }

static i64 tl_t(const TlCanvas &c, f32 x) { return c.start + (i64)((f64)(x - c.x0) / c.px_per_us); }

// Draws [a, b] as a bar in one row, clipped to the view, at least a pixel wide.
static void tl_span(const TlCanvas &c, i64 a, i64 b, f32 y, f32 h, ImU32 col)
{
    f32 xa = tl_x(c, a), xb = tl_x(c, b);
    if (xb < c.x0 || xa > c.x1) return;
    if (xa < c.x0) xa = c.x0;
    if (xb > c.x1) xb = c.x1;
    if (xb - xa < 1) xb = xa + 1;
    c.dl->AddRectFilled(ImVec2(xa, y), ImVec2(xb, y + h), col, 2);
}

// Spans that touch, or whose gap is under a pixel and couldn't be seen anyway, are drawn as one.
struct TlRun { i64 a, z; };

static void tl_run_add(const TlCanvas &c, TlRun *r, i64 a, i64 z, f32 y, f32 h, ImU32 col)
{
    if (r->a && (a - r->z <= 50000 || (f64)(a - r->z) * c.px_per_us < 1)) {
        if (z > r->z) r->z = z;
        return;
    }
    if (r->a) tl_span(c, r->a, r->z, y, h, col);
    r->a = a;
    r->z = z;
}

// ---------------------------------------------------------------------------------------------------------------
// Drawing helpers. tl_text_fit and tl_pill are generic enough to move to the kit.

#define TL_ICON_DOWN "\xEE\x9C\x8D" // E70D chevron down

static const u32 TL_COL_VIDEO = 0x5a7fbf, TL_COL_LANE = 0x131418;

// Text cut to max_w with an ellipsis, so a long name never runs into what's next to it.
static void tl_text_fit(ImDrawList *dl, ImVec2 p, f32 max_w, ImU32 col, const char *text)
{
    if (max_w <= 0) return;
    if (ImGui::CalcTextSize(text).x <= max_w) {
        dl->AddText(p, col, text);
        return;
    }
    static const char ell[] = "...";
    f32 ew = ImGui::CalcTextSize(ell).x;
    const char *end = text + strlen(text);
    while (end > text && ImGui::CalcTextSize(text, end).x + ew > max_w) {
        end--;
        while (end > text && ((u8)*end & 0xC0) == 0x80) end--; // whole UTF-8 characters
    }
    while (end > text && end[-1] == ' ') end--;
    if (end == text) return; // not even one letter fits: nothing rather than a lone "..."
    dl->AddText(p, col, text, end);
    dl->AddText(ImVec2(p.x + ImGui::CalcTextSize(text, end).x, p.y), col, ell);
}

// A small rounded label centred on x, its top at y, kept inside [lo, hi]. Current font.
static void tl_pill(ImDrawList *dl, f32 x, f32 y, f32 lo, f32 hi, const char *text, ImU32 bg, ImU32 fg)
{
    ImVec2 ts = ImGui::CalcTextSize(text);
    f32 px = kit_px(7), py = kit_px(2);
    f32 w = ts.x + 2 * px, h = ts.y + 2 * py;
    f32 a = x - w / 2;
    if (a + w > hi) a = hi - w;
    if (a < lo) a = lo;
    dl->AddRectFilled(ImVec2(a, y), ImVec2(a + w, y + h), bg, h / 2);
    dl->AddText(ImVec2(floorf(a + px), floorf(y + py)), fg, text);
}

// Something saved from the buffer, as the Saved row draws it
struct TlSaved {
    i64  a, z;
    bool session;
    char name[96];
};

// The saves that reach into [from, to]: sessions first (drawn under), then clips, each oldest first. Saved sessions
// from before saved.tsv existed come from sessions.txt.
static i32 tl_saved_make(TlSaved *out, i32 cap, i64 from, i64 to)
{
    const Saving &sv = g_app.saving;
    const Sessions &ss = g_app.sessions;
    i32 n = 0;
    for (i32 pass = 0; pass < 2; pass++) {
        bool sessions = pass == 0;
        for (i32 i = 0; i < sv.saved_count && n < cap; i++) {
            const SavedRange &r = sv.saved[i];
            if (r.gone || (r.kind == SAVE_SESSION) != sessions || r.to_us < from || r.from_us > to) continue;
            TlSaved &v = out[n++];
            v.a = r.from_us;
            v.z = r.to_us;
            v.session = sessions;
            const char *file = strrchr(r.path, '\\');
            str_set(v.name, file ? file + 1 : r.path);
            char *dot = strrchr(v.name, '.');
            if (dot) *dot = 0;
        }
        if (!sessions) continue;
        for (i32 i = 0; i < COUNT(ss.items) && n < cap; i++) {
            const Session &e = ss.items[i];
            if (e.state != SES_SAVED || e.end_us < from || e.start_us > to) continue;
            bool logged = false; // saved.tsv has it already
            for (i32 k = 0; k < sv.saved_count && !logged; k++)
                logged = sv.saved[k].kind == SAVE_SESSION && llabs(sv.saved[k].from_us - e.start_us) < 5 * TL_SEC;
            if (logged) continue;
            TlSaved &v = out[n++];
            v.a = e.start_us;
            v.z = e.end_us;
            v.session = true;
            str_set(v.name, e.name);
        }
    }
    return n;
}

// Sound groups, collapsible from their header in the track list. Kept for the run, like the view.
static bool g_tl_closed[3];
static const char *const TL_GROUP_NAMES[3] = {ICON_MIC "  MICROPHONE", ICON_GAME "  GAMES", ICON_APPS "  APPS"};

// One line of the sound area: a group's header (row -1) or an app's row.
struct TlLine { i8 group; i16 row; };

static void screen_timeline()
{
    Timeline &tl = g_app.timeline;
    f32 s = g_dpi_scale;
    i64 now = now_us();
    if (!tl.idx) tl_ask_index();
    if (!tl.idx) {
        kit_dim("Not enough memory to show the buffer.");
        return;
    }
    if (!tl.view_span_us) {
        tl.view_span_us = TL_DEFAULT_SPAN;
        tl.follow = true;
    }
    tl_check_job();
    const BufferIdx &b = *tl.idx;
    // While an edge is being dragged the view holds still, even when following now: the time under the mouse must
    // not slide away (at 1 min shown, now moves a pixel every few frames).
    bool holding = tl.drag == TL_DRAG_NEW || tl.drag == TL_DRAG_IN || tl.drag == TL_DRAG_OUT;
    if (!holding) tl_clamp_view(now);
    tl_clamp_selection();
    tl_keys(now);
    tl_play_update();
    bool has_sel = tl.sel_in && tl.sel_out > tl.sel_in;
    ImGuiIO &io = ImGui::GetIO();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 avail = ImGui::GetContentRegionAvail();
    ImVec2 top = ImGui::GetCursorScreenPos();
    f32 fh = ImGui::GetFrameHeight();

    // --- Sizes: fixed for a given window size, whatever is shown ---------------------------------------------------
    // The preview top left with the selection beside it, then the toolbar, then the tracks filling the rest. The
    // gap above the toolbar is a splitter (dragged, double-click for the default), kept as a share of the height.
    Config &cfg = g_app.config;
    const f32 gap = 16 * s;
    const f32 pad_r = 14 * s, ruler_h = 28 * s, video_h = 40 * s, game_h = 32 * s, saved_h = 26 * s;
    const f32 line_max = 28 * s, line_min = 18 * s; // a sound line: as tall as fits every app, between these
    const f32 min_tracks = 8 * s + ruler_h + video_h + game_h + saved_h + 10 * s + 3 * line_max + 10 * s;
    const f32 top_min = 260 * s; // the selection card's contents need this
    auto top_height = [&]() {
        f32 h = cfg.timeline_top > 0 ? avail.y * cfg.timeline_top : (avail.y - fh - 2 * gap) * 0.55f;
        if (cfg.timeline_top <= 0 && h > 560 * s) h = 560 * s;
        if (h > avail.y - fh - 2 * gap - min_tracks) h = avail.y - fh - 2 * gap - min_tracks;
        if (h < top_min) h = top_min;
        return floorf(h);
    };
    f32 top_h = top_height();
    {
        static f32 grab_dy;
        ImGui::SetCursorScreenPos(ImVec2(top.x, top.y + top_h));
        ImGui::InvisibleButton("##split_top", ImVec2(avail.x, gap));
        bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
        if (ImGui::IsItemActivated()) grab_dy = io.MousePos.y - (top.y + top_h);
        if (ImGui::IsItemActive() && ImGui::IsMousePosValid() && avail.y > 0) {
            cfg.timeline_top = (io.MousePos.y - grab_dy - top.y) / avail.y;
            if (cfg.timeline_top < 0.01f) cfg.timeline_top = 0.01f;
            top_h = top_height();
            cfg.timeline_top = top_h / avail.y; // where it stopped, so a drag past the end comes straight back
        }
        if (hot && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            cfg.timeline_top = 0;
            top_h = top_height();
        }
        if (ImGui::IsItemDeactivated()) config_save(cfg, g_app.paths);
        if (hot) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        f32 cx = top.x + avail.x / 2, cy = floorf(top.y + top_h + gap / 2);
        dl->AddRectFilled(ImVec2(cx - 22 * s, cy - 1.5f * s), ImVec2(cx + 22 * s, cy + 1.5f * s),
                          col32(hot ? COL_DIM : COL_PANEL2), 2 * s);
    }
    f32 y_tools = top.y + top_h + gap;
    f32 card_h = avail.y - (top_h + gap + fh + gap);
    if (card_h < min_tracks) card_h = min_tracks;
    // The track names' column; its right edge is dragged (double-click for the default)
    auto names_width = [&]() {
        f32 w = (cfg.timeline_names > 0 ? cfg.timeline_names : 176) * s, most = ImMin(360 * s, avail.x * 0.4f);
        if (w > most) w = most;
        if (w < 110 * s) w = 110 * s;
        return floorf(w);
    };
    f32 label_w = names_width();

    // --- Toolbar: how much to show, zoom, pan, and the range shown ----------------------------------------------
    ImGui::SetCursorScreenPos(ImVec2(top.x, y_tools));
    {
        static const char *const labels[] = {"1 min", "10 min", "1 h", "6 h", "All"};
        const i64 spans[] = {60 * TL_SEC, 600 * TL_SEC, 3600 * TL_SEC, 21600 * TL_SEC, tl_max_span(b, now)};
        i32 sel = -1;
        for (i32 i = 0; i < COUNT(spans); i++)
            if (tl.view_span_us == spans[i]) sel = i;
        if (kit_segmented("##span", labels, COUNT(labels), &sel, 64 * s)) tl_set_span(spans[sel], now);
        i64 centre = tl.view_end_us - tl.view_span_us / 2;
        ImGui::SameLine(0, 18 * s);
        kit_next_tip("Zoom out", "-");
        if (kit_icon_button(ICON_ZOOM_OUT, "##zoom_out", KIT_GHOST, tl.view_span_us < tl_max_span(b, now)))
            tl_zoom(2, centre, now);
        ImGui::SameLine(0, 2 * s);
        kit_next_tip("Zoom in", "+");
        if (kit_icon_button(ICON_ZOOM_IN, "##zoom_in", KIT_GHOST, tl.view_span_us > TL_MIN_SPAN)) tl_zoom(0.5, centre, now);
        ImGui::SameLine(0, 14 * s);
        kit_next_tip("Earlier");
        if (kit_icon_button(ICON_LEFT, "##earlier")) {
            tl.follow = false;
            tl.view_end_us -= tl.view_span_us / 2;
            tl_clamp_view(now);
        }
        ImGui::SameLine(0, 2 * s);
        kit_next_tip("Later");
        if (kit_icon_button(ICON_RIGHT, "##later", KIT_GHOST, !tl.follow)) {
            tl.view_end_us += tl.view_span_us / 2;
            tl_clamp_view(now);
        }
        ImGui::SameLine(0, 2 * s);
        kit_next_tip("Back to now", "Home");
        if (kit_icon_button(ICON_NOW, "##now", KIT_GHOST, !tl.follow)) {
            tl.follow = true;
            tl_clamp_view(now);
        }
        char a[16], z[16], text[48];
        bool secs = tl.view_span_us < 600 * TL_SEC;
        tl_clock(tl.view_end_us - tl.view_span_us, secs, a, sizeof a);
        tl_clock(tl.view_end_us, secs, z, sizeof z);
        snprintf(text, sizeof text, "%s  -  %s", a, z);
        f32 tw = ImGui::CalcTextSize(text).x;
        dl->AddText(ImVec2(top.x + avail.x - tw - 4 * s, floorf(y_tools + (fh - ImGui::GetFontSize()) / 2)), col32(COL_DIM), text);
    }

    ImVec2 origin(top.x, y_tools + fh + gap);
    ImVec2 card_z(origin.x + avail.x, origin.y + card_h);
    TlCanvas c = {};
    c.dl = dl;
    c.x1 = card_z.x - pad_r;
    f32 y_ruler = origin.y + 8 * s, y_video = y_ruler + ruler_h, y_game = y_video + video_h, y_saved = y_game + game_h;
    f32 y_snd = y_saved + saved_h + 10 * s;
    f32 y_bottom = card_z.y - 10 * s;
    bool names_hot = false;
    {
        static f32 grab_dx;
        f32 ex = origin.x + label_w;
        ImGui::SetCursorScreenPos(ImVec2(ex - 7 * s, y_video));
        ImGui::InvisibleButton("##split_names", ImVec2(7 * s, y_bottom - y_video));
        bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
        if (ImGui::IsItemActivated()) grab_dx = io.MousePos.x - ex;
        if (ImGui::IsItemActive() && ImGui::IsMousePosValid()) {
            cfg.timeline_names = (i32)((io.MousePos.x - grab_dx - origin.x) / s + 0.5f);
            if (cfg.timeline_names < 1) cfg.timeline_names = 1;
            label_w = names_width();
            cfg.timeline_names = (i32)(label_w / s + 0.5f);
        }
        if (hot && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            cfg.timeline_names = 0;
            label_w = names_width();
        }
        if (ImGui::IsItemDeactivated()) config_save(cfg, g_app.paths);
        if (hot) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        names_hot = hot;
    }
    c.x0 = origin.x + label_w;
    auto set_view = [&]() {
        c.span = tl.view_span_us;
        c.start = tl.view_end_us - tl.view_span_us;
        c.px_per_us = (f64)(c.x1 - c.x0) / (f64)c.span;
    };
    set_view();

    // The sound lines: a header per group that has rows, then its rows unless it's collapsed
    TlLine lines[COUNT(tl.rows) + 3];
    i32 line_count = 0, head_line[3] = {-1, -1, -1}, row_line[COUNT(tl.rows)];
    for (i32 g = 0; g < 3; g++) {
        bool any = false;
        for (i32 r = 0; r < tl.row_count && !any; r++) any = tl_row_group(tl.rows[r]) == g;
        if (!any) continue;
        head_line[g] = line_count;
        lines[line_count++] = {(i8)g, -1};
        for (i32 r = 0; r < tl.row_count; r++) {
            if (tl_row_group(tl.rows[r]) != g) continue;
            row_line[r] = g_tl_closed[g] ? head_line[g] : line_count;
            if (!g_tl_closed[g]) lines[line_count++] = {(i8)g, (i16)r};
        }
    }
    // Every sound line as tall as lets them all show (open), within limits; collapsing a group doesn't change it, so
    // nothing moves but the lines under the header clicked.
    i32 all_lines = tl.row_count;
    for (i32 g = 0; g < 3; g++) all_lines += head_line[g] >= 0;
    f32 line_h = all_lines ? floorf((y_bottom - y_snd) / all_lines) : line_max;
    if (line_h > line_max) line_h = line_max;
    if (line_h < line_min) line_h = line_min;
    bool compact = line_h < 24 * s; // names in the small font
    f32 lane_in = floorf(line_h * 0.14f), bar_in = floorf(line_h * 0.32f);
    i32 lines_fit = (i32)((y_bottom - y_snd) / line_h);
    if (lines_fit < 1) lines_fit = 1;

    // The mouse wheel over the track names scrolls the sound lines (over the bars it zooms)
    if (ImGui::IsWindowHovered() && io.MouseWheel != 0 &&
        ImGui::IsMouseHoveringRect(ImVec2(origin.x, y_snd), ImVec2(c.x0, y_bottom)))
        tl.row_scroll -= (i32)(io.MouseWheel > 0 ? ceilf(io.MouseWheel) : floorf(io.MouseWheel));
    if (tl.row_scroll > line_count - lines_fit) tl.row_scroll = line_count - lines_fit;
    if (tl.row_scroll < 0) tl.row_scroll = 0;

    // What's been saved from here: sessions under, clips over them. A click on one selects it.
    saved_check();
    const Saving &sv = g_app.saving;
    static TlSaved saved[COUNT(sv.saved) + COUNT(g_app.sessions.items)];
    i32 saved_count = tl_saved_make(saved, COUNT(saved), c.start - c.span, c.start + 2 * c.span); // room for a pan
    i32 saved_hit = -1;
    if (ImGui::IsWindowHovered() && io.MousePos.y >= y_saved && io.MousePos.y < y_saved + saved_h &&
        io.MousePos.x >= c.x0 && io.MousePos.x <= c.x1 && tl.drag == TL_DRAG_NONE) {
        i64 t = tl_t(c, io.MousePos.x), slack = (i64)(3 * s / c.px_per_us); // tiny clips stay clickable
        for (i32 i = saved_count - 1; i >= 0 && saved_hit < 0; i--)
            if (t >= saved[i].a - slack && t <= saved[i].z + slack) saved_hit = i;
    }

    dl->AddRectFilled(origin, card_z, col32(COL_PANEL), 10 * s);
    if (names_hot) {
        f32 x = floorf(c.x0 - 3 * s) + 0.5f;
        dl->AddLine(ImVec2(x, y_video), ImVec2(x, y_bottom), col32(COL_DIM), 1.5f * s);
    }

    // --- The mouse on the bars: one invisible button, so ImGui keeps the drag even outside it ---------------------
    ImGui::SetCursorScreenPos(ImVec2(c.x0, origin.y));
    ImGui::InvisibleButton("bar", ImVec2(c.x1 - c.x0, card_h),
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                               ImGuiButtonFlags_MouseButtonMiddle);
    bool bar_hovered = ImGui::IsItemHovered();
    ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
    f32 mx = io.MousePos.x < c.x0 ? c.x0 : io.MousePos.x > c.x1 ? c.x1 : io.MousePos.x;
    i64 mouse_t = tl_t(c, mx);
    i64 mouse_sec = tl_whole_sec(mouse_t);
    // No position while the mouse is outside the window (WM_MOUSELEAVE): leave the drag where it was rather than
    // jumping it to the left edge.
    bool mouse_ok = ImGui::IsMousePosValid();
    f32 grab = 7 * s;
    bool near_in = has_sel && fabsf(io.MousePos.x - tl_x(c, tl.sel_in)) <= grab;
    bool near_out = has_sel && !near_in && fabsf(io.MousePos.x - tl_x(c, tl.sel_out)) <= grab;
    if (ImGui::IsItemActivated() && b.seg_count) {
        tl.drag_x = io.MousePos.x;
        tl.dragged = false;
        tl.drag_anchor_us = mouse_sec;
        tl.drag_view_end = tl.view_end_us;
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) tl.drag = TL_DRAG_PAN;
        else if (near_in) tl.drag = TL_DRAG_IN;
        else if (near_out) tl.drag = TL_DRAG_OUT;
        else tl.drag = TL_DRAG_NEW;
    }
    if (ImGui::IsItemActive() && tl.drag != TL_DRAG_NONE && mouse_ok) {
        if (fabsf(io.MousePos.x - tl.drag_x) > 4 * s) tl.dragged = true;
        if (tl.drag == TL_DRAG_PAN) {
            tl.follow = false;
            tl.view_end_us = tl.drag_view_end - (i64)((f64)(io.MousePos.x - tl.drag_x) / c.px_per_us);
            tl_clamp_view(now);
        } else if (tl.drag == TL_DRAG_NEW && tl.dragged) {
            tl.sel_in = tl.drag_anchor_us < mouse_sec ? tl.drag_anchor_us : mouse_sec;
            tl.sel_out = tl.drag_anchor_us < mouse_sec ? mouse_sec : tl.drag_anchor_us;
            tl.playhead_us = mouse_t;
            tl_clamp_selection();
        } else if (tl.drag == TL_DRAG_IN) {
            tl.sel_in = mouse_sec < tl.sel_out - TL_SEC ? mouse_sec : tl.sel_out - TL_SEC;
            tl.playhead_us = tl.sel_in;
            tl_clamp_selection();
        } else if (tl.drag == TL_DRAG_OUT) {
            tl.sel_out = mouse_sec > tl.sel_in + TL_SEC ? mouse_sec : tl.sel_in + TL_SEC;
            tl.playhead_us = tl.sel_out;
            tl_clamp_selection();
        }
    }
    if (ImGui::IsItemDeactivated()) {
        if (tl.drag == TL_DRAG_NEW && !tl.dragged && mouse_ok && saved_hit >= 0) {
            // A click on something saved: it becomes the selection
            tl.sel_in = tl_whole_sec(saved[saved_hit].a);
            tl.sel_out = tl_whole_sec(saved[saved_hit].z);
            if (tl.sel_out <= tl.sel_in) tl.sel_out = tl.sel_in + TL_SEC;
            tl_clamp_selection();
            tl.playhead_us = tl.sel_in;
        } else if (tl.drag == TL_DRAG_NEW && !tl.dragged && mouse_ok) {
            // A click: the playhead goes there, kept on footage
            i64 t = mouse_t;
            if (t >= b.newest_us) t = b.newest_us - 1;
            if (t < b.oldest_us) t = b.oldest_us;
            tl.playhead_us = t;
        }
        tl.drag = TL_DRAG_NONE;
    }
    if (bar_hovered && io.MouseWheel != 0) tl_zoom(pow(0.8, (f64)io.MouseWheel), mouse_t, now);
    if (tl.drag == TL_DRAG_IN || tl.drag == TL_DRAG_OUT || (bar_hovered && tl.drag == TL_DRAG_NONE && (near_in || near_out)))
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    else if (tl.drag == TL_DRAG_PAN) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    else if (bar_hovered && saved_hit >= 0) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    set_view(); // the view may have moved: draw it where it is now
    tl.hovering = bar_hovered && tl.drag == TL_DRAG_NONE && b.seg_count > 0;
    tl.hover_us = tl_t(c, mx);
    has_sel = tl.sel_in && tl.sel_out > tl.sel_in;
    i64 view_end = c.start + c.span;
    // The time shown on the ruler: under the mouse, or at the edge being dragged
    i64 pill_t = 0;
    if (tl.hovering) pill_t = tl.hover_us;
    else if (tl.drag == TL_DRAG_IN) pill_t = tl.sel_in;
    else if (tl.drag == TL_DRAG_OUT) pill_t = tl.sel_out;
    else if (tl.drag == TL_DRAG_NEW && tl.dragged) pill_t = mouse_sec;

    // --- Track names on the left ----------------------------------------------------------------------------------
    f32 tx = origin.x + 18 * s, th = ImGui::GetTextLineHeight();
    f32 name_w = c.x0 - 16 * s - tx;
    {
        ImGui::PushFont(g_font_small);
        f32 sh = ImGui::GetTextLineHeight();
        dl->AddText(ImVec2(tx, floorf(y_video + (video_h - sh) / 2)), col32(COL_FAINT), ICON_VIDEO);
        dl->AddText(ImVec2(tx, floorf(y_game + (game_h - sh) / 2)), col32(COL_FAINT), ICON_GAME);
        dl->AddText(ImVec2(tx, floorf(y_saved + (saved_h - sh) / 2)), col32(COL_FAINT), ICON_SAVE);
        ImGui::PopFont();
        dl->AddText(ImVec2(tx + 22 * s, floorf(y_video + (video_h - th) / 2)), col32(COL_DIM), "Video");
        dl->AddText(ImVec2(tx + 22 * s, floorf(y_game + (game_h - th) / 2)), col32(COL_DIM), "Game");
        dl->AddText(ImVec2(tx + 22 * s, floorf(y_saved + (saved_h - th) / 2)), col32(COL_DIM), "Saved");
    }
    dl->AddLine(ImVec2(origin.x + 14 * s, y_snd - 5 * s), ImVec2(c.x1, y_snd - 5 * s), col32(COL_LINE));
    if (!line_count) {
        dl->AddText(ImVec2(tx, floorf(y_snd + (line_h - th) / 2)), col32(COL_FAINT), "No sound recorded");
    }
    for (i32 i = 0; i < lines_fit && tl.row_scroll + i < line_count; i++) {
        const TlLine &ln = lines[tl.row_scroll + i];
        f32 y = y_snd + i * line_h;
        if (ln.row < 0) {
            // A group header: a chevron and a caption; a click collapses or opens it
            ImGui::SetCursorScreenPos(ImVec2(origin.x + 8 * s, y));
            ImGui::PushID(ln.group);
            if (ImGui::InvisibleButton("##group", ImVec2(c.x0 - origin.x - 16 * s, line_h))) g_tl_closed[ln.group] = !g_tl_closed[ln.group];
            bool hot = ImGui::IsItemHovered();
            ImGui::PopID();
            if (hot) dl->AddRectFilled(ImVec2(origin.x + 8 * s, y + 2 * s), ImVec2(c.x0 - 8 * s, y + line_h - 2 * s), col32(COL_PANEL2), 6 * s);
            ImGui::PushFont(g_font_small);
            f32 sh = ImGui::GetTextLineHeight();
            u32 ink = hot ? COL_TEXT : COL_DIM;
            dl->AddText(ImVec2(tx, floorf(y + (line_h - sh) / 2)), col32(ink), g_tl_closed[ln.group] ? ICON_RIGHT : TL_ICON_DOWN);
            dl->AddText(ImVec2(tx + 22 * s, floorf(y + (line_h - sh) / 2)), col32(hot ? COL_TEXT : COL_FAINT), TL_GROUP_NAMES[ln.group]);
            ImGui::PopFont();
        } else {
            // The app's own icon (the mic's glyph), then its name
            const TimelineRow &row = tl.rows[ln.row];
            f32 is = compact ? 13 * s : 16 * s, iy = floorf(y + (line_h - is) / 2);
            ID3D11ShaderResourceView *srv = strcmp(row.exe, "mic") ? app_icon(row.exe) : 0;
            if (srv) {
                dl->AddImage((ImTextureID)srv, ImVec2(tx, iy), ImVec2(tx + is, iy + is));
            } else {
                ImGui::PushFont(g_font_small);
                f32 sh = ImGui::GetTextLineHeight();
                dl->AddText(ImVec2(tx, floorf(y + (line_h - sh) / 2)), col32(COL_FAINT), strcmp(row.exe, "mic") ? ICON_WINDOW : ICON_MIC);
                ImGui::PopFont();
            }
            if (compact) ImGui::PushFont(g_font_small);
            f32 nh = ImGui::GetTextLineHeight();
            tl_text_fit(dl, ImVec2(tx + 22 * s, floorf(y + (line_h - nh) / 2)), name_w - 22 * s, col32(COL_DIM), row.name);
            if (compact) ImGui::PopFont();
        }
    }
    // When the lines don't all fit, a thin bar shows where the list is (the wheel scrolls it)
    if (line_count > lines_fit) {
        f32 ta = y_snd + 3 * s, tz = y_snd + lines_fit * line_h - 3 * s, x = c.x0 - 8 * s;
        dl->AddRectFilled(ImVec2(x, ta), ImVec2(x + 3 * s, tz), col32(COL_PANEL2), 2 * s);
        f32 h = (tz - ta) * lines_fit / line_count, y = ta + (tz - ta) * tl.row_scroll / line_count;
        dl->AddRectFilled(ImVec2(x, y), ImVec2(x + 3 * s, y + h), col32(COL_FAINT), 2 * s);
    }

    // --- The bars -------------------------------------------------------------------------------------------------
    dl->PushClipRect(ImVec2(c.x0 - 6 * s, origin.y), ImVec2(c.x1 + 6 * s, card_z.y), true);
    f32 lane_r = 6 * s;
    dl->AddRectFilled(ImVec2(c.x0, y_video + 3 * s), ImVec2(c.x1, y_video + video_h - 3 * s), col32(TL_COL_LANE), lane_r);
    dl->AddRectFilled(ImVec2(c.x0, y_game + 3 * s), ImVec2(c.x1, y_game + game_h - 3 * s), col32(TL_COL_LANE), lane_r);
    dl->AddRectFilled(ImVec2(c.x0, y_saved + 3 * s), ImVec2(c.x1, y_saved + saved_h - 3 * s), col32(TL_COL_LANE), lane_r);
    for (i32 i = 0; i < lines_fit && tl.row_scroll + i < line_count; i++) {
        const TlLine &ln = lines[tl.row_scroll + i];
        if (ln.row < 0 && !g_tl_closed[ln.group]) continue; // an open header has no bar of its own
        f32 y = y_snd + i * line_h;
        dl->AddRectFilled(ImVec2(c.x0, y + lane_in), ImVec2(c.x1, y + line_h - lane_in), col32(TL_COL_LANE),
                          ImMin(lane_r, (line_h - 2 * lane_in) / 2));
    }

    // Ruler, ticks on round local times, and their lines faintly through the tracks
    {
        i64 step = tl_tick_step(c.px_per_us, 96 * s);
        i64 off = tl_utc_offset(now);
        ImGui::PushFont(g_font_small);
        f32 sh = ImGui::GetTextLineHeight();
        f32 pill_x = pill_t ? tl_x(c, pill_t) : -1e9f, pill_half = ImGui::CalcTextSize(g_app.config.clock_24h ? "00:00:00" : "00:00:00 pm").x / 2 + 10 * s;
        for (i64 t = ((c.start + off) / step + 1) * step - off; t < view_end; t += step) {
            f32 x = floorf(tl_x(c, t)) + 0.5f;
            dl->AddLine(ImVec2(x, y_ruler + ruler_h - 7 * s), ImVec2(x, y_ruler + ruler_h), col32(COL_FAINT));
            dl->AddLine(ImVec2(x, y_video), ImVec2(x, y_bottom), col32(COL_LINE, 0.55f));
            char label[16];
            SYSTEMTIME lt = local_time(t);
            if (step >= 3600 * TL_SEC && lt.wHour == 0 && lt.wMinute == 0)
                snprintf(label, sizeof label, "%d %.3s", lt.wDay, month_name(lt.wMonth));
            else tl_clock(t, step < 60 * TL_SEC, label, sizeof label);
            f32 lw = ImGui::CalcTextSize(label).x;
            if (x + 5 * s < pill_x + pill_half && x + 5 * s + lw > pill_x - pill_half) continue; // under the time pill
            dl->AddText(ImVec2(x + 5 * s, floorf(y_ruler + (ruler_h - 7 * s - sh) / 2)), col32(COL_DIM), label);
        }
        ImGui::PopFont();
    }

    // Video: footage as bars; gaps (lock screen, paused, clipback not running) stay empty
    {
        TlRun run = {};
        ImU32 col = col32(TL_COL_VIDEO, 0.9f);
        f32 y = y_video + 7 * s, h = video_h - 14 * s;
        for (i32 i = tl_seg_after(b, c.start); i < b.seg_count && b.segs[i].start_us < view_end; i++)
            tl_run_add(c, &run, b.segs[i].start_us, b.segs[i].end_us, y, h, col);
        if (run.a) tl_span(c, run.a, run.z, y, h, col);
    }

    // Games: a band per stretch of a game being played, named where there's room
    for (i32 i = 0; i < b.activity_count; i++) {
        const ActivityEntry &e = b.activity[i];
        i64 end = i + 1 < b.activity_count ? b.activity[i + 1].us : now;
        if (!e.exe[0] || end < c.start || e.us > view_end) continue;
        f32 y = y_game + 6 * s, h = game_h - 12 * s;
        tl_span(c, e.us, end, y, h, col32(tl_game_colour(e.exe), 0.9f));
        f32 xa = tl_x(c, e.us), xb = tl_x(c, end);
        if (xa < c.x0) xa = c.x0;
        if (xb > c.x1) xb = c.x1;
        char name[64];
        audio_display_name(g_app.config, e.exe, name, sizeof name);
        ImGui::PushFont(g_font_small);
        tl_text_fit(dl, ImVec2(xa + 7 * s, floorf(y + (h - ImGui::GetTextLineHeight()) / 2)), xb - xa - 12 * s, col32(0xffffff), name);
        ImGui::PopFont();
    }

    // Saved: sessions as a tint the height of the lane, clips solid over them, each named where there's room
    ImGui::PushFont(g_font_small);
    for (i32 i = 0; i < saved_count; i++) {
        const TlSaved &v = saved[i];
        bool hot = i == saved_hit && bar_hovered;
        f32 y = v.session ? y_saved + 5 * s : y_saved + 7 * s, h = v.session ? saved_h - 10 * s : saved_h - 14 * s;
        ImU32 col = v.session ? col32(COL_ACCENT, hot ? 0.5f : 0.3f) : col32(hot ? COL_ACCENT_HOVER : COL_ACCENT, 0.95f);
        tl_span(c, v.a, v.z, y, h, col);
        f32 xa = tl_x(c, v.a), xb = tl_x(c, v.z);
        if (xa < c.x0) xa = c.x0;
        if (xb > c.x1) xb = c.x1;
        tl_text_fit(dl, ImVec2(xa + 6 * s, floorf(y_saved + (saved_h - ImGui::GetTextLineHeight()) / 2)), xb - xa - 10 * s,
                    col32(v.session ? COL_TEXT : 0xffffff, v.session ? 0.8f : 1), v.name);
    }
    ImGui::PopFont();

    // Sound: when each app had a stream open. A collapsed group shows all its apps on its header line.
    for (i32 st = 0; st < b.stream_count && st < COUNT(tl.stream_row); st++) {
        if (tl.stream_row[st] < 0) continue;
        i32 r = row_line[tl.stream_row[st]] - tl.row_scroll;
        if (r < 0 || r >= lines_fit) continue;
        const AudioStreamInfo &info = b.streams[st];
        f32 y = y_snd + r * line_h + bar_in, h = line_h - 2 * bar_in;
        ImU32 col = col32(tl.rows[tl.stream_row[st]].rgb, 0.9f);
        TlRun run = {};
        for (i32 k = 0; k < info.seg_count; k++) {
            const AudioSeg &seg = b.audio_segs[info.first_seg + k];
            // An open segment is the one being written, or one left behind when clipback was killed: segments are
            // 10 s, so it can't reach past that (else a dead stream would seem to run up to now).
            i64 e = seg.open ? ImMin(now, seg.start_us + 12 * TL_SEC) : seg.end_us;
            if (e < c.start || seg.start_us > view_end) continue;
            tl_run_add(c, &run, seg.start_us, e, y, h, col);
        }
        if (run.a) tl_span(c, run.a, run.z, y, h, col);
    }

    // The future, dimmed, and the now edge
    f32 x_now = tl_x(c, now);
    if (x_now < c.x1) {
        dl->AddRectFilled(ImVec2(x_now, y_video), ImVec2(c.x1 + 6 * s, y_bottom), col32(COL_PANEL, 0.7f));
        dl->AddLine(ImVec2(x_now, y_video), ImVec2(x_now, y_bottom), col32(COL_FAINT), 1.5f * s);
    }

    // The selection: an accent tint, a band on the ruler, and a handle on each edge
    f32 y_band = y_ruler + ruler_h - 6 * s;
    if (has_sel) {
        f32 xa = tl_x(c, tl.sel_in), xb = tl_x(c, tl.sel_out);
        dl->AddRectFilled(ImVec2(xa, y_video), ImVec2(xb, y_bottom), col32(COL_ACCENT, 0.12f));
        dl->AddRectFilled(ImVec2(xa, y_band), ImVec2(xb, y_band + 4 * s), col32(COL_ACCENT, 0.85f));
        for (i32 k = 0; k < 2; k++) {
            f32 x = k ? xb : xa;
            bool hot = (k ? tl.drag == TL_DRAG_OUT : tl.drag == TL_DRAG_IN) ||
                       (bar_hovered && tl.drag == TL_DRAG_NONE && (k ? near_out : near_in));
            ImU32 col = col32(hot ? COL_ACCENT_HOVER : COL_ACCENT);
            dl->AddLine(ImVec2(x, y_band), ImVec2(x, y_bottom), col, 2 * s);
            f32 hw = 5 * s, hy = y_band + 2 * s;
            dl->AddRectFilled(ImVec2(x - hw, hy - 11 * s), ImVec2(x + hw, hy + 11 * s), col, 4 * s);
            dl->AddLine(ImVec2(x - 1.5f * s, hy - 5 * s), ImVec2(x - 1.5f * s, hy + 5 * s), col32(0xffffff, 0.8f), 1);
            dl->AddLine(ImVec2(x + 1.5f * s, hy - 5 * s), ImVec2(x + 1.5f * s, hy + 5 * s), col32(0xffffff, 0.8f), 1);
        }
    }

    // The playhead (where the preview comes from)
    i64 play = tl_playhead();
    if (play) {
        f32 x = tl_x(c, play);
        ImU32 col = col32(COL_TEXT);
        dl->AddLine(ImVec2(x, y_video - 4 * s), ImVec2(x, y_bottom), col, 1.5f * s);
        f32 hy = y_video - 4 * s;
        dl->AddTriangleFilled(ImVec2(x - 5 * s, hy - 7 * s), ImVec2(x + 5 * s, hy - 7 * s), ImVec2(x, hy), col);
    }
    // The time under the mouse, or at the edge being dragged, on the ruler
    {
        i64 t = pill_t;
        if (t) {
            f32 x = tl_x(c, t);
            if (tl.hovering) dl->AddLine(ImVec2(x, y_video), ImVec2(x, y_bottom), col32(COL_TEXT, 0.3f));
            char when[16];
            tl_clock(t, true, when, sizeof when);
            ImGui::PushFont(g_font_small);
            tl_pill(dl, x, y_ruler + 2 * s, c.x0, c.x1, when, col32(COL_HOVER), col32(COL_TEXT));
            ImGui::PopFont();
        }
    }
    if (!tl.loaded || !b.seg_count) {
        const char *msg = !tl.loaded ? "Reading the buffer..." : "Nothing recorded yet";
        ImVec2 sz = ImGui::CalcTextSize(msg);
        dl->AddText(ImVec2(floorf((c.x0 + c.x1 - sz.x) / 2), floorf(y_video + (video_h - th) / 2)), col32(COL_DIM), msg);
    }
    dl->PopClipRect();

    // --- The preview, top left: the video playing, or the still frame at the playhead (under the mouse while hovering)
    f32 transport_h = fh + 12 * s;
    f32 min_card_w = 450 * s;
    f32 pw = (top_h - transport_h) * 16 / 9;
    if (pw > avail.x - gap - min_card_w) pw = avail.x - gap - min_card_w;
    if (pw < 240 * s) pw = 240 * s;
    f32 ph = pw * (f32)TL_FRAME_H / (f32)TL_FRAME_W;
    ImVec2 p0(top.x, top.y), p1(top.x + pw, top.y + ph);
    f32 pr = 10 * s;
    dl->AddRectFilled(p0, p1, col32(0x000000), pr);
    Player &pl = tl.player;
    bool playing_view = pl.open && pl.frame_ready && !pl.error[0];
    if (playing_view) {
        ImVec2 v0, v1;
        player_fit(pl, p0, ImVec2(pw, ph), &v0, &v1);
        dl->AddImage((ImTextureID)(intptr_t)pl.srv, v0, v1);
    } else {
        // Hovering shows the keyframe under the mouse (quick); otherwise the exact frame at the playhead. While Play
        // gets ready the last frame stays.
        bool busy_play = tl.play_job || pl.open;
        bool exact = !tl.hovering;
        i64 want_t = tl.hovering && !busy_play ? tl.hover_us : play;
        i64 want_key = b.seg_count && want_t && !busy_play ? tl_want_frame(want_t, exact) : 0;
        TimelineFrame *frame = tl_frame_find(want_key);
        if (frame) tl.shown_key = frame->key;
        else if (want_key || busy_play) frame = tl_frame_find(tl.shown_key); // the last one shown, until the new one is ready
        if (frame) dl->AddImageRounded((ImTextureID)(intptr_t)frame->srv, p0, p1, ImVec2(0, 0), ImVec2(1, 1), col32(0xffffff), pr);
        char when[16], msg[200] = "";
        if (pl.error[0]) str_set(msg, "This stretch couldn't be played");
        else if (!b.seg_count) str_set(msg, "No footage");
        else if (busy_play) msg[0] = 0;
        else if (!want_key) {
            tl_clock(want_t, true, when, sizeof when);
            snprintf(msg, sizeof msg, "Nothing recorded at %s", when);
        } else if (want_key == tl.failed_key) str_set(msg, "No frame here");
        else if (!frame) str_set(msg, "Loading...");
        if (msg[0] && !frame) {
            ImVec2 sz = ImGui::CalcTextSize(msg);
            dl->AddText(ImVec2(floorf((p0.x + p1.x - sz.x) / 2), floorf((p0.y + p1.y - sz.y) / 2)), col32(COL_DIM), msg);
        }
        if (busy_play) dl->AddRectFilled(p0, p1, col32(0x000000, 0.45f), pr);
    }

    // Under it: Play / Pause, the playhead's time, and what Play is doing
    {
        f32 ty = p1.y + 12 * s;
        bool active = tl.play_job || (pl.open && !pl.paused);
        ImGui::SetCursorScreenPos(ImVec2(p0.x, ty));
        kit_next_tip(active ? "Pause" : "Play", "Space");
        if (kit_icon_button(active ? ICON_PAUSE : ICON_PLAY, "##play", KIT_PRIMARY, b.seg_count > 0)) tl_play_toggle();
        char when[16] = "--:--:--";
        if (play) tl_clock(play, true, when, sizeof when);
        ImGui::PushFont(g_font_bold);
        f32 cx = p0.x + fh + 12 * s;
        dl->AddText(ImVec2(cx, floorf(ty + (fh - ImGui::GetFontSize()) / 2)), col32(COL_TEXT), when);
        f32 cw = ImGui::CalcTextSize("00:00:00").x;
        ImGui::PopFont();
        char status[200] = "";
        u32 ink = COL_DIM;
        if (tl.play_job) {
            SaveJobView *v = save_view(tl.play_job);
            if (v && v->state == SAVE_RUNNING) snprintf(status, sizeof status, "Getting it ready  %d%%", (int)(v->progress * 100));
            else str_set(status, "Waiting for a save to finish");
        } else if (tl.play_error[0]) {
            str_set(status, tl.play_error);
            ink = COL_WARN;
        } else if (pl.error[0]) {
            str_set(status, pl.error);
            ink = COL_WARN;
        } else if (pl.open && !pl.loaded) str_set(status, "Opening...");
        else if (!pl.open) str_set(status, b.seg_count ? "Play starts at the playhead" : "");
        if (status[0]) {
            ImGui::PushFont(g_font_small);
            f32 sx = cx + cw + 16 * s;
            tl_text_fit(dl, ImVec2(sx, floorf(ty + (fh - ImGui::GetFontSize()) / 2)), p1.x - sx, col32(ink), status);
            ImGui::PopFont();
        }
    }

    // --- The selection card, beside the preview ---------------------------------------------------------------------
    ImGui::SetCursorScreenPos(ImVec2(p0.x + pw + gap, top.y));
    if (kit_card_begin("##selection", ImVec2(avail.x - pw - gap, top_h), 20)) {
        ImDrawList *cd = ImGui::GetWindowDrawList();
        ImVec2 c0 = ImGui::GetCursorScreenPos();
        f32 cw = ImGui::GetContentRegionAvail().x, ch = ImGui::GetContentRegionAvail().y;
        kit_label("SELECTION");
        f32 ib = 28 * s;
        ImGui::SetCursorScreenPos(ImVec2(c0.x + cw - ib, c0.y - 6 * s));
        kit_next_tip("Clear the selection");
        if (kit_icon_button(ICON_CLOSE, "##clear", KIT_GHOST, has_sel, false, ib)) tl.sel_in = tl.sel_out = 0;

        // Start, end and length, the nudges beside each time
        char a[16] = "--:--:--", z[16] = "--:--:--", len[32] = "-";
        if (has_sel) {
            tl_clock(tl.sel_in, true, a, sizeof a);
            tl_clock(tl.sel_out, true, z, sizeof z);
            tl_length(tl.sel_out - tl.sel_in, len, sizeof len);
        }
        bool longer = has_sel && tl.sel_out - tl.sel_in > TL_SEC;
        f32 y_cap = c0.y + 34 * s, y_val = y_cap + 22 * s, col_w = cw / 3, sb = 24 * s;
        ImFont *time_font = g_font_title;
        ImGui::PushFont(time_font);
        f32 big_h = ImGui::GetTextLineHeight(), time_w = ImGui::CalcTextSize("00:00:00").x;
        ImGui::PopFont();
        if (time_w + 2 * sb + 16 * s > col_w) { // narrow window: a smaller size so the nudges still fit
            time_font = g_font_big;
            ImGui::PushFont(time_font);
            big_h = ImGui::GetTextLineHeight();
            time_w = ImGui::CalcTextSize("00:00:00").x;
            ImGui::PopFont();
        }
        for (i32 k = 0; k < 3; k++) {
            f32 x = c0.x + k * col_w;
            ImGui::SetCursorScreenPos(ImVec2(x, y_cap));
            kit_label(k == 0 ? "START" : k == 1 ? "END" : "LENGTH");
            ImGui::PushFont(time_font);
            cd->AddText(ImVec2(x, y_val), col32(has_sel ? COL_TEXT : COL_FAINT), k == 0 ? a : k == 1 ? z : len);
            ImGui::PopFont();
            if (k == 2) continue;
            // Earlier and later by a second
            f32 bx = x + time_w + 8 * s, by = floorf(y_val + (big_h - sb) / 2);
            ImGui::PushID(k);
            ImGui::SetCursorScreenPos(ImVec2(bx, by));
            bool can_down = k == 0 ? has_sel : longer, can_up = k == 0 ? longer : has_sel;
            kit_next_tip(k == 0 ? "Start 1 s earlier" : "End 1 s earlier");
            if (kit_icon_button(ICON_LEFT, "##down", KIT_SECONDARY, can_down, false, sb)) {
                if (k == 0) tl.sel_in -= TL_SEC;
                else tl.sel_out -= TL_SEC;
                tl_clamp_selection();
                tl.playhead_us = k == 0 ? tl.sel_in : tl.sel_out;
            }
            ImGui::SetCursorScreenPos(ImVec2(bx + sb + 3 * s, by));
            kit_next_tip(k == 0 ? "Start 1 s later" : "End 1 s later");
            if (kit_icon_button(ICON_RIGHT, "##up", KIT_SECONDARY, can_up, false, sb)) {
                if (k == 0) tl.sel_in += TL_SEC;
                else tl.sel_out += TL_SEC;
                tl_clamp_selection();
                tl.playhead_us = k == 0 ? tl.sel_in : tl.sel_out;
            }
            ImGui::PopID();
        }

        // The games in it
        GameTime games[16];
        i32 game_count = has_sel ? games_in(b, tl.sel_in, tl.sel_out, games, COUNT(games)) : 0;
        bool has_video = has_sel && tl_has_video(b, tl.sel_in, tl.sel_out);
        {
            char list[256] = {};
            for (i32 g = 0; g < game_count; g++) {
                char n[64];
                audio_display_name(g_app.config, games[g].exe, n, sizeof n);
                size_t l = strlen(list);
                snprintf(list + l, sizeof list - l, "%s%s", g ? ", " : "", n);
            }
            const char *what = !has_sel ? "Drag across the tracks to select"
                             : !has_video ? "No video in this range"
                             : game_count ? list : "Desktop only";
            f32 y = y_val + big_h + 18 * s;
            ImGui::PushFont(g_font_small);
            f32 sh = ImGui::GetTextLineHeight();
            if (has_sel) cd->AddText(ImVec2(c0.x, y), col32(COL_FAINT), ICON_GAME);
            ImGui::PopFont();
            f32 x = has_sel ? c0.x + 22 * s : c0.x;
            tl_text_fit(cd, ImVec2(x, floorf(y + (sh - th) / 2)), c0.x + cw - x, col32(has_sel && !has_video ? COL_WARN : COL_DIM), what);
        }

        // The actions, then the last save's progress, both pinned to the bottom of the card
        bool busy = tl.job_edit; // waiting for the save to open the editor
        bool can = has_video && !busy;
        f32 bgap = 8 * s, y_status = c0.y + ch - fh, y_buttons = y_status - 14 * s - fh;
        f32 w1 = 118 * s, w2 = 128 * s, w3 = 146 * s;
        if (w1 + w2 + w3 + 2 * bgap > cw) w1 = w2 = w3 = (cw - 2 * bgap) / 3;
        ImGui::SetCursorScreenPos(ImVec2(c0.x, y_buttons));
        if (kit_button(ICON_SAVE "  Save clip", KIT_PRIMARY, ImVec2(w1, 0), can)) tl_save(SAVE_CLIP, false);
        ImGui::SameLine(0, bgap);
        if (kit_button(ICON_EDIT "  Save and edit", KIT_SECONDARY, ImVec2(w2, 0), can)) tl_save(SAVE_CLIP, true);
        ImGui::SameLine(0, bgap);
        ImVec2 ses_pos = ImGui::GetCursorScreenPos();
        if (kit_button(ICON_GAME "  Save session...", KIT_SECONDARY, ImVec2(w3, 0), can)) ImGui::OpenPopup("##session");

        // A session: a name typed, or a game picked. In a popup over the card, so nothing moves.
        // Anchored to the button's right edge so it stays inside the window
        ImGui::SetNextWindowPos(ImVec2(ses_pos.x + w3, ses_pos.y - 8 * s), ImGuiCond_Appearing, ImVec2(1, 1));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18 * s, 16 * s));
        ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 10 * s);
        ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1);
        ImGui::PushStyleColor(ImGuiCol_PopupBg, rgb(COL_PANEL2));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, rgb(COL_PANEL));
        if (ImGui::BeginPopup("##session")) {
            // One box: the name, filled in with the game played most when there is one (typing replaces it). Typing
            // suggests games; the arrow lists them all.
            f32 fw = 300 * s, bh = ImGui::GetFrameHeight(), aw = bh;
            if (ImGui::IsWindowAppearing()) {
                tl_games_make(b, now);
                tl.session_name[0] = 0;
                if (game_count) audio_display_name(g_app.config, games[0].exe, tl.session_name, sizeof tl.session_name);
                tl.suggest_pick = -1;
                tl.suggest_close = tl.suggest_was_open = false;
                tl.suggest_refocus = true;
            }
            i32 match[COUNT(g_tl_games)], match_count = 0;
            for (i32 i = 0; i < g_tl_game_count; i++)
                if (tl.suggest_all || tl_has_text(g_tl_games[i], tl.session_name)) match[match_count++] = i;
            bool suggesting = ImGui::IsPopupOpen("##suggest");

            ImVec2 bp = ImGui::GetCursorScreenPos();
            ImDrawList *pd = ImGui::GetWindowDrawList();
            pd->AddRectFilled(bp, ImVec2(bp.x + fw, bp.y + bh), col32(COL_PANEL), ImGui::GetStyle().FrameRounding);
            ImGui::PushStyleColor(ImGuiCol_FrameBg, 0);
            ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, 0);
            ImGui::PushStyleColor(ImGuiCol_FrameBgActive, 0);
            if (tl.suggest_refocus) ImGui::SetKeyboardFocusHere();
            tl.suggest_refocus = false;
            ImGui::SetNextItemWidth(fw - aw);
            bool enter = ImGui::InputTextWithHint("##session_name", "Name", tl.session_name, sizeof tl.session_name,
                                                  ImGuiInputTextFlags_AutoSelectAll | ImGuiInputTextFlags_EnterReturnsTrue);
            bool typing = ImGui::IsItemActive();
            if (ImGui::IsItemEdited()) {
                tl.suggest_all = false;
                tl.suggest_pick = -1;
                if (tl.session_name[0]) ImGui::OpenPopup("##suggest");
                else tl.suggest_close = true;
            }
            ImGui::PopStyleColor(3);

            // The arrow, inside the same box
            ImGui::SameLine(0, 0);
            bool arrow = ImGui::InvisibleButton("##all", ImVec2(aw, bh));
            if (ImGui::IsItemActivated()) tl.suggest_was_open = suggesting;
            ImGui::PushFont(g_font_icon);
            ImVec2 is = ImGui::CalcTextSize(ICON_DOWN);
            pd->AddText(ImVec2(floorf(bp.x + fw - aw + (aw - is.x) / 2), floorf(bp.y + (bh - is.y) / 2)),
                        col32(ImGui::IsItemHovered() ? COL_TEXT : COL_DIM), ICON_DOWN);
            ImGui::PopFont();
            if (arrow) {
                if (!tl.suggest_was_open) {
                    tl.suggest_all = true;
                    tl.suggest_pick = -1;
                    ImGui::OpenPopup("##suggest");
                }
                tl.suggest_refocus = true;
            }

            // Up and Down move through the suggestions, Enter takes the highlighted one (or saves when there's none)
            bool save = false;
            if (typing && suggesting && match_count) {
                if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) tl.suggest_pick = (tl.suggest_pick + 1) % match_count;
                if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) tl.suggest_pick = tl.suggest_pick <= 0 ? match_count - 1 : tl.suggest_pick - 1;
            }
            if (enter) {
                if (suggesting && tl.suggest_pick >= 0 && tl.suggest_pick < match_count) {
                    str_set(tl.session_name, g_tl_games[match[tl.suggest_pick]]);
                    tl.suggest_close = tl.suggest_refocus = true;
                } else save = true;
            }

            // The suggestions, in a popup of their own under the box that leaves the cursor in it
            f32 row_h = ImGui::GetTextLineHeight() + ImGui::GetStyle().ItemSpacing.y;
            ImGui::SetNextWindowPos(ImVec2(bp.x - ImGui::GetStyle().WindowPadding.x, bp.y + bh + 4 * s));
            ImGui::SetNextWindowSizeConstraints(ImVec2(fw + 2 * ImGui::GetStyle().WindowPadding.x, 0),
                                                ImVec2(fw + 2 * ImGui::GetStyle().WindowPadding.x, 8 * row_h + 2 * ImGui::GetStyle().WindowPadding.y));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8 * s, 8 * s));
            if (ImGui::BeginPopup("##suggest", ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav)) {
                if (tl.suggest_close || !match_count) ImGui::CloseCurrentPopup();
                for (i32 m = 0; m < match_count; m++) {
                    ImGui::PushID(m);
                    if (ImGui::Selectable(g_tl_games[match[m]], m == tl.suggest_pick)) {
                        str_set(tl.session_name, g_tl_games[match[m]]);
                        tl.suggest_refocus = true;
                        ImGui::CloseCurrentPopup();
                    }
                    static i32 scrolled_to = -1; // only when the pick moves, so the wheel still scrolls
                    if (m == tl.suggest_pick && m != scrolled_to) ImGui::SetScrollHereY();
                    if (m == tl.suggest_pick) scrolled_to = m;
                    ImGui::PopID();
                }
                ImGui::EndPopup();
            }
            ImGui::PopStyleVar();
            tl.suggest_close = false;

            ImGui::Dummy(ImVec2(0, 6 * s));
            f32 hw = (fw - bgap) / 2;
            if (kit_button(ICON_SAVE "  Save session", KIT_PRIMARY, ImVec2(hw, 0), can) || (save && can)) {
                tl_save(SAVE_SESSION, false);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine(0, bgap);
            if (kit_button(ICON_CLOSE "  Cancel", KIT_SECONDARY, ImVec2(hw, 0))) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(3);

        // The last save started here: what it's doing on the left, a thin bar under it while it runs, and Cancel on
        // the right while it can be cancelled (its space is kept either way).
        SaveJobView *v = save_view(tl.job);
        if (v) {
            f32 progress = v->state == SAVE_DONE ? 1 : v->progress;
            bool running = v->state < SAVE_DONE;
            const char *kind = v->kind == SAVE_SESSION ? "session" : "clip";
            char text[600];
            u32 ink = COL_DIM;
            const char *icon = 0;
            if (v->state == SAVE_QUEUED) str_set(text, "Waiting for another save to finish");
            else if (v->state == SAVE_RUNNING) snprintf(text, sizeof text, "Saving the %s  %d%%", kind, (int)(progress * 100));
            else if (v->state == SAVE_DONE) {
                const char *file = strrchr(v->path, '\\');
                snprintf(text, sizeof text, "Saved %s", file ? file + 1 : v->path);
                icon = ICON_CHECK;
                ink = COL_OK;
            } else if (v->state == SAVE_FAILED) {
                snprintf(text, sizeof text, "Save failed: %s", v->error);
                icon = ICON_WARNING;
                ink = COL_ACCENT;
            } else str_set(text, "Save cancelled");
            f32 cancel_w = 84 * s, text_w = cw - cancel_w - 12 * s;
            ImGui::PushFont(g_font_small);
            f32 sh = ImGui::GetTextLineHeight();
            f32 ty = running ? y_status + 3 * s : floorf(y_status + (fh - sh) / 2);
            f32 x = c0.x;
            if (icon) {
                cd->AddText(ImVec2(x, ty), col32(ink), icon);
                x += 20 * s;
            }
            tl_text_fit(cd, ImVec2(x, ty), c0.x + text_w - x, col32(icon ? COL_TEXT : COL_DIM), text);
            ImGui::PopFont();
            if (running) {
                f32 by = y_status + fh - 10 * s;
                cd->AddRectFilled(ImVec2(c0.x, by), ImVec2(c0.x + text_w, by + 4 * s), col32(COL_PANEL2), 2 * s);
                if (progress > 0)
                    cd->AddRectFilled(ImVec2(c0.x, by), ImVec2(c0.x + text_w * progress, by + 4 * s), col32(COL_ACCENT), 2 * s);
                ImGui::SetCursorScreenPos(ImVec2(c0.x + cw - cancel_w, y_status));
                if (kit_button(ICON_CLOSE "  Cancel", KIT_GHOST, ImVec2(cancel_w, 0))) save_cancel(v->id);
            }
        }
    }
    kit_card_end();
    ImGui::SetCursorScreenPos(top);
    ImGui::Dummy(avail);
}
