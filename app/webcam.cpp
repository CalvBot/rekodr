// Webcam: the camera the player picks, recorded into the buffer beside the screen (Options > Recording; off by
// default), and a small live picture in Options while that row is on screen.
//
// One ffmpeg reads the camera through DirectShow. While recording it writes 2 s H.264 segments in the video's
// format and, down its stdout, a small picture; for the Options picture alone it writes only the picture. The camera
// is open only while one of the two is wanted, and recording follows the screen's recording switch.
//
// On-disk format, as recorder.cpp's video (the pruner and buffer_read_cam read it the same way):
//   <buffer>\cam\<run>\v_00000.mkv ...   list.csv, anchor.txt
//
// The worker owns the ffmpeg. The main thread hands it what's wanted every frame (webcam_tick) and reads back what
// it's doing, both through g_cam under its lock.

enum { CAM_PW = 256, CAM_PH = 144, CAM_FPS = 30, CAM_MAX_DEVICES = 16, CAM_NAME = 128 };

struct CamWant {
    bool record;
    char device[CAM_NAME]; // "" = the first camera
    Path ffmpeg;           // as configured; empty = find it
    Path buffer_dir;
    Path local_dir;        // for ffmpeg-cam.log
    i32  vendor;           // the H.264 encoder's Vendor
};

struct CamShared {
    SRWLOCK lock;
    CamWant want;
    volatile LONG64 preview_tick; // GetTickCount64 when Options last drew the picture
    volatile LONG   list_now;     // Options wants the camera list read again
    HANDLE wake, stop, thread;
    // The worker's, read by the main thread
    char devices[CAM_MAX_DEVICES][CAM_NAME];
    i32  device_count;
    bool devices_known;
    bool open;            // the camera is open
    bool recording;       // and segments are being written
    wchar_t run[64];      // the run folder being written; the pruner leaves it alone
    char error[256];      // why it isn't working; empty while it is
    u32  pic[CAM_PW * CAM_PH];
    u64  pic_seq;         // counts pictures; 0 = none since the camera opened
};
static CamShared g_cam;

// The cameras in ffmpeg's DirectShow list: '[in#0 @ 0000...] "Name" (video)' lines.
static i32 cam_parse_devices(const char *text, char (*out)[CAM_NAME], i32 cap)
{
    i32 n = 0;
    for (const char *line = text; *line && n < cap;) {
        const char *eol = strchr(line, '\n');
        size_t len = eol ? (size_t)(eol - line) : strlen(line);
        const char *q1 = (const char *)memchr(line, '"', len);
        const char *q2 = q1 ? (const char *)memchr(q1 + 1, '"', len - (size_t)(q1 + 1 - line)) : 0;
        if (q2 && q2 - q1 > 1 && q2 - q1 - 1 < CAM_NAME && len - (size_t)(q2 + 1 - line) >= 8 && !strncmp(q2 + 1, " (video)", 8)) {
            memcpy(out[n], q1 + 1, (size_t)(q2 - q1 - 1));
            out[n][q2 - q1 - 1] = 0;
            n++;
        }
        line += len + (eol ? 1 : 0);
    }
    return n;
}

// The H.264 encoder settings for the camera: small, steady quality, a keyframe every segment.
static void cam_encoder_args(Args *a, i32 vendor)
{
    switch (vendor) {
    case VENDOR_NVENC: ADD("-c:v", "h264_nvenc", "-preset", "p4", "-rc", "vbr", "-cq", "26", "-b:v", "0", "-maxrate", "4M", "-bufsize", "8M"); break;
    case VENDOR_AMF: ADD("-c:v", "h264_amf", "-quality", "balanced", "-rc", "vbr_peak", "-b:v", "3M", "-maxrate", "4M", "-bufsize", "8M"); break;
    case VENDOR_QSV: ADD("-c:v", "h264_qsv", "-preset", "faster", "-global_quality", "26", "-b:v", "3M", "-maxrate", "4M", "-bufsize", "8M"); break;
    default: ADD("-c:v", "libx264", "-preset", "veryfast", "-crf", "24", "-maxrate", "4M", "-bufsize", "8M", "-sc_threshold", "0"); break;
    }
    ADD("-bf", "0", "-g");
    addf(a, "%d", CAM_FPS * SEGMENT_SECS);
}

