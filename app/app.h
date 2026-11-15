// The whole program's state: one App, owned by the main thread, which is the only thread that changes it.
// Worker threads keep their own private data and report to the main thread through the event queue (events.cpp).
// The window draws straight from App, and its controls change App directly.

// ---------------------------------------------------------------------------------------------------------------
// Settings (saved to settings.txt by config.cpp whenever Options changes them)

enum AppCategory : u8 {
    CAT_NONE,          // heard but not sorted yet (shown as new in Options)
    CAT_APP,           // its own track
    CAT_GAME,          // its own track, games first; found as "the game being played"; can start sessions
    CAT_COMMUNICATION, // mixed onto the Communication track
    CAT_IGNORED,       // never recorded
};

enum SessionMode : u8 {
    SESSION_ASK,    // popup when the session ends (default)
    SESSION_ALWAYS, // saved without asking
    SESSION_NEVER,  // not detected
};

// One known app, keyed by its lower-case exe name. Holds the per-app settings too (category, per-game bitrate,
// session mode), so there's one table instead of several lists of exe names.
struct AppInfo {
    Str64       exe;          // "overwatch.exe"
    Str64       name;         // shown name, "Overwatch"
    AppCategory category;
    SessionMode session_mode; // games only
    f32         bitrate_mbps; // games only: bitrate while it's the game being played; 0 = the normal bitrate
    i64         last_heard_us;
};

struct Hotkey {
    Str64 keys;   // "Alt+F10"
    Str64 action; // "save 30s" | "timeline" | "open" | "pause"
};

struct Config {
    Path save_dir;     // <Videos>\Clips, holding 2026\Clips, 2026\Sessions, 2026\Data
    Path buffer_dir;   // the rolling buffer
    f32  buffer_hours; // keep this much footage (24)
    f32  min_free_gb;  // delete the oldest footage early rather than go below this much free space (50)
    Path ffmpeg;       // empty = find it

    // Video
    i32   output_idx; // DXGI output index on the capture GPU
    i32   fps;
    Str64 codec;      // "h264" | "hevc" | "av1"
    Str64 encoder;    // "auto" | "nvenc" | "amf" | "qsv" | "cpu"
    f32   bitrate_mbps;
    bool  draw_mouse;
    bool  advanced;   // the settings below only apply when this is on
    Str64 preset;     // "p1".."p7"
    Str64 rate_control; // "vbr" | "cbr" | "cq"
    i32   cq;
    f32   peak_factor;
    bool  spatial_aq;

    // Audio
    bool mic;

    // Sessions
    f32 session_gap_min;      // a game gone this long ends its session (5)
    f32 session_min_length_min; // shorter sessions aren't offered (15)

    // Sharing
    f32 share_keep_days; // share copies older than this are cleared from %TEMP%\clipback\share (7)

    bool start_with_windows;

    Hotkey hotkeys[16];
    i32    hotkey_count;

    AppInfo apps[512];
    i32     app_count;
};

// ---------------------------------------------------------------------------------------------------------------
// Live state

enum Screen : u8 {
    SCREEN_LIBRARY,
    SCREEN_TIMELINE,
    SCREEN_EDITOR,
    SCREEN_OPTIONS,
};

struct Ui {
    HWND   hwnd;     // the main window; exists from startup, hidden until opened
    bool   shown;
    Screen screen;
    bool   help_open;
    i32    frames_left; // frames still to draw after the last input or change (ImGui settles over a few)
    bool   animating;   // something moves on its own (playback, progress): keep drawing
};

// Each module below owns one struct. Only the main thread changes it; the module's worker threads keep their own
// data and report through events (EV_RECORDER, EV_AUDIO, EV_SAVE).

// recorder.cpp: video capture into the buffer, and pruning the buffer.
enum Vendor : u8 { VENDOR_NVENC, VENDOR_AMF, VENDOR_QSV, VENDOR_CPU, VENDOR_COUNT }; // tried in this order
enum { CODEC_COUNT = 3 };                                                            // h264, hevc, av1

