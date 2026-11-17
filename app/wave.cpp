// Waveforms for the editor's audio lanes (the approach of Compress for Discord's Waveform.cs).
//
// One ffmpeg reads every track of the file at once: each to mono at 8 kHz, padded so a short one doesn't end the
// others, merged one track per channel, as raw 16-bit samples on its stdout. The loudest sample of each 10 ms
// slice is kept, stored as the square root (0..255, the scale it's drawn at), and every level above holds the
// loudest of two below it, so drawing any zoom reads only a value or two per pixel. The lanes fill in from the
// start as it's measured. A finished measure is cached next to the thumbnails (w-<key>.bin), so opening the file
// again shows it at once.
//
// The thread writes while the window reads; `done` (written last, with a barrier) says how far is safe to read,
// and a stale read only means a slice shows a moment late. Both hold a reference, so whichever lets go last frees it.

enum { WAVE_RATE = 100, WAVE_SAMPLE_RATE = 8000, WAVE_PER_SLICE = WAVE_SAMPLE_RATE / WAVE_RATE, WAVE_MAX_LEVELS = 32 };

struct WaveData {
    volatile LONG refs;
    volatile LONG cancel;   // the editor let go: stop measuring
    volatile LONG done;     // slices measured, from the start, on every track
    volatile LONG state;    // 0 measuring, 1 finished, 2 failed
    i32  count;             // tracks measured
    i32  track[MEDIA_MAX_TRACKS]; // which audio stream each one is
    i32  slices, levels;
    i32  size[WAVE_MAX_LEVELS];   // entries in each level
    u8  *lvl[MEDIA_MAX_TRACKS][WAVE_MAX_LEVELS];
    u8  *block;             // every level of every track, one allocation
    u8   loudest[MEDIA_MAX_TRACKS];
    Path src, ffmpeg, cache;
    f64  duration;
};

static void wave_release(WaveData *w)
{
    if (!w || InterlockedDecrement(&w->refs)) return;
    if (w->block) HeapFree(GetProcessHeap(), 0, w->block);
    HeapFree(GetProcessHeap(), 0, w);
}

static u8 wave_encode(i32 sample)
{
    f64 v = ceil(sqrt(sample / 32768.0) * 255);
    return (u8)(v > 255 ? 255 : v);
}

// The levels above the finest, for slices lo..hi of it (a stretch just measured).
static void wave_build(WaveData *w, i32 lo, i32 hi)
{
    for (i32 l = 1; l < w->levels; l++) {
        lo >>= 1;
        hi = (hi + 1) >> 1;
        if (hi > w->size[l]) hi = w->size[l];
        i32 below = w->size[l - 1];
        for (i32 c = 0; c < w->count; c++) {
            u8 *b = w->lvl[c][l - 1], *h = w->lvl[c][l];
            for (i32 i = lo; i < hi; i++) {
                u8 x = b[2 * i], y = 2 * i + 1 < below ? b[2 * i + 1] : 0;
                h[i] = x > y ? x : y;
            }
        }
    }
}

static bool wave_load_cache(WaveData *w)
{
    FILE *f = _wfopen(w->cache.s, L"rb");
    if (!f) return false;
    i32 head[2] = {};
    bool ok = fread(head, sizeof head, 1, f) == 1 && head[0] == w->count && head[1] == w->slices;
    for (i32 c = 0; ok && c < w->count; c++) ok = fread(w->lvl[c][0], 1, w->slices, f) == (size_t)w->slices;
    fclose(f);
    if (!ok) return false;
    wave_build(w, 0, w->slices);
    for (i32 c = 0; c < w->count; c++) {
        u8 m = 0;
        for (i32 i = 0; i < w->size[w->levels - 1]; i++) m = m > w->lvl[c][w->levels - 1][i] ? m : w->lvl[c][w->levels - 1][i];
        w->loudest[c] = m;
    }
    return true;
}

static void wave_save_cache(WaveData *w)
{
    Path tmp = {};
    swprintf(tmp.s, MAX_PATH, L"%s.tmp", w->cache.s);
    FILE *f = _wfopen(tmp.s, L"wb");
    if (!f) return;
    i32 head[2] = {w->count, w->slices};
    bool ok = fwrite(head, sizeof head, 1, f) == 1;
    for (i32 c = 0; ok && c < w->count; c++) ok = fwrite(w->lvl[c][0], 1, w->slices, f) == (size_t)w->slices;
    fclose(f);
    if (ok) MoveFileExW(tmp.s, w->cache.s, MOVEFILE_REPLACE_EXISTING);
    else DeleteFileW(tmp.s);
}

