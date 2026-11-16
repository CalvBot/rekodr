// Library: the saved clips and sessions, found under <save_dir>\<year>\{Clips,Sessions} (and loose in <year>\,
// where the Rust version saved them), and the player.
//
// Two worker threads do the slow parts: the scan (listing the folders, ffprobe for files not seen before) and the
// thumbnails (ffmpeg makes a strip of 24 frames per file, cached under local_dir\thumbs, decoded with WIC). They
// share only LibShared below, under its lock, and post EV_LIBRARY events; the main thread copies their results
// into g_app.library and turns pixels into textures. The cache is keyed by a file's size and time, so a rename
// keeps its thumbnail.
//
// Everything on screen is fixed-size: a paged grid that fits the window (the wheel turns pages), paged game chips,
// and a pager row whose controls never move.

static void editor_open(const LibItem &item); // editor.cpp

enum LibTaskKind : u8 { LT_SCAN, LT_THUMB, LT_STRIP };
enum { LIB_STRIP_FRAMES = 24 };

struct LibTask {
    LibTaskKind kind;
    u64         key;
    f64         duration;
    Path        path;
};

struct LibResult {
    u64  key;
    bool strip, failed;
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
    LibItem            items[LIB_MAX_ITEMS];        // the newest scan's published list
    i32                item_count;
    u32                items_gen;
    LibResult          results[64];
    i32                result_count;
    HANDLE             threads[2];
};

static LibShared g_lib;
static LibItem   g_scan_items[LIB_MAX_ITEMS]; // the scan's own list (one scan runs at a time)

static u64 fnv64(const void *data, size_t n, u64 h = 14695981039346656037ull)
{
    const u8 *b = (const u8 *)data;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}


static Path lib_cache_file(const Path &cache, u64 key, const wchar_t *ext)
{
    wchar_t name[64];
    swprintf(name, COUNT(name), L"c-%016llx%s", key, ext);
    return path_join(cache, name);
}

// "Overwatch" from "Overwatch 2026-11-15 21-04-10": the words before the date. A clip saved on the desktop is named
// "Clip <date>", which is "Desktop"; a file without a date isn't one of ours, which is "Other".
static void game_from_name(const char *name, char *out, i32 cap)
{
    size_t n = strlen(name);
    for (size_t i = 0; i + 10 <= n; i++) {
        const char *d = name + i;
        bool date = isdigit((u8)d[0]) && isdigit((u8)d[1]) && isdigit((u8)d[2]) && isdigit((u8)d[3]) && d[4] == '-' &&
                    isdigit((u8)d[5]) && isdigit((u8)d[6]) && d[7] == '-' && isdigit((u8)d[8]) && isdigit((u8)d[9]);
        if (!date || (i > 0 && name[i - 1] != ' ')) continue;
        char prefix[256];
        snprintf(prefix, sizeof prefix, "%.*s", (int)i, name);
        char *p = trim(prefix);
        if (!*p || !_stricmp(p, "Clip")) snprintf(out, cap, "Desktop");
        else snprintf(out, cap, "%s", p);
        return;
    }
    snprintf(out, cap, "Other");
}

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
    bool ok = false;
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = trim(line), *v = trim(eq + 1);
        if (!strcmp(k, "duration")) { m->duration = atof(v); ok = true; }
        else if (!strcmp(k, "width")) m->width = atoi(v);
        else if (!strcmp(k, "height")) m->height = atoi(v);
        else if (!strcmp(k, "fps")) m->fps = atof(v);
        else if (!strcmp(k, "tagged")) snprintf(tagged, tagged_cap, "%s", v);
        else if (!strcmp(k, "track") && m->track_count < MEDIA_MAX_TRACKS) str_set(m->tracks[m->track_count++], v);
    }
    fclose(f);
    return ok;
}

static void lib_cache_write(const Path &cache, u64 key, const MediaInfo &m, const char *tagged)
{
    Path final_path = lib_cache_file(cache, key, L".txt"), tmp = lib_cache_file(cache, key, L".txt.tmp");
    FILE *f = _wfopen(tmp.s, L"wb");
    if (!f) return;
    fprintf(f, "duration = %.3f\nwidth = %d\nheight = %d\nfps = %.3f\ntagged = %s\n", m.duration, m.width, m.height,
            m.fps, tagged);
    for (i32 t = 0; t < m.track_count; t++) fprintf(f, "track = %s\n", m.tracks[t]);
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
                                       "avg_frame_rate:stream_tags=title,handler_name",
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
    auto end_stream = [&]() {
        if (in_audio && m->track_count < MEDIA_MAX_TRACKS) {
            char *t = m->tracks[m->track_count];
            if (title[0]) snprintf(t, 32, "%s", title);
            else if (handler[0] && _stricmp(handler, "SoundHandler")) snprintf(t, 32, "%s", handler);
            else snprintf(t, 32, "Track %d", m->track_count + 1);
            m->track_count++;
        }
        in_audio = in_video = false;
        title[0] = handler[0] = 0;
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
        else if (!strcmp(k, "TAG:title")) snprintf(title, sizeof title, "%s", v);
        else if (!strcmp(k, "TAG:handler_name")) snprintf(handler, sizeof handler, "%s", v);
        else if (!strcmp(k, "duration")) m->duration = atof(v);
        else if (!strcmp(k, "TAG:genre")) snprintf(genre, sizeof genre, "%s", v);
        else if (!strcmp(k, "TAG:comment")) snprintf(comment, sizeof comment, "%s", v);
    }
    if (starts_with(comment, "clipback") && genre[0]) snprintf(tagged, tagged_cap, "%s", genre);
    return m->duration > 0;
}

