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

struct Recorder {
    bool  paused;
    bool  recording;    // ffmpeg is capturing right now
    Str64 encoder_used; // "h264_nvenc"
    Str256 last_error;
};

struct Paths {
    Path config_dir; // %APPDATA%\clipback (or %CLIPBACK_HOME%\config)
    Path local_dir;  // %LOCALAPPDATA%\clipback (or %CLIPBACK_HOME%\local): log, thumbnail cache
    Path settings;   // config_dir\settings.txt
    Path share_dir;  // %TEMP%\clipback\share
};

struct App {
    Paths    paths;
    Config   config;
    Recorder recorder;
    Ui       ui;
    HWND     msg_hwnd;  // hidden message window: tray, hotkeys, commands, worker events
    bool     quitting;
};

static App g_app;
