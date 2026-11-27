// layers.cpp: things that happen while the buffer records, kept beside it and written into each clip.
//
// A layer is one stream of samples from one source, on the wall clock like markers, of one of four kinds:
//   Value  a number over time; each sample holds until the next
//   Text   lines, each with a start and an end
//   Input  key and button downs and ups, by code
//   Spans  named stretches (the program in front), each with a start and an end
// A Text or Spans sample pushed with layer_hold has no end yet (0): it lasts until the layer's next sample.
// A source registers its layer by name with a label (and for a Value its unit and range), pushes samples as they
// come, and may later replace a span of its own samples (a better transcript of the same minute, say). The store
// keeps them for as long as the buffer keeps footage: <local>\layers\<hash of the name>.tsv, a line of
// <kind> <tab> <name> <tab> <label> <tab> <unit> <tab> <lo> <tab> <hi>, then a line per sample of
// <time in us> <tab> <end in us> <tab> <value> <tab> <code> <tab> <text>, oldest first.
//
// A save takes the clip's slice (layers_slice) and writes it, in the clip's own time, to
// <save_dir>\Data\<video's name>.layers.txt beside its edit. An edit can switch layers on; a render then draws each
// one that is on into the picture (layers_ass, ffmpeg's subtitles filter).

struct LayerStore {
    SRWLOCK  lock;
    bool     loaded;
    Path     dir; // <local>\layers unless set before first use
    LayerSet set;
};
static LayerStore g_layers;

static const char *const LAYER_KIND_NAMES[LAYER_KINDS] = {"value", "text", "input", "spans"};

static i32 layer_kind_named(const char *word)
{
    for (i32 k = 0; k < LAYER_KINDS; k++)
        if (!strcmp(word, LAYER_KIND_NAMES[k])) return k;
    return -1;
}

// A name a layer can have: 1 to LAYER_NAME_MAX - 1 bytes of printable text, no tab or |, nothing around it.
static bool layer_name_ok(const char *name)
{
    size_t n = name ? strlen(name) : 0;
    if (!n || n >= LAYER_NAME_MAX || name[0] == ' ' || name[n - 1] == ' ') return false;
    for (size_t i = 0; i < n; i++)
        if ((u8)name[i] < 0x20 || name[i] == 0x7f || name[i] == '|') return false;
    return true;
}

// A label or unit as it is kept: printable, no tab or |, trimmed, cut to fit.
static void layer_word_set(char *out, i32 cap, const char *in)
{
    if (!in) in = "";
    while (*in == ' ') in++;
    i32 n = 0;
    for (; *in && n < cap - 1; in++) out[n++] = (u8)*in < 0x20 || *in == 0x7f || *in == '|' ? ' ' : *in;
    while (n && out[n - 1] == ' ') n--;
    out[n] = 0;
}

static bool layer_has_lines(u8 kind) { return kind == LAYER_TEXT || kind == LAYER_SPANS; }

// When sample i ends: its own end, or for one still held the next sample's start, else `open_end`.
static i64 layer_end_of(const Layer &l, i32 i, i64 open_end)
{
    if (l.s[i].end_us) return l.s[i].end_us;
    return i + 1 < l.count ? l.s[i + 1].t_us : open_end;
}

// A line of text as it is kept: one line, no tabs, at most LAYER_TEXT_MAX - 1 bytes. Malloc'd.
static char *layer_text_dup(const char *text)
{
    if (!text) text = "";
    while (*text == ' ') text++;
    size_t n = strlen(text);
    if (n >= LAYER_TEXT_MAX) n = LAYER_TEXT_MAX - 1;
    while (n && text[n - 1] == ' ') n--;
    char *s = (char *)malloc(n + 1);
    for (size_t i = 0; i < n; i++) s[i] = (u8)text[i] < 0x20 ? ' ' : text[i];
    s[n] = 0;
    return s;
}

static void layer_free_samples(Layer *l)
{
    for (i32 i = 0; i < l->count; i++) free(l->s[i].text);
    free(l->s);
    l->s = 0;
    l->count = l->cap = 0;
}

static void layer_set_free(LayerSet *set)
{
    for (i32 i = 0; i < set->count; i++) layer_free_samples(&set->layers[i]);
    *set = {};
}

// A sample added in its place by time, after any at the same time. The layer takes its text.
static void layer_insert(Layer *l, LayerSample s)
{
    if (l->count == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 64;
        l->s = (LayerSample *)realloc(l->s, l->cap * sizeof *l->s);
    }
    i32 at = l->count;
    while (at > 0 && l->s[at - 1].t_us > s.t_us) at--;
    memmove(l->s + at + 1, l->s + at, (l->count - at) * sizeof *l->s);
    l->s[at] = s;
    l->count++;
}

// A sample as given by a source, made fit for its kind. Its text is a copy.
static LayerSample layer_sample_clean(u8 kind, const LayerSample &in)
{
    LayerSample s = {};
    s.t_us = in.t_us;
    if (kind == LAYER_VALUE) s.value = in.value;
    else if (layer_has_lines(kind)) {
        s.end_us = !in.end_us ? 0 : in.end_us > in.t_us ? in.end_us : in.t_us;
        s.text = layer_text_dup(in.text);
    } else {
        s.code = in.code;
        s.value = in.value != 0 ? 1 : 0;
    }
    return s;
}

// ---------------------------------------------------------------------------------------------------------------
// The store's files

static Path layer_file(const LayerStore *ls, const char *name)
{
    Path p = {};
    swprintf(p.s, MAX_PATH, L"%s\\%016llx.tsv", ls->dir.s, (unsigned long long)fnv64(name, strlen(name)));
    return p;
}

static i32 layer_sample_line(const LayerSample &s, char *out, i32 cap)
{
    return snprintf(out, cap, "%lld\t%lld\t%.9g\t%u\t%s\n", (long long)s.t_us, (long long)s.end_us, s.value, s.code,
                    s.text ? s.text : "");
}

