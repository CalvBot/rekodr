// The whole program's state: one App, owned by the main thread, which is the only thread that changes it.
// Worker threads keep their own private data and report to the main thread through the event queue (events.cpp).
// The window draws straight from App, and its controls change App directly.

// ---------------------------------------------------------------------------------------------------------------
// Settings (saved to settings.txt by config.cpp whenever Options changes them)

enum AppCategory : u8 {
    CAT_NONE,          // heard but not sorted yet (shown as new in Options)
    CAT_APP,           // its own track
    CAT_GAME,          // its own track, games first; found as "the game being played"; can start sessions
    CAT_COMMUNICATION, // Chat: a track of its own, before the games
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
    i32   window;       // record its window, not the monitor: 0 = Config.window_capture, 1 = yes, -1 = no
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
    Str64       desc;           // its exe's File description ("sam" for a VoiceStreams player), or the game
                                // the games database says it is (gamedb.cpp); not saved
};

// A folder rule: every exe in `path` or below it gets `category`, unless the exe has a category of its own. The
// deepest folder wins; a `*` part matches any one folder name ("...\VoiceStreams\*\clips-off").
struct FolderRule {
    char        path[MAX_PATH * 2]; // UTF-8
    AppCategory category;
};

// A recording device besides Windows' default input (Config.mic), on a track of its own. Only the ones turned on
// are kept in the settings.
enum InputMode : u8 { INPUT_OFF, INPUT_MONO, INPUT_STEREO };
struct InputDevice {
    char      id[128];  // the endpoint id, "{0.0.1.00000000}.{...}"
    char      name[96]; // Windows' name for it: "Line (RODE AI-1)"
    InputMode mode;
};

struct Hotkey {
    Str64 keys;   // "Alt+F10"
    Str64 action; // "save 30s" | "timeline" | "open" | "pause"
};

static const char ART_SERVICE_HOST[] = "api.clipback.workers.dev"; // clipback-api, its own repo

static const char SAVE_NAME_DEFAULT[] = "{game} {hh}-{mm} {DD}-{MM}-{YYYY}";

// The two "games" that aren't games: footage of no game, and files the save template doesn't read.
static const char GAME_DESKTOP[] = "Desktop";
static const char GAME_NOT_OURS[] = "Other";

struct Config {
    Path save_dir;     // <Videos>\Clips, holding 2026\Clips, 2026\Sessions, 2026\Data
    Path buffer_dir;   // the rolling buffer
    Path play_dir;     // the Timeline's Play files (preview-*.mp4); the local folder unless moved to a faster drive
    bool backup_on;    // copy sessions to backup_dir (backup.cpp)
    Path backup_dir;   // another drive or a NAS folder; "" = none chosen
    bool backup_clips; // clips too, not only sessions
    char name_template[128]; // what clips and sessions are named: {game} {YYYY} {YY} {MM} {DD} {hh} {mm} {ss}
    bool buffer_by_size; // the buffer is limited by buffer_gb, else by buffer_hours; never both
    f32  buffer_hours; // keep this much footage (24)
    f32  buffer_gb;    // or this much (100)
    f32  min_free_gb;  // delete the oldest footage early rather than go below this much free space (50)
    Path ffmpeg;       // empty = find it

    // Video
    i32   output_idx; // DXGI output index on the capture GPU
    i32   popup_monitor; // where the toasts go: an output index, -2 = the one recorded, -1 = no Clip saved
    i32   popup_monitor_game; // and while a game is in front: an output index, -2 = the one recorded, -3 = as above
    bool  toast_small_in_game; // Clip saved over a game without its buttons, gone sooner
    i32   fps;        // never above the monitor's refresh rate
    i32   height;     // output height (1080...); 0 = the monitor's own. Never above the monitor's
    Str64 codec;      // "h264" | "hevc" | "av1"
    Str64 encoder;    // "auto" | "nvenc" | "amf" | "qsv" | "cpu"
    f32   bitrate_mbps;         // while a game is played (each game can have its own)
    f32   desktop_bitrate_mbps; // while no game is
    bool  draw_mouse;
    Str64 capture;    // "auto" | "wgc" (Windows Graphics Capture) | "dxgi" (Desktop Duplication)
    bool  window_capture; // record the window of the game last in front, not the whole monitor; a game's own
                          // setting (RecOverride.window) wins
    Str64 preset;     // "p1".."p7"
    Str64 rate_control; // "vbr" | "cbr" | "cq"
    i32   cq;
    f32   peak_factor;
    bool  spatial_aq;
    char  encoder_args[256]; // the player's own ffmpeg options for the encoder, added after clipback's ("" = none)
    Path  premiere;   // Adobe Premiere Pro's exe, for "Open in Premiere Pro"; empty = find it
    Path  resolve;    // DaVinci Resolve's exe; empty = find it

