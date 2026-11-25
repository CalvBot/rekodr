// Video: the capture ffmpeg (ddagrab -> GPU encoder -> 2 s segments in the buffer), finding a working encoder, and
// pruning the buffer to buffer_hours and the free-space floor.
//
// Workers, none of which touch g_app:
// - detection: a one-frame test encode per vendor and codec at start, retried while no GPU encoder works (the
//   driver can still be loading when Windows starts Rekodr);
// - the recorder thread: starts ffmpeg, and restarts it when it exits (lock screen, UAC, display change, driver
//   reset), on pause and resume, and when settings that affect it change;
// - the pruner: every 30 s deletes footage older than buffer_hours, or the oldest early when free space is low.
// The main thread hands them settings through g_rec (under its lock); they report back with EV_RECORDER events.
//
// On-disk format:
//   <buffer>\video\<run>\v_00000.mkv ...   one folder per ffmpeg run, named after its local start time
//                        list.csv         "v_00000.mkv,0.000000,2.000000" per finished segment (ffmpeg writes it)
//                        anchor.txt       wall-clock us of the run's first frame, no line ending
//   <buffer>\audio\<stream>\a_00000.mka   audio.cpp's, with list.csv and meta.json ("start_us": us)
//   <buffer>\activity.tsv, focus.tsv     "us<TAB>exe" lines; the pruner drops the old ones

#define SEGMENT_SECS 2

static const char *VENDOR_KEYS[VENDOR_COUNT] = {"nvenc", "amf", "qsv", "cpu"};
static const char *CODECS[CODEC_COUNT] = {"h264", "hevc", "av1"};

// Event.a for EV_RECORDER
enum RecEvent : i64 {
    REC_DETECTED = 1, // b: vendor bits per codec, codec i in byte i
    REC_STARTED,      // b: vendor; text: "<encoder>|<run>"
    REC_STOPPED,      // ffmpeg isn't capturing any more
    REC_ERROR,        // text: what's wrong; empty when healthy again
    REC_BUFFER,       // b: oldest buffered video (us, 0 = none); text: "<buffer bytes> <free bytes>"
    REC_STRAIN,       // text: the encoder can't keep up, with a tip; empty when it can again
    REC_PACE,         // text: a 10 s window while a game is played, "wall_ms|video_ms|frames|drops" (quality.cpp)
};

static void quality_pace(u64 tick, const char *text);

static i32 codec_index(const char *codec)
{
    for (i32 i = 0; i < CODEC_COUNT; i++)
        if (!strcmp(CODECS[i], codec)) return i;
    return 0; // anything else records H.264
}

static i32 vendor_parse(const char *s)
{
    for (i32 i = 0; i < VENDOR_COUNT; i++)
        if (!strcmp(VENDOR_KEYS[i], s)) return i;
    return -1; // "auto"
}

// ffmpeg's name for a vendor's encoder of `codec`.
static const char *encoder_name(i32 vendor, const char *codec)
{
    static const char *names[VENDOR_COUNT][CODEC_COUNT] = {
        {"h264_nvenc", "hevc_nvenc", "av1_nvenc"},
        {"h264_amf", "hevc_amf", "av1_amf"},
        {"h264_qsv", "hevc_qsv", "av1_qsv"},
        {"libx264", "libx265", "libsvtav1"},
    };
    return names[vendor][codec_index(codec)];
}

// What goes after ddagrab in the capture graph. NVENC and AMF take its D3D11 textures as they are; QSV maps them
// to its own surfaces (its encoders want NV12); the CPU needs them in memory.
static const char *capture_filter(i32 vendor)
{
    switch (vendor) {
    case VENDOR_QSV: return ",hwmap=derive_device=qsv,format=qsv,vpp_qsv=format=nv12";
    case VENDOR_CPU: return ",hwdownload,format=bgra,format=yuv420p";
    default: return "";
    }
}

// The same, scaled down to w x h on the way. QSV scales on its own surfaces; the rest scale on the CPU, because
// this ffmpeg's scale_d3d11 and the D3D11 to CUDA mapping both fail on ddagrab's frames. Bilinear 1440p to 1080p
// keeps up at 60 fps, and NVENC takes the BGRA frames from memory as they are.
static void capture_filter_scaled(i32 vendor, i32 w, i32 h, char *out, i32 cap)
{
    if (w <= 0 || h <= 0) { snprintf(out, cap, "%s", capture_filter(vendor)); return; }
    switch (vendor) {
    case VENDOR_QSV: snprintf(out, cap, ",hwmap=derive_device=qsv,format=qsv,vpp_qsv=format=nv12:w=%d:h=%d", w, h); break;
    case VENDOR_CPU: snprintf(out, cap, ",hwdownload,format=bgra,scale=%d:%d:flags=bilinear,format=yuv420p", w, h); break;
    case VENDOR_AMF: snprintf(out, cap, ",hwdownload,format=bgra,scale=%d:%d:flags=bilinear:threads=0,format=nv12", w, h); break;
    default: snprintf(out, cap, ",hwdownload,format=bgra,scale=%d:%d:flags=bilinear:threads=0", w, h); break;
    }
}

// A monitor's refresh rate as Windows reports it, in whole Hz: 59.94 Hz reports as 59, so it counts as 60.
static i32 display_hz(DWORD reported) { return reported > 1 && (reported + 1) % 10 == 0 ? (i32)reported + 1 : (i32)reported; }

// A monitor's size in pixels, refresh rate and HMONITOR (gfxcapture's hmonitor), by its index on the first GPU (as
// ddagrab's output_idx counts). False if it isn't there.
static bool monitor_mode(i32 idx, i32 *w, i32 *h, i32 *hz, u64 *hmon)
{
    *w = *h = *hz = 0;
    *hmon = 0;
    IDXGIFactory1 *factory = 0;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;
    IDXGIAdapter1 *adapter = 0;
    IDXGIOutput *out = 0;
    if (SUCCEEDED(factory->EnumAdapters1(0, &adapter))) {
        if (adapter->EnumOutputs((UINT)idx, &out) == S_OK) {
            DXGI_OUTPUT_DESC d = {};
            out->GetDesc(&d);
            *hmon = (u64)(uintptr_t)d.Monitor;
            *w = d.DesktopCoordinates.right - d.DesktopCoordinates.left;
            *h = d.DesktopCoordinates.bottom - d.DesktopCoordinates.top;
            DEVMODEW mode = {};
            mode.dmSize = sizeof mode;
            if (EnumDisplaySettingsW(d.DeviceName, ENUM_CURRENT_SETTINGS, &mode)) *hz = display_hz(mode.dmDisplayFrequency);
            out->Release();
        }
        adapter->Release();
    }
    factory->Release();
    return *w > 0;
}