static bool wave_measure(WaveData *w)
{
    i32 n = w->count;
    // A long filter for many tracks: built in UTF-8, then put on the command line.
    char *graph = (char *)HeapAlloc(GetProcessHeap(), 0, 16384);
    i32 gl = 0;
    for (i32 c = 0; c < n; c++)
        gl += snprintf(graph + gl, 16384 - gl,
                       "%s[0:a:%d]aformat=channel_layouts=mono,aresample=%d,aformat=sample_fmts=s16:channel_layouts=mono:"
                       "sample_rates=%d%s[w%d]",
                       c ? ";" : "", w->track[c], (int)WAVE_SAMPLE_RATE, (int)WAVE_SAMPLE_RATE, n > 1 ? ",apad" : "", c);
    if (n > 1) {
        gl += snprintf(graph + gl, 16384 - gl, ";");
        for (i32 c = 0; c < n; c++) gl += snprintf(graph + gl, 16384 - gl, "[w%d]", c);
        gl += snprintf(graph + gl, 16384 - gl, "amerge=inputs=%d[w]", n);
    }
    i32 cap = 20000;
    wchar_t *args = (wchar_t *)HeapAlloc(GetProcessHeap(), 0, cap * sizeof(wchar_t));
    args[0] = 0;
    static const char *const head[] = {"-hide_banner", "-loglevel", "error", "-nostdin"};
    arg_list(args, cap, head, COUNT(head));
    cmd_arg8(args, cap, "-i");
    cmd_arg(args, cap, w->src.s);
    cmd_arg8(args, cap, "-filter_complex");
    cmd_arg8(args, cap, graph);
    cmd_arg8(args, cap, "-map");
    cmd_arg8(args, cap, n > 1 ? "[w]" : "[w0]");
    char t[32];
    snprintf(t, sizeof t, "%.3f", w->duration); // the padded tracks would go on for ever
    cmd_arg8(args, cap, "-t");
    cmd_arg8(args, cap, t);
    static const char *const tail[] = {"-f", "s16le", "-"};
    arg_list(args, cap, tail, COUNT(tail));
    HeapFree(GetProcessHeap(), 0, graph);

    Proc p = {};
    bool started = proc_start(w->ffmpeg.s, args, PROC_STDOUT, &p);
    HeapFree(GetProcessHeap(), 0, args);
    if (!started) return false;

    i32 frame = 2 * n; // one sample of every track
    static thread_local u8 buf[1 << 16];
    i32 have = 0, in_slice = 0, slice = 0, built = 0;
    i32 peak[MEDIA_MAX_TRACKS] = {};
    u64 got = 0;
    for (;;) {
        if (w->cancel) break;
        DWORD r = 0;
        if (!ReadFile(p.out, buf + have, (DWORD)(sizeof buf - have), &r, 0) || !r) break;
        got += r;
        have += (i32)r;
        i32 i = 0;
        for (; i + frame <= have && slice < w->slices; i += frame) {
            const i16 *s = (const i16 *)(buf + i);
            for (i32 c = 0; c < n; c++) {
                i32 v = s[c] < 0 ? -(i32)s[c] : s[c];
                if (v > peak[c]) peak[c] = v;
            }
            if (++in_slice < WAVE_PER_SLICE) continue;
            for (i32 c = 0; c < n; c++) {
                u8 e = wave_encode(peak[c]);
                w->lvl[c][0][slice] = e;
                if (e > w->loudest[c]) w->loudest[c] = e;
                peak[c] = 0;
            }
            in_slice = 0;
            slice++;
            // A second's worth: build the levels above it and let the window see it.
            if (slice - built >= WAVE_RATE) {
                wave_build(w, built, slice);
                built = slice;
                MemoryBarrier();
                InterlockedExchange(&w->done, slice);
            }
        }
        memmove(buf, buf + i, have - i);
        have -= i;
    }
    if (w->cancel) proc_kill(&p);
    i32 code = proc_wait(&p, 10000);
    proc_close(&p);
    if (w->cancel) return false;
    if (in_slice && slice < w->slices) {
        for (i32 c = 0; c < n; c++) w->lvl[c][0][slice] = wave_encode(peak[c]);
        slice++;
    }
    // What the sound didn't reach is silence.
    wave_build(w, built, w->slices);
    MemoryBarrier();
    InterlockedExchange(&w->done, w->slices);
    if (!got) {
        log_line("waveform: ffmpeg read no sound (exit code %d)", code);
        return false;
    }
    return true;
}

