// Playable copies of edited clips, so Clips\<year> always holds something that plays anywhere as it was meant.
//
// An unedited clip stays as it is, and so does another app's video (LibItem.dir), edited or not. An edited clip's
// Clips\<year>\<name>.mp4 is its edit rendered as Keep makes it (save.cpp: at the recording's quality, every track
// kept with the Mix rebuilt, a single trim stream-copied), and the untouched original is
// Clips\<year>\Source\<name>.mp4. The library lists the clip by its original (LibItem.rendered): its edit,
// pictures, records and the editor all keep to that file.
//
// A render is made on the save worker at low priority into Data\Rendering, then swapped in with ReplaceFile,
// which puts the render in Clips and moves the original into Source in one step on the same drive: Clips never
// lacks the file, and a render that fails changes nothing. Data\<name>.render.txt notes the edit it was made
// from. One render runs at a time, only with no other save waiting and no game being played: a save or a game
// stops it, and it's made again later. An edit is rendered once the editor is off the clip; while the editor is on
// a clip already rendered, 30 s after its last change, and a first render finished meanwhile waits to be swapped in
// until the editor is off it. A changed edit is rendered again. An edit reset to nothing, or
// render_edits off, puts the original back over the render.
//
// Open with (editors.cpp) asks for a render when the clip's copy isn't its edit as it is now (render_ask): that
// render starts at once, ahead of any other, and goes on while a game is played. A first render is swapped in under
// the editor, which lets go of the original and plays it again from Source. Then the editor app starts with the
// copy. With render_edits off the copy isn't swapped in: the app gets it from Data\Rendering. A render that fails
// sends nothing.

static const i64 RENDER_SETTLE_US = 30ll * 1000000;
static const u64 RENDER_EVERY_MS = 10000; // how often the clips are looked over

struct RenderNote {
    i64 edit_mtime_us; // the edit file's time when the render was made or last found the same
    u64 edit_hash;     // its bytes
};

struct RenderState {
    u32     job;        // the render on the save worker, 0 = none
    LibItem item;       // the clip it's for
    i64     edit_mtime_us;
    u64     edit_hash;
    Path    made;       // a finished render still to be swapped in ("" = none)
    i32     swap_tries;
    u64     next_ms;    // the next look over the clips
    bool    cleaned;    // leftovers in Data\Rendering cleared since start
    struct { u64 key, hash; } failed[32]; // renders that failed, not tried again until the edit changes
    i32     failed_at;
    u64     restore_logged; // the key whose original couldn't go back, logged once
    u64     waiting[LIB_MAX_ITEMS]; // clips the last look found needing a render, not yet started (keys)
    i32     waiting_count;
    u64     ask_key;      // the clip Open with waits on (render_ask), 0 = none
    Path    ask_exe;      // the editor app it opens in
    char    ask_name[64];
};
static RenderState g_render;

// What a clip's rendered copies come to: how many, and their bytes into `bytes`. Turning render_edits off frees
// that much.
static i32 render_copies_in(const LibItem *items, i32 n, i64 *bytes)
{
    i32 count = 0;
    i64 sum = 0;
    for (i32 i = 0; i < n; i++)
        if (items[i].rendered && items[i].render_size) {
            count++;
            sum += (i64)items[i].render_size;
        }
    if (bytes) *bytes = sum;
    return count;
}

// The library's rendered copies now (render_copies_in): for Options' storage and the render_edits switch.
static i32 render_copies(i64 *bytes)
{
    const Library &lib = g_app.library;
    return render_copies_in(lib.items, lib.item_count, bytes);
}

// How far the render of the clip `key` has got, 0..1, or -1 when none is running for it: the library card's mark.
static f32 render_progress(u64 key)
{
    if (!g_render.job || g_render.item.key != key) return -1;
    SaveJobView *v = save_view(g_render.job);
    return v && v->state == SAVE_RUNNING ? v->progress : 0;
}

// Whether a clip's edit waits to be rendered: found needing it on the last look over the clips (every
// RENDER_EVERY_MS), and not the one rendering now (render_progress).
static bool render_waiting(u64 key)
{
    if (g_render.job && g_render.item.key == key) return false;
    for (i32 i = 0; i < g_render.waiting_count; i++)
        if (g_render.waiting[i] == key) return true;
    return false;
}

