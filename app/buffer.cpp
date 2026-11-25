// Reading the buffer's index from disk into a BufferIdx, for saves and the timeline.
//
// The format:
//
//   buffer\video\<run>\v_00000.mkv ...  one folder per capture ffmpeg run (restarts after the lock screen etc.)
//                      list.csv       "v_00000.mkv,0.000000,2.000000" per finished segment (ffmpeg writes it)
//                      anchor.txt     wall-clock us of the run's first frame
//   buffer\audio\<stream>\a_00000.mka one folder per app audio stream (or the mic), Opus, 10 s segments
//                      list.csv       as above, seconds from the stream's start
//                      meta.json      {"kind":"app","exe":"spotify.exe","pid":1,"start_us":...}
//   buffer\activity.tsv               "us<TAB>exe" per change of the game being played (empty exe = desktop)
//   buffer\focus.tsv                  "us<TAB>exe" per change of the program in front, whatever it is
//
// Every time is wall-clock us, so video and every audio stream line up. Files are only read here: ffmpeg may be
// writing them right now. The pruner (recorder.cpp) and Delete footage (erase.cpp) change them.

static i64 filetime_to_us(FILETIME ft)
{
    i64 t = ((i64)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return (t - 116444736000000000LL) / 10;
}

static i64 secs_to_us(f64 s) { return (i64)(s * 1e6 + (s >= 0 ? 0.5 : -0.5)); }

// "v_00012.mkv" -> 12. Segment names sort by this number, which is also how they're matched to list.csv.
static bool seg_number(const char *name, u32 *out)
{
    const char *u = strchr(name, '_');
    if (!u || !isdigit((u8)u[1])) return false;
    *out = (u32)strtoul(u + 1, 0, 10);
    return true;
}

struct ScratchFile { u32 num; u64 size; i64 written_us; char name[20]; };
struct ScratchLine { u32 num; f64 start, end; char name[20]; };
struct ScratchDir  { char name[128]; };

// buffer_read's working memory, on the heap and grown as needed; buffer_read may run on two threads at once.
struct BufferScratch {
    ScratchFile *files;
    i32          file_count, file_cap;
    ScratchLine *lines;
    i32          line_count, line_cap;
    ScratchDir  *dirs;
    i32          dir_count, dir_cap;
    bool        *seen; // read_video_since: prev's runs still there
    i32          seen_cap;
};

// Makes room for `need` entries in a heap array, doubling it. False (logged once) when memory runs out; the
// entries that don't fit are left out.
template <typename T> static bool buf_grow(T *&p, i32 &cap, i32 need)
{
    if (need <= cap) return true;
    i32 n = cap ? cap : 256;
    while (n < need) n *= 2;
    T *q = (T *)realloc(p, (size_t)n * sizeof(T));
    if (!q) {
        static bool logged;
        if (!logged) log_line("buffer: out of memory for %d entries of %d bytes; the rest left out", n, (int)sizeof(T));
        logged = true;
        return false;
    }
    p = q;
    cap = n;
    return true;
}

static int cmp_file(const void *a, const void *b)
{
    u32 x = ((const ScratchFile *)a)->num, y = ((const ScratchFile *)b)->num;
    return x < y ? -1 : x > y;
}
static int cmp_line(const void *a, const void *b)
{
    u32 x = ((const ScratchLine *)a)->num, y = ((const ScratchLine *)b)->num;
    return x < y ? -1 : x > y;
}
static int cmp_dir(const void *a, const void *b)
{
    return strcmp(((const ScratchDir *)a)->name, ((const ScratchDir *)b)->name);
}

// The sub-folders of `dir`, sorted by name (run and stream folders are named by start time).
static void list_dirs(const Path &dir, BufferScratch *s)
{
    s->dir_count = 0;
    Path pattern = path_join(dir, L"*");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(pattern.s, FindExInfoBasic, &fd, FindExSearchNameMatch, 0, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
        if (!buf_grow(s->dirs, s->dir_cap, s->dir_count + 1)) break;
        wide_to_utf8(fd.cFileName, s->dirs[s->dir_count].name, COUNT(s->dirs[0].name));
        s->dir_count++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    qsort(s->dirs, s->dir_count, sizeof s->dirs[0], cmp_dir);
}

// The numbered media files (`ext` like L"*.mkv") in `dir`, sorted by number.
static void list_media(const Path &dir, const wchar_t *ext, BufferScratch *s)
{
    s->file_count = 0;
    Path pattern = path_join(dir, ext);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(pattern.s, FindExInfoBasic, &fd, FindExSearchNameMatch, 0, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (!buf_grow(s->files, s->file_cap, s->file_count + 1)) break;
        ScratchFile *f = &s->files[s->file_count];
        wide_to_utf8(fd.cFileName, f->name, COUNT(f->name));
        if (!seg_number(f->name, &f->num)) continue;
        f->size = ((u64)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        i64 t = ((i64)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime;
        f->written_us = (t - 116444736000000000LL) / 10;
        s->file_count++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    qsort(s->files, s->file_count, sizeof s->files[0], cmp_file);
}

// Reads list.csv lines ("name,start,end"; a half-written last line is skipped), sorted by number.
static void read_list(const Path &dir, BufferScratch *s)
{
    s->line_count = 0;
    Path p = path_join(dir, L"list.csv");
    FILE *f = _wfopen(p.s, L"rb");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        // Split from the right: the name could in theory hold a comma.
        char *c2 = strrchr(line, ',');
        if (!c2) continue;
        *c2 = 0;
        char *c1 = strrchr(line, ',');
        if (!c1) continue;
        *c1 = 0;
        char *end_s = trim(c2 + 1), *start_s = trim(c1 + 1), *name = trim(line);
        if (!*end_s || !*start_s) continue;
        if (*name == '"') { name++; char *q = strchr(name, '"'); if (q) *q = 0; }
        if (!buf_grow(s->lines, s->line_cap, s->line_count + 1)) break;
        ScratchLine *l = &s->lines[s->line_count];
        if (!seg_number(name, &l->num)) continue;
        char *e1 = 0, *e2 = 0;
        l->start = strtod(start_s, &e1);
        l->end = strtod(end_s, &e2);
        if (e1 == start_s || e2 == end_s) continue;
        str_set(l->name, name);
        s->line_count++;
    }
    fclose(f);
    qsort(s->lines, s->line_count, sizeof s->lines[0], cmp_line);
}

static const ScratchFile *find_file(const BufferScratch *s, u32 num)
{
    ScratchFile key = {num};
    return (const ScratchFile *)bsearch(&key, s->files, s->file_count, sizeof key, cmp_file);
}

static const ScratchLine *find_line(const BufferScratch *s, u32 num)
{
    ScratchLine key = {num};
    return (const ScratchLine *)bsearch(&key, s->lines, s->line_count, sizeof key, cmp_line);
}

// Reads a small text file whole into `buf` (cut to fit). Returns false if it isn't there.
static bool read_small(const Path &p, char *buf, i32 cap)
{
    buf[0] = 0;
    FILE *f = _wfopen(p.s, L"rb");
    if (!f) return false;
    size_t n = fread(buf, 1, cap - 1, f);
    buf[n] = 0;
    fclose(f);
    return true;
}

// The value after "key": in a flat JSON object, as text (strings without their quotes). Enough for meta.json,
// which Rekodr writes itself.
static bool json_field(const char *json, const char *key, char *out, i32 cap)
{
    char pat[80];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *k = strstr(json, pat);
    if (!k) return false;
    const char *v = strchr(k + strlen(pat), ':');
    if (!v) return false;
    v++;
    while (*v == ' ' || *v == '\t' || *v == '\r' || *v == '\n') v++;
    i32 n = 0;
    if (*v == '"') {
        for (v++; *v && *v != '"'; v++) {
            if (*v == '\\' && v[1]) v++;
            if (n + 1 < cap) out[n++] = *v;
        }
    } else {
        while (*v && *v != ',' && *v != '}' && *v != ' ' && *v != '\r' && *v != '\n' && n + 1 < cap) out[n++] = *v++;
    }
    out[n] = 0;
    return true;
}

static bool file_mtime_us(const Path &p, i64 *out)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(p.s, GetFileExInfoStandard, &a)) return false;
    *out = filetime_to_us(a.ftLastWriteTime);
    return true;
}

// When list.csv in `dir` was last written and how long it is; 0 and 0 when it isn't there.
static void list_stamp(const Path &dir, i64 *written_us, i64 *bytes)
{
    *written_us = *bytes = 0;
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(path_join(dir, L"list.csv").s, GetFileExInfoStandard, &a)) return;
    *written_us = filetime_to_us(a.ftLastWriteTime);
    *bytes = ((i64)a.nFileSizeHigh << 32) | a.nFileSizeLow;
}

static bool file_exists(const Path &dir, const char *name)
{
    wchar_t w[64];
    utf8_to_wide(name, w, COUNT(w));
    return GetFileAttributesW(path_join(dir, w).s) != INVALID_FILE_ATTRIBUTES;
}

static Path sub_dir(const Path &dir, const char *name)
{
    wchar_t w[128];
    utf8_to_wide(name, w, COUNT(w));
    return path_join(dir, w);
}

static int cmp_run(const void *a, const void *b)
{
    i64 x = ((const VideoRun *)a)->anchor_us, y = ((const VideoRun *)b)->anchor_us;
    return x < y ? -1 : x > y;
}

// Reads run `i`'s segments (its list.csv lines whose files are there) onto the end of b->segs.
static void read_run(const Path &video_dir, BufferIdx *b, i32 i, BufferScratch *s)
{
    VideoRun *r = &b->runs[i];
    Path run_dir = sub_dir(video_dir, r->name);
    list_stamp(run_dir, &r->list_written_us, &r->list_bytes);
    list_media(run_dir, L"*.mkv", s);
    read_list(run_dir, s);
    if (r->list_written_us > b->video_written_us) b->video_written_us = r->list_written_us;
    r->first_seg = b->seg_count;
    for (i32 l = 0; l < s->line_count; l++) {
        const ScratchLine *line = &s->lines[l];
        const ScratchFile *file = find_file(s, line->num);
        if (!file) continue; // pruned, its line stays in list.csv
        if (!buf_grow(b->segs, b->seg_cap, b->seg_count + 1)) break;
        VideoSeg *v = &b->segs[b->seg_count++];
        v->start_us = r->anchor_us + secs_to_us(line->start);
        v->end_us = r->anchor_us + secs_to_us(line->end);
        v->inpoint = line->start;
        v->outpoint = line->end;
        v->size = file->size;
        v->run = i;
        str_set(v->name, file->name);
    }
    r->seg_count = b->seg_count - r->first_seg;
}

static void video_span(BufferIdx *b)
{
    if (!b->seg_count) return;
    b->oldest_us = b->segs[0].start_us;
    b->newest_us = b->segs[b->seg_count - 1].end_us;
}

static void read_video(const Path &video_dir, BufferIdx *b, BufferScratch *s)
{
    list_dirs(video_dir, s);
    for (i32 d = 0; d < s->dir_count && buf_grow(b->runs, b->run_cap, b->run_count + 1); d++) {
        wchar_t wname[128];
        utf8_to_wide(s->dirs[d].name, wname, COUNT(wname));
        Path run_dir = path_join(video_dir, wname);
        char text[64];
        if (!read_small(path_join(run_dir, L"anchor.txt"), text, sizeof text)) continue;
        char *end = 0;
        i64 anchor = strtoll(trim(text), &end, 10);
        if (end == text || anchor <= 0) continue;
        VideoRun *r = &b->runs[b->run_count++];
        *r = {};
        str_set(r->name, s->dirs[d].name);
        r->anchor_us = anchor;
    }
    // Runs never overlap, so in anchor order their segments come out oldest first without a global sort.
    qsort(b->runs, b->run_count, sizeof b->runs[0], cmp_run);
    for (i32 i = 0; i < b->run_count; i++) read_run(video_dir, b, i, s);
    video_span(b);
}

// Fills the index from `prev`, re-reading only the runs whose list.csv changed and the new ones. False when the
// buffer changed in a way that needs a full read: a run folder gone, or the oldest segment pruned.
static bool read_video_since(const Path &video_dir, const BufferIdx &prev, BufferIdx *b, BufferScratch *s)
{
    // The pruner deletes the oldest footage first, so a prune always takes this one
    if (prev.seg_count && !file_exists(sub_dir(video_dir, prev.runs[prev.segs[0].run].name), prev.segs[0].name))
        return false;
    list_dirs(video_dir, s);
    if (!buf_grow(s->seen, s->seen_cap, prev.run_count)) return false;
    bool *seen = s->seen;
    memset(seen, 0, prev.run_count * sizeof *seen);
    for (i32 d = 0; d < s->dir_count; d++) {
        if (!buf_grow(b->runs, b->run_cap, b->run_count + 1)) return false;
        i32 p = 0;
        while (p < prev.run_count && strcmp(prev.runs[p].name, s->dirs[d].name)) p++;
        VideoRun *r = &b->runs[b->run_count];
        if (p < prev.run_count) {
            *r = prev.runs[p];
            seen[p] = true;
        } else {
            char text[64];
            if (!read_small(path_join(sub_dir(video_dir, s->dirs[d].name), L"anchor.txt"), text, sizeof text)) continue;
            char *end = 0;
            i64 anchor = strtoll(trim(text), &end, 10);
            if (end == text || anchor <= 0) continue;
            *r = {};
            str_set(r->name, s->dirs[d].name);
            r->anchor_us = anchor;
            r->first_seg = -1; // nothing to take from prev
        }
        b->run_count++;
    }
    for (i32 p = 0; p < prev.run_count; p++) if (!seen[p]) return false;
    qsort(b->runs, b->run_count, sizeof b->runs[0], cmp_run);
    for (i32 i = 0; i < b->run_count; i++) {
        VideoRun *r = &b->runs[i];
        i64 written, bytes;
        list_stamp(sub_dir(video_dir, r->name), &written, &bytes);
        if (r->first_seg < 0 || written != r->list_written_us || bytes != r->list_bytes) {
            read_run(video_dir, b, i, s);
            continue;
        }
        if (!buf_grow(b->segs, b->seg_cap, b->seg_count + r->seg_count)) return false;
        memcpy(&b->segs[b->seg_count], &prev.segs[r->first_seg], r->seg_count * sizeof(VideoSeg));
        for (i32 k = 0; k < r->seg_count; k++) b->segs[b->seg_count + k].run = i;
        r->first_seg = b->seg_count;
        b->seg_count += r->seg_count;
        if (written > b->video_written_us) b->video_written_us = written;
    }
    video_span(b);
    return true;
}

// Reads the segments of the stream at b->streams[b->stream_count], in `dir`, onto the end of b->audio_segs.
static void read_stream(const Path &dir, BufferIdx *b, BufferScratch *s)
{
    AudioStreamInfo *st = &b->streams[b->stream_count];
    list_stamp(dir, &st->list_written_us, &st->list_bytes);
    list_media(dir, L"*.mka", s);
    read_list(dir, s);
    st->first_seg = b->audio_seg_count;
    i64 next_start = 0; // offset from the stream start
    for (i32 f = 0; f < s->file_count; f++) {
        if (!buf_grow(b->audio_segs, b->audio_seg_cap, b->audio_seg_count + 1)) break;
        const ScratchFile *file = &s->files[f];
        const ScratchLine *line = find_line(s, file->num);
        // Unlisted and not written for a minute: left by an ffmpeg that was killed (a live one flushes every
        // second and starts a new file every 10 s). It's often empty, and ffmpeg can't open it.
        if (!line && now_us() - file->written_us > 60 * 1000000LL) continue;
        AudioSeg *a = &b->audio_segs[b->audio_seg_count++];
        *a = {};
        str_set(a->name, file->name);
        a->size = file->size;
        if (line) {
            a->start_us = st->start_us + secs_to_us(line->start);
            a->end_us = st->start_us + secs_to_us(line->end);
            next_start = secs_to_us(line->end);
        } else {
            a->start_us = st->start_us + next_start; // not listed yet: the one being written
            a->open = true;
        }
    }
    // An unlisted file before the last one means ffmpeg died mid-segment: keep only the segments after it,
    // which run on contiguously to the end.
    i32 last_open = -1;
    for (i32 k = st->first_seg; k < b->audio_seg_count; k++) if (b->audio_segs[k].open) last_open = k;
    if (last_open >= 0 && last_open + 1 != b->audio_seg_count) {
        i32 keep = b->audio_seg_count - (last_open + 1);
        memmove(&b->audio_segs[st->first_seg], &b->audio_segs[last_open + 1], keep * sizeof(AudioSeg));
        b->audio_seg_count = st->first_seg + keep;
    }
    st->seg_count = b->audio_seg_count - st->first_seg;
}

// Reads the stream in folder `name` (meta.json and its segments) onto the end of the index; one that isn't
// readable is left out. False when the index has no room for another stream.
static bool read_stream_new(const Path &audio_dir, const char *name, BufferIdx *b, BufferScratch *s)
{
    if (!buf_grow(b->streams, b->stream_cap, b->stream_count + 1)) return false;
    Path dir = sub_dir(audio_dir, name);
    char json[2048], kind[32], exe[64], num[32];
    if (!read_small(path_join(dir, L"meta.json"), json, sizeof json)) return true;
    if (!json_field(json, "kind", kind, sizeof kind) || !json_field(json, "exe", exe, sizeof exe) ||
        !json_field(json, "start_us", num, sizeof num))
        return true;
    AudioStreamInfo *st = &b->streams[b->stream_count];
    *st = {};
    str_set(st->dir, name);
    for (char *c = exe; *c; c++) *c = (char)tolower((u8)*c);
    str_set(st->exe, exe);
    st->mic = !strcmp(kind, "mic");
    st->start_us = strtoll(num, 0, 10);
    if (json_field(json, "pid", num, sizeof num)) st->pid = (u32)strtoul(num, 0, 10);
    json_field(json, "path", st->path, sizeof st->path);
    read_stream(dir, b, s);
    b->stream_count++;
    return true;
}

static void read_audio(const Path &audio_dir, BufferIdx *b, BufferScratch *s)
{
    list_dirs(audio_dir, s);
    for (i32 d = 0; d < s->dir_count; d++)
        if (!read_stream_new(audio_dir, s->dirs[d].name, b, s)) break;
}

// Fills the index from `prev`, re-reading only the streams whose list.csv changed or that were being written, and
// the new ones. False when a stream folder is gone or a stream's oldest segment was pruned.
static bool read_audio_since(const Path &audio_dir, const BufferIdx &prev, BufferIdx *b, BufferScratch *s)
{
    list_dirs(audio_dir, s);
    i32 j = 0; // prev's streams are in folder order too
    for (i32 d = 0; d < s->dir_count; d++) {
        const char *name = s->dirs[d].name;
        if (j < prev.stream_count && strcmp(prev.streams[j].dir, name) < 0) return false;
        if (j == prev.stream_count || strcmp(prev.streams[j].dir, name)) {
            if (!read_stream_new(audio_dir, name, b, s)) break;
            continue;
        }
        const AudioStreamInfo &ps = prev.streams[j++];
        if (!buf_grow(b->streams, b->stream_cap, b->stream_count + 1)) return false;
        Path dir = sub_dir(audio_dir, name);
        AudioStreamInfo *st = &b->streams[b->stream_count];
        *st = ps;
        i64 written, bytes;
        list_stamp(dir, &written, &bytes);
        bool open = ps.seg_count && prev.audio_segs[ps.first_seg + ps.seg_count - 1].open;
        if (open || written != ps.list_written_us || bytes != ps.list_bytes) {
            read_stream(dir, b, s);
        } else {
            if (ps.seg_count && !file_exists(dir, prev.audio_segs[ps.first_seg].name)) return false;
            if (!buf_grow(b->audio_segs, b->audio_seg_cap, b->audio_seg_count + ps.seg_count)) return false;
            memcpy(&b->audio_segs[b->audio_seg_count], &prev.audio_segs[ps.first_seg], ps.seg_count * sizeof(AudioSeg));
            st->first_seg = b->audio_seg_count;
            b->audio_seg_count += ps.seg_count;
        }
        b->stream_count++;
    }
    return j == prev.stream_count;
}

static void read_activity(const Path &buffer_dir, BufferIdx *b)
{
    FILE *f = _wfopen(path_join(buffer_dir, L"activity.tsv").s, L"rb");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        char *end = 0;
        i64 us = strtoll(line, &end, 10);
        if (end == line) continue;
        if (b->activity_count == COUNT(b->activity)) {
            // Only the recent past matters for saves: drop the older half.
            i32 half = COUNT(b->activity) / 2;
            memmove(b->activity, b->activity + half, (COUNT(b->activity) - half) * sizeof b->activity[0]);
            b->activity_count -= half;
        }
        ActivityEntry *e = &b->activity[b->activity_count++];
        e->us = us;
        str_set(e->exe, trim(tab + 1));
    }
    fclose(f);
}