struct Recorder {
    bool   paused;
    bool   recording;          // ffmpeg is capturing right now (it has run for a few seconds)
    Str64  encoder_used;       // "h264_nvenc"; empty while not recording
    u8     vendor;             // Vendor of encoder_used
    Str64  run;                // the run folder being written, "20261115_210654_504"
    i64    recording_since_us;
    Str256 last_error;         // why it isn't recording; empty while healthy
    bool   encoders_known;     // the test encodes have finished
    u8     encoders_ok[CODEC_COUNT]; // per codec (h264, hevc, av1): a bit per Vendor whose test encode worked
    i64    oldest_us;          // the oldest buffered video, 0 = none (updated by the pruner every 30 s)
    i64    buffer_bytes;       // footage in the buffer, video and audio
    i64    free_bytes;         // free space on the buffer's drive, -1 = unknown
};

// audio.cpp: one stream per app playing sound, plus the mic, and the game being played.
enum AudioKind : u8 {
    AUDIO_APP, // an app's process tree (process loopback)
    AUDIO_MIC, // the default capture device
};

// Which track of a saved file a stream ends up on (SPEC.md, "Track layout"). Follows the app's category, so it's
// worked out again whenever the settings change.
enum AudioTrack : u8 {
    TRACK_MIC,
    TRACK_COMMUNICATION,
    TRACK_GAME, // its own track, before other apps
    TRACK_APP,  // its own track
};

struct AudioLive {
    u32        id;       // the audio worker's stream number, unique while clipback runs
    AudioKind  kind;
    AudioTrack track;
    Str64      exe;      // lower-case exe, "mic" for the mic
    i64        since_us; // when the stream started
};

struct Audio {
    AudioLive streams[64]; // live streams in the order they started
    i32       stream_count;
    Str64     game;        // the game being played (lower-case exe); empty on the desktop
};

// buffer.cpp: the buffer's index as read from disk (format in buffer.cpp). Big: the arrays hold 24 h of 2 s video
// segments with room to spare, so instances are static, never on a stack.
struct VideoRun {
    char name[64];  // folder under buffer\video
    i64  anchor_us; // wall clock of the run's first frame (anchor.txt)
    i32  first_seg, seg_count;
};

struct VideoSeg {
    i64  start_us, end_us;  // wall clock
    f64  inpoint, outpoint; // the segment's own timestamps (seconds from its run's start), for ffmpeg's concat
    u64  size;
    i32  run;
    char name[20];          // "v_00012.mkv"
};

struct AudioStreamInfo {
    char dir[128]; // folder under buffer\audio
    char exe[64];  // lower case; "mic" for the mic
    bool mic;
    u32  pid;
    i64  start_us; // wall clock of the first sample (meta.json)
    i32  first_seg, seg_count;
};

struct AudioSeg {
    i64  start_us, end_us; // wall clock; end_us is 0 while `open`
    u64  size;
    bool open;             // being written (not in list.csv yet); only ever a stream's last segment
    char name[20];         // "a_00003.mka"
};

struct ActivityEntry {
    i64  us;
    char exe[64]; // the game being played from `us` on; empty = the desktop
};

struct BufferIdx {
    VideoRun        runs[512];
    i32             run_count;
    VideoSeg        segs[65536]; // every run's segments, oldest first
    i32             seg_count;
    AudioStreamInfo streams[1024];
    i32             stream_count;
    AudioSeg        audio_segs[65536]; // grouped by stream (first_seg, seg_count)
    i32             audio_seg_count;
    ActivityEntry   activity[4096];
    i32             activity_count;
    i64             oldest_us, newest_us; // video footage span (0 if none)
    u64             total_bytes;          // video and audio segments
    i64             video_written_us;     // last write of the newest run's list.csv: recent = recording now
};

// save.cpp: turning ranges of the buffer into files (clips, sessions; later Keep and Share), one ffmpeg at a time
// on a worker thread. `jobs` is what the UI shows; the worker reports into it through EV_SAVE events.
enum SaveKind : u8 { SAVE_CLIP, SAVE_SESSION };

