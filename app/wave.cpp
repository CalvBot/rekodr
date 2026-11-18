// Waveforms for the editor's audio lanes (the approach of Compress for Discord's Waveform.cs).
//
// The finest level holds the loudest sample of each millisecond, so zoomed all the way in there's still detail;
// each level above holds the loudest of two below it, so drawing any zoom reads only a value or two per pixel.
// Values are one byte, the square root of the level, which is the scale it's drawn at.
//
// It's measured in 10-second chunks: the part on screen first, then outwards from it, by a few ffmpegs at once,
// each reading a run of up to 30 chunks of every track (each to mono at 8 kHz, padded so a short one doesn't end
// the others, merged one track per channel, raw 16-bit samples on its stdout). So a long session's waveform pops in
// where you're looking rather than from the start. A finished measure is cached next to the thumbnails
// (w2-<key>.bin, Options › General clears them), so opening the file again shows it at once.
//
// The workers write while the window reads; a chunk is marked known (with a barrier) once its levels are built, and
// a stale read only means it shows a moment late. The workers and the window each hold a reference, and whichever
// lets go last frees it.

enum {
    WAVE_RATE = 1000, // finest slices a second
    WAVE_SAMPLE_RATE = 8000,
    WAVE_PER_SLICE = WAVE_SAMPLE_RATE / WAVE_RATE,
    WAVE_CHUNK_S = 10,
    WAVE_CHUNK = WAVE_CHUNK_S * WAVE_RATE, // slices in a chunk
    WAVE_RUN_CHUNKS = 30,                  // the most one ffmpeg measures: fewer means following the view sooner
    WAVE_MAX_WORKERS = 4,
    WAVE_MAX_LEVELS = 32,
};
enum : u8 { WAVE_UNKNOWN, WAVE_CLAIMED, WAVE_KNOWN };

struct WaveData {
    volatile LONG refs;
    volatile LONG cancel;  // the editor let go: stop measuring
    volatile LONG state;   // 0 measuring, 1 finished, 2 failed
    volatile LONG known_count, failed_runs, workers;
    volatile f64  view_from, view_to; // what the editor shows, so that's measured first
    SRWLOCK       lock;    // claiming chunks, and building the levels above them
    i32  count;            // tracks measured
    i32  track[MEDIA_MAX_TRACKS]; // which audio stream each one is
    i32  slices, levels, chunks;
    i32  size[WAVE_MAX_LEVELS];   // entries in each level
    u8  *lvl[MEDIA_MAX_TRACKS][WAVE_MAX_LEVELS];
    u8  *block;            // every level of every track, one allocation
    volatile u8 *known;    // each chunk's WAVE_UNKNOWN / CLAIMED / KNOWN
    u8   loudest[MEDIA_MAX_TRACKS];
    Path src, ffmpeg, cache;
    f64  duration;
    u64  started;
};

static void wave_release(WaveData *w)
{
    if (!w || InterlockedDecrement(&w->refs)) return;
    if (w->block) HeapFree(GetProcessHeap(), 0, w->block);
    if (w->known) HeapFree(GetProcessHeap(), 0, (void *)w->known);
    HeapFree(GetProcessHeap(), 0, w);
}

static u8 wave_encode(i32 sample)
{
    f64 v = ceil(sqrt(sample / 32768.0) * 255);
    return (u8)(v > 255 ? 255 : v);
}

// The levels above the finest, for slices lo..hi of it (a stretch just measured). Under the lock: two workers
// finishing neighbouring chunks share the entries above their edge.
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