    // Audio
    bool        mic;        // Windows' default input, on the Mic track
    InputDevice inputs[16]; // other inputs, each on its own track
    i32         input_count;

    // Sessions
    f32 session_gap_min;      // a game gone this long ends its session (5)
    f32 session_min_length_min; // shorter sessions aren't offered (15)

    // Sharing
    bool game_lookup;    // fetch the games list and art (gamedb.cpp); off = only what is already on disk
    bool share_names;    // "Help name games": naming a game the art service wasn't sure of tells it (gamedb_confirm)
    bool send_stats;     // a daily usage report to the service (telemetry.cpp); off unless the player says yes
    bool auto_update;    // fetch and verify new versions (update.cpp); the top bar then offers the restart
    char sgdb_key[72];   // the player's own SteamGridDB key, for art the art service doesn't hand out; settings.txt only
    Str64 art_service;   // host of the art service gamedb.cpp asks for the art Discord and Steam don't have; "" = none
    // Links (r2.cpp): the player's own Cloudflare R2 bucket. The secret key is in Windows' Credential Manager.
    char r2_account[40];     // Cloudflare account ID
    char r2_access_key[72];  // the R2 API token's access key ID
    char r2_bucket[64];
    char r2_public_url[256]; // the bucket's public address: https://pub-<id>.r2.dev or a custom domain
    f32  r2_budget;          // dollars a month to spend on storage; 0 = the free 10 GB
    i32  r2_read_cap;        // R2 links pause near this many reads (Class B) in the account's month; 0 = no cap
    i32  r2_read_rate;       // an R2 link read this many times in 24 hours pauses until the next day; 0 = no limit
    char link_hosts[256];    // hosts for links while R2 isn't set up, in order: "catbox,litterbox" and the names
                             // of uploaders in <config_dir>\uploaders (.sxcu files); "" = none
    i32  litterbox_hours;    // how long Litterbox keeps a clip: 1, 12, 24 or 72
    bool free_links_seen;    // the player was told free links are public
    bool voice_mono;      // saves keep the Mic and chat tracks in mono (voices are mono anyway; half the size)
    bool clock_24h;      // times of day as 21:04 rather than 9:04 pm (Windows' own format until changed)
    f32 share_sizes_mb[3]; // Share's three sizes, in MiB as Discord counts (20, 50, 1024)
    f32 share_keep_days; // share copies older than this are cleared from %TEMP%\clipback\share (7)
    f32 wave_keep_days;  // audio previews (waveforms) not opened for this long are deleted, 0 = kept (30)

    bool tooltips;     // hovering a control shows what it does and its key (asked in the first-run setup)
    bool setup_done;   // the first-run setup was finished or skipped; false only on a first start
    bool dev_log;      // the detailed log (dev mode, see base.h)