static DWORD WINAPI wave_thread(void *arg)
{
    WaveData *w = (WaveData *)arg;
    u64 started = GetTickCount64();
    bool ok;
    if (wave_load_cache(w)) {
        InterlockedExchange(&w->done, w->slices);
        ok = true;
    } else {
        ok = wave_measure(w);
        if (ok) {
            wave_save_cache(w);
            log_line("waveform: %d tracks, %.0f s measured in %.1f s", w->count, w->duration,
                     (GetTickCount64() - started) / 1000.0);
        }
    }
    InterlockedExchange(&w->state, ok ? 1 : 2);
    ui_wake();
    wave_release(w);
    return 0;
}

// Starts measuring the given audio streams of a file. Returns the waveform to read (the caller holds a reference
// and lets go with wave_release), or 0 without ffmpeg or sound.
static WaveData *wave_open(const Path &src, u64 key, f64 duration, const i32 *tracks, i32 n)
{
    Path ffmpeg = {};
    if (n <= 0 || duration <= 0 || !ffmpeg_find(g_app.config.ffmpeg, &ffmpeg)) return 0;
    if (n > MEDIA_MAX_TRACKS) n = MEDIA_MAX_TRACKS;
    WaveData *w = (WaveData *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(WaveData));
    if (!w) return 0;
    w->count = n;
    memcpy(w->track, tracks, n * sizeof tracks[0]);
    w->duration = duration;
    w->slices = (i32)ceil(duration * (f64)WAVE_RATE);
    if (w->slices < 1) w->slices = 1;
    i64 per_track = 0;
    for (i32 s = w->slices;; s = (s + 1) / 2) {
        w->size[w->levels++] = s;
        per_track += s;
        if (s == 1 || w->levels == WAVE_MAX_LEVELS) break;
    }
    w->block = (u8 *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (size_t)(per_track * n));
    if (!w->block) {
        HeapFree(GetProcessHeap(), 0, w);
        return 0;
    }
    u8 *at = w->block;
    for (i32 c = 0; c < n; c++)
        for (i32 l = 0; l < w->levels; l++) {
            w->lvl[c][l] = at;
            at += w->size[l];
        }
    w->src = src;
    w->ffmpeg = ffmpeg;
    // Keyed by the file and which tracks, so a different choice of tracks isn't read from the wrong cache.
    u64 h = key;
    for (i32 c = 0; c < n; c++) h = (h ^ (u64)(tracks[c] + 1)) * 1099511628211ull;
    wchar_t name[64];
    swprintf(name, COUNT(name), L"w-%016llx.bin", h);
    Path dir = path_join(g_app.paths.local_dir, L"thumbs");
    SHCreateDirectoryExW(0, dir.s, 0);
    w->cache = path_join(dir, name);
    w->refs = 2; // the caller's and the thread's
    HANDLE th = thread_start(wave_thread, w);
    if (!th) {
        w->refs = 1;
        wave_release(w);
        return 0;
    }
    CloseHandle(th);
    return w;
}

// Stops measuring (if it still is) and lets go of it.
static void wave_close(WaveData **w)
{
    if (!*w) return;
    InterlockedExchange(&(*w)->cancel, 1);
    wave_release(*w);
    *w = 0;
}

// The loudest sample (0..1, as a linear amplitude) of track c between t0 and t1 seconds, read from the level where
// that's a value or two. False if none of it has been measured yet.
static bool wave_peak(const WaveData *w, i32 c, f64 t0, f64 t1, f32 *out)
{
    *out = 0;
    if (!w || c < 0 || c >= w->count) return false;
    f64 s0 = t0 * (f64)WAVE_RATE, s1 = t1 * (f64)WAVE_RATE;
    if (s1 < s0 + 1e-6) s1 = s0 + 1e-6;
    i32 done = w->done;
    if (s0 >= done) return false;
    if (s1 > done) s1 = done;
    i32 level = 0;
    for (f64 span = s1 - s0; span >= 2 && level + 1 < w->levels; span /= 2) level++;
    f64 scale = (f64)(1 << level);
    i32 i0 = (i32)(s0 / scale), i1 = (i32)ceil(s1 / scale);
    if (i0 < 0) i0 = 0;
    if (i1 <= i0) i1 = i0 + 1;
    if (i1 > w->size[level]) i1 = w->size[level];
    const u8 *a = w->lvl[c][level];
    u8 m = 0;
    for (i32 i = i0; i < i1; i++) m = a[i] > m ? a[i] : m;
    f32 v = m / 255.f;
    *out = v * v;
    return true;
}