// Writes a layer's file whole, through a temp file. Called with the lock held.
static void layer_write_locked(LayerStore *ls, const Layer &l)
{
    SHCreateDirectoryExW(0, ls->dir.s, 0);
    Path file = layer_file(ls, l.name), tmp = file;
    wcscat_s(tmp.s, L".tmp");
    FILE *f = _wfopen(tmp.s, L"wb");
    if (!f) { log_line("layers: couldn't write %s (%d)", l.name, (int)GetLastError()); return; }
    fprintf(f, "%s\t%s\t%s\t%s\t%.9g\t%.9g\n", LAYER_KIND_NAMES[l.kind], l.name, l.label, l.unit, l.lo, l.hi);
    static char line[LAYER_TEXT_MAX + 128];
    for (i32 i = 0; i < l.count; i++) {
        i32 n = layer_sample_line(l.s[i], line, sizeof line);
        if (n > 0 && n < (i32)sizeof line) fwrite(line, 1, (size_t)n, f);
    }
    fclose(f);
    if (!MoveFileExW(tmp.s, file.s, MOVEFILE_REPLACE_EXISTING))
        log_line("layers: couldn't replace %s (%d)", l.name, (int)GetLastError());
}

// Adds samples to the end of a layer's file. Called with the lock held.
static void layer_append_locked(LayerStore *ls, const Layer &l, const LayerSample *s, i32 n)
{
    FILE *f = _wfopen(layer_file(ls, l.name).s, L"ab");
    if (!f) { log_line("layers: couldn't add to %s (%d)", l.name, (int)GetLastError()); return; }
    static char line[LAYER_TEXT_MAX + 128];
    for (i32 i = 0; i < n; i++) {
        i32 len = layer_sample_line(s[i], line, sizeof line);
        if (len > 0 && len < (i32)sizeof line) fwrite(line, 1, (size_t)len, f);
    }
    fclose(f);
}

// Splits a line at its tabs into at most `max` fields, the last taking the rest. Returns how many.
static i32 layer_fields(char *s, char **f, i32 max)
{
    i32 n = 0;
    f[n++] = s;
    for (; *s && n < max; s++)
        if (*s == '\t') *s = 0, f[n++] = s + 1;
    return n;
}

static void layer_load_file(LayerStore *ls, const Path &file)
{
    FILE *f = _wfopen(file.s, L"rb");
    if (!f) return;
    static char line[LAYER_TEXT_MAX + 256];
    Layer *l = 0;
    if (fgets(line, sizeof line, f) && ls->set.count < LAYER_MAX) {
        line[strcspn(line, "\r\n")] = 0;
        char *fld[6];
        i32 kind = layer_fields(line, fld, 6) == 6 ? layer_kind_named(fld[0]) : -1;
        if (kind >= 0 && layer_name_ok(fld[1]) && !wcscmp(layer_file(ls, fld[1]).s, file.s)) {
            l = &ls->set.layers[ls->set.count++];
            *l = {};
            str_set(l->name, fld[1]);
            layer_word_set(l->label, sizeof l->label, fld[2]);
            layer_word_set(l->unit, sizeof l->unit, fld[3]);
            l->lo = strtod(fld[4], 0), l->hi = strtod(fld[5], 0);
            l->kind = (u8)kind;
        }
    }
    while (l && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *fld[5];
        if (layer_fields(line, fld, 5) != 5) continue;
        LayerSample s = {};
        s.t_us = strtoll(fld[0], 0, 10), s.end_us = strtoll(fld[1], 0, 10);
        s.value = strtod(fld[2], 0), s.code = (u32)strtoul(fld[3], 0, 10);
        s.text = fld[4];
        if (s.t_us > 0) layer_insert(l, layer_sample_clean(l->kind, s));
    }
    fclose(f);
}

// Ends each layer's held Text or Spans line at `at` (its own start if that is later) and rewrites the layers that
// changed. Only a layer's last line can be held. Called with the lock held.
static bool layers_close_held_locked(LayerStore *ls, i64 at)
{
    bool any = false;
    for (i32 k = 0; k < ls->set.count; k++) {
        Layer &l = ls->set.layers[k];
        if (!layer_has_lines(l.kind) || !l.count || l.s[l.count - 1].end_us) continue;
        LayerSample &s = l.s[l.count - 1];
        s.end_us = at > s.t_us ? at : s.t_us;
        layer_write_locked(ls, l);
        any = true;
    }
    return any;
}

// The last time the sources were seen watching (layers_seen), from <dir>\seen.txt; 0 when there's none.
static i64 layers_seen_read(const LayerStore *ls)
{
    FILE *f = _wfopen(path_join(ls->dir, L"seen.txt").s, L"rb");
    if (!f) return 0;
    long long t = 0;
    if (fscanf(f, "%lld", &t) != 1) t = 0;
    fclose(f);
    return t;
}

// A line still held when the files were last written was held by a Rekodr that has stopped: it ends where that
// Rekodr was last seen watching, never at this run's first line.
static void layers_load_locked(LayerStore *ls)
{
    if (ls->loaded) return;
    ls->loaded = true;
    if (!ls->dir.s[0]) ls->dir = path_join(g_app.paths.local_dir, L"layers");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(path_join(ls->dir, L"*.tsv").s, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) layer_load_file(ls, path_join(ls->dir, fd.cFileName));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    layers_close_held_locked(ls, layers_seen_read(ls));
}

// ---------------------------------------------------------------------------------------------------------------
// The source interface. Any thread.

