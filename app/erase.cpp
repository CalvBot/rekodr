// Deleting a stretch of the buffer for good (the Timeline's Delete footage), on a worker thread, a stretch at a time.
//
// - Video: every segment that overlaps the stretch is deleted, so it goes in the segments' 2 s steps, rounded out.
//   A segment missing from a run reads as a gap (buffer.cpp), and saves cut gaps out.
// - Sound: saves join a stream's segments end to end, so none is deleted. Every segment that overlaps the stretch
//   is written again by ffmpeg with the stretch silent, exactly at its edges, and the stream's peaks.bin is zeroed
//   there.
// - The webcam's video (buffer\cam) goes as the screen's does.
// - An unsaved session's poster made from a deleted segment is deleted too (thumbs\posters).
// Footage still being written (the newest video segment, a stream's open sound segment) is done once ffmpeg has
// finished it: the stretch is gone over every second until 15 s after its end. Each pass that changed something
// posts EV_ERASE (b = 0), and the end of the job posts it with b = 1.

enum { ERASE_QUEUE = 8, ERASE_DONE_MAX = 4096 };
static const i64 ERASE_LIVE_US = 15 * 1000000LL; // after a stretch's end, everything over it has been finished

struct EraseJob {
    i64  from_us, to_us;
    Path buffer_dir, ffmpeg, posters; // posters: thumbs\posters
};

struct EraseDone { // a sound segment already silenced in this job
    char dir[128];
    char name[20];
};

static struct {
    SRWLOCK            lock;
    CONDITION_VARIABLE wake;
    HANDLE             thread; // started on first use; main thread only
    EraseJob           q[ERASE_QUEUE];
    i32                n;
    bool               busy; // a job taken from q is being done
} g_erase;

static bool erase_was_done(const EraseDone *done, i32 n, const char *dir, const char *name)
{
    for (i32 i = 0; i < n; i++)
        if (!strcmp(done[i].name, name) && !strcmp(done[i].dir, dir)) return true;
    return false;
}

// Writes `file` again with [a, b] (seconds of its stream) silent, through a temp file in the same folder.
static bool erase_silence(const EraseJob &j, const Path &dir, const char *name, f64 a, f64 b)
{
    wchar_t wname[32], tname[48];
    utf8_to_wide(name, wname, COUNT(wname));
    swprintf(tname, COUNT(tname), L"erase-%s.tmp", wname);
    Path in = path_join(dir, wname), tmp = path_join(dir, tname);
    wchar_t args[4096];
    args[0] = 0;
    i32 cap = COUNT(args);
    // -copyts keeps the segment's timestamps, which carry on from the stream's start, so `t` is a stream time.
    static const char *const head[] = {"-hide_banner", "-loglevel", "error", "-nostdin", "-y", "-copyts", "-i"};
    arg_list(args, cap, head, COUNT(head));
    cmd_arg(args, cap, in.s);
    arg_pair(args, cap, "-af", "volume=enable='between(t,%.6f,%.6f)':volume=0", a, b);
    static const char *const tail[] = {"-c:a", "libopus", "-b:a", "128k", "-application", "audio", "-f", "matroska"};
    arg_list(args, cap, tail, COUNT(tail));
    cmd_arg(args, cap, tmp.s);
    char out[512];
    i32 code = proc_run(j.ffmpeg.s, args, 60000, out, sizeof out);
    if (code != 0) {
        log_line("erase: couldn't silence %s (%d): %s", name, code, trim(out));
        DeleteFileW(tmp.s);
        return false;
    }
    if (!MoveFileExW(tmp.s, in.s, MOVEFILE_REPLACE_EXISTING)) {
        log_line("erase: couldn't replace %s (%d)", name, (int)GetLastError());
        DeleteFileW(tmp.s);
        return false;
    }
    return true;
}