// Zeroing only the counts and header: the arrays are big and every entry used is written whole.
static void idx_clear(BufferIdx *b)
{
    b->run_count = b->seg_count = b->stream_count = b->audio_seg_count = b->activity_count = 0;
    b->oldest_us = b->newest_us = b->video_written_us = 0;
}

// Fills `b` from the buffer folder. With `prev`, an index of the same folder read before, only what changed since is
// read (a full read when something was pruned or a folder went away); *full says which it was. Returns false only if
// the working memory couldn't be had; an empty or missing buffer is just an empty index.
static bool buffer_read(const Path &buffer_dir, BufferIdx *b, const BufferIdx *prev = 0, bool *full = 0)
{
    idx_clear(b);
    BufferScratch scratch = {}, *s = &scratch;
    Path video = path_join(buffer_dir, L"video"), audio = path_join(buffer_dir, L"audio");
    bool whole = !prev || !read_video_since(video, *prev, b, s) || !read_audio_since(audio, *prev, b, s);
    if (whole) {
        idx_clear(b);
        read_video(video, b, s);
        read_audio(audio, b, s);
    }
    read_activity(buffer_dir, b);
    free(s->files);
    free(s->lines);
    free(s->dirs);
    free(s->seen);
    if (full) *full = whole;
    return true;
}