// A source's layer: the id to push to, the same one again for a name already registered with this kind (a source
// carries on after a restart; a new label, unit or range replaces the old). The label is the name when empty.
// -1 if the name won't do, it is taken by another kind, or there are LAYER_MAX.
static i32 layer_register(const char *name, LayerKind kind, const char *label, const char *unit, f64 lo, f64 hi)
{
    if (!layer_name_ok(name) || kind >= LAYER_KINDS) return -1;
    Layer head = {};
    str_set(head.name, name);
    layer_word_set(head.label, sizeof head.label, label && *label ? label : name);
    layer_word_set(head.unit, sizeof head.unit, unit);
    if (kind == LAYER_VALUE && hi > lo) head.lo = lo, head.hi = hi;
    head.kind = (u8)kind;
    LayerStore *ls = &g_layers;
    AcquireSRWLockExclusive(&ls->lock);
    layers_load_locked(ls);
    i32 id = -1;
    for (i32 i = 0; i < ls->set.count && id < 0; i++)
        if (!strcmp(ls->set.layers[i].name, name)) id = ls->set.layers[i].kind == kind ? i : -2;
    if (id >= 0) {
        Layer &l = ls->set.layers[id];
        if (strcmp(l.label, head.label) || strcmp(l.unit, head.unit) || l.lo != head.lo || l.hi != head.hi) {
            str_set(l.label, head.label);
            str_set(l.unit, head.unit);
            l.lo = head.lo, l.hi = head.hi;
            layer_write_locked(ls, l);
        }
    } else if (id == -1 && ls->set.count < LAYER_MAX) {
        id = ls->set.count++;
        ls->set.layers[id] = head;
        layer_write_locked(ls, head);
    }
    ReleaseSRWLockExclusive(&ls->lock);
    return id < 0 ? -1 : id;
}

// A sample for the layer: Value uses value; Text end_us and text; Input code, and value 1 for down, 0 for up.
static bool layer_push(i32 id, const LayerSample &in)
{
    LayerStore *ls = &g_layers;
    AcquireSRWLockExclusive(&ls->lock);
    layers_load_locked(ls);
    bool ok = id >= 0 && id < ls->set.count && in.t_us > 0;
    if (ok) {
        Layer &l = ls->set.layers[id];
        LayerSample s = layer_sample_clean(l.kind, in);
        layer_insert(&l, s);
        layer_append_locked(ls, l, &s, 1);
    }
    ReleaseSRWLockExclusive(&ls->lock);
    return ok;
}

static bool layer_value(i32 id, i64 t_us, f64 v)
{
    LayerSample s = {};
    s.t_us = t_us, s.value = v;
    return layer_push(id, s);
}

static bool layer_text(i32 id, i64 t_us, i64 end_us, const char *text)
{
    LayerSample s = {};
    s.t_us = t_us, s.end_us = end_us, s.text = (char *)text;
    return layer_push(id, s);
}

static bool layer_hold(i32 id, i64 t_us, const char *text)
{
    LayerSample s = {};
    s.t_us = t_us, s.text = (char *)text;
    return layer_push(id, s);
}

static bool layer_input(i32 id, i64 t_us, u32 code, bool down)
{
    LayerSample s = {};
    s.t_us = t_us, s.code = code, s.value = down ? 1 : 0;
    return layer_push(id, s);
}

// The sources are watching at `at`: kept in <dir>\seen.txt, where a line held when Rekodr stops ends. The audio
// watcher calls it each time it looks at the front window.
static void layers_seen(i64 at)
{
    LayerStore *ls = &g_layers;
    AcquireSRWLockExclusive(&ls->lock);
    layers_load_locked(ls);
    SHCreateDirectoryExW(0, ls->dir.s, 0);
    FILE *f = _wfopen(path_join(ls->dir, L"seen.txt").s, L"wb");
    if (f) {
        fprintf(f, "%lld\n", (long long)at);
        fclose(f);
    }
    ReleaseSRWLockExclusive(&ls->lock);
}

// Ends every held line at `at`: Windows is going to sleep or ending the session, Rekodr is quitting, or the watcher
// found it hadn't run since `at`. A source whose line was ended holds a fresh one when it next looks
// (layers_closed tells it).
static volatile LONG g_layers_closed;
static void layers_close_held(i64 at)
{
    LayerStore *ls = &g_layers;
    AcquireSRWLockExclusive(&ls->lock);
    layers_load_locked(ls);
    if (layers_close_held_locked(ls, at)) InterlockedIncrement(&g_layers_closed);
    ReleaseSRWLockExclusive(&ls->lock);
}

// How many times held lines have been ended; a source holds its line afresh when this changes.
static LONG layers_closed() { return InterlockedCompareExchange(&g_layers_closed, 0, 0); }

// Puts samples in place of the layer's own that start in [from, to). The new ones may lie anywhere.
static bool layer_replace(i32 id, i64 from, i64 to, const LayerSample *s, i32 n)
{
    LayerStore *ls = &g_layers;
    AcquireSRWLockExclusive(&ls->lock);
    layers_load_locked(ls);
    bool ok = id >= 0 && id < ls->set.count && to >= from;
    if (ok) {
        Layer &l = ls->set.layers[id];
        i32 kept = 0;
        for (i32 i = 0; i < l.count; i++) {
            if (l.s[i].t_us >= from && l.s[i].t_us < to) free(l.s[i].text);
            else l.s[kept++] = l.s[i];
        }
        l.count = kept;
        for (i32 i = 0; i < n; i++)
            if (s[i].t_us > 0) layer_insert(&l, layer_sample_clean(l.kind, s[i]));
        layer_write_locked(ls, l);
    }
    ReleaseSRWLockExclusive(&ls->lock);
    return ok;
}

// How stale the oldest kept sample may get before its file is rewritten without it.
static const i64 LAYER_PRUNE_SLACK_US = 10 * 60 * 1000000ll;