// The camera's ffmpeg up to the segment list's and the pattern's paths (recording), or whole (the picture alone).
// Recording, frame 0's wall-clock time is printed (as the screen's is) and saved as the run's anchor. The picture
// goes down stdout as raw BGRA, CAM_PW x CAM_PH, 10 a second.
static void cam_args(Args *a, const char *device, bool record, i32 vendor)
{
    a->s[0] = 0;
    ADD("-hide_banner", "-nostats", "-loglevel", "info", "-f", "dshow", "-rtbufsize", "64M", "-i");
    addf(a, "video=%s", device);
    char pic[160];
    snprintf(pic, sizeof pic, "fps=10,scale=%d:%d:force_original_aspect_ratio=decrease,pad=%d:%d:-1:-1,format=bgra", CAM_PW,
             CAM_PH, CAM_PW, CAM_PH);
    ADD("-filter_complex");
    if (record) {
        addf(a,
             "[0:v]setpts=PTS-STARTPTS,setpts='PTS+0*if(eq(N\\,0)\\,print(RTCTIME)\\,0)',scale=w=-2:h='min(720\\,ih)',"
             "fps=%d,format=nv12,split=2[rec][p];[p]%s[pic]",
             CAM_FPS, pic);
        ADD("-map", "[pic]", "-f", "rawvideo", "pipe:1", "-map", "[rec]");
        cam_encoder_args(a, vendor);
        ADD("-f", "segment", "-segment_time");
        addf(a, "%d", SEGMENT_SECS);
        ADD("-segment_format", "matroska", "-reset_timestamps", "0", "-segment_list_type", "csv", "-segment_list");
    } else {
        addf(a, "[0:v]%s[pic]", pic);
        ADD("-map", "[pic]", "-f", "rawvideo", "pipe:1");
    }
}

struct CamRun {
    Proc   p;
    HANDLE pic_thread, err_thread;
    Path   run_dir; // recording: where the anchor goes; else empty
    Path   log;
    char   last[256]; // ffmpeg's last line, for the error
};

static DWORD WINAPI cam_pic_thread(void *arg)
{
    CamRun *r = (CamRun *)arg;
    static u32 frame[CAM_PW * CAM_PH]; // one run at a time
    for (;;) {
        u8 *at = (u8 *)frame;
        DWORD left = sizeof frame;
        while (left) {
            DWORD got = 0;
            if (!ReadFile(r->p.out, at, left, &got, 0) || !got) return 0;
            at += got;
            left -= got;
        }
        AcquireSRWLockExclusive(&g_cam.lock);
        memcpy(g_cam.pic, frame, sizeof frame);
        g_cam.pic_seq++;
        g_cam.error[0] = 0;
        ReleaseSRWLockExclusive(&g_cam.lock);
    }
}

// The recorded size from ffmpeg's line for the H.264 output stream ("Stream #1:0: Video: h264 (Main), nv12(...),
// 1280x720 [SAR 1:1 DAR 16:9], ..."). False for any other line.
static bool cam_parse_size(const char *line, i32 *w, i32 *h)
{
    const char *v = strstr(line, "Video: h264");
    if (!v) return false;
    for (const char *c = v; *c; c++) {
        if (c[0] != ' ' || c[1] < '1' || c[1] > '9') continue;
        i32 x = 0, y = 0;
        if (sscanf(c + 1, "%dx%d", &x, &y) == 2 && x >= 16 && y >= 16) {
            *w = x;
            *h = y;
            return true;
        }
    }
    return false;
}