    // Look
    i32 accent;     // index into ACCENTS (ui.cpp)
    bool games_collapsed; // Library: the games list as a rail of icons
    bool unsaved_collapsed; // Library: the unsaved sessions row folded to its heading
    bool side_folders;      // Library: the list on the left shows folders, not games
    i32 card_width; // Library cards: about how wide, in pixels at 100% (Ctrl + wheel over the grid changes it)
    f32 timeline_top;   // Timeline: the share of its height above the tracks (the splitter); 0 = the default
    i32 timeline_names; // Timeline: the track names' column, in pixels at 100% (its edge is dragged); 0 = the default
    i32 timeline_lane; // Timeline: a sound lane's height, in pixels at 100% (Shift+wheel); 0 = as tall as fits
    bool timeline_simple; // Timeline: the sound in four lanes (Mic, Game, Chat, Apps) instead of one per app
    f32 editor_side;    // Editor: the side panel's width in points (the splitter); 0 = the default
    f32 editor_top;     // Editor: the share of its height above the timeline (the splitter); 0 = the default
    i32 editor_lane;    // Editor: a timeline lane's height, in pixels at 100% (Shift + wheel); 0 = the default
    i32 editor_zoom_view; // Editor: how the picture shows the zoom: 0 = a frame over the whole footage (Crop),
                          // 1 = the footage moved and scaled behind a fixed frame (Footage). The result is the same.

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
    bool   help_close; // a command asked for the help to close
    bool   setup_open; // the first-run setup (setup.cpp) covers the window
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
    bool   wgc;                // capturing with Windows Graphics Capture (else Desktop Duplication)
    Str64  run;                // the run folder being written, "20261115_210654_504"
    i64    recording_since_us;
    Str256 last_error;         // why it isn't recording; empty while healthy
    Str256 strain;             // recording, but the PC can't keep up (with a tip); empty while it can
    bool   encoders_known;     // the test encodes have finished
    u8     encoders_ok[CODEC_COUNT]; // per codec (h264, hevc, av1): a bit per Vendor whose test encode worked
    i64    oldest_us;          // the oldest buffered video, 0 = none (updated by the pruner every 30 s)
    i64    buffer_bytes;       // footage in the buffer, video and audio
    i64    free_bytes;         // free space on the buffer's drive, -1 = unknown
};

// audio.cpp: one stream per app playing sound, plus the mic, and the game being played.
enum AudioKind : u8 {
    AUDIO_APP, // an app's process tree (process loopback)
    AUDIO_MIC, // a capture device: Windows' default (path empty) or the one whose endpoint id is the path
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
};

struct Audio {
    AudioLive streams[64]; // live streams in the order they started
    i32       stream_count;
    Str64     game;        // the game being played (lower-case exe); empty on the desktop
    u64       window;      // the game window being recorded (an HWND); 0 = the monitor
};

// buffer.cpp: the buffer's index as read from disk (format in buffer.cpp). Big: the arrays hold 24 h of 2 s video
// segments with room to spare, so instances are static, never on a stack.
struct VideoRun {
    char name[64];  // folder under buffer\video
    i64  anchor_us; // wall clock of the run's first frame (anchor.txt)
    i32  first_seg, seg_count;
    i64  list_written_us, list_bytes; // list.csv as it was read: unchanged means the run is too
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
    char exe[64];  // lower case; "mic" for the default input, the device's name for another input
    bool mic;      // an input; path is the device's endpoint id, empty for the default one
    u32  pid;
    char path[MAX_PATH]; // the exe's full path when it was recorded (meta.json; "" in older buffers)
    i64  start_us; // wall clock of the first sample (meta.json)
    i32  first_seg, seg_count;
    i64  list_written_us, list_bytes; // list.csv as it was read
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
    SAVE_LINK,    // a copy for a link: H.264 with the Mix only, played from the start while it loads, into the link folder
};

enum SaveState : u8 { SAVE_QUEUED, SAVE_RUNNING, SAVE_DONE, SAVE_FAILED, SAVE_CANCELLED };

// ---------------------------------------------------------------------------------------------------------------
// Saved files and edits (library.cpp probes files, editor.cpp edits them, save.cpp renders Keep and Share)

enum { MEDIA_MAX_TRACKS = 32, EDIT_MAX_KEYS = 64, EDIT_MAX_PIECES = 64 };

// What ffprobe says about a saved file (cached next to its thumbnails).
struct MediaInfo {
    f64  duration; // seconds
    i32  width, height;
    f64  fps;
    i32  track_count;                  // audio tracks
    char tracks[MEDIA_MAX_TRACKS][32]; // their titles: "Mix", "Mic", "sam (chat)", "Overwatch (game)" ...
    u8   channels[MEDIA_MAX_TRACKS];   // and channel counts: 1 for a mono Mic or chat, 2 for the rest
    char game[64];                     // what it's of: the tagged game, GAME_DESKTOP or GAME_NOT_OURS
};

// How a keyframed value moves away from a key and into the next (as in Compress for Discord): Smooth and EaseOut
// leave a key slowly, Smooth and EaseIn arrive slowly, Hold keeps the value until the next key.
enum Ease : u8 { EASE_LINEAR, EASE_SMOOTH, EASE_IN, EASE_OUT, EASE_HOLD, EASE_COUNT };

