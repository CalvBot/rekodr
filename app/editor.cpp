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
    for (i32 t = 0; t < MEDIA_MAX_TRACKS; t++)
        if (e.muted[t] || e.boost[t] != 0) return false;
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
            if (sscanf_s(v, "%lf %lf %lf %lf", &z.t, &z.z, &z.x, &z.y) == 4) e->keys[e->key_count++] = z;
        } else if (!strcmp(k, "volume")) {
            i32 t = 0, muted = 0;
            f32 vol = 1;
            if (sscanf_s(v, "%d %f %d", &t, &vol, &muted) >= 2 && t >= 0 && t < MEDIA_MAX_TRACKS) {
                e->boost[t] = vol - 1;
                e->muted[t] = muted != 0;
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
    fprintf(f, "# clipback edit: trim in seconds, zoom keys (time zoom x y), volumes (track volume muted)\n");
    fprintf(f, "in = %.3f\nout = %.3f\n", e.in_s, e.out_s);
    for (i32 k = 0; k < e.key_count; k++)
        fprintf(f, "key = %.3f %.3f %.4f %.4f\n", e.keys[k].t, e.keys[k].z, e.keys[k].x, e.keys[k].y);
    for (i32 t = 0; t < MEDIA_MAX_TRACKS; t++)
        if (e.muted[t] || e.boost[t] != 0) fprintf(f, "volume = %d %.3f %d\n", t, 1 + e.boost[t], e.muted[t] ? 1 : 0);
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
    f64 u = b->t > a->t ? (t - a->t) / (b->t - a->t) : 0;
    u = u < 0 ? 0 : u > 1 ? 1 : u;
    u = u * u * (3 - 2 * u);
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
    if (p.track_engine_count) {
        for (i32 i = 0; i < n; i++)
            player_track_volume(&p, tracks[i], 1 + ed.edit.boost[tracks[i]], ed.edit.muted[tracks[i]]);
    } else if (n == 1) {
        f32 v = 1 + ed.edit.boost[tracks[0]];
        p.engine->SetVolume(v < 1 ? v : 1);
        p.engine->SetMuted(ed.edit.muted[tracks[0]]);
    }
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
    if (e.a == SAVE_EV_DONE) library_rescan(); // a new clip, session or Keep to show
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

// The timeline: the strip across the whole file, dimmed outside the trim, with handles for the trim, the playhead
// and the zoom keys.
static void ed_timeline(f32 w, f32 h)
{
    Editor &ed = g_app.editor;
    Library &lib = g_app.library;
    Player &p = ed.player;
    f32 s = g_dpi_scale;
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
        f32 grab = 8 * s;
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

    f32 round = 6 * s;
    dl->AddRectFilled(a, b, ImGui::GetColorU32(rgb(COL_PANEL2)), round);
    if (lib_want_strip(ed.item))
        dl->AddImageRounded((ImTextureID)(intptr_t)lib.strip, a, b, ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, round);
    // Outside the trim, dimmed.
    in_x = x_of(ed.edit.in_s);
    out_x = x_of(ed_out());
    if (in_x > a.x) dl->AddRectFilled(a, ImVec2(in_x, b.y), IM_COL32(0, 0, 0, 170), round, ImDrawFlags_RoundCornersLeft);
    if (out_x < b.x) dl->AddRectFilled(ImVec2(out_x, a.y), b, IM_COL32(0, 0, 0, 170), round, ImDrawFlags_RoundCornersRight);
    u32 handle = ImGui::GetColorU32(rgb(COL_TEXT));
    f32 hw = 4 * s;
    dl->AddRect(ImVec2(in_x, a.y), ImVec2(out_x, b.y), handle, 0, 0, 2 * s);
    dl->AddRectFilled(ImVec2(in_x, a.y), ImVec2(in_x + hw, b.y), handle);
    dl->AddRectFilled(ImVec2(out_x - hw, a.y), ImVec2(out_x, b.y), handle);
    // Zoom keys along the top.
    for (i32 k = 0; k < ed.edit.key_count; k++)
    {
        // Kept inside the strip so a key at the very start or end is still seen whole.
        f32 kx = x_of(ed.edit.keys[k].t), r = 6 * s;
        kx = kx < a.x + r ? a.x + r : kx > b.x - r ? b.x - r : kx;
        diamond(dl, ImVec2(kx, a.y + 9 * s), r, ImGui::GetColorU32(rgb(COL_BLUE)));
    }
    f32 px = x_of(p.pos);
    dl->AddRectFilled(ImVec2(px - 1 * s, a.y - 3 * s), ImVec2(px + 1 * s, b.y + 3 * s), ImGui::GetColorU32(rgb(COL_ACCENT)));
}

static void ed_section(const char *title)
{
    ImGui::PushFont(g_font_bold);
    ImGui::TextDisabled("%s", title);
    ImGui::PopFont();
}

// A row of page controls at a fixed place: "←  1 of 3  →".
static void ed_pager(const char *id, i32 *page, i32 pages)
{
    f32 s = g_dpi_scale, bw = 36 * s, tw = 70 * s;
    ImGui::PushID(id);
    if (*page >= pages) *page = pages - 1;
    if (*page < 0) *page = 0;
    if (button_enabled("\xE2\x86\x90", ImVec2(bw, 0), *page > 0)) (*page)--;
    ImGui::SameLine();
    char t[32];
    snprintf(t, sizeof t, "%d of %d", *page + 1, pages);
    f32 x = ImGui::GetCursorPosX();
    ImGui::SetCursorPosX(x + (tw - ImGui::CalcTextSize(t).x) / 2);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", t);
    ImGui::SameLine(x + tw + ImGui::GetStyle().ItemSpacing.x);
    if (button_enabled("\xE2\x86\x92", ImVec2(bw, 0), *page + 1 < pages)) (*page)++;
    ImGui::PopID();
}

static void ed_edit_tab()
{
    Editor &ed = g_app.editor;
    Player &p = ed.player;
    f32 s = g_dpi_scale;
    f32 w = ImGui::GetContentRegionAvail().x;

    ed_section("ZOOM");
    f64 z, cx, cy;
    zoom_at(ed.edit, p.pos, &z, &cx, &cy);
    f32 zf = (f32)z;
    ImGui::SetNextItemWidth(w);
    if (ImGui::SliderFloat("##zoom", &zf, 1.f, 4.f, "Zoom at the playhead: %.1fx")) {
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
    ImGui::PushStyleColor(ImGuiCol_Text, rgb(COL_DIM));
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w);
    ImGui::TextUnformatted("Zooming adds a keyframe at the playhead. Drag the picture to aim. The view eases from one "
                           "keyframe to the next.");
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    f32 bw = (w - 2 * ImGui::GetStyle().ItemSpacing.x) / 3;
    i32 here = ed_key_at(p.pos);
    if (button_enabled("Add keyframe", ImVec2(bw, 0), here < 0 && ed.edit.key_count < EDIT_MAX_KEYS)) ed_add_key(p.pos);
    ImGui::SameLine();
    if (button_enabled("Delete keyframe", ImVec2(bw, 0), here >= 0)) ed_delete_key(here);
    ImGui::SameLine();
    if (button_enabled("Clear all", ImVec2(bw, 0), ed.edit.key_count > 0)) {
        ed.edit.key_count = 0;
        ed.dirty = true;
    }
    // The keys, three to a page.
    const i32 key_rows = 3;
    i32 key_pages = ed.edit.key_count ? (ed.edit.key_count + key_rows - 1) / key_rows : 1;
    if (ed.key_page >= key_pages) ed.key_page = key_pages - 1;
    for (i32 r = 0; r < key_rows; r++) {
        i32 k = ed.key_page * key_rows + r;
        ImGui::PushID(r);
        if (k < ed.edit.key_count) {
            const ZoomKey &key = ed.edit.keys[k];
            char t[32], label[96];
            fmt_time(key.t, t, sizeof t, true);
            snprintf(label, sizeof label, "%s   %.1fx###key", t, key.z);
            f32 del_w = 80 * s;
            if (ImGui::Selectable(label, k == here, 0, ImVec2(w - del_w - ImGui::GetStyle().ItemSpacing.x, ImGui::GetFrameHeight())))
                player_seek(&p, key.t);
            ImGui::SameLine();
            if (ImGui::Button("Delete", ImVec2(del_w, 0))) ed_delete_key(k);
        } else if (k == 0) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("No zoom keyframes: the whole clip is shown.");
        } else {
            ImGui::Dummy(ImVec2(w, ImGui::GetFrameHeight()));
        }
        ImGui::PopID();
    }
    ed_pager("keys", &ed.key_page, key_pages);
    ImGui::Spacing();

    ed_section("AUDIO");
    i32 tracks[MEDIA_MAX_TRACKS];
    i32 n = ed_tracks(ed.item.info, tracks, MEDIA_MAX_TRACKS);
    const i32 track_rows = 5;
    i32 track_pages = n ? (n + track_rows - 1) / track_rows : 1;
    if (ed.track_page >= track_pages) ed.track_page = track_pages - 1;
    f32 mute_w = 80 * s, name_w = 120 * s, slider_w = w - mute_w - name_w - 2 * ImGui::GetStyle().ItemSpacing.x;
    bool changed = false;
    for (i32 r = 0; r < track_rows; r++) {
        i32 i = ed.track_page * track_rows + r;
        ImGui::PushID(100 + r);
        if (i < n) {
            i32 t = tracks[i];
            bool muted = ed.edit.muted[t];
            ImGui::PushStyleColor(ImGuiCol_Button, muted ? rgb(COL_ACCENT, 0.8f) : rgb(COL_PANEL2));
            if (ImGui::Button(muted ? "Muted###mute" : "Mute###mute", ImVec2(mute_w, 0))) {
                ed.edit.muted[t] = !muted;
                changed = true;
            }
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImVec2 np = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(name_w, ImGui::GetFrameHeight()));
            text_clipped(ImGui::GetWindowDrawList(), ImVec2(np.x, np.y + ImGui::GetStyle().FramePadding.y), name_w - 6 * s,
                         ImGui::GetColorU32(rgb(muted ? COL_DIM : COL_TEXT)), ed.item.info.tracks[t]);
            ImGui::SameLine();
            f32 pct = (1 + ed.edit.boost[t]) * 100;
            ImGui::SetNextItemWidth(slider_w);
            ImGui::BeginDisabled(muted);
            if (ImGui::SliderFloat("##vol", &pct, 0, 200, "%.0f%%")) {
                ed.edit.boost[t] = pct / 100 - 1;
                changed = true;
            }
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                ed.edit.boost[t] = 0;
                changed = true;
            }
            ImGui::EndDisabled();
        } else if (i == 0) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("This file has no sound.");
        } else {
            ImGui::Dummy(ImVec2(w, ImGui::GetFrameHeight()));
        }
        ImGui::PopID();
    }
    ed_pager("tracks", &ed.track_page, track_pages);
    if (changed) {
        ed.dirty = true;
        ed_apply_volumes();
    }
}

