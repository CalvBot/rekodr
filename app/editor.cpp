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
            i32 ease = EASE_SMOOTH; // keys from before eases were all smooth
            if (sscanf_s(v, "%lf %lf %lf %lf %d", &z.t, &z.z, &z.x, &z.y, &ease) >= 4) {
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

// The key at the playhead (within half a frame), or -1.
static i32 ed_key_at(f64 t)
{
    Editor &ed = g_app.editor;
    f64 tol = ed_frame() / 2 + 0.01;
    for (i32 k = 0; k < ed.edit.key_count; k++)
        if (ed.edit.keys[k].t > t - tol && ed.edit.keys[k].t < t + tol) return k;
    return -1;
}

// A key at `t` holding the zoom as it is there now. Returns its index, or -1 if there's no room.
static i32 ed_add_key(f64 t)
{
    Editor &ed = g_app.editor;
    i32 k = ed_key_at(t);
    if (k >= 0) return k;
    if (ed.edit.key_count == EDIT_MAX_KEYS) return -1;
    ZoomKey z = {};
    z.t = t;
    z.ease = EASE_SMOOTH;
    zoom_at(ed.edit, t, &z.z, &z.x, &z.y);
    i32 at = 0;
    while (at < ed.edit.key_count && ed.edit.keys[at].t < t) at++;
    memmove(ed.edit.keys + at + 1, ed.edit.keys + at, (ed.edit.key_count - at) * sizeof z);
    ed.edit.keys[at] = z;
    ed.edit.key_count++;
    ed.dirty = true;
    return at;
}

static void ed_delete_key(i32 k)
{
    Editor &ed = g_app.editor;
    if (k < 0 || k >= ed.edit.key_count) return;
    memmove(ed.edit.keys + k, ed.edit.keys + k + 1, (ed.edit.key_count - k - 1) * sizeof ed.edit.keys[0]);
    ed.edit.key_count--;
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
    ed.key_page = ed.track_page = 0;
    ed.drag = 0;
    ed.aiming = false;
    ed.result[0] = 0;
    ed.result_error = false;
    ed.player.id = 1;
    player_open(&ed.player, ed.path, ed.edit.in_s, false);
    editor_refresh_shares();
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
    if (e.a == SAVE_EV_DONE) {
        if (share) {
            wchar_t w[MAX_PATH];
            utf8_to_wide(e.text, w, MAX_PATH);
            WIN32_FILE_ATTRIBUTE_DATA fa = {};
            GetFileAttributesExW(w, GetFileExInfoStandard, &fa);
            char size[24];
            fmt_size(((u64)fa.nFileSizeHigh << 32) | fa.nFileSizeLow, size, sizeof size);
            bool copied = clipboard_copy_file(w);
            snprintf(ed.result, sizeof ed.result, copied ? "Shared (%s): on the clipboard, ready to paste." :
                                                           "Shared (%s), but the clipboard was busy: use Copy below.", size);
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
        snprintf(j.stem, sizeof j.stem, "%s edit", ed.item.name);
    } else {
        j.out_dir = g_app.paths.share_dir;
        snprintf(j.stem, sizeof j.stem, "%s", ed.item.name);
        j.share_mb = ed.share_size == 0 ? 50.f : ed.share_size == 1 ? 10.f : ed.custom_mb;
        if (j.share_mb < 1) j.share_mb = 1;
    }
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

static void diamond(ImDrawList *dl, ImVec2 c, f32 r, u32 col)
{
    dl->AddQuadFilled(ImVec2(c.x, c.y - r), ImVec2(c.x + r, c.y), ImVec2(c.x, c.y + r), ImVec2(c.x - r, c.y), col);
}

// The timeline: the strip across the whole file, dimmed outside the trim, with accent handles for the trim, the
// playhead and the zoom keys.
static void ed_timeline(f32 w, f32 h)
{
    Editor &ed = g_app.editor;
    Library &lib = g_app.library;
    Player &p = ed.player;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + w, a.y + h);
    f64 dur = ed_duration();
    ImGui::InvisibleButton("timeline", ImVec2(w, h));
    bool active = ImGui::IsItemActive();
    f32 mx = ImGui::GetIO().MousePos.x;
    f64 mt = dur > 0 ? (f64)((mx - a.x) / w) * dur : 0;
    mt = mt < 0 ? 0 : mt > dur ? dur : mt;
    auto x_of = [&](f64 t) { return a.x + (dur > 0 ? (f32)(t / dur) * w : 0); };
    f32 in_x = x_of(ed.edit.in_s), out_x = x_of(ed_out());
    if (ImGui::IsItemActivated()) {
        f32 grab = kit_px(10);
        if (fabsf(mx - out_x) < grab) ed.drag = 2;
        else if (fabsf(mx - in_x) < grab) ed.drag = 1;
        else ed.drag = 3;
    }
    if (active && dur > 0) {
        f64 minlen = 0.2;
        if (ed.drag == 1) {
            f64 t = mt < ed_out() - minlen ? mt : ed_out() - minlen;
            ed.edit.in_s = t < 0 ? 0 : t;
            ed.dirty = true;
            p.scrubbing = true;
            player_seek(&p, ed.edit.in_s);
        } else if (ed.drag == 2) {
            f64 t = mt > ed.edit.in_s + minlen ? mt : ed.edit.in_s + minlen;
            ed.edit.out_s = t >= dur - 0.01 ? 0 : t;
            ed.dirty = true;
            p.scrubbing = true;
            player_seek(&p, t);
        } else if (ed.drag == 3) {
            p.scrubbing = true;
            if (mt < p.pos - 0.02 || mt > p.pos + 0.02) player_seek(&p, mt);
        }
    } else if (ed.drag) {
        ed.drag = 0;
        p.scrubbing = false;
        player_seek(&p, p.pos);
    }

    f32 round = kit_px(8);
    dl->AddRectFilled(a, b, col32(COL_PANEL2), round);
    if (lib_want_strip(ed.item))
        dl->AddImageRounded((ImTextureID)(intptr_t)lib.strip, a, b, ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, round);
    in_x = x_of(ed.edit.in_s);
    out_x = x_of(ed_out());
    // Outside the trim, darkened.
    if (in_x > a.x) dl->AddRectFilled(a, ImVec2(in_x, b.y), IM_COL32(0, 0, 0, 185), round, ImDrawFlags_RoundCornersLeft);
    if (out_x < b.x) dl->AddRectFilled(ImVec2(out_x, a.y), b, IM_COL32(0, 0, 0, 185), round, ImDrawFlags_RoundCornersRight);
    // The kept part: an accent frame with a grip at each end.
    u32 acc = col32(ed.drag == 1 || ed.drag == 2 ? COL_ACCENT_HOVER : COL_ACCENT);
    f32 hw = kit_px(7), bw = kit_px(2);
    dl->AddRectFilled(ImVec2(in_x, a.y), ImVec2(out_x, a.y + bw), acc);
    dl->AddRectFilled(ImVec2(in_x, b.y - bw), ImVec2(out_x, b.y), acc);
    dl->AddRectFilled(ImVec2(in_x, a.y), ImVec2(in_x + hw, b.y), acc, kit_px(4), ImDrawFlags_RoundCornersLeft);
    dl->AddRectFilled(ImVec2(out_x - hw, a.y), ImVec2(out_x, b.y), acc, kit_px(4), ImDrawFlags_RoundCornersRight);
    f32 gh = kit_px(8), cy = (a.y + b.y) / 2;
    dl->AddRectFilled(ImVec2(in_x + hw / 2 - kit_px(1), cy - gh), ImVec2(in_x + hw / 2 + kit_px(1), cy + gh), IM_COL32(255, 255, 255, 200));
    dl->AddRectFilled(ImVec2(out_x - hw / 2 - kit_px(1), cy - gh), ImVec2(out_x - hw / 2 + kit_px(1), cy + gh), IM_COL32(255, 255, 255, 200));
    // Zoom keys along the top, kept inside the strip so a key at the very start or end is still seen whole.
    for (i32 k = 0; k < ed.edit.key_count; k++) {
        f32 kx = x_of(ed.edit.keys[k].t), r = kit_px(6);
        kx = kx < a.x + r ? a.x + r : kx > b.x - r ? b.x - r : kx;
        diamond(dl, ImVec2(kx, a.y + kit_px(11)), r + kit_px(1.5f), IM_COL32(0, 0, 0, 160));
        diamond(dl, ImVec2(kx, a.y + kit_px(11)), r, col32(COL_BLUE));
    }
    // The playhead: a white line with a knob above the strip.
    f32 px = x_of(p.pos);
    dl->AddRectFilled(ImVec2(px - kit_px(1), a.y - kit_px(4)), ImVec2(px + kit_px(1), b.y + kit_px(4)), IM_COL32_WHITE);
    dl->AddCircleFilled(ImVec2(px, a.y - kit_px(4)), kit_px(5), IM_COL32_WHITE);
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

static void ed_edit_tab(f32 w)
{
    Editor &ed = g_app.editor;
    Player &p = ed.player;
    const ImGuiStyle &st = ImGui::GetStyle();
    f32 fh = ImGui::GetFrameHeight(), sp = st.ItemSpacing.x;
    ImDrawList *dl = ImGui::GetWindowDrawList();

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

    // Keyframe actions: one filled, two quiet.
    i32 here = ed_key_at(p.pos);
    f32 bw = (w - 2 * sp) / 3;
    if (kit_button(ICON_ADD "  Keyframe", KIT_SECONDARY, ImVec2(bw, 0), here < 0 && ed.edit.key_count < EDIT_MAX_KEYS))
        ed_add_key(p.pos);
    ImGui::SameLine();
    if (kit_button(ICON_DELETE "  Delete", KIT_GHOST, ImVec2(bw, 0), here >= 0)) ed_delete_key(here);
    ImGui::SameLine();
    if (kit_button(ICON_DELETE "  Clear all", KIT_GHOST, ImVec2(bw, 0), ed.edit.key_count > 0)) {
        ed.edit.key_count = 0;
        ed.dirty = true;
    }
    ImGui::Dummy(ImVec2(1, kit_px(4)));

    // The keys, three to a page.
    const i32 key_rows = 3;
    i32 key_pages = ed.edit.key_count ? (ed.edit.key_count + key_rows - 1) / key_rows : 1;
    ed_group_head("KEYFRAMES", "keys", &ed.key_page, key_pages, w);
    for (i32 r = 0; r < key_rows; r++) {
        i32 k = ed.key_page * key_rows + r;
        ImGui::PushID(r);
        ImVec2 a = ImGui::GetCursorScreenPos();
        f32 row_w = w - fh - kit_px(4);
        if (k < ed.edit.key_count) {
            const ZoomKey &key = ed.edit.keys[k];
            if (ImGui::InvisibleButton("##key", ImVec2(row_w, fh))) player_seek(&p, key.t);
            bool hot = ImGui::IsItemHovered();
            if (k == here || hot) dl->AddRectFilled(a, ImVec2(a.x + row_w, a.y + fh), col32(k == here ? COL_HOVER : COL_PANEL2), st.FrameRounding);
            diamond(dl, ImVec2(a.x + kit_px(14), a.y + fh / 2), kit_px(5), col32(COL_BLUE));
            char t[32], zl[16];
            fmt_time(key.t, t, sizeof t, true);
            snprintf(zl, sizeof zl, "%.1fx", key.z);
            ImVec2 ts = ImGui::CalcTextSize(t), zs2 = ImGui::CalcTextSize(zl);
            f32 ty = floorf(a.y + (fh - ts.y) / 2);
            dl->AddText(ImVec2(a.x + kit_px(30), ty), col32(COL_TEXT), t);
            dl->AddText(ImVec2(a.x + row_w - kit_px(10) - zs2.x, ty), col32(COL_DIM), zl);
            ImGui::SameLine(0, kit_px(4));
            if (kit_icon_button(ICON_DELETE, "##del", KIT_GHOST)) ed_delete_key(k);
        } else {
            if (k == 0) {
                ImVec2 ts = ImGui::CalcTextSize("No keyframes");
                dl->AddText(ImVec2(a.x + kit_px(10), floorf(a.y + (fh - ts.y) / 2)), col32(COL_FAINT), "No keyframes");
            }
            ImGui::Dummy(ImVec2(w, fh));
        }
        ImGui::PopID();
    }
    ImGui::Dummy(ImVec2(1, kit_px(6)));

    // Sound: a mute button, the track's name, its volume.
    i32 tracks[MEDIA_MAX_TRACKS];
    i32 n = ed_tracks(ed.item.info, tracks, MEDIA_MAX_TRACKS);
    const i32 track_rows = 5;
    i32 track_pages = n ? (n + track_rows - 1) / track_rows : 1;
    ed_group_head("SOUND", "tracks", &ed.track_page, track_pages, w);
    f32 name_w = kit_px(104), pct_w = kit_px(44), slider_w = w - fh - kit_px(6) - name_w - pct_w;
    bool changed = false;
    for (i32 r = 0; r < track_rows; r++) {
        i32 i = ed.track_page * track_rows + r;
        ImGui::PushID(100 + r);
        ImVec2 a = ImGui::GetCursorScreenPos();
        if (i < n) {
            i32 t = tracks[i];
            bool muted = ed.edit.muted[t];
            if (kit_icon_button(muted ? ICON_MUTE : ICON_VOLUME, "##mute", KIT_GHOST, true, muted)) {
                ed.edit.muted[t] = !muted;
                changed = true;
            }
            f32 nx = a.x + fh + kit_px(6);
            ImVec2 ts = ImGui::CalcTextSize("A");
            text_clipped(dl, ImVec2(nx, floorf(a.y + (fh - ts.y) / 2)), name_w - kit_px(8), col32(muted ? COL_FAINT : COL_TEXT),
                         ed.item.info.tracks[t]);
            ImGui::SetCursorScreenPos(ImVec2(nx + name_w, a.y));
            f32 pct = (1 + ed.edit.boost[t]) * 100;
            if (lib_slider("##vol", &pct, 0, 200, slider_w, fh, 100, !muted)) {
                ed.edit.boost[t] = pct / 100 - 1;
                changed = true;
            }
            if (!muted && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                ed.edit.boost[t] = 0;
                pct = 100;
                changed = true;
            }
            char pt[16];
            snprintf(pt, sizeof pt, "%.0f%%", pct);
            ImVec2 ps = ImGui::CalcTextSize(pt);
            dl->AddText(ImVec2(a.x + w - ps.x, floorf(a.y + (fh - ps.y) / 2)), col32(muted ? COL_FAINT : COL_DIM), pt);
            ImGui::SetCursorScreenPos(a);
            ImGui::Dummy(ImVec2(w, fh));
        } else {
            if (i == 0) {
                ImVec2 ts = ImGui::CalcTextSize("No sound");
                dl->AddText(ImVec2(a.x + kit_px(10), floorf(a.y + (fh - ts.y) / 2)), col32(COL_FAINT), "No sound");
            }
            ImGui::Dummy(ImVec2(w, fh));
        }
        ImGui::PopID();
    }
    if (changed) {
        ed.dirty = true;
        ed_apply_volumes();
    }
}

static void ed_share_tab(f32 w)
{
    Editor &ed = g_app.editor;
    const ImGuiStyle &st = ImGui::GetStyle();
    f32 fh = ImGui::GetFrameHeight();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    bool keeping = ed_job_busy(ed.keep_job), sharing = ed_job_busy(ed.share_job);

    ed_caption("KEEP");
    kit_dim("Full quality, next to the original");
    if (kit_button(ICON_SAVE "  Keep", KIT_SECONDARY, ImVec2(w, 0), !keeping)) ed_start(SAVE_KEEP);
    ImGui::Dummy(ImVec2(1, kit_px(8)));

    ed_caption("SHARE");
    kit_dim("A small copy, put on the clipboard");
    static const char *const sizes[] = {"50 MB", "10 MB", "Custom"};
    f32 mb_w = kit_px(84), seg_w = (w - mb_w - st.ItemSpacing.x - kit_px(6)) / 3;
    kit_segmented("##size", sizes, 3, &ed.share_size, seg_w);
    ImGui::SameLine();
    if (ed.custom_mb <= 0) ed.custom_mb = 25;
    ImGui::SetNextItemWidth(w - (3 * seg_w + kit_px(6)) - st.ItemSpacing.x);
    ImGui::BeginDisabled(ed.share_size != 2);
    ImGui::InputFloat("##mb", &ed.custom_mb, 0, 0, "%.0f MB");
    ImGui::EndDisabled();
    if (ed.custom_mb < 1) ed.custom_mb = 1;
    if (ed.custom_mb > 4000) ed.custom_mb = 4000;
    if (kit_button(ICON_SHARE "  Share", KIT_PRIMARY, ImVec2(w, 0), !sharing)) ed_start(SAVE_SHARE);

    // Progress and the result, always in their places: a status line, a thin bar, two lines for the result.
    SaveJobView *v = save_view(sharing ? ed.share_job : keeping ? ed.keep_job : 0);
    f32 frac = 0;
    ImVec2 a = ImGui::GetCursorScreenPos();
    f32 line_h = ImGui::GetTextLineHeight(), bar_h = kit_px(4);
    if (v) {
        char label[64];
        frac = v->progress;
        snprintf(label, sizeof label, "%s%s", v->kind == SAVE_SHARE ? "Sharing" : "Keeping", v->state == SAVE_QUEUED ? ", waiting" : "");
        char pc[16];
        snprintf(pc, sizeof pc, "%.0f%%", frac * 100);
        dl->AddText(a, col32(COL_DIM), label);
        dl->AddText(ImVec2(a.x + w - ImGui::CalcTextSize(pc).x, a.y), col32(COL_DIM), pc);
        f32 by = a.y + line_h + kit_px(6);
        dl->AddRectFilled(ImVec2(a.x, by), ImVec2(a.x + w, by + bar_h), col32(COL_PANEL2), bar_h);
        if (frac > 0) dl->AddRectFilled(ImVec2(a.x, by), ImVec2(a.x + w * (frac > 1 ? 1 : frac), by + bar_h), col32(COL_ACCENT), bar_h);
    } else if (ed.result[0]) {
        ImGui::PushClipRect(a, ImVec2(a.x + w, a.y + line_h * 2 + kit_px(2)), true);
        dl->AddText(g_font, ImGui::GetFontSize(), a, col32(ed.result_error ? 0xff8a92 : COL_OK), ed.result, 0, w);
        ImGui::PopClipRect();
    }
    ImGui::Dummy(ImVec2(w, line_h * 2 + kit_px(4)));
    ImGui::Dummy(ImVec2(1, kit_px(4)));

    // Recent shares, five to a page.
    const i32 rows = 5;
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
            if (mins < 1) str_set(age, "just now");
            else if (mins < 60) snprintf(age, sizeof age, "%lld min ago", mins);
            else if (mins < 48 * 60) snprintf(age, sizeof age, "%lld h ago", mins / 60);
            else snprintf(age, sizeof age, "%lld days ago", mins / 1440);
            fmt_size(f.size, size, sizeof size);
            snprintf(line, sizeof line, "%s \xC2\xB7 %s", age, size);
            f32 text_w = w - fh - kit_px(8);
            ImVec2 ls = ImGui::CalcTextSize(line);
            f32 ty = floorf(at.y + (fh - ls.y) / 2);
            text_clipped(dl, ImVec2(at.x, ty), text_w - ls.x - kit_px(10), col32(COL_TEXT), f.name);
            dl->AddText(ImVec2(at.x + text_w - ls.x, ty), col32(COL_DIM), line);
            ImGui::SetCursorScreenPos(ImVec2(at.x + w - fh, at.y));
            if (kit_icon_button(ICON_COPY, "##copy", KIT_GHOST)) {
                wchar_t name[200];
                utf8_to_wide(f.name, name, COUNT(name));
                Path pth = path_join(g_app.paths.share_dir, name);
                bool ok = clipboard_copy_file(pth.s);
                snprintf(ed.result, sizeof ed.result, ok ? "Copied %s" : "Couldn't copy %s", f.name);
                ed.result_error = !ok;
            }
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

    ImDrawList *dl = ImGui::GetWindowDrawList();
    f32 fh = ImGui::GetFrameHeight();
    ImVec2 avail = ImGui::GetContentRegionAvail();

    // Header: back, the name, and the game and when under it.
    ImVec2 h0 = ImGui::GetCursorScreenPos();
    f32 head_h = fmaxf(fh, g_font_big->FontSize + g_font_small->FontSize + kit_px(4));
    ImGui::SetCursorScreenPos(ImVec2(h0.x, h0.y + (head_h - fh) / 2));
    bool back = kit_icon_button(ICON_BACK, "##back", KIT_GHOST);
    f32 tx = h0.x + fh + kit_px(14), tw = avail.x - (tx - h0.x);
    f32 ty = h0.y + (head_h - g_font_big->FontSize - g_font_small->FontSize - kit_px(4)) / 2;
    ImGui::PushFont(g_font_big);
    text_clipped(dl, ImVec2(tx, ty), tw, col32(COL_TEXT), ed.item.name);
    ImGui::PopFont();
    char when[48], sub[160];
    fmt_when(ed.item.mtime_us, when, sizeof when);
    if (ed.item.info.game[0]) snprintf(sub, sizeof sub, "Editing  \xC2\xB7  %s  \xC2\xB7  %s", ed.item.info.game, when);
    else snprintf(sub, sizeof sub, "Editing  \xC2\xB7  %s", when);
    ImGui::PushFont(g_font_small);
    text_clipped(dl, ImVec2(tx, ty + g_font_big->FontSize + kit_px(4)), tw, col32(COL_DIM), sub);
    ImGui::PopFont();

    // Body: the picture, the transport bar and the timeline on the left; the panel on the right.
    f32 body_y = h0.y + head_h + kit_px(18), body_h = h0.y + avail.y - body_y;
    f32 side_w = kit_px(372), gap = kit_px(20), main_w = avail.x - side_w - gap;
    f32 bar_h = fh + kit_px(16), tl_h = kit_px(64), tl_gap = kit_px(14);
    f32 video_h = body_h - bar_h - tl_h - kit_px(12) - tl_gap - kit_px(6);
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
    player_draw(p, a, b, uv0, uv1);
    if (p.loaded && p.paused && !p.error[0] && !ed.aiming && !ed.drag)
        lib_play_mark(dl, ImVec2((a.x + b.x) / 2, (a.y + b.y) / 2), kit_px(34));
    if (z > 1.001) {
        char zt[32];
        snprintf(zt, sizeof zt, "%.1fx", z);
        badge(dl, ImVec2(a.x + kit_px(10), a.y + kit_px(10)), false, false, zt);
    }

    // The transport bar: play, the time, the trim buttons, and how much is kept on the right. Fixed widths.
    ImVec2 r0(pos.x, pos.y + video_h + kit_px(12));
    dl->AddRectFilled(r0, ImVec2(r0.x + main_w, r0.y + bar_h), col32(COL_PANEL), kit_px(10));
    f32 pad = kit_px(8), y = r0.y + (bar_h - fh) / 2;
    ImGui::SetCursorScreenPos(ImVec2(r0.x + pad, y));
    if (kit_icon_button(p.paused ? ICON_PLAY : ICON_PAUSE, "##play", KIT_GHOST)) player_toggle(&p);
    char t1[32], t2[32], tm[80];
    fmt_time(p.pos, t1, sizeof t1, true);
    fmt_time(ed_duration(), t2, sizeof t2, true);
    snprintf(tm, sizeof tm, "%s / %s", t1, t2);
    f32 time_x = r0.x + pad + fh + kit_px(8);
    ImVec2 ts = ImGui::CalcTextSize(tm);
    dl->AddText(ImVec2(time_x, floorf(y + (fh - ts.y) / 2)), col32(COL_DIM), tm);
    static const char *const trims[] = {ICON_START "  Set start", ICON_NOW "  Set end", ICON_UNDO "  Reset"};
    f32 bx = time_x + kit_px(170);
    for (i32 i = 0; i < 3; i++) {
        f32 w = ImGui::CalcTextSize(trims[i]).x + 2 * ImGui::GetStyle().FramePadding.x;
        ImGui::SetCursorScreenPos(ImVec2(bx, y));
        bool enabled = i < 2 || ed.edit.in_s > 0 || ed.edit.out_s > 0;
        if (kit_button(trims[i], KIT_GHOST, ImVec2(w, 0), enabled)) {
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
    if (kx > bx + kit_px(8)) dl->AddText(ImVec2(kx, floorf(y + (fh - ks.y) / 2)), col32(COL_TEXT), sel);

    // The timeline under it.
    ImGui::SetCursorScreenPos(ImVec2(pos.x, r0.y + bar_h + tl_gap + kit_px(6)));
    ed_timeline(main_w, tl_h);

    // The side panel: Edit, or Keep & share.
    ImGui::SetCursorScreenPos(ImVec2(pos.x + main_w + gap, body_y));
    if (kit_card_begin("ed_side", ImVec2(side_w, body_h))) {
        f32 cw = ImGui::GetContentRegionAvail().x;
        static const char *const tabs[] = {"Edit", "Keep & share"};
        kit_segmented("##tab", tabs, 2, &ed.tab, (cw - kit_px(6)) / 2);
        ImGui::Dummy(ImVec2(1, kit_px(6)));
        if (ed.tab == 0) ed_edit_tab(cw);
        else ed_share_tab(cw);
    }
    kit_card_end();

    // Keys (never while typing).
    if (!io.WantTextInput && !ImGui::IsPopupOpen((const char *)0, ImGuiPopupFlags_AnyPopupId)) {
        if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) player_toggle(&p);
        if (ImGui::IsKeyPressed(ImGuiKey_I, false) && p.pos < ed_out() - 0.2) { ed.edit.in_s = p.pos; ed.dirty = true; }
        if (ImGui::IsKeyPressed(ImGuiKey_O, false) && p.pos > ed.edit.in_s + 0.2) {
            ed.edit.out_s = p.pos >= ed_duration() - 0.01 ? 0 : p.pos;
            ed.dirty = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_K, false)) ed_add_key(p.pos);
        if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) ed_delete_key(ed_key_at(p.pos));
        f64 step = io.KeyShift ? 1.0 : ed_frame();
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) { player_pause(&p); player_seek(&p, p.pos - step); }
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) { player_pause(&p); player_seek(&p, p.pos + step); }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) back = true;
    }
    // The record is written once a change settles (nothing held).
    if (ed.dirty && !ImGui::IsAnyItemActive()) ed_save_record();
    if (back) {
        editor_close();
        g_app.ui.screen = SCREEN_LIBRARY;
    }
}
