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

// Sounds: each event Rekodr has a sound for plays one from the sound library (assets/sounds, RCDATA SOUND_RES +
// its index) or the player's own .wav, when its switch is on. The first SOUND_EVENT_COUNT sounds of the library are
// the events' own, in the same order; the rest are there to pick from.
enum SoundEvent : u8 {
    SOUND_CLIP_SAVED,
    SOUND_SHARE_READY,   // a shared file or link is on the clipboard
    SOUND_MARKER,
    SOUND_RECORDING_ON,  // recording resumed
    SOUND_RECORDING_OFF, // recording paused
    SOUND_SESSION_SAVED,
    SOUND_FAILED,        // a save or share failed
    SOUND_EVENT_COUNT,
};

struct SoundInfo {
    const char *key;   // the file name in assets/sounds, and the name in settings.txt
    const char *label;
};

static const i32 SOUND_RES = 40;
static const SoundInfo SOUND_LIBRARY[] = {
    {"clip-saved", "Clip saved"}, {"share-ready", "Share ready"}, {"marker", "Marker"},
    {"recording-on", "Recording on"}, {"recording-off", "Recording off"}, {"session-saved", "Session saved"},
    {"failed", "Failed"}, {"chime", "Chime"}, {"high", "High"}, {"low", "Low"}, {"echo", "Echo"}, {"soft", "Soft"},
    {"tick", "Tick"}, {"arpeggio", "Arpeggio"}, {"chord", "Chord"}, {"double", "Double"}, {"reverse", "Reverse"},
    {"shimmer", "Shimmer"}, {"retro", "Retro"}, {"flutter", "Flutter"}, {"deep", "Deep"},
};
static const i32 SOUND_COUNT = (i32)(sizeof SOUND_LIBRARY / sizeof SOUND_LIBRARY[0]);
static const u8 SOUND_OWN = 0xff; // SoundPick.sound: the player's own file

// The events' names in settings.txt ("sound_<key>") and in Options.
static const SoundInfo SOUND_EVENTS[SOUND_EVENT_COUNT] = {
    {"clip_saved", "Clip saved"}, {"share_ready", "Share ready"}, {"marker", "Marker"},
    {"recording_on", "Recording on"}, {"recording_off", "Recording off"}, {"session_saved", "Session saved"},
    {"failed", "Failed"},
};

struct SoundPick {
    bool on;
    u8   sound; // an index in SOUND_LIBRARY, or SOUND_OWN
    Path own;   // SOUND_OWN's .wav
};

// How "Clip saved" shows over a game: the whole popup, the small one (no buttons, gone sooner) or none (the clip is
// saved all the same). DEFAULT is a game's "same as all games" (Config.toast_clip); toast_clip_for picks.
// The corner of its monitor the toasts go in; the next one up stacks away from that edge.
enum PopupCorner : u8 {
    CORNER_BR, // the default
    CORNER_BL,
    CORNER_TR,
    CORNER_TL,
};

// Config.popup_monitor and popup_monitor_game, besides an output index
static const i32 POPUP_RECORDED = -2, POPUP_AS_NORMAL = -3;

