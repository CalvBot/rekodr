// Timeline: the buffer on a zoomable, pannable bar, a range picked on it, and Save / Edit / Session. The preview
// sits top left: a still frame at the playhead, or the footage playing with its sound after Play.
//
// Two workers, neither touching g_app:
// - the index worker reads the buffer (buffer_read) every 3 s while the screen is shown: from a copy of its last
//   read it reads only the folders that changed, and the whole buffer after a prune. There are two heap blocks of
//   BufferIdx (about 8 MB each): the main thread draws from one while the worker fills the other, and they swap
//   on EV_TIMELINE. A third holds the worker's copy. All are freed while the screen isn't shown.
// - the preview worker runs ffmpeg for one small frame of the segment under the wanted time (the newest request
//   wins) and hands back BGRA pixels. The main thread uploads them as D3D11 textures and keeps the last few.
//
// Rows, top to bottom: the time ruler, what has been saved (saved.tsv; a click selects it, a double-click plays it,
// a right click has Open, Edit and Show in folder), the game being played (from activity.tsv, a band per game in its
// colour), the video (gaps in recording are gaps; frames along it once zoomed in), then one row per app that made
// sound (every stream of one exe on one row; a stream lasts while the app plays sound, plus 30 s).

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

// Keyframes asked for in a batch (timeline_keyframes): at most this many queued, being read or read and not taken.
enum { TL_BATCH_MAX = 20, TL_BATCH_THREADS = 4 };

struct TlBatchDone {
    i64 key;
    u8 *px; // TL_FRAME_BYTES from malloc, the main thread frees it; 0 when ffmpeg gave no frame
};

// The only data shared between the main thread and the workers, always under `lock`.
struct TlShared {
    SRWLOCK            lock;
    CONDITION_VARIABLE index_wake, preview_wake, batch_wake;
    HANDLE             index_thread, preview_thread; // started on first use; main thread only
    bool               index_want;
    Path               buffer_dir;
    BufferIdx         *spare; // the block the worker fills next; 0 while it's filling it
    BufferIdx         *ready; // filled, not taken by the main thread yet
    BufferIdx         *last;  // the worker's copy of what it read last, of last_dir; 0 until the first read
    bool               preview_want;
    TlPreviewReq       req;
    bool               result_ready, result_ok;
    i64                result_key;
    u8                 result_px[TL_FRAME_BYTES];
    HANDLE             batch_threads[TL_BATCH_THREADS]; // started on first use; main thread only
    TlPreviewReq       batch_q[TL_BATCH_MAX];           // waiting for a batch worker, oldest first
    i32                batch_n;
    TlBatchDone        batch_done[TL_BATCH_MAX];
    i32                batch_done_n;
};

static TlShared g_tl;

// Main thread only: the batch's keys asked for and not taken back yet, and those that gave no frame (the last
// TL_BATCH_MAX of them, not asked again until the screen is left).
static i64 g_tl_batch_asked[TL_BATCH_MAX], g_tl_batch_failed[TL_BATCH_MAX];
static i32 g_tl_batch_asked_n, g_tl_batch_failed_n;

// ---------------------------------------------------------------------------------------------------------------
// Workers