// What a clip needs: put the original back (no edit, or rendering off), render it (an edit not rendered as it is
// now), or nothing. `has_edit` is a non-empty edit, `same` the render note matches it.
enum RenderNeed : u8 { RENDER_NOTHING, RENDER_RESTORE, RENDER_MAKE };
static RenderNeed render_need(bool on, bool rendered, bool has_edit, bool same)
{
    if (!on || !has_edit) return rendered ? RENDER_RESTORE : RENDER_NOTHING;
    return rendered && same ? RENDER_NOTHING : RENDER_MAKE;
}

// Whether a render may start for a clip whose edit changed at `edit_mtime_us`: the editor is off it, or the clip is
// rendered already (the editor holds the original in Source, which a new render doesn't move) and the edit has
// been left alone RENDER_SETTLE_US.
static bool render_settled(bool editor_on_it, bool rendered, i64 edit_mtime_us, i64 now)
{
    return !editor_on_it || (rendered && now - edit_mtime_us >= RENDER_SETTLE_US);
}

static bool render_note_read(const LibItem &it, RenderNote *n)
{
    *n = {};
    FILE *f = _wfopen(lib_render_note_path(it).s, L"rb");
    if (!f) return false;
    bool ok = fscanf(f, "edit_time = %lld\nedit_hash = %llx", &n->edit_mtime_us, &n->edit_hash) == 2;
    fclose(f);
    return ok;
}

static void render_note_write(const LibItem &it, const RenderNote &n)
{
    Path p = lib_render_note_path(it);
    FILE *f = _wfopen(p.s, L"wb");
    if (!f) return;
    fprintf(f, "edit_time = %lld\nedit_hash = %llx\n", n.edit_mtime_us, n.edit_hash);
    fclose(f);
}

// The edit file's time and the hash of its bytes; false when there's none.
static bool render_edit_stamp(const LibItem &it, i64 *mtime_us, u64 *hash)
{
    Path ef = lib_edit_path(it);
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(ef.s, GetFileExInfoStandard, &fa)) return false;
    *mtime_us = filetime_us(fa.ftLastWriteTime);
    if (!hash) return true;
    FILE *f = _wfopen(ef.s, L"rb");
    if (!f) return false;
    u64 h = fnv64("", 0);
    char buf[4096];
    size_t got;
    while ((got = fread(buf, 1, sizeof buf, f)) > 0) h = fnv64(buf, got, h);
    fclose(f);
    *hash = h;
    return true;
}

// Whether a clip's playable copy is its edit as the edit is now: rendered from an edit with the same bytes.
static bool render_current(const LibItem &it)
{
    RenderNote n;
    i64 mtime;
    u64 hash;
    return it.rendered && render_note_read(it, &n) && render_edit_stamp(it, &mtime, &hash) && n.edit_hash == hash;
}

// Where a render is made before it's swapped in: <save_dir>\Data\Rendering
static Path render_dir() { return path_join(save_data_dir(g_app.config.save_dir), L"Rendering"); }

// Puts a clip's original back in Clips over its render. True when it went (or there was nothing to put back).
static bool render_restore(const LibItem &it)
{
    Path src = lib_item_path(it), clip = lib_playable_path(it);
    if (!file_exists(src.s)) return true;
    if (!MoveFileExW(src.s, clip.s, MOVEFILE_REPLACE_EXISTING)) {
        if (g_render.restore_logged != it.key)
            log_line("render: couldn't put %s's original back (%d); trying again", it.name, (int)GetLastError());
        g_render.restore_logged = it.key;
        return false;
    }
    DeleteFileW(lib_render_note_path(it).s);
    log_line("render: %s is its original again", it.name);
    return true;
}