static i64 filetime_us(FILETIME ft)
{
    i64 t = ((i64)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return (t - 116444736000000000LL) / 10;
}

// ---------------------------------------------------------------------------------------------------------------
// Shared between the main thread and the workers: only these fields cross threads, always under `lock`.

// What the workers need from Config, copied by the main thread.
struct RecSettings {
    Path  ffmpeg;    // as configured; empty = find it
    Path  buffer_dir;
    Path  local_dir; // for ffmpeg-video.log
    bool  buffer_by_size;
    f32   buffer_hours;
    f32   buffer_gb;
    f32   min_free_gb;
    i32   output_idx;
    i32   fps;           // never above the monitor's refresh rate
    u64   hmonitor;      // output_idx's monitor, for Windows Graphics Capture
    u64   hwnd;          // a game's window to record instead of the monitor (Windows Graphics Capture only); 0 = none
    i32   capture;       // CAPTURE_*
    bool  game;          // a game is being played: its screen should keep changing (the frozen-frame check)
    i32   width, height; // scaled to this; 0 = the monitor's own size (never scaled up)
    Str64 codec;
    Str64 encoder;
    f32   bitrate_mbps;
    bool  draw_mouse;
    // The encoder settings in effect
    Str64 preset;
    Str64 rate_control;
    i32   cq;
    f32   peak_factor;
    bool  spatial_aq;
    char  encoder_args[256];
};

enum { CAPTURE_AUTO, CAPTURE_WGC, CAPTURE_DXGI };

static i32 capture_parse(const char *s)
{
    if (!strcmp(s, "wgc")) return CAPTURE_WGC;
    if (!strcmp(s, "dxgi")) return CAPTURE_DXGI;
    return CAPTURE_AUTO;
}

struct RecShared {
    SRWLOCK     lock;
    RecSettings settings;
    u32         generation; // bumped when the settings or the detection result change
    bool        paused;
    bool        detected;   // detection has a result
    u8          works[CODEC_COUNT]; // per codec, bit per vendor whose test encode worked
    wchar_t     run[64];    // the run folder being written; the pruner leaves it alone
    HANDLE      wake;        // auto-reset: the recorder thread looks at the shared state now
    HANDLE      stop;        // manual-reset: Rekodr is quitting
    HANDLE      detect_done; // manual-reset: detection has a result
    HANDLE      recorder_thread, prune_thread;
};

static RecShared g_rec;

static void rec_post(i64 sub, i64 b, const char *text)
{
    Event e = {};
    e.kind = EV_RECORDER;
    e.a = sub;
    e.b = b;
    str_set(e.text, text);
    event_post(e);
}

static bool rec_stopping()
{
    return WaitForSingleObject(g_rec.stop, 0) == WAIT_OBJECT_0;
}

// Sleeps up to `ms`, ending early when Rekodr quits (then returns true) or, with `wake`, when the main thread
// changed something.
static bool rec_sleep(u32 ms, bool wake)
{
    HANDLE h[2] = {g_rec.stop, g_rec.wake};
    return WaitForMultipleObjects(wake ? 2 : 1, h, FALSE, ms) == WAIT_OBJECT_0;
}

// The settings to record with while `game` (lower-case exe, empty = the desktop) is the game being played: the
// desktop bitrate with no game, else the game bitrate, then whatever that game sets for itself. `window` is the
// game window to record instead of the monitor (audio.cpp keeps it), 0 for the monitor.
static RecSettings rec_settings_from(const Config &c, const Paths &paths, const char *game, u64 window)
{
    RecSettings s = {};
    s.ffmpeg = c.ffmpeg;
    s.buffer_dir = c.buffer_dir;
    s.local_dir = paths.local_dir;
    s.buffer_by_size = c.buffer_by_size;
    s.buffer_hours = c.buffer_hours;
    s.buffer_gb = c.buffer_gb;
    s.min_free_gb = c.min_free_gb;
    s.output_idx = c.output_idx;
    s.fps = c.fps > 0 ? c.fps : 60;
    s.codec = c.codec;
    s.encoder = c.encoder;
    s.draw_mouse = c.draw_mouse;
    s.capture = capture_parse(c.capture.s);
    s.game = game && game[0];
    s.preset = c.preset;
    s.rate_control = c.rate_control;
    s.cq = c.cq;
    s.peak_factor = c.peak_factor > 0 ? c.peak_factor : 2;
    s.spatial_aq = c.spatial_aq;
    str_set(s.encoder_args, c.encoder_args);
    f32 rate = c.bitrate_mbps;
    i32 want_h = c.height;
    const AppInfo *g = 0;
    if (game && game[0]) {
        for (i32 i = 0; i < c.app_count && !g; i++)
            if (app_category(c, c.apps[i]) == CAT_GAME && !strcmp(c.apps[i].exe.s, game)) g = &c.apps[i];
    } else {
        rate = c.desktop_bitrate_mbps;
    }
    if (g) {
        const RecOverride &o = g->rec;
        if (o.bitrate_mbps > 0) rate = o.bitrate_mbps;
        if (o.fps > 0) s.fps = o.fps;
        if (o.height) want_h = o.height > 0 ? o.height : 0; // -1 = the monitor's own
        if (o.preset.s[0]) s.preset = o.preset;
        if (o.rate_control.s[0]) s.rate_control = o.rate_control;
        if (o.cq > 0) s.cq = o.cq;
    }
    s.bitrate_mbps = rate > 0 ? rate : 20;
    // Recording above what the monitor shows only makes bigger files: a 4K setting on a 1440p screen records
    // 1440p, and 120 fps on a 60 Hz screen records 60.
    i32 mw, mh, hz;
    if (monitor_mode(s.output_idx, &mw, &mh, &hz, &s.hmonitor)) {
        if (hz > 0 && s.fps > hz) s.fps = hz;
        if (want_h > 0 && want_h < mh) {
            s.height = want_h & ~1;
            s.width = (i32)(((i64)mw * want_h / mh + 1) & ~1);
        }
        if (window && s.capture != CAPTURE_DXGI && s.hmonitor) {
            s.hwnd = window;
            // Fitted into the monitor's size (bars where its shape differs), so a window's video is the size the
            // monitor's would be whatever size the window is.
            if (!s.width) {
                s.width = mw & ~1;
                s.height = mh & ~1;
            }
        }
    }
    return s;
}

// The vendor to record with: a forced one as is (it may fail, which is logged), "auto" the first that passed.
static i32 pick_vendor(const RecSettings &s, u8 works)
{
    i32 forced = vendor_parse(s.encoder.s);
    if (forced >= 0) return forced;
    for (i32 v = 0; v < VENDOR_COUNT; v++)
        if (works & (1 << v)) return v;
    return -1;
}

// ---------------------------------------------------------------------------------------------------------------
// ffmpeg arguments, built straight into a command line. These match encoders.rs and recorder.rs argument for
// argument, so the files come out the same.

struct Args {
    wchar_t s[8192];
};

static void add(Args *a, const char *arg)
{
    cmd_arg8(a->s, COUNT(a->s), arg);
}

static void addf(Args *a, const char *fmt, ...)
{
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof buf, fmt, args);
    va_end(args);
    add(a, buf);
}

#define ADD(...)                                                                                                  \
    do {                                                                                                          \
        const char *args_[] = {__VA_ARGS__};                                                                      \
        for (const char *x_ : args_) add(a, x_);                                                                  \
    } while (0)

// One small frame through the path a recording takes: uploaded to the capture D3D11 device (ddagrab's frames are
// BGRA textures there), then mapped like capture_filter does. So a second GPU that can't take the captured frames
// (an AMD iGPU next to an NVIDIA card) counts as missing.
static void probe_args(Args *a, i32 vendor, const char *codec)
{
    a->s[0] = 0;
    ADD("-hide_banner", "-loglevel", "error");
    if (vendor != VENDOR_CPU) ADD("-init_hw_device", "d3d11va");
    ADD("-f", "lavfi", "-i", "color=s=256x256:d=0.1", "-vf");
    if (vendor == VENDOR_CPU) add(a, "format=yuv420p");
    else addf(a, "format=bgra,hwupload%s", capture_filter(vendor));
    ADD("-frames:v", "1", "-c:v", encoder_name(vendor, codec), "-f", "null", "-");
}

// 0 (p1, fastest) ... 6 (p7, best).
static i32 preset_step(const char *preset)
{
    if (preset[0] != 'p') return 3;
    char *end = 0;
    long n = strtol(preset + 1, &end, 10);
    if (end == preset + 1 || *end) return 3;
    return (i32)(n < 1 ? 1 : n > 7 ? 7 : n) - 1;
}