struct ZoomKey {
    f64 t;    // seconds into the video
    f64 z;    // 1 = no zoom
    f64 x, y; // centre of the view, 0..1 across and down
    u8  ease;
};

// A volume key: 1 = as recorded, 0 = silent, 2 = double.
struct VolKey {
    f64 t;
    f32 v;
    u8  ease;
};
enum { EDIT_MAX_VOL_KEYS = 48, EDIT_MASTER = MEDIA_MAX_TRACKS }; // volume slot EDIT_MASTER is the whole mix

// The picture's colour, each kept as keys like a volume's (1 = as recorded, 0..2): brightness, contrast, saturation.
enum { COLOR_BRIGHTNESS, COLOR_CONTRAST, COLOR_SATURATION, EDIT_COLORS };

// How one piece goes into the next: a cut, a fade through black, or a crossfade. At the very start and end only a
// fade (from or to black) is possible.
enum { TRANS_CUT, TRANS_FADE, TRANS_CROSS, TRANS_COUNT };

struct EditPiece {
    f64 from, to;  // seconds into the video
    u8  trans;     // how the piece before goes into this one (TRANS_*; none for the first)
    f32 trans_len; // and over how long, in seconds
};

// Advanced edits: items, each a span of the video placed on a track of the sequence. Track 0 is the picture, 1 + t the
// file's audio track t (a clipback file's Mix has none: it's rebuilt). Items on one track never overlap; time with no
// item plays as black or silence.
enum EditMode : u8 { EDIT_SIMPLE, EDIT_ADVANCED };
enum { EDIT_MAX_ITEMS = 128, EDIT_MAX_ITEM_KEYS = 512 };

struct EditItem {
    f64 start;             // on the sequence, seconds
    f64 from, to;          // seconds into the video
    f32 fade_in, fade_out; // from and to black (or silence) at its ends, seconds
    f32 cross;             // a crossfade from the item before it on its track, when that one ends where this starts
    u16 id;                // its own number, which its keys name (never 0)
    u16 link;              // items with the same link (not 0) move, trim and split together
    u8  track;
};

// What an item's key sets: the zoom (v, centred on x y), a colour value (COLOR_* after IKEY_COLOR) or the volume.
enum { IKEY_ZOOM, IKEY_COLOR, IKEY_VOLUME = IKEY_COLOR + EDIT_COLORS, IKEY_KINDS };

// An item's key, in seconds into the video like the item's span, so it stays on its moment when the item is trimmed.
// v is a zoom (1..4), a colour value or a volume (0..2, 1 = as recorded).
struct ItemKey {
    f64 t, v, x, y;
    u16 item; // the item's id
    u8  what; // IKEY_*
    u8  ease;
};

// An edit never changes the original: it's this small record, kept in <year>\Data\<video name>.edit.txt.
// A Simple edit is the pieces, keys and volumes; an Advanced one is the items and their keys, with muted[] per audio
// track and the rest empty.
struct EditRecord {
    u8      mode; // EditMode
    // The parts of the video kept, in the order they play, each a span of the video in seconds; they never
    // overlap. None means the whole video.
    EditPiece pieces[EDIT_MAX_PIECES];
    i32     piece_count;
    f32     fade_in, fade_out; // seconds of fade from black at the start and to black at the end; 0 = none
    ZoomKey keys[EDIT_MAX_KEYS];     // sorted by t
    i32     key_count;
    // Per audio track, and the whole mix at EDIT_MASTER: the volume while there are no keys (volume - 1, so zero
    // is as recorded, -1 .. +1), muted, and the keys (sorted by t) when the volume changes over time.
    f32     boost[MEDIA_MAX_TRACKS + 1];
    bool    muted[MEDIA_MAX_TRACKS + 1];
    VolKey  vol[MEDIA_MAX_TRACKS + 1][EDIT_MAX_VOL_KEYS];
    i32     vol_count[MEDIA_MAX_TRACKS + 1];
    // The colour's keys (sorted by t), per EDIT_COLORS value; none means as recorded.
    VolKey  color[EDIT_COLORS][EDIT_MAX_VOL_KEYS];
    i32     color_count[EDIT_COLORS];
    EditItem items[EDIT_MAX_ITEMS];
    i32      item_count;
    ItemKey  ikeys[EDIT_MAX_ITEM_KEYS]; // sorted by item, what, t
    i32      ikey_count;
};

