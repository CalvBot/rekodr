// Editor: trim, zoom keyframes and track volumes for one saved clip or session, then Keep or Share.
//
// An edit never touches the original. It's a small text file, <year>\Data\<name>.edit.txt, written whenever a
// change settles (a slider let go, a key pressed), so it's there next time the file is opened. Keep and Share go
// through the save queue (save.cpp, edit_run); a finished Share is put on the clipboard as a file.
//
// The preview plays the video muted and each edited audio track on its own audio-only engine (player.cpp), so
// mutes and volumes are heard as they're set (up to 100%: the engine can't go louder).

// The tracks that can be edited: in clipback's files the first track is a Mix of the others, so it's left out
// (Keep and Share rebuild it).
static i32 ed_tracks(const MediaInfo &m, i32 *out, i32 cap)
{
    i32 n = 0;
    for (i32 t = edit_has_mix(m) ? 1 : 0; t < m.track_count && t < MEDIA_MAX_TRACKS && n < cap; t++) out[n++] = t;
    return n;
}

static bool edit_is_default(const EditRecord &e)
{
    if (e.in_s > 0 || e.out_s > 0 || e.key_count) return false;
    for (i32 t = 0; t <= EDIT_MASTER; t++)
        if (e.muted[t] || e.boost[t] != 0 || e.vol_count[t]) return false;
    return true;
}

static void edit_read(const Path &file, EditRecord *e)
{
    *e = {};
    FILE *f = _wfopen(file.s, L"rb");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = trim(line), *v = trim(eq + 1);
        if (!strcmp(k, "in")) e->in_s = atof(v);
        else if (!strcmp(k, "out")) e->out_s = atof(v);
        else if (!strcmp(k, "key") && e->key_count < EDIT_MAX_KEYS) {
            ZoomKey z = {};
            i32 ease = EASE_SMOOTH;
            if (sscanf_s(v, "%lf %lf %lf %lf %d", &z.t, &z.z, &z.x, &z.y, &ease) == 5) {
                z.ease = (u8)(ease >= 0 && ease < EASE_COUNT ? ease : EASE_SMOOTH);
                e->keys[e->key_count++] = z;
            }
        } else if (!strcmp(k, "volume") || !strcmp(k, "vkey")) {
            // The track's number, or "m" for the whole mix.
            i32 t = *v == 'm' ? EDIT_MASTER : atoi(v);
            while (*v && *v != ' ') v++;
            if (t < 0 || t > EDIT_MASTER) continue;
            if (!strcmp(k, "volume")) {
                i32 muted = 0;
                f32 vol = 1;
                if (sscanf_s(v, "%f %d", &vol, &muted) >= 1) {
                    e->boost[t] = vol - 1;
                    e->muted[t] = muted != 0;
                }
            } else if (e->vol_count[t] < EDIT_MAX_VOL_KEYS) {
                VolKey vk = {};
                i32 ease = EASE_LINEAR;
                if (sscanf_s(v, "%lf %f %d", &vk.t, &vk.v, &ease) >= 2) {
                    vk.v = vk.v < 0 ? 0 : vk.v > 2 ? 2 : vk.v;
                    vk.ease = (u8)(ease >= 0 && ease < EASE_COUNT ? ease : EASE_LINEAR);
                    i32 n = e->vol_count[t], at = n;
                    while (at > 0 && e->vol[t][at - 1].t > vk.t) at--; // kept sorted whatever the file's order
                    memmove(e->vol[t] + at + 1, e->vol[t] + at, (n - at) * sizeof vk);
                    e->vol[t][at] = vk;
                    e->vol_count[t] = n + 1;
                }
            }
        }
    }
    fclose(f);
}

static void edit_write(const Path &file, const EditRecord &e)
{
    if (edit_is_default(e)) { // nothing to remember
        DeleteFileW(file.s);
        return;
    }
    Path dir = file;
    wchar_t *slash = wcsrchr(dir.s, L'\\');
    if (slash) *slash = 0;
    SHCreateDirectoryExW(0, dir.s, 0);
    Path tmp = {};
    swprintf(tmp.s, MAX_PATH, L"%s.tmp", file.s);
    FILE *f = _wfopen(tmp.s, L"wb");
    if (!f) {
        log_line("editor: couldn't write the edit");
        return;
    }
    fprintf(f, "# clipback edit: trim in seconds, zoom keys (time zoom x y ease), volumes (track volume muted),\n"
               "# volume keys (track time volume ease); track m is the whole mix; eases 0 linear 1 smooth 2 in 3 out 4 hold\n");
    fprintf(f, "in = %.3f\nout = %.3f\n", e.in_s, e.out_s);
    for (i32 k = 0; k < e.key_count; k++)
        fprintf(f, "key = %.3f %.3f %.4f %.4f %d\n", e.keys[k].t, e.keys[k].z, e.keys[k].x, e.keys[k].y, e.keys[k].ease);
    for (i32 t = 0; t <= EDIT_MASTER; t++) {
        char id[8];
        if (t == EDIT_MASTER) str_set(id, "m");
        else snprintf(id, sizeof id, "%d", t);
        if (e.muted[t] || e.boost[t] != 0) fprintf(f, "volume = %s %.3f %d\n", id, 1 + e.boost[t], e.muted[t] ? 1 : 0);
        for (i32 k = 0; k < e.vol_count[t]; k++)
            fprintf(f, "vkey = %s %.3f %.3f %d\n", id, e.vol[t][k].t, e.vol[t][k].v, e.vol[t][k].ease);
    }
    fclose(f);
    MoveFileExW(tmp.s, file.s, MOVEFILE_REPLACE_EXISTING);
}

// The zoom at `t`: eased between keys with smoothstep, the same curve the render uses.
static void zoom_at(const EditRecord &e, f64 t, f64 *z, f64 *x, f64 *y)
{
    *z = 1;
    *x = *y = 0.5;
    i32 n = e.key_count;
    if (!n) return;
    const ZoomKey *a = &e.keys[0], *b = a;
    if (t <= e.keys[0].t) b = a;
    else if (t >= e.keys[n - 1].t) a = b = &e.keys[n - 1];
    else
        for (i32 k = 0; k + 1 < n; k++)
            if (t < e.keys[k + 1].t) { a = &e.keys[k]; b = &e.keys[k + 1]; break; }
    f64 u = b->t > a->t ? ease_curve(a->ease, b->ease, (t - a->t) / (b->t - a->t)) : 0;
    *z = a->z + (b->z - a->z) * u;
    if (*z < 1) *z = 1;
    *x = a->x + (b->x - a->x) * u;
    *y = a->y + (b->y - a->y) * u;
}

static void zoom_uv(const EditRecord &e, f64 t, ImVec2 *uv0, ImVec2 *uv1)
{
    f64 z, cx, cy;
    zoom_at(e, t, &z, &cx, &cy);
    f64 w = 1 / z;
    f64 ox = cx - w / 2, oy = cy - w / 2;
    ox = ox < 0 ? 0 : ox > 1 - w ? 1 - w : ox;
    oy = oy < 0 ? 0 : oy > 1 - w ? 1 - w : oy;
    *uv0 = ImVec2((f32)ox, (f32)oy);
    *uv1 = ImVec2((f32)(ox + w), (f32)(oy + w));
}

// Lanes: the zoom keys (lane 0) and each volume slot's keys (lane 1 + slot) read and written alike, so the
// timeline's selecting, dragging, eases, copy and paste work the same on all of them. Values are a zoom (1..4) or a
// volume (0..2, 1 = as recorded).
struct LaneKey {
    f64 t, v, x, y; // x, y: a zoom key's aim
    u8  ease;
};

static i32 ln_count(const EditRecord &e, i32 l) { return l ? e.vol_count[l - 1] : e.key_count; }
static i32 ln_cap(i32 l) { return l ? EDIT_MAX_VOL_KEYS : EDIT_MAX_KEYS; }
static f64 ln_lo(i32 l) { return l ? 0 : 1; }
static f64 ln_hi(i32 l) { return l ? 2 : 4; }

static LaneKey ln_get(const EditRecord &e, i32 l, i32 k)
{
    LaneKey q = {};
    if (l) {
        const VolKey &v = e.vol[l - 1][k];
        q.t = v.t;
        q.v = v.v;
        q.x = q.y = 0.5;
        q.ease = v.ease;
    } else {
        const ZoomKey &z = e.keys[k];
        q.t = z.t;
        q.v = z.z;
        q.x = z.x;
        q.y = z.y;
        q.ease = z.ease;
    }
    return q;
}

static void ln_put(EditRecord &e, i32 l, i32 k, const LaneKey &q)
{
    if (l) {
        VolKey &v = e.vol[l - 1][k];
        v.t = q.t;
        v.v = (f32)q.v;
        v.ease = q.ease;
    } else {
        ZoomKey &z = e.keys[k];
        z.t = q.t;
        z.z = q.v;
        z.x = q.x;
        z.y = q.y;
        z.ease = q.ease;
    }
}

static f64 ln_value_at(const EditRecord &e, i32 l, f64 t)
{
    if (l) return vol_at(e, l - 1, t);
    f64 z, x, y;
    zoom_at(e, t, &z, &x, &y);
    return z;
}

// A selection is a bit per key; these keep it on the same keys as one is put in or taken out at i.
static u64 bits_insert(u64 m, i32 i)
{
    u64 low = m & ((1ull << i) - 1);
    return low | ((m & ~low) << 1);
}

static u64 bits_erase(u64 m, i32 i)
{
    u64 low = m & ((1ull << i) - 1);
    return low | (i < 63 ? (m >> (i + 1)) << i : 0);
}

static u64 bits_all(i32 n) { return n >= 64 ? ~0ull : (1ull << n) - 1; }

// Puts a key in its place by time (after any at the same time). Returns where, or -1 when the lane is full.
static i32 ln_insert(EditRecord &e, i32 l, const LaneKey &q, u64 *sel)
{
    i32 n = ln_count(e, l);
    if (n >= ln_cap(l)) return -1;
    i32 at = n;
    while (at > 0 && ln_get(e, l, at - 1).t > q.t) at--;
    if (l) {
        memmove(e.vol[l - 1] + at + 1, e.vol[l - 1] + at, (n - at) * sizeof(VolKey));
        e.vol_count[l - 1]++;
    } else {
        memmove(e.keys + at + 1, e.keys + at, (n - at) * sizeof(ZoomKey));
        e.key_count++;
    }
    ln_put(e, l, at, q);
    if (sel) *sel = bits_insert(*sel, at);
    return at;
}

static void ln_erase(EditRecord &e, i32 l, i32 k, u64 *sel)
{
    i32 n = ln_count(e, l);
    if (k < 0 || k >= n) return;
    if (l) {
        memmove(e.vol[l - 1] + k, e.vol[l - 1] + k + 1, (n - k - 1) * sizeof(VolKey));
        e.vol_count[l - 1]--;
    } else {
        memmove(e.keys + k, e.keys + k + 1, (n - k - 1) * sizeof(ZoomKey));
        e.key_count--;
    }
    if (sel) *sel = bits_erase(*sel, k);
}

// Back in time order after keys were moved, the selection going with them.
static void ln_sort(EditRecord &e, i32 l, u64 *sel)
{
    i32 n = ln_count(e, l);
    for (i32 i = 1; i < n; i++) {
        LaneKey q = ln_get(e, l, i);
        u64 bit = (*sel >> i) & 1;
        i32 j = i;
        for (; j > 0; j--) {
            LaneKey b = ln_get(e, l, j - 1);
            if (b.t <= q.t) break;
            ln_put(e, l, j, b);
            *sel = (*sel & ~(1ull << j)) | (((*sel >> (j - 1)) & 1) << j);
        }
        ln_put(e, l, j, q);
        *sel = (*sel & ~(1ull << j)) | (bit << j);
    }
}

static f64 ed_duration()
{
    Editor &ed = g_app.editor;
    return ed.player.duration > 0 ? ed.player.duration : ed.item.info.duration;
}

static f64 ed_out()
{
    Editor &ed = g_app.editor;
    return ed.edit.out_s > 0 ? ed.edit.out_s : ed_duration();
}

static f64 ed_frame()
{
    f64 fps = g_app.editor.item.info.fps;
    return 1 / (fps > 1 ? fps : 60);
}

// The key on lane l at t (within half a frame), or -1.
static i32 ln_key_at(const EditRecord &e, i32 l, f64 t)
{
    f64 tol = ed_frame() / 2 + 0.01;
    for (i32 k = 0; k < ln_count(e, l); k++) {
        f64 kt = ln_get(e, l, k).t;
        if (kt > t - tol && kt < t + tol) return k;
    }
    return -1;
}

static i32 ed_key_at(f64 t) { return ln_key_at(g_app.editor.edit, 0, t); }

// A key on lane l at t with value v (a zoom key aims where the zoom aims there now). Returns its index, or -1 if
// the lane is full.
static i32 ed_put_key(i32 l, f64 t, f64 v)
{
    Editor &ed = g_app.editor;
    LaneKey q = {};
    q.t = t;
    q.v = v;
    q.x = q.y = 0.5;
    q.ease = l ? EASE_LINEAR : EASE_SMOOTH;
    if (!l) {
        f64 z;
        zoom_at(ed.edit, t, &z, &q.x, &q.y);
    }
    i32 k = ln_insert(ed.edit, l, q, &ed.sel[l]);
    if (k >= 0) ed.dirty = true;
    return k;
}

// A zoom key at `t` holding the zoom as it is there now. Returns its index, or -1 if there's no room.
static i32 ed_add_key(f64 t)
{
    Editor &ed = g_app.editor;
    i32 k = ed_key_at(t);
    if (k >= 0) return k;
    return ed_put_key(0, t, ln_value_at(ed.edit, 0, t));
}

static void ed_delete_key(i32 k)
{
    Editor &ed = g_app.editor;
    if (k < 0 || k >= ed.edit.key_count) return;
    ln_erase(ed.edit, 0, k, &ed.sel[0]);
    ed.dirty = true;
}

static void ed_apply_volumes()
{
    Editor &ed = g_app.editor;
    Player &p = ed.player;
    if (!p.engine) return;
    i32 tracks[MEDIA_MAX_TRACKS];
    i32 n = ed_tracks(ed.item.info, tracks, MEDIA_MAX_TRACKS);
    // At the playhead: each track's volume times the whole mix's (the engines can't go past 100%). Only what's
    // changed is handed over, since this runs every frame while playing.
    f64 master = vol_heard(ed.edit, EDIT_MASTER, p.pos);
    for (i32 i = 0; i < n; i++) {
        i32 t = tracks[i];
        f32 v = (f32)(vol_heard(ed.edit, t, p.pos) * master);
        if (ed.solo >= 0 && t != ed.solo) v = 0;
        if (p.track_engine_count) {
            if (fabsf(v - ed.heard[t]) < 0.002f && ed.heard_set) continue;
            player_track_volume(&p, t, v, v <= 0);
        } else if (n == 1) {
            if (fabsf(v - ed.heard[t]) < 0.002f && ed.heard_set) continue;
            p.engine->SetVolume(v < 1 ? v : 1);
            p.engine->SetMuted(v <= 0);
        }
        ed.heard[t] = v;
    }
    ed.heard_set = true;
}