enum SaveState : u8 { SAVE_QUEUED, SAVE_RUNNING, SAVE_DONE, SAVE_FAILED, SAVE_CANCELLED };

struct SaveJobView {
    u32       id; // 0 = free slot
    SaveKind  kind;
    SaveState state;
    i64       from_us, to_us;
    f32       progress;  // 0..1 while running
    char      name[256]; // the name asked for; empty = named after the game played most
    char      path[512]; // the output file once known (UTF-8)
    char      error[512];
};

struct Saving {
    SaveJobView jobs[32];        // a new job takes the oldest finished slot
    i32         jobs_running;    // queued or running
    u32         next_id;
    char        last_saved[512]; // path of the last file saved
};

struct Paths {
    Path config_dir; // %APPDATA%\clipback (or %CLIPBACK_HOME%\config)
    Path local_dir;  // %LOCALAPPDATA%\clipback (or %CLIPBACK_HOME%\local): log, thumbnail cache
    Path settings;   // config_dir\settings.txt
    Path share_dir;  // %TEMP%\clipback\share
};

// Screens, one struct each, owned by the file of the same name (included from ui.cpp).
struct Library { i32 page; };   // library.cpp: clips and sessions, the player
struct Editor { bool open; };   // editor.cpp: trim, volumes, Keep and Share
struct Timeline { i32 zoom; };  // timeline.cpp: the buffer and a range to save
struct Sessions { i32 count; }; // sessions.cpp: detecting sessions, the save popup, Not saved

// options.cpp: every setting. Lists are paged and their order is frozen while Options is open, so a row never
// moves under the mouse after a click (sorting a new app doesn't send it to another page).
enum OptionsTab : u8 {
    OPT_APPS, OPT_HOTKEYS, OPT_RECORDING, OPT_GAME_BITRATES, OPT_ADVANCED, OPT_SESSIONS, OPT_STORAGE, OPT_STARTUP,
    OPT_TAB_COUNT,
};

struct MonitorInfo {
    char name[64]; // "DELL S2721DGF"
    i32  width, height, hz;
    bool primary;
};

struct OptionsUi {
    i32  tab;             // OptionsTab
    i32  last_frame;      // ImGui frame Options was last drawn in: a gap means it was entered again
    // Apps
    i32  filter;          // AppsFilter (options.cpp)
    char search[64];
    char add_exe[64];
    i32  add_category;    // AppCategory for "Add"; 0 = Communication
    i16  order[512];      // indexes into config.apps, in the order shown
    i32  order_count;
    i32  order_app_count; // config.app_count when `order` was made; apps heard since go on the end
    bool order_dirty;     // make `order` again (filter or search changed)
    i32  apps_page;
    i32  menu_app;        // the app right-clicked
    Path menu_path;       // its exe, empty if it isn't running and lifelog never saw it
    // Pages of the other lists
    i32  hotkeys_page, bitrates_page, sessions_page;
    // Hotkeys: the one whose keys are being pressed, + 1 (0 = none)
    i32  capture_slot;
    bool capture_added;   // it was just added, so cancelling removes it again
    // Recording
    MonitorInfo monitors[16];
    i32  monitor_count;
    // The text box being edited keeps its text here until the edit is finished (Enter or clicking away), so a
    // half-typed number never reaches the settings.
    char edit[MAX_PATH * 3];
    bool startup_logged;
};

struct App {
    Paths    paths;
    Config   config;
    Recorder recorder;
    Audio    audio;
    Saving   saving;
    Ui       ui;
    Library  library;
    Editor   editor;
    Timeline timeline;
    OptionsUi options;
    Sessions sessions;
    HWND     msg_hwnd;  // hidden message window: tray, hotkeys, commands, worker events
    bool     quitting;
};

static App g_app;

// activity.tsv is appended by the audio watcher and rewritten by the pruner, on different threads; both hold this.
static SRWLOCK g_activity_lock = SRWLOCK_INIT;