// The editor's keyed lanes: the zoom (0), each volume slot (1 + slot), then each colour (ED_COLOR + colour).
enum { ED_COLOR = EDIT_MASTER + 2, ED_LANES = ED_COLOR + EDIT_COLORS };

// A Keep, Share or Link job: everything the save worker needs, copied with the request.
struct EditJob {
    Path       src;       // the original
    Path       out_dir;   // <year>\Clips for Keep, the share folder for Share, the link folder for Link
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
    bool      trimmed;   // from_us was moved later because the buffer didn't reach back that far
    f32       progress;  // 0..1 while running
    char      name[256]; // the name asked for; empty = named after the game played most
    char      path[512]; // the output file once known (UTF-8)
    char      error[512];
};

// A clip or session saved from the buffer: the stretch it came from, so the timeline can show what's been saved.
// Kept in <local_dir>\saved.tsv (save.cpp).
struct SavedRange {
    i64      from_us, to_us; // wall clock
    SaveKind kind;           // SAVE_CLIP or SAVE_SESSION
    bool     gone;           // the file isn't there any more (deleted or renamed)
    char     path[512];      // UTF-8
};

struct Saving {
    SavedRange  saved[256];      // oldest first
    i32         saved_count;
    bool        saved_loaded;
    u64         saved_checked;   // GetTickCount64 of the last look for files that have gone
    SaveJobView jobs[32];        // a new job takes the oldest finished slot nothing still follows
    i32         jobs_running;    // queued or running
    u32         next_id;
    char        last_saved[512]; // path of the last file saved
};

struct Paths {
    Path config_dir; // %APPDATA%\clipback (or %CLIPBACK_HOME%\config)
    Path local_dir;  // %LOCALAPPDATA%\clipback (or %CLIPBACK_HOME%\local): log, thumbnail cache
    Path settings;   // config_dir\settings.txt
    Path share_dir;  // %TEMP%\clipback\share
    Path link_dir;   // %TEMP%\clipback\link: copies made for a link, until they're uploaded
};

// Screens, one struct each, owned by the file of the same name (included from ui.cpp).

// player.cpp: one video through Media Foundation's Media Engine into a texture on the UI's device. The library and
// the editor each have one; only one is open at a time.
enum { PLAYER_MAX_TRACK_ENGINES = 8 };

struct Player {
    bool              open;
    Path              path;
    u8                id;  // which player (0 library, 1 editor, 2 timeline, 3 card preview): media events carry it
    u32               gen; // bumped on every open, so events from a closed file are dropped
    IMFMediaEngine   *engine;
    IMFMediaEngineEx *engine_ex;
    // The editor plays each audio track on its own audio-only engine, so each has its own volume. The main engine
    // is muted then.
    IMFMediaEngine   *track_engine[PLAYER_MAX_TRACK_ENGINES];
    i32               track_of_engine[PLAYER_MAX_TRACK_ENGINES]; // the audio track each one plays
    i32               track_engine_count;
    f64               track_synced[PLAYER_MAX_TRACK_ENGINES]; // ImGui time each was last started or moved
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
    u32               sound_mask;         // library: the audio tracks heard, one bit each; 0 = just `sound`
    f32               rate;               // playback speed; 0 = normal
    f64               stop_at, loop_from; // editor: playing stops at the trim's end; 0 = the whole file
    char              error[256];
    bool              set_animating; // this player asked ui.animating to keep the frames coming
};

enum LibView : u8 { LIB_CLIPS, LIB_SESSIONS };
enum { LIB_MAX_ITEMS = 4096, LIB_MAX_GAMES = 128, LIB_THUMBS = 160, LIB_MAX_FOLDERS = 64, LIB_MAX_FAVOURITES = 64 };

struct LibItem {
    char      name[256]; // file name without the extension (UTF-8)
    char      ext[8];    // ".mp4"
    i16       year;      // the year folder it's in
    LibView   kind;      // in Clips or Sessions
    bool      probed;    // `info` is known
    u64       size;
    i64       mtime_us;
    i64       when_us;   // when it's from: the time in its name (by the template in Options), else mtime_us
    u64       key;       // thumbnail cache key; changes when the file does, not when it's renamed
    MediaInfo info;
    char      display_name[128]; // typed by hand (names.txt); shown in place of its file name
};

