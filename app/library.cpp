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
    i32 n = 0;
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
                    if (it->probed && tagged[0]) str_set(it->info.game, tagged);
                    else game_from_name(it->name, it->info.game, sizeof it->info.game);
                } while (FindNextFileW(h, &f));
                FindClose(h);
            }
        } while (FindNextFileW(years, &fd));
        FindClose(years);
    }
    qsort(items, n, sizeof items[0], lib_item_newer);
    lib_publish(items, n);

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
        if (tagged[0]) str_set(it->info.game, tagged);
        else game_from_name(it->name, it->info.game, sizeof it->info.game);
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

// The view's game chips and the cards that match the chip and the search.
static void lib_refilter()
{
    Library &lib = g_app.library;
    lib.game_count = 0;
    lib.view_count = 0;
    lib.shown_count = 0;
    i32 desktop = 0, other = 0;
    for (i32 i = 0; i < lib.item_count; i++) {
        const LibItem &it = lib.items[i];
        if (it.kind != lib.view) continue;
        lib.view_count++;
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

static ID3D11ShaderResourceView *lib_texture(const u32 *pixels, i32 w, i32 h)
{
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = (UINT)w;
    d.Height = (UINT)h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_IMMUTABLE;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data = {pixels, (UINT)w * 4, 0};
    ID3D11Texture2D *tex = 0;
    ID3D11ShaderResourceView *srv = 0;
    if (FAILED(g_gpu.device->CreateTexture2D(&d, &data, &tex))) return 0;
    g_gpu.device->CreateShaderResourceView(tex, 0, &srv);
    tex->Release();
    return srv;
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
                lib.strip = lib_texture(r.pixels, r.w, r.h);
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
                if (!r.failed) t->srv = lib_texture(r.pixels, r.w, r.h);
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

// Text cut with "..." to fit `width`.
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

static void badge(ImDrawList *dl, ImVec2 corner, bool right, bool bottom, const char *text)
{
    f32 pad = 6 * g_dpi_scale;
    ImVec2 ts = ImGui::CalcTextSize(text);
    ImVec2 a(right ? corner.x - ts.x - 2 * pad : corner.x, bottom ? corner.y - ts.y - pad : corner.y);
    dl->AddRectFilled(a, ImVec2(a.x + ts.x + 2 * pad, a.y + ts.y + pad), IM_COL32(0, 0, 0, 170), 4 * g_dpi_scale);
    dl->AddText(ImVec2(a.x + pad, a.y + pad / 2), IM_COL32(235, 235, 235, 255), text);
}

// A button that's drawn (and keeps its place) even when it does nothing.
static bool button_enabled(const char *label, ImVec2 size, bool enabled)
{
    ImGui::BeginDisabled(!enabled);
    bool clicked = ImGui::Button(label, size);
    ImGui::EndDisabled();
    return clicked && enabled;
}

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
    if (ImGui::BeginPopup("card_menu")) {
        const LibItem &it = lib.target;
        Path path = lib_item_path(it);
        if (ImGui::MenuItem("Play")) library_play(it, 0);
        if (ImGui::MenuItem("Edit")) editor_open(it);
        if (ImGui::MenuItem("Open in the default player")) ShellExecuteW(0, L"open", path.s, 0, 0, SW_SHOWNORMAL);
        if (ImGui::MenuItem("Copy the file")) {
            if (clipboard_copy_file(path.s)) lib_message(false, "Copied %s to the clipboard", it.name);
            else lib_message(true, "Couldn't copy to the clipboard");
        }
        if (ImGui::MenuItem("Show in folder")) show_in_folder(path.s);
        ImGui::Separator();
        if (ImGui::MenuItem("Rename...")) rename = true;
        if (ImGui::MenuItem("Delete...")) del = true;
        ImGui::EndPopup();
    }
    if (rename) {
        str_set(lib.rename_buf, lib.target.name);
        lib.rename_error[0] = 0;
        ImGui::OpenPopup("Rename");
    }
    if (del) ImGui::OpenPopup("Delete");
}

static void rename_modal()
{
    Library &lib = g_app.library;
    f32 s = g_dpi_scale;
    ImGui::SetNextWindowSize(ImVec2(520 * s, 0));
    if (!ImGui::BeginPopupModal("Rename", 0, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) return;
    ImGui::TextUnformatted("New name (the file's type stays the same):");
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(-1);
    bool enter = ImGui::InputText("##name", lib.rename_buf, sizeof lib.rename_buf, ImGuiInputTextFlags_EnterReturnsTrue);
    // The error line is always there, so the buttons never move.
    ImGui::PushStyleColor(ImGuiCol_Text, rgb(COL_ACCENT));
    ImGui::TextUnformatted(lib.rename_error[0] ? lib.rename_error : " ");
    ImGui::PopStyleColor();
    f32 bw = 100 * s;
    ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - 2 * bw - ImGui::GetStyle().ItemSpacing.x);
    bool go = ImGui::Button("Rename", ImVec2(bw, 0)) || enter;
    ImGui::SameLine();
    bool cancel = ImGui::Button("Cancel", ImVec2(bw, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape);
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
    f32 s = g_dpi_scale;
    ImGui::SetNextWindowSize(ImVec2(520 * s, 0));
    if (!ImGui::BeginPopupModal("Delete", 0, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) return;
    ImGui::PushFont(g_font_bold);
    ImGui::TextUnformatted(lib.target.name);
    ImGui::PopFont();
    ImGui::TextUnformatted("This goes to the Recycle Bin, with its edit.");
    ImGui::Spacing();
    f32 bw = 100 * s;
    ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - 2 * bw - ImGui::GetStyle().ItemSpacing.x);
    ImGui::PushStyleColor(ImGuiCol_Button, rgb(COL_ACCENT, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, rgb(COL_ACCENT));
    bool go = ImGui::Button("Delete", ImVec2(bw, 0));
    ImGui::PopStyleColor(2);
    ImGui::SameLine();
    bool cancel = ImGui::Button("Cancel", ImVec2(bw, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape);
    if (go) {
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

// One card: the day above it (on the first card of a day in each row), the picture, and two lines under it.
static void lib_card(const LibItem &it, ImVec2 a, f32 w, f32 thumb_h, f32 day_h, f32 meta_h, bool show_day)
{
    Library &lib = g_app.library;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    f32 s = g_dpi_scale;
    if (show_day) {
        char day[64];
        fmt_day(it.mtime_us, day, sizeof day);
        ImGui::PushFont(g_font_bold);
        dl->AddText(ImVec2(a.x, a.y + day_h - ImGui::GetTextLineHeight() - 6 * s), ImGui::GetColorU32(rgb(COL_TEXT)), day);
        ImGui::PopFont();
    }
    ImVec2 t0(a.x, a.y + day_h), t1(a.x + w, a.y + day_h + thumb_h);
    ImGui::SetCursorScreenPos(t0);
    ImGui::PushID((const void *)(uintptr_t)it.key);
    ImGui::InvisibleButton("card", ImVec2(w, thumb_h + meta_h));
    bool hovered = ImGui::IsItemHovered();
    bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    bool menu = ImGui::IsItemClicked(ImGuiMouseButton_Right);
    ImGui::PopID();
    if (menu) { // opened outside the card's ID, where card_menu() looks for it
        lib.target = it;
        ImGui::OpenPopup("card_menu");
    }
    f32 round = 8 * s;
    LibThumb *th = lib_thumb(it);
    f32 frac = -1;
    if (hovered) {
        frac = (ImGui::GetIO().MousePos.x - t0.x) / w;
        frac = frac < 0 ? 0 : frac > 0.999f ? 0.999f : frac;
    }
    bool scrub = hovered && lib_want_strip(it);
    if (scrub) {
        i32 f = (i32)(frac * lib.strip_frames);
        f32 u0 = (f32)f / lib.strip_frames, u1 = (f32)(f + 1) / lib.strip_frames;
        dl->AddImageRounded((ImTextureID)(intptr_t)lib.strip, t0, t1, ImVec2(u0, 0), ImVec2(u1, 1), IM_COL32_WHITE, round);
    } else if (th && th->srv) {
        dl->AddImageRounded((ImTextureID)(intptr_t)th->srv, t0, t1, ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, round);
    } else {
        dl->AddRectFilled(t0, t1, ImGui::GetColorU32(rgb(COL_PANEL2)), round);
        const char *msg = th && th->failed ? "No picture" : "";
        ImVec2 ts = ImGui::CalcTextSize(msg);
        dl->AddText(ImVec2((t0.x + t1.x - ts.x) / 2, (t0.y + t1.y - ts.y) / 2), ImGui::GetColorU32(rgb(COL_DIM)), msg);
    }
    if (hovered) dl->AddRect(t0, t1, ImGui::GetColorU32(rgb(COL_BLUE)), round, 0, 2 * s);
    char text[64];
    if (it.info.track_count > 1) {
        snprintf(text, sizeof text, "%d tracks", it.info.track_count);
        badge(dl, ImVec2(t0.x + 8 * s, t0.y + 8 * s), false, false, text);
    }
    if (it.info.duration > 0) {
        fmt_time(it.info.duration, text, sizeof text);
        badge(dl, ImVec2(t1.x - 8 * s, t1.y - 8 * s), true, true, text);
    }
    if (hovered) { // where a click starts playing
        f32 x = t0.x + frac * w;
        dl->AddRectFilled(ImVec2(t0.x, t1.y - 3 * s), ImVec2(x, t1.y), ImGui::GetColorU32(rgb(COL_ACCENT)));
    }
    f32 y = t1.y + 6 * s;
    ImGui::PushFont(g_font_bold);
    text_clipped(dl, ImVec2(a.x, y), w, ImGui::GetColorU32(rgb(COL_TEXT)), it.name);
    ImGui::PopFont();
    char when[48], size[24], line[160];
    fmt_when(it.mtime_us, when, sizeof when);
    fmt_size(it.size, size, sizeof size);
    snprintf(line, sizeof line, "%s \xC2\xB7 %s \xC2\xB7 %s", it.info.game, when, size);
    text_clipped(dl, ImVec2(a.x, y + ImGui::GetTextLineHeight() + 2 * s), w, ImGui::GetColorU32(rgb(COL_DIM)), line);
    if (clicked) library_play(it, it.info.duration > 0 && frac >= 0 ? frac * it.info.duration : 0);
}

static void lib_player_view()
{
    Library &lib = g_app.library;
    Player &p = lib.player;
    f32 s = g_dpi_scale;
    const ImGuiStyle &st = ImGui::GetStyle();
    player_update(&p);

    ImGui::PushFont(g_font_big);
    ImGui::TextUnformatted(lib.playing.name);
    ImGui::PopFont();
    char when[48], line[160];
    fmt_when(lib.playing.mtime_us, when, sizeof when);
    snprintf(line, sizeof line, "%s \xC2\xB7 %s", lib.playing.info.game, when);
    ImGui::TextDisabled("%s", line);

    ImVec2 avail = ImGui::GetContentRegionAvail();
    f32 row_h = ImGui::GetFrameHeight();
    ImVec2 area = ImVec2(avail.x, avail.y - row_h - st.ItemSpacing.y);
    ImVec2 pos = ImGui::GetCursorScreenPos(), a, b;
    player_fit(p, pos, area, &a, &b);
    ImGui::InvisibleButton("video", area);
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) player_toggle(&p);
    player_draw(p, a, b);

    // The controls row, every part at a fixed width.
    f32 play_w = 90 * s, time_w = 120 * s, sound_w = 220 * s, edit_w = 70 * s, folder_w = 130 * s, close_w = 80 * s;
    f32 fixed = play_w + time_w + sound_w + edit_w + folder_w + close_w + 6 * st.ItemSpacing.x;
    f32 bar_w = avail.x - fixed;
    if (bar_w < 100 * s) bar_w = 100 * s;
    if (ImGui::Button(p.paused ? "Play###play" : "Pause###play", ImVec2(play_w, 0))) player_toggle(&p);
    ImGui::SameLine();
    scrub_bar("##seek", &p, bar_w, row_h);
    ImGui::SameLine();
    char a1[32], a2[32], tm[80];
    fmt_time(p.pos, a1, sizeof a1);
    fmt_time(p.duration > 0 ? p.duration : lib.playing.info.duration, a2, sizeof a2);
    snprintf(tm, sizeof tm, "%s / %s", a1, a2);
    f32 tx = ImGui::GetCursorPosX();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(tm);
    ImGui::SameLine(tx + time_w + st.ItemSpacing.x);
    // Which audio track is heard. A file with one track has nothing to choose.
    const MediaInfo &mi = lib.playing.info;
    i32 tracks = p.audio_count ? p.audio_count : mi.track_count;
    char cur[64];
    snprintf(cur, sizeof cur, "Sound: %s", p.sound < mi.track_count ? mi.tracks[p.sound] : "Track 1");
    ImGui::SetNextItemWidth(sound_w);
    ImGui::BeginDisabled(tracks < 2);
    if (ImGui::BeginCombo("##sound", cur)) {
        for (i32 t = 0; t < tracks && t < MEDIA_MAX_TRACKS; t++) {
            char name[48];
            if (t < mi.track_count) snprintf(name, sizeof name, "%s##%d", mi.tracks[t], t);
            else snprintf(name, sizeof name, "Track %d##%d", t + 1, t);
            if (ImGui::Selectable(name, p.sound == t)) player_set_sound(&p, t);
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Edit", ImVec2(edit_w, 0))) {
        LibItem it = lib.playing;
        library_close_player();
        editor_open(it);
        return;
    }
    ImGui::SameLine();
    if (ImGui::Button("Show in folder", ImVec2(folder_w, 0))) show_in_folder(p.path.s);
    ImGui::SameLine();
    bool close = ImGui::Button("Close", ImVec2(close_w, 0));

    if (!ImGui::GetIO().WantTextInput && !ImGui::IsPopupOpen((const char *)0, ImGuiPopupFlags_AnyPopupId)) {
        if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) player_toggle(&p);
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) player_seek(&p, p.pos - 5);
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) player_seek(&p, p.pos + 5);
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) close = true;
    }
    if (close) library_close_player();
}

static bool view_tab(const char *label, bool active, f32 w)
{
    ImGui::PushStyleColor(ImGuiCol_Button, active ? rgb(COL_BLUE, 0.85f) : rgb(COL_PANEL2));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, active ? rgb(COL_BLUE) : rgb(COL_HOVER));
    bool c = ImGui::Button(label, ImVec2(w, 0));
    ImGui::PopStyleColor(2);
    return c;
}

static void lib_set_view(LibView v)
{
    Library &lib = g_app.library;
    if (lib.view == v) return;
    lib.view = v;
    lib.game[0] = 0;
    lib.page = lib.chip_page = 0;
    lib_refilter();
}

static void screen_library()
{
    Library &lib = g_app.library;
    f32 s = g_dpi_scale;
    const ImGuiStyle &st = ImGui::GetStyle();
    if (!lib.scanned && !lib.scanning) library_rescan();
    if (lib.player.open) {
        lib_player_view();
        return;
    }
    ImGuiIO &io = ImGui::GetIO();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    f32 full_w = ImGui::GetContentRegionAvail().x;
    f32 right = ImGui::GetWindowContentRegionMax().x;

    // Row 1: the views, and the search on the right.
    if (view_tab("Clips", lib.view == LIB_CLIPS, 110 * s)) lib_set_view(LIB_CLIPS);
    ImGui::SameLine();
    if (view_tab("Sessions", lib.view == LIB_SESSIONS, 110 * s)) lib_set_view(LIB_SESSIONS);
    ImGui::SameLine();
    if (view_tab("Not saved", lib.view == LIB_NOT_SAVED, 110 * s)) lib_set_view(LIB_NOT_SAVED);
    f32 search_w = 300 * s;
    ImGui::SameLine(right - search_w);
    bool not_saved = lib.view == LIB_NOT_SAVED;
    if (!not_saved && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F, false)) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(search_w);
    ImGui::BeginDisabled(not_saved); // stays in its place; it searches saved files only
    if (ImGui::InputTextWithHint("##search", "Search names and games", lib.search, sizeof lib.search)) {
        lib.page = 0;
        lib_refilter();
    }
    ImGui::EndDisabled();

    // Not saved: sessions still in the buffer, sessions.cpp's own fixed-size, paged list, with the whole screen.
    if (not_saved) {
        ImGui::BeginChild("not_saved", ImVec2(full_w, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
        sessions_not_saved();
        ImGui::EndChild();
        return;
    }

    // Row 2: game chips, paged; the arrows are always there.
    f32 chip_w = 160 * s, arrow_w = 40 * s;
    i32 per_row = (i32)((full_w - 2 * (arrow_w + st.ItemSpacing.x) + st.ItemSpacing.x) / (chip_w + st.ItemSpacing.x));
    if (per_row < 1) per_row = 1;
    i32 chips = lib.game_count + 1, chip_pages = (chips + per_row - 1) / per_row;
    if (lib.chip_page >= chip_pages) lib.chip_page = chip_pages - 1;
    if (lib.chip_page < 0) lib.chip_page = 0;
    for (i32 k = 0; k < per_row; k++) {
        i32 c = lib.chip_page * per_row + k;
        if (k) ImGui::SameLine();
        if (c >= chips) {
            ImGui::Dummy(ImVec2(chip_w, ImGui::GetFrameHeight()));
            continue;
        }
        const char *name = c == 0 ? "All" : lib.games[c - 1];
        i32 count = c == 0 ? lib.view_count : lib.game_items[c - 1];
        bool active = c == 0 ? !lib.game[0] : !strcmp(lib.game, name);
        ImGui::PushID(c);
        ImVec2 a = ImGui::GetCursorScreenPos();
        ImGui::PushStyleColor(ImGuiCol_Button, active ? rgb(COL_BLUE, 0.85f) : rgb(COL_PANEL2));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, active ? rgb(COL_BLUE) : rgb(COL_HOVER));
        bool pick = ImGui::Button("##chip", ImVec2(chip_w, 0));
        ImGui::PopStyleColor(2);
        ImGui::PopID();
        // The label, cut to fit, with the count kept whole on the right.
        char num[16];
        snprintf(num, sizeof num, "%d", count);
        ImVec2 ns = ImGui::CalcTextSize(num);
        f32 pad = st.FramePadding.x, ty = a.y + st.FramePadding.y;
        text_clipped(dl, ImVec2(a.x + pad, ty), chip_w - 3 * pad - ns.x, ImGui::GetColorU32(rgb(COL_TEXT)), name);
        dl->AddText(ImVec2(a.x + chip_w - pad - ns.x, ty), ImGui::GetColorU32(rgb(active ? COL_TEXT : COL_DIM)), num);
        if (pick) {
            if (c == 0) lib.game[0] = 0;
            else str_set(lib.game, name);
            lib.page = 0;
            lib_refilter();
        }
    }
    ImGui::SameLine(right - 2 * arrow_w - st.ItemSpacing.x);
    if (button_enabled("\xE2\x86\x90##chips", ImVec2(arrow_w, 0), lib.chip_page > 0)) lib.chip_page--;
    ImGui::SameLine();
    if (button_enabled("\xE2\x86\x92##chips", ImVec2(arrow_w, 0), lib.chip_page + 1 < chip_pages)) lib.chip_page++;

    // The grid fills what's left above the pager row.
    f32 pager_h = ImGui::GetFrameHeight();
    ImVec2 g0 = ImGui::GetCursorScreenPos();
    f32 grid_h = ImGui::GetContentRegionAvail().y - pager_h - st.ItemSpacing.y;
    if (grid_h < 1) grid_h = 1;
    f32 gap = 14 * s, min_card = 260 * s, day_h = 30 * s, meta_h = 52 * s;
    i32 cols = (i32)((full_w + gap) / (min_card + gap));
    if (cols < 1) cols = 1;
    f32 card_w = (full_w - (cols - 1) * gap) / cols, thumb_h = card_w * 9 / 16, card_h = day_h + thumb_h + meta_h;
    i32 rows = (i32)((grid_h + gap) / (card_h + gap));
    if (rows < 1) rows = 1;
    i32 per_page = cols * rows;
    i32 pages = lib.shown_count ? (lib.shown_count + per_page - 1) / per_page : 1;
    i32 old_page = lib.page;
    if (lib.page >= pages) lib.page = pages - 1;
    if (lib.page < 0) lib.page = 0;

    ImGui::Dummy(ImVec2(full_w, grid_h));
    ImVec2 after = ImGui::GetCursorScreenPos();
    bool over_grid = ImGui::IsMouseHoveringRect(g0, ImVec2(g0.x + full_w, g0.y + grid_h)) &&
                     ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    if (lib.shown_count == 0) {
        const char *msg = !lib.scanned ? "Looking for saved files..."
                          : lib.view_count == 0
                              ? (lib.view == LIB_CLIPS ? "No clips yet. Saved clips appear here."
                                                       : "No sessions yet. Saved sessions appear here.")
                              : "Nothing matches.";
        ImVec2 ts = ImGui::CalcTextSize(msg);
        dl->AddText(ImVec2(g0.x + (full_w - ts.x) / 2, g0.y + 60 * s), ImGui::GetColorU32(rgb(COL_DIM)), msg);
    }
    i64 prev_day = -1;
    for (i32 k = 0; k < per_page; k++) {
        i32 idx = lib.page * per_page + k;
        if (idx >= lib.shown_count) break;
        const LibItem &it = lib.items[lib.shown[idx]];
        i32 r = k / cols, c = k % cols;
        i64 day = local_day(it.mtime_us, 0);
        bool show_day = c == 0 || day != prev_day;
        prev_day = day;
        lib_card(it, ImVec2(g0.x + c * (card_w + gap), g0.y + r * (card_h + gap)), card_w, thumb_h, day_h, meta_h, show_day);
    }
    ImGui::SetCursorScreenPos(after);
    card_menu();
    rename_modal();
    delete_modal();

    // Pages: the wheel over the grid, Page Up / Page Down.
    bool popup = ImGui::IsPopupOpen((const char *)0, ImGuiPopupFlags_AnyPopupId);
    if (!popup && over_grid && io.MouseWheel != 0) lib.page += io.MouseWheel < 0 ? 1 : -1;
    if (!popup && !io.WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_PageDown)) lib.page++;
        if (ImGui::IsKeyPressed(ImGuiKey_PageUp)) lib.page--;
    }

    // The pager row: the count on the left, the last message, and the page controls at fixed places on the right.
    char count[80];
    snprintf(count, sizeof count, "%d %s%s", lib.shown_count,
             lib.view == LIB_CLIPS ? (lib.shown_count == 1 ? "clip" : "clips") : (lib.shown_count == 1 ? "session" : "sessions"),
             lib.scanning ? ", looking for more" : "");
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", count);
    f32 nav_w = 110 * s, page_w = 130 * s, msg_x = 260 * s;
    f32 nav_x = right - 2 * nav_w - page_w - 2 * st.ItemSpacing.x;
    ImGui::SameLine(msg_x);
    ImVec2 mp = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(1, ImGui::GetFrameHeight()));
    text_clipped(dl, ImVec2(mp.x, mp.y + st.FramePadding.y), nav_x - msg_x - 2 * st.ItemSpacing.x,
                 ImGui::GetColorU32(rgb(lib.message_error ? COL_ACCENT : COL_DIM)), lib.message);
    ImGui::SameLine(nav_x);
    if (button_enabled("\xE2\x86\x90 Previous", ImVec2(nav_w, 0), lib.page > 0)) lib.page--;
    ImGui::SameLine();
    char pg[48];
    snprintf(pg, sizeof pg, "Page %d of %d", lib.page + 1, pages);
    ImVec2 ps = ImGui::CalcTextSize(pg);
    f32 px = ImGui::GetCursorPosX();
    ImGui::SetCursorPosX(px + (page_w - ps.x) / 2);
    ImGui::TextUnformatted(pg);
    ImGui::SameLine(px + page_w + st.ItemSpacing.x);
    if (button_enabled("Next \xE2\x86\x92", ImVec2(nav_w, 0), lib.page + 1 < pages)) lib.page++;

    if (lib.page >= pages) lib.page = pages - 1;
    if (lib.page < 0) lib.page = 0;
    if (lib.page != old_page) lib_drop_queued_thumbs();
}