static BufferIdx *tl_alloc_idx()
{
    return (BufferIdx *)VirtualAlloc(0, sizeof(BufferIdx), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
}

static DWORD WINAPI tl_index_thread(void *)
{
    static Path dir, last_dir;
    for (;;) {
        AcquireSRWLockExclusive(&g_tl.lock);
        while (!g_tl.index_want || !g_tl.spare) SleepConditionVariableSRW(&g_tl.index_wake, &g_tl.lock, INFINITE, 0);
        BufferIdx *b = g_tl.spare, *last = g_tl.last;
        g_tl.spare = 0;
        g_tl.index_want = false;
        dir = g_tl.buffer_dir;
        ReleaseSRWLockExclusive(&g_tl.lock);

        u64 started = GetTickCount64();
        bool same = last && !_wcsicmp(dir.s, last_dir.s), full = true;
        buffer_read(dir, b, same ? last : 0, &full);
        u64 ms = GetTickCount64() - started;
        if (!last) last = tl_alloc_idx();
        if (last) {
            buffer_idx_copy(last, b);
            last_dir = dir;
        }
        AcquireSRWLockExclusive(&g_tl.lock);
        g_tl.ready = b;
        g_tl.last = last;
        ReleaseSRWLockExclusive(&g_tl.lock);
        DEV_LOG("timeline: %s buffer read in %llu ms", full ? "full" : "partial", ms);
        Event e = {};
        e.kind = EV_TIMELINE;
        e.a = TL_EV_INDEX;
        e.b = (i64)ms;
        event_post(e);
    }
}

// One frame, scaled and letterboxed to TL_FRAME_W x TL_FRAME_H, as raw BGRA on ffmpeg's stdout.
static bool tl_extract(const TlPreviewReq &r, u8 *px)
{
    wchar_t args[4096];
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

// TL_BATCH_THREADS of these read the batch's keyframes, each with its own ffmpeg.
static DWORD WINAPI tl_batch_thread(void *)
{
    TlPreviewReq req;
    for (;;) {
        AcquireSRWLockExclusive(&g_tl.lock);
        while (!g_tl.batch_n) SleepConditionVariableSRW(&g_tl.batch_wake, &g_tl.lock, INFINITE, 0);
        req = g_tl.batch_q[0];
        g_tl.batch_n--;
        memmove(g_tl.batch_q, g_tl.batch_q + 1, g_tl.batch_n * sizeof g_tl.batch_q[0]);
        ReleaseSRWLockExclusive(&g_tl.lock);

        u8 *px = (u8 *)malloc(TL_FRAME_BYTES);
        if (px && !tl_extract(req, px)) {
            free(px);
            px = 0;
        }
        AcquireSRWLockExclusive(&g_tl.lock);
        // Never full: the main thread keeps queued, running and done together at TL_BATCH_MAX at most
        g_tl.batch_done[g_tl.batch_done_n++] = {req.key, px};
        ReleaseSRWLockExclusive(&g_tl.lock);
        Event e = {};
        e.kind = EV_TIMELINE;
        e.a = TL_EV_PREVIEW;
        event_post(e);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Main thread: the index

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

// Sound rows are grouped by kind: the mic, then games, chat, and other apps.
static i32 tl_row_group(const TimelineRow &r) { return r.kind; }

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
            row->kind = st.mic ? KIND_MIC : cat == CAT_GAME ? KIND_GAME : cat == CAT_COMMUNICATION ? KIND_CHAT : KIND_APP;
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
    FRAME_PART("timeline index swap");
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
static void tl_play_sweep();

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
    // The batch's queued keyframes are dropped; those being read come back and are let go
    AcquireSRWLockExclusive(&g_tl.lock);
    for (i32 i = 0; i < g_tl.batch_n; i++) {
        for (i32 k = 0; k < g_tl_batch_asked_n; k++)
            if (g_tl_batch_asked[k] == g_tl.batch_q[i].key) {
                g_tl_batch_asked[k] = g_tl_batch_asked[--g_tl_batch_asked_n];
                break;
            }
    }
    g_tl.batch_n = 0;
    ReleaseSRWLockExclusive(&g_tl.lock);
    g_tl_batch_failed_n = 0;
    if (!tl.idx) return;
    AcquireSRWLockExclusive(&g_tl.lock);
    BufferIdx *spare = g_tl.spare, *ready = g_tl.ready, *last = g_tl.last;
    bool idle = spare || ready; // otherwise the worker is reading into the second block
    if (idle) {
        g_tl.spare = g_tl.ready = g_tl.last = 0;
        g_tl.index_want = false;
    }
    ReleaseSRWLockExclusive(&g_tl.lock);
    if (!idle) return; // next tick
    VirtualFree(tl.idx, 0, MEM_RELEASE);
    if (spare) VirtualFree(spare, 0, MEM_RELEASE);
    if (ready) VirtualFree(ready, 0, MEM_RELEASE);
    if (last) VirtualFree(last, 0, MEM_RELEASE);
    tl.idx = 0;
    tl.loaded = false;
    tl.row_count = 0;
}

// ---------------------------------------------------------------------------------------------------------------
// Main thread: preview frames

static ID3D11ShaderResourceView *tl_upload(const u8 *px)
{
    FRAME_PART("timeline frame upload");
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

// Puts a frame in the cache, in place of the least recently shown one (free slots first: their `used` is 0).
static void tl_frame_store(i64 key, ID3D11ShaderResourceView *srv)
{
    Timeline &tl = g_app.timeline;
    TimelineFrame *slot = &tl.frames[0];
    for (i32 i = 1; i < COUNT(tl.frames); i++)
        if (tl.frames[i].used < slot->used) slot = &tl.frames[i];
    if (slot->srv) slot->srv->Release();
    slot->key = key;
    slot->srv = srv;
    slot->used = ++tl.frame_clock;
}

// The batch's keyframes that are back: into the cache, or onto the failed list.
static void tl_take_batch()
{
    Timeline &tl = g_app.timeline;
    TlBatchDone done[TL_BATCH_MAX];
    AcquireSRWLockExclusive(&g_tl.lock);
    i32 n = g_tl.batch_done_n;
    memcpy(done, g_tl.batch_done, n * sizeof done[0]);
    g_tl.batch_done_n = 0;
    ReleaseSRWLockExclusive(&g_tl.lock);
    for (i32 i = 0; i < n; i++) {
        for (i32 k = 0; k < g_tl_batch_asked_n; k++)
            if (g_tl_batch_asked[k] == done[i].key) {
                g_tl_batch_asked[k] = g_tl_batch_asked[--g_tl_batch_asked_n];
                break;
            }
        // Let go while the screen holds nothing (tl_release has run)
        ID3D11ShaderResourceView *srv = done[i].px && tl.idx ? tl_upload(done[i].px) : 0;
        free(done[i].px);
        if (srv) tl_frame_store(done[i].key, srv);
        else if (tl.idx) {
            if (g_tl_batch_failed_n == TL_BATCH_MAX) {
                memmove(g_tl_batch_failed, g_tl_batch_failed + 1, (TL_BATCH_MAX - 1) * sizeof g_tl_batch_failed[0]);
                g_tl_batch_failed_n--;
            }
            g_tl_batch_failed[g_tl_batch_failed_n++] = done[i].key;
        }
    }
}

static void tl_take_preview()
{
    Timeline &tl = g_app.timeline;
    tl_take_batch();
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
    tl_frame_store(key, srv);
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

// ---------------------------------------------------------------------------------------------------------------
// Share: the range is saved as a clip, read with ffprobe on a thread, then shared as the editor's Share would share
// the whole file (share_decide), or as a link to the host picked from the arrow.

enum { TL_SHARE_NONE, TL_SHARE_SAVING, TL_SHARE_READING, TL_SHARE_RUNNING, TL_SHARE_DONE, TL_SHARE_FAILED };

static struct {
    Path          ffprobe, file;
    MediaInfo     m;
    char          game[64];
    volatile LONG state; // 0 idle, 1 running, 2 read, 3 failed
} g_tl_probe;

static DWORD WINAPI tl_probe_thread(void *)
{
    bool ok = lib_probe(g_tl_probe.ffprobe, g_tl_probe.file, &g_tl_probe.m, g_tl_probe.game, sizeof g_tl_probe.game);
    InterlockedExchange(&g_tl_probe.state, ok ? 2 : 3);
    return 0;
}

static void tl_share_end(bool ok, const char *line)
{
    Timeline &tl = g_app.timeline;
    tl.share_step = (u8)(ok ? TL_SHARE_DONE : TL_SHARE_FAILED);
    str_set(tl.share_line, line);
    log_line("timeline: share %s: %s", ok ? "done" : "failed", line);
    ui_wake();
}

static bool tl_sharing() { return g_app.timeline.share_step >= TL_SHARE_SAVING && g_app.timeline.share_step <= TL_SHARE_RUNNING; }

// What the selection would be as a file, for judging the hosts before it's saved: its length at the recording's
// size and frame rate.
static MediaInfo tl_share_guess()
{
    Timeline &tl = g_app.timeline;
    const Config &c = g_app.config;
    MediaInfo m = {};
    i32 sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    m.height = c.height > 0 ? c.height : sh > 0 ? sh : 1080;
    m.width = sh > 0 ? (i32)((i64)m.height * sw / sh) : m.height * 16 / 9;
    m.fps = c.fps > 0 ? c.fps : 60;
    m.duration = tl.sel_out > tl.sel_in ? (f64)(tl.sel_out - tl.sel_in) / 1e6 : 0;
    m.track_count = 1;
    return m;
}

// Once the clip is read: the Share itself. A first link to a free host isn't agreed to here: the library opens on
// the clip with its Share menu instead.
static void tl_share_go(const char *path, const MediaInfo &m, const char *game)
{
    Timeline &tl = g_app.timeline;
    const char *host = tl.share_host[0] ? tl.share_host : 0;
    ShareDecision d = host ? share_decide_host(m, 0, host) : share_decide(m, 0);
    bool link = host || d.kind == SHARE_LINK;
    if (link && !d.link_ok) return tl_share_end(false, d.link_why);
    bool bucket = host ? !strcmp(host, "bucket") : r2_ready();
    if (link && !bucket && !g_app.config.free_links_seen) {
        log_line("timeline: share %s needs the free links notice", path);
        tl.share_step = TL_SHARE_NONE;
        library_share_file(path);
        return;
    }
    wchar_t w[MAX_PATH];
    utf8_to_wide(path, w, MAX_PATH);
    char name[200];
    const char *slash = strrchr(path, '\\');
    str_set(name, slash ? slash + 1 : path);
    if (char *dot = strrchr(name, '.')) *dot = 0;
    tl.share = share_start(d.kind, w, m, 0, name, game, 0, host);
    if (!tl.share) return tl_share_end(false, "Share is busy: try again in a moment");
    tl.share_step = TL_SHARE_RUNNING;
    str_set(tl.share_line, "Getting it ready");
}

// Each frame and tick while a Share runs: the save, the read, then the share's step and percent, then its end.
static void tl_share_poll()
{
    Timeline &tl = g_app.timeline;
    if (tl.share_step == TL_SHARE_SAVING) {
        SaveJobView *v = save_view(tl.job);
        if (v && v->state < SAVE_DONE) return;
        if (!v || v->state != SAVE_DONE) return tl_share_end(false, v && v->error[0] ? v->error : "The save didn't finish");
        Path ffmpeg;
        if (g_tl_probe.state == 1) return tl_share_end(false, "Busy: try again in a moment");
        if (!ffmpeg_find(g_app.config.ffmpeg, &ffmpeg)) return tl_share_end(false, "Share needs ffmpeg");
        g_tl_probe.ffprobe = ffprobe_for(ffmpeg);
        utf8_to_wide(v->path, g_tl_probe.file.s, MAX_PATH);
        g_tl_probe.state = 1;
        HANDLE th = CreateThread(0, 0, tl_probe_thread, 0, 0, 0);
        if (!th) {
            g_tl_probe.state = 0;
            return tl_share_end(false, "Couldn't read the clip");
        }
        CloseHandle(th);
        tl.share_step = TL_SHARE_READING;
        str_set(tl.share_line, "Getting it ready");
    } else if (tl.share_step == TL_SHARE_READING) {
        LONG st = g_tl_probe.state;
        if (st < 2) return;
        g_tl_probe.state = 0;
        if (st == 3) return tl_share_end(false, "Couldn't read the clip");
        char path[MAX_PATH * 3];
        wide_to_utf8(g_tl_probe.file.s, path, sizeof path);
        tl_share_go(path, g_tl_probe.m, g_tl_probe.game);
    } else if (tl.share_step == TL_SHARE_RUNNING) {
        ShareJobView j = share_job(tl.share);
        if (!j.id) tl_share_end(false, "Couldn't share it");
        else if (j.state == SHARE_FAILED) tl_share_end(false, j.error);
        else if (j.state == SHARE_DONE) {
            tl_share_end(true, j.kind == SHARE_LINK ? "Link copied" : "Copied, ready to paste");
            if (j.kind != SHARE_LINK) editor_refresh_shares();
        } else {
            snprintf(tl.share_line, sizeof tl.share_line, "%s %d%%", j.verb[0] ? j.verb : "Sharing", (i32)(j.progress * 100));
        }
    }
}

static bool tl_save(SaveKind kind, bool edit);

// Share the selection (host: a link there; 0 Share's rule)
static void tl_share(const char *host)
{
    Timeline &tl = g_app.timeline;
    if (tl_sharing() || !tl_save(SAVE_CLIP, false)) return;
    tl.share_step = TL_SHARE_SAVING;
    str_set(tl.share_host, host ? host : "");
    str_set(tl.share_line, "Saving the clip");
    log_line("timeline: share queued%s%s", host ? " to " : "", host ? host : "");
}

// After "Save and edit": open the editor once the file is there. A Share goes on a step.
static void tl_check_job()
{
    Timeline &tl = g_app.timeline;
    tl_share_poll();
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

// The Library's unsaved sessions show the buffer's frames too, so the index is kept while the Library is shown, on
// either tab, and Sessions has it ready the moment it's picked.
static bool tl_wanted_by_library()
{
    const Library &lib = g_app.library;
    return g_app.ui.shown && g_app.ui.screen == SCREEN_LIBRARY && !IsIconic(g_app.ui.hwnd) && !lib.player.open &&
           g_app.sessions.row_count > 0;
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

// Asks for the keyframes timeline_keyframe would give for each of `ts` that aren't cached, read by up to
// TL_BATCH_THREADS ffmpegs at once; timeline_keyframe(t, false) then gives each as it's read. Meant to be called
// every frame with what's on screen: a keyframe still queued that isn't in `ts` is dropped. The cache holds
// COUNT(Timeline::frames) frames, shared with the Timeline, so `ts` should be a page of tiles, not a whole list.
static void timeline_keyframes(const i64 *ts, i32 n)
{
    Timeline &tl = g_app.timeline;
    if (!tl.idx) {
        tl_ask_index();
        return;
    }
    const BufferIdx &b = *tl.idx;
    i64 keys[TL_BATCH_MAX];
    i32 segs[TL_BATCH_MAX], kn = 0;
    for (i32 i = 0; i < n && kn < TL_BATCH_MAX; i++) {
        i32 s = tl_seg_at(b, ts[i]);
        if (s < 0) s = tl_seg_after(b, ts[i]);
        if (s >= b.seg_count) continue;
        i64 key = tl_frame_key(b.segs[s].start_us, false);
        bool skip = tl_frame_find(key) != 0;
        for (i32 k = 0; k < kn && !skip; k++) skip = keys[k] == key;
        for (i32 k = 0; k < g_tl_batch_failed_n && !skip; k++) skip = g_tl_batch_failed[k] == key;
        if (skip) continue;
        keys[kn] = key;
        segs[kn++] = s;
    }

    Path ffmpeg = {};
    bool have_ffmpeg = kn && ffmpeg_find(g_app.config.ffmpeg, &ffmpeg);
    AcquireSRWLockExclusive(&g_tl.lock);
    // Queued ones no longer on screen go
    i32 kept = 0;
    for (i32 i = 0; i < g_tl.batch_n; i++) {
        bool want = false;
        for (i32 k = 0; k < kn && !want; k++) want = keys[k] == g_tl.batch_q[i].key;
        if (want) {
            g_tl.batch_q[kept++] = g_tl.batch_q[i];
            continue;
        }
        for (i32 k = 0; k < g_tl_batch_asked_n; k++)
            if (g_tl_batch_asked[k] == g_tl.batch_q[i].key) {
                g_tl_batch_asked[k] = g_tl_batch_asked[--g_tl_batch_asked_n];
                break;
            }
    }
    g_tl.batch_n = kept;
    i32 added = 0;
    for (i32 i = 0; i < kn && have_ffmpeg && g_tl_batch_asked_n < TL_BATCH_MAX; i++) {
        bool asked = false;
        for (i32 k = 0; k < g_tl_batch_asked_n && !asked; k++) asked = g_tl_batch_asked[k] == keys[i];
        if (asked) continue;
        const VideoSeg &seg = b.segs[segs[i]];
        wchar_t run[64], name[32];
        utf8_to_wide(b.runs[seg.run].name, run, COUNT(run));
        utf8_to_wide(seg.name, name, COUNT(name));
        TlPreviewReq &r = g_tl.batch_q[g_tl.batch_n++];
        r = {};
        r.key = keys[i];
        r.file = path_join(path_join(path_join(g_app.config.buffer_dir, L"video"), run), name);
        r.ffmpeg = ffmpeg;
        g_tl_batch_asked[g_tl_batch_asked_n++] = keys[i];
        added++;
    }
    if (added) WakeAllConditionVariable(&g_tl.batch_wake);
    ReleaseSRWLockExclusive(&g_tl.lock);
    if (kn && !have_ffmpeg) {
        static bool told;
        if (!told) log_line("timeline: ffmpeg not found, so no preview frames");
        told = true;
    }
    if (added && !g_tl.batch_threads[0])
        for (i32 i = 0; i < TL_BATCH_THREADS; i++) g_tl.batch_threads[i] = thread_start(tl_batch_thread, 0);
}

// Main thread, once a second.
static void timeline_tick()
{
    Timeline &tl = g_app.timeline;
    tl_check_job();
    // Play's files left over (from a crash, or a build cut short) are deleted at the start and whenever it opens,
    // so names aren't taken when save ids start again from 1. At the start, so are saves' work folders, once no
    // save is running.
    static bool started, was_shown, work_swept;
    bool shown = tl_visible();
    if (!started || (shown && !was_shown)) tl_play_sweep();
    if (!work_swept) work_swept = save_sweep_work();
    started = true;
    was_shown = shown;
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
    if (game_is_none(name)) return;
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

static bool tl_save(SaveKind kind, bool edit)
{
    Timeline &tl = g_app.timeline;
    if (!tl.sel_in || tl.sel_out <= tl.sel_in || tl.job_edit || !tl.idx) return false;
    if (!tl_has_video(*tl.idx, tl.sel_in, tl.sel_out)) return false;
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
        if (!sid) return false;
        sessions_save(sid);
        const Session *ses = session_find(sid);
        if (ses && ses->state == SES_SAVING) id = ses->job;
    } else {
        id = save_range(tl.sel_in, tl.sel_out, kind, "");
    }
    if (!id) return false;
    tl.job = id;
    tl.job_edit = edit;
    if (tl.share_step >= TL_SHARE_DONE) tl.share_step = TL_SHARE_NONE; // the status shows this save now
    log_line("timeline: save %u queued (%s%s, %.1f s)", id, kind == SAVE_SESSION ? "session" : "clip",
             edit ? ", then edit" : "", (f64)(tl.sel_out - tl.sel_in) / 1e6);
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// Play: the buffer's segments have no sound of their own and are 2 s each, so Play builds a piece of the buffer with
// its sound mixed into one track (a SAVE_PREVIEW) and plays that. The first piece is 10 s, so it starts in about a
// second; while a piece plays the next one (a minute) is built, so they follow on without a stop. A playhead left
// still for a moment has its first piece built ahead, given up as soon as it moves, so Play can start at once.

static i64 tl_playhead();

static const i64 TL_PLAY_FIRST = 10 * TL_SEC, TL_PLAY_CHUNK = 60 * TL_SEC;
static const u64 TL_PLAY_REST_MS = 750;

// Deletes preview files, finished or half written, left in the playback cache, except the ones in use.
static void tl_play_sweep()
{
    Timeline &tl = g_app.timeline;
    const Path &dir = g_app.config.play_dir;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(path_join(dir, L"preview-*").s, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        Path f = path_join(dir, fd.cFileName);
        const Path *used[] = {&tl.play.path, &tl.next.path, &tl.rest.path};
        bool keep = false;
        for (const Path *u : used) keep |= u->s[0] && !_wcsicmp(f.s, u->s);
        if (!keep) DeleteFileW(f.s);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// Gives up a piece: its build cancelled, its file deleted.
static void tl_piece_drop(PlayPiece &pc)
{
    if (pc.job) save_cancel(pc.job);
    if (pc.path.s[0]) DeleteFileW(pc.path.s);
    pc = {};
}

// Starts building a piece from `at` (the footage after it when it's in a gap), at most `len` long and not past `end`
// or the newest footage. 1 when started, 0 when it couldn't be queued, -1 when there's under a second of footage.
static i32 tl_piece_build(PlayPiece &pc, i64 at, i64 len, i64 end)
{
    const BufferIdx &b = *g_app.timeline.idx;
    pc = {};
    i32 first = tl_seg_after(b, at);
    if (first >= b.seg_count) return -1;
    if (at < b.segs[first].start_us) at = b.segs[first].start_us;
    i64 to = ImMin(at + len, ImMin(end, b.newest_us));
    if (to - at < TL_SEC) return -1;
    pc.job = save_range(at, to, SAVE_PREVIEW, "");
    if (!pc.job) return 0;
    pc.file_start = b.segs[first].start_us;
    pc.at = at;
    pc.to = to;
    return 1;
}

// Where Play asked at `at` starts: at the live edge, 30 s back; never before the oldest footage.
static i64 tl_play_from(i64 at)
{
    const BufferIdx &b = *g_app.timeline.idx;
    if (at >= b.newest_us - TL_SEC) at = b.newest_us - 30 * TL_SEC;
    return at < b.oldest_us ? b.oldest_us : at;
}

// Where Play from `at` stops: the selection's end when `at` is in it, else wherever the footage ends.
static i64 tl_play_end(i64 at)
{
    const Timeline &tl = g_app.timeline;
    return tl.sel_in && at >= tl.sel_in && at < tl.sel_out ? tl.sel_out : INT64_MAX / 2;
}

// Stops playing and every build, and deletes their files.
static void tl_play_stop()
{
    Timeline &tl = g_app.timeline;
    bool had = tl.player.open || tl.play.job || tl.play.path.s[0] || tl.next.job || tl.next.path.s[0] || tl.rest.job ||
               tl.rest.path.s[0];
    player_close(&tl.player);
    tl_piece_drop(tl.play);
    tl_piece_drop(tl.next);
    tl_piece_drop(tl.rest);
    if (had) tl_play_sweep();
}

// Where the playing file's `pos` seconds are on the wall clock: its segments end to end from its file_start.
static i64 tl_play_wall(f64 pos)
{
    Timeline &tl = g_app.timeline;
    const BufferIdx &b = *tl.idx;
    i64 left = (i64)(pos * 1e6);
    for (i32 i = tl_seg_after(b, tl.play.file_start); i < b.seg_count && b.segs[i].start_us < tl.play.to; i++) {
        i64 len = b.segs[i].end_us - b.segs[i].start_us;
        if (left < len) return b.segs[i].start_us + left;
        left -= len;
    }
    return tl.play.to;
}

// The other way: the file's time at `t`, the next footage when `t` is in a gap; -1 when the file doesn't reach it.
static f64 tl_play_pos(i64 t)
{
    Timeline &tl = g_app.timeline;
    const BufferIdx &b = *tl.idx;
    if (t < tl.play.file_start) return -1;
    i64 acc = 0;
    for (i32 i = tl_seg_after(b, tl.play.file_start); i < b.seg_count && b.segs[i].start_us < tl.play.to; i++) {
        const VideoSeg &g = b.segs[i];
        if (t < g.start_us) return (f64)acc / 1e6;
        if (t < g.end_us) return (f64)(acc + t - g.start_us) / 1e6;
        acc += g.end_us - g.start_us;
    }
    return -1;
}

// Plays the finished piece in tl.play from where it was asked, and starts building the one after it.
static void tl_play_open()
{
    Timeline &tl = g_app.timeline;
    const BufferIdx &b = *tl.idx;
    tl.player.id = 2;
    f64 pos = tl_play_pos(tl.play.at);
    player_open(&tl.player, tl.play.path, pos > 0 ? pos : 0, true);
    tl.playhead_us = tl.play_set_us = tl.play.at;
    tl_piece_drop(tl.next);
    if (tl.play.to < ImMin(tl.play_end, b.newest_us) - 2 * TL_SEC)
        tl_piece_build(tl.next, tl.play.to, TL_PLAY_CHUNK, tl.play_end);
}

// Plays from `at`: builds its first piece.
static void tl_play_render(i64 at)
{
    Timeline &tl = g_app.timeline;
    const BufferIdx &b = *tl.idx;
    tl_play_stop();
    tl.play_error[0] = 0;
    if (!b.seg_count) return;
    at = tl_play_from(at);
    tl.play_end = tl_play_end(at);
    i32 r = tl_piece_build(tl.play, at, TL_PLAY_FIRST, tl.play_end);
    if (r == 0) str_set(tl.play_error, "Couldn't start the preview");
    if (r > 0) tl.playhead_us = tl.play_set_us = tl.play.at;
}

// Play or Pause.
static void tl_play_toggle()
{
    Timeline &tl = g_app.timeline;
    Player &p = tl.player;
    if (tl.play.job) { // still building: Pause gives up on it
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
    // The piece built while the playhead rested here: play it, or as soon as it's done
    if ((tl.rest.job || tl.rest.path.s[0]) && tl.rest_us == tl.playhead_us) {
        PlayPiece r = tl.rest;
        tl.rest = {};
        tl_play_stop();
        tl.play = r;
        tl.play_error[0] = 0;
        tl.play_end = tl_play_end(r.at);
        tl.playhead_us = tl.play_set_us = r.at;
        if (!r.job) tl_play_open();
        return;
    }
    tl_play_render(at);
}

// Every frame: take the builds that finished, keep the playhead on the playing video, go on to the next piece at the
// end of one, and build ahead at a resting playhead.
static void tl_play_update()
{
    Timeline &tl = g_app.timeline;
    Player &p = tl.player;
    char why[160] = "";
    // 1 = just done (its path set), -1 = failed or cancelled (why says), 0 = still building or none
    auto settle = [&](PlayPiece &pc) {
        if (!pc.job) return 0;
        SaveJobView *v = save_view(pc.job);
        if (v && v->state < SAVE_DONE) return 0;
        pc.job = 0;
        if (v && v->state == SAVE_DONE) {
            utf8_to_wide(v->path, pc.path.s, MAX_PATH);
            return 1;
        }
        str_set(why, v && v->state == SAVE_FAILED ? v->error : "The preview stopped");
        pc = {};
        return -1;
    };
    i32 r = settle(tl.play);
    if (r < 0) str_set(tl.play_error, why);
    if (r > 0) {
        tl_play_open();
        return;
    }
    if (settle(tl.next) < 0) str_set(tl.play_error, why);
    settle(tl.rest);
    if (tl.play.job) return;

    if (!p.open) {
        // A playhead left still: its first piece built ahead, one at a time, given up as soon as the playhead moves.
        // Not at the live edge, which moves on.
        u64 ms = GetTickCount64();
        if (tl.playhead_us != tl.rest_us) {
            tl_piece_drop(tl.rest);
            tl.rest_us = tl.playhead_us;
            tl.rest_since = ms;
            tl.rest_tried = false;
        } else if (tl.drag != TL_DRAG_NONE) {
            tl.rest_since = ms; // a drag that hasn't moved the playhead (yet) keeps what's built, and waits to build
        } else if (!tl.rest_tried && ms - tl.rest_since >= TL_PLAY_REST_MS) {
            tl.rest_tried = true;
            const BufferIdx &b = *tl.idx;
            if (b.seg_count && tl.playhead_us && tl.playhead_us < b.newest_us - TL_SEC) {
                i64 at = tl_play_from(tl.playhead_us);
                tl_piece_build(tl.rest, at, TL_PLAY_FIRST, tl_play_end(at));
            }
        }
        return;
    }
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
    // At the end of a piece, the next one once it's built; none means the end (of the selection, the footage, or a
    // build that failed).
    if (p.ended && p.autoplay) {
        if (tl.next.path.s[0]) {
            player_close(&p);
            DeleteFileW(tl.play.path.s);
            tl.play = tl.next;
            tl.next = {};
            tl_play_open();
        } else if (!tl.next.job) p.autoplay = false;
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

// Keeps the view within the buffer. A view following now keeps its right edge there; only Back to now starts
// following, never a pan or zoom that reaches now.
static void tl_clamp_view(i64 now)
{
    Timeline &tl = g_app.timeline;
    const BufferIdx &b = *tl.idx;
    i64 max_span = tl_max_span(b, now);
    if (tl.view_span_us < TL_MIN_SPAN) tl.view_span_us = TL_MIN_SPAN;
    if (tl.view_span_us > max_span) tl.view_span_us = max_span;
    i64 margin = tl.view_span_us / 30; // room to see the now edge
    i64 latest = now + margin;
    if (tl.follow) tl.view_end_us = latest;
    i64 earliest = (b.oldest_us ? b.oldest_us : now - TL_DAY) - margin + tl.view_span_us;
    if (tl.view_end_us < earliest) tl.view_end_us = earliest;
    if (tl.view_end_us > latest) tl.view_end_us = latest;
}

static void tl_zoom(f64 factor, i64 around, i64 now)
{
    Timeline &tl = g_app.timeline;
    i64 start = tl.view_end_us - tl.view_span_us;
    // The time zoomed around keeps its place on screen, or comes to the middle if it was off it
    f64 frac = tl.view_span_us && around >= start && around <= tl.view_end_us ? (f64)(around - start) / (f64)tl.view_span_us : 0.5;
    i64 span = (i64)((f64)tl.view_span_us * factor);
    i64 max_span = tl_max_span(*tl.idx, now);
    if (span < TL_MIN_SPAN) span = TL_MIN_SPAN;
    if (span > max_span) span = max_span;
    tl.view_span_us = span;
    // A view following now keeps following; otherwise the time zoomed around stays put.
    if (!tl.follow) tl.view_end_us = around + (i64)((1 - frac) * (f64)span);
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
    // With no other end, or the other end on the wrong side, it goes as far as the buffer reaches
    if (in) {
        if (!tl.sel_in || tl.sel_out <= t) tl.sel_out = b.newest_us;
        tl.sel_in = t;
    } else {
        if (!tl.sel_in || tl.sel_in >= t) tl.sel_in = b.oldest_us;
        tl.sel_out = t;
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

// The timeline hotkey, pressed in a game: the last 2 minutes following now, the last 30 s picked (or the selection
// left there, if it ended within the minute). Enter saves it and hides the window, Esc just hides it.
static void timeline_quick()
{
    Timeline &tl = g_app.timeline;
    ui_show(SCREEN_TIMELINE);
    i64 now = now_us();
    tl.quick = true;
    tl.follow = false;
    tl.view_span_us = 120 * TL_SEC;
    tl.view_end_us = now + tl.view_span_us / 30;
    if (!(tl.sel_in && tl.sel_out > tl.sel_in && now - tl.sel_out < 60 * TL_SEC)) {
        tl.sel_out = tl_whole_sec(now) - TL_SEC;
        tl.sel_in = tl.sel_out - 30 * TL_SEC;
    }
    tl.playhead_us = tl.sel_in;
    log_line("timeline: quick save");
}

// Quick save is done: back to the game
static void tl_quick_done()
{
    Timeline &tl = g_app.timeline;
    if (!tl.quick) return;
    tl.quick = false;
    PostMessageW(g_app.ui.hwnd, WM_CLOSE, 0, 0);
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
    if (ImGui::IsKeyPressed(ImGuiKey_I, false) || ImGui::IsKeyPressed(ImGuiKey_Q, false)) tl_set_edge(true, tl_playhead());
    if (ImGui::IsKeyPressed(ImGuiKey_O, false) || ImGui::IsKeyPressed(ImGuiKey_E, false)) tl_set_edge(false, tl_playhead());
    if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) {
        tl_save(SAVE_CLIP, false);
        tl_quick_done();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) tl_quick_done();
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

// Something saved from the buffer, as the Saved row draws it
struct TlSaved {
    i64  a, z;
    bool session;
    char name[96];
    char path[512]; // UTF-8
};

// The saves that reach into [from, to]: sessions first (drawn under), then clips, each oldest first.
static i32 tl_saved_make(TlSaved *out, i32 cap, i64 from, i64 to)
{
    const Saving &sv = g_app.saving;
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
            str_set(v.path, r.path);
            const char *file = strrchr(r.path, '\\');
            str_set(v.name, file ? file + 1 : r.path);
            char *dot = strrchr(v.name, '.');
            if (dot) *dot = 0;
        }
    }
    return n;
}

// Opens a save in the Library's player, or the default player when the library doesn't have it yet.
static void tl_saved_open(const char *path)
{
    Library &lib = g_app.library;
    Path w = {};
    utf8_to_wide(path, w.s, COUNT(w.s));
    for (i32 i = 0; i < lib.item_count; i++) {
        if (_wcsicmp(lib_item_path(lib.items[i]).s, w.s)) continue;
        g_app.ui.screen = SCREEN_LIBRARY;
        library_play(lib.items[i]);
        return;
    }
    ShellExecuteW(0, L"open", w.s, 0, 0, SW_SHOWNORMAL);
}

// The time between frames along the video row for frames at least `least` apart; 0 when zoomed out too far for them.
static i64 tl_thumb_step(i64 least)
{
    static const i64 steps[] = {2, 4, 10, 20, 30, 60, 120};
    for (i64 st : steps)
        if (st * TL_SEC >= least) return st * TL_SEC;
    return 0;
}

// The keyframe at `t` for the video row; asked for when nothing else is being read.
static ID3D11ShaderResourceView *tl_thumb(i64 t)
{
    Timeline &tl = g_app.timeline;
    const BufferIdx &b = *tl.idx;
    i32 sg = tl_seg_at(b, t);
    if (sg < 0) return 0;
    TimelineFrame *f = tl_frame_find(tl_frame_key(b.segs[sg].start_us, false));
    if (f) {
        f->used = ++tl.frame_clock;
        return f->srv;
    }
    if (!tl.pending_key) tl_want_frame(t, false);
    return 0;
}

// Sound groups, collapsible from their header in the track list. Kept for the run, like the view.
// The sound lanes' waveforms: each stream's peaks.bin (audio.cpp), read whole once and then as it grows, with a
// coarse copy (the loudest of each 64) so a long view reads a value or two per pixel. Kept for the streams drawn
// lately; the rest are let go.
enum { TL_PEAKS_MAX = 192, TL_PEAK_US = 100000, TL_PEAK_COARSE = 64 };
struct TlPeaks {
    char dir[128];
    u8  *v, *coarse;
    i32  count, cap;
    u64  read_tick;  // GetTickCount64 when the file was last read
    u32  used;       // the frame it was last drawn in
};
static TlPeaks g_tl_peaks[TL_PEAKS_MAX];
static u32 g_tl_peaks_frame;

static const TlPeaks *tl_peaks(const BufferIdx &b, const AudioStreamInfo &info, bool growing)
{
    TlPeaks *p = 0, *oldest = &g_tl_peaks[0];
    for (TlPeaks &q : g_tl_peaks) {
        if (q.dir[0] && !strcmp(q.dir, info.dir)) { p = &q; break; }
        if (q.used < oldest->used) oldest = &q;
    }
    u64 tick = GetTickCount64();
    if (!p) {
        p = oldest;
        free(p->v);
        free(p->coarse);
        *p = {};
        str_set(p->dir, info.dir);
    } else if (!growing || tick - p->read_tick < 1000) {
        p->used = g_tl_peaks_frame;
        return p;
    }
    p->used = g_tl_peaks_frame;
    p->read_tick = tick;
    // What's been added since the last read
    wchar_t wdir[128];
    utf8_to_wide(info.dir, wdir, COUNT(wdir));
    Path path = path_join(path_join(path_join(g_tl.buffer_dir, L"audio"), wdir), L"peaks.bin");
    HANDLE h = CreateFileW(path.s, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0, OPEN_EXISTING, 0, 0);
    if (h == INVALID_HANDLE_VALUE) return p;
    LARGE_INTEGER size;
    if (GetFileSizeEx(h, &size) && size.QuadPart > p->count && size.QuadPart < (1 << 24)) {
        i32 n = (i32)size.QuadPart;
        if (n > p->cap) {
            i32 cap = n + 6000; // ten minutes to grow into
            u8 *v = (u8 *)realloc(p->v, cap), *c = (u8 *)realloc(p->coarse, cap / TL_PEAK_COARSE + 1);
            if (v) p->v = v;
            if (c) p->coarse = c;
            if (v && c) p->cap = cap;
        }
        LARGE_INTEGER at;
        at.QuadPart = p->count;
        DWORD got = 0;
        if (n <= p->cap && SetFilePointerEx(h, at, 0, FILE_BEGIN) && ReadFile(h, p->v + p->count, n - p->count, &got, 0)) {
            i32 from = p->count / TL_PEAK_COARSE;
            p->count += (i32)got;
            for (i32 k = from; k * TL_PEAK_COARSE < p->count; k++) {
                u8 m = 0;
                for (i32 i = k * TL_PEAK_COARSE; i < (k + 1) * TL_PEAK_COARSE && i < p->count; i++) m = p->v[i] > m ? p->v[i] : m;
                p->coarse[k] = m;
            }
        }
    }
    CloseHandle(h);
    (void)b;
    return p;
}

// The loudest of a stream's peaks from a to z (wall clock), 0..1 as drawn
static f32 tl_peak_in(const TlPeaks &p, i64 start_us, i64 a, i64 z)
{
    i64 i0 = (a - start_us) / TL_PEAK_US, i1 = (z - start_us) / TL_PEAK_US + 1;
    if (i0 < 0) i0 = 0;
    if (i1 > p.count) i1 = p.count;
    if (i1 <= i0) return 0;
    u8 m = 0;
    if (i1 - i0 > 2 * TL_PEAK_COARSE) {
        for (i64 k = i0 / TL_PEAK_COARSE; k * TL_PEAK_COARSE < i1; k++) m = p.coarse[k] > m ? p.coarse[k] : m;
    } else {
        for (i64 i = i0; i < i1; i++) m = p.v[i] > m ? p.v[i] : m;
    }
    return m / 255.f;
}

// The sound lanes: a lane for each app, or every app of a kind summed into one of four (the quick clip timeline
// always, the main one in Simple).
static const i32 TL_LANES = 4;

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
        tl.view_end_us = now + tl.view_span_us / 30;
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
    // The preview on top, then one row of controls, then the tracks filling the rest. The gap above the controls is
    // a splitter (dragged, double-click for the default), kept as a share of the height.
    Config &cfg = g_app.config;
    const f32 gap = 12 * s;
    const f32 pad_r = 14 * s, ruler_h = wa_ruler_h(), saved_h = 9 * s, game_h = 22 * s, video_h = 36 * s;
    const f32 zoom_bar_h = wa_viewbar_h(), zoom_bar_room = zoom_bar_h + 6 * s;
    const f32 line_max = 40 * s, line_min = 20 * s; // a sound lane: as tall as fits, between these
    const f32 min_tracks = 8 * s + zoom_bar_room + ruler_h + saved_h + game_h + video_h + 3 * s + TL_LANES * line_min + 10 * s;
    const f32 top_min = 160 * s;
    auto top_height = [&]() {
        // By default the tracks get what they use at their tallest and the preview the rest
        const f32 tracks_full = 8 * s + zoom_bar_room + ruler_h + saved_h + game_h + video_h + 3 * s + TL_LANES * line_max + 10 * s;
        f32 h = cfg.timeline_top > 0 ? avail.y * cfg.timeline_top : avail.y - fh - 2 * gap - tracks_full;
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

    ImVec2 origin(top.x, y_tools + fh + gap);
    ImVec2 card_z(origin.x + avail.x, origin.y + card_h);
    TlCanvas c = {};
    c.dl = dl;
    c.x1 = card_z.x - pad_r;
    f32 y_zoom = origin.y + 8 * s, y_ruler = y_zoom + zoom_bar_room, y_tracks = y_ruler + ruler_h;
    f32 y_saved = y_tracks + 2 * s, y_game = y_saved + saved_h + 1 * s, y_video = y_game + game_h;
    f32 y_snd = y_video + video_h; // each row is inset 1 px, so every gap is 2
    f32 y_bottom = card_z.y - 10 * s;
    bool names_hot = false;
    {
        static f32 grab_dx;
        f32 ex = origin.x + label_w;
        ImGui::SetCursorScreenPos(ImVec2(ex - 7 * s, y_tracks));
        ImGui::InvisibleButton("##split_names", ImVec2(7 * s, y_bottom - y_tracks));
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

    // The sound lanes share what's left below the video, scrolled when there are more than fit
    bool summed = tl.quick || cfg.timeline_simple || !tl.row_count;
    i32 lanes = summed ? TL_LANES : tl.row_count;
    i8 row_lane[COUNT(tl.rows)];
    for (i32 r = 0; r < tl.row_count; r++) row_lane[r] = (i8)(summed ? tl_row_group(tl.rows[r]) : r);
    // As tall as fits, or the height picked with Shift+wheel over the tracks
    f32 line_h = floorf((y_bottom - y_snd) / (lanes < TL_LANES ? TL_LANES : lanes));
    if (line_h > line_max) line_h = line_max;
    if (line_h < line_min) line_h = line_min;
    const f32 lane_least = 16 * s, lane_most = 120 * s;
    if (cfg.timeline_lane > 0) line_h = ImClamp(floorf(cfg.timeline_lane * s), lane_least, lane_most);
    bool over_lanes = ImGui::IsWindowHovered() && io.MousePos.y >= y_snd && io.MousePos.y < y_bottom &&
                      io.MousePos.x >= origin.x && io.MousePos.x < c.x1;
    if (over_lanes && io.MouseWheel != 0 && io.KeyShift && !io.KeyAlt) {
        f32 old_h = line_h;
        line_h = ImClamp(floorf(line_h + (io.MouseWheel > 0 ? 4 : -4) * s), lane_least, lane_most);
        cfg.timeline_lane = (i32)(line_h / s + 0.5f);
        tl.lane_scroll *= line_h / old_h; // the same lanes stay in view
        config_save(cfg, g_app.paths);
    }
    f32 scroll_most = lanes * line_h - (y_bottom - y_snd);
    if (scroll_most < 0) scroll_most = 0;
    if (over_lanes && io.MouseWheel != 0 && !io.KeyShift && !io.KeyAlt && io.MousePos.x < c.x0)
        tl.lane_scroll -= io.MouseWheel * line_h; // the wheel on the names scrolls the lanes (on the bars it pans)
    if (tl.lane_scroll > scroll_most) tl.lane_scroll = scroll_most;
    if (tl.lane_scroll < 0) tl.lane_scroll = 0;
    f32 y_lanes = y_snd - tl.lane_scroll; // where lane 0 is
    // Summed, lane k is the kind k (KIND_MIC to KIND_APP); otherwise row k
    auto lane_icon = [&](i32 k) { return KIND_LOOK[summed ? k : tl.rows[k].kind].icon; };
    auto lane_name = [&](i32 k) { return summed ? KIND_LOOK[k].name : (const char *)tl.rows[k].name; };
    auto lane_col = [&](i32 k) { return KIND_LOOK[summed ? k : tl.rows[k].kind].rgb; };
    bool compact = line_h < 24 * s; // names in the small font
    f32 lane_in = ImMax(floorf(1 * s), 1.0f), bar_in = lane_in; // rows 2 px apart

    // What's been saved from here: sessions under, clips over them. A click on one selects it.
    saved_check();
    const Saving &sv = g_app.saving;
    static TlSaved saved[COUNT(sv.saved) + COUNT(g_app.sessions.items)];
    static TlSaved saved_open; // the one whose menu is open
    i32 saved_count = tl_saved_make(saved, COUNT(saved), c.start - c.span, c.start + 2 * c.span); // room for a pan
    i32 saved_hit = -1;
    if (ImGui::IsWindowHovered() && io.MousePos.y >= y_saved - 2 * s && io.MousePos.y < y_saved + saved_h + 2 * s &&
        io.MousePos.x >= c.x0 && io.MousePos.x <= c.x1 && tl.drag == TL_DRAG_NONE) {
        i64 t = tl_t(c, io.MousePos.x), slack = (i64)(3 * s / c.px_per_us); // tiny clips stay clickable
        for (i32 i = saved_count - 1; i >= 0 && saved_hit < 0; i--)
            if (t >= saved[i].a - slack && t <= saved[i].z + slack) saved_hit = i;
    }

    // The game part under the mouse, on the game row: Shift and a click adds it to the selected region
    i64 game_a = 0, game_z = 0;
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) && io.MousePos.y >= y_game &&
        io.MousePos.y < y_game + game_h && io.MousePos.x >= c.x0 && io.MousePos.x <= c.x1) {
        i64 t = tl_t(c, io.MousePos.x);
        for (i32 i = b.activity_count - 1; i >= 0; i--) {
            i64 end = i + 1 < b.activity_count ? b.activity[i + 1].us : now;
            if (b.activity[i].exe[0] && t >= b.activity[i].us && t < end) {
                game_a = b.activity[i].us;
                game_z = end;
                break;
            }
        }
    }

    // Markers in view, on the ruler. A click on one puts the playhead there and shows its note; a right click opens
    // the whole note.
    static Marker marks[256];
    static Marker mark_open; // the one whose note is open
    i32 mark_count = markers_in(c.start - c.span / 50, c.start + c.span + c.span / 50, marks, COUNT(marks));
    i32 mark_hit = -1;
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) && io.MousePos.y >= y_ruler &&
        io.MousePos.y < y_tracks && (tl.drag == TL_DRAG_NONE || !tl.dragged)) { // the timeline's own click is active
        f32 best = 6 * s;
        for (i32 i = 0; i < mark_count; i++) {
            f32 d = fabsf(io.MousePos.x - tl_x(c, marks[i].t_us));
            if (d <= best) { best = d; mark_hit = i; }
        }
    }

    dl->AddRectFilled(origin, card_z, col32(COL_PANEL), 10 * s);
    if (names_hot) {
        f32 x = floorf(c.x0 - 3 * s) + 0.5f;
        dl->AddLine(ImVec2(x, y_tracks), ImVec2(x, y_bottom), col32(COL_DIM), 1.5f * s);
    }

    // --- The zoom bar: the whole buffer, with a window on the part shown. Drag the window to pan, either end to
    // zoom, beside it to bring it there; double-click shows everything. --------------------------------------------
    {
        static i32 grab;              // 1 the left end, 2 the right end, 3 the window
        static i64 grab_off, grab_span, held_lo, held_hi, grab_start, grab_end, grab_play;
        i64 lo = (b.oldest_us ? b.oldest_us : now - TL_DAY) - c.span / 30, hi = now + c.span / 30;
        if (c.start < lo) lo = c.start;
        if (c.start + c.span > hi) hi = c.start + c.span;
        f32 bx0 = c.x0, bw = c.x1 - c.x0;
        ImGui::SetCursorScreenPos(ImVec2(bx0, y_zoom - 4 * s));
        ImGui::InvisibleButton("##zoombar", ImVec2(bw, zoom_bar_h + 8 * s));
        bool active = ImGui::IsItemActive(), hot = ImGui::IsItemHovered() || active;
        if (active && grab) { lo = held_lo; hi = held_hi; } // still for the whole drag
        f64 whole = (f64)(hi - lo);
        auto bx = [&](i64 t) { return bx0 + (f32)((f64)(t - lo) / whole * bw); };
        auto bt = [&](f32 x) { return lo + (i64)((f64)(x - bx0) / bw * whole); };
        // The window is drawn at least wide enough for its two grips
        auto window = [&](f32 *wa, f32 *wb) {
            *wa = bx(tl.view_end_us - tl.view_span_us);
            *wb = bx(tl.view_end_us);
            if (*wb - *wa < 14 * s) {
                f32 m = (*wa + *wb) / 2;
                *wa = m - 7 * s;
                *wb = m + 7 * s;
            }
        };
        f32 wa, wb, mx = io.MousePos.x;
        window(&wa, &wb);
        i32 over = fabsf(mx - wa) <= 7 * s && mx < (wa + wb) / 2 ? 1 : fabsf(mx - wb) <= 7 * s ? 2 : mx > wa && mx < wb ? 3 : 0;
        if (ImGui::IsItemActivated()) {
            held_lo = lo;
            held_hi = hi;
            grab = over;
            grab_span = tl.view_span_us;
            grab_start = tl.view_end_us - tl.view_span_us;
            grab_end = tl.view_end_us;
            grab_play = tl_playhead();
            tl.follow = false;
            if (!grab) { // beside the window: it comes here and is held
                tl.view_end_us = bt(mx) + grab_span / 2;
                tl_clamp_view(now);
                grab = 3;
            }
            grab_off = bt(mx) - (grab == 2 ? tl.view_end_us : tl.view_end_us - tl.view_span_us);
        }
        if (active && grab && ImGui::IsMousePosValid()) {
            i64 t = bt(mx) - grab_off, end = tl.view_end_us, start = end - tl.view_span_us;
            f64 a, z;
            // Shift on an end: the work area scales around the playhead, which keeps its place on screen
            if ((grab == 1 || grab == 2) && io.KeyShift &&
                wa_scale((f64)grab_play, (f64)grab_start, (f64)grab_end, grab == 1, (f64)t, (f64)held_lo, (f64)held_hi,
                         (f64)TL_MIN_SPAN, &a, &z)) {
                tl.view_end_us = (i64)z;
                tl.view_span_us = (i64)z - (i64)a;
            } else if (grab == 1) {
                if (t > end - TL_MIN_SPAN) t = end - TL_MIN_SPAN;
                tl.view_span_us = end - t;
            } else if (grab == 2) {
                if (t < start + TL_MIN_SPAN) t = start + TL_MIN_SPAN;
                tl.view_end_us = t;
                tl.view_span_us = t - start;
            } else {
                tl.view_end_us = t + grab_span;
            }
            tl.follow = false;
            tl_clamp_view(now);
        }
        if (ImGui::IsItemDeactivated()) grab = 0;
        if (hot && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            i64 have = now - (b.oldest_us ? b.oldest_us : now - TL_DAY);
            tl.view_span_us = have * 30 / 28; // the whole buffer inside the margins
            tl.view_end_us = now + tl.view_span_us / 30;
            tl.follow = false;
            tl_clamp_view(now);
        }
        if ((hot && !active && (over == 1 || over == 2)) || (active && (grab == 1 || grab == 2)))
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        set_view();

        wa_draw_viewbar(dl, bx0, bx0 + bw, y_zoom, bx(tl.view_end_us - tl.view_span_us), bx(tl.view_end_us), hot,
                        has_sel ? bx(tl.sel_in) : 0, has_sel ? bx(tl.sel_out) : 0, bx(tl_playhead()));
    }

    // --- Top left over the track names, a toggle: lit, every app has its own lane; off, the sound is in four lanes
    // (every app summed into its kind's). Its icon is drawn: three staggered track bars.
    if (!tl.quick) {
        f32 bs = 24 * s;
        bool all = !cfg.timeline_simple;
        ImGui::SetCursorScreenPos(ImVec2(origin.x + 14 * s, floorf(y_ruler + (ruler_h - bs) / 2) - 2 * s));
        kit_next_tip(all ? "View: every app in its own lane"
                         : "View: a lane per kind of sound");
        if (kit_icon_button("", "##lanes", KIT_GHOST, true, all, bs)) {
            cfg.timeline_simple = !cfg.timeline_simple;
            tl.lane_scroll = 0;
            config_save(cfg, g_app.paths);
            log_line("timeline: simple %s", cfg.timeline_simple ? "on" : "off");
        }
        ImVec2 a = ImGui::GetItemRectMin();
        ImU32 ink = col32(all || ImGui::IsItemHovered() ? COL_TEXT : 0xc4c8cf);
        static const f32 bars[3][2] = {{5, 15}, {9, 19}, {5, 13}}; // each bar's start and end, of 24
        for (i32 k = 0; k < 3; k++) {
            f32 y = a.y + (7 + 4.5f * k) * s;
            dl->AddRectFilled(ImVec2(a.x + bars[k][0] * s, floorf(y)), ImVec2(a.x + bars[k][1] * s, floorf(y) + 2 * s), ink, 1 * s);
        }
    }

    // --- The mouse on the bars: one invisible button, so ImGui keeps the drag even outside it ---------------------
    ImGui::SetCursorScreenPos(ImVec2(c.x0, y_ruler));
    ImGui::InvisibleButton("bar", ImVec2(c.x1 - c.x0, card_z.y - y_ruler),
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
    // Shift and a drag anywhere draws a new selection. On the ruler its ends and the band between are taken hold
    // of (a marker first, over the band); everywhere else a press moves the playhead.
    f32 grab = wa_grab();
    bool on_ruler = io.MousePos.y >= y_ruler && io.MousePos.y < y_tracks + 2 * s;
    bool near_in = has_sel && on_ruler && fabsf(io.MousePos.x - tl_x(c, tl.sel_in)) <= grab;
    bool near_out = has_sel && on_ruler && !near_in && fabsf(io.MousePos.x - tl_x(c, tl.sel_out)) <= grab;
    bool on_band = has_sel && on_ruler && !near_in && !near_out && mark_hit < 0 && mouse_t > tl.sel_in &&
                   mouse_t < tl.sel_out;
    // The playhead kept on footage
    auto on_footage = [&](i64 t) { return t >= b.newest_us ? b.newest_us - 1 : t < b.oldest_us ? b.oldest_us : t; };
    // An end of the in and out lands on a marker that comes close
    auto on_marker = [&](i64 t) {
        f32 best = grab;
        i64 to = t;
        for (i32 i = 0; i < mark_count; i++) {
            f32 d = fabsf(tl_x(c, marks[i].t_us) - tl_x(c, t));
            if (d <= best) { best = d; to = marks[i].t_us; }
        }
        return to;
    };
    static i64 grab_off, grab_len, grab_play;
    // Close enough to the playhead (where it was when the drag began) on screen to land on it
    auto near_play = [&](i64 t) { return fabsf(tl_x(c, t) - tl_x(c, grab_play)) <= grab; };
    // With Shift, an end of the in and out catches the nearest edge that comes close on screen: where the video or a
    // shown sound track starts or stops (gaps of up to 50 ms don't count), a game's part, a save, a marker, the
    // buffer's ends, or the playhead (where it was when the drag began).
    auto snap_edge = [&](i64 t) {
        f32 best = 10 * s;
        i64 to = t, w = (i64)(best / c.px_per_us) + 1;
        auto take = [&](i64 e) {
            f32 d = fabsf(tl_x(c, e) - tl_x(c, t));
            if (d <= best) { best = d; to = e; }
        };
        for (i32 i = tl_seg_after(b, t - w); i < b.seg_count && b.segs[i].start_us <= t + w; i++) {
            if (!i || b.segs[i].start_us - b.segs[i - 1].end_us > 50000) take(b.segs[i].start_us);
            if (i + 1 == b.seg_count || b.segs[i + 1].start_us - b.segs[i].end_us > 50000) take(b.segs[i].end_us);
        }
        for (i32 st = 0; st < b.stream_count && st < COUNT(tl.stream_row); st++) {
            if (tl.stream_row[st] < 0) continue;
            const AudioStreamInfo &info = b.streams[st];
            const AudioSeg *sg = b.audio_segs + info.first_seg;
            auto seg_end = [&](i32 j) { return sg[j].open ? ImMin(now, sg[j].start_us + 12 * TL_SEC) : sg[j].end_us; };
            for (i32 j = 0; j < info.seg_count; j++) {
                if (!j || sg[j].start_us - seg_end(j - 1) > 50000) take(sg[j].start_us);
                if (j + 1 == info.seg_count || sg[j + 1].start_us - seg_end(j) > 50000) take(seg_end(j));
            }
        }
        for (i32 i = 0; i < b.activity_count; i++)
            if (b.activity[i].exe[0]) {
                take(b.activity[i].us);
                take(i + 1 < b.activity_count ? b.activity[i + 1].us : now);
            }
        for (i32 i = 0; i < saved_count; i++) { take(saved[i].a); take(saved[i].z); }
        for (i32 i = 0; i < mark_count; i++) take(marks[i].t_us);
        take(b.oldest_us);
        take(b.newest_us);
        take(grab_play);
        return to;
    };
    if (ImGui::IsItemActivated() && b.seg_count) {
        tl.drag_x = io.MousePos.x;
        tl.dragged = false;
        tl.drag_anchor_us = mouse_sec;
        tl.drag_view_end = tl.view_end_us;
        grab_off = mouse_t - tl.sel_in;
        grab_len = tl.sel_out - tl.sel_in;
        grab_play = tl_playhead();
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) tl.drag = TL_DRAG_PAN;
        else if (near_in) tl.drag = TL_DRAG_IN;
        else if (near_out) tl.drag = TL_DRAG_OUT;
        else if (io.KeyShift) tl.drag = TL_DRAG_NEW;
        else if (mark_hit >= 0) tl.drag = TL_DRAG_MARK;
        else if (on_band) tl.drag = TL_DRAG_RANGE;
        else tl.drag = TL_DRAG_SEEK;
        if (tl.drag == TL_DRAG_SEEK) tl.playhead_us = on_footage(mouse_t);
        if (tl.drag == TL_DRAG_MARK) tl.playhead_us = marks[mark_hit].t_us;
    }
    // Dragging an end or the band, the playhead follows the start or the end being moved so its frame shows, and goes
    // back when it's let go. The band lands on the playhead when close; with Shift an end lands on an edge.
    if (ImGui::IsItemActive() && tl.drag != TL_DRAG_NONE && mouse_ok) {
        if (fabsf(io.MousePos.x - tl.drag_x) > 4 * s) tl.dragged = true;
        bool keep = io.KeyShift;
        if (tl.drag == TL_DRAG_PAN) {
            tl.follow = false;
            tl.view_end_us = tl.drag_view_end - (i64)((f64)(io.MousePos.x - tl.drag_x) / c.px_per_us);
            tl_clamp_view(now);
        } else if (tl.drag == TL_DRAG_SEEK) {
            tl.playhead_us = on_footage(mouse_t);
        } else if (tl.drag == TL_DRAG_NEW && tl.dragged) {
            i64 m = on_marker(mouse_sec);
            tl.sel_in = tl.drag_anchor_us < m ? tl.drag_anchor_us : m;
            tl.sel_out = tl.drag_anchor_us < m ? m : tl.drag_anchor_us;
            tl_clamp_selection();
        } else if (tl.drag == TL_DRAG_RANGE && tl.dragged) {
            i64 to = tl_whole_sec(mouse_t - grab_off);
            if (near_play(to)) to = grab_play;
            else if (near_play(to + grab_len)) to = grab_play - grab_len;
            if (to + grab_len > b.newest_us) to = b.newest_us - grab_len;
            if (to < b.oldest_us) to = b.oldest_us;
            tl.sel_in = to;
            tl.sel_out = to + grab_len;
            tl.playhead_us = tl.sel_in;
        } else if (tl.drag == TL_DRAG_IN) {
            i64 t = keep ? snap_edge(mouse_t) : on_marker(mouse_sec);
            tl.sel_in = t < tl.sel_out - TL_SEC ? t : tl.sel_out - TL_SEC;
            tl_clamp_selection();
            tl.playhead_us = on_footage(tl.sel_in);
        } else if (tl.drag == TL_DRAG_OUT) {
            i64 t = keep ? snap_edge(mouse_t) : on_marker(mouse_sec);
            tl.sel_out = t > tl.sel_in + TL_SEC ? t : tl.sel_in + TL_SEC;
            tl_clamp_selection();
            tl.playhead_us = on_footage(tl.sel_out);
        }
    }
    // A double-click on a save plays it from its start, once the button is let go
    static i64 play_saved_at;
    if (bar_hovered && saved_hit >= 0 && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) play_saved_at = saved[saved_hit].a;
    if (ImGui::IsItemDeactivated()) {
        bool click = !tl.dragged && mouse_ok;
        // Moving an end or the band shows its frame while dragging; the playhead stays where it was
        if (tl.drag == TL_DRAG_IN || tl.drag == TL_DRAG_OUT || tl.drag == TL_DRAG_RANGE) tl.playhead_us = grab_play;
        if (play_saved_at) {
            tl.playhead_us = play_saved_at;
            tl.marker_pick = 0;
            play_saved_at = 0;
            tl_play_stop();
            tl_play_render(tl.playhead_us);
        }
        if (click && (tl.drag == TL_DRAG_SEEK || tl.drag == TL_DRAG_NEW)) tl.marker_pick = 0;
        // Shift and a click on a game part: the selected region becomes it, or grows to take it in. A part already
        // inside it is taken out again by the nearer end moving past it (all of it, the region is cleared).
        if (click && tl.drag == TL_DRAG_NEW && game_z > game_a) {
            i64 a = tl_whole_sec(game_a), z = tl_whole_sec(game_z);
            bool had = tl.sel_in && tl.sel_out > tl.sel_in;
            // the part as the region would hold it: within the footage
            i64 ca = ImMax(a, tl_whole_sec(b.oldest_us)), cz = ImMin(z, tl_whole_sec(b.newest_us));
            if (had && ca >= tl.sel_in && cz <= tl.sel_out) {
                if (ca <= tl.sel_in && cz >= tl.sel_out) tl.sel_in = tl.sel_out = 0;
                else if (ca - tl.sel_in <= tl.sel_out - cz) tl.sel_in = cz;
                else tl.sel_out = ca;
            } else {
                tl.sel_in = had && tl.sel_in < a ? tl.sel_in : a;
                tl.sel_out = had && tl.sel_out > z ? tl.sel_out : z;
            }
            tl_clamp_selection();
        }
        if (tl.drag == TL_DRAG_PAN && click && mark_hit >= 0) { // a right (or middle) click
            mark_open = marks[mark_hit];
            tl.marker_pick = mark_open.t_us;
            ImGui::OpenPopup("##marknote");
        } else if (tl.drag == TL_DRAG_PAN && click && saved_hit >= 0) {
            saved_open = saved[saved_hit];
            ImGui::OpenPopup("##savedmenu");
        } else if (tl.drag == TL_DRAG_PAN && click && has_sel && on_ruler && mouse_t >= tl.sel_in && mouse_t <= tl.sel_out) {
            ImGui::OpenPopup("##selmenu"); // a right click on the selection
        } else if (tl.drag == TL_DRAG_MARK && click && mark_hit >= 0) {
            tl.playhead_us = marks[mark_hit].t_us;
            tl.marker_pick = marks[mark_hit].t_us;
        } else if (tl.drag == TL_DRAG_SEEK && click && saved_hit >= 0) {
            // A click on something saved: it becomes the selection
            tl.sel_in = tl_whole_sec(saved[saved_hit].a);
            tl.sel_out = tl_whole_sec(saved[saved_hit].z);
            if (tl.sel_out <= tl.sel_in) tl.sel_out = tl.sel_in + TL_SEC;
            tl_clamp_selection();
            tl.playhead_us = tl.sel_in;
        }
        tl.drag = TL_DRAG_NONE;
    }
    // The wheel pans; with Alt it zooms around the mouse, with Alt and Shift around the playhead
    if (bar_hovered && io.MouseWheel != 0) {
        if (io.KeyAlt) {
            tl_zoom(pow(0.75, (f64)io.MouseWheel), io.KeyShift ? tl_playhead() : mouse_t, now);
        } else if (!io.KeyShift) { // Shift: the lanes' height (above)
            tl.follow = false;
            tl.view_end_us -= (i64)((f64)tl.view_span_us * 0.15 * io.MouseWheel);
            tl_clamp_view(now);
        }
    }
    if (tl.drag == TL_DRAG_IN || tl.drag == TL_DRAG_OUT || (bar_hovered && tl.drag == TL_DRAG_NONE && (near_in || near_out)))
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    else if (tl.drag == TL_DRAG_PAN || tl.drag == TL_DRAG_RANGE || (bar_hovered && tl.drag == TL_DRAG_NONE && on_band))
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    else if (bar_hovered && (saved_hit >= 0 || mark_hit >= 0 || (io.KeyShift && game_z > game_a)))
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
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
    else if (tl.drag == TL_DRAG_RANGE) pill_t = tl.sel_in;
    else if (tl.drag == TL_DRAG_SEEK) pill_t = tl.playhead_us;
    else if (tl.drag == TL_DRAG_NEW && tl.dragged) pill_t = mouse_sec;

    // --- Track names on the left ----------------------------------------------------------------------------------
    f32 tx = origin.x + 18 * s, th = ImGui::GetTextLineHeight();
    f32 name_w = c.x0 - 16 * s - tx;
    {
        ImGui::PushFont(g_font_small);
        f32 sh = ImGui::GetTextLineHeight();
        dl->AddText(ImVec2(tx, floorf(y_video + (video_h - sh) / 2)), col32(COL_FAINT), ICON_VIDEO);
        dl->AddText(ImVec2(tx, floorf(y_game + (game_h - sh) / 2)), col32(COL_FAINT), ICON_GAME);
        dl->AddText(ImVec2(tx + 22 * s, floorf(y_game + (game_h - sh) / 2)), col32(COL_DIM), "Game");
        dl->AddText(ImVec2(tx + 22 * s, floorf(y_saved + (saved_h - sh) / 2)), col32(COL_DIM), "Saved");
        ImGui::PopFont();
        dl->AddText(ImVec2(tx + 22 * s, floorf(y_video + (video_h - th) / 2)), col32(COL_DIM), "Video");
    }
    dl->PushClipRect(ImVec2(origin.x, y_snd), ImVec2(c.x0, y_bottom), true);
    for (i32 k = 0; k < lanes; k++) {
        f32 y = y_lanes + k * line_h;
        if (y + line_h < y_snd || y > y_bottom) continue;
        // An app's lane shows its icon (a game its art from the games database); the mic, the sums and apps without
        // one show the kind's symbol
        ID3D11ShaderResourceView *srv = 0;
        if (!summed && strcmp(tl.rows[k].exe, "mic")) {
            if (tl_row_group(tl.rows[k]) == 1) srv = game_db_icon(tl.rows[k].name);
            if (!srv) srv = app_icon(tl.rows[k].exe);
        }
        f32 isz = floorf(ImMin(16 * s, line_h - 4 * s));
        if (srv) {
            ImVec2 ia(tx, floorf(y + (line_h - isz) / 2));
            dl->AddImage((ImTextureID)srv, ia, ImVec2(ia.x + isz, ia.y + isz));
        } else {
            ImGui::PushFont(g_font_small);
            f32 sh = ImGui::GetTextLineHeight();
            dl->AddText(ImVec2(tx, floorf(y + (line_h - sh) / 2)), col32(COL_FAINT), lane_icon(k));
            ImGui::PopFont();
        }
        if (compact) ImGui::PushFont(g_font_small);
        f32 nh = ImGui::GetTextLineHeight();
        tl_text_fit(dl, ImVec2(tx + 22 * s, floorf(y + (line_h - nh) / 2)), name_w - 22 * s, col32(COL_DIM), lane_name(k));
        if (compact) ImGui::PopFont();
    }
    dl->PopClipRect();
    if (scroll_most > 0) { // where the scrolled lanes are: a thin thumb at the names' left edge
        f32 track = y_bottom - y_snd, thumb = track * track / (lanes * line_h);
        f32 ty = y_snd + (track - thumb) * tl.lane_scroll / scroll_most;
        dl->AddRectFilled(ImVec2(origin.x + 6 * s, ty), ImVec2(origin.x + 9 * s, ty + thumb), col32(COL_FAINT), 2 * s);
    }

    // --- The bars -------------------------------------------------------------------------------------------------
    dl->PushClipRect(ImVec2(c.x0 - 6 * s, origin.y), ImVec2(c.x1 + 6 * s, card_z.y), true);
    f32 lane_r = 6 * s;
    dl->AddRectFilled(ImVec2(c.x0, y_saved), ImVec2(c.x1, y_saved + saved_h), col32(TL_COL_LANE), 3 * s);
    dl->AddRectFilled(ImVec2(c.x0, y_game), ImVec2(c.x1, y_game + game_h), col32(TL_COL_LANE), lane_r);
    dl->AddRectFilled(ImVec2(c.x0, y_video + 1 * s), ImVec2(c.x1, y_video + video_h - 1 * s), col32(TL_COL_LANE), lane_r);
    dl->PushClipRect(ImVec2(c.x0 - 6 * s, y_snd - 4 * s), ImVec2(c.x1 + 6 * s, y_bottom), true);
    for (i32 k = 0; k < lanes; k++) {
        f32 y = y_lanes + k * line_h;
        dl->AddRectFilled(ImVec2(c.x0, y + lane_in), ImVec2(c.x1, y + line_h - lane_in), col32(TL_COL_LANE),
                          ImMin(lane_r, (line_h - 2 * lane_in) / 2));
    }
    dl->PopClipRect();

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
            dl->AddLine(ImVec2(x, y_tracks), ImVec2(x, y_bottom), col32(COL_LINE, 0.55f));
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

    // Video: footage as bars, gaps (lock screen, paused, clipback not running) empty. Zoomed in far enough, a frame
    // every so often along it: each the keyframe at a step of the time grid, so a pan keeps the same frames.
    {
        f32 y = y_video + 1 * s, h = video_h - 2 * s;
        TlRun run = {};
        ImU32 col = col32(TL_COL_VIDEO, 0.9f);
        for (i32 i = tl_seg_after(b, c.start); i < b.seg_count && b.segs[i].start_us < view_end; i++)
            tl_run_add(c, &run, b.segs[i].start_us, b.segs[i].end_us, y, h, col);
        if (run.a) tl_span(c, run.a, run.z, y, h, col);
        f32 thumb_w = floorf(h * (f32)TL_FRAME_W / (f32)TL_FRAME_H);
        i64 step = tl_thumb_step((i64)((f64)thumb_w / c.px_per_us));
        if (step) {
            dl->PushClipRect(ImVec2(c.x0, y), ImVec2(c.x1, y + h), true);
            for (i64 t = (c.start / step) * step; t < view_end; t += step) {
                i32 sg = tl_seg_at(b, t);
                if (sg < 0) continue;
                // Cut off where the footage ends
                i64 end = b.segs[sg].end_us;
                for (i32 k = sg + 1; k < b.seg_count && b.segs[k].start_us - end <= 50000 && end < t + step; k++) end = b.segs[k].end_us;
                f32 xa = tl_x(c, t), xb = ImMin(tl_x(c, end), xa + thumb_w);
                ImVec2 a(floorf(xa), y), z(floorf(xb), y + h);
                ID3D11ShaderResourceView *pic = tl_thumb(t);
                dl->PushClipRect(a, z, true);
                if (pic) dl->AddImage((ImTextureID)pic, a, ImVec2(a.x + thumb_w, z.y));
                dl->PopClipRect();
                dl->AddLine(ImVec2(a.x + 0.5f, y), ImVec2(a.x + 0.5f, y + h), col32(TL_COL_LANE, 0.8f));
            }
            dl->PopClipRect();
        }
    }

    // The game being played, a band each in the game's colour with its icon and name. Bands may overlap.
    {
        f32 y = y_game + 1 * s, h = game_h - 2 * s, isz = floorf(ImMin(16 * s, h - 2 * s));
        ImGui::PushFont(g_font_small);
        for (i32 i = 0; i < b.activity_count; i++) {
            const ActivityEntry &e = b.activity[i];
            i64 end = i + 1 < b.activity_count ? b.activity[i + 1].us : now;
            if (!e.exe[0] || end < c.start || e.us > view_end) continue;
            ImU32 gc = col32(tl_game_colour(e.exe), 0.9f);
            TlRun gr = {};
            i64 lo = e.us > c.start ? e.us : c.start, hi = end < view_end ? end : view_end;
            for (i32 k = tl_seg_after(b, lo); k < b.seg_count && b.segs[k].start_us < hi; k++) {
                i64 sa = b.segs[k].start_us > e.us ? b.segs[k].start_us : e.us, sz = b.segs[k].end_us < end ? b.segs[k].end_us : end;
                if (sz > sa) tl_run_add(c, &gr, sa, sz, y, h, gc);
            }
            if (gr.a) tl_span(c, gr.a, gr.z, y, h, gc);
            f32 xa = tl_x(c, e.us), xb = tl_x(c, end);
            if (xa < c.x0) xa = c.x0;
            if (xb > c.x1) xb = c.x1;
            char name[64];
            audio_display_name(g_app.config, e.exe, name, sizeof name);
            f32 nx = xa + 6 * s;
            ID3D11ShaderResourceView *icon = game_db_icon(name);
            if (!icon) icon = app_icon(e.exe);
            if (icon && xb - xa > isz + 10 * s) {
                ImVec2 ia(nx, floorf(y + (h - isz) / 2));
                dl->AddImage((ImTextureID)icon, ia, ImVec2(ia.x + isz, ia.y + isz));
                nx += isz + 5 * s;
            }
            tl_text_fit(dl, ImVec2(nx, floorf(y + (h - ImGui::GetTextLineHeight()) / 2)), xb - nx - 6 * s, col32(0xffffff), name);
        }
        ImGui::PopFont();
    }

    // Saved, a thin stripe at the top: sessions faint, clips solid over them
    for (i32 i = 0; i < saved_count; i++) {
        const TlSaved &v = saved[i];
        bool hot = i == saved_hit && bar_hovered;
        ImU32 col = v.session ? col32(COL_ACCENT, hot ? 0.75f : 0.4f) : col32(hot ? COL_ACCENT_HOVER : COL_ACCENT, 0.95f);
        tl_span(c, v.a, v.z, y_saved + 1 * s, saved_h - 2 * s, col);
    }

    // Sound: when each app had a stream open, faintly, and over it the loudest of the lane's apps at each moment
    g_tl_peaks_frame++;
    static f32 lane_peak[COUNT(tl.rows)][2048];
    static f32 lane_cov[COUNT(tl.rows)][2048][2]; // the part of each column the lane's bars cover, x from and to
    f32 wstep = floorf(2 * s) > 1 ? floorf(2 * s) : 1;
    i32 columns = (i32)((c.x1 - c.x0) / wstep) + 1;
    if (columns > 2048) columns = 2048;
    memset(lane_peak, 0, lanes * sizeof lane_peak[0]);
    memset(lane_cov, 0, lanes * sizeof lane_cov[0]);
    dl->PushClipRect(ImVec2(c.x0 - 6 * s, y_snd - 4 * s), ImVec2(c.x1 + 6 * s, y_bottom), true);
    for (i32 st = 0; st < b.stream_count && st < COUNT(tl.stream_row); st++) {
        if (tl.stream_row[st] < 0) continue;
        const AudioStreamInfo &info = b.streams[st];
        if (!info.seg_count) continue;
        const AudioSeg &last = b.audio_segs[info.first_seg + info.seg_count - 1];
        i64 st_end = last.open ? now : last.end_us;
        if (st_end < c.start || info.start_us > view_end) continue;
        const TlPeaks *pk = tl_peaks(b, info, last.open);
        if (!pk->count) continue;
        i32 k = row_lane[tl.stream_row[st]];
        for (i32 i = 0; i < columns; i++) {
            f32 x = c.x0 + i * wstep;
            i64 a = tl_t(c, x), z = tl_t(c, x + wstep);
            if (z < info.start_us || a > st_end) continue;
            f32 v = tl_peak_in(*pk, info.start_us, a, z);
            if (v > lane_peak[k][i]) lane_peak[k][i] = v;
        }
    }
    // Each lane's bars: every stream's segments in it (in Simple, every app of its kind), merged so that where
    // streams overlap the bar is drawn once. Each bar is also marked on the columns it covers, so the waveform stays
    // inside the bars.
    struct LaneSpan { i64 a, z; i32 k; };
    static LaneSpan *spans;
    static i32 spans_cap;
    i32 span_count = 0;
    for (i32 st = 0; st < b.stream_count && st < COUNT(tl.stream_row); st++) {
        if (tl.stream_row[st] < 0) continue;
        i32 k = row_lane[tl.stream_row[st]];
        const AudioStreamInfo &info = b.streams[st];
        for (i32 j = 0; j < info.seg_count; j++) {
            const AudioSeg &seg = b.audio_segs[info.first_seg + j];
            // An open segment is the one being written, or one left behind when clipback was killed: segments are
            // 10 s, so it can't reach past that (else a dead stream would seem to run up to now).
            i64 e = seg.open ? ImMin(now, seg.start_us + 12 * TL_SEC) : seg.end_us;
            if (e < c.start || seg.start_us > view_end) continue;
            if (span_count == spans_cap) {
                i32 cap = spans_cap ? spans_cap * 2 : 4096;
                LaneSpan *grown = (LaneSpan *)realloc(spans, cap * sizeof *spans);
                if (!grown) break;
                spans = grown;
                spans_cap = cap;
            }
            spans[span_count++] = {seg.start_us, e, k};
        }
    }
    qsort(spans, span_count, sizeof *spans, [](const void *x, const void *y) {
        const LaneSpan &p = *(const LaneSpan *)x, &q = *(const LaneSpan *)y;
        return p.k != q.k ? p.k - q.k : p.a < q.a ? -1 : p.a > q.a ? 1 : 0;
    });
    for (i32 i = 0; i < span_count;) {
        i32 k = spans[i].k;
        f32 y = y_lanes + k * line_h + bar_in, h = line_h - 2 * bar_in;
        ImU32 col = col32(lane_col(k), 0.16f);
        auto cover = [&](i64 ta, i64 tz) {
            f32 xa = ImMax(tl_x(c, ta), c.x0), xb = ImMin(tl_x(c, tz), c.x1);
            if (xb <= xa) return;
            for (i32 n = ImMax((i32)((xa - c.x0) / wstep), 0); n < columns; n++) {
                f32 x = c.x0 + n * wstep;
                if (x >= xb) break;
                f32 *cv = lane_cov[k][n], ca = ImMax(x, xa), cz = ImMin(x + wstep, xb);
                if (cv[1] <= cv[0]) { cv[0] = ca; cv[1] = cz; }
                else { cv[0] = ImMin(cv[0], ca); cv[1] = ImMax(cv[1], cz); }
            }
        };
        TlRun run = {};
        for (; i < span_count && spans[i].k == k; i++) {
            TlRun was = run;
            tl_run_add(c, &run, spans[i].a, spans[i].z, y, h, col);
            if (was.a && run.a != was.a) cover(was.a, was.z); // the run before was drawn
        }
        if (run.a) {
            tl_span(c, run.a, run.z, y, h, col);
            cover(run.a, run.z);
        }
    }
    // The waveform stands on the lane's floor
    for (i32 k = 0; k < lanes; k++) {
        f32 floor_y = y_lanes + (k + 1) * line_h - lane_in - 1 * s, most = line_h - 2 * lane_in - 2 * s;
        if (floor_y < y_snd || floor_y - most > y_bottom) continue;
        ImU32 col = col32(lane_col(k), 0.9f);
        for (i32 i = 0; i < columns; i++) {
            // Columns touch, and sound too quiet to rise a pixel draws nothing
            f32 hh = lane_peak[k][i] * most;
            const f32 *cv = lane_cov[k][i];
            if (hh < 1 * s || cv[1] <= cv[0]) continue;
            dl->AddRectFilled(ImVec2(cv[0], floor_y - hh), ImVec2(cv[1], floor_y), col);
        }
    }

    dl->PopClipRect();

    // The future, dimmed, and the now edge
    f32 x_now = tl_x(c, now);
    if (x_now < c.x1) {
        dl->AddRectFilled(ImVec2(x_now, y_tracks), ImVec2(c.x1 + 6 * s, y_bottom), col32(COL_PANEL, 0.7f));
        dl->AddLine(ImVec2(x_now, y_tracks), ImVec2(x_now, y_bottom), col32(COL_FAINT), 1.5f * s);
    }

    // The selection: an accent tint, a band on the ruler, and a handle on each edge
    if (has_sel) {
        bool idle = bar_hovered && tl.drag == TL_DRAG_NONE;
        wa_draw_inout(dl, tl_x(c, tl.sel_in), tl_x(c, tl.sel_out), y_ruler, y_tracks, y_bottom,
                      tl.drag == TL_DRAG_IN || (idle && near_in), tl.drag == TL_DRAG_OUT || (idle && near_out));
    }

    // Markers: a diamond on the ruler in the marker's colour (the playhead's is a white triangle), bigger when picked
    // or under the mouse. The line down through the tracks shows only while the mouse is on it.
    for (i32 i = 0; i < mark_count; i++) {
        f32 x = floorf(tl_x(c, marks[i].t_us)) + 0.5f;
        u32 rgb = MARKER_COLOURS[marks[i].colour < COUNT(MARKER_COLOURS) ? marks[i].colour : 0].rgb;
        bool picked = marks[i].t_us == tl.marker_pick, hot = i == mark_hit;
        if (hot) dl->AddLine(ImVec2(x, y_tracks), ImVec2(x, y_bottom), col32(rgb, 0.8f), 1 * s);
        f32 r = (hot || picked ? 5.5f : 4.5f) * s, cy = y_ruler + ruler_h - r - 1 * s;
        ImVec2 d[4] = {ImVec2(x, cy - r), ImVec2(x + r, cy), ImVec2(x, cy + r), ImVec2(x - r, cy)};
        dl->AddConvexPolyFilled(d, 4, col32(rgb));
        if (picked) dl->AddPolyline(d, 4, col32(COL_TEXT), ImDrawFlags_Closed, 1.5f * s);
    }

    // The playhead (where the preview comes from)
    i64 play = tl_playhead();
    if (play) wa_draw_playhead(dl, tl_x(c, play), y_tracks, y_bottom);
    // The time under the mouse, or at the edge being dragged, on the ruler
    {
        i64 t = pill_t;
        if (t) {
            f32 x = tl_x(c, t);
            if (tl.hovering) dl->AddLine(ImVec2(x, y_tracks), ImVec2(x, y_bottom), col32(COL_TEXT, 0.3f));
            char when[200];
            tl_clock(t, true, when, sizeof when);
            if (mark_hit >= 0 && bar_hovered) {
                tl_clock(marks[mark_hit].t_us, true, when, sizeof when);
                size_t l = strlen(when);
                snprintf(when + l, sizeof when - l, "   %s", marks[mark_hit].note[0] ? marks[mark_hit].note : "Marker");
            } else if (saved_hit >= 0 && bar_hovered) {
                size_t l = strlen(when);
                snprintf(when + l, sizeof when - l, "   %s", saved[saved_hit].name);
            }
            ImGui::PushFont(g_font_small);
            wa_pill(dl, x, y_ruler + 2 * s, c.x0, c.x1, when, col32(COL_HOVER), col32(COL_TEXT));
            ImGui::PopFont();
        }
    }
    // The picked marker's note, under the ruler
    for (i32 i = 0; i < mark_count; i++) {
        if (marks[i].t_us != tl.marker_pick || (pill_t && mark_hit == i)) continue;
        ImGui::PushFont(g_font_small);
        wa_pill(dl, tl_x(c, marks[i].t_us), y_tracks + 2 * s, c.x0, c.x1, marks[i].note[0] ? marks[i].note : "Marker",
                col32(MARKER_COLOURS[marks[i].colour < COUNT(MARKER_COLOURS) ? marks[i].colour : 0].rgb, 0.9f), col32(0x000000));
        ImGui::PopFont();
    }
    // A save's right-click menu
    lib_popup_style_push(6);
    if (ImGui::BeginPopup("##savedmenu")) {
        if (lib_menu_item(ICON_PLAY, "Open")) tl_saved_open(saved_open.path);
        if (lib_menu_item(ICON_EDIT, "Edit")) editor_open_file(saved_open.path);
        if (lib_menu_item(ICON_FOLDER, "Show in folder")) {
            Path w = {};
            utf8_to_wide(saved_open.path, w.s, COUNT(w.s));
            show_in_folder(w.s);
        }
        ImGui::EndPopup();
    }
    lib_popup_style_pop();

    // The selection's right-click menu
    lib_popup_style_push(6);
    if (ImGui::BeginPopup("##selmenu")) {
        if (lib_menu_item(ICON_CLOSE, "Clear in and out")) {
            tl.sel_in = tl.sel_out = 0;
            log_line("timeline: selection cleared");
        }
        ImGui::EndPopup();
    }
    lib_popup_style_pop();

    // A marker's whole note, with its time and a copy button on the top line. The note wraps at a reading width; a
    // short one gets a window just wide enough, a long one a taller window.
    {
        const char *note = mark_open.note[0] ? mark_open.note : "No note";
        f32 pad = 12 * s, wrap = 300 * s, bs = fh;
        char when[64];
        tl_clock(mark_open.t_us, true, when, sizeof when);
        f32 head = 16 * s + ImGui::CalcTextSize(when).x + 12 * s + (mark_open.note[0] ? bs : 0);
        f32 body = ImGui::CalcTextSize(note).x;
        f32 w = ImClamp(ImMax(head, body), 160 * s, wrap);
        ImGui::SetNextWindowSize(ImVec2(w + 2 * pad, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8 * s, 6 * s));
        bool open = ImGui::BeginPopup("##marknote", ImGuiWindowFlags_AlwaysAutoResize);
        ImGui::PopStyleVar(2);
        if (open) {
            u32 mrgb = MARKER_COLOURS[mark_open.colour < COUNT(MARKER_COLOURS) ? mark_open.colour : 0].rgb;
            ImVec2 p = ImGui::GetCursorScreenPos();
            f32 lh = ImGui::GetTextLineHeight(), ty = floorf(p.y + (bs - lh) / 2);
            ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + 5 * s, ty + lh / 2), 5 * s, col32(mrgb));
            ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + 16 * s, ty), col32(COL_DIM), when);
            if (mark_open.note[0]) {
                ImGui::SetCursorScreenPos(ImVec2(p.x + w - bs, p.y));
                kit_next_tip("Copy the note");
                if (kit_icon_button(ICON_COPY, "##copynote", KIT_GHOST, true, false, bs)) {
                    ImGui::SetClipboardText(mark_open.note);
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + bs + 6 * s));
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w);
            ImGui::TextUnformatted(note);
            ImGui::PopTextWrapPos();
            ImGui::EndPopup();
        }
    }
    if (!tl.loaded || !b.seg_count) {
        const char *msg = !tl.loaded ? "Reading the buffer..." : "Nothing recorded yet";
        ImVec2 sz = ImGui::CalcTextSize(msg);
        dl->AddText(ImVec2(floorf((c.x0 + c.x1 - sz.x) / 2), floorf(y_video + (video_h - th) / 2)), col32(COL_DIM), msg);
    }
    dl->PopClipRect();

    // --- The preview, top left: the video playing, or the still frame at the playhead
    f32 pw = top_h * (f32)TL_FRAME_W / (f32)TL_FRAME_H;
    if (pw > avail.x) pw = avail.x;
    f32 ph = floorf(pw * (f32)TL_FRAME_H / (f32)TL_FRAME_W);
    ImVec2 p0(floorf(top.x + (avail.x - pw) / 2), top.y + floorf((top_h - ph) / 2));
    ImVec2 p1(p0.x + pw, p0.y + ph);
    f32 pr = 10 * s;
    // The wheel zooms the preview in toward the cursor (up to 8x) and back out; a middle drag moves around in it.
    // Only the view: nothing saved changes.
    static f32 pv_z = 1, pv_u = 0, pv_v = 0; // the zoom, and the picture's top left as shown (0..1 - 1/zoom)
    ImVec2 uv0, uv1;
    {
        ImGui::SetCursorScreenPos(p0);
        ImGui::InvisibleButton("##preview", ImVec2(pw > 1 ? pw : 1, ph > 1 ? ph : 1), ImGuiButtonFlags_MouseButtonMiddle);
        ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
        f32 w = 1 / pv_z;
        auto clamp_view = [&]() {
            w = 1 / pv_z;
            pv_u = ImClamp(pv_u, 0.f, 1 - w);
            pv_v = ImClamp(pv_v, 0.f, 1 - w);
        };
        if (ImGui::IsItemHovered() && io.MouseWheel != 0) {
            f32 mu = ImClamp((io.MousePos.x - p0.x) / pw, 0.f, 1.f), mv = ImClamp((io.MousePos.y - p0.y) / ph, 0.f, 1.f);
            f32 at_u = pv_u + mu * w, at_v = pv_v + mv * w; // the point under the cursor stays under it
            pv_z = ImClamp(pv_z * powf(1.25f, io.MouseWheel), 1.f, 8.f);
            pv_u = at_u - mu / pv_z;
            pv_v = at_v - mv / pv_z;
            clamp_view();
        }
        if (ImGui::IsItemActive() && pv_z > 1) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
            pv_u -= io.MouseDelta.x / pw * w;
            pv_v -= io.MouseDelta.y / ph * w;
            clamp_view();
        }
        uv0 = ImVec2(pv_u, pv_v);
        uv1 = ImVec2(pv_u + w, pv_v + w);
    }
    dl->AddRectFilled(p0, p1, col32(0x000000), pr);
    Player &pl = tl.player;
    bool playing_view = pl.open && pl.frame_ready && !pl.error[0];
    if (playing_view) {
        ImVec2 v0, v1;
        player_fit(pl, p0, ImVec2(pw, ph), &v0, &v1);
        dl->AddImage((ImTextureID)(intptr_t)pl.srv, v0, v1, uv0, uv1);
    } else {
        // The exact frame at the playhead, wherever the mouse is. While Play gets ready the last frame stays.
        bool busy_play = tl.play.job || pl.open;
        i64 want_t = play;
        i64 want_key = b.seg_count && want_t && !busy_play ? tl_want_frame(want_t, true) : 0;
        TimelineFrame *frame = tl_frame_find(want_key);
        if (frame) tl.shown_key = frame->key;
        else if (want_key || busy_play) frame = tl_frame_find(tl.shown_key); // the last one shown, until the new one is ready
        if (frame) dl->AddImageRounded((ImTextureID)(intptr_t)frame->srv, p0, p1, uv0, uv1, col32(0xffffff), pr);
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

    // --- One row under the preview: Play and the playhead's time, zoom and pan, what's going on, then the selection
    // and its saves on the right. Every part has its own place, so nothing moves.
    {
        bool busy = tl.job_edit; // waiting for the save to open the editor
        GameTime games[16];
        i32 game_count = has_sel ? games_in(b, tl.sel_in, tl.sel_out, games, COUNT(games)) : 0;
        bool has_video = has_sel && tl_has_video(b, tl.sel_in, tl.sel_out);
        bool can = has_video && !busy;
        const f32 bgap = 8 * s;
        f32 ty = y_tools, x = top.x, mid = floorf(ty + (fh - ImGui::GetFontSize()) / 2);
        const char *clock_w = g_app.config.clock_24h ? "00:00:00" : "00:00:00 pm";

        bool active = tl.play.job || (pl.open && !pl.paused);
        ImGui::SetCursorScreenPos(ImVec2(x, ty));
        kit_next_tip(active ? "Pause" : "Play", "Space");
        if (kit_icon_button(active ? ICON_PAUSE : ICON_PLAY, "##play", KIT_PRIMARY, b.seg_count > 0)) tl_play_toggle();
        x += fh + 12 * s;
        char when[16] = "--:--:--";
        if (play) tl_clock(play, true, when, sizeof when);
        ImGui::PushFont(g_font_bold);
        dl->AddText(ImVec2(x, floorf(ty + (fh - ImGui::GetFontSize()) / 2)), col32(COL_TEXT), when);
        x += ImGui::CalcTextSize(clock_w).x + 18 * s;
        ImGui::PopFont();

        // Zoom and pan
        i64 centre = tl.view_end_us - tl.view_span_us / 2;
        ImGui::SetCursorScreenPos(ImVec2(x, ty));
        kit_next_tip("Zoom out", "-");
        if (kit_icon_button(ICON_ZOOM_OUT, "##zoom_out", KIT_GHOST, tl.view_span_us < tl_max_span(b, now)))
            tl_zoom(2, centre, now);
        ImGui::SameLine(0, 2 * s);
        kit_next_tip("Zoom in", "+");
        if (kit_icon_button(ICON_ZOOM_IN, "##zoom_in", KIT_GHOST, tl.view_span_us > TL_MIN_SPAN)) tl_zoom(0.5, centre, now);
        ImGui::SameLine(0, 10 * s);
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
        kit_next_tip("Follow the recording", "Home");
        if (kit_icon_button(ICON_NOW, "##now", KIT_GHOST, !tl.follow)) {
            tl.follow = true;
            tl_clamp_view(now);
        }
        x = ImGui::GetItemRectMax().x + 18 * s;

        // The saves, on the right. Share's arrow (or a right-click) picks the link's host.
        f32 w1 = 104 * s, w4 = 112 * s, w2 = 124 * s, w3 = 108 * s;
        f32 xr = top.x + avail.x - (w1 + w4 + w2 + w3 + 3 * bgap);
        ImGui::SetCursorScreenPos(ImVec2(xr, ty));
        if (kit_button(ICON_SAVE "  Save clip", KIT_PRIMARY, ImVec2(w1, 0), can)) {
            tl_save(SAVE_CLIP, false);
            tl_quick_done();
        }
        ImGui::SameLine(0, bgap);
        bool sharing = tl_sharing();
        i32 sb = kit_split_button(sharing ? "Sharing..." : ICON_SHARE "  Share", "##tlshare", KIT_SECONDARY, ImVec2(w4, 0),
                                  can && !sharing, can && !sharing);
        if (sb == 1) tl_share(0);
        else if (sb == 2) ImGui::OpenPopup("tl_link_to");
        lib_popup_style_push(6);
        if (ImGui::BeginPopup("tl_link_to")) {
            char pick[64];
            if (link_to_rows(tl_share_guess(), 0, 300 * s, pick)) tl_share(pick);
            ImGui::EndPopup();
        }
        lib_popup_style_pop();
        ImGui::SameLine(0, bgap);
        if (kit_button(ICON_EDIT "  Save and edit", KIT_SECONDARY, ImVec2(w2, 0), can)) tl_save(SAVE_CLIP, true);
        ImGui::SameLine(0, bgap);
        ImVec2 ses_pos = ImGui::GetCursorScreenPos();
        if (kit_button(ICON_GAME "  Session...", KIT_SECONDARY, ImVec2(w3, 0), can)) ImGui::OpenPopup("##session");

        // The selection: start, end and length, and a button to clear it
        char sel_max[48];
        snprintf(sel_max, sizeof sel_max, "%s - %s   00:00:00", clock_w, clock_w);
        f32 sel_w = ImGui::CalcTextSize(sel_max).x;
        f32 x_clear = xr - 14 * s - fh, xs = x_clear - 6 * s - sel_w;
        ImGui::SetCursorScreenPos(ImVec2(x_clear, ty));
        kit_next_tip("Clear the selection");
        if (kit_icon_button(ICON_CLOSE, "##clear", KIT_GHOST, has_sel)) tl.sel_in = tl.sel_out = 0;
        {
            char sel[96] = "Nothing selected";
            u32 ink = COL_FAINT;
            if (has_sel && !has_video) {
                str_set(sel, "No video in this range");
                ink = COL_WARN;
            } else if (has_sel) {
                char a[16], z[16], len[32];
                tl_clock(tl.sel_in, true, a, sizeof a);
                tl_clock(tl.sel_out, true, z, sizeof z);
                tl_length(tl.sel_out - tl.sel_in, len, sizeof len);
                snprintf(sel, sizeof sel, "%s - %s   %s", a, z, len);
                ink = COL_TEXT;
            }
            f32 sw = ImGui::CalcTextSize(sel).x;
            tl_text_fit(dl, ImVec2(sw < sel_w ? x_clear - 6 * s - sw : xs, mid), sel_w, col32(ink), sel);
        }

        // What's going on, between: Play getting ready, or the last save here. A running save can be cancelled.
        f32 sx = x, sz = xs - 18 * s - fh - 6 * s - sx;
        char status[600] = "";
        u32 ink = COL_DIM;
        const char *icon = 0;
        f32 progress = -1;
        SaveJobView *v = save_view(tl.job);
        u32 building = tl.play.job ? tl.play.job : pl.open && pl.ended ? tl.next.job : 0; // waited for
        if (building) {
            SaveJobView *pv = save_view(building);
            if (pv && pv->state == SAVE_RUNNING) snprintf(status, sizeof status, "Getting it ready  %d%%", (int)(pv->progress * 100));
            else str_set(status, "Waiting for a save to finish");
        } else if (tl.play_error[0]) {
            str_set(status, tl.play_error);
            ink = COL_WARN;
        } else if (pl.error[0]) {
            str_set(status, pl.error);
            ink = COL_WARN;
        } else if (pl.open && !pl.loaded) {
            str_set(status, "Opening...");
        } else if (tl.share_step != TL_SHARE_NONE) {
            str_set(status, tl.share_line);
            if (tl.share_step == TL_SHARE_DONE) icon = ICON_CHECK, ink = COL_OK;
            else if (tl.share_step == TL_SHARE_FAILED) icon = ICON_WARNING, ink = COL_ACCENT;
            else if (tl.share_step == TL_SHARE_SAVING && v && v->state == SAVE_RUNNING) {
                progress = v->progress;
                snprintf(status, sizeof status, "Saving the clip  %d%%", (int)(progress * 100));
            } else if (tl.share_step == TL_SHARE_RUNNING) {
                ShareJobView j = share_job(tl.share);
                if (j.state == SHARE_MAKING || j.state == SHARE_UPLOADING) progress = j.progress;
            }
        } else if (v) {
            const char *kind = v->kind == SAVE_SESSION ? "session" : "clip";
            if (v->state == SAVE_QUEUED) str_set(status, "Waiting for another save to finish");
            else if (v->state == SAVE_RUNNING) {
                progress = v->progress;
                snprintf(status, sizeof status, "Saving the %s  %d%%", kind, (int)(progress * 100));
            } else if (v->state == SAVE_DONE) {
                const char *file = strrchr(v->path, '\\');
                snprintf(status, sizeof status, "Saved %s", file ? file + 1 : v->path);
                icon = ICON_CHECK;
                ink = COL_OK;
            } else if (v->state == SAVE_FAILED) {
                snprintf(status, sizeof status, "Save failed: %s", v->error);
                icon = ICON_WARNING;
                ink = COL_ACCENT;
            } else str_set(status, "Save cancelled");
        }
        if (status[0] && sz > 20 * s) {
            ImGui::PushFont(g_font_small);
            f32 sh = ImGui::GetTextLineHeight(), y = progress >= 0 ? ty + 3 * s : floorf(ty + (fh - sh) / 2), x2 = sx;
            if (icon) {
                dl->AddText(ImVec2(x2, y), col32(ink), icon);
                x2 += 20 * s;
            }
            tl_text_fit(dl, ImVec2(x2, y), sx + sz - x2, col32(icon ? COL_TEXT : ink), status);
            ImGui::PopFont();
            if (progress >= 0) {
                f32 by = ty + fh - 8 * s;
                dl->AddRectFilled(ImVec2(sx, by), ImVec2(sx + sz, by + 4 * s), col32(COL_PANEL2), 2 * s);
                if (progress > 0) dl->AddRectFilled(ImVec2(sx, by), ImVec2(sx + sz * progress, by + 4 * s), col32(COL_ACCENT), 2 * s);
            }
        }
        if (v && v->state < SAVE_DONE && sz > 20 * s) {
            ImGui::SetCursorScreenPos(ImVec2(sx + sz + 6 * s, ty));
            kit_next_tip("Cancel the save");
            if (kit_icon_button(ICON_CLOSE, "##cancel_save")) save_cancel(v->id);
        }

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

    }
    ImGui::SetCursorScreenPos(top);
    ImGui::Dummy(avail);
}