enum ToastClip : u8 {
    TOAST_CLIP_DEFAULT,
    TOAST_CLIP_FULL,
    TOAST_CLIP_SMALL,
    TOAST_CLIP_OFF,
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
    ToastClip   toast_clip;   // games only: Clip saved over it, DEFAULT = Config.toast_clip
    i8          mic;          // games only: the Mic track while it's played, 1 = on, -1 = off, 0 = Config.mic
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

// The title of a save's Mic track as recorded, beside the cleaned Mic (Config.mic_raw).
static const char MIC_RAW_TITLE[] = "Mic (raw)";

struct Hotkey {
    Str64 keys;   // "Alt+F7"
    Str64 action; // "save 30s" | "timeline" | "open" | "pause"
    Str64 hold;   // what holding the keys does instead, "save 3m"; "" = holding is the same as a tap
    Str64 game;   // "" = every game and the desktop; a game's exe = only while it's played, where it wins over an
                  // every-game hotkey on the same keys
};

// The service (clipback-api, its own repo): its host and the header its requests are signed in
static const char ART_SERVICE_HOST[] = "api.clipback.workers.dev";
#define ART_SERVICE_SIG_HEADER L"x-clipback"
static const u32 ACCENT_DEFAULT = 0x00897b; // the accent until Options picks another: teal

static const char SAVE_NAME_DEFAULT[] = "{game} {hh}-{mm} {DD}-{MM}-{YYYY}";

// The two "games" that aren't games: footage of no game, and files the save template doesn't read.
static const char GAME_DESKTOP[] = "Desktop";
static const char GAME_NOT_OURS[] = "Other";

enum { WATCH_MAX = 8 }; // other apps' folders the library shows (Config.watch_dirs)
enum { LINK_DAYS_MAX = 90 }; // the most days a bucket link can last short of forever (Config.share_link_days)
enum { FOLDER_MAX = 128 }; // folder rules (Config.folders), and store folders added as rules (Config.store_seen)

// Games that mark their events on the timeline (automarks.cpp). Each marks the AmEvent bits it has, saved as the
// words of AM_EVENT_WORDS (bit i). AM_WIN is a won match; AM_OBJECTIVE is a dragon, Herald, Baron, turret or
// inhibitor the player took in League. AM_LAP is a lap better than the player's best (F1: the session's fastest);
// AM_INCIDENT is contact, a penalty or iRacing's incident points. In Rocket League AM_KILL is a demolition the
// player made and AM_DEATH one they took. AM_QUEST is a quest finished; AM_GROUP someone joining the player's group.
// AM_DOWNED is the player knocked down (Rust's wounded). AM_CONTRACT a contract taken, AM_ZONE a zone entered or left,
// AM_TRAVEL a jump (quantum or to another system), AM_SPAWN the player spawning (Star Citizen). AM_SERVER the
// player joining a server, AM_ACCOUNT the account they play as (Rust); an account's marker stays on this PC.
// AM_HEADSHOT a kill by a headshot, AM_MVP the round's MVP, AM_PLANT the bomb planted by the player, AM_DEFUSE
// the bomb defused by them (Counter-Strike 2).
enum AmEvent : u32 {
    AM_KILL = 1, AM_MULTI = 2, AM_DEATH = 4, AM_ROUND = 8, AM_WIN = 16, AM_OBJECTIVE = 32, AM_ASSIST = 64,
    AM_ADVANCEMENT = 128, AM_LEVEL = 256, AM_LAP = 512, AM_OVERTAKE = 1024, AM_INCIDENT = 2048, AM_GOAL = 4096,
    AM_SAVE = 8192, AM_RAID_START = 16384, AM_RAID_END = 32768, AM_QUEST = 65536, AM_GROUP = 131072,
    AM_DOWNED = 262144, AM_CONTRACT = 524288, AM_ZONE = 1048576, AM_TRAVEL = 2097152, AM_SPAWN = 4194304,
    AM_SERVER = 8388608, AM_ACCOUNT = 16777216, AM_HEADSHOT = 33554432, AM_MVP = 67108864, AM_PLANT = 134217728,
    AM_DEFUSE = 268435456
};
static const char *const AM_EVENT_WORDS[] = {"kill", "multi", "death", "round", "win", "objective", "assist", "advancement", "level",
                                             "lap", "overtake", "incident", "goal", "save", "raid_start", "raid_end", "quest", "group", "downed",
                                             "contract", "zone", "travel", "spawn", "server", "account",
                                             "headshot", "mvp", "plant", "defuse"};
// Each event's marker colour (MARKER_COLOURS) and the note it gets when its source gives none: a plugin's events.
static const u8 AM_EVENT_COLOURS[] = {4, 4, 1, 3, 3, 5, 5, 6, 6, 6, 4, 1, 4, 5, 5, 5, 3, 6, 2, 5, 6, 5, 3, 5, 6, 4, 3, 5, 5};
static const char *const AM_EVENT_NOTES[] = {"Kill", "Multi-kill", "Death", "Round won", "Win", "Objective", "Assist",
                                             "Advancement", "Level up", "Best lap", "Overtake", "Incident", "Goal", "Save",
                                             "Raid start", "Raid end", "Quest done", "Group joined", "Downed",
                                             "Contract accepted", "Zone", "Travel", "Spawned", "Joined server", "Playing as",
                                             "Headshot", "MVP", "Bomb planted", "Bomb defused"};
static_assert(sizeof AM_EVENT_COLOURS == sizeof AM_EVENT_WORDS / sizeof *AM_EVENT_WORDS, "a colour for each event");
static_assert(sizeof AM_EVENT_NOTES == sizeof AM_EVENT_WORDS, "a note for each event");
enum AmGameId { AMG_CS2, AMG_DOTA2, AMG_LOL, AMG_MINECRAFT, AMG_POE, AMG_POE2, AMG_TF2, AMG_F1_25, AMG_IRACING, AMG_ACC, AMG_FORTNITE, AMG_SIEGE,
                AMG_ROCKET_LEAGUE, AMG_HEARTHSTONE, AMG_WAR_THUNDER, AMG_TARKOV, AMG_RUST, AMG_STAR_CITIZEN, AMG_HOUSE_FLIPPER_2, AMG_COUNT };
// key: the config's name for it (auto_marks_<key>, auto_marks_<key>_on); need: what the player has to do in the
// game for its markers, or 0.
struct AmGame { const char *key, *name, *need; u32 events; };
static const AmGame AM_GAMES[AMG_COUNT] = {
    {"cs2", "Counter-Strike 2", 0, AM_KILL | AM_MULTI | AM_HEADSHOT | AM_DEATH | AM_MVP | AM_PLANT | AM_DEFUSE | AM_ROUND | AM_WIN | AM_RAID_START | AM_RAID_END},
    {"dota2", "Dota 2", "Add -gamestateintegration to its launch options", AM_KILL | AM_DEATH | AM_ASSIST | AM_WIN},
    {"lol", "League of Legends", 0, AM_KILL | AM_MULTI | AM_DEATH | AM_OBJECTIVE | AM_WIN},
    {"minecraft", "Minecraft", 0, AM_KILL | AM_DEATH | AM_ADVANCEMENT},
    {"poe", "Path of Exile", 0, AM_DEATH | AM_LEVEL},
    {"poe2", "Path of Exile 2", 0, AM_DEATH | AM_LEVEL},
    {"tf2", "Team Fortress 2", 0, AM_KILL | AM_DEATH},
    {"f1_25", "F1 25", "Turn on UDP Telemetry in the game's settings", AM_LAP | AM_OVERTAKE | AM_INCIDENT | AM_WIN},
    {"iracing", "iRacing", 0, AM_LAP | AM_INCIDENT | AM_WIN},
    {"acc", "Assetto Corsa Competizione", 0, AM_LAP | AM_OVERTAKE},
    {"fortnite", "Fortnite", "Turn on Record Replays in the game", AM_KILL | AM_DEATH},
    {"siege", "Rainbow Six Siege", 0, AM_KILL | AM_DEATH},
    {"rocket_league", "Rocket League", 0, AM_GOAL | AM_SAVE | AM_ASSIST | AM_KILL | AM_DEATH | AM_WIN},
    {"hearthstone", "Hearthstone", 0, AM_WIN},
    {"war_thunder", "War Thunder", 0, AM_KILL | AM_DEATH | AM_WIN},
    {"tarkov", "Escape from Tarkov", 0, AM_RAID_START | AM_RAID_END | AM_QUEST | AM_GROUP | AM_KILL},
    {"rust", "Rust", 0, AM_KILL | AM_DEATH | AM_DOWNED | AM_SERVER | AM_ACCOUNT},
    {"star_citizen", "Star Citizen", 0, AM_KILL | AM_DEATH | AM_DOWNED | AM_SPAWN | AM_CONTRACT | AM_QUEST | AM_ZONE | AM_TRAVEL},
    {"house_flipper_2", "House Flipper 2", 0, AM_ADVANCEMENT | AM_QUEST},
};

// A plugin's event ticks in Options > Auto markers, saved as am_events_plugin_<folder> (plugins.cpp). A plugin
// with none saved marks every event it has.
enum { AM_PLUGIN_TICKS = 16, AM_PLUGIN_FOLDER = 18 };
struct AmPluginTicks { char folder[AM_PLUGIN_FOLDER]; u32 events; };

struct Config {
    Path save_dir;     // <Videos>\Rekodr, holding Clips, Sessions and Data (save_clips_dir and the rest)
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
    Path watch_dirs[WATCH_MAX]; // other apps' folders (OBS, ShadowPlay...) whose videos show in the library too;
    i32  watch_count;           // by default the ones found on the PC (watch_found)
    Path discord_dir;           // where Discord saves its clips, shown like those; "" = none. Discord keeps it in
                                // its own data, which isn't read: the player picks it, else Videos\Discord or
                                // Documents\Discord if there is one