static void cam_write_small(const Path &dir, const wchar_t *name, const char *text)
{
    HANDLE f = CreateFileW(path_join(dir, name).s, GENERIC_WRITE, FILE_SHARE_READ, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD written;
    WriteFile(f, text, (DWORD)strlen(text), &written, 0);
    CloseHandle(f);
}

static DWORD WINAPI cam_err_thread(void *arg)
{
    CamRun *r = (CamRun *)arg;
    FILE *log = _wfopen(r->log.s, L"ab");
    char line[2048];
    while (pipe_read_line(r->p.err, line, sizeof line)) {
        if (starts_with(line, "[Eval @")) { // RTCTIME of frame 0, us since 1970
            const char *v = strrchr(line, ' ');
            f64 us = v ? strtod(v + 1, 0) : 0;
            if (us > 0 && r->run_dir.s[0]) {
                char text[32];
                snprintf(text, sizeof text, "%lld", (i64)llround(us));
                cam_write_small(r->run_dir, L"anchor.txt", text);
            }
            continue;
        }
        i32 sw, sh; // saves fit a later run of another size into this one's
        if (r->run_dir.s[0] && cam_parse_size(line, &sw, &sh)) {
            char text[32];
            snprintf(text, sizeof text, "%d %d", sw, sh);
            cam_write_small(r->run_dir, L"size.txt", text);
        }
        if (starts_with(line, "  ") || strstr(line, "Opening '")) continue;
        if (log) fprintf(log, "%s\n", line);
        str_set(r->last, line);
    }
    if (log) fclose(log);
    return 0;
}

// Reads the camera list (no camera is opened for it).
static void cam_list(const Path &ffmpeg)
{
    wchar_t args[256] = L"";
    static const char *const list[] = {"-hide_banner", "-list_devices", "true", "-f", "dshow", "-i", "dummy"};
    for (const char *x : list) cmd_arg8(args, COUNT(args), x);
    static char out[1 << 15];
    proc_run(ffmpeg.s, args, 15000, out, sizeof out);
    static char found[CAM_MAX_DEVICES][CAM_NAME];
    i32 n = cam_parse_devices(out, found, CAM_MAX_DEVICES);
    AcquireSRWLockExclusive(&g_cam.lock);
    memcpy(g_cam.devices, found, sizeof found);
    g_cam.device_count = n;
    g_cam.devices_known = true;
    ReleaseSRWLockExclusive(&g_cam.lock);
    log_line("webcam: %d camera%s found", n, n == 1 ? "" : "s");
}

static bool cam_open(CamRun *r, const Path &ffmpeg, const CamWant &w, const char *device, bool record, wchar_t *run)
{
    *r = {};
    r->log = path_join(w.local_dir, L"ffmpeg-cam.log");
    static Args cmd;
    cam_args(&cmd, device, record, w.vendor);
    run[0] = 0;
    if (record) {
        run_name(run, 64);
        r->run_dir = path_join(path_join(w.buffer_dir, L"cam"), run);
        i32 made = SHCreateDirectoryExW(0, r->run_dir.s, 0);
        if (made != ERROR_SUCCESS && made != ERROR_ALREADY_EXISTS) {
            log_line("webcam: couldn't create its buffer folder (%d)", made);
            return false;
        }
        cmd_arg(cmd.s, COUNT(cmd.s), path_join(r->run_dir, L"list.csv").s);
        cmd_arg(cmd.s, COUNT(cmd.s), path_join(r->run_dir, L"v_%05d.mkv").s);
    }
    if (!proc_start(ffmpeg.s, cmd.s, PROC_STDIN | PROC_STDOUT | PROC_STDERR, &r->p)) return false;
    r->pic_thread = thread_start(cam_pic_thread, r);
    r->err_thread = thread_start(cam_err_thread, r);
    log_line("webcam: %s opened%s", device, record ? ", recording" : " for the Options picture");
    return true;
}

static void cam_close(CamRun *r)
{
    if (r->p.in) {
        DWORD written;
        WriteFile(r->p.in, "q", 1, &written, 0);
        proc_close_stdin(&r->p);
    }
    if (proc_wait(&r->p, 5000) < 0) {
        proc_kill(&r->p);
        proc_wait(&r->p, 2000);
    }
    if (r->pic_thread) { WaitForSingleObject(r->pic_thread, 3000); CloseHandle(r->pic_thread); }
    if (r->err_thread) { WaitForSingleObject(r->err_thread, 3000); CloseHandle(r->err_thread); }
    proc_close(&r->p);
    *r = {};
}

static DWORD WINAPI cam_thread(void *)
{
    static CamRun r;
    bool running = false, running_record = false;
    char running_dev[CAM_NAME] = "";
    Path running_buffer = {};
    wchar_t run[64] = L"";
    u64 retry_at = 0;
    for (;;) {
        CamWant w;
        AcquireSRWLockShared(&g_cam.lock);
        w = g_cam.want;
        bool known = g_cam.devices_known;
        ReleaseSRWLockShared(&g_cam.lock);
        u64 now = GetTickCount64();
        bool preview = now - (u64)g_cam.preview_tick < 1500;
        Path ffmpeg = {};
        bool have_ffmpeg = ffmpeg_find(w.ffmpeg, &ffmpeg);
        if (have_ffmpeg && (InterlockedExchange(&g_cam.list_now, 0) || ((w.record || preview) && !w.device[0] && !known)))
            cam_list(ffmpeg);
        char dev[CAM_NAME];
        AcquireSRWLockShared(&g_cam.lock);
        str_set(dev, w.device[0] ? w.device : g_cam.device_count ? g_cam.devices[0] : "");
        ReleaseSRWLockShared(&g_cam.lock);
        bool want_open = have_ffmpeg && dev[0] && (w.record || preview);
        bool want_record = want_open && w.record && w.buffer_dir.s[0];

        if (running) {
            bool exited = proc_wait(&r.p, 0) >= 0;
            if (exited || !want_open || want_record != running_record || strcmp(dev, running_dev) ||
                (want_record && wcscmp(w.buffer_dir.s, running_buffer.s))) {
                char last[256];
                str_set(last, r.last);
                cam_close(&r);
                running = false;
                if (exited) {
                    log_line("webcam: ffmpeg stopped: %s", last[0] ? last : "(nothing said)");
                    AcquireSRWLockExclusive(&g_cam.lock);
                    snprintf(g_cam.error, sizeof g_cam.error, "The camera stopped: %s", last[0] ? last : "it may be in use");
                    ReleaseSRWLockExclusive(&g_cam.lock);
                    retry_at = now + 10000;
                } else {
                    log_line("webcam: closed");
                }
            }
        }
        if (!running && want_open && now >= retry_at) {
            if (cam_open(&r, ffmpeg, w, dev, want_record, run)) {
                running = true;
                running_record = want_record;
                str_set(running_dev, dev);
                running_buffer = w.buffer_dir;
            } else {
                retry_at = now + 10000;
            }
        }
        AcquireSRWLockExclusive(&g_cam.lock);
        g_cam.open = running;
        g_cam.recording = running && running_record;
        wcscpy_s(g_cam.run, running && running_record ? run : L"");
        if (!running) g_cam.pic_seq = 0;
        if (w.record && known && !dev[0]) str_set(g_cam.error, "No camera found");
        else if (!want_open && !w.record) g_cam.error[0] = 0;
        ReleaseSRWLockExclusive(&g_cam.lock);

        HANDLE waits[2] = {g_cam.stop, g_cam.wake};
        if (WaitForMultipleObjects(2, waits, FALSE, 250) == WAIT_OBJECT_0) break;
    }
    if (running) cam_close(&r);
    return 0;
}

// ---------------------------------------------------------------------------------------------------------------
// The main thread's side

static struct {
    ID3D11ShaderResourceView *srv; // the Options picture
    u64 seq;
} g_cam_ui;

static void webcam_start()
{
    g_cam.wake = CreateEventW(0, FALSE, FALSE, 0);
    g_cam.stop = CreateEventW(0, TRUE, FALSE, 0);
    g_cam.thread = thread_start(cam_thread, 0);
}

static void webcam_stop()
{
    if (!g_cam.thread) return;
    SetEvent(g_cam.stop);
    WaitForSingleObject(g_cam.thread, 10000);
    CloseHandle(g_cam.thread);
    g_cam.thread = 0;
    if (g_cam_ui.srv) g_cam_ui.srv->Release();
    g_cam_ui = {};
}

// What the worker should be doing, from the settings and the recording switch; it's woken when that changes.
static void webcam_tick()
{
    if (!g_cam.thread) return;
    const Config &c = g_app.config;
    const Recorder &rec = g_app.recorder;
    CamWant w = {};
    // H.264 on the encoder the screen uses when it can make H.264, else the first that can, else the CPU
    w.vendor = VENDOR_CPU;
    u8 ok = rec.encoders_ok[0];
    if (ok & (1u << rec.vendor)) w.vendor = rec.vendor;
    else
        for (i32 v = 0; v < VENDOR_COUNT; v++)
            if (ok & (1u << v)) { w.vendor = v; break; }
    w.record = c.cam_on && !rec.paused && rec.encoders_known;
    str_set(w.device, c.cam_device.s);
    w.ffmpeg = c.ffmpeg;
    w.buffer_dir = c.buffer_dir;
    w.local_dir = g_app.paths.local_dir;
    bool changed;
    AcquireSRWLockExclusive(&g_cam.lock);
    changed = memcmp(&w, &g_cam.want, sizeof w) != 0;
    if (changed) g_cam.want = w;
    ReleaseSRWLockExclusive(&g_cam.lock);
    if (changed) SetEvent(g_cam.wake);
}

// The cameras found; false until they've been looked for.
static bool webcam_devices(char (*out)[128], i32 cap, i32 *n)
{
    AcquireSRWLockShared(&g_cam.lock);
    *n = g_cam.device_count < cap ? g_cam.device_count : cap;
    memcpy(out, g_cam.devices, (size_t)*n * CAM_NAME);
    bool known = g_cam.devices_known;
    ReleaseSRWLockShared(&g_cam.lock);
    return known;
}

static void webcam_list_again()
{
    InterlockedExchange(&g_cam.list_now, 1);
    SetEvent(g_cam.wake);
}

static bool webcam_recording()
{
    AcquireSRWLockShared(&g_cam.lock);
    bool r = g_cam.recording;
    ReleaseSRWLockShared(&g_cam.lock);
    return r;
}

static void webcam_error(char *out, i32 cap)
{
    AcquireSRWLockShared(&g_cam.lock);
    snprintf(out, cap, "%s", g_cam.error);
    ReleaseSRWLockShared(&g_cam.lock);
}

static void webcam_run(wchar_t *out, i32 cap)
{
    AcquireSRWLockShared(&g_cam.lock);
    swprintf(out, cap, L"%s", g_cam.run);
    ReleaseSRWLockShared(&g_cam.lock);
}

// The live picture for Options, CAM_PW x CAM_PH; 0 until the camera sends one. Calling it is what keeps the camera
// open for the picture, so Options calls it only while the row is on screen.
static ID3D11ShaderResourceView *webcam_picture()
{
    if (!g_cam.thread) return 0;
    u64 now = GetTickCount64();
    if (now - (u64)InterlockedExchange64(&g_cam.preview_tick, (LONG64)now) > 1500) SetEvent(g_cam.wake); // just shown
    ui_wake(100);
    static u32 px[CAM_PW * CAM_PH];
    bool fresh = false;
    AcquireSRWLockShared(&g_cam.lock);
    u64 seq = g_cam.pic_seq;
    if (seq && seq != g_cam_ui.seq) {
        memcpy(px, g_cam.pic, sizeof px);
        fresh = true;
    }
    ReleaseSRWLockShared(&g_cam.lock);
    if (!seq && g_cam_ui.srv) {
        g_cam_ui.srv->Release();
        g_cam_ui.srv = 0;
    }
    if (fresh) {
        for (u32 &p : px) p |= 0xff000000u; // opaque, whatever ffmpeg's alpha says
        ID3D11ShaderResourceView *srv = gpu_texture(px, CAM_PW, CAM_PH);
        if (srv) {
            if (g_cam_ui.srv) g_cam_ui.srv->Release();
            g_cam_ui.srv = srv;
        }
        g_cam_ui.seq = seq;
    }
    return g_cam_ui.srv;
}