// The encoder and its rate control. The keyframe interval, B-frames and the muxer are capture_args', the same for
// every vendor.
static void record_args(Args *a, i32 v, const RecSettings &s)
{
    const char *codec = s.codec.s;
    const char *rc = s.rate_control.s;
    bool av1 = !strcmp(codec, "av1");
    bool cbr = !strcmp(rc, "cbr"), cqm = !strcmp(rc, "cq");
    char bitrate[32], peak[32], cq[16];
    snprintf(bitrate, sizeof bitrate, "%lldk", (i64)llround((f64)s.bitrate_mbps * 1000.0));
    snprintf(peak, sizeof peak, "%lldk", (i64)llround((f64)s.bitrate_mbps * (cbr ? 1.0 : (f64)s.peak_factor) * 1000.0));
    snprintf(cq, sizeof cq, "%d", s.cq);
    i32 step = preset_step(s.preset.s);

    ADD("-c:v", encoder_name(v, codec));
    switch (v) {
    case VENDOR_NVENC:
        ADD("-preset", s.preset.s);
        // Constant quality: the bitrate is only a cap, so quiet scenes take far less space.
        if (cqm) ADD("-rc", "vbr", "-cq", cq, "-b:v", "0", "-maxrate", peak, "-bufsize", peak);
        else if (cbr) ADD("-rc", "cbr", "-b:v", bitrate, "-maxrate", bitrate, "-bufsize", bitrate);
        else ADD("-rc", "vbr", "-b:v", bitrate, "-maxrate", peak, "-bufsize", peak);
        if (s.spatial_aq) ADD("-spatial-aq", "1");
        break;
    case VENDOR_AMF: {
        // AMF has three speed levels (AV1 a fourth); p1-p7 spread over them.
        const char *quality = step <= 1 ? "speed" : step <= 4 ? "balanced" : (av1 && step == 6) ? "high_quality" : "quality";
        ADD("-quality", quality);
        if (cqm) {
            // No capped constant quality in AMF: fixed QP, on AV1's 0-255 scale there.
            char qp[16];
            snprintf(qp, sizeof qp, "%d", av1 ? (s.cq * 5 < 255 ? s.cq * 5 : 255) : s.cq);
            ADD("-rc", "cqp", "-qp_i", qp, "-qp_p", qp);
        } else if (cbr) {
            ADD("-rc", "cbr", "-b:v", bitrate, "-maxrate", bitrate, "-bufsize", bitrate);
        } else {
            ADD("-rc", "vbr_peak", "-b:v", bitrate, "-maxrate", peak, "-bufsize", peak);
        }
        if (s.spatial_aq) {
            if (av1) ADD("-aq_mode", "caq");
            else ADD("-vbaq", "1");
        }
        break;
    }
    case VENDOR_QSV: {
        static const char *presets[7] = {"veryfast", "faster", "fast", "medium", "slow", "slower", "veryslow"};
        ADD("-preset", presets[step]);
        // QSV picks its mode from what's set: maxrate = bitrate is CBR, a quality level with a bitrate is QVBR
        // (the closest it has to a capped constant quality), else VBR.
        if (cqm) ADD("-global_quality", cq, "-b:v", bitrate, "-maxrate", peak, "-bufsize", peak);
        else if (cbr) ADD("-b:v", bitrate, "-maxrate", bitrate, "-bufsize", bitrate);
        else ADD("-b:v", bitrate, "-maxrate", peak, "-bufsize", peak);
        break;
    }
    default: { // the CPU. Fast presets only: this runs on the cores the game wants.
        static const char *av1_presets[7] = {"12", "11", "10", "10", "9", "8", "7"};
        static const char *x26x_presets[7] = {"ultrafast", "superfast", "veryfast", "veryfast", "faster", "fast", "medium"};
        ADD("-preset", av1 ? av1_presets[step] : x26x_presets[step]);
        if (cqm) {
            // CRF capped by the peak bitrate. SVT-AV1's scale goes to 63.
            char crf[16];
            snprintf(crf, sizeof crf, "%d", av1 ? s.cq * 63 / 51 : s.cq);
            ADD("-crf", crf, "-maxrate", peak, "-bufsize", peak);
        } else if (av1) {
            // SVT-AV1 takes a peak only with CRF, and its CBR needs a low-delay mode too slow to keep up.
            ADD("-b:v", bitrate);
        } else if (cbr) {
            ADD("-b:v", bitrate, "-maxrate", bitrate, "-bufsize", bitrate);
        } else {
            ADD("-b:v", bitrate, "-maxrate", peak, "-bufsize", peak);
        }
        // No extra keyframes on scene cuts, which would make segments longer than 2 s.
        if (!strcmp(codec, "hevc")) ADD("-x265-params", "log-level=error:scenecut=0");
        else if (!av1) ADD("-sc_threshold", "0");
        break;
    }
    }
}

// Everything before the two output paths: the capture -> the encoder -> 2 s segments. Frame 0's wall-clock time is
// printed once (the `print` in setpts) and saved as the run's anchor, which lines the video up with the audio.
// Two of these compare equal exactly when a settings change doesn't need a new ffmpeg.
//
// Windows Graphics Capture (gfxcapture, `wgc`) is the default: Desktop Duplication (ddagrab) can keep running while
// handing out the same frame forever. gfxcapture scales on the GPU, and it only delivers a frame when the screen
// changed. The output side repeats frames to hold the frame rate and counts them (dup_frames in -progress), but
// only once the next frame comes: while the screen stays the same no segment is written, and the gap is filled in
// when it changes. The dup count is how a frozen capture is spotted.
static void capture_args(Args *a, i32 vendor, const RecSettings &s, bool wgc)
{
    a->s[0] = 0;
    ADD("-hide_banner", "-nostats", "-loglevel", "info", "-progress", "pipe:2", "-stats_period", "1", "-init_hw_device",
        "d3d11va", "-filter_complex");
    if (wgc) {
        char size[64] = "";
        if (s.width > 0 && s.height > 0)
            snprintf(size, sizeof size, ":width=%d:height=%d:resize_mode=scale_aspect", s.width, s.height);
        addf(a,
             "gfxcapture=%s=%llu:max_framerate=%d:capture_cursor=%d:display_border=0%s,"
             "setpts='PTS+0*if(eq(N\\,0)\\,print(RTCTIME)\\,0)'%s",
             s.hwnd ? "hwnd" : "hmonitor", (unsigned long long)(s.hwnd ? s.hwnd : s.hmonitor), s.fps, s.draw_mouse ? 1 : 0,
             size, capture_filter(vendor));
        ADD("-fps_mode", "cfr", "-r");
        addf(a, "%d", s.fps);
    } else {
        char filter[160];
        capture_filter_scaled(vendor, s.width, s.height, filter, sizeof filter);
        addf(a, "ddagrab=output_idx=%d:framerate=%d:draw_mouse=%d,setpts='PTS+0*if(eq(N\\,0)\\,print(RTCTIME)\\,0)'%s",
             s.output_idx, s.fps, s.draw_mouse ? 1 : 0, filter);
    }
    record_args(a, vendor, s);
    // No B-frames and a keyframe every segment: easy to cut, quick to scrub in an editor.
    ADD("-bf", "0", "-g");
    addf(a, "%d", s.fps * SEGMENT_SECS);
    // The player's own options last, so they win (ffmpeg takes the last of a repeated option)
    char custom[256];
    str_set(custom, s.encoder_args);
    for (char *p = custom; *p;) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        char *start = p;
        if (*p == '"') {
            start = ++p;
            while (*p && *p != '"') p++;
        } else {
            while (*p && *p != ' ' && *p != '\t') p++;
        }
        if (*p) *p++ = 0;
        add(a, start);
    }
    ADD("-f", "segment", "-segment_time");
    addf(a, "%d", SEGMENT_SECS);
    ADD("-segment_format", "matroska", "-reset_timestamps", "0", "-segment_list_type", "csv", "-segment_list");
}

// ---------------------------------------------------------------------------------------------------------------
// Encoder detection

struct DetectJob {
    Path ffmpeg;
    i32  codec;
    u8   works;
};

static DetectJob g_detect_jobs[CODEC_COUNT];