static void ed_save_record()
{
    Editor &ed = g_app.editor;
    if (!ed.dirty || !ed.item.name[0]) return;
    edit_write(lib_edit_path(ed.item), ed.edit);
    ed.dirty = false;
}

// Recent shares: the share folder's files, newest first. Copies older than share_keep_days are cleared.
static void editor_refresh_shares()
{
    Editor &ed = g_app.editor;
    ed.share_count = 0;
    const Path &dir = g_app.paths.share_dir;
    i64 keep_us = (i64)(g_app.config.share_keep_days * 86400.0 * 1e6), now = now_us();
    WIN32_FIND_DATAW f;
    HANDLE h = FindFirstFileW(path_join(dir, L"*.mp4").s, &f);
    if (h == INVALID_HANDLE_VALUE) return;
    i32 cleared = 0;
    do {
        if (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        i64 mt = filetime_us(f.ftLastWriteTime);
        if (keep_us > 0 && now - mt > keep_us) {
            if (DeleteFileW(path_join(dir, f.cFileName).s)) cleared++;
            continue;
        }
        ShareFile s = {};
        wide_to_utf8(f.cFileName, s.name, sizeof s.name);
        s.mtime_us = mt;
        s.size = ((u64)f.nFileSizeHigh << 32) | f.nFileSizeLow;
        // Newest first, keeping the newest 64.
        i32 at = 0;
        while (at < ed.share_count && ed.shares[at].mtime_us >= mt) at++;
        if (at >= COUNT(ed.shares)) continue;
        i32 n = ed.share_count < COUNT(ed.shares) ? ed.share_count : COUNT(ed.shares) - 1;
        memmove(ed.shares + at + 1, ed.shares + at, (n - at) * sizeof s);
        ed.shares[at] = s;
        ed.share_count = n + 1;
    } while (FindNextFileW(h, &f));
    FindClose(h);
    if (cleared) log_line("editor: cleared %d share copies older than %g days", cleared, g_app.config.share_keep_days);
}

static void editor_open(const LibItem &item)
{
    Editor &ed = g_app.editor;
    ed_save_record();
    player_close(&ed.player);
    ed.open = true;
    ed.pending[0] = 0;
    ed.item = item;
    ed.path = lib_item_path(item);
    edit_read(lib_edit_path(item), &ed.edit);
    ed.dirty = false;
    ed.tracks_ready = false;
    ed.lane_drag = 0;
    ed.lane_scroll = 0;
    ed.solo = -1;
    ed.view_from = ed.view_span = 0;
    memset(ed.sel, 0, sizeof ed.sel);
    ed.settled = ed.edit;
    ed.undo_count = ed.redo_count = 0;
    ed.aiming = false;
    ed.result[0] = 0;
    ed.result_path[0] = 0;
    ed.result_error = false;
    str_set(ed.out_name, ed.item.name);
    ed.player.id = 1;
    player_open(&ed.player, ed.path, ed.edit.in_s, false);
    editor_refresh_shares();
    // The waveforms of the tracks the timeline shows, measured (or read from the cache) in the background.
    wave_close(&ed.wave);
    if (g_app.config.wave_keep_days > 0) {
        i32 gone = wave_cache_clear(g_app.config.wave_keep_days);
        if (gone) log_line("editor: deleted %d audio previews not opened in %g days", gone, g_app.config.wave_keep_days);
    }
    i32 tracks[MEDIA_MAX_TRACKS];
    i32 n = ed_tracks(item.info, tracks, MEDIA_MAX_TRACKS);
    ed.wave = wave_open(ed.path, item.key, item.info.duration, tracks, n);
    g_app.ui.screen = SCREEN_EDITOR;
    log_line("editor: %s", item.name);
    ui_wake();
}

// Opens a file by its path (UTF-8), for screens that just saved one: it opens once the library's scan has found
// and probed it, since the editor needs its tracks and length.
static void editor_open_file(const char *path)
{
    Editor &ed = g_app.editor;
    str_set(ed.pending, path);
    ed.pending_since = GetTickCount64();
    library_rescan();
    g_app.ui.screen = SCREEN_EDITOR;
    log_line("editor: waiting for the library to have %s", path);
    ui_wake();
}

// The pending file, if the library has it now.
static void ed_check_pending()
{
    Editor &ed = g_app.editor;
    if (!ed.pending[0]) return;
    Library &lib = g_app.library;
    wchar_t want[MAX_PATH];
    utf8_to_wide(ed.pending, want, MAX_PATH);
    for (i32 i = 0; i < lib.item_count; i++) {
        if (_wcsicmp(lib_item_path(lib.items[i]).s, want)) continue;
        if (!lib.items[i].probed) return; // found, not probed yet
        ed.pending[0] = 0;
        editor_open(lib.items[i]);
        return;
    }
    if (GetTickCount64() - ed.pending_since > 15000 && !lib.scanning) {
        log_line("editor: %s never turned up in the library", ed.pending);
        ed.pending[0] = 0;
    }
}

static void editor_close()
{
    Editor &ed = g_app.editor;
    ed_save_record();
    player_close(&ed.player);
    wave_close(&ed.wave);
    ed.open = false;
}

// From ui_frame: a player whose screen isn't shown is closed (the library's stops when you leave the Library).
static void screens_left_behind()
{
    Screen s = g_app.ui.screen;
    if (s != SCREEN_EDITOR && g_app.editor.open) editor_close();
    if (s != SCREEN_LIBRARY && g_app.library.player.open) library_close_player();
}

static void editor_on_save_event(const Event &e)
{
    Editor &ed = g_app.editor;
    u32 id = (u32)(e.b & 0xffffffff);
    const SaveJobView *job = save_view(id);
    if (e.a == SAVE_EV_DONE && !(job && job->kind == SAVE_PREVIEW)) library_rescan(); // a new clip, session or Keep
    if (!id || (id != ed.keep_job && id != ed.share_job)) return;
    bool share = id == ed.share_job;
    const char *name = strrchr(e.text, '\\');
    name = name ? name + 1 : e.text;
    ed.result_path[0] = 0;
    if (e.a == SAVE_EV_DONE) {
        str_set(ed.result_path, e.text);
        if (share) {
            wchar_t w[MAX_PATH];
            utf8_to_wide(e.text, w, MAX_PATH);
            WIN32_FILE_ATTRIBUTE_DATA fa = {};
            GetFileAttributesExW(w, GetFileExInfoStandard, &fa);
            char size[24];
            fmt_size(((u64)fa.nFileSizeHigh << 32) | fa.nFileSizeLow, size, sizeof size);
            bool copied = clipboard_copy_file(w);
            snprintf(ed.result, sizeof ed.result, copied ? "Shared (%s): on the clipboard, ready to paste." :
                                                           "Shared (%s), but the clipboard was busy: use Copy.", size);
            ed.result_error = !copied;
            editor_refresh_shares();
            ed.share_page = 0;
        } else {
            snprintf(ed.result, sizeof ed.result, "Kept as %s", name);
            ed.result_error = false;
        }
    } else if (e.a == SAVE_EV_FAILED) {
        snprintf(ed.result, sizeof ed.result, "%s failed: %s", share ? "Share" : "Keep", e.text);
        ed.result_error = true;
    } else if (e.a == SAVE_EV_CANCELLED) {
        snprintf(ed.result, sizeof ed.result, "%s cancelled", share ? "Share" : "Keep");
        ed.result_error = false;
    }
}

static bool ed_job_busy(u32 id)
{
    SaveJobView *v = save_view(id);
    return v && v->state < SAVE_DONE;
}

// The size Share aims for, in MiB (what Discord means by MB).
static f32 ed_share_mb()
{
    Editor &ed = g_app.editor;
    f32 mb = ed.share_size == 0 ? 20.f : ed.share_size == 1 ? 50.f : ed.custom_mb;
    return mb < 1 ? 1 : mb;
}

static void ed_start(SaveKind kind)
{
    Editor &ed = g_app.editor;
    ed_save_record();
    static EditJob j;
    j = {};
    j.src = ed.path;
    j.info = ed.item.info;
    if (ed.player.duration > 0) j.info.duration = ed.player.duration;
    j.edit = ed.edit;
    if (kind == SAVE_KEEP) {
        swprintf(j.out_dir.s, MAX_PATH, L"%s\\%d\\Clips", g_app.config.save_dir.s, ed.item.year);
        // Kept beside clips that may hold the original: its own name, or the original's with " edit"
        if (ed.out_name[0] && strcmp(ed.out_name, ed.item.name)) snprintf(j.stem, sizeof j.stem, "%s", ed.out_name);
        else snprintf(j.stem, sizeof j.stem, "%s edit", ed.item.name);
    } else {
        j.out_dir = g_app.paths.share_dir;
        snprintf(j.stem, sizeof j.stem, "%s", ed.out_name[0] ? ed.out_name : ed.item.name);
        j.share_mb = ed_share_mb();
    }
    clean_name(j.stem); // the name is typed: drop what a file name can't hold
    if (!j.stem[0]) snprintf(j.stem, sizeof j.stem, "%s", ed.item.name);
    u32 id = save_edit(kind, j);
    if (kind == SAVE_KEEP) ed.keep_job = id;
    else ed.share_job = id;
    if (!id) {
        str_set(ed.result, "Too many saves are waiting; try again in a moment.");
        ed.result_error = true;
    } else {
        ed.result[0] = 0;
    }
}

// ---- The selection, copy and paste, undo ----

enum { ED_LANES = EDIT_MASTER + 2 };

// A lane the timeline shows for this file (copied keys aren't pasted onto a track it doesn't have).
static bool ed_lane_shown(i32 l)
{
    if (l == 0 || l == 1 + EDIT_MASTER) return true;
    i32 tracks[MEDIA_MAX_TRACKS];
    i32 n = ed_tracks(g_app.editor.item.info, tracks, MEDIA_MAX_TRACKS);
    for (i32 i = 0; i < n; i++)
        if (tracks[i] == l - 1) return true;
    return false;
}

static void ed_sel_clear() { memset(g_app.editor.sel, 0, sizeof g_app.editor.sel); }

static i32 ed_sel_count()
{
    i32 n = 0;
    for (i32 l = 0; l < ED_LANES; l++)
        for (u64 m = g_app.editor.sel[l]; m; m &= m - 1) n++;
    return n;
}

static void ed_select_all()
{
    Editor &ed = g_app.editor;
    for (i32 l = 0; l < ED_LANES; l++) ed.sel[l] = ed_lane_shown(l) ? bits_all(ln_count(ed.edit, l)) : 0;
}

// The selected keys' ease if they share one, else -1.
static i32 ed_sel_ease()
{
    Editor &ed = g_app.editor;
    i32 ease = -1;
    for (i32 l = 0; l < ED_LANES; l++)
        for (i32 k = 0; k < ln_count(ed.edit, l); k++) {
            if (!((ed.sel[l] >> k) & 1)) continue;
            i32 e = ln_get(ed.edit, l, k).ease;
            if (ease == -1) ease = e;
            else if (ease != e) return -1;
        }
    return ease;
}

static void ed_set_ease(i32 ease)
{
    Editor &ed = g_app.editor;
    for (i32 l = 0; l < ED_LANES; l++)
        for (i32 k = 0; k < ln_count(ed.edit, l); k++) {
            if (!((ed.sel[l] >> k) & 1)) continue;
            LaneKey q = ln_get(ed.edit, l, k);
            q.ease = (u8)ease;
            ln_put(ed.edit, l, k, q);
            ed.dirty = true;
        }
}

static void ed_delete_selected()
{
    Editor &ed = g_app.editor;
    for (i32 l = 0; l < ED_LANES; l++)
        for (i32 k = ln_count(ed.edit, l) - 1; k >= 0; k--)
            if ((ed.sel[l] >> k) & 1) {
                ln_erase(ed.edit, l, k, &ed.sel[l]);
                ed.dirty = true;
            }
}

static void ed_copy()
{
    Editor &ed = g_app.editor;
    f64 first = 1e300;
    for (i32 l = 0; l < ED_LANES; l++)
        for (i32 k = 0; k < ln_count(ed.edit, l); k++)
            if ((ed.sel[l] >> k) & 1 && ln_get(ed.edit, l, k).t < first) first = ln_get(ed.edit, l, k).t;
    if (first > 1e299) return;
    ed.clip_count = 0;
    for (i32 l = 0; l < ED_LANES; l++)
        for (i32 k = 0; k < ln_count(ed.edit, l) && ed.clip_count < COUNT(ed.clip); k++) {
            if (!((ed.sel[l] >> k) & 1)) continue;
            LaneKey q = ln_get(ed.edit, l, k);
            auto &c = ed.clip[ed.clip_count++];
            c.lane = l;
            c.t = q.t - first;
            c.v = q.v;
            c.x = q.x;
            c.y = q.y;
            c.ease = q.ease;
        }
}

// The copied keys, the first at `at`; they become the selection.
static void ed_paste(f64 at)
{
    Editor &ed = g_app.editor;
    if (!ed.clip_count) return;
    ed_sel_clear();
    f64 dur = ed_duration();
    for (i32 i = 0; i < ed.clip_count; i++) {
        const auto &c = ed.clip[i];
        if (!ed_lane_shown(c.lane) || at + c.t > dur + 1e-6) continue;
        LaneKey q = {};
        q.t = at + c.t;
        q.v = c.v;
        q.x = c.x;
        q.y = c.y;
        q.ease = c.ease;
        i32 k = ln_insert(ed.edit, c.lane, q, &ed.sel[c.lane]);
        if (k >= 0) {
            ed.sel[c.lane] |= 1ull << k;
            ed.dirty = true;
        }
    }
}

static void ed_push(EditRecord *stack, i32 *count, i32 cap, const EditRecord &r)
{
    if (*count == cap) {
        memmove(stack, stack + 1, (cap - 1) * sizeof r);
        (*count)--;
    }
    stack[(*count)++] = r;
}

// A change has settled (nothing held): what it was before goes on the undo list.
static void ed_settle()
{
    Editor &ed = g_app.editor;
    if (!memcmp(&ed.edit, &ed.settled, sizeof ed.edit)) return;
    ed_push(ed.undo, &ed.undo_count, COUNT(ed.undo), ed.settled);
    ed.redo_count = 0;
    ed.settled = ed.edit;
}

static void ed_undo(bool redo)
{
    Editor &ed = g_app.editor;
    ed_settle();
    EditRecord *from = redo ? ed.redo : ed.undo, *to = redo ? ed.undo : ed.redo;
    i32 *from_n = redo ? &ed.redo_count : &ed.undo_count, *to_n = redo ? &ed.undo_count : &ed.redo_count;
    if (!*from_n) return;
    ed_push(to, to_n, COUNT(ed.undo), ed.edit);
    ed.edit = from[--*from_n];
    ed.settled = ed.edit;
    ed.dirty = true;
    ed.heard_set = false;
    ed_sel_clear();
}

// ---- The view: which seconds the lanes show ----

static void ed_view_clamp()
{
    Editor &ed = g_app.editor;
    f64 dur = ed_duration();
    if (dur <= 0) {
        ed.view_from = 0;
        ed.view_span = 1;
        return;
    }
    f64 least = dur < 0.5 ? dur : 0.5;
    if (ed.view_span <= 0 || ed.view_span > dur) ed.view_span = dur;
    if (ed.view_span < least) ed.view_span = least;
    if (ed.view_from > dur - ed.view_span) ed.view_from = dur - ed.view_span;
    if (ed.view_from < 0) ed.view_from = 0;
}

// Zooms the view by `factor` around time `at`.
static void ed_view_zoom(f64 factor, f64 at)
{
    Editor &ed = g_app.editor;
    // `at` keeps its place on screen, or comes to the middle if it was off it
    f64 frac = ed.view_span > 0 && at >= ed.view_from && at <= ed.view_from + ed.view_span ? (at - ed.view_from) / ed.view_span : 0.5;
    ed.view_span *= factor;
    ed_view_clamp();
    ed.view_from = at - frac * ed.view_span;
    ed_view_clamp();
}

static f32 ed_lane_h()
{
    i32 v = g_app.config.editor_lane;
    if (v <= 0) v = 56;
    v = v < 36 ? 36 : v > 160 ? 160 : v;
    return kit_px((f32)v);
}

// ---- Drawing and reading a lane ----

static f32 ln_pad() { return kit_px(8); }

static f32 ln_y(i32 l, f64 v, f32 y0, f32 y1)
{
    f64 f = (ln_hi(l) - v) / (ln_hi(l) - ln_lo(l));
    return y0 + ln_pad() + (f32)f * (y1 - y0 - 2 * ln_pad());
}

static f64 ln_v_of(i32 l, f32 y, f32 y0, f32 y1)
{
    f64 f = (y - y0 - ln_pad()) / (f64)(y1 - y0 - 2 * ln_pad());
    return ln_hi(l) - f * (ln_hi(l) - ln_lo(l));
}

// Kept in range and caught by 100% volume (or a whole zoom) within a few pixels; vh is the lane's value height.
static f64 ln_snap(i32 l, f64 v, f32 vh)
{
    f64 lo = ln_lo(l), hi = ln_hi(l);
    v = v < lo ? lo : v > hi ? hi : v;
    f64 tol = vh > 0 ? kit_px(5) / vh * (hi - lo) : 0;
    f64 r = l ? 1 : floor(v + 0.5);
    if (fabs(v - r) < tol) v = r;
    return v;
}

static void diamond(ImDrawList *dl, ImVec2 c, f32 r, u32 col)
{
    dl->AddQuadFilled(ImVec2(c.x, c.y - r), ImVec2(c.x + r, c.y), ImVec2(c.x, c.y + r), ImVec2(c.x - r, c.y), col);
}

// A key drawn by its ease: a point where the value moves at full speed, round where it moves slowly (an ease in is
// round on the side it arrives from, an ease out on the side it leaves by), a square for a hold.
static void key_shape(ImDrawList *dl, ImVec2 c, f32 r, u8 ease, u32 col)
{
    f32 tip = r * 1.25f;
    switch (ease) {
    case EASE_LINEAR: diamond(dl, c, tip, col); break;
    case EASE_SMOOTH: dl->AddCircleFilled(c, r, col, 16); break;
    case EASE_HOLD: dl->AddRectFilled(ImVec2(c.x - r * 0.9f, c.y - r * 0.9f), ImVec2(c.x + r * 0.9f, c.y + r * 0.9f), col); break;
    default:
        dl->PathClear();
        if (ease == EASE_IN) {
            dl->PathArcTo(c, r, IM_PI * 0.5f, IM_PI * 1.5f, 10);
            dl->PathLineTo(ImVec2(c.x + tip, c.y));
        } else {
            dl->PathArcTo(c, r, -IM_PI * 0.5f, IM_PI * 0.5f, 10);
            dl->PathLineTo(ImVec2(c.x - tip, c.y));
        }
        dl->PathFillConvex(col);
        break;
    }
}

enum {
    ED_DRAG_NONE, ED_DRAG_IN, ED_DRAG_OUT, ED_DRAG_SEEK, ED_DRAG_KEYS, ED_DRAG_LEVEL, ED_DRAG_STRETCH, ED_DRAG_PAN,
    ED_DRAG_BAR, ED_DRAG_BAR_IN, ED_DRAG_BAR_OUT
};

// The selected keys moved by the drag so far, from where they were when it began. The grabbed key lands on a frame
// (or the playhead, with Shift) and its value snaps; the rest keep their places relative to it.
static void ed_drag_keys(f64 dt, f64 dfrac, f32 px_per_s, f32 vh, bool time_only, bool value_only)
{
    Editor &ed = g_app.editor;
    f64 dur = ed_duration();
    ed.edit = ed.drag_from;
    memcpy(ed.sel, ed.drag_sel, sizeof ed.sel);
    LaneKey g = ln_get(ed.drag_from, ed.drag_lane, ed.drag_key);
    if (value_only) dt = 0;
    if (time_only) dfrac = 0;
    f64 nt = g.t + dt, fr = ed_frame();
    nt = floor(nt / fr + 0.5) * fr;
    if (time_only && fabs(nt - ed.player.pos) * px_per_s < kit_px(8)) nt = ed.player.pos;
    if (value_only) nt = g.t;
    dt = nt - g.t;
    f64 tmin = 1e300, tmax = -1e300;
    for (i32 l = 0; l < ED_LANES; l++)
        for (i32 k = 0; k < ln_count(ed.edit, l); k++) {
            if (!((ed.sel[l] >> k) & 1)) continue;
            f64 t = ln_get(ed.edit, l, k).t;
            tmin = t < tmin ? t : tmin;
            tmax = t > tmax ? t : tmax;
        }
    if (tmin > tmax) return;
    if (tmin + dt < 0) dt = -tmin;
    if (tmax + dt > dur) dt = dur - tmax;
    f64 range = ln_hi(ed.drag_lane) - ln_lo(ed.drag_lane);
    f64 df = (ln_snap(ed.drag_lane, g.v + dfrac * range, vh) - g.v) / range;
    for (i32 l = 0; l < ED_LANES; l++) {
        if (!ed.sel[l]) continue;
        for (i32 k = 0; k < ln_count(ed.edit, l); k++) {
            if (!((ed.sel[l] >> k) & 1)) continue;
            LaneKey q = ln_get(ed.edit, l, k);
            q.t += dt;
            q.v += df * (ln_hi(l) - ln_lo(l));
            q.v = q.v < ln_lo(l) ? ln_lo(l) : q.v > ln_hi(l) ? ln_hi(l) : q.v;
            ln_put(ed.edit, l, k, q);
        }
        ln_sort(ed.edit, l, &ed.sel[l]);
    }
    ed.dirty = true;
}

// A sound lane's colour, by the kind of track its title says it is: chat, game, an app, or the mic.
enum EdKind { ED_KIND_MIX, ED_KIND_MIC, ED_KIND_CHAT, ED_KIND_GAME, ED_KIND_APP };
static EdKind ed_track_kind(const char *title, i32 slot)
{
    if (slot == EDIT_MASTER) return ED_KIND_MIX;
    size_t n = strlen(title);
    if (n >= 6 && !strcmp(title + n - 6, "(chat)")) return ED_KIND_CHAT;
    if (n >= 6 && !strcmp(title + n - 6, "(game)")) return ED_KIND_GAME;
    if (!strcmp(title, "Mic")) return ED_KIND_MIC;
    return ED_KIND_APP;
}

static u32 ed_kind_rgb(EdKind k)
{
    switch (k) {
    case ED_KIND_MIX: return 0xeceef1;
    case ED_KIND_MIC: return COL_TRACK_MIC;
    case ED_KIND_CHAT: return COL_TRACK_CHAT;
    case ED_KIND_GAME: return COL_TRACK_GAME;
    default: return COL_TRACK_APP;
    }
}

static u32 ed_rgba(u32 rgb, i32 alpha) { return IM_COL32((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255, alpha); }

struct EdRow {
    i32         lane;  // 0 zoom, 1 + slot
    i32         wave;  // the waveform's track, -1 none
    i32         slot;  // the volume slot, -1 for the zoom
    f32         y0, y1;
    bool        scrolls; // one of the sound tracks, in the scrolled area
    const char *name;
};

// The timeline: a bar for the view, a ruler, the video with the trim, the zoom's lane, a lane for each sound track
// (its waveform and its volume) and one for the whole mix. Lanes have their name and buttons on the left.
// Measures the waveforms again, from the file rather than the cache
static void ed_refresh_wave()
{
    Editor &ed = g_app.editor;
    if (ed.wave) {
        Path cache = ed.wave->cache;
        wave_close(&ed.wave);
        DeleteFileW(cache.s);
    }
    i32 tracks[MEDIA_MAX_TRACKS];
    i32 n = ed_tracks(ed.item.info, tracks, MEDIA_MAX_TRACKS);
    ed.wave = wave_open(ed.path, ed.item.key, ed.item.info.duration, tracks, n);
    log_line("editor: audio preview measured again for %s", ed.item.name);
}

static void ed_timeline(ImVec2 o, f32 w, f32 h)
{
    Editor &ed = g_app.editor;
    Library &lib = g_app.library;
    Player &p = ed.player;
    ImGuiIO &io = ImGui::GetIO();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    f32 fh = ImGui::GetFrameHeight();
    f64 dur = ed_duration();
    if (dur <= 0) return;
    ed_view_clamp();
    if (!p.paused && ed.lane_drag == ED_DRAG_NONE && (p.pos < ed.view_from || p.pos > ed.view_from + ed.view_span)) {
        ed.view_from = p.pos - ed.view_span * 0.05; // follow the playhead a page at a time
        ed_view_clamp();
    }
    f64 from = ed.view_from, span = ed.view_span;

    f32 gut = kit_px(176), gap = kit_px(3), round = kit_px(6);
    f32 bx0 = o.x + gut, bx1 = o.x + w, bw = bx1 - bx0;
    if (bw < kit_px(40)) return;
    f32 bar_h = kit_px(8), ruler_h = kit_px(22), video_h = kit_px(46), lane_h = ed_lane_h(), vh = lane_h - 2 * ln_pad();
    auto x_of = [&](f64 t) { return bx0 + (f32)((t - from) / span) * bw; };
    auto t_of = [&](f32 x) {
        f64 t = from + (f64)(x - bx0) / bw * span;
        return t < 0 ? 0 : t > dur ? dur : t;
    };
    f32 mx = io.MousePos.x, my = io.MousePos.y;
    bool over = ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(o, ImVec2(o.x + w, o.y + h), false);

    i32 tracks[MEDIA_MAX_TRACKS];
    i32 nt = ed_tracks(ed.item.info, tracks, MEDIA_MAX_TRACKS);
    f32 bar_y = o.y, ruler_y = bar_y + bar_h + kit_px(8), video_y = ruler_y + ruler_h;
    f32 zoom_y = video_y + video_h + gap, mix_y = zoom_y + lane_h + gap;
    f32 area0 = nt ? mix_y + lane_h + gap : mix_y;
    f32 content = nt * (lane_h + gap);
    f32 area1 = o.y + h;
    if (area1 > area0 + content) area1 = area0 + content;
    if (area1 < area0) area1 = area0;
    f32 max_scroll = content - (area1 - area0) > 0 ? content - (area1 - area0) : 0;

    // The wheel pans; with Alt it zooms around the mouse, with Alt and Shift around the playhead. Shift alone sets
    // the lanes' height, and over the sound tracks' names the wheel scrolls them (when they don't all fit).
    if (over && (io.MouseWheel != 0 || io.MouseWheelH != 0)) {
        f32 wh = io.MouseWheel;
        if (wh && io.KeyAlt) {
            ed_view_zoom(pow(0.75, wh), !io.KeyShift && mx >= bx0 && mx < bx1 ? t_of(mx) : p.pos);
        } else if (wh && io.KeyShift) {
            Config &cfg = g_app.config;
            i32 v = (i32)(lane_h / kit_px(1) + 0.5f) + (wh > 0 ? 6 : -6);
            cfg.editor_lane = v < 36 ? 36 : v > 160 ? 160 : v;
            config_save(cfg, g_app.paths);
        } else if (wh && my >= area0 && my < area1 && mx < bx0 && max_scroll > 0) {
            ed.lane_scroll -= wh * lane_h * 0.6f;
        } else {
            ed.view_from -= (wh + io.MouseWheelH) * span * 0.15;
            ed_view_clamp();
        }
    }
    // The middle button pans wherever it's pressed on the timeline.
    if (over && ImGui::IsMouseClicked(ImGuiMouseButton_Middle) && ed.lane_drag == ED_DRAG_NONE) {
        ed.lane_drag = ED_DRAG_PAN;
        ed.drag_mx = mx;
        ed.drag_t = ed.view_from;
    }
    if (ed.lane_drag == ED_DRAG_PAN) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
            ed.view_from = ed.drag_t - (mx - ed.drag_mx) / bw * span;
            ed_view_clamp();
        } else {
            ed.lane_drag = ED_DRAG_NONE;
        }
    }
    ed.lane_scroll = ed.lane_scroll < 0 ? 0 : ed.lane_scroll > max_scroll ? max_scroll : ed.lane_scroll;

    // The view bar: the whole file, with the part shown; drag it to pan, its ends to zoom, double-click for all.
    ImGui::SetCursorScreenPos(ImVec2(bx0, bar_y - kit_px(3)));
    ImGui::InvisibleButton("##view", ImVec2(bw, bar_h + kit_px(6)));
    {
        f32 tx0 = bx0 + (f32)(ed.view_from / dur) * bw, tx1 = bx0 + (f32)((ed.view_from + ed.view_span) / dur) * bw;
        if (ImGui::IsItemActivated()) {
            f32 grip = kit_px(7);
            ed.drag_mx = mx;
            if (fabsf(mx - tx0) < grip && ed.view_span < dur) {
                ed.lane_drag = ED_DRAG_BAR_IN;
                ed.drag_t = ed.view_from + ed.view_span;
            } else if (fabsf(mx - tx1) < grip && ed.view_span < dur) {
                ed.lane_drag = ED_DRAG_BAR_OUT;
                ed.drag_t = ed.view_from;
            } else {
                if (mx < tx0 || mx > tx1) {
                    ed.view_from = (mx - bx0) / bw * dur - ed.view_span / 2;
                    ed_view_clamp();
                }
                ed.lane_drag = ED_DRAG_BAR;
                ed.drag_t = ed.view_from;
            }
        }
        if (ImGui::IsItemActive()) {
            f64 bt = (f64)(mx - bx0) / bw * dur;
            bt = bt < 0 ? 0 : bt > dur ? dur : bt;
            if (ed.lane_drag == ED_DRAG_BAR) ed.view_from = ed.drag_t + (mx - ed.drag_mx) / bw * dur;
            else if (ed.lane_drag == ED_DRAG_BAR_IN && bt < ed.drag_t) {
                ed.view_from = bt;
                ed.view_span = ed.drag_t - bt;
            } else if (ed.lane_drag == ED_DRAG_BAR_OUT && bt > ed.drag_t) {
                ed.view_span = bt - ed.drag_t;
            }
            ed_view_clamp();
        } else if (ImGui::IsItemDeactivated()) {
            ed.lane_drag = ED_DRAG_NONE;
        }
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            ed.view_from = 0;
            ed.view_span = dur;
        }
        bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
        if (hot && (fabsf(mx - tx0) < kit_px(7) || fabsf(mx - tx1) < kit_px(7)) && ed.view_span < dur)
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        tx0 = bx0 + (f32)(ed.view_from / dur) * bw;
        tx1 = bx0 + (f32)((ed.view_from + ed.view_span) / dur) * bw;
        if (tx1 - tx0 < kit_px(12)) {
            f32 c = (tx0 + tx1) / 2;
            tx0 = c - kit_px(6);
            tx1 = c + kit_px(6);
        }
        dl->AddRectFilled(ImVec2(bx0, bar_y), ImVec2(bx1, bar_y + bar_h), col32(COL_PANEL), bar_h / 2);
        dl->AddRectFilled(ImVec2(tx0, bar_y), ImVec2(tx1, bar_y + bar_h), col32(hot ? COL_DIM : COL_HOVER), bar_h / 2);
        f32 px = bx0 + (f32)(p.pos / dur) * bw;
        dl->AddRectFilled(ImVec2(px - kit_px(1), bar_y), ImVec2(px + kit_px(1), bar_y + bar_h), IM_COL32(255, 255, 255, 200));
    }
    from = ed.view_from;
    span = ed.view_span;

    // The view's zoom buttons, in the ruler's gutter.
    ImGui::SetCursorScreenPos(ImVec2(o.x + kit_px(8), ruler_y + (ruler_h + video_h - fh) / 2));
    kit_next_tip("Zoom out", "-");
    if (kit_icon_button(ICON_ZOOM_OUT, "##view_out", KIT_GHOST, span < dur)) ed_view_zoom(1.6, p.pos);
    ImGui::SameLine(0, kit_px(2));
    kit_next_tip("Zoom in", "+");
    if (kit_icon_button(ICON_ZOOM_IN, "##view_in", KIT_GHOST, span > 0.51)) ed_view_zoom(1 / 1.6, p.pos);
    {
        char vt[32], vs[48];
        fmt_time(span, vt, sizeof vt, span < 10);
        snprintf(vs, sizeof vs, "%s shown", vt);
        ImGui::PushFont(g_font_small);
        f32 tx = o.x + kit_px(8) + 2 * fh + kit_px(10);
        text_clipped(dl, ImVec2(tx, floorf(ruler_y + (ruler_h + video_h - g_font_small->FontSize) / 2)),
                     bx0 - gap - tx - kit_px(4), col32(COL_FAINT), vs);
        ImGui::PopFont();
    }

    // Ruler and video: scrub, or drag the trim's ends.
    f32 in_x = x_of(ed.edit.in_s), out_x = x_of(ed_out());
    ImGui::SetCursorScreenPos(ImVec2(bx0, ruler_y));
    ImGui::InvisibleButton("##scrub", ImVec2(bw, ruler_h + video_h));
    {
        f64 mt = t_of(mx);
        f32 grab = kit_px(9);
        bool near_in = fabsf(mx - in_x) < grab, near_out = fabsf(mx - out_x) < grab;
        if (ImGui::IsItemActivated()) ed.lane_drag = near_out ? ED_DRAG_OUT : near_in ? ED_DRAG_IN : ED_DRAG_SEEK;
        if (ImGui::IsItemActive()) {
            f64 minlen = 0.2;
            if (ed.lane_drag == ED_DRAG_IN) {
                f64 t = mt < ed_out() - minlen ? mt : ed_out() - minlen;
                ed.edit.in_s = t < 0 ? 0 : t;
                ed.dirty = true;
                p.scrubbing = true;
                player_seek(&p, ed.edit.in_s);
            } else if (ed.lane_drag == ED_DRAG_OUT) {
                f64 t = mt > ed.edit.in_s + minlen ? mt : ed.edit.in_s + minlen;
                ed.edit.out_s = t >= dur - 0.01 ? 0 : t;
                ed.dirty = true;
                p.scrubbing = true;
                player_seek(&p, t);
            } else if (ed.lane_drag == ED_DRAG_SEEK) {
                p.scrubbing = true;
                if (mt < p.pos - 0.02 || mt > p.pos + 0.02) player_seek(&p, mt);
            }
        } else if (ImGui::IsItemDeactivated()) {
            ed.lane_drag = ED_DRAG_NONE;
            p.scrubbing = false;
            player_seek(&p, p.pos);
        }
        if ((ImGui::IsItemHovered() && (near_in || near_out)) || ed.lane_drag == ED_DRAG_IN || ed.lane_drag == ED_DRAG_OUT)
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    }
    in_x = x_of(ed.edit.in_s);
    out_x = x_of(ed_out());

    // The ruler: a label every 70 px or more, small ticks between.
    dl->AddRectFilled(ImVec2(bx0, ruler_y), ImVec2(bx1, ruler_y + ruler_h), col32(COL_PANEL), round, ImDrawFlags_RoundCornersTop);
    {
        static const f64 steps[] = {0.1, 0.2, 0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 900, 1800, 3600, 7200, 14400};
        f64 step = steps[COUNT(steps) - 1];
        for (i32 i = 0; i < COUNT(steps); i++)
            if (steps[i] / span * bw >= kit_px(70)) { step = steps[i]; break; }
        f64 minor = step / 5;
        dl->PushClipRect(ImVec2(bx0, ruler_y), ImVec2(bx1, ruler_y + ruler_h), true);
        ImGui::PushFont(g_font_small);
        for (i64 i = (i64)floor(from / minor), last = (i64)ceil((from + span) / minor); i <= last; i++) {
            f32 x = floorf(x_of(i * minor));
            bool major = i % 5 == 0;
            f32 th = major ? ruler_h * 0.42f : ruler_h * 0.2f;
            dl->AddRectFilled(ImVec2(x, ruler_y + ruler_h - th), ImVec2(x + kit_px(1), ruler_y + ruler_h), col32(major ? COL_DIM : COL_FAINT));
            if (major) {
                char lt[32];
                fmt_time(i * minor, lt, sizeof lt, step < 1);
                dl->AddText(ImVec2(x + kit_px(4), ruler_y + kit_px(2)), col32(COL_DIM), lt);
            }
        }
        ImGui::PopFont();
        // The kept part, as a line along the ruler's foot.
        dl->AddRectFilled(ImVec2(in_x, ruler_y + ruler_h - kit_px(2)), ImVec2(out_x, ruler_y + ruler_h), col32(COL_ACCENT));
        dl->PopClipRect();
    }

    // The video: frames from the file's strip at the picture's shape, then the trim over them.
    {
        f32 y0 = video_y, y1 = video_y + video_h;
        dl->AddRectFilled(ImVec2(bx0, y0), ImVec2(bx1, y1), col32(COL_PANEL2));
        dl->PushClipRect(ImVec2(bx0, y0), ImVec2(bx1, y1), true);
        if (lib_want_strip(ed.item) && lib.strip_frames > 0) {
            f32 aspect = ed.item.info.height > 0 ? (f32)ed.item.info.width / ed.item.info.height : 16.f / 9;
            f32 tile = video_h * (aspect > 0.3f ? aspect : 1.f);
            for (f32 x = bx0; x < bx1; x += tile) {
                f64 t = t_of(x + tile / 2);
                i32 f = (i32)(t / dur * lib.strip_frames);
                f = f < 0 ? 0 : f >= lib.strip_frames ? lib.strip_frames - 1 : f;
                f32 u0 = (f32)f / lib.strip_frames, u1 = (f32)(f + 1) / lib.strip_frames;
                dl->AddImage((ImTextureID)(intptr_t)lib.strip, ImVec2(x, y0), ImVec2(x + tile, y1), ImVec2(u0, 0), ImVec2(u1, 1));
            }
        }
        if (in_x > bx0) dl->AddRectFilled(ImVec2(bx0, y0), ImVec2(in_x, y1), IM_COL32(0, 0, 0, 185));
        if (out_x < bx1) dl->AddRectFilled(ImVec2(out_x, y0), ImVec2(bx1, y1), IM_COL32(0, 0, 0, 185));
        bool trimming = ed.lane_drag == ED_DRAG_IN || ed.lane_drag == ED_DRAG_OUT;
        u32 acc = col32(trimming ? COL_ACCENT_HOVER : COL_ACCENT);
        f32 hw = kit_px(7), bw2 = kit_px(2), cy = (y0 + y1) / 2, gh = kit_px(8);
        dl->AddRectFilled(ImVec2(in_x, y0), ImVec2(out_x, y0 + bw2), acc);
        dl->AddRectFilled(ImVec2(in_x, y1 - bw2), ImVec2(out_x, y1), acc);
        dl->AddRectFilled(ImVec2(in_x, y0), ImVec2(in_x + hw, y1), acc, kit_px(3), ImDrawFlags_RoundCornersLeft);
        dl->AddRectFilled(ImVec2(out_x - hw, y0), ImVec2(out_x, y1), acc, kit_px(3), ImDrawFlags_RoundCornersRight);
        dl->AddRectFilled(ImVec2(in_x + hw / 2 - kit_px(1), cy - gh), ImVec2(in_x + hw / 2 + kit_px(1), cy + gh), IM_COL32(255, 255, 255, 200));
        dl->AddRectFilled(ImVec2(out_x - hw / 2 - kit_px(1), cy - gh), ImVec2(out_x - hw / 2 + kit_px(1), cy + gh), IM_COL32(255, 255, 255, 200));
        dl->PopClipRect();
    }

    // The lanes, top to bottom: the zoom, the mix, the sound tracks (scrolled).
    EdRow rows[MEDIA_MAX_TRACKS + 2];
    i32 nr = 0;
    rows[nr++] = {0, -1, -1, zoom_y, zoom_y + lane_h, false, "Zoom"};
    if (nt) rows[nr++] = {1 + EDIT_MASTER, -1, EDIT_MASTER, mix_y, mix_y + lane_h, false, "Mix"};
    for (i32 i = 0; i < nt; i++) {
        f32 ry = area0 + i * (lane_h + gap) - ed.lane_scroll;
        rows[nr++] = {1 + tracks[i], i, tracks[i], ry, ry + lane_h, true, ed.item.info.tracks[tracks[i]]};
    }
    f32 lanes_y1 = nt ? area1 : zoom_y + lane_h;

    auto row_at = [&](f32 y) -> i32 {
        for (i32 r = 0; r < nr; r++) {
            if (rows[r].scrolls && (y < area0 || y >= area1)) continue;
            if (y >= rows[r].y0 && y < rows[r].y1) return r;
        }
        return -1;
    };
    auto key_at = [&](i32 r, f32 x, f32 y) -> i32 {
        i32 l = rows[r].lane, best = -1;
        f32 bd = kit_px(8) * kit_px(8);
        for (i32 k = 0; k < ln_count(ed.edit, l); k++) {
            LaneKey q = ln_get(ed.edit, l, k);
            f32 dx = x - x_of(q.t), dy = y - ln_y(l, q.v, rows[r].y0, rows[r].y1);
            if (dx * dx + dy * dy < bd) {
                bd = dx * dx + dy * dy;
                best = k;
            }
        }
        return best;
    };

    // Pressing a lane: on a key, select it (Shift adds or takes it away) and drag the selection; Ctrl or a double-click
    // off a key adds one; on a flat stretch between two keys, drag its level; on a track with no keys, drag its whole
    // volume; anywhere else, move the playhead.
    ImGui::SetCursorScreenPos(ImVec2(bx0, zoom_y));
    ImGui::InvisibleButton("##lanes", ImVec2(bw, lanes_y1 - zoom_y > 1 ? lanes_y1 - zoom_y : 1));
    bool lanes_hot = ImGui::IsItemHovered();
    static bool keep_sel; // the press chose the selection, so letting go without moving leaves it
    if (ImGui::IsItemActivated()) {
        i32 r = row_at(my);
        ed.drag_mx = mx;
        ed.drag_my = my;
        ed.drag_moved = false;
        ed.lane_drag = ED_DRAG_NONE;
        keep_sel = false;
        if (r >= 0) {
            const EdRow &row = rows[r];
            i32 l = row.lane, k = key_at(r, mx, my);
            f64 mt = t_of(mx);
            bool dbl = ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
            if (k < 0 && (dbl || io.KeyCtrl)) {
                f64 v = dbl ? ln_snap(l, ln_v_of(l, my, row.y0, row.y1), vh) : ln_value_at(ed.edit, l, mt);
                ed_sel_clear();
                k = ed_put_key(l, floor(mt / ed_frame() + 0.5) * ed_frame(), v);
                if (k >= 0) ed.sel[l] |= 1ull << k;
                keep_sel = true;
            } else if (k >= 0 && io.KeyShift) {
                ed.sel[l] ^= 1ull << k;
                keep_sel = true;
                if (!((ed.sel[l] >> k) & 1)) k = -2; // taken away: nothing to drag
            } else if (k >= 0 && !((ed.sel[l] >> k) & 1)) {
                ed_sel_clear();
                ed.sel[l] |= 1ull << k;
                keep_sel = true;
            }
            if (k >= 0) {
                ed.lane_drag = ED_DRAG_KEYS;
                ed.drag_lane = l;
                ed.drag_key = k;
                ed.drag_from = ed.edit;
                memcpy(ed.drag_sel, ed.sel, sizeof ed.sel);
            } else if (k == -1) {
                i32 seg = -1, n = ln_count(ed.edit, l);
                for (i32 i = 0; l && i + 1 < n; i++) {
                    LaneKey a = ln_get(ed.edit, l, i), b = ln_get(ed.edit, l, i + 1);
                    if (a.t <= mt && mt <= b.t && fabs(a.v - b.v) < 1e-4 && a.ease != EASE_HOLD &&
                        fabsf(my - ln_y(l, a.v, row.y0, row.y1)) < kit_px(6)) {
                        seg = i;
                        break;
                    }
                }
                ed.drag_lane = l;
                if (seg >= 0) {
                    ed.lane_drag = ED_DRAG_STRETCH;
                    ed.drag_key = seg;
                    ed.drag_t = ln_get(ed.edit, l, seg).v;
                } else if (l && !n) {
                    ed.lane_drag = ED_DRAG_LEVEL;
                    ed.drag_t = 1 + ed.edit.boost[l - 1];
                } else {
                    if (!io.KeyShift) ed_sel_clear();
                    ed.lane_drag = ED_DRAG_SEEK;
                    p.scrubbing = true;
                    player_seek(&p, mt);
                }
            }
        }
    }
    if (ImGui::IsItemActive()) {
        if (fabsf(mx - ed.drag_mx) > kit_px(3) || fabsf(my - ed.drag_my) > kit_px(3)) ed.drag_moved = true;
        f64 dfrac = -(f64)(my - ed.drag_my) / vh, mt = t_of(mx);
        i32 l = ed.drag_lane;
        if (ed.lane_drag == ED_DRAG_KEYS && ed.drag_moved) {
            ed_drag_keys((mx - ed.drag_mx) / bw * span, dfrac, (f32)(bw / span), vh, io.KeyShift, io.KeyCtrl);
        } else if (ed.lane_drag == ED_DRAG_LEVEL && ed.drag_moved && l > 0) {
            ed.edit.boost[l - 1] = (f32)(ln_snap(l, ed.drag_t + dfrac * 2, vh) - 1);
            ed.dirty = true;
        } else if (ed.lane_drag == ED_DRAG_STRETCH && ed.drag_moved && l > 0 && ed.drag_key + 1 < ln_count(ed.edit, l)) {
            f32 v = (f32)ln_snap(l, ed.drag_t + dfrac * 2, vh);
            ed.edit.vol[l - 1][ed.drag_key].v = ed.edit.vol[l - 1][ed.drag_key + 1].v = v;
            ed.dirty = true;
        } else if (ed.lane_drag == ED_DRAG_SEEK) {
            if (mt < p.pos - 0.02 || mt > p.pos + 0.02) player_seek(&p, mt);
        }
    } else if (ImGui::IsItemDeactivated()) {
        if (ed.lane_drag == ED_DRAG_KEYS && !ed.drag_moved) {
            if (!keep_sel) {
                ed_sel_clear();
                ed.sel[ed.drag_lane] |= 1ull << ed.drag_key;
            }
            if (ed.drag_key < ln_count(ed.edit, ed.drag_lane)) player_seek(&p, ln_get(ed.edit, ed.drag_lane, ed.drag_key).t);
        } else if ((ed.lane_drag == ED_DRAG_LEVEL || ed.lane_drag == ED_DRAG_STRETCH) && !ed.drag_moved) {
            ed_sel_clear();
            player_seek(&p, t_of(ed.drag_mx));
        } else if (ed.lane_drag == ED_DRAG_SEEK) {
            p.scrubbing = false;
            player_seek(&p, p.pos);
        }
        if (ed.lane_drag != ED_DRAG_PAN) ed.lane_drag = ED_DRAG_NONE;
    }
    if (lanes_hot && ed.lane_drag == ED_DRAG_NONE) {
        i32 r = row_at(my);
        if (r >= 0 && key_at(r, mx, my) >= 0) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }

    // Right-click: the keys' eases, copy and delete; off a key, add one, paste, select or reset the lane.
    if (lanes_hot && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && ed.lane_drag == ED_DRAG_NONE) {
        i32 r = row_at(my);
        if (r >= 0) {
            i32 l = rows[r].lane, k = key_at(r, mx, my);
            ed.menu_lane = l;
            ed.menu_key = k;
            ed.menu_t = floor(t_of(mx) / ed_frame() + 0.5) * ed_frame();
            ed.menu_v = ln_snap(l, ln_v_of(l, my, rows[r].y0, rows[r].y1), vh);
            if (k >= 0 && !((ed.sel[l] >> k) & 1)) {
                ed_sel_clear();
                ed.sel[l] |= 1ull << k;
            }
            ImGui::OpenPopup("lane_menu");
        }
    }
    lib_popup_style_push(6);
    if (ImGui::BeginPopup("lane_menu")) {
        i32 l = ed.menu_lane;
        if (ed.menu_key >= 0 && ed_sel_count()) {
            static const char *const eases[EASE_COUNT] = {"Linear", "Smooth", "Ease in", "Ease out", "Hold"};
            i32 common = ed_sel_ease();
            for (i32 i = 0; i < EASE_COUNT; i++)
                if (lib_menu_item(common == i ? ICON_CHECK : "", eases[i])) ed_set_ease(i);
            ImGui::Separator();
            if (lib_menu_item(ICON_COPY, "Copy")) ed_copy();
            if (lib_menu_item(ICON_DELETE, "Delete", true)) ed_delete_selected();
        } else {
            if (ln_count(ed.edit, l) < ln_cap(l) && lib_menu_item(ICON_ADD, "Add a key here")) {
                ed_sel_clear();
                i32 k = ed_put_key(l, ed.menu_t, ed.menu_v);
                if (k >= 0) ed.sel[l] |= 1ull << k;
            }
            if (ed.clip_count && lib_menu_item(ICON_COPY, "Paste at the playhead")) ed_paste(p.pos);
            if (ln_count(ed.edit, l) && lib_menu_item(ICON_CHECK, "Select the lane's keys")) {
                ed_sel_clear();
                ed.sel[l] = bits_all(ln_count(ed.edit, l));
            }
            ImGui::Separator();
            if (lib_menu_item(ICON_UNDO, l ? "Reset the volume" : "Reset the zoom", true)) {
                if (l) {
                    ed.edit.vol_count[l - 1] = 0;
                    ed.edit.boost[l - 1] = 0;
                } else {
                    ed.edit.key_count = 0;
                }
                ed.sel[l] = 0;
                ed.dirty = true;
            }
            if (l && lib_menu_item(ICON_VOLUME, "Refresh the audio preview")) ed_refresh_wave();
        }
        ImGui::EndPopup();
    }
    lib_popup_style_pop();

    // Each lane: its gutter (mute, solo, a key at the playhead, the name and the value there), then its body.
    f64 in_s = ed.edit.in_s, out_s = ed_out();
    static ImVec2 pts[4096];
    for (i32 r = 0; r < nr; r++) {
        const EdRow &row = rows[r];
        if (row.scrolls && (row.y1 <= area0 || row.y0 >= area1)) continue;
        i32 l = row.lane, s = row.slot;
        f32 y0 = row.y0, y1 = row.y1;
        if (row.scrolls) ImGui::PushClipRect(ImVec2(o.x, area0), ImVec2(bx1, area1), true);
        bool muted = s >= 0 && ed.edit.muted[s];
        dl->AddRectFilled(ImVec2(o.x, y0), ImVec2(bx0 - gap, y1), col32(COL_PANEL), round, ImDrawFlags_RoundCornersLeft);
        dl->AddRectFilled(ImVec2(bx0, y0), ImVec2(bx1, y1), col32(COL_PANEL), round, ImDrawFlags_RoundCornersRight);
        u32 kind_rgb = s >= 0 ? ed_kind_rgb(ed_track_kind(row.name, s)) : 0;
        if (s >= 0) // the kind of track, as a strip down the gutter's left edge
            dl->AddRectFilled(ImVec2(o.x, y0), ImVec2(o.x + kit_px(3), y1), ed_rgba(kind_rgb, muted ? 70 : 220), round,
                              ImDrawFlags_RoundCornersLeft);

        ImGui::PushID(l);
        f32 gx = o.x + kit_px(6), by = floorf((y0 + y1 - fh) / 2);
        if (s >= 0) {
            ImGui::SetCursorScreenPos(ImVec2(gx, by));
            kit_next_tip(muted ? "Unmute this track" : "Mute this track");
            if (kit_icon_button(muted ? ICON_MUTE : ICON_VOLUME, "##mute", KIT_GHOST, true, muted)) {
                ed.edit.muted[s] = !muted;
                ed.dirty = true;
                muted = !muted;
            }
            bool solo = ed.solo == s;
            ImGui::SetCursorScreenPos(ImVec2(gx + fh + kit_px(2), by));
            kit_next_tip(solo ? "Hear every track" : "Hear only this track");
            if (kit_icon_button(ICON_SOLO, "##solo", KIT_GHOST, true, solo)) {
                ed.solo = solo ? -1 : s;
                log_line("editor: solo %d", ed.solo);
            }
        }
        i32 here = ln_key_at(ed.edit, l, p.pos);
        ImGui::SetCursorScreenPos(ImVec2(gx + 2 * (fh + kit_px(2)), by));
        kit_next_tip(here >= 0 ? "Delete this key" : "Add a key at the playhead");
        if (kit_icon_button(here >= 0 ? ICON_DELETE : ICON_ADD, "##key", KIT_GHOST, here >= 0 || ln_count(ed.edit, l) < ln_cap(l))) {
            if (here >= 0) {
                ln_erase(ed.edit, l, here, &ed.sel[l]);
                ed.dirty = true;
            } else {
                ed_sel_clear();
                i32 k = ed_put_key(l, p.pos, ln_value_at(ed.edit, l, p.pos));
                if (k >= 0) ed.sel[l] |= 1ull << k;
            }
        }
        ImGui::PopID();
        {
            f32 nx = gx + 3 * fh + kit_px(12), nw = bx0 - gap - nx - kit_px(6);
            bool unheard = muted || (s >= 0 && ed.solo >= 0 && ed.solo != s);
            f32 lh = ImGui::GetTextLineHeight(), sh = g_font_small->FontSize;
            f32 ty = floorf((y0 + y1 - lh - sh - kit_px(2)) / 2);
            text_clipped(dl, ImVec2(nx, ty), nw, col32(unheard ? COL_FAINT : COL_TEXT), row.name);
            char vt[24];
            f64 v = ln_value_at(ed.edit, l, p.pos);
            if (muted) str_set(vt, "Muted");
            else if (l) snprintf(vt, sizeof vt, "%.0f%%", v * 100);
            else snprintf(vt, sizeof vt, "%.1fx", v);
            ImGui::PushFont(g_font_small);
            text_clipped(dl, ImVec2(nx, ty + lh + kit_px(2)), nw, col32(COL_DIM), vt);
            ImGui::PopFont();
        }

        dl->PushClipRect(ImVec2(bx0, y0), ImVec2(bx1, y1), true);
        // The waveform, from the bottom, at the volume it's heard at: a track's own, the mix's the loudest of the
        // tracks as they're mixed. Not yet measured: a faint stub.
        if (s >= 0 && ed.wave) {
            f32 step = floorf(kit_px(2)) > 1 ? floorf(kit_px(2)) : 1, reach = y1 - y0 - kit_px(3);
            f32 end_x = x_of(dur);
            for (f32 x = bx0; x < bx1 && x < end_x; x += step) {
                f64 t0 = t_of(x), t1 = t_of(x + step), tm = (t0 + t1) / 2;
                f32 pk = 0;
                bool got = false;
                if (s == EDIT_MASTER) {
                    for (i32 i = 0; i < nt; i++) {
                        f32 q;
                        if (!wave_peak(ed.wave, i, t0, t1, &q)) continue;
                        got = true;
                        q *= (f32)vol_heard(ed.edit, tracks[i], tm);
                        pk = q > pk ? q : pk;
                    }
                } else {
                    got = wave_peak(ed.wave, row.wave, t0, t1, &pk);
                }
                bool kept = t1 > in_s && t0 < out_s;
                if (!got) {
                    dl->AddRectFilled(ImVec2(x, y1 - kit_px(2)), ImVec2(x + step, y1), IM_COL32(142, 147, 156, 40));
                    continue;
                }
                f32 g = (f32)vol_heard(ed.edit, s, tm), e = pk * g / 2;
                f32 hh = sqrtf(e < 1 ? e : 1) * reach;
                if (hh < 0.5f) continue;
                u32 col = ed_rgba(kind_rgb, s == EDIT_MASTER ? (kept ? 64 : 24) : (kept ? 110 : 34));
                dl->AddRectFilled(ImVec2(x, y1 - hh), ImVec2(x + step, y1), col);
            }
        }
        // 100% (or no zoom), then the value's line as the render will move it.
        f32 rest = ln_y(l, 1, y0, y1);
        dl->AddRectFilled(ImVec2(bx0, rest), ImVec2(bx1, rest + kit_px(1)), col32(COL_LINE));
        {
            i32 np = 0;
            f32 lstep = kit_px(3);
            for (f32 x = bx0; np < COUNT(pts) - 1; x += lstep) {
                if (x > bx1) x = bx1;
                pts[np++] = ImVec2(x, ln_y(l, ln_value_at(ed.edit, l, t_of(x)), y0, y1));
                if (x >= bx1) break;
            }
            u32 lc = !l ? col32(COL_BLUE) : muted ? col32(COL_FAINT) : col32(COL_TEXT);
            dl->AddPolyline(pts, np, lc, ImDrawFlags_None, kit_px(1.5f));
        }
        // The keys.
        for (i32 k = 0; k < ln_count(ed.edit, l); k++) {
            LaneKey q = ln_get(ed.edit, l, k);
            f32 kx = x_of(q.t), r0 = kit_px(5);
            if (kx < bx0 - 2 * r0 || kx > bx1 + 2 * r0) continue;
            ImVec2 c(kx, ln_y(l, q.v, y0, y1));
            bool on = (ed.sel[l] >> k) & 1;
            key_shape(dl, c, r0 + kit_px(2), q.ease, on ? IM_COL32_WHITE : IM_COL32(0, 0, 0, 170));
            key_shape(dl, c, r0, q.ease, col32(on ? COL_ACCENT : !l ? COL_BLUE : muted ? COL_FAINT : 0xd6d9de));
        }
        // Outside the trim, darkened.
        if (in_x > bx0) dl->AddRectFilled(ImVec2(bx0, y0), ImVec2(in_x, y1), IM_COL32(0, 0, 0, 110));
        if (out_x < bx1) dl->AddRectFilled(ImVec2(out_x, y0), ImVec2(bx1, y1), IM_COL32(0, 0, 0, 110));
        dl->PopClipRect();
        if (row.scrolls) ImGui::PopClipRect();
    }

    // The playhead, through everything.
    f32 px = x_of(p.pos);
    if (px >= bx0 - 1 && px <= bx1 + 1) {
        dl->AddRectFilled(ImVec2(px - kit_px(1), ruler_y), ImVec2(px + kit_px(1), lanes_y1), IM_COL32_WHITE);
        f32 k = kit_px(6);
        dl->AddTriangleFilled(ImVec2(px - k, ruler_y), ImVec2(px + k, ruler_y), ImVec2(px, ruler_y + k), IM_COL32_WHITE);
    }
}