// Drops what is older than the buffer's oldest footage. A Value keeps its last sample before then, which still
// says what it was at the start. Main thread, as the buffer reports its age.
static void layers_prune(i64 before_us)
{
    LayerStore *ls = &g_layers;
    AcquireSRWLockExclusive(&ls->lock);
    layers_load_locked(ls);
    for (i32 k = 0; k < ls->set.count; k++) {
        Layer &l = ls->set.layers[k];
        i32 drop = 0; // a Text line goes once it has ended, and not before one ahead of it has
        while (drop < l.count && (layer_has_lines(l.kind) ? layer_end_of(l, drop, INT64_MAX) : l.s[drop].t_us) < before_us)
            drop++;
        if (l.kind == LAYER_VALUE && drop) drop--;
        if (!drop || l.s[0].t_us >= before_us - LAYER_PRUNE_SLACK_US) continue;
        for (i32 i = 0; i < drop; i++) free(l.s[i].text);
        memmove(l.s, l.s + drop, (l.count - drop) * sizeof *l.s);
        l.count -= drop;
        layer_write_locked(ls, l);
    }
    ReleaseSRWLockExclusive(&ls->lock);
}

// The samples that bear on [from, to), on the wall clock, as a set of its own (layer_set_free): a Value's last one
// before `from` as well, the Text and Spans lines that overlap (a held one given its end, `to` for the last), and an
// Input's keys held at `from` as downs at `from`.
// Layers with nothing in it are left out. Any thread.
static void layers_slice(i64 from, i64 to, LayerSet *out)
{
    *out = {};
    LayerStore *ls = &g_layers;
    AcquireSRWLockExclusive(&ls->lock);
    layers_load_locked(ls);
    for (i32 k = 0; k < ls->set.count; k++) {
        const Layer &l = ls->set.layers[k];
        Layer &o = out->layers[out->count];
        o = l;
        o.count = o.cap = 0;
        o.s = 0;
        i32 first = 0;
        while (first < l.count && l.s[first].t_us < from) first++;
        if (l.kind == LAYER_VALUE && first) layer_insert(&o, l.s[first - 1]);
        if (l.kind == LAYER_INPUT) {
            u32 held[64];
            i32 nh = 0;
            for (i32 i = 0; i < first; i++) {
                i32 at = 0;
                while (at < nh && held[at] != l.s[i].code) at++;
                if (l.s[i].value != 0 && at == nh && nh < COUNT(held)) held[nh++] = l.s[i].code;
                else if (l.s[i].value == 0 && at < nh) held[at] = held[--nh];
            }
            for (i32 i = 0; i < nh; i++) {
                LayerSample d = {};
                d.t_us = from, d.code = held[i], d.value = 1;
                layer_insert(&o, d);
            }
        }
        for (i32 i = 0; i < l.count && l.s[i].t_us < to; i++) {
            LayerSample s = l.s[i];
            if (layer_has_lines(l.kind)) {
                s.end_us = layer_end_of(l, i, to);
                if (s.end_us <= from) continue;
            } else if (i < first) continue;
            if (s.text) s.text = layer_text_dup(s.text);
            layer_insert(&o, s);
        }
        if (o.count) out->count++;
        else layer_free_samples(&o);
    }
    ReleaseSRWLockExclusive(&ls->lock);
}

// ---------------------------------------------------------------------------------------------------------------
// A clip's layers: <save_dir>\Data\<video's name>.layers.txt. Its times are the wall clock (UTC microseconds since
// 1970), with the clip's pieces, which place the wall clock in the video; read back, they are in the video's time.

// A stretch of the wall clock in a saved file
struct Piece {
    i64 start_us, end_us; // wall clock
    i64 offset_us;        // where it starts in the saved file
};

// Where a wall-clock moment lands in the saved file; -1 when it falls in a recording gap
static i64 file_time(const Piece *pieces, i32 piece_count, i64 t_us)
{
    for (i32 k = 0; k < piece_count; k++)
        if (t_us >= pieces[k].start_us && t_us < pieces[k].end_us) return pieces[k].offset_us + t_us - pieces[k].start_us;
    return -1;
}

// Where a wall-clock moment lands in the saved file, one in a gap where the file carries on after it (the end for
// one after the last piece)
static i64 file_time_held(const Piece *pieces, i32 piece_count, i64 t_us)
{
    for (i32 k = 0; k < piece_count; k++) {
        if (t_us < pieces[k].start_us) return pieces[k].offset_us;
        if (t_us < pieces[k].end_us) return pieces[k].offset_us + t_us - pieces[k].start_us;
    }
    return piece_count ? pieces[piece_count - 1].offset_us + pieces[piece_count - 1].end_us - pieces[piece_count - 1].start_us : 0;
}

// A slice of the layers (layers_slice) moved from the wall clock to the saved file's time. Text and Spans lines
// wholly in a gap go, and layers left with nothing.
static void layers_to_file_time(LayerSet *set, const Piece *pieces, i32 piece_count)
{
    i32 kept_layers = 0;
    for (i32 k = 0; k < set->count; k++) {
        Layer &l = set->layers[k];
        i32 kept = 0;
        for (i32 i = 0; i < l.count; i++) {
            LayerSample s = l.s[i];
            s.t_us = file_time_held(pieces, piece_count, s.t_us);
            if (layer_has_lines(l.kind)) {
                s.end_us = file_time_held(pieces, piece_count, s.end_us);
                if (s.end_us - s.t_us < 1000) { free(s.text); continue; }
            }
            l.s[kept++] = s;
        }
        l.count = kept;
        if (!kept) { layer_free_samples(&l); continue; }
        if (kept_layers != k) set->layers[kept_layers] = l, l = {};
        kept_layers++;
    }
    set->count = kept_layers;
}


static Path layers_clip_path(const Path &save_dir, const wchar_t *stem)
{
    Path p = {};
    swprintf(p.s, MAX_PATH, L"%s\\%s.layers.txt", save_data_dir(save_dir).s, stem);
    return p;
}

// The layers file of a saved video, by its path
static Path layers_video_path(const Path &save_dir, const Path &video)
{
    wchar_t stem[MAX_PATH];
    const wchar_t *name = wcsrchr(video.s, L'\\');
    wcscpy_s(stem, name ? name + 1 : video.s);
    wchar_t *dot = wcsrchr(stem, L'.');
    if (dot) *dot = 0;
    return layers_clip_path(save_dir, stem);
}