static void ed_share_tab()
{
    Editor &ed = g_app.editor;
    f32 s = g_dpi_scale;
    const ImGuiStyle &st = ImGui::GetStyle();
    f32 w = ImGui::GetContentRegionAvail().x;
    auto dim_text = [&](const char *t) {
        ImGui::PushStyleColor(ImGuiCol_Text, rgb(COL_DIM));
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w);
        ImGui::TextUnformatted(t);
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
    };
    bool keeping = ed_job_busy(ed.keep_job), sharing = ed_job_busy(ed.share_job);

    ed_section("KEEP");
    dim_text("Saves the trimmed, zoomed clip as a new file next to the original, at full quality, with every track.");
    if (button_enabled("Keep", ImVec2(w, 0), !keeping)) ed_start(SAVE_KEEP);
    ImGui::Spacing();

    ed_section("SHARE");
    dim_text("A small copy with one mix of the tracks, put on the clipboard to paste into a chat.");
    static const char *const sizes[] = {"50 MB", "10 MB", "Custom"};
    f32 sw = (w - 3 * st.ItemSpacing.x - 90 * s) / 3;
    for (i32 i = 0; i < 3; i++) {
        if (i) ImGui::SameLine();
        bool on = ed.share_size == i;
        ImGui::PushStyleColor(ImGuiCol_Button, on ? rgb(COL_BLUE, 0.85f) : rgb(COL_PANEL2));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, on ? rgb(COL_BLUE) : rgb(COL_HOVER));
        if (ImGui::Button(sizes[i], ImVec2(sw, 0))) ed.share_size = i;
        ImGui::PopStyleColor(2);
    }
    ImGui::SameLine();
    if (ed.custom_mb <= 0) ed.custom_mb = 25;
    ImGui::SetNextItemWidth(90 * s);
    ImGui::BeginDisabled(ed.share_size != 2);
    ImGui::InputFloat("##mb", &ed.custom_mb, 0, 0, "%.0f MB");
    ImGui::EndDisabled();
    if (ed.custom_mb < 1) ed.custom_mb = 1;
    if (ed.custom_mb > 4000) ed.custom_mb = 4000;
    if (button_enabled("Share", ImVec2(w, 0), !sharing)) ed_start(SAVE_SHARE);

    // Progress and the result, always in their places.
    SaveJobView *v = save_view(sharing ? ed.share_job : keeping ? ed.keep_job : 0);
    char label[64] = " ";
    f32 frac = 0;
    if (v) {
        frac = v->progress;
        snprintf(label, sizeof label, "%s%s %.0f%%", v->kind == SAVE_SHARE ? "Sharing" : "Keeping",
                 v->state == SAVE_QUEUED ? " (waiting)" : "", frac * 100);
    }
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, rgb(COL_BLUE));
    ImGui::ProgressBar(frac, ImVec2(w, 0), label);
    ImGui::PopStyleColor();
    ImVec2 rp = ImGui::GetCursorScreenPos();
    f32 line_h = ImGui::GetTextLineHeightWithSpacing();
    ImGui::Dummy(ImVec2(w, line_h * 2));
    // Two lines kept for the result, wrapped inside them.
    ImGui::PushClipRect(rp, ImVec2(rp.x + w, rp.y + line_h * 2), true);
    ImGui::GetWindowDrawList()->AddText(g_font, ImGui::GetFontSize(), rp,
                                        ImGui::GetColorU32(rgb(ed.result_error ? COL_ACCENT : COL_OK)), ed.result, 0, w);
    ImGui::PopClipRect();
    ImGui::Spacing();

    ed_section("RECENT SHARES");
    const i32 rows = 5;
    i32 pages = ed.share_count ? (ed.share_count + rows - 1) / rows : 1;
    if (ed.share_page >= pages) ed.share_page = pages - 1;
    f32 copy_w = 70 * s;
    for (i32 r = 0; r < rows; r++) {
        i32 i = ed.share_page * rows + r;
        ImGui::PushID(200 + r);
        ImVec2 at = ImGui::GetCursorScreenPos();
        if (i < ed.share_count) {
            const ShareFile &f = ed.shares[i];
            ImGui::Dummy(ImVec2(w - copy_w - st.ItemSpacing.x, ImGui::GetFrameHeight()));
            char age[48], size[24], line[96];
            i64 mins = (now_us() - f.mtime_us) / 60000000;
            if (mins < 1) str_set(age, "just now");
            else if (mins < 60) snprintf(age, sizeof age, "%lld min ago", mins);
            else if (mins < 48 * 60) snprintf(age, sizeof age, "%lld h ago", mins / 60);
            else snprintf(age, sizeof age, "%lld days ago", mins / 1440);
            fmt_size(f.size, size, sizeof size);
            snprintf(line, sizeof line, "%s \xC2\xB7 %s", age, size);
            ImVec2 ls = ImGui::CalcTextSize(line);
            f32 text_w = w - copy_w - 2 * st.ItemSpacing.x;
            ImVec2 tp(at.x, at.y + st.FramePadding.y);
            ImDrawList *dl = ImGui::GetWindowDrawList();
            text_clipped(dl, tp, text_w - ls.x - 8 * s, ImGui::GetColorU32(rgb(COL_TEXT)), f.name);
            dl->AddText(ImVec2(at.x + text_w - ls.x, tp.y), ImGui::GetColorU32(rgb(COL_DIM)), line);
            ImGui::SameLine();
            if (ImGui::Button("Copy", ImVec2(copy_w, 0))) {
                wchar_t name[200];
                utf8_to_wide(f.name, name, COUNT(name));
                Path pth = path_join(g_app.paths.share_dir, name);
                bool ok = clipboard_copy_file(pth.s);
                snprintf(ed.result, sizeof ed.result, ok ? "Copied %s to the clipboard" : "Couldn't copy %s", f.name);
                ed.result_error = !ok;
            }
        } else if (i == 0) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("Nothing shared in the last %g days.", g_app.config.share_keep_days);
        } else {
            ImGui::Dummy(ImVec2(w, ImGui::GetFrameHeight()));
        }
        ImGui::PopID();
    }
    ed_pager("shares", &ed.share_page, pages);
}