// A group's caption on the left and its page controls on the right, on one row.
static void ed_group_head(const char *label, const char *id, i32 *page, i32 pages, f32 w)
{
    ImVec2 a = ImGui::GetCursorScreenPos();
    f32 fh = ImGui::GetFrameHeight();
    ImGui::PushFont(g_font_small);
    ImVec2 ts = ImGui::CalcTextSize(label);
    ImGui::GetWindowDrawList()->AddText(ImVec2(a.x, floorf(a.y + (fh - ts.y) / 2)), col32(COL_FAINT), label);
    ImGui::PopFont();
    ImGui::SetCursorScreenPos(ImVec2(a.x + w - lib_pager_w(), a.y));
    lib_pager(id, page, pages);
}

// A plain caption row the same height as ed_group_head, for groups with no pages.
static void ed_caption(const char *label)
{
    ImGui::PushFont(g_font_small);
    ImVec2 a = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddText(a, col32(COL_FAINT), label);
    ImGui::Dummy(ImVec2(1, g_font_small->FontSize));
    ImGui::PopFont();
}

// Open in Premiere Pro or DaVinci Resolve: the edit as a Final Cut Pro 7 XML (xmeml) sequence, which both import.
// One video track with the zoom as Basic Motion keys, and each recorded track (the Mix left out) with its volume
// times the whole mix's as Audio Levels keys; muted tracks are turned off. Eased curves are written as keys a fifth
// of a second apart. Key times count frames from the start of the file.
// A stereo track is a linked pair of channel tracks marked Stereo (one stereo track once in Premiere), a mono Mic or
// chat one track marked Mono. Premiere numbers a file's audio by slot, each track taking as many slots as the widest
// one has channels: with any stereo track, track t's channel c is slot 2t + c + 1.