    // Video
    i32   output_idx; // DXGI output index on the capture GPU
    i32   popup_monitor; // where the toasts go: an output index or POPUP_RECORDED
    PopupCorner popup_corner;
    i32   popup_monitor_game; // and while a game is in front: an output index, POPUP_RECORDED or POPUP_AS_NORMAL
    PopupCorner popup_corner_game; // unless popup_monitor_game is POPUP_AS_NORMAL
    ToastClip toast_clip; // Clip saved over a game, unless the game has its own (FULL, SMALL or OFF)
    SoundPick sounds[SOUND_EVENT_COUNT]; // what each event plays (sounds.cpp)
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
    char  encoder_args[256]; // the player's own ffmpeg options for the encoder, added after Rekodr's ("" = none)
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
    bool share_names;    // "Help name games": naming a game by hand tells the art service (gamedb_rename)
    bool steam_type_check; // a Steam app the service says Steam's store calls software is an App, even in a Game
                           // folder (gamedb_name_app)
    bool send_stats;     // a daily usage report to the service (telemetry.cpp); on unless the player says no
    bool auto_update;    // fetch and verify new versions (update.cpp); the top bar then offers the restart
    char sgdb_key[72];   // the player's own SteamGridDB key, for art the art service doesn't hand out; settings.txt only
    Str64 art_service;   // host of the art service gamedb.cpp asks for the art Discord and Steam don't have; "" = none
    // Links (r2.cpp): the player's own Cloudflare R2 bucket. The secret key is in Windows' Credential Manager.
    char r2_account[40];     // Cloudflare account ID
    char r2_access_key[72];  // the R2 API token's access key ID
    char r2_bucket[64];
    char r2_public_url[256]; // the bucket's public address: https://pub-<id>.r2.dev or a custom domain
    f32  r2_budget;          // dollars a month to spend on storage beyond Rekodr's space
    f32  r2_space_gb;        // GB of the account's free storage Rekodr may use; < 0 = not set (r2_space_default)
    i32  r2_read_cap;        // R2 links pause near this many reads (Class B) in the account's month; 0 = no cap
    i32  r2_read_rate;       // an R2 link read this many times in 24 hours pauses until the next day; 0 = no limit
    char link_uploaders[256]; // the player's uploaders that are on (.sxcu files in <config_dir>\uploaders, by name),
                             // comma separated, used without R2; "" = none
    bool voice_mono;      // saves keep the Mic and chat tracks in mono (voices are mono anyway; half the size)
    bool cam_on;          // the webcam is recorded into the buffer while the screen is (webcam.cpp; off)
    Str256 cam_device;    // its DirectShow name; "" = the first camera found
    bool mic_clean;       // saves take steady noise (fans, hiss, hum) out of the Mic track (on)
    bool mic_raw;         // with mic_clean, saves keep the Mic as recorded too, on a "Mic (raw)" track that no Mix
                          // takes (off)
    bool clock_24h;      // times of day as 21:04 rather than 9:04 pm (Windows' own format until changed)
    bool render_edits;   // an edited clip's file in Clips is its edit rendered, the original in Source (on)
    f32 share_max_mb;    // the biggest file Share makes, in MiB as Discord counts (20)
    f32 social_copy_mb;  // the biggest video chat cards play from a link (MB), made about 5% under it (150)
    i32 share_min_quality; // Lower quality to fit: a copy may go down to this percent of the recommended bitrate
                           // to fit a size, 50 to 100; 100 = never (67)
    i32 share_kbps[6][2];  // the player's own video bitrate in kbit/s for each SHARE_KBPS_HEIGHTS row, at 30 and
                           // 60 fps (SHARE_KBPS_DEFAULT), used while share_kbps_custom is on
    bool share_kbps_custom; // Share uses share_kbps, else SHARE_KBPS_DEFAULT ("Use recommended", off)
    bool share_link_page; // a bucket link is a preview page that plays the clip, not the .mp4 itself (on)
    bool share_copy_on_close; // when a game closes, its clips' share copies are made while nothing else runs (off)
    bool auto_marks;      // Options' Auto markers tab as a whole: games' and Steam achievements' markers (on)
    bool auto_marks_on[AMG_COUNT];     // each of AM_GAMES marks its events on the timeline (on)
    u32  auto_marks_events[AMG_COUNT]; // AmEvent bits: what it marks while on (all it has)
    char wt_name[64];                  // the player's War Thunder name, whose kills and deaths are marked ("")
    u8   rust_binds;                   // the keys Rekodr binds in Rust to print its combat log: 1 << RUST_BIND_* bits
    bool ach_marks;       // a Steam achievement rarer than ach_rarer gets a marker (achievements.cpp; on)
    i32  ach_rarer;       // percent of players: 1, 5 (default), 10 or 100 (any)
    i32  share_link_days; // how long a new bucket link lasts, in days: 0 forever (0), else 1 to LINK_DAYS_MAX; the
                          // Share menu can pick another for one link
    u8   embed_style;      // the look of a link's card, an index of the styles in r2.cpp (embed_style_id); saved
                           // by its id ("classic")
    f32 folder_warn_gb;  // the Library warns once its clips and sessions hold more than this; 0 = never. Nothing
                         // is deleted (500)
    f32 share_keep_days; // share copies older than this are cleared from %TEMP%\Rekodr\share (7)
    f32 wave_keep_days;  // audio previews (waveforms) not opened for this long are deleted, 0 = kept (30)

