// layers.cpp: things that happen while the buffer records, kept beside it and written into each clip.
//
// A layer is one stream of samples from one source, on the wall clock like markers, of one of three kinds:
//   Value  a number over time; each sample holds until the next
//   Text   lines, each with a start and an end
//   Input  key and button downs and ups, by code
// A source registers its layer by name, pushes samples as they come, and may later replace a span of its own
// samples (a better transcript of the same minute, say). The store keeps them for as long as the buffer keeps
// footage: <local>\layers\<hash of the name>.tsv, a line of <kind> <tab> <name>, then a line per sample of
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

static const char *const LAYER_KIND_NAMES[LAYER_KINDS] = {"value", "text", "input"};

static i32 layer_kind_named(const char *word)
{
    for (i32 k = 0; k < LAYER_KINDS; k++)
        if (!strcmp(word, LAYER_KIND_NAMES[k])) return k;
    return -1;
}

// A name a layer can have: 1 to LAYER_NAME_MAX - 1 bytes of printable text, no tab, nothing around it.
static bool layer_name_ok(const char *name)
{
    size_t n = name ? strlen(name) : 0;
    if (!n || n >= LAYER_NAME_MAX || name[0] == ' ' || name[n - 1] == ' ') return false;
    for (size_t i = 0; i < n; i++)
        if ((u8)name[i] < 0x20 || name[i] == 0x7f) return false;
    return true;
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
    else if (kind == LAYER_TEXT) {
        s.end_us = in.end_us > in.t_us ? in.end_us : in.t_us;
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
    fprintf(f, "%s\t%s\n", LAYER_KIND_NAMES[l.kind], l.name);
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
        char *fld[2];
        i32 kind = layer_fields(line, fld, 2) == 2 ? layer_kind_named(fld[0]) : -1;
        if (kind >= 0 && layer_name_ok(fld[1]) && !wcscmp(layer_file(ls, fld[1]).s, file.s)) {
            l = &ls->set.layers[ls->set.count++];
            *l = {};
            str_set(l->name, fld[1]);
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
}

// ---------------------------------------------------------------------------------------------------------------
// The source interface. Any thread.

// A source's layer: the id to push to, the same one again for a name already registered with this kind (a source
// carries on after a restart). -1 if the name won't do, it is taken by another kind, or there are LAYER_MAX.
static i32 layer_register(const char *name, LayerKind kind)
{
    if (!layer_name_ok(name) || kind >= LAYER_KINDS) return -1;
    LayerStore *ls = &g_layers;
    AcquireSRWLockExclusive(&ls->lock);
    layers_load_locked(ls);
    i32 id = -1;
    for (i32 i = 0; i < ls->set.count && id < 0; i++)
        if (!strcmp(ls->set.layers[i].name, name)) id = ls->set.layers[i].kind == kind ? i : -2;
    if (id == -1 && ls->set.count < LAYER_MAX) {
        id = ls->set.count++;
        Layer &l = ls->set.layers[id];
        l = {};
        str_set(l.name, name);
        l.kind = (u8)kind;
        layer_write_locked(ls, l);
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

static bool layer_input(i32 id, i64 t_us, u32 code, bool down)
{
    LayerSample s = {};
    s.t_us = t_us, s.code = code, s.value = down ? 1 : 0;
    return layer_push(id, s);
}

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
        while (drop < l.count && (l.kind == LAYER_TEXT ? l.s[drop].end_us : l.s[drop].t_us) < before_us) drop++;
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
// before `from` as well, the Text lines that overlap, and an Input's keys held at `from` as downs at `from`.
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
        o = {};
        str_set(o.name, l.name);
        o.kind = l.kind;
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
            if (l.kind == LAYER_TEXT ? s.end_us <= from : i < first) continue;
            if (s.text) s.text = layer_text_dup(s.text);
            layer_insert(&o, s);
        }
        if (o.count) out->count++;
        else layer_free_samples(&o);
    }
    ReleaseSRWLockExclusive(&ls->lock);
}

// ---------------------------------------------------------------------------------------------------------------
// A clip's layers: <save_dir>\Data\<video's name>.layers.txt, times in seconds into the video

static Path layers_clip_path(const Path &save_dir, const wchar_t *stem)
{
    Path p = {};
    swprintf(p.s, MAX_PATH, L"%s\\%s.layers.txt", save_data_dir(save_dir).s, stem);
    return p;
}

static bool layers_write(const Path &file, const LayerSet &set)
{
    Path dir = file;
    wchar_t *slash = wcsrchr(dir.s, L'\\');
    if (slash) *slash = 0;
    SHCreateDirectoryExW(0, dir.s, 0);
    Path tmp = {};
    swprintf(tmp.s, MAX_PATH, L"%s.tmp", file.s);
    FILE *f = _wfopen(tmp.s, L"wb");
    if (!f) return false;
    fprintf(f, "# Rekodr layers: each layer (kind name), then its samples, seconds into the video:\n"
               "# v = time value; t = time end text; i = time code down (1) or up (0)\n");
    for (i32 k = 0; k < set.count; k++) {
        const Layer &l = set.layers[k];
        fprintf(f, "layer = %s %s\n", LAYER_KIND_NAMES[l.kind], l.name);
        for (i32 i = 0; i < l.count; i++) {
            const LayerSample &s = l.s[i];
            if (l.kind == LAYER_VALUE) fprintf(f, "v = %.3f %.9g\n", s.t_us / 1e6, s.value);
            else if (l.kind == LAYER_TEXT) fprintf(f, "t = %.3f %.3f %s\n", s.t_us / 1e6, s.end_us / 1e6, s.text ? s.text : "");
            else fprintf(f, "i = %.3f %u %d\n", s.t_us / 1e6, s.code, s.value != 0 ? 1 : 0);
        }
    }
    fclose(f);
    return MoveFileExW(tmp.s, file.s, MOVEFILE_REPLACE_EXISTING) != 0;
}

// A clip's layers, into a set of their own (layer_set_free). False when it has none.
static bool layers_read(const Path &file, LayerSet *set)
{
    *set = {};
    FILE *f = _wfopen(file.s, L"rb");
    if (!f) return false;
    static char line[LAYER_TEXT_MAX + 128];
    Layer *l = 0;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (!eq || line[0] == '#') continue;
        *eq = 0;
        char *k = trim(line), *v = eq + 1;
        if (*v == ' ') v++;
        if (!strcmp(k, "layer")) {
            l = 0;
            char *sp = strchr(v, ' ');
            if (!sp || set->count == LAYER_MAX) continue;
            *sp = 0;
            i32 kind = layer_kind_named(v);
            if (kind < 0 || !layer_name_ok(sp + 1)) continue;
            l = &set->layers[set->count++];
            *l = {};
            str_set(l->name, sp + 1);
            l->kind = (u8)kind;
            continue;
        }
        if (!l || strlen(k) != 1 || *k != "vti"[l->kind]) continue;
        LayerSample s = {};
        f64 t = 0, end = 0;
        i32 down = 0, used = 0;
        if (l->kind == LAYER_VALUE) {
            if (sscanf_s(v, "%lf %lf", &t, &s.value) != 2) continue;
        } else if (l->kind == LAYER_TEXT) {
            if (sscanf_s(v, "%lf %lf %n", &t, &end, &used) < 2 || !used) continue;
            s.end_us = (i64)llround(end * 1e6);
            s.text = v + used;
        } else {
            if (sscanf_s(v, "%lf %u %d", &t, &s.code, &down) != 3) continue;
            s.value = down ? 1 : 0;
        }
        s.t_us = (i64)llround(t * 1e6);
        if (t >= 0) layer_insert(l, layer_sample_clean(l->kind, s));
    }
    fclose(f);
    return set->count > 0;
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
// held; more than one of a kind stack. Returns how many events it holds; f = 0 only counts them.
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
        static const struct { const char *name; i32 align; } styles[] = {{"Value", 9}, {"Text", 2}, {"Input", 1}};
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
