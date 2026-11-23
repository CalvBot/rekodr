// Editor: trim, zoom and colour keyframes and track volumes for one saved clip or session, then Keep or Share.
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

// Advanced editing lives in adv_editor.cpp; these are called from the shared editor code before it.
static bool adv_on() { return g_app.editor.edit.mode == EDIT_ADVANCED; }
static void adv_seek(f64 u);
static void adv_toggle();
static void adv_apply_volumes();
static void adv_resync();

static bool edit_is_default(const EditRecord &e)
{
    if (e.mode == EDIT_ADVANCED) return false;
    if (e.piece_count || e.key_count || e.fade_in > 0 || e.fade_out > 0) return false;
    for (i32 t = 0; t <= EDIT_MASTER; t++)
        if (e.muted[t] || e.boost[t] != 0 || e.vol_count[t]) return false;
    for (i32 c = 0; c < EDIT_COLORS; c++)
        if (e.color_count[c]) return false;
    return true;
}

// Whether an edit file holds an Advanced edit: its first line says so.
static bool edit_is_advanced(const Path &file)
{
    FILE *f = _wfopen(file.s, L"rb");
    if (!f) return false;
    char line[64] = {};
    bool adv = fgets(line, sizeof line, f) && !strncmp(line, "mode = advanced", 15);
    fclose(f);
    return adv;
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
        if (!strcmp(k, "mode")) {
            e->mode = !strcmp(v, "advanced") ? EDIT_ADVANCED : EDIT_SIMPLE;
        } else if (!strcmp(k, "item") && e->item_count < EDIT_MAX_ITEMS) {
            // id track start from to link, then fade in, fade out and crossfade
            EditItem it = {};
            i32 id = 0, track = 0, link = 0;
            if (sscanf_s(v, "%d %d %lf %lf %lf %d %f %f %f", &id, &track, &it.start, &it.from, &it.to, &link,
                         &it.fade_in, &it.fade_out, &it.cross) < 6 ||
                id <= 0 || id > 65535 || track < 0 || track > MEDIA_MAX_TRACKS || link < 0 || link > 65535 ||
                it.to <= it.from)
                continue;
            it.id = (u16)id, it.track = (u8)track, it.link = (u16)link;
            it.fade_in = ImMax(it.fade_in, 0.f), it.fade_out = ImMax(it.fade_out, 0.f), it.cross = ImMax(it.cross, 0.f);
            e->items[e->item_count++] = it;
        } else if (!strcmp(k, "ikey")) {
            // item, then z time zoom x y ease, or b, c, s or v (a colour or the volume) time value ease
            ItemKey q = {};
            i32 id = 0, ease = EASE_LINEAR;
            char what = 0;
            i32 got = sscanf_s(v, "%d %c %lf %lf", &id, &what, 1, &q.t, &q.v);
            const char *kinds = "zbcsv", *at = what ? strchr(kinds, what) : 0;
            if (got < 4 || !at || id <= 0 || id > 65535) continue;
            q.item = (u16)id, q.what = (u8)(at - kinds);
            const char *rest = v;
            for (i32 skip = 0; skip < 4 && *rest; skip++) { // past the four read
                while (*rest && *rest != ' ') rest++;
                while (*rest == ' ') rest++;
            }
            if (q.what == IKEY_ZOOM) {
                if (sscanf_s(rest, "%lf %lf %d", &q.x, &q.y, &ease) < 2) continue;
                q.v = q.v < 1 ? 1 : q.v > 4 ? 4 : q.v;
            } else {
                sscanf_s(rest, "%d", &ease);
                q.v = q.v < 0 ? 0 : q.v > 2 ? 2 : q.v;
            }
            q.ease = (u8)(ease >= 0 && ease < EASE_COUNT ? ease : EASE_LINEAR);
            ikey_add(e, q);
        } else if (!strcmp(k, "piece") && e->piece_count < EDIT_MAX_PIECES) { // from to, then f or x and a length
            EditPiece p = {};
            char kind = 0;
            if (sscanf_s(v, "%lf %lf %c %f", &p.from, &p.to, &kind, 1, &p.trans_len) < 2 || p.to <= p.from) continue;
            p.trans = (u8)(kind == 'f' ? TRANS_FADE : kind == 'x' ? TRANS_CROSS : TRANS_CUT);
            if (p.trans == TRANS_CUT || p.trans_len <= 0) p.trans = TRANS_CUT, p.trans_len = 0;
            e->pieces[e->piece_count++] = p;
        } else if (!strcmp(k, "fade")) { // in out
            sscanf_s(v, "%f %f", &e->fade_in, &e->fade_out);
            e->fade_in = e->fade_in > 0 ? e->fade_in : 0;
            e->fade_out = e->fade_out > 0 ? e->fade_out : 0;
        } else if (!strcmp(k, "key") && e->key_count < EDIT_MAX_KEYS) {
            ZoomKey z = {};
            i32 ease = EASE_SMOOTH;
            if (sscanf_s(v, "%lf %lf %lf %lf %d", &z.t, &z.z, &z.x, &z.y, &ease) == 5) {
                z.ease = (u8)(ease >= 0 && ease < EASE_COUNT ? ease : EASE_SMOOTH);
                e->keys[e->key_count++] = z;
            }
        } else if (!strcmp(k, "ckey")) { // b, c or s, then time value ease
            i32 c = *v == 'b' ? COLOR_BRIGHTNESS : *v == 'c' ? COLOR_CONTRAST : *v == 's' ? COLOR_SATURATION : -1;
            VolKey ck = {};
            i32 ease = EASE_LINEAR;
            if (c < 0 || e->color_count[c] >= EDIT_MAX_VOL_KEYS || sscanf_s(v + 1, "%lf %f %d", &ck.t, &ck.v, &ease) < 2) continue;
            ck.v = ck.v < 0 ? 0 : ck.v > 2 ? 2 : ck.v;
            ck.ease = (u8)(ease >= 0 && ease < EASE_COUNT ? ease : EASE_LINEAR);
            i32 n = e->color_count[c], at = n;
            while (at > 0 && e->color[c][at - 1].t > ck.t) at--;
            memmove(e->color[c] + at + 1, e->color[c] + at, (n - at) * sizeof ck);
            e->color[c][at] = ck;
            e->color_count[c] = n + 1;
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
    if (e.mode == EDIT_ADVANCED) {
        fprintf(f, "mode = advanced\n"
                   "# clipback edit, Advanced: items (id track start from to link, then fade in, fade out and crossfade\n"
                   "# from the item before, seconds; track 0 the picture, 1 + n audio track n; start on the sequence,\n"
                   "# from and to in the video), item keys (item z time zoom x y ease, or item b, c, s or v for\n"
                   "# brightness, contrast, saturation or volume, then time value ease; times in the video),\n"
                   "# muted tracks (volume = track 1 1); eases 0 linear 1 smooth 2 in 3 out 4 hold\n");
        for (i32 i = 0; i < e.item_count; i++) {
            const EditItem &it = e.items[i];
            fprintf(f, "item = %d %d %.3f %.3f %.3f %d %.3f %.3f %.3f\n", it.id, it.track, it.start, it.from, it.to,
                    it.link, it.fade_in, it.fade_out, it.cross);
        }
        for (i32 k = 0; k < e.ikey_count; k++) {
            const ItemKey &q = e.ikeys[k];
            if (q.what == IKEY_ZOOM)
                fprintf(f, "ikey = %d z %.3f %.3f %.4f %.4f %d\n", q.item, q.t, q.v, q.x, q.y, q.ease);
            else fprintf(f, "ikey = %d %c %.3f %.3f %d\n", q.item, "zbcsv"[q.what], q.t, q.v, q.ease);
        }
        for (i32 t = 0; t < MEDIA_MAX_TRACKS; t++)
            if (e.muted[t]) fprintf(f, "volume = %d 1 1\n", t);
        fclose(f);
        MoveFileExW(tmp.s, file.s, MOVEFILE_REPLACE_EXISTING);
        return;
    }
    fprintf(f, "# clipback edit: pieces kept in the order they play (from to, seconds; then f fade or x crossfade and\n"
               "# its length when the piece before goes into it that way), fades from and to black (in out, seconds),\n"
               "# zoom keys (time zoom x y ease), volumes (track volume muted), volume keys (track time volume ease); track m is the whole mix;\n"
               "# colour keys (b brightness, c contrast or s saturation, time value ease; 1 = as recorded);\n"
               "# eases 0 linear 1 smooth 2 in 3 out 4 hold\n");
    for (i32 i = 0; i < e.piece_count; i++) {
        const EditPiece &p = e.pieces[i];
        if (i && p.trans != TRANS_CUT) fprintf(f, "piece = %.3f %.3f %c %.3f\n", p.from, p.to, p.trans == TRANS_FADE ? 'f' : 'x', p.trans_len);
        else fprintf(f, "piece = %.3f %.3f\n", p.from, p.to);
    }
    if (e.fade_in > 0 || e.fade_out > 0) fprintf(f, "fade = %.3f %.3f\n", e.fade_in, e.fade_out);
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
    for (i32 c = 0; c < EDIT_COLORS; c++)
        for (i32 k = 0; k < e.color_count[c]; k++)
            fprintf(f, "ckey = %c %.3f %.3f %d\n", "bcs"[c], e.color[c][k].t, e.color[c][k].v, e.color[c][k].ease);
    fclose(f);
    MoveFileExW(tmp.s, file.s, MOVEFILE_REPLACE_EXISTING);
}

// The zoom at `t`: eased between keys with smoothstep, the same curve the render uses.
// In Advanced, the zoom of the picture item at the playhead (none over a gap); `t` is in the file either way.
static void zoom_at(const EditRecord &e, f64 t, f64 *z, f64 *x, f64 *y)
{
    if (e.mode == EDIT_ADVANCED) {
        i32 v = item_at(e, 0, g_app.editor.adv_u);
        if (v >= 0) item_zoom_at(e, e.items[v].id, t, z, x, y);
        else *z = 1, *x = *y = 0.5;
        return;
    }
    zoom_keys_at(e.keys, e.key_count, t, z, x, y);
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

// Lanes: the zoom keys (lane 0), each volume slot's keys (lane 1 + slot) and each colour's (ED_COLOR + colour) read
// and written alike, so the timeline's selecting, dragging, eases, copy and paste work the same on all of them.
// Values are a zoom (1..4), or a volume or a colour (0..2, 1 = as recorded).
struct LaneKey {
    f64 t, v, x, y; // x, y: a zoom key's aim
    u8  ease;
};

static bool ln_is_volume(i32 l) { return l >= 1 && l <= 1 + EDIT_MASTER; }
static const char *const COLOR_NAMES[EDIT_COLORS] = {"Brightness", "Contrast", "Saturation"};
// A volume or colour lane's keys and their count (l != 0).
static VolKey *ln_levels(EditRecord &e, i32 l) { return l >= ED_COLOR ? e.color[l - ED_COLOR] : e.vol[l - 1]; }
static const VolKey *ln_levels(const EditRecord &e, i32 l) { return l >= ED_COLOR ? e.color[l - ED_COLOR] : e.vol[l - 1]; }
static i32 &ln_level_count(EditRecord &e, i32 l) { return l >= ED_COLOR ? e.color_count[l - ED_COLOR] : e.vol_count[l - 1]; }
static i32 ln_count(const EditRecord &e, i32 l)
{
    return !l ? e.key_count : l >= ED_COLOR ? e.color_count[l - ED_COLOR] : e.vol_count[l - 1];
}
static i32 ln_cap(i32 l) { return l ? EDIT_MAX_VOL_KEYS : EDIT_MAX_KEYS; }
static f64 ln_lo(i32 l) { return l ? 0 : 1; }
static f64 ln_hi(i32 l) { return l ? 2 : 4; }

static LaneKey ln_get(const EditRecord &e, i32 l, i32 k)
{
    LaneKey q = {};
    if (l) {
        const VolKey &v = ln_levels(e, l)[k];
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
        VolKey &v = ln_levels(e, l)[k];
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
    if (l >= ED_COLOR) return color_at(e, l - ED_COLOR, t);
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
        memmove(ln_levels(e, l) + at + 1, ln_levels(e, l) + at, (n - at) * sizeof(VolKey));
        ln_level_count(e, l)++;
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
        memmove(ln_levels(e, l) + k, ln_levels(e, l) + k + 1, (n - k - 1) * sizeof(VolKey));
        ln_level_count(e, l)--;
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

// The trim in and out: the first piece's start and end.
static f64 ed_in()
{
    EditPiece p[EDIT_MAX_PIECES];
    pieces_of(g_app.editor.edit, ed_duration(), p);
    return p[0].from;
}

static f64 ed_out()
{
    EditPiece p[EDIT_MAX_PIECES];
    pieces_of(g_app.editor.edit, ed_duration(), p);
    return p[0].to;
}

static void ed_set_trim(f64 a, f64 z)
{
    Editor &ed = g_app.editor;
    EditPiece p = {a < 0 ? 0 : a, z};
    pieces_store(&ed.edit, &p, 1, ed_duration());
    ed.dirty = true;
}

static void ed_clear_trim()
{
    Editor &ed = g_app.editor;
    ed.edit.piece_count = 0;
    ed.dirty = true;
}

static f64 ed_frame()
{
    f64 fps = g_app.editor.item.info.fps;
    return 1 / (fps > 1 ? fps : 60);
}

// The timeline's axis. With more than one piece it's the pieces joined in the order they play ("output time", u);
// otherwise it's the file's own time. Keys and pieces are kept in the file's time (t).
static bool ed_joined() { return g_app.editor.edit.piece_count > 1; }

static f64 ed_axis_len()
{
    if (adv_on()) return ImMax(adv_len(g_app.editor.edit), ed_duration());
    return ed_joined() ? pieces_len(g_app.editor.edit, ed_duration()) : ed_duration();
}

// A moment in the file on the axis; -1 when it's cut out.
static f64 ed_u(f64 t)
{
    if (!ed_joined()) return t;
    f64 u;
    return piece_out(g_app.editor.edit, ed_duration(), t, &u) ? u : -1;
}

// A place on the axis as a moment in the file.
static f64 ed_s(f64 u) { return ed_joined() ? piece_src(g_app.editor.edit, ed_duration(), u) : u; }

// The playhead on the axis, in the piece it's playing: a piece's end that's also another one's start stays where
// it was reached.
static f64 ed_play_u()
{
    Editor &ed = g_app.editor;
    if (adv_on()) return ed.adv_u;
    f64 pos = ed.player.pos;
    if (!ed_joined()) return pos;
    EditPiece pc[EDIT_MAX_PIECES];
    i32 np = pieces_of(ed.edit, ed_duration(), pc), i = ed.play_piece;
    if (i >= 0 && i < np && pos >= pc[i].from - 0.001 && pos <= pc[i].to + 0.001) {
        f64 d = pos - pc[i].from, len = pc[i].to - pc[i].from;
        return piece_start(ed.edit, ed_duration(), i) + (d < 0 ? 0 : d > len ? len : d);
    }
    f64 u = ed_u(pos);
    return u >= 0 ? u : 0;
}

// Moves the playhead to a place on the axis.
static void ed_seek_u(f64 u)
{
    Editor &ed = g_app.editor;
    if (adv_on()) return adv_seek(u);
    if (ed_joined()) player_seek(&ed.player, piece_src(ed.edit, ed_duration(), u, &ed.play_piece));
    else player_seek(&ed.player, u);
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

// The piece the playhead is in.
static i32 ed_play_piece() { return ed_joined() ? g_app.editor.play_piece : 0; }

static void ed_apply_volumes()
{
    Editor &ed = g_app.editor;
    Player &p = ed.player;
    if (!p.engine) return;
    if (adv_on()) return adv_apply_volumes();
    i32 tracks[MEDIA_MAX_TRACKS];
    i32 n = ed_tracks(ed.item.info, tracks, MEDIA_MAX_TRACKS);
    // At the playhead: each track's volume times the whole mix's (the engines can't go past 100%). Only what's
    // changed is handed over, since this runs every frame while playing.
    f64 master = vol_heard(ed.edit, EDIT_MASTER, p.pos) * fade_gain(ed.edit, ed_duration(), ed_play_piece(), p.pos);
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

// Recent shares: the share folder's files, last shared first. Copies not shared for share_keep_days are cleared;
// copying one again counts as sharing it (ed_share_touch).
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

// A share copy copied again was shared just now: its time is moved up, so it stays share_keep_days from today.
static void ed_share_touch(const wchar_t *path)
{
    const Path &dir = g_app.paths.share_dir;
    size_t n = wcslen(dir.s);
    if (_wcsnicmp(path, dir.s, n) || path[n] != L'\\') return;
    HANDLE h = CreateFileW(path, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0, OPEN_EXISTING, 0, 0);
    if (h == INVALID_HANDLE_VALUE) return;
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    SetFileTime(h, 0, 0, &ft);
    CloseHandle(h);
    editor_refresh_shares();
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
    ed.play_piece = -1;
    ed.settled = ed.edit;
    ed.undo_count = ed.redo_count = 0;
    ed.aiming = false;
    ed.adv_u = 0;
    ed.adv_playing = false;
    ed.adv_sel_count = 0;
    ed.adv_drag = 0;
    ed.adv_ask_simple = false;
    ed.adv_key = -1;
    adv_resync();
    ed.result[0] = 0;
    ed.result_path[0] = ed.result_url[0] = 0;
    ed.result_error = false;
    str_set(ed.out_name, ed.item.name);
    ed.player.id = 1;
    player_open(&ed.player, ed.path, ed_in(), false);
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
    if (e.a == SAVE_EV_DONE && !(job && (job->kind == SAVE_PREVIEW || job->kind == SAVE_LINK)))
        library_rescan(); // a new clip, session or Keep
    if (!id || id != ed.keep_job) return;
    const char *name = strrchr(e.text, '\\');
    name = name ? name + 1 : e.text;
    ed.result_path[0] = ed.result_url[0] = 0;
    if (e.a == SAVE_EV_DONE) {
        str_set(ed.result_path, e.text);
        snprintf(ed.result, sizeof ed.result, "Kept as %s", name);
        ed.result_error = false;
    } else if (e.a == SAVE_EV_FAILED) {
        snprintf(ed.result, sizeof ed.result, "Keep failed: %s", e.text);
        ed.result_error = true;
    } else if (e.a == SAVE_EV_CANCELLED) {
        str_set(ed.result, "Keep cancelled");
        ed.result_error = false;
    }
}

static bool ed_job_busy(u32 id)
{
    SaveJobView *v = save_view(id);
    return v && v->state < SAVE_DONE;
}

// The file as the editor plays it, for share_decide: its length the player's when known
static MediaInfo ed_share_info()
{
    MediaInfo m = g_app.editor.item.info;
    if (g_app.editor.player.duration > 0) m.duration = g_app.editor.player.duration;
    return m;
}

// Keep, or with small a Keep at the size Share would make its file (share_decide): a smaller clip kept in Clips for
// good, not a share copy.
static void ed_start(bool small_keep = false)
{
    Editor &ed = g_app.editor;
    ed_save_record();
    static EditJob j;
    j = {};
    j.src = ed.path;
    j.info = ed_share_info();
    j.edit = ed.edit;
    swprintf(j.out_dir.s, MAX_PATH, L"%s\\%d\\Clips", g_app.config.save_dir.s, ed.item.year);
    // Kept beside clips that may hold the original: its own name, or the original's with " edit" (" small")
    if (ed.out_name[0] && strcmp(ed.out_name, ed.item.name)) snprintf(j.stem, sizeof j.stem, "%s", ed.out_name);
    else snprintf(j.stem, sizeof j.stem, "%s %s", ed.item.name, small_keep ? "small" : "edit");
    SaveKind kind = SAVE_KEEP;
    if (small_keep) { // made as Share's file is, into Clips
        ShareDecision d = share_decide(j.info, &ed.edit);
        kind = SAVE_SHARE;
        j.aim_bytes = d.file_squeezed ? d.max_bytes : d.reco_bytes;
        j.limit_bytes = d.max_bytes;
    }
    clean_name(j.stem); // the name is typed: drop what a file name can't hold
    if (!j.stem[0]) snprintf(j.stem, sizeof j.stem, "%s", ed.item.name);
    ed.keep_job = save_edit(kind, j);
    ed.result_url[0] = 0;
    if (!ed.keep_job) {
        str_set(ed.result, "Too many saves are waiting; try again in a moment.");
        ed.result_error = true;
    } else {
        ed.result[0] = 0;
    }
}

// Share for the edit as `kind` (share.cpp), a link on `host` when one is picked (0: Share's rule): the file or link
// goes on the clipboard when it's made (ed_share_poll).
static void ed_share(ShareKind kind, const char *host)
{
    Editor &ed = g_app.editor;
    ed_save_record();
    const char *title = ed.out_name[0] ? ed.out_name : ed.item.name;
    ed.share_job = share_start(kind, ed.path.s, ed_share_info(), &ed.edit, title, ed.item.info.game, ed.item.key, host);
    ed.result_path[0] = ed.result_url[0] = 0;
    if (!ed.share_job) {
        str_set(ed.result, "Share is busy: try again in a moment.");
        ed.result_error = true;
    } else {
        ed.result[0] = 0;
    }
}

// Share as `kind` (a link on `host` when one is picked), with a first link to a free host asked about first
// (links_ask_first)
static void ed_share_ask(ShareKind kind, const char *host = 0)
{
    if (kind == SHARE_LINK && !links_any_host()) {
        lib_open_sharing();
        return;
    }
    if (kind != SHARE_LINK || !links_ask_first(ASK_EDITOR, 0, host)) ed_share(kind, host);
}

static bool ed_share_busy()
{
    Editor &ed = g_app.editor;
    if (!ed.share_job) return false;
    ShareJobView j = share_job(ed.share_job);
    return j.id && j.state < SHARE_DONE;
}

// Follows the Share under way, on whichever tab: once it's done, the result line says what went on the clipboard; a
// failure says why. The job id stays, so its tile can say "Copied".
static void ed_share_poll()
{
    Editor &ed = g_app.editor;
    static u32 seen; // the job whose end was taken
    if (!ed.share_job || seen == ed.share_job) return;
    ShareJobView j = share_job(ed.share_job);
    if (j.id && j.state < SHARE_DONE) {
        ui_wake();
        return;
    }
    seen = ed.share_job;
    ed.result_path[0] = ed.result_url[0] = 0;
    if (j.state == SHARE_DONE && j.kind == SHARE_LINK) {
        str_set(ed.result_url, j.url);
        if (j.error[0]) str_set(ed.result, j.error);
        else if (j.expires_h) snprintf(ed.result, sizeof ed.result, "Link copied, expires in %d h.", j.expires_h);
        else str_set(ed.result, "Link copied, ready to paste.");
        ed.result_error = j.error[0] != 0;
    } else if (j.state == SHARE_DONE) {
        str_set(ed.result_path, j.path);
        str_set(ed.result, j.error[0] ? j.error : "Copied, ready to paste.");
        ed.result_error = j.error[0] != 0;
        editor_refresh_shares();
        ed.share_page = 0;
    } else {
        snprintf(ed.result, sizeof ed.result, "Share failed: %s", j.id ? j.error : "it was lost");
        ed.result_error = true;
    }
}

// ---- The selection, copy and paste, undo ----

// A lane the timeline shows for this file (copied keys aren't pasted onto a track it doesn't have).
static bool ed_lane_shown(i32 l)
{
    if (l == 0 || l == 1 + EDIT_MASTER || l >= ED_COLOR) return true;
    i32 tracks[MEDIA_MAX_TRACKS];
    i32 n = ed_tracks(g_app.editor.item.info, tracks, MEDIA_MAX_TRACKS);
    for (i32 i = 0; i < n; i++)
        if (tracks[i] == l - 1) return true;
    return false;
}

static void ed_sel_clear()
{
    memset(g_app.editor.sel, 0, sizeof g_app.editor.sel);
}

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
    bool was_adv = adv_on();
    ed.edit = from[--*from_n];
    ed.settled = ed.edit;
    ed.dirty = true;
    ed.heard_set = false;
    ed.play_piece = -1;
    ed_sel_clear();
    if (was_adv != adv_on()) { // Undo went back across Simple / Advanced
        f64 u = was_adv ? ed.adv_u : ed.player.pos;
        ed.tracks_ready = false;
        ed.adv_playing = false;
        ed.adv_sel_count = 0;
        player_pause(&ed.player);
        ed_seek_u(u);
    }
    if (adv_on()) {
        f64 len = adv_len(ed.edit);
        if (ed.adv_u > len) ed.adv_u = len;
        ed.adv_key = -1;
        adv_resync();
    }
}

// ---- Pieces: playing them one after another ----

// Play or pause. On the joined pieces, Play from the end or from a cut starts again at the first piece.
static void ed_toggle()
{
    Editor &ed = g_app.editor;
    Player &p = ed.player;
    if (adv_on()) return adv_toggle();
    f64 u;
    if (p.paused && ed_joined() &&
        (!piece_out(ed.edit, ed_duration(), p.pos, &u) || ed_play_u() >= ed_axis_len() - 0.05))
        ed_seek_u(0);
    player_toggle(&p);
}

// While the joined pieces play: at a piece's end, on to the next one's start (no seek when it carries straight on
// in the file), and a pause at the last one's end.
static void ed_follow_pieces()
{
    Editor &ed = g_app.editor;
    Player &p = ed.player;
    if (adv_on() || !ed_joined() || !p.loaded) {
        ed.play_piece = -1;
        return;
    }
    EditPiece pc[EDIT_MAX_PIECES];
    f64 dur = ed_duration(), pos = p.pos, u;
    i32 np = pieces_of(ed.edit, dur, pc), cur = ed.play_piece;
    if (cur >= np) cur = -1;
    bool playing = !p.paused && !p.scrubbing && !p.has_seek && !p.seeking;
    if (playing && cur >= 0 && pos >= pc[cur].to - 0.02 && pos < pc[cur].to + 0.25) {
        if (cur + 1 < np) {
            if (fabs(pc[cur + 1].from - pc[cur].to) > 0.02) player_seek(&p, pc[cur + 1].from);
            cur++;
        } else {
            player_pause(&p);
            player_seek(&p, pc[cur].to);
        }
    } else if (cur < 0 || pos < pc[cur].from - 0.02 || pos > pc[cur].to + 0.02) {
        i32 at = -1;
        if (piece_out(ed.edit, dur, pos, &u, &at)) {
            cur = at;
        } else if (playing) { // in a cut: on to the piece that starts next in the file, or stop
            i32 next = -1;
            for (i32 k = 0; k < np; k++)
                if (pc[k].from > pos && (next < 0 || pc[k].from < pc[next].from)) next = k;
            if (next >= 0) {
                player_seek(&p, pc[next].from);
                cur = next;
            } else {
                cur = np - 1;
                player_pause(&p);
                player_seek(&p, pc[cur].to);
            }
        }
    }
    ed.play_piece = cur;
}

// ---- The view: which stretch of the axis the lanes show ----

static void ed_view_clamp()
{
    Editor &ed = g_app.editor;
    f64 dur = ed_axis_len();
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

// Kept in range and caught by 100% (or a whole zoom) within a few pixels; vh is the lane's value height.
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
    ED_DRAG_NONE, ED_DRAG_IN, ED_DRAG_OUT, ED_DRAG_RANGE, ED_DRAG_NEW, ED_DRAG_MENU, ED_DRAG_SEEK, ED_DRAG_KEYS,
    ED_DRAG_LEVEL, ED_DRAG_STRETCH, ED_DRAG_PAN, ED_DRAG_BAR, ED_DRAG_BAR_IN, ED_DRAG_BAR_OUT
};

// The selected keys moved by the drag so far (dt on the axis), from where they were when it began. The grabbed key
// lands on a frame (or the playhead, with Shift) and its value snaps; the rest keep their places relative to it.
static void ed_drag_keys(f64 dt, f64 dfrac, f32 px_per_s, f32 vh, bool time_only, bool value_only)
{
    Editor &ed = g_app.editor;
    f64 dur = ed_duration();
    ed.edit = ed.drag_from;
    memcpy(ed.sel, ed.drag_sel, sizeof ed.sel);
    LaneKey g = ln_get(ed.drag_from, ed.drag_lane, ed.drag_key);
    if (value_only) dt = 0;
    if (time_only) dfrac = 0;
    f64 nt = ed_joined() ? ed_s(ed_u(g.t) + dt) : g.t + dt, fr = ed_frame();
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

// A sound lane's kind, by what its title says it is: chat, game, an app, or the mic; the master is the mix.
static TrackKind ed_track_kind(const char *title, i32 slot)
{
    if (slot == EDIT_MASTER) return KIND_MIX;
    size_t n = strlen(title);
    if (n >= 6 && !strcmp(title + n - 6, "(chat)")) return KIND_CHAT;
    if (n >= 6 && !strcmp(title + n - 6, "(game)")) return KIND_GAME;
    if (!strcmp(title, "Mic")) return KIND_MIC;
    return KIND_APP;
}

static u32 ed_rgba(u32 rgb, i32 alpha) { return IM_COL32((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255, alpha); }

struct EdRow {
    i32         lane;  // 0 zoom, 1 + slot, ED_COLOR + colour
    i32         wave;  // the waveform's track, -1 none
    i32         slot;  // the volume slot, -1 for the zoom and the colour
    f32         y0, y1;
    bool        scrolls; // one of the sound tracks, in the scrolled area
    const char *name;
};

// The timeline: a bar for the view, a ruler, the video with the trim or the pieces, the zoom's lane, a lane for each sound track
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

// The preview's colour: the picture drawn again with a pixel shader in place of ImGui's, doing the sums of ffmpeg's eq
// filter (save.cpp, color_filter). eq works on the coded, limited-range luma around its middle (0.5 coded is 0.509 of
// the full range) and adds brightness in coded units (255/219 of the full range); it scales the chroma.
static ID3D11PixelShader *g_color_ps;
static ID3D11Buffer *g_color_cb;
static bool g_color_failed;

static bool color_shader_ready()
{
    if (g_color_ps && g_color_cb) return true;
    if (g_color_failed || !g_gpu.device) return false;
    static const char *const src =
        "cbuffer C : register(b0) { float4 eqv; };\n"
        "struct PS_INPUT { float4 pos : SV_POSITION; float4 col : COLOR0; float2 uv : TEXCOORD0; };\n"
        "sampler sampler0;\n"
        "Texture2D texture0;\n"
        "float4 main(PS_INPUT i) : SV_Target {\n"
        "  float4 c = i.col * texture0.Sample(sampler0, i.uv);\n"
        "  float y = dot(c.rgb, float3(0.2126, 0.7152, 0.0722));\n"
        "  float3 ch = c.rgb - y;\n"
        "  y = eqv.y * (y - 0.509) + 0.509 + eqv.x * 1.164;\n"
        "  return float4(saturate(y + ch * eqv.z), c.a);\n"
        "}\n";
    ID3DBlob *blob = 0, *err = 0;
    HRESULT hr = D3DCompile(src, strlen(src), 0, 0, 0, "main", "ps_4_0", 0, 0, &blob, &err);
    if (FAILED(hr) && err) log_line("editor: colour shader: %s", (const char *)err->GetBufferPointer());
    if (err) err->Release();
    if (SUCCEEDED(hr)) hr = g_gpu.device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), 0, &g_color_ps);
    if (blob) blob->Release();
    D3D11_BUFFER_DESC d = {};
    d.ByteWidth = 16;
    d.Usage = D3D11_USAGE_DYNAMIC;
    d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (SUCCEEDED(hr)) hr = g_gpu.device->CreateBuffer(&d, 0, &g_color_cb);
    if (FAILED(hr)) {
        log_line("editor: no colour preview (0x%08lx)", (unsigned long)hr);
        g_color_failed = true;
        return false;
    }
    return true;
}

// Runs while ImGui draws: the next draw (the picture) goes through the colour shader with the values given.
static void color_shader_on(const ImDrawList *, const ImDrawCmd *cmd)
{
    ID3D11DeviceContext *ctx = g_gpu.context;
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ctx->Map(g_color_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, cmd->UserCallbackData, 16);
        ctx->Unmap(g_color_cb, 0);
    }
    ctx->PSSetShader(g_color_ps, 0, 0);
    ctx->PSSetConstantBuffers(0, 1, &g_color_cb);
}

// The picture from a to b, with the colour at the playhead.
static void ed_draw_picture(const Player &p, ImVec2 a, ImVec2 b)
{
    Editor &ed = g_app.editor;
    const EditRecord &e = ed.edit;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    i32 pic = adv_on() ? item_at(e, 0, ed.adv_u) : -1;
    if (adv_on() && pic < 0) { // a gap: black
        dl->AddRectFilled(a, b, IM_COL32(0, 0, 0, 255));
        return;
    }
    player_draw(p, a, b);
    f32 eqv[4] = {};
    bool any = false;
    for (i32 c = 0; c < EDIT_COLORS; c++) {
        f64 v = pic >= 0 ? item_level_at(e, e.items[pic].id, IKEY_COLOR + c, p.pos) : color_at(e, c, p.pos);
        if (fabs(v - 1) > 0.001) any = true;
        eqv[c] = (f32)color_eq(c, v);
    }
    if (any && p.srv && p.frame_ready && color_shader_ready()) {
        dl->AddCallback(color_shader_on, eqv, sizeof eqv);
        dl->AddImage((ImTextureID)(intptr_t)p.srv, a, b);
        dl->AddCallback(ImDrawCallback_ResetRenderState, 0);
    }
    // Fades to black over it (a crossfade shows as a cut: one video plays at a time)
    f64 g = pic >= 0 ? item_fade_gain(e, ed_duration(), pic, p.pos) : fade_gain(e, ed_duration(), ed_play_piece(), p.pos);
    if (g < 0.999) dl->AddRectFilled(a, b, IM_COL32(0, 0, 0, (i32)((1 - g) * 255)));
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
    // The axis (u) is the joined pieces' or the file's (ed_joined); keys and pieces are in the file's time (t).
    bool joined = ed_joined();
    f64 alen = ed_axis_len(), play_u = ed_play_u();
    EditPiece pc[EDIT_MAX_PIECES];
    f64 pstart[EDIT_MAX_PIECES + 1] = {}; // where each piece starts on the axis
    i32 np = 0;
    auto read_pieces = [&]() {
        np = pieces_of(ed.edit, dur, pc);
        for (i32 i = 0; i < np; i++) pstart[i + 1] = pstart[i] + pc[i].to - pc[i].from;
        alen = ed_axis_len();
    };
    read_pieces();
    ed_view_clamp();
    if (!p.paused && ed.lane_drag == ED_DRAG_NONE && (play_u < ed.view_from || play_u > ed.view_from + ed.view_span)) {
        ed.view_from = play_u - ed.view_span * 0.05; // follow the playhead a page at a time
        ed_view_clamp();
    }
    f64 from = ed.view_from, span = ed.view_span;

    f32 gut = kit_px(176), gap = kit_px(3), round = kit_px(6);
    f32 bx0 = o.x + gut, bx1 = o.x + w, bw = bx1 - bx0;
    if (bw < kit_px(40)) return;
    f32 bar_h = wa_viewbar_h(), ruler_h = wa_ruler_h(), video_h = kit_px(46), lane_h = ed_lane_h(), vh = lane_h - 2 * ln_pad();
    auto xu_of = [&](f64 u) { return bx0 + (f32)((u - from) / span) * bw; };
    auto u_of = [&](f32 x) {
        f64 u = from + (f64)(x - bx0) / bw * span;
        return u < 0 ? 0 : u > alen ? alen : u;
    };
    // A moment in the file on screen (far off it when it's cut out), and the moment under x.
    auto x_of = [&](f64 t) {
        f64 u = ed_u(t);
        return u < 0 ? -1e7f : xu_of(u);
    };
    auto t_of = [&](f32 x) { return ed_s(u_of(x)); };
    f32 mx = io.MousePos.x, my = io.MousePos.y;
    bool over = ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(o, ImVec2(o.x + w, o.y + h), false);

    i32 tracks[MEDIA_MAX_TRACKS];
    i32 nt = ed_tracks(ed.item.info, tracks, MEDIA_MAX_TRACKS);
    f32 bar_y = o.y, ruler_y = bar_y + bar_h + kit_px(6), video_y = ruler_y + ruler_h;
    f32 zoom_y = video_y + video_h + gap, color_y = zoom_y + lane_h + gap, mix_y = color_y + lane_h + gap;
    f32 area0 = nt ? mix_y + lane_h + gap : mix_y;
    f32 content = nt * (lane_h + gap);
    f32 area1 = o.y + h;
    if (area1 > area0 + content) area1 = area0 + content;
    if (area1 < area0) area1 = area0;
    f32 max_scroll = content - (area1 - area0) > 0 ? content - (area1 - area0) : 0;
    f32 lanes_y1 = nt ? area1 : color_y + lane_h;

    // The wheel pans; with Alt it zooms around the mouse, with Alt and Shift around the playhead. Shift alone sets
    // the lanes' height, and over the sound tracks' names the wheel scrolls them (when they don't all fit).
    if (over && (io.MouseWheel != 0 || io.MouseWheelH != 0)) {
        f32 wh = io.MouseWheel;
        if (wh && io.KeyAlt) {
            ed_view_zoom(pow(0.75, wh), !io.KeyShift && mx >= bx0 && mx < bx1 ? u_of(mx) : play_u);
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

    // The view bar: the whole axis, with a window on the work area (the part shown). Drag the window to pan,
    // either end to zoom (with Shift around the playhead, which keeps its place on screen), beside it to bring it
    // there; double-click shows everything.
    ImGui::SetCursorScreenPos(ImVec2(bx0, bar_y - kit_px(4)));
    ImGui::InvisibleButton("##view", ImVec2(bw, bar_h + kit_px(8)));
    {
        auto bx = [&](f64 t) { return bx0 + (f32)(t / alen) * bw; };
        f32 tx0 = bx(ed.view_from), tx1 = bx(ed.view_from + ed.view_span), grip = wa_grab();
        static f64 held_from, held_to, held_play;
        if (ImGui::IsItemActivated()) {
            ed.drag_mx = mx;
            held_from = ed.view_from;
            held_to = ed.view_from + ed.view_span;
            held_play = play_u;
            if (fabsf(mx - tx0) <= grip && mx < (tx0 + tx1) / 2) {
                ed.lane_drag = ED_DRAG_BAR_IN;
                ed.drag_t = ed.view_from + ed.view_span;
            } else if (fabsf(mx - tx1) <= grip) {
                ed.lane_drag = ED_DRAG_BAR_OUT;
                ed.drag_t = ed.view_from;
            } else {
                if (mx < tx0 || mx > tx1) {
                    ed.view_from = (mx - bx0) / bw * alen - ed.view_span / 2;
                    ed_view_clamp();
                }
                ed.lane_drag = ED_DRAG_BAR;
                ed.drag_t = ed.view_from;
            }
        }
        if (ImGui::IsItemActive()) {
            f64 bt = (f64)(mx - bx0) / bw * alen;
            bt = bt < 0 ? 0 : bt > alen ? alen : bt;
            f64 a, z;
            bool end = ed.lane_drag == ED_DRAG_BAR_IN || ed.lane_drag == ED_DRAG_BAR_OUT;
            if (ed.lane_drag == ED_DRAG_BAR) ed.view_from = ed.drag_t + (mx - ed.drag_mx) / bw * alen;
            else if (end && io.KeyShift &&
                     wa_scale(held_play, held_from, held_to, ed.lane_drag == ED_DRAG_BAR_IN, bt, 0, alen, 0.5, &a, &z)) {
                ed.view_from = a;
                ed.view_span = z - a;
            } else if (ed.lane_drag == ED_DRAG_BAR_IN && bt < ed.drag_t) {
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
            ed.view_span = alen;
        }
        bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
        if (hot && (fabsf(mx - tx0) <= grip || fabsf(mx - tx1) <= grip))
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        wa_draw_viewbar(dl, bx0, bx1, bar_y, bx(ed.view_from), bx(ed.view_from + ed.view_span), hot,
                        bx(joined ? 0 : ed_in()), bx(joined ? alen : ed_out()), bx(play_u));
    }
    from = ed.view_from;
    span = ed.view_span;

    // Shift and a drag on the ruler or the picture's strip draws a new in and out, as on the Timeline. On the ruler
    // their ends and the band between are taken hold of; everywhere else a press moves the playhead. Shift on an end
    // lands it on the clip's start or end or the playhead when close; the band lands on the playhead when close.
    // Dragging an end or the band shows its frame, and the playhead goes back once it's let go. On joined pieces
    // there's no in and out; in Advanced the strip holds the pieces: a click selects one (Shift or Ctrl adds more),
    // an edge drags to trim it (Shift catches the playhead and the other pieces' edges), and the body drags it to
    // another place in the order.
    ImGui::SetCursorScreenPos(ImVec2(bx0, ruler_y));
    ImGui::InvisibleButton("##scrub", ImVec2(bw, ruler_h + video_h), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    bool scrub_hot = ImGui::IsItemHovered();
    f64 mouse_t = t_of(mx);
    bool on_ruler = my >= ruler_y && my < video_y;
    f32 grab = wa_grab();
    bool near_in = !joined && on_ruler && fabsf(mx - x_of(ed_in())) <= grab;
    bool near_out = !joined && on_ruler && !near_in && fabsf(mx - x_of(ed_out())) <= grab;
    bool on_band = !joined && on_ruler && !near_in && !near_out && mouse_t > ed_in() && mouse_t < ed_out();
    {
        const f64 least = 0.2;
        static f64 grab_play, grab_in, grab_out, grab_off;
        auto near_play = [&](f64 t) { return fabsf(x_of(t) - x_of(grab_play)) <= grab; };
        // With Shift, an end catches the nearest edge that comes close on screen: the clip's start or end, or the
        // playhead (where it was when the drag began)
        auto snap_edge = [&](f64 t) {
            f32 best = kit_px(10);
            f64 to = t;
            const f64 edges[] = {0.0, dur, grab_play};
            for (i32 i = 0; i < COUNT(edges); i++) {
                f32 d = fabsf(x_of(edges[i]) - x_of(t));
                if (d <= best) { best = d; to = edges[i]; }
            }
            return to;
        };
        auto set = [&](f64 a, f64 z) { ed_set_trim(a, z); };
        if (ImGui::IsItemActivated()) {
            ed.drag_mx = mx;
            ed.drag_moved = false;
            grab_play = p.pos;
            grab_in = ed_in();
            grab_out = ed_out();
            grab_off = mouse_t - grab_in;
            ed.drag_t = mouse_t;
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) ed.lane_drag = ED_DRAG_MENU;
            else if (near_in) ed.lane_drag = ED_DRAG_IN;
            else if (near_out) ed.lane_drag = ED_DRAG_OUT;
            else if (io.KeyShift && !joined) ed.lane_drag = ED_DRAG_NEW;
            else if (on_band) ed.lane_drag = ED_DRAG_RANGE;
            else {
                ed.lane_drag = ED_DRAG_SEEK;
                p.scrubbing = true;
                player_seek(&p, mouse_t);
            }
        }
        if (ImGui::IsItemActive() && ImGui::IsMousePosValid()) {
            if (fabsf(mx - ed.drag_mx) > kit_px(4)) ed.drag_moved = true;
            bool keep = io.KeyShift;
            f64 show = -1; // the frame to show while dragging
            if (ed.lane_drag == ED_DRAG_SEEK) {
                if (mouse_t < p.pos - 0.02 || mouse_t > p.pos + 0.02) ed_seek_u(u_of(mx));
            } else if (ed.lane_drag == ED_DRAG_NEW && ed.drag_moved) {
                f64 a = ImMin(ed.drag_t, mouse_t), z = ImMax(ed.drag_t, mouse_t);
                if (z - a < least) z = ImMin(a + least, dur);
                set(a, z);
            } else if (ed.lane_drag == ED_DRAG_RANGE && ed.drag_moved) {
                f64 len = grab_out - grab_in, to = mouse_t - grab_off;
                if (near_play(to)) to = grab_play;
                else if (near_play(to + len)) to = grab_play - len;
                to = to + len > dur ? dur - len : to < 0 ? 0 : to;
                set(to, to + len);
                show = to;
            } else if (ed.lane_drag == ED_DRAG_IN || ed.lane_drag == ED_DRAG_OUT) {
                f64 t = keep ? snap_edge(mouse_t) : mouse_t;
                if (ed.lane_drag == ED_DRAG_IN) {
                    set(ImMin(t, ed_out() - least), ed_out());
                    show = ed_in();
                } else {
                    set(ed_in(), ImMax(t, ed_in() + least));
                    show = ed_out();
                }
            }
            if (show >= 0) {
                p.scrubbing = true;
                player_seek(&p, show);
            } else if (ed.lane_drag != ED_DRAG_SEEK && p.scrubbing) {
                p.scrubbing = false;
                player_seek(&p, grab_play);
            }
        }
        if (ImGui::IsItemDeactivated()) {
            bool moved_ends = ed.lane_drag == ED_DRAG_IN || ed.lane_drag == ED_DRAG_OUT || ed.lane_drag == ED_DRAG_RANGE;
            if (ed.lane_drag == ED_DRAG_MENU && !ed.drag_moved && !joined && on_ruler && mouse_t >= ed_in() &&
                mouse_t <= ed_out())
                ImGui::OpenPopup("##inout_menu"); // a right click on the in and out
            if (p.scrubbing || moved_ends) {
                p.scrubbing = false;
                player_seek(&p, moved_ends ? grab_play : p.pos);
            }
            ed.lane_drag = ED_DRAG_NONE;
        }
        if (ed.lane_drag == ED_DRAG_IN || ed.lane_drag == ED_DRAG_OUT || (scrub_hot && ed.lane_drag == ED_DRAG_NONE && (near_in || near_out)))
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        else if (ed.lane_drag == ED_DRAG_RANGE || (scrub_hot && ed.lane_drag == ED_DRAG_NONE && on_band))
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    }
    lib_popup_style_push(6);
    if (ImGui::BeginPopup("##inout_menu")) {
        if (lib_menu_item(ICON_CLOSE, "Clear in and out")) ed_clear_trim();
        ImGui::EndPopup();
    }
    lib_popup_style_pop();
    read_pieces(); // as the drags and the menu left them
    f32 in_x = x_of(ed_in()), out_x = x_of(ed_out());

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
            f32 x = floorf(xu_of(i * minor));
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
        dl->PopClipRect();
    }

    // The video: frames from the file's strip at the picture's shape.
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
        // The transitions: black shading over the fades, a light band over each crossfade
        {
            Transitions tr = transitions_of(ed.edit, pc, np, dur);
            f64 a0 = joined ? 0 : pc[0].from, a1 = joined ? alen : pc[0].to;
            u32 dark = IM_COL32(0, 0, 0, 200), clear = IM_COL32(0, 0, 0, 0);
            auto shade = [&](f64 u0, f64 u1, bool dark_left) {
                f32 x0 = xu_of(u0), x1 = xu_of(u1);
                if (x1 - x0 >= 1)
                    dl->AddRectFilledMultiColor(ImVec2(x0, y0), ImVec2(x1, y1), dark_left ? dark : clear, dark_left ? clear : dark,
                                                dark_left ? clear : dark, dark_left ? dark : clear);
            };
            if (tr.fade_in > 0) shade(a0, a0 + tr.fade_in, true);
            if (tr.fade_out > 0) shade(a1 - tr.fade_out, a1, false);
            for (i32 i = 1; joined && i < np; i++) {
                f64 half = tr.len[i] / 2;
                if (tr.into[i] == TRANS_FADE) {
                    shade(pstart[i] - half, pstart[i], false);
                    shade(pstart[i], pstart[i] + half, true);
                } else if (tr.into[i] == TRANS_CROSS) {
                    dl->AddRectFilled(ImVec2(xu_of(pstart[i] - half), y0), ImVec2(xu_of(pstart[i] + half), y1), col32(COL_TEXT, 0.25f));
                }
            }
        }
        // The pieces: a gap at each cut
        if (joined) {
            f32 gw = kit_px(4);
            for (i32 i = 1; i < np; i++) {
                f32 x = xu_of(pstart[i]);
                dl->AddRectFilled(ImVec2(x - gw / 2, y0), ImVec2(x + gw / 2, y1), col32(COL_BG));
            }
        }
        dl->PopClipRect();
    }

    // The lanes, top to bottom: the zoom, the colour (the value picked in its gutter), the mix, the sound tracks
    // (scrolled).
    EdRow rows[MEDIA_MAX_TRACKS + 3];
    i32 nr = 0;
    rows[nr++] = {0, -1, -1, zoom_y, zoom_y + lane_h, false, "Zoom"};
    ed.color_pick = ed.color_pick < 0 || ed.color_pick >= EDIT_COLORS ? 0 : ed.color_pick;
    rows[nr++] = {ED_COLOR + ed.color_pick, -1, -1, color_y, color_y + lane_h, false, COLOR_NAMES[ed.color_pick]};
    if (nt) rows[nr++] = {1 + EDIT_MASTER, -1, EDIT_MASTER, mix_y, mix_y + lane_h, false, "Mix"};
    for (i32 i = 0; i < nt; i++) {
        f32 ry = area0 + i * (lane_h + gap) - ed.lane_scroll;
        rows[nr++] = {1 + tracks[i], i, tracks[i], ry, ry + lane_h, true, ed.item.info.tracks[tracks[i]]};
    }

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
                } else if (ln_is_volume(l) && !n) {
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
        } else if (ed.lane_drag == ED_DRAG_LEVEL && ed.drag_moved && ln_is_volume(l)) {
            ed.edit.boost[l - 1] = (f32)(ln_snap(l, ed.drag_t + dfrac * 2, vh) - 1);
            ed.dirty = true;
        } else if (ed.lane_drag == ED_DRAG_STRETCH && ed.drag_moved && l > 0 && ed.drag_key + 1 < ln_count(ed.edit, l)) {
            f32 v = (f32)ln_snap(l, ed.drag_t + dfrac * 2, vh);
            ln_levels(ed.edit, l)[ed.drag_key].v = ln_levels(ed.edit, l)[ed.drag_key + 1].v = v;
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
            static const char *const resets[EDIT_COLORS] = {"Reset the brightness", "Reset the contrast", "Reset the saturation"};
            const char *reset = !l ? "Reset the zoom" : l >= ED_COLOR ? resets[l - ED_COLOR] : "Reset the volume";
            if (lib_menu_item(ICON_UNDO, reset, true)) {
                if (l) ln_level_count(ed.edit, l) = 0;
                else ed.edit.key_count = 0;
                if (ln_is_volume(l)) ed.edit.boost[l - 1] = 0;
                ed.sel[l] = 0;
                ed.dirty = true;
            }
            if (ln_is_volume(l) && lib_menu_item(ICON_VOLUME, "Refresh the audio preview")) ed_refresh_wave();
        }
        ImGui::EndPopup();
    }
    lib_popup_style_pop();

    // Each lane: its gutter (mute, solo, a key at the playhead, the name and the value there), then its body.
    f64 in_s = ed_in(), out_s = ed_out();
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
        u32 kind_rgb = s >= 0 ? KIND_LOOK[ed_track_kind(row.name, s)].rgb : 0;
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
        } else if (l >= ED_COLOR) {
            ImGui::SetCursorScreenPos(ImVec2(gx, by));
            kit_next_tip("Which colour this lane shows");
            if (kit_icon_button(ICON_SLIDERS, "##pick", KIT_GHOST)) ImGui::OpenPopup("color_pick");
            lib_popup_style_push(6);
            if (ImGui::BeginPopup("color_pick")) {
                for (i32 c = 0; c < EDIT_COLORS; c++)
                    if (lib_menu_item(c == ed.color_pick ? ICON_CHECK : "", COLOR_NAMES[c])) ed.color_pick = c;
                ImGui::EndPopup();
            }
            lib_popup_style_pop();
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
            f32 end_x = xu_of(alen);
            for (f32 x = bx0; x < bx1 && x < end_x; x += step) {
                f64 t0 = t_of(x), t1 = t0 + step * span / bw, tm = (t0 + t1) / 2;
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
                bool kept = joined || (t1 > in_s && t0 < out_s);
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
            i32 npt = 0;
            f32 lstep = kit_px(3);
            for (f32 x = bx0; npt < COUNT(pts) - 1; x += lstep) {
                if (x > bx1) x = bx1;
                pts[npt++] = ImVec2(x, ln_y(l, ln_value_at(ed.edit, l, t_of(x)), y0, y1));
                if (x >= bx1) break;
            }
            u32 lc = !l ? col32(COL_BLUE) : l >= ED_COLOR ? col32(COL_WARN) : muted ? col32(COL_FAINT) : col32(COL_TEXT);
            dl->AddPolyline(pts, npt, lc, ImDrawFlags_None, kit_px(1.5f));
        }
        // The keys.
        for (i32 k = 0; k < ln_count(ed.edit, l); k++) {
            LaneKey q = ln_get(ed.edit, l, k);
            f32 kx = x_of(q.t), r0 = kit_px(5);
            if (kx < bx0 - 2 * r0 || kx > bx1 + 2 * r0) continue;
            ImVec2 c(kx, ln_y(l, q.v, y0, y1));
            bool on = (ed.sel[l] >> k) & 1;
            key_shape(dl, c, r0 + kit_px(2), q.ease, on ? IM_COL32_WHITE : IM_COL32(0, 0, 0, 170));
            key_shape(dl, c, r0, q.ease, col32(on ? COL_ACCENT : !l ? COL_BLUE : l >= ED_COLOR ? COL_WARN : muted ? COL_FAINT : 0xd6d9de));
        }
        dl->PopClipRect();
        if (row.scrolls) ImGui::PopClipRect();
    }

    // The in and out (or the cuts, faint across the lanes, and where a dragged piece goes), the playhead, and the
    // time under the mouse (or at the end being dragged) on the ruler
    dl->PushClipRect(ImVec2(bx0 - kit_px(6), ruler_y), ImVec2(bx1 + kit_px(6), lanes_y1), true);
    bool idle = scrub_hot && ed.lane_drag == ED_DRAG_NONE;
    if (!joined)
        wa_draw_inout(dl, in_x, out_x, ruler_y, video_y, lanes_y1, ed.lane_drag == ED_DRAG_IN || (idle && near_in),
                      ed.lane_drag == ED_DRAG_OUT || (idle && near_out));
    else {
        for (i32 i = 1; i < np; i++) {
            f32 x = floorf(xu_of(pstart[i]));
            dl->AddRectFilled(ImVec2(x, video_y + video_h), ImVec2(x + kit_px(1), lanes_y1), col32(COL_TEXT, 0.12f));
        }
    }
    play_u = ed_play_u();
    f32 px = xu_of(play_u);
    if (px >= bx0 - 1 && px <= bx1 + 1) wa_draw_playhead(dl, px, video_y, lanes_y1);
    f64 pill_u = -1;
    if (idle) pill_u = u_of(mx);
    else if (ed.lane_drag == ED_DRAG_IN || ed.lane_drag == ED_DRAG_RANGE) pill_u = ed_in();
    else if (ed.lane_drag == ED_DRAG_OUT) pill_u = ed_out();
    else if (ed.lane_drag == ED_DRAG_SEEK) pill_u = play_u;
    else if (ed.lane_drag == ED_DRAG_NEW && ed.drag_moved) pill_u = u_of(mx);
    if (pill_u >= 0) {
        f32 x = xu_of(pill_u);
        if (idle) dl->AddLine(ImVec2(x, video_y), ImVec2(x, lanes_y1), col32(COL_TEXT, 0.3f));
        char when[32];
        fmt_time(pill_u, when, sizeof when, true);
        ImGui::PushFont(g_font_small);
        wa_pill(dl, x, ruler_y + kit_px(2), bx0, bx1, when, col32(COL_HOVER), col32(COL_TEXT));
        ImGui::PopFont();
    }
    dl->PopClipRect();
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

#include "adv_editor.cpp"

// Open in Premiere Pro or DaVinci Resolve: the edit as a Final Cut Pro 7 XML (xmeml) sequence, which both import.
// A Simple edit is its pieces one after another, each a clip on every track; an Advanced edit is its items, each a
// clip on its own track where it was put. The video track has the zoom as Basic Motion keys, and each recorded
// track (the Mix left out) its volume as Audio Levels keys (a Simple edit's times the whole mix's); muted tracks are
// turned off. Eased curves are written as keys a fifth of a second apart. Key times count frames from the start of
// the file.
// Transitions are transitionitems on each track: a crossfade a Cross Dissolve (Cross Fade on the sound) centred on
// its cut; a fade dissolves to or from black (end-black, start-black).
// A stereo track is a linked pair of channel tracks marked Stereo (one stereo track once in Premiere), a mono Mic or
// chat one track marked Mono. Premiere numbers a file's audio by slot, each track taking as many slots as the widest
// one has channels: with any stereo track, track t's channel c is slot 2t + c + 1.
// TODO: colour in XML

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

// The times a curve is written at over a clip, a fifth of a second apart (wider on long clips).
static i32 xml_samples(f64 from, f64 to, f64 *at, i32 cap)
{
    f64 step = (to - from) / (cap - 1) > 0.2 ? (to - from) / (cap - 1) : 0.2;
    i32 n = 0;
    for (f64 t = from; t < to && n < cap - 1; t += step) at[n++] = t;
    at[n++] = to;
    return n;
}

static void xml_rate(FILE *f, i32 fps) { fprintf(f, "<rate><timebase>%d</timebase><ntsc>FALSE</ntsc></rate>", fps); }

// One clip on a track of the sequence: where it is (start, end) and what of the file it plays (in, out), in frames;
// its fades and the half of the crossfade into it, in frames; the source seconds it plays; the piece or item it is.
struct XmlClip {
    i64 start, end, in, out, fade_in, fade_out, cross_half;
    f64 from, to;
    i32 src;
};
enum { XML_MAX_CLIPS = EDIT_MAX_ITEMS > EDIT_MAX_PIECES ? EDIT_MAX_ITEMS : EDIT_MAX_PIECES };

// Writes <video name>.xml next to the video.
static bool ed_export_xml(Path *out)
{
    Editor &ed = g_app.editor;
    const MediaInfo &m = ed.item.info;
    const EditRecord &e = ed.edit;
    bool adv = e.mode == EDIT_ADVANCED;
    i32 fps = m.fps > 1 ? (i32)(m.fps + 0.5) : 60;
    auto fr = [&](f64 t) { return (i64)(t * fps + 0.5); };
    auto clip = [&](i64 start, f64 from, f64 to, i32 src) {
        XmlClip c = {};
        c.start = start, c.in = fr(from), c.out = fr(to), c.end = start + c.out - c.in;
        c.from = from, c.to = to, c.src = src;
        return c;
    };

    // The clips: [0] the picture, [1 + t] sound track t
    static XmlClip clips[1 + MEDIA_MAX_TRACKS][XML_MAX_CLIPS];
    i32 nc[1 + MEDIA_MAX_TRACKS] = {};
    i64 seq_len = 0;
    if (!adv) {
        EditPiece pc[EDIT_MAX_PIECES];
        i32 np = pieces_of(e, ed_duration(), pc);
        Transitions tr = transitions_of(e, pc, np, ed_duration());
        for (i32 i = 0; i < np; i++) {
            XmlClip &c = clips[0][i];
            c = clip(seq_len, pc[i].from, pc[i].to, i);
            seq_len = c.end;
            if (!i) c.fade_in = fr(tr.fade_in);
            if (i && tr.into[i] == TRANS_CROSS) c.cross_half = fr(tr.len[i] / 2);
            if (i && tr.into[i] == TRANS_FADE) c.fade_in = clips[0][i - 1].fade_out = fr(tr.len[i] / 2);
        }
        if (np) clips[0][np - 1].fade_out = fr(tr.fade_out);
        for (i32 t = 0; t <= m.track_count && t <= MEDIA_MAX_TRACKS; t++) {
            nc[t] = np;
            if (t) memcpy(clips[t], clips[0], np * sizeof **clips);
        }
    } else {
        for (i32 tk = 0; tk <= m.track_count && tk <= MEDIA_MAX_TRACKS; tk++) {
            i32 idx[EDIT_MAX_ITEMS], n = track_items(e, tk, idx);
            ItemFades fd[EDIT_MAX_ITEMS];
            track_fades(e, ed_duration(), idx, n, fd);
            for (i32 k = 0; k < n; k++) {
                const EditItem &it = e.items[idx[k]];
                XmlClip &c = clips[tk][k];
                c = clip(fr(it.start), it.from, it.to, idx[k]);
                c.fade_in = fr(fd[k].in), c.fade_out = fr(fd[k].out), c.cross_half = fr(fd[k].cross / 2);
                seq_len = ImMax(seq_len, c.end);
            }
            nc[tk] = n;
        }
    }
    auto item_keys = [&](const XmlClip &c, i32 what) {
        i32 first;
        return ikey_range(e, e.items[c.src].id, what, &first) > 0;
    };
    auto zoom_of = [&](const XmlClip &c, f64 s, f64 *z, f64 *x, f64 *y) {
        if (adv) item_zoom_at(e, e.items[c.src].id, s, z, x, y);
        else zoom_at(e, s, z, x, y);
    };
    auto level_of = [&](const XmlClip &c, i32 t, f64 s) {
        return adv ? item_level_at(e, e.items[c.src].id, IKEY_VOLUME, s) : vol_at(e, t, s) * vol_at(e, EDIT_MASTER, s);
    };
    bool master_keys = e.vol_count[EDIT_MASTER] > 0;

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
    i64 file_len = fr(m.duration);
    static f64 at[2000];
    // A transition from `s` to `t` frames on the sequence, as a dissolve aligned `align`.
    auto trans_item = [&](i64 s, i64 t, const char *align, bool audio) {
        if (t <= s) return;
        fputs("<transitionitem>", f);
        xml_rate(f, fps);
        fprintf(f, "<start>%lld</start><end>%lld</end><alignment>%s</alignment>", s, t, align);
        if (audio)
            fputs("<effect><name>Cross Fade (+3dB)</name><effectid>KGAudioTransCrossFade3dB</effectid>"
                  "<effecttype>transition</effecttype><mediatype>audio</mediatype></effect>", f);
        else
            fputs("<effect><name>Cross Dissolve</name><effectid>Cross Dissolve</effectid><effectcategory>Dissolve</effectcategory>"
                  "<effecttype>transition</effecttype><mediatype>video</mediatype></effect>", f);
        fputs("</transitionitem>\n", f);
    };
    // The transitions on a track between clip k - 1 and clip k (before the first with k 0, after the last with k n).
    auto trans_before = [&](const XmlClip *cs, i32 n, i32 k, bool audio) {
        if (k > 0) trans_item(cs[k - 1].end - cs[k - 1].fade_out, cs[k - 1].end, "end-black", audio);
        if (k < n) {
            trans_item(cs[k].start - cs[k].cross_half, cs[k].start + cs[k].cross_half, "center", audio);
            trans_item(cs[k].start, cs[k].start + cs[k].fade_in, "start-black", audio);
        }
    };

    fputs("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE xmeml>\n<xmeml version=\"5\">\n<sequence id=\"sequence-1\">\n<name>", f);
    xml_text(f, ed.item.name);
    fprintf(f, "</name>\n<duration>%lld</duration>", seq_len);
    xml_rate(f, fps);
    fputs("\n<media>\n<video>\n<format><samplecharacteristics>", f);
    xml_rate(f, fps);
    fprintf(f, "<width>%d</width><height>%d</height><pixelaspectratio>square</pixelaspectratio></samplecharacteristics></format>\n",
            m.width, m.height);
    // The file is written in full in the first clip, then referred to by its id
    auto chans = [&](i32 t) { return m.channels[t] == 1 ? 1 : 2; };
    bool file_done = false;
    auto clip_head = [&](i32 id, const XmlClip &c, bool enabled, const char *channel_type) {
        fprintf(f, "<clipitem id=\"clipitem-%d\"%s%s%s>\n<name>", id, channel_type ? " premiereChannelType=\"" : "",
                channel_type ? channel_type : "", channel_type ? "\"" : "");
        xml_text(f, file_name);
        fprintf(f, "</name><enabled>%s</enabled><duration>%lld</duration>", enabled ? "TRUE" : "FALSE", file_len);
        xml_rate(f, fps);
        fprintf(f, "<start>%lld</start><end>%lld</end><in>%lld</in><out>%lld</out>\n", c.start, c.end, c.in, c.out);
        if (file_done) {
            fputs("<file id=\"file-1\"/>\n", f);
            return;
        }
        file_done = true;
        fputs("<file id=\"file-1\"><name>", f);
        xml_text(f, file_name);
        fputs("</name><pathurl>", f);
        xml_pathurl(f, src);
        fputs("</pathurl>", f);
        xml_rate(f, fps);
        fprintf(f, "<duration>%lld</duration>\n<media><video><samplecharacteristics>", file_len);
        xml_rate(f, fps);
        fprintf(f, "<width>%d</width><height>%d</height></samplecharacteristics></video>", m.width, m.height);
        for (i32 t = 0; t < m.track_count; t++)
            fprintf(f, "<audio><samplecharacteristics><depth>16</depth><samplerate>48000</samplerate></samplecharacteristics>"
                    "<channelcount>%d</channelcount></audio>", chans(t));
        fputs("</media></file>\n", f);
    };

    // The video
    fputs("<track>\n", f);
    for (i32 k = 0; k < nc[0]; k++) {
        const XmlClip &c = clips[0][k];
        trans_before(clips[0], nc[0], k, false);
        clip_head(1 + k, c, true, 0);
        if (adv ? item_keys(c, IKEY_ZOOM) : e.key_count > 0) {
            // Scale in percent; Center moves the picture's middle, in fractions of the frame from the middle
            i32 na = xml_samples(c.from, c.to, at, COUNT(at));
            fputs("<filter><effect><name>Basic Motion</name><effectid>basic</effectid><effectcategory>motion</effectcategory>"
                  "<effecttype>motion</effecttype><mediatype>video</mediatype>\n"
                  "<parameter><parameterid>scale</parameterid><name>Scale</name><valuemin>0</valuemin><valuemax>1000</valuemax>"
                  "<value>100</value>\n", f);
            for (i32 q = 0; q < na; q++) {
                f64 z, x, y;
                zoom_of(c, at[q], &z, &x, &y);
                fprintf(f, "<keyframe><when>%lld</when><value>%.2f</value></keyframe>\n", fr(at[q]), z * 100);
            }
            fputs("</parameter>\n<parameter><parameterid>center</parameterid><name>Center</name>"
                  "<value><horiz>0</horiz><vert>0</vert></value>\n", f);
            for (i32 q = 0; q < na; q++) {
                f64 z, x, y;
                zoom_of(c, at[q], &z, &x, &y);
                // The kept square of the frame, held inside it as the picture shows it
                f32 w = (f32)(1 / z), ox = (f32)(x - w / 2), oy = (f32)(y - w / 2);
                ox = ox < 0 ? 0 : ox > 1 - w ? 1 - w : ox;
                oy = oy < 0 ? 0 : oy > 1 - w ? 1 - w : oy;
                f64 s = w > 0.001f ? 1.0 / w : 1;
                f64 cx = (0.5 - (ox + (ox + w)) / 2) * s, cy = (0.5 - (oy + (oy + w)) / 2) * s;
                fprintf(f, "<keyframe><when>%lld</when><value><horiz>%.4f</horiz><vert>%.4f</vert></value></keyframe>\n",
                        fr(at[q]), cx, cy);
            }
            fputs("</parameter></effect></filter>\n", f);
        }
        fputs("</clipitem>\n", f);
    }
    trans_before(clips[0], nc[0], nc[0], false);
    fputs("</track>\n</video>\n<audio>\n", f);

    // The audio: each recorded track's channels, from the same file, a track each. The clips are numbered on from
    // the video's, sequence track by sequence track (seq_base).
    i32 tracks[MEDIA_MAX_TRACKS];
    i32 nt = ed_tracks(m, tracks, MEDIA_MAX_TRACKS);
    i32 seq_base[2 * MEDIA_MAX_TRACKS + 2] = {}, ns = 1, slots = 1;
    seq_base[1] = 1 + nc[0];
    for (i32 ti = 0; ti < nt; ti++)
        for (i32 c = 0; c < chans(tracks[ti]); c++, ns++) seq_base[ns + 1] = seq_base[ns] + nc[1 + tracks[ti]];
    for (i32 t = 0; t < m.track_count; t++) slots = ImMax(slots, chans(t));
    i32 seq_track = 1;
    for (i32 ti = 0; ti < nt; ti++) {
        i32 t = tracks[ti], n = chans(t);
        const XmlClip *cs = clips[1 + t];
        i32 ncs = nc[1 + t];
        for (i32 ch = 0; ch < n; ch++) {
            fprintf(f, "<track premiereTrackType=\"%s\" currentExplodedTrackIndex=\"%d\" totalExplodedTrackCount=\"%d\">\n",
                    n == 2 ? "Stereo" : "Mono", ch, n);
            for (i32 k = 0; k < ncs; k++) {
                const XmlClip &c = cs[k];
                trans_before(cs, ncs, k, true);
                clip_head(seq_base[seq_track + ch] + k, c, !e.muted[t] && !e.muted[EDIT_MASTER], n == 2 ? "stereo" : "mono");
                fprintf(f, "<sourcetrack><mediatype>audio</mediatype><trackindex>%d</trackindex></sourcetrack>\n", slots * t + ch + 1);
                if (n == 2) // the pair stays together
                    for (i32 l = 0; l < 2; l++)
                        fprintf(f, "<link><linkclipref>clipitem-%d</linkclipref><mediatype>audio</mediatype><trackindex>%d</trackindex>"
                                "<clipindex>%d</clipindex><groupindex>1</groupindex></link>\n", seq_base[seq_track + l] + k,
                                seq_track + l, k + 1);
                fputs("<filter><effect><name>Audio Levels</name><effectid>audiolevels</effectid><effectcategory>audiolevels"
                      "</effectcategory><effecttype>audiolevels</effecttype><mediatype>audio</mediatype>\n<parameter>"
                      "<parameterid>level</parameterid><name>Level</name><valuemin>0</valuemin><valuemax>3.98109</valuemax>", f);
                fprintf(f, "<value>%.4f</value>\n", level_of(c, t, c.from));
                if (adv ? item_keys(c, IKEY_VOLUME) : e.vol_count[t] || master_keys) {
                    i32 na = xml_samples(c.from, c.to, at, COUNT(at));
                    for (i32 q = 0; q < na; q++)
                        fprintf(f, "<keyframe><when>%lld</when><value>%.4f</value></keyframe>\n", fr(at[q]), level_of(c, t, at[q]));
                }
                fputs("</parameter></effect></filter>\n</clipitem>\n", f);
            }
            trans_before(cs, ncs, ncs, true);
            fputs("</track>\n", f);
        }
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

// Simple's controls: zoom, colour and transitions at the playhead, and the selected keys.
static void ed_simple_panel(f32 w)
{
    Editor &ed = g_app.editor;
    Player &p = ed.player;
    f32 fh = ImGui::GetFrameHeight(), sp = ImGui::GetStyle().ItemSpacing.x;
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
    // How the picture shows it; the result is the same either way.
    static const char *const views[] = {"Crop", "Footage"};
    if (kit_segmented("##zoom_view", views, 2, &g_app.config.editor_zoom_view, (w - kit_px(6)) / 2))
        config_save(g_app.config, g_app.paths);
    kit_dim(g_app.config.editor_zoom_view ? "Drag the footage to aim" : "Drag the frame to aim");
    ImGui::Dummy(ImVec2(1, kit_px(8)));

    // Colour at the playhead: a slider for each value that keys it there (double-click puts it back to 100%); the
    // one last moved is the one the timeline's Colour lane shows.
    ed_caption("COLOUR");
    f32 cname_w = kit_px(84), lh = ImGui::GetTextLineHeight();
    for (i32 c = 0; c < EDIT_COLORS; c++) {
        ImGui::PushID(c);
        i32 l = ED_COLOR + c;
        f32 cv = (f32)color_at(ed.edit, c, p.pos);
        ImVec2 cp = ImGui::GetCursorScreenPos();
        dl->AddText(ImVec2(cp.x, floorf(cp.y + (fh - lh) / 2)), col32(c == ed.color_pick ? COL_TEXT : COL_DIM), COLOR_NAMES[c]);
        ImGui::Dummy(ImVec2(cname_w, fh));
        ImGui::SameLine(0, 0);
        bool reset = false;
        if (lib_slider("##color", &cv, 0.f, 2.f, w - cname_w - val_w, fh)) {
            if (cv > 0.97f && cv < 1.03f) cv = 1; // caught by 100%
            ed.color_pick = c;
            reset = true;
        }
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            cv = 1;
            reset = true;
        }
        if (reset) {
            i32 k = ln_key_at(ed.edit, l, p.pos);
            if (k < 0) k = ed_put_key(l, p.pos, cv);
            if (k >= 0) {
                ed.edit.color[c][k].v = cv;
                ed.dirty = true;
            }
        }
        ImGui::SameLine(0, 0);
        char ct[16];
        snprintf(ct, sizeof ct, "%.0f%%", cv * 100);
        ImVec2 vp = ImGui::GetCursorScreenPos(), vs = ImGui::CalcTextSize(ct);
        dl->AddText(ImVec2(vp.x + val_w - vs.x, floorf(vp.y + (fh - vs.y) / 2)), col32(COL_TEXT), ct);
        ImGui::Dummy(ImVec2(val_w, fh));
        ImGui::PopID();
    }
    ImGui::Dummy(ImVec2(1, kit_px(8)));

    // Transitions: fades from black at the start and to black at the end, then how the cut nearest the playhead
    // goes from one piece into the next. Lengths in seconds, 0 being none.
    ed_caption("TRANSITIONS");
    {
        EditPiece pc[EDIT_MAX_PIECES];
        i32 np = pieces_of(ed.edit, ed_duration(), pc);
        auto length_row = [&](const char *name, const char *id, f32 *len, bool on) {
            ImVec2 cp = ImGui::GetCursorScreenPos();
            dl->AddText(ImVec2(cp.x, floorf(cp.y + (fh - lh) / 2)), col32(on ? COL_TEXT : COL_FAINT), name);
            ImGui::Dummy(ImVec2(cname_w, fh));
            ImGui::SameLine(0, 0);
            ImGui::BeginDisabled(!on);
            f32 v = *len;
            if (lib_slider(id, &v, 0.f, 3.f, w - cname_w - val_w, fh)) {
                *len = v < TRANS_LEAST ? 0 : floorf(v * 20 + 0.5f) / 20; // in twentieths of a second
                ed.dirty = true;
            }
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                *len = 0;
                ed.dirty = true;
            }
            ImGui::EndDisabled();
            ImGui::SameLine(0, 0);
            char lt[16];
            if (*len > 0) snprintf(lt, sizeof lt, "%.2f s", *len);
            else str_set(lt, "Off");
            ImVec2 vp = ImGui::GetCursorScreenPos(), vs = ImGui::CalcTextSize(lt);
            dl->AddText(ImVec2(vp.x + val_w - vs.x, floorf(vp.y + (fh - vs.y) / 2)), col32(on ? COL_TEXT : COL_FAINT), lt);
            ImGui::Dummy(ImVec2(val_w, fh));
        };
        length_row("Fade in", "##fade_in", &ed.edit.fade_in, true);
        length_row("Fade out", "##fade_out", &ed.edit.fade_out, true);
        // The cut nearest the playhead (none with one piece)
        i32 cut = -1;
        f64 best = 0, u = ed_play_u();
        for (i32 i = 1; i < np; i++) {
            f64 d = fabs(piece_start(ed.edit, ed_duration(), i) - u);
            if (cut < 0 || d < best) cut = i, best = d;
        }
        static const char *const kinds[TRANS_COUNT] = {"Cut", "Fade", "Crossfade"};
        static f32 no_len;
        i32 kind = cut > 0 ? ed.edit.pieces[cut].trans : -1;
        ImGui::BeginDisabled(cut < 0);
        if (kit_segmented("##trans", kinds, TRANS_COUNT, &kind, (w - kit_px(6)) / (f32)TRANS_COUNT) && cut > 0) {
            EditPiece &q = ed.edit.pieces[cut];
            q.trans = (u8)kind;
            if (kind == TRANS_CUT) q.trans_len = 0;
            else if (q.trans_len <= 0) q.trans_len = 0.5f;
            ed.dirty = true;
        }
        ImGui::EndDisabled();
        bool len_on = cut > 0 && ed.edit.pieces[cut].trans != TRANS_CUT;
        length_row("Length", "##trans_len", len_on ? &ed.edit.pieces[cut].trans_len : &no_len, len_on);
        if (len_on && ed.edit.pieces[cut].trans_len <= 0) ed.edit.pieces[cut].trans = TRANS_CUT;
        kit_dim(cut > 0 ? "At the cut nearest the playhead" : "Cuts get a transition here");
    }
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
}

static void ed_edit_tab(f32 w)
{
    Editor &ed = g_app.editor;
    const ImGuiStyle &st = ImGui::GetStyle();
    f32 sp = st.ItemSpacing.x;

    // The game it's filed under: the library's picker (lib_game_field)
    ed_caption("GAME");
    if (lib_game_field("##game", ed.item, w)) {
        snprintf(ed.result, sizeof ed.result, "Filed under %s", ed.item.info.game);
        ed.result_error = false;
    }
    ImGui::Dummy(ImVec2(1, kit_px(8)));

    if (adv_on()) adv_edit_panel(w);
    else ed_simple_panel(w);

    // Undo and redo, as buttons too.
    f32 bw = (w - sp) / 2;
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
    ImGui::Dummy(ImVec2(1, kit_px(8)));

    // Its comment (the same as the card's), saved when the box is left.
    ed_caption("COMMENT");
    static char comment[LIB_COMMENT_MAX + 1];
    static u64 comment_key;
    if (comment_key != ed.item.key) {
        comment_key = ed.item.key;
        str_set(comment, lib_comment(ed.item.key));
    }
    f32 box_h = 5 * ImGui::GetTextLineHeight() + 2 * st.FramePadding.y;
    if (lib_comment_box("##comment", comment, sizeof comment, w, box_h)) library_set_comment(ed.item.key, comment, ed.item.name);
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
        if (ok) ed_share_touch(w);
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

// A link's actions, right-aligned at `right` on the row at y: copy it, open it in the browser.
static void ed_link_actions(const char *url, f32 right, f32 y)
{
    Editor &ed = g_app.editor;
    f32 fh = ImGui::GetFrameHeight(), gap = kit_px(2);
    ImGui::SetCursorScreenPos(ImVec2(right - 2 * fh - gap, y));
    kit_next_tip("Copy the link");
    if (kit_icon_button(ICON_COPY, "##copy_link", KIT_GHOST)) {
        ImGui::SetClipboardText(url);
        str_set(ed.result, "Link copied, ready to paste.");
        ed.result_error = false;
    }
    ImGui::SameLine(0, gap);
    kit_next_tip("Open the link");
    if (kit_icon_button(ICON_LINK, "##open_link", KIT_GHOST)) {
        wchar_t w[512];
        utf8_to_wide(url, w, COUNT(w));
        ShellExecuteW(0, L"open", w, 0, 0, SW_SHOWNORMAL);
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

// Keep and Share. Keep makes a full-quality copy of the trim in Clips. Share does what share_decide picks for the
// edit; under it the same three tiles as the library's Share menu, File (a copy with the edit and one mixed sound
// track), Original (the source file, in its folder) and Link, each starting its own kind, the running one showing its
// step and percent. None touches the original.
static void ed_share_tab(f32 w)
{
    Editor &ed = g_app.editor;
    const ImGuiStyle &st = ImGui::GetStyle();
    f32 fh = ImGui::GetFrameHeight();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    bool keeping = ed_job_busy(ed.keep_job), sharing = ed_share_busy();
    f32 iw = w;

    // The name both use
    if (!ed.out_name[0]) str_set(ed.out_name, ed.item.name);
    ImGui::SetNextItemWidth(w);
    ImGui::InputTextWithHint("##outname", "Name", ed.out_name, sizeof ed.out_name);
    ImGui::Dummy(ImVec2(1, kit_px(2)));

    // How long the trim is, and what Share would do with it
    f64 len = pieces_len(ed.edit, ed_duration());
    char len_s[32];
    fmt_time(len, len_s, sizeof len_s);
    ShareDecision d = share_decide(ed_share_info(), &ed.edit);
    char max_s[24];
    share_size_text(d.max_bytes, max_s, sizeof max_s);

    // Keep: the title row with its buttons beside it. Keep small makes Share's file into Clips, kept for good.
    f32 head_h = ed_card_head_h(), bw = kit_px(120), gap = kit_px(12);
    f32 keep_row = head_h > fh ? head_h : fh;
    {
        char line[96];
        snprintf(line, sizeof line, "Full quality, %s", len_s);
        f32 kw = kit_px(92);
        ImVec2 top = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(ImVec2(top.x, top.y + (keep_row - head_h) / 2));
        ed_card_head(ICON_SAVE, "Keep", line, iw - 2 * kw - gap - kit_px(8));
        ImGui::SetCursorScreenPos(ImVec2(top.x + iw - 2 * kw - kit_px(8), top.y + (keep_row - fh) / 2));
        char small_tip[96];
        snprintf(small_tip, sizeof small_tip, d.file_ok ? "A copy under %s, kept in Clips for good" :
                                                          "Too big for %s even squeezed: trim it shorter", max_s);
        kit_next_tip(small_tip);
        if (kit_button("Keep small", KIT_SECONDARY, ImVec2(kw, 0), !keeping && d.file_ok)) ed_start(true);
        ImGui::SameLine(0, kit_px(8));
        kit_next_tip("Full quality, saved in Clips");
        if (kit_button(keeping ? "Keeping..." : "Keep", KIT_SECONDARY, ImVec2(kw, 0), !keeping)) ed_start();
        ImGui::SetCursorScreenPos(ImVec2(top.x, top.y + keep_row + st.ItemSpacing.y));
    }
    ImGui::Dummy(ImVec2(1, kit_px(4)));

    // Share: the title row says what one click does, with the button beside it
    {
        bool can = d.kind == SHARE_LINK ? d.link_ok : d.file_ok;
        ImVec2 top = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(ImVec2(top.x, top.y + (keep_row - head_h) / 2));
        ed_card_head(ICON_SHARE, "Share", can ? d.why : d.link_why, iw - bw - gap, can ? 0 : col32(0xff8a92));
        ImGui::SetCursorScreenPos(ImVec2(top.x + iw - bw, top.y + (keep_row - fh) / 2));
        // Its arrow, or a right-click, opens the Share menu. A running Share turns the button into its Cancel: the X
        // in a ring filled to the percent, on the plain button so the accent ring shows. Then how it went.
        ShareJobView j = ed.share_job ? share_job(ed.share_job) : ShareJobView{};
        ShareMenuState js = share_menu_state(j);
        char label[48];
        if (js.running) str_set(label, " ");
        else if (js.done) str_set(label, js.line);
        else str_set(label, sharing ? "Sharing..." : ICON_SHARE "  Share");
        ImVec2 share_at = ImGui::GetCursorScreenPos();
        i32 sb = kit_split_button(label, "##edshare", js.running ? KIT_SECONDARY : KIT_PRIMARY, ImVec2(bw, 0),
                                  js.running || (!sharing && can), true, -1,
                                  js.running ? "Cancel the share" : "Share. Right-click for more options", "More share options");
        if (js.running) {
            share_cancel_ring(ImGui::GetWindowDrawList(), ImVec2(floorf(share_at.x + (bw - fh) / 2), floorf(share_at.y + fh / 2)),
                              kit_px(8), js.progress, COL_TEXT);
            ui_wake();
        }
        if (sb == 1 && js.running) {
            log_line("editor: share %u cancelled", ed.share_job);
            share_cancel(ed.share_job);
        } else if (sb == 1) ed_share_ask(d.kind);
        else if (sb == 2) ImGui::OpenPopup("ed_share_menu");
        lib_popup_style_push(8);
        if (ImGui::BeginPopup("ed_share_menu")) {
            char pick[64];
            i32 r = share_menu_body(d, ed_share_info(), &ed.edit, js, ed.item.key, ed.path.s, pick);
            if (r == SHARE_MENU_SHARE) ed_share_ask(d.kind);
            else if (r == SHARE_MENU_HOST) ed_share_ask(SHARE_LINK, pick);
            else if (r == SHARE_MENU_CANCEL) share_cancel(ed.share_job);
            ImGui::EndPopup();
        }
        lib_popup_style_pop();
        ImGui::SetCursorScreenPos(ImVec2(top.x, top.y + keep_row + st.ItemSpacing.y));
    }
    ImGui::Dummy(ImVec2(1, kit_px(4)));

    // Keep's progress, or the last result with its file's actions: always one row, in its place.
    SaveJobView *v = save_view(keeping ? ed.keep_job : 0);
    ImVec2 a = ImGui::GetCursorScreenPos();
    f32 line_h = ImGui::GetTextLineHeight(), bar_h = kit_px(4);
    f32 ty = floorf(a.y + (fh - line_h) / 2);
    if (v) {
        char label[64], pc[16];
        f32 frac = v->progress;
        snprintf(label, sizeof label, "Keeping%s", v->state == SAVE_QUEUED ? ", waiting" : "");
        snprintf(pc, sizeof pc, "%.0f%%", frac * 100);
        dl->AddText(ImVec2(a.x, a.y), col32(COL_DIM), label);
        dl->AddText(ImVec2(a.x + w - ImGui::CalcTextSize(pc).x, a.y), col32(COL_DIM), pc);
        f32 by = a.y + line_h + kit_px(4);
        dl->AddRectFilled(ImVec2(a.x, by), ImVec2(a.x + w, by + bar_h), col32(COL_PANEL2), bar_h);
        if (frac > 0) dl->AddRectFilled(ImVec2(a.x, by), ImVec2(a.x + w * (frac > 1 ? 1 : frac), by + bar_h), col32(COL_ACCENT), bar_h);
        ImGui::Dummy(ImVec2(w, fh));
    } else if (ed.result[0]) {
        bool file = ed.result_path[0] && !ed.result_error, url = ed.result_url[0] && !ed.result_error;
        f32 text_w = file ? w - 3 * fh - kit_px(12) : url ? w - 2 * fh - kit_px(12) : w;
        text_clipped(dl, ImVec2(a.x, ty), text_w, col32(ed.result_error ? 0xff8a92 : COL_OK), ed.result);
        ImGui::PushID("result");
        if (file) ed_file_actions(ed.result_path, a.x + w, a.y);
        else if (url) ed_link_actions(ed.result_url, a.x + w, a.y);
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
                snprintf(none, sizeof none, "Nothing shared in the last %g days", g_app.config.share_keep_days);
                ImVec2 ts = ImGui::CalcTextSize(none);
                dl->AddText(ImVec2(at.x, floorf(at.y + (fh - ts.y) / 2)), col32(COL_FAINT), none);
            }
            ImGui::Dummy(ImVec2(w, fh));
        }
        ImGui::PopID();
    }
}

// Where Left or Right goes from `at` on the axis: the nearest key on a lane shown, end of the trim or start of a
// piece, that way.
static f64 ed_next_stop(f64 at, i32 dir)
{
    Editor &ed = g_app.editor;
    f64 best = dir > 0 ? ed_axis_len() : 0, eps = ed_frame() * 0.5;
    auto take = [&](f64 u) {
        if (u >= 0 && (dir > 0 ? u > at + eps && u < best : u < at - eps && u > best)) best = u;
    };
    if (ed_joined()) {
        EditPiece pc[EDIT_MAX_PIECES];
        i32 np = pieces_of(ed.edit, ed_duration(), pc);
        for (i32 i = 1; i < np; i++) take(piece_start(ed.edit, ed_duration(), i));
    } else {
        take(ed_in());
        take(ed_out());
    }
    for (i32 l = 0; l < ED_LANES; l++) {
        if (!ed_lane_shown(l)) continue;
        for (i32 k = 0; k < ln_count(ed.edit, l); k++) take(ed_u(ln_get(ed.edit, l, k).t));
    }
    return best;
}

static void screen_editor()
{
    Editor &ed = g_app.editor;
    Player &p = ed.player;
    ImGuiIO &io = ImGui::GetIO();
    ed_check_pending();
    ed_share_poll();
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
        player_open(&p, ed.path, ed_in(), false);
    }
    // Simple plays the sound with the picture (on an engine per track when there's more than one); Advanced always
    // has an engine per track, each moved to its own item (adv_update).
    if (p.loaded && !ed.tracks_ready) {
        i32 tracks[MEDIA_MAX_TRACKS];
        i32 n = ed_tracks(ed.item.info, tracks, PLAYER_MAX_TRACK_ENGINES);
        bool adv = adv_on();
        p.tracks_free = adv;
        if (adv ? n >= 1 && p.audio_count >= 1 : n > 1 && p.audio_count > 1) player_set_tracks(&p, tracks, n);
        else if (p.track_engine_count) player_set_tracks(&p, tracks, 0);
        adv_resync();
        ed.heard_set = false;
        ed_apply_volumes();
        ed.tracks_ready = true;
    }
    // One piece plays from the in to the out; joined pieces play one after another (ed_follow_pieces).
    bool joined = ed_joined();
    if (adv_on()) {
        player_update(&p);
        adv_update();
    } else {
        p.loop_from = joined ? ed_s(0) : ed_in();
        p.stop_at = joined ? 0 : ed_out();
        player_update(&p);
        ed_follow_pieces();
    }
    // Every half second everything is handed over again, for a track engine that's only just loaded.
    static u64 heard_at;
    if (GetTickCount64() - heard_at > 500) { heard_at = GetTickCount64(); ed.heard_set = false; }
    if (ed.tracks_ready) ed_apply_volumes();
    // While the waveforms are measured, what's on screen goes first (each chunk measured wakes the window).
    if (ed.wave && !ed.wave->state && !adv_on()) {
        f64 wa = ed_s(ed.view_from), wz = ed_s(ed.view_from + ed.view_span);
        wave_view(ed.wave, wa < wz ? wa : wz, wa < wz ? wz : wa);
    }

    ImDrawList *dl = ImGui::GetWindowDrawList();
    f32 fh = ImGui::GetFrameHeight();
    ImVec2 avail = ImGui::GetContentRegionAvail();
    Config &cfg = g_app.config;

    // Header: back, the name, and the game and when under it; the Simple / Advanced switch on the right.
    ImVec2 h0 = ImGui::GetCursorScreenPos();
    f32 head_h = fmaxf(fh, g_font_big->FontSize + g_font_small->FontSize + kit_px(4));
    ImGui::SetCursorScreenPos(ImVec2(h0.x, h0.y + (head_h - fh) / 2));
    kit_next_tip("Back to the library", "Esc");
    bool back = kit_icon_button(ICON_BACK, "##back", KIT_GHOST);
    f32 mode_w = kit_px(196);
    {
        static const char *const modes[] = {"Simple", "Advanced"};
        ImGui::SetCursorScreenPos(ImVec2(h0.x + avail.x - mode_w, h0.y + (head_h - fh) / 2));
        if (lib_switch("##mode", modes, mode_w, fh, adv_on(), COL_PANEL2, COL_HOVER, 0x3a3d44)) adv_set_mode(!adv_on(), false);
        adv_ask_dialog();
    }
    f32 tx = h0.x + fh + kit_px(14), tw = avail.x - (tx - h0.x) - mode_w - kit_px(14);
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

    // The picture with the zoom at the playhead, on black, in one of two views (editor_zoom_view):
    //   Crop: the whole footage stays put and the frame that's kept moves and scales over it, the rest dimmed;
    //   Footage: the frame stays put (a little inside the panel) and the footage moves and scales behind it, what
    //   falls outside dimmed. Dragging aims either way: the frame or the footage follows the mouse.
    bool footage_view = cfg.editor_zoom_view == 1;
    ImVec2 pos(h0.x, body_y), pos1(pos.x + main_w, pos.y + video_h), a, b;
    dl->AddRectFilled(pos, pos1, IM_COL32(0, 0, 0, 255), kit_px(10));
    if (footage_view) {
        ImVec2 m(main_w * 0.1f, video_h * 0.1f);
        player_fit(p, ImVec2(pos.x + m.x, pos.y + m.y), ImVec2(main_w - 2 * m.x, video_h - 2 * m.y), &a, &b);
        ImGui::SetCursorScreenPos(pos);
        ImGui::InvisibleButton("picture", ImVec2(main_w > 1 ? main_w : 1, video_h > 1 ? video_h : 1));
    } else {
        player_fit(p, pos, ImVec2(main_w, video_h), &a, &b);
        ImGui::SetCursorScreenPos(a);
        ImGui::InvisibleButton("picture", ImVec2(b.x - a.x > 1 ? b.x - a.x : 1, b.y - a.y > 1 ? b.y - a.y : 1));
    }
    f64 z, cx, cy;
    zoom_at(ed.edit, p.pos, &z, &cx, &cy);
    i32 pic = adv_on() ? adv_picture_item() : -1;
    if (ImGui::IsItemActivated()) {
        ed.aiming = z > 1.001;
        if (ed.aiming) {
            ed.aim_key = adv_on() ? (pic >= 0 ? adv_key_at(ed.edit, pic, IKEY_ZOOM, p.pos) : -1) : ed_add_key(p.pos);
            ed.aim_mx = io.MousePos.x;
            ed.aim_my = io.MousePos.y;
            ed.aim_kx = cx;
            ed.aim_ky = cy;
            if (ed.aim_key < 0) ed.aiming = false;
        }
    }
    bool adv_aim = adv_on() && ed.aim_key < ed.edit.ikey_count && ed.edit.ikeys[ed.aim_key].what == IKEY_ZOOM;
    if (ImGui::IsItemActive() && ed.aiming && (adv_aim || (!adv_on() && ed.aim_key < ed.edit.key_count))) {
        f64 *kz = adv_on() ? &ed.edit.ikeys[ed.aim_key].v : &ed.edit.keys[ed.aim_key].z;
        f64 *kx = adv_on() ? &ed.edit.ikeys[ed.aim_key].x : &ed.edit.keys[ed.aim_key].x;
        f64 *ky = adv_on() ? &ed.edit.ikeys[ed.aim_key].y : &ed.edit.keys[ed.aim_key].y;
        // The footage follows the mouse at the zoom's scale; the frame follows it over the unzoomed footage
        f64 s = footage_view ? -1 / *kz : 1;
        f64 nx = ed.aim_kx + (io.MousePos.x - ed.aim_mx) / (b.x - a.x) * s;
        f64 ny = ed.aim_ky + (io.MousePos.y - ed.aim_my) / (b.y - a.y) * s;
        *kx = nx < 0 ? 0 : nx > 1 ? 1 : nx;
        *ky = ny < 0 ? 0 : ny > 1 ? 1 : ny;
        ed.dirty = true;
    } else if (ImGui::IsItemDeactivated()) {
        if (!ed.aiming) ed_toggle();
        ed.aiming = false;
    }
    ImVec2 uv0, uv1;
    zoom_uv(ed.edit, p.pos, &uv0, &uv1);
    // The kept frame (k0..k1) and the whole footage (f0..f1) on screen
    ImVec2 k0, k1, f0, f1;
    if (footage_view) {
        k0 = a, k1 = b;
        f32 sx = (b.x - a.x) / (uv1.x - uv0.x), sy = (b.y - a.y) / (uv1.y - uv0.y);
        f0 = ImVec2(a.x - uv0.x * sx, a.y - uv0.y * sy);
        f1 = ImVec2(f0.x + sx, f0.y + sy);
    } else {
        f0 = a, f1 = b;
        k0 = ImVec2(a.x + uv0.x * (b.x - a.x), a.y + uv0.y * (b.y - a.y));
        k1 = ImVec2(a.x + uv1.x * (b.x - a.x), a.y + uv1.y * (b.y - a.y));
    }
    dl->PushClipRect(pos, pos1, true);
    ed_draw_picture(p, f0, f1);
    ImU32 shade = IM_COL32(0, 0, 0, 150);
    ImVec2 c0(f0.x > pos.x ? f0.x : pos.x, f0.y > pos.y ? f0.y : pos.y);
    ImVec2 c1(f1.x < pos1.x ? f1.x : pos1.x, f1.y < pos1.y ? f1.y : pos1.y);
    dl->AddRectFilled(c0, ImVec2(c1.x, k0.y), shade);
    dl->AddRectFilled(ImVec2(c0.x, k1.y), c1, shade);
    dl->AddRectFilled(ImVec2(c0.x, k0.y), ImVec2(k0.x, k1.y), shade);
    dl->AddRectFilled(ImVec2(k1.x, k0.y), ImVec2(c1.x, k1.y), shade);
    if (z > 1.001 || footage_view) dl->AddRect(k0, k1, IM_COL32(255, 255, 255, 200), 0, 0, kit_px(1.5f));
    dl->PopClipRect();
    if (z > 1.001) {
        char zt[32];
        snprintf(zt, sizeof zt, "%.1fx", z);
        badge(dl, ImVec2(a.x + kit_px(10), a.y + kit_px(10)), false, false, zt);
    }

    // The transport bar, as the Timeline's: Play and the playhead's time, zoom, then the in and out and their
    // length on the right with a button to clear them (with pieces: how many and their length, and in Advanced
    // Split and Delete). Fixed places, so nothing moves.
    ImVec2 r0(pos.x, pos.y + video_h + kit_px(10));
    dl->AddRectFilled(r0, ImVec2(r0.x + main_w, r0.y + bar_h), col32(COL_PANEL), kit_px(10));
    f32 pad = kit_px(8), y = r0.y + (bar_h - fh) / 2;
    ImGui::SetCursorScreenPos(ImVec2(r0.x + pad, y));
    bool stopped = adv_on() ? !ed.adv_playing : p.paused;
    kit_next_tip(stopped ? "Play" : "Pause", "Space");
    if (kit_icon_button(stopped ? ICON_PLAY : ICON_PAUSE, "##play", KIT_PRIMARY)) ed_toggle();
    f64 dur = ed_duration(), alen = ed_axis_len();
    char t1[32];
    fmt_time(ed_play_u(), t1, sizeof t1, true);
    f32 x = r0.x + pad + fh + kit_px(12);
    ImGui::PushFont(g_font_bold);
    dl->AddText(ImVec2(x, floorf(y + (fh - ImGui::GetFontSize()) / 2)), col32(COL_TEXT), t1);
    fmt_time(dur, t1, sizeof t1, true); // the file's length holds the place in either mode, so nothing moves
    x += ImGui::CalcTextSize(t1).x + kit_px(18);
    ImGui::PopFont();
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    kit_next_tip("Zoom out", "-");
    if (kit_icon_button(ICON_ZOOM_OUT, "##view_out", KIT_GHOST, ed.view_span < alen)) ed_view_zoom(2, ed.view_from + ed.view_span / 2);
    ImGui::SameLine(0, kit_px(2));
    kit_next_tip("Zoom in", "+");
    if (kit_icon_button(ICON_ZOOM_IN, "##view_in", KIT_GHOST, ed.view_span > 0.51)) ed_view_zoom(0.5, ed.view_from + ed.view_span / 2);
    {
        bool lit = ed.edit.piece_count > 0 && !adv_on();
        f32 x_clear = r0.x + main_w - pad - fh, text_r = x_clear;
        char ta[32], tz[32], len[32], sel[112];
        ImGui::SetCursorScreenPos(ImVec2(x_clear, y));
        kit_next_tip("Clear in and out");
        if (kit_icon_button(ICON_CLOSE, "##clear_inout", KIT_GHOST, lit)) ed_clear_trim();
        if (adv_on()) {
            fmt_time(adv_len(ed.edit), len, sizeof len, true);
            snprintf(sel, sizeof sel, "%d item%s   %s", ed.edit.item_count, ed.edit.item_count == 1 ? "" : "s", len);
            lit = true;
        } else if (!joined) {
            fmt_time(ed_in(), ta, sizeof ta, true);
            fmt_time(ed_out(), tz, sizeof tz, true);
            fmt_time(ed_out() - ed_in(), len, sizeof len, true);
            snprintf(sel, sizeof sel, "%s - %s   %s", ta, tz, len);
        } else {
            EditPiece pc[EDIT_MAX_PIECES];
            i32 np = pieces_of(ed.edit, dur, pc);
            fmt_time(alen, len, sizeof len, true);
            snprintf(sel, sizeof sel, "%d piece%s   %s", np, np == 1 ? "" : "s", len);
        }
        ImVec2 ks = ImGui::CalcTextSize(sel);
        f32 kx = text_r - kit_px(6) - ks.x;
        dl->AddText(ImVec2(kx, floorf(y + (fh - ks.y) / 2)), col32(lit ? COL_TEXT : COL_DIM), sel);
    }

    // The side panel: Edit, or Keep & share.
    ImGui::SetCursorScreenPos(ImVec2(pos.x + main_w + gap, body_y));
    if (kit_card_begin("ed_side", ImVec2(side_w, top_h))) {
        f32 cw = ImGui::GetContentRegionAvail().x;
        static const char *const tabs[] = {"Edit", "Keep & share"};
        kit_segmented("##tab", tabs, 2, &ed.tab, (cw - kit_px(6)) / 2);
        ImGui::Dummy(ImVec2(1, kit_px(6)));
        // The tab scrolls when it's taller than the panel: a thin bar out in the card's right margin, so the
        // controls keep their width whether it shows or not.
        f32 bar = kit_px(10);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, bar);
        ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarRounding, kit_px(3));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, rgb(COL_PANEL, 0));
        ImGui::BeginChild("##ed_tab_body", ImVec2(cw + bar, ImGui::GetContentRegionAvail().y));
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(3);
        if (ed.tab == 0) ed_edit_tab(cw);
        else ed_share_tab(cw);
        ImGui::EndChild();
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
    if (adv_on()) adv_timeline(ImVec2(h0.x, tl_y), avail.x, body_y + body_h - tl_y);
    else ed_timeline(ImVec2(h0.x, tl_y), avail.x, body_y + body_h - tl_y);

    // Keys (never while typing or with a menu open).
    bool keys_free = !io.WantTextInput && !ImGui::IsPopupOpen((const char *)0, ImGuiPopupFlags_AnyPopupId);
    if (keys_free && adv_on()) {
        adv_keys(&back);
    } else if (keys_free) {
        bool ctrl = io.KeyCtrl;
        joined = ed_joined();
        if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) ed_toggle();
        if (!joined && !ctrl && ImGui::IsKeyPressed(ImGuiKey_I, false) && p.pos < ed_out() - 0.2) ed_set_trim(p.pos, ed_out());
        if (!joined && !ctrl && ImGui::IsKeyPressed(ImGuiKey_O, false) && p.pos > ed_in() + 0.2) ed_set_trim(ed_in(), p.pos);
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
        if (!ctrl && (ImGui::IsKeyPressed(ImGuiKey_Equal) || ImGui::IsKeyPressed(ImGuiKey_KeypadAdd))) ed_view_zoom(0.5, ed.view_from + ed.view_span / 2);
        if (!ctrl && (ImGui::IsKeyPressed(ImGuiKey_Minus) || ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract))) ed_view_zoom(2, ed.view_from + ed.view_span / 2);
        // Left and Right jump between keys, the trim's ends and the pieces (a second with Shift); comma and period
        // step a frame. All on the axis, so cuts are stepped over.
        for (i32 dir = -1; dir <= 1; dir += 2) {
            f64 u = ed_play_u(), axis = ed_axis_len();
            auto in_axis = [&](f64 v) { return v < 0 ? 0 : v > axis ? axis : v; };
            if (ImGui::IsKeyPressed(dir < 0 ? ImGuiKey_LeftArrow : ImGuiKey_RightArrow)) {
                player_pause(&p);
                ed_seek_u(io.KeyShift ? in_axis(u + dir) : ed_next_stop(u, dir));
            }
            if (!ctrl && ImGui::IsKeyPressed(dir < 0 ? ImGuiKey_Comma : ImGuiKey_Period)) {
                player_pause(&p);
                ed_seek_u(in_axis(u + dir * ed_frame()));
            }
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            if (ed_sel_count()) ed_sel_clear();
            else back = true;
        }
    }
    // A change settles once nothing is held: it goes on the undo list and the record is written.
    if (!ImGui::IsAnyItemActive() && ed.lane_drag == ED_DRAG_NONE && !ed.adv_drag) {
        ed_settle();
        if (ed.dirty) ed_save_record();
    }
    if (back) {
        editor_close();
        g_app.ui.screen = SCREEN_LIBRARY;
    }
}