static bool layers_write(const Path &file, const LayerSet &set, const Piece *pieces, i32 piece_count)
{
    Path dir = file;
    wchar_t *slash = wcsrchr(dir.s, L'\\');
    if (slash) *slash = 0;
    SHCreateDirectoryExW(0, dir.s, 0);
    Path tmp = {};
    swprintf(tmp.s, MAX_PATH, L"%s.tmp", file.s);
    FILE *f = _wfopen(tmp.s, L"wb");
    if (!f) return false;
    fprintf(f, "# Rekodr layers. Times are UTC microseconds since 1970. piece = start end offset: the wall clock from start\n"
               "# to end is in the video from offset. Then each layer (kind | name | label | unit | lo | hi) and its\n"
               "# samples: v = time value; t = time end text; i = time code down (1) or up (0); s = time end label\n");
    for (i32 k = 0; k < piece_count; k++)
        fprintf(f, "piece = %lld %lld %lld\n", (long long)pieces[k].start_us, (long long)pieces[k].end_us,
                (long long)pieces[k].offset_us);
    for (i32 k = 0; k < set.count; k++) {
        const Layer &l = set.layers[k];
        fprintf(f, "layer = %s | %s | %s | %s | %.9g | %.9g\n", LAYER_KIND_NAMES[l.kind], l.name, l.label, l.unit, l.lo,
                l.hi);
        for (i32 i = 0; i < l.count; i++) {
            const LayerSample &s = l.s[i];
            if (l.kind == LAYER_VALUE) fprintf(f, "v = %lld %.9g\n", (long long)s.t_us, s.value);
            else if (layer_has_lines(l.kind))
                fprintf(f, "%c = %lld %lld %s\n", l.kind == LAYER_TEXT ? 't' : 's', (long long)s.t_us, (long long)s.end_us,
                        s.text ? s.text : "");
            else fprintf(f, "i = %lld %u %d\n", (long long)s.t_us, s.code, s.value != 0 ? 1 : 0);
        }
    }
    fclose(f);
    return MoveFileExW(tmp.s, file.s, MOVEFILE_REPLACE_EXISTING) != 0;
}

// A clip's layers in the video's time, into a set of their own (layer_set_free). False when it has none.
static bool layers_read(const Path &file, LayerSet *set)
{
    *set = {};
    FILE *f = _wfopen(file.s, L"rb");
    if (!f) return false;
    static char line[LAYER_TEXT_MAX + 128];
    static Piece pieces[512];
    i32 piece_count = 0;
    Layer *l = 0;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (!eq || line[0] == '#') continue;
        *eq = 0;
        char *k = trim(line), *v = eq + 1;
        if (*v == ' ') v++;
        if (!strcmp(k, "piece")) {
            long long a = 0, z = 0, at = 0;
            if (sscanf_s(v, "%lld %lld %lld", &a, &z, &at) == 3 && z > a && at >= 0 && piece_count < COUNT(pieces))
                pieces[piece_count++] = {a, z, at};
            continue;
        }
        if (!strcmp(k, "layer")) {
            l = 0;
            char *fld[6];
            i32 n = 0;
            fld[n++] = v;
            for (char *c = v; *c && n < 6; c++)
                if (*c == '|') *c = 0, fld[n++] = c + 1;
            if (n != 6 || set->count == LAYER_MAX) continue;
            for (i32 i = 0; i < n; i++) fld[i] = trim(fld[i]);
            i32 kind = layer_kind_named(fld[0]);
            if (kind < 0 || !layer_name_ok(fld[1])) continue;
            l = &set->layers[set->count++];
            *l = {};
            str_set(l->name, fld[1]);
            layer_word_set(l->label, sizeof l->label, fld[2][0] ? fld[2] : fld[1]);
            layer_word_set(l->unit, sizeof l->unit, fld[3]);
            l->lo = strtod(fld[4], 0), l->hi = strtod(fld[5], 0);
            l->kind = (u8)kind;
            continue;
        }
        if (!l || strlen(k) != 1 || *k != "vtis"[l->kind]) continue;
        LayerSample s = {};
        long long t = 0, end = 0;
        i32 down = 0, used = 0;
        if (l->kind == LAYER_VALUE) {
            if (sscanf_s(v, "%lld %lf", &t, &s.value) != 2) continue;
        } else if (layer_has_lines(l->kind)) {
            if (sscanf_s(v, "%lld %lld %n", &t, &end, &used) < 2 || !used || end <= t) continue;
            s.end_us = end;
            s.text = v + used;
        } else {
            if (sscanf_s(v, "%lld %u %d", &t, &s.code, &down) != 3) continue;
            s.value = down ? 1 : 0;
        }
        s.t_us = t;
        if (t > 0) layer_insert(l, layer_sample_clean(l->kind, s));
    }
    fclose(f);
    layers_to_file_time(set, pieces, piece_count);
    return set->count > 0;
}

// ---------------------------------------------------------------------------------------------------------------
// Markers that came after the save (from a game's replay, once the match is over) are kept in the same file as
// "mark = time colour note", the time on the wall clock and the colour an index into MARKER_COLOURS, and a tab and
// the marker's detail when it has one. The markers made before the save are the video's chapters.

static void marker_note_set(char (&note)[256], const char *text); // markers.cpp
static void marker_detail_set(char (&d)[256], const char *text);