static int lib_item_newer(const void *a, const void *b)
{
    i64 x = ((const LibItem *)a)->mtime_us, y = ((const LibItem *)b)->mtime_us;
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

// Games given by hand, for files that carry no tag of their own (clips from before clipback, renamed ones):
// <config_dir>\games.txt, a line each of "<key in hex> | <game> | <file name>". The key is the item's (its size and
// time), so a rename keeps the game; the name is only there for people reading the file. Read at every scan, by
// the scan's thread.
struct GameOverride {
    u64  key;
    char game[64];
};
static GameOverride g_game_overrides[LIB_MAX_ITEMS];
static i32          g_game_override_count;

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

// A file's game: the one given by hand, else its own tag, else what its name says.
static bool lib_set_game(LibItem *it, const char *tagged)
{
    for (i32 i = 0; i < g_game_override_count; i++) {
        if (g_game_overrides[i].key != it->key) continue;
        str_set(it->info.game, g_game_overrides[i].game);
        return true;
    }
    if (tagged[0]) str_set(it->info.game, tagged);
    else game_from_name(it->name, it->info.game, sizeof it->info.game);
    return false;
}

// Where an item is: <save_dir>\<year>\{Clips,Sessions}\<name><ext>, or straight in <save_dir>\<year>\ for clips
// saved by the Rust version, which are listed where they are rather than moved.
static Path lib_file_path(const Path &save_dir, const LibItem &it)
{
    Path p = {};
    wchar_t name[256], ext[8];
    utf8_to_wide(it.name, name, COUNT(name));
    utf8_to_wide(it.ext, ext, COUNT(ext));
    if (it.loose) swprintf(p.s, MAX_PATH, L"%s\\%d\\%s%s", save_dir.s, it.year, name, ext);
    else swprintf(p.s, MAX_PATH, L"%s\\%d\\%s\\%s%s", save_dir.s, it.year, it.kind ? L"Sessions" : L"Clips", name, ext);
    return p;
}

static void lib_scan(const Path &save_dir, const Path &cache, const Path &ffmpeg)
{
    LibItem *items = g_scan_items;
    i32 n = 0, by_hand = 0;
    lib_overrides_load();
    WIN32_FIND_DATAW fd;
    HANDLE years = FindFirstFileW(path_join(save_dir, L"*").s, &fd);
    if (years != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || wcslen(fd.cFileName) != 4 || !iswdigit(fd.cFileName[0]))
                continue;
            i16 year = (i16)_wtoi(fd.cFileName);
            Path ydir = path_join(save_dir, fd.cFileName);
            // 0: Clips, 1: Sessions, 2: loose videos in the year folder itself (the Rust version's layout), as clips.
            for (i32 kind = 0; kind < 3; kind++) {
                Path dir = kind == 2 ? ydir : path_join(ydir, kind ? L"Sessions" : L"Clips");
                WIN32_FIND_DATAW f;
                HANDLE h = FindFirstFileW(path_join(dir, L"*").s, &f);
                if (h == INVALID_HANDLE_VALUE) continue;
                do {
                    if (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                    wchar_t *dot = wcsrchr(f.cFileName, L'.');
                    if (!dot || (_wcsicmp(dot, L".mp4") && _wcsicmp(dot, L".mkv") && _wcsicmp(dot, L".mov"))) continue;
                    if (n == LIB_MAX_ITEMS) break;
                    LibItem *it = &items[n++];
                    *it = {};
                    wide_to_utf8(dot, it->ext, sizeof it->ext);
                    *dot = 0;
                    wide_to_utf8(f.cFileName, it->name, sizeof it->name);
                    it->year = year;
                    it->kind = kind == 1 ? LIB_SESSIONS : LIB_CLIPS;
                    it->loose = kind == 2;
                    it->size = ((u64)f.nFileSizeHigh << 32) | f.nFileSizeLow;
                    it->mtime_us = filetime_us(f.ftLastWriteTime);
                    u64 k = fnv64(&it->size, sizeof it->size);
                    it->key = fnv64(&it->mtime_us, sizeof it->mtime_us, k);
                    char tagged[64];
                    it->probed = lib_cache_read(cache, it->key, &it->info, tagged, sizeof tagged);
                    by_hand += lib_set_game(it, it->probed ? tagged : "");
                } while (FindNextFileW(h, &f));
                FindClose(h);
            }
        } while (FindNextFileW(years, &fd));
        FindClose(years);
    }
    qsort(items, n, sizeof items[0], lib_item_newer);
    lib_publish(items, n);
    if (g_game_override_count) DEV_LOG("library: games.txt names %d files, %d of them here", g_game_override_count, by_hand);

    // Files not seen before: ffprobe them, newest first, showing them as they come.
    if (!ffmpeg.s[0]) return;
    Path ffprobe = ffprobe_for(ffmpeg);
    i32 probed = 0;
    u64 last = GetTickCount64();
    for (i32 i = 0; i < n; i++) {
        LibItem *it = &items[i];
        if (it->probed) continue;
        Path file = lib_file_path(save_dir, *it);
        char tagged[64];
        if (lib_probe(ffprobe, file, &it->info, tagged, sizeof tagged)) {
            lib_cache_write(cache, it->key, it->info, tagged);
        } else {
            log_line("library: couldn't probe %s%s", it->name, it->ext);
            it->info.duration = 0; // shown without a length rather than asked again and again this scan
        }
        it->probed = true;
        lib_set_game(it, tagged);
        probed++;
        if (GetTickCount64() - last > 400) {
            lib_publish(items, n);
            last = GetTickCount64();
        }
    }
    if (probed) {
        lib_publish(items, n);
        log_line("library: %d files, %d probed", n, probed);
    }
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
        // The poster: the first frame of the strip.
        i32 fw = w / LIB_STRIP_FRAMES;
        if (fw <= 0) fw = w;
        r.pixels = (u32 *)HeapAlloc(GetProcessHeap(), 0, (size_t)fw * h * 4);
        if (r.pixels)
            for (i32 y = 0; y < h; y++) memcpy(r.pixels + (size_t)y * fw, px + (size_t)y * w, fw * 4);
        HeapFree(GetProcessHeap(), 0, px);
        r.w = fw;
        r.h = h;
        r.failed = !r.pixels;
    }
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
        Path save_dir = g_lib.save_dir, cache = g_lib.cache_dir, ffmpeg = g_lib.ffmpeg;
        if (t.kind == LT_SCAN) {
            g_lib.scan_queued = false;
            g_lib.scan_running = true;
        }
        ReleaseSRWLockExclusive(&g_lib.lock);

        if (t.kind == LT_SCAN) {
            u64 started = GetTickCount64();
            lib_scan(save_dir, cache, ffmpeg);
            AcquireSRWLockExclusive(&g_lib.lock);
            g_lib.scan_running = false;
            ReleaseSRWLockExclusive(&g_lib.lock);
            Event e = {};
            e.kind = EV_LIBRARY;
            e.a = LIB_EV_SCANNED; // once more, so the main thread sees the scan has ended
            event_post(e);
            if (GetTickCount64() - started > 2000) log_line("library: scan took %.1f s", (GetTickCount64() - started) / 1000.0);
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
    SHCreateDirectoryExW(0, cache.s, 0);
    AcquireSRWLockExclusive(&g_lib.lock);
    bool queued = g_lib.scan_queued;
    g_lib.save_dir = g_app.config.save_dir;
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

static Path lib_item_path(const LibItem &it) { return lib_file_path(g_app.config.save_dir, it); }

// Where an item's edit is kept: <save_dir>\<year>\Data\<name>.edit.txt
static Path lib_edit_path(const LibItem &it)
{
    Path p = {};
    wchar_t name[256];
    utf8_to_wide(it.name, name, COUNT(name));
    swprintf(p.s, MAX_PATH, L"%s\\%d\\Data\\%s.edit.txt", g_app.config.save_dir.s, it.year, name);
    return p;
}

static bool lib_matches(const char *hay, const char *needle)
{
    if (!*needle) return true;
    size_t n = strlen(needle);
    for (const char *h = hay; *h; h++)
        if (!_strnicmp(h, needle, n)) return true;
    return false;
}

enum LibRange : i32 { RANGE_ALL, RANGE_TODAY, RANGE_WEEK, RANGE_MONTH, RANGE_CUSTOM };
enum LibSort : i32 { SORT_NEWEST, SORT_OLDEST, SORT_LONGEST, SORT_BIGGEST };

static i64 local_day(i64 us, SYSTEMTIME *out);

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
    case SORT_OLDEST: if (a.mtime_us != b.mtime_us) return a.mtime_us < b.mtime_us ? -1 : 1; break;
    case SORT_LONGEST: if (a.info.duration != b.info.duration) return a.info.duration > b.info.duration ? -1 : 1; break;
    case SORT_BIGGEST: if (a.size != b.size) return a.size > b.size ? -1 : 1; break;
    default: break;
    }
    return a.mtime_us > b.mtime_us ? -1 : a.mtime_us < b.mtime_us ? 1 : 0;
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
    for (i32 i = 0; i < lib.item_count; i++) {
        const LibItem &it = lib.items[i];
        if (it.kind != lib.view) continue;
        lib.view_count++;
        if (!lib_in_range(it.mtime_us)) continue;
        lib.range_count++;
        const char *g = it.info.game;
        if (!strcmp(g, "Desktop")) desktop++;
        else if (!strcmp(g, "Other")) other++;
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
        if (lib.game[0] && strcmp(lib.game, g)) continue;
        if (!lib_matches(it.name, lib.search) && !lib_matches(g, lib.search)) continue;
        lib.shown[lib.shown_count++] = i;
    }
    // Games most recent first, then Desktop, then files not ours.
    if (desktop) { str_set(lib.games[lib.game_count], "Desktop"); lib.game_items[lib.game_count++] = desktop; }
    if (other) { str_set(lib.games[lib.game_count], "Other"); lib.game_items[lib.game_count++] = other; }
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
        if (r.strip) {
            if (!r.failed && r.key == lib.strip_want && g_app.ui.shown) {
                if (lib.strip) lib.strip->Release();
                lib.strip = gpu_texture(r.pixels, r.w, r.h);
                lib.strip_key = r.key;
                lib.strip_w = r.w;
                lib.strip_h = r.h;
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
    player_close(&g_app.editor.player); // the editor opens it again when it's next shown
    lib_free_textures();
}

static void library_on_event(const Event &e)
{
    Library &lib = g_app.library;
    switch (e.a) {
    case LIB_EV_SCANNED: {
        AcquireSRWLockShared(&g_lib.lock);
        lib.item_count = g_lib.item_count;
        memcpy(lib.items, g_lib.items, lib.item_count * sizeof lib.items[0]);
        bool running = g_lib.scan_running || g_lib.scan_queued;
        ReleaseSRWLockShared(&g_lib.lock);
        lib.scanned = true;
        lib.scanning = running;
        lib_refilter();
        break;
    }
    case LIB_EV_RESULTS: lib_take_results(); break;
    case LIB_EV_MEDIA: player_on_media(e); break;
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

static void show_in_folder(const wchar_t *path)
{
    PIDLIST_ABSOLUTE pidl = 0;
    if (SUCCEEDED(SHParseDisplayName(path, 0, &pidl, 0, 0))) {
        SHOpenFolderAndSelectItems(pidl, 0, 0, 0);
        CoTaskMemFree(pidl);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Drawing

static void fmt_size(u64 bytes, char *out, i32 cap)
{
    if (bytes >= (1ull << 30)) snprintf(out, cap, "%.1f GB", bytes / 1073741824.0);
    else if (bytes >= (1ull << 20)) snprintf(out, cap, "%.0f MB", bytes / 1048576.0);
    else snprintf(out, cap, "%.0f KB", bytes / 1024.0);
}

// Days since 1601 in local time, for "today" and day headings.
static i64 local_day(i64 us, SYSTEMTIME *out)
{
    SYSTEMTIME t = local_time(us);
    if (out) *out = t;
    FILETIME ft;
    SystemTimeToFileTime(&t, &ft);
    return (i64)((((u64)ft.dwHighDateTime << 32) | ft.dwLowDateTime) / 864000000000ull);
}

static const char *const LIB_MONTHS[] = {"January", "February", "March", "April", "May", "June", "July", "August",
                                     "September", "October", "November", "December"};
static const char *const WEEKDAYS[] = {"Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"};

static void fmt_day(i64 us, char *out, i32 cap)
{
    SYSTEMTIME t;
    i64 day = local_day(us, &t), today = local_day(now_us(), 0);
    if (day == today) snprintf(out, cap, "Today");
    else if (day == today - 1) snprintf(out, cap, "Yesterday");
    else snprintf(out, cap, "%s %d %s", WEEKDAYS[day % 7], t.wDay, LIB_MONTHS[(t.wMonth + 11) % 12]); // 1601-01-01 was a Monday
}

// "today 9:04 pm", "yesterday 9:04 pm", "14 Nov 9:04 pm"
static void fmt_when(i64 us, char *out, i32 cap)
{
    SYSTEMTIME t;
    i64 day = local_day(us, &t), today = local_day(now_us(), 0);
    i32 h = t.wHour % 12 ? t.wHour % 12 : 12;
    char when[32];
    if (day == today) str_set(when, "today");
    else if (day == today - 1) str_set(when, "yesterday");
    else snprintf(when, sizeof when, "%d %.3s", t.wDay, LIB_MONTHS[(t.wMonth + 11) % 12]);
    snprintf(out, cap, "%s %d:%02d %s", when, h, t.wMinute, t.wHour < 12 ? "am" : "pm");
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

// A small label on a dark pill over a picture: a clip's length, its track count, the zoom.
static void badge(ImDrawList *dl, ImVec2 corner, bool right, bool bottom, const char *text)
{
    ImGui::PushFont(g_font_small);
    f32 px = kit_px(6), py = kit_px(2);
    ImVec2 ts = ImGui::CalcTextSize(text);
    f32 w = ts.x + 2 * px, h = ts.y + 2 * py;
    ImVec2 a(floorf(right ? corner.x - w : corner.x), floorf(bottom ? corner.y - h : corner.y));
    dl->AddRectFilled(a, ImVec2(a.x + w, a.y + h), IM_COL32(0, 0, 0, 180), kit_px(5));
    dl->AddText(ImVec2(a.x + px, a.y + py), IM_COL32(240, 240, 240, 255), text);
    ImGui::PopFont();
}

// A button that's drawn (and keeps its place) even when it does nothing.
static bool button_enabled(const char *label, ImVec2 size, bool enabled)
{
    return kit_button(label, KIT_SECONDARY, size, enabled);
}

// ---------------------------------------------------------------------------------------------------------------
// Controls the library, the player and the editor share. Generic enough to move to the kit in ui.cpp.

// A round dark button floating over a picture, with one icon; white under the mouse.
static bool lib_float_button(const char *icon, const char *id, ImVec2 center, f32 r)
{
    ImGui::SetCursorScreenPos(ImVec2(center.x - r, center.y - r));
    bool clicked = ImGui::InvisibleButton(id, ImVec2(2 * r, 2 * r));
    bool hot = ImGui::IsItemHovered();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddCircleFilled(center, r, hot ? IM_COL32(255, 255, 255, 240) : IM_COL32(0, 0, 0, 160));
    ImGui::PushFont(g_font_icon);
    ImVec2 ts = ImGui::CalcTextSize(icon);
    dl->AddText(ImVec2(floorf(center.x - ts.x / 2), floorf(center.y - ts.y / 2)), hot ? col32(COL_BG) : IM_COL32_WHITE, icon);
    ImGui::PopFont();
    return clicked;
}

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
                       u32 fill = COL_TEXT)
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
    if (kit_icon_button(ICON_LEFT, "##prev", KIT_GHOST, *page > 0)) (*page)--;
    ImGui::SameLine(0, 0);
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(tw, fh));
    char t[32];
    snprintf(t, sizeof t, "%d / %d", *page + 1, pages);
    ImVec2 ts = ImGui::CalcTextSize(t);
    ImGui::GetWindowDrawList()->AddText(ImVec2(floorf(p.x + (tw - ts.x) / 2), floorf(p.y + (fh - ts.y) / 2)), col32(COL_DIM), t);
    ImGui::SameLine(0, 0);
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
// many it has on the right. The picked one is in the accent colour.
static bool lib_game_item(const char *game, const char *label, i32 count, bool active)
{
    f32 w = kit_px(228), h = kit_px(30), is = kit_px(20);
    ImVec2 a = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::Selectable("##game", false, 0, ImVec2(w, h));
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - ImGui::GetStyle().ItemSpacing.y + kit_px(2)); // rows 2 apart
    if (clicked) dev_click("menu", label);
    ImDrawList *dl = ImGui::GetWindowDrawList();
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
        ImVec2 gs = ImGui::CalcTextSize(ICON_APPS);
        dl->AddText(ImVec2(floorf(i0.x + (is - gs.x) / 2), floorf(i0.y + (is - gs.y) / 2)), col32(COL_DIM), ICON_APPS);
        ImGui::PopFont();
    }
    char n[16];
    snprintf(n, sizeof n, "%d", count);
    ImVec2 ts = ImGui::CalcTextSize(label), ns = ImGui::CalcTextSize(n);
    f32 ty = floorf(a.y + (h - ts.y) / 2), nx = a.x + w - kit_px(10) - ns.x;
    dl->PushClipRect(ImVec2(a.x, a.y), ImVec2(nx - kit_px(10), a.y + h), true);
    dl->AddText(ImVec2(i1.x + kit_px(10), ty), col32(active ? COL_ACCENT : COL_TEXT), label);
    dl->PopClipRect();
    dl->AddText(ImVec2(nx, ty), col32(COL_DIM), n);
    return clicked;
}

// A pill of the Library's header, all of one height. A plain one (Clips, Sessions) has its label in the middle and
// is lit when `on`. One that drops a list has an optional picture, its label from the left and an arrow on the
// right, and is outlined when `on`: when what it is set to narrows what is shown.
static bool lib_pill(const char *id, const char *label, f32 w, bool on, bool drops, ID3D11ShaderResourceView *picture = 0)
{
    f32 h = ImGui::GetFrameHeight(), round = ImGui::GetStyle().FrameRounding;
    ImVec2 a = ImGui::GetCursorScreenPos(), z(a.x + w, a.y + h);
    bool clicked = ImGui::InvisibleButton(id, ImVec2(w, h)) && !kit_closed_here(ImGui::GetItemID());
    if (clicked) dev_click("pill", label);
    bool hot = ImGui::IsItemHovered();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(a, z, col32(!drops && on ? 0x3a3d44 : hot ? COL_HOVER : COL_PANEL2), round);
    f32 ty = floorf(a.y + (h - g_font->FontSize) / 2);
    if (!drops) {
        ImFont *font = on ? g_font_bold : g_font;
        f32 tw = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0, label).x;
        dl->AddText(font, font->FontSize, ImVec2(floorf(a.x + (w - tw) / 2), ty), col32(on || hot ? COL_TEXT : COL_DIM), label);
        return clicked;
    }
    if (on) dl->AddRect(a, z, col32(COL_ACCENT, 0.7f), round, 0, kit_px(1));
    f32 x = a.x + kit_px(12), is = kit_px(20);
    if (picture) {
        ImVec2 i0(x - kit_px(3), floorf(a.y + (h - is) / 2));
        dl->AddImageRounded((ImTextureID)picture, i0, ImVec2(i0.x + is, i0.y + is), ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, kit_px(4));
        x = i0.x + is + kit_px(8);
    }
    f32 cw = g_font_small->CalcTextSizeA(g_font_small->FontSize, FLT_MAX, 0, ICON_DOWN).x, cx = z.x - kit_px(12) - cw;
    dl->AddText(g_font_small, g_font_small->FontSize, ImVec2(cx, floorf(a.y + (h - g_font_small->FontSize) / 2)), col32(COL_DIM), ICON_DOWN);
    dl->PushClipRect(ImVec2(x, a.y), ImVec2(cx - kit_px(6), z.y), true);
    dl->AddText(g_font, g_font->FontSize, ImVec2(x, ty), col32(COL_TEXT), label);
    dl->PopClipRect();
    return clicked;
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

static void library_play(const LibItem &it, f64 from)
{
    Library &lib = g_app.library;
    lib.playing = it;
    lib.player.id = 0;
    player_open(&lib.player, lib_item_path(it), from, true);
}

static void card_menu()
{
    Library &lib = g_app.library;
    bool rename = false, del = false;
    lib_popup_style_push(6);
    if (ImGui::BeginPopup("card_menu")) {
        const LibItem &it = lib.target;
        Path path = lib_item_path(it);
        if (lib_menu_item(ICON_PLAY, "Play")) library_play(it, 0);
        if (lib_menu_item(ICON_EDIT, "Edit")) editor_open(it);
        if (lib_menu_item(ICON_VIDEO, "Open in the default player")) ShellExecuteW(0, L"open", path.s, 0, 0, SW_SHOWNORMAL);
        if (lib_menu_item(ICON_COPY, "Copy the file")) {
            if (clipboard_copy_file(path.s)) lib_message(false, "Copied %s to the clipboard", it.name);
            else lib_message(true, "Couldn't copy to the clipboard");
        }
        if (lib_menu_item(ICON_FOLDER, "Show in folder")) show_in_folder(path.s);
        ImGui::Separator();
        if (lib_menu_item(ICON_RENAME, "Rename...")) rename = true;
        if (lib_menu_item(ICON_DELETE, "Delete...", true)) del = true;
        ImGui::EndPopup();
    }
    lib_popup_style_pop();
    if (rename) {
        str_set(lib.rename_buf, lib.target.name);
        lib.rename_error[0] = 0;
        ImGui::OpenPopup("Rename");
    }
    if (del) ImGui::OpenPopup("Delete");
}

// The buttons at the bottom right of a dialog: Cancel, then the action. 1 = the action, -1 = Cancel.
static i32 lib_dialog_buttons(const char *action, KitKind kind)
{
    f32 bw = kit_px(104);
    ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - 2 * bw - ImGui::GetStyle().ItemSpacing.x);
    i32 r = 0;
    if (kit_button(ICON_CLOSE "  Cancel", KIT_SECONDARY, ImVec2(bw, 0))) r = -1;
    ImGui::SameLine();
    if (kit_button(action, kind, ImVec2(bw, 0))) r = 1;
    return r;
}

static bool lib_dialog_begin(const char *id)
{
    ImGui::SetNextWindowSize(ImVec2(kit_px(480), 0));
    lib_popup_style_push(24);
    bool open = ImGui::BeginPopupModal(id, 0, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar);
    lib_popup_style_pop();
    return open;
}

static void rename_modal()
{
    Library &lib = g_app.library;
    if (!lib_dialog_begin("Rename")) return;
    kit_heading("Rename");
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(-1);
    bool enter = ImGui::InputText("##name", lib.rename_buf, sizeof lib.rename_buf, ImGuiInputTextFlags_EnterReturnsTrue);
    // The error line is always there, so the buttons never move.
    ImGui::PushFont(g_font_small);
    ImGui::PushStyleColor(ImGuiCol_Text, rgb(COL_ACCENT_HOVER));
    ImGui::TextUnformatted(lib.rename_error[0] ? lib.rename_error : " ");
    ImGui::PopStyleColor();
    ImGui::PopFont();
    i32 b = lib_dialog_buttons(ICON_RENAME "  Rename", KIT_PRIMARY);
    bool go = b == 1 || enter;
    bool cancel = b == -1 || ImGui::IsKeyPressed(ImGuiKey_Escape);
    if (go) {
        char name[256], cleaned[256];
        str_set(name, lib.rename_buf);
        char *t = trim(name);
        str_set(cleaned, t);
        clean_name(cleaned);
        if (!*t) str_set(lib.rename_error, "The name can't be empty.");
        else if (strcmp(cleaned, t)) str_set(lib.rename_error, "A name can't hold < > : \" / \\ | ? * or end in a dot.");
        else if (!strcmp(t, lib.target.name)) cancel = true;
        else {
            LibItem renamed = lib.target;
            str_set(renamed.name, t);
            Path from = lib_item_path(lib.target), to = lib_item_path(renamed);
            if (file_exists(to.s)) str_set(lib.rename_error, "There's already a file with that name.");
            else {
                if (lib.player.open && !wcscmp(lib.player.path.s, from.s)) library_close_player();
                if (MoveFileW(from.s, to.s)) {
                    // The edit goes with it.
                    Path ef = lib_edit_path(lib.target), et = lib_edit_path(renamed);
                    if (file_exists(ef.s)) MoveFileW(ef.s, et.s);
                    lib_message(false, "Renamed to %s", t);
                    library_rescan();
                    cancel = true;
                } else {
                    snprintf(lib.rename_error, sizeof lib.rename_error,
                             "Couldn't rename it (error %d). Is it open somewhere?", (int)GetLastError());
                }
            }
        }
    }
    if (cancel) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

static void delete_modal()
{
    Library &lib = g_app.library;
    if (!lib_dialog_begin("Delete")) return;
    kit_heading("Delete?");
    ImGui::PushFont(g_font_bold);
    ImVec2 p = ImGui::GetCursorScreenPos();
    f32 w = ImGui::GetContentRegionAvail().x;
    ImGui::Dummy(ImVec2(w, ImGui::GetTextLineHeight()));
    text_clipped(ImGui::GetWindowDrawList(), p, w, col32(COL_TEXT), lib.target.name);
    ImGui::PopFont();
    kit_dim("It goes to the Recycle Bin, with its edit.");
    ImGui::Spacing();
    i32 b = lib_dialog_buttons(ICON_DELETE "  Delete", KIT_PRIMARY);
    bool cancel = b == -1 || ImGui::IsKeyPressed(ImGuiKey_Escape);
    if (b == 1) {
        Path path = lib_item_path(lib.target), edit = lib_edit_path(lib.target);
        if (lib.player.open && !wcscmp(lib.player.path.s, path.s)) library_close_player();
        // A list of paths ending in two nulls: the video, and its edit if there is one.
        static wchar_t from[2 * MAX_PATH + 4];
        memset(from, 0, sizeof from);
        size_t n = wcslen(path.s);
        memcpy(from, path.s, n * sizeof(wchar_t));
        if (file_exists(edit.s)) memcpy(from + n + 1, edit.s, wcslen(edit.s) * sizeof(wchar_t));
        SHFILEOPSTRUCTW op = {};
        op.hwnd = g_app.ui.hwnd;
        op.wFunc = FO_DELETE;
        op.pFrom = from;
        op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_WANTNUKEWARNING | FOF_SILENT;
        int r = SHFileOperationW(&op);
        if (r == 0 && !op.fAnyOperationsAborted) lib_message(false, "%s went to the Recycle Bin", lib.target.name);
        else lib_message(true, "Couldn't delete %s (error %d)", lib.target.name, r);
        library_rescan();
        cancel = true;
    }
    if (cancel) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

// The name and the line under a card's picture.
static f32 lib_meta_h() { return kit_px(10) + g_font_bold->FontSize + kit_px(3) + g_font_small->FontSize + kit_px(4); }

// One card: the day above it (on the first card of a day in each row), the picture, the name and one dim line.
// Under the mouse the picture scrubs through the file (a click plays from there) and a "more" button opens the
// same menu as a right-click.
static void lib_card(const LibItem &it, ImVec2 a, f32 w, f32 thumb_h, f32 day_h, bool show_day)
{
    Library &lib = g_app.library;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    f32 meta_h = lib_meta_h();
    if (show_day && (lib.sort == SORT_NEWEST || lib.sort == SORT_OLDEST)) {
        char day[64];
        fmt_day(it.mtime_us, day, sizeof day);
        ImGui::PushFont(g_font_bold);
        dl->AddText(ImVec2(a.x, a.y + day_h - ImGui::GetFontSize() - kit_px(10)), col32(COL_DIM), day);
        ImGui::PopFont();
    }
    ImVec2 t0(a.x, a.y + day_h), t1(a.x + w, a.y + day_h + thumb_h);
    ImGui::SetCursorScreenPos(t0);
    ImGui::PushID((const void *)(uintptr_t)it.key);
    ImGui::SetNextItemAllowOverlap(); // the "more" button sits on top
    ImGui::InvisibleButton("card", ImVec2(w, thumb_h + meta_h));
    bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    bool menu = ImGui::IsItemClicked(ImGuiMouseButton_Right);
    bool over = ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(t0, ImVec2(t1.x, t1.y + meta_h));
    f32 round = kit_px(10);
    LibThumb *th = lib_thumb(it);
    f32 frac = -1;
    if (over) {
        frac = (ImGui::GetIO().MousePos.x - t0.x) / w;
        frac = frac < 0 ? 0 : frac > 0.999f ? 0.999f : frac;
    }
    bool scrub = over && lib_want_strip(it);
    i32 cover_w = 0, cover_h = 0;
    ID3D11ShaderResourceView *cover = it.kind == LIB_SESSIONS ? game_db_cover(it.info.game, &cover_w, &cover_h) : 0;
    if (cover && (cover_w <= 0 || cover_h <= 0)) cover = 0;
    if (scrub) {
        i32 f = (i32)(frac * lib.strip_frames);
        f32 u0 = (f32)f / lib.strip_frames, u1 = (f32)(f + 1) / lib.strip_frames;
        dl->AddImageRounded((ImTextureID)(intptr_t)lib.strip, t0, t1, ImVec2(u0, 0), ImVec2(u1, 1), IM_COL32_WHITE, round);
    } else if (cover) { // a session shows its game's cover, cut to the card's shape about its middle
        f32 want = w / thumb_h, have = (f32)cover_w / cover_h, u = 0, v = 0;
        if (have > want) u = (1 - want / have) / 2;
        else v = (1 - have / want) / 2;
        dl->AddImageRounded((ImTextureID)(intptr_t)cover, t0, t1, ImVec2(u, v), ImVec2(1 - u, 1 - v), IM_COL32_WHITE, round);
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
    bool more = false;
    if (over) {
        dl->AddRectFilled(t0, t1, IM_COL32(0, 0, 0, 50), round);
        lib_play_mark(dl, ImVec2((t0.x + t1.x) / 2, (t0.y + t1.y) / 2), kit_px(24));
        // Where a click starts playing: a bar along the bottom, and the time.
        f32 in = kit_px(10), by = t1.y - kit_px(8), bh = kit_px(3);
        dl->AddRectFilled(ImVec2(t0.x + in, by - bh), ImVec2(t1.x - in, by), IM_COL32(255, 255, 255, 70), bh);
        dl->AddRectFilled(ImVec2(t0.x + in, by - bh), ImVec2(t0.x + in + frac * (w - 2 * in), by), col32(COL_ACCENT), bh);
        if (it.info.duration > 0) {
            fmt_time(frac * it.info.duration, text, sizeof text);
            badge(dl, ImVec2(t1.x - in, by - bh - kit_px(6)), true, true, text);
        }
        if (it.info.track_count > 1) {
            snprintf(text, sizeof text, "%d tracks", it.info.track_count);
            badge(dl, ImVec2(t0.x + in, t0.y + in), false, false, text);
        }
        more = lib_float_button(ICON_MORE, "##more", ImVec2(t1.x - kit_px(24), t0.y + kit_px(24)), kit_px(16));
    } else if (it.info.duration > 0) {
        fmt_time(it.info.duration, text, sizeof text);
        badge(dl, ImVec2(t1.x - kit_px(8), t1.y - kit_px(8)), true, true, text);
    }
    ImGui::PopID();
    if (menu || more) { // opened outside the card's ID, where card_menu() looks for it
        lib.target = it;
        lib.open_menu = true;
    }
    f32 y = t1.y + kit_px(10);
    ImGui::PushFont(g_font_bold);
    text_clipped(dl, ImVec2(a.x, y), w, col32(COL_TEXT), it.name);
    y += ImGui::GetFontSize() + kit_px(3);
    ImGui::PopFont();
    char when[48], size[24], line[160];
    fmt_when(it.mtime_us, when, sizeof when);
    fmt_size(it.size, size, sizeof size);
    snprintf(line, sizeof line, "%s  \xC2\xB7  %s  \xC2\xB7  %s", it.info.game, when, size);
    ImGui::PushFont(g_font_small);
    text_clipped(dl, ImVec2(a.x, y), w, col32(COL_DIM), line);
    ImGui::PopFont();
    if (clicked) library_play(it, it.info.duration > 0 && frac >= 0 ? frac * it.info.duration : 0);
}

// The library player's volume: one for every file, kept while clipback runs.
static f32 g_lib_volume = 1;
static bool g_lib_muted;
static u32 g_lib_volume_gen = 0xffffffff; // the player gen it was last given to

static void lib_player_view()
{
    Library &lib = g_app.library;
    Player &p = lib.player;
    const ImGuiStyle &st = ImGui::GetStyle();
    player_update(&p);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    f32 fh = ImGui::GetFrameHeight(), full_w = ImGui::GetContentRegionAvail().x;

    // Header: back, the name and when, and the actions on the right.
    ImVec2 h0 = ImGui::GetCursorScreenPos();
    f32 head_h = fmaxf(fh, g_font_big->FontSize + g_font_small->FontSize + kit_px(4));
    ImGui::SetCursorScreenPos(ImVec2(h0.x, h0.y + (head_h - fh) / 2));
    bool close = kit_icon_button(ICON_BACK, "##back", KIT_GHOST);
    static const char *const actions[] = {ICON_EDIT "  Edit", ICON_FOLDER "  Show in folder"};
    f32 aw[2], acts_w = 0;
    for (i32 i = 0; i < 2; i++) {
        aw[i] = ImGui::CalcTextSize(actions[i]).x + 2 * st.FramePadding.x;
        acts_w += aw[i] + (i ? st.ItemSpacing.x : 0);
    }
    f32 tx = h0.x + fh + kit_px(14), tw = h0.x + full_w - acts_w - kit_px(24) - tx;
    f32 ty = h0.y + (head_h - g_font_big->FontSize - g_font_small->FontSize - kit_px(4)) / 2;
    ImGui::PushFont(g_font_big);
    text_clipped(dl, ImVec2(tx, ty), tw, col32(COL_TEXT), lib.playing.name);
    ImGui::PopFont();
    char when[48], line[160];
    fmt_when(lib.playing.mtime_us, when, sizeof when);
    snprintf(line, sizeof line, "%s  \xC2\xB7  %s", lib.playing.info.game, when);
    ImGui::PushFont(g_font_small);
    text_clipped(dl, ImVec2(tx, ty + g_font_big->FontSize + kit_px(4)), tw, col32(COL_DIM), line);
    ImGui::PopFont();
    ImGui::SetCursorScreenPos(ImVec2(h0.x + full_w - acts_w, h0.y + (head_h - fh) / 2));
    bool edit = kit_button(actions[0], KIT_SECONDARY, ImVec2(aw[0], 0));
    ImGui::SameLine();
    if (kit_button(actions[1], KIT_SECONDARY, ImVec2(aw[1], 0))) show_in_folder(p.path.s);
    if (edit) {
        LibItem it = lib.playing;
        library_close_player();
        editor_open(it);
        return;
    }

    // The picture, as big as fits, on black.
    ImGui::SetCursorScreenPos(ImVec2(h0.x, h0.y + head_h + kit_px(18)));
    f32 bar_h = fh + kit_px(16);
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImVec2 area(full_w, ImGui::GetContentRegionAvail().y - bar_h - kit_px(12));
    if (area.y < 1) area.y = 1;
    dl->AddRectFilled(pos, ImVec2(pos.x + area.x, pos.y + area.y), IM_COL32(0, 0, 0, 255), kit_px(10));
    ImVec2 a, b;
    player_fit(p, pos, area, &a, &b);
    ImGui::InvisibleButton("video", area);
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) player_toggle(&p);
    player_draw(p, a, b);
    if (p.loaded && p.paused && !p.error[0]) lib_play_mark(dl, ImVec2((a.x + b.x) / 2, (a.y + b.y) / 2), kit_px(34));

    // The transport bar: play, the time, the seek bar, the sound, the volume. Fixed widths but the seek bar's.
    ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + area.y + kit_px(12)));
    ImVec2 r0 = ImGui::GetCursorScreenPos();
    dl->AddRectFilled(r0, ImVec2(r0.x + full_w, r0.y + bar_h), col32(COL_PANEL), kit_px(10));
    f32 pad = kit_px(8), y = r0.y + (bar_h - fh) / 2;
    ImGui::SetCursorScreenPos(ImVec2(r0.x + pad, y));
    if (kit_icon_button(p.paused ? ICON_PLAY : ICON_PAUSE, "##play", KIT_GHOST)) player_toggle(&p);
    char t1[32], t2[32], tm[80];
    fmt_time(p.pos, t1, sizeof t1);
    fmt_time(p.duration > 0 ? p.duration : lib.playing.info.duration, t2, sizeof t2);
    snprintf(tm, sizeof tm, "%s / %s", t1, t2);
    f32 time_x = r0.x + pad + fh + kit_px(8), time_w = kit_px(104);
    ImVec2 ts = ImGui::CalcTextSize(tm);
    dl->AddText(ImVec2(time_x, floorf(y + (fh - ts.y) / 2)), col32(COL_DIM), tm);

    const MediaInfo &mi = lib.playing.info;
    i32 tracks = p.audio_count ? p.audio_count : mi.track_count;
    f32 vol_w = kit_px(96), sound_w = tracks > 1 ? kit_px(180) : 0;
    f32 right_w = sound_w + (sound_w > 0 ? kit_px(12) : 0) + fh + vol_w;
    f32 seek_x = time_x + time_w + kit_px(8), seek_w = r0.x + full_w - pad - right_w - kit_px(16) - seek_x;
    if (seek_w < kit_px(60)) seek_w = kit_px(60);
    ImGui::SetCursorScreenPos(ImVec2(seek_x, y));
    scrub_bar("##seek", &p, seek_w, fh);
    f32 x = r0.x + full_w - pad - right_w;
    if (tracks > 1) { // which audio track is heard
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        char cur[64];
        snprintf(cur, sizeof cur, "%s", p.sound < mi.track_count ? mi.tracks[p.sound] : "Track 1");
        ImGui::SetNextItemWidth(sound_w);
        ImGui::PushStyleColor(ImGuiCol_PopupBg, rgb(COL_PANEL2));
        if (ImGui::BeginCombo("##sound", cur)) {
            for (i32 t = 0; t < tracks && t < MEDIA_MAX_TRACKS; t++) {
                char name[48];
                if (t < mi.track_count) snprintf(name, sizeof name, "%s##%d", mi.tracks[t], t);
                else snprintf(name, sizeof name, "Track %d##%d", t + 1, t);
                if (ImGui::Selectable(name, p.sound == t)) player_set_sound(&p, t);
            }
            ImGui::EndCombo();
        }
        ImGui::PopStyleColor();
        x += sound_w + kit_px(12);
    }
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    bool vol_changed = false;
    if (kit_icon_button(g_lib_muted || g_lib_volume <= 0 ? ICON_MUTE : ICON_VOLUME, "##mute", KIT_GHOST)) {
        g_lib_muted = !g_lib_muted;
        vol_changed = true;
    }
    ImGui::SetCursorScreenPos(ImVec2(x + fh, y));
    if (lib_slider("##volume", &g_lib_volume, 0, 1, vol_w, fh)) {
        g_lib_muted = false;
        vol_changed = true;
    }
    if (p.engine && p.loaded && (vol_changed || g_lib_volume_gen != p.gen)) {
        p.engine->SetVolume(g_lib_volume);
        p.engine->SetMuted(g_lib_muted);
        g_lib_volume_gen = p.gen;
    }

    if (!ImGui::GetIO().WantTextInput && !ImGui::IsPopupOpen((const char *)0, ImGuiPopupFlags_AnyPopupId)) {
        if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) player_toggle(&p);
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) player_seek(&p, p.pos - 5);
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) player_seek(&p, p.pos + 5);
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) close = true;
    }
    if (close) library_close_player();
}

