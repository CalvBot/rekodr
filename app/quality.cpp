// quality.cpp: which recording settings are more than this PC keeps up with, and why, for the amber lines in
// Options > Recording: frames the recorder actually lost while games were played, and estimates from the
// hardware. Nothing is blocked; the player decides.
//
// The limits are rough rates, in pixels a second (height * 16/9 * height * fps):
// - the CPU encoder (x264 at its fast presets) does about 12 million per logical processor, a third of that for
//   HEVC and AV1
// - a GPU encoder keeps up to about 4K at 60 (500 million); the slow presets (P6, P7) to about 1440p at 60
// - the buffer on a spinning disk is kept under 160 Mbps, so a game loading from it still has room

enum QualityRow { Q_RES, Q_FPS, Q_BITRATE, Q_DESKTOP_BITRATE, Q_ENCODER, Q_PRESET, Q_COUNT };

struct QualityHw {
    i32  mon_h, mon_hz; // the recorded monitor; 0 = unknown
    i32  threads;       // logical processors
    i32  vendor;        // the encoder that records (Vendor), -1 = not known yet
    bool hdd;           // the buffer is on a spinning disk
    i32  lost_pct;      // frames lost in the last 10 minutes of play (quality_lost), -1 = not enough measured
};

// What the recorder measured while a game was played, one 10 s window each (recorder.cpp REC_PACE), kept for the
// last 10 minutes. A frame is lost when ffmpeg drops it or when the video falls behind real time.
struct QualityPace { u64 tick; i64 wall_ms, video_ms, frames, drops; };
static QualityPace g_pace[64];
static i32 g_pace_count, g_pace_next;
static const u64 QUALITY_SPAN_MS = 10 * 60 * 1000;
static const i32 QUALITY_LOST_PCT = 3; // shown from this many percent lost

// A window from the recorder: "wall_ms|video_ms|frames|drops".
static void quality_pace(u64 tick, const char *text)
{
    QualityPace p = {tick};
    if (sscanf(text, "%lld|%lld|%lld|%lld", &p.wall_ms, &p.video_ms, &p.frames, &p.drops) != 4 || p.wall_ms <= 0) return;
    g_pace[g_pace_next] = p;
    g_pace_next = (g_pace_next + 1) % COUNT(g_pace);
    if (g_pace_count < COUNT(g_pace)) g_pace_count++;
}

// The percent of frames lost over the last 10 minutes of play, -1 below a minute of it.
static i32 quality_lost(u64 now)
{
    i64 wall = 0, video = 0, frames = 0, drops = 0;
    for (i32 i = 0; i < g_pace_count; i++) {
        const QualityPace &p = g_pace[i];
        if (now - p.tick > QUALITY_SPAN_MS) continue;
        wall += p.wall_ms;
        video += p.video_ms;
        frames += p.frames;
        drops += p.drops;
    }
    if (wall < 60000) return -1;
    f64 behind = video < wall ? 1 - (f64)video / wall : 0;
    f64 dropped = frames + drops > 0 ? (f64)drops / (frames + drops) : 0;
    return (i32)(fmax(behind, dropped) * 100 + 0.5);
}

struct QualityWarn { char why[Q_COUNT][96]; };

static const i32 QUALITY_HEIGHTS[] = {2160, 1440, 1080, 900, 720};
static const i32 QUALITY_FPS[] = {240, 165, 144, 120, 60, 30};
static const f32 QUALITY_HDD_MBPS = 160;

static f64 quality_px(i32 h, i32 fps) { return (f64)h * h * 16 / 9 * fps; }

// The pixel rate the encoder keeps up with, 0 = no limit known.
static f64 quality_budget(const Config &c, const QualityHw &hw)
{
    if (hw.vendor < 0) return 0;
    if (hw.vendor == VENDOR_CPU) {
        f64 per = strcmp(c.codec.s, "h264") ? 4e6 : 12e6;
        return per * (hw.threads > 0 ? hw.threads : 4);
    }
    return preset_step(c.preset.s) >= 5 ? quality_px(1440, 60) : quality_px(2160, 60);
}