static void xml_text(FILE *f, const char *s)
{
    for (; *s; s++) {
        if (*s == '&') fputs("&amp;", f);
        else if (*s == '<') fputs("&lt;", f);
        else if (*s == '>') fputs("&gt;", f);
        else if (*s == '"') fputs("&quot;", f);
        else fputc(*s, f);
    }
}

// file://localhost/C:/Clips/a%20b.mp4
static void xml_pathurl(FILE *f, const char *path)
{
    fputs("file://localhost/", f);
    for (const u8 *p = (const u8 *)path; *p; p++) {
        u8 c = *p == '\\' ? '/' : *p;
        bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || strchr("/-_.~:", c);
        if (plain) fputc(c, f);
        else fprintf(f, "%%%02X", c);
    }
}

// The times a curve is written at between the trim's ends, a fifth of a second apart (wider on long files).
static i32 xml_samples(f64 in_s, f64 out_s, f64 *at, i32 cap)
{
    f64 step = (out_s - in_s) / (cap - 1) > 0.2 ? (out_s - in_s) / (cap - 1) : 0.2;
    i32 n = 0;
    for (f64 t = in_s; t < out_s && n < cap - 1; t += step) at[n++] = t;
    at[n++] = out_s;
    return n;
}

static void xml_rate(FILE *f, i32 fps) { fprintf(f, "<rate><timebase>%d</timebase><ntsc>FALSE</ntsc></rate>", fps); }