static DWORD WINAPI detect_codec_thread(void *arg)
{
    DetectJob *j = (DetectJob *)arg;
    j->works = 0;
    for (i32 v = 0; v < VENDOR_COUNT; v++) {
        Args a;
        probe_args(&a, v, CODECS[j->codec]);
        if (proc_run(j->ffmpeg.s, a.s, 15000, 0, 0) == 0) j->works |= (u8)(1 << v);
    }
    return 0;
}

// "h264 nvenc, cpu - hevc nvenc, cpu - av1 none"
static void detect_summary(const u8 *works, char *out, i32 cap)
{
    out[0] = 0;
    for (i32 c = 0; c < CODEC_COUNT; c++) {
        char part[64];
        i32 n = snprintf(part, sizeof part, "%s%s ", c ? " - " : "", CODECS[c]);
        bool any = false;
        for (i32 v = 0; v < VENDOR_COUNT; v++) {
            if (!(works[c] & (1 << v))) continue;
            n += snprintf(part + n, sizeof part - n, "%s%s", any ? ", " : "", VENDOR_KEYS[v]);
            any = true;
        }
        if (!any) snprintf(part + n, sizeof part - n, "none");
        strncat_s(out, cap, part, _TRUNCATE);
    }
}

// Until ffmpeg is found (being downloaded by ffget, or a path still to be set in Options) it is looked for every
// 5 s, and the encoders are tested as soon as it is there. A GPU driver can still be loading when Windows starts
// Rekodr, so finding no GPU encoder is then retried 4 times, 30 s apart. The recorder thread switches over when
// a retry finds one.
static DWORD WINAPI detect_thread(void *)
{
    char last[256] = "";
    bool posted = false;
    for (i32 attempt = 0;;) {
        AcquireSRWLockShared(&g_rec.lock);
        Path configured = g_rec.settings.ffmpeg;
        ReleaseSRWLockShared(&g_rec.lock);

        u8 works[CODEC_COUNT] = {};
        Path ffmpeg;
        u64 took = GetTickCount64();
        bool have = ffmpeg_find(configured, &ffmpeg);
        if (!have && posted) { // nothing new to say: wait for it
            if (rec_sleep(5000, false)) break;
            continue;
        }
        if (have) {
            HANDLE threads[CODEC_COUNT];
            for (i32 c = 0; c < CODEC_COUNT; c++) {
                g_detect_jobs[c].ffmpeg = ffmpeg;
                g_detect_jobs[c].codec = c;
                threads[c] = thread_start(detect_codec_thread, &g_detect_jobs[c]);
            }
            WaitForMultipleObjects(CODEC_COUNT, threads, TRUE, INFINITE);
            for (i32 c = 0; c < CODEC_COUNT; c++) {
                CloseHandle(threads[c]);
                works[c] = g_detect_jobs[c].works;
            }
        }
        char summary[256];
        detect_summary(works, summary, sizeof summary);
        if (strcmp(summary, last)) {
            log_line("encoders (%.1fs): %s", (GetTickCount64() - took) / 1000.0, summary);
            str_set(last, summary);
        }

        AcquireSRWLockExclusive(&g_rec.lock);
        memcpy(g_rec.works, works, sizeof works);
        g_rec.detected = true;
        g_rec.generation++;
        ReleaseSRWLockExclusive(&g_rec.lock);
        SetEvent(g_rec.detect_done);
        SetEvent(g_rec.wake);
        i64 packed = 0;
        for (i32 c = 0; c < CODEC_COUNT; c++) packed |= (i64)works[c] << (8 * c);
        rec_post(REC_DETECTED, packed, summary);
        posted = true;
        if (!have) {
            if (rec_sleep(5000, false)) break;
            continue;
        }

        bool gpu = false;
        for (i32 c = 0; c < CODEC_COUNT; c++) gpu |= (works[c] & ~(1 << VENDOR_CPU)) != 0;
        if (gpu || ++attempt > 4 || rec_sleep(30000, false)) break;
    }
    return 0;
}

// ---------------------------------------------------------------------------------------------------------------
// The capture ffmpeg

// Reads ffmpeg's stderr for one run: saves the anchor, writes the rest (minus per-segment chatter) to
// ffmpeg-video.log and keeps the last line for the error message.
struct StderrReader {
    HANDLE pipe;
    Path   run_dir;
    Path   log;
    char   last[512];
    volatile i64 segment_tick; // GetTickCount64 when ffmpeg last opened a segment (the stall watchdog reads it)
    volatile i64 frames, dups; // -progress totals: frames written, and how many of them were repeats
    volatile i64 drops;        // -progress drop_frames: frames ffmpeg dropped
    volatile i64 out_us;       // -progress out_time_us: how much video has been written
    volatile i64 progress_tick; // GetTickCount64 of the last -progress line: ffmpeg is still running its loop
};

// A -progress line ("frame=123", "dup_frames=4", "progress=continue"): a lower-case key, '=', no spaces.
static bool rec_progress_line(const char *line)
{
    if (line[0] < 'a' || line[0] > 'z' || strchr(line, ' ')) return false;
    const char *eq = strchr(line, '=');
    return eq != 0;
}

static StderrReader g_reader;

static DWORD WINAPI stderr_thread(void *arg)
{
    StderrReader *r = (StderrReader *)arg;
    r->last[0] = 0;
    FILE *log = _wfopen(r->log.s, L"ab");
    char line[2048];
    while (pipe_read_line(r->pipe, line, sizeof line)) {
        if (starts_with(line, "[Eval @")) {
            // "[Eval @ 0000...] 1794748014750753.000000": RTCTIME of frame 0, us since 1970
            const char *v = strrchr(line, ' ');
            f64 us = v ? strtod(v + 1, 0) : 0;
            if (us > 0) {
                char text[32];
                i32 n = snprintf(text, sizeof text, "%lld", (i64)llround(us));
                Path anchor = path_join(r->run_dir, L"anchor.txt");
                HANDLE f = CreateFileW(anchor.s, GENERIC_WRITE, FILE_SHARE_READ, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
                if (f != INVALID_HANDLE_VALUE) {
                    DWORD written;
                    WriteFile(f, text, (DWORD)n, &written, 0);
                    CloseHandle(f);
                } else {
                    log_line("couldn't write the video anchor (%d)", (int)GetLastError());
                }
            }
            continue;
        }
        if (strstr(line, "Opening '")) {
            r->segment_tick = (i64)GetTickCount64();
            continue;
        }
        if (rec_progress_line(line)) {
            r->progress_tick = (i64)GetTickCount64();
            if (starts_with(line, "frame=")) r->frames = _atoi64(line + 6);
            else if (starts_with(line, "dup_frames=")) r->dups = _atoi64(line + 11);
            else if (starts_with(line, "drop_frames=")) r->drops = _atoi64(line + 12);
            else if (starts_with(line, "out_time_us=")) r->out_us = _atoi64(line + 12);
            continue;
        }
        if (starts_with(line, "  ")) continue;
        if (log) fprintf(log, "%s\n", line);
        str_set(r->last, line);
    }
    if (log) fclose(log);
    return 0;
}

// Asks ffmpeg to finish the current segment and exit; kills it if it doesn't within 5 s.
static void capture_stop(Proc *p)
{
    if (p->in) {
        DWORD written;
        WriteFile(p->in, "q", 1, &written, 0);
        proc_close_stdin(p);
    }
    if (proc_wait(p, 5000) < 0) {
        log_line("video ffmpeg didn't stop when asked; killing it");
        proc_kill(p);
        proc_wait(p, 2000);
    }
}

static void run_name(wchar_t *out, i32 cap)
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    swprintf(out, cap, L"%04d%02d%02d_%02d%02d%02d_%03d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
             t.wMilliseconds);
}

struct RecSnapshot {
    RecSettings s;
    u32         generation;
    bool        paused;
    bool        detected;
    u8          works;
};

