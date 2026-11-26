// markers.cpp: moments marked in the buffer, each with an optional colour and note.
//
// The `mark` command makes one, from a hotkey, a Stream Deck key or `rekodr.exe mark`:
//   mark                    a plain marker at this moment
//   mark <note>             one with a note
//   mark <colour> <note>    one with a colour (red, orange, yellow, green, blue, purple) and, if given, a note
//   mark ask                a plain marker at this moment, and a toast to give it a colour and a note (toast.cpp)
// The moment is when the command came, not when the note was finished, and the marker is kept even if the window
// is closed without a word.
//
// They are kept in <local>\markers.tsv, a line each of <time in us> <tab> <colour's name> <tab> <note>, oldest
// first, the newest MARKER_MAX of them. markers_in gives the ones in a stretch of time, for the timeline to draw
// and for a save to write into the file as chapters.

enum { MARKER_MAX = 2048 };
struct Markers {
    SRWLOCK lock;
    bool    loaded;
    i32     count;
    Marker  items[MARKER_MAX]; // oldest first
};
static Markers g_markers;

// The colour a word names, 0 if it names none.
static u8 marker_colour_named(const char *word, size_t len)
{
    for (i32 i = 1; i < COUNT(MARKER_COLOURS); i++)
        if (strlen(MARKER_COLOURS[i].name) == len && !_strnicmp(MARKER_COLOURS[i].name, word, len)) return (u8)i;
    return 0;
}

// A note as it is kept: one line, no tabs, nothing around it.
static void marker_note_set(char (&note)[256], const char *text)
{
    while (*text == ' ') text++;
    i32 n = 0;
    for (; *text && n < (i32)sizeof note - 1; text++) note[n++] = (u8)*text < 0x20 ? ' ' : *text;
    while (n && note[n - 1] == ' ') n--;
    note[n] = 0;
}

// What follows `mark` on the command line: a colour if the first word names one, and the rest as the note.
static void marker_args(const char *arg, u8 *colour, char (&note)[256])
{
    while (*arg == ' ') arg++;
    size_t word = strcspn(arg, " ");
    *colour = marker_colour_named(arg, word);
    marker_note_set(note, *colour ? arg + word : arg);
}

static i32 marker_line(const Marker &m, char *out, i32 cap)
{
    return snprintf(out, cap, "%lld\t%s\t%s\n", (long long)m.t_us, MARKER_COLOURS[m.colour].name, m.note);
}

// A line of markers.tsv. False if it isn't one.
static bool marker_from_line(char *line, Marker *m)
{
    size_t len = strlen(line);
    while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = 0;
    char *f[3];
    if (split_tabs(line, f, 3) != 3) return false;
    *m = {};
    m->t_us = strtoll(f[0], 0, 10);
    if (m->t_us <= 0) return false;
    m->colour = marker_colour_named(f[1], strlen(f[1]));
    marker_note_set(m->note, f[2]);
    return true;
}

// Puts a marker in its place by time, moved on a microsecond at a time until its time is its own. With the list
// full, the oldest goes. Returns the time it was given.
static i64 markers_insert(Markers *ms, Marker m)
{
    if (ms->count == MARKER_MAX) {
        memmove(&ms->items[0], &ms->items[1], (MARKER_MAX - 1) * sizeof ms->items[0]);
        ms->count--;
    }
    i32 at = ms->count;
    while (at > 0 && ms->items[at - 1].t_us > m.t_us) at--;
    while (at > 0 && ms->items[at - 1].t_us == m.t_us) m.t_us++;
    while (at < ms->count && ms->items[at].t_us <= m.t_us) {
        if (ms->items[at].t_us == m.t_us) m.t_us++;
        at++;
    }
    memmove(&ms->items[at + 1], &ms->items[at], (ms->count - at) * sizeof ms->items[0]);
    ms->items[at] = m;
    ms->count++;
    return m.t_us;
}

// The markers in [from, to], oldest first. How many.
static i32 markers_range(const Markers *ms, i64 from, i64 to, Marker *out, i32 cap)
{
    i32 n = 0;
    for (i32 i = 0; i < ms->count && n < cap; i++)
        if (ms->items[i].t_us >= from && ms->items[i].t_us <= to) out[n++] = ms->items[i];
    return n;
}