// Lets go of an index's arrays, leaving it empty.
static void buffer_idx_free(BufferIdx *b)
{
    free(b->runs);
    free(b->segs);
    free(b->streams);
    free(b->audio_segs);
    b->runs = 0;
    b->segs = 0;
    b->streams = 0;
    b->audio_segs = 0;
    b->run_cap = b->seg_cap = b->stream_cap = b->audio_seg_cap = 0;
    idx_clear(b);
}

// Copies the entries `src` uses into `dst`, growing its arrays as needed (left empty if memory runs out).
static void buffer_idx_copy(BufferIdx *dst, const BufferIdx *src)
{
    if (!buf_grow(dst->runs, dst->run_cap, src->run_count) || !buf_grow(dst->segs, dst->seg_cap, src->seg_count) ||
        !buf_grow(dst->streams, dst->stream_cap, src->stream_count) ||
        !buf_grow(dst->audio_segs, dst->audio_seg_cap, src->audio_seg_count)) {
        idx_clear(dst);
        return;
    }
    memcpy(dst->runs, src->runs, src->run_count * sizeof src->runs[0]);
    memcpy(dst->segs, src->segs, src->seg_count * sizeof src->segs[0]);
    memcpy(dst->streams, src->streams, src->stream_count * sizeof src->streams[0]);
    memcpy(dst->audio_segs, src->audio_segs, src->audio_seg_count * sizeof src->audio_segs[0]);
    memcpy(dst->activity, src->activity, src->activity_count * sizeof src->activity[0]);
    dst->run_count = src->run_count;
    dst->seg_count = src->seg_count;
    dst->stream_count = src->stream_count;
    dst->audio_seg_count = src->audio_seg_count;
    dst->activity_count = src->activity_count;
    dst->oldest_us = src->oldest_us;
    dst->newest_us = src->newest_us;
    dst->video_written_us = src->video_written_us;
}

