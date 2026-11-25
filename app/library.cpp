// Library: the saved clips and sessions, found under <save_dir>\{Clips,Sessions}\<year>, and the player.
//
// Two worker threads do the slow parts: the scan (listing the folders, ffprobe for files not seen before) and the
// thumbnails (ffmpeg makes a strip of 24 frames per file, cached under local_dir\thumbs, decoded with WIC), and the
// unsaved sessions' posters (one frame of the buffer each, cached under thumbs\posters). They
// share only LibShared below, under its lock, and post EV_LIBRARY events; the main thread copies their results
// into g_app.library and turns pixels into textures. The cache is keyed by a file's size and time, so a rename
// keeps its thumbnail.
//
// Everything on screen is fixed-size: a paged grid that fits the window (the wheel turns pages), paged game chips,
// and a pager row whose controls never move.

static void editor_open(const LibItem &item); // editor.cpp

// Detailed log: how long after a switch between Clips and Sessions every card and tile on screen has its picture.
static i64 g_lib_switch_us;
static i32 g_lib_missing; // pictures on screen still missing this frame
static i64 g_lib_first_us; // the first frame drawn after the switch

enum LibTaskKind : u8 { LT_SCAN, LT_THUMB, LT_STRIP, LT_POSTER };
enum { LIB_STRIP_FRAMES = 24 };

struct LibTask {
    LibTaskKind kind;
    u64         key;
    f64         duration;
    Path        path;   // LT_POSTER: the buffer segment, empty when its poster is on disk
    i32         poster; // which frame of the strip a card shows
};

struct LibResult {
    u64  key;
    bool strip, poster, failed; // poster: an unsaved session's, keyed by its buffer segment's start
    i32  w, h, frames; // w and h of the whole image
    u32 *pixels;       // BGRA, HeapAlloc'd by the worker, freed by the main thread
};

struct LibShared {
    SRWLOCK            lock;
    CONDITION_VARIABLE wake;
    LibTask            tasks[256];
    i32                task_count;
    bool               scan_running, scan_queued;
    Path               save_dir, cache_dir, ffmpeg; // copied with every scan request
    Path               buffer_dir, watch[WATCH_MAX];
    i32                watch_count;
    char               name_template[128];
    LibItem            items[LIB_MAX_ITEMS];        // the newest scan's published list
    i32                item_count;
    u32                items_gen;
    LibResult          results[64];
    i32                result_count;
    HANDLE             threads[2];
};

static LibShared g_lib;
static LibItem   g_scan_items[LIB_MAX_ITEMS]; // the scan's own list (one scan runs at a time)

static Path lib_cache_file(const Path &cache, u64 key, const wchar_t *ext)
{
    wchar_t name[64];
    swprintf(name, COUNT(name), L"c-%016llx%s", key, ext);
    return path_join(cache, name);
}

static void game_from_name(const char *tmpl, const char *name, char *out, i32 cap);

// ---------------------------------------------------------------------------------------------------------------
// Workers

// The probe cache: key = value lines next to the strip.
static bool lib_cache_read(const Path &cache, u64 key, MediaInfo *m, char *tagged, i32 tagged_cap)
{
    FILE *f = _wfopen(lib_cache_file(cache, key, L".txt").s, L"rb");
    if (!f) return false;
    *m = {};
    tagged[0] = 0;
    char line[512];
    bool ok = false, bad = false;
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = trim(line), *v = trim(eq + 1);
        if (!strcmp(k, "width")) m->width = atoi(v);
        else if (!strcmp(k, "height")) m->height = atoi(v);
        else if (!strcmp(k, "fps")) m->fps = atof(v);
        else if (!strcmp(k, "tagged")) snprintf(tagged, tagged_cap, "%s", v);
        else if (!strcmp(k, "track") && m->track_count < MEDIA_MAX_TRACKS) { // track = <channels> <title>
            char *title = 0;
            i32 ch = (i32)strtol(v, &title, 10);
            if (ch < 1 || ch > 8 || *title != ' ') bad = true; // not this layout: probed again
            m->channels[m->track_count] = (u8)ch;
            str_set(m->tracks[m->track_count++], title + (*title == ' '));
        }
        else if (!strcmp(k, "duration")) { m->duration = atof(v); ok = true; }
    }
    fclose(f);
    return ok && !bad && m->duration > 0;
}

static void lib_cache_write(const Path &cache, u64 key, const MediaInfo &m, const char *tagged)
{
    Path final_path = lib_cache_file(cache, key, L".txt"), tmp = lib_cache_file(cache, key, L".txt.tmp");
    FILE *f = _wfopen(tmp.s, L"wb");
    if (!f) return;
    fprintf(f, "duration = %.3f\nwidth = %d\nheight = %d\nfps = %.3f\ntagged = %s\n", m.duration, m.width, m.height,
            m.fps, tagged);
    for (i32 t = 0; t < m.track_count; t++) fprintf(f, "track = %d %s\n", m.channels[t], m.tracks[t]);
    fclose(f);
    MoveFileExW(tmp.s, final_path.s, MOVEFILE_REPLACE_EXISTING);
}

static f64 parse_rate(const char *s)
{
    f64 a = atof(s);
    const char *slash = strchr(s, '/');
    f64 b = slash ? atof(slash + 1) : 1;
    return b > 0 ? a / b : 0;
}

// ffprobe's view of a file: length, size, frame rate, the audio track titles and our tags.
static bool lib_probe(const Path &ffprobe, const Path &file, MediaInfo *m, char *tagged, i32 tagged_cap)
{
    static thread_local char out[65536];
    static thread_local wchar_t args[2048];
    args[0] = 0;
    static const char *const head[] = {"-v", "error", "-show_entries",
                                       "format=duration:format_tags=genre,comment:stream=codec_type,width,height,"
                                       "avg_frame_rate,channels:stream_tags=title,handler_name",
                                       "-of", "default"};
    arg_list(args, COUNT(args), head, COUNT(head));
    cmd_arg(args, COUNT(args), file.s);
    i32 code = proc_run(ffprobe.s, args, 20000, out, sizeof out);
    *m = {};
    tagged[0] = 0;
    if (code != 0) return false;
    char genre[64] = {}, comment[128] = {};
    bool in_audio = false, in_video = false, have_video = false;
    char title[64] = {}, handler[64] = {};
    i32 channels = 2;
    auto end_stream = [&]() {
        if (in_audio && m->track_count < MEDIA_MAX_TRACKS) {
            char *t = m->tracks[m->track_count];
            if (title[0]) snprintf(t, 32, "%s", title);
            else if (handler[0] && _stricmp(handler, "SoundHandler")) snprintf(t, 32, "%s", handler);
            else snprintf(t, 32, "Track %d", m->track_count + 1);
            m->channels[m->track_count++] = (u8)channels;
        }
        in_audio = in_video = false;
        title[0] = handler[0] = 0;
        channels = 2;
    };
    char *ctx = 0;
    for (char *line = strtok_s(out, "\r\n", &ctx); line; line = strtok_s(0, "\r\n", &ctx)) {
        if (!strcmp(line, "[/STREAM]")) { end_stream(); continue; }
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        const char *k = line, *v = eq + 1;
        if (!strcmp(k, "codec_type")) { in_audio = !strcmp(v, "audio"); in_video = !strcmp(v, "video"); }
        else if (!strcmp(k, "width") && in_video && !have_video) m->width = atoi(v);
        else if (!strcmp(k, "height") && in_video && !have_video) m->height = atoi(v);
        else if (!strcmp(k, "avg_frame_rate") && in_video && !have_video) { m->fps = parse_rate(v); have_video = true; }
        else if (!strcmp(k, "channels")) channels = atoi(v) >= 1 && atoi(v) <= 8 ? atoi(v) : 2;
        else if (!strcmp(k, "TAG:title")) snprintf(title, sizeof title, "%s", v);
        else if (!strcmp(k, "TAG:handler_name")) snprintf(handler, sizeof handler, "%s", v);
        else if (!strcmp(k, "duration")) m->duration = atof(v);
        else if (!strcmp(k, "TAG:genre")) snprintf(genre, sizeof genre, "%s", v);
        else if (!strcmp(k, "TAG:comment")) snprintf(comment, sizeof comment, "%s", v);
    }
    if (starts_with(comment, "rekodr") && genre[0]) snprintf(tagged, tagged_cap, "%s", genre);
    return m->duration > 0;
}

static int lib_item_newer(const void *a, const void *b)
{
    i64 x = ((const LibItem *)a)->when_us, y = ((const LibItem *)b)->when_us;
    return x > y ? -1 : x < y ? 1 : 0;
}

static void lib_publish(const LibItem *items, i32 n)
{
    AcquireSRWLockExclusive(&g_lib.lock);
    memcpy(g_lib.items, items, n * sizeof items[0]);
    g_lib.item_count = n;
    u32 gen = ++g_lib.items_gen;
    ReleaseSRWLockExclusive(&g_lib.lock);
    Event e = {};
    e.kind = EV_LIBRARY;
    e.a = LIB_EV_SCANNED;
    e.b = gen;
    event_post(e);
}

// Games given by hand, for files that carry no tag of their own (renamed ones, or videos Rekodr didn't make):
// <config_dir>\games.txt, a line each of "<key in hex> | <game> | <file name>". The key is the item's (its size and
// time), so a rename keeps the game; the name is only there for people reading the file. Read at every scan, by
// the scan's thread.
struct GameOverride {
    u64  key;
    char game[64];
};
static GameOverride g_game_overrides[LIB_MAX_ITEMS];
static i32          g_game_override_count;

// Display names typed on cards: <config_dir>\names.txt, lines of "<key in hex> | <name> | <file name>" like games.txt.
struct NameNote {
    u64  key;
    char text[128];
};
static NameNote g_names[LIB_MAX_ITEMS];
static i32         g_name_count;

static void lib_overrides_load()
{
    g_game_override_count = 0;
    FILE *f = _wfopen(path_join(g_app.paths.config_dir, L"games.txt").s, L"rb");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f) && g_game_override_count < LIB_MAX_ITEMS) {
        GameOverride o = {};
        char game[64] = "";
        if (sscanf(line, "%llx | %63[^|\r\n]", &o.key, game) != 2) continue;
        char *g = trim(game);
        if (!*g || !o.key) continue;
        snprintf(o.game, sizeof o.game, "%s", g);
        g_game_overrides[g_game_override_count++] = o;
    }
    fclose(f);
}

static void lib_names_load()
{
    g_name_count = 0;
    FILE *f = _wfopen(path_join(g_app.paths.config_dir, L"names.txt").s, L"rb");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f) && g_name_count < LIB_MAX_ITEMS) {
        NameNote c = {};
        char text[128] = "";
        if (sscanf(line, "%llx | %127[^|\r\n]", &c.key, text) != 2) continue;
        char *t = trim(text);
        if (!*t || !c.key) continue;
        str_set(c.text, t);
        g_names[g_name_count++] = c;
    }
    fclose(f);
}

static void lib_set_display_name(LibItem *it)
{
    for (i32 i = 0; i < g_name_count; i++)
        if (g_names[i].key == it->key) {
            str_set(it->display_name, g_names[i].text);
            return;
        }
}

// Comments on clips and sessions: <config_dir>\comments.txt holds a line "<key in hex> | <file name> | <comment>" for
// each, keyed as games.txt is. The comment goes last so it can hold '|'; its line breaks are written \n and its
// backslashes \\. Loaded on the UI thread with the folders, and only read there.
enum { LIB_COMMENT_MAX = 4000 };
struct ClipComment {
    u64   key;
    char  name[256];
    char *text; // malloc'd
};
static ClipComment *g_clip_comments;
static i32          g_clip_comment_count, g_clip_comment_cap;

// A file's comment, or "".
static const char *lib_comment(u64 key)
{
    for (i32 i = 0; i < g_clip_comment_count; i++)
        if (g_clip_comments[i].key == key) return g_clip_comments[i].text;
    return "";
}

// Sets (or, with an empty text, takes off) a file's comment in memory.
static void lib_comment_put(u64 key, const char *name, const char *text)
{
    i32 i = 0;
    while (i < g_clip_comment_count && g_clip_comments[i].key != key) i++;
    if (!*text) {
        if (i == g_clip_comment_count) return;
        free(g_clip_comments[i].text);
        g_clip_comments[i] = g_clip_comments[--g_clip_comment_count];
        return;
    }
    if (i == g_clip_comment_count) {
        if (g_clip_comment_count == g_clip_comment_cap) {
            g_clip_comment_cap = g_clip_comment_cap ? 2 * g_clip_comment_cap : 64;
            g_clip_comments = (ClipComment *)realloc(g_clip_comments, g_clip_comment_cap * sizeof *g_clip_comments);
        }
        g_clip_comments[g_clip_comment_count++] = {key, "", 0};
    }
    ClipComment &cc = g_clip_comments[i];
    str_set(cc.name, name);
    free(cc.text);
    cc.text = _strdup(text);
}

static void lib_comments_load()
{
    for (i32 i = 0; i < g_clip_comment_count; i++) free(g_clip_comments[i].text);
    g_clip_comment_count = 0;
    FILE *f = _wfopen(path_join(g_app.paths.config_dir, L"comments.txt").s, L"rb");
    if (!f) return;
    static char line[4 * LIB_COMMENT_MAX];
    while (fgets(line, sizeof line, f)) {
        unsigned long long key = 0;
        char name[256] = "";
        i32 at = 0;
        if (sscanf(line, "%llx | %255[^|] |%n", &key, name, &at) != 2 || !at || !key) continue;
        char text[LIB_COMMENT_MAX + 1];
        i32 n = 0;
        const char *s = line + at;
        if (*s == ' ') s++;
        for (; *s && *s != '\r' && *s != '\n' && n < LIB_COMMENT_MAX; s++) {
            if (*s == '\\' && s[1] == 'n') text[n++] = '\n', s++;
            else if (*s == '\\' && s[1] == '\\') text[n++] = '\\', s++;
            else text[n++] = *s;
        }
        text[n] = 0;
        lib_comment_put(key, trim(name), text);
    }
    fclose(f);
}

static bool lib_comments_save()
{
    FILE *f = _wfopen(path_join(g_app.paths.config_dir, L"comments.txt").s, L"wb");
    if (!f) return false;
    for (i32 i = 0; i < g_clip_comment_count; i++) {
        const ClipComment &cc = g_clip_comments[i];
        fprintf(f, "%llx | %s | ", (unsigned long long)cc.key, cc.name);
        for (const char *s = cc.text; *s; s++) {
            if (*s == '\n') fputs("\\n", f);
            else if (*s == '\\') fputs("\\\\", f);
            else if (*s != '\r') fputc(*s, f);
        }
        fputs("\r\n", f);
    }
    return fclose(f) == 0;
}

// Tags on clips and sessions ("ace", "funny"), found by search: <config_dir>\tags.txt holds a line
// "<key in hex> | <file name> | ace,funny" for each, keyed as comments.txt is. A tag is lower case and holds no comma
// or '|'. Loaded on the UI thread with the comments, and only read there.
enum { LIB_TAGS_MAX = 256, LIB_TAG_MAX = 32 };
struct ClipTags {
    u64  key;
    char name[256];
    char tags[LIB_TAGS_MAX]; // comma-separated
};
static ClipTags *g_clip_tags;
static i32       g_clip_tag_count, g_clip_tag_cap;

// A file's tags, comma-separated, or "".
static const char *lib_tags(u64 key)
{
    for (i32 i = 0; i < g_clip_tag_count; i++)
        if (g_clip_tags[i].key == key) return g_clip_tags[i].tags;
    return "";
}

// Is tag one of tags (a comma-separated list)? An empty tag matches any file with a tag.
static bool lib_tag_has(const char *tags, const char *tag)
{
    if (!*tag) return *tags != 0;
    size_t n = strlen(tag);
    for (const char *s = tags; *s;) {
        const char *e = strchr(s, ',');
        size_t len = e ? (size_t)(e - s) : strlen(s);
        if (len == n && !_strnicmp(s, tag, n)) return true;
        if (!e) break;
        s = e + 1;
    }
    return false;
}

// Sets (or, with no tags, takes off) a file's tags in memory.
static void lib_tags_put(u64 key, const char *name, const char *tags)
{
    i32 i = 0;
    while (i < g_clip_tag_count && g_clip_tags[i].key != key) i++;
    if (!*tags) {
        if (i < g_clip_tag_count) g_clip_tags[i] = g_clip_tags[--g_clip_tag_count];
        return;
    }
    if (i == g_clip_tag_count) {
        if (g_clip_tag_count == g_clip_tag_cap) {
            g_clip_tag_cap = g_clip_tag_cap ? 2 * g_clip_tag_cap : 64;
            g_clip_tags = (ClipTags *)realloc(g_clip_tags, g_clip_tag_cap * sizeof *g_clip_tags);
        }
        g_clip_tags[g_clip_tag_count++] = {key};
    }
    str_set(g_clip_tags[i].name, name);
    str_set(g_clip_tags[i].tags, tags);
}

static void lib_tags_load()
{
    g_clip_tag_count = 0;
    FILE *f = _wfopen(path_join(g_app.paths.config_dir, L"tags.txt").s, L"rb");
    if (!f) return;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        unsigned long long key = 0;
        char name[256] = "", tags[LIB_TAGS_MAX] = "";
        if (sscanf(line, "%llx | %255[^|] | %255[^\r\n]", &key, name, tags) != 3 || !key) continue;
        lib_tags_put(key, trim(name), trim(tags));
    }
    fclose(f);
}

static bool lib_tags_save()
{
    FILE *f = _wfopen(path_join(g_app.paths.config_dir, L"tags.txt").s, L"wb");
    if (!f) return false;
    for (i32 i = 0; i < g_clip_tag_count; i++)
        fprintf(f, "%llx | %s | %s\r\n", (unsigned long long)g_clip_tags[i].key, g_clip_tags[i].name, g_clip_tags[i].tags);
    return fclose(f) == 0;
}

// A file's game: the one given by hand, else its own tag, else what its name says, else for a video Rekodr didn't
// save the game its name starts with, when that is sure (game_guess).
static bool game_guess(const char *name, bool (*known)(const char *, char *, i32), char *out, i32 cap);
static bool game_known(const char *name, char *out, i32 cap);
// The folders of other apps' videos the scans have found (Config.watch_dirs and a folder down from each, as ShadowPlay
// keeps a folder per game): LibItem.dir is an index into it, plus one. Only ever added to, by the scan's thread, so
// an item's folder stays put for the run; `live` says the newest scan went through it. `game` is the name of a
// folder below a watched one, which names its videos' game when nothing else does.
struct LibDir {
    Path path;
    char game[64];
    bool live;
};
static LibDir        g_lib_dirs[255];
static volatile LONG g_lib_dir_count;

// Folder `path`'s place in g_lib_dirs, plus one; 0 when it's full
static u8 lib_dir_add(const Path &path, const char *game)
{
    i32 n = (i32)g_lib_dir_count;
    for (i32 i = 0; i < n; i++)
        if (!_wcsicmp(g_lib_dirs[i].path.s, path.s)) {
            g_lib_dirs[i].live = true;
            return (u8)(i + 1);
        }
    if (n == COUNT(g_lib_dirs)) return 0;
    g_lib_dirs[n].path = path;
    str_set(g_lib_dirs[n].game, game);
    g_lib_dirs[n].live = true;
    InterlockedIncrement(&g_lib_dir_count);
    return (u8)(n + 1);
}

static bool lib_set_game(LibItem *it, const char *tagged, const char *tmpl)
{
    for (i32 i = 0; i < g_game_override_count; i++) {
        if (g_game_overrides[i].key != it->key) continue;
        str_set(it->info.game, g_game_overrides[i].game);
        return true;
    }
    if (tagged[0]) str_set(it->info.game, tagged);
    else game_from_name(tmpl, it->name, it->info.game, sizeof it->info.game);
    if (!tagged[0] && it->dir && g_lib_dirs[it->dir - 1].game[0]) str_set(it->info.game, g_lib_dirs[it->dir - 1].game);
    char guess[64];
    if (!strcmp(it->info.game, GAME_NOT_OURS) && game_guess(it->name, game_known, guess, sizeof guess)) str_set(it->info.game, guess);
    return false;
}

// A card's original file: Clips\<year>\<name> or Sessions\<year>\<name>, or Clips\<year>\Source\<name> when it's
// rendered; one of another app's is in its folder.
static Path lib_file_path(const Path &save_dir, const LibItem &it)
{
    wchar_t name[256], ext[8], file[272];
    utf8_to_wide(it.name, name, COUNT(name));
    utf8_to_wide(it.ext, ext, COUNT(ext));
    swprintf(file, COUNT(file), L"%s%s", name, ext);
    if (it.dir) return path_join(g_lib_dirs[it.dir - 1].path, file);
    return path_join(it.kind ? save_sessions_dir(save_dir, it.year)
                     : it.rendered ? save_source_dir(save_dir, it.year) : save_clips_dir(save_dir, it.year), file);
}

// The file that plays anywhere: Clips\<year>\<name> or Sessions\<year>\<name>, the rendered edit for a rendered
// clip.
static Path lib_playable_file(const Path &save_dir, const LibItem &it)
{
    LibItem plain = it;
    plain.rendered = false;
    return lib_file_path(save_dir, plain);
}

// The probing part of a scan, shared by its probers. Each takes the next unprobed item, probes a copy of it without
// the lock (ffprobe is the slow part) and puts it back under the lock, which the scan holds while it publishes.
enum { LIB_PROBERS = 4 };
struct LibProbeRun {
    SRWLOCK       lock;
    LibItem      *items;
    i32           n;
    volatile LONG next;
    i32           probed;
    Path          save_dir, cache, ffprobe;
    const char   *tmpl; // the save template, for the game in a name
};

static DWORD WINAPI lib_prober(void *param)
{
    LibProbeRun &run = *(LibProbeRun *)param;
    for (;;) {
        i32 i = (i32)InterlockedIncrement(&run.next) - 1;
        if (i >= run.n) return 0;
        LibItem it = run.items[i];
        if (it.probed) continue;
        char tagged[64];
        if (lib_probe(run.ffprobe, lib_file_path(run.save_dir, it), &it.info, tagged, sizeof tagged)) {
            lib_cache_write(run.cache, it.key, it.info, tagged);
        } else {
            log_line("library: couldn't probe %s%s", it.name, it.ext);
            it.info.duration = 0; // shown without a length rather than asked again and again this scan
        }
        it.probed = true;
        lib_set_game(&it, tagged, run.tmpl);
        AcquireSRWLockExclusive(&run.lock);
        run.items[i] = it;
        run.probed++;
        ReleaseSRWLockExclusive(&run.lock);
    }
}

static i64 lib_name_us(const char *tmpl, const char *name, i64 mtime_us);

// A file's key is its size and time, so a renamed file keeps its comment, folders and pictures. A copy has the same
// size and time, though, and would share all of that and a tick box with it: so each file whose key is already in
// `seen` (a table of LIB_MAX_ITEMS * 2 keys, 0 = empty) gets its name mixed in until it isn't. Returns the key used.
static u64 lib_key_unique(u64 *seen, u64 key, const char *name)
{
    enum { SLOTS = LIB_MAX_ITEMS * 2 };
    for (;;) {
        if (!key) key = 1;
        u32 i = (u32)(key % SLOTS);
        while (seen[i] && seen[i] != key) i = (i + 1) % SLOTS;
        if (!seen[i]) {
            seen[i] = key;
            return key;
        }
        key = fnv64(name, strlen(name), key);
    }
}

// Whether `dir` is `top` or inside it
static bool path_within(const wchar_t *dir, const wchar_t *top)
{
    size_t n = wcslen(top);
    while (n && top[n - 1] == L'\\') n--;
    return n && !_wcsnicmp(dir, top, n) && (!dir[n] || dir[n] == L'\\');
}

// Another app's videos: those in each watched folder and in the folders right under it, other than Rekodr's own
// (the save and buffer folders, which can sit in Videos with them) and another watched folder. Each is added to
// `items` as a clip of the year of its time.
static void lib_scan_watched(const Path &save_dir, const Path &buffer_dir, const Path *watch, i32 watch_count,
                             const Path &cache, LibItem *items, i32 *n, u64 *seen, const char *tmpl, i32 *by_hand)
{
    for (i32 i = 0; i < (i32)g_lib_dir_count; i++) g_lib_dirs[i].live = false;
    for (i32 w = 0; w < watch_count; w++) {
        const Path &top = watch[w];
        bool again = false;
        for (i32 k = 0; k < w; k++) again |= !_wcsicmp(watch[k].s, top.s);
        if (again || path_within(top.s, save_dir.s) || path_within(top.s, buffer_dir.s)) continue;
        Path dirs[65];
        char games[65][64] = {};
        i32 dir_count = 1;
        dirs[0] = top;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(path_join(top, L"*").s, &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.' ||
                (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) || dir_count == COUNT(dirs))
                continue;
            Path sub = path_join(top, fd.cFileName);
            bool skip = path_within(save_dir.s, sub.s) || path_within(sub.s, save_dir.s) ||
                        path_within(buffer_dir.s, sub.s);
            for (i32 k = 0; k < watch_count && !skip; k++) skip = k != w && !_wcsicmp(watch[k].s, sub.s);
            if (skip) continue;
            dirs[dir_count] = sub;
            wide_to_utf8(fd.cFileName, games[dir_count], sizeof games[0]);
            dir_count++;
        } while (FindNextFileW(h, &fd));
        FindClose(h);

        for (i32 d = 0; d < dir_count; d++) {
            u8 dir = d ? 0 : lib_dir_add(top, ""); // the watched one even while it's empty, for lib_watch_poll
            WIN32_FIND_DATAW f;
            HANDLE fh = FindFirstFileW(path_join(dirs[d], L"*").s, &f);
            if (fh == INVALID_HANDLE_VALUE) continue;
            do {
                if (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                wchar_t *dot = wcsrchr(f.cFileName, L'.');
                if (!dot || (_wcsicmp(dot, L".mp4") && _wcsicmp(dot, L".mkv") && _wcsicmp(dot, L".mov"))) continue;
                if (*n == LIB_MAX_ITEMS) break;
                if (!dir && !(dir = lib_dir_add(dirs[d], games[d]))) break;
                LibItem *it = &items[(*n)++];
                *it = {};
                wide_to_utf8(dot, it->ext, sizeof it->ext);
                *dot = 0;
                wide_to_utf8(f.cFileName, it->name, sizeof it->name);
                it->dir = dir;
                it->kind = LIB_CLIPS;
                it->size = ((u64)f.nFileSizeHigh << 32) | f.nFileSizeLow;
                it->mtime_us = filetime_us(f.ftLastWriteTime);
                FILETIME local;
                SYSTEMTIME st;
                FileTimeToLocalFileTime(&f.ftLastWriteTime, &local);
                FileTimeToSystemTime(&local, &st);
                it->year = (i16)st.wYear;
                it->when_us = lib_name_us(tmpl, it->name, it->mtime_us);
                u64 k = fnv64(&it->size, sizeof it->size);
                it->key = lib_key_unique(seen, fnv64(&it->mtime_us, sizeof it->mtime_us, k), it->name);
                char tagged[64];
                it->probed = lib_cache_read(cache, it->key, &it->info, tagged, sizeof tagged);
                *by_hand += lib_set_game(it, it->probed ? tagged : "", tmpl);
                lib_set_display_name(it);
            } while (FindNextFileW(fh, &f));
            FindClose(fh);
        }
    }
}

static void lib_scan(const Path &save_dir, const Path &cache, const Path &ffmpeg, const char *tmpl,
                     const Path &buffer_dir, const Path *watch, i32 watch_count)
{
    LibItem *items = g_scan_items;
    i32 n = 0, by_hand = 0;
    static u64 seen[LIB_MAX_ITEMS * 2];
    memset(seen, 0, sizeof seen);
    lib_overrides_load();
    lib_names_load();
    // The year folders of Clips, then of Sessions
    for (i32 top = 0; top < 2; top++) {
        WIN32_FIND_DATAW fd;
        HANDLE years = FindFirstFileW(path_join(path_join(save_dir, top ? L"Sessions" : L"Clips"), L"*").s, &fd);
        if (years == INVALID_HANDLE_VALUE) continue;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || wcslen(fd.cFileName) != 4 || !iswdigit(fd.cFileName[0]))
                continue;
            i16 year = (i16)_wtoi(fd.cFileName);
            Path clips = save_clips_dir(save_dir, year), source = save_source_dir(save_dir, year);
            // 0: Clips, 1: Sessions, 2: Clips\<year>\Source, for originals whose render isn't in Clips (render.cpp
            // puts them back or renders them again)
            for (i32 kind = top ? 1 : 0; kind < 3; kind += top ? 3 : 2) {
                Path dir = kind == 1 ? save_sessions_dir(save_dir, year) : kind == 2 ? source : clips;
                WIN32_FIND_DATAW f;
                HANDLE h = FindFirstFileW(path_join(dir, L"*").s, &f);
                if (h == INVALID_HANDLE_VALUE) continue;
                do {
                    if (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                    wchar_t *dot = wcsrchr(f.cFileName, L'.');
                    if (!dot || (_wcsicmp(dot, L".mp4") && _wcsicmp(dot, L".mkv") && _wcsicmp(dot, L".mov"))) continue;
                    // A clip with its original in Source is that original, rendered; one in Source with its clip
                    // there was listed already
                    WIN32_FILE_ATTRIBUTE_DATA orig = {};
                    bool rendered = kind == 0 && GetFileAttributesExW(path_join(source, f.cFileName).s,
                                                                      GetFileExInfoStandard, &orig);
                    if (kind == 2 && file_exists(path_join(clips, f.cFileName).s)) continue;
                    if (n == LIB_MAX_ITEMS) break;
                    LibItem *it = &items[n++];
                    *it = {};
                    wide_to_utf8(dot, it->ext, sizeof it->ext);
                    *dot = 0;
                    wide_to_utf8(f.cFileName, it->name, sizeof it->name);
                    it->year = year;
                    it->kind = kind == 1 ? LIB_SESSIONS : LIB_CLIPS;
                    it->size = ((u64)f.nFileSizeHigh << 32) | f.nFileSizeLow;
                    it->mtime_us = filetime_us(f.ftLastWriteTime);
                    it->rendered = rendered || kind == 2;
                    if (rendered) {
                        it->render_size = it->size;
                        it->size = ((u64)orig.nFileSizeHigh << 32) | orig.nFileSizeLow;
                        it->mtime_us = filetime_us(orig.ftLastWriteTime);
                    }
                    it->when_us = lib_name_us(tmpl, it->name, it->mtime_us);
                    u64 k = fnv64(&it->size, sizeof it->size);
                    it->key = lib_key_unique(seen, fnv64(&it->mtime_us, sizeof it->mtime_us, k), it->name);
                    char tagged[64];
                    it->probed = lib_cache_read(cache, it->key, &it->info, tagged, sizeof tagged);
                    by_hand += lib_set_game(it, it->probed ? tagged : "", tmpl);
                    lib_set_display_name(it);
                } while (FindNextFileW(h, &f));
                FindClose(h);
            }
        } while (FindNextFileW(years, &fd));
        FindClose(years);
    }
    lib_scan_watched(save_dir, buffer_dir, watch, watch_count, cache, items, &n, seen, tmpl, &by_hand);
    qsort(items, n, sizeof items[0], lib_item_newer);
    lib_publish(items, n);
    if (g_game_override_count) DEV_LOG("library: games.txt names %d files, %d of them here", g_game_override_count, by_hand);

    // Files not seen before: ffprobe them, LIB_PROBERS at once, newest first, showing them as they come.
    if (!ffmpeg.s[0]) return;
    LibProbeRun run = {};
    run.items = items;
    run.n = n;
    run.save_dir = save_dir;
    run.cache = cache;
    run.ffprobe = ffprobe_for(ffmpeg);
    run.tmpl = tmpl;
    i32 todo = 0;
    for (i32 i = 0; i < n; i++) todo += !items[i].probed;
    if (!todo) return;
    HANDLE threads[LIB_PROBERS];
    i32 started = 0;
    for (i32 i = 0; i < LIB_PROBERS && i < todo; i++)
        if ((threads[started] = CreateThread(0, 0, lib_prober, &run, 0, 0))) started++;
    if (!started) lib_prober(&run);
    while (started && WaitForMultipleObjects(started, threads, TRUE, 400) == WAIT_TIMEOUT) {
        AcquireSRWLockShared(&run.lock);
        lib_publish(items, n);
        ReleaseSRWLockShared(&run.lock);
    }
    for (i32 i = 0; i < started; i++) CloseHandle(threads[i]);
    lib_publish(items, n);
    log_line("library: %d files, %d probed", n, run.probed);
}

// Decodes a JPEG to BGRA with WIC. Returns HeapAlloc'd pixels, or 0.
static u32 *wic_load(IWICImagingFactory *wic, const wchar_t *path, i32 *w, i32 *h)
{
    IWICBitmapDecoder *dec = 0;
    IWICBitmapFrameDecode *frame = 0;
    IWICFormatConverter *conv = 0;
    u32 *pixels = 0;
    UINT fw = 0, fh = 0;
    if (FAILED(wic->CreateDecoderFromFilename(path, 0, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec))) goto done;
    if (FAILED(dec->GetFrame(0, &frame))) goto done;
    if (FAILED(wic->CreateFormatConverter(&conv))) goto done;
    if (FAILED(conv->Initialize(frame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, 0, 0, WICBitmapPaletteTypeCustom)))
        goto done;
    conv->GetSize(&fw, &fh);
    if (!fw || !fh || fw > 16384 || fh > 4096) goto done;
    pixels = (u32 *)HeapAlloc(GetProcessHeap(), 0, (size_t)fw * fh * 4);
    if (pixels && FAILED(conv->CopyPixels(0, fw * 4, fw * fh * 4, (BYTE *)pixels))) {
        HeapFree(GetProcessHeap(), 0, pixels);
        pixels = 0;
    }
    *w = (i32)fw;
    *h = (i32)fh;
done:
    if (conv) conv->Release();
    if (frame) frame->Release();
    if (dec) dec->Release();
    return pixels;
}

// The strip for a file: 24 frames across its length, 320 wide each. Made once, then read from the cache.
static bool lib_make_strip(const Path &ffmpeg, const Path &src, f64 duration, const Path &jpg)
{
    if (file_exists(jpg.s)) return true;
    if (!ffmpeg.s[0] || duration <= 0) return false;
    Path tmp = {};
    swprintf(tmp.s, MAX_PATH, L"%s.tmp.jpg", jpg.s);
    static thread_local wchar_t args[4096];
    for (i32 attempt = 0; attempt < 2; attempt++) {
        args[0] = 0;
        static const char *const head[] = {"-hide_banner", "-loglevel", "error", "-y"};
        arg_list(args, COUNT(args), head, COUNT(head));
        // Keyframes only is much faster on long files; some files have too few, so it's retried without.
        if (attempt == 0 && duration > 12) { cmd_arg8(args, COUNT(args), "-skip_frame"); cmd_arg8(args, COUNT(args), "nokey"); }
        cmd_arg8(args, COUNT(args), "-i");
        cmd_arg(args, COUNT(args), src.s);
        cmd_arg8(args, COUNT(args), "-an");
        char vf[256];
        snprintf(vf, sizeof vf, "tpad=stop_mode=clone:stop_duration=3,fps=%d/%.3f,scale=320:-2:flags=bilinear,tile=%dx1",
                 (int)LIB_STRIP_FRAMES, duration, (int)LIB_STRIP_FRAMES);
        cmd_arg8(args, COUNT(args), "-vf");
        cmd_arg8(args, COUNT(args), vf);
        static const char *const tail[] = {"-frames:v", "1", "-q:v", "6"};
        arg_list(args, COUNT(args), tail, COUNT(tail));
        cmd_arg(args, COUNT(args), tmp.s);
        i32 code = proc_run(ffmpeg.s, args, 120000, 0, 0);
        if (code == 0 && file_exists(tmp.s) && MoveFileExW(tmp.s, jpg.s, MOVEFILE_REPLACE_EXISTING)) return true;
        DeleteFileW(tmp.s);
    }
    return false;
}

// An unsaved session's poster on disk: thumbs\posters\p-<its buffer segment's start, hex>.jpg
static Path lib_poster_file(const Path &cache, u64 key)
{
    wchar_t name[64];
    swprintf(name, COUNT(name), L"p-%016llx.jpg", key);
    return path_join(path_join(cache, L"posters"), name);
}

static void lib_add_result(const LibResult &r)
{
    for (;;) {
        AcquireSRWLockExclusive(&g_lib.lock);
        bool room = g_lib.result_count < COUNT(g_lib.results);
        if (room) g_lib.results[g_lib.result_count++] = r;
        ReleaseSRWLockExclusive(&g_lib.lock);
        if (room) break;
        Sleep(20); // the main thread takes them on its next event
    }
    Event e = {};
    e.kind = EV_LIBRARY;
    e.a = LIB_EV_RESULTS;
    event_post(e);
}

static void lib_picture(IWICImagingFactory *wic, const LibTask &t, const Path &cache, const Path &ffmpeg)
{
    LibResult r = {};
    r.key = t.key;
    r.strip = t.kind == LT_STRIP;
    r.frames = LIB_STRIP_FRAMES;
    Path jpg = lib_cache_file(cache, t.key, L".jpg");
    i32 w = 0, h = 0;
    u32 *px = 0;
    if (lib_make_strip(ffmpeg, t.path, t.duration, jpg) && wic) px = wic_load(wic, jpg.s, &w, &h);
    if (!px) {
        r.failed = true;
        lib_add_result(r);
        return;
    }
    if (r.strip) {
        r.pixels = px;
        r.w = w;
        r.h = h;
    } else {
        // The poster: one frame of the strip, the asked-for one or the next that isn't near black (a fade in, a
        // loading screen), so a card never shows an empty picture when the file has something to show.
        i32 fw = w / LIB_STRIP_FRAMES;
        if (fw <= 0) fw = w;
        i32 frames = w / fw, pick = t.poster < frames ? t.poster : 0;
        for (i32 k = 0; k < frames; k++) {
            i32 f = (t.poster + k) % frames;
            u64 sum = 0, n = 0;
            for (i32 y = 0; y < h; y += 8)
                for (i32 x = 0; x < fw; x += 8) {
                    u32 c = px[(size_t)y * w + f * fw + x];
                    sum += ((c >> 16) & 255) + ((c >> 8) & 255) + (c & 255);
                    n += 3;
                }
            if (n && sum / n > 14) {
                pick = f;
                break;
            }
        }
        i32 x0 = fw * pick;
        r.pixels = (u32 *)HeapAlloc(GetProcessHeap(), 0, (size_t)fw * h * 4);
        if (r.pixels)
            for (i32 y = 0; y < h; y++) memcpy(r.pixels + (size_t)y * fw, px + (size_t)y * w + x0, fw * 4);
        HeapFree(GetProcessHeap(), 0, px);
        r.w = fw;
        r.h = h;
        r.failed = !r.pixels;
    }
    lib_add_result(r);
}

// A poster: the segment's first frame, read once by ffmpeg into its file, then from the file.
static void lib_poster_read(IWICImagingFactory *wic, const LibTask &t, const Path &cache, const Path &ffmpeg)
{
    LibResult r = {};
    r.key = t.key;
    r.poster = true;
    Path jpg = lib_poster_file(cache, t.key);
    if (!file_exists(jpg.s) && t.path.s[0] && ffmpeg.s[0]) {
        Path tmp = {};
        swprintf(tmp.s, MAX_PATH, L"%s.tmp.jpg", jpg.s);
        static thread_local wchar_t args[2048];
        args[0] = 0;
        static const char *const head[] = {"-hide_banner", "-loglevel", "error", "-y", "-i"};
        arg_list(args, COUNT(args), head, COUNT(head));
        cmd_arg(args, COUNT(args), t.path.s);
        static const char *const tail[] = {"-an", "-frames:v", "1", "-vf", "scale=320:-2:flags=bilinear", "-q:v", "6"};
        arg_list(args, COUNT(args), tail, COUNT(tail));
        cmd_arg(args, COUNT(args), tmp.s);
        i32 code = proc_run(ffmpeg.s, args, 30000, 0, 0);
        if (!(code == 0 && file_exists(tmp.s) && MoveFileExW(tmp.s, jpg.s, MOVEFILE_REPLACE_EXISTING))) DeleteFileW(tmp.s);
    }
    i32 w = 0, h = 0;
    if (wic && file_exists(jpg.s)) r.pixels = wic_load(wic, jpg.s, &w, &h);
    r.w = w;
    r.h = h;
    r.failed = !r.pixels;
    lib_add_result(r);
}

static DWORD WINAPI lib_thread(void *)
{
    CoInitializeEx(0, COINIT_MULTITHREADED);
    IWICImagingFactory *wic = 0;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, 0, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) {
        log_line("library: WIC isn't available; no thumbnails");
        wic = 0;
    }
    for (;;) {
        AcquireSRWLockExclusive(&g_lib.lock);
        while (!g_lib.task_count) SleepConditionVariableSRW(&g_lib.wake, &g_lib.lock, INFINITE, 0);
        // One scan at a time: the other thread leaves a scan at the front while one runs, and takes what's behind.
        i32 pick = 0;
        while (pick < g_lib.task_count && g_lib.tasks[pick].kind == LT_SCAN && g_lib.scan_running) pick++;
        if (pick == g_lib.task_count) {
            ReleaseSRWLockExclusive(&g_lib.lock);
            Sleep(50);
            continue;
        }
        LibTask t = g_lib.tasks[pick];
        g_lib.task_count--;
        memmove(g_lib.tasks + pick, g_lib.tasks + pick + 1, (g_lib.task_count - pick) * sizeof g_lib.tasks[0]);
        Path save_dir = g_lib.save_dir, cache = g_lib.cache_dir, ffmpeg = g_lib.ffmpeg, buffer_dir = g_lib.buffer_dir;
        Path watch[WATCH_MAX];
        i32 watch_count = g_lib.watch_count;
        memcpy(watch, g_lib.watch, sizeof watch);
        char tmpl[sizeof g_lib.name_template];
        str_set(tmpl, g_lib.name_template);
        if (t.kind == LT_SCAN) {
            g_lib.scan_queued = false;
            g_lib.scan_running = true;
        }
        ReleaseSRWLockExclusive(&g_lib.lock);

        if (t.kind == LT_SCAN) {
            u64 started = GetTickCount64();
            lib_scan(save_dir, cache, ffmpeg, tmpl, buffer_dir, watch, watch_count);
            AcquireSRWLockExclusive(&g_lib.lock);
            g_lib.scan_running = false;
            ReleaseSRWLockExclusive(&g_lib.lock);
            Event e = {};
            e.kind = EV_LIBRARY;
            e.a = LIB_EV_SCANNED; // once more, so the main thread sees the scan has ended
            event_post(e);
            if (GetTickCount64() - started > 2000) log_line("library: scan took %.1f s", (GetTickCount64() - started) / 1000.0);
        } else if (t.kind == LT_POSTER) {
            lib_poster_read(wic, t, cache, ffmpeg);
        } else {
            lib_picture(wic, t, cache, ffmpeg);
        }
    }
}

static void lib_init()
{
    if (g_lib.threads[0]) return;
    InitializeSRWLock(&g_lib.lock);
    InitializeConditionVariable(&g_lib.wake);
    for (i32 i = 0; i < COUNT(g_lib.threads); i++) g_lib.threads[i] = thread_start(lib_thread, 0);
}

static void lib_push(const LibTask &t, bool front)
{
    lib_init();
    AcquireSRWLockExclusive(&g_lib.lock);
    if (g_lib.task_count == COUNT(g_lib.tasks)) g_lib.task_count--; // the request at the back goes
    if (front) {
        memmove(g_lib.tasks + 1, g_lib.tasks, g_lib.task_count * sizeof g_lib.tasks[0]);
        g_lib.tasks[0] = t;
    } else {
        g_lib.tasks[g_lib.task_count] = t;
    }
    g_lib.task_count++;
    WakeAllConditionVariable(&g_lib.wake);
    ReleaseSRWLockExclusive(&g_lib.lock);
}

// ---------------------------------------------------------------------------------------------------------------
// Main thread

static void library_rescan()
{
    lib_init();
    Path ffmpeg = {};
    if (!ffmpeg_find(g_app.config.ffmpeg, &ffmpeg)) ffmpeg = {};
    Path cache = path_join(g_app.paths.local_dir, L"thumbs");
    SHCreateDirectoryExW(0, path_join(cache, L"posters").s, 0);
    AcquireSRWLockExclusive(&g_lib.lock);
    bool queued = g_lib.scan_queued;
    g_lib.save_dir = g_app.config.save_dir;
    g_lib.buffer_dir = g_app.config.buffer_dir;
    g_lib.watch_count = g_app.config.watch_count;
    memcpy(g_lib.watch, g_app.config.watch_dirs, sizeof g_lib.watch);
    str_set(g_lib.name_template, g_app.config.name_template);
    g_lib.cache_dir = cache;
    g_lib.ffmpeg = ffmpeg;
    g_lib.scan_queued = true;
    ReleaseSRWLockExclusive(&g_lib.lock);
    if (!queued) {
        LibTask t = {};
        t.kind = LT_SCAN;
        lib_push(t, true);
    }
    g_app.library.scanning = true;
}

// A card's original (the one its edit, pictures and records are of) and the file that plays anywhere: the same
// file unless the clip is rendered (render.cpp). Show in folder points at the playable one, the editor and Share's
// Original at the original.
static Path lib_item_path(const LibItem &it) { return lib_file_path(g_app.config.save_dir, it); }
static Path lib_playable_path(const LibItem &it) { return lib_playable_file(g_app.config.save_dir, it); }

// Where an item's edit is kept: <save_dir>\Data\<name>.edit.txt
static Path lib_edit_path(const LibItem &it)
{
    Path p = {};
    wchar_t name[256];
    utf8_to_wide(it.name, name, COUNT(name));
    swprintf(p.s, MAX_PATH, L"%s\\%s.edit.txt", save_data_dir(g_app.config.save_dir).s, name);
    return p;
}

// Where a rendered clip notes what its render was made from: <save_dir>\Data\<name>.render.txt
static Path lib_render_note_path(const LibItem &it)
{
    Path p = lib_edit_path(it);
    wcscpy(wcsrchr(p.s, L'.') - 5, L".render.txt"); // over ".edit.txt"
    return p;
}

enum { LIB_ITEM_FILES = 4 };

// A card's files that are there: its original, its rendered copy, its edit and its render note. Returns how many.
static i32 lib_item_files(const LibItem &it, Path *out)
{
    i32 n = 0;
    out[n++] = lib_item_path(it);
    Path more[] = {it.rendered ? lib_playable_path(it) : Path{}, lib_edit_path(it), lib_render_note_path(it)};
    for (const Path &p : more)
        if (p.s[0] && file_exists(p.s)) out[n++] = p;
    return n;
}

static bool lib_matches(const char *hay, const char *needle)
{
    if (!*needle) return true;
    size_t n = strlen(needle);
    for (const char *h = hay; *h; h++)
        if (!_strnicmp(h, needle, n)) return true;
    return false;
}

// Does a file's tags answer the search? "#ace" asks for the tag ace itself ("#" for any tag); other text is looked
// for in them as it is in names.
static bool lib_tags_match(const char *tags, const char *search)
{
    return search[0] == '#' ? lib_tag_has(tags, search + 1) : lib_matches(tags, search);
}

enum LibRange : i32 { RANGE_ALL, RANGE_TODAY, RANGE_WEEK, RANGE_MONTH, RANGE_CUSTOM };
enum LibSort : i32 { SORT_NEWEST, SORT_OLDEST, SORT_LONGEST, SORT_BIGGEST, SORT_GAME, SORT_COUNT };

static i64 local_day(i64 us, SYSTEMTIME *out);
static void lib_shown_name(const char *name, char *out, i32 cap);

// A file name matches a search by itself or by the name it's shown with ("Desktop" for a clip of no game)
static bool lib_name_matches(const char *name, const char *needle)
{
    if (lib_matches(name, needle)) return true;
    char shown[300];
    lib_shown_name(name, shown, sizeof shown);
    return lib_matches(shown, needle);
}

// Whether a file's time is in the chosen range, by local days. 1601-01-01 (day 0) was a Monday, so a week
// starts on a day that's a multiple of 7.
static bool lib_in_range(i64 us)
{
    const Library &lib = g_app.library;
    if (lib.range == RANGE_ALL) return true;
    SYSTEMTIME t;
    i64 today = local_day(now_us(), &t), day = local_day(us, 0);
    switch (lib.range) {
    case RANGE_TODAY: return day == today;
    case RANGE_WEEK: return day >= today - today % 7;
    case RANGE_MONTH: return day >= today - (t.wDay - 1);
    case RANGE_CUSTOM: return day >= lib.range_from && day <= lib.range_to;
    default: return true;
    }
}

static int lib_sort_cmp(const void *pa, const void *pb)
{
    const Library &lib = g_app.library;
    const LibItem &a = lib.items[*(const i32 *)pa], &b = lib.items[*(const i32 *)pb];
    switch (lib.sort) {
    case SORT_OLDEST: if (a.when_us != b.when_us) return a.when_us < b.when_us ? -1 : 1; break;
    case SORT_LONGEST: if (a.info.duration != b.info.duration) return a.info.duration > b.info.duration ? -1 : 1; break;
    case SORT_BIGGEST: if (a.size != b.size) return a.size > b.size ? -1 : 1; break;
    case SORT_GAME: { // by game, A to Z, with no game last; newest first within each
        bool an = game_is_none(a.info.game), bn = game_is_none(b.info.game);
        if (an != bn) return an ? 1 : -1;
        int c = _stricmp(a.info.game, b.info.game);
        if (c) return c;
        break;
    }
    default: break;
    }
    return a.when_us > b.when_us ? -1 : a.when_us < b.when_us ? 1 : 0;
}

// Folders: groups of clips, and of sessions, made by hand (each view has its own). A card can be in several.
// <config_dir>\folders.txt holds a line "folder | <clips or sessions> | <name>" for each folder, in order, and a
// line "<key in hex> | <clips or sessions> | <folder> | <file name>" for each card in each folder. The key is the
// card's, as in games.txt, so a rename keeps it; the file name is only there for people reading the file.

static void lib_message(bool error, const char *fmt, ...);
static void lib_refilter();
static void lib_favourites_load();
static const char *lib_noun(i32 n);

static const char *lib_view_word(LibView v) { return v == LIB_SESSIONS ? "sessions" : "clips"; }

static i32 lib_folder_find(LibView v, const char *name)
{
    Library &lib = g_app.library;
    for (i32 i = 0; i < lib.folder_count; i++)
        if (lib.folders[i].view == v && !strcmp(lib.folders[i].name, name)) return i;
    return -1;
}

// Where `key` is in lib.filed, or where it would go.
static i32 lib_filed_at(u64 key)
{
    Library &lib = g_app.library;
    i32 lo = 0, hi = lib.filed_count;
    while (lo < hi) {
        i32 mid = (lo + hi) / 2;
        if (lib.filed[mid].key < key) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

static u64 lib_folder_mask(u64 key)
{
    Library &lib = g_app.library;
    i32 i = lib_filed_at(key);
    return i < lib.filed_count && lib.filed[i].key == key ? lib.filed[i].folders : 0;
}

static void lib_folder_mask_set(u64 key, u64 mask)
{
    Library &lib = g_app.library;
    i32 i = lib_filed_at(key);
    bool has = i < lib.filed_count && lib.filed[i].key == key;
    if (mask) {
        if (!has) {
            if (lib.filed_count == LIB_MAX_ITEMS) return;
            memmove(&lib.filed[i + 1], &lib.filed[i], (lib.filed_count - i) * sizeof lib.filed[0]);
            lib.filed_count++;
            lib.filed[i].key = key;
        }
        lib.filed[i].folders = mask;
    } else if (has) {
        memmove(&lib.filed[i], &lib.filed[i + 1], (lib.filed_count - i - 1) * sizeof lib.filed[0]);
        lib.filed_count--;
    }
}

static void lib_folders_load()
{
    Library &lib = g_app.library;
    lib.folders_loaded = true;
    lib.folder_count = 0;
    lib.filed_count = 0;
    FILE *f = _wfopen(path_join(g_app.paths.config_dir, L"folders.txt").s, L"rb");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char first[32] = "", view[16] = "", name[64] = "";
        if (sscanf(line, "%31[^|]| %15[^| \r\n] | %63[^|\r\n]", first, view, name) != 3) continue;
        LibView v = !strcmp(view, "sessions") ? LIB_SESSIONS : LIB_CLIPS;
        char *k = trim(first), *n = trim(name);
        if (!*n) continue;
        if (!strcmp(k, "folder")) {
            if (lib.folder_count < LIB_MAX_FOLDERS && lib_folder_find(v, n) < 0) {
                str_set(lib.folders[lib.folder_count].name, n);
                lib.folders[lib.folder_count++].view = v;
            }
            continue;
        }
        unsigned long long key = 0;
        i32 i = lib_folder_find(v, n);
        if (sscanf(k, "%llx", &key) == 1 && key && i >= 0) lib_folder_mask_set(key, lib_folder_mask(key) | 1ull << i);
    }
    fclose(f);
    log_line("library: %d folders, %d cards in them", lib.folder_count, lib.filed_count);
    lib_favourites_load();
    lib_comments_load();
    lib_tags_load();
}

static void lib_folders_save()
{
    Library &lib = g_app.library;
    FILE *f = _wfopen(path_join(g_app.paths.config_dir, L"folders.txt").s, L"wb");
    if (!f) {
        lib_message(true, "Couldn't save the folders");
        return;
    }
    for (i32 i = 0; i < lib.folder_count; i++)
        fprintf(f, "folder | %s | %s\r\n", lib_view_word(lib.folders[i].view), lib.folders[i].name);
    for (i32 j = 0; j < lib.filed_count; j++) {
        const LibFiled &e = lib.filed[j];
        const char *file = "";
        for (i32 k = 0; k < lib.item_count; k++)
            if (lib.items[k].key == e.key) {
                file = lib.items[k].name;
                break;
            }
        for (i32 i = 0; i < lib.folder_count; i++)
            if (e.folders >> i & 1)
                fprintf(f, "%llx | %s | %s | %s\r\n", (unsigned long long)e.key, lib_view_word(lib.folders[i].view),
                        lib.folders[i].name, file);
    }
    fclose(f);
}

// Favourites, the rows at the top of the list on the left: <config_dir>\favourites.txt holds a line "<games, clips or
// sessions> | <name>" for each, in the order they were added. clips and sessions are folders of those views.
static i32 lib_favourite_find(const char *list, const char *name)
{
    Library &lib = g_app.library;
    for (i32 i = 0; i < lib.favourite_count; i++)
        if (!strcmp(lib.favourites[i].list, list) && !strcmp(lib.favourites[i].name, name)) return i;
    return -1;
}

static void lib_favourites_load()
{
    Library &lib = g_app.library;
    lib.favourite_count = 0;
    FILE *f = _wfopen(path_join(g_app.paths.config_dir, L"favourites.txt").s, L"rb");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f) && lib.favourite_count < LIB_MAX_FAVOURITES) {
        LibFavourite p = {};
        char list[16] = "", name[64] = "";
        if (sscanf(line, "%15[^| ] | %63[^|\r\n]", list, name) != 2) continue;
        char *n = trim(name);
        if (!*n || lib_favourite_find(list, n) >= 0) continue;
        str_set(p.list, list);
        str_set(p.name, n);
        lib.favourites[lib.favourite_count++] = p;
    }
    fclose(f);
}

static void lib_favourites_save()
{
    Library &lib = g_app.library;
    FILE *f = _wfopen(path_join(g_app.paths.config_dir, L"favourites.txt").s, L"wb");
    if (!f) {
        lib_message(true, "Couldn't save the favourites");
        return;
    }
    for (i32 i = 0; i < lib.favourite_count; i++) fprintf(f, "%s | %s\r\n", lib.favourites[i].list, lib.favourites[i].name);
    fclose(f);
}

// Makes a row a favourite (below the ones already there) or not.
static void lib_favourite_set(const char *list, const char *name, bool on)
{
    Library &lib = g_app.library;
    i32 i = lib_favourite_find(list, name);
    if (on == (i >= 0)) return;
    if (on) {
        if (lib.favourite_count == LIB_MAX_FAVOURITES) {
            lib_message(true, "That's as many favourites as there can be");
            return;
        }
        LibFavourite &p = lib.favourites[lib.favourite_count++];
        str_set(p.list, list);
        str_set(p.name, name);
    } else {
        memmove(&lib.favourites[i], &lib.favourites[i + 1], (lib.favourite_count - i - 1) * sizeof lib.favourites[0]);
        lib.favourite_count--;
    }
    log_line("library: %s %s %s", list, name, on ? "favourited" : "unfavourited");
    lib_favourites_save();
}

// The order of a list's rows: its favourites in the order they were added, then the rest as they come. names(i) is row i's name,
// or null for a row that isn't shown.
template <typename F> static i32 lib_favourite_order(const char *list, i32 count, F names, i32 *order)
{
    Library &lib = g_app.library;
    i32 n = 0;
    for (i32 p = 0; p < lib.favourite_count; p++) {
        if (strcmp(lib.favourites[p].list, list)) continue;
        for (i32 i = 0; i < count; i++)
            if (names(i) && !strcmp(names(i), lib.favourites[p].name)) {
                order[n++] = i;
                break;
            }
    }
    for (i32 i = 0; i < count; i++)
        if (names(i) && lib_favourite_find(list, names(i)) < 0) order[n++] = i;
    return n;
}

// A folder name as it can be kept: one line, no '|', trimmed.
static void lib_folder_clean(char (&out)[64], const char *in)
{
    char v[64];
    str_set(v, in);
    for (char *c = v; *c; c++)
        if (*c == '|' || *c == '\t' || *c == '\r' || *c == '\n') *c = *c == '|' ? '/' : ' ';
    str_set(out, trim(v));
}

// Makes a folder in this view called `base`, or "base 2", "base 3"... when that's taken. Its index, or -1.
static i32 lib_folder_new(const char *base)
{
    Library &lib = g_app.library;
    if (lib.folder_count == LIB_MAX_FOLDERS) {
        lib_message(true, "That's as many folders as there can be");
        return -1;
    }
    char name[64];
    str_set(name, base);
    for (i32 n = 2; lib_folder_find(lib.view, name) >= 0; n++) snprintf(name, sizeof name, "%s %d", base, n);
    i32 i = lib.folder_count++;
    str_set(lib.folders[i].name, name);
    lib.folders[i].view = lib.view;
    log_line("library: folder %s made in %s", name, lib_view_word(lib.view));
    lib_folders_save();
    lib_refilter();
    return i;
}

static void lib_folder_rename(i32 i, const char *to)
{
    Library &lib = g_app.library;
    LibFolder &fo = lib.folders[i];
    char n[64];
    lib_folder_clean(n, to);
    if (!*n || !strcmp(n, fo.name)) return;
    if (lib_folder_find(fo.view, n) >= 0) {
        lib_message(true, "There's already a folder called %s", n);
        return;
    }
    if (fo.view == lib.view && !strcmp(lib.folder, fo.name)) str_set(lib.folder, n);
    log_line("library: folder %s is now %s", fo.name, n);
    i32 fav = lib_favourite_find(lib_view_word(fo.view), fo.name);
    str_set(fo.name, n);
    if (fav >= 0) {
        str_set(lib.favourites[fav].name, n);
        lib_favourites_save();
    }
    lib_folders_save();
    lib_refilter();
}

// Deletes a folder; its cards stay where they are.
static void lib_folder_delete(i32 i)
{
    Library &lib = g_app.library;
    LibFolder &fo = lib.folders[i];
    if (fo.view == lib.view && !strcmp(lib.folder, fo.name)) lib.folder[0] = 0;
    lib_message(false, "Deleted the folder %s", fo.name);
    lib_favourite_set(lib_view_word(fo.view), fo.name, false);
    memmove(&lib.folders[i], &lib.folders[i + 1], (lib.folder_count - i - 1) * sizeof lib.folders[0]);
    lib.folder_count--;
    u64 low = (1ull << i) - 1;
    for (i32 j = lib.filed_count - 1; j >= 0; j--) {
        u64 m = lib.filed[j].folders;
        lib_folder_mask_set(lib.filed[j].key, (m & low) | (m >> 1 & ~low));
    }
    lib_folders_save();
    lib_refilter();
}

// Puts a card in folder i, or takes it out.
static void lib_folder_put(const LibItem &it, i32 i, bool in)
{
    Library &lib = g_app.library;
    u64 m = lib_folder_mask(it.key), b = 1ull << i;
    if (((m & b) != 0) == in) return;
    lib_folder_mask_set(it.key, in ? m | b : m & ~b);
    if (in) lib_message(false, "Added %s to %s", it.name, lib.folders[i].name);
    else lib_message(false, "Took %s out of %s", it.name, lib.folders[i].name);
    lib_folders_save();
    lib_refilter();
}

// The view's game chips and the cards that match the date range, the chip and the search, in the chosen order.
static void lib_refilter()
{
    Library &lib = g_app.library;
    lib.game_count = 0;
    lib.view_count = 0;
    lib.range_count = 0;
    lib.shown_count = 0;
    i32 desktop = 0, other = 0;
    for (i32 f = 0; f < lib.folder_count; f++) lib.folder_items[f] = 0;
    i32 in_folder = lib.folder[0] ? lib_folder_find(lib.view, lib.folder) : -1;
    if (in_folder < 0) lib.folder[0] = 0;
    bool by_folder = g_app.config.side_folders;
    for (i32 i = 0; i < lib.item_count; i++) {
        const LibItem &it = lib.items[i];
        if (it.kind != lib.view) continue;
        lib.view_count++;
        if (!lib_in_range(it.when_us)) continue;
        lib.range_count++;
        u64 fm = lib_folder_mask(it.key);
        for (i32 f = 0; f < LIB_MAX_FOLDERS && fm >> f; f++)
            if (fm >> f & 1) lib.folder_items[f]++;
        const char *g = it.info.game;
        if (!strcmp(g, GAME_DESKTOP)) desktop++;
        else if (!strcmp(g, GAME_NOT_OURS)) other++;
        else {
            i32 k = 0;
            while (k < lib.game_count && strcmp(lib.games[k], g)) k++;
            if (k == lib.game_count && k < LIB_MAX_GAMES - 2) {
                str_set(lib.games[k], g);
                lib.game_items[k] = 0;
                lib.game_count++;
            }
            if (k < lib.game_count) lib.game_items[k]++;
        }
        // The list on the left picks by folder or by game, whichever it shows.
        if (by_folder ? in_folder >= 0 && !(fm >> in_folder & 1) : lib.game[0] && strcmp(lib.game, g)) continue;
        if (!lib_name_matches(it.name, lib.search) && !lib_matches(g, lib.search) && !lib_matches(lib_comment(it.key), lib.search) &&
            !lib_tags_match(lib_tags(it.key), lib.search))
            continue;
        lib.shown[lib.shown_count++] = i;
    }
    // Games most recent first, then Desktop, then files not ours.
    if (desktop) { str_set(lib.games[lib.game_count], GAME_DESKTOP); lib.game_items[lib.game_count++] = desktop; }
    if (other) { str_set(lib.games[lib.game_count], GAME_NOT_OURS); lib.game_items[lib.game_count++] = other; }
    if (lib.sort != SORT_NEWEST) qsort(lib.shown, lib.shown_count, sizeof lib.shown[0], lib_sort_cmp);
}

static void lib_free_textures()
{
    Library &lib = g_app.library;
    for (i32 i = 0; i < LIB_THUMBS; i++) {
        if (lib.thumbs[i].srv) lib.thumbs[i].srv->Release();
        lib.thumbs[i] = {};
    }
    if (lib.strip) lib.strip->Release();
    lib.strip = 0;
    lib.strip_key = lib.strip_want = 0;
}

// The slot holding `key`; with `make`, a free one or the least recently shown is given to it.
static LibThumb *lib_thumb_slot(u64 key, bool make)
{
    Library &lib = g_app.library;
    LibThumb *pick = 0;
    for (i32 i = 0; i < LIB_THUMBS; i++) {
        LibThumb *t = &lib.thumbs[i];
        if (t->key == key) return t;
        if (!make || t->pending) continue;
        if (!t->key) { if (!pick || pick->key) pick = t; continue; }
        if (!pick || (pick->key && t->used < pick->used)) pick = t;
    }
    if (!pick) return 0;
    if (pick->srv) pick->srv->Release();
    *pick = {};
    pick->key = key;
    return pick;
}

static void lib_poster_landed(const LibResult &r); // below

static void lib_take_results()
{
    Library &lib = g_app.library;
    static LibResult got[64];
    AcquireSRWLockExclusive(&g_lib.lock);
    i32 n = g_lib.result_count;
    memcpy(got, g_lib.results, n * sizeof got[0]);
    g_lib.result_count = 0;
    ReleaseSRWLockExclusive(&g_lib.lock);
    for (i32 i = 0; i < n; i++) {
        LibResult &r = got[i];
        if (r.poster) lib_poster_landed(r);
        else if (r.strip) {
            if (!r.failed && r.key == lib.strip_want && g_app.ui.shown) {
                if (lib.strip) lib.strip->Release();
                lib.strip = gpu_texture(r.pixels, r.w, r.h);
                lib.strip_key = r.key;
                lib.strip_frames = r.frames;
            }
        } else if (g_app.ui.shown) {
            LibThumb *t = lib_thumb_slot(r.key, true);
            if (t) {
                t->pending = false;
                t->failed = r.failed;
                if (!r.failed) t->srv = gpu_texture(r.pixels, r.w, r.h);
                t->w = r.w;
                t->h = r.h;
                t->used = ++lib.thumb_clock;
            }
        }
        if (r.pixels) HeapFree(GetProcessHeap(), 0, r.pixels);
    }
}

// The thumbnail for a card, asked for if it isn't there yet. Null until it's ready.
static LibThumb *lib_thumb(const LibItem &it)
{
    Library &lib = g_app.library;
    LibThumb *t = lib_thumb_slot(it.key, false);
    if (t) {
        t->used = ++lib.thumb_clock;
        return t;
    }
    if (!it.probed || it.info.duration <= 0) return 0;
    t = lib_thumb_slot(it.key, true);
    if (!t) return 0;
    t->pending = true;
    t->used = ++lib.thumb_clock;
    LibTask task = {};
    task.kind = LT_THUMB;
    task.key = it.key;
    task.duration = it.info.duration;
    task.path = lib_item_path(it);
    // A clip starts on the moment; a session often starts on a menu or the desktop, so it shows its middle.
    task.poster = it.kind == LIB_SESSIONS ? LIB_STRIP_FRAMES / 2 : 0;
    lib_push(task, false);
    return t;
}

// Asks for an item's whole strip (hover-scrub, the editor's timeline). True once it's the one loaded.
static bool lib_want_strip(const LibItem &it)
{
    Library &lib = g_app.library;
    if (lib.strip_key == it.key && lib.strip) return true;
    if (lib.strip_want != it.key && it.probed && it.info.duration > 0) {
        lib.strip_want = it.key;
        LibTask task = {};
        task.kind = LT_STRIP;
        task.key = it.key;
        task.duration = it.info.duration;
        task.path = lib_item_path(it);
        lib_push(task, true);
    }
    return false;
}

// A page change: thumbnails asked for but not started are dropped, so the new page's come first.
static void lib_drop_queued_thumbs()
{
    if (!g_lib.threads[0]) return;
    AcquireSRWLockExclusive(&g_lib.lock);
    i32 w = 0;
    for (i32 i = 0; i < g_lib.task_count; i++) {
        if (g_lib.tasks[i].kind == LT_THUMB) {
            LibThumb *t = lib_thumb_slot(g_lib.tasks[i].key, false);
            if (t && t->pending) *t = {};
            continue;
        }
        g_lib.tasks[w++] = g_lib.tasks[i];
    }
    g_lib.task_count = w;
    ReleaseSRWLockExclusive(&g_lib.lock);
}

static void lib_message(bool error, const char *fmt, ...);
static void library_rescan();

// Sets the lines of n keys (names[i] is keys[i]'s file name) in a "<key> | <value> | <file name>" file (games.txt,
// names.txt) to one value: their old lines go, and an empty value leaves none. Then a rescan shows it.
static bool lib_keyed_write(const wchar_t *file, const u64 *keys, const char *const *names, i32 n_keys, const char *value)
{
    char v[128];
    str_set(v, value);
    for (char *c = v; *c; c++)
        if (*c == '|' || *c == '\t' || *c == '\r' || *c == '\n') *c = c[0] == '|' ? '/' : ' ';
    char *t = trim(v);
    Path path = path_join(g_app.paths.config_dir, file);
    static char old[LIB_MAX_ITEMS * 200];
    size_t n = 0;
    if (FILE *f = _wfopen(path.s, L"rb")) {
        n = fread(old, 1, sizeof old - 1, f);
        fclose(f);
    }
    old[n] = 0;
    FILE *f = _wfopen(path.s, L"wb");
    if (!f) {
        lib_message(true, "Couldn't save that for %s", n_keys == 1 ? names[0] : "them");
        return false;
    }
    for (char *line = old; *line;) {
        char *end = strchr(line, '\n');
        size_t len = end ? (size_t)(end - line + 1) : strlen(line);
        unsigned long long k = 0;
        bool ours = false;
        if (sscanf(line, "%llx", &k) == 1)
            for (i32 i = 0; i < n_keys && !ours; i++) ours = keys[i] == k;
        if (!ours) fwrite(line, 1, len, f);
        line += len;
    }
    if (*t)
        for (i32 i = 0; i < n_keys; i++) fprintf(f, "%llx | %s | %s\r\n", (unsigned long long)keys[i], t, names[i]);
    fclose(f);
    library_rescan();
    return true;
}

// Gives a clip or session a game by hand.
static bool library_set_game(const LibItem &it, const char *game)
{
    char g[64];
    str_set(g, game);
    char *t = trim(g);
    if (!*t || strchr(t, '|')) return false;
    log_line("library: %s is now %s", it.name, t);
    const char *name = it.name;
    return lib_keyed_write(L"games.txt", &it.key, &name, 1, t);
}

// A display name for a file (empty takes it off).
static bool library_set_display_name(u64 key, const char *text, const char *name)
{
    log_line("library: display name on %s %s", name, text[0] ? "set" : "removed");
    return lib_keyed_write(L"names.txt", &key, &name, 1, text);
}

static void lib_message(bool error, const char *fmt, ...)
{
    Library &lib = g_app.library;
    va_list args;
    va_start(args, fmt);
    vsnprintf(lib.message, sizeof lib.message, fmt, args);
    va_end(args);
    lib.message_error = error;
    log_line("library: %s", lib.message);
}

static void library_close_player()
{
    Library &lib = g_app.library;
    player_close(&lib.player);
    lib.playing = {};
}

static void library_window_shown(bool shown)
{
    if (shown) {
        library_rescan();
        return;
    }
    library_close_player();
    player_close(&g_app.library.hover);
    player_close(&g_app.editor.player); // the editor opens it again when it's next shown
    lib_free_textures();
}

// What the clips and sessions hold together, originals and their rendered copies, as of the last scan
static u64 g_lib_folder_bytes;
static void bytes_text(i64 bytes, char *out, i32 cap); // options.cpp

static void library_on_event(const Event &e)
{
    Library &lib = g_app.library;
    switch (e.a) {
    case LIB_EV_SCANNED: {
        AcquireSRWLockShared(&g_lib.lock);
        lib.item_count = g_lib.item_count;
        memcpy(lib.items, g_lib.items, lib.item_count * sizeof lib.items[0]);
        g_lib_folder_bytes = 0;
        for (i32 i = 0; i < lib.item_count; i++)
            if (!lib.items[i].dir) g_lib_folder_bytes += lib.items[i].size + lib.items[i].render_size;
        bool running = g_lib.scan_running || g_lib.scan_queued;
        ReleaseSRWLockShared(&g_lib.lock);
        lib.scanned = true;
        lib.scanning = running;
        lib_refilter();
        break;
    }
    case LIB_EV_RESULTS: lib_take_results(); break;
    case LIB_EV_MEDIA: player_on_media(e); break;
    case LIB_EV_DELETED: {
        i32 all = (i32)(e.b >> 32), gone = (i32)(e.b & 0xffffffff);
        lib.deleting = false;
        if (gone == all) lib_message(false, "%d %s went to the Recycle Bin", gone, lib_noun(gone));
        else lib_message(true, "%d of %d %s couldn't be deleted", all - gone, all, lib_noun(all));
        library_rescan();
        break;
    }
    }
}

// The clipboard holds the file itself, so it pastes into Discord, Explorer or a chat as a file.
static bool clipboard_copy_file(const wchar_t *path)
{
    size_t n = wcslen(path);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DROPFILES) + (n + 2) * sizeof(wchar_t));
    if (!mem) return false;
    DROPFILES *df = (DROPFILES *)GlobalLock(mem);
    df->pFiles = sizeof(DROPFILES);
    df->fWide = TRUE;
    memcpy((u8 *)df + sizeof(DROPFILES), path, n * sizeof(wchar_t)); // then two nulls, from GMEM_ZEROINIT
    GlobalUnlock(mem);
    if (!OpenClipboard(g_app.ui.hwnd)) {
        GlobalFree(mem);
        return false;
    }
    EmptyClipboard();
    bool ok = SetClipboardData(CF_HDROP, mem) != 0;
    CloseClipboard();
    if (!ok) GlobalFree(mem);
    return ok;
}

// Opens Explorer on the file's folder with the file picked, on a thread of its own: SHOpenFolderAndSelectItems can
// take 20 s to return, and the window would hang that long.
static DWORD WINAPI show_in_folder_thread(void *arg)
{
    wchar_t *path = (wchar_t *)arg;
    CoInitializeEx(0, COINIT_APARTMENTTHREADED);
    PIDLIST_ABSOLUTE pidl = 0;
    if (SUCCEEDED(SHParseDisplayName(path, 0, &pidl, 0, 0))) {
        SHOpenFolderAndSelectItems(pidl, 0, 0, 0);
        CoTaskMemFree(pidl);
    }
    CoUninitialize();
    free(path);
    return 0;
}

static void show_in_folder(const wchar_t *path)
{
    wchar_t *copy = _wcsdup(path);
    if (!copy) return;
    HANDLE t = CreateThread(0, 0, show_in_folder_thread, copy, 0, 0);
    if (t) CloseHandle(t);
    else free(copy);
}

// ---------------------------------------------------------------------------------------------------------------
// Drawing

static void fmt_size(u64 bytes, char *out, i32 cap)
{
    if (bytes >= (1ull << 30)) snprintf(out, cap, "%.1f GB", bytes / 1073741824.0);
    else if (bytes >= (1ull << 20)) snprintf(out, cap, "%.0f MB", bytes / 1048576.0);
    else snprintf(out, cap, "%.0f KB", bytes / 1024.0);
}

// Days since 1601 in local time, for "today" and the Library's periods.
static i64 local_day(i64 us, SYSTEMTIME *out)
{
    SYSTEMTIME t = local_time(us);
    if (out) *out = t;
    FILETIME ft;
    SystemTimeToFileTime(&t, &ft);
    return (i64)((((u64)ft.dwHighDateTime << 32) | ft.dwLowDateTime) / 864000000000ull);
}

// The Library's heading for a time, the period it falls in: "Today", "Yesterday", "This week", "Last week",
// "This month", "Last month", then the month ("September", "May 2024" when not this year). A week starts on Monday
// (1601-01-01, day 0, was one). Returns the period's key: cards next to each other with the same key share a
// heading. `out` may be null.
static i64 lib_period(i64 us, char *out, i32 cap)
{
    SYSTEMTIME t, now;
    i64 day = local_day(us, &t), today = local_day(now_us(), &now), week = today - today % 7;
    i64 month = now.wYear * 12 + now.wMonth - 1, its = t.wYear * 12 + t.wMonth - 1;
    const char *name = 0;
    i64 key = its;
    if (day >= today) name = "Today", key = -1;
    else if (day == today - 1) name = "Yesterday", key = -2;
    else if (day >= week) name = "This week", key = -3;
    else if (day >= week - 7) name = "Last week", key = -4;
    else if (its == month) name = "This month", key = -5;
    else if (its == month - 1) name = "Last month", key = -6;
    if (!out) return key;
    if (name) snprintf(out, cap, "%s", name);
    else if (t.wYear == now.wYear) snprintf(out, cap, "%s", month_name(t.wMonth));
    else snprintf(out, cap, "%s %d", month_name(t.wMonth), t.wYear);
    return key;
}

// A clip's own date and time: "7 Oct 5:32 pm", with the year when it isn't this one (or 17:32, by the clock setting)
static void fmt_when(i64 us, char *out, i32 cap)
{
    SYSTEMTIME t, now;
    local_day(us, &t);
    local_day(now_us(), &now);
    char hm[16];
    fmt_hm(t, hm, sizeof hm);
    if (t.wYear == now.wYear) snprintf(out, cap, "%d %.3s %s", t.wDay, month_name(t.wMonth), hm);
    else snprintf(out, cap, "%d %.3s %d %s", t.wDay, month_name(t.wMonth), t.wYear, hm);
}

// What a name from the template in Options holds, read back with that template: {game}, and the date and time.
struct NameParts {
    const char *game;
    i32         game_len, year, month, day, hour, minute;
};

// Whether s matches the template from tmpl on. A template's characters a file name can't hold were dropped when
// it was named, so they're skipped here too; {game} takes as little as lets the rest match.
static bool name_match(const char *tmpl, const char *s, NameParts *p)
{
    static const char *const tokens[] = {"{game}", "{YYYY}", "{YY}", "{MM}", "{DD}", "{hh}", "{mm}", "{ss}"};
    while (*tmpl) {
        i32 k = 0;
        while (k < COUNT(tokens) && strncmp(tmpl, tokens[k], strlen(tokens[k]))) k++;
        if (k == COUNT(tokens)) {
            if (strchr("<>:\"/\\|?*", *tmpl) || (u8)*tmpl < 32) { tmpl++; continue; }
            if (*s != *tmpl) {
                if (*s) return false;
                for (; *tmpl; tmpl++) // the end of a name was trimmed of spaces and dots
                    if (*tmpl != ' ' && *tmpl != '.' && !strchr("<>:\"/\\|?*", *tmpl)) return false;
                return true;
            }
            tmpl++, s++;
            continue;
        }
        const char *rest = tmpl + strlen(tokens[k]);
        if (k == 0) {
            for (i32 n = 1; s[n - 1]; n++) {
                p->game = s;
                p->game_len = n;
                if (name_match(rest, s + n, p)) return true;
            }
            return false;
        }
        i32 digits = k == 1 ? 4 : 2, v = 0;
        for (i32 i = 0; i < digits; i++) {
            if (!isdigit((u8)s[i])) return false;
            v = v * 10 + s[i] - '0';
        }
        if (k == 1) p->year = v;
        else if (k == 2) p->year = 2000 + v;
        else if (k == 3) p->month = v;
        else if (k == 4) p->day = v;
        else if (k == 5) p->hour = v;
        else if (k == 6) p->minute = v;
        s += digits;
        tmpl = rest;
    }
    return !*s;
}

// Reads a name with the template: false unless it holds a valid date. t gets the local date and time, copy a
// " (2)" ending kept off it.
static bool lib_name_read(const char *tmpl, const char *name, NameParts &p, SYSTEMTIME &t, char (&copy)[16])
{
    char stem[300]; // " (2)": a second file of the same name
    copy[0] = 0;
    str_set(stem, name);
    size_t n = strlen(stem);
    if (n && stem[n - 1] == ')') {
        size_t i = n - 1;
        while (i && isdigit((u8)stem[i - 1])) i--;
        if (i >= 2 && i < n - 1 && stem[i - 1] == '(' && stem[i - 2] == ' ') {
            str_set(copy, stem + i - 2);
            stem[i - 2] = 0;
        }
    }
    p = {0, 0, 0, 0, 0, -1, 0};
    if (!name_match(tmpl, stem, &p)) {
        if (!copy[0]) return false;
        str_set(stem, name); // the " (n)" may belong to the name itself
        copy[0] = 0;
        p = {0, 0, 0, 0, 0, -1, 0};
        if (!name_match(tmpl, stem, &p)) return false;
    }
    if (!p.year || p.month < 1 || p.month > 12 || p.day < 1 || p.day > 31 || p.hour > 23 || p.minute > 59) return false;
    if (p.game) p.game = name + (p.game - stem); // stem is a copy of the start of name
    t = {};
    t.wYear = (WORD)p.year, t.wMonth = (WORD)p.month, t.wDay = (WORD)p.day;
    t.wHour = (WORD)(p.hour < 0 ? 0 : p.hour), t.wMinute = (WORD)p.minute;
    FILETIME ft;
    return SystemTimeToFileTime(&t, &ft) != 0;
}

// When a clip or session is from: the time in its name (by the template), else `mtime_us`
static i64 lib_name_us(const char *tmpl, const char *name, i64 mtime_us)
{
    NameParts p;
    SYSTEMTIME local, utc;
    char copy[16];
    if (!lib_name_read(tmpl, name, p, local, copy) || !TzSpecificLocalTimeToSystemTime(0, &local, &utc)) return mtime_us;
    FILETIME ft;
    SystemTimeToFileTime(&utc, &ft);
    return ((((i64)ft.dwHighDateTime << 32) | ft.dwLowDateTime) - 116444736000000000LL) / 10;
}

// A read name's {game}, where "Clip" or none is the desktop
static void lib_name_game(const NameParts &p, char *out, i32 cap)
{
    char g[256] = "";
    if (p.game) snprintf(g, sizeof g, "%.*s", (int)p.game_len, p.game);
    char *s = trim(g);
    snprintf(out, cap, "%s", !*s || !_stricmp(s, "Clip") ? GAME_DESKTOP : s);
}

// The game in a file name from the save template `tmpl`: its {game}, where "Clip" or none is the desktop. A name
// the template doesn't read isn't one of ours.
static void game_from_name(const char *tmpl, const char *name, char *out, i32 cap)
{
    NameParts p;
    SYSTEMTIME t;
    char copy[16];
    if (!lib_name_read(tmpl, name, p, t, copy)) {
        snprintf(out, cap, "%s", GAME_NOT_OURS);
        return;
    }
    lib_name_game(p, out, cap);
}

// The game in the name of a video Rekodr didn't save, when it is sure: the name starts with a game `known` knows
// and the rest is only numbers (a date and time) and words other recorders add, as in ShadowPlay's "Overwatch 2
// 2024.01.15 - 20.14.03.02.DVR" or the Game Bar's "Minecraft 2024-01-15 20-14-03". The longest such start wins.
// `known` puts a game's own name in its out and says whether it is one.
static bool game_guess(const char *name, bool (*known)(const char *, char *, i32), char *out, i32 cap)
{
    static const char *const extra[] = {"dvr", "clip", "replay", "am", "pm"};
    enum { MAX_WORDS = 24 };
    i32 at[MAX_WORDS], len[MAX_WORDS], n = 0;
    for (const char *s = name; *s && n < MAX_WORDS;) {
        while (*s == ' ' || *s == '_' || *s == '-' || *s == '.') s++;
        if (!*s) break;
        const char *w = s;
        while (*s && *s != ' ' && *s != '_' && *s != '-' && *s != '.') s++;
        at[n] = (i32)(w - name), len[n++] = (i32)(s - w);
    }
    // The first word from which the rest is plain: every word numbers or one of the extra ones, with a number of
    // two or more digits among them
    i32 plain = n;
    bool number = false;
    for (i32 i = n - 1; i > 0; i--) {
        const char *w = name + at[i];
        bool digits = true;
        for (i32 c = 0; c < len[i]; c++) digits = digits && isdigit((u8)w[c]);
        bool word = false;
        for (const char *e : extra) word = word || ((i32)strlen(e) == len[i] && !_strnicmp(w, e, len[i]));
        if (!digits && !word) break;
        if (digits && len[i] >= 2) number = true;
        if (number) plain = i;
    }
    if (plain == n) return false;
    for (i32 k = n - 1; k >= 1; k--) { // the start: words 0 to k-1, then plain words to the end
        if (k < plain) break;
        char start[128];
        snprintf(start, sizeof start, "%.*s", at[k - 1] + len[k - 1], name);
        bool letter = false;
        for (const char *c = start; *c; c++) letter = letter || isalpha((u8)*c);
        if (letter && known(start, out, cap)) return true;
    }
    return false;
}

// A game in the games list, by its name (gamedb_by_name), for game_guess.
static bool game_known(const char *name, char *out, i32 cap)
{
    GameInfo g;
    if (!gamedb_by_name(name, &g)) return false;
    snprintf(out, cap, "%s", g.name);
    return true;
}

// The name a clip or session is shown with. A name from the template in Options shows only its game
// ("Minecraft", "Minecraft (2)" for a second file of that name), as the card's heading and line say when it is
// from. Any other name is shown as it is.
static void lib_shown_name(const char *name, char *out, i32 cap)
{
    snprintf(out, cap, "%s", name);
    NameParts p;
    SYSTEMTIME t;
    char copy[16], game[256];
    if (!lib_name_read(g_app.config.name_template, name, p, t, copy)) return;
    lib_name_game(p, game, sizeof game);
    snprintf(out, cap, "%s%s", game, copy);
}

// Text cut with "..." to fit `width`, in the current font.
static void text_clipped(ImDrawList *dl, ImVec2 pos, f32 width, u32 col, const char *text)
{
    ImVec2 ts = ImGui::CalcTextSize(text);
    if (ts.x <= width) {
        dl->AddText(pos, col, text);
        return;
    }
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    ImGui::RenderTextEllipsis(dl, pos, ImVec2(pos.x + width, pos.y + ts.y), pos.x + width, pos.x + width, text, 0, &ts);
    ImGui::PopStyleColor();
}

// A small label on a dark pill over a picture: a clip's length, its track count, the zoom. Returns its width.
static f32 badge(ImDrawList *dl, ImVec2 corner, bool right, bool bottom, const char *text)
{
    ImGui::PushFont(g_font_small);
    f32 px = kit_px(6), py = kit_px(2);
    ImVec2 ts = ImGui::CalcTextSize(text);
    f32 w = ts.x + 2 * px, h = ts.y + 2 * py;
    ImVec2 a(floorf(right ? corner.x - w : corner.x), floorf(bottom ? corner.y - h : corner.y));
    dl->AddRectFilled(a, ImVec2(a.x + w, a.y + h), IM_COL32(0, 0, 0, 192), h / 2);
    dl->AddText(ImVec2(a.x + px, a.y + py), IM_COL32(240, 240, 240, 255), text);
    ImGui::PopFont();
    return w;
}

static bool edit_is_advanced(const Path &edit_file); // editor.cpp
static void fmt_count(i64 n, char *out, i32 cap); // options.cpp

// Whether a card's edit is an Advanced one, for its badge. The edit file's first line is read again at most every
// 2 s per card, so an edit saved in the editor shows up without a rescan.
static bool lib_is_advanced(const LibItem &it)
{
    struct Seen { u64 key; f64 at; bool advanced; };
    static Seen seen[256];
    Seen &s = seen[it.key % COUNT(seen)];
    f64 now = ImGui::GetTime();
    if (s.key != it.key || now - s.at >= 2) {
        s.key = it.key;
        s.at = now;
        s.advanced = edit_is_advanced(lib_edit_path(it));
    }
    return s.advanced;
}

// ---------------------------------------------------------------------------------------------------------------
// Controls the library, the player and the editor share. Generic enough to move to the kit in ui.cpp.

// A play triangle on a dark disc.
static void lib_play_mark(ImDrawList *dl, ImVec2 c, f32 r)
{
    dl->AddCircleFilled(c, r, IM_COL32(0, 0, 0, 150));
    f32 t = r * 0.40f;
    dl->AddTriangleFilled(ImVec2(c.x - t * 0.7f, c.y - t), ImVec2(c.x - t * 0.7f, c.y + t), ImVec2(c.x + t * 1.1f, c.y),
                          IM_COL32_WHITE);
}

// A flat slider: a thin track filled up to the value and a knob, both a little bigger under the mouse. `mark` puts a
// tick on the track (100% on a volume); below `lo` there's none. Returns true when the value changed.
static bool lib_slider(const char *id, f32 *v, f32 lo, f32 hi, f32 w, f32 h, f32 mark = -1e9f, bool enabled = true,
                       u32 fill = COL_ACCENT)
{
    ImVec2 a = ImGui::GetCursorScreenPos();
    if (!enabled) ImGui::BeginDisabled();
    ImGui::InvisibleButton(id, ImVec2(w, h));
    if (!enabled) ImGui::EndDisabled();
    bool held = enabled && ImGui::IsItemActive(), hot = enabled && (ImGui::IsItemHovered() || held);
    f32 pad = kit_px(7); // the knob stays inside the item
    f32 x0 = a.x + pad, x1 = a.x + w - pad;
    bool changed = false;
    if (held && x1 > x0) {
        f32 u = (ImGui::GetIO().MousePos.x - x0) / (x1 - x0);
        u = u < 0 ? 0 : u > 1 ? 1 : u;
        f32 nv = lo + u * (hi - lo);
        if (nv != *v) {
            *v = nv;
            changed = true;
        }
    }
    ImDrawList *dl = ImGui::GetWindowDrawList();
    f32 cy = floorf(a.y + h / 2), th = kit_px(hot ? 5.f : 4.f);
    f32 u = hi > lo ? (*v - lo) / (hi - lo) : 0;
    u = u < 0 ? 0 : u > 1 ? 1 : u;
    f32 kx = x0 + u * (x1 - x0);
    dl->AddRectFilled(ImVec2(x0, cy - th / 2), ImVec2(x1, cy + th / 2), col32(COL_HOVER), th);
    if (mark >= lo && mark <= hi) {
        f32 mx = floorf(x0 + (mark - lo) / (hi - lo) * (x1 - x0));
        dl->AddRectFilled(ImVec2(mx - kit_px(1), cy - kit_px(6)), ImVec2(mx + kit_px(1), cy + kit_px(6)), col32(COL_FAINT));
    }
    dl->AddRectFilled(ImVec2(x0, cy - th / 2), ImVec2(kx, cy + th / 2), col32(enabled ? fill : COL_FAINT), th);
    dl->AddCircleFilled(ImVec2(kx, cy), kit_px(hot ? 7.f : 6.f), col32(enabled ? 0xffffff : COL_FAINT));
    return changed;
}

// Page controls of a fixed width: "<  3 / 13  >". lib_pager_w() is how wide they are.
static f32 lib_pager_w() { return 2 * ImGui::GetFrameHeight() + kit_px(76); }
static void lib_pager(const char *id, i32 *page, i32 pages)
{
    if (pages < 1) pages = 1;
    if (*page >= pages) *page = pages - 1;
    if (*page < 0) *page = 0;
    f32 fh = ImGui::GetFrameHeight(), tw = kit_px(76);
    ImGui::PushID(id);
    kit_next_tip("Previous page");
    if (kit_icon_button(ICON_LEFT, "##prev", KIT_GHOST, *page > 0)) (*page)--;
    ImGui::SameLine(0, 0);
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(tw, fh));
    char t[32];
    snprintf(t, sizeof t, "%d / %d", *page + 1, pages);
    ImVec2 ts = ImGui::CalcTextSize(t);
    ImGui::GetWindowDrawList()->AddText(ImVec2(floorf(p.x + (tw - ts.x) / 2), floorf(p.y + (fh - ts.y) / 2)), col32(COL_DIM), t);
    ImGui::SameLine(0, 0);
    kit_next_tip("Next page");
    if (kit_icon_button(ICON_RIGHT, "##next", KIT_GHOST, *page + 1 < pages)) (*page)++;
    ImGui::PopID();
}

// A row in a popup menu: an icon and a label, a fixed width. Closes the menu when clicked.
static bool lib_menu_item(const char *icon, const char *label, bool danger = false, f32 w = 0)
{
    f32 h = kit_px(28);
    if (w <= 0) w = kit_px(216);
    ImVec2 a = ImGui::GetCursorScreenPos();
    char id[64];
    snprintf(id, sizeof id, "##%s", label);
    bool clicked = ImGui::Selectable(id, false, 0, ImVec2(w, h));
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - ImGui::GetStyle().ItemSpacing.y + kit_px(2)); // rows 2 apart
    if (clicked) dev_click("menu", label);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    u32 ink = danger ? 0xff8a92 : COL_TEXT;
    ImGui::PushFont(g_font_icon);
    ImVec2 is = ImGui::CalcTextSize(icon);
    dl->AddText(ImVec2(a.x + kit_px(10), floorf(a.y + (h - is.y) / 2)), col32(danger ? ink : COL_DIM), icon);
    ImGui::PopFont();
    ImVec2 ts = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(a.x + kit_px(36), floorf(a.y + (h - ts.y) / 2)), col32(ink), label);
    return clicked;
}

// A row of the games list: the game's icon (a tile with its first letter when it has none), its name, and how
// many it has on the right, with a star left of that when it's a favourite. The picked one is lit.
// Collapsed, only the icon, where it was.
static bool lib_game_item(const char *game, const char *label, i32 count, bool active, f32 w, bool collapsed,
                          const char *glyph = 0, bool favourite = false)
{
    f32 h = kit_px(30), is = kit_px(20);
    ImVec2 a = ImGui::GetCursorScreenPos();
    ImGui::PushID(label); // rows that share an ID stack (All, the folders, New folder) differ by their label
    bool clicked = ImGui::InvisibleButton("##game", ImVec2(w, h));
    ImGui::PopID();
    bool hot = ImGui::IsItemHovered();
    if (collapsed) kit_tip(label);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - ImGui::GetStyle().ItemSpacing.y + kit_px(2)); // rows 2 apart
    if (clicked) dev_click("menu", label);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    // Lit a little inside the row, so the rows above and below stay apart
    if (active || hot)
        dl->AddRectFilled(ImVec2(a.x + kit_px(2), a.y + kit_px(2)), ImVec2(a.x + w - kit_px(2), a.y + h - kit_px(2)),
                          col32(active ? COL_HOVER : COL_PANEL2), kit_px(6));
    ImVec2 i0(a.x + kit_px(8), floorf(a.y + (h - is) / 2)), i1(i0.x + is, i0.y + is);
    ID3D11ShaderResourceView *icon = game ? game_icon(game) : 0;
    if (icon) {
        dl->AddImageRounded((ImTextureID)icon, i0, i1, ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, kit_px(5));
    } else if (game) {
        dl->AddRectFilled(i0, i1, col32(COL_HOVER), kit_px(5));
        char letter[2] = {(char)toupper((u8)game[0]), 0};
        ImVec2 ls = g_font_bold->CalcTextSizeA(g_font_bold->FontSize, FLT_MAX, 0, letter);
        dl->AddText(g_font_bold, g_font_bold->FontSize, ImVec2(floorf(i0.x + (is - ls.x) / 2), floorf(i0.y + (is - ls.y) / 2)),
                    col32(COL_DIM), letter);
    } else {
        ImGui::PushFont(g_font_icon);
        const char *ic = glyph ? glyph : ICON_APPS;
        ImVec2 gs = ImGui::CalcTextSize(ic);
        dl->AddText(ImVec2(floorf(i0.x + (is - gs.x) / 2), floorf(i0.y + (is - gs.y) / 2)), col32(COL_DIM), ic);
        ImGui::PopFont();
    }
    if (collapsed) return clicked;
    char n[16] = "";
    if (count >= 0) snprintf(n, sizeof n, "%d", count);
    ImVec2 ts = ImGui::CalcTextSize(label), ns = ImGui::CalcTextSize(n);
    f32 ty = floorf(a.y + (h - ts.y) / 2), nx = a.x + w - kit_px(10) - ns.x, text_end = nx - kit_px(10);
    if (favourite) { // at the same spot on every favourite row, clear of a count up to four digits
        f32 psz = kit_px(12), slot = ImGui::CalcTextSize("0000").x;
        ImVec2 ps = g_font_icon->CalcTextSizeA(psz, FLT_MAX, 0, ICON_STAR_FILL);
        f32 px = floorf(a.x + w - kit_px(10) - slot - kit_px(6) - ps.x);
        dl->AddText(g_font_icon, psz, ImVec2(px, floorf(a.y + (h - ps.y) / 2)), col32(COL_TEXT), ICON_STAR_FILL);
        text_end = ImMin(text_end, px - kit_px(6));
    }
    dl->PushClipRect(ImVec2(a.x, a.y), ImVec2(text_end, a.y + h), true);
    dl->AddText(ImVec2(i1.x + kit_px(10), ty), col32(active ? COL_WHITE : COL_TEXT), label);
    dl->PopClipRect();
    dl->AddText(ImVec2(nx, ty), col32(COL_DIM), n);
    return clicked;
}

// A pill of the Library's header that drops a list: an optional picture, its label from the left and an arrow on
// the right, outlined in white when `on`: when what it is set to narrows what is shown.
static bool lib_pill(const char *id, const char *label, f32 w, bool on, ID3D11ShaderResourceView *picture = 0)
{
    f32 h = ImGui::GetFrameHeight(), round = h / 2;
    ImVec2 a = ImGui::GetCursorScreenPos(), z(a.x + w, a.y + h);
    bool clicked = ImGui::InvisibleButton(id, ImVec2(w, h)) && !kit_closed_here(ImGui::GetItemID());
    if (clicked) dev_click("pill", label);
    bool hot = ImGui::IsItemHovered();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    kit_shadow(dl, a, z, round);
    dl->AddRectFilled(a, z, col32(hot ? COL_HOVER : COL_PANEL2), round);
    f32 ty = floorf(a.y + (h - g_font->FontSize) / 2);
    if (on) dl->AddRect(a, z, col32(COL_WHITE, 0.7f), round, 0, kit_px(1));
    f32 x = a.x + kit_px(12), is = kit_px(20);
    if (picture) {
        ImVec2 i0(x - kit_px(3), floorf(a.y + (h - is) / 2));
        dl->AddImageRounded((ImTextureID)picture, i0, ImVec2(i0.x + is, i0.y + is), ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, kit_px(3));
        x = i0.x + is + kit_px(8);
    }
    f32 cw = g_font_small->CalcTextSizeA(g_font_small->FontSize, FLT_MAX, 0, ICON_DOWN).x, cx = z.x - kit_px(12) - cw;
    dl->AddText(g_font_small, g_font_small->FontSize, ImVec2(cx, floorf(a.y + (h - g_font_small->FontSize) / 2)), col32(COL_DIM), ICON_DOWN);
    dl->PushClipRect(ImVec2(x, a.y), ImVec2(cx - kit_px(6), z.y), true);
    dl->AddText(g_font, g_font->FontSize, ImVec2(x, ty), col32(COL_TEXT), label);
    dl->PopClipRect();
    return clicked;
}

// A two-sided switch (Clips / Sessions, Games / Folders): one button, w by h, that a click anywhere flips. The lit
// half shows which side is on; under the mouse the whole track lightens. True when clicked.
static bool lib_switch(const char *id, const char *const *labels, f32 w, f32 h, bool second, u32 track, u32 track_hot,
                       u32 lit)
{
    ImVec2 a = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::InvisibleButton(id, ImVec2(w, h)) && !kit_closed_here(ImGui::GetItemID());
    if (clicked) dev_click("switch", labels[second ? 0 : 1]);
    bool hot = ImGui::IsItemHovered();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    f32 round = ImGui::GetStyle().FrameRounding, in = kit_px(3);
    kit_shadow(dl, a, ImVec2(a.x + w, a.y + h), round);
    dl->AddRectFilled(a, ImVec2(a.x + w, a.y + h), col32(hot ? track_hot : track), round);
    for (i32 s = 0; s < 2; s++) {
        ImVec2 p(a.x + floorf(s * w / 2), a.y), q(a.x + floorf((s + 1) * w / 2), a.y + h);
        bool on = (s == 1) == second;
        if (on) dl->AddRectFilled(ImVec2(p.x + in, p.y + in), ImVec2(q.x - in, q.y - in), col32(lit), ImMax(round - in, 0.0f));
        ImFont *font = on ? g_font_bold : g_font;
        ImVec2 ts = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0, labels[s]);
        dl->AddText(font, font->FontSize, ImVec2(floorf((p.x + q.x - ts.x) / 2), floorf(a.y + (h - ts.y) / 2)),
                    col32(on || hot ? COL_TEXT : COL_DIM), labels[s]);
    }
    return clicked;
}

// A row of `n` equal segments, `on` lit, like lib_switch. Returns the segment clicked, else -1.
static i32 lib_segments(const char *id, const char *const *labels, i32 n, f32 w, f32 h, i32 on, u32 track, u32 track_hot,
                        u32 lit)
{
    ImVec2 a = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::InvisibleButton(id, ImVec2(w, h)) && !kit_closed_here(ImGui::GetItemID());
    bool hot = ImGui::IsItemHovered();
    i32 under = hot ? (i32)((ImGui::GetIO().MousePos.x - a.x) / w * n) : -1;
    if (under >= n) under = n - 1;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    f32 round = ImGui::GetStyle().FrameRounding, in = kit_px(3);
    kit_shadow(dl, a, ImVec2(a.x + w, a.y + h), round);
    dl->AddRectFilled(a, ImVec2(a.x + w, a.y + h), col32(hot ? track_hot : track), round);
    for (i32 s = 0; s < n; s++) {
        ImVec2 p(a.x + floorf(s * w / n), a.y), q(a.x + floorf((s + 1) * w / n), a.y + h);
        bool lit_s = s == on;
        if (lit_s) dl->AddRectFilled(ImVec2(p.x + in, p.y + in), ImVec2(q.x - in, q.y - in), col32(lit), ImMax(round - in, 0.0f));
        ImFont *font = lit_s ? g_font_bold : g_font;
        ImVec2 ts = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0, labels[s]);
        dl->AddText(font, font->FontSize, ImVec2(floorf((p.x + q.x - ts.x) / 2), floorf(a.y + (h - ts.y) / 2)),
                    col32(lit_s || s == under ? COL_TEXT : COL_DIM), labels[s]);
    }
    if (!clicked || under < 0) return -1;
    dev_click("switch", labels[under]);
    return under;
}

// Popup and dialog looks: roomy, rounded, no title bar.
static void lib_popup_style_push(f32 pad)
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(kit_px(pad), kit_px(pad)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(kit_px(8), pad < 10 ? kit_px(2) : kit_px(10)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, kit_px(10));
}
static void lib_popup_style_pop() { ImGui::PopStyleVar(3); }

// ---------------------------------------------------------------------------------------------------------------
// The Library screen

// Opens a clip in the player, from its start: pointing at a card only previews it.
static void library_play(const LibItem &it)
{
    Library &lib = g_app.library;
    player_close(&lib.hover);
    lib.playing = it;
    lib.player.id = 0;
    player_open(&lib.player, lib_item_path(it), 0, true);
}

static void lib_rename_start(const LibItem &it, bool display_name = false);
static void lib_folder_rename_start(i32 i);

// After the screen is drawn: the card preview gets its next frame, and stops once no card has the mouse.
static void library_hover_end_frame()
{
    Library &lib = g_app.library;
    if (lib.hover_frame != ImGui::GetFrameCount()) {
        lib.hover_key = 0;
        player_close(&lib.hover);
    }
    if (lib.hover.open) player_update(&lib.hover);
}

static char g_game_pick_typed[64]; // what the picker opens with
static bool g_game_pick_show;      // `show game`: the editor's picker opens

// A row of the game picker: a game in the library, one from the games list (gamedb_find_local), one from the art
// service, or the name as typed (a custom game).
enum GamePickKind : u8 { PICK_LIBRARY, PICK_LIST, PICK_ONLINE, PICK_TYPED };
struct GamePickRow {
    char name[64];
    i32  year;
    u8   kind; // GamePickKind
};

// The picker's rows for what's typed, at most `max`: a library or list game that is exactly what's typed first, then
// the library's games (`games`, n_games of them) that match, then the games list's (`local`, n_local), then the art
// service's (`online`, n_online), and last, with something typed, the name as typed unless a row has it already. A
// name is one row, however it's written. The art service's answer only adds rows below the others.
static i32 game_pick_rows(const char *typed, const char (*games)[64], i32 n_games, const char (*local)[128], i32 n_local,
                          const char (*online)[128], const i32 *online_years, i32 n_online, GamePickRow *rows, i32 max)
{
    char q[64], want[128], letters[128];
    str_set(q, typed);
    char *tq = trim(q);
    gdb_letters(tq, want, sizeof want);
    i32 n = 0, cap = *tq ? max - 1 : max; // with something typed, the last row is the name as typed
    auto add = [&](const char *name, i32 year, u8 kind) {
        if (n >= cap || !name[0]) return;
        gdb_letters(name, letters, sizeof letters);
        for (i32 d = 0; d < n; d++) {
            char seen[128];
            gdb_letters(rows[d].name, seen, sizeof seen);
            if (!strcmp(seen, letters)) return;
        }
        str_set(rows[n].name, name);
        rows[n].year = year;
        rows[n++].kind = kind;
    };
    for (i32 pass = 0; pass < 2; pass++) { // 0: what's typed, exactly; 1: the rest
        for (i32 i = 0; i < n_games; i++) {
            const char *g = games[i];
            if (!strcmp(g, GAME_NOT_OURS) || (*tq && !lib_matches(g, tq))) continue;
            gdb_letters(g, letters, sizeof letters);
            if (pass == 1 || (*tq && !strcmp(letters, want))) add(g, 0, PICK_LIBRARY);
        }
        for (i32 i = 0; i < n_local; i++) {
            gdb_letters(local[i], letters, sizeof letters);
            if (pass == 1 || !strcmp(letters, want)) add(local[i], 0, PICK_LIST);
        }
    }
    for (i32 i = 0; i < n_online; i++) add(online[i], online_years[i], PICK_ONLINE);
    if (*tq) {
        cap = max;
        add(tq, 0, PICK_TYPED);
    }
    return n;
}

// Whether a pick is a game the art service may be told of: one from the games list or the art service, or a real
// game in the library.
static bool game_pick_known(const GamePickRow &r)
{
    return r.kind == PICK_LIST || r.kind == PICK_ONLINE ||
           (r.kind == PICK_LIBRARY && !game_is_none(r.name) && !game_is_custom(r.name));
}

// A pick has been filed: one typed is a custom game from now on, and one from the games list or the art service isn't.
static void game_pick_filed(const GamePickRow &r)
{
    if (r.kind == PICK_TYPED) game_set_custom(r.name, true);
    else if (r.kind == PICK_LIST || r.kind == PICK_ONLINE) game_set_custom(r.name, false);
}

// "Change game": a search box over the games (game_pick_rows). The library's games and the games list answer each
// key at once; the art service is asked once the text stays the same a moment, and its answer shows only while it
// is for the text in the box. One row is lit: the first, or the one the arrow keys or the mouse moved to, and it
// stays lit when rows come in under it. Enter picks the row lit as it was last drawn, never one still to come. The
// rows and the line under them keep their place. `current` is the game whose name is in the accent colour. The
// popup `id` is opened by the caller; returns the row picked, else null.
static const GamePickRow *lib_game_pick(const char *id, const char *current)
{
    Library &lib = g_app.library;
    const i32 ROWS = 7;
    static char typed[64];
    static f64 changed_at;
    static GamePickRow rows[ROWS], drawn[ROWS], picked;
    static i32 lit, drawn_n;
    ImGui::SetNextWindowSize(ImVec2(kit_px(360), 0));
    lib_popup_style_push(10);
    bool open = ImGui::BeginPopup(id);
    lib_popup_style_pop();
    if (!open) return 0;
    if (ImGui::IsWindowAppearing()) {
        str_set(typed, g_game_pick_typed);
        g_game_pick_typed[0] = 0;
        changed_at = ImGui::GetTime();
        lit = drawn_n = 0;
        ImGui::SetKeyboardFocusHere();
    }
    // The search box: framed like the inputs in Options' popups
    ImGui::PushStyleColor(ImGuiCol_FrameBg, rgb(COL_HOVER));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(kit_px(12), kit_px(10)));
    ImGui::SetNextItemWidth(-1);
    bool enter = ImGui::InputTextWithHint("##game_q", "Search for a game", typed, sizeof typed,
                                          ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    bool edited = ImGui::IsItemEdited();
    if (edited) {
        changed_at = ImGui::GetTime();
        lit = 0;
    }
    const GamePickRow *got = 0;
    if (enter && lit < drawn_n) {
        picked = drawn[lit];
        got = &picked;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) lit = ImMin(lit + 1, ImMax(drawn_n - 1, 0));
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) lit = ImMax(lit - 1, 0);
    char sq[64];
    str_set(sq, typed);
    char *ts = trim(sq);
    static char local[ROWS][128], online[8][128];
    static i32 online_years[8];
    i32 nl = gamedb_find_local(ts, local, ROWS);
    // The art service is asked once the text has stayed the same a moment, not for each key
    bool settled = ImGui::GetTime() - changed_at > 0.15;
    i32 no = *ts && settled ? gamedb_find_online(ts, online, online_years, 8) : *ts ? -1 : 0;
    i32 n = game_pick_rows(typed, lib.games, lib.game_count, local, nl, online, online_years, ImMax(no, 0), rows, ROWS);
    // New rows come in under the lit one; should it move all the same, the light goes with it
    if (!edited && lit < drawn_n && (lit >= n || strcmp(rows[lit].name, drawn[lit].name) || rows[lit].kind != drawn[lit].kind))
        for (i32 i = 0; i < n; i++)
            if (!strcmp(rows[i].name, drawn[lit].name) && rows[i].kind == drawn[lit].kind) lit = i;
    lit = ImMin(lit, ImMax(n - 1, 0));

    f32 w = ImGui::GetContentRegionAvail().x, rh = kit_px(38), is = kit_px(26);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 l0 = ImGui::GetCursorScreenPos();
    l0.y += kit_px(4);
    bool moved = ImGui::GetIO().MouseDelta.x != 0 || ImGui::GetIO().MouseDelta.y != 0;
    for (i32 i = 0; i < n; i++) {
        const GamePickRow &r = rows[i];
        ImVec2 r0(l0.x, l0.y + i * rh);
        ImGui::SetCursorScreenPos(r0);
        ImGui::PushID(i);
        if (ImGui::Selectable("##game_row", i == lit, 0, ImVec2(w, rh - kit_px(2))) && !got) {
            picked = r;
            got = &picked;
        }
        if (moved && ImGui::IsItemHovered()) lit = i;
        ImGui::PopID();
        // The icon: the game's, else a glyph (a controller for a custom game, the monitor for Desktop) or a tile
        ImVec2 i0(r0.x + kit_px(6), floorf(r0.y + (rh - kit_px(2) - is) / 2));
        bool custom = r.kind == PICK_TYPED || (r.kind == PICK_LIBRARY && game_is_custom(r.name));
        ID3D11ShaderResourceView *icon = custom ? 0 : game_db_icon(r.name);
        const char *glyph = custom ? ICON_GAME : !strcmp(r.name, GAME_DESKTOP) ? ICON_MONITOR : 0;
        if (icon) dl->AddImageRounded((ImTextureID)icon, i0, ImVec2(i0.x + is, i0.y + is), ImVec2(0, 0), ImVec2(1, 1),
                                      IM_COL32_WHITE, kit_px(5));
        else if (glyph) {
            ImGui::PushFont(g_font_icon);
            ImVec2 gs = ImGui::CalcTextSize(glyph);
            dl->AddText(ImVec2(floorf(i0.x + (is - gs.x) / 2), floorf(i0.y + (is - gs.y) / 2)), col32(COL_DIM), glyph);
            ImGui::PopFont();
        } else dl->AddRectFilled(i0, ImVec2(i0.x + is, i0.y + is), col32(COL_HOVER), kit_px(5));
        char note[16] = "", label[96];
        if (r.kind == PICK_LIBRARY) str_set(note, "In the library");
        else if (r.year) snprintf(note, sizeof note, "%d", r.year);
        if (r.kind == PICK_TYPED) snprintf(label, sizeof label, "Use \"%s\" as a new game", r.name);
        else str_set(label, r.name);
        f32 right = r0.x + w - kit_px(8);
        if (note[0]) {
            f32 nw = g_font_small->CalcTextSizeA(g_font_small->FontSize, FLT_MAX, 0, note).x;
            right -= nw;
            dl->AddText(g_font_small, g_font_small->FontSize,
                        ImVec2(right, floorf(r0.y + (rh - kit_px(2) - g_font_small->FontSize) / 2)), col32(COL_FAINT), note);
            right -= kit_px(10);
        }
        text_clipped(dl, ImVec2(i0.x + is + kit_px(10), floorf(r0.y + (rh - kit_px(2) - ImGui::GetFontSize()) / 2)),
                     right - (i0.x + is + kit_px(10)), col32(!strcmp(r.name, current) ? COL_WHITE : COL_DIM), label);
    }
    memcpy(drawn, rows, n * sizeof rows[0]);
    drawn_n = n;
    ImGui::SetCursorScreenPos(ImVec2(l0.x, l0.y + ROWS * rh));
    kit_dim(no < 0 ? "Searching..." : n ? " " : "Type the game's name");
    if (got) ImGui::CloseCurrentPopup();
    else if (ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    return got;
}

// The picker for one card: a pick files `it` under the game, and a known one is told to the art service
// (gamedb_clip_game). True when the game changed.
static bool lib_game_picker(const char *id, LibItem &it)
{
    const GamePickRow *r = lib_game_pick(id, it.info.game);
    if (!r || !strcmp(r->name, it.info.game) || !library_set_game(it, r->name)) return false;
    bool known = game_pick_known(*r);
    game_pick_filed(*r);
    str_set(it.info.game, r->name);
    if (known) gamedb_clip_game(it, r->name);
    return true;
}

// A game field (the editor's): the game, or "None", on a button `w` wide that opens the picker under it. True when
// the game changed.
static bool lib_game_field(const char *id, LibItem &it, f32 w)
{
    ImGui::PushID(id);
    ImVec2 a = ImGui::GetCursorScreenPos();
    f32 fh = ImGui::GetFrameHeight();
    bool hit = ImGui::InvisibleButton("##field", ImVec2(w, fh));
    bool hot = ImGui::IsItemHovered();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(a, ImVec2(a.x + w, a.y + fh), col32(hot ? COL_HOVER : COL_PANEL2), ImGui::GetStyle().FrameRounding);
    f32 pad = ImGui::GetStyle().FramePadding.x, ty = floorf(a.y + (fh - ImGui::GetFontSize()) / 2);
    ImGui::PushFont(g_font_icon);
    ImVec2 cs = ImGui::CalcTextSize(ICON_DOWN);
    dl->AddText(ImVec2(a.x + w - pad - cs.x, floorf(a.y + (fh - cs.y) / 2)), col32(COL_DIM), ICON_DOWN);
    ImGui::PopFont();
    text_clipped(dl, ImVec2(a.x + pad, ty), w - 3 * pad - cs.x, col32(it.info.game[0] ? COL_TEXT : COL_DIM),
                 it.info.game[0] ? it.info.game : "None");
    if (hit || g_game_pick_show) ImGui::OpenPopup("game_pick");
    g_game_pick_show = false;
    ImGui::SetNextWindowPos(ImVec2(a.x, a.y + fh + kit_px(4)), ImGuiCond_Appearing);
    bool changed = lib_game_picker("game_pick", it);
    ImGui::PopID();
    return changed;
}

static bool lib_dialog_begin(const char *id);
static void lib_dialog_end();
static i32 lib_dialog_pair(const char *action, KitKind kind, f32 right);

// The Comment dialog: the card's name, a box for the comment (several lines) and Cancel / Save. lib_comment_open asks for it; lib_comment_dialog draws it, where the card menu and the player are.
static LibItem g_comment_item;
static char    g_comment_buf[LIB_COMMENT_MAX + 1];
static bool    g_comment_ask, g_comment_focus;

static void lib_comment_open(const LibItem &it)
{
    g_comment_item = it;
    str_set(g_comment_buf, lib_comment(it.key));
    g_comment_ask = g_comment_focus = true;
}

// Saves a file's comment (empty takes it off).
static bool library_set_comment(u64 key, const char *text, const char *name)
{
    char t[LIB_COMMENT_MAX + 1];
    str_set(t, text);
    char *s = trim(t);
    if (!strcmp(s, lib_comment(key))) return true;
    lib_comment_put(key, name, s);
    log_line("library: comment on %s %s", name, *s ? "set" : "removed");
    lib_refilter();
    if (lib_comments_save()) return true;
    lib_message(true, "Couldn't save the comment on %s", name);
    return false;
}

// A box for a comment, w x h, with a dim hint while it's empty. True when it's left after a change.
static bool lib_comment_box(const char *id, char *buf, i32 cap, f32 w, f32 h)
{
    ImVec2 a = ImGui::GetCursorScreenPos();
    ImGui::InputTextMultiline(id, buf, cap, ImVec2(w, h), ImGuiInputTextFlags_NoHorizontalScroll);
    bool done = ImGui::IsItemDeactivatedAfterEdit();
    if (!buf[0] && !ImGui::IsItemActive()) {
        ImVec2 pad = ImGui::GetStyle().FramePadding;
        ImGui::GetWindowDrawList()->AddText(ImVec2(a.x + pad.x, a.y + pad.y), col32(COL_FAINT), "Add a comment");
    }
    return done;
}

static void lib_comment_dialog()
{
    if (g_comment_ask) ImGui::OpenPopup("Comment");
    g_comment_ask = false;
    if (!lib_dialog_begin("Comment")) return;
    f32 w = kit_px(460);
    ImGui::PushFont(g_font_title);
    ImGui::TextUnformatted("Comment");
    ImGui::PopFont();
    char shown[300];
    const LibItem &it = g_comment_item;
    if (it.display_name[0]) str_set(shown, it.display_name);
    else lib_shown_name(it.name, shown, sizeof shown);
    ImVec2 n = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(w, ImGui::GetTextLineHeight()));
    text_clipped(ImGui::GetWindowDrawList(), n, w, col32(COL_DIM), shown);
    ImGui::Dummy(ImVec2(1, kit_px(4)));
    if (g_comment_focus) {
        ImGui::SetKeyboardFocusHere();
        g_comment_focus = false;
    }
    lib_comment_box("##comment", g_comment_buf, sizeof g_comment_buf, w, 7 * ImGui::GetTextLineHeight() + 2 * ImGui::GetStyle().FramePadding.y);
    ImGui::Dummy(ImVec2(1, kit_px(8)));
    i32 b = lib_dialog_pair(ICON_SAVE "  Save", KIT_PRIMARY, n.x + w);
    if (b == 1) library_set_comment(it.key, g_comment_buf, it.name);
    if (b) ImGui::CloseCurrentPopup();
    else if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
    lib_dialog_end();
}

// The Tags dialog: the card's name, a chip for every tag in the library (lit when the card has it, a click puts it on
// or takes it off), a box that adds a new one on Enter, and Cancel / Save. The chips sit in a box of a set height so
// nothing below them moves as tags are added. lib_tags_open asks for it; lib_tags_dialog draws it.
static LibItem g_tags_item;
static char    g_tags_buf[LIB_TAGS_MAX], g_tags_new[LIB_TAG_MAX];
static bool    g_tags_ask, g_tags_focus;

static void lib_tags_open(const LibItem &it)
{
    g_tags_item = it;
    str_set(g_tags_buf, lib_tags(it.key));
    g_tags_new[0] = 0;
    g_tags_ask = g_tags_focus = true;
}

// A tag as typed made into one: lower case, no '#' in front, no comma or '|', no spaces at either end.
static void lib_tag_clean(const char *in, char *out, i32 cap)
{
    i32 n = 0;
    while (*in == ' ' || *in == '#') in++;
    for (; *in && n < cap - 1; in++)
        if (*in != ',' && *in != '|') out[n++] = (char)tolower((unsigned char)*in);
    while (n && out[n - 1] == ' ') n--;
    out[n] = 0;
}

// Puts tag on or takes it off a comma-separated list. False when the list has no room for it.
static bool lib_tag_set(char *tags, i32 cap, const char *tag, bool on)
{
    if (!*tag || lib_tag_has(tags, tag) == on) return true;
    if (on) {
        i32 n = (i32)strlen(tags), k = (i32)strlen(tag);
        if (n + (n ? 1 : 0) + k >= cap) return false;
        snprintf(tags + n, cap - n, "%s%s", n ? "," : "", tag);
        return true;
    }
    char out[LIB_TAGS_MAX] = "";
    i32 o = 0;
    size_t len = strlen(tag);
    for (const char *s = tags; *s;) {
        const char *e = strchr(s, ',');
        size_t l = e ? (size_t)(e - s) : strlen(s);
        if (!(l == len && !_strnicmp(s, tag, len)) && o + (i32)l + 1 < (i32)sizeof out)
            o += snprintf(out + o, sizeof out - o, "%s%.*s", o ? "," : "", (int)l, s);
        if (!e) break;
        s = e + 1;
    }
    snprintf(tags, cap, "%s", out);
    return true;
}

// Saves a file's tags (empty takes them off).
static bool library_set_tags(u64 key, const char *tags, const char *name)
{
    if (!strcmp(tags, lib_tags(key))) return true;
    lib_tags_put(key, name, tags);
    log_line("library: tags on %s %s", name, *tags ? "set" : "removed");
    lib_refilter();
    if (lib_tags_save()) return true;
    lib_message(true, "Couldn't save the tags on %s", name);
    return false;
}

static int lib_tag_cmp(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }

// Every tag in the library and in list, each once, in A-Z order. The count.
static i32 lib_tag_names(const char *list, char (*out)[LIB_TAG_MAX], i32 max)
{
    i32 n = 0;
    for (i32 i = -1; i < g_clip_tag_count; i++) {
        const char *tags = i < 0 ? list : g_clip_tags[i].tags;
        for (const char *s = tags; *s && n < max;) {
            const char *e = strchr(s, ',');
            i32 l = e ? (i32)(e - s) : (i32)strlen(s);
            char t[LIB_TAG_MAX];
            snprintf(t, sizeof t, "%.*s", l, s);
            i32 k = 0;
            while (k < n && strcmp(out[k], t)) k++;
            if (k == n && t[0]) snprintf(out[n++], LIB_TAG_MAX, "%s", t);
            if (!e) break;
            s = e + 1;
        }
    }
    qsort(out, n, sizeof out[0], lib_tag_cmp);
    return n;
}

static void lib_tags_dialog()
{
    if (g_tags_ask) ImGui::OpenPopup("Tags");
    g_tags_ask = false;
    if (!lib_dialog_begin("Tags")) return;
    f32 w = kit_px(460);
    ImGui::PushFont(g_font_title);
    ImGui::TextUnformatted("Tags");
    ImGui::PopFont();
    char shown[300];
    const LibItem &it = g_tags_item;
    if (it.display_name[0]) str_set(shown, it.display_name);
    else lib_shown_name(it.name, shown, sizeof shown);
    ImVec2 n = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(w, ImGui::GetTextLineHeight()));
    text_clipped(ImGui::GetWindowDrawList(), n, w, col32(COL_DIM), shown);
    ImGui::Dummy(ImVec2(1, kit_px(4)));

    // The chips, wrapped, in a box three rows high that scrolls past that
    static char names[128][LIB_TAG_MAX];
    i32 count = lib_tag_names(g_tags_buf, names, COUNT(names));
    f32 fh = ImGui::GetFrameHeight(), gap = kit_px(6);
    ImGui::BeginChild("##tag_chips", ImVec2(w, 3 * fh + 2 * gap), ImGuiChildFlags_None);
    if (!count) ImGui::TextColored(rgb(COL_FAINT), "No tags yet. Type one below");
    f32 x0 = ImGui::GetCursorScreenPos().x, right = x0 + w - ImGui::GetStyle().ScrollbarSize;
    for (i32 i = 0; i < count; i++) {
        char label[LIB_TAG_MAX + 8];
        snprintf(label, sizeof label, "%s##tag%d", names[i], i);
        f32 cw = ImGui::CalcTextSize(names[i]).x + 2 * kit_px(14);
        if (i) {
            ImGui::SameLine(0, gap);
            if (ImGui::GetCursorScreenPos().x + cw > right) ImGui::SetCursorScreenPos(ImVec2(x0, ImGui::GetCursorScreenPos().y + fh + gap));
        }
        bool on = lib_tag_has(g_tags_buf, names[i]);
        if (kit_chip(label, on)) lib_tag_set(g_tags_buf, sizeof g_tags_buf, names[i], !on);
    }
    kit_scroll_fade_window(COL_PANEL2);
    ImGui::EndChild();
    ImGui::Dummy(ImVec2(1, kit_px(2)));

    // A new tag: Enter puts it on and keeps the box for the next
    if (g_tags_focus) {
        ImGui::SetKeyboardFocusHere();
        g_tags_focus = false;
    }
    ImVec2 a = ImGui::GetCursorScreenPos();
    ImGui::SetNextItemWidth(w);
    if (ImGui::InputText("##new_tag", g_tags_new, sizeof g_tags_new, ImGuiInputTextFlags_EnterReturnsTrue)) {
        char t[LIB_TAG_MAX];
        lib_tag_clean(g_tags_new, t, sizeof t);
        if (lib_tag_set(g_tags_buf, sizeof g_tags_buf, t, true)) g_tags_new[0] = 0;
        ImGui::SetKeyboardFocusHere(-1);
    }
    if (!g_tags_new[0] && !ImGui::IsItemActive()) {
        ImVec2 pad = ImGui::GetStyle().FramePadding;
        ImGui::GetWindowDrawList()->AddText(ImVec2(a.x + pad.x, a.y + pad.y), col32(COL_FAINT), "Add a tag");
    }
    ImGui::Dummy(ImVec2(1, kit_px(8)));
    i32 b = lib_dialog_pair(ICON_SAVE "  Save", KIT_PRIMARY, n.x + w);
    if (b == 1) library_set_tags(it.key, g_tags_buf, it.name);
    if (b) ImGui::CloseCurrentPopup();
    else if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
    lib_dialog_end();
}

// ---------------------------------------------------------------------------------------------------------------
// Share (share.cpp) and links (r2.cpp): Share makes a file or a link and puts it on the clipboard; Share again in the
// Shared list uploads a removed link's file again. The jobs under way, then what they did or why one failed, show in
// a note at the bottom of the window.

enum { LIB_UPLOADS_MAX = 8 };
struct LibUpload {
    u32  id;
    bool share; // a share_start job, else an r2 one (Share again)
    char name[128];
};
static LibUpload g_lib_uploads[LIB_UPLOADS_MAX]; // oldest first
static i32 g_lib_upload_count;
static char g_lib_note[200];
static bool g_lib_note_error;
static i64 g_lib_note_us;  // when the note was set; it shows for a few seconds

static void lib_note(bool error, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vsnprintf(g_lib_note, sizeof g_lib_note, fmt, args);
    va_end(args);
    g_lib_note_error = error;
    g_lib_note_us = now_us();
    log_line("library: %s", g_lib_note);
    ui_wake();
}

// What an upload is doing; its progress starts again at each step.
static const char *lib_upload_verb(R2State s)
{
    return s == R2_QUEUED ? "Waiting to upload" : s == R2_PREPARING ? "Compressing for upload" : "Uploading";
}

// A size as Cloudflare counts it, in thousands: "3.2 GB", "10 GB", "640 MB".
static void lib_r2_size(i64 bytes, char *out, i32 cap)
{
    f64 gb = (f64)bytes / R2_GB;
    if (gb >= 100 || (gb >= 1 && gb == (f64)(i64)gb)) snprintf(out, cap, "%.0f GB", gb);
    else if (gb >= 1) snprintf(out, cap, "%.1f GB", gb);
    else snprintf(out, cap, "%.0f MB", (f64)bytes / 1e6);
}

static void lib_upload_add(u32 id, const char *name, bool share = false)
{
    LibUpload &u = g_lib_uploads[g_lib_upload_count++];
    u.id = id;
    u.share = share;
    snprintf(u.name, sizeof u.name, "%s", name);
    ui_wake();
}

// A job in that list as the note and the Shared list show it.
struct LibJobNow {
    bool link;     // it ends in a link
    bool waiting;  // not started yet: no percent
    f32  progress; // 0..1 in the current step
    char verb[32]; // "Compressing", "Uploading"
};

static LibJobNow lib_job_now(const LibUpload &u)
{
    LibJobNow n = {};
    if (u.share) {
        ShareJobView j = share_job(u.id);
        n.link = j.kind == SHARE_LINK;
        n.progress = j.progress;
        str_set(n.verb, j.verb[0] ? j.verb : "Sharing");
    } else {
        R2Job j = r2_job(u.id);
        n.link = true;
        n.waiting = j.state == R2_QUEUED;
        n.progress = n.waiting ? 0 : j.progress;
        str_set(n.verb, lib_upload_verb(j.state));
    }
    return n;
}

// Options, on the page with Sharing.
static void lib_open_sharing()
{
    g_app.options.tab = OPT_INTEGRATIONS;
    ui_show(SCREEN_OPTIONS);
}

// The hosts a link can go to without R2: the player's uploaders (.sxcu), each by its name (link_uploaders) and the
// one shown. Read again every few seconds, and at once after an uploader is added.
struct LinkHostName {
    char name[64], label[64];
};
static LinkHostName g_link_names[16];
static i32 g_link_name_count = -1;
static f64 g_link_names_at;

static i32 link_host_names()
{
    f64 t = ImGui::GetTime();
    if (g_link_name_count >= 0 && t - g_link_names_at < 3) return g_link_name_count;
    g_link_names_at = t;
    i32 n = 0;
    static char stems[16][64];
    static Uploader u;
    i32 m = uploader_list(stems, COUNT(stems));
    for (i32 i = 0; i < m && n < (i32)COUNT(g_link_names); i++, n++) {
        char why[160];
        str_set(g_link_names[n].name, stems[i]);
        str_set(g_link_names[n].label, uploader_load(stems[i], &u, why, sizeof why) && u.name[0] ? u.name : stems[i]);
    }
    return g_link_name_count = n;
}

// Where `name` is in a comma-separated list (link_uploaders), -1 when it isn't.
static i32 hosts_find(const char *hosts, const char *name)
{
    i32 at = 0;
    size_t nl = strlen(name);
    for (const char *p = hosts; *p; at++) {
        const char *end = strchr(p, ',');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (len == nl && !_strnicmp(p, name, len)) return at;
        if (!end) break;
        p = end + 1;
    }
    return -1;
}

// Takes `name` out of a comma-separated list (link_uploaders), or puts it on the end when it isn't there.
static void hosts_toggle(char *hosts, i32 cap, const char *name)
{
    char out[256] = "";
    bool had = false;
    for (const char *p = hosts; *p;) {
        const char *end = strchr(p, ',');
        size_t len = end ? (size_t)(end - p) : strlen(p), l = strlen(out);
        if (len == strlen(name) && !_strnicmp(p, name, len)) had = true;
        else if (len) snprintf(out + l, sizeof out - l, "%s%.*s", l ? "," : "", (int)len, p);
        if (!end) break;
        p = end + 1;
    }
    if (!had) {
        size_t l = strlen(out);
        snprintf(out + l, sizeof out - l, "%s%s", l ? "," : "", name);
    }
    snprintf(hosts, cap, "%s", out);
}

// A size in MiB as a chip shows it: "50 MB", "1 GB", "1.5 GB"
static void share_size_label(f32 mb, char *out, i32 cap)
{
    if (mb >= 1024) snprintf(out, cap, "%g GB", floorf(mb / 1024 * 10 + 0.5f) / 10);
    else snprintf(out, cap, "%.0f MB", mb);
}

// Whether links may go to the uploader named `name`.
static bool link_host_on(const char *name)
{
    return hosts_find(g_app.config.link_uploaders, name) >= 0;
}

// Switches the uploader `stem` on or off for links (link_uploaders). The caller saves the settings.
static void link_uploader_set(const char *stem, bool on)
{
    Config &c = g_app.config;
    if ((hosts_find(c.link_uploaders, stem) >= 0) != on) hosts_toggle(c.link_uploaders, sizeof c.link_uploaders, stem);
}

// Options' Add uploader...: copies the .sxcu file in (uploader_add) and switches it on. `stem` gets its name, or
// `why` says why it can't be used. The caller saves the settings.
static bool link_uploader_add(const wchar_t *file, char *stem, i32 stem_cap, char *why, i32 why_cap)
{
    if (!uploader_add(file, stem, stem_cap, why, why_cap)) return false;
    link_uploader_set(stem, true);
    g_link_name_count = -1;
    return true;
}

// Options' remove: deletes the uploader's file and switches it off. The caller saves the settings.
static bool link_uploader_remove(const char *stem)
{
    link_uploader_set(stem, false);
    g_link_name_count = -1;
    return uploader_remove(stem);
}

// A link can be made: R2 is set up, or an uploader that's on is there to take it.
static bool links_any_host()
{
    if (r2_ready()) return true;
    i32 n = link_host_names();
    for (i32 i = 0; i < n; i++)
        if (link_host_on(g_link_names[i].name)) return true;
    return false;
}

// The host a share went to as shown: an uploader's name, "" for R2.
static void share_host_label(const R2Share &s, char *out, i32 cap)
{
    out[0] = 0;
    if (s.host == HOST_CUSTOM) {
        const char *slash = strchr(s.key, '/');
        snprintf(out, cap, "%.*s", (int)(slash ? slash - s.key : (i64)strlen(s.key)), s.key);
        i32 n = link_host_names();
        for (i32 i = 0; i < n; i++)
            if (!_stricmp(g_link_names[i].name, out)) snprintf(out, cap, "%s", g_link_names[i].label);
    }
}

static void ed_share(ShareKind kind, const char *host = 0); // editor.cpp
static void edit_read(const Path &file, EditRecord *e); // editor.cpp
static void editor_refresh_shares(); // editor.cpp

// A card's edit, when it has one: `rec` filled and true; false shares the whole file.
static bool lib_item_edit(const LibItem &it, EditRecord *rec)
{
    Path ef = lib_edit_path(it);
    if (GetFileAttributesW(ef.s) == INVALID_FILE_ATTRIBUTES) return false;
    edit_read(ef, rec);
    return true;
}

// What Share would do with a card, played as its edit
static ShareDecision lib_share_decide(const LibItem &it)
{
    static EditRecord rec;
    bool edited = lib_item_edit(it, &rec);
    return share_decide(it.info, edited ? &rec : 0);
}

// What a card has ready to share (share_clip_state), for its marks. Asked again at most every 2 s per card, which
// is when its edit is read.
static ClipShareState lib_clip_state(const LibItem &it)
{
    struct Seen { u64 key; f64 at; ClipShareState st; };
    static Seen seen[256];
    static EditRecord rec;
    Seen &s = seen[it.key % COUNT(seen)];
    f64 now = ImGui::GetTime();
    if (s.key != it.key || now - s.at >= 2) {
        bool edited = lib_item_edit(it, &rec);
        s.key = it.key;
        s.at = now;
        s.st = share_clip_state(it.key, lib_item_path(it).s, it.info, edited ? &rec : 0);
    }
    return s.st;
}

// How a Share button looks for what one click gives, at the same size and place whatever it is: filled in the accent
// with the link glyph for a link that plays (Share copies it), tinted for a copy ready on disk, else `plain`.
static KitKind share_ready_kind(const ClipShareState &cs, KitKind plain)
{
    return cs.link_ready ? KIT_ACCENT : cs.file ? KIT_TINT : plain;
}
static const char *share_ready_icon(const ClipShareState &cs) { return cs.link_ready ? ICON_LINK : ICON_SHARE; }

// The Share button's tip: what one click does.
static const char *share_ready_tip(const ClipShareState &cs)
{
    return cs.link_ready ? "Copy the link. Right-click for more options" : "Share. Right-click for more options";
}

// What deleting cards takes with it, as the Delete dialog's line: their share copies and links, and the links that
// stay up. "" when they have none.
static void lib_delete_note(const u64 *keys, i32 n, char *out, i32 cap)
{
    i32 gone = 0, stay = 0;
    bool link = false;
    char cant[160] = "", one[160];
    for (i32 i = 0; i < n; i++) {
        R2Share ls;
        link = link || share_clip_link(keys[i], &ls);
        gone += share_clip_delete_preview(keys[i], one, sizeof one);
        if (one[0]) {
            stay++;
            str_set(cant, one);
        }
    }
    out[0] = 0;
    if (gone) snprintf(out, cap, "%s %d share %s deleted too.", gone == 1 ? "Its" : "Their", gone, gone == 1 ? "copy or link is" : "copies and links are");
    if (stay) {
        size_t l = strlen(out);
        snprintf(out + l, cap - l, "%s%s.", l ? " " : "", stay == 1 ? cant : "Some uploader links can't be removed and stay up");
    }
    if (link) { // a posted link's preview is the chat app's own copy
        size_t l = strlen(out);
        snprintf(out + l, cap - l, "%sDiscord may keep showing a posted link for a while.", l ? " " : "");
    }
}

// The last Share started from the library, followed by its tile in the Share menu
static u32 g_lib_share_job;
static u64 g_lib_share_job_key;

// Starts Share for a card as `kind`, with its edit; `host` (a ShareHostChoice's name) picks a link's host, 0 lets
// Share's rule pick. The job joins the list the note follows. Returns it, 0 when it couldn't start.
static u32 lib_share_now(const LibItem &it, ShareKind kind, const char *host = 0)
{
    const char *name = it.display_name[0] ? it.display_name : it.name;
    if (g_lib_upload_count >= LIB_UPLOADS_MAX) {
        lib_note(true, "Wait for a share to finish");
        return 0;
    }
    static EditRecord rec;
    bool edited = lib_item_edit(it, &rec);
    Path path = lib_item_path(it);
    u32 id = share_start(kind, path.s, it.info, edited ? &rec : 0, name, it.info.game, it.key, host);
    if (!id) {
        lib_note(true, "Share is busy: try again in a moment");
        return 0;
    }
    lib_upload_add(id, name, true);
    g_lib_share_job = id;
    g_lib_share_job_key = it.key;
    return id;
}

// A session goes to editors as it is, never squeezed: none of the hosts on taking its size refuses its Share.
static bool lib_share_too_big(const LibItem &it)
{
    return it.kind == LIB_SESSIONS && !share_host_fits((i64)it.size);
}
// Says so when it is, for any Share of `it`: greyed buttons still take the click so the reason shows without tips.
static bool lib_share_refused(const LibItem &it)
{
    if (!lib_share_too_big(it)) return false;
    lib_note(true, "Too big for your connected hosts: %s", it.display_name[0] ? it.display_name : it.name);
    return true;
}

// Share on a card or in the player as `kind`, a link on `host` when one is picked. A link with no host opens Options
// where Cloudflare signs in.
static void lib_share_item(const LibItem &it, ShareKind kind, const char *host = 0)
{
    if (lib_share_refused(it)) return;
    if (kind == SHARE_LINK && !links_any_host()) {
        lib_open_sharing();
        lib_note(true, "Sign in with Cloudflare to share links");
        return;
    }
    lib_share_now(it, kind, host);
}

// One-click Share: what share_decide picks
static void lib_share_decided(const LibItem &it)
{
    if (lib_share_refused(it)) return;
    ShareDecision d = lib_share_decide(it);
    if (d.kind == SHARE_LINK && !d.link_ok && links_any_host()) lib_note(true, "Couldn't share %s: %s", it.name, d.link_why);
    else lib_share_item(it, d.kind);
}

// Follows the jobs: a finished one says what went on the clipboard, a failed one why. Each frame of the library, and
// on EV_R2 and EV_SAVE wherever the window is.
static void lib_uploads_poll()
{
    for (i32 i = 0; i < g_lib_upload_count;) {
        LibUpload &u = g_lib_uploads[i];
        if (u.share) {
            ShareJobView j = share_job(u.id);
            if (!j.id) lib_note(true, "Couldn't share %s", u.name);
            else if (j.state == SHARE_FAILED) lib_note(true, "Couldn't share %s: %s", u.name, j.error);
            else if (j.state == SHARE_DONE && j.error[0]) lib_note(true, "%s: %s", u.name, j.error);
            else if (j.state == SHARE_DONE && j.kind == SHARE_LINK) lib_note(false, "Link copied: %s", u.name);
            else if (j.state == SHARE_DONE) {
                lib_note(false, "Copied %s, ready to paste", u.name);
                editor_refresh_shares();
            } else {
                i++;
                continue;
            }
        } else {
            R2Job j = r2_job(u.id);
            if (j.state == R2_DONE) {
                ImGui::SetClipboardText(j.url);
                sound_event(SOUND_SHARE_READY);
                lib_note(false, "Link copied: %s", u.name);
            } else if (j.state == R2_FAILED) {
                sound_event(SOUND_FAILED);
                lib_note(true, "Couldn't share %s: %s", u.name, j.error);
            }
            else {
                i++;
                continue;
            }
        }
        memmove(&g_lib_uploads[i], &g_lib_uploads[i + 1], (g_lib_upload_count - i - 1) * sizeof *g_lib_uploads);
        g_lib_upload_count--;
    }
    if (g_lib_upload_count) ui_wake();
}

// The note at the bottom of the window, a fixed size: the oldest upload under way with how far it is, else the
// last word for a few seconds.
static bool         g_lib_share_ask;          // the player's Share menu opens next frame
static char         g_lib_share_pending[512]; // a file Share was asked for (the toast's): played once the library has it
static u64          g_lib_share_since;

static bool ffget_status(char *out, i32 cap, f32 *progress); // ffget.cpp
static void ffget_start(bool force);
static bool ffget_failed();

// While Rekodr fetches ffmpeg, or after that failed: a panel at the bottom, above the library's notes, with the
// line from ffget_status, a progress line while it downloads and Retry after a failure. The library and the
// first-run setup draw it.
static void ffget_bar()
{
    char text[300];
    f32 progress = 0;
    if (!ffget_status(text, sizeof text, &progress)) return;
    bool failed = ffget_failed();
    if (!failed) ui_wake();
    ImGuiViewport *vp = ImGui::GetMainViewport();
    f32 w = kit_px(520), h = kit_px(44), r = kit_px(10), pad = kit_px(14);
    ImVec2 a(floorf(vp->Pos.x + (vp->Size.x - w) / 2), floorf(vp->Pos.y + vp->Size.y - 2 * h - kit_px(64)));
    ImGui::SetNextWindowPos(a);
    ImGui::SetNextWindowSize(ImVec2(w, h));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, 0);
    ImGui::Begin("##ffget", 0, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 b(a.x + w, a.y + h);
    kit_frost(dl, a, b, r);
    dl->AddRect(a, b, col32(COL_LINE), r, 0, kit_px(1));
    const char *icon = failed ? ICON_WARNING : ICON_VIDEO;
    f32 fs = g_font_icon->FontSize;
    ImVec2 gs = g_font_icon->CalcTextSizeA(fs, FLT_MAX, 0, icon);
    dl->AddText(g_font_icon, fs, ImVec2(a.x + pad, floorf(a.y + (h - gs.y) / 2)), col32(failed ? 0xff8a92 : COL_ACCENT), icon);
    f32 tx = a.x + pad + gs.x + kit_px(10), right = b.x - pad;
    if (failed) {
        f32 bw = ImGui::CalcTextSize("Retry").x + kit_px(28), fh = ImGui::GetFrameHeight();
        right -= bw;
        ImGui::SetCursorScreenPos(ImVec2(right, floorf(a.y + (h - fh) / 2)));
        if (kit_button("Retry##ffget", KIT_SECONDARY, ImVec2(bw, 0))) {
            dev_click("ffmpeg", "Retry");
            ffget_start(false);
        }
        right -= kit_px(10);
    }
    for (size_t n = strlen(text); n > 3 && ImGui::CalcTextSize(text).x > right - tx; n--) memcpy(text + n - 4, "...", 4);
    dl->AddText(ImVec2(tx, floorf(a.y + (h - ImGui::GetTextLineHeight()) / 2)), col32(COL_TEXT), text);
    if (!failed) {
        f32 y = b.y - kit_px(5), x0 = a.x + pad, x1 = b.x - pad;
        dl->AddRectFilled(ImVec2(x0, y), ImVec2(x1, y + kit_px(2)), col32(COL_HOVER), kit_px(1));
        dl->AddRectFilled(ImVec2(x0, y), ImVec2(x0 + (x1 - x0) * fminf(progress, 1), y + kit_px(2)), col32(COL_ACCENT), kit_px(1));
    }
    ImGui::End();
}

static void share_cancel_ring(ImDrawList *dl, ImVec2 c, f32 r, f32 progress, u32 ink);

// Cancels the upload the note shows: the oldest under way. The ones after it stay queued.
static void lib_upload_cancel(const LibUpload &u)
{
    log_line("library: %s %u cancelled from the note", u.share ? "share" : "upload", u.id);
    if (u.share) share_cancel(u.id);
    else r2_cancel(u.id);
    lib_uploads_poll();
}

static void lib_note_draw()
{
    bool busy = g_lib_upload_count > 0;
    if (!busy && (!g_lib_note[0] || now_us() - g_lib_note_us > 4000000)) return;
    ui_wake(); // until it goes
    char text[256], pct[16] = "";
    f32 progress = -1;
    const char *icon = ICON_LINK;
    u32 ink = COL_ACCENT;
    if (busy) {
        LibJobNow n = lib_job_now(g_lib_uploads[0]);
        progress = n.progress;
        if (!n.link) icon = ICON_SHARE;
        if (g_lib_upload_count > 1)
            snprintf(text, sizeof text, "%s %s, %d more after it", n.verb, g_lib_uploads[0].name, g_lib_upload_count - 1);
        else snprintf(text, sizeof text, "%s %s", n.verb, g_lib_uploads[0].name);
        if (!n.waiting) snprintf(pct, sizeof pct, "%d%%", (i32)(progress * 100 + 0.5f));
    } else {
        snprintf(text, sizeof text, "%s", g_lib_note);
        if (g_lib_note_error) {
            icon = ICON_WARNING;
            ink = 0xff8a92;
        }
    }
    ImGuiViewport *vp = ImGui::GetMainViewport();
    ImDrawList *dl = ImGui::GetForegroundDrawList(vp);
    f32 w = kit_px(420), h = kit_px(44), r = kit_px(10);
    ImVec2 a(floorf(vp->Pos.x + (vp->Size.x - w) / 2), floorf(vp->Pos.y + vp->Size.y - h - kit_px(56)));
    ImVec2 b(a.x + w, a.y + h);
    kit_frost(dl, a, b, r);
    dl->AddRect(a, b, col32(COL_LINE), r, 0, kit_px(1));
    f32 fs = g_font_icon->FontSize;
    ImVec2 gs = g_font_icon->CalcTextSizeA(fs, FLT_MAX, 0, icon);
    dl->AddText(g_font_icon, fs, ImVec2(a.x + kit_px(14), floorf(a.y + (h - gs.y) / 2)), col32(ink), icon);
    f32 tx = a.x + kit_px(14) + gs.x + kit_px(10), right = b.x - kit_px(14);
    if (busy) { // Cancel, the same X in a ring as the Share button's, at the right end
        f32 rr = kit_px(11);
        ImVec2 c(floorf(right - rr), floorf(a.y + h / 2));
        ImGui::SetNextWindowPos(ImVec2(c.x - rr - kit_px(4), c.y - rr - kit_px(4)));
        ImGui::SetNextWindowSize(ImVec2(2 * rr + kit_px(8), 2 * rr + kit_px(8)));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
        ImGui::Begin("##lib_note_cancel", 0, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                                ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground |
                                                ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
        ImGui::PopStyleVar(2);
        bool clicked = ImGui::InvisibleButton("##cancel", ImGui::GetContentRegionAvail());
        bool hot = ImGui::IsItemHovered();
        kit_tip("Cancel the share");
        ImGui::End();
        if (hot) dl->AddCircleFilled(c, rr + kit_px(3), col32(COL_HOVER), 32);
        share_cancel_ring(dl, c, rr, progress < 0 ? 0 : progress, COL_TEXT);
        right -= 2 * rr + kit_px(12);
        if (clicked) {
            dev_click("note", "Cancel");
            lib_upload_cancel(g_lib_uploads[0]);
        }
    }
    if (pct[0]) {
        f32 pw = ImGui::CalcTextSize(pct).x;
        dl->AddText(ImVec2(right - pw, floorf(a.y + (h - ImGui::GetTextLineHeight()) / 2)), col32(COL_DIM), pct);
        right -= kit_px(44);
    }
    // text_clipped draws on the window's list, under popups; the note goes over everything
    char shown[256];
    snprintf(shown, sizeof shown, "%s", text);
    f32 room = right - tx;
    for (size_t n = strlen(shown); n > 3 && ImGui::CalcTextSize(shown).x > room; n--) memcpy(shown + n - 4, "...", 4);
    dl->AddText(ImVec2(tx, floorf(a.y + (h - ImGui::GetTextLineHeight()) / 2)), col32(COL_TEXT), shown);
    if (progress >= 0) {
        f32 y = b.y - kit_px(5), x0 = a.x + kit_px(14), x1 = b.x - kit_px(14);
        dl->AddRectFilled(ImVec2(x0, y), ImVec2(x1, y + kit_px(2)), col32(COL_HOVER), kit_px(1));
        dl->AddRectFilled(ImVec2(x0, y), ImVec2(x0 + (x1 - x0) * progress, y + kit_px(2)), col32(COL_ACCENT), kit_px(1));
    }
}

static void library_r2_event() { lib_uploads_poll(); }

// A save finished: a Share making a file may be done.
static void library_on_save_event(const Event &) { lib_uploads_poll(); }

// Share for a file by its path (the Clip saved toast's): the library plays it with the Share menu open, once a scan
// has it.
static void library_share_file(const char *path)
{
    str_set(g_lib_share_pending, path);
    g_lib_share_since = GetTickCount64();
    library_rescan();
    ui_show(SCREEN_LIBRARY);
    log_line("library: Share asked for %s", path);
}

static void lib_share_pending_check()
{
    if (!g_lib_share_pending[0]) return;
    Library &lib = g_app.library;
    wchar_t want[MAX_PATH];
    utf8_to_wide(g_lib_share_pending, want, MAX_PATH);
    for (i32 i = 0; i < lib.item_count; i++) {
        if (_wcsicmp(lib_item_path(lib.items[i]).s, want)) continue;
        if (!lib.items[i].probed) return;
        g_lib_share_pending[0] = 0;
        library_play(lib.items[i]);
        g_lib_share_ask = true;
        return;
    }
    if (GetTickCount64() - g_lib_share_since > 15000 && !lib.scanning) {
        log_line("library: %s never turned up for Share", g_lib_share_pending);
        g_lib_share_pending[0] = 0;
    }
}

// The Share menu's state of a Share: running with its step and percent, or just finished (for 4 s) with how it went.
struct ShareMenuState {
    bool running;
    f32  progress;  // 0..1 while running, < 0 for no bar
    char line[96];  // while running: "Uploading 40%"; after: "Copied", "Link copied", "Not shared"
    i32  done;      // 1 shared, -1 failed, 0 neither
    char error[256];
};

// The menu's state of a share job (ShareJobView), done for 4 s after it ends.
static ShareMenuState share_menu_state(const ShareJobView &j)
{
    static u32 done_id;
    static u64 done_ms;
    ShareMenuState st = {};
    st.progress = -1;
    if (!j.id) return st;
    u64 now = GetTickCount64();
    if (j.state < SHARE_DONE) {
        st.running = true;
        st.progress = j.progress;
        snprintf(st.line, sizeof st.line, "%s %d%%", j.verb[0] ? j.verb : "Sharing", (i32)(j.progress * 100 + 0.5f));
        ui_wake();
        return st;
    }
    if (done_id != j.id) done_id = j.id, done_ms = now;
    if (now - done_ms >= 4000) return st;
    bool ok = j.state == SHARE_DONE && !j.error[0];
    st.done = ok ? 1 : -1;
    str_set(st.line, ok ? (j.kind == SHARE_LINK ? "Link copied" : "Copied") : "Not shared");
    str_set(st.error, j.error);
    ui_wake();
    return st;
}

// A bar `w` wide of `frac` in `ink` over a dim track.
static void share_bar(ImDrawList *dl, ImVec2 a, f32 w, f32 frac, u32 ink)
{
    f32 h = kit_px(5), r = h / 2;
    dl->AddRectFilled(a, ImVec2(a.x + w, a.y + h), col32(COL_FAINT, 0.45f), r);
    if (frac <= 0) return;
    f32 fw = w * (frac > 1 ? 1 : frac);
    dl->AddRectFilled(a, ImVec2(a.x + (fw < h ? h : fw), a.y + h), col32(ink), r);
}

// Cancel for a running Share: an X in a ring of radius r round `c`, the ring filled to `progress` in the accent
// clockwise from the top over a dim track.
static void share_cancel_ring(ImDrawList *dl, ImVec2 c, f32 r, f32 progress, u32 ink)
{
    f32 th = kit_px(2);
    dl->AddCircle(c, r, col32(COL_FAINT, 0.6f), 32, th);
    if (progress > 0) {
        f32 a0 = -3.14159265f / 2;
        dl->PathArcTo(c, r, a0, a0 + 2 * 3.14159265f * fminf(progress, 1), 32);
        dl->PathStroke(col32(COL_ACCENT), 0, th);
    }
    f32 x = floorf(r * 0.42f);
    dl->AddLine(ImVec2(c.x - x, c.y - x), ImVec2(c.x + x, c.y + x), col32(ink), kit_px(1.5f));
    dl->AddLine(ImVec2(c.x - x, c.y + x), ImVec2(c.x + x, c.y - x), col32(ink), kit_px(1.5f));
}

// The most a host takes, 0 for no cap.
static ID3D11ShaderResourceView *resource_texture(i32 id, i32 *w, i32 *h); // setup.cpp

// A host's own icon, as its site shows it (built into the exe): Cloudflare R2's; 0 for the player's servers.
// Loaded once.
static ImTextureID host_logo(LinkHost host)
{
    static ID3D11ShaderResourceView *tex;
    static bool tried;
    if (host != HOST_R2) return 0;
    if (!tried) {
        tried = true;
        i32 w = 0, h = 0;
        tex = resource_texture(21, &w, &h);
    }
    return (ImTextureID)(intptr_t)tex;
}

// The host's icon in the sz square at `a`, as it is (only faded by `alpha`); false when it has none.
static bool host_logo_draw(ImDrawList *dl, LinkHost host, ImVec2 a, f32 sz, f32 alpha = 1)
{
    ImTextureID t = host_logo(host);
    if (!t) return false;
    dl->AddImage(t, a, ImVec2(a.x + sz, a.y + sz), ImVec2(0, 0), ImVec2(1, 1), col32(0xffffff, alpha));
    return true;
}

static i64 share_host_cap(LinkHost host)
{
    if (host == HOST_R2) return r2_cap_now();
    return 0;
}

// A square icon button in the menu, `ink` its icon's colour; true when clicked.
static bool share_menu_icon(const char *icon, const char *id, u32 ink, f32 sz, bool enabled = true)
{
    ImVec2 a = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::InvisibleButton(id, ImVec2(sz, sz)) && enabled;
    bool hot = enabled && ImGui::IsItemHovered();
    if (!enabled) ink = COL_FAINT;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    if (hot) dl->AddRectFilled(a, ImVec2(a.x + sz, a.y + sz), col32(COL_HOVER), kit_px(6));
    f32 fs = g_font_icon->FontSize;
    ImVec2 gs = g_font_icon->CalcTextSizeA(fs, FLT_MAX, 0, icon);
    dl->AddText(g_font_icon, fs, ImVec2(floorf(a.x + (sz - gs.x) / 2), floorf(a.y + (sz - gs.y) / 2)), col32(ink), icon);
    return clicked;
}

static void open_url(const char *url); // setup.cpp
static i32 render_copies(i64 *bytes);  // render.cpp
static f32 render_progress(u64 key);   // render.cpp
static bool render_waiting(u64 key);   // render.cpp
static bool g_card_share_open; // a card's Share was right-clicked, or Share... in its menu: its Share menu opens

enum ShareMenuPick { SHARE_MENU_NONE, SHARE_MENU_SHARE, SHARE_MENU_HOST, SHARE_MENU_EDIT, SHARE_MENU_CANCEL };

// A host's label for the menu: its name with a capital ("your bucket" -> "Your bucket").
static void share_label_name(const char *label, char *name, i32 name_cap)
{
    snprintf(name, name_cap, "%s", label);
    if (name[0] >= 'a' && name[0] <= 'z') name[0] -= 32;
}

// The Share menu, the same wherever it opens (a card's Share, the player's, the editor's and the Timeline's arrow),
// drawn inside the caller's popup:
// - Share: what one click does, "Recommended: Your bucket" (or File) over its size; while a Share runs the line is its
//   step over a progress bar and the button its Cancel, then "Copied".
// - The clip's link (link_key's, when it has one): the address, then Copy, Open, Pause (the bucket's) and Delete.
// - Link to: a row per host with the clip's size against the host's cap ("35 MB of 10 GB" over a bar); greyed when
//   it can't.
// - Original (shows `original` in its folder; greyed with none) and Edit (greyed unless `can_edit`).
// Its size follows only the clip: whether it has a link and how many hosts. Returns SHARE_MENU_SHARE for the
// one-click Share, SHARE_MENU_HOST for a host (its name in `pick`), SHARE_MENU_EDIT for Edit, SHARE_MENU_CANCEL
// for Cancel.
static i32 share_menu_body(const ShareDecision &d, const MediaInfo &m, const EditRecord *edit, const ShareMenuState &st,
                           u64 link_key, const wchar_t *original, char (&pick)[64], bool can_edit = false)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    f32 W = kit_px(300), gap = kit_px(4), sfs = g_font_small->FontSize, line_h = ImGui::GetTextLineHeight();
    ImVec2 o = ImGui::GetCursorScreenPos();
    f32 y = o.y;
    i32 result = SHARE_MENU_NONE;
    bool any_host = links_any_host();

    // Share
    {
        f32 h = kit_px(48), pad = kit_px(10), pill_w = kit_px(80), pill_h = kit_px(28);
        bool link = d.kind == SHARE_LINK, can = (link ? d.link_ok : d.file_ok) || !any_host;
        ImVec2 a(o.x, y), b(o.x + W, y + h);
        ImGui::SetCursorScreenPos(a);
        bool clicked = ImGui::InvisibleButton("##smshare", ImVec2(W, h));
        bool hot = (can || st.running) && ImGui::IsItemHovered();
        if (hot) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        kit_tip(st.done < 0 && st.error[0] ? st.error : st.running ? "Cancel the share" : link ? "Upload it and copy the link"
                                                                                           : "Copy the file to paste anywhere");
        if (clicked && st.running) result = SHARE_MENU_CANCEL;
        else if (clicked && can) result = SHARE_MENU_SHARE;
        f32 r = kit_px(7);
        dl->AddRectFilled(a, b, col32(COL_WHITE, hot ? 0.14f : 0.08f), r);
        // The pill: plainly the button
        ImVec2 p0(b.x - pad - pill_w, floorf(a.y + (h - pill_h) / 2)), p1(b.x - pad, p0.y + pill_h);
        if (st.running) { // Cancel: the X in its ring
            dl->AddRectFilled(p0, p1, col32(hot ? COL_HOVER : COL_PANEL2), pill_h / 2);
            ImGui::PushFont(g_font_bold);
            ImVec2 ts = ImGui::CalcTextSize("Cancel");
            f32 rr = kit_px(8), tx = floorf(p0.x + (pill_w - 2 * rr - kit_px(6) - ts.x) / 2);
            share_cancel_ring(dl, ImVec2(tx + rr, p0.y + pill_h / 2), rr, st.progress, COL_TEXT);
            dl->AddText(ImVec2(tx + 2 * rr + kit_px(6), floorf(p0.y + (pill_h - ts.y) / 2)), col32(COL_TEXT), "Cancel");
            ImGui::PopFont();
        } else {
            dl->AddRectFilled(p0, p1, col32(can ? (hot ? 0xe6e6e6 : COL_WHITE) : COL_PANEL2), pill_h / 2);
            f32 fs = g_font_icon->FontSize;
            ImVec2 gs = g_font_icon->CalcTextSizeA(fs, FLT_MAX, 0, ICON_SHARE);
            ImGui::PushFont(g_font_bold);
            ImVec2 ts = ImGui::CalcTextSize("Share");
            f32 tx = floorf(p0.x + (pill_w - gs.x - kit_px(6) - ts.x) / 2);
            u32 ink = can ? COL_BG : COL_FAINT;
            dl->AddText(g_font_icon, fs, ImVec2(tx, floorf(p0.y + (pill_h - gs.y) / 2)), col32(ink), ICON_SHARE);
            dl->AddText(ImVec2(tx + gs.x + kit_px(6), floorf(p0.y + (pill_h - ts.y) / 2)), col32(ink), "Share");
            ImGui::PopFont();
        }
        // The line and what's under it: the size, or the Share's progress
        char label[128], sub[128] = "", size_s[24];
        f32 progress = -1;
        u32 ink = COL_TEXT;
        if (st.running || st.done) {
            str_set(label, st.line);
            if (st.running) progress = st.progress;
            if (st.done < 0) ink = COL_WARN, str_set(sub, st.error);
        } else if (!any_host && link) {
            str_set(label, "Sign in with Cloudflare");
        } else if (link && !d.link_ok) {
            str_set(label, "No host takes it");
            str_set(sub, d.link_why);
            ink = COL_DIM;
        } else if (link) {
            char name[48];
            share_label_name(d.host_label, name, sizeof name);
            snprintf(label, sizeof label, "Recommended: %s", name);
            str_set(sub, d.why);
        } else {
            str_set(label, "Recommended: File");
            share_size_text(d.file_squeezed ? d.max_bytes : d.file_bytes, size_s, sizeof size_s);
            if (d.file_squeezed) snprintf(sub, sizeof sub, "Lowered to %s", size_s);
            else str_set(sub, size_s);
            if (!d.file_ok) ink = COL_DIM;
        }
        f32 tx = a.x + pad, tw = p0.x - kit_px(10) - tx, bh = g_font_bold->FontSize;
        f32 ty = floorf(a.y + (h - bh - kit_px(3) - sfs) / 2);
        ImGui::PushFont(g_font_bold);
        text_clipped(dl, ImVec2(tx, ty), tw, col32(ink), label);
        ImGui::PopFont();
        f32 sy = ty + bh + kit_px(3);
        if (progress >= 0) share_bar(dl, ImVec2(tx, floorf(sy + (sfs - kit_px(5)) / 2)), tw, progress, COL_ACCENT);
        else if (sub[0]) {
            ImGui::PushFont(g_font_small);
            text_clipped(dl, ImVec2(tx, sy), tw, col32(COL_DIM), sub);
            ImGui::PopFont();
        }
        y = b.y + gap;
    }

    // The clip's link
    R2Share s;
    if (link_key && share_clip_link(link_key, &s)) {
        f32 h = kit_px(34), pad = kit_px(10), isz = kit_px(26), igap = kit_px(2);
        ImVec2 a(o.x, y), b(o.x + W, y + h);
        dl->AddRectFilled(a, b, col32(COL_PANEL2), kit_px(7));
        dl->AddRect(a, b, col32(COL_LINE), kit_px(7), 0, kit_px(1));
        bool r2 = s.host == HOST_R2, paused = r2 && s.paused;
        i32 icons = r2 ? 4 : 3;
        f32 dr = kit_px(4);
        dl->AddCircleFilled(ImVec2(a.x + pad + dr, a.y + h / 2), dr, col32(paused ? COL_WARN : COL_OK));
        const char *url = s.url;
        if (!strncmp(url, "https://", 8)) url += 8;
        else if (!strncmp(url, "http://", 7)) url += 7;
        f32 bx = b.x - kit_px(4) - icons * isz - (icons - 1) * igap, tx = a.x + pad + 2 * dr + kit_px(8);
        f32 ux = bx; // where the address ends: before the views, when they're known
        if (r2 && s.views_month >= 0) {
            char n[24];
            snprintf(n, sizeof n, "%lld", s.views_month);
            ImGui::PushFont(g_font_small);
            f32 nw = ImGui::CalcTextSize(n).x, es = floorf(sfs * 1.1f);
            ImVec2 ns = g_font_icon->CalcTextSizeA(es, FLT_MAX, 0, ICON_VIEW);
            ux = bx - kit_px(6) - nw - kit_px(4) - ns.x;
            dl->AddText(g_font_icon, es, ImVec2(ux, floorf(a.y + (h - ns.y) / 2)), col32(COL_DIM), ICON_VIEW);
            dl->AddText(ImVec2(ux + ns.x + kit_px(4), floorf(a.y + (h - sfs) / 2)), col32(COL_DIM), n);
            ImGui::PopFont();
            kit_tip_rect(ImVec2(ux, a.y), ImVec2(bx - kit_px(6), b.y), "Times the link was opened this month");
        }
        text_clipped(dl, ImVec2(tx, floorf(a.y + (h - line_h) / 2)), ux - kit_px(8) - tx, col32(COL_TEXT), url);
        f32 by = floorf(a.y + (h - isz) / 2);
        ImGui::SetCursorScreenPos(ImVec2(bx, by));
        if (share_menu_icon(ICON_COPY, "##smcopy", COL_TEXT, isz)) {
            ImGui::SetClipboardText(s.url);
            lib_note(false, "Link copied: %s", s.title);
        }
        kit_tip(paused ? "Copy link: it plays once resumed" : "Copy link");
        ImGui::SetCursorScreenPos(ImVec2(bx + isz + igap, by));
        if (share_menu_icon(ICON_OPEN, "##smopen", COL_TEXT, isz)) open_url(s.url);
        kit_tip("Open in browser");
        if (r2) {
            ImGui::SetCursorScreenPos(ImVec2(bx + 2 * (isz + igap), by));
            if (share_menu_icon(paused ? ICON_PLAY : ICON_PAUSE, "##smpause", COL_TEXT, isz, !s.moving)) {
                if (s.paused) r2_resume(s.key);
                else r2_pause(s.key);
                log_line("library: link %s %s", s.key, s.paused ? "resumed" : "paused");
                lib_note(false, s.paused ? "Link playing again: %s" : "Link paused: %s", s.title);
            }
            kit_tip(s.moving ? (s.paused ? "Resuming..." : "Pausing...")
                    : s.paused == 2 ? "Paused until tomorrow: click to let it play again"
                    : s.paused ? "Paused: click to let it play again" : "Pause: nobody can play it until resumed");
        }
        char host[64];
        share_host_label(s, host, sizeof host);
        ImGui::SetCursorScreenPos(ImVec2(bx + (icons - 1) * (isz + igap), by));
        if (share_menu_icon(ICON_DELETE, "##smdelete", 0xff8a92, isz)) ImGui::OpenPopup("link_delete");
        kit_tip(host[0] ? "Remove from the list" : "Delete the link");
        lib_popup_style_push(6);
        if (ImGui::BeginPopup("link_delete")) {
            ImGui::PushFont(g_font_small);
            if (host[0]) kit_dim("  The file stays on %s", host);
            else kit_dim("  Discord may keep showing it for a while");
            ImGui::PopFont();
            if (lib_menu_item(ICON_DELETE, host[0] ? "Remove from the list" : "Delete it from the bucket", true, kit_px(240))) {
                log_line("library: link %s %s", s.key, host[0] ? "taken off the list" : "deleted");
                r2_delete(s.key);
            }
            lib_menu_item(ICON_CLOSE, "Keep it", false, kit_px(240));
            ImGui::EndPopup();
        }
        lib_popup_style_pop();
        y = b.y + gap;
    }

    // Link to, with how long a bucket link lasts on the right (the next share only; the setting is the default)
    {
        static ShareHostChoice c[24];
        i32 n = share_host_choices(m, edit, c, COUNT(c));
        bool bucket = false;
        for (i32 i = 0; i < n; i++) bucket |= c[i].host == HOST_R2;
        if (ImGui::IsWindowAppearing()) g_share_link_days = -1;
        ImGui::PushFont(g_font_small);
        f32 ph = bucket ? sfs + kit_px(8) : sfs + kit_px(4);
        dl->AddText(ImVec2(o.x + kit_px(4), floorf(y + (ph - sfs) / 2) + (bucket ? 0 : kit_px(2))), col32(COL_DIM), "LINK TO");
        if (bucket) {
            static const char *names[] = {"Forever", "1 week", "1 day"};
            static const i32 days[] = {0, 7, 1};
            i32 cur = g_share_link_days >= 0 ? g_share_link_days : g_app.config.share_link_days;
            f32 x = o.x + W, padx = kit_px(8);
            for (i32 i = 0; i < 3; i++) {
                f32 w = ImGui::CalcTextSize(names[i]).x + 2 * padx;
                x -= w;
                ImVec2 a(x, y), b(x + w, y + ph);
                ImGui::SetCursorScreenPos(a);
                ImGui::PushID(names[i]);
                bool clicked = ImGui::InvisibleButton("##life", ImVec2(w, ph));
                ImGui::PopID();
                bool hot = ImGui::IsItemHovered(), on = cur == days[i];
                if (hot) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                if (clicked) {
                    g_share_link_days = days[i];
                    dev_click("chip", names[i]);
                }
                dl->AddRectFilled(a, b, col32(COL_WHITE, on ? 1 : hot ? 0.14f : 0.06f), ph / 2);
                dl->AddText(ImVec2(a.x + padx, floorf(a.y + (ph - sfs) / 2)), col32(on ? COL_BG : COL_DIM), names[i]);
                x -= kit_px(4);
            }
            kit_tip_rect(ImVec2(x, y), ImVec2(o.x + W, y + ph), "How long a link to your bucket lasts");
        }
        ImGui::PopFont();
        y += ph + kit_px(4);
        f32 h = kit_px(32), pad = kit_px(8), tile = kit_px(16), bar_w = kit_px(96), bar_h = kit_px(3);
        if (!n) {
            ImGui::SetCursorScreenPos(ImVec2(o.x, y));
            if (lib_menu_item(ICON_SETTINGS, "Sign in with Cloudflare...", false, W)) lib_open_sharing();
            y = ImGui::GetCursorScreenPos().y;
        }
        for (i32 i = 0; i < n; i++) {
            const ShareHostChoice &x = c[i];
            bool ok = x.fits;
            ImGui::PushID(i);
            ImVec2 a(o.x, y), b(o.x + W, y + h);
            ImGui::SetCursorScreenPos(a);
            bool clicked = ImGui::InvisibleButton("##host", ImVec2(W, h));
            bool hot = ok && ImGui::IsItemHovered();
            if (hot) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            char name[48], tip[128] = "";
            share_label_name(x.label, name, sizeof name);
            if (!ok) str_set(tip, x.why);
            else if (x.host == HOST_R2) str_set(tip, "Your bucket");
            if (tip[0]) kit_tip(tip);
            if (clicked && ok && !st.running) {
                str_set(pick, x.name);
                dev_click("menu", x.name);
                result = SHARE_MENU_HOST;
            }
            if (hot) dl->AddRectFilled(a, b, col32(COL_HOVER), kit_px(6));
            f32 alpha = ok ? 1.f : 0.4f;
            // The host's own icon; the player's servers a server glyph
            ImVec2 t0(a.x + pad, floorf(a.y + (h - tile) / 2)), t1(t0.x + tile, t0.y + tile);
            if (!host_logo_draw(dl, x.host, t0, tile, alpha)) {
                dl->AddRectFilled(t0, t1, col32(0x6e6e6e, alpha), kit_px(4));
                f32 fs = floorf(tile * 0.7f);
                ImVec2 gs = g_font_icon->CalcTextSizeA(fs, FLT_MAX, 0, ICON_SERVER);
                dl->AddText(g_font_icon, fs, ImVec2(floorf(t0.x + (tile - gs.x) / 2), floorf(t0.y + (tile - gs.y) / 2)),
                            col32(0xffffff, alpha), ICON_SERVER);
            }
            // The clip's size against the host's cap, over a bar of it
            f32 bx = b.x - pad - bar_w;
            i64 cap = share_host_cap(x.host), bytes = ok && x.bytes > 0 ? x.bytes : d.reco_bytes;
            char size_s[24], cap_s[24], fit[64];
            share_size_text(bytes, size_s, sizeof size_s);
            if (!ok) str_set(fit, "Too big");
            else if (cap > 0) {
                share_size_text(cap, cap_s, sizeof cap_s);
                snprintf(fit, sizeof fit, "%s of %s", size_s, cap_s);
            } else str_set(fit, size_s);
            f32 frac = !ok ? 1 : cap > 0 ? (f32)((f64)bytes / cap) : 0;
            f32 fy = floorf(a.y + (h - sfs - kit_px(3) - bar_h) / 2);
            ImGui::PushFont(g_font_small);
            f32 fw = ImGui::CalcTextSize(fit).x;
            dl->AddText(ImVec2(b.x - pad - fw, fy), col32(ok ? COL_DIM : COL_FAINT), fit);
            ImGui::PopFont();
            ImVec2 q0(bx, fy + sfs + kit_px(3));
            dl->AddRectFilled(q0, ImVec2(bx + bar_w, q0.y + bar_h), col32(COL_FAINT, 0.45f), bar_h / 2);
            u32 bar_ink = ok ? COL_OK : COL_FAINT;
            if (frac > 0) dl->AddRectFilled(q0, ImVec2(bx + fmaxf(bar_h, bar_w * fminf(frac, 1)), q0.y + bar_h), col32(bar_ink), bar_h / 2);
            // The name
            f32 nx = t1.x + kit_px(10), name_end = fminf(bx, b.x - pad - fw) - kit_px(10);
            f32 ny = floorf(a.y + (h - line_h) / 2);
            text_clipped(dl, ImVec2(nx, ny), name_end - nx, col32(ok ? COL_TEXT : COL_FAINT), name);
            ImGui::PopID();
            y = b.y;
        }
        y += gap;
    }

    // Original and Edit
    {
        f32 cw = floorf((W - gap) / 2);
        ImGui::SetCursorScreenPos(ImVec2(o.x, y));
        bool orig_clicked = kit_button(ICON_FOLDER "  Original", KIT_SECONDARY, ImVec2(cw, 0), original != 0);
        kit_tip(original ? "Show the unedited file in its folder" : "Save it first to have an original");
        if (orig_clicked) {
            show_in_folder(original);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SetCursorScreenPos(ImVec2(o.x + W - cw, y));
        bool edit_clicked = kit_button(ICON_EDIT "  Edit", KIT_SECONDARY, ImVec2(cw, 0), can_edit);
        if (!can_edit) kit_tip("Edit it from the Library");
        if (edit_clicked) {
            result = SHARE_MENU_EDIT;
            ImGui::CloseCurrentPopup();
        }
        y += ImGui::GetFrameHeight();
    }
    ImGui::SetCursorScreenPos(ImVec2(o.x, y));
    ImGui::Dummy(ImVec2(W, 0));
    return result;
}

// The Share menu for one card (share_menu_body). The edit is read once, as the menu opens. Opened with
// ImGui::OpenPopup(id) where it's drawn.
static void lib_share_menu(const char *id, const LibItem &it)
{
    static u64 key;
    static ShareDecision d;
    static EditRecord rec;
    static bool edited;
    lib_popup_style_push(8);
    if (!ImGui::BeginPopup(id)) {
        lib_popup_style_pop();
        return;
    }
    if (ImGui::IsWindowAppearing() || key != it.key) {
        key = it.key;
        d = lib_share_decide(it);
        edited = lib_item_edit(it, &rec);
    }
    ShareJobView j = {};
    if (g_lib_share_job && g_lib_share_job_key == it.key) j = share_job(g_lib_share_job);
    char pick[64];
    i32 r = share_menu_body(d, it.info, edited ? &rec : 0, share_menu_state(j), it.key, lib_item_path(it).s, pick, true);
    if (r == SHARE_MENU_SHARE) lib_share_decided(it);
    else if (r == SHARE_MENU_HOST) lib_share_item(it, SHARE_LINK, pick);
    else if (r == SHARE_MENU_EDIT) editor_open(it);
    else if (r == SHARE_MENU_CANCEL) {
        log_line("library: share %u cancelled", g_lib_share_job);
        share_cancel(g_lib_share_job);
    }
    ImGui::EndPopup();
    lib_popup_style_pop();
}

static bool g_card_pick_game; // "Pick a game" on a card was clicked: the picker opens for lib.target

static void card_menu()
{
    Library &lib = g_app.library;
    bool rename = false, display_name = false, del = false, folders = false, game = false, comment = false, tags = false;
    lib_popup_style_push(6);
    if (ImGui::BeginPopup("card_menu")) {
        const LibItem &it = lib.target;
        Path path = lib_playable_path(it);
        if (lib_menu_item(ICON_PLAY, "Play")) library_play(it);
        if (lib_menu_item(ICON_EDIT, "Edit")) editor_open(it);
        if (lib_menu_item(ICON_VIDEO, "Open in the default player")) ShellExecuteW(0, L"open", path.s, 0, 0, SW_SHOWNORMAL);
        if (lib_menu_item(ICON_SHARE, "Share...") && !lib_share_refused(it)) g_card_share_open = true;
        if (lib_menu_item(ICON_LINK, "Copy link")) lib_share_item(it, SHARE_LINK);
        if (lib_menu_item(ICON_FOLDER, "Show in folder")) show_in_folder(path.s);
        ImGui::Separator();
        if (lib_menu_item(ICON_ADD, "Add to a folder")) folders = true;
        if (lib_menu_item(ICON_GAME, "Change game")) game = true;
        if (lib_menu_item(ICON_RENAME, "Rename")) rename = true;
        if (lib_menu_item(ICON_FONT, it.display_name[0] ? "Change the display name" : "Add a display name")) display_name = true;
        if (lib_menu_item(ICON_TAG, "Tags...")) tags = true;
        if (lib_menu_item(ICON_COMMENT, "Comment...")) comment = true;
        if (lib_menu_item(ICON_DELETE, "Delete...", true)) del = true;
        ImGui::EndPopup();
    }
    lib_popup_style_pop();
    if (rename) lib_rename_start(lib.target);
    if (display_name) lib_rename_start(lib.target, true);
    if (comment) lib_comment_open(lib.target);
    lib_comment_dialog();
    if (tags) lib_tags_open(lib.target);
    lib_tags_dialog();
    if (g_card_share_open) ImGui::OpenPopup("card_share");
    g_card_share_open = false;
    lib_share_menu("card_share", lib.target);
    if (del) ImGui::OpenPopup("Delete");
    if (game || g_card_pick_game) ImGui::OpenPopup("card_game");
    g_card_pick_game = false;
    if (lib_game_picker("card_game", lib.target)) lib_message(false, "%s is filed under %s", lib.target.name, lib.target.info.game);
    // The card's folders, ticked when it's in one: a click puts it in or takes it out. New folder makes one with
    // it in and names it in the list.
    if (folders) ImGui::OpenPopup("card_folders");
    lib_popup_style_push(6);
    if (ImGui::BeginPopup("card_folders")) {
        const LibItem &it = lib.target;
        u64 m = lib_folder_mask(it.key);
        i32 n = 0;
        for (i32 i = 0; i < lib.folder_count; i++) {
            if (lib.folders[i].view != it.kind) continue;
            n++;
            bool in = m >> i & 1;
            ImGui::PushID(i);
            if (lib_menu_item(in ? ICON_CHECK : "", lib.folders[i].name)) lib_folder_put(it, i, !in);
            ImGui::PopID();
        }
        if (n) ImGui::Separator();
        if (lib_menu_item(ICON_ADD, "New folder")) {
            i32 i = lib_folder_new("New folder");
            if (i >= 0) {
                lib_folder_put(it, i, true);
                lib_folder_rename_start(i);
            }
        }
        kit_scroll_fade_window(COL_PANEL2);
        ImGui::EndPopup();
    }
    lib_popup_style_pop();
}

// The confirm dialogs (Delete, Delete folder) share one layout, only as big as what's in it: the question in large
// bold type, what it's about under it, then one row: a tick box on the left when there is one, and Cancel and the
// action on the right, two narrow buttons of one width. `show delete` and `show delete-folder [ticked]` open them.
static i32 g_show_dialog; // 1 Delete (the first card), 2 Delete folder (the first folder), 3 the same, ticked

static bool lib_dialog_begin(const char *id)
{
    lib_popup_style_push(22);
    bool open = ImGui::BeginPopupModal(id, 0, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar |
                                                  ImGuiWindowFlags_AlwaysAutoResize);
    lib_popup_style_pop();
    if (open) {
        // Idle frames stop after a few; keep drawing until the dimmed background has faded in.
        if (ImGui::IsWindowAppearing()) ui_wake(30);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(kit_px(8), kit_px(6)));
    }
    return open;
}

static void lib_dialog_end()
{
    ImGui::PopStyleVar();
    ImGui::EndPopup();
}

// One of the pair: a filled button, both the same size.
static bool lib_dialog_button(const char *label, u32 fill, u32 hover, u32 ink, f32 w)
{
    ImGui::PushStyleColor(ImGuiCol_Button, rgb(fill));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, rgb(hover));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, rgb(hover, 0.8f));
    ImGui::PushStyleColor(ImGuiCol_Text, rgb(ink));
    bool hit = ImGui::Button(label, ImVec2(w, kit_px(34)));
    ImGui::PopStyleColor(4);
    if (hit) dev_click("button", label);
    return hit;
}

#define LIB_CANCEL ICON_CLOSE "  Cancel"

// The width of each of the pair: the longer label and some room either side.
static f32 lib_dialog_button_w(const char *action)
{
    return ImMax(ImGui::CalcTextSize(LIB_CANCEL).x, ImGui::CalcTextSize(action).x) + kit_px(32);
}

// The pair, Cancel then the action, ending at the right edge `right` (screen x) on the current line: the action red
// for KIT_DANGER and the accent otherwise. 1 = the action, -1 = Cancel.
static i32 lib_dialog_pair(const char *action, KitKind kind, f32 right)
{
    f32 bw = lib_dialog_button_w(action), gap = kit_px(8);
    ImGui::SetCursorScreenPos(ImVec2(right - 2 * bw - gap, ImGui::GetCursorScreenPos().y));
    i32 r = 0;
    if (lib_dialog_button(LIB_CANCEL, COL_HOVER, 0x383838, COL_TEXT, bw)) r = -1;
    ImGui::SameLine(0, gap);
    bool red = kind == KIT_DANGER;
    if (lib_dialog_button(action, red ? COL_RED : COL_WHITE, red ? 0xf45d67 : 0xe6e6e6, red ? 0xffffff : COL_BG, bw)) r = 1;
    return r;
}

// The pair at the bottom right of a dialog with a set width (an app's name in Options).
static i32 lib_dialog_buttons(const char *action, KitKind kind)
{
    ImGui::Dummy(ImVec2(1, kit_px(4)));
    return lib_dialog_pair(action, kind, ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x);
}

static f32 lib_dialog_check_w(const char *label) { return kit_px(20 + 10) + ImGui::CalcTextSize(label).x; }

static bool lib_dialog_check(const char *label, bool *v, bool enabled);

// A confirm dialog's body (between lib_dialog_begin and lib_dialog_end): `title`, `name` under it, an optional
// dim `note`, then the row: the box `check` (when given, ticked in *ticked, greyed unless check_on) and the pair.
static i32 lib_confirm(const char *title, const char *name, const char *note, const char *check, bool *ticked,
                       bool check_on, const char *action, KitKind kind)
{
    f32 bw = lib_dialog_button_w(action), gap = kit_px(8);
    f32 row_w = (check ? lib_dialog_check_w(check) + kit_px(40) : 0) + 2 * bw + gap;
    f32 title_w = g_font_title->CalcTextSizeA(g_font_title->FontSize, FLT_MAX, 0, title).x;
    f32 name_w = ImMin(ImGui::CalcTextSize(name).x, kit_px(480));
    f32 w = ImMax(ImMax(row_w, title_w), ImMax(name_w, kit_px(300)));
    ImVec2 a = ImGui::GetCursorScreenPos();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImGui::PushFont(g_font_title);
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
    ImVec2 n = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(w, ImGui::GetTextLineHeight()));
    text_clipped(dl, n, w, col32(COL_DIM), name);
    if (note) {
        ImGui::PushTextWrapPos(a.x - ImGui::GetWindowPos().x + w);
        kit_dim("%s", note);
        ImGui::PopTextWrapPos();
    }
    ImGui::Dummy(ImVec2(1, kit_px(12)));
    f32 bh = kit_px(34), y = ImGui::GetCursorScreenPos().y;
    if (check) {
        ImGui::SetCursorScreenPos(ImVec2(a.x, floorf(y + (bh - ImMax(kit_px(20), ImGui::GetTextLineHeight())) / 2)));
        lib_dialog_check(check, ticked, check_on);
        ImGui::SetCursorScreenPos(ImVec2(a.x, y));
    }
    return lib_dialog_pair(action, kind, a.x + w);
}

// A tick box with its label, from the left edge: a ringed box, filled with a tick when on.
static bool lib_dialog_check(const char *label, bool *v, bool enabled)
{
    f32 box = kit_px(20), h = ImMax(box, ImGui::GetTextLineHeight()), gap = kit_px(10);
    ImVec2 a = ImGui::GetCursorScreenPos();
    if (!enabled) ImGui::BeginDisabled();
    bool hit = ImGui::InvisibleButton(label, ImVec2(box + gap + ImGui::CalcTextSize(label).x, h)) && enabled;
    if (!enabled) ImGui::EndDisabled();
    bool hot = enabled && ImGui::IsItemHovered();
    if (hit) {
        *v = !*v;
        dev_click("check", label);
    }
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 b0(a.x, floorf(a.y + (h - box) / 2)), b1(b0.x + box, b0.y + box);
    if (*v && enabled) {
        dl->AddRectFilled(b0, b1, col32(hot ? COL_ACCENT_HOVER : COL_ACCENT), kit_px(4));
        ImGui::PushFont(g_font_icon);
        ImVec2 cs = ImGui::CalcTextSize(ICON_CHECK);
        f32 s = box * 0.8f / ImMax(cs.x, cs.y);
        ImGui::PopFont();
        dl->AddText(g_font_icon, g_font_icon->FontSize * s, ImVec2(floorf(b0.x + (box - cs.x * s) / 2), floorf(b0.y + (box - cs.y * s) / 2)),
                    col32(COL_ACCENT_INK), ICON_CHECK);
    } else {
        dl->AddRectFilled(b0, b1, col32(COL_BG), kit_px(4));
        dl->AddRect(b0, b1, col32(!enabled ? COL_LINE : hot ? COL_DIM : COL_FAINT), kit_px(4), 0, kit_px(1.5f));
    }
    dl->AddText(ImVec2(b0.x + box + gap, floorf(a.y + (h - ImGui::GetTextLineHeight()) / 2)),
                col32(enabled ? COL_TEXT : COL_FAINT), label);
    return hit;
}

// Renames lib.target to what's in lib.rename_buf, with its edit. True when done (renamed, or the name didn't
// change); false with lib.rename_error set when it can't be.
static bool lib_rename_apply()
{
    Library &lib = g_app.library;
    if (lib.rename_display_name) {
        if (strcmp(lib.rename_buf, lib.target.display_name)) library_set_display_name(lib.target.key, lib.rename_buf, lib.target.name);
        return true;
    }
    char name[256], cleaned[256];
    str_set(name, lib.rename_buf);
    char *t = trim(name);
    str_set(cleaned, t);
    clean_name(cleaned);
    if (!*t) str_set(lib.rename_error, "The name can't be empty");
    else if (strcmp(cleaned, t)) str_set(lib.rename_error, "A name can't hold < > : \" / \\ | ? * or end in a dot");
    else if (!strcmp(t, lib.target.name)) return true;
    else {
        LibItem renamed = lib.target;
        str_set(renamed.name, t);
        Path from = lib_item_path(lib.target), to = lib_item_path(renamed);
        if (file_exists(to.s) || file_exists(lib_playable_path(renamed).s))
            str_set(lib.rename_error, "There's already a file with that name");
        else {
            if (lib.player.open && !wcscmp(lib.player.path.s, from.s)) library_close_player();
            if (MoveFileW(from.s, to.s)) {
                // The rendered copy, the edit and the render note go with it.
                Path copy = lib.target.rendered ? lib_playable_path(lib.target) : Path{};
                Path pairs[][2] = {{copy, lib_playable_path(renamed)},
                                   {lib_edit_path(lib.target), lib_edit_path(renamed)},
                                   {lib_render_note_path(lib.target), lib_render_note_path(renamed)}};
                for (auto &p : pairs)
                    if (p[0].s[0] && file_exists(p[0].s)) MoveFileW(p[0].s, p[1].s);
                lib_message(false, "Renamed to %s", t);
                library_rescan();
                return true;
            }
            snprintf(lib.rename_error, sizeof lib.rename_error, "Couldn't rename it (error %d). Is it open somewhere?",
                     (int)GetLastError());
        }
    }
    log_line("library: rename refused: %s", lib.rename_error);
    return false;
}

// Starts editing a card's name where it's shown.
static void lib_rename_start(const LibItem &it, bool display_name)
{
    Library &lib = g_app.library;
    lib.target = it;
    lib.rename_display_name = display_name;
    str_set(lib.rename_buf, display_name ? it.display_name : it.name);
    lib.rename_error[0] = 0;
    lib.rename_key = it.key;
    lib.rename_focus = true;
}

// A list of paths ending in two nulls, as SHFileOperation wants, from `n` paths (empty ones left out).
struct LibPathList { wchar_t s[LIB_ITEM_FILES * MAX_PATH + 4]; };
static const wchar_t *lib_path_list(const Path *paths, i32 n, LibPathList *out)
{
    wchar_t *list = out->s;
    memset(list, 0, sizeof out->s);
    wchar_t *at = list;
    for (i32 i = 0; i < n; i++) {
        size_t len = wcslen(paths[i].s);
        if (!len) continue;
        memcpy(at, paths[i].s, len * sizeof(wchar_t));
        at += len + 1;
    }
    return list;
}

// Sends a clip or session to the Recycle Bin, with its rendered copy and edit if there are any; true when it
// went. The player lets go of it first.
static bool lib_recycle(const LibItem &it)
{
    Library &lib = g_app.library;
    Path files[LIB_ITEM_FILES];
    i32 count = lib_item_files(it, files);
    for (i32 i = 0; i < count; i++)
        if (lib.player.open && !wcscmp(lib.player.path.s, files[i].s)) library_close_player();
    static LibPathList list;
    const wchar_t *from = lib_path_list(files, count, &list);
    SHFILEOPSTRUCTW op = {};
    op.hwnd = g_app.ui.hwnd;
    op.wFunc = FO_DELETE;
    op.pFrom = from;
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_WANTNUKEWARNING | FOF_SILENT;
    int r = SHFileOperationW(&op);
    bool ok = r == 0 && !op.fAnyOperationsAborted;
    log_line("library: %s %s the Recycle Bin (%d)", it.name, ok ? "went to" : "didn't go to", r);
    if (ok) share_clip_delete(it.key); // its share copies and links
    return ok;
}

static void delete_modal()
{
    Library &lib = g_app.library;
    if (!lib_dialog_begin("Delete")) return;
    static char note[256];
    if (ImGui::IsWindowAppearing()) lib_delete_note(&lib.target.key, 1, note, sizeof note);
    i32 b = lib_confirm("Delete?", lib.target.name, note[0] ? note : 0, 0, 0, false, ICON_DELETE "  Delete", KIT_DANGER);
    bool cancel = b == -1 || ImGui::IsKeyPressed(ImGuiKey_Escape);
    if (b == 1) {
        if (lib_recycle(lib.target)) lib_message(false, "%s went to the Recycle Bin", lib.target.name);
        else lib_message(true, "Couldn't delete %s", lib.target.name);
        library_rescan();
        cancel = true;
    }
    if (cancel) ImGui::CloseCurrentPopup();
    lib_dialog_end();
}

// ---------------------------------------------------------------------------------------------------------------
// Selecting several cards to act on together: their keys, kept in order so a card finds itself quickly. Only the
// view's shown cards count; the selection clears when the view or a filter changes (lib_sel_track).

static i32 lib_sel_slot(u64 key) // where key is or would go in lib.selected
{
    const Library &lib = g_app.library;
    i32 lo = 0, hi = lib.selected_count;
    while (lo < hi) {
        i32 mid = (lo + hi) / 2;
        if (lib.selected[mid] < key) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

static bool lib_selected(u64 key)
{
    const Library &lib = g_app.library;
    i32 i = lib_sel_slot(key);
    return i < lib.selected_count && lib.selected[i] == key;
}

static void lib_sel_set(u64 key, bool on)
{
    Library &lib = g_app.library;
    i32 i = lib_sel_slot(key);
    bool in = i < lib.selected_count && lib.selected[i] == key;
    if (on == in) return;
    if (on) {
        memmove(&lib.selected[i + 1], &lib.selected[i], (lib.selected_count - i) * sizeof lib.selected[0]);
        lib.selected[i] = key;
        lib.selected_count++;
    } else {
        memmove(&lib.selected[i], &lib.selected[i + 1], (lib.selected_count - i - 1) * sizeof lib.selected[0]);
        lib.selected_count--;
    }
}

static void lib_sel_clear()
{
    Library &lib = g_app.library;
    lib.selected_count = 0;
    lib.select_anchor = 0;
}

// Every card shown; with no anchor, the first becomes it.
static void lib_sel_all()
{
    Library &lib = g_app.library;
    if (!lib.select_anchor && lib.shown_count) lib.select_anchor = lib.items[lib.shown[0]].key;
    for (i32 s = 0; s < lib.shown_count; s++) lib_sel_set(lib.items[lib.shown[s]].key, true);
}

// Shift + click's range in n keys (the grid's order): from `anchor` to `to`, both included, as positions
// [*lo, *hi]. An anchor not among them gives `to` alone. False when `to` isn't among them.
static bool lib_range_bounds(const u64 *keys, i32 n, u64 anchor, u64 to, i32 *lo, i32 *hi)
{
    i32 from_at = -1, to_at = -1;
    for (i32 s = 0; s < n; s++) {
        if (keys[s] == anchor) from_at = s;
        if (keys[s] == to) to_at = s;
    }
    if (to_at < 0) return false;
    if (from_at < 0) from_at = to_at;
    *lo = ImMin(from_at, to_at);
    *hi = ImMax(from_at, to_at);
    return true;
}

// Shift + click: every card from the anchor to this one, in the grid's order, takes the opposite of this card's state
// (ticked when it's unticked, unticked when it's ticked), and this card becomes the anchor. With nothing selected,
// that's this card alone.
static void lib_sel_range(u64 to)
{
    Library &lib = g_app.library;
    if (lib.selected_count == 0) lib.select_anchor = to;
    static u64 keys[LIB_MAX_ITEMS];
    for (i32 s = 0; s < lib.shown_count; s++) keys[s] = lib.items[lib.shown[s]].key;
    i32 lo, hi;
    if (!lib_range_bounds(keys, lib.shown_count, lib.select_anchor, to, &lo, &hi)) return;
    bool on = !lib_selected(to);
    for (i32 s = lo; s <= hi; s++) lib_sel_set(keys[s], on);
    lib.select_anchor = to;
}

// A left click on card `key` (on its tick box when `on_box`). Shift + click, on the box or not, ticks or unticks the
// range from the anchor (lib_sel_range). A click on the box, Ctrl + click, or any click while cards are selected ticks or
// unticks the card, and it becomes the anchor. False when the click plays the card instead.
static bool lib_card_click(u64 key, bool shift, bool ctrl, bool on_box)
{
    Library &lib = g_app.library;
    if (shift) lib_sel_range(key);
    else if (on_box || ctrl || lib.selected_count > 0) {
        lib_sel_set(key, !lib_selected(key));
        lib.select_anchor = key;
    } else return false;
    return true;
}

// The selected cards that are shown, as indices into lib.items in the grid's order; how many.
static i32 lib_sel_indices(i32 *out)
{
    const Library &lib = g_app.library;
    i32 n = 0;
    if (lib.selected_count == 0) return 0;
    for (i32 s = 0; s < lib.shown_count; s++)
        if (lib_selected(lib.items[lib.shown[s]].key)) out[n++] = lib.shown[s];
    return n;
}

// Once a frame: the selection clears when the view or a filter changes, and drops cards no longer shown (once a
// scan has settled, so cards coming back from a rescan stay ticked).
static void lib_sel_track()
{
    Library &lib = g_app.library;
    u64 h = fnv64(&lib.view, sizeof lib.view);
    h = fnv64(&lib.range, sizeof lib.range, h);
    h = fnv64(&lib.range_from, sizeof lib.range_from, h);
    h = fnv64(&lib.range_to, sizeof lib.range_to, h);
    h = fnv64(&lib.sort, sizeof lib.sort, h);
    h = fnv64(lib.search, strlen(lib.search), h);
    h = fnv64(lib.game, strlen(lib.game), h);
    h = fnv64(lib.folder, strlen(lib.folder), h);
    h = fnv64(&g_app.config.side_folders, sizeof g_app.config.side_folders, h);
    if (h != lib.select_filter) {
        lib.select_filter = h;
        lib_sel_clear();
    }
    if (lib.scanning || lib.selected_count == 0) return;
    static i32 idx[LIB_MAX_ITEMS];
    i32 n = lib_sel_indices(idx);
    if (n == lib.selected_count) return;
    lib.selected_count = 0;
    for (i32 i = 0; i < n; i++) lib_sel_set(lib.items[idx[i]].key, true);
}

static const char *lib_noun(i32 n)
{
    return g_app.library.view == LIB_CLIPS ? (n == 1 ? "clip" : "clips") : (n == 1 ? "session" : "sessions");
}

// Puts the selected cards in folder fi, or takes them out of it: one save, one message.
static void lib_sel_folder_put(i32 fi, bool in)
{
    Library &lib = g_app.library;
    static i32 idx[LIB_MAX_ITEMS];
    i32 n = lib_sel_indices(idx);
    u64 b = 1ull << fi;
    for (i32 i = 0; i < n; i++) {
        u64 key = lib.items[idx[i]].key, m = lib_folder_mask(key);
        if (((m & b) != 0) != in) lib_folder_mask_set(key, in ? m | b : m & ~b);
    }
    if (in) lib_message(false, "Added %d %s to %s", n, lib_noun(n), lib.folders[fi].name);
    else lib_message(false, "Took %d %s out of %s", n, lib_noun(n), lib.folders[fi].name);
    log_line("library: %d %s %s %s", n, lib_noun(n), in ? "put in" : "taken out of", lib.folders[fi].name);
    lib_folders_save();
    lib_refilter();
}

// Files the selected cards under one game (one write of games.txt) and tells the art service about each.
static void lib_sel_set_game(const GamePickRow &r)
{
    const char *game = r.name;
    bool known = game_pick_known(r);
    Library &lib = g_app.library;
    static i32 idx[LIB_MAX_ITEMS];
    static u64 keys[LIB_MAX_ITEMS];
    static const char *names[LIB_MAX_ITEMS];
    static LibItem items[LIB_MAX_ITEMS / 8]; // copies told to the art service; a rescan rewrites lib.items
    i32 n = lib_sel_indices(idx), m = 0;
    for (i32 i = 0; i < n; i++) {
        const LibItem &it = lib.items[idx[i]];
        if (!strcmp(it.info.game, game)) continue;
        keys[m] = it.key;
        names[m++] = it.name;
    }
    if (m == 0) return;
    for (i32 i = 0, told = 0; i < n && told < COUNT(items); i++)
        if (strcmp(lib.items[idx[i]].info.game, game)) items[told++] = lib.items[idx[i]];
    if (!lib_keyed_write(L"games.txt", keys, names, m, game)) return;
    log_line("library: %d %s are now %s", m, lib_noun(m), game);
    game_pick_filed(r);
    for (i32 i = 0; i < m && i < COUNT(items); i++) {
        str_set(items[i].info.game, game);
        if (known) gamedb_clip_game(items[i], game);
    }
    lib_message(false, "%d %s filed under %s", m, lib_noun(m), game);
}

// Deleting the selection: each file (and its edit) goes to the Recycle Bin on a thread of its own, then the grid
// is scanned again once.
struct LibDeleteJob {
    Path *files; // LIB_ITEM_FILES a video: lib_item_files, empty paths after
    i32   count; // videos
};

static DWORD WINAPI lib_delete_thread(void *arg)
{
    LibDeleteJob *job = (LibDeleteJob *)arg;
    CoInitializeEx(0, COINIT_APARTMENTTHREADED);
    i32 gone = 0;
    for (i32 i = 0; i < job->count; i++) {
        static LibPathList list; // this thread's own: one delete runs at a time
        const wchar_t *from = lib_path_list(job->files + LIB_ITEM_FILES * i, LIB_ITEM_FILES, &list);
        SHFILEOPSTRUCTW op = {};
        op.wFunc = FO_DELETE;
        op.pFrom = from;
        op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
        int r = SHFileOperationW(&op);
        if (r == 0 && !op.fAnyOperationsAborted) gone++;
        else log_line("library: a file didn't go to the Recycle Bin (%d)", r);
    }
    CoUninitialize();
    Event e = {};
    e.kind = EV_LIBRARY;
    e.a = LIB_EV_DELETED;
    e.b = ((i64)job->count << 32) | gone;
    free(job->files);
    free(job);
    event_post(e);
    return 0;
}

static void lib_sel_delete()
{
    Library &lib = g_app.library;
    static i32 idx[LIB_MAX_ITEMS];
    i32 n = lib_sel_indices(idx);
    if (n == 0 || lib.deleting) return;
    LibDeleteJob *job = (LibDeleteJob *)calloc(1, sizeof *job);
    job->files = (Path *)calloc(LIB_ITEM_FILES * n, sizeof(Path));
    job->count = n;
    player_close(&lib.hover);
    for (i32 i = 0; i < n; i++) {
        const LibItem &it = lib.items[idx[i]];
        Path *files = job->files + LIB_ITEM_FILES * i;
        for (i32 k = 0, count = lib_item_files(it, files); k < count; k++)
            if (lib.player.open && !wcscmp(lib.player.path.s, files[k].s)) library_close_player();
        share_clip_delete(it.key); // its share copies and links
    }
    log_line("library: deleting %d %s", n, lib_noun(n));
    lib_message(false, "Deleting %d %s...", n, lib_noun(n));
    lib.deleting = true;
    lib_sel_clear();
    HANDLE t = CreateThread(0, 0, lib_delete_thread, job, 0, 0);
    if (t) CloseHandle(t);
    else {
        lib.deleting = false;
        free(job->files);
        free(job);
        lib_message(true, "Couldn't delete them");
    }
}

// The selection's Delete dialog: "Delete 3 clips?" over the first names.
static void lib_sel_delete_modal()
{
    Library &lib = g_app.library;
    if (!lib_dialog_begin("Delete selected")) return;
    static i32 idx[LIB_MAX_ITEMS];
    i32 n = lib_sel_indices(idx);
    char title[64], names[300] = "";
    snprintf(title, sizeof title, "Delete %d %s?", n, lib_noun(n));
    i32 len = 0, listed = 0;
    for (; listed < n && listed < 3; listed++) {
        const LibItem &it = lib.items[idx[listed]];
        len += snprintf(names + len, sizeof names - len, "%s%s", listed ? ", " : "", it.display_name[0] ? it.display_name : it.name);
        if (len >= (i32)sizeof names - 1) break;
    }
    if (n > listed && len < (i32)sizeof names - 1) snprintf(names + len, sizeof names - len, " and %d more", n - listed);
    static char note[256];
    if (ImGui::IsWindowAppearing()) {
        static u64 keys[LIB_MAX_ITEMS];
        for (i32 i = 0; i < n; i++) keys[i] = lib.items[idx[i]].key;
        lib_delete_note(keys, n, note, sizeof note);
    }
    i32 b = lib_confirm(title, names, note[0] ? note : 0, 0, 0, false, ICON_DELETE "  Delete", KIT_DANGER);
    if (b == 1) lib_sel_delete();
    if (b != 0 || n == 0 || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    lib_dialog_end();
}

// A button of the selection bar: an icon and a word on a quiet fill, red for Delete. True when clicked.
static bool lib_bar_button(const char *id, const char *icon, const char *label, bool danger = false)
{
    f32 h = ImGui::GetFrameHeight(), pad = kit_px(12), gap = kit_px(8), round = ImGui::GetStyle().FrameRounding;
    f32 isz = g_font->FontSize;
    ImVec2 is = g_font_icon->CalcTextSizeA(isz, FLT_MAX, 0, icon), ts = ImGui::CalcTextSize(label);
    f32 w = pad + is.x + gap + ts.x + pad;
    ImVec2 a = ImGui::GetCursorScreenPos();
    bool hit = ImGui::InvisibleButton(id, ImVec2(w, h)) && !kit_closed_here(ImGui::GetItemID());
    if (hit) dev_click("button", label);
    bool hot = ImGui::IsItemHovered();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    u32 fill = hot ? COL_HOVER : COL_PANEL2;
    u32 ink = danger ? COL_RED : COL_TEXT;
    dl->AddRectFilled(a, ImVec2(a.x + w, a.y + h), col32(fill), round);
    dl->AddText(g_font_icon, isz, ImVec2(a.x + pad, floorf(a.y + (h - is.y) / 2)), col32(danger ? ink : COL_DIM), icon);
    dl->AddText(ImVec2(a.x + pad + is.x + gap, floorf(a.y + (h - ts.y) / 2)), col32(ink), label);
    ImGui::SameLine(0, kit_px(6));
    return hit;
}

// The selection bar, in place of the date, order and search on the header's row from `at` to `right` (screen x):
// how many are selected, then what can be done to them all, and Clear at the far right.
static void lib_sel_bar(ImVec2 at, f32 right, i32 n)
{
    Library &lib = g_app.library;
    f32 fh = ImGui::GetFrameHeight();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    char count[32];
    snprintf(count, sizeof count, "%d selected", n);
    f32 slot = g_font_bold->CalcTextSizeA(g_font_bold->FontSize, FLT_MAX, 0, "0000 selected").x;
    dl->AddText(g_font_bold, g_font_bold->FontSize, ImVec2(at.x + kit_px(4), floorf(at.y + (fh - g_font_bold->FontSize) / 2)),
                col32(COL_TEXT), count);
    ImGui::SetCursorScreenPos(ImVec2(at.x + kit_px(4) + slot + kit_px(12), at.y));
    bool in_folder = g_app.config.side_folders && lib.folder[0];
    if (lib_bar_button("##sel_all", ICON_CHECK, "Select all")) lib_sel_all();
    ImVec2 game_at = ImGui::GetCursorScreenPos();
    if (lib_bar_button("##sel_game", ICON_GAME, "Change game")) ImGui::OpenPopup("sel_game");
    ImVec2 folder_at = ImGui::GetCursorScreenPos();
    if (lib_bar_button("##sel_folder", ICON_FOLDER, "Move to folder")) ImGui::OpenPopup("sel_folders");
    if (in_folder && lib_bar_button("##sel_out", ICON_CLOSE, "Remove from folder")) {
        i32 fi = lib_folder_find(lib.view, lib.folder);
        if (fi >= 0) lib_sel_folder_put(fi, false);
    }
    if (lib_bar_button("##sel_delete", ICON_DELETE, "Delete", true)) ImGui::OpenPopup("Delete selected");
    ImGui::SetCursorScreenPos(ImVec2(right - fh, at.y));
    if (kit_icon_button(ICON_CLOSE, "##sel_clear")) lib_sel_clear();

    ImGui::SetNextWindowPos(ImVec2(game_at.x, at.y + fh + kit_px(6)), ImGuiCond_Appearing);
    if (const GamePickRow *r = lib_game_pick("sel_game", "")) lib_sel_set_game(*r);
    ImGui::SetNextWindowPos(ImVec2(folder_at.x, at.y + fh + kit_px(6)));
    lib_popup_style_push(6);
    if (ImGui::BeginPopup("sel_folders")) {
        i32 shown = 0;
        for (i32 i = 0; i < lib.folder_count; i++) {
            if (lib.folders[i].view != lib.view) continue;
            shown++;
            ImGui::PushID(i);
            if (lib_menu_item(ICON_FOLDER, lib.folders[i].name, false, kit_px(200))) lib_sel_folder_put(i, true);
            ImGui::PopID();
        }
        if (shown) ImGui::Separator();
        if (lib_menu_item(ICON_ADD, "New folder", false, kit_px(200))) {
            i32 i = lib_folder_new("New folder");
            if (i >= 0) {
                lib_sel_folder_put(i, true);
                lib_folder_rename_start(i);
            }
        }
        kit_scroll_fade_window(COL_PANEL2);
        ImGui::EndPopup();
    }
    lib_popup_style_pop();
}

// The name and the line under a card's picture.
static f32 lib_meta_h() { return kit_px(8) + g_font_bold->FontSize + kit_px(2) + g_font_small->FontSize + kit_px(2); }

// One card: its period above it (on the first card of one, lib_period), the picture, the name and one dim line.
// Under the mouse the picture scrubs through the file (a click plays from there); a right-click opens its menu.
// A tick box in the picture's top left corner, under the mouse or while any card is selected, selects it; while
// any is, a click on a card ticks it too (Ctrl + click always, Shift + click a range on or off).
static void lib_card(const LibItem &it, ImVec2 a, f32 w, f32 thumb_h, f32 day_h, bool show_day)
{
    Library &lib = g_app.library;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    f32 meta_h = lib_meta_h();
    if (show_day && (lib.sort == SORT_NEWEST || lib.sort == SORT_OLDEST || lib.sort == SORT_GAME)) {
        char day[64];
        if (lib.sort == SORT_GAME) str_set(day, it.info.game);
        else lib_period(it.when_us, day, sizeof day);
        ImGui::PushFont(g_font_bold);
        dl->AddText(ImVec2(a.x, a.y + day_h - ImGui::GetFontSize() - kit_px(7)), col32(COL_DIM), day);
        ImGui::PopFont();
    }
    ImVec2 t0(a.x, a.y + day_h), t1(a.x + w, a.y + day_h + thumb_h);
    ImGui::SetCursorScreenPos(t0);
    ImGui::PushID((const void *)(uintptr_t)it.key);
    bool renaming = lib.rename_key == it.key;
    char shown[300];
    lib_shown_name(it.name, shown, sizeof shown);
    ImGui::SetNextItemAllowOverlap(); // the Share button sits on it
    bool pressed = ImGui::InvisibleButton("card", ImVec2(w, renaming ? thumb_h : thumb_h + meta_h));
    if (ImGui::IsItemHovered() && ImGui::IsKeyPressed(ImGuiKey_F2, false) && !renaming) lib_rename_start(it);
    // A click plays it; a drag carries it to a folder in the list on the left.
    ImGuiIO &io = ImGui::GetIO();
    bool clicked = pressed && io.MouseDragMaxDistanceSqr[0] < io.MouseDragThreshold * io.MouseDragThreshold;
    // The Share button's place in the caption: a right-click there is the button's, never the card's menu
    f32 sh = kit_px(28), sw = sh, isz = g_font_bold->FontSize + kit_px(2) + g_font_small->FontSize;
    ImVec2 s0(a.x + w - sw, floorf(t1.y + kit_px(8) + (isz - sh) / 2)), s1(s0.x + sw, s0.y + sh);
    bool menu = ImGui::IsItemClicked(ImGuiMouseButton_Right) && !ImGui::IsMouseHoveringRect(s0, s1);
    bool over = ImGui::IsWindowHovered() && !ImGui::GetDragDropPayload() &&
                ImGui::IsMouseHoveringRect(t0, ImVec2(t1.x, t1.y + meta_h));
    f32 round = kit_px(4);
    LibThumb *th = lib_thumb(it);
    if (!(th && (th->srv || th->failed)) && it.probed && it.info.duration > 0) g_lib_missing++;
    bool sel = lib_selected(it.key), picking = lib.selected_count > 0;
    // Dragging a selected card carries the whole selection: the picture gets a badge with how many.
    if (!renaming && ImGui::BeginDragDropSource()) {
        ImGui::SetDragDropPayload("LIB_CARD", &it.key, sizeof it.key);
        ImVec2 p0 = ImGui::GetCursorScreenPos(), ps(kit_px(112), kit_px(63));
        if (th && th->srv) ImGui::Image((ImTextureID)(intptr_t)th->srv, ps);
        else ImGui::Dummy(ps);
        if (sel && lib.selected_count > 1) {
            char many[16];
            snprintf(many, sizeof many, "%d", lib.selected_count);
            badge(ImGui::GetWindowDrawList(), ImVec2(p0.x + ps.x - kit_px(6), p0.y + kit_px(6)), true, false, many);
        }
        ImGui::TextUnformatted(it.display_name[0] ? it.display_name : shown);
        ImGui::EndDragDropSource();
    }
    f32 frac = -1;
    if (over) {
        frac = (ImGui::GetIO().MousePos.x - t0.x) / w;
        frac = frac < 0 ? 0 : frac > 0.999f ? 0.999f : frac;
    }
    // Resting on the card: after half a second its video plays there, muted, from the mouse's point, until the
    // mouse moves or leaves.
    Player &hp = lib.hover;
    bool live = false;
    if (over) {
        ImVec2 m = ImGui::GetIO().MousePos;
        f64 now = ImGui::GetTime();
        if (lib.hover_key != it.key || fabsf(m.x - lib.hover_mx) > 2 || fabsf(m.y - lib.hover_my) > 2) {
            player_close(&hp);
            lib.hover_key = it.key;
            lib.hover_mx = m.x;
            lib.hover_my = m.y;
            lib.hover_since = now;
        }
        lib.hover_frame = ImGui::GetFrameCount();
        if (!hp.open && !renaming && it.info.duration > 0) {
            if (now - lib.hover_since >= 0.5) {
                hp.id = 3;
                if (player_open(&hp, lib_item_path(it), frac * it.info.duration, true)) hp.engine->SetMuted(TRUE);
            } else ui_wake(2);
        }
        live = hp.open && hp.loaded && hp.frame_ready && hp.srv && !hp.paused && !hp.seeking && !hp.has_seek;
    }
    bool scrub = over && !live && lib_want_strip(it);
    if (live) {
        dl->AddImageRounded((ImTextureID)(intptr_t)hp.srv, t0, t1, ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, round);
    } else if (scrub) {
        i32 f = (i32)(frac * lib.strip_frames);
        f32 u0 = (f32)f / lib.strip_frames, u1 = (f32)(f + 1) / lib.strip_frames;
        dl->AddImageRounded((ImTextureID)(intptr_t)lib.strip, t0, t1, ImVec2(u0, 0), ImVec2(u1, 1), IM_COL32_WHITE, round);
    } else if (th && th->srv) {
        dl->AddImageRounded((ImTextureID)(intptr_t)th->srv, t0, t1, ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, round);
    } else {
        dl->AddRectFilled(t0, t1, col32(COL_PANEL), round);
        ImGui::PushFont(g_font_icon);
        ImVec2 is = ImGui::CalcTextSize(ICON_VIDEO);
        dl->AddText(ImVec2(floorf((t0.x + t1.x - is.x) / 2), floorf((t0.y + t1.y - is.y) / 2)),
                    col32(th && th->failed ? COL_FAINT : COL_HOVER), ICON_VIDEO);
        ImGui::PopFont();
    }
    char text[64];
    if (over) {
        if (live) frac = it.info.duration > 0 ? (f32)(hp.pos / it.info.duration) : 0;
        frac = frac < 0 ? 0 : frac > 0.999f ? 0.999f : frac;
        if (!live) {
            dl->AddRectFilled(t0, t1, IM_COL32(0, 0, 0, 50), round);
            if (!picking) lib_play_mark(dl, ImVec2((t0.x + t1.x) / 2, (t0.y + t1.y) / 2), kit_px(24));
        }
        // Where a click starts playing: a bar the whole width of the picture's bottom edge (the picture's own
        // rounded shape, clipped to a strip, so it follows the corners), and the time.
        f32 in = kit_px(10), bh = kit_px(4), by = t1.y - bh;
        if (!picking) {
            dl->PushClipRect(ImVec2(t0.x, by), t1, true);
            dl->AddRectFilled(t0, t1, IM_COL32(255, 255, 255, 70), round);
            dl->PopClipRect();
            dl->PushClipRect(ImVec2(t0.x, by), ImVec2(t0.x + frac * w, t1.y), true);
            dl->AddRectFilled(t0, t1, col32(COL_ACCENT), round);
            dl->PopClipRect();
        }
        f32 time_w = 0;
        if (it.info.duration > 0) {
            fmt_time(frac * it.info.duration, text, sizeof text);
            time_w = badge(dl, ImVec2(t1.x - in, by - kit_px(6)), true, true, text) + kit_px(6);
        }
        if (it.info.track_count > 1) { // left of the time
            snprintf(text, sizeof text, "%d tracks", it.info.track_count);
            badge(dl, ImVec2(t1.x - in - time_w, by - kit_px(6)), true, true, text);
        }
    } else if (it.info.duration > 0) {
        fmt_time(it.info.duration, text, sizeof text);
        badge(dl, ImVec2(t1.x - kit_px(8), t1.y - kit_px(8)), true, true, text);
    }
    // Bottom left, where nothing else goes: edited in Advanced mode, then a round badge while its playable copy is
    // being made (render.cpp): an accent arc turning while it renders, a still faint ring while it waits its turn.
    f32 bx = t0.x + kit_px(8);
    if (lib_is_advanced(it)) bx += badge(dl, ImVec2(bx, t1.y - kit_px(10)), false, true, "Advanced") + kit_px(4);
    f32 rf = render_progress(it.key);
    bool rendering = rf >= 0;
    if (!rendering && render_waiting(it.key)) rf = 0;
    if (rf >= 0) {
        f32 br = (g_font_small->FontSize + kit_px(4)) / 2, ar = br - kit_px(4);
        ImVec2 bc(floorf(bx + br), floorf(t1.y - kit_px(10) - br));
        dl->AddCircleFilled(bc, br, IM_COL32(0, 0, 0, 192), 24);
        dl->AddCircle(bc, ar, col32(0xffffff, rendering ? 0.2f : 0.45f), 24, kit_px(1.5f));
        if (rendering) {
            f32 a0 = (f32)fmod(ImGui::GetTime() * 2.5, 2 * IM_PI);
            dl->PathArcTo(bc, ar, a0, a0 + IM_PI * 0.7f, 16);
            dl->PathStroke(col32(COL_ACCENT), 0, kit_px(1.5f));
        }
    }
    // Along the top edge while its playable copy is being made: a thin bar of how far it's got. Waiting its turn, the
    // empty track alone.
    if (rf >= 0) {
        f32 eh = kit_px(3);
        dl->PushClipRect(t0, ImVec2(t1.x, t0.y + eh), true);
        dl->AddRectFilled(t0, t1, col32(0xffffff, 0.25f), round);
        dl->PopClipRect();
        dl->PushClipRect(t0, ImVec2(t0.x + w * (rf > 1 ? 1 : rf), t0.y + eh), true);
        dl->AddRectFilled(t0, t1, col32(COL_ACCENT), round);
        dl->PopClipRect();
        if (render_progress(it.key) >= 0) ui_wake(); // waiting, it changes only with the look every 10 s
    }
    // Top right, where nothing else goes: an amber pause mark while the clip's link is paused, which its Share
    // button doesn't show. What Share gives at once is on the button itself (share_ready_kind). Under the mouse, a
    // pencil left of it renames the clip right there on the card.
    f32 mark_r = kit_px(11);
    ImVec2 pen_c(t1.x - kit_px(8) - mark_r, t0.y + kit_px(8) + mark_r);
    {
        ClipShareState cs = lib_clip_state(it);
        if (cs.link && cs.link_paused && !cs.link_ready) {
            f32 msz = kit_px(12);
            dl->AddCircleFilled(pen_c, mark_r, IM_COL32(0, 0, 0, 170));
            ImVec2 gs = g_font_icon->CalcTextSizeA(msz, FLT_MAX, 0, ICON_PAUSE);
            dl->AddText(g_font_icon, msz, ImVec2(floorf(pen_c.x - gs.x / 2), floorf(pen_c.y - gs.y / 2)), col32(COL_WARN), ICON_PAUSE);
            pen_c.x -= 2 * mark_r + kit_px(6);
        }
    }
    bool pen = over && !picking && !renaming;
    ImVec2 pen0(pen_c.x - mark_r, pen_c.y - mark_r), pen1(pen_c.x + mark_r, pen_c.y + mark_r);
    if (pen) {
        bool hot = ImGui::IsMouseHoveringRect(pen0, pen1);
        kit_tip_rect(pen0, pen1, "Rename");
        dl->AddCircleFilled(pen_c, mark_r, IM_COL32(10, 10, 10, hot ? 235 : 170));
        f32 msz = kit_px(12);
        ImVec2 gs = g_font_icon->CalcTextSizeA(msz, FLT_MAX, 0, ICON_RENAME);
        dl->AddText(g_font_icon, msz, ImVec2(floorf(pen_c.x - gs.x / 2), floorf(pen_c.y - gs.y / 2)),
                    IM_COL32(255, 255, 255, hot ? 255 : 210), ICON_RENAME);
    }
    // Selected: an accent ring just outside the picture, and the name in the accent. The tick box: filled with a tick when selected, else a ring, which
    // out of select mode shows only faintly while the card is under the mouse, and in full under the mouse itself.
    f32 box = kit_px(17);
    ImVec2 b0(t0.x + kit_px(10), t0.y + kit_px(10)), b1(b0.x + box, b0.y + box);
    if (sel) {
        f32 out = kit_px(4); // 2 px wide, 3 px off the picture
        dl->AddRect(ImVec2(t0.x - out, t0.y - out), ImVec2(t1.x + out, t1.y + out), col32(COL_ACCENT), round + out, 0, kit_px(2));
    }
    if (sel || over || picking) {
        ImVec2 cm = io.MousePos;
        bool box_hot = over && cm.x >= b0.x - kit_px(4) && cm.x < b1.x + kit_px(4) && cm.y >= b0.y - kit_px(4) && cm.y < b1.y + kit_px(4);
        if (over) kit_tip_rect(ImVec2(b0.x - kit_px(4), b0.y - kit_px(4)), ImVec2(b1.x + kit_px(4), b1.y + kit_px(4)),
                               sel ? "Unselect" : "Select");
        if (sel) {
            dl->AddRectFilled(b0, b1, col32(box_hot ? COL_ACCENT_HOVER : COL_ACCENT), kit_px(4));
            ImVec2 cs = g_font_icon->CalcTextSizeA(box * 0.6f, FLT_MAX, 0, ICON_CHECK);
            dl->AddText(g_font_icon, box * 0.6f, ImVec2(floorf(b0.x + (box - cs.x) / 2), floorf(b0.y + (box - cs.y) / 2)),
                        col32(COL_ACCENT_INK), ICON_CHECK);
        } else {
            bool faint = !picking && !box_hot;
            dl->AddRectFilled(b0, b1, IM_COL32(10, 10, 10, box_hot ? 235 : faint ? 90 : 220), kit_px(4)); // reads over bright pictures
            dl->AddRect(b0, b1, IM_COL32(255, 255, 255, box_hot ? 255 : faint ? 80 : 220), kit_px(4), 0, kit_px(1.5f));
        }
    }
    ImGui::PopID();
    if (menu) { // opened outside the card's ID, where card_menu() looks for it
        lib.target = it;
        lib.open_menu = true;
    }
    // The caption: the game's icon, then the name and one dim line, and the Share button on the right, in the same
    // place on every card. Renaming edits the name right there.
    f32 y = t1.y + kit_px(8);
    f32 tx = a.x + isz + kit_px(10), tw = a.x + w - sw - kit_px(10) - tx;
    // No game (GAME_DESKTOP, GAME_NOT_OURS) and a custom game get a plain glyph, never an icon looked up by name.
    bool desktop = !strcmp(it.info.game, GAME_DESKTOP), no_game = game_is_none(it.info.game);
    bool custom = !no_game && it.info.game[0] && game_is_custom(it.info.game);
    ID3D11ShaderResourceView *icon = it.info.game[0] && !no_game && !custom ? game_icon(it.info.game) : 0;
    const char *glyph = custom ? ICON_GAME : desktop ? ICON_MONITOR : ICON_VIDEO;
    if (icon) dl->AddImageRounded((ImTextureID)icon, ImVec2(a.x, y), ImVec2(a.x + isz, y + isz), ImVec2(0, 0), ImVec2(1, 1),
                                  IM_COL32_WHITE, kit_px(3));
    else {
        dl->AddRectFilled(ImVec2(a.x, y), ImVec2(a.x + isz, y + isz), col32(COL_PANEL), kit_px(3));
        ImGui::PushFont(g_font_icon);
        ImVec2 is = ImGui::CalcTextSize(glyph);
        dl->AddText(ImVec2(floorf(a.x + (isz - is.x) / 2), floorf(y + (isz - is.y) / 2)), col32(COL_FAINT), glyph);
        ImGui::PopFont();
    }
    ImGui::PushFont(g_font_bold);
    if (renaming) {
        ImGui::PushID((const void *)(uintptr_t)it.key);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(kit_px(4), kit_px(1)));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, kit_px(4));
        ImGui::SetCursorScreenPos(ImVec2(tx - kit_px(4), y - kit_px(1)));
        ImGui::SetNextItemWidth(tw + kit_px(4));
        if (lib.rename_focus) {
            ImGui::SetKeyboardFocusHere();
            lib.rename_focus = false;
        }
        i32 cap = lib.rename_display_name ? (i32)sizeof it.display_name : (i32)sizeof lib.rename_buf;
        bool enter = ImGui::InputTextWithHint("##rename", lib.rename_display_name ? "Add a display name" : "", lib.rename_buf, cap,
                                              ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        bool left = ImGui::IsItemDeactivated(), esc = ImGui::IsKeyPressed(ImGuiKey_Escape, false);
        ImGui::PopStyleVar(2);
        ImGui::PopID();
        if (esc) lib.rename_key = 0; // Esc: as it was
        else if (enter || left) { // Enter or clicking away: rename (or keep editing, with why it can't be)
            if (lib_rename_apply()) lib.rename_key = 0;
            else lib.rename_focus = true;
        }
    } else text_clipped(dl, ImVec2(tx, y), tw, col32(sel ? COL_ACCENT : COL_TEXT), it.display_name[0] ? it.display_name : shown);
    y += ImGui::GetFontSize() + kit_px(2);
    ImGui::PopFont();
    char when[48], size[24], line[300];
    fmt_when(it.when_us, when, sizeof when);
    fmt_size(it.size, size, sizeof size);
    bool err = renaming && lib.rename_error[0];
    if (err) str_set(line, lib.rename_error);
    else if (it.display_name[0] || (renaming && lib.rename_display_name)) snprintf(line, sizeof line, "%s  -  %s", it.name, size);
    else snprintf(line, sizeof line, "%s  -  %s  -  %s", it.info.game, when, size);
    ImGui::PushFont(g_font_small);
    f32 line_w = tw;
    if (lib_comment(it.key)[0] && !err) { // a note at the line's right end, the same spot on every card
        f32 gsz = g_font_small->FontSize;
        ImVec2 gs = g_font_icon->CalcTextSizeA(gsz, FLT_MAX, 0, ICON_COMMENT);
        dl->AddText(g_font_icon, gsz, ImVec2(floorf(tx + tw - gs.x), floorf(y + (gsz - gs.y) / 2 + kit_px(1))), col32(COL_DIM),
                    ICON_COMMENT);
        line_w = tw - gs.x - kit_px(8);
    }
    // A video with no game says "Pick a game" in its place, and a click on those words opens the game picker
    bool pick = !err && !it.display_name[0] && !renaming && !picking && !strcmp(it.info.game, GAME_NOT_OURS);
    ImVec2 p0, p1;
    if (pick) {
        const char *ask = "Pick a game";
        f32 aw = ImMin(ImGui::CalcTextSize(ask).x, line_w);
        p0 = ImVec2(tx, y), p1 = ImVec2(tx + aw, y + ImGui::GetFontSize());
        bool hot = ImGui::IsMouseHoveringRect(p0, p1) && over;
        if (hot) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        text_clipped(dl, p0, line_w, col32(hot ? COL_WHITE : COL_TEXT), ask);
        const char *rest = line + strlen(GAME_NOT_OURS); // "  -  <when>  -  <size>"
        f32 rx = tx + ImGui::CalcTextSize(ask).x;
        if (rx < tx + line_w) text_clipped(dl, ImVec2(rx, y), tx + line_w - rx, col32(COL_DIM), rest);
    } else text_clipped(dl, ImVec2(tx, y), line_w, col32(err ? COL_RED : COL_DIM), line);
    ImGui::PopFont();
    // Share: a click shares as the one-click rule picks, a right-click opens the card's Share menu
    ImGui::PushID((const void *)(uintptr_t)it.key);
    ImGui::SetCursorScreenPos(s0);
    bool share = ImGui::InvisibleButton("##cardshare", ImVec2(sw, sh));
    bool share_hot = ImGui::IsItemHovered(), share_held = share_hot && ImGui::IsItemActive();
    bool share_menu = share_hot && ImGui::IsMouseClicked(ImGuiMouseButton_Right);
    ShareJobView sj = g_lib_share_job && g_lib_share_job_key == it.key ? share_job(g_lib_share_job) : ShareJobView{};
    bool share_running = sj.id && sj.state < SHARE_DONE; // the button is its Cancel
    bool share_big = !share_running && lib_share_too_big(it); // greyed, its click says why
    ClipShareState share_cs = lib_clip_state(it);
    kit_tip(share_running ? "Cancel the share" : share_big ? "Too big for your connected hosts"
                                                           : share_ready_tip(share_cs));
    ImGui::PopID();
    {
        KitKind sk = share_running || share_big ? KIT_SECONDARY : share_ready_kind(share_cs, KIT_SECONDARY);
        u32 sfill = sk == KIT_ACCENT ? (share_hot ? COL_ACCENT_HOVER : COL_ACCENT)
                                     : share_hot && !share_big ? COL_HOVER : COL_PANEL2;
        dl->AddRectFilled(s0, s1, col32(sfill, share_held ? 0.8f : 1.f), ImGui::GetStyle().FrameRounding);
        if (sk == KIT_TINT)
            dl->AddRectFilled(s0, s1, col32(COL_ACCENT, share_hot ? 0.3f : 0.2f), ImGui::GetStyle().FrameRounding);
        if (share_running) {
            share_cancel_ring(dl, ImVec2(floorf((s0.x + s1.x) / 2), floorf((s0.y + s1.y) / 2)), kit_px(8), sj.progress,
                              COL_TEXT);
            ui_wake();
        } else {
            f32 is = g_font_icon->FontSize;
            const char *sicon = share_big ? ICON_SHARE : share_ready_icon(share_cs);
            u32 ink = share_big ? COL_FAINT : sk == KIT_ACCENT ? COL_WHITE : sk == KIT_TINT ? COL_ACCENT_HOVER : COL_TEXT;
            ImVec2 ts = g_font_icon->CalcTextSizeA(is, FLT_MAX, 0, sicon);
            dl->AddText(g_font_icon, is, ImVec2(floorf(s0.x + (sw - ts.x) / 2), floorf(s0.y + (sh - ts.y) / 2)),
                        col32(ink), sicon);
        }
    }
    if (share && share_running) {
        log_line("library: share %u cancelled", sj.id);
        share_cancel(sj.id);
    } else if (share) {
        dev_click("library", "card share");
        lib_share_decided(it);
    } else if (share_menu && !lib_share_refused(it)) {
        dev_click("library", "card share menu");
        lib.target = it;
        g_card_share_open = true;
    }
    if (clicked) {
        ImVec2 c = io.MouseClickedPos[0];
        bool on_box = c.x >= b0.x - kit_px(4) && c.x < b1.x + kit_px(4) && c.y >= b0.y - kit_px(4) && c.y < b1.y + kit_px(4);
        bool on_pick = pick && c.x >= p0.x && c.x < p1.x && c.y >= p0.y - kit_px(2) && c.y < p1.y + kit_px(2);
        bool on_pen = pen && c.x >= pen0.x && c.x < pen1.x && c.y >= pen0.y && c.y < pen1.y;
        if (on_pen) {
            dev_click("library", "card rename");
            lib_rename_start(it);
        } else if (on_pick) {
            lib.target = it;
            g_card_pick_game = true;
        } else if (!lib_card_click(it.key, io.KeyShift, io.KeyCtrl, on_box)) library_play(it);
    }
}

// The library player's volume: one for every file, kept while Rekodr runs.
static f32 g_lib_volume = 1;
static bool g_lib_muted;
static u32 g_lib_volume_gen = 0xffffffff; // the player gen it was last given to
static u32 g_lib_sound_gen = 0xffffffff;  // the player gen its tracks were last picked for
static bool g_lib_sound_show;              // `show sound`: the sound menu opens

static TrackKind ed_track_kind(const char *title, i32 slot); // editor.cpp

// What plays for the ticked tracks in `mask` (one bit per audio track, 0 = all ticked). The file's own tracks come
// after its Mix when it has one: with all of them ticked the Mix plays (-1), else the ticked ones go in `out` and
// their count comes back.
static i32 lib_sound_plan(bool has_mix, i32 tracks, u32 mask, i32 *out)
{
    i32 n = 0;
    bool all = true;
    for (i32 t = has_mix ? 1 : 0; t < tracks && t < MEDIA_MAX_TRACKS; t++) {
        if (!mask || mask >> t & 1) out[n++] = t;
        else all = false;
    }
    return has_mix && all ? -1 : n;
}

// A saved track's picture: a game's art, else the icon of the app it was saved from (found by the name it was
// saved under), a Discord person's being Discord's. 0 when there's none, for the kind's symbol.
static ID3D11ShaderResourceView *lib_track_icon(const char *title, TrackKind kind)
{
    if (kind == KIND_MIC || kind == KIND_MIX) return 0;
    char base[128];
    snprintf(base, sizeof base, "%s", title);
    size_t n = strlen(base);
    if (kind != KIND_APP && n >= 7) base[n - 7] = 0; // " (game)", " (chat)"
    if (kind == KIND_GAME)
        if (ID3D11ShaderResourceView *srv = game_db_icon(base)) return srv;
    const Config &c = g_app.config;
    for (i32 i = 0; i < c.app_count; i++) {
        char dn[128];
        display_name_of(&c.apps[i], c.apps[i].exe.s, dn, sizeof dn);
        if (strcmp(dn, base)) continue;
        return app_icon(!strncmp(c.apps[i].exe.s, "discord-", 8) ? "discord.exe" : c.apps[i].exe.s);
    }
    return 0;
}

// A row of the sound menu: a tick box, the track's icon and its name. True when clicked; the menu stays open.
static bool lib_track_row(i32 t, const char *title, bool ticked, f32 w)
{
    f32 h = kit_px(34), box = kit_px(16), is = kit_px(20);
    ImVec2 a = ImGui::GetCursorScreenPos();
    ImGui::PushID(t);
    bool clicked = ImGui::Selectable("##track", false, ImGuiSelectableFlags_NoAutoClosePopups, ImVec2(w, h));
    bool hot = ImGui::IsItemHovered();
    ImGui::PopID();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - ImGui::GetStyle().ItemSpacing.y + kit_px(2)); // rows 2 apart
    if (clicked) dev_click("track", title);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 b0(a.x + kit_px(10), floorf(a.y + (h - box) / 2)), b1(b0.x + box, b0.y + box);
    if (ticked) {
        dl->AddRectFilled(b0, b1, col32(hot ? COL_ACCENT_HOVER : COL_ACCENT), kit_px(4));
        ImVec2 cs = g_font_icon->CalcTextSizeA(box * 0.6f, FLT_MAX, 0, ICON_CHECK);
        dl->AddText(g_font_icon, box * 0.6f, ImVec2(floorf(b0.x + (box - cs.x) / 2), floorf(b0.y + (box - cs.y) / 2)),
                    col32(COL_ACCENT_INK), ICON_CHECK);
    } else {
        dl->AddRectFilled(b0, b1, col32(COL_BG), kit_px(4));
        dl->AddRect(b0, b1, col32(hot ? COL_DIM : COL_FAINT), kit_px(4), 0, kit_px(1.5f));
    }
    TrackKind kind = ed_track_kind(title, t);
    f32 ix = b1.x + kit_px(12), iy = floorf(a.y + (h - is) / 2);
    if (ID3D11ShaderResourceView *srv = lib_track_icon(title, kind)) {
        dl->AddImageRounded((ImTextureID)srv, ImVec2(ix, iy), ImVec2(ix + is, iy + is), ImVec2(0, 0), ImVec2(1, 1),
                            IM_COL32_WHITE, kit_px(4));
    } else {
        const char *g = KIND_LOOK[kind].icon;
        ImVec2 gs = g_font_icon->CalcTextSizeA(g_font_icon->FontSize, FLT_MAX, 0, g);
        dl->AddText(g_font_icon, g_font_icon->FontSize, ImVec2(floorf(ix + (is - gs.x) / 2), floorf(a.y + (h - gs.y) / 2)),
                    col32(KIND_LOOK[kind].rgb), g);
    }
    f32 tx = ix + is + kit_px(10);
    text_clipped(dl, ImVec2(tx, floorf(a.y + (h - ImGui::GetTextLineHeight()) / 2)), a.x + w - kit_px(10) - tx,
                 col32(ticked ? COL_TEXT : COL_DIM), title);
    return clicked;
}

static void lib_player_view()
{
    Library &lib = g_app.library;
    Player &p = lib.player;
    player_update(&p);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    f32 fh = ImGui::GetFrameHeight(), full_w = ImGui::GetContentRegionAvail().x;

    // Header, one block: back, the name over a dim line (game, when, size, then its comment on one line or a faint
    // "Add a comment"; a click opens it whole), and the actions as icons on the right.
    ImVec2 h0 = ImGui::GetCursorScreenPos();
    f32 fs = g_font_small->FontSize, text_h = g_font_big->FontSize + kit_px(3) + fs;
    f32 head_h = fmaxf(fh, text_h), gap = kit_px(2), acts_w = 4 * fh + 3 * gap;
    ImGui::SetCursorScreenPos(ImVec2(h0.x, h0.y + (head_h - fh) / 2));
    bool close = kit_icon_button(ICON_BACK, "##back", KIT_GHOST);
    kit_tip("Back to the library", "Esc");
    f32 tx = h0.x + fh + kit_px(12), tw = h0.x + full_w - acts_w - kit_px(24) - tx;
    f32 ty = h0.y + (head_h - text_h) / 2, my = ty + g_font_big->FontSize + kit_px(3);
    ImGui::PushFont(g_font_big);
    char shown[300];
    lib_shown_name(lib.playing.name, shown, sizeof shown);
    text_clipped(dl, ImVec2(tx, ty), tw, col32(COL_TEXT), shown);
    ImGui::PopFont();
    char when[48], size[24], line[200];
    fmt_when(lib.playing.when_us, when, sizeof when);
    fmt_size(lib.playing.size, size, sizeof size);
    snprintf(line, sizeof line, "%s  -  %s  -  %s", lib.playing.info.game, when, size);
    ImGui::PushFont(g_font_small);
    f32 lw = ImMin(ImGui::CalcTextSize(line).x, tw);
    text_clipped(dl, ImVec2(tx, my), tw, col32(COL_DIM), line);
    {
        const char *cm = lib_comment(lib.playing.key);
        char one[LIB_COMMENT_MAX + 1];
        i32 k = 0;
        for (const char *s = cm; *s && k < LIB_COMMENT_MAX - 2; s++) {
            if (*s == '\n') { one[k++] = ' '; one[k++] = ' '; }
            else one[k++] = *s;
        }
        one[k] = 0;
        f32 cx = tx + lw + kit_px(18), room = tx + tw - cx;
        // Its tags ("#ace #funny"), when it has any: a click opens Tags
        const char *tg = lib_tags(lib.playing.key);
        if (tg[0] && room > kit_px(40)) {
            char hash[LIB_TAGS_MAX * 2];
            i32 k2 = 0;
            hash[k2++] = '#';
            for (const char *s = tg; *s && k2 < (i32)sizeof hash - 3; s++) {
                if (*s == ',') { hash[k2++] = ' '; hash[k2++] = '#'; }
                else hash[k2++] = *s;
            }
            hash[k2] = 0;
            ImVec2 gs = g_font_icon->CalcTextSizeA(fs, FLT_MAX, 0, ICON_TAG);
            f32 gw = ImMin(gs.x + kit_px(6) + ImGui::CalcTextSize(hash).x, room);
            ImVec2 save = ImGui::GetCursorScreenPos();
            ImGui::SetCursorScreenPos(ImVec2(cx, my));
            bool hit = ImGui::InvisibleButton("##player_tags", ImVec2(ImMax(gw, 1.f), fs));
            bool hot = ImGui::IsItemHovered();
            if (hot) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            dl->AddText(g_font_icon, fs, ImVec2(cx, floorf(my + (fs - gs.y) / 2 + kit_px(1))), col32(hot ? COL_TEXT : COL_DIM), ICON_TAG);
            f32 tx2 = cx + gs.x + kit_px(6);
            text_clipped(dl, ImVec2(tx2, my), cx + room - tx2, col32(hot ? COL_ACCENT : COL_TEXT), hash);
            if (hit) lib_tags_open(lib.playing);
            ImGui::SetCursorScreenPos(save);
            cx += gw + kit_px(18);
            room = tx + tw - cx;
        }
        if (room > kit_px(40)) {
            const char *shown_c = one[0] ? one : "Add a comment";
            ImVec2 gs = g_font_icon->CalcTextSizeA(fs, FLT_MAX, 0, ICON_COMMENT);
            f32 cw = ImMin(gs.x + kit_px(6) + ImGui::CalcTextSize(shown_c).x, room);
            ImVec2 save = ImGui::GetCursorScreenPos();
            ImGui::SetCursorScreenPos(ImVec2(cx, my));
            bool hit = ImGui::InvisibleButton("##player_comment", ImVec2(ImMax(cw, 1.f), fs));
            bool hot = ImGui::IsItemHovered();
            if (hot) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            dl->AddText(g_font_icon, fs, ImVec2(cx, floorf(my + (fs - gs.y) / 2 + kit_px(1))), col32(hot ? COL_TEXT : COL_DIM),
                        ICON_COMMENT);
            f32 tx2 = cx + gs.x + kit_px(6);
            text_clipped(dl, ImVec2(tx2, my), cx + room - tx2,
                         col32(one[0] ? (hot ? COL_ACCENT : COL_TEXT) : (hot ? COL_DIM : COL_FAINT)), shown_c);
            if (hit) lib_comment_open(lib.playing);
            ImGui::SetCursorScreenPos(save);
        }
    }
    ImGui::PopFont();
    lib_comment_dialog();
    lib_tags_dialog();
    ImGui::SetCursorScreenPos(ImVec2(h0.x + full_w - acts_w, h0.y + (head_h - fh) / 2));
    // Share does what share_decide picks; the arrow beside it, or a right-click on Share, opens the Share menu
    // (lib_share_menu), under the icons and right-aligned.
    ImVec2 share_at = ImGui::GetCursorScreenPos();
    ClipShareState share_cs = lib_clip_state(lib.playing);
    kit_next_tip(share_ready_tip(share_cs));
    if (kit_icon_button(share_ready_icon(share_cs), "##share", share_ready_kind(share_cs, KIT_GHOST)))
        lib_share_decided(lib.playing);
    bool share_right = ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right);
    ImGui::SameLine(0, gap);
    kit_next_tip("More share options");
    if ((kit_icon_button(ICON_DOWN, "##sharemenu", KIT_GHOST) || share_right || g_lib_share_ask) && !lib_share_refused(lib.playing))
        ImGui::OpenPopup("share_menu");
    g_lib_share_ask = false;
    ImGui::SameLine(0, gap);
    kit_next_tip("Edit");
    bool edit = kit_icon_button(ICON_EDIT, "##edit", KIT_GHOST);
    ImGui::SameLine(0, gap);
    kit_next_tip("Show in folder");
    if (kit_icon_button(ICON_FOLDER, "##folder", KIT_GHOST)) show_in_folder(lib_playable_path(lib.playing).s);
    ImGui::SetNextWindowPos(ImVec2(h0.x + full_w, share_at.y + fh + kit_px(4)), 0, ImVec2(1, 0));
    lib_share_menu("share_menu", lib.playing);
    if (edit) {
        LibItem it = lib.playing;
        library_close_player();
        editor_open(it);
        return;
    }

    // The picture, as big as fits, on black.
    // Under it the controls, then the seek bar along the bottom as wide as the picture's area; fixed heights.
    ImGui::SetCursorScreenPos(ImVec2(h0.x, h0.y + head_h + kit_px(10)));
    f32 ctl_gap = kit_px(10), seek_gap = kit_px(6), seek_h = kit_px(20);
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImVec2 area(full_w, ImGui::GetContentRegionAvail().y - ctl_gap - fh - seek_gap - seek_h);
    if (area.y < 1) area.y = 1;
    dl->AddRectFilled(pos, ImVec2(pos.x + area.x, pos.y + area.y), IM_COL32(0, 0, 0, 255), kit_px(10));
    ImVec2 a, b;
    player_fit(p, pos, area, &a, &b);
    ImGui::InvisibleButton("video", area);
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) player_toggle(&p);
    player_draw(p, a, b);
    if (p.loaded && p.paused && !p.error[0]) lib_play_mark(dl, ImVec2((a.x + b.x) / 2, (a.y + b.y) / 2), kit_px(34));

    // The controls: back, play, forward, the speed and the time on the left; the sound and the volume on the right.
    // Fixed widths, lined up with the picture's area.
    ImVec2 r0(pos.x, pos.y + area.y + ctl_gap);
    f32 pad = 0, y = r0.y;
    ImGui::SetCursorScreenPos(ImVec2(r0.x + pad, y));
    kit_next_tip("Back 10 s", "Shift+Left");
    if (kit_icon_button(ICON_BACK10, "##back10", KIT_GHOST)) player_seek(&p, p.pos - 10);
    ImGui::SameLine(0, kit_px(2));
    if (kit_icon_button(p.paused ? ICON_PLAY : ICON_PAUSE, "##play", KIT_GHOST)) player_toggle(&p);
    kit_tip(p.paused ? "Play" : "Pause", "Space");
    ImGui::SameLine(0, kit_px(2));
    kit_next_tip("Forward 30 s", "Shift+Right");
    if (kit_icon_button(ICON_FWD30, "##fwd30", KIT_GHOST)) player_seek(&p, p.pos + 30);
    // Speed: one chip that steps through the speeds.
    static const f32 rates[] = {0.5f, 1, 1.5f, 2};
    f32 rate = p.rate > 0 ? p.rate : 1;
    char rl[16];
    snprintf(rl, sizeof rl, "%gx##rate", rate);
    ImGui::SameLine(0, kit_px(2));
    bool step_rate = kit_button(rl, KIT_GHOST, ImVec2(kit_px(46), 0));
    kit_tip("Speed", "Shift+. / Shift+,");
    char t1[32], t2[32], tm[80];
    fmt_time(p.pos, t1, sizeof t1);
    fmt_time(p.duration > 0 ? p.duration : lib.playing.info.duration, t2, sizeof t2);
    snprintf(tm, sizeof tm, "%s / %s", t1, t2);
    f32 time_x = r0.x + pad + 3 * fh + kit_px(46) + 3 * kit_px(2) + kit_px(10);
    ImVec2 ts = ImGui::CalcTextSize(tm);
    dl->AddText(ImVec2(time_x, floorf(y + (fh - ts.y) / 2)), col32(COL_DIM), tm);

    const MediaInfo &mi = lib.playing.info;
    i32 tracks = p.audio_count ? p.audio_count : mi.track_count;
    bool has_mix = edit_has_mix(mi) && tracks >= 2;
    i32 first_real = has_mix ? 1 : 0, real = tracks - first_real;
    f32 vol_w = kit_px(96), sound_w = real > 1 ? kit_px(180) : 0;
    f32 right_w = sound_w + (sound_w > 0 ? kit_px(12) : 0) + fh + vol_w;
    // The seek bar, edge to edge under the controls
    ImGui::SetCursorScreenPos(ImVec2(pos.x, y + fh + seek_gap));
    scrub_bar("##seek", &p, full_w, seek_h, 0);
    f32 x = r0.x + full_w - pad - right_w;
    bool tracks_changed = false;
    if (real > 1) { // which of the file's tracks are heard: all of them is its Mix
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        u32 all = 0;
        i32 on = 0, one = -1;
        for (i32 t = first_real; t < tracks && t < MEDIA_MAX_TRACKS; t++) {
            all |= 1u << t;
            if (!p.sound_mask || p.sound_mask >> t & 1) {
                on++;
                one = t;
            }
        }
        char cur[64];
        if (on >= real) snprintf(cur, sizeof cur, "All tracks");
        else if (on > 1) snprintf(cur, sizeof cur, "%d tracks", on);
        else if (one >= 0 && one < mi.track_count) snprintf(cur, sizeof cur, "%s", mi.tracks[one]);
        else snprintf(cur, sizeof cur, "Track %d", one + 1);
        if (lib_pill("##sound", cur, sound_w, on < real) || g_lib_sound_show) ImGui::OpenPopup("sound");
        g_lib_sound_show = false;
        ImGui::SetNextWindowPos(ImVec2(x + sound_w, y - kit_px(6)), 0, ImVec2(1, 1)); // above it, right-aligned
        lib_popup_style_push(8);
        bool sound_open = ImGui::BeginPopup("sound");
        lib_popup_style_pop();
        if (sound_open) {
            for (i32 t = first_real; t < tracks && t < MEDIA_MAX_TRACKS; t++) {
                char name[48];
                if (t < mi.track_count) snprintf(name, sizeof name, "%s", mi.tracks[t]);
                else snprintf(name, sizeof name, "Track %d", t + 1);
                u32 m = p.sound_mask ? p.sound_mask : all;
                if (!lib_track_row(t, name, m >> t & 1, kit_px(240))) continue;
                m ^= 1u << t;
                if (m & all) { // at least one stays on
                    p.sound_mask = (m & all) == all ? 0 : m & all;
                    tracks_changed = true;
                }
            }
            ImGui::EndPopup();
        }
        x += sound_w + kit_px(12);
    }
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    bool vol_changed = false;
    if (kit_icon_button(g_lib_muted || g_lib_volume <= 0 ? ICON_MUTE : ICON_VOLUME, "##mute", KIT_GHOST)) {
        g_lib_muted = !g_lib_muted;
        vol_changed = true;
    }
    kit_tip(g_lib_muted ? "Unmute" : "Mute", "M");
    ImGui::SetCursorScreenPos(ImVec2(x + fh, y));
    if (lib_slider("##volume", &g_lib_volume, 0, 1, vol_w, fh)) {
        g_lib_muted = false;
        vol_changed = true;
    }
    if (p.engine && p.loaded && (tracks_changed || g_lib_sound_gen != p.gen)) {
        // The Mix or one track plays on the video's engine; several each get their own engine, mixed by Windows.
        i32 list[MEDIA_MAX_TRACKS];
        i32 n = lib_sound_plan(has_mix, p.audio_count, p.sound_mask, list);
        if (n <= 1) {
            player_clear_tracks(&p);
            player_set_sound(&p, n == 1 ? list[0] : 0);
        } else player_set_tracks(&p, list, n);
        g_lib_sound_gen = p.gen;
        vol_changed = true;
    }
    if (p.engine && p.loaded && (vol_changed || g_lib_volume_gen != p.gen)) {
        if (p.track_engine_count) {
            p.engine->SetMuted(true);
            for (i32 i = 0; i < p.track_engine_count; i++)
                player_track_volume(&p, p.track_of_engine[i], g_lib_volume, g_lib_muted);
        } else {
            p.engine->SetVolume(g_lib_volume);
            p.engine->SetMuted(g_lib_muted);
        }
        g_lib_volume_gen = p.gen;
    }
    bool keys = !ImGui::GetIO().WantTextInput && !ImGui::IsPopupOpen((const char *)0, ImGuiPopupFlags_AnyPopupId);
    i32 ri = 0;
    while (ri < 3 && rates[ri] != rate) ri++;
    i32 to = step_rate ? (ri + 1) % 4 : ri; // the chip goes round
    bool shift = ImGui::GetIO().KeyShift;
    if (keys && shift && ImGui::IsKeyPressed(ImGuiKey_Period, false) && ri < 3) to = ri + 1;
    if (keys && shift && ImGui::IsKeyPressed(ImGuiKey_Comma, false) && ri > 0) to = ri - 1;
    if (to != ri) player_set_rate(&p, rates[to]);

    if (!ImGui::GetIO().WantTextInput && !ImGui::IsPopupOpen((const char *)0, ImGuiPopupFlags_AnyPopupId)) {
        if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) player_toggle(&p);
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) player_seek(&p, p.pos - (ImGui::GetIO().KeyShift ? 10 : 5));
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) player_seek(&p, p.pos + (ImGui::GetIO().KeyShift ? 30 : 5));
        if (ImGui::IsKeyPressed(ImGuiKey_M, false)) {
            g_lib_muted = !g_lib_muted;
            g_lib_volume_gen--; // applied next frame
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) close = true;
    }
    if (close) library_close_player();
}

static void lib_set_view(LibView v)
{
    Library &lib = g_app.library;
    if (lib.view == v) return;
    g_lib_switch_us = now_us();
    g_lib_first_us = 0;
    lib.view = v;
    lib.game[0] = 0;
    lib.folder[0] = 0;
    lib.folder_renaming = 0;
    lib.scroll = lib.scroll_to = 0;
    lib_refilter();
}

// A local day number (local_day's) for a date.
static i64 day_number(i32 y, i32 m, i32 d)
{
    SYSTEMTIME t = {};
    t.wYear = (WORD)y;
    t.wMonth = (WORD)m;
    t.wDay = (WORD)d;
    FILETIME ft;
    SystemTimeToFileTime(&t, &ft);
    return (i64)((((u64)ft.dwHighDateTime << 32) | ft.dwLowDateTime) / 864000000000ull);
}

// "3 Nov", from a day number.
static void fmt_day_short(i64 day, char *out, i32 cap)
{
    u64 v = (u64)day * 864000000000ull;
    FILETIME ft = {(DWORD)v, (DWORD)(v >> 32)};
    SYSTEMTIME t;
    FileTimeToSystemTime(&ft, &t);
    snprintf(out, cap, "%d %.3s", t.wDay, month_name(t.wMonth));
}

// Custom range: a month calendar. The first click picks the first day, the second the last.
static void lib_calendar()
{
    Library &lib = g_app.library;
    lib_popup_style_push(14);
    bool open = ImGui::BeginPopup("calendar");
    lib_popup_style_pop();
    if (!open) return;
    SYSTEMTIME now;
    i64 today = local_day(now_us(), &now);
    if (lib.cal_month <= 0) lib.cal_month = now.wYear * 12 + now.wMonth - 1;
    i32 y = lib.cal_month / 12, m = lib.cal_month % 12 + 1;
    f32 cell = kit_px(36), w = 7 * cell, fh = ImGui::GetFrameHeight();

    // <  November 2026  >
    ImVec2 a = ImGui::GetCursorScreenPos();
    kit_next_tip("Previous month");
    if (kit_icon_button(ICON_LEFT, "##prevm")) lib.cal_month--;
    char title[32];
    snprintf(title, sizeof title, "%s %d", month_name(m), y);
    ImGui::PushFont(g_font_bold);
    ImVec2 ts = ImGui::CalcTextSize(title);
    ImGui::GetWindowDrawList()->AddText(ImVec2(floorf(a.x + (w - ts.x) / 2), floorf(a.y + (fh - ts.y) / 2)), col32(COL_TEXT), title);
    ImGui::PopFont();
    ImGui::SetCursorScreenPos(ImVec2(a.x + w - fh, a.y));
    kit_next_tip("Next month");
    if (kit_icon_button(ICON_RIGHT, "##nextm")) lib.cal_month++;

    ImDrawList *dl = ImGui::GetWindowDrawList();
    f32 y0 = a.y + fh + kit_px(8);
    static const char *wd[] = {"Mo", "Tu", "We", "Th", "Fr", "Sa", "Su"};
    ImGui::PushFont(g_font_small);
    for (i32 c = 0; c < 7; c++) {
        ImVec2 ws = ImGui::CalcTextSize(wd[c]);
        dl->AddText(ImVec2(floorf(a.x + c * cell + (cell - ws.x) / 2), y0), col32(COL_FAINT), wd[c]);
    }
    ImGui::PopFont();
    y0 += g_font_small->FontSize + kit_px(6);
    i64 first = day_number(y, m, 1);
    i32 ny = m == 12 ? y + 1 : y, nm = m == 12 ? 1 : m + 1;
    i32 days = (i32)(day_number(ny, nm, 1) - first), lead = (i32)(first % 7);
    for (i32 k = 0; k < 42; k++) { // always six weeks, so the popup keeps its size
        i32 d = k - lead + 1;
        ImVec2 p(a.x + (k % 7) * cell, y0 + (k / 7) * cell);
        if (d < 1 || d > days) continue;
        i64 day = first + d - 1;
        ImGui::SetCursorScreenPos(p);
        ImGui::PushID(k);
        bool clicked = ImGui::InvisibleButton("##d", ImVec2(cell, cell));
        bool hot = ImGui::IsItemHovered();
        ImGui::PopID();
        bool set = lib.range == RANGE_CUSTOM || lib.cal_picking;
        bool end = set && (day == lib.range_from || (!lib.cal_picking && day == lib.range_to));
        bool inside = set && !lib.cal_picking && day > lib.range_from && day < lib.range_to;
        ImVec2 c(p.x + cell / 2, p.y + cell / 2);
        if (inside) dl->AddRectFilled(ImVec2(p.x, p.y + kit_px(3)), ImVec2(p.x + cell, p.y + cell - kit_px(3)), col32(COL_ACCENT_SOFT));
        if (end) dl->AddCircleFilled(c, cell / 2 - kit_px(3), col32(COL_ACCENT));
        else if (hot) dl->AddCircleFilled(c, cell / 2 - kit_px(3), col32(COL_HOVER));
        if (day == today && !end) dl->AddCircle(c, cell / 2 - kit_px(3), col32(COL_DIM), 0, kit_px(1));
        char n[4];
        snprintf(n, sizeof n, "%d", d);
        ImVec2 ns = ImGui::CalcTextSize(n);
        dl->AddText(ImVec2(floorf(c.x - ns.x / 2), floorf(c.y - ns.y / 2)), col32(end ? 0xffffff : day > today ? COL_FAINT : COL_TEXT), n);
        if (clicked) {
            if (!lib.cal_picking) {
                lib.range_from = lib.range_to = day;
                lib.cal_picking = true;
            } else {
                if (day < lib.range_from) {
                    lib.range_to = lib.range_from;
                    lib.range_from = day;
                } else lib.range_to = day;
                lib.cal_picking = false;
                lib.range = RANGE_CUSTOM;
                lib.scroll = lib.scroll_to = 0;
                lib_refilter();
                ImGui::CloseCurrentPopup();
            }
        }
    }
    ImGui::SetCursorScreenPos(ImVec2(a.x, y0 + 6 * cell + kit_px(4)));
    ImGui::PushFont(g_font_small);
    kit_dim("%s", lib.cal_picking ? "Now pick the last day" : "Pick the first day");
    ImGui::PopFont();
    ImGui::EndPopup();
}

// ---------------------------------------------------------------------------------------------------------------
// Unsaved sessions: a row of tiles at the top of Sessions that slides sideways when there are more than fit.


static ID3D11ShaderResourceView *timeline_keyframe(i64 t, bool ask); // timeline.cpp

static i32 tl_seg_at(const BufferIdx &b, i64 t);    // timeline.cpp
static i32 tl_seg_after(const BufferIdx &b, i64 t); // timeline.cpp

// Each unsaved session's poster: the first frame of the buffer segment at its middle. The library's workers read it
// with ffmpeg once into thumbs\posters (lib_poster_read), named by the segment's start, so after a relaunch it comes
// from the file without the buffer's index or ffmpeg. The textures of the ones shown lately are kept here. While
// Clips is shown, the first ones are read ahead, so Sessions has them at once. A poster goes, its file too, once no
// unsaved session shows it: its session saved, or its footage pruned from the buffer.
struct UnsavedPoster {
    u64                       key; // its segment's start; 0 = a free slot
    ID3D11ShaderResourceView *srv;
    bool                      pending, failed;
    u32                       used;
};
static UnsavedPoster g_posters[64];
static u32           g_poster_clock;
static u64           g_poster_disk[1024]; // the keys of the files there, sorted
static i32           g_poster_disk_n = -1; // -1 = not listed yet
static u64           g_poster_swept;       // GetTickCount64 of the last sweep

static Path lib_poster_cache() { return path_join(g_app.paths.local_dir, L"thumbs"); }

static int lib_u64_cmp(const void *a, const void *b)
{
    u64 x = *(const u64 *)a, y = *(const u64 *)b;
    return x < y ? -1 : x > y;
}

static void lib_poster_disk_list()
{
    if (g_poster_disk_n >= 0) return;
    g_poster_disk_n = 0;
    Path pattern = path_join(path_join(lib_poster_cache(), L"posters"), L"p-*.jpg");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern.s, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        u64 key = 0;
        if (swscanf(fd.cFileName, L"p-%16llx.jpg", &key) == 1 && key && g_poster_disk_n < COUNT(g_poster_disk))
            g_poster_disk[g_poster_disk_n++] = key;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    qsort(g_poster_disk, g_poster_disk_n, sizeof g_poster_disk[0], lib_u64_cmp);
}

static bool lib_poster_on_disk(u64 key)
{
    lib_poster_disk_list();
    return bsearch(&key, g_poster_disk, g_poster_disk_n, sizeof key, lib_u64_cmp) != 0;
}

static void lib_poster_disk_add(u64 key)
{
    if (lib_poster_on_disk(key) || g_poster_disk_n == COUNT(g_poster_disk)) return;
    i32 i = g_poster_disk_n++;
    while (i > 0 && g_poster_disk[i - 1] > key) {
        g_poster_disk[i] = g_poster_disk[i - 1];
        i--;
    }
    g_poster_disk[i] = key;
}

// The poster's key for a session: from the buffer's index once it's read, else the file nearest before its middle
// (segments are a few seconds long).
static u64 lib_poster_key(const Session &s)
{
    i64 mid = s.start_us + (s.end_us - s.start_us) / 2;
    const Timeline &tl = g_app.timeline;
    if (tl.idx && tl.loaded) {
        const BufferIdx &b = *tl.idx;
        i32 k = tl_seg_at(b, mid);
        if (k < 0) k = tl_seg_after(b, mid);
        return k < b.seg_count ? (u64)b.segs[k].start_us : 0;
    }
    lib_poster_disk_list();
    for (i32 i = g_poster_disk_n - 1; i >= 0; i--) {
        if (g_poster_disk[i] > (u64)mid) continue;
        return (u64)mid - g_poster_disk[i] < 10000000ull ? g_poster_disk[i] : 0;
    }
    return 0;
}

// A session's poster, null until it's read. It's asked for when it isn't there: `soon` puts it first in the queue.
static ID3D11ShaderResourceView *lib_poster(const Session &s, bool soon)
{
    u64 key = lib_poster_key(s);
    if (!key) return 0;
    UnsavedPoster *slot = 0;
    for (UnsavedPoster &p : g_posters) {
        if (p.key == key) {
            p.used = ++g_poster_clock;
            return p.srv;
        }
        if (p.pending) continue;
        if (!slot || (slot->key && (!p.key || p.used < slot->used))) slot = &p;
    }
    if (!slot) return 0;
    LibTask t = {};
    t.kind = LT_POSTER;
    t.key = key;
    if (!lib_poster_on_disk(key)) {
        const Timeline &tl = g_app.timeline;
        if (!tl.idx || !tl.loaded) return 0; // which file it's in waits for the index
        const BufferIdx &b = *tl.idx;
        i32 k = tl_seg_at(b, (i64)key);
        if (k < 0) return 0;
        const VideoSeg &seg = b.segs[k];
        wchar_t run[64], name[32];
        utf8_to_wide(b.runs[seg.run].name, run, COUNT(run));
        utf8_to_wide(seg.name, name, COUNT(name));
        t.path = path_join(path_join(path_join(g_app.config.buffer_dir, L"video"), run), name);
    }
    if (slot->srv) slot->srv->Release();
    *slot = {};
    slot->key = key;
    slot->pending = true;
    slot->used = ++g_poster_clock;
    lib_push(t, soon);
    return 0;
}

static void lib_poster_landed(const LibResult &r)
{
    for (UnsavedPoster &p : g_posters) {
        if (p.key != r.key || !p.pending) continue;
        p.pending = false;
        if (!r.failed && g_app.ui.shown) p.srv = gpu_texture(r.pixels, r.w, r.h);
        p.failed = !p.srv;
        if (!g_app.ui.shown) p = {}; // asked again when it's shown
        break;
    }
    if (!r.failed) lib_poster_disk_add(r.key);
}

// Every few seconds: the posters no tile shows go from memory, and those of no unsaved session from disk. Only once
// the sessions and the buffer's index are read, so a poster isn't lost for want of knowing.
static void lib_posters_sweep()
{
    const Sessions &ss = g_app.sessions;
    const Timeline &tl = g_app.timeline;
    if (!ss.loaded || !tl.idx || !tl.loaded || !tl.idx->seg_count) return;
    u64 now = GetTickCount64();
    if (g_poster_swept && now - g_poster_swept < 5000) return;
    g_poster_swept = now;
    static u64 keep[COUNT(ss.rows)], shown[COUNT(ss.rows)];
    i32 kn = 0, sn = 0;
    for (i32 r = 0; r < ss.row_count; r++) {
        const Session *s = session_find(ss.rows[r]);
        if (!s) continue;
        shown[sn++] = lib_poster_key(*s);
        if (s->state != SES_SAVED) keep[kn++] = shown[sn - 1];
    }
    qsort(keep, kn, sizeof keep[0], lib_u64_cmp);
    qsort(shown, sn, sizeof shown[0], lib_u64_cmp);
    for (UnsavedPoster &p : g_posters) {
        if (!p.key || p.pending || bsearch(&p.key, shown, sn, sizeof p.key, lib_u64_cmp)) continue;
        if (p.srv) p.srv->Release();
        p = {};
    }
    lib_poster_disk_list();
    Path cache = lib_poster_cache();
    i32 w = 0, gone = 0;
    for (i32 i = 0; i < g_poster_disk_n; i++) {
        u64 key = g_poster_disk[i];
        if (bsearch(&key, keep, kn, sizeof key, lib_u64_cmp)) {
            g_poster_disk[w++] = key;
            continue;
        }
        DeleteFileW(lib_poster_file(cache, key).s);
        gone++;
    }
    g_poster_disk_n = w;
    if (gone) DEV_LOG("library: %d session posters no longer shown went from disk", gone);
}

// While Clips is shown, the posters of the first unsaved sessions are read ahead.
static void lib_posters_ahead()
{
    sessions_unsaved_take();
    lib_posters_sweep();
    const Sessions &ss = g_app.sessions;
    for (i32 r = 0; r < ss.row_count && r < 12; r++) {
        const Session *s = session_find(ss.rows[r]);
        if (s && s->state != SES_SAVED) lib_poster(*s, false);
    }
}
static void timeline_review(i64 from_us, i64 to_us, const char *name);  // timeline.cpp

// An unsaved session, drawn like a clip's card: its picture from the buffer (pointing scrubs through it, a click
// looks at it on the timeline), then its icon, the file name it gets when saved, and its length and times. Save sits
// on the right of the caption and asks for a title first. `ask` lets this one read a frame it hasn't got: the tile
// under the mouse, as it scrubs. Otherwise it shows its poster.
// New sessions: those not seen yet (Session.seen), and those first seen on this visit to the page, which keep
// their dot until the page is left.
static u32 g_unsaved_new[COUNT(((Sessions *)0)->rows)];
static i32 g_unsaved_new_n, g_unsaved_new_frame = -100;
static bool lib_unsaved_new(const Session &s)
{
    if (!s.seen) return true;
    for (i32 i = 0; i < g_unsaved_new_n; i++)
        if (g_unsaved_new[i] == s.id) return true;
    return false;
}

static void lib_unsaved_tile(u32 id, ImVec2 a, f32 w, f32 thumb_h, bool ask)
{
    Sessions &ss = g_app.sessions;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    f32 fh = ImGui::GetFrameHeight(), round = kit_px(4);
    Session *s = session_find(id);
    ImGui::PushID((i32)id);
    ImVec2 t0 = a, t1(a.x + w, a.y + thumb_h);
    dl->AddRectFilled(t0, t1, col32(COL_PANEL), round);
    if (!s) {
        dl->AddText(ImVec2(a.x + kit_px(14), floorf(a.y + (thumb_h - ImGui::GetFontSize()) / 2)), col32(COL_FAINT),
                    "Gone from the buffer");
        ImGui::PopID();
        return;
    }
    bool saved = s->state == SES_SAVED, saving = s->state == SES_SAVING;

    // The picture
    ImGui::SetCursorScreenPos(t0);
    ImGui::InvisibleButton("pic", ImVec2(w, thumb_h));
    bool hover = ImGui::IsItemHovered();
    f32 frac = 0.5f;
    if (hover) {
        frac = (ImGui::GetIO().MousePos.x - t0.x) / w;
        frac = frac < 0 ? 0 : frac > 1 ? 1 : frac;
    }
    i64 len = s->end_us - s->start_us;
    i64 t = s->start_us + (i64)(frac * (f64)len);
    // Scrubbing keeps the last frame it showed until the next one is read; the poster before there's one
    static u32 shown_id;
    static i64 shown_t;
    ID3D11ShaderResourceView *pic = hover ? timeline_keyframe(t, ask) : 0;
    if (pic) {
        shown_id = id;
        shown_t = t;
    } else if (hover && shown_id == id) pic = timeline_keyframe(shown_t, false);
    if (!pic) pic = lib_poster(*s, true);
    if (!pic) g_lib_missing++;
    if (pic) dl->AddImageRounded((ImTextureID)pic, t0, t1, ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, round);
    else {
        i32 cw = 0, ch = 0;
        ID3D11ShaderResourceView *blur = 0;
        if (game_db_cover(s->name, &cw, &ch, &blur) && blur && cw > 0 && ch > 0) {
            f32 want = w / thumb_h, have = (f32)cw / ch, u = 0, v = 0;
            if (have > want) u = (1 - want / have) / 2;
            else v = (1 - have / want) / 2;
            dl->AddImageRounded((ImTextureID)blur, t0, t1, ImVec2(u, v), ImVec2(1 - u, 1 - v), IM_COL32_WHITE, round);
        }
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !saved) {
        timeline_review(s->start_us, s->end_us, s->name);
        ImGui::PopID();
        return;
    }
    char text[64], len_s[32];
    fmt_length(len, len_s, sizeof len_s);
    f32 in = kit_px(10);
    if (hover) { // where it is: a bar along the bottom edge and the clock time
        f32 bh = kit_px(4), by = t1.y - bh;
        dl->PushClipRect(ImVec2(t0.x, by), t1, true);
        dl->AddRectFilled(t0, t1, IM_COL32(255, 255, 255, 70), round);
        dl->PopClipRect();
        dl->PushClipRect(ImVec2(t0.x, by), ImVec2(t0.x + frac * w, t1.y), true);
        dl->AddRectFilled(t0, t1, col32(COL_ACCENT), round);
        dl->PopClipRect();
        fmt_clock(t, text, sizeof text);
        badge(dl, ImVec2(t1.x - in, by - kit_px(6)), true, true, text);
    } else badge(dl, ImVec2(t1.x - in, t1.y - in), true, true, len_s);
    // What happens to it: time left in the buffer, saving, saved
    if (saved) str_set(text, "Saved");
    else if (s->state == SES_ASKING) str_set(text, "Asking");
    else if (saving) {
        const SaveJobView *v = save_view(s->job);
        if (v && v->state == SAVE_RUNNING) snprintf(text, sizeof text, "Saving %d%%", (int)(v->progress * 100));
        else str_set(text, "Waiting to save");
        f32 p = v ? v->progress : 0;
        dl->PushClipRect(ImVec2(t0.x, t1.y - kit_px(4)), ImVec2(t0.x + p * w, t1.y), true);
        dl->AddRectFilled(t0, t1, col32(COL_ACCENT), round);
        dl->PopClipRect();
    } else session_time_left(*s, text, sizeof text);
    badge(dl, ImVec2(t0.x + in, t0.y + in), false, false, text);
    if (lib_unsaved_new(*s)) { // new: a white dot in the top right
        f32 r = kit_px(5);
        ImVec2 c(t1.x - in - r, t0.y + in + r);
        dl->AddCircleFilled(c, r + kit_px(2), IM_COL32(0, 0, 0, 110));
        dl->AddCircleFilled(c, r, col32(COL_WHITE));
    }

    // The caption: icon, then the file name it gets and a dim line; Save on the right
    f32 y = t1.y + kit_px(8), isz = g_font_bold->FontSize + kit_px(2) + g_font_small->FontSize;
    ID3D11ShaderResourceView *icon = game_db_icon(s->name);
    if (!icon && s->exe[0]) icon = app_icon(s->exe);
    if (icon) dl->AddImageRounded((ImTextureID)icon, ImVec2(a.x, y), ImVec2(a.x + isz, y + isz), ImVec2(0, 0), ImVec2(1, 1),
                                  IM_COL32_WHITE, kit_px(3));
    else {
        dl->AddRectFilled(ImVec2(a.x, y), ImVec2(a.x + isz, y + isz), col32(COL_PANEL), kit_px(3));
        ImGui::PushFont(g_font_icon);
        ImVec2 gs = ImGui::CalcTextSize(ICON_GAME);
        dl->AddText(ImVec2(floorf(a.x + (isz - gs.x) / 2), floorf(y + (isz - gs.y) / 2)), col32(COL_FAINT), ICON_GAME);
        ImGui::PopFont();
    }
    f32 by = floorf(y + (isz - fh) / 2), bx = a.x + w - fh;
    ImGui::SetCursorScreenPos(ImVec2(bx, by));
    kit_next_tip(saved ? "Saved" : "Save the session");
    if (kit_icon_button(saved ? ICON_CHECK : ICON_SAVE, "##save", saved ? KIT_GHOST : KIT_PRIMARY, !saving && !saved)) {
        ss.name_buf[0] = 0;
        ss.name_focus = true;
        ImGui::OpenPopup("##savename");
    }
    // The save prompt, under the button: a display name for its card (the file keeps its name). Enter saves, with the
    // display name or, left empty, none; Esc or a click away doesn't save.
    f32 pw = kit_px(300);
    ImGui::SetNextWindowPos(ImVec2(ImMax(bx + fh - pw - 2 * kit_px(14), kit_px(8)), by + fh + kit_px(6)));
    lib_popup_style_push(14);
    bool prompt = ImGui::BeginPopup("##savename");
    lib_popup_style_pop();
    if (prompt) {
        kit_label("DISPLAY NAME");
        ImGui::SetNextItemWidth(pw);
        if (ss.name_focus) {
            ImGui::SetKeyboardFocusHere();
            ss.name_focus = false;
        }
        bool enter = ImGui::InputTextWithHint("##dname", "Optional", ss.name_buf, sizeof ss.name_buf,
                                              ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::Dummy(ImVec2(0, kit_px(2)));
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + pw - kit_px(96));
        bool go = kit_button("Save", KIT_PRIMARY, ImVec2(kit_px(96), 0));
        if (enter || go) {
            for (char *c = ss.name_buf; *c; c++)
                if (*c == '\t' || *c == '|') *c = ' ';
            str_set(s->display_name, trim(ss.name_buf));
            ss.dirty = true;
            log_line("session %u: saving%s", s->id, s->display_name[0] ? " with a display name" : "");
            session_save(s);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    f32 tx = a.x + isz + kit_px(10), tw = bx - kit_px(8) - tx;
    char file[300];
    save_file_name(s->exe, s->name, s->start_us, file, sizeof file); // as session_save names it
    ImGui::PushFont(g_font_bold);
    char shown[300];
    lib_shown_name(file, shown, sizeof shown);
    text_clipped(dl, ImVec2(tx, y), tw, col32(COL_TEXT), shown);
    ImGui::PopFont();
    y += g_font_bold->FontSize + kit_px(2);
    char from[32], to[32], line[200];
    fmt_clock(s->start_us, from, sizeof from);
    fmt_clock(s->end_us, to, sizeof to);
    snprintf(line, sizeof line, "%s  -  %s - %s", len_s, from, to);
    ImGui::PushFont(g_font_small);
    text_clipped(dl, ImVec2(tx, y), tw, col32(COL_DIM), line);
    ImGui::PopFont();
    ImGui::PopID();
}

// Whether an unsaved session passes the grid's filters, judged as the card it becomes when saved: its game, its
// end in the date range, the search. None is in a folder.
static bool lib_unsaved_shown(u32 id)
{
    const Library &lib = g_app.library;
    if (g_app.config.side_folders && lib.folder[0]) return false;
    const Session *s = session_find(id);
    if (!s) return true; // "Gone from the buffer" stays until the row is next taken
    if (!lib_in_range(s->end_us)) return false;
    if (!lib.game[0] && !lib.search[0]) return true;
    char file[300], game[128];
    save_file_name(s->exe, s->name, s->start_us, file, sizeof file);
    // The game its file is tagged with: the exe's shown name; a session named by hand, what its file name says
    if (s->exe[0]) audio_display_name(g_app.config, s->exe, game, sizeof game);
    else game_from_name(g_app.config.name_template, file, game, sizeof game);
    if (!g_app.config.side_folders && lib.game[0] && strcmp(lib.game, game)) return false;
    return lib_name_matches(file, lib.search) || lib_matches(game, lib.search);
}

// The row, drawn from the cursor; returns false (and draws nothing) when none pass the grid's filters. A click on
// the heading (a chevron, then the words) folds it to the heading alone (kept in the settings). The tiles slide
// sideways (kit_strip_*): the arrows on the right a row's width at a time, Shift + wheel or a sideways wheel over
// them, or the bar under them. lib_unsaved_over says the mouse is on the tiles, where the plain wheel scrolls the grid.
static bool lib_unsaved_over;
static bool lib_unsaved_row(f32 full_w)
{
    Sessions &ss = g_app.sessions;
    ImGuiIO &io = ImGui::GetIO();
    sessions_unsaved_take();
    lib_posters_sweep();
    static u32 rows[COUNT(ss.rows)];
    i32 row_count = 0;
    for (i32 r = 0; r < ss.row_count; r++)
        if (lib_unsaved_shown(ss.rows[r])) rows[row_count++] = ss.rows[r];
    if (row_count == 0) return false;
    i32 frame = ImGui::GetFrameCount();
    if (g_unsaved_new_frame != frame - 1) g_unsaved_new_n = 0; // a new visit
    g_unsaved_new_frame = frame;
    bool any_new = false;
    for (i32 r = 0; r < row_count && !any_new; r++) {
        const Session *s = session_find(rows[r]);
        any_new = s && lib_unsaved_new(*s);
    }
    f32 gap = kit_px(14), fh = ImGui::GetFrameHeight();
    i32 per = (i32)((full_w + gap) / (kit_px(280) + gap));
    if (per < 1) per = 1;
    f32 tile_w = floorf((full_w - (per - 1) * gap) / per), thumb_h = floorf(tile_w * 9 / 16), step = tile_w + gap;
    f32 max_scroll = row_count > per ? (row_count - per) * step : 0;

    ImVec2 a = ImGui::GetCursorScreenPos();
    bool &folded = g_app.config.unsaved_collapsed;
    char head[48];
    snprintf(head, sizeof head, "UNSAVED SESSIONS  %d", row_count);
    {
        ImFont *tf = g_font_small;
        const char *ic = folded ? ICON_RIGHT : ICON_DOWN;
        f32 isz = floorf(tf->FontSize * 0.8f), ty = floorf(a.y + (fh - tf->FontSize) / 2);
        ImVec2 is = g_font_icon->CalcTextSizeA(isz, FLT_MAX, 0, ic);
        f32 iw = kit_px(12), tw = tf->CalcTextSizeA(tf->FontSize, FLT_MAX, 0, head).x;
        ImGui::SetCursorScreenPos(a);
        bool hit = ImGui::InvisibleButton("##unsaved_fold", ImVec2(iw + kit_px(4) + tw, fh));
        bool hot = ImGui::IsItemHovered();
        if (hot) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        u32 ink = col32(hot ? COL_DIM : COL_FAINT);
        ImDrawList *hdl = ImGui::GetWindowDrawList();
        hdl->AddText(g_font_icon, isz, ImVec2(floorf(a.x + (iw - is.x) / 2), floorf(a.y + (fh - is.y) / 2)), ink, ic);
        hdl->AddText(tf, tf->FontSize, ImVec2(a.x + iw + kit_px(4), ty), ink, head);
        if (any_new) { // "new" in a white pill after the count, past the fold's click area
            ImFont *nf = tf;
            ImVec2 ns = nf->CalcTextSizeA(nf->FontSize, FLT_MAX, 0, "new");
            f32 px = kit_px(7), ph = floorf(nf->FontSize + kit_px(4)), nx = a.x + iw + kit_px(4) + tw + kit_px(8);
            f32 py = floorf(a.y + (fh - ph) / 2);
            hdl->AddRectFilled(ImVec2(nx, py), ImVec2(nx + ns.x + 2 * px, py + ph), col32(COL_WHITE), ph / 2);
            hdl->AddText(nf, nf->FontSize, ImVec2(nx + px, floorf(py + (ph - nf->FontSize) / 2)), col32(COL_BG), "new");
        }
        if (hit) {
            folded = !folded;
            config_save(g_app.config, g_app.paths);
            log_line("library: unsaved sessions %s", folded ? "folded" : "shown");
        }
    }
    if (folded) {
        ImGui::SetCursorScreenPos(ImVec2(a.x, a.y + fh + kit_px(10)));
        return true;
    }
    KitStrip &st = ss.strip;
    if (max_scroll > 0) {
        ImGui::SetCursorScreenPos(ImVec2(a.x + full_w - 2 * fh - kit_px(2), a.y));
        kit_next_tip("Earlier ones");
        if (kit_icon_button(ICON_LEFT, "##unsaved_back", KIT_GHOST, st.to > 0.5f)) kit_strip_go(st, st.to - per * step, max_scroll);
        ImGui::SameLine(0, kit_px(2));
        kit_next_tip("Later ones");
        if (kit_icon_button(ICON_RIGHT, "##unsaved_on", KIT_GHOST, st.to < max_scroll - 0.5f))
            kit_strip_go(st, st.to + per * step, max_scroll);
    }
    f32 y = a.y + fh + kit_px(6), row_h = thumb_h + lib_meta_h();
    ImVec2 m = io.MousePos;
    bool over = m.x >= a.x && m.x < a.x + full_w && m.y >= y && m.y < y + row_h + kit_strip_bar_h() &&
                !ImGui::IsPopupOpen((const char *)0, ImGuiPopupFlags_AnyPopupId);
    lib_unsaved_over = over;
    kit_strip_wheel(st, over, step * 0.6f, max_scroll);
    kit_strip_bar("##unsaved_bar", st, ImVec2(a.x, y + row_h), full_w, max_scroll);
    kit_strip_glide(st, max_scroll);

    // The tile under the mouse reads the frames it scrubs through.
    i32 first = (i32)(st.pos / step), hovered = -1;
    for (i32 r = first; r < row_count && r <= first + per; r++) {
        f32 x = a.x + r * step - st.pos;
        if (m.x >= ImMax(x, a.x) && m.x < ImMin(x + tile_w, a.x + full_w) && m.y >= y && m.y < y + thumb_h) hovered = r;
    }
    ImGui::PushClipRect(ImVec2(a.x, y), ImVec2(a.x + full_w, y + row_h), true);
    // A tile wholly in view on a window that's showing is seen: its dot stays until the page is left
    bool showing = g_app.ui.shown && !IsIconic(g_app.ui.hwnd);
    for (i32 r = first; r < row_count && r <= first + per; r++) {
        f32 x = floorf(a.x + r * step - st.pos);
        lib_unsaved_tile(rows[r], ImVec2(x, y), tile_w, thumb_h, hovered == r);
        Session *s = session_find(rows[r]);
        if (showing && s && !s->seen && x >= a.x - 1 && x + tile_w <= a.x + full_w + 1) {
            if (g_unsaved_new_n < COUNT(g_unsaved_new)) g_unsaved_new[g_unsaved_new_n++] = s->id;
            session_seen(s->id);
        }
    }
    ImGui::PopClipRect();
    ImGui::SetCursorScreenPos(ImVec2(a.x, y + row_h + kit_strip_bar_h() + kit_px(8)));
    return true;
}

// The Games / Folders switch at the top of the list on the left (lib_switch), the same size as the Clips / Sessions
// one. Collapsed, it's one icon (what the list shows) where the rows' icons are; a click shows the other. True when
// it's flipped.
static bool lib_side_toggle(f32 w, bool collapsed)
{
    bool folders = g_app.config.side_folders, flip = false;
    f32 h = ImGui::GetFrameHeight(), is = kit_px(20);
    ImVec2 a = ImGui::GetCursorScreenPos();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    if (collapsed) {
        flip = ImGui::InvisibleButton("##sidemode", ImVec2(w, h));
        bool hot = ImGui::IsItemHovered();
        kit_tip(folders ? "Show the games" : "Show the folders");
        dl->AddRectFilled(a, ImVec2(a.x + w, a.y + h), col32(hot ? COL_HOVER : COL_PANEL2),
                          ImGui::GetStyle().FrameRounding);
        const char *ic = folders ? ICON_FOLDER : ICON_GAME;
        ImGui::PushFont(g_font_icon);
        ImVec2 gs = ImGui::CalcTextSize(ic);
        dl->AddText(ImVec2(floorf(a.x + kit_px(8) + (is - gs.x) / 2), floorf(a.y + (h - gs.y) / 2)), col32(hot ? COL_TEXT : COL_DIM), ic);
        ImGui::PopFont();
    } else {
        static const char *const sides[] = {ICON_GAME "  Games", ICON_FOLDER "  Folders"};
        flip = lib_switch("##sidemode", sides, w, h, folders, COL_PANEL2, COL_HOVER, COL_OFF);
        kit_tip(folders ? "Show games" : "Show folders");
    }
    ImGui::SetCursorScreenPos(ImVec2(a.x, a.y + h + kit_px(6)));
    return flip;
}

// Starts editing folder i's name in the list (showing the folders, not collapsed).
static void lib_folder_rename_start(i32 i)
{
    Library &lib = g_app.library;
    Config &c = g_app.config;
    if (!c.side_folders || c.games_collapsed) {
        c.side_folders = true;
        c.games_collapsed = false;
        config_save(c, g_app.paths);
        lib_refilter();
    }
    lib.folder_renaming = i + 1;
    str_set(lib.folder_buf, lib.folders[i].name);
    lib.folder_focus = true;
}

// A folder's row while its name is edited: the icon, then the field. Enter or a click away keeps the name; Esc
// leaves it as it was.
static void lib_folder_name_field(i32 i, f32 w)
{
    Library &lib = g_app.library;
    f32 h = kit_px(30), is = kit_px(20);
    ImVec2 a = ImGui::GetCursorScreenPos();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(a.x + kit_px(2), a.y + kit_px(2)), ImVec2(a.x + w - kit_px(2), a.y + h - kit_px(2)), col32(COL_HOVER), kit_px(6));
    ImGui::PushFont(g_font_icon);
    ImVec2 gs = ImGui::CalcTextSize(ICON_FOLDER);
    dl->AddText(ImVec2(floorf(a.x + kit_px(8) + (is - gs.x) / 2), floorf(a.y + (h - gs.y) / 2)), col32(COL_DIM), ICON_FOLDER);
    ImGui::PopFont();
    f32 tx = a.x + kit_px(8) + is + kit_px(6), fy = kit_px(3);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(kit_px(4), fy));
    ImGui::SetCursorScreenPos(ImVec2(tx, floorf(a.y + (h - ImGui::GetFontSize() - 2 * fy) / 2)));
    ImGui::SetNextItemWidth(a.x + w - kit_px(6) - tx);
    if (lib.folder_focus) {
        ImGui::SetKeyboardFocusHere();
        lib.folder_focus = false;
    }
    bool enter = ImGui::InputText("##foldername", lib.folder_buf, sizeof lib.folder_buf,
                                  ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
    bool gone = ImGui::IsItemDeactivated();
    ImGui::PopStyleVar();
    if (enter || gone) {
        lib.folder_renaming = 0;
        lib_folder_rename(i, lib.folder_buf);
    }
    ImGui::SetCursorScreenPos(ImVec2(a.x, a.y + h + kit_px(2)));
}

// The folders of this view: all its cards, then each folder with how many it has (the favourites first), then New
// folder. A card dropped on a folder goes into it; right-click favourites, renames or deletes one.
static void lib_folder_list(f32 w, bool collapsed)
{
    Library &lib = g_app.library;
    i32 pick = -2;
    bool menu = false, add = false;
    ImGui::PushID("folders");
    if (lib_game_item(0, lib.view == LIB_CLIPS ? "All clips" : "All sessions", lib.range_count, !lib.folder[0], w, collapsed))
        pick = -1;
    const char *list = lib_view_word(lib.view);
    i32 order[LIB_MAX_FOLDERS];
    i32 shown = lib_favourite_order(list, lib.folder_count, [&](i32 i) {
        return lib.folders[i].view == lib.view ? (const char *)lib.folders[i].name : (const char *)0;
    }, order);
    for (i32 o = 0; o < shown; o++) {
        i32 i = order[o];
        LibFolder &fo = lib.folders[i];
        ImGui::PushID(i);
        if (lib.folder_renaming == i + 1 && !collapsed) lib_folder_name_field(i, w);
        else {
            if (lib_game_item(0, fo.name, lib.folder_items[i], lib.folder[0] && !strcmp(lib.folder, fo.name), w, collapsed,
                              ICON_FOLDER, lib_favourite_find(list, fo.name) >= 0))
                pick = i;
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                lib.folder_target = i;
                menu = true;
            }
            if (ImGui::BeginDragDropTarget()) {
                const ImGuiPayload *pl = ImGui::AcceptDragDropPayload(
                    "LIB_CARD", ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect);
                if (pl) {
                    ImVec2 r0 = ImGui::GetItemRectMin(), r1 = ImGui::GetItemRectMax();
                    ImGui::GetWindowDrawList()->AddRect(ImVec2(r0.x + kit_px(2), r0.y + kit_px(2)), ImVec2(r1.x - kit_px(2), r1.y - kit_px(2)),
                                                        col32(COL_ACCENT), kit_px(6), 0, kit_px(1.5f));
                    if (pl->IsDelivery()) {
                        u64 key;
                        memcpy(&key, pl->Data, sizeof key);
                        if (lib_selected(key) && lib.selected_count > 1) lib_sel_folder_put(i, true);
                        else for (i32 k = 0; k < lib.item_count; k++)
                            if (lib.items[k].key == key) {
                                lib_folder_put(lib.items[k], i, true);
                                break;
                            }
                    }
                }
                ImGui::EndDragDropTarget();
            }
        }
        ImGui::PopID();
    }
    if (lib_game_item(0, "New folder", -1, false, w, collapsed, ICON_ADD)) add = true;
    if (add) {
        i32 i = lib_folder_new("New folder");
        if (i >= 0) lib_folder_rename_start(i);
    }
    if (menu) ImGui::OpenPopup("folder_menu");
    bool rename = false, del = false, fav_flip = false;
    bool favourite = lib.folder_target >= 0 && lib.folder_target < lib.folder_count &&
                  lib_favourite_find(list, lib.folders[lib.folder_target].name) >= 0;
    lib_popup_style_push(6);
    if (ImGui::BeginPopup("folder_menu")) {
        if (lib_menu_item(favourite ? ICON_STAR : ICON_STAR_FILL, favourite ? "Unfavourite" : "Favourite")) fav_flip = true;
        if (lib_menu_item(ICON_RENAME, "Rename")) rename = true;
        if (lib_menu_item(ICON_DELETE, "Delete...", true)) del = true;
        ImGui::EndPopup();
    }
    lib_popup_style_pop();
    if (fav_flip) lib_favourite_set(list, lib.folders[lib.folder_target].name, !favourite);
    if (rename) lib_folder_rename_start(lib.folder_target);
    // Deleting a folder: unticked, only the folder goes; ticked, everything in it goes to the Recycle Bin as a card's
    // Delete sends it, even what is in other folders too.
    static bool with_items;
    if (g_show_dialog >= 2) {
        for (i32 i = lib.folder_count - 1; i >= 0; i--)
            if (lib.folders[i].view == lib.view) lib.folder_target = i;
        del = lib.folder_target >= 0 && lib.folder_target < lib.folder_count;
    }
    if (del) {
        with_items = g_show_dialog == 3;
        ImGui::OpenPopup("Delete folder");
    }
    if (g_show_dialog >= 2) g_show_dialog = 0;
    if (lib_dialog_begin("Delete folder")) {
        i32 fi = lib.folder_target;
        u64 bit = 1ull << fi;
        i32 count = 0, shared = 0;
        for (i32 k = 0; k < lib.item_count; k++) {
            if (lib.items[k].kind != lib.view) continue;
            u64 m = lib_folder_mask(lib.items[k].key);
            if (!(m & bit)) continue;
            count++;
            if (m & ~bit) shared++;
        }
        const char *noun = lib.view == LIB_CLIPS ? (count == 1 ? "clip" : "clips") : (count == 1 ? "session" : "sessions");
        char box[64], note[96] = "";
        snprintf(box, sizeof box, "Also delete the %s in it", lib.view == LIB_CLIPS ? "clips" : "sessions");
        if (shared)
            snprintf(note, sizeof note, shared == 1 ? "%d of them is also in another folder; it's deleted too."
                                                    : "%d of them are also in other folders; they're deleted too.",
                     shared);
        i32 b = lib_confirm("Delete the folder?", lib.folders[fi].name, shared ? note : 0, box, &with_items, count > 0,
                            ICON_DELETE "  Delete", KIT_DANGER);
        bool go = with_items && count > 0;
        if (b == 1) {
            if (go) {
                i32 gone = 0;
                for (i32 k = 0; k < lib.item_count; k++) {
                    LibItem it = lib.items[k];
                    if (it.kind == lib.view && (lib_folder_mask(it.key) & bit) && lib_recycle(it)) gone++;
                }
                if (gone == count) lib_message(false, "%s and %d %s went to the Recycle Bin", lib.folders[fi].name, gone, noun);
                else lib_message(true, "%d of %d %s couldn't be deleted", count - gone, count, noun);
                library_rescan();
            }
            lib_folder_delete(fi);
        }
        if (b != 0 || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        lib_dialog_end();
    }
    if (pick != -2) {
        if (pick == -1) lib.folder[0] = 0;
        else str_set(lib.folder, lib.folders[pick].name);
        lib.scroll = lib.scroll_to = 0;
        lib_refilter();
    }
    ImGui::PopID();
}

enum { LIB_PAD_Y = 16 }; // the screen's padding above and below, which the list on the left keeps too

// Another app's folders the newest scan went through, looked at every 2 s while the library shows: a file or folder
// added, renamed or deleted there scans again, so an OBS or ShadowPlay save shows up. A file still being written
// shows with what it holds so far, and in full the next scan.
static void lib_watch_poll()
{
    static HANDLE change[COUNT(g_lib_dirs)];
    static u64 last;
    u64 now = GetTickCount64();
    if (now - last < 2000) return;
    last = now;
    bool fire = false;
    for (i32 i = 0; i < (i32)g_lib_dir_count; i++) {
        if (!change[i]) {
            change[i] = FindFirstChangeNotificationW(g_lib_dirs[i].path.s, FALSE,
                                                     FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME);
            continue;
        }
        if (change[i] == INVALID_HANDLE_VALUE || WaitForSingleObject(change[i], 0) != WAIT_OBJECT_0) continue;
        FindNextChangeNotification(change[i]);
        fire |= g_lib_dirs[i].live;
    }
    if (fire) library_rescan(); // queued behind one that's running
}

static void screen_library()
{
    Library &lib = g_app.library;
    if (!lib.scanned && !lib.scanning) library_rescan();
    lib_watch_poll();
    if (!lib.folders_loaded) {
        lib_folders_load();
        lib_refilter();
    }
    ImDrawList *dl = ImGui::GetWindowDrawList();
    f32 fh = ImGui::GetFrameHeight(), switch_w = kit_px(2 * 104 + 4); // both switches are switch_w by fh
    // The list down the left, docked to the window's left edge from the top bar to the bottom: on the same ground as
    // the top bar, with no line between it and the clips. A Games / Folders switch level with the Clips and
    // Sessions one and coloured like it, then all of them and each game in this view with how many it has, most
    // recent first, or this view's folders. It scrolls on its own (the wheel, no bar) when there are more than fit.
    // The button at its foot, level with the footer, collapses it to a rail of icons, which stay where they were.
    if (!lib.player.open) {
        bool &collapsed = g_app.config.games_collapsed;
        f32 icon_end = kit_px(6 + 8 + 20); // a row's icon ends this far in from the list's left
        f32 side_w = collapsed ? icon_end + kit_px(14) : switch_w + kit_px(12), side_h = ImGui::GetContentRegionAvail().y;
        f32 pad_y = kit_px(LIB_PAD_Y);
        ImVec2 s0 = ImGui::GetCursorScreenPos();
        s0.x += kit_px(8); // the list, its foot button and the clips after it sit this far in from the window's edge
        ImGui::SetCursorScreenPos(ImVec2(s0.x + kit_px(6), s0.y + pad_y));
        f32 foot = fh + kit_px(6);
        ImGui::BeginChild("##gameside", ImVec2(side_w - kit_px(12), side_h - 2 * pad_y - foot), 0,
                          ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
        bool &by_folder = g_app.config.side_folders;
        if (lib_side_toggle(side_w - kit_px(12), collapsed)) {
            by_folder = !by_folder;
            lib.folder_renaming = 0;
            config_save(g_app.config, g_app.paths);
            lib.scroll = lib.scroll_to = 0;
            lib_refilter();
        }
        i32 pick = -1;
        static char game_target[64]; // the game a right-click menu is about
        bool game_menu = false;
        i32 order[LIB_MAX_GAMES + 1] = {0}; // All games, then the favourite games, then the rest
        i32 shown = by_folder ? 0 : 1 + lib_favourite_order("games", lib.game_count, [&](i32 i) { return (const char *)lib.games[i]; }, order + 1);
        for (i32 o = 1; o < shown; o++) order[o]++;
        if (by_folder) lib_folder_list(side_w - kit_px(12), collapsed);
        else for (i32 o = 0; o < shown; o++) {
            i32 c = order[o];
            const char *name = c == 0 ? "All games" : lib.games[c - 1];
            bool active = c == 0 ? !lib.game[0] : !strcmp(lib.game, name);
            ImGui::PushID(c);
            // No game gets a built-in glyph: a monitor for Desktop, a video for files not ours; a custom game a
            // controller
            bool none = c > 0 && game_is_none(name), custom = c > 0 && !none && game_is_custom(name);
            const char *glyph = custom ? ICON_GAME : !none ? 0 : !strcmp(name, GAME_DESKTOP) ? ICON_MONITOR : ICON_VIDEO;
            if (lib_game_item(c == 0 || none || custom ? 0 : name, name, c == 0 ? lib.range_count : lib.game_items[c - 1], active,
                              side_w - kit_px(12), collapsed, glyph, c > 0 && lib_favourite_find("games", name) >= 0))
                pick = c;
            if (c > 0 && ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                str_set(game_target, name);
                game_menu = true;
            }
            ImGui::PopID();
        }
        if (game_menu) ImGui::OpenPopup("game_menu");
        lib_popup_style_push(6);
        if (ImGui::BeginPopup("game_menu")) {
            bool favourite = lib_favourite_find("games", game_target) >= 0;
            if (lib_menu_item(favourite ? ICON_STAR : ICON_STAR_FILL, favourite ? "Unfavourite" : "Favourite"))
                lib_favourite_set("games", game_target, !favourite);
            ImGui::EndPopup();
        }
        lib_popup_style_pop();
        kit_scroll_fade_window();
        ImGui::EndChild();
        // The collapse button at the list's foot: in its bottom left corner, in the middle of the rail it collapses to,
        // so it's in the same place either way and only its arrow turns.
        f32 rail_w = icon_end + kit_px(14);
        ImGui::SetCursorScreenPos(ImVec2(floorf(s0.x + (rail_w - fh) / 2), s0.y + side_h - pad_y - fh));
        kit_next_tip(collapsed ? "Show the list" : "Collapse the list");
        if (kit_icon_button(collapsed ? ICON_RIGHT : ICON_LEFT, "##games_collapse")) {
            collapsed = !collapsed;
            config_save(g_app.config, g_app.paths);
            log_line("library: the list on the left %s", collapsed ? "collapsed" : "shown");
        }
        if (pick >= 0) {
            if (pick == 0) lib.game[0] = 0;
            else str_set(lib.game, lib.games[pick - 1]);
            lib.scroll = lib.scroll_to = 0;
            lib_refilter();
        }
        ImGui::SetCursorScreenPos(ImVec2(s0.x + side_w - kit_px(14), s0.y)); // the clips start 16 px past the list's rows
    }

    // Everything else, the player too, right of the list in the screen's usual padding (the Library's screen has
    // none of its own, so the list reaches the window's edges)
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(kit_px(UI_GUTTER), kit_px(LIB_PAD_Y)));
    ImGui::BeginChild("##libmain", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground);
    ImGui::PopStyleVar();
    lib_uploads_poll();
    lib_note_draw();
    ffget_bar();
    lib_share_pending_check();
    if (lib.player.open) {
        lib_player_view();
        ImGui::EndChild();
        return;
    }
    ImGuiIO &io = ImGui::GetIO();
    dl = ImGui::GetWindowDrawList();
    f32 full_w = ImGui::GetContentRegionAvail().x;

    // Header, all of one height: the Clips / Sessions switch, then pills that each drop a list (the date range, the
    // order), and the search on the right. A list that is narrowing what's shown is outlined.
    ImVec2 h0 = ImGui::GetCursorScreenPos();
    static const char *const views[] = {ICON_VIDEO "  Clips", ICON_GAME "  Sessions"};
    static const char *const ranges[] = {"All time", "Today", "This week", "This month", "Custom"};
    static const char *const sorts[SORT_COUNT] = {"Newest", "Oldest", "Longest", "Biggest", "Game"};
    f32 hgap = kit_px(8), drop_w = kit_px(112);
    ImGui::SetCursorScreenPos(h0);
    bool flip_view = lib_switch("##view", views, switch_w, fh, lib.view == LIB_SESSIONS, COL_PANEL2, COL_HOVER,
                                COL_OFF);
    kit_tip(lib.view == LIB_SESSIONS ? "Show clips" : "Show sessions");
    if (flip_view) lib_set_view(lib.view == LIB_SESSIONS ? LIB_CLIPS : LIB_SESSIONS);

    // While cards are selected, the selection bar takes the rest of the row (lib_sel_bar).
    lib_sel_track();
    static i32 sel_idx[LIB_MAX_ITEMS];
    i32 sel_n = lib_sel_indices(sel_idx);
    bool typing = io.WantTextInput, popup_open = ImGui::IsPopupOpen((const char *)0, ImGuiPopupFlags_AnyPopupId);
    if (!typing && !popup_open && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A, false)) {
        lib_sel_all();
        sel_n = lib_sel_indices(sel_idx);
    }
    if (sel_n > 0 && !typing && !popup_open && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        lib_sel_clear();
        sel_n = 0;
    }

    // The date range; Custom opens the calendar
    i32 step;
    ImVec2 range_at(h0.x + switch_w + kit_px(16), h0.y);
    if (sel_n > 0) lib_sel_bar(range_at, h0.x + full_w, sel_n);
    else {
    ImGui::SetCursorScreenPos(range_at);
    if (lib_pill("##range", ranges[lib.range], drop_w, lib.range != 0)) ImGui::OpenPopup("range");
    step = lib.range;
    if (kit_wheel_step(&step, RANGE_CUSTOM)) { // every range but Custom, which needs its calendar
        lib.range = step;
        lib.scroll = lib.scroll_to = 0;
        lib_refilter();
    }
    ImGui::SetNextWindowPos(ImVec2(range_at.x, range_at.y + fh + kit_px(6)));
    lib_popup_style_push(8);
    bool range_open = ImGui::BeginPopup("range");
    lib_popup_style_pop();
    bool open_calendar = false;
    if (range_open) {
        for (i32 i = 0; i < 5; i++) {
            if (!lib_menu_item(lib.range == i ? ICON_CHECK : "", ranges[i], false, kit_px(132))) continue;
            if (i == RANGE_CUSTOM) {
                open_calendar = true;
            } else if (lib.range != i) {
                lib.range = i;
                lib.scroll = lib.scroll_to = 0;
                lib_refilter();
            }
        }
        ImGui::EndPopup();
    }
    if (open_calendar) {
        lib.cal_picking = false;
        ImGui::OpenPopup("calendar");
    }
    ImGui::SetNextWindowPos(ImVec2(range_at.x, range_at.y + fh + kit_px(6)));
    lib_calendar();

    // The order, then the search, which takes what the row has left between a narrowest and a widest
    ImVec2 sort_at(range_at.x + drop_w + hgap, h0.y);
    f32 search_w = full_w - (sort_at.x - h0.x) - drop_w - kit_px(12);
    search_w = search_w < kit_px(100) ? kit_px(100) : search_w > kit_px(240) ? kit_px(240) : search_w;
    ImGui::SetCursorScreenPos(sort_at);
    if (lib_pill("##sort", sorts[lib.sort], drop_w, false)) ImGui::OpenPopup("sort");
    step = lib.sort;
    if (kit_wheel_step(&step, SORT_COUNT)) {
        lib.sort = step;
        lib.scroll = lib.scroll_to = 0;
        lib_refilter();
    }
    ImGui::SetNextWindowPos(ImVec2(sort_at.x, sort_at.y + fh + kit_px(6)));
    lib_popup_style_push(8);
    bool sort_open = ImGui::BeginPopup("sort");
    lib_popup_style_pop();
    if (sort_open) {
        for (i32 i = 0; i < SORT_COUNT; i++)
            if (lib_menu_item(lib.sort == i ? ICON_CHECK : "", sorts[i], false, kit_px(132)) && lib.sort != i) {
                lib.sort = i;
                lib.scroll = lib.scroll_to = 0;
                lib_refilter();
            }
        ImGui::EndPopup();
    }
    ImGui::SetCursorScreenPos(ImVec2(h0.x + full_w - search_w, h0.y));
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F, false) && !ImGui::IsPopupOpen("game")) ImGui::SetKeyboardFocusHere();
    {
        ImVec2 s0 = ImGui::GetCursorScreenPos();
        kit_shadow(ImGui::GetWindowDrawList(), s0, ImVec2(s0.x + search_w, s0.y + fh), ImGui::GetStyle().FrameRounding);
    }
    if (kit_search("##search", lib.search, sizeof lib.search, "Search", search_w)) {
        lib.scroll = lib.scroll_to = 0;
        lib_refilter();
    }
    }
    ImGui::SetCursorScreenPos(ImVec2(h0.x, h0.y + fh + kit_px(16)));


    g_lib_missing = 0;
    lib_unsaved_over = false;
    if (lib.view == LIB_SESSIONS) lib_unsaved_row(full_w);
    else lib_posters_ahead();


    // The grid fills what's left above the footer and scrolls: the wheel, Page Up / Page Down, Home / End, or the
    // bar on its right.
    f32 foot_h = fh;
    ImVec2 g0 = ImGui::GetCursorScreenPos();
    f32 grid_h = ImGui::GetContentRegionAvail().y - foot_h - kit_px(8);
    if (grid_h < 1) grid_h = 1;
    // As many columns as bring the cards nearest the width asked for; Ctrl + wheel asks for one column fewer or
    // more (never cards under 140 wide), keeping the row that was at the top there.
    const f32 narrowest = kit_px(140);
    f32 gap = kit_px(14), day_h = kit_px(30);
    i32 &want_w = g_app.config.card_width;
    if (want_w < 140) want_w = 140;
    i32 cols = (i32)((full_w + gap) / (kit_px((f32)want_w) + gap) + 0.5f);
    i32 most = (i32)((full_w + gap) / (narrowest + gap));
    if (most < 1) most = 1;
    cols = cols < 1 ? 1 : cols > most ? most : cols;
    bool over_grid = ImGui::IsMouseHoveringRect(g0, ImVec2(g0.x + full_w, g0.y + grid_h)), ctrl = io.KeyCtrl;
    if (ctrl && io.MouseWheel != 0 && over_grid && !ImGui::IsPopupOpen((const char *)0, ImGuiPopupFlags_AnyPopupId)) {
        i32 to = cols + (io.MouseWheel > 0 ? -1 : 1);
        if (to >= 1 && to <= most) {
            f32 old_h = day_h + floorf(floorf((full_w - (cols - 1) * gap) / cols) * 9 / 16) + lib_meta_h();
            i32 first = (i32)(lib.scroll / old_h) * cols; // the first card of the top row
            cols = to;
            f32 w = floorf((full_w - (cols - 1) * gap) / cols);
            want_w = (i32)(w / g_dpi_scale + 0.5f);
            lib.scroll = lib.scroll_to = (first / cols) * (day_h + floorf(w * 9 / 16) + lib_meta_h());
            config_save(g_app.config, g_app.paths);
        }
    }
    f32 card_w = floorf((full_w - (cols - 1) * gap) / cols), thumb_h = floorf(card_w * 9 / 16);
    f32 card_h = day_h + thumb_h + lib_meta_h();
    i32 rows = (lib.shown_count + cols - 1) / cols;
    f32 content_h = rows * card_h, max_scroll = content_h > grid_h ? content_h - grid_h : 0;

    bool popup = ImGui::IsPopupOpen((const char *)0, ImGuiPopupFlags_AnyPopupId);
    f32 bar_w = kit_px(14), edge = kit_px(4); // edge: room left of the first column for a selected card's outline
    ImGui::SetCursorScreenPos(ImVec2(g0.x - edge, g0.y));
    ImGui::BeginChild("##grid", ImVec2(edge + full_w + bar_w, grid_h), 0,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground);
    dl = ImGui::GetWindowDrawList();
    if (!popup && !ctrl && io.MouseWheel != 0 &&
        (ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) || (lib_unsaved_over && !io.KeyShift)))
        lib.scroll_to -= io.MouseWheel * floorf(card_h * 0.6f);
    if (!popup && !io.WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_PageDown)) lib.scroll_to += floorf(grid_h * 0.9f);
        if (ImGui::IsKeyPressed(ImGuiKey_PageUp)) lib.scroll_to -= floorf(grid_h * 0.9f);
        if (ImGui::IsKeyPressed(ImGuiKey_Home)) lib.scroll_to = 0;
        if (ImGui::IsKeyPressed(ImGuiKey_End)) lib.scroll_to = max_scroll;
    }

    // The bar: drag its thumb, or click the track to go there.
    static f32 grab; // where in the thumb the drag took hold
    if (max_scroll > 0) {
        f32 thumb = floorf(grid_h * grid_h / content_h);
        if (thumb < kit_px(36)) thumb = kit_px(36);
        f32 travel = grid_h - thumb, ty = g0.y + (lib.scroll / max_scroll) * travel;
        ImGui::SetCursorScreenPos(ImVec2(g0.x + full_w + kit_px(2), g0.y));
        ImGui::InvisibleButton("##bar", ImVec2(bar_w - kit_px(2), grid_h));
        bool hot = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
        if (ImGui::IsItemActivated()) grab = io.MousePos.y >= ty && io.MousePos.y <= ty + thumb ? io.MousePos.y - ty : thumb / 2;
        if (held && travel > 0) lib.scroll = lib.scroll_to = (io.MousePos.y - grab - g0.y) / travel * max_scroll;
        f32 bx = g0.x + full_w + kit_px(6);
        dl->AddRectFilled(ImVec2(bx, ty), ImVec2(bx + kit_px(6), ty + thumb), col32(held ? COL_DIM : hot ? COL_FAINT : COL_HOVER), kit_px(3));
    }

    // Glide to where it was sent
    if (lib.scroll_to < 0) lib.scroll_to = 0;
    if (lib.scroll_to > max_scroll) lib.scroll_to = max_scroll;
    f32 away = lib.scroll_to - lib.scroll;
    if (fabsf(away) < 0.5f) {
        lib.scroll = lib.scroll_to;
    } else {
        lib.scroll += away * (1 - expf(-io.DeltaTime * 16));
        ui_wake();
    }
    if (lib.scroll < 0) lib.scroll = 0;
    if (lib.scroll > max_scroll) lib.scroll = max_scroll;
    // Scrolled well past what was asked for: thumbnails not started are dropped, so the ones in view come first
    static f32 dropped_at;
    if (fabsf(lib.scroll - dropped_at) > grid_h) {
        lib_drop_queued_thumbs();
        dropped_at = lib.scroll;
    }

    if (lib.shown_count == 0) {
        const char *msg = !lib.scanned ? "Looking for saved files..."
                          : lib.view_count == 0 ? (lib.view == LIB_CLIPS ? "No clips yet" : "No sessions yet")
                          : lib.range_count == 0 ? "Nothing in this time"
                          : g_app.config.side_folders && lib.folder[0] && !lib.search[0] ? "Nothing in this folder"
                                                 : "Nothing matches";
        f32 cy = floorf(g0.y + grid_h * 0.38f);
        ImGui::PushFont(g_font_title);
        ImVec2 is = ImGui::CalcTextSize(ICON_VIDEO);
        dl->AddText(ImVec2(floorf(g0.x + (full_w - is.x) / 2), cy - is.y), col32(COL_FAINT), ICON_VIDEO);
        ImGui::PopFont();
        ImGui::PushFont(g_font_big);
        ImVec2 ts = ImGui::CalcTextSize(msg);
        dl->AddText(ImVec2(floorf(g0.x + (full_w - ts.x) / 2), cy + kit_px(8)), col32(COL_DIM), msg);
        ImGui::PopFont();
    }
    // Only the rows in view are drawn (and ask for their thumbnails)
    f32 top = floorf(lib.scroll);
    i32 r0 = (i32)(top / card_h), r1 = (i32)((top + grid_h) / card_h);
    // A period's (or game's) heading goes over its first card only, so its rows read as one section
    auto group = [&](i32 idx) {
        const LibItem &it = lib.items[lib.shown[idx]];
        return lib.sort == SORT_GAME ? (i64)fnv64(it.info.game, strlen(it.info.game)) : lib_period(it.when_us, 0, 0);
    };
    for (i32 r = r0; r <= r1 && r < rows; r++) {
        for (i32 c = 0; c < cols; c++) {
            i32 idx = r * cols + c;
            if (idx >= lib.shown_count) break;
            const LibItem &it = lib.items[lib.shown[idx]];
            bool show_day = idx == 0 || group(idx) != group(idx - 1);
            lib_card(it, ImVec2(g0.x + c * (card_w + gap), g0.y + r * card_h - top), card_w, thumb_h, day_h, show_day);
        }
    }
    kit_scroll_fade(g0, ImVec2(g0.x + full_w, g0.y + grid_h), lib.scroll, max_scroll - lib.scroll);
    ImGui::SetCursorScreenPos(g0); // the cards above and below the view don't count as this window's content
    ImGui::EndChild();
    dl = ImGui::GetWindowDrawList();
    if (lib.open_menu) ImGui::OpenPopup("card_menu");
    lib.open_menu = false;
    card_menu();
    if (g_show_dialog == 1 && lib.shown_count > 0) {
        lib.target = lib.items[lib.shown[0]];
        ImGui::OpenPopup("Delete");
    }
    if (g_show_dialog == 1) g_show_dialog = 0;
    delete_modal();
    lib_sel_delete_modal();
    if (g_lib_switch_us && !g_lib_first_us) g_lib_first_us = now_us();
    if (g_lib_switch_us && lib.scanned && !g_lib_missing) {
        DEV_LOG("library: %s drawn %.0f ms after the switch, with all its pictures %.0f ms after that",
                lib.view == LIB_CLIPS ? "clips" : "sessions", (g_lib_first_us - g_lib_switch_us) / 1000.0,
                (now_us() - g_lib_first_us) / 1000.0);
        g_lib_switch_us = 0;
    }

    // Footer: how many, and the last message.
    ImGui::SetCursorScreenPos(ImVec2(g0.x, g0.y + grid_h + kit_px(8)));
    ImVec2 f0 = ImGui::GetCursorScreenPos();
    char count[120], span[48] = "";
    if (lib.range == RANGE_CUSTOM) {
        char d0[16], d1[16];
        fmt_day_short(lib.range_from, d0, sizeof d0);
        fmt_day_short(lib.range_to, d1, sizeof d1);
        if (lib.range_from == lib.range_to) snprintf(span, sizeof span, " on %s", d0);
        else snprintf(span, sizeof span, " from %s to %s", d0, d1);
    }
    snprintf(count, sizeof count, "%d %s%s%s", lib.shown_count,
             lib.view == LIB_CLIPS ? (lib.shown_count == 1 ? "clip" : "clips") : (lib.shown_count == 1 ? "session" : "sessions"),
             span, lib.scanning ? ", looking for more" : "");
    f32 ty = floorf(f0.y + (fh - ImGui::GetFontSize()) / 2);
    dl->AddText(ImVec2(f0.x, ty), col32(COL_DIM), count);
    f32 msg_x = f0.x + kit_px(320), msg_end = f0.x + full_w;
    // At the right end, once the clips and sessions pass Options' size: an amber warning with how much they hold.
    // A click opens Options at that setting. Nothing is ever deleted for it.
    f32 warn_gb = g_app.config.folder_warn_gb;
    if (warn_gb > 0 && (f64)g_lib_folder_bytes > (f64)warn_gb * 1e9) {
        char held[32], warn[96];
        bytes_text((i64)g_lib_folder_bytes, held, sizeof held);
        snprintf(warn, sizeof warn, "%s of clips, over %g GB", held, warn_gb);
        f32 gsz = g_font_icon->FontSize;
        ImVec2 gs = g_font_icon->CalcTextSizeA(gsz, FLT_MAX, 0, ICON_WARNING);
        f32 ww = gs.x + kit_px(6) + ImGui::CalcTextSize(warn).x, wx = f0.x + full_w - ww;
        ImVec2 w0(wx, f0.y), w1(f0.x + full_w, f0.y + fh);
        ImGui::SetCursorScreenPos(w0);
        if (ImGui::InvisibleButton("##folderwarn", ImVec2(ww, fh))) {
            dev_click("library", "folder warning");
            g_app.options.tab = OPT_GENERAL;
            ui_show(SCREEN_OPTIONS);
        }
        bool hot = ImGui::IsItemHovered();
        if (hot) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        kit_tip_rect(w0, w1, "Change the warning in Options");
        dl->AddText(g_font_icon, gsz, ImVec2(wx, floorf(f0.y + (fh - gs.y) / 2)), col32(COL_WARN), ICON_WARNING);
        dl->AddText(ImVec2(wx + gs.x + kit_px(6), ty), col32(hot ? COL_WHITE : COL_WARN), warn);
        msg_end = wx - kit_px(16);
    }
    text_clipped(dl, ImVec2(msg_x, ty), msg_end - msg_x, col32(lib.message_error ? 0xff8a92 : COL_DIM), lib.message);
    ImGui::SetCursorScreenPos(ImVec2(f0.x, f0.y + fh));
    ImGui::EndChild();
}