// Swaps a finished render in for the clip: the original into Source in the same step if it's still in Clips.
// False when it couldn't (the file's open somewhere): it's tried again.
static bool render_swap(const LibItem &it, const Path &made)
{
    Path clip = lib_playable_path(it);
    LibItem orig = it;
    orig.rendered = true;
    Path src = lib_item_path(orig);
    bool ok;
    if (!it.rendered) {
        Path dir = src;
        *wcsrchr(dir.s, L'\\') = 0;
        SHCreateDirectoryExW(0, dir.s, 0);
        if (file_exists(src.s)) {
            log_line("render: %s has an original in Source already; left as it is", it.name);
            return false;
        }
        ok = ReplaceFileW(clip.s, made.s, src.s, REPLACEFILE_IGNORE_MERGE_ERRORS, 0, 0) != 0;
        // The original went to Source but the render couldn't take its place: in it goes now, or the original back
        if (!ok && GetLastError() == ERROR_UNABLE_TO_MOVE_REPLACEMENT_2 && !file_exists(clip.s) &&
            !MoveFileExW(made.s, clip.s, 0))
            MoveFileExW(src.s, clip.s, 0);
        ok = ok || (file_exists(clip.s) && file_exists(src.s) && !file_exists(made.s));
    } else if (file_exists(clip.s)) ok = MoveFileExW(made.s, clip.s, MOVEFILE_REPLACE_EXISTING) != 0;
    else ok = MoveFileExW(made.s, clip.s, 0) != 0;
    if (!ok) log_line("render: couldn't put %s's render in place (%d)", it.name, (int)GetLastError());
    return ok;
}

static bool render_failed_before(u64 key, u64 hash)
{
    for (auto &f : g_render.failed)
        if (f.key == key && f.hash == hash) return true;
    return false;
}

// Queues the render of `it` with its edit `rec`. False when it couldn't be queued.
static bool render_start(const LibItem &it, const EditRecord &rec, i64 edit_mtime_us, u64 hash)
{
    static EditJob j;
    j = {};
    j.src = lib_item_path(it);
    j.out_dir = render_dir();
    j.info = it.info;
    j.edit = rec;
    str_set(j.stem, it.name);
    wchar_t stem[256];
    utf8_to_wide(it.name, stem, COUNT(stem));
    Path old = {};
    swprintf(old.s, MAX_PATH, L"%s\\%s.mp4", j.out_dir.s, stem);
    DeleteFileW(old.s);
    wcscat_s(old.s, L".part");
    DeleteFileW(old.s);
    u32 id = save_queue(SAVE_RENDER, 0, 0, j.stem, &j);
    if (!id) return false;
    g_render.job = id;
    g_render.item = it;
    g_render.edit_mtime_us = edit_mtime_us;
    g_render.edit_hash = hash;
    log_line("render: %s queued (save %u)", it.name, id);
    return true;
}

// The finished render in place: noted, and the library sees the clip as rendered.
static void render_finish()
{
    RenderNote note = {g_render.edit_mtime_us, g_render.edit_hash};
    LibItem it = g_render.item;
    it.rendered = true;
    render_note_write(it, note);
    log_line("render: %s is its edit now, the original in Clips\\Source", g_render.item.name);
    g_render.made = {};
    library_rescan();
}

// The editor is on the clip (it plays the original, which a first render's swap moves).
static bool render_editor_on(const LibItem &it) { return g_app.editor.open && g_app.editor.item.key == it.key; }

// render_swap with the editor on a clip rendered for the first time: the editor lets go of the original, which the
// swap moves into Source, and plays it from there at the same place.
static bool render_swap_editor(const LibItem &it, const Path &made)
{
    if (it.rendered || !render_editor_on(it)) return render_swap(it, made);
    Editor &ed = g_app.editor;
    f64 at = ed.player.pos;
    player_close(&ed.player);
    wave_close(&ed.wave);
    bool ok = render_swap(it, made);
    if (ok) {
        ed.item.rendered = true;
        ed.path = lib_item_path(ed.item);
    }
    ed.tracks_ready = false;
    player_open(&ed.player, ed.path, at, false);
    i32 tracks[MEDIA_MAX_TRACKS];
    i32 n = ed_tracks(ed.item.info, tracks, MEDIA_MAX_TRACKS);
    ed.wave = wave_open(ed.path, ed.item.key, ed.item.info.duration, tracks, n);
    return ok;
}

// What came of an Open with: in the editor's note when the editor is on the clip, else the library's.
static void render_ask_say(u64 key, bool error, const char *text)
{
    Editor &ed = g_app.editor;
    if (ed.open && ed.item.key == key) {
        str_set(ed.result, text);
        ed.result_error = error;
        ui_wake();
    } else lib_note(error, "%s", text);
}