// Writes <video name>.xml next to the video
static bool ed_export_xml(Path *out)
{
    Editor &ed = g_app.editor;
    const MediaInfo &m = ed.item.info;
    const EditRecord &e = ed.edit;
    i32 fps = m.fps > 1 ? (i32)(m.fps + 0.5) : 60;
    f64 in_s = e.in_s, out_s = ed_out();
    auto fr = [&](f64 t) { return (i64)(t * fps + 0.5); };
    *out = ed.path;
    wchar_t *dot = wcsrchr(out->s, L'.');
    if (!dot || dot < wcsrchr(out->s, L'\\')) return false;
    wcscpy_s(dot, MAX_PATH - (dot - out->s), L".xml");
    FILE *f = _wfopen(out->s, L"wb");
    if (!f) return false;
    char src[MAX_PATH * 3], file_name[MAX_PATH * 3];
    wide_to_utf8(ed.path.s, src, sizeof src);
    const char *slash = strrchr(src, '\\');
    str_set(file_name, slash ? slash + 1 : src);
    i64 len = fr(out_s) - fr(in_s), file_len = fr(m.duration);
    static f64 at[2000];
    i32 na = xml_samples(in_s, out_s, at, COUNT(at));

    fputs("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE xmeml>\n<xmeml version=\"5\">\n<sequence id=\"sequence-1\">\n<name>", f);
    xml_text(f, ed.item.name);
    fprintf(f, "</name>\n<duration>%lld</duration>", len);
    xml_rate(f, fps);
    fputs("\n<media>\n<video>\n<format><samplecharacteristics>", f);
    xml_rate(f, fps);
    fprintf(f, "<width>%d</width><height>%d</height><pixelaspectratio>square</pixelaspectratio></samplecharacteristics></format>\n",
            m.width, m.height);
    auto clip_head = [&](i32 id, bool enabled, const char *channel_type) {
        fprintf(f, "<clipitem id=\"clipitem-%d\"%s%s%s>\n<name>", id, channel_type ? " premiereChannelType=\"" : "",
                channel_type ? channel_type : "", channel_type ? "\"" : "");
        xml_text(f, file_name);
        fprintf(f, "</name><enabled>%s</enabled><duration>%lld</duration>", enabled ? "TRUE" : "FALSE", file_len);
        xml_rate(f, fps);
        fprintf(f, "<start>0</start><end>%lld</end><in>%lld</in><out>%lld</out>\n", len, fr(in_s), fr(out_s));
    };
    // The video, and the file (written in full once, then referred to by its id)
    fputs("<track>\n", f);
    clip_head(1, true, 0);
    fputs("<file id=\"file-1\"><name>", f);
    xml_text(f, file_name);
    fputs("</name><pathurl>", f);
    xml_pathurl(f, src);
    fputs("</pathurl>", f);
    xml_rate(f, fps);
    fprintf(f, "<duration>%lld</duration>\n<media><video><samplecharacteristics>", file_len);
    xml_rate(f, fps);
    fprintf(f, "<width>%d</width><height>%d</height></samplecharacteristics></video>", m.width, m.height);
    auto chans = [&](i32 t) { return m.channels[t] == 1 ? 1 : 2; };
    for (i32 t = 0; t < m.track_count; t++)
        fprintf(f, "<audio><samplecharacteristics><depth>16</depth><samplerate>48000</samplerate></samplecharacteristics>"
                "<channelcount>%d</channelcount></audio>", chans(t));
    fputs("</media></file>\n", f);
    if (e.key_count) {
        // Scale in percent; Center moves the picture's middle, in fractions of the frame from the middle
        fputs("<filter><effect><name>Basic Motion</name><effectid>basic</effectid><effectcategory>motion</effectcategory>"
              "<effecttype>motion</effecttype><mediatype>video</mediatype>\n"
              "<parameter><parameterid>scale</parameterid><name>Scale</name><valuemin>0</valuemin><valuemax>1000</valuemax>"
              "<value>100</value>\n", f);
        for (i32 i = 0; i < na; i++) {
            f64 z, x, y;
            zoom_at(e, at[i], &z, &x, &y);
            fprintf(f, "<keyframe><when>%lld</when><value>%.2f</value></keyframe>\n", fr(at[i]), z * 100);
        }
        fputs("</parameter>\n<parameter><parameterid>center</parameterid><name>Center</name>"
              "<value><horiz>0</horiz><vert>0</vert></value>\n", f);
        for (i32 i = 0; i < na; i++) {
            ImVec2 a, b;
            zoom_uv(e, at[i], &a, &b);
            f64 z = b.x - a.x > 0.001f ? 1.0 / (b.x - a.x) : 1;
            f64 cx = (0.5 - (a.x + b.x) / 2) * z, cy = (0.5 - (a.y + b.y) / 2) * z;
            fprintf(f, "<keyframe><when>%lld</when><value><horiz>%.4f</horiz><vert>%.4f</vert></value></keyframe>\n",
                    fr(at[i]), cx, cy);
        }
        fputs("</parameter></effect></filter>\n", f);
    }
    fputs("</clipitem>\n</track>\n</video>\n<audio>\n", f);

    // The audio: each recorded track's channels, from the same file
    i32 tracks[MEDIA_MAX_TRACKS];
    i32 nt = ed_tracks(m, tracks, MEDIA_MAX_TRACKS);
    bool master_keys = e.vol_count[EDIT_MASTER] > 0;
    i32 clip_id = 2, seq_track = 1, slots = 1;
    for (i32 t = 0; t < m.track_count; t++) slots = ImMax(slots, chans(t));
    for (i32 i = 0; i < nt; i++) {
        i32 t = tracks[i], n = chans(t);
        for (i32 c = 0; c < n; c++) {
            fprintf(f, "<track premiereTrackType=\"%s\" currentExplodedTrackIndex=\"%d\" totalExplodedTrackCount=\"%d\">\n",
                    n == 2 ? "Stereo" : "Mono", c, n);
            clip_head(clip_id + c, !e.muted[t] && !e.muted[EDIT_MASTER], n == 2 ? "stereo" : "mono");
            fprintf(f, "<file id=\"file-1\"/>\n<sourcetrack><mediatype>audio</mediatype><trackindex>%d</trackindex></sourcetrack>\n",
                    slots * t + c + 1);
            if (n == 2) // the pair stays together
                for (i32 l = 0; l < 2; l++)
                    fprintf(f, "<link><linkclipref>clipitem-%d</linkclipref><mediatype>audio</mediatype><trackindex>%d</trackindex>"
                            "<clipindex>1</clipindex><groupindex>1</groupindex></link>\n", clip_id + l, seq_track + l);
            fputs("<filter><effect><name>Audio Levels</name><effectid>audiolevels</effectid><effectcategory>audiolevels"
                  "</effectcategory><effecttype>audiolevels</effecttype><mediatype>audio</mediatype>\n<parameter>"
                  "<parameterid>level</parameterid><name>Level</name><valuemin>0</valuemin><valuemax>3.98109</valuemax>", f);
            fprintf(f, "<value>%.4f</value>\n", vol_at(e, t, in_s) * vol_at(e, EDIT_MASTER, in_s));
            if (e.vol_count[t] || master_keys)
                for (i32 k = 0; k < na; k++)
                    fprintf(f, "<keyframe><when>%lld</when><value>%.4f</value></keyframe>\n", fr(at[k]),
                            vol_at(e, t, at[k]) * vol_at(e, EDIT_MASTER, at[k]));
            fputs("</parameter></effect></filter>\n</clipitem>\n</track>\n", f);
        }
        clip_id += n;
        seq_track += n;
    }
    fputs("</audio>\n</media>\n</sequence>\n</xmeml>\n", f);
    bool ok = !ferror(f);
    fclose(f);
    return ok;
}