// The end of the newest finished video segment, read cheaply (only the newest run's list.csv tail), and when that
// list was last written. For waiting on the segment being written without re-reading the whole index.
static bool buffer_video_newest_end(const Path &buffer_dir, i64 *end_us, i64 *written_us)
{
    *end_us = *written_us = 0;
    Path video_dir = path_join(buffer_dir, L"video");
    // Run folders are named by their start time, so the newest is the greatest name.
    wchar_t newest[MAX_PATH] = {};
    Path pattern = path_join(video_dir, L"*");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(pattern.s, FindExInfoBasic, &fd, FindExSearchNameMatch, 0, 0);
    if (h == INVALID_HANDLE_VALUE) return false;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != L'.' && wcscmp(fd.cFileName, newest) > 0)
            wcscpy_s(newest, fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (!newest[0]) return false;
    Path run_dir = path_join(video_dir, newest);
    char text[64];
    if (!read_small(path_join(run_dir, L"anchor.txt"), text, sizeof text)) return false;
    i64 anchor = strtoll(trim(text), 0, 10);
    Path list = path_join(run_dir, L"list.csv");
    file_mtime_us(list, written_us);

    FILE *f = _wfopen(list.s, L"rb");
    if (!f) return false;
    char tail[1024];
    _fseeki64(f, 0, SEEK_END);
    i64 size = _ftelli64(f);
    _fseeki64(f, size > (i64)sizeof tail - 1 ? size - (i64)sizeof tail + 1 : 0, SEEK_SET);
    size_t n = fread(tail, 1, sizeof tail - 1, f);
    tail[n] = 0;
    fclose(f);
    // The last complete line: ends in a newline, has two commas.
    char *last_nl = strrchr(tail, '\n');
    if (!last_nl) return false;
    *last_nl = 0;
    char *line = strrchr(tail, '\n');
    line = line ? line + 1 : tail;
    char *comma = strrchr(line, ',');
    if (!comma) return false;
    *end_us = anchor + secs_to_us(strtod(comma + 1, 0));
    return true;
}

