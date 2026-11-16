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

// A game's own recording settings, used while it's the game being played. Every field has a "same as the
// default" value (0 or empty), so a game only differs where it was set. The codec and the encoder stay the same for
// every game: saves stream-copy across ffmpeg runs, which needs one codec.
struct RecOverride {
    f32   bitrate_mbps; // 0 = Config.bitrate_mbps
    i32   fps;          // 0 = Config.fps
    i32   height;       // 0 = Config.height, -1 = the monitor's own
    Str64 preset;       // "" = Config.preset
    Str64 rate_control; // "" = Config.rate_control
    i32   cq;           // 0 = Config.cq
};

// One known app, keyed by its lower-case exe name. Holds the per-app settings too (category, a game's recording
// settings, session mode), so there's one table instead of several lists of exe names.
struct AppInfo {
    Str64       exe;          // "overwatch.exe"
    Str64       name;         // shown name, "Overwatch"
    AppCategory category;
    SessionMode session_mode; // games only
    RecOverride rec;          // games only
    i64         last_heard_us;
    char        path[MAX_PATH]; // where its exe was last heard from (UTF-8), for folder rules; "" = not known
    Str64       desc;           // its exe's File description ("sam" for a VoiceStreams player); read from
                                // `path`, not saved
};

// A folder rule: every exe in `path` or below it gets `category`, unless the exe has a category of its own. The
// deepest folder wins; a `*` part matches any one folder name ("...\VoiceStreams\*\clips-off").
struct FolderRule {
    char        path[MAX_PATH * 2]; // UTF-8
    AppCategory category;
};

struct Hotkey {
    Str64 keys;   // "Alt+F10"
    Str64 action; // "save 30s" | "timeline" | "open" | "pause"
};

struct Config {
    Path save_dir;     // <Videos>\Clips, holding 2026\Clips, 2026\Sessions, 2026\Data
    Path buffer_dir;   // the rolling buffer
    bool buffer_by_size; // the buffer is limited by buffer_gb, else by buffer_hours; never both
    f32  buffer_hours; // keep this much footage (24)
    f32  buffer_gb;    // or this much (100)
    f32  min_free_gb;  // delete the oldest footage early rather than go below this much free space (50)
    Path ffmpeg;       // empty = find it

    // Video
    i32   output_idx; // DXGI output index on the capture GPU
    i32   fps;        // never above the monitor's refresh rate
    i32   height;     // output height (1080…); 0 = the monitor's own. Never above the monitor's
    Str64 codec;      // "h264" | "hevc" | "av1"
    Str64 encoder;    // "auto" | "nvenc" | "amf" | "qsv" | "cpu"
    f32   bitrate_mbps;         // while a game is played (each game can have its own)
    f32   desktop_bitrate_mbps; // while no game is
    bool  draw_mouse;
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
    bool dev_log;      // the detailed log (dev mode, see base.h)

    // Look
    i32 accent;     // index into ACCENTS (ui.cpp)
    i32 thumb_size; // Library cards: 0 small, 1 medium, 2 large

    Hotkey hotkeys[16];
    i32    hotkey_count;

    AppInfo apps[512];
    i32     app_count;