static bool editor_exe(bool premiere, Path *out); // options.cpp

// Writes the XML next to the video and starts the editor; it's brought in there with File > Import.
static void ed_open_in(bool premiere)
{
    Editor &ed = g_app.editor;
    const char *app = premiere ? "Premiere Pro" : "DaVinci Resolve";
    Path xml;
    ed_save_record();
    if (!ed_export_xml(&xml)) {
        snprintf(ed.result, sizeof ed.result, "Couldn't write the XML for %s", app);
        ed.result_error = true;
        return;
    }
    char x8[MAX_PATH * 3];
    wide_to_utf8(xml.s, x8, sizeof x8);
    const char *name = strrchr(x8, '\\');
    name = name ? name + 1 : x8;
    log_line("editor: wrote %s for %s", x8, app);
    Path exe;
    if (!editor_exe(premiere, &exe)) {
        snprintf(ed.result, sizeof ed.result, "Saved %s next to the video. %s isn't found: pick its exe in Options.", name, app);
        ed.result_error = true;
        return;
    }
    // Neither app takes the XML on its command line, so the XML is shown in its folder, ready for File > Open.
    ShellExecuteW(0, L"open", exe.s, 0, 0, SW_SHOWNORMAL);
    wchar_t sel[MAX_PATH + 16];
    swprintf_s(sel, L"/select,\"%s\"", xml.s);
    ShellExecuteW(0, L"open", L"explorer.exe", sel, 0, SW_SHOWNORMAL);
    snprintf(ed.result, sizeof ed.result, "Saved %s next to the video. In %s: %s, then pick it.", name, app,
             premiere ? "File > Open" : "File > Import > Timeline");
    ed.result_error = false;
}

static void ed_edit_tab(f32 w)
{
    Editor &ed = g_app.editor;
    Player &p = ed.player;
    const ImGuiStyle &st = ImGui::GetStyle();
    f32 fh = ImGui::GetFrameHeight(), sp = st.ItemSpacing.x;
    ImDrawList *dl = ImGui::GetWindowDrawList();

    // The game it's filed under: pick one the library knows or type another.
    ed_caption("GAME");
    {
        Library &lib = g_app.library;
        static char other[64];
        ImGui::SetNextItemWidth(w);
        ImGui::PushStyleColor(ImGuiCol_PopupBg, rgb(COL_PANEL2));
        const char *pick = 0;
        if (ImGui::BeginCombo("##game", ed.item.info.game[0] ? ed.item.info.game : "None")) {
            if (ImGui::IsWindowAppearing()) {
                other[0] = 0;
                ImGui::SetKeyboardFocusHere();
            }
            ImGui::SetNextItemWidth(-1);
            char typed[64];
            bool enter = ImGui::InputTextWithHint("##other", "Type a game, then Enter", other, sizeof other,
                                                  ImGuiInputTextFlags_EnterReturnsTrue);
            str_set(typed, other);
            char *tt = trim(typed);
            if (enter && *tt) {
                pick = trim(other);
                ImGui::CloseCurrentPopup();
            }
            for (i32 i = 0; i < lib.game_count; i++) {
                if (*tt && !lib_matches(lib.games[i], tt)) continue;
                if (ImGui::Selectable(lib.games[i], !strcmp(lib.games[i], ed.item.info.game))) pick = lib.games[i];
            }
            ImGui::EndCombo();
        }
        ImGui::PopStyleColor();
        char g[64] = "";
        if (pick) str_set(g, pick); // pick may point into the list the rescan rewrites
        if (g[0] && strcmp(g, ed.item.info.game) && library_set_game(ed.item, g)) {
            str_set(ed.item.info.game, g);
            snprintf(ed.result, sizeof ed.result, "Filed under %s", g);
            ed.result_error = false;
        }
    }
    ImGui::Dummy(ImVec2(1, kit_px(8)));

    // Zoom at the playhead: a slider with its value on the right.
    ed_caption("ZOOM");
    f64 z, cx, cy;
    zoom_at(ed.edit, p.pos, &z, &cx, &cy);
    f32 zf = (f32)z, val_w = kit_px(44);
    if (lib_slider("##zoom", &zf, 1.f, 4.f, w - val_w, fh)) {
        i32 k = ed_add_key(p.pos);
        if (k >= 0) {
            ed.edit.keys[k].z = zf;
            ed.dirty = true;
        }
    }
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        i32 k = ed_key_at(p.pos);
        if (k >= 0) { ed.edit.keys[k].z = 1; ed.dirty = true; }
    }
    ImGui::SameLine(0, 0);
    char zt[16];
    snprintf(zt, sizeof zt, "%.1fx", zf);
    ImVec2 zp = ImGui::GetCursorScreenPos(), zs = ImGui::CalcTextSize(zt);
    dl->AddText(ImVec2(zp.x + val_w - zs.x, floorf(zp.y + (fh - zs.y) / 2)), col32(COL_TEXT), zt);
    ImGui::Dummy(ImVec2(val_w, fh));
    kit_dim("Drag the picture to aim");
    ImGui::Dummy(ImVec2(1, kit_px(8)));

    // The selected keys: how they ease into and out of the next, then copy and delete.
    i32 n = ed_sel_count();
    char cap[48];
    if (n) snprintf(cap, sizeof cap, "%d KEY%s SELECTED", n, n > 1 ? "S" : "");
    else str_set(cap, "KEYS");
    ed_caption(cap);
    static const char *const eases[EASE_COUNT] = {"Linear", "Smooth", "In", "Out", "Hold"};
    i32 common = n ? ed_sel_ease() : -1, pick = common;
    ImGui::BeginDisabled(!n);
    if (kit_segmented("##ease", eases, EASE_COUNT, &pick, (w - kit_px(6)) / (f32)EASE_COUNT) && pick >= 0) ed_set_ease(pick);
    f32 bw = (w - sp) / 2;
    if (kit_button(ICON_COPY "  Copy", KIT_GHOST, ImVec2(bw, 0), n > 0)) ed_copy();
    ImGui::SameLine();
    if (kit_button(ICON_DELETE "  Delete", KIT_GHOST, ImVec2(bw, 0), n > 0)) ed_delete_selected();
    ImGui::EndDisabled();
    ImGui::Dummy(ImVec2(1, kit_px(8)));

    // Undo and redo, as buttons too.
    ed_caption("CHANGES");
    if (kit_button(ICON_UNDO "  Undo", KIT_GHOST, ImVec2(bw, 0), ed.undo_count > 0 || memcmp(&ed.edit, &ed.settled, sizeof ed.edit)))
        ed_undo(false);
    ImGui::SameLine();
    if (kit_button(ICON_RIGHT "  Redo", KIT_GHOST, ImVec2(bw, 0), ed.redo_count > 0)) ed_undo(true);
    ImGui::Dummy(ImVec2(1, kit_px(8)));

    // The edit as an XML sequence, into a full editor
    ed_caption("OPEN IN");
    if (kit_button(ICON_VIDEO "  Premiere Pro", KIT_GHOST, ImVec2(bw, 0))) ed_open_in(true);
    ImGui::SameLine();
    if (kit_button(ICON_VIDEO "  DaVinci", KIT_GHOST, ImVec2(bw, 0))) ed_open_in(false);
    if (ed.result[0]) {
        ImGui::PushStyleColor(ImGuiCol_Text, rgb(ed.result_error ? 0xff8a92 : COL_DIM));
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w);
        ImGui::TextUnformatted(ed.result);
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
    }
}