// Seconds spent on each game within [from, to], most first (desktop time isn't counted). Returns how many.
struct GameTime { char exe[64]; i64 us; };

static i32 games_in(const BufferIdx &b, i64 from, i64 to, GameTime *out, i32 cap)
{
    i32 n = 0;
    for (i32 i = 0; i < b.activity_count; i++) {
        const ActivityEntry &e = b.activity[i];
        i64 end = i + 1 < b.activity_count ? b.activity[i + 1].us : INT64_MAX;
        i64 overlap = (end < to ? end : to) - (e.us > from ? e.us : from);
        if (!e.exe[0] || overlap <= 0) continue;
        i32 k = 0;
        while (k < n && strcmp(out[k].exe, e.exe)) k++;
        if (k == n) {
            if (n == cap) continue;
            str_set(out[n].exe, e.exe);
            out[n].us = 0;
            n++;
        }
        out[k].us += overlap;
    }
    // Most time first, then by name, so the order is the same every time.
    for (i32 i = 1; i < n; i++) {
        GameTime g = out[i];
        i32 j = i;
        while (j > 0 && (out[j - 1].us < g.us || (out[j - 1].us == g.us && strcmp(out[j - 1].exe, g.exe) > 0))) {
            out[j] = out[j - 1];
            j--;
        }
        out[j] = g;
    }
    return n;
}