struct LibFolder {
    char    name[64];
    LibView view; // Clips and Sessions each have their own folders
};

// A favourite: a row at the top of the list on the left, a game, or a folder of clips or of sessions.
struct LibFavourite {
    char list[12]; // "games", "clips" or "sessions"
    char name[64];
};

struct LibFiled {
    u64 key;     // the card's (LibItem.key)
    u64 folders; // the folders it's in, one bit each (Library.folders' indices)
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
    f32      scroll, scroll_to;    // the grid: how far down it is, in pixels, and where it is gliding to
    bool     open_menu;            // a card asked for its menu (opened outside the grid's own window)
    char     search[128];
    char     game[64];             // the game chip picked; empty = all
    char     folder[64];           // the folder picked (in this view); empty = all
    LibFolder folders[LIB_MAX_FOLDERS]; // both views' folders, in the order they were made
    i32      folder_count;
    i32      folder_items[LIB_MAX_FOLDERS]; // this view's cards in each, in the date range
    LibFiled filed[LIB_MAX_ITEMS]; // the cards that are in folders, in key order
    i32      filed_count;
    bool     folders_loaded;
    i32      folder_renaming;      // the folder whose name is being edited in the list, index + 1; 0 = none
    bool     folder_focus;         // give its name field the keyboard next frame
    char     folder_buf[64];
    i32      folder_target;        // the folder a menu or Delete is about
    LibFavourite favourites[LIB_MAX_FAVOURITES]; // in the order they were added (favourites.txt)
    i32      favourite_count;
    i32      range;                // LibRange (library.cpp): all, today, this week, this month, custom
    i64      range_from, range_to; // the custom range, in local days (local_day)
    i32      sort;                 // LibSort: newest, oldest, longest, biggest
    i32      cal_month;            // the calendar's month, year * 12 + month - 1
    bool     cal_picking;          // the range's start is picked; the next click is its end
    LibItem  items[LIB_MAX_ITEMS]; // both views, newest first (handed over by the scan worker)
    i32      item_count;
    bool     scanned;              // a scan has finished at least once
    bool     scanning;
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
    i32                       strip_frames;
    u64      selected[LIB_MAX_ITEMS]; // the cards ticked to act on together (their keys, in key order)
    i32      selected_count;
    u64      select_anchor;        // the card last ticked or clicked, where Shift + click's range starts
    u64      select_filter;        // what the grid shows (a hash of the view and filters); the selection clears when it changes
    bool     deleting;             // the selection's Delete is running on its own thread
    LibItem  target;               // the card a menu, rename or delete is about
    char     rename_buf[256];
    char     rename_error[256];
    u64      rename_key;           // the card whose name is being edited in place (its key), 0 = none
    bool     rename_focus;         // give the name field the keyboard next frame
    bool     rename_display_name;  // the field edits the card's display name, not its file name
    char     message[256];         // the last result, shown in the pager row ("Copied", "Couldn't rename ...")
    bool     message_error;
    Player   player;
    LibItem  playing;              // the item the player shows
    // The muted preview that plays in a card the mouse has rested on.
    Player   hover;
    u64      hover_key;            // the card under the mouse (its key), 0 = none
    f32      hover_mx, hover_my;   // where the mouse came to rest on it
    f64      hover_since;          // ImGui time it came to rest there
    i32      hover_frame;          // the frame a card last had the mouse
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
    i32        solo;            // the track heard alone, -1 = all (for listening only: not saved)
    i32        color_pick;      // the colour value the timeline's Colour lane shows (COLOR_*)
    i32        share_page;
    bool       aiming;          // dragging the picture aims the zoom
    i32        aim_key;
    f64        aim_mx, aim_my, aim_kx, aim_ky;
    bool       tracks_ready;    // the player's per-track engines are set up for this file
    f32        heard[MEDIA_MAX_TRACKS]; // the volume each track's engine was last given
    bool       heard_set;       // heard holds something (cleared when engines change)
    char       pending[MAX_PATH * 3]; // a file to open once the library's scan has it (Timeline's "Save and edit")
    u64        pending_since;   // GetTickCount64 when it was asked for
    i32        share_size;      // 0..2 = Config.share_sizes_mb, SHARE_LINK = a link, SHARE_CUSTOM = custom_mb
    f32        custom_mb;
    u32        keep_job, share_job;
    u32        link_job;        // the last link's r2 job
    char       result[300];     // what the last Keep, Share or link did
    char       result_path[MAX_PATH * 2]; // the file it made, for Copy, Play and Show; empty when it made none
    char       result_url[512]; // the link it made, for Copy and Open; empty when it made none
    char       out_name[160];   // what Keep and Share call the file
    bool       result_error;
    ShareFile  shares[64];      // Recent shares, newest first
    i32        share_count;
    // The timeline (editor.cpp). Lanes: 0 the zoom, 1 + n volume slot n (EDIT_MASTER = the whole mix).
    struct WaveData *wave;      // the tracks' waveforms (wave.cpp)
    f64        view_from, view_span; // seconds shown across the lanes
    f32        lane_scroll;     // the sound lanes scrolled up by, in pixels
    u64        sel[ED_LANES];   // selected keys, a bit per key, per lane
    i32        lane_drag;       // what the mouse is doing on the timeline (ED_DRAG_*)
    i32        drag_lane, drag_key;
    f64        drag_t;          // the grabbed key's time (or the level) when the drag began
    f32        drag_mx, drag_my;
    bool       drag_moved;
    u64        drag_sel[ED_LANES];
    EditRecord drag_from;       // the record when the drag began
    i32        menu_lane, menu_key; // the right-click menu's target
    f64        menu_t, menu_v;
    struct { i32 lane; f64 t, v, x, y; u8 ease; } clip[128]; // copied keys, t from the first
    i32        clip_count;
    EditRecord settled;         // the record as of the last settled change (what undo goes back from)
    EditRecord undo[20], redo[20];
    i32        undo_count, redo_count;
    i32        play_piece;      // the piece the playhead is in (more than one piece plays them joined), -1 not known
};
// timeline.cpp: the buffer and a range to save. Times are wall-clock us; 0 = not set.
// A stretch of the buffer built for the Timeline's Play. Its file is the video segments end to end from file_start, so
// gaps in recording are skipped.
struct PlayPiece {
    u32  job;        // its build; 0 once done, or none
    Path path;       // the finished file, deleted when done with
    i64  file_start; // wall clock of the file's first frame (its first segment's start)
    i64  at, to;     // built from here (where it starts playing) to here
};