    FolderRule folders[32];
    i32        folder_count;
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
    char       path[MAX_PATH]; // its exe's full path (UTF-8), "" for the mic
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
    char path[MAX_PATH]; // the exe's full path when it was recorded (meta.json; "" in older buffers)
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
enum SaveKind : u8 {
    SAVE_CLIP,    // a range of the buffer into <year>\Clips
    SAVE_SESSION, // a range of the buffer into <year>\Sessions
    SAVE_KEEP,    // the editor's Keep: an edit of a saved file, at full quality, into <year>\Clips
    SAVE_SHARE,   // the editor's Share: a small copy with one stereo mix, into the share folder
    SAVE_PREVIEW, // a stretch of the buffer into the local folder, for the timeline's Play; not a saved file
};

enum SaveState : u8 { SAVE_QUEUED, SAVE_RUNNING, SAVE_DONE, SAVE_FAILED, SAVE_CANCELLED };

// ---------------------------------------------------------------------------------------------------------------
// Saved files and edits (library.cpp probes files, editor.cpp edits them, save.cpp renders Keep and Share)

enum { MEDIA_MAX_TRACKS = 16, EDIT_MAX_KEYS = 64 };

// What ffprobe says about a saved file (cached next to its thumbnails).
struct MediaInfo {
    f64  duration; // seconds
    i32  width, height;
    f64  fps;
    i32  track_count;                  // audio tracks
    char tracks[MEDIA_MAX_TRACKS][32]; // their titles: "Mix", "Mic", "Communication", "Overwatch (game)" ...
    char game[64];                     // what it's of: the tagged game, "Desktop", or "Other" for files not ours
};

struct ZoomKey {
    f64 t;    // seconds into the video
    f64 z;    // 1 = no zoom
    f64 x, y; // centre of the view, 0..1 across and down
};

// An edit never changes the original: it's this small record, kept in <year>\Data\<video name>.edit.txt.
struct EditRecord {
    f64     in_s, out_s;             // trim, seconds into the video; out_s 0 = to the end
    ZoomKey keys[EDIT_MAX_KEYS];     // sorted by t
    i32     key_count;
    f32     boost[MEDIA_MAX_TRACKS]; // per audio track: volume - 1, so zero is as recorded (-1 .. +1)
    bool    muted[MEDIA_MAX_TRACKS];
};

// A Keep or Share job: everything the save worker needs, copied with the request.
struct EditJob {
    Path       src;       // the original
    Path       out_dir;   // <year>\Clips for Keep, the share folder for Share
    char       stem[256]; // the output's name without ".mp4" (made unique if taken)
    MediaInfo  info;
    EditRecord edit;
    f32        share_mb;  // Share: the size to aim for
};

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

// player.cpp: one video through Media Foundation's Media Engine into a texture on the UI's device. The library and
// the editor each have one; only one is open at a time.
enum { PLAYER_MAX_TRACK_ENGINES = 8 };

struct Player {
    bool              open;
    Path              path;
    u8                id;  // which player (0 library, 1 editor): media events carry it
    u32               gen; // bumped on every open, so events from a closed file are dropped
    IMFMediaEngine   *engine;
    IMFMediaEngineEx *engine_ex;
    // The editor plays each audio track on its own audio-only engine, so each has its own volume. The main engine
    // is muted then.
    IMFMediaEngine   *track_engine[PLAYER_MAX_TRACK_ENGINES];
    i32               track_of_engine[PLAYER_MAX_TRACK_ENGINES]; // the audio track each one plays
    i32               track_engine_count;
    ID3D11Texture2D          *tex;
    ID3D11ShaderResourceView *srv;
    u32               width, height; // the video's own size, 0 until it's loaded
    bool              loaded, frame_ready, autoplay;
    f64               duration, pos;
    bool              paused, ended, seeking;
    f64               start_at;  // seek here once loaded
    bool              scrubbing; // the scrub bar is held: seeks are approximate until it's let go
    bool              has_seek;  // a seek waits for the one before it to finish
    f64               seek_to;
    i32               audio_stream[MEDIA_MAX_TRACKS]; // the engine's stream index of each audio track
    i32               audio_count;
    i32               sound;              // the audio track heard (the library's Sound choice)
    f64               stop_at, loop_from; // editor: playing stops at the trim's end; 0 = the whole file
    char              error[256];
    bool              set_animating; // this player asked ui.animating to keep the frames coming
};

enum LibView : u8 { LIB_CLIPS, LIB_SESSIONS };
enum { LIB_MAX_ITEMS = 4096, LIB_MAX_GAMES = 128, LIB_THUMBS = 64 };

struct LibItem {
    char      name[256]; // file name without the extension (UTF-8)
    char      ext[8];    // ".mp4"
    i16       year;      // the year folder it's in
    LibView   kind;      // in Clips or Sessions
    bool      loose;     // directly in the year folder (the Rust version's layout), not in its Clips subfolder
    bool      probed;    // `info` is known
    u64       size;
    i64       mtime_us;
    u64       key;       // thumbnail cache key; changes when the file does, not when it's renamed
    MediaInfo info;
};

struct LibThumb {
    u64                       key;
    ID3D11ShaderResourceView *srv;
    i32                       w, h; // one frame
    bool                      pending, failed;
    u32                       used; // for dropping the least recently shown
};

struct Library {
    LibView  view;
    i32      page;
    char     search[128];
    char     game[64];             // the game chip picked; empty = all
    i32      range;                // LibRange (library.cpp): all, today, this week, this month, custom
    i64      range_from, range_to; // the custom range, in local days (local_day)
    i32      sort;                 // LibSort: newest, oldest, longest, biggest
    i32      cal_month;            // the calendar's month, year * 12 + month - 1
    bool     cal_picking;          // the range's start is picked; the next click is its end
    i32      chip_page;
    LibItem  items[LIB_MAX_ITEMS]; // both views, newest first (handed over by the scan worker)
    i32      item_count;
    bool     scanned;              // a scan has finished at least once
    bool     scanning, scan_again;
    u32      scan_gen;
    i32      shown[LIB_MAX_ITEMS]; // this view's items that match the chip and the search
    i32      shown_count;
    char     games[LIB_MAX_GAMES][64]; // chips for this view, most recent first
    i32      game_items[LIB_MAX_GAMES];
    i32      game_count;
    i32      view_count;           // items in this view
    i32      range_count;          // of those, items in the date range
    LibThumb thumbs[LIB_THUMBS];
    u32      thumb_clock;
    // The hovered card's whole scrub strip (each is a few MB, so only one is kept). The editor's timeline uses it too.
    u64                       strip_key, strip_want;
    ID3D11ShaderResourceView *strip;
    i32                       strip_w, strip_h, strip_frames;
    LibItem  target;               // the card a menu, rename or delete is about
    char     rename_buf[256];
    char     rename_error[256];
    char     message[256];         // the last result, shown in the pager row ("Copied", "Couldn't rename ...")
    bool     message_error;
    Player   player;
    LibItem  playing;              // the item the player shows
};

struct ShareFile {
    char name[200]; // in the share folder (UTF-8)
    i64  mtime_us;
    u64  size;
};

struct Editor {                 // editor.cpp: trim, zoom, track volumes, Keep and Share
    bool       open;
    LibItem    item;            // the clip or session being edited
    Path       path;
    EditRecord edit;
    bool       dirty;           // the record changed and isn't written yet
    Player     player;
    i32        tab;             // side panel: 0 edit, 1 keep and share
    i32        key_page, track_page, share_page;
    i32        drag;            // timeline: 0 none, 1 trim start, 2 trim end, 3 playhead
    bool       aiming;          // dragging the picture aims the zoom
    i32        aim_key;
    f64        aim_mx, aim_my, aim_kx, aim_ky;
    bool       tracks_ready;    // the player's per-track engines are set up for this file
    char       pending[MAX_PATH * 3]; // a file to open once the library's scan has it (Timeline's "Save and edit")
    u64        pending_since;   // GetTickCount64 when it was asked for
    i32        share_size;      // 0 = 50 MB, 1 = 10 MB, 2 = custom
    f32        custom_mb;
    u32        keep_job, share_job;
    char       result[300];     // what the last Keep or Share did
    bool       result_error;
    ShareFile  shares[64];      // Recent shares, newest first
    i32        share_count;
};
// timeline.cpp: the buffer and a range to save. Times are wall-clock µs; 0 = not set.
struct TimelineRow {   // one audio row: every stream of one exe (or the mic)
    char exe[64];
    char name[64];
};

struct TimelineFrame { // a preview frame uploaded to the GPU
    i64                       key;     // what it shows (see tl_frame_key), 0 = free slot
    i64                       time_us;
    ID3D11ShaderResourceView *srv;
    u32                       used;    // for evicting the least recently shown
};

enum TimelineDrag : u8 { TL_DRAG_NONE, TL_DRAG_NEW, TL_DRAG_IN, TL_DRAG_OUT, TL_DRAG_PAN };

struct Timeline {
    BufferIdx    *idx;          // the index drawn (heap, swapped with the worker's; freed while hidden)
    bool          loaded;       // idx holds a read, not just zeroes
    u32           ticks;        // 1 s ticks while shown, for the 3 s refresh
    i64           view_end_us, view_span_us;
    bool          follow;       // the right edge stays at now
    i64           sel_in, sel_out;
    i64           playhead_us;  // 0 = the newest footage
    TimelineDrag  drag;
    i64           drag_anchor_us, drag_view_end;
    f32           drag_x;
    bool          dragged;      // moved far enough to be a drag, not a click
    bool          hovering;
    i64           hover_us;
    TimelineRow   rows[64];
    i32           row_count;
    i32           row_scroll;   // first audio row shown
    i16           stream_row[1024]; // the row of each idx->streams entry, -1 = none
    TimelineFrame frames[8];
    u32           frame_clock;
    i64           pending_key, failed_key, shown_key;
    char          session_name[128];
    u32           job;          // the last save started here
    bool          job_edit;     // open the editor when it's done
    // The session box's game suggestions
    i32           suggest_pick;     // the highlighted one; -1 = none
    bool          suggest_all;      // opened from the arrow: every game, not only matches
    bool          suggest_was_open; // the arrow was pressed while they showed, so it closes them
    bool          suggest_close;    // close them next frame
    bool          suggest_refocus;  // put the cursor back in the box next frame
    // Play: a stretch of the buffer rendered with its sound (SAVE_PREVIEW), then played here. The file is the video
    // segments end to end from play_file_start, so gaps in recording are skipped.
    Player        player;          // id 2
    u32           play_job;        // the render being made; 0 = none
    i64           play_file_start; // wall clock of the file's first frame (its first segment's start)
    i64           play_at, play_to; // asked from here (where it starts playing) to here
    i64           play_set_us;     // the playhead as playing last left it, so a click elsewhere can be told apart
    Path          play_path;       // the file played, deleted when done with
    char          play_error[160];
};
// sessions.cpp: detecting sessions, the save popup, unsaved sessions (SPEC.md, "1. Whole sessions").
enum SessionState : u8 {
    SES_FREE,      // empty slot
    SES_OPEN,      // its game is being played, or was less than the session gap ago
    SES_ASKING,    // ended; the popup asks whether to save it
    SES_NOT_SAVED, // ended and not saved (Not now, or by hand): in the Library's unsaved sessions
    SES_SAVING,    // a save job is copying it
    SES_SAVED,
    SES_DISMISSED, // taken off the unsaved sessions; kept until its footage is gone so it's never found again
};

struct Session {
    u32          id;
    SessionState state;
    bool         custom;           // a name typed by hand rather than a game
    bool         prompted;         // the window was brought up for its popup
    char         exe[64];          // the game (lower case); empty for a custom session
    char         name[128];        // shown, and names the file: "Overwatch", "Stream with Sam"
    i64          start_us, end_us; // the real start and end; while open, end is when the game was last seen
    u32          job;              // the save job while SAVING
    char         video[256];       // the saved file's name once SAVED
    char         record[256];      // its record under save_dir, "2026\Data\<name>.session.txt"; empty = none
};

struct Sessions {
    Session items[256];     // slot order means nothing; SES_FREE slots are empty
    u32     next_id;
    bool    loaded;         // sessions.txt and activity.tsv have been read
    bool    game_known;     // the watcher has said what's being played since start
    bool    dirty;          // sessions.txt needs writing
    i64     written_us;     // when sessions.txt was last written
    i64     woke_minute;    // the minute the window was last redrawn for the countdowns
    u32     waiting_logged; // the session whose popup waits on a game in front (logged once)
    // The unsaved sessions in the Library: their tiles stay put while on screen, so a click never moves the others.
    u32     rows[256];
    i32     row_count;
    i32     page;
    i32     drawn_frame;
};

static void sessions_on_game(const char *exe); // audio.cpp tells sessions.cpp the game being played

// options.cpp: every setting. Lists are paged and their order is frozen while Options is open, so a row never
// moves under the mouse after a click (sorting a new app doesn't send it to another page).
enum OptionsTab : u8 {
    OPT_APPS, OPT_RECORDING, OPT_HOTKEYS, OPT_GENERAL,
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
    i32  menu_folder;     // the folder rule whose menu is open
    i32  folders_page;
    i32  folders_shown;   // rules the Folders list shows (after the search)
    bool show_folder_apps; // list apps whose only category is a folder rule's (hidden by default: one per person called)
    i32  hotkeys_page;
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