// A file the tab made or lists: Copy (to paste anywhere), Play, Show in folder. Right-aligned at the cursor's row.
static void ed_file_actions(const char *path, f32 right, f32 y)
{
    Editor &ed = g_app.editor;
    f32 fh = ImGui::GetFrameHeight(), gap = kit_px(2);
    wchar_t w[MAX_PATH];
    utf8_to_wide(path, w, MAX_PATH);
    const char *name = strrchr(path, '\\');
    name = name ? name + 1 : path;
    ImGui::SetCursorScreenPos(ImVec2(right - 3 * fh - 2 * gap, y));
    kit_next_tip("Copy the file");
    if (kit_icon_button(ICON_COPY, "##copy", KIT_GHOST)) {
        bool ok = clipboard_copy_file(w);
        snprintf(ed.result, sizeof ed.result, ok ? "Copied %s" : "Couldn't copy %s", name);
        ed.result_error = !ok;
        str_set(ed.result_path, path);
    }
    ImGui::SameLine(0, gap);
    kit_next_tip("Play");
    if (kit_icon_button(ICON_PLAY, "##play", KIT_GHOST)) ShellExecuteW(0, L"open", w, 0, 0, SW_SHOWNORMAL);
    ImGui::SameLine(0, gap);
    kit_next_tip("Show in folder");
    if (kit_icon_button(ICON_FOLDER, "##show", KIT_GHOST)) {
        Path p;
        wcsncpy(p.s, w, MAX_PATH - 1);
        p.s[MAX_PATH - 1] = 0;
        reveal_file(p);
    }
}

// The height of a card's title row
static f32 ed_card_head_h() { return ImGui::GetTextLineHeight() + kit_px(2) + g_font_small->FontSize; }

// A card's title row: icon, title, one dim line under it, kept to width w (its button goes to the right of it)
static void ed_card_head(const char *icon, const char *title, const char *line, f32 w, u32 line_col = 0)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 a = ImGui::GetCursorScreenPos();
    f32 lh = ImGui::GetTextLineHeight();
    dl->AddText(a, col32(COL_ACCENT), icon);
    f32 tx = a.x + lh + kit_px(8);
    text_clipped(dl, ImVec2(tx, a.y), a.x + w - tx, col32(COL_TEXT), title);
    ImGui::PushFont(g_font_small);
    text_clipped(dl, ImVec2(tx, a.y + lh + kit_px(2)), a.x + w - tx, line_col ? line_col : col32(COL_DIM), line);
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(1, ed_card_head_h()));
}

// Keep and Share. Keep makes a full-quality copy of the trim in Clips; Share makes a small one in the temp share
// folder and puts it on the clipboard. Neither touches the original, so a share can be made again at another size.
static void ed_share_tab(f32 w)
{
    Editor &ed = g_app.editor;
    const ImGuiStyle &st = ImGui::GetStyle();
    f32 fh = ImGui::GetFrameHeight();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    bool keeping = ed_job_busy(ed.keep_job), sharing = ed_job_busy(ed.share_job);
    f32 iw = w;

    // The name both use
    if (!ed.out_name[0]) str_set(ed.out_name, ed.item.name);
    ImGui::SetNextItemWidth(w);
    ImGui::InputTextWithHint("##outname", "Name", ed.out_name, sizeof ed.out_name);
    ImGui::Dummy(ImVec2(1, kit_px(2)));

    // How long the trim is, for both cards
    const MediaInfo &m = ed.item.info;
    f64 in_s = ed.edit.in_s > 0 ? ed.edit.in_s : 0, len = m.duration;
    if (ed.player.duration > 0) len = ed.player.duration;
    if (ed.edit.out_s > in_s) len = ed.edit.out_s;
    len -= in_s;
    char len_s[32];
    fmt_time(len, len_s, sizeof len_s);

    // Keep: the title row with its button beside it
    f32 head_h = ed_card_head_h(), bw = kit_px(120), gap = kit_px(12);
    f32 keep_row = head_h > fh ? head_h : fh;
    {
        char line[96];
        snprintf(line, sizeof line, "Full quality, %s, saved in Clips", len_s);
        ImVec2 top = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(ImVec2(top.x, top.y + (keep_row - head_h) / 2));
        ed_card_head(ICON_SAVE, "Keep", line, iw - bw - gap);
        ImGui::SetCursorScreenPos(ImVec2(top.x + iw - bw, top.y + (keep_row - fh) / 2));
        if (kit_button(keeping ? "Keeping..." : "Keep", KIT_SECONDARY, ImVec2(bw, 0), !keeping)) ed_start(SAVE_KEEP);
        ImGui::SetCursorScreenPos(ImVec2(top.x, top.y + keep_row + st.ItemSpacing.y));
    }
    ImGui::Dummy(ImVec2(1, kit_px(4)));

    // Share: the title row says what it comes out as, with the button beside it; the size chips under it
    {
        f64 src_kbps = m.duration > 0 ? ed.item.size * 8 / 1000.0 / m.duration : 0;
        SharePlan plan = share_plan(len, ed_share_mb(), m.track_count > 0, m.width, m.height, m.fps, src_kbps);
        char est[96];
        if (plan.fits) {
            char size[24];
            fmt_size((u64)plan.estimate_bytes, size, sizeof size);
            snprintf(est, sizeof est, "About %s, %dp%.0f, %.1f Mbps", size, plan.side, plan.fps, plan.video_kbps / 1000.0);
        } else {
            char most[32];
            fmt_time(plan.max_duration, most, sizeof most);
            snprintf(est, sizeof est, "Too long: trim to under %s", most);
        }
        ImVec2 top = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(ImVec2(top.x, top.y + (keep_row - head_h) / 2));
        ed_card_head(ICON_SHARE, "Share", est, iw - bw - gap, plan.fits ? 0 : col32(0xff8a92));
        ImGui::SetCursorScreenPos(ImVec2(top.x + iw - bw, top.y + (keep_row - fh) / 2));
        if (kit_button(sharing ? "Sharing..." : ICON_SHARE "  Share", KIT_PRIMARY, ImVec2(bw, 0), !sharing && plan.fits))
            ed_start(SAVE_SHARE);
        ImGui::SetCursorScreenPos(ImVec2(top.x, top.y + keep_row + st.ItemSpacing.y));

        static const char *const sizes[] = {"20 MB", "50 MB", "Custom"};
        // The size field keeps its width beside the chips, greyed with the preset's size until Custom.
        f32 mb_w = kit_px(84);
        kit_segmented("##size", sizes, 3, &ed.share_size, (iw - mb_w - st.ItemSpacing.x - kit_px(6)) / 3);
        ImGui::SameLine();
        if (ed.custom_mb <= 0) ed.custom_mb = 25;
        ImGui::SetNextItemWidth(mb_w);
        if (ed.share_size == 2) ImGui::InputFloat("##mb", &ed.custom_mb, 0, 0, "%.0f MB");
        else {
            f32 shown = ed_share_mb();
            ImGui::BeginDisabled();
            ImGui::InputFloat("##mb", &shown, 0, 0, "%.0f MB");
            ImGui::EndDisabled();
        }
        if (ed.custom_mb < 1) ed.custom_mb = 1;
        if (ed.custom_mb > 4000) ed.custom_mb = 4000;
    }
    ImGui::Dummy(ImVec2(1, kit_px(4)));

    // Progress, or the last result with its file's actions: always one row, in its place.
    SaveJobView *v = save_view(sharing ? ed.share_job : keeping ? ed.keep_job : 0);
    ImVec2 a = ImGui::GetCursorScreenPos();
    f32 line_h = ImGui::GetTextLineHeight(), bar_h = kit_px(4);
    f32 ty = floorf(a.y + (fh - line_h) / 2);
    if (v) {
        char label[64], pc[16];
        f32 frac = v->progress;
        snprintf(label, sizeof label, "%s%s", v->kind == SAVE_SHARE ? "Sharing" : "Keeping", v->state == SAVE_QUEUED ? ", waiting" : "");
        snprintf(pc, sizeof pc, "%.0f%%", frac * 100);
        dl->AddText(ImVec2(a.x, a.y), col32(COL_DIM), label);
        dl->AddText(ImVec2(a.x + w - ImGui::CalcTextSize(pc).x, a.y), col32(COL_DIM), pc);
        f32 by = a.y + line_h + kit_px(4);
        dl->AddRectFilled(ImVec2(a.x, by), ImVec2(a.x + w, by + bar_h), col32(COL_PANEL2), bar_h);
        if (frac > 0) dl->AddRectFilled(ImVec2(a.x, by), ImVec2(a.x + w * (frac > 1 ? 1 : frac), by + bar_h), col32(COL_ACCENT), bar_h);
        ImGui::Dummy(ImVec2(w, fh));
    } else if (ed.result[0]) {
        bool file = ed.result_path[0] && !ed.result_error;
        f32 text_w = file ? w - 3 * fh - kit_px(12) : w;
        text_clipped(dl, ImVec2(a.x, ty), text_w, col32(ed.result_error ? 0xff8a92 : COL_OK), ed.result);
        ImGui::PushID("result");
        if (file) ed_file_actions(ed.result_path, a.x + w, a.y);
        else ImGui::Dummy(ImVec2(w, fh));
        ImGui::PopID();
        ImGui::SetCursorScreenPos(ImVec2(a.x, a.y + fh + st.ItemSpacing.y));
    } else {
        ImGui::Dummy(ImVec2(w, fh));
    }
    ImGui::Dummy(ImVec2(1, kit_px(4)));

    // Recent shares, as many to a page as fit: name, age and size, then the file's actions
    f32 row_step = fh + st.ItemSpacing.y;
    i32 rows = (i32)((ImGui::GetContentRegionAvail().y - row_step - kit_px(4)) / row_step);
    rows = rows < 1 ? 1 : rows > 8 ? 8 : rows;
    if (ed.share_page * rows >= ed.share_count && ed.share_page > 0) ed.share_page = (ed.share_count - 1) / rows;
    i32 pages = ed.share_count ? (ed.share_count + rows - 1) / rows : 1;
    ed_group_head("RECENT SHARES", "shares", &ed.share_page, pages, w);
    for (i32 r = 0; r < rows; r++) {
        i32 i = ed.share_page * rows + r;
        ImGui::PushID(200 + r);
        ImVec2 at = ImGui::GetCursorScreenPos();
        if (i < ed.share_count) {
            const ShareFile &f = ed.shares[i];
            char age[48], size[24], line[96];
            i64 mins = (now_us() - f.mtime_us) / 60000000;
            if (mins < 1) str_set(age, "now");
            else if (mins < 60) snprintf(age, sizeof age, "%lldm", mins);
            else if (mins < 48 * 60) snprintf(age, sizeof age, "%lldh", mins / 60);
            else snprintf(age, sizeof age, "%lldd", mins / 1440);
            fmt_size(f.size, size, sizeof size);
            snprintf(line, sizeof line, "%s - %s", size, age);
            f32 text_w = w - 3 * fh - kit_px(12);
            ImVec2 ls = ImGui::CalcTextSize(line);
            f32 ry = floorf(at.y + (fh - ls.y) / 2);
            text_clipped(dl, ImVec2(at.x, ry), text_w - ls.x - kit_px(8), col32(COL_TEXT), f.name);
            dl->AddText(ImVec2(at.x + text_w - ls.x, ry), col32(COL_DIM), line);
            wchar_t name[200];
            utf8_to_wide(f.name, name, COUNT(name));
            Path pth = path_join(g_app.paths.share_dir, name);
            char u[MAX_PATH * 2];
            wide_to_utf8(pth.s, u, sizeof u);
            ed_file_actions(u, at.x + w, at.y);
            ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + fh + st.ItemSpacing.y));
        } else {
            if (i == 0) {
                char none[64];
                snprintf(none, sizeof none, "Nothing in the last %g days", g_app.config.share_keep_days);
                ImVec2 ts = ImGui::CalcTextSize(none);
                dl->AddText(ImVec2(at.x, floorf(at.y + (fh - ts.y) / 2)), col32(COL_FAINT), none);
            }
            ImGui::Dummy(ImVec2(w, fh));
        }
        ImGui::PopID();
    }
}

// Where Left or Right goes from `at`: the nearest key on a lane shown, or end of the trim, that way.
static f64 ed_next_stop(f64 at, i32 dir)
{
    Editor &ed = g_app.editor;
    f64 best = dir > 0 ? ed_duration() : 0, eps = ed_frame() * 0.5;
    auto take = [&](f64 t) {
        if (dir > 0 ? t > at + eps && t < best : t < at - eps && t > best) best = t;
    };
    take(ed.edit.in_s);
    take(ed_out());
    for (i32 l = 0; l < ED_LANES; l++) {
        if (!ed_lane_shown(l)) continue;
        for (i32 k = 0; k < ln_count(ed.edit, l); k++) take(ln_get(ed.edit, l, k).t);
    }
    return best;
}