static void quality_check(const Config &c, const QualityHw &hw, QualityWarn *w)
{
    *w = {};
    i32 h = c.height > 0 ? c.height : hw.mon_h;
    if (hw.mon_h > 0 && h > hw.mon_h) h = hw.mon_h;
    i32 fps = c.fps;
    if (hw.mon_hz > 0 && fps > hw.mon_hz) {
        snprintf(w->why[Q_FPS], sizeof w->why[Q_FPS], "This monitor shows %d Hz: recorded at %d fps", hw.mon_hz, hw.mon_hz);
        fps = hw.mon_hz;
    }

    // Too many pixels a second for the encoder: name the most that keeps up at the other setting
    f64 budget = quality_budget(c, hw);
    if (h > 0 && budget > 0 && quality_px(h, fps) > budget) {
        const char *who = hw.vendor == VENDOR_CPU ? "the CPU" : "this encoder";
        i32 ok_h = 0, ok_fps = 0;
        for (i32 i = 0; i < COUNT(QUALITY_HEIGHTS) && !ok_h; i++)
            if (QUALITY_HEIGHTS[i] < h && quality_px(QUALITY_HEIGHTS[i], fps) <= budget) ok_h = QUALITY_HEIGHTS[i];
        for (i32 i = 0; i < COUNT(QUALITY_FPS) && !ok_fps; i++)
            if (QUALITY_FPS[i] < fps && quality_px(h, QUALITY_FPS[i]) <= budget) ok_fps = QUALITY_FPS[i];
        if (ok_h) snprintf(w->why[Q_RES], sizeof w->why[Q_RES], "Too much for %s at %d fps: %dp keeps up", who, fps, ok_h);
        else snprintf(w->why[Q_RES], sizeof w->why[Q_RES], "Too much for %s at %d fps", who, fps);
        if (ok_fps) snprintf(w->why[Q_FPS], sizeof w->why[Q_FPS], "Too much for %s at %dp: %d fps keeps up", who, h, ok_fps);
        else snprintf(w->why[Q_FPS], sizeof w->why[Q_FPS], "Too much for %s at %dp", who, h);
        if (hw.vendor == VENDOR_CPU) snprintf(w->why[Q_ENCODER], sizeof w->why[Q_ENCODER], "The CPU encoder costs game frames at %dp %d fps", h, fps);
        else if (preset_step(c.preset.s) >= 5 && quality_px(h, fps) <= quality_px(2160, 60))
            snprintf(w->why[Q_PRESET], sizeof w->why[Q_PRESET], "Slow at %dp %d fps: P5 or faster keeps up", h, fps);
    }

    // What happened on this PC wins over the estimate
    if (hw.lost_pct >= QUALITY_LOST_PCT)
        snprintf(w->why[Q_FPS], sizeof w->why[Q_FPS], "Lost %d%% of frames in the last 10 min of play", hw.lost_pct);

    if (hw.hdd) {
        if (c.bitrate_mbps > QUALITY_HDD_MBPS)
            snprintf(w->why[Q_BITRATE], sizeof w->why[Q_BITRATE], "The buffer is on a hard disk: %g Mbps or less", QUALITY_HDD_MBPS);
        if (c.desktop_bitrate_mbps > QUALITY_HDD_MBPS)
            snprintf(w->why[Q_DESKTOP_BITRATE], sizeof w->why[Q_DESKTOP_BITRATE], "The buffer is on a hard disk: %g Mbps or less", QUALITY_HDD_MBPS);
    }
}

// Is the drive holding dir a spinning disk (it has a seek penalty)? False when it can't be told.
static bool quality_drive_hdd(const wchar_t *dir)
{
    if (!dir[0] || dir[1] != L':') return false;
    wchar_t dev[8] = {L'\\', L'\\', L'.', L'\\', dir[0], L':', 0};
    HANDLE hd = CreateFileW(dev, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING, 0, 0);
    if (hd == INVALID_HANDLE_VALUE) return false;
    STORAGE_PROPERTY_QUERY q = {};
    q.PropertyId = StorageDeviceSeekPenaltyProperty;
    q.QueryType = PropertyStandardQuery;
    DEVICE_SEEK_PENALTY_DESCRIPTOR d = {};
    DWORD got = 0;
    bool hdd = DeviceIoControl(hd, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof q, &d, sizeof d, &got, 0) &&
               got >= sizeof d && d.IncursSeekPenalty;
    CloseHandle(hd);
    return hdd;
}