// Chunk k is measured (or given up on): build above it, then let the window see it.
static void wave_chunk_done(WaveData *w, i32 k)
{
    i32 lo = k * WAVE_CHUNK, hi = lo + WAVE_CHUNK < w->slices ? lo + WAVE_CHUNK : w->slices;
    AcquireSRWLockExclusive(&w->lock);
    wave_build(w, lo, hi);
    for (i32 c = 0; c < w->count; c++)
        for (i32 i = lo; i < hi; i++)
            if (w->lvl[c][0][i] > w->loudest[c]) w->loudest[c] = w->lvl[c][0][i];
    MemoryBarrier();
    w->known[k] = WAVE_KNOWN;
    ReleaseSRWLockExclusive(&w->lock);
    InterlockedIncrement(&w->known_count);
    ui_wake();
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
    // Opened: it counts as used, for Options' "keep for".
    HANDLE h = CreateFileW(w->cache.s, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0,
                           OPEN_EXISTING, 0, 0);
    if (h != INVALID_HANDLE_VALUE) {
        FILETIME now;
        GetSystemTimeAsFileTime(&now);
        SetFileTime(h, 0, 0, &now);
        CloseHandle(h);
    }
    for (i32 k = 0; k < w->chunks; k++) wave_chunk_done(w, k);
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

// Picks the next stretch to measure and claims it: the first chunk not yet taken from the left of the view, else
// the nearest one outside it, then the ones after it up to a run. False when every chunk is taken.
static bool wave_claim(WaveData *w, i32 *first, i32 *end)
{
    f64 vf = w->view_from, vt = w->view_to;
    i32 a = (i32)(vf / (f64)WAVE_CHUNK_S), b = (i32)(vt / (f64)WAVE_CHUNK_S);
    if (a < 0) a = 0;
    if (b >= w->chunks) b = w->chunks - 1;
    if (b < a) b = a;
    AcquireSRWLockExclusive(&w->lock);
    i32 k = -1;
    for (i32 i = a; i <= b && i < w->chunks && k < 0; i++)
        if (w->known[i] == WAVE_UNKNOWN) k = i;
    for (i32 d = 1; k < 0 && (a - d >= 0 || b + d < w->chunks); d++) {
        if (b + d < w->chunks && w->known[b + d] == WAVE_UNKNOWN) k = b + d;
        else if (a - d >= 0 && w->known[a - d] == WAVE_UNKNOWN) k = a - d;
    }
    if (k >= 0) {
        i32 e = k;
        while (e < w->chunks && e - k < WAVE_RUN_CHUNKS && w->known[e] == WAVE_UNKNOWN) w->known[e++] = WAVE_CLAIMED;
        *first = k;
        *end = e;
    }
    ReleaseSRWLockExclusive(&w->lock);
    return k >= 0;
}

// Measures chunks first..end-1 with one ffmpeg. Returns false if it read no sound.
static bool wave_measure(WaveData *w, i32 first, i32 end)
{
    i32 n = w->count;
    i32 s0 = first * WAVE_CHUNK, s1 = end * WAVE_CHUNK < w->slices ? end * WAVE_CHUNK : w->slices;
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
    char t[32];
    if (s0 > 0) {
        snprintf(t, sizeof t, "%.3f", s0 / (f64)WAVE_RATE);
        cmd_arg8(args, cap, "-ss");
        cmd_arg8(args, cap, t);
    }
    cmd_arg8(args, cap, "-vn"); // the picture isn't decoded
    cmd_arg8(args, cap, "-i");
    cmd_arg(args, cap, w->src.s);
    cmd_arg8(args, cap, "-filter_complex");
    cmd_arg8(args, cap, graph);
    cmd_arg8(args, cap, "-map");
    cmd_arg8(args, cap, n > 1 ? "[w]" : "[w0]");
    snprintf(t, sizeof t, "%.3f", (s1 - s0) / (f64)WAVE_RATE); // the padded tracks would go on for ever
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
    u8 *buf = (u8 *)HeapAlloc(GetProcessHeap(), 0, 1 << 16);
    i32 have = 0, in_slice = 0, slice = s0, chunk = first;
    i32 peak[MEDIA_MAX_TRACKS] = {};
    u64 got = 0;
    for (;;) {
        if (w->cancel) break;
        DWORD r = 0;
        if (!ReadFile(p.out, buf + have, (DWORD)((1 << 16) - have), &r, 0) || !r) break;
        got += r;
        have += (i32)r;
        i32 i = 0;
        for (; i + frame <= have && slice < s1; i += frame) {
            const i16 *s = (const i16 *)(buf + i);
            for (i32 c = 0; c < n; c++) {
                i32 v = s[c] < 0 ? -(i32)s[c] : s[c];
                if (v > peak[c]) peak[c] = v;
            }
            if (++in_slice < WAVE_PER_SLICE) continue;
            for (i32 c = 0; c < n; c++) {
                w->lvl[c][0][slice] = wave_encode(peak[c]);
                peak[c] = 0;
            }
            in_slice = 0;
            slice++;
            if (slice == (chunk + 1) * WAVE_CHUNK && chunk < end) wave_chunk_done(w, chunk++);
        }
        memmove(buf, buf + i, have - i);
        have -= i;
    }
    HeapFree(GetProcessHeap(), 0, buf);
    if (w->cancel) proc_kill(&p);
    i32 code = proc_wait(&p, 10000);
    proc_close(&p);
    if (w->cancel) return false;
    if (in_slice && slice < s1) {
        for (i32 c = 0; c < n; c++) w->lvl[c][0][slice] = wave_encode(peak[c]);
    }
    // What the sound didn't reach is silence.
    while (chunk < end) wave_chunk_done(w, chunk++);
    if (!got) {
        log_line("waveform: ffmpeg read no sound from %.0f s (exit code %d)", s0 / (f64)WAVE_RATE, code);
        return false;
    }
    return true;
}

static DWORD WINAPI wave_worker(void *arg)
{
    WaveData *w = (WaveData *)arg;
    i32 first, end;
    while (!w->cancel && wave_claim(w, &first, &end))
        if (!wave_measure(w, first, end)) InterlockedIncrement(&w->failed_runs);
    // The last one out says how it went, and caches it when it all read.
    if (!InterlockedDecrement(&w->workers) && !w->cancel) {
        bool ok = w->failed_runs == 0 && w->known_count == w->chunks;
        if (ok) {
            wave_save_cache(w);
            log_line("waveform: %d tracks, %.0f s measured in %.1f s", w->count, w->duration,
                     (GetTickCount64() - w->started) / 1000.0);
        }
        InterlockedExchange(&w->state, ok ? 1 : 2);
        ui_wake();
    }
    wave_release(w);
    return 0;
}

// Reads the cache, or starts the workers measuring.
static DWORD WINAPI wave_thread(void *arg)
{
    WaveData *w = (WaveData *)arg;
    if (wave_load_cache(w)) {
        InterlockedExchange(&w->state, 1);
        ui_wake();
        wave_release(w);
        return 0;
    }
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    i32 n = (i32)si.dwNumberOfProcessors / 2;
    n = n < 1 ? 1 : n > WAVE_MAX_WORKERS ? WAVE_MAX_WORKERS : n;
    if (n > w->chunks) n = w->chunks;
    // This thread's reference goes to the first worker; each other one takes its own.
    InterlockedExchange(&w->workers, n);
    InterlockedAdd(&w->refs, n - 1);
    for (i32 i = 1; i < n; i++) {
        HANDLE th = thread_start(wave_worker, w);
        if (th) {
            CloseHandle(th);
        } else { // fewer workers, that's all: this thread is one, so the count can't reach 0 here
            InterlockedDecrement(&w->workers);
            wave_release(w);
        }
    }
    return wave_worker(w);
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
    InitializeSRWLock(&w->lock);
    w->count = n;
    memcpy(w->track, tracks, n * sizeof tracks[0]);
    w->duration = duration;
    w->started = GetTickCount64();
    w->slices = (i32)ceil(duration * (f64)WAVE_RATE);
    if (w->slices < 1) w->slices = 1;
    w->chunks = (w->slices + WAVE_CHUNK - 1) / WAVE_CHUNK;
    w->view_to = duration;
    i64 per_track = 0;
    for (i32 s = w->slices;; s = (s + 1) / 2) {
        w->size[w->levels++] = s;
        per_track += s;
        if (s == 1 || w->levels == WAVE_MAX_LEVELS) break;
    }
    w->block = (u8 *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (size_t)(per_track * n));
    w->known = (volatile u8 *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (size_t)w->chunks);
    if (!w->block || !w->known) {
        w->refs = 1;
        wave_release(w);
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
    swprintf(name, COUNT(name), L"w2-%016llx.bin", h);
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

// What the editor shows, so it's measured first.
static void wave_view(WaveData *w, f64 from, f64 to)
{
    if (!w) return;
    w->view_from = from;
    w->view_to = to;
}

// The loudest sample (0..1, as a linear amplitude) of track c between t0 and t1 seconds, read from the level where
// that's a value or two. False if none of it has been measured yet.
static bool wave_peak(const WaveData *w, i32 c, f64 t0, f64 t1, f32 *out)
{
    *out = 0;
    if (!w || c < 0 || c >= w->count) return false;
    f64 s0 = t0 * (f64)WAVE_RATE, s1 = t1 * (f64)WAVE_RATE;
    if (s1 < s0 + 1e-6) s1 = s0 + 1e-6;
    if (s0 < 0) s0 = 0;
    if (s1 > w->slices) s1 = w->slices;
    if (s0 >= s1) return false;
    i32 k0 = (i32)(s0 / (f64)WAVE_CHUNK), k1 = (i32)((s1 - 1e-6) / (f64)WAVE_CHUNK);
    bool any = false;
    for (i32 k = k0; k <= k1 && k < w->chunks && !any; k++) any = w->known[k] == WAVE_KNOWN;
    if (!any) return false;
    MemoryBarrier();
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

// The audio previews cached on disk (old 10 ms ones too): how many, and their size.
static void wave_cache_stats(i32 *files, u64 *bytes)
{
    *files = 0;
    *bytes = 0;
    Path pat = path_join(path_join(g_app.paths.local_dir, L"thumbs"), L"w*.bin");
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW(pat.s, &fd);
    if (f == INVALID_HANDLE_VALUE) return;
    do {
        (*files)++;
        *bytes += ((u64)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
    } while (FindNextFileW(f, &fd));
    FindClose(f);
}

// Deletes the cached audio previews not used in `days` (all of them when days <= 0). Returns how many went.
static i32 wave_cache_clear(f64 days)
{
    Path dir = path_join(g_app.paths.local_dir, L"thumbs");
    Path pat = path_join(dir, L"w*.bin");
    FILETIME now_ft;
    GetSystemTimeAsFileTime(&now_ft);
    u64 now = ((u64)now_ft.dwHighDateTime << 32) | now_ft.dwLowDateTime;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW(pat.s, &fd);
    if (f == INVALID_HANDLE_VALUE) return 0;
    i32 n = 0;
    do {
        u64 t = ((u64)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime;
        if (days > 0 && now - t < (u64)(days * 864000000000.0)) continue;
        if (DeleteFileW(path_join(dir, fd.cFileName).s)) n++;
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    return n;
}