// A layers file's pieces and late markers, the markers still on the wall clock. False when it can't be read.
static bool layers_file_marks(const Path &file, Piece *pieces, i32 piece_cap, i32 *piece_count, Marker *marks,
                              i32 mark_cap, i32 *mark_count)
{
    *piece_count = *mark_count = 0;
    FILE *f = _wfopen(file.s, L"rb");
    if (!f) return false;
    static thread_local char line[LAYER_TEXT_MAX + 128];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        long long a = 0, z = 0, at = 0;
        i32 colour = 0, used = 0;
        if (!strncmp(line, "piece = ", 8)) {
            if (sscanf_s(line + 8, "%lld %lld %lld", &a, &z, &at) == 3 && z > a && at >= 0 && *piece_count < piece_cap)
                pieces[(*piece_count)++] = {a, z, at};
        } else if (!strncmp(line, "mark = ", 7) && *mark_count < mark_cap) {
            if (sscanf_s(line + 7, "%lld %d %n", &a, &colour, &used) < 2 || !used || a <= 0) continue;
            Marker &m = marks[(*mark_count)++];
            m = {};
            m.t_us = a;
            m.colour = colour > 0 && colour < (i32)COUNT(MARKER_COLOURS) ? (u8)colour : 0;
            char *note = line + 7 + used, *tab = strchr(note, '\t');
            if (tab) *tab++ = 0;
            marker_note_set(m.note, note);
            marker_detail_set(m.detail, tab);
        }
    }
    fclose(f);
    return true;
}

// Adds a marker to a saved video's layers file, when its moment is in the video and it isn't there already. True
// when it was added.
static bool layers_mark_add(const Path &file, i64 t_us, u8 colour, const char *note, const char *detail = 0)
{
    static thread_local Piece pieces[512];
    static thread_local Marker marks[256];
    i32 np, nm;
    if (!layers_file_marks(file, pieces, COUNT(pieces), &np, marks, COUNT(marks), &nm)) return false;
    if (file_time(pieces, np, t_us) < 0 || nm == COUNT(marks)) return false;
    Marker m = {};
    marker_note_set(m.note, note);
    marker_detail_set(m.detail, detail);
    for (i32 i = 0; i < nm; i++)
        if (marks[i].t_us == t_us && !strcmp(marks[i].note, m.note)) return false;
    FILE *f = _wfopen(file.s, L"ab");
    if (!f) return false;
    fprintf(f, "mark = %lld %d %s%s%s\n", (long long)t_us, colour < COUNT(MARKER_COLOURS) ? colour : 0, m.note,
            m.detail[0] ? "\t" : "", m.detail);
    fclose(f);
    return true;
}

// A saved video's late markers, in the video's time, oldest first. How many.
static i32 layers_marks_read(const Path &file, Marker *out, i32 cap)
{
    static thread_local Piece pieces[512];
    i32 np, n, kept = 0;
    if (!layers_file_marks(file, pieces, COUNT(pieces), &np, out, cap, &n)) return 0;
    for (i32 i = 0; i < n; i++) {
        i64 t = file_time(pieces, np, out[i].t_us);
        if (t < 0) continue;
        out[kept] = out[i];
        out[kept++].t_us = t;
    }
    for (i32 i = 1; i < kept; i++) // oldest first
        for (i32 j = i; j > 0 && out[j - 1].t_us > out[j].t_us; j--) {
            Marker m = out[j];
            out[j] = out[j - 1], out[j - 1] = m;
        }
    return kept;
}

// The layers there are, without their samples, in the order registered. Any thread.
static i32 layers_heads(Layer *out, i32 cap)
{
    LayerStore *ls = &g_layers;
    AcquireSRWLockExclusive(&ls->lock);
    layers_load_locked(ls);
    i32 n = 0;
    for (i32 k = 0; k < ls->set.count && n < cap; k++) {
        out[n] = ls->set.layers[k];
        out[n].count = out[n].cap = 0;
        out[n++].s = 0;
    }
    ReleaseSRWLockExclusive(&ls->lock);
    return n;
}

// ---------------------------------------------------------------------------------------------------------------
// Values as they are shown: the range and the spikes

// The range a Value's colours run over: its own, or else what its samples reach.
static void layer_range(const Layer &l, f64 *lo, f64 *hi)
{
    *lo = l.lo, *hi = l.hi;
    if (l.hi > l.lo) return;
    for (i32 i = 0; i < l.count; i++) {
        if (!i || l.s[i].value < *lo) *lo = l.s[i].value;
        if (!i || l.s[i].value > *hi) *hi = l.s[i].value;
    }
    if (*hi <= *lo) *hi = *lo + 1;
}

// A sudden rise: a sample at least LAYER_SPIKE_JUMP of the range over the mean of the samples 3 to 6 before it,
// lasting while the samples stay LAYER_SPIKE_HOLD of the range over that mean.
struct LayerSpike {
    i64 a, z;    // its first and last sample
    i64 peak_us; // its highest
    f64 peak;
};
static const f64 LAYER_SPIKE_JUMP = 0.18, LAYER_SPIKE_HOLD = 0.09;

static i32 layer_spikes(const Layer &l, f64 lo, f64 hi, LayerSpike *out, i32 cap)
{
    i32 n = 0;
    f64 range = hi - lo;
    for (i32 i = 6; i < l.count && n < cap; i++) {
        f64 base = (l.s[i - 6].value + l.s[i - 5].value + l.s[i - 4].value + l.s[i - 3].value) / 4;
        if (l.s[i].value - base < LAYER_SPIKE_JUMP * range) continue;
        LayerSpike sp = {l.s[i].t_us, l.s[i].t_us, l.s[i].t_us, l.s[i].value};
        i32 j = i + 1;
        for (; j < l.count && l.s[j].value - base >= LAYER_SPIKE_HOLD * range; j++) {
            sp.z = l.s[j].t_us;
            if (l.s[j].value > sp.peak) sp.peak = l.s[j].value, sp.peak_us = l.s[j].t_us;
        }
        out[n++] = sp;
        i = j - 1;
    }
    return n;
}

// ---------------------------------------------------------------------------------------------------------------
// Spans as they are shown: steady runs

// What a Spans layer shows over a stretch: one run per label.
struct LayerRun {
    i64         a, z;
    const char *label; // the layer's own text, never ""
};

// Runs as drawn at a zoom where a pixel column is `col_us` long. A run narrower than a column is gathered with the
// others in its column, and the column shows the label that covers most of it. Runs of one label less than a
// column apart join. So there are at most about two runs a column however many the layer has, and past `cap`
// nothing is dropped: the last run reaches on to the end.
struct LayerRunLod {
    LayerRun   *out;
    i32         n, cap;
    i64         from, col_us; // col_us 0: every run as it is
    i64         col;          // the column being gathered, -1 = none
    i64         ca, cz;       // what its runs cover
    const char *label[8];
    i64         cover[8];
    i32         labels;
};