static void lib_set_view(LibView v)
{
    Library &lib = g_app.library;
    if (lib.view == v) return;
    lib.view = v;
    lib.game[0] = 0;
    lib.scroll = lib.scroll_to = 0;
    lib.chip_page = 0;
    lib_refilter();
}

// A game chip's label: the name, cut short when it's long.
static void lib_chip_label(const char *name, char *out, i32 cap)
{
    if (strlen(name) <= 22) snprintf(out, cap, "%s", name);
    else snprintf(out, cap, "%.20s\xE2\x80\xA6", name); // cut by bytes; game names are mostly ASCII
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
    snprintf(out, cap, "%d %.3s", t.wDay, LIB_MONTHS[(t.wMonth + 11) % 12]);
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

    // ‹  November 2026  ›
    ImVec2 a = ImGui::GetCursorScreenPos();
    if (kit_icon_button(ICON_LEFT, "##prevm")) lib.cal_month--;
    char title[32];
    snprintf(title, sizeof title, "%s %d", LIB_MONTHS[m - 1], y);
    ImGui::PushFont(g_font_bold);
    ImVec2 ts = ImGui::CalcTextSize(title);
    ImGui::GetWindowDrawList()->AddText(ImVec2(floorf(a.x + (w - ts.x) / 2), floorf(a.y + (fh - ts.y) / 2)), col32(COL_TEXT), title);
    ImGui::PopFont();
    ImGui::SetCursorScreenPos(ImVec2(a.x + w - fh, a.y));
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
// Unsaved sessions: a row of tiles at the top of Sessions, paged when there are more than fit.


static void lib_unsaved_tile(u32 id, ImVec2 a, f32 w, f32 h)
{
    Sessions &ss = g_app.sessions;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    f32 fh = ImGui::GetFrameHeight(), pad = kit_px(14), is = kit_px(40);
    dl->AddRectFilled(a, ImVec2(a.x + w, a.y + h), col32(COL_PANEL), kit_px(10));
    Session *s = session_find(id);
    ImGui::PushID((i32)id);
    if (!s) {
        dl->AddText(ImVec2(a.x + pad, floorf(a.y + (h - ImGui::GetFontSize()) / 2)), col32(COL_FAINT), "Gone from the buffer");
        ImGui::PopID();
        return;
    }

    // The game's icon, or a game tile
    ImVec2 ip(a.x + pad, floorf(a.y + (h - is) / 2));
    ID3D11ShaderResourceView *icon = game_db_icon(s->name);
    if (!icon && s->exe[0]) icon = app_icon(s->exe);
    if (icon) dl->AddImageRounded((ImTextureID)icon, ip, ImVec2(ip.x + is, ip.y + is), ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, kit_px(8));
    else {
        dl->AddRectFilled(ip, ImVec2(ip.x + is, ip.y + is), col32(COL_ACCENT_SOFT), kit_px(8));
        ImGui::PushFont(g_font_icon);
        ImVec2 gs = ImGui::CalcTextSize(ICON_GAME);
        dl->AddText(ImVec2(floorf(ip.x + (is - gs.x) / 2), floorf(ip.y + (is - gs.y) / 2)), col32(COL_ACCENT), ICON_GAME);
        ImGui::PopFont();
    }

    // Buttons on the right: Save, and Dismiss (Undo once dismissed)
    f32 bx = a.x + w - pad - fh, by = floorf(a.y + (h - fh) / 2);
    bool dismissed = s->state == SES_DISMISSED, saved = s->state == SES_SAVED, saving = s->state == SES_SAVING;
    ImGui::SetCursorScreenPos(ImVec2(bx, by));
    if (kit_icon_button(dismissed ? ICON_BACK : ICON_CLOSE, "##dismiss", KIT_GHOST, !saving && !saved)) {
        s->state = dismissed ? SES_NOT_SAVED : SES_DISMISSED;
        ss.dirty = true;
        log_line("session %u (%s): %s", s->id, s->name, dismissed ? "back under unsaved sessions" : "dismissed");
    }
    bx -= fh + kit_px(6);
    ImGui::SetCursorScreenPos(ImVec2(bx, by));
    if (kit_icon_button(saved ? ICON_CHECK : ICON_SAVE, "##save", saved ? KIT_GHOST : KIT_PRIMARY, !saving && !saved && !dismissed))
        session_save(s);
    bx -= fh + kit_px(6);
    ImGui::SetCursorScreenPos(ImVec2(bx, by));
    if (kit_icon_button(ICON_PLAY, "##review", KIT_GHOST, !saved)) { // look at it on the timeline first
        timeline_review(s->start_us, s->end_us, s->name);
        ImGui::PopID();
        return;
    }

    // Name; length and times; what happens next
    f32 tx = ip.x + is + kit_px(12), tw = bx - kit_px(10) - tx;
    f32 lh = g_font_bold->FontSize + kit_px(2) + g_font_small->FontSize * 2 + kit_px(2);
    f32 ty = floorf(a.y + (h - lh) / 2);
    ImGui::PushFont(g_font_bold);
    text_clipped(dl, ImVec2(tx, ty), tw, col32(dismissed ? COL_DIM : COL_TEXT), s->name);
    ImGui::PopFont();
    ty += g_font_bold->FontSize + kit_px(2);
    char len[32], from[32], to[32], line[128];
    fmt_length(s->end_us - s->start_us, len, sizeof len);
    fmt_clock(s->start_us, from, sizeof from);
    fmt_clock(s->end_us, to, sizeof to);
    snprintf(line, sizeof line, "%s  \xC2\xB7  %s \xE2\x80\x93 %s", len, from, to);
    ImGui::PushFont(g_font_small);
    text_clipped(dl, ImVec2(tx, ty), tw, col32(COL_DIM), line);
    ty += g_font_small->FontSize + kit_px(2);
    u32 col = COL_FAINT;
    if (dismissed) str_set(line, "Dismissed");
    else if (saved) str_set(line, "Saved");
    else if (s->state == SES_ASKING) str_set(line, "Asking in the popup");
    else if (saving) {
        const SaveJobView *v = save_view(s->job);
        if (v && v->state == SAVE_RUNNING) snprintf(line, sizeof line, "Saving %d%%", (int)(v->progress * 100));
        else str_set(line, "Waiting to save");
        col = COL_DIM;
        f32 p = v ? v->progress : 0;
        dl->AddRectFilled(ImVec2(a.x + kit_px(10), a.y + h - kit_px(4)), ImVec2(a.x + kit_px(10) + p * (w - kit_px(20)), a.y + h - kit_px(2)),
                          col32(COL_ACCENT), kit_px(1));
    } else {
        session_time_left(*s, line, sizeof line);
        col = COL_WARN;
    }
    text_clipped(dl, ImVec2(tx, ty), tw, col32(col), line);
    ImGui::PopFont();
    ImGui::PopID();
}

// The row, drawn from the cursor; returns false (and draws nothing) when there are none.
static bool lib_unsaved_row(f32 full_w)
{
    Sessions &ss = g_app.sessions;
    sessions_unsaved_take();
    if (ss.row_count == 0) return false;
    f32 gap = kit_px(16), tile_h = kit_px(78), fh = ImGui::GetFrameHeight();
    i32 per = (i32)((full_w + gap) / (kit_px(320) + gap));
    if (per < 1) per = 1;
    i32 pages = (ss.row_count + per - 1) / per;
    if (ss.page >= pages) ss.page = pages - 1;
    if (ss.page < 0) ss.page = 0;
    f32 tile_w = floorf((full_w - (per - 1) * gap) / per);

    ImVec2 a = ImGui::GetCursorScreenPos();
    char head[48];
    snprintf(head, sizeof head, "UNSAVED SESSIONS  %d", ss.row_count);
    ImGui::SetCursorScreenPos(ImVec2(a.x, a.y + (fh - g_font_small->FontSize) / 2));
    kit_label(head);
    if (pages > 1) {
        ImGui::SetCursorScreenPos(ImVec2(a.x + full_w - lib_pager_w(), a.y));
        lib_pager("unsaved", &ss.page, pages);
    }
    f32 y = a.y + fh + kit_px(6);
    for (i32 k = 0; k < per; k++) {
        i32 r = ss.page * per + k;
        if (r >= ss.row_count) break;
        lib_unsaved_tile(ss.rows[r], ImVec2(a.x + k * (tile_w + gap), y), tile_w, tile_h);
    }
    ImGui::SetCursorScreenPos(ImVec2(a.x, y + tile_h + kit_px(18)));
    return true;
}

static void screen_library()
{
    Library &lib = g_app.library;
    if (!lib.scanned && !lib.scanning) library_rescan();
    if (lib.player.open) {
        lib_player_view();
        return;
    }
    ImGuiIO &io = ImGui::GetIO();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    f32 full_w = ImGui::GetContentRegionAvail().x, fh = ImGui::GetFrameHeight();

    // Header: pills of one height. Clips and Sessions, then three that each drop a list (the game, the date range,
    // the order), and the search on the right. A list that is narrowing what's shown is outlined.
    ImVec2 h0 = ImGui::GetCursorScreenPos();
    static const char *const views[] = {ICON_VIDEO "  Clips", ICON_GAME "  Sessions"};
    static const char *const ranges[] = {"All time", "Today", "This week", "This month", "Custom"};
    static const char *const sorts[] = {"Newest", "Oldest", "Longest", "Biggest"};
    f32 hgap = kit_px(8), view_w = kit_px(104), game_w = kit_px(168), drop_w = kit_px(112);
    for (i32 v = 0; v < 2; v++) {
        ImGui::SetCursorScreenPos(ImVec2(h0.x + v * (view_w + kit_px(4)), h0.y));
        if (lib_pill(v ? "##sessions" : "##clips", views[v], view_w, lib.view == v, false) && lib.view != v) lib_set_view((LibView)v);
    }

    // The game: every game in this view with how many it has, most recent first, and a box to find one by name
    ImVec2 game_at(h0.x + 2 * view_w + kit_px(4) + kit_px(16), h0.y);
    ImGui::SetCursorScreenPos(game_at);
    static char game_find[64];
    if (lib_pill("##game", lib.game[0] ? lib.game : "All games", game_w, lib.game[0] != 0, true, lib.game[0] ? game_icon(lib.game) : 0)) {
        game_find[0] = 0;
        ImGui::OpenPopup("game");
    }
    i32 step = 0;
    for (i32 c = 0; c < lib.game_count; c++)
        if (!strcmp(lib.game, lib.games[c])) step = c + 1;
    if (kit_wheel_step(&step, lib.game_count + 1)) {
        if (step == 0) lib.game[0] = 0;
        else str_set(lib.game, lib.games[step - 1]);
        lib.scroll = lib.scroll_to = 0;
        lib_refilter();
    }
    ImGui::SetNextWindowPos(ImVec2(game_at.x, game_at.y + fh + kit_px(6)));
    lib_popup_style_push(8);
    bool game_open = ImGui::BeginPopup("game");
    lib_popup_style_pop();
    if (game_open) {
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::PushStyleColor(ImGuiCol_FrameBg, rgb(0x2a2c32));
        kit_search("##find", game_find, sizeof game_find, "Find a game", kit_px(228));
        bool enter = ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_Enter, false);
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, kit_px(2)));
        // The list scrolls under the box; Enter picks the first one shown
        i32 shown = 0, pick = -1;
        for (i32 c = 0; c <= lib.game_count; c++)
            if (c == 0 ? !game_find[0] : lib_matches(lib.games[c - 1], game_find)) shown++;
        f32 item_h = kit_px(30) + kit_px(2);
        ImGui::BeginChild("##games", ImVec2(kit_px(228), (shown < 1 ? 1 : shown > 10 ? 10 : shown) * item_h), 0, ImGuiWindowFlags_NoBackground);
        if (!shown) kit_dim("  No game matches");
        for (i32 c = 0; c <= lib.game_count; c++) {
            const char *name = c == 0 ? "All games" : lib.games[c - 1];
            if (c == 0 ? game_find[0] != 0 : !lib_matches(name, game_find)) continue;
            bool active = c == 0 ? !lib.game[0] : !strcmp(lib.game, name);
            ImGui::PushID(c);
            if (lib_game_item(c == 0 ? 0 : name, name, c == 0 ? lib.range_count : lib.game_items[c - 1], active) || enter) pick = c;
            ImGui::PopID();
            if (pick >= 0) break;
        }
        ImGui::EndChild();
        if (pick >= 0) {
            if (pick == 0) lib.game[0] = 0;
            else str_set(lib.game, lib.games[pick - 1]);
            lib.scroll = lib.scroll_to = 0;
            lib_refilter();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // The date range; Custom opens the calendar
    ImVec2 range_at(game_at.x + game_w + hgap, h0.y);
    ImGui::SetCursorScreenPos(range_at);
    if (lib_pill("##range", ranges[lib.range], drop_w, lib.range != 0, true)) ImGui::OpenPopup("range");
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
    if (lib_pill("##sort", sorts[lib.sort], drop_w, false, true)) ImGui::OpenPopup("sort");
    step = lib.sort;
    if (kit_wheel_step(&step, 4)) {
        lib.sort = step;
        lib.scroll = lib.scroll_to = 0;
        lib_refilter();
    }
    ImGui::SetNextWindowPos(ImVec2(sort_at.x, sort_at.y + fh + kit_px(6)));
    lib_popup_style_push(8);
    bool sort_open = ImGui::BeginPopup("sort");
    lib_popup_style_pop();
    if (sort_open) {
        for (i32 i = 0; i < 4; i++)
            if (lib_menu_item(lib.sort == i ? ICON_CHECK : "", sorts[i], false, kit_px(132)) && lib.sort != i) {
                lib.sort = i;
                lib.scroll = lib.scroll_to = 0;
                lib_refilter();
            }
        ImGui::EndPopup();
    }
    ImGui::SetCursorScreenPos(ImVec2(h0.x + full_w - search_w, h0.y));
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F, false) && !ImGui::IsPopupOpen("game")) ImGui::SetKeyboardFocusHere();
    if (kit_search("##search", lib.search, sizeof lib.search, "Search", search_w)) {
        lib.scroll = lib.scroll_to = 0;
        lib_refilter();
    }
    ImGui::SetCursorScreenPos(ImVec2(h0.x, h0.y + fh + kit_px(16)));

    if (lib.view == LIB_SESSIONS) lib_unsaved_row(full_w);


    // The grid fills what's left above the footer and scrolls: the wheel, Page Up / Page Down, Home / End, or the
    // bar on its right.
    f32 foot_h = fh;
    ImVec2 g0 = ImGui::GetCursorScreenPos();
    f32 grid_h = ImGui::GetContentRegionAvail().y - foot_h - kit_px(8);
    if (grid_h < 1) grid_h = 1;
    // As many columns as bring the cards nearest the width asked for; Ctrl + wheel asks for one column fewer or
    // more (never cards under 140 wide), keeping the row that was at the top there.
    const f32 narrowest = kit_px(140);
    f32 gap = kit_px(20), day_h = kit_px(36);
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
    f32 bar_w = kit_px(14);
    ImGui::BeginChild("##grid", ImVec2(full_w + bar_w, grid_h), 0,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground);
    dl = ImGui::GetWindowDrawList();
    if (!popup && !ctrl && io.MouseWheel != 0 && ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem))
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
    for (i32 r = r0; r <= r1 && r < rows; r++) {
        i64 prev_day = -1;
        for (i32 c = 0; c < cols; c++) {
            i32 idx = r * cols + c;
            if (idx >= lib.shown_count) break;
            const LibItem &it = lib.items[lib.shown[idx]];
            i64 day = local_day(it.mtime_us, 0);
            bool show_day = c == 0 || day != prev_day;
            prev_day = day;
            lib_card(it, ImVec2(g0.x + c * (card_w + gap), g0.y + r * card_h - top), card_w, thumb_h, day_h, show_day);
        }
    }
    ImGui::SetCursorScreenPos(g0); // the cards above and below the view don't count as this window's content
    ImGui::EndChild();
    dl = ImGui::GetWindowDrawList();
    if (lib.open_menu) ImGui::OpenPopup("card_menu");
    lib.open_menu = false;
    card_menu();
    rename_modal();
    delete_modal();

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
    f32 msg_x = f0.x + kit_px(320);
    text_clipped(dl, ImVec2(msg_x, ty), f0.x + full_w - msg_x, col32(lib.message_error ? 0xff8a92 : COL_DIM), lib.message);
    ImGui::SetCursorScreenPos(ImVec2(f0.x, f0.y + fh));
}