static void markers_load_locked()
{
    if (g_markers.loaded) return;
    g_markers.loaded = true;
    FILE *f = _wfopen(path_join(g_app.paths.local_dir, L"markers.tsv").s, L"rb");
    if (!f) return;
    char line[512];
    Marker m;
    while (fgets(line, sizeof line, f))
        if (marker_from_line(line, &m)) markers_insert(&g_markers, m);
    fclose(f);
}

// Writes markers.tsv whole, through a temp file. Called with the lock held.
static void markers_write_locked()
{
    Path file = path_join(g_app.paths.local_dir, L"markers.tsv"), tmp = file;
    wcscat_s(tmp.s, L".tmp");
    FILE *f = _wfopen(tmp.s, L"wb");
    if (!f) { log_line("markers: couldn't write markers.tsv (%d)", (int)GetLastError()); return; }
    char line[512];
    for (i32 i = 0; i < g_markers.count; i++) {
        i32 n = marker_line(g_markers.items[i], line, sizeof line);
        if (n > 0 && n < (i32)sizeof line) fwrite(line, 1, (size_t)n, f);
    }
    fclose(f);
    if (!MoveFileExW(tmp.s, file.s, MOVEFILE_REPLACE_EXISTING)) log_line("markers: couldn't replace markers.tsv (%d)", (int)GetLastError());
}

// A new marker. Returns its time, which is how it is named afterwards.
static i64 markers_add(i64 t_us, u8 colour, const char *note)
{
    Marker m = {};
    m.t_us = t_us;
    m.colour = colour < COUNT(MARKER_COLOURS) ? colour : 0;
    marker_note_set(m.note, note);
    AcquireSRWLockExclusive(&g_markers.lock);
    markers_load_locked();
    i64 id = markers_insert(&g_markers, m);
    markers_write_locked();
    ReleaseSRWLockExclusive(&g_markers.lock);
    return id;
}

// Gives the marker made at t_us a colour and a note.
static void markers_set(i64 t_us, u8 colour, const char *note)
{
    AcquireSRWLockExclusive(&g_markers.lock);
    markers_load_locked();
    for (i32 i = 0; i < g_markers.count; i++) {
        Marker &m = g_markers.items[i];
        if (m.t_us != t_us) continue;
        m.colour = colour < COUNT(MARKER_COLOURS) ? colour : 0;
        marker_note_set(m.note, note);
        markers_write_locked();
        break;
    }
    ReleaseSRWLockExclusive(&g_markers.lock);
}

// A marker made after its moment (from a game's replay, read once the match is over): on the timeline, and in the
// layers file of each clip or session saved over it, since the save's chapters are already written. Main thread.
static void markers_add_late(i64 t_us, u8 colour, const char *note)
{
    markers_add(t_us, colour, note);
    Saving &sv = g_app.saving;
    saved_load();
    for (i32 i = 0; i < sv.saved_count; i++) {
        const SavedRange &r = sv.saved[i];
        if (t_us < r.from_us || t_us >= r.to_us) continue;
        Path video = {};
        utf8_to_wide(r.path, video.s, COUNT(video.s));
        if (layers_mark_add(layers_video_path(g_app.config.save_dir, video), t_us, colour, note))
            log_line("markers: \"%s\" added to %s", note, r.path);
    }
}

// The markers in [from, to], oldest first, for the timeline and for a save's chapters. Any thread.
static i32 markers_in(i64 from, i64 to, Marker *out, i32 cap)
{
    AcquireSRWLockExclusive(&g_markers.lock);
    markers_load_locked();
    i32 n = markers_range(&g_markers, from, to, out, cap);
    ReleaseSRWLockExclusive(&g_markers.lock);
    return n;
}

static void toast_marker(i64 t_us); // toast.cpp

// The `mark` command. Main thread.
static void marker_command(const char *arg)
{
    i64 t = now_us();
    sound_event(SOUND_MARKER);
    if (!strcmp(arg, "ask")) {
        toast_marker(markers_add(t, 0, ""));
        log_line("marker: asking for a colour and a note");
    } else {
        u8 colour;
        char note[256];
        marker_args(arg, &colour, note);
        markers_add(t, colour, note);
        log_line("marker: %s%s%s", MARKER_COLOURS[colour].name, colour && note[0] ? ", " : "", note[0] ? note : colour ? "" : "plain");
    }
    ui_wake();
}