// Zeroes the stream's waveform over the stretch, as far as it has been written.
static void erase_peaks(const EraseJob &j, const Path &dir, const AudioStreamInfo &st)
{
    HANDLE h = CreateFileW(path_join(dir, L"peaks.bin").s, GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0, OPEN_EXISTING, 0, 0);
    if (h == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER size = {};
    GetFileSizeEx(h, &size);
    i64 a = (j.from_us - st.start_us) / 100000, b = (j.to_us - st.start_us + 99999) / 100000;
    if (a < 0) a = 0;
    if (b > size.QuadPart) b = size.QuadPart;
    static const u8 zero[4096] = {};
    for (i64 at = a; at < b;) {
        DWORD n = (DWORD)(b - at < (i64)sizeof zero ? b - at : (i64)sizeof zero), wrote = 0;
        LARGE_INTEGER pos;
        pos.QuadPart = at;
        if (!SetFilePointerEx(h, pos, 0, FILE_BEGIN) || !WriteFile(h, zero, n, &wrote, 0) || !wrote) break;
        at += wrote;
    }
    CloseHandle(h);
}

// One pass over the stretch. True when something was deleted or silenced; *open when a part of it is still being
// written.
static bool erase_pass(const EraseJob &j, BufferIdx *b, EraseDone *done, i32 *done_n, i32 *videos, bool *open)
{
    buffer_read(j.buffer_dir, b);
    bool changed = false;
    *open = false;
    Path video = path_join(j.buffer_dir, L"video"), audio = path_join(j.buffer_dir, L"audio");
    for (i32 i = 0; i < b->seg_count; i++) {
        const VideoSeg &v = b->segs[i];
        if (v.end_us <= j.from_us || v.start_us >= j.to_us) continue;
        wchar_t run[64], name[32], poster[64];
        utf8_to_wide(b->runs[v.run].name, run, COUNT(run));
        utf8_to_wide(v.name, name, COUNT(name));
        Path p = path_join(path_join(video, run), name);
        if (DeleteFileW(p.s)) {
            (*videos)++;
            changed = true;
        } else {
            log_line("erase: couldn't delete %s\\%s (%d)", b->runs[v.run].name, v.name, (int)GetLastError());
        }
        swprintf(poster, COUNT(poster), L"p-%016llx.jpg", (u64)v.start_us);
        DeleteFileW(path_join(j.posters, poster).s);
    }
    // The newest video segment isn't listed until it's finished, so it's not in the index yet
    if (b->video_written_us && now_us() - b->video_written_us < 10 * 1000000LL && b->newest_us < j.to_us) *open = true;

    // The webcam's segments the same way (not counted in *videos)
    static BufferIdx cam; // the erase thread's
    buffer_read_cam(j.buffer_dir, &cam);
    Path cam_dir = path_join(j.buffer_dir, L"cam");
    for (i32 i = 0; i < cam.seg_count; i++) {
        const VideoSeg &v = cam.segs[i];
        if (v.end_us <= j.from_us || v.start_us >= j.to_us) continue;
        wchar_t run[64], name[32];
        utf8_to_wide(cam.runs[v.run].name, run, COUNT(run));
        utf8_to_wide(v.name, name, COUNT(name));
        if (DeleteFileW(path_join(path_join(cam_dir, run), name).s)) changed = true;
        else log_line("erase: couldn't delete cam %s\\%s (%d)", cam.runs[v.run].name, v.name, (int)GetLastError());
    }
    if (cam.video_written_us && now_us() - cam.video_written_us < 10 * 1000000LL && cam.newest_us < j.to_us) *open = true;

    for (i32 s = 0; s < b->stream_count; s++) {
        const AudioStreamInfo &st = b->streams[s];
        wchar_t wdir[128];
        utf8_to_wide(st.dir, wdir, COUNT(wdir));
        Path dir = path_join(audio, wdir);
        bool touched = false;
        for (i32 k = 0; k < st.seg_count; k++) {
            const AudioSeg &g = b->audio_segs[st.first_seg + k];
            if (g.open) {
                if (g.start_us < j.to_us) {
                    *open = true;
                    touched = true; // its peaks are being written
                }
                continue;
            }
            if (g.end_us <= j.from_us || g.start_us >= j.to_us) continue;
            touched = true;
            if (erase_was_done(done, *done_n, st.dir, g.name)) continue;
            f64 a = (f64)(j.from_us - st.start_us) / 1e6, z = (f64)(j.to_us - st.start_us) / 1e6;
            if (erase_silence(j, dir, g.name, a, z)) changed = true;
            if (*done_n < ERASE_DONE_MAX) {
                str_set(done[*done_n].dir, st.dir);
                str_set(done[*done_n].name, g.name);
                (*done_n)++;
            }
        }
        if (touched) erase_peaks(j, dir, st);
    }
    return changed;
}

static DWORD WINAPI erase_thread(void *)
{
    BufferIdx *b = (BufferIdx *)VirtualAlloc(0, sizeof(BufferIdx), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    EraseDone *done = (EraseDone *)malloc(ERASE_DONE_MAX * sizeof(EraseDone));
    static EraseJob j;
    for (;;) {
        AcquireSRWLockExclusive(&g_erase.lock);
        g_erase.busy = false;
        while (!g_erase.n) SleepConditionVariableSRW(&g_erase.wake, &g_erase.lock, INFINITE, 0);
        j = g_erase.q[0];
        memmove(&g_erase.q[0], &g_erase.q[1], (g_erase.n - 1) * sizeof g_erase.q[0]);
        g_erase.n--;
        g_erase.busy = true;
        ReleaseSRWLockExclusive(&g_erase.lock);

        i32 done_n = 0, videos = 0;
        if (b && done) {
            for (;;) {
                bool open = false, live = now_us() < j.to_us + ERASE_LIVE_US;
                bool changed = erase_pass(j, b, done, &done_n, &videos, &open);
                if (changed) {
                    Event e = {};
                    e.kind = EV_ERASE;
                    event_post(e);
                }
                if (!live || !open) break;
                Sleep(1000);
            }
        }
        log_line("erase: %lld s of footage deleted, %d video segments and %d sound segments",
                 (j.to_us - j.from_us) / 1000000, videos, done_n);
        Event e = {};
        e.kind = EV_ERASE;
        e.b = 1;
        event_post(e);
    }
}

// Main thread: deletes [from_us, to_us) of the buffer for good. False when too many stretches are waiting.
static bool erase_start(const Path &buffer_dir, const Path &ffmpeg, const Path &posters, i64 from_us, i64 to_us)
{
    if (to_us <= from_us) return false;
    AcquireSRWLockExclusive(&g_erase.lock);
    bool queued = g_erase.n < ERASE_QUEUE;
    if (queued) {
        EraseJob &j = g_erase.q[g_erase.n++];
        j.from_us = from_us;
        j.to_us = to_us;
        j.buffer_dir = buffer_dir;
        j.ffmpeg = ffmpeg;
        j.posters = posters;
        WakeConditionVariable(&g_erase.wake);
    }
    ReleaseSRWLockExclusive(&g_erase.lock);
    if (!queued) log_line("erase: too many stretches waiting; this one wasn't deleted");
    if (!g_erase.thread) g_erase.thread = thread_start(erase_thread, 0);
    return queued;
}

// A stretch is waiting or being deleted.
static bool erase_running()
{
    AcquireSRWLockShared(&g_erase.lock);
    bool r = g_erase.n > 0 || g_erase.busy;
    ReleaseSRWLockShared(&g_erase.lock);
    return r;
}