struct TimelineRow {   // one audio row: every stream of one exe (or the mic)
    char exe[64];
    char name[64];
    u8   kind;    // what kind of track it is (TrackKind), which gives its lane's colour
};

struct TimelineFrame { // a preview frame uploaded to the GPU
    i64                       key;     // what it shows (see tl_frame_key), 0 = free slot
    ID3D11ShaderResourceView *srv;
    u32                       used;    // for evicting the least recently shown
};

// SEEK moves the playhead (on the tracks); on the ruler NEW draws a selection, RANGE moves it, IN and OUT its ends;
// MARK is a press on a marker, in the strip over the ruler
enum TimelineDrag : u8 { TL_DRAG_NONE, TL_DRAG_SEEK, TL_DRAG_NEW, TL_DRAG_RANGE, TL_DRAG_IN, TL_DRAG_OUT, TL_DRAG_PAN, TL_DRAG_MARK };

struct Timeline {
    BufferIdx    *idx;          // the index drawn (heap, swapped with the worker's; freed while hidden)
    bool          loaded;       // idx holds a read, not just zeroes
    u32           ticks;        // 1 s ticks while shown, for the 3 s refresh
    i64           view_end_us, view_span_us;
    bool          follow;       // the right edge stays at now
    i64           sel_in, sel_out;
    i64           playhead_us;  // 0 = the newest footage
    i64           marker_pick;  // the marker clicked on (its time), its note shown; 0 = none
    f32           lane_scroll;  // the main timeline's sound lanes, scrolled down this many pixels
    TimelineDrag  drag;
    i64           drag_anchor_us, drag_view_end;
    f32           drag_x;
    bool          dragged;      // moved far enough to be a drag, not a click
    bool          quick;        // opened by the hotkey from a game: a save (Enter) or Esc hides the window again
    bool          hovering;
    i64           hover_us;
    TimelineRow   rows[64];
    i32           row_count;
    i16           stream_row[1024]; // the row of each idx->streams entry, -1 = none
    TimelineFrame frames[48]; // the preview and the frames along the video row
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
    // Play (timeline.cpp): the buffer built a piece at a time with its sound mixed (SAVE_PREVIEW) and played here.
    Player        player;          // id 2
    PlayPiece     play;            // the piece playing, or being built to play now
    PlayPiece     next;            // the one after it, built while it plays
    PlayPiece     rest;            // built ahead at a playhead left still, for Play to take
    i64           play_end;        // Play stops here: the selection's end when it started in it
    i64           play_set_us;     // the playhead as playing last left it, so a click elsewhere can be told apart
    i64           rest_us;         // the playhead as last seen while not playing
    u64           rest_since;      // GetTickCount64 when it got there
    bool          rest_tried;      // its piece was built (or couldn't be): not again until it moves
    char          play_error[160];
};
// sessions.cpp: detecting sessions, the save popup, unsaved sessions (SPEC.md, "1. Whole sessions").
enum SessionState : u8 {
    SES_FREE,      // empty slot
    SES_OPEN,      // its game is being played, or was less than the session gap ago
    SES_ASKING,    // ended; the session toast asks whether to save it
    SES_NOT_SAVED, // ended and not saved (Not now, or by hand): in the Library's unsaved sessions
    SES_SAVING,    // a save job is copying it
    SES_SAVED,
};