// Starts the editor app Open with asked for with `file`, the render made for it.
static void render_ask_open(const Path &file)
{
    RenderState &r = g_render;
    char msg[160];
    bool ok = open_with_start(r.ask_exe, r.ask_name, file, msg, sizeof msg);
    render_ask_say(r.ask_key, !ok, msg);
    r.ask_key = 0;
}

// The render Open with waits on, finished: swapped in (render_edits on) and sent, or sent from Data\Rendering.
// False when the swap has to be tried again.
static bool render_ask_done()
{
    RenderState &r = g_render;
    if (!g_app.config.render_edits) {
        render_ask_open(r.made);
        r.made = {};
        return true;
    }
    if (!render_swap_editor(r.item, r.made)) return false;
    render_finish();
    LibItem it = r.item;
    it.rendered = true;
    render_ask_open(lib_playable_path(it));
    return true;
}

// The save worker finished, failed or stopped a render.
static void render_on_save(u32 id, i64 what, const char *text)
{
    RenderState &r = g_render;
    if (id != r.job) return;
    r.job = 0;
    bool asked = r.ask_key == r.item.key;
    if (what == SAVE_EV_DONE) {
        utf8_to_wide(text, r.made.s, MAX_PATH);
        r.swap_tries = 0;
        if (asked) render_ask_done();
        else if (!(!r.item.rendered && render_editor_on(r.item)) && render_swap(r.item, r.made)) render_finish();
    } else if (what == SAVE_EV_FAILED) {
        auto &f = r.failed[r.failed_at++ % COUNT(r.failed)];
        f.key = r.item.key;
        f.hash = r.edit_hash;
        if (asked) {
            char msg[160];
            snprintf(msg, sizeof msg, "Couldn't make the edited copy, so nothing went to %s", r.ask_name);
            render_ask_say(r.ask_key, true, msg);
            r.ask_key = 0;
        }
    }
    r.next_ms = 0; // the next clip, if any (an asked render stopped by a save starts again)
}

// Open with asks for the clip's playable copy as its edit is now, to start `exe` with it (render_ask_open): the
// render starts at once, a render of another clip stopping for it. False when it couldn't start.
static bool render_ask(const LibItem &it, const Path &exe, const char *name)
{
    RenderState &r = g_render;
    i64 mtime;
    u64 hash;
    EditRecord rec;
    edit_read(lib_edit_path(it), &rec);
    if (edit_is_default(rec) || !render_edit_stamp(it, &mtime, &hash)) return false;
    r.ask_key = it.key;
    r.ask_exe = exe;
    if (name != r.ask_name) str_set(r.ask_name, name);
    if (r.item.key == it.key && r.edit_hash == hash && (r.job || r.made.s[0])) { // under way, or made
        if (r.made.s[0]) render_ask_done();
        return true;
    }
    if (r.job) {
        log_line("render: %s waits for %s, which Open with asked for", r.item.name, it.name);
        save_cancel(r.job);
        r.job = 0;
    }
    if (r.made.s[0]) { // another clip's, not swapped in yet: made again later
        DeleteFileW(r.made.s);
        r.made = {};
    }
    if (!render_start(it, rec, mtime, hash)) {
        r.ask_key = 0;
        return false;
    }
    return true;
}

// How far the render Open with waits on for clip `key` and `exe` has got, 0..1, or -1 when it waits on none.
static f32 render_ask_progress(u64 key, const Path &exe)
{
    const RenderState &r = g_render;
    if (!r.ask_key || r.ask_key != key || _wcsicmp(r.ask_exe.s, exe.s)) return -1;
    if (r.made.s[0]) return 1;
    return ImMax(0.0f, render_progress(key));
}