    bool tooltips;     // hovering a control shows what it does and its key (asked in the first-run setup)
    bool flat_look;    // plain flat surfaces: no art texture, grain or see-through panels (off)
    bool setup_done;   // the first-run setup was finished or skipped; false only on a first start
    char seen_version[16]; // the newest version whose What's new the player has seen (Help > What's new)
    bool dev_log;      // the detailed log (dev mode, see base.h)

    // Look
    u32 accent;     // the accent colour, 0xRRGGBB: the app's, the edge of Discord cards and the clip page's
    bool games_collapsed; // Library: the games list as a rail of icons
    bool unsaved_collapsed; // Library: the unsaved sessions row folded to its heading
    bool side_folders;      // Library: the list on the left shows folders, not games
    i32 card_width; // Library cards: about how wide, in pixels at 100% (Ctrl + wheel over the grid changes it)
    f32 timeline_top;   // Timeline: the share of its height above the tracks (the splitter); 0 = the default
    i32 timeline_names; // Timeline: the track names' column, in pixels at 100% (its edge is dragged); 0 = the default
    i32 timeline_lane; // Timeline: a sound lane's height, in pixels at 100% (Shift+wheel); 0 = as tall as fits
    bool timeline_simple; // Timeline: the sound in four lanes (Mic, Game, Chat, Apps) instead of one per app
    // Timeline: the rows shown, |-separated: "Clips", "Sessions", and layers by name (Clips|Sessions)
    char timeline_show[512];
    bool front_window;      // the Front window layer is recorded (on)
    bool input_busy;        // the Busy layer is recorded: how many keys and mouse moves a second, never which (off)
    char plugins_off[512];  // plugin folders switched off, |-separated
    char am_open[512];      // Auto markers: the rows shown open, |-separated: game keys, plugin:<folder>, ach, other
    AmPluginTicks am_plugin[AM_PLUGIN_TICKS]; // plugins' event ticks, newest last
    i32  am_plugin_count;
    f32 editor_side;    // Editor: the side panel's width in points (the splitter); 0 = the default
    f32 editor_top;     // Editor: the share of its height above the timeline (the splitter); 0 = the default
    i32 editor_lane;    // Editor: a timeline lane's height, in pixels at 100% (Shift + wheel); 0 = the default
    i32 editor_zoom_view; // Editor: how the picture shows the zoom: 0 = a frame over the whole footage (Crop),
                          // 1 = the footage moved and scaled behind a fixed frame (Footage). The result is the same.

    Hotkey hotkeys[16];
    i32    hotkey_count;

    AppInfo apps[512];
    i32     app_count;