static void rec_snapshot(RecSnapshot *out)
{
    AcquireSRWLockShared(&g_rec.lock);
    out->s = g_rec.settings;
    out->generation = g_rec.generation;
    out->paused = g_rec.paused;
    out->detected = g_rec.detected;
    out->works = g_rec.works[codec_index(g_rec.settings.codec.s)];
    ReleaseSRWLockShared(&g_rec.lock);
}

// Runs until Rekodr quits. Only changes of state are logged, so hours on the lock screen don't flood the log.
static DWORD WINAPI recorder_thread(void *)
{
    static RecSnapshot snap, now; // big (paths); static keeps them off the thread's stack
    static Args base, cmd, base_now;
    bool healthy = true;
    char last_encoder[64] = "";
    u32 backoff = 2000;
    bool log_cleared = false;
    // Automatic capture uses Windows Graphics Capture until it fails to start twice running, or freezes twice
    // within 2 minutes; then Desktop Duplication until the capture setting changes.
    i32 wgc_fails = 0;
    i32 last_capture = -1;
    u64 last_frozen = 0;
    auto use_wgc = [&](const RecSettings &rs) {
        if (rs.capture == CAPTURE_DXGI || !rs.hmonitor) return false;
        return rs.hwnd || rs.capture == CAPTURE_WGC || wgc_fails < 2; // only it can record a window
    };
    // A window that couldn't be recorded (it didn't start, or froze) is left alone: the monitor is recorded until
    // another window is asked for.
    u64 bad_hwnd = 0;
    auto without_bad_window = [&](RecSettings rs) {
        if (!rs.hwnd) bad_hwnd = 0;
        else if (rs.hwnd == bad_hwnd || !IsWindow((HWND)(intptr_t)rs.hwnd)) rs.hwnd = 0;
        return rs;
    };

    auto fail = [&](const char *msg) {
        if (!healthy) return;
        log_line("%s", msg);
        rec_post(REC_ERROR, 0, msg);
        healthy = false;
    };

    while (!rec_stopping()) {
        rec_snapshot(&snap);
        if (snap.paused) {
            rec_sleep(1000, true);
            continue;
        }
        if (!snap.detected) {
            HANDLE h[2] = {g_rec.stop, g_rec.detect_done};
            WaitForMultipleObjects(2, h, FALSE, INFINITE);
            continue;
        }
        const RecSettings s = without_bad_window(snap.s);
        if (!log_cleared) {
            // A fresh ffmpeg-video.log each start.
            Path log = path_join(s.local_dir, L"ffmpeg-video.log");
            FILE *f = _wfopen(log.s, L"wb");
            if (f) fclose(f);
            log_cleared = true;
        }
        Path ffmpeg;
        if (!ffmpeg_find(s.ffmpeg, &ffmpeg)) {
            fail("waiting for ffmpeg (" APP_NAME " is getting it, or set its path in Options)");
            rec_sleep(5000, true);
            continue;
        }
        i32 vendor = pick_vendor(s, snap.works);
        if (vendor < 0) {
            char msg[128];
            snprintf(msg, sizeof msg, "no working %s encoder found (see the encoders line in the log)", s.codec.s);
            fail(msg);
            rec_sleep(5000, true);
            continue;
        }
        const char *name = encoder_name(vendor, s.codec.s);
        if (strcmp(name, last_encoder)) {
            if (vendor == VENDOR_CPU) log_line("encoding with %s on the CPU (no GPU encoder): this costs game frames", name);
            else log_line("encoding with %s", name);
            str_set(last_encoder, name);
        }

        // A new run folder
        wchar_t run[64];
        run_name(run, COUNT(run));
        Path video_dir = path_join(s.buffer_dir, L"video");
        Path run_dir = path_join(video_dir, run);
        i32 made = SHCreateDirectoryExW(0, run_dir.s, 0);
        if (made != ERROR_SUCCESS && made != ERROR_ALREADY_EXISTS) {
            char msg[MAX_PATH * 3 + 64], dir8[MAX_PATH * 3];
            wide_to_utf8(run_dir.s, dir8, sizeof dir8);
            snprintf(msg, sizeof msg, "couldn't create %s (%d)", dir8, made);
            fail(msg);
            rec_sleep(backoff, true);
            continue;
        }

        if (s.capture != last_capture) {
            last_capture = s.capture;
            wgc_fails = 0;
        }
        bool wgc = use_wgc(s);
        capture_args(&base, vendor, s, wgc);
        cmd = base;
        Path list = path_join(run_dir, L"list.csv");
        Path pattern = path_join(run_dir, L"v_%05d.mkv");
        cmd_arg(cmd.s, COUNT(cmd.s), list.s);
        cmd_arg(cmd.s, COUNT(cmd.s), pattern.s);
        Proc p;
        if (!proc_start(ffmpeg.s, cmd.s, PROC_STDIN | PROC_STDERR, &p)) {
            fail("the video ffmpeg couldn't start");
            rec_sleep(backoff, true);
            continue;
        }
        AcquireSRWLockExclusive(&g_rec.lock);
        wcscpy_s(g_rec.run, run);
        ReleaseSRWLockExclusive(&g_rec.lock);
        g_reader.pipe = p.err;
        g_reader.run_dir = run_dir;
        g_reader.log = path_join(s.local_dir, L"ffmpeg-video.log");
        g_reader.segment_tick = 0;
        g_reader.frames = g_reader.dups = g_reader.drops = g_reader.progress_tick = g_reader.out_us = 0;
        HANDLE reader = thread_start(stderr_thread, &g_reader);

        u64 started = GetTickCount64();
        bool announced = false;
        bool frozen = false, idle = false, renewed = false;
        u64 window_tick = started;
        i64 window_frames = 0, window_dups = 0, window_drops = 0, window_out = 0;
        // Strain: while a game is played, video should be written as fast as time passes. Three 10 s windows
        // in a row under 92% of real time means the encoder is behind; three good ones clear it.
        i32 slow_windows = 0, good_windows = 0;
        bool strained = false;
        i32 code = -1;
        u32 generation = snap.generation;
        for (;;) {
            code = proc_wait(&p, 0);
            if (code >= 0) break;
            rec_snapshot(&now);
            bool restart = false;
            if (now.generation != generation) {
                generation = now.generation;
                // A detection retry can find a GPU whose driver wasn't ready at start, which also lands here.
                i32 v2 = pick_vendor(now.s, now.works);
                const RecSettings ns = without_bad_window(now.s);
                if (v2 >= 0) capture_args(&base_now, v2, ns, use_wgc(ns));
                restart = v2 != vendor || wcscmp(base_now.s, base.s) || wcscmp(now.s.buffer_dir.s, s.buffer_dir.s) ||
                          wcscmp(now.s.ffmpeg.s, s.ffmpeg.s);
                if (restart) log_line("video settings changed: restarting the capture");
            }
            if (rec_stopping() || now.paused || restart) {
                capture_stop(&p);
                break;
            }
            // ffmpeg can hit an encoder or capture error and stay running without writing anything ("Generic
            // error in an external library"). No new segment for 15 s (they come every 2 s) means it's stuck.
            i64 last_seg = g_reader.segment_tick;
            u64 since = last_seg ? (u64)last_seg : started;
            // Windows Graphics Capture on a screen that isn't changing writes nothing either. That's an idle screen,
            // not a stuck ffmpeg, when ffmpeg still reports progress, no game is played and nobody has touched the
            // mouse or keyboard since the last segment: wait, and the repeats fill the gap once the screen changes.
            // Up to a minute, so the encoder never has a long stretch of repeats to catch up on.
            LASTINPUTINFO input = {sizeof input};
            bool still = wgc && !now.s.game && GetTickCount64() - (u64)g_reader.progress_tick < 3000 &&
                         GetLastInputInfo(&input) && (i32)(input.dwTime - (DWORD)since) <= 0;
            u64 quiet = GetTickCount64() - since;
            if (still && quiet > 15000 && quiet <= 60000) {
                if (!idle) log_line("screen unchanged: the capture waits for it to change");
                idle = true;
            } else if (still && quiet > 60000) {
                capture_stop(&p); // a fresh capture starts the next stretch with the screen as it is
                renewed = true;
                break;
            } else if (quiet > 15000) {
                log_line("video capture stalled: no new segment for 15 s (%s); restarting it", g_reader.last);
                capture_stop(&p);
                code = 2; // handled like ffmpeg stopping by itself: retry with backoff
                break;
            }
            // Frozen: a game is being played but every frame of the last 10 s repeated the one before. A game redraws
            // all the time, so the capture lost it (Windows Graphics Capture only hands out changed frames; the
            // output side fills in repeats). A new capture picks it up again.
            u64 tick = GetTickCount64();
            if (tick - window_tick >= 10000) {
                i64 f = g_reader.frames, d = g_reader.dups, dr = g_reader.drops, o = g_reader.out_us;
                bool same = f > window_frames && f - window_frames <= d - window_dups;
                f64 pace = window_out && o > window_out ? (f64)(o - window_out) / ((f64)(tick - window_tick) * 1000) : 0;
                if (announced && now.s.game && pace > 0) {
                    // The window for the quality warning (quality.cpp)
                    char window[96];
                    snprintf(window, sizeof window, "%lld|%lld|%lld|%lld", (long long)(tick - window_tick),
                             (long long)((o - window_out) / 1000), (long long)(f - window_frames), (long long)(dr - window_drops));
                    rec_post(REC_PACE, 0, window);
                }
                window_out = o;
                window_drops = dr;
                if (announced && now.s.game && pace > 0) {
                    if (pace < 0.92) { slow_windows++; good_windows = 0; }
                    else { good_windows++; slow_windows = 0; }
                    if (!strained && slow_windows >= 3) {
                        strained = true;
                        char tip[200];
                        snprintf(tip, sizeof tip, "The PC can't keep up: video is written at %d%% of real time. "
                                 "Try a lower frame rate or size in Options > Video", (int)(pace * 100));
                        log_line("recording strained: pace %.2f", pace);
                        rec_post(REC_STRAIN, 0, tip);
                    } else if (strained && good_windows >= 3) {
                        strained = false;
                        log_line("recording keeps up again");
                        rec_post(REC_STRAIN, 0, "");
                    }
                }
                window_tick = tick;
                window_frames = f;
                window_dups = d;
                if (wgc && announced && now.s.game && same) {
                    bool again = last_frozen && tick - last_frozen < 120000;
                    last_frozen = tick;
                    if (again && now.s.capture == CAPTURE_AUTO) {
                        wgc_fails = 2;
                        log_line("video frozen again while a game is played: switching to Desktop Duplication");
                    } else {
                        log_line("video frozen: no new frame for 10 s while a game is played; restarting the capture");
                    }
                    capture_stop(&p);
                    frozen = true;
                    break;
                }
            }
            if (!announced && GetTickCount64() - started > 4000) {
                // Alive for 4 s: ddagrab got the desktop and the encoder took the frames.
                log_line(healthy ? "recording video" : "recording video again");
                char text[160], run8[64];
                wide_to_utf8(run, run8, sizeof run8);
                snprintf(text, sizeof text, "%s|%s|%s", name, run8, wgc ? "wgc" : "dxgi");
                rec_post(REC_STARTED, vendor, text);
                if (!healthy) rec_post(REC_ERROR, 0, "");
                announced = true;
                healthy = true;
            }
            rec_sleep(200, true);
        }
        u64 lived = GetTickCount64() - started;
        WaitForSingleObject(reader, INFINITE); // the pipe ends with ffmpeg
        CloseHandle(reader);
        proc_close(&p);
        AcquireSRWLockExclusive(&g_rec.lock);
        g_rec.run[0] = 0;
        ReleaseSRWLockExclusive(&g_rec.lock);
        if (announced) rec_post(REC_STOPPED, 0, "");
        if (strained) rec_post(REC_STRAIN, 0, "");
        if (s.hwnd && code >= 0 && (!announced || frozen)) {
            bad_hwnd = s.hwnd;
            log_line("that game's window couldn't be recorded (%s): recording the monitor", frozen ? "no new frames" : g_reader.last);
        } else if (wgc && s.capture == CAPTURE_AUTO && !announced && !frozen && code >= 0) {
            // Didn't get going: an ffmpeg without gfxcapture, or a Windows without Graphics Capture.
            if (++wgc_fails == 2)
                log_line("Windows Graphics Capture didn't start twice (%s): using Desktop Duplication", g_reader.last);
        } else if (wgc && announced && !frozen) {
            wgc_fails = 0;
        }

        if (frozen || renewed) {
            backoff = 2000;
        } else if (code >= 0) {
            // It stopped by itself. Retry soon (unlocking should record again within seconds), backing off while
            // it keeps failing straight away.
            if (lived > 10000) backoff = 2000;
            char msg[700];
            snprintf(msg, sizeof msg, "video capture stopped (exit code %d): %s (retrying, normal while locked)", code,
                     g_reader.last);
            fail(msg);
            rec_sleep(backoff, true);
            backoff = backoff * 2 > 8000 ? 8000 : backoff * 2;
        } else {
            backoff = 2000;
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------------------------------------------
// Pruning. Every finished file in the buffer, video and audio alike, is placed on the wall clock by its end time;
// the oldest go first. A file is finished when it's in its folder's list.csv, or, for files ffmpeg never listed
// (it died mid-segment), when it hasn't been written to for 5 minutes. The newest minute is never touched.

enum { PRUNE_MAX_DIRS = 8192, PRUNE_MAX_FILES = 1 << 18, PRUNE_MAX_LIST = 1 << 18 };

struct PruneDir {
    wchar_t name[128];
    bool    video;
    bool    current;  // the run being written
    i64     idle_us;  // when list.csv (or the folder) last changed
    i32     media;    // media files in it
    i32     deleted;
};

struct PruneFile {
    i64  start_us, end_us;
    i64  size;
    i32  dir;
    i32  num;    // v_<num>.mkv / a_<num>.mka
    bool listed; // finished for certain
};

struct ListEntry {
    i32 num;
    f64 start, end;
};

struct Pruner {
    PruneDir  dirs[PRUNE_MAX_DIRS];
    i32       dir_count;
    PruneFile files[PRUNE_MAX_FILES];
    i32       file_count;
    ListEntry list[PRUNE_MAX_LIST];
    i32       list_count;
    bool      warned_full;
    bool      freeing;      // the last pass deleted for free space (logged once per episode)
    u32       passes;
};

static Pruner g_prune;

// "v_00012.mkv" -> 12, or -1 when it isn't one of ours. Only names that match are ever deleted.
static i32 media_num(const char *name, bool video)
{
    if (name[0] != (video ? 'v' : 'a') || name[1] != '_' || !isdigit((u8)name[2])) return -1;
    char *end = 0;
    long n = strtol(name + 2, &end, 10);
    if (strcmp(end, video ? ".mkv" : ".mka")) return -1;
    return (i32)n;
}

static i32 media_num_w(const wchar_t *name, bool video)
{
    char n8[MAX_PATH];
    wide_to_utf8(name, n8, sizeof n8);
    return media_num(n8, video);
}

static int list_cmp(const void *a, const void *b)
{
    i32 x = ((const ListEntry *)a)->num, y = ((const ListEntry *)b)->num;
    return x < y ? -1 : x > y;
}

static int file_cmp(const void *a, const void *b)
{
    i64 x = ((const PruneFile *)a)->end_us, y = ((const PruneFile *)b)->end_us;
    return x < y ? -1 : x > y;
}

// list.csv -> g_prune.list, sorted by segment number. Lines are "name,start,end" (the name may be quoted).
static void read_list(const Path &dir, bool video)
{
    g_prune.list_count = 0;
    Path p = path_join(dir, L"list.csv");
    char *text = read_file(p.s, 0);
    if (!text) return;
    for (char *line = text; *line;) {
        char *next = strchr(line, '\n');
        if (next) *next++ = 0;
        else next = line + strlen(line);
        char *c2 = strrchr(line, ',');
        if (c2) {
            *c2 = 0;
            char *c1 = strrchr(line, ',');
            if (c1) {
                *c1 = 0;
                char *name = trim(line);
                if (*name == '"') name++;
                size_t n = strlen(name);
                if (n && name[n - 1] == '"') name[n - 1] = 0;
                i32 num = media_num(name, video);
                if (num >= 0 && g_prune.list_count < PRUNE_MAX_LIST) {
                    ListEntry *e = &g_prune.list[g_prune.list_count++];
                    e->num = num;
                    e->start = strtod(c1 + 1, 0);
                    e->end = strtod(c2 + 1, 0);
                }
            }
        }
        line = next;
    }
    free(text);
    qsort(g_prune.list, g_prune.list_count, sizeof(ListEntry), list_cmp);
}

static const ListEntry *list_find(i32 num)
{
    i32 lo = 0, hi = g_prune.list_count - 1;
    while (lo <= hi) {
        i32 mid = (lo + hi) / 2;
        i32 n = g_prune.list[mid].num;
        if (n == num) return &g_prune.list[mid];
        if (n < num) lo = mid + 1;
        else hi = mid - 1;
    }
    return 0;
}

// The time the folder's footage is measured from: anchor.txt for a video run, meta.json's start_us for an audio
// stream. 0 if there's none yet.
static i64 dir_origin(const Path &dir, bool video)
{
    Path p = path_join(dir, video ? L"anchor.txt" : L"meta.json");
    char *text = read_file(p.s, 0);
    if (!text) return 0;
    i64 us = 0;
    if (video) {
        us = _strtoi64(text, 0, 10);
    } else {
        const char *k = strstr(text, "\"start_us\"");
        if (k) k = strchr(k, ':');
        if (k) us = _strtoi64(k + 1, 0, 10);
    }
    free(text);
    return us;
}

static void scan_dir(const Path &parent, const WIN32_FIND_DATAW &fd, bool video, const wchar_t *current_run)
{
    if (g_prune.dir_count == PRUNE_MAX_DIRS || wcslen(fd.cFileName) >= COUNT(g_prune.dirs[0].name)) {
        if (!g_prune.warned_full) log_line("prune: too many buffer folders; some are left for the next pass");
        g_prune.warned_full = true;
        return;
    }
    i32 di = g_prune.dir_count++;
    PruneDir *d = &g_prune.dirs[di];
    *d = {};
    wcscpy_s(d->name, fd.cFileName);
    d->video = video;
    d->current = video && current_run[0] && !wcscmp(fd.cFileName, current_run);
    d->idle_us = filetime_us(fd.ftLastWriteTime);

    Path dir = path_join(parent, fd.cFileName);
    i64 origin = dir_origin(dir, video);
    read_list(dir, video);
    Path list = path_join(dir, L"list.csv");
    WIN32_FILE_ATTRIBUTE_DATA la;
    if (GetFileAttributesExW(list.s, GetFileExInfoStandard, &la)) d->idle_us = filetime_us(la.ftLastWriteTime);

    Path pattern = path_join(dir, video ? L"*.mkv" : L"*.mka");
    WIN32_FIND_DATAW f;
    HANDLE h = FindFirstFileW(pattern.s, &f);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        i32 num = media_num_w(f.cFileName, video);
        if (num < 0) continue;
        d->media++;
        if (g_prune.file_count == PRUNE_MAX_FILES) {
            if (!g_prune.warned_full) log_line("prune: too many buffer files; some are left for the next pass");
            g_prune.warned_full = true;
            continue;
        }
        PruneFile *pf = &g_prune.files[g_prune.file_count++];
        *pf = {};
        pf->dir = di;
        pf->num = num;
        pf->size = ((i64)f.nFileSizeHigh << 32) | f.nFileSizeLow;
        const ListEntry *e = origin ? list_find(num) : 0;
        if (e) {
            pf->listed = true;
            pf->start_us = origin + llround(e->start * 1e6);
            pf->end_us = origin + llround(e->end * 1e6);
        } else {
            pf->start_us = pf->end_us = filetime_us(f.ftLastWriteTime);
        }
    } while (FindNextFileW(h, &f));
    FindClose(h);
}

static void scan_tree(const Path &root, bool video, const wchar_t *current_run)
{
    Path pattern = path_join(root, L"*");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern.s, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
            continue;
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        scan_dir(root, fd, video, current_run);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static Path prune_dir_path(const RecSettings &s, const PruneDir &d)
{
    Path parent = path_join(s.buffer_dir, d.video ? L"video" : L"audio");
    return path_join(parent, d.name);
}

// Drops a log's lines older than the buffer (activity.tsv, focus.tsv), keeping the one that says what was on at
// its start.
static void prune_activity(const RecSettings &s, i64 oldest_us, const wchar_t *name)
{
    Path path = path_join(s.buffer_dir, name);
    AcquireSRWLockExclusive(&g_activity_lock);
    i64 size = 0;
    char *text = read_file(path.s, &size);
    if (!text) { ReleaseSRWLockExclusive(&g_activity_lock); return; }
    // The start of the last line at or before oldest_us
    char *keep = 0;
    i32 index = 0, keep_index = 0;
    for (char *line = text; *line; index++) {
        char *next = strchr(line, '\n');
        next = next ? next + 1 : line + strlen(line);
        char *tab = strchr(line, '\t');
        if (tab && tab < next) {
            i64 t = _strtoi64(line, 0, 10);
            if (t <= oldest_us) { keep = line; keep_index = index; }
        }
        line = next;
    }
    if (keep && keep_index > 0) {
        // Through a temp file, so a crash mid-write never leaves half a log.
        Path tmp = path;
        wcscat_s(tmp.s, L".tmp");
        HANDLE f = CreateFileW(tmp.s, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
        if (f != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            DWORD n = (DWORD)(text + size - keep);
            BOOL ok = WriteFile(f, keep, n, &written, 0) && written == n;
            CloseHandle(f);
            if (!ok || !MoveFileExW(tmp.s, path.s, MOVEFILE_REPLACE_EXISTING)) {
                log_line("prune: couldn't rewrite %ls (%d)", name, (int)GetLastError());
                DeleteFileW(tmp.s);
            }
        }
    }
    free(text);
    ReleaseSRWLockExclusive(&g_activity_lock);
}

static void prune_pass()
{
    static RecSettings s;
    wchar_t current_run[64];
    AcquireSRWLockShared(&g_rec.lock);
    s = g_rec.settings;
    wcscpy_s(current_run, g_rec.run);
    ReleaseSRWLockShared(&g_rec.lock);
    if (!s.buffer_dir.s[0]) return;

    g_prune.dir_count = 0;
    g_prune.file_count = 0;
    scan_tree(path_join(s.buffer_dir, L"video"), true, current_run);
    scan_tree(path_join(s.buffer_dir, L"audio"), false, current_run);
    qsort(g_prune.files, g_prune.file_count, sizeof(PruneFile), file_cmp);

    i64 now = now_us();
    // One limit: by length, or by size (then footage of any age stays while it fits).
    f64 hours = s.buffer_hours > 0 ? s.buffer_hours : 24;
    i64 cutoff = s.buffer_by_size ? INT64_MIN : now - (i64)(hours * 3600e6);
    i64 over = 0;
    if (s.buffer_by_size) {
        i64 total = 0;
        for (i32 i = 0; i < g_prune.file_count; i++) total += g_prune.files[i].size;
        i64 limit = (i64)((f64)(s.buffer_gb > 0 ? s.buffer_gb : 100) * 1e9);
        over = total > limit ? total - limit : 0;
    }
    i64 keep_after = now - 60 * 1000000LL;
    i64 settled = now - 300 * 1000000LL; // unlisted files untouched this long aren't being written
    ULARGE_INTEGER free_bytes = {};
    bool have_free = GetDiskFreeSpaceExW(s.buffer_dir.s, &free_bytes, 0, 0) != 0;
    i64 floor = (i64)((f64)s.min_free_gb * 1e9);
    i64 need = have_free && floor > (i64)free_bytes.QuadPart ? floor - (i64)free_bytes.QuadPart : 0;

    i64 freed = 0, freed_for_space = 0;
    for (i32 i = 0; i < g_prune.file_count; i++) {
        PruneFile *f = &g_prune.files[i];
        if (f->end_us > keep_after) break;
        PruneDir *d = &g_prune.dirs[f->dir];
        bool old = f->end_us < cutoff || freed < over;
        bool short_on_space = freed < need;
        if (!old && !short_on_space) break; // sorted by end: nothing later is due either
        if (!f->listed && (d->current || f->end_us > settled)) continue; // may be the segment being written
        Path dir = prune_dir_path(s, *d);
        wchar_t name[32];
        swprintf(name, COUNT(name), d->video ? L"v_%05d.mkv" : L"a_%05d.mka", f->num);
        Path p = path_join(dir, name);
        if (DeleteFileW(p.s)) {
            freed += f->size;
            if (!old) freed_for_space += f->size;
            d->deleted++;
            f->size = -1; // gone
        }
    }
    if (freed_for_space && !g_prune.freeing) {
        log_line("prune: under %.0f GB free, deleting the oldest footage early (%.1f GB so far)", s.min_free_gb,
                 freed_for_space / 1e9);
    }
    g_prune.freeing = freed_for_space > 0;

    // Folders with no media left whose list hasn't changed for 5 minutes are finished.
    for (i32 i = 0; i < g_prune.dir_count; i++) {
        PruneDir *d = &g_prune.dirs[i];
        if (d->current || d->media - d->deleted > 0 || d->idle_us > settled) continue;
        Path dir = prune_dir_path(s, *d);
        Path pattern = path_join(dir, L"*");
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(pattern.s, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                Path p = path_join(dir, fd.cFileName);
                DeleteFileW(p.s);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        RemoveDirectoryW(dir.s); // fails harmlessly if something's still in it
    }

    // What's left, for the UI and the activity log
    i64 oldest = 0, bytes = 0;
    for (i32 i = 0; i < g_prune.file_count; i++) {
        const PruneFile &f = g_prune.files[i];
        if (f.size < 0) continue;
        bytes += f.size;
        if (f.listed && g_prune.dirs[f.dir].video && (!oldest || f.start_us < oldest)) oldest = f.start_us;
    }
    if (g_prune.passes++ % 120 == 0) { // at start, then hourly
        prune_activity(s, oldest ? oldest : cutoff, L"activity.tsv");
        prune_activity(s, oldest ? oldest : cutoff, L"focus.tsv");
    }

    if (have_free) GetDiskFreeSpaceExW(s.buffer_dir.s, &free_bytes, 0, 0);
    char text[64];
    snprintf(text, sizeof text, "%lld %lld", bytes, have_free ? (i64)free_bytes.QuadPart : -1LL);
    rec_post(REC_BUFFER, oldest, text);
}

static DWORD WINAPI prune_thread(void *)
{
    do {
        prune_pass();
    } while (!rec_sleep(30000, false));
    return 0;
}

// ---------------------------------------------------------------------------------------------------------------
// The main thread's side

static void recorder_start()
{
    g_rec.wake = CreateEventW(0, FALSE, FALSE, 0);
    g_rec.stop = CreateEventW(0, TRUE, FALSE, 0);
    g_rec.detect_done = CreateEventW(0, TRUE, FALSE, 0);
    CHECK(g_rec.wake && g_rec.stop && g_rec.detect_done, "CreateEvent failed (%d)", (int)GetLastError());
    AcquireSRWLockExclusive(&g_rec.lock);
    g_rec.settings = rec_settings_from(g_app.config, g_app.paths, g_app.audio.game.s, g_app.audio.window);
    g_rec.paused = g_app.recorder.paused;
    ReleaseSRWLockExclusive(&g_rec.lock);

    char dir8[MAX_PATH * 3];
    wide_to_utf8(g_app.config.buffer_dir.s, dir8, sizeof dir8);
    if (g_app.config.buffer_by_size)
        log_line("buffer %s: keeping %g GB, at least %g GB free", dir8, g_app.config.buffer_gb, g_app.config.min_free_gb);
    else
        log_line("buffer %s: keeping %g h, at least %g GB free", dir8, g_app.config.buffer_hours, g_app.config.min_free_gb);

    CloseHandle(thread_start(detect_thread, 0)); // not waited for at exit: the job object ends its ffmpegs
    g_rec.recorder_thread = thread_start(recorder_thread, 0);
    g_rec.prune_thread = thread_start(prune_thread, 0);
}

// Stops the capture cleanly (ffmpeg finishes its segment) and the pruner.
static void recorder_stop()
{
    if (!g_rec.stop) return;
    SetEvent(g_rec.stop);
    if (g_rec.recorder_thread && WaitForSingleObject(g_rec.recorder_thread, 10000) != WAIT_OBJECT_0)
        log_line("the recorder thread didn't stop in time");
    if (g_rec.prune_thread) WaitForSingleObject(g_rec.prune_thread, 5000);
}

// Pause stops the capture; resume starts a new run.
static void recorder_set_paused(bool paused)
{
    if (g_app.recorder.paused != paused) log_line(paused ? "paused" : "resumed");
    g_app.recorder.paused = paused;
    AcquireSRWLockExclusive(&g_rec.lock);
    g_rec.paused = paused;
    ReleaseSRWLockExclusive(&g_rec.lock);
    if (g_rec.wake) SetEvent(g_rec.wake);
}

// Call after changing Config, and when the game being played changes: the workers pick up the new settings, and
// the capture restarts if any change needs a new ffmpeg (monitor, fps, codec, encoder, bitrate and rate control,
// mouse, buffer folder, ffmpeg path). A game with its own settings starts a new run when it's played.
static void recorder_settings_changed()
{
    AcquireSRWLockExclusive(&g_rec.lock);
    g_rec.settings = rec_settings_from(g_app.config, g_app.paths, g_app.audio.game.s, g_app.audio.window);
    g_rec.generation++;
    ReleaseSRWLockExclusive(&g_rec.lock);
    if (g_rec.wake) SetEvent(g_rec.wake);
}

static void recorder_on_event(const Event &e)
{
    Recorder &r = g_app.recorder;
    switch (e.a) {
    case REC_DETECTED:
        r.encoders_known = true;
        for (i32 c = 0; c < CODEC_COUNT; c++) r.encoders_ok[c] = (u8)(e.b >> (8 * c));
        break;
    case REC_STARTED: {
        r.recording = true;
        r.vendor = (u8)e.b;
        r.recording_since_us = now_us();
        char text[512];
        str_set(text, e.text);
        char *bar = strchr(text, '|');
        if (bar) *bar++ = 0;
        char *bar2 = bar ? strchr(bar, '|') : 0;
        if (bar2) *bar2++ = 0;
        str_set(r.encoder_used.s, text);
        str_set(r.run.s, bar ? bar : "");
        r.wgc = bar2 && !strcmp(bar2, "wgc");
        break;
    }
    case REC_STOPPED:
        r.recording = false;
        r.encoder_used.s[0] = 0;
        r.run.s[0] = 0;
        break;
    case REC_ERROR: str_set(r.last_error.s, e.text); break;
    case REC_STRAIN: str_set(r.strain.s, e.text); break;
    case REC_PACE: quality_pace(GetTickCount64(), e.text); break;
    case REC_BUFFER:
        r.oldest_us = e.b;
        sscanf_s(e.text, "%lld %lld", &r.buffer_bytes, &r.free_bytes);
        break;
    default: break;
    }
}