// Clears what a render left in Data\Rendering when the app stopped in the middle.
static void render_clean_leftovers()
{
    Path dir = render_dir();
    WIN32_FIND_DATAW f;
    HANDLE h = FindFirstFileW(path_join(dir, L"*").s, &f);
    if (h == INVALID_HANDLE_VALUE) return;
    do
        if (!(f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) DeleteFileW(path_join(dir, f.cFileName).s);
    while (FindNextFileW(h, &f));
    FindClose(h);
}

// Once a second on the main thread: stops a render when a game is played, swaps in a finished one, and every
// RENDER_EVERY_MS looks over the clips for one to render or put back.
static void render_tick()
{
    RenderState &r = g_render;
    bool game = g_app.audio.game.s[0] != 0;
    if (r.job) {
        const SaveJobView *v = save_view(r.job);
        if (!v || v->state >= SAVE_DONE) r.job = 0; // cancelled before it ran
        else if (game && r.ask_key != r.item.key) {
            log_line("render: a game is being played; %s waits", r.item.name);
            save_cancel(r.job);
            r.job = 0;
        }
        return;
    }
    if (r.made.s[0]) {
        bool asked = r.ask_key == r.item.key;
        if (asked) { // the editor lets go of the clip only every few seconds, as its player starts again
            if (++r.swap_tries % 5 == 0 && render_ask_done()) return;
        } else if (!r.item.rendered && render_editor_on(r.item)) return;
        else if (render_swap(r.item, r.made)) {
            render_finish();
            return;
        } else ++r.swap_tries;
        if (r.swap_tries >= 300) { // five minutes of it held open somewhere
            DeleteFileW(r.made.s);
            r.made = {};
            if (asked) {
                char msg[160];
                snprintf(msg, sizeof msg, "The edited copy couldn't go in its place, so nothing went to %s", r.ask_name);
                render_ask_say(r.ask_key, true, msg);
                r.ask_key = 0;
            }
        }
        return;
    }
    if (r.ask_key) { // Open with's render, stopped by a save: again, once nothing else is waiting
        if (g_app.saving.jobs_running > 0) return;
        const LibItem *it = 0;
        for (i32 i = 0; i < g_app.library.item_count; i++)
            if (g_app.library.items[i].key == r.ask_key) it = &g_app.library.items[i];
        if (!it || !render_ask(*it, r.ask_exe, r.ask_name)) {
            if (it) render_ask_say(r.ask_key, true, "Couldn't make the edited copy, so nothing was sent");
            r.ask_key = 0;
        }
        return;
    }
    u64 now_ms = GetTickCount64();
    if (now_ms < r.next_ms || g_app.library.scanning) return;
    r.next_ms = now_ms + RENDER_EVERY_MS;
    if (!g_app.config.save_dir.s[0]) return;
    if (!r.cleaned) {
        render_clean_leftovers();
        r.cleaned = true;
    }
    bool on = g_app.config.render_edits, busy = game || g_app.saving.jobs_running > 0;
    i64 now = now_us();
    const Library &lib = g_app.library;
    bool started = false;
    r.waiting_count = 0;
    for (i32 i = 0; i < lib.item_count; i++) { // newest first, the first that may be rendered is
        const LibItem &it = lib.items[i];
        if (it.kind != LIB_CLIPS || it.dir || _stricmp(it.ext, ".mp4") || !it.probed) continue; // not another app's
        i64 mtime = 0;
        bool has_edit = render_edit_stamp(it, &mtime, 0);
        RenderNote note = {};
        bool noted = it.rendered && render_note_read(it, &note);
        if (has_edit && on && !(noted && note.edit_mtime_us == mtime)) {
            // The edit's time moved: it may be the same edit written again, or reset to nothing
            u64 hash = 0;
            EditRecord rec;
            edit_read(lib_edit_path(it), &rec);
            has_edit = !edit_is_default(rec) && render_edit_stamp(it, &mtime, &hash);
            if (has_edit && noted && note.edit_hash == hash) {
                note.edit_mtime_us = mtime;
                render_note_write(it, note);
            }
            RenderNeed need = render_need(on, it.rendered, has_edit, noted && note.edit_hash == hash);
            if (need == RENDER_RESTORE && render_restore(it)) library_rescan();
            if (need != RENDER_MAKE || render_failed_before(it.key, hash)) continue;
            if (!started && !busy && render_settled(render_editor_on(it), it.rendered, mtime, now) &&
                render_start(it, rec, mtime, hash)) {
                started = true;
                continue;
            }
            r.waiting[r.waiting_count++] = it.key;
            continue;
        }
        if (render_need(on, it.rendered, has_edit, true) == RENDER_RESTORE && render_restore(it)) library_rescan();
    }
}