    FolderRule folders[FOLDER_MAX];
    i32        folder_count;
    // The store game folders already added as folder rules (store_folders_update), so one the player removed or gave
    // another category isn't added again
    char store_seen[FOLDER_MAX][MAX_PATH * 2];
    i32  store_seen_count;
};

// The save folder: <save_dir>\Clips\<year> (with Source\ in it for rendered clips' originals),
// <save_dir>\Sessions\<year>, and <save_dir>\Data for edits, layers, render notes, session records and Rendering.
// The year is the year the footage starts in.
static inline Path save_clips_dir(const Path &root, i32 year)
{ Path p; swprintf(p.s, MAX_PATH, L"%s\\Clips\\%d", root.s, year); return p; }
static inline Path save_source_dir(const Path &root, i32 year)
{ Path p; swprintf(p.s, MAX_PATH, L"%s\\Clips\\%d\\Source", root.s, year); return p; }
static inline Path save_sessions_dir(const Path &root, i32 year)
{ Path p; swprintf(p.s, MAX_PATH, L"%s\\Sessions\\%d", root.s, year); return p; }
static inline Path save_data_dir(const Path &root) { return path_join(root, L"Data"); }

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
    bool   news_open;  // open What's new: every version's notes, or only those after seen_version (news_since)
    bool   news_since;
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

// Which track of a saved file a stream ends up on (docs/SPEC.md, "Track layout in every saved file"). Follows the
// app's category, so it's worked out again whenever the settings change.
enum AudioTrack : u8 {
    TRACK_MIC,
    TRACK_COMMUNICATION,
    TRACK_GAME, // its own track, before other apps
    TRACK_APP,  // its own track
};

struct AudioLive {
    u32        id;       // the audio worker's stream number, unique while Rekodr runs
    AudioKind  kind;
    AudioTrack track;
    Str64      exe;      // lower-case exe, "mic" for the mic
    char       path[MAX_PATH]; // its exe's full path (UTF-8), "" for the mic
};

struct Audio {
    AudioLive streams[64]; // live streams in the order they started
    i32       stream_count;
    Str64     game;        // the game being played (lower-case exe); empty on the desktop
    u32       game_pid;    // its process, when its exe is one several games share (javaw.exe); 0 = not known
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

// The arrays are on the heap and grow to hold the whole buffer (buffer.cpp); buffer_idx_free lets go of them. A
// zeroed BufferIdx is an empty one.
struct BufferIdx {
    VideoRun        *runs;
    i32             run_count, run_cap;
    VideoSeg        *segs; // every run's segments, oldest first
    i32             seg_count, seg_cap;
    AudioStreamInfo *streams;
    i32             stream_count, stream_cap;
    AudioSeg        *audio_segs; // grouped by stream (first_seg, seg_count)
    i32             audio_seg_count, audio_seg_cap;
    ActivityEntry   activity[4096];
    i32             activity_count;
    i64             oldest_us, newest_us; // video footage span (0 if none)
    i64             video_written_us;     // last write of the newest run's list.csv: recent = recording now
};

// save.cpp: turning ranges of the buffer into files (clips, sessions; later Keep and Share), one ffmpeg at a time
// on a worker thread. `jobs` is what the UI shows; the worker reports into it through EV_SAVE events.
enum SaveKind : u8 {
    SAVE_CLIP,    // a range of the buffer into Clips\<year>
    SAVE_SESSION, // a range of the buffer into Sessions\<year>
    SAVE_KEEP,    // the editor's Keep: an edit of a saved file, at full quality, into Clips\<year>
    SAVE_SHARE,   // the editor's Share: a small copy with one stereo mix, into the share folder
    SAVE_PREVIEW, // a stretch of the buffer into the local folder, for the timeline's Play; not a saved file
    SAVE_LINK,    // a copy for a link: H.264 with the Mix only, played from the start while it loads, into the link folder
    SAVE_RENDER,  // an edited clip's playable copy, made as Keep makes it, into Data\Rendering (render.cpp)
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
    i32  cam_w, cam_h;                 // the webcam stream's size (CAM_STREAM_TITLE); 0 when there's none
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
// file's audio track t (a Rekodr file's Mix has none: it's rebuilt), EDIT_CAM_TRACK the webcam, laid over the
// picture. Items on one track never overlap; time with no item plays as black or silence (or no webcam).
enum EditMode : u8 { EDIT_SIMPLE, EDIT_ADVANCED };
enum { EDIT_MAX_ITEMS = 128, EDIT_MAX_ITEM_KEYS = 512, EDIT_CAM_TRACK = MEDIA_MAX_TRACKS + 1 };

struct EditItem {
    f64 start;             // on the sequence, seconds
    f64 from, to;          // seconds into the video
    f32 fade_in, fade_out; // from and to black (or silence) at its ends, seconds
    f32 cross;             // a crossfade from the item before it on its track, when that one ends where this starts
    u16 id;                // its own number, which its keys name (never 0)
    u16 link;              // items with the same link (not 0) move, trim and split together
    u8  track;
    f32 cam_x, cam_y;      // on EDIT_CAM_TRACK: the webcam's centre and width, as parts of the picture's width and
    f32 cam_w;             // height
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

// Layers (layers.cpp): samples from a source on the wall clock, kept beside the buffer and written into each clip
// Spans are Text's shape (a start, an end, a label) drawn as a band of named stretches instead of captions.
enum LayerKind : u8 { LAYER_VALUE, LAYER_TEXT, LAYER_INPUT, LAYER_SPANS, LAYER_KINDS };
enum { LAYER_MAX = 16, LAYER_NAME_MAX = 32, LAYER_UNIT_MAX = 16, LAYER_TEXT_MAX = 512 };
struct LayerSample {
    i64   t_us;   // when: the wall clock in the buffer, the time into the video in a clip's layers
    i64   end_us; // Text, Spans: when it ends, on the same clock; 0 in the buffer = it holds until the next one
    f64   value;  // Value: the number; Input: 1 down, 0 up
    u32   code;   // Input: the key or button
    char *text;   // Text, Spans: the line or label, UTF-8, owned by the layer; "" = nothing there
};
struct Layer {
    char         name[LAYER_NAME_MAX];  // unique: "Front window", or "<plugin folder>/<id>"
    char         label[LAYER_NAME_MAX]; // what the player sees
    char         unit[LAYER_UNIT_MAX];  // Value: "bpm", or ""
    f64          lo, hi;                // Value: the range its colours run over; equal = taken from the samples
    u8           kind;                  // LayerKind
    i32          count, cap;
    LayerSample *s; // oldest first
};
struct LayerSet { // owns its samples: layer_set_free
    i32   count;
    Layer layers[LAYER_MAX];
};
static void layers_prune(i64 before_us); // drops what is older than the buffer; main thread
static i32  layer_register(const char *name, LayerKind kind, const char *label, const char *unit, f64 lo, f64 hi);
static bool layer_hold(i32 id, i64 t_us, const char *text); // a Text or Spans line until the next one
static void layers_seen(i64 at);       // the sources are watching at `at`; held lines end there if Rekodr stops
static void layers_close_held(i64 at); // ends every held line at `at` (sleep, session end, quitting, a stall)
static LONG layers_closed();           // changes each time held lines are ended: hold a fresh one

// An edit never changes the original: it's this small record, kept in Data\<video name>.edit.txt.
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
    char     layer_on[LAYER_MAX][LAYER_NAME_MAX]; // the layers drawn into the picture, by name
    i32      layer_on_count;
};

// The editor's keyed lanes: the zoom (0), each volume slot (1 + slot), then each colour (ED_COLOR + colour).
enum { ED_COLOR = EDIT_MASTER + 2, ED_LANES = ED_COLOR + EDIT_COLORS };

// A Keep, Share or Link job: everything the save worker needs, copied with the request.
struct EditJob {
    Path       src;       // the original
    Path       out_dir;   // Clips\<year> for Keep, the share folder for Share, the link folder for Link
    char       stem[256]; // the output's name without ".mp4" (made unique if taken)
    MediaInfo  info;
    EditRecord edit;
    i64        aim_bytes, limit_bytes; // Share and Link: the size made for and the most it may be (0 = no most); a
                                       // Link with no aim is made at about the source's quality
    i32        max_h;     // Link: the picture made at most this tall and 1920 wide (link_fit_size), 0 = as it is
    f32        end_card;  // seconds of "Continues on the page" after the picture (save_social_copy's preview), 0 = none
    bool       tracks;    // Link: every sound track kept as Keep keeps them, nothing made louder (a shared session)
    bool       quiet;     // no sound or popup when it ends: Share's own copies, whose sound share.cpp plays
};

// The rows of Share's bitrate table (share_kbps) and what they start at: YouTube's recommended SDR upload
// bitrates (support.google.com/youtube/answer/1722171) in kbit/s, for 24-30 fps and 48-60 fps, at the low end
// of its ranges for 2160p.
static const i32 SHARE_KBPS_HEIGHTS[6] = {360, 480, 720, 1080, 1440, 2160};
static const i32 SHARE_KBPS_DEFAULT[6][2] = {{1000, 1500}, {2500, 4000},   {5000, 7500},
                                             {8000, 12000}, {16000, 24000}, {35000, 53000}};

// share.cpp: Share. What a clip (its edit, when it has one) needs to look right is its recommended size. Under
// share_max_mb Share makes a file at that size; when share_min_quality of it fits the max, a file squeezed to the
// max; further over, a link. The link's copy follows the same rule against its host. Length, picture size and frame
// rate never change, only the bitrate; files and links are H.264.
enum LinkHost : u8 { HOST_R2, HOST_CUSTOM, HOST_COUNT };
enum ShareKind : u8 { SHARE_AS_IS, SHARE_SQUEEZED, SHARE_LINK };
// Why a link can't be made with no bucket and no uploader on, and what to do.
static const char SHARE_NO_BUCKET[] = "Sign in with Cloudflare in Options to share links";

struct ShareDecision {
    ShareKind kind;            // what Share does with one click
    i64       reco_bytes;      // the recommended size
    i64       max_bytes;       // share_max_mb in bytes
    // File: the file Share makes
    bool      file_ok;         // false: too far over the max even squeezed
    bool      file_squeezed;   // squeezed to the max, else at the recommended size
    i64       file_bytes;      // about what it comes out at
    // Link: where the link goes and the copy made for it
    bool      link_ok;         // false: no host takes it (link_why)
    LinkHost  host;
    i64       link_bytes;      // about what the copy comes out at
    char      host_label[48];  // "your bucket", an uploader's name
    char      why[96];         // the menu's line for the one-click choice: "18 MB, as is", "28 MB squeezed to 20 MB"
    char      link_why[96];    // why there's no link: "Sign in with Cloudflare in Options to share links"
};

// What Share would do with `m` played as `edit` (0: the whole file). Cheap: a menu can ask each frame.
static ShareDecision share_decide(const MediaInfo &m, const EditRecord *edit);

// A host the player can pick for a link, and how the clip goes there.
struct ShareHostChoice {
    char     name[64];  // for share_start: "bucket" or an uploader's name (link_uploaders)
    char     label[48]; // "your bucket", the uploader's name
    LinkHost host;
    bool     fits;      // the host takes the clip as it is
    i64      bytes;     // about what the copy comes out at, 0 when it doesn't fit
    char     why[96];   // when it doesn't fit: "Bigger than your bucket's 10 GB: raise it in Options"
};

// Every host that's set up or switched on, the bucket first, then the uploaders, with whether `m` played as `edit`
// fits there. Cheap: a menu can ask each frame. Returns how many.
static i32 share_host_choices(const MediaInfo &m, const EditRecord *edit, ShareHostChoice *out, i32 max);

// A Share under way or done. File: made (or found already made) and put on the clipboard as a file. Link: made,
// uploaded and its address put on the clipboard. The library, the editor and the toast all follow the same id.
enum ShareState : u8 { SHARE_MAKING, SHARE_UPLOADING, SHARE_DONE, SHARE_FAILED };
struct ShareJobView {
    u32        id;             // 0: no such job
    ShareKind  kind;
    ShareState state;
    f32        progress;       // 0..1 in the current state
    char       verb[32];       // "Compressing for upload", "Compressing", "Making share copy", "Uploading"; "" once done
    bool       reused;         // an earlier one was given again, nothing made
    char       path[MAX_PATH * 3]; // the file (UTF-8), for a link the copy uploaded
    char       url[512];       // a link's address
    char       error[256];
};

// Starts Share for the file at `path`. `edit` 0 shares the whole file; the editor passes its own record. `name` is
// what the file or link is called, `lib_key` the library card (0 = none). `host` (a ShareHostChoice's name) makes
// a link on that host, squeezed to it as the rule says, or fails with why it can't; 0 lets Share's rule pick.
// A link made before on the same host for the same clip and edit is given again. `quiet` makes the file and keeps
// it for the next Share without putting it on the clipboard. Returns the job, 0 when it couldn't start
// (share_job(0).error doesn't exist: the caller says "busy, try again").
static u32 share_start(ShareKind kind, const wchar_t *path, const MediaInfo &m, const EditRecord *edit,
                       const char *name, const char *game, u64 lib_key, const char *host = 0, bool quiet = false);
static ShareJobView share_job(u32 id);
static void share_cancel(u32 id);

// Per library clip. Every share file and link copy is made for the clip as edited, with one sound track; a new
// edit or new settings make the next Share a new one. A made file is given again for share_keep_days, a link while
// it plays; a link that plays for the clip as it is goes on the clipboard at once, whatever the settings.
struct ClipShareState {
    bool     file;        // a share file for the clip's edit and the settings as they are, still on disk
    bool     link;        // a link that plays or is paused (share_clip_link)
    bool     link_ready;  // a link that plays, made from the clip as it is now: Share copies it at once
    u8       link_paused; // 0 plays, else as R2Share.paused
    LinkHost host;        // the link's host
};
// Cheap enough for every card each frame.
static ClipShareState share_clip_state(u64 lib_key, const wchar_t *path, const MediaInfo &m, const EditRecord *edit);
struct R2Share;
// The clip's newest link that plays or is paused, for its menu row (r2_pause, r2_resume, r2_delete take its key).
static bool share_clip_link(u64 lib_key, R2Share *out);
// Deleting a clip: how many of its share files and links go with it, and in `cant` the links that stay up ("1
// uploader link can't be removed and stays up"; "" for none). share_clip_delete deletes them; the clip is the caller's.
static i32 share_clip_delete_preview(u64 lib_key, char *cant, i32 cap);
static void share_clip_delete(u64 lib_key);

struct SaveJobView {
    u32       id; // 0 = free slot
    SaveKind  kind;
    SaveState state;
    i64       from_us, to_us;
    bool      trimmed;   // from_us was moved later because the buffer didn't reach back that far
    bool      quiet;     // EditJob.quiet: ends with no sound or popup
    f32       progress;  // 0..1 while running
    char      name[256]; // the name asked for; empty = named after the game played most
    char      path[512]; // the output file once known (UTF-8)
    char      game[96];  // the game played most in it, once known; empty for none or when it isn't known
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
    Path config_dir; // %APPDATA%\Rekodr (or %REKODR_HOME%\config)
    Path local_dir;  // %LOCALAPPDATA%\Rekodr (or %REKODR_HOME%\local): log, thumbnail cache
    Path settings;   // config_dir\settings.txt
    Path share_dir;  // %TEMP%\Rekodr\share
    Path link_dir;   // %TEMP%\Rekodr\link: copies made for a link, until they're uploaded
};

// Screens, one struct each, owned by the file of the same name (included from ui.cpp).

// player.cpp: one video through Media Foundation's Media Engine into a texture on the UI's device. The library and
// the editor each have one; only one is open at a time.
enum { PLAYER_MAX_TRACK_ENGINES = 8 };

struct Player {
    bool              open;
    Path              path;
    u8                id;  // which player (0 library, 1 editor, 2 timeline, 3 card preview, 4 the editor's webcam): media events carry it
    u32               gen; // bumped on every open, so events from a closed file are dropped
    IMFMediaEngine   *engine;
    IMFMediaEngineEx *engine_ex;
    // The editor plays each audio track on its own audio-only engine, so each has its own volume. The main engine
    // is muted then.
    IMFMediaEngine   *track_engine[PLAYER_MAX_TRACK_ENGINES];
    i32               track_of_engine[PLAYER_MAX_TRACK_ENGINES]; // the audio track each one plays
    i32               track_engine_count;
    f64               track_synced[PLAYER_MAX_TRACK_ENGINES]; // ImGui time each was last started or moved
    bool              tracks_free; // the track engines don't follow the video: the editor moves each (Advanced)
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
    i16       year;      // the year folder it's in (Clips\<year> or Sessions\<year>)
    LibView   kind;      // in Clips or Sessions
    bool      probed;    // `info` is known
    u64       size;
    i64       mtime_us;
    i64       when_us;   // when it's from: the time in its name (by the template in Options), else mtime_us
    u64       key;       // thumbnail cache key; changes when the file does, not when it's renamed
    bool      rendered;    // a clip whose original is in Clips\<year>\Source, its edit rendered in Clips (render.cpp);
                           // name, size, times, key and info are the original's
    u64       render_size; // the rendered copy's size, 0 = there's none in Clips yet
    u8        dir;         // 0 = in the save folder; else in another app's folder, g_lib_dirs[dir - 1] (year is
                           // then only the year of its time)
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
    u32        keep_job;        // the last Keep's save job
    u32        share_job;       // the last Share's job (share_start)
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
    // Advanced (adv_editor.cpp): the playhead on the sequence, moved on by the picture's engine while an item plays and
    // by the clock over a gap
    f64        adv_u;
    bool       adv_playing;
    f64        adv_clock;       // ImGui time adv_u was last moved on
    i32        adv_vitem;       // the item the picture's engine is on, -1 none, -2 to be moved there again
    i32        adv_titem[PLAYER_MAX_TRACK_ENGINES]; // the same for each track engine
    u16        adv_sel[EDIT_MAX_ITEMS]; // the selected items' ids
    i32        adv_sel_count;
    u8         adv_drag;        // what the mouse is doing on the Advanced timeline (ADV_DRAG_*)
    u16        adv_drag_id;     // the item it took hold of
    f64        adv_drag_u;      // where on the sequence it was pressed
    bool       adv_ask_simple;  // asking before going back to Simple loses something
    i32        adv_key;         // the selected item key (an index into edit.ikeys), -1 none
    i32        adv_drag_key;    // the key being dragged, as an index into drag_from.ikeys
    // The webcam layer (adv_editor.cpp): the clip's webcam stream copied to a file of its own, shown by its own engine
    Player     cam;
    struct CamCopy *cam_copy;
    i32        adv_citem;       // the item the webcam's engine is on, as adv_vitem
    f64        cam_synced;      // ImGui time the webcam's engine was last moved
    f32        cam_k[4];        // the kept frame on screen as last drawn (x0 y0 x1 y1), which the webcam is placed in
    bool       cam_hot;         // the mouse is on the webcam in the picture, or dragging it
    u8         cam_drag;        // 0 none, 1 moving the webcam, 2 sizing it
    f32        cam_drag_mx, cam_drag_my; // where the drag began
    f64        cam_drag_x, cam_drag_y, cam_drag_w; // the webcam's place then
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
    i16           stream_row[16384]; // the row of each idx->streams entry, -1 = none
    TimelineFrame frames[48]; // the preview and the frames along the video row
    u32           frame_clock;
    i64           pending_key, failed_key, shown_key;
    char          session_name[128];
    u32           job;          // the last save started here
    // Delete footage (erase.cpp): the selection asked about, then deleted
    bool          erase_ask;    // open the confirm next frame
    bool          erase_open;   // the confirm is showing: the selection is drawn red
    u8            erase_step;   // TL_ERASE_*
    bool          job_edit;     // open the editor when it's done
    // Share (timeline.cpp): the range saved as a clip (job), read, then shared (share, a share_start job)
    u8            share_step;       // TL_SHARE_*
    char          share_host[64];   // the host picked from the arrow; empty = Share's rule
    u32           share;
    char          share_line[160];  // what it's doing, or how it ended
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
// sessions.cpp: detecting sessions, the save popup, unsaved sessions (docs/SPEC.md, "1. Whole sessions").
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
    u32          pid;              // the game's process for an exe several games share (javaw.exe), 0 = not known
                                   // yet; in memory only, so after a restart the first one seen is taken
    i64          carried_us;       // open across a restart on such an exe: when it was last seen before it, until the exe is
                                   // named again (0 = not carried); in memory only
    char         name[128];        // shown, and names the file: "Overwatch", "Stream with Sam"
    i64          start_us, end_us; // the real start and end; while open, end is when the game was last seen
    u32          job;              // the save job while SAVING
    char         video[256];       // the saved file's name once SAVED
    char         record[256];      // its record under save_dir, "Data\<name>.session.txt"; empty = none
    char         display_name[128]; // typed by hand; shown as its title, the name and times kept under it
    bool         seen;             // its row was drawn on the Sessions page, or Review was pressed on its toast
                                   // (session_seen); until then it counts as new (sessions_unseen_count)
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
    OPT_APPS, OPT_RECORDING, OPT_HOTKEYS, OPT_INTEGRATIONS, OPT_AUTOMARKS, OPT_GENERAL,
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

// Whether a marker's detail has local=1: the marker stays on this PC, out of chapters, shared pages and cards.
static bool marker_local(const char *detail)
{
    return !strncmp(detail, "local=1", 7) || strstr(detail, ";local=1");
}

struct Marker {
    i64  t_us;      // when, on the wall clock; no two markers share one, so it also says which marker
    u8   colour;    // an index into MARKER_COLOURS; 0 = none
    char note[256]; // UTF-8, one line
    char detail[256]; // more about it, kept on this PC only: key=value pairs (markers.cpp, marker_detail_add)
};

struct MarkerColour {
    const char *name;
    u32         rgb;
};
static const MarkerColour MARKER_COLOURS[] = {
    {"", 0x969696}, // none: grey
    {"red", 0xf0434f}, {"orange", 0xf08a3c}, {"yellow", 0xe8c53d}, {"green", 0x3fbf74}, {"blue", 0x5b9cff}, {"purple", 0xb07cff},
};

static i32 markers_in(i64 from, i64 to, Marker *out, i32 cap); // the ones in [from, to], oldest first; any thread

// Markers from games (automarks.cpp)
// What Options says of a game's feed: off, waiting for the game, not installed, or its port taken.
enum AmState { AM_OFF, AM_READY, AM_NOT_FOUND, AM_PORT_BUSY };
static void automarks_apply();
static AmState automarks_state(i32 game);
static void automarks_uninstall();
// The keys Rekodr binds in Rust's keys.cfg to print the combat log (Config.rust_binds): one Rekodr presses on a death
// (F15) and Space, which the player presses
enum { RUST_BIND_DEATH, RUST_BIND_SPACE, RUST_BIND_COUNT };
static const char *const RUST_BIND_WORDS[RUST_BIND_COUNT] = {"death", "space"};
// A bind's state for Options: off, in keys.cfg, waiting for Rust to close to go in or out, or not in and not waiting
enum { RUST_SHOW_OFF, RUST_SHOW_BOUND, RUST_SHOW_PENDING, RUST_SHOW_UNBIND, RUST_SHOW_NOT_BOUND };
struct RustBindView {
    i32  show;      // RUST_SHOW_*
    char key[16];   // the key, as keys.cfg names it
    char line[160]; // the bind line to paste in the game's console
};
static void rust_binds_sync();                        // rustlog.cpp
static void rust_bind_view(i32 bind, RustBindView *v); // rustlog.cpp
static void achievements_apply(); // achievements.cpp

// Webcam (webcam.cpp). A saved clip carries the webcam as its second video stream, titled CAM_STREAM_TITLE.
#define CAM_STREAM_TITLE "Webcam"
static void webcam_run(wchar_t *out, i32 cap); // the run folder being written, "" when none
static bool webcam_devices(char (*out)[128], i32 cap, i32 *n);
static void webcam_list_again();
static bool webcam_recording();
static void webcam_error(char *out, i32 cap);
static ID3D11ShaderResourceView *webcam_picture();