static LayerRunLod layer_lod_begin(LayerRun *out, i32 cap, i64 from, i64 col_us)
{
    LayerRunLod b = {};
    b.out = out, b.cap = cap, b.from = from, b.col_us = col_us > 0 ? col_us : 0, b.col = -1;
    return b;
}

static void layer_lod_emit(LayerRunLod *b, LayerRun r)
{
    if (b->n) {
        LayerRun &last = b->out[b->n - 1];
        if (r.a < last.z) r.a = last.z;
        bool join = !strcmp(last.label, r.label) && r.a <= last.z + b->col_us;
        if (join || b->n == b->cap || r.z <= r.a) {
            if (r.z > last.z) last.z = r.z;
            return;
        }
    }
    if (b->cap > 0) b->out[b->n++] = r;
}

static void layer_lod_flush(LayerRunLod *b)
{
    if (b->col < 0) return;
    i32 best = 0;
    for (i32 k = 1; k < b->labels; k++)
        if (b->cover[k] > b->cover[best]) best = k;
    LayerRun r = {b->ca, b->cz, b->label[best]};
    b->col = -1;
    b->labels = 0;
    layer_lod_emit(b, r);
}

static void layer_lod_add(LayerRunLod *b, LayerRun r)
{
    if (!b->col_us || r.z - r.a >= b->col_us) {
        layer_lod_flush(b);
        layer_lod_emit(b, r);
        return;
    }
    i64 d = r.a - b->from, col = d >= 0 ? d / b->col_us : -((-d + b->col_us - 1) / b->col_us);
    if (col != b->col) {
        layer_lod_flush(b);
        b->col = col, b->ca = r.a, b->cz = r.z;
    }
    if (r.a < b->ca) b->ca = r.a;
    if (r.z > b->cz) b->cz = r.z;
    i32 k = 0;
    while (k < b->labels && strcmp(b->label[k], r.label)) k++;
    if (k == b->labels) {
        if (k < COUNT(b->label)) b->label[b->labels++] = r.label, b->cover[k] = 0;
        else k = COUNT(b->label) - 1; // more labels in one column than that: counted with the last
    }
    b->cover[k] += r.z - r.a;
}

static i32 layer_lod_end(LayerRunLod *b)
{
    layer_lod_flush(b);
    return b->n;
}

// The runs of a Spans (or Text) layer that reach into [from, to): a sample shorter than `least_us` is left out and
// the run before it carries on over it, and runs of one label next to each other join. "" (nothing in front) is a
// gap. A held last sample lasts until `open_end`. With `col_us`, they are gathered into pixel columns
// (LayerRunLod). Returns how many, at most `cap`.
static const i64 LAYER_STEADY_US = 2000000;
static i32 layer_steady_runs(const Layer &l, i64 from, i64 to, i64 open_end, i64 least_us, i64 col_us, LayerRun *out,
                             i32 cap)
{
    LayerRunLod lod = layer_lod_begin(out, cap, from, col_us);
    LayerRun cur = {};
    bool have = false;
    // Back from the sample in force at `from` to where its run starts, so a run reaching in from before keeps it
    i32 lo = 0, hi = l.count;
    while (lo < hi) {
        i32 mid = (lo + hi) / 2;
        if (l.s[mid].t_us < from) lo = mid + 1;
        else hi = mid;
    }
    i32 i = lo > 0 ? lo - 1 : 0;
    auto short_at = [&](i32 k) { return layer_end_of(l, k, open_end) - l.s[k].t_us < least_us; };
    auto label_at = [&](i32 k) { return l.s[k].text ? l.s[k].text : ""; };
    while (i > 0 && (short_at(i - 1) || short_at(i) || !strcmp(label_at(i - 1), label_at(i)))) i--;
    for (; i < l.count && l.s[i].t_us < to; i++) {
        i64 a = l.s[i].t_us, z = layer_end_of(l, i, open_end);
        const char *label = l.s[i].text ? l.s[i].text : "";
        if (z <= a) continue;
        if (have && (z - a < least_us || !strcmp(cur.label, label)) && a <= cur.z + least_us) {
            if (z > cur.z) cur.z = z;
            continue;
        }
        if (have && cur.label[0] && cur.z > from) layer_lod_add(&lod, cur);
        cur = {a, z, label};
        have = true;
    }
    if (have && cur.label[0] && cur.z > from && cur.a < to) layer_lod_add(&lod, cur);
    return layer_lod_end(&lod);
}

// ---------------------------------------------------------------------------------------------------------------
// Burning layers into the picture: an .ass file for ffmpeg's subtitles filter

// Where a stretch of the source video plays in the render: [from, to] in the source at `at` in the output, seconds.
struct LayerSpan { f64 from, to, at; };

static bool layer_switched_on(const char (*on)[LAYER_NAME_MAX], i32 on_count, const char *name)
{
    for (i32 i = 0; i < on_count; i++)
        if (!strcmp(on[i], name)) return true;
    return false;
}

static void ass_time(f64 s, char *out, i32 cap)
{
    i64 cs = (i64)llround(s * 100);
    if (cs < 0) cs = 0;
    snprintf(out, cap, "%lld:%02lld:%02lld.%02lld", cs / 360000, cs / 6000 % 60, cs / 100 % 60, cs % 100);
}

// Text as an event's text: one line, braces shown as braces, a backslash kept from starting an override.
static void ass_text(FILE *f, const char *s)
{
    static const u8 joiner[] = {0xe2, 0x81, 0xa0}; // U+2060 in UTF-8, between a backslash and what follows
    for (; *s; s++) {
        if (*s == '{' || *s == '}') fputc('\\', f), fputc(*s, f);
        else if (*s == '\\') fputc('\\', f), fwrite(joiner, 1, sizeof joiner, f);
        else fputc(*s, f);
    }
}