static void screen_editor()
{
    Editor &ed = g_app.editor;
    Player &p = ed.player;
    f32 s = g_dpi_scale;
    const ImGuiStyle &st = ImGui::GetStyle();
    ImGuiIO &io = ImGui::GetIO();
    ed_check_pending();
    if (!ed.open && ed.pending[0]) {
        ImGui::TextDisabled("Opening the saved file...");
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
        ed_apply_volumes();
        ed.tracks_ready = true;
    }
    p.loop_from = ed.edit.in_s;
    p.stop_at = ed_out();
    player_update(&p);
    if (p.track_engine_count && ed.tracks_ready) ed_apply_volumes(); // a track engine that's just loaded

    // Title row.
    bool back = ImGui::Button("\xE2\x86\x90 Library", ImVec2(110 * s, 0));
    ImGui::SameLine();
    ImGui::PushFont(g_font_big);
    ImGui::TextUnformatted(ed.item.name);
    ImGui::PopFont();

    ImVec2 avail = ImGui::GetContentRegionAvail();
    f32 side_w = 400 * s, main_w = avail.x - side_w - st.ItemSpacing.x;
    f32 row_h = ImGui::GetFrameHeight(), tl_h = 72 * s;
    f32 video_h = avail.y - row_h - tl_h - 2 * st.ItemSpacing.y;

    ImGui::BeginChild("ed_main", ImVec2(main_w, avail.y), 0, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground);
    {
        // The picture, as it will be rendered (zoomed at the playhead).
        ImVec2 pos = ImGui::GetCursorScreenPos(), a, b;
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
        if (z > 1.001) {
            char zt[32];
            snprintf(zt, sizeof zt, "%.1fx", z);
            badge(ImGui::GetWindowDrawList(), ImVec2(a.x + 10 * s, a.y + 10 * s), false, false, zt);
        }
        ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + video_h + st.ItemSpacing.y));

        // Transport row, fixed widths.
        if (ImGui::Button(p.paused ? "Play###play" : "Pause###play", ImVec2(90 * s, 0))) player_toggle(&p);
        ImGui::SameLine();
        char t1[32], t2[32], tm[80];
        fmt_time(p.pos, t1, sizeof t1, true);
        fmt_time(ed_duration(), t2, sizeof t2, true);
        snprintf(tm, sizeof tm, "%s / %s", t1, t2);
        f32 x = ImGui::GetCursorPosX();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(tm);
        ImGui::SameLine(x + 150 * s);
        if (ImGui::Button("Set start", ImVec2(100 * s, 0))) {
            if (p.pos < ed_out() - 0.2) { ed.edit.in_s = p.pos; ed.dirty = true; }
        }
        ImGui::SameLine();
        if (ImGui::Button("Set end", ImVec2(100 * s, 0))) {
            if (p.pos > ed.edit.in_s + 0.2) { ed.edit.out_s = p.pos >= ed_duration() - 0.01 ? 0 : p.pos; ed.dirty = true; }
        }
        ImGui::SameLine();
        if (button_enabled("Reset trim", ImVec2(100 * s, 0), ed.edit.in_s > 0 || ed.edit.out_s > 0)) {
            ed.edit.in_s = ed.edit.out_s = 0;
            ed.dirty = true;
        }
        ImGui::SameLine();
        char sel[48], st1[32];
        fmt_time(ed_out() - ed.edit.in_s, st1, sizeof st1, true);
        snprintf(sel, sizeof sel, "Kept: %s", st1);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", sel);
        ed_timeline(main_w, tl_h);
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("ed_side", ImVec2(side_w, avail.y), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
    {
        f32 tw = (ImGui::GetContentRegionAvail().x - st.ItemSpacing.x) / 2;
        if (view_tab("Edit", ed.tab == 0, tw)) ed.tab = 0;
        ImGui::SameLine();
        if (view_tab("Keep / Share", ed.tab == 1, tw)) ed.tab = 1;
        ImGui::Spacing();
        if (ed.tab == 0) ed_edit_tab();
        else ed_share_tab();
    }
    ImGui::EndChild();

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