struct Session {
    u32          id;
    SessionState state;
    bool         custom;           // a name typed by hand rather than a game
    bool         asked;            // the session toast has shown it
    char         exe[64];          // the game (lower case); empty for a custom session
    char         name[128];        // shown, and names the file: "Overwatch", "Stream with Sam"
    i64          start_us, end_us; // the real start and end; while open, end is when the game was last seen
    u32          job;              // the save job while SAVING
    char         video[256];       // the saved file's name once SAVED
    char         record[256];      // its record under save_dir, "2026\Data\<name>.session.txt"; empty = none
    char         display_name[128]; // typed by hand; shown as its title, the name and times kept under it
};

// A row that slides sideways (kit_strip_* in ui.cpp): where it is and where it's gliding to (px), and where and
// when the glide set off.
struct KitStrip {
    f32 pos, to, from;
    i64 from_us;
};

struct Sessions {
    Session items[256];     // slot order means nothing; SES_FREE slots are empty
    u32     next_id;
    bool    loaded;         // sessions.txt and activity.tsv have been read
    bool    game_known;     // the watcher has said what's being played since start
    bool    dirty;          // sessions.txt needs writing
    i64     written_us;     // when sessions.txt was last written
    i64     woke_minute;    // the minute the window was last redrawn for the countdowns
    u32     waiting_logged; // the session whose toast waits on a game in front (logged once)
    // The unsaved sessions in the Library: their tiles stay put while on screen, so a click never moves the others.
    u32     rows[256];
    i32     row_count;
    KitStrip strip;           // how far along the row is slid
    bool    name_focus;       // the save prompt's display name field takes the keyboard next frame
    char    name_buf[128];    // the display name being typed in the save prompt
    i32     drawn_frame;
};

static void sessions_on_game(const char *exe); // audio.cpp tells sessions.cpp the game being played

// options.cpp: every setting. Lists are paged and their order is frozen while Options is open, so a row never
// moves under the mouse after a click (sorting a new app doesn't send it to another page).
enum OptionsTab : u8 {
    OPT_APPS, OPT_RECORDING, OPT_HOTKEYS, OPT_INTEGRATIONS, OPT_GENERAL,
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

// Markers (markers.cpp): moments marked in the buffer, each with an optional colour and note
struct Marker {
    i64  t_us;      // when, on the wall clock; no two markers share one, so it also says which marker
    u8   colour;    // an index into MARKER_COLOURS; 0 = none
    char note[256]; // UTF-8, one line
};

struct MarkerColour {
    const char *name;
    u32         rgb;
};
static const MarkerColour MARKER_COLOURS[] = {
    {"", 0x8e939c}, // none: grey
    {"red", 0xf0434f}, {"orange", 0xf08a3c}, {"yellow", 0xe8c53d}, {"green", 0x3fbf74}, {"blue", 0x5b9cff}, {"purple", 0xb07cff},
};

static i32 markers_in(i64 from, i64 to, Marker *out, i32 cap); // the ones in [from, to], oldest first; any thread