// One event for [a, b) in the source, wherever the spans play it. Returns how many were written; f = 0 counts.
static i32 ass_event(FILE *f, const LayerSpan *spans, i32 n, f64 a, f64 b, const char *style, i32 margin_v,
                     const char *text)
{
    i32 count = 0;
    for (i32 k = 0; k < n; k++) {
        f64 s = ImMax(a, spans[k].from), e = ImMin(b, spans[k].to);
        if (e - s < 0.01) continue;
        count++;
        if (!f) continue;
        char ts[32], te[32];
        ass_time(spans[k].at + s - spans[k].from, ts, sizeof ts);
        ass_time(spans[k].at + e - spans[k].from, te, sizeof te);
        fprintf(f, "Dialogue: 0,%s,%s,%s,,0,0,%d,,", ts, te, style, margin_v);
        ass_text(f, text);
        fputc('\n', f);
    }
    return count;
}

// What an Input layer shows while keys are held: a letter or digit as itself, anything else as #code.
static void layer_keys_text(const u32 *held, i32 n, char *out, i32 cap)
{
    i32 len = 0;
    out[0] = 0;
    for (i32 i = 0; i < n && len < cap - 16; i++) {
        u32 c = held[i];
        bool plain = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z');
        len += plain ? snprintf(out + len, cap - len, "%s%c", len ? " " : "", (char)c)
                     : snprintf(out + len, cap - len, "%s#%u", len ? " " : "", c);
    }
}

// The layers switched on, as an .ass file for a w x h picture `dur` seconds long whose source plays through `spans`:
// a Value at the top right as its number, a Text as captions at the bottom, an Input at the bottom left as the keys
// held, Spans at the top left as the steady runs' labels; more than one of a kind stack. Returns how many events it holds; f = 0 only counts them.
static i32 layers_ass(FILE *f, const LayerSet &set, const char (*on)[LAYER_NAME_MAX], i32 on_count,
                      const LayerSpan *spans, i32 n, f64 dur, i32 w, i32 h)
{
    i32 size = h / 22 > 12 ? h / 22 : 12, margin = h / 30 > 8 ? h / 30 : 8, line = size * 5 / 4;
    if (f) {
        fprintf(f, "[Script Info]\nScriptType: v4.00+\nPlayResX: %d\nPlayResY: %d\nWrapStyle: 0\n"
                   "ScaledBorderAndShadow: yes\n\n[V4+ Styles]\n"
                   "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, "
                   "Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, "
                   "Alignment, MarginL, MarginR, MarginV, Encoding\n", w, h);
        static const struct { const char *name; i32 align; } styles[] = {{"Value", 9}, {"Text", 2}, {"Input", 1},
                                                                         {"Spans", 7}};
        for (auto &s : styles)
            fprintf(f, "Style: %s,Segoe UI,%d,&H00FFFFFF,&H00FFFFFF,&H00000000,&H80000000,-1,0,0,0,100,100,0,0,1,%d,0,"
                       "%d,%d,%d,%d,1\n", s.name, size, size / 14 > 1 ? size / 14 : 1, s.align, margin, margin, margin);
        fprintf(f, "\n[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n");
    }
    f64 src_end = 0;
    for (i32 k = 0; k < n; k++) src_end = ImMax(src_end, spans[k].to);
    if (src_end <= 0) src_end = dur;
    i32 count = 0, stack[LAYER_KINDS] = {};
    for (i32 k = 0; k < set.count; k++) {
        const Layer &l = set.layers[k];
        if (!layer_switched_on(on, on_count, l.name)) continue;
        i32 mv = margin + line * stack[l.kind]++;
        if (l.kind == LAYER_VALUE) {
            for (i32 i = 0; i < l.count; i++) {
                f64 a = l.s[i].t_us / 1e6, b = i + 1 < l.count ? l.s[i + 1].t_us / 1e6 : src_end;
                f64 v = l.s[i].value;
                char text[48];
                if (fabs(v - llround(v)) < 1e-6) snprintf(text, sizeof text, "%lld", (long long)llround(v));
                else snprintf(text, sizeof text, "%.1f", v);
                count += ass_event(f, spans, n, a, b, "Value", mv, text);
            }
        } else if (l.kind == LAYER_TEXT) {
            for (i32 i = 0; i < l.count; i++)
                if (l.s[i].text && l.s[i].text[0])
                    count += ass_event(f, spans, n, l.s[i].t_us / 1e6, l.s[i].end_us / 1e6, "Text", mv, l.s[i].text);
        } else if (l.kind == LAYER_SPANS) {
            static LayerRun runs[4096];
            i64 end_us = (i64)llround(src_end * 1e6);
            i32 nr = layer_steady_runs(l, 0, end_us, end_us, LAYER_STEADY_US, 0, runs, COUNT(runs));
            for (i32 i = 0; i < nr; i++)
                count += ass_event(f, spans, n, runs[i].a / 1e6, runs[i].z / 1e6, "Spans", mv, runs[i].label);
        } else {
            u32 held[32];
            i32 nh = 0;
            for (i32 i = 0; i < l.count; i++) {
                const LayerSample &s = l.s[i];
                i32 at = 0;
                while (at < nh && held[at] != s.code) at++;
                if (s.value != 0 && at == nh && nh < COUNT(held)) held[nh++] = s.code;
                else if (s.value == 0 && at < nh) memmove(held + at, held + at + 1, (--nh - at) * sizeof *held);
                if (!nh || (i + 1 < l.count && l.s[i + 1].t_us == s.t_us)) continue; // shown once its moment is done
                char text[256];
                layer_keys_text(held, nh, text, sizeof text);
                count += ass_event(f, spans, n, s.t_us / 1e6, i + 1 < l.count ? l.s[i + 1].t_us / 1e6 : src_end,
                                   "Input", mv, text);
            }
        }
    }
    return count;
}