static void screen_editor()
{
    Editor &ed = g_app.editor;
    Player &p = ed.player;
    ImGuiIO &io = ImGui::GetIO();
    ed_check_pending();
    if (!ed.open && ed.pending[0]) {
        kit_dim("Opening the saved file...");
        ui_wake(10); // look again soon: the scan posts its results, but probing can lag them
        return;
    }
    if (!ed.open) {
        g_app.ui.screen = SCREEN_LIBRARY;
        return;
    }
    if (!p.open) { // closed while the window was hidden
        p.id = 1;
        ed.tracks_ready = false;
        player_open(&p, ed.path, ed.edit.in_s, false);
    }
    if (p.loaded && !ed.tracks_ready) {
        i32 tracks[MEDIA_MAX_TRACKS];
        i32 n = ed_tracks(ed.item.info, tracks, PLAYER_MAX_TRACK_ENGINES);
        if (n > 1 && p.audio_count > 1) player_set_tracks(&p, tracks, n);
        ed.heard_set = false;
        ed_apply_volumes();
        ed.tracks_ready = true;
    }
    p.loop_from = ed.edit.in_s;
    p.stop_at = ed_out();
    player_update(&p);
    // Every half second everything is handed over again, for a track engine that's only just loaded.
    static u64 heard_at;
    if (GetTickCount64() - heard_at > 500) { heard_at = GetTickCount64(); ed.heard_set = false; }
    if (ed.tracks_ready) ed_apply_volumes();
    // While the waveforms are measured, what's on screen goes first (each chunk measured wakes the window).
    if (ed.wave && !ed.wave->state) wave_view(ed.wave, ed.view_from, ed.view_from + ed.view_span);

    ImDrawList *dl = ImGui::GetWindowDrawList();
    f32 fh = ImGui::GetFrameHeight();
    ImVec2 avail = ImGui::GetContentRegionAvail();
    Config &cfg = g_app.config;

    // Header: back, the name, and the game and when under it.
    ImVec2 h0 = ImGui::GetCursorScreenPos();
    f32 head_h = fmaxf(fh, g_font_big->FontSize + g_font_small->FontSize + kit_px(4));
    ImGui::SetCursorScreenPos(ImVec2(h0.x, h0.y + (head_h - fh) / 2));
    kit_next_tip("Back to the library", "Esc");
    bool back = kit_icon_button(ICON_BACK, "##back", KIT_GHOST);
    f32 tx = h0.x + fh + kit_px(14), tw = avail.x - (tx - h0.x);
    f32 ty = h0.y + (head_h - g_font_big->FontSize - g_font_small->FontSize - kit_px(4)) / 2;
    ImGui::PushFont(g_font_big);
    text_clipped(dl, ImVec2(tx, ty), tw, col32(COL_TEXT), ed.item.name);
    ImGui::PopFont();
    char when[48], sub[160];
    fmt_when(ed.item.mtime_us, when, sizeof when);
    if (ed.item.info.game[0]) snprintf(sub, sizeof sub, "Editing  -  %s  -  %s", ed.item.info.game, when);
    else snprintf(sub, sizeof sub, "Editing  -  %s", when);
    ImGui::PushFont(g_font_small);
    text_clipped(dl, ImVec2(tx, ty + g_font_big->FontSize + kit_px(4)), tw, col32(COL_DIM), sub);
    ImGui::PopFont();

    // Body: the picture with its transport bar and the side panel on top; the timeline across the bottom. The
    // edge between them is dragged (double-click puts it back).
    f32 body_y = h0.y + head_h + kit_px(14), body_h = h0.y + avail.y - body_y;
    f32 split = kit_px(14);
    auto top_height = [&]() {
        f32 t = body_h * (cfg.editor_top > 0 ? cfg.editor_top : 0.56f);
        f32 lo = kit_px(240), hi = body_h - split - kit_px(200);
        if (t > hi) t = hi;
        if (t < lo) t = lo;
        if (t > body_h - split) t = body_h - split;
        return t > 1 ? t : 1;
    };
    f32 top_h = top_height();
    // The side panel's width: the edge between it and the picture is dragged too (double-click puts it back).
    f32 gap = kit_px(20);
    auto side_width = [&]() {
        f32 sw = cfg.editor_side > 0 ? kit_px(cfg.editor_side) : kit_px(372);
        f32 lo = kit_px(300), hi = avail.x - gap - kit_px(420);
        if (sw > hi) sw = hi;
        if (sw < lo) sw = lo;
        return sw;
    };
    f32 side_w = side_width(), main_w = avail.x - side_w - gap;
    f32 bar_h = fh + kit_px(16);
    f32 video_h = top_h - bar_h - kit_px(10);
    if (video_h < 1) video_h = 1;

    // The picture, as it will be rendered (zoomed at the playhead), on black.
    ImVec2 pos(h0.x, body_y), a, b;
    dl->AddRectFilled(pos, ImVec2(pos.x + main_w, pos.y + video_h), IM_COL32(0, 0, 0, 255), kit_px(10));
    player_fit(p, pos, ImVec2(main_w, video_h), &a, &b);
    ImGui::SetCursorScreenPos(a);
    ImGui::InvisibleButton("picture", ImVec2(b.x - a.x > 1 ? b.x - a.x : 1, b.y - a.y > 1 ? b.y - a.y : 1));
    f64 z, cx, cy;
    zoom_at(ed.edit, p.pos, &z, &cx, &cy);
    if (ImGui::IsItemActivated()) {
        ed.aiming = z > 1.001;
        if (ed.aiming) {
            ed.aim_key = ed_add_key(p.pos);
            ed.aim_mx = io.MousePos.x;
            ed.aim_my = io.MousePos.y;
            ed.aim_kx = cx;
            ed.aim_ky = cy;
            if (ed.aim_key < 0) ed.aiming = false;
        }
    }
    if (ImGui::IsItemActive() && ed.aiming && ed.aim_key < ed.edit.key_count) {
        ZoomKey &k = ed.edit.keys[ed.aim_key];
        f64 nx = ed.aim_kx - (io.MousePos.x - ed.aim_mx) / (b.x - a.x) / k.z;
        f64 ny = ed.aim_ky - (io.MousePos.y - ed.aim_my) / (b.y - a.y) / k.z;
        k.x = nx < 0 ? 0 : nx > 1 ? 1 : nx;
        k.y = ny < 0 ? 0 : ny > 1 ? 1 : ny;
        ed.dirty = true;
    } else if (ImGui::IsItemDeactivated()) {
        if (!ed.aiming) player_toggle(&p);
        ed.aiming = false;
    }
    ImVec2 uv0, uv1;
    zoom_uv(ed.edit, p.pos, &uv0, &uv1);
    player_draw(p, a, b, uv0, uv1); // nothing drawn over it: an editor needs to see the whole picture
    if (z > 1.001) {
        char zt[32];
        snprintf(zt, sizeof zt, "%.1fx", z);
        badge(dl, ImVec2(a.x + kit_px(10), a.y + kit_px(10)), false, false, zt);
    }

    // The transport bar: play, the time, the trim buttons, and how much is kept on the right. Fixed widths.
    ImVec2 r0(pos.x, pos.y + video_h + kit_px(10));
    dl->AddRectFilled(r0, ImVec2(r0.x + main_w, r0.y + bar_h), col32(COL_PANEL), kit_px(10));
    f32 pad = kit_px(8), y = r0.y + (bar_h - fh) / 2;
    ImGui::SetCursorScreenPos(ImVec2(r0.x + pad, y));
    kit_next_tip(p.paused ? "Play" : "Pause", "Space");
    if (kit_icon_button(p.paused ? ICON_PLAY : ICON_PAUSE, "##play", KIT_GHOST)) player_toggle(&p);
    char t1[32], t2[32], tm[80];
    fmt_time(p.pos, t1, sizeof t1, true);
    fmt_time(ed_duration(), t2, sizeof t2, true);
    snprintf(tm, sizeof tm, "%s / %s", t1, t2);
    f32 time_x = r0.x + pad + fh + kit_px(8);
    ImVec2 ts = ImGui::CalcTextSize(tm);
    dl->AddText(ImVec2(time_x, floorf(y + (fh - ts.y) / 2)), col32(COL_DIM), tm);
    static const char *const trims[] = {ICON_START "  Set start", ICON_NOW "  Set end", ICON_UNDO "  Reset"};
    static const char *const trim_icons[] = {ICON_START "##start", ICON_NOW "##end", ICON_UNDO "##reset"};
    // Narrow, the trim buttons drop their words; narrower still, the kept length goes. Decided by the width alone
    // (the widest the texts get), so nothing moves as the time or the trim changes.
    f32 pad_x = 2 * ImGui::GetStyle().FramePadding.x, words_w = 0;
    for (i32 i = 0; i < 3; i++) words_w += ImGui::CalcTextSize(trims[i]).x + pad_x + kit_px(4);
    f32 kept_most = ImGui::CalcTextSize("00:00:00 kept").x + kit_px(24), bar_end = r0.x + main_w;
    f32 bx = time_x + kit_px(170);
    bool words = bx + words_w + kept_most <= bar_end;
    bool show_kept = bx + (words ? words_w : 3 * (fh + kit_px(4))) + kept_most <= bar_end;
    for (i32 i = 0; i < 3; i++) {
        const char *label = words ? trims[i] : trim_icons[i];
        f32 w = words ? ImGui::CalcTextSize(trims[i]).x + pad_x : fh;
        ImGui::SetCursorScreenPos(ImVec2(bx, y));
        bool enabled = i < 2 || ed.edit.in_s > 0 || ed.edit.out_s > 0;
        if (kit_button(label, KIT_GHOST, ImVec2(w, 0), enabled)) {
            if (i == 0 && p.pos < ed_out() - 0.2) { ed.edit.in_s = p.pos; ed.dirty = true; }
            if (i == 1 && p.pos > ed.edit.in_s + 0.2) { ed.edit.out_s = p.pos >= ed_duration() - 0.01 ? 0 : p.pos; ed.dirty = true; }
            if (i == 2) { ed.edit.in_s = ed.edit.out_s = 0; ed.dirty = true; }
        }
        bx += w + kit_px(4);
    }
    char kept[32], sel[48];
    fmt_time(ed_out() - ed.edit.in_s, kept, sizeof kept, true);
    snprintf(sel, sizeof sel, "%s kept", kept);
    ImVec2 ks = ImGui::CalcTextSize(sel);
    f32 kx = r0.x + main_w - kit_px(16) - ks.x;
    if (show_kept) dl->AddText(ImVec2(kx, floorf(y + (fh - ks.y) / 2)), col32(COL_TEXT), sel);

    // The side panel: Edit, or Keep & share.
    ImGui::SetCursorScreenPos(ImVec2(pos.x + main_w + gap, body_y));
    if (kit_card_begin("ed_side", ImVec2(side_w, top_h))) {
        f32 cw = ImGui::GetContentRegionAvail().x;
        static const char *const tabs[] = {"Edit", "Keep & share"};
        kit_segmented("##tab", tabs, 2, &ed.tab, (cw - kit_px(6)) / 2);
        ImGui::Dummy(ImVec2(1, kit_px(6)));
        if (ed.tab == 0) ed_edit_tab(cw);
        else ed_share_tab(cw);
    }
    kit_card_end();

    // The edge between the picture and the side panel, in the gap between them.
    {
        static f32 grab_dx;
        f32 sx = pos.x + main_w;
        ImGui::SetCursorScreenPos(ImVec2(sx, body_y));
        ImGui::InvisibleButton("##split_side", ImVec2(gap, top_h));
        bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
        if (ImGui::IsItemActivated()) grab_dx = io.MousePos.x - sx;
        if (ImGui::IsItemActive() && ImGui::IsMousePosValid()) {
            cfg.editor_side = (h0.x + avail.x - (io.MousePos.x - grab_dx) - gap) / kit_px(1);
            if (cfg.editor_side < 1) cfg.editor_side = 1;
            cfg.editor_side = side_width() / kit_px(1); // where it stopped, so a drag past the end comes straight back
        }
        if (hot && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) cfg.editor_side = 0;
        if (ImGui::IsItemDeactivated()) config_save(cfg, g_app.paths);
        if (hot) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        f32 s = kit_px(1), cx2 = floorf(sx + gap / 2), mid = body_y + top_h / 2;
        dl->AddRectFilled(ImVec2(cx2 - 1.5f * s, mid - 22 * s), ImVec2(cx2 + 1.5f * s, mid + 22 * s), col32(hot ? COL_DIM : COL_PANEL2), 2 * s);
    }

    // The edge between the top and the timeline.
    {
        static f32 grab_dy;
        f32 sy = body_y + top_h;
        ImGui::SetCursorScreenPos(ImVec2(h0.x, sy));
        ImGui::InvisibleButton("##split_editor", ImVec2(avail.x, split));
        bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
        if (ImGui::IsItemActivated()) grab_dy = io.MousePos.y - sy;
        if (ImGui::IsItemActive() && ImGui::IsMousePosValid() && body_h > 0) {
            cfg.editor_top = (io.MousePos.y - grab_dy - body_y) / body_h;
            if (cfg.editor_top < 0.01f) cfg.editor_top = 0.01f;
            top_h = top_height();
            cfg.editor_top = top_h / body_h; // where it stopped, so a drag past the end comes straight back
        }
        if (hot && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) cfg.editor_top = 0;
        if (ImGui::IsItemDeactivated()) config_save(cfg, g_app.paths);
        if (hot) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        f32 s = kit_px(1), mid = h0.x + avail.x / 2, cy2 = floorf(sy + split / 2);
        dl->AddRectFilled(ImVec2(mid - 22 * s, cy2 - 1.5f * s), ImVec2(mid + 22 * s, cy2 + 1.5f * s), col32(hot ? COL_DIM : COL_PANEL2), 2 * s);
    }

    // The timeline, across the whole width.
    f32 tl_y = body_y + top_h + split;
    ed_timeline(ImVec2(h0.x, tl_y), avail.x, body_y + body_h - tl_y);

    // Keys (never while typing or with a menu open).
    if (!io.WantTextInput && !ImGui::IsPopupOpen((const char *)0, ImGuiPopupFlags_AnyPopupId)) {
        bool ctrl = io.KeyCtrl;
        if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) player_toggle(&p);
        if (!ctrl && ImGui::IsKeyPressed(ImGuiKey_I, false) && p.pos < ed_out() - 0.2) { ed.edit.in_s = p.pos; ed.dirty = true; }
        if (!ctrl && ImGui::IsKeyPressed(ImGuiKey_O, false) && p.pos > ed.edit.in_s + 0.2) {
            ed.edit.out_s = p.pos >= ed_duration() - 0.01 ? 0 : p.pos;
            ed.dirty = true;
        }
        if (!ctrl && ImGui::IsKeyPressed(ImGuiKey_K, false)) ed_add_key(p.pos);
        if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false)) {
            if (ed_sel_count()) ed_delete_selected();
            else ed_delete_key(ed_key_at(p.pos));
        }
        for (i32 i = 0; i < EASE_COUNT; i++)
            if (!ctrl && ImGui::IsKeyPressed((ImGuiKey)(ImGuiKey_1 + i), false) && ed_sel_count()) ed_set_ease(i);
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_A, false)) ed_select_all();
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_C, false)) ed_copy();
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_V, false)) ed_paste(p.pos);
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) ed_undo(io.KeyShift);
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) ed_undo(true);
        if (!ctrl && (ImGui::IsKeyPressed(ImGuiKey_Equal) || ImGui::IsKeyPressed(ImGuiKey_KeypadAdd))) ed_view_zoom(1 / 1.6, p.pos);
        if (!ctrl && (ImGui::IsKeyPressed(ImGuiKey_Minus) || ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract))) ed_view_zoom(1.6, p.pos);
        // Left and Right jump between keys and the trim's ends (a second with Shift); comma and period step a frame
        for (i32 dir = -1; dir <= 1; dir += 2) {
            if (ImGui::IsKeyPressed(dir < 0 ? ImGuiKey_LeftArrow : ImGuiKey_RightArrow)) {
                player_pause(&p);
                player_seek(&p, io.KeyShift ? p.pos + dir : ed_next_stop(p.pos, dir));
            }
            if (!ctrl && ImGui::IsKeyPressed(dir < 0 ? ImGuiKey_Comma : ImGuiKey_Period)) {
                player_pause(&p);
                player_seek(&p, p.pos + dir * ed_frame());
            }
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            if (ed_sel_count()) ed_sel_clear();
            else back = true;
        }
    }
    // A change settles once nothing is held: it goes on the undo list and the record is written.
    if (!ImGui::IsAnyItemActive() && ed.lane_drag == ED_DRAG_NONE) {
        ed_settle();
        if (ed.dirty) ed_save_record();
    }
    if (back) {
        editor_close();
        g_app.ui.screen = SCREEN_LIBRARY;
    }
}
