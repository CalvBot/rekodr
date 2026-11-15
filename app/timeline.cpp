// Timeline: the buffer on a zoomable, pannable bar, a range picked on it, and Save / Edit / Session.
//
// Two workers, neither touching g_app:
// - the index worker reads the buffer (buffer_read) every 3 s while the screen is shown. There are two heap blocks
//   of BufferIdx (about 8 MB each): the main thread draws from one while the worker fills the other, and they swap
//   on EV_TIMELINE. Both are freed while the screen isn't shown.
// - the preview worker runs ffmpeg for one small frame of the segment under the wanted time (the newest request
//   wins) and hands back BGRA pixels. The main thread uploads them as D3D11 textures and keeps the last few.
//
// Rows, top to bottom: the time ruler, video (gaps in recording are gaps), the game being played (a band per game,
// from activity.tsv), then one row per app that made sound (every stream of one exe on one row; a stream lasts
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
    return audio_category(g_app.config, r.exe) == CAT_GAME ? 1 : 2;
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
        if (!st.mic && audio_category(g_app.config, exe) == CAT_IGNORED) continue;
        i32 r = 0;
        while (r < tl.row_count && strcmp(tl.rows[r].exe, exe)) r++;
        if (r == tl.row_count) {
            if (r == COUNT(tl.rows)) continue;
            TimelineRow *row = &tl.rows[tl.row_count++];
            str_set(row->exe, exe);
            if (st.mic) str_set(row->name, "Mic");
            else audio_display_name(g_app.config, exe, row->name, sizeof row->name);
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

// Frees the index blocks and preview textures while the screen isn't shown (SPEC: hidden, the window holds nothing).
static void tl_release()
{
    Timeline &tl = g_app.timeline;
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
    slot->time_us = key >> 1;
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

// Main thread, once a second.
static void timeline_tick()
{
    Timeline &tl = g_app.timeline;
    tl_check_job();
    if (!tl_visible()) {
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

static void tl_save(SaveKind kind, bool edit)
{
    Timeline &tl = g_app.timeline;
    if (!tl.sel_in || tl.sel_out <= tl.sel_in || tl.job_edit) return;
    if (!tl_has_video(*tl.idx, tl.sel_in, tl.sel_out)) return;
    u32 id = 0;
    if (kind == SAVE_SESSION) {
        // Through sessions.cpp, so the session gets its record in Data and shows under Not saved if the save fails.
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
// Drawing

static void tl_clock(i64 us, bool secs, char *out, i32 cap)
{
    SYSTEMTIME t = local_time(us);
    if (secs) snprintf(out, cap, "%02d:%02d:%02d", t.wHour, t.wMinute, t.wSecond);
    else snprintf(out, cap, "%02d:%02d", t.wHour, t.wMinute);
}

static void tl_length(i64 us, char *out, i32 cap)
{
    i64 s = (us + 500000) / TL_SEC;
    if (s < 60) snprintf(out, cap, "%lld s", s);
    else if (s < 3600) snprintf(out, cap, "%lld min %02lld s", s / 60, s % 60);
    else snprintf(out, cap, "%lld h %02lld min", s / 3600, s / 60 % 60);
}

// Local time minus UTC, in µs, so ruler ticks fall on round local times.
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

// A fixed-width button that's disabled rather than hidden, so nothing moves.
static bool tl_button(const char *label, f32 w, bool enabled)
{
    ImGui::BeginDisabled(!enabled);
    bool clicked = ImGui::Button(label, ImVec2(w * g_dpi_scale, 0));
    ImGui::EndDisabled();
    return clicked;
}

static void tl_keys(i64 now)
{
    Timeline &tl = g_app.timeline;
    ImGuiIO &io = ImGui::GetIO();
    if (io.WantTextInput || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) return;
    // Keyboard navigation would act on these too (Enter pressing the focused button), so the timeline owns them.
    static const ImGuiKey owned[] = {ImGuiKey_LeftArrow, ImGuiKey_RightArrow, ImGuiKey_UpArrow, ImGuiKey_DownArrow,
                                     ImGuiKey_Enter, ImGuiKey_KeypadEnter};
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

static void screen_timeline()
{
    Timeline &tl = g_app.timeline;
    f32 s = g_dpi_scale;
    i64 now = now_us();
    if (!tl.idx) tl_ask_index();
    if (!tl.idx) {
        ImGui::TextDisabled("Not enough memory to show the buffer.");
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
    bool has_sel = tl.sel_in && tl.sel_out > tl.sel_in;

    // --- Toolbar: zoom presets and panning, all fixed widths ----------------------------------------------------
    struct Preset { const char *label; i64 span; f32 w; };
    static const Preset presets[] = {{"1 min", 60 * TL_SEC, 72},      {"10 min", 600 * TL_SEC, 72},
                                     {"1 hour", 3600 * TL_SEC, 72},   {"6 hours", 21600 * TL_SEC, 72},
                                     {"Whole buffer", 0, 110}};
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Show");
    for (const Preset &p : presets) {
        ImGui::SameLine();
        i64 span = p.span ? p.span : tl_max_span(b, now);
        ImGui::PushStyleColor(ImGuiCol_Button, tl.view_span_us == span ? rgb(COL_LINE) : rgb(COL_PANEL2));
        if (tl_button(p.label, p.w, true)) tl_set_span(span, now);
        ImGui::PopStyleColor();
    }
    ImGui::SameLine(0, 20 * s);
    i64 centre = tl.view_end_us - tl.view_span_us / 2;
    if (tl_button("Zoom in", 80, tl.view_span_us > TL_MIN_SPAN)) tl_zoom(0.5, centre, now);
    ImGui::SameLine();
    if (tl_button("Zoom out", 80, tl.view_span_us < tl_max_span(b, now))) tl_zoom(2, centre, now);
    ImGui::SameLine(0, 20 * s);
    if (tl_button("Earlier", 80, true)) {
        tl.follow = false;
        tl.view_end_us -= tl.view_span_us / 2;
        tl_clamp_view(now);
    }
    ImGui::SameLine();
    if (tl_button("Later", 80, !tl.follow)) {
        tl.view_end_us += tl.view_span_us / 2;
        tl_clamp_view(now);
    }
    ImGui::SameLine();
    if (tl_button("Now", 80, !tl.follow)) {
        tl.follow = true;
        tl_clamp_view(now);
    }
    {
        char a[16], z[16], len[32];
        bool secs = tl.view_span_us < 600 * TL_SEC;
        tl_clock(tl.view_end_us - tl.view_span_us, secs, a, sizeof a);
        tl_clock(tl.view_end_us, secs, z, sizeof z);
        tl_length(tl.view_span_us, len, sizeof len);
        ImGui::SameLine(0, 20 * s);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s to %s (%s)", a, z, len);
    }

    // --- The bar ---------------------------------------------------------------------------------------------
    const f32 bottom_h = 300 * s; // the preview and selection panel, fixed so the bar never changes size
    const f32 label_w = 150 * s, ruler_h = 24 * s, video_h = 30 * s, game_h = 24 * s, head_h = 28 * s, row_h = 20 * s;
    ImVec2 avail = ImGui::GetContentRegionAvail();
    f32 canvas_h = avail.y - bottom_h;
    f32 min_h = ruler_h + video_h + game_h + head_h + row_h;
    if (canvas_h < min_h) canvas_h = min_h;
    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    TlCanvas c = {};
    c.dl = dl;
    c.x0 = origin.x + label_w;
    c.x1 = origin.x + avail.x;
    f32 y_video = origin.y + ruler_h, y_game = y_video + video_h, y_head = y_game + game_h, y_rows = y_head + head_h;
    f32 y_end = origin.y + canvas_h;
    i32 rows_fit = (i32)((y_end - y_rows) / row_h);
    if (rows_fit < 1) rows_fit = 1;
    if (tl.row_scroll > tl.row_count - rows_fit) tl.row_scroll = tl.row_count - rows_fit;
    if (tl.row_scroll < 0) tl.row_scroll = 0;
    auto set_view = [&]() {
        c.span = tl.view_span_us;
        c.start = tl.view_end_us - tl.view_span_us;
        c.px_per_us = (f64)(c.x1 - c.x0) / (f64)c.span;
    };
    set_view();

    dl->AddRectFilled(origin, ImVec2(c.x1, y_end), col32(COL_PANEL), 8);

    // The mouse: the bar is one invisible button, so ImGui keeps the drag even outside it.
    ImGui::SetCursorScreenPos(ImVec2(c.x0, origin.y));
    ImGui::InvisibleButton("bar", ImVec2(c.x1 - c.x0, canvas_h),
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                               ImGuiButtonFlags_MouseButtonMiddle);
    ImGuiIO &io = ImGui::GetIO();
    bool bar_hovered = ImGui::IsItemHovered();
    ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
    f32 mx = io.MousePos.x < c.x0 ? c.x0 : io.MousePos.x > c.x1 ? c.x1 : io.MousePos.x;
    i64 mouse_t = tl_t(c, mx);
    i64 mouse_sec = tl_whole_sec(mouse_t);
    // No position while the mouse is outside the window (WM_MOUSELEAVE): leave the drag where it was rather than
    // jumping it to the left edge.
    bool mouse_ok = ImGui::IsMousePosValid();
    if (ImGui::IsItemActivated() && b.seg_count) {
        f32 grab = 6 * s;
        tl.drag_x = io.MousePos.x;
        tl.dragged = false;
        tl.drag_anchor_us = mouse_sec;
        tl.drag_view_end = tl.view_end_us;
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) tl.drag = TL_DRAG_PAN;
        else if (has_sel && fabsf(io.MousePos.x - tl_x(c, tl.sel_in)) <= grab) tl.drag = TL_DRAG_IN;
        else if (has_sel && fabsf(io.MousePos.x - tl_x(c, tl.sel_out)) <= grab) tl.drag = TL_DRAG_OUT;
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
        if (tl.drag == TL_DRAG_NEW && !tl.dragged && mouse_ok) {
            // A click: the playhead goes there, kept on footage
            i64 t = mouse_t;
            if (t >= b.newest_us) t = b.newest_us - 1;
            if (t < b.oldest_us) t = b.oldest_us;
            tl.playhead_us = t;
        }
        tl.drag = TL_DRAG_NONE;
    }
    if (bar_hovered && io.MouseWheel != 0) tl_zoom(pow(0.8, (f64)io.MouseWheel), mouse_t, now);
    set_view(); // the view may have moved: draw it where it is now
    tl.hovering = bar_hovered && tl.drag == TL_DRAG_NONE && b.seg_count > 0;
    tl.hover_us = tl_t(c, mx);
    has_sel = tl.sel_in && tl.sel_out > tl.sel_in;
    i64 view_end = c.start + c.span;

    // Row labels on the left
    f32 tx = origin.x + 12 * s, th = ImGui::GetTextLineHeight();
    ImGui::PushFont(g_font_bold);
    dl->AddText(ImVec2(tx, y_video + (video_h - th) / 2), col32(COL_DIM), "Video");
    dl->AddText(ImVec2(tx, y_game + (game_h - th) / 2), col32(COL_DIM), "Game");
    dl->AddText(ImVec2(tx, y_head + (head_h - th) / 2), col32(COL_DIM), "Sound");
    ImGui::PopFont();
    for (i32 r = 0; r < rows_fit && tl.row_scroll + r < tl.row_count; r++) {
        f32 y = y_rows + r * row_h;
        dl->PushClipRect(ImVec2(origin.x, y), ImVec2(c.x0 - 6 * s, y + row_h), true);
        dl->AddText(ImVec2(tx + 8 * s, y + (row_h - th) / 2), col32(COL_TEXT), tl.rows[tl.row_scroll + r].name);
        dl->PopClipRect();
    }
    // Paging the sound rows when there are more than fit: always there, disabled when not needed
    {
        f32 bw = ImGui::GetFrameHeight();
        ImGui::SetCursorScreenPos(ImVec2(c.x0 - 2 * bw - 14 * s, y_head + (head_h - bw) / 2));
        ImGui::BeginDisabled(tl.row_scroll <= 0);
        if (ImGui::ArrowButton("rows_up", ImGuiDir_Up)) tl.row_scroll -= rows_fit;
        ImGui::EndDisabled();
        ImGui::SameLine(0, 4 * s);
        ImGui::BeginDisabled(tl.row_scroll + rows_fit >= tl.row_count);
        if (ImGui::ArrowButton("rows_down", ImGuiDir_Down)) tl.row_scroll += rows_fit;
        ImGui::EndDisabled();
        if (tl.row_scroll < 0) tl.row_scroll = 0;
    }

    dl->PushClipRect(ImVec2(c.x0, origin.y), ImVec2(c.x1, y_end), true);
    dl->AddRectFilled(ImVec2(c.x0, y_video + 3 * s), ImVec2(c.x1, y_video + video_h - 3 * s), col32(COL_BG));
    dl->AddLine(ImVec2(c.x0, y_head), ImVec2(c.x1, y_head), col32(COL_LINE));
    for (i32 r = 1; r < rows_fit; r += 2)
        dl->AddRectFilled(ImVec2(c.x0, y_rows + r * row_h), ImVec2(c.x1, y_rows + (r + 1) * row_h), col32(COL_PANEL2, 0.5f));

    // Ruler, ticks on round local times
    {
        static const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
        i64 step = tl_tick_step(c.px_per_us, 90 * s);
        i64 off = tl_utc_offset(now);
        for (i64 t = ((c.start + off) / step + 1) * step - off; t < view_end; t += step) {
            f32 x = tl_x(c, t);
            dl->AddLine(ImVec2(x, origin.y + ruler_h - 6 * s), ImVec2(x, y_end), col32(COL_LINE, 0.6f));
            char label[16];
            SYSTEMTIME lt = local_time(t);
            if (step >= 3600 * TL_SEC && lt.wHour == 0 && lt.wMinute == 0)
                snprintf(label, sizeof label, "%d %s", lt.wDay, months[(lt.wMonth + 11) % 12]);
            else tl_clock(t, step < 60 * TL_SEC, label, sizeof label);
            dl->AddText(ImVec2(x + 4 * s, origin.y + 4 * s), col32(COL_DIM), label);
        }
    }

    // Video: footage as bars; gaps (lock screen, paused, clipback not running) stay empty
    {
        TlRun run = {};
        ImU32 col = col32(COL_BLUE, 0.75f);
        f32 y = y_video + 5 * s, h = video_h - 10 * s;
        for (i32 i = tl_seg_after(b, c.start); i < b.seg_count && b.segs[i].start_us < view_end; i++)
            tl_run_add(c, &run, b.segs[i].start_us, b.segs[i].end_us, y, h, col);
        if (run.a) tl_span(c, run.a, run.z, y, h, col);
    }

    // Games: a band per stretch of a game being played, named where there's room
    for (i32 i = 0; i < b.activity_count; i++) {
        const ActivityEntry &e = b.activity[i];
        i64 end = i + 1 < b.activity_count ? b.activity[i + 1].us : now;
        if (!e.exe[0] || end < c.start || e.us > view_end) continue;
        tl_span(c, e.us, end, y_game + 3 * s, game_h - 6 * s, col32(tl_game_colour(e.exe), 0.85f));
        f32 xa = tl_x(c, e.us), xb = tl_x(c, end);
        if (xa < c.x0) xa = c.x0;
        if (xb > c.x1) xb = c.x1;
        char name[64];
        audio_display_name(g_app.config, e.exe, name, sizeof name);
        if (xb - xa > ImGui::CalcTextSize(name).x + 10 * s) {
            dl->PushClipRect(ImVec2(xa, y_game), ImVec2(xb, y_game + game_h), true);
            dl->AddText(ImVec2(xa + 5 * s, y_game + (game_h - th) / 2), col32(COL_TEXT), name);
            dl->PopClipRect();
        }
    }

    // Sound: when each app had a stream open
    for (i32 st = 0; st < b.stream_count && st < COUNT(tl.stream_row); st++) {
        i32 r = tl.stream_row[st] - tl.row_scroll;
        if (tl.stream_row[st] < 0 || r < 0 || r >= rows_fit) continue;
        const AudioStreamInfo &info = b.streams[st];
        f32 y = y_rows + r * row_h + 4 * s, h = row_h - 8 * s;
        ImU32 col = col32(info.mic ? 0xd0a24a : COL_OK, 0.8f);
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
    dl->AddRectFilled(ImVec2(x_now, origin.y), ImVec2(c.x1, y_end), col32(COL_BG, 0.6f));
    dl->AddLine(ImVec2(x_now, origin.y), ImVec2(x_now, y_end), col32(COL_ACCENT), 2 * s);

    // The selection, with a handle on each edge
    if (has_sel) {
        f32 edges[2] = {tl_x(c, tl.sel_in), tl_x(c, tl.sel_out)};
        dl->AddRectFilled(ImVec2(edges[0], origin.y), ImVec2(edges[1], y_end), col32(COL_BLUE, 0.18f));
        for (f32 x : edges) {
            dl->AddLine(ImVec2(x, origin.y), ImVec2(x, y_end), col32(COL_TEXT), 2 * s);
            dl->AddRectFilled(ImVec2(x - 4 * s, origin.y), ImVec2(x + 4 * s, origin.y + ruler_h - 6 * s), col32(COL_TEXT), 2);
        }
    }

    // The playhead (where the preview comes from) and the time under the mouse
    i64 play = tl_playhead();
    if (play) {
        f32 x = tl_x(c, play);
        ImU32 col = col32(0xf2c94c);
        dl->AddLine(ImVec2(x, origin.y + ruler_h - 8 * s), ImVec2(x, y_end), col, 1.5f * s);
        dl->AddTriangleFilled(ImVec2(x - 5 * s, origin.y + ruler_h - 12 * s), ImVec2(x + 5 * s, origin.y + ruler_h - 12 * s),
                              ImVec2(x, origin.y + ruler_h - 3 * s), col);
    }
    if (tl.hovering) {
        f32 x = tl_x(c, tl.hover_us);
        dl->AddLine(ImVec2(x, origin.y + ruler_h), ImVec2(x, y_end), col32(COL_TEXT, 0.35f));
    }
    if (!tl.loaded || !b.seg_count) {
        const char *msg = !tl.loaded ? "Reading the buffer..." : "Nothing recorded yet.";
        ImVec2 sz = ImGui::CalcTextSize(msg);
        dl->AddText(ImVec2((c.x0 + c.x1 - sz.x) / 2, y_video + (video_h - th) / 2), col32(COL_DIM), msg);
    }
    dl->PopClipRect();

    // --- Preview and the selection panel ---------------------------------------------------------------------
    f32 pw = 400 * s, ph = pw * (f32)TL_FRAME_H / (f32)TL_FRAME_W;
    ImVec2 p0 = ImVec2(origin.x, y_end + 12 * s);
    dl->AddRectFilled(p0, ImVec2(p0.x + pw, p0.y + ph), col32(0x000000), 6);
    // Hovering shows the keyframe under the mouse (quick); otherwise the exact frame at the playhead.
    bool exact = !tl.hovering;
    i64 want_t = tl.hovering ? tl.hover_us : play;
    i64 want_key = b.seg_count && want_t ? tl_want_frame(want_t, exact) : 0;
    TimelineFrame *frame = tl_frame_find(want_key);
    if (frame) tl.shown_key = frame->key;
    else if (want_key) frame = tl_frame_find(tl.shown_key); // the last one shown, until the new one is ready
    if (frame) dl->AddImage((ImTextureID)(intptr_t)frame->srv, p0, ImVec2(p0.x + pw, p0.y + ph));
    {
        char when[16], caption[96];
        if (!b.seg_count) str_set(caption, "No footage");
        else if (!want_key) {
            tl_clock(want_t, true, when, sizeof when);
            snprintf(caption, sizeof caption, "Nothing recorded at %s", when);
        } else if (want_key == tl.failed_key) str_set(caption, "No frame (see the log)");
        else if (frame) {
            tl_clock(frame->time_us, true, when, sizeof when);
            snprintf(caption, sizeof caption, "Frame at %s", when);
        } else str_set(caption, "Loading the frame...");
        ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + ph + 6 * s));
        ImGui::TextDisabled("%s", caption);
    }

    // Right of the preview: the selection and what to do with it
    ImGui::SetCursorScreenPos(ImVec2(p0.x + pw + 24 * s, p0.y));
    ImGui::BeginGroup();
    ImGui::PushFont(g_font_bold);
    ImGui::TextUnformatted("Selection");
    ImGui::PopFont();
    {
        char a[16] = "--:--:--", z[16] = "--:--:--", len[32] = "none";
        if (has_sel) {
            tl_clock(tl.sel_in, true, a, sizeof a);
            tl_clock(tl.sel_out, true, z, sizeof z);
            tl_length(tl.sel_out - tl.sel_in, len, sizeof len);
        }
        ImGui::TextDisabled("Start");
        ImGui::SameLine(60 * s);
        ImGui::TextUnformatted(a);
        ImGui::SameLine(160 * s);
        ImGui::TextDisabled("End");
        ImGui::SameLine(200 * s);
        ImGui::TextUnformatted(z);
        ImGui::SameLine(300 * s);
        ImGui::TextDisabled("Length");
        ImGui::SameLine(360 * s);
        ImGui::PushFont(g_font_bold);
        ImGui::TextUnformatted(len);
        ImGui::PopFont();
    }
    // Fine adjustment, a second at a time
    {
        bool longer = has_sel && tl.sel_out - tl.sel_in > TL_SEC;
        if (tl_button("Start -1 s", 90, has_sel)) { tl.sel_in -= TL_SEC; tl_clamp_selection(); tl.playhead_us = tl.sel_in; }
        ImGui::SameLine();
        if (tl_button("Start +1 s", 90, longer)) { tl.sel_in += TL_SEC; tl_clamp_selection(); tl.playhead_us = tl.sel_in; }
        ImGui::SameLine(0, 20 * s);
        if (tl_button("End -1 s", 90, longer)) { tl.sel_out -= TL_SEC; tl_clamp_selection(); tl.playhead_us = tl.sel_out; }
        ImGui::SameLine();
        if (tl_button("End +1 s", 90, has_sel)) { tl.sel_out += TL_SEC; tl_clamp_selection(); tl.playhead_us = tl.sel_out; }
        ImGui::SameLine(0, 20 * s);
        if (tl_button("Clear", 70, has_sel)) tl.sel_in = tl.sel_out = 0;
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
        ImGui::TextDisabled("Games");
        ImGui::SameLine(60 * s);
        ImGui::TextUnformatted(!has_sel ? "" : !has_video ? "No video in this range" : game_count ? list : "Desktop only");
    }
    ImGui::Spacing();
    bool busy = tl.job_edit; // waiting for the save to open the editor
    if (tl_button("Save clip", 130, has_video && !busy)) tl_save(SAVE_CLIP, false);
    ImGui::SameLine();
    if (tl_button("Save and edit", 130, has_video && !busy)) tl_save(SAVE_CLIP, true);
    // A session: a name typed, or a game picked
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Session name");
    ImGui::SameLine(110 * s);
    ImGui::SetNextItemWidth(230 * s);
    ImGui::InputTextWithHint("##session", "empty: the game played most", tl.session_name, sizeof tl.session_name);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140 * s);
    if (ImGui::BeginCombo("##pick", "Pick a game", ImGuiComboFlags_HeightRegular)) {
        // Games in the selection first, then the other games in the buffer
        GameTime all[64];
        i32 n = b.seg_count ? games_in(b, b.oldest_us, now, all, COUNT(all)) : 0;
        for (i32 pass = 0; pass < 2; pass++) {
            for (i32 g = 0; g < (pass ? n : game_count); g++) {
                const char *exe = pass ? all[g].exe : games[g].exe;
                bool dup = false;
                for (i32 k = 0; pass && k < game_count; k++) dup |= !strcmp(games[k].exe, exe);
                if (dup) continue;
                char name[64];
                audio_display_name(g_app.config, exe, name, sizeof name);
                ImGui::PushID(exe);
                if (ImGui::Selectable(name)) str_set(tl.session_name, name);
                ImGui::PopID();
            }
        }
        if (!n) ImGui::TextDisabled("No games in the buffer");
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (tl_button("Save as session", 140, has_video && !busy)) tl_save(SAVE_SESSION, false);
    ImGui::Spacing();
    // The last save started here: bar and Cancel first, so the text changing length moves nothing
    {
        SaveJobView *v = save_view(tl.job);
        f32 progress = !v ? 0 : v->state == SAVE_DONE ? 1 : v->progress;
        ImGui::ProgressBar(progress, ImVec2(200 * s, 0), "");
        ImGui::SameLine();
        if (tl_button("Cancel", 80, v && v->state < SAVE_DONE)) save_cancel(v->id);
        char text[600];
        const char *kind = v && v->kind == SAVE_SESSION ? "session" : "clip";
        if (!v) str_set(text, "Nothing saved from here yet");
        else if (v->state == SAVE_QUEUED) str_set(text, "Waiting for another save to finish");
        else if (v->state == SAVE_RUNNING) snprintf(text, sizeof text, "Saving the %s, %d%%", kind, (int)(progress * 100));
        else if (v->state == SAVE_DONE) {
            const char *file = strrchr(v->path, '\\');
            snprintf(text, sizeof text, "Saved %s", file ? file + 1 : v->path);
        } else if (v->state == SAVE_FAILED) snprintf(text, sizeof text, "Save failed: %s", v->error);
        else str_set(text, "Save cancelled");
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::PushStyleColor(ImGuiCol_Text, v && v->state == SAVE_FAILED ? rgb(COL_ACCENT) : rgb(COL_TEXT));
        ImGui::TextUnformatted(text);
        ImGui::PopStyleColor();
    }
    ImGui::EndGroup();
}
