// Advanced editing (included from editor.cpp): the sequence's playback, its timeline of items on tracks, and the side
// panel's controls. An Advanced edit's playhead (adv_u) is on the sequence: while a picture item plays it follows the
// picture's engine, over a gap it follows the clock and the picture is black. Each audio track's engine plays the item
// under the playhead on its own track, so a track moved apart from the picture is heard where it was put.

enum AdvDrag : u8 { ADV_DRAG_NONE, ADV_DRAG_SEEK, ADV_DRAG_MOVE, ADV_DRAG_TRIM_IN, ADV_DRAG_TRIM_OUT, ADV_DRAG_KEY };

// Every engine is put in place again on the next update.
static void adv_resync()
{
    Editor &ed = g_app.editor;
    ed.adv_vitem = ed.adv_citem = -2;
    for (i32 i = 0; i < PLAYER_MAX_TRACK_ENGINES; i++) ed.adv_titem[i] = -2;
}

static void adv_seek(f64 u)
{
    Editor &ed = g_app.editor;
    f64 len = adv_len(ed.edit);
    ed.adv_u = u < 0 ? 0 : u > len ? len : u;
    ed.adv_clock = ImGui::GetTime();
    adv_resync();
    ui_wake(6);
}

static void adv_toggle()
{
    Editor &ed = g_app.editor;
    if (!ed.adv_playing && ed.adv_u >= adv_len(ed.edit) - 0.05) adv_seek(0);
    ed.adv_playing = !ed.adv_playing;
    ed.adv_clock = ImGui::GetTime();
    adv_resync();
    ui_wake();
}

// Item it's source time at sequence time u, held inside the item.
static f64 adv_src(const EditItem &it, f64 u)
{
    f64 s = it.from + (u - it.start);
    return s < it.from ? it.from : s > it.to ? it.to : s;
}

// Each frame: the playhead moved on, and every engine on the item under it, playing or not.
static void adv_update()
{
    Editor &ed = g_app.editor;
    Player &p = ed.player;
    const EditRecord &e = ed.edit;
    f64 now = ImGui::GetTime(), len = adv_len(e);
    p.loop_from = 0, p.stop_at = 0;
    if (!p.loaded) {
        ed.adv_clock = now;
        return;
    }
    if (ed.adv_playing) {
        i32 v = ed.adv_vitem >= 0 && ed.adv_vitem < e.item_count ? ed.adv_vitem : -1;
        if (v < 0) {
            ed.adv_u += (now - ed.adv_clock) * (p.rate > 0 ? p.rate : 1); // a gap: the clock
        } else if (!p.paused && !p.seeking && !p.has_seek && p.pos >= e.items[v].from - 0.05 &&
                   p.pos <= e.items[v].to + 0.25) {
            ed.adv_u = e.items[v].start + (p.pos - e.items[v].from); // the picture's engine
        } // else held while the picture gets to its place
        if (ed.adv_u >= len) {
            ed.adv_u = len;
            ed.adv_playing = false;
        }
        ui_wake(2);
    }
    ed.adv_clock = now;
    f64 u = ed.adv_u;

    // The picture: on to the next item (no seek where it carries straight on in the file), paused over a gap
    i32 v = item_at(e, 0, u);
    if (v >= 0) {
        f64 s = adv_src(e.items[v], u);
        bool moved = v != ed.adv_vitem;
        bool carries = moved && ed.adv_vitem >= 0 && ed.adv_playing && fabs(p.pos - s) < 0.08;
        if (moved && !carries) player_seek(&p, s);
        if (ed.adv_playing && p.paused && !p.seeking && !p.has_seek) player_play(&p);
        if (!ed.adv_playing && !p.paused) player_pause(&p);
    } else if (!p.paused) {
        player_pause(&p);
    }
    ed.adv_vitem = v;

    // The sound: each track's engine on its own item, moved back when it drifts once it has settled
    for (i32 i = 0; i < p.track_engine_count; i++) {
        i32 a = item_at(e, 1 + p.track_of_engine[i], u);
        if (a >= 0) {
            f64 s = adv_src(e.items[a], u), at = player_track_pos(p, i);
            bool placed = p.track_synced[i] >= 0;
            bool moved = a != ed.adv_titem[i] || !placed;
            bool carries = moved && placed && ed.adv_titem[i] >= 0 && ed.adv_playing && at >= 0 && fabs(at - s) < 0.08;
            bool drift = !moved && ed.adv_playing && at >= 0 && now - p.track_synced[i] > 1.0 && fabs(at - s) > 0.15;
            if ((moved && !carries) || drift) player_track_seek(&p, i, s);
            player_track_run(&p, i, ed.adv_playing);
        } else {
            player_track_run(&p, i, false);
        }
        ed.adv_titem[i] = a;
    }
}

// Each track's engine at its item's volume and fades where the playhead is; silent over a gap, muted or not solo.
static void adv_apply_volumes()
{
    Editor &ed = g_app.editor;
    Player &p = ed.player;
    const EditRecord &e = ed.edit;
    f64 dur = ed_duration();
    for (i32 i = 0; i < p.track_engine_count; i++) {
        i32 t = p.track_of_engine[i];
        i32 a = item_at(e, 1 + t, ed.adv_u);
        f32 v = 0;
        if (a >= 0 && !e.muted[t] && (ed.solo < 0 || ed.solo == t)) {
            f64 s = adv_src(e.items[a], ed.adv_u);
            v = (f32)(item_level_at(e, e.items[a].id, IKEY_VOLUME, s) * item_fade_gain(e, dur, a, s));
        }
        if (fabsf(v - ed.heard[t]) < 0.002f && ed.heard_set) continue;
        player_track_volume(&p, t, v, v <= 0);
        ed.heard[t] = v;
    }
    ed.heard_set = true;
}

// ---- Selecting and changing items ----

static bool adv_selected(u16 id)
{
    const Editor &ed = g_app.editor;
    for (i32 i = 0; i < ed.adv_sel_count; i++)
        if (ed.adv_sel[i] == id) return true;
    return false;
}

static void adv_sel_set(u16 id, bool on)
{
    Editor &ed = g_app.editor;
    for (i32 i = 0; i < ed.adv_sel_count; i++)
        if (ed.adv_sel[i] == id) {
            if (!on) ed.adv_sel[i] = ed.adv_sel[--ed.adv_sel_count];
            return;
        }
    if (on && ed.adv_sel_count < EDIT_MAX_ITEMS) ed.adv_sel[ed.adv_sel_count++] = id;
}

// Item i, with the items linked to it (`links`), selected alone, or added to (or with `toggle` taken out of) the
// selection.
static void adv_select(i32 i, bool links, bool add, bool toggle = false)
{
    Editor &ed = g_app.editor;
    const EditRecord &e = ed.edit;
    bool on = !(toggle && adv_selected(e.items[i].id));
    if (!add) ed.adv_sel_count = 0;
    for (i32 k = 0; k < e.item_count; k++)
        if (k == i || (links && e.items[i].link && e.items[k].link == e.items[i].link)) adv_sel_set(e.items[k].id, on);
}

// Drops ids of items that are gone.
static void adv_sel_tidy()
{
    Editor &ed = g_app.editor;
    i32 n = 0;
    for (i32 i = 0; i < ed.adv_sel_count; i++)
        if (item_find(ed.edit, ed.adv_sel[i]) >= 0) ed.adv_sel[n++] = ed.adv_sel[i];
    ed.adv_sel_count = n;
}

static u16 adv_new_link(const EditRecord &e)
{
    u16 l = 1;
    for (i32 i = 0; i < e.item_count; i++)
        if (e.items[i].link >= l) l = (u16)(e.items[i].link + 1);
    return l;
}

// After a change: the edit kept playable, written, and every engine put in place again.
static void adv_changed()
{
    Editor &ed = g_app.editor;
    adv_settle(&ed.edit, ed_duration());
    adv_sel_tidy();
    ed.adv_key = -1;
    ed.dirty = true;
    ed.heard_set = false;
    adv_resync();
}

// The items under sequence time u that are selected, or all of them when none is.
static i32 adv_targets(f64 u, i32 *out)
{
    const Editor &ed = g_app.editor;
    const EditRecord &e = ed.edit;
    i32 n = 0;
    for (i32 pass = 0; pass < 2 && !n; pass++)
        for (i32 i = 0; i < e.item_count; i++) {
            const EditItem &it = e.items[i];
            if (u > it.start + 0.0005 && u < item_end(it) - 0.0005 && (pass || adv_selected(it.id))) out[n++] = i;
        }
    return n;
}

// Cuts the items at the playhead in two (the selected ones there, or all of them). The second halves are linked as
// the first halves were, and each half keeps the keys and the fade at its own end.
static void adv_split(f64 u)
{
    Editor &ed = g_app.editor;
    EditRecord &e = ed.edit;
    i32 idx[EDIT_MAX_ITEMS], n = adv_targets(u, idx);
    u16 old_link[EDIT_MAX_ITEMS], new_link[EDIT_MAX_ITEMS];
    i32 links = 0, done = 0;
    for (i32 k = 0; k < n && e.item_count < EDIT_MAX_ITEMS; k++) {
        EditItem &it = e.items[idx[k]];
        f64 s = it.from + (u - it.start);
        if (s - it.from < PIECE_LEAST || it.to - s < PIECE_LEAST) continue;
        EditItem b = it;
        b.id = item_new_id(e);
        b.from = s, b.start = u;
        b.fade_in = 0, b.cross = 0;
        b.link = 0;
        if (it.link) {
            i32 j = 0;
            while (j < links && old_link[j] != it.link) j++;
            if (j == links) {
                old_link[links] = it.link;
                new_link[links++] = (u16)(adv_new_link(e) + links);
            }
            b.link = new_link[j];
        }
        it.to = s, it.fade_out = 0;
        u16 from_id = it.id;
        e.items[e.item_count++] = b;
        static ItemKey copy[EDIT_MAX_ITEM_KEYS];
        i32 nc = 0;
        for (i32 q = 0; q < e.ikey_count; q++)
            if (e.ikeys[q].item == from_id) copy[nc] = e.ikeys[q], copy[nc++].item = b.id;
        for (i32 q = 0; q < nc; q++) ikey_add(&e, copy[q]);
        done++;
    }
    if (!done) return;
    adv_changed();
    log_line("editor: split %d item%s at %.3f", done, done == 1 ? "" : "s", u);
}

static void adv_delete_selected()
{
    Editor &ed = g_app.editor;
    EditRecord &e = ed.edit;
    i32 gone = 0;
    for (i32 i = e.item_count - 1; i >= 0; i--)
        if (adv_selected(e.items[i].id)) item_remove(&e, i), gone++;
    if (!gone) return;
    ed.adv_sel_count = 0;
    adv_changed();
}

// The selected items linked to each other (they move and trim together), or each on its own.
static void adv_link_selected(bool link)
{
    Editor &ed = g_app.editor;
    EditRecord &e = ed.edit;
    u16 l = link ? adv_new_link(e) : 0;
    for (i32 i = 0; i < e.item_count; i++)
        if (adv_selected(e.items[i].id)) e.items[i].link = l;
    adv_changed();
}

// Whether the selected items are all linked together already.
static bool adv_sel_linked()
{
    const Editor &ed = g_app.editor;
    const EditRecord &e = ed.edit;
    u16 l = 0;
    for (i32 i = 0; i < e.item_count; i++) {
        if (!adv_selected(e.items[i].id)) continue;
        if (!e.items[i].link || (l && e.items[i].link != l)) return false;
        l = e.items[i].link;
    }
    return l != 0;
}

// The key of an item's kind `what` at source time t (within half a frame), added there with the curve's value if
// there's none. Its index in e.ikeys, or -1 when there's no room.
static i32 adv_key_at(EditRecord &e, i32 i, i32 what, f64 t)
{
    const EditItem &it = e.items[i];
    t = t < it.from ? it.from : t > it.to ? it.to : t;
    f64 tol = ed_frame() / 2 + 0.01;
    for (i32 k = 0; k < e.ikey_count; k++)
        if (e.ikeys[k].item == it.id && e.ikeys[k].what == what && fabs(e.ikeys[k].t - t) < tol) return k;
    ItemKey q = {};
    q.item = it.id, q.what = (u8)what, q.t = t, q.ease = EASE_LINEAR;
    if (what == IKEY_ZOOM) item_zoom_at(e, it.id, t, &q.v, &q.x, &q.y);
    else q.v = item_level_at(e, it.id, what, t);
    if (!ikey_add(&e, q)) return -1;
    for (i32 k = 0; k < e.ikey_count; k++)
        if (e.ikeys[k].item == it.id && e.ikeys[k].what == what && e.ikeys[k].t == t) return k;
    return -1;
}

// Item key k taken out (the slots past the end kept clear).
static void ikey_erase(EditRecord *e, i32 k)
{
    if (k < 0 || k >= e->ikey_count) return;
    memmove(e->ikeys + k, e->ikeys + k + 1, (e->ikey_count - k - 1) * sizeof *e->ikeys);
    e->ikeys[--e->ikey_count] = {};
}

// Where a key just put back with ikey_add went.
static i32 ikey_find(const EditRecord &e, const ItemKey &q)
{
    for (i32 k = 0; k < e.ikey_count; k++) {
        const ItemKey &c = e.ikeys[k];
        if (c.item == q.item && c.what == q.what && c.t == q.t && c.v == q.v && c.ease == q.ease) return k;
    }
    return -1;
}

static bool adv_key_ok() { return g_app.editor.adv_key >= 0 && g_app.editor.adv_key < g_app.editor.edit.ikey_count; }

static void adv_delete_key()
{
    Editor &ed = g_app.editor;
    if (!adv_key_ok()) return;
    ikey_erase(&ed.edit, ed.adv_key);
    ed.adv_key = -1;
    ed.dirty = true;
    ed.heard_set = false;
}

static void adv_set_ease(i32 ease)
{
    Editor &ed = g_app.editor;
    if (!adv_key_ok()) return;
    ed.edit.ikeys[ed.adv_key].ease = (u8)ease;
    ed.dirty = true;
    ed.heard_set = false;
}

// The grabbed key moved by the drag so far (dt seconds, dfrac of its lane's height) from where it was when the drag
// began: onto a frame inside its item, its value caught by 100% or a whole zoom. With Shift only its time moves and
// the playhead catches it; with Ctrl only its value.
static void adv_drag_key(f64 dt, f64 dfrac, f32 px_per_s, f32 vh, bool time_only, bool value_only)
{
    Editor &ed = g_app.editor;
    const EditRecord &f = ed.drag_from;
    i32 k = ed.adv_drag_key;
    if (k < 0 || k >= f.ikey_count) return;
    ItemKey q = f.ikeys[k];
    i32 i = item_find(f, q.item);
    if (i < 0) return;
    const EditItem &it = f.items[i];
    i32 l = q.what == IKEY_ZOOM ? 0 : 1;
    if (!value_only) {
        f64 fr = ed_frame(), nt = floor((q.t + dt) / fr + 0.5) * fr, at = adv_src(it, ed.adv_u);
        bool under = ed.adv_u >= it.start && ed.adv_u <= item_end(it);
        if (time_only && under && fabs(nt - at) * px_per_s < kit_px(8)) nt = at;
        q.t = nt < it.from ? it.from : nt > it.to ? it.to : nt;
    }
    if (!time_only) q.v = ln_snap(l, q.v + dfrac * (ln_hi(l) - ln_lo(l)), vh);
    ed.edit = f;
    ikey_erase(&ed.edit, k);
    ikey_add(&ed.edit, q);
    ed.adv_key = ikey_find(ed.edit, q);
    ed.dirty = true;
    ed.heard_set = false;
}

// The picture item under the playhead, -1 over a gap.
static i32 adv_picture_item() { return item_at(g_app.editor.edit, 0, g_app.editor.adv_u); }

// Going between Simple and Advanced. Back to Simple asks first when it would lose something (edit_round_trips);
// either way it's one step Undo takes back.
static void adv_set_mode(bool to_adv, bool sure)
{
    Editor &ed = g_app.editor;
    const MediaInfo &m = ed.item.info;
    if (to_adv == adv_on()) return;
    if (!to_adv && !sure && !edit_round_trips(ed.edit, m)) {
        ed.adv_ask_simple = true;
        return;
    }
    ed_settle();
    f64 u = ed_play_u();
    player_pause(&ed.player);
    ed.adv_playing = false;
    if (to_adv) edit_to_advanced(&ed.edit, m);
    else edit_to_simple(&ed.edit, m);
    ed_settle();
    ed.dirty = true;
    ed.tracks_ready = false;
    ed.heard_set = false;
    ed.play_piece = -1;
    ed.adv_sel_count = 0;
    ed.adv_key = -1;
    ed_sel_clear();
    ed.lane_drag = ED_DRAG_NONE;
    ed.adv_drag = ADV_DRAG_NONE;
    ed_seek_u(u);
    log_line("editor: %s", to_adv ? "advanced" : "simple");
}

static void adv_ask_dialog()
{
    Editor &ed = g_app.editor;
    if (ed.adv_ask_simple) {
        ImGui::OpenPopup("##adv_simple");
        ed.adv_ask_simple = false;
    }
    if (lib_dialog_begin("##adv_simple")) {
        i32 r = lib_confirm("Back to Simple?", "Back to Simple loses the Advanced edits.", "Undo brings them back.", 0, 0,
                            false, "Back to Simple", KIT_DANGER);
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) r = -1;
        if (r) ImGui::CloseCurrentPopup();
        if (r > 0) adv_set_mode(false, true);
        lib_dialog_end();
    }
}

// Left and Right: the next item edge on any track, the start or the end.
static f64 adv_next_stop(f64 at, i32 dir)
{
    const EditRecord &e = g_app.editor.edit;
    f64 best = dir > 0 ? adv_len(e) : 0;
    for (i32 i = 0; i < e.item_count; i++) {
        f64 ends[2] = {e.items[i].start, item_end(e.items[i])};
        for (f64 x : ends)
            if (dir > 0 ? x > at + 0.001 && x < best : x < at - 0.001 && x > best) best = x;
    }
    return best;
}

// ---- The timeline ----

// The view bar, the ruler, then a row per track: V1 (the picture) and one for each recorded sound track, each with
// its items. Items are selected with a click (with what's linked to them; Alt for the one alone, Ctrl or Shift to
// add), moved by their body and trimmed by their edges, linked ones together (Shift catches the playhead and other
// items' edges). The ruler and empty rows move the playhead; a right click opens an item's menu.
// The webcam layer. Its stream is copied out of the clip into a file of its own (play_dir\cam-<key>.mp4) for an
// engine of its own, which plays the cam item under the playhead as the picture's engine plays the picture's. The
// copy's thread and the editor each hold a reference, and whichever lets go last frees it; the file is deleted by the
// thread when the editor let go while it was copying, else by the editor.
enum : LONG { CAM_COPYING, CAM_COPIED, CAM_COPY_FAILED, CAM_COPY_DROPPED };
struct CamCopy {
    volatile LONG refs, state;
    Path src, dst, ffmpeg;
};

static void cam_copy_release(CamCopy *c)
{
    if (!c || InterlockedDecrement(&c->refs)) return;
    HeapFree(GetProcessHeap(), 0, c);
}

static DWORD WINAPI cam_copy_thread(void *arg)
{
    CamCopy *c = (CamCopy *)arg;
    wchar_t args[2048];
    args[0] = 0;
    static const char *const head[] = {"-v", "error", "-y", "-i"};
    static const char *const mid[] = {"-map", "0:v:1", "-c", "copy", "-an", "-sn", "-dn"};
    arg_list(args, COUNT(args), head, COUNT(head));
    cmd_arg(args, COUNT(args), c->src.s);
    arg_list(args, COUNT(args), mid, COUNT(mid));
    cmd_arg(args, COUNT(args), c->dst.s);
    bool ok = proc_run(c->ffmpeg.s, args, 30 * 60 * 1000, 0, 0) == 0;
    LONG was = InterlockedCompareExchange(&c->state, ok ? CAM_COPIED : CAM_COPY_FAILED, CAM_COPYING);
    if (!ok || was == CAM_COPY_DROPPED) DeleteFileW(c->dst.s);
    ui_wake();
    cam_copy_release(c);
    return 0;
}

// The copy, started the first time Advanced shows a clip with a webcam.
static void adv_cam_start()
{
    Editor &ed = g_app.editor;
    if (ed.cam_copy || ed.item.info.cam_w <= 0) return;
    CamCopy *c = (CamCopy *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *c);
    if (!c) return;
    ed.cam_copy = c;
    c->refs = 1;
    if (!ffmpeg_find(g_app.config.ffmpeg, &c->ffmpeg)) {
        c->state = CAM_COPY_FAILED;
        return;
    }
    c->src = ed.path;
    SHCreateDirectoryExW(0, g_app.config.play_dir.s, 0);
    wchar_t name[64];
    swprintf(name, COUNT(name), L"cam-%016llx.mp4", (unsigned long long)ed.item.key);
    c->dst = path_join(g_app.config.play_dir, name);
    c->refs = 2;
    HANDLE th = thread_start(cam_copy_thread, c);
    if (!th) {
        c->refs = 1;
        c->state = CAM_COPY_FAILED;
        return;
    }
    CloseHandle(th);
}

// Lets go of the webcam's engine and its copy (the editor closing, or opening another file).
static void adv_cam_close()
{
    Editor &ed = g_app.editor;
    player_close(&ed.cam);
    CamCopy *c = ed.cam_copy;
    if (!c) return;
    ed.cam_copy = 0;
    if (InterlockedCompareExchange(&c->state, CAM_COPY_DROPPED, CAM_COPYING) == CAM_COPIED) DeleteFileW(c->dst.s);
    cam_copy_release(c);
}

// Each frame in Advanced: the webcam's engine on the cam item under the playhead, playing with the picture
static void adv_cam_update()
{
    Editor &ed = g_app.editor;
    Player &c = ed.cam;
    const EditRecord &e = ed.edit;
    if (ed.item.info.cam_w <= 0) return;
    adv_cam_start();
    if (!c.open) {
        if (ed.cam_copy && ed.cam_copy->state == CAM_COPIED) {
            c.id = 4;
            if (player_open(&c, ed.cam_copy->dst, 0, false)) c.engine->SetMuted(TRUE);
            ed.adv_citem = -2;
        }
        return;
    }
    player_update(&c);
    if (!c.loaded) return;
    f64 u = ed.adv_u, now = ImGui::GetTime();
    i32 k = item_at(e, EDIT_CAM_TRACK, u);
    if (k >= 0) {
        f64 s = adv_src(e.items[k], u);
        bool moved = k != ed.adv_citem;
        bool carries = moved && ed.adv_citem >= 0 && ed.adv_playing && fabs(c.pos - s) < 0.08;
        // Playing, it's moved back once it has settled and drifted; held, it shows the frame at the playhead
        bool off = ed.adv_playing ? !c.seeking && !c.has_seek && now - ed.cam_synced > 1.0 && fabs(c.pos - s) > 0.15
                                  : fabs(c.pos - s) > 0.005;
        if ((moved && !carries) || (!moved && off)) {
            player_seek(&c, s);
            ed.cam_synced = now;
        }
        f32 rate = ed.player.rate > 0 ? ed.player.rate : 1;
        if (c.rate != rate) player_set_rate(&c, rate);
        if (ed.adv_playing && c.paused && !c.seeking && !c.has_seek) player_play(&c);
        if (!ed.adv_playing && !c.paused) player_pause(&c);
    } else if (!c.paused) {
        player_pause(&c);
    }
    ed.adv_citem = k;
}

// The cam items the webcam's place is set on: with the one at the playhead (k) selected, the selected cam items; else
// every cam item
static i32 adv_cam_targets(i32 k, i32 *out)
{
    const EditRecord &e = g_app.editor.edit;
    if (k < 0 || !adv_selected(e.items[k].id)) return track_items(e, EDIT_CAM_TRACK, out);
    i32 n = 0;
    for (i32 i = 0; i < e.item_count; i++)
        if (e.items[i].track == EDIT_CAM_TRACK && adv_selected(e.items[i].id)) out[n++] = i;
    return n;
}

// Where a cam item puts the webcam on screen, in the kept frame as last drawn
static void adv_cam_screen(const EditItem &it, ImVec2 *a, ImVec2 *b)
{
    Editor &ed = g_app.editor;
    const MediaInfo &m = ed.item.info;
    CamRect r = cam_rect(it, m.width, m.height, m.cam_w, m.cam_h);
    f32 sx = (ed.cam_k[2] - ed.cam_k[0]) / m.width, sy = (ed.cam_k[3] - ed.cam_k[1]) / m.height;
    *a = ImVec2(ed.cam_k[0] + r.x * sx, ed.cam_k[1] + r.y * sy);
    *b = ImVec2(a->x + r.w * sx, a->y + r.h * sy);
}

// The webcam's place set on its targets
static void adv_cam_set(i32 k, f64 x, f64 y, f64 w)
{
    Editor &ed = g_app.editor;
    const MediaInfo &m = ed.item.info;
    i32 idx[EDIT_MAX_ITEMS], n = adv_cam_targets(k, idx);
    for (i32 q = 0; q < n; q++) cam_place(&ed.edit.items[idx[q]], x, y, w, m.width, m.height, m.cam_w, m.cam_h);
    ed.dirty = true;
}

// The webcam in the picture: its body moves it, the corner toward the picture's middle sizes it with the far corner
// kept where it is. Submitted before the picture, so a press on the webcam isn't the picture's, at the place it was
// drawn last frame.
static void adv_cam_drag()
{
    Editor &ed = g_app.editor;
    EditRecord &e = ed.edit;
    const MediaInfo &m = ed.item.info;
    ImGuiIO &io = ImGui::GetIO();
    ed.cam_hot = false;
    bool shown = m.cam_w > 0 && m.width > 0 && m.height > 0 && ed.cam_k[2] > ed.cam_k[0] && ed.cam_k[3] > ed.cam_k[1];
    i32 k = shown ? item_at(e, EDIT_CAM_TRACK, ed.adv_u) : -1;
    if (k < 0) {
        ed.cam_drag = 0;
        return;
    }
    const EditItem &it = e.items[k];
    ImVec2 a, b;
    adv_cam_screen(it, &a, &b);
    ImGui::SetCursorScreenPos(a);
    ImGui::InvisibleButton("##cam", ImVec2(b.x - a.x > 1 ? b.x - a.x : 1, b.y - a.y > 1 ? b.y - a.y : 1));
    f32 grip = kit_px(14), mx = io.MousePos.x, my = io.MousePos.y;
    ImVec2 g(it.cam_x < 0.5f ? b.x : a.x, it.cam_y < 0.5f ? b.y : a.y);
    bool on_grip = fabsf(mx - g.x) <= grip && fabsf(my - g.y) <= grip;
    if (ImGui::IsItemActivated()) {
        ed.cam_drag = on_grip ? 2 : 1;
        ed.cam_drag_mx = mx, ed.cam_drag_my = my;
        ed.cam_drag_x = it.cam_x, ed.cam_drag_y = it.cam_y, ed.cam_drag_w = it.cam_w;
    }
    if (ImGui::IsItemActive() && ed.cam_drag) {
        f64 dx = (mx - ed.cam_drag_mx) / (ed.cam_k[2] - ed.cam_k[0]), dy = (my - ed.cam_drag_my) / (ed.cam_k[3] - ed.cam_k[1]);
        f64 x = ed.cam_drag_x + dx, y = ed.cam_drag_y + dy, w = ed.cam_drag_w;
        if (ed.cam_drag == 2) {
            f64 gx = ed.cam_drag_x < 0.5 ? 1 : -1, gy = ed.cam_drag_y < 0.5 ? 1 : -1, w0, h0, h1;
            cam_size(ed.cam_drag_w, m.width, m.height, m.cam_w, m.cam_h, &w0, &h0);
            cam_size(ed.cam_drag_w + gx * dx, m.width, m.height, m.cam_w, m.cam_h, &w, &h1);
            x = ed.cam_drag_x + gx * (w - w0) / 2, y = ed.cam_drag_y + gy * (h1 - h0) / 2;
        }
        adv_cam_set(k, x, y, w);
    } else if (ImGui::IsItemDeactivated()) {
        ed.cam_drag = 0;
    }
    ed.cam_hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
    if (ed.cam_hot) {
        bool nwse = (it.cam_x < 0.5f) == (it.cam_y < 0.5f);
        ImGui::SetMouseCursor(ed.cam_drag == 2 || (!ed.cam_drag && on_grip)
                                  ? (nwse ? ImGuiMouseCursor_ResizeNWSE : ImGuiMouseCursor_ResizeNESW)
                                  : ImGuiMouseCursor_ResizeAll);
    }
}

// The webcam over the kept frame (k0..k1), faded as its item is at the playhead; an outline and the sizing grip while
// the mouse is on it
static void adv_cam_draw(ImVec2 k0, ImVec2 k1)
{
    Editor &ed = g_app.editor;
    const EditRecord &e = ed.edit;
    const MediaInfo &m = ed.item.info;
    ed.cam_k[0] = k0.x, ed.cam_k[1] = k0.y, ed.cam_k[2] = k1.x, ed.cam_k[3] = k1.y;
    i32 k = m.cam_w > 0 && m.width > 0 && m.height > 0 ? item_at(e, EDIT_CAM_TRACK, ed.adv_u) : -1;
    if (k < 0) return;
    const EditItem &it = e.items[k];
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 a, b;
    adv_cam_screen(it, &a, &b);
    f64 gain = item_fade_gain(e, ed_duration(), k, adv_src(it, ed.adv_u));
    u32 alpha = (u32)(ImClamp(gain, 0.0, 1.0) * 255 + 0.5);
    const Player &c = ed.cam;
    if (c.srv && c.frame_ready && c.loaded) {
        dl->AddImage((ImTextureID)(intptr_t)c.srv, a, b, ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, alpha));
    } else { // still being copied out, or it couldn't be
        dl->AddRectFilled(a, b, IM_COL32(48, 44, 60, alpha * 3 / 4));
        ImVec2 ts = ImGui::CalcTextSize("Webcam");
        if (ts.x < b.x - a.x)
            dl->AddText(ImVec2(floorf((a.x + b.x - ts.x) / 2), floorf((a.y + b.y - ts.y) / 2)), col32(COL_DIM), "Webcam");
    }
    if (ed.cam_hot) {
        dl->AddRect(a, b, col32(COL_ACCENT), 0, 0, kit_px(2));
        f32 s = kit_px(5);
        ImVec2 g(it.cam_x < 0.5f ? b.x : a.x, it.cam_y < 0.5f ? b.y : a.y);
        dl->AddRectFilled(ImVec2(g.x - s, g.y - s), ImVec2(g.x + s, g.y + s), col32(COL_ACCENT));
    }
}

static void adv_timeline(ImVec2 o, f32 w, f32 h)
{
    Editor &ed = g_app.editor;
    Library &lib = g_app.library;
    Player &p = ed.player;
    EditRecord &e = ed.edit;
    ImGuiIO &io = ImGui::GetIO();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    f32 fh = ImGui::GetFrameHeight();
    f64 dur = ed_duration();
    if (dur <= 0) return;
    f64 alen = ed_axis_len(), slen = adv_len(e), play_u = ed.adv_u;
    ed_view_clamp();
    if (ed.adv_playing && !ed.adv_drag && ed.lane_drag == ED_DRAG_NONE &&
        (play_u < ed.view_from || play_u > ed.view_from + ed.view_span)) {
        ed.view_from = play_u - ed.view_span * 0.05; // follow the playhead a page at a time
        ed_view_clamp();
    }
    f64 from = ed.view_from, span = ed.view_span;

    f32 gut = kit_px(176), gap = kit_px(3), round = kit_px(6);
    f32 bx0 = o.x + gut, bx1 = o.x + w, bw = bx1 - bx0;
    if (bw < kit_px(40)) return;
    f32 bar_h = wa_viewbar_h(), ruler_h = wa_ruler_h(), video_h = kit_px(56), lane_h = ed_lane_h();
    auto xu = [&](f64 u) { return bx0 + (f32)((u - from) / span) * bw; };
    auto u_of = [&](f32 x) {
        f64 u = from + (f64)(x - bx0) / bw * span;
        return u < 0 ? 0 : u > alen ? alen : u;
    };
    f32 mx = io.MousePos.x, my = io.MousePos.y;
    bool over = ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(o, ImVec2(o.x + w, o.y + h), false);

    i32 tracks[MEDIA_MAX_TRACKS];
    i32 nt = ed_tracks(ed.item.info, tracks, MEDIA_MAX_TRACKS), nc = ed.item.info.cam_w > 0 ? 1 : 0;
    f32 bar_y = o.y, ruler_y = bar_y + bar_h + kit_px(6), video_y = ruler_y + ruler_h;
    f32 zoom_y = video_y + video_h + gap, color_y = zoom_y + lane_h + gap;
    f32 area0 = color_y + lane_h + gap, content = (nc + nt) * (lane_h + gap), area1 = o.y + h;
    if (area1 > area0 + content) area1 = area0 + content;
    if (area1 < area0) area1 = area0;
    f32 max_scroll = content - (area1 - area0) > 0 ? content - (area1 - area0) : 0;
    f32 rows_y1 = nc + nt ? area1 : color_y + lane_h;

    // The wheel pans; with Alt it zooms around the mouse, with Alt and Shift around the playhead; Shift alone sets the
    // rows' height; over the track names it scrolls them. The middle button pans.
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
    if (over && ImGui::IsMouseClicked(ImGuiMouseButton_Middle) && ed.lane_drag == ED_DRAG_NONE && !ed.adv_drag) {
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

    // The view bar: drag the window to pan, an end to zoom, beside it to bring it there; double-click shows it all.
    ImGui::SetCursorScreenPos(ImVec2(bx0, bar_y - kit_px(4)));
    ImGui::InvisibleButton("##adv_view", ImVec2(bw, bar_h + kit_px(8)));
    {
        auto bx = [&](f64 t) { return bx0 + (f32)(t / alen) * bw; };
        f32 tx0 = bx(ed.view_from), tx1 = bx(ed.view_from + ed.view_span), grip = wa_grab();
        if (ImGui::IsItemActivated()) {
            ed.drag_mx = mx;
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
            if (ed.lane_drag == ED_DRAG_BAR) ed.view_from = ed.drag_t + (mx - ed.drag_mx) / bw * alen;
            else if (ed.lane_drag == ED_DRAG_BAR_IN && bt < ed.drag_t) ed.view_from = bt, ed.view_span = ed.drag_t - bt;
            else if (ed.lane_drag == ED_DRAG_BAR_OUT && bt > ed.drag_t) ed.view_span = bt - ed.drag_t;
            ed_view_clamp();
        } else if (ImGui::IsItemDeactivated()) {
            ed.lane_drag = ED_DRAG_NONE;
        }
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            ed.view_from = 0;
            ed.view_span = alen;
        }
        bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
        if (hot && (fabsf(mx - tx0) <= grip || fabsf(mx - tx1) <= grip)) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        wa_draw_viewbar(dl, bx0, bx1, bar_y, tx0, tx1, hot, bx(0), bx(slen), bx(play_u));
    }
    from = ed.view_from;
    span = ed.view_span;

    // Where each row is: row 0 the picture (track 0), then the webcam's when there is one (nc rows, EDIT_CAM_TRACK),
    // then one for each of `tracks` (sound track t is track 1 + t).
    auto row_y = [&](i32 r, f32 *y0, f32 *y1) {
        if (!r) *y0 = video_y, *y1 = video_y + video_h;
        else *y0 = area0 + (r - 1) * (lane_h + gap) - ed.lane_scroll, *y1 = *y0 + lane_h;
    };
    auto row_track = [&](i32 r) { return !r ? 0 : r <= nc ? (i32)EDIT_CAM_TRACK : 1 + tracks[r - 1 - nc]; };
    i32 nrows = 1 + nc + nt;
    // The row under y, -1 for none (the ruler or below)
    auto row_at = [&](f32 y) {
        for (i32 r = 0; r < nrows; r++) {
            f32 y0, y1;
            row_y(r, &y0, &y1);
            if (r && (y1 <= area0 || y0 >= area1)) continue;
            if (y >= y0 && y < y1 + gap) return r;
        }
        return -1;
    };
    // The item on a track under x (by what's drawn), -1 none
    auto item_under = [&](i32 track, f32 x) {
        i32 best = -1;
        for (i32 i = 0; i < e.item_count; i++) {
            const EditItem &it = e.items[i];
            if (it.track != track || x < xu(it.start) || x >= xu(item_end(it))) continue;
            if (best < 0 || it.start > e.items[best].start) best = i;
        }
        return best;
    };

    // Keys: the picture items' zoom and picked colour on the two lanes under the picture, each sound item's volume
    // over its waveform. Where key q is drawn (its lane's top and bottom in y0 and y1); false when it isn't shown.
    ed.color_pick = ed.color_pick < 0 || ed.color_pick >= EDIT_COLORS ? 0 : ed.color_pick;
    f32 vh = lane_h - 2 * ln_pad();
    auto key_place = [&](const ItemKey &q, ImVec2 *c, f32 *y0, f32 *y1) {
        i32 i = item_find(e, q.item);
        if (i < 0) return false;
        const EditItem &it = e.items[i];
        if (q.what == IKEY_ZOOM) {
            *y0 = zoom_y, *y1 = zoom_y + lane_h;
        } else if (q.what == IKEY_COLOR + ed.color_pick) {
            *y0 = color_y, *y1 = color_y + lane_h;
        } else if (q.what == IKEY_VOLUME) {
            i32 r = 0;
            for (i32 k = 0; k < nt; k++)
                if (1 + tracks[k] == it.track) r = k + 1 + nc;
            if (!r) return false;
            row_y(r, y0, y1);
            if (*y1 <= area0 || *y0 >= area1) return false;
        } else {
            return false;
        }
        *c = ImVec2(xu(it.start + (q.t - it.from)), ln_y(q.what == IKEY_ZOOM ? 0 : 1, q.v, *y0, *y1));
        return true;
    };
    auto key_hit = [&](f32 x, f32 y) {
        i32 best = -1;
        f32 bd = kit_px(8) * kit_px(8);
        for (i32 k = 0; k < e.ikey_count; k++) {
            ImVec2 c;
            f32 y0, y1;
            if (!key_place(e.ikeys[k], &c, &y0, &y1)) continue;
            f32 d = (x - c.x) * (x - c.x) + (y - c.y) * (y - c.y);
            if (d < bd) bd = d, best = k;
        }
        return best;
    };
    auto lane_at = [&](f32 y) { // 0 the zoom lane, 1 the colour lane, -1 neither
        return y >= zoom_y && y < zoom_y + lane_h ? 0 : y >= color_y && y < color_y + lane_h ? 1 : -1;
    };

    // The press: on a key it drags the key; a double-click off one (or Ctrl on a key lane) adds one. On an item it
    // takes hold of an edge (trim) or the body (move); elsewhere it moves the playhead.
    f32 grab = wa_grab();
    ImGui::SetCursorScreenPos(ImVec2(bx0, ruler_y));
    ImGui::InvisibleButton("##adv_lanes", ImVec2(bw, ImMax(rows_y1 - ruler_y, 1.f)));
    bool lanes_hot = ImGui::IsItemHovered();
    i32 hover_row = lanes_hot && my >= video_y ? row_at(my) : -1;
    i32 hover_item = hover_row >= 0 ? item_under(row_track(hover_row), mx) : -1;
    i32 hover_edge = 0; // -1 its start, 1 its end
    if (hover_item >= 0) {
        f32 x0 = xu(e.items[hover_item].start), x1 = xu(item_end(e.items[hover_item]));
        bool room = x1 - x0 > 3 * grab;
        if (mx - x0 <= grab && room) hover_edge = -1;
        else if (x1 - mx <= grab && room) hover_edge = 1;
    }
    // The selected items or the grabbed one and its links, as they were when the drag began
    auto in_drag = [&](const EditItem &f) {
        if (ed.adv_drag == ADV_DRAG_MOVE) return adv_selected(f.id);
        i32 g = item_find(ed.drag_from, ed.adv_drag_id);
        if (g < 0) return false;
        const EditItem &gi = ed.drag_from.items[g];
        return f.id == gi.id || (!io.KeyAlt && gi.link && f.link == gi.link);
    };
    // With Shift, a moved edge catches the playhead or another item's edge that comes close on screen
    auto snap = [&](f64 d, const f64 *edges, i32 ne) {
        if (!io.KeyShift) return d;
        f32 best = kit_px(10);
        f64 to = d;
        for (i32 a = 0; a < ne; a++) {
            f64 targets[1 + 2 * EDIT_MAX_ITEMS];
            i32 nt2 = 0;
            targets[nt2++] = ed.adv_drag_u >= 0 ? play_u : 0;
            for (i32 i = 0; i < ed.drag_from.item_count; i++) {
                const EditItem &f = ed.drag_from.items[i];
                if (in_drag(f)) continue;
                targets[nt2++] = f.start;
                targets[nt2++] = item_end(f);
            }
            for (i32 q = 0; q < nt2; q++) {
                f32 px = fabsf(xu(edges[a] + d) - xu(targets[q]));
                if (px <= best) best = px, to = targets[q] - edges[a];
            }
        }
        return to;
    };
    if (ImGui::IsItemActivated()) {
        ed.drag_mx = mx;
        ed.drag_my = my;
        ed.drag_moved = false;
        ed.adv_drag_u = u_of(mx);
        bool dbl = ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
        i32 hit = key_hit(mx, my), lane = lane_at(my), add_i = -1, add_what = 0, add_l = 1;
        f32 ay0 = 0, ay1 = 0;
        if (hit < 0 && lane >= 0 && (dbl || io.KeyCtrl)) {
            add_i = item_under(0, mx);
            add_what = lane ? IKEY_COLOR + ed.color_pick : IKEY_ZOOM, add_l = lane ? 1 : 0;
            ay0 = lane ? color_y : zoom_y, ay1 = ay0 + lane_h;
        } else if (hit < 0 && dbl && hover_row > nc && hover_item >= 0) {
            add_i = hover_item, add_what = IKEY_VOLUME;
            row_y(hover_row, &ay0, &ay1);
        }
        if (add_i >= 0) {
            f64 s = floor(adv_src(e.items[add_i], u_of(mx)) / ed_frame() + 0.5) * ed_frame();
            hit = adv_key_at(e, add_i, add_what, s);
            if (hit >= 0) e.ikeys[hit].v = ln_snap(add_l, ln_v_of(add_l, my, ay0, ay1), vh);
            ed.dirty = true;
            ed.heard_set = false;
        }
        ed.drag_from = e;
        if (hit >= 0) {
            ed.adv_key = ed.adv_drag_key = hit;
            ed.adv_drag = ADV_DRAG_KEY;
        } else if (hover_item >= 0) {
            ed.adv_key = -1;
            const EditItem &it = e.items[hover_item];
            bool add = io.KeyCtrl || io.KeyShift;
            if (add) adv_select(hover_item, !io.KeyAlt, true, true);
            else if (!adv_selected(it.id) || io.KeyAlt) adv_select(hover_item, !io.KeyAlt, false);
            ed.adv_drag_id = it.id;
            ed.adv_drag = hover_edge < 0 ? ADV_DRAG_TRIM_IN : hover_edge > 0 ? ADV_DRAG_TRIM_OUT : ADV_DRAG_MOVE;
            if (add && !adv_selected(it.id)) ed.adv_drag = ADV_DRAG_NONE; // a Ctrl click that took it out
        } else {
            ed.adv_key = -1;
            ed.adv_drag = ADV_DRAG_SEEK;
            p.scrubbing = true;
            adv_seek(ed.adv_drag_u);
        }
    }
    if (ImGui::IsItemActive() && ed.adv_drag) {
        if (fabsf(mx - ed.drag_mx) > kit_px(3)) ed.drag_moved = true;
        f64 du = (mx - ed.drag_mx) / bw * span;
        if (ed.adv_drag == ADV_DRAG_KEY) {
            if (fabsf(my - ed.drag_my) > kit_px(3)) ed.drag_moved = true;
            if (ed.drag_moved) adv_drag_key(du, -(f64)(my - ed.drag_my) / vh, (f32)(bw / span), vh, io.KeyShift, io.KeyCtrl);
        } else if (ed.adv_drag == ADV_DRAG_SEEK) {
            f64 u = u_of(mx);
            if (fabs(u - ed.adv_u) > 1e-6) adv_seek(u);
        } else if (ed.drag_moved) {
            const EditRecord &f = ed.drag_from;
            f64 lo = -1e9, hi = 1e9, edges[2 * EDIT_MAX_ITEMS];
            i32 ne = 0;
            for (i32 i = 0; i < f.item_count; i++) {
                const EditItem &q = f.items[i];
                if (!in_drag(q)) continue;
                f64 len = q.to - q.from;
                if (ed.adv_drag == ADV_DRAG_MOVE) {
                    lo = ImMax(lo, -q.start);
                    edges[ne++] = q.start, edges[ne++] = item_end(q);
                } else if (ed.adv_drag == ADV_DRAG_TRIM_IN) {
                    lo = ImMax(lo, ImMax(-q.from, -q.start));
                    hi = ImMin(hi, len - PIECE_LEAST);
                    edges[ne++] = q.start;
                } else {
                    lo = ImMax(lo, PIECE_LEAST - len);
                    hi = ImMin(hi, dur - q.to);
                    edges[ne++] = item_end(q);
                }
            }
            du = snap(du, edges, ne);
            du = du < lo ? lo : du > hi ? hi : du;
            for (i32 i = 0; i < e.item_count && i < f.item_count; i++) {
                const EditItem &q = f.items[i];
                EditItem &it = e.items[i];
                if (it.id != q.id || !in_drag(q)) continue;
                if (ed.adv_drag == ADV_DRAG_MOVE) it.start = q.start + du;
                else if (ed.adv_drag == ADV_DRAG_TRIM_IN) it.start = q.start + du, it.from = q.from + du;
                else it.to = q.to + du;
            }
            ed.dirty = true;
            adv_resync();
        }
    } else if (ImGui::IsItemDeactivated() && ed.adv_drag) {
        if (ed.adv_drag == ADV_DRAG_KEY) {
            if (!ed.drag_moved && adv_key_ok()) { // a click on a key: the playhead goes to it
                const ItemKey &q = e.ikeys[ed.adv_key];
                i32 i = item_find(e, q.item);
                if (i >= 0) adv_seek(e.items[i].start + (q.t - e.items[i].from));
            }
        } else if (ed.adv_drag == ADV_DRAG_SEEK) {
            p.scrubbing = false;
            if (!ed.drag_moved && !io.KeyCtrl && !io.KeyShift) ed.adv_sel_count = 0;
            adv_seek(ed.adv_u);
        } else if (ed.drag_moved) {
            adv_changed();
        } else if (ed.adv_drag == ADV_DRAG_MOVE && !io.KeyCtrl && !io.KeyShift) {
            i32 i = item_find(e, ed.adv_drag_id); // a plain click on a selected item selects it (and its links) alone
            if (i >= 0) adv_select(i, !io.KeyAlt, false);
        }
        ed.adv_drag = ADV_DRAG_NONE;
    }
    if (ed.adv_drag == ADV_DRAG_TRIM_IN || ed.adv_drag == ADV_DRAG_TRIM_OUT || (lanes_hot && !ed.adv_drag && hover_edge))
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (ed.adv_drag == ADV_DRAG_KEY || (lanes_hot && !ed.adv_drag && key_hit(mx, my) >= 0))
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

    // A right click on a key: its ease, or Delete. On an item: its menu (it's selected first, with its links, unless
    // it already is).
    static f64 menu_u;
    i32 right_key = lanes_hot && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && !ed.adv_drag ? key_hit(mx, my) : -1;
    if (right_key >= 0) {
        ed.adv_key = right_key;
        ImGui::OpenPopup("##adv_key");
    } else if (lanes_hot && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && !ed.adv_drag) {
        if (hover_item >= 0 && !adv_selected(e.items[hover_item].id)) adv_select(hover_item, !io.KeyAlt, false);
        if (hover_item >= 0) {
            menu_u = u_of(mx);
            ImGui::OpenPopup("##adv_item");
        }
    }
    lib_popup_style_push(6);
    if (ImGui::BeginPopup("##adv_key")) {
        if (adv_key_ok()) {
            static const char *const eases[EASE_COUNT] = {"Linear", "Smooth", "Ease in", "Ease out", "Hold"};
            for (i32 i = 0; i < EASE_COUNT; i++)
                if (lib_menu_item(e.ikeys[ed.adv_key].ease == i ? ICON_CHECK : "", eases[i])) adv_set_ease(i);
            ImGui::Separator();
            if (lib_menu_item(ICON_DELETE, "Delete", true)) adv_delete_key();
        }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("##adv_item")) {
        if (lib_menu_item("", "Split at the playhead")) adv_split(ed.adv_u);
        if (lib_menu_item("", "Split here")) adv_split(menu_u);
        ImGui::Separator();
        if (ed.adv_sel_count > 1 && !adv_sel_linked() && lib_menu_item(ICON_LINK, "Link")) adv_link_selected(true);
        if (lib_menu_item(ICON_LINK, "Unlink")) adv_link_selected(false);
        ImGui::Separator();
        if (lib_menu_item(ICON_DELETE, "Delete", true)) adv_delete_selected();
        ImGui::EndPopup();
    }
    lib_popup_style_pop();

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
            f32 x = floorf(xu(i * minor));
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

    // The rows: the gutter (the track's name, and Mute and Solo for sound) and the items.
    for (i32 r = 0; r < nrows; r++) {
        f32 y0, y1;
        row_y(r, &y0, &y1);
        if (r && (y1 <= area0 || y0 >= area1)) continue;
        i32 track = row_track(r), t = r > nc ? tracks[r - 1 - nc] : -1;
        if (r) ImGui::PushClipRect(ImVec2(o.x, area0), ImVec2(bx1, area1), true);
        bool muted = t >= 0 && e.muted[t];
        const char *name = t >= 0 ? ed.item.info.tracks[t] : r ? "Webcam" : "Picture";
        u32 kind_rgb = t >= 0 ? KIND_LOOK[ed_track_kind(name, t)].rgb : r ? 0xb48cff : 0x6aa7ff;
        dl->AddRectFilled(ImVec2(o.x, y0), ImVec2(bx0 - gap, y1), col32(COL_PANEL), round, ImDrawFlags_RoundCornersLeft);
        dl->AddRectFilled(ImVec2(bx0, y0), ImVec2(bx1, y1), col32(COL_PANEL), round, ImDrawFlags_RoundCornersRight);
        dl->AddRectFilled(ImVec2(o.x, y0), ImVec2(o.x + kit_px(3), y1), ed_rgba(kind_rgb, muted ? 70 : 220), round,
                          ImDrawFlags_RoundCornersLeft);
        ImGui::PushID(r);
        f32 gx = o.x + kit_px(6), by = floorf((y0 + y1 - fh) / 2), nx = gx;
        if (t >= 0) {
            ImGui::SetCursorScreenPos(ImVec2(gx, by));
            kit_next_tip(muted ? "Unmute this track" : "Mute this track");
            if (kit_icon_button(muted ? ICON_MUTE : ICON_VOLUME, "##mute", KIT_GHOST, true, muted)) {
                e.muted[t] = !muted;
                ed.dirty = true;
                ed.heard_set = false;
                muted = !muted;
            }
            bool solo = ed.solo == t;
            ImGui::SetCursorScreenPos(ImVec2(gx + fh + kit_px(2), by));
            kit_next_tip(solo ? "Hear every track" : "Hear only this track");
            if (kit_icon_button(ICON_SOLO, "##solo", KIT_GHOST, true, solo)) {
                ed.solo = solo ? -1 : t;
                ed.heard_set = false;
            }
            nx = gx + 2 * fh + kit_px(8);
        } else {
            nx = gx + kit_px(4);
        }
        ImGui::PopID();
        f32 lh = ImGui::GetTextLineHeight();
        text_clipped(dl, ImVec2(nx, floorf((y0 + y1 - lh) / 2)), bx0 - gap - nx - kit_px(6), col32(muted ? COL_FAINT : COL_TEXT), name);

        // The items: the picture's frames or the sound's waveform with its volume line, shaded over the fades, a
        // light band over a crossfade, and an outline when selected
        dl->PushClipRect(ImVec2(bx0, y0), ImVec2(bx1, y1), true);
        i32 idx[EDIT_MAX_ITEMS], n = track_items(e, track, idx);
        ItemFades fd[EDIT_MAX_ITEMS];
        track_fades(e, dur, idx, n, fd);
        i32 wave_c = r - 1 - nc;
        for (i32 k = 0; k < n; k++) {
            const EditItem &it = e.items[idx[k]];
            f32 x0 = xu(it.start), x1 = xu(item_end(it));
            if (x1 < bx0 || x0 > bx1) continue;
            ImVec2 a(x0, y0 + kit_px(2)), b(x1, y1 - kit_px(2));
            bool sel = adv_selected(it.id), hot = idx[k] == hover_item && !ed.adv_drag;
            dl->AddRectFilled(a, b, ed_rgba(kind_rgb, sel ? 70 : 44), kit_px(4));
            f32 cx0 = ImMax(x0, bx0), cx1 = ImMin(x1, bx1);
            dl->PushClipRect(a, b, true);
            if (!r && lib_want_strip(ed.item) && lib.strip_frames > 0) {
                f32 ih = b.y - a.y, aspect = ed.item.info.height > 0 ? (f32)ed.item.info.width / ed.item.info.height : 16.f / 9;
                f32 tile = ih * (aspect > 0.3f ? aspect : 1.f);
                for (f32 x = x0; x < cx1; x += tile) {
                    if (x + tile < cx0) continue;
                    f64 s = adv_src(it, u_of(x + tile / 2));
                    i32 fr = (i32)(s / dur * lib.strip_frames);
                    fr = fr < 0 ? 0 : fr >= lib.strip_frames ? lib.strip_frames - 1 : fr;
                    f32 u0 = (f32)fr / lib.strip_frames, u1 = (f32)(fr + 1) / lib.strip_frames;
                    dl->AddImage((ImTextureID)(intptr_t)lib.strip, ImVec2(x, a.y), ImVec2(x + tile, b.y), ImVec2(u0, 0), ImVec2(u1, 1));
                }
            } else if (t >= 0 && ed.wave) {
                f32 step = floorf(kit_px(2)) > 1 ? floorf(kit_px(2)) : 1, reach = b.y - a.y - kit_px(2);
                for (f32 x = cx0; x < cx1; x += step) {
                    f64 s0 = adv_src(it, u_of(x)), s1 = adv_src(it, u_of(x + step));
                    f32 pk = 0;
                    if (!wave_peak(ed.wave, wave_c, s0, s1, &pk)) {
                        dl->AddRectFilled(ImVec2(x, b.y - kit_px(2)), ImVec2(x + step, b.y), IM_COL32(142, 147, 156, 40));
                        continue;
                    }
                    f64 sm = (s0 + s1) / 2;
                    f32 g = (f32)(item_level_at(e, it.id, IKEY_VOLUME, sm) * item_fade_gain(e, dur, idx[k], sm));
                    f32 en = pk * g / 2, hh = sqrtf(en < 1 ? en : 1) * reach;
                    if (hh >= 0.5f) dl->AddRectFilled(ImVec2(x, b.y - hh), ImVec2(x + step, b.y), ed_rgba(kind_rgb, muted ? 40 : 150));
                }
            }
            if (t >= 0) { // the volume, as the render moves it
                static ImVec2 vpts[2048];
                i32 np = 0;
                for (f32 x = cx0; np < COUNT(vpts); x += kit_px(3)) {
                    if (x > cx1) x = cx1;
                    vpts[np++] = ImVec2(x, ln_y(1, item_level_at(e, it.id, IKEY_VOLUME, adv_src(it, u_of(x))), y0, y1));
                    if (x >= cx1) break;
                }
                dl->AddPolyline(vpts, np, col32(muted ? COL_FAINT : COL_TEXT, 0.8f), ImDrawFlags_None, kit_px(1.5f));
            }
            u32 dark = IM_COL32(0, 0, 0, 190), clear = IM_COL32(0, 0, 0, 0);
            if (fd[k].in > 0) {
                f32 fx = xu(it.start + fd[k].in);
                dl->AddRectFilledMultiColor(a, ImVec2(fx, b.y), dark, clear, clear, dark);
            }
            if (fd[k].out > 0) {
                f32 fx = xu(item_end(it) - fd[k].out);
                dl->AddRectFilledMultiColor(ImVec2(fx, a.y), b, clear, dark, dark, clear);
            }
            dl->PopClipRect();
            if (fd[k].cross > 0) {
                f32 c0 = xu(it.start - fd[k].cross / 2), c1 = xu(it.start + fd[k].cross / 2);
                dl->AddRectFilled(ImVec2(c0, a.y), ImVec2(c1, b.y), IM_COL32(255, 255, 255, 46));
            }
            if (sel) dl->AddRect(a, b, col32(COL_ACCENT), kit_px(4), 0, kit_px(2));
            else if (hot) dl->AddRect(a, b, col32(COL_TEXT, 0.35f), kit_px(4), 0, kit_px(1));
        }
        dl->PopClipRect();
        if (r) ImGui::PopClipRect();
    }

    // The key lanes: the gutter (for the colour, which value it shows; a key at the playhead, added or deleted; the
    // name and the value there), then the value's line through each picture item.
    i32 pic_now = item_at(e, 0, play_u);
    for (i32 ln = 0; ln < 2; ln++) {
        f32 y0 = ln ? color_y : zoom_y, y1 = y0 + lane_h;
        i32 what = ln ? IKEY_COLOR + ed.color_pick : IKEY_ZOOM, l = ln ? 1 : 0;
        dl->AddRectFilled(ImVec2(o.x, y0), ImVec2(bx0 - gap, y1), col32(COL_PANEL), round, ImDrawFlags_RoundCornersLeft);
        dl->AddRectFilled(ImVec2(bx0, y0), ImVec2(bx1, y1), col32(COL_PANEL), round, ImDrawFlags_RoundCornersRight);
        auto value_at = [&](i32 i, f64 s) {
            if (what != IKEY_ZOOM) return item_level_at(e, e.items[i].id, what, s);
            f64 z, zx, zy;
            item_zoom_at(e, e.items[i].id, s, &z, &zx, &zy);
            return z;
        };
        ImGui::PushID(100 + ln);
        f32 gx = o.x + kit_px(6), by = floorf((y0 + y1 - fh) / 2);
        if (ln) {
            ImGui::SetCursorScreenPos(ImVec2(gx, by));
            kit_next_tip("Which colour this lane shows");
            if (kit_icon_button(ICON_SLIDERS, "##pick", KIT_GHOST)) ImGui::OpenPopup("##adv_color_pick");
            lib_popup_style_push(6);
            if (ImGui::BeginPopup("##adv_color_pick")) {
                for (i32 c = 0; c < EDIT_COLORS; c++)
                    if (lib_menu_item(c == ed.color_pick ? ICON_CHECK : "", COLOR_NAMES[c])) ed.color_pick = c;
                ImGui::EndPopup();
            }
            lib_popup_style_pop();
        }
        i32 here = -1;
        f64 s_now = pic_now >= 0 ? adv_src(e.items[pic_now], play_u) : 0;
        for (i32 k = 0; pic_now >= 0 && k < e.ikey_count; k++)
            if (e.ikeys[k].item == e.items[pic_now].id && e.ikeys[k].what == what &&
                fabs(e.ikeys[k].t - s_now) < ed_frame() / 2 + 0.01)
                here = k;
        ImGui::SetCursorScreenPos(ImVec2(gx + fh + kit_px(2), by));
        kit_next_tip(here >= 0 ? "Delete this key" : "Add a key at the playhead");
        if (kit_icon_button(here >= 0 ? ICON_DELETE : ICON_ADD, "##key", KIT_GHOST, pic_now >= 0)) {
            if (here >= 0) {
                ed.adv_key = here;
                adv_delete_key();
            } else {
                ed.adv_key = adv_key_at(e, pic_now, what, s_now);
                ed.dirty = true;
                ed.heard_set = false;
            }
        }
        ImGui::PopID();
        f32 nx = gx + 2 * fh + kit_px(8), nw = bx0 - gap - nx - kit_px(6), lh = ImGui::GetTextLineHeight();
        f32 ty = floorf((y0 + y1 - lh - g_font_small->FontSize - kit_px(2)) / 2);
        text_clipped(dl, ImVec2(nx, ty), nw, col32(COL_TEXT), ln ? COLOR_NAMES[ed.color_pick] : "Zoom");
        char vt[24];
        f64 v = pic_now >= 0 ? value_at(pic_now, s_now) : 1;
        if (ln) snprintf(vt, sizeof vt, "%.0f%%", v * 100);
        else snprintf(vt, sizeof vt, "%.1fx", v);
        ImGui::PushFont(g_font_small);
        text_clipped(dl, ImVec2(nx, ty + lh + kit_px(2)), nw, col32(COL_DIM), vt);
        ImGui::PopFont();

        dl->PushClipRect(ImVec2(bx0, y0), ImVec2(bx1, y1), true);
        f32 rest = ln_y(l, 1, y0, y1);
        dl->AddRectFilled(ImVec2(bx0, rest), ImVec2(bx1, rest + kit_px(1)), col32(COL_LINE));
        i32 idx[EDIT_MAX_ITEMS], n = track_items(e, 0, idx);
        static ImVec2 pts[2048];
        for (i32 k = 0; k < n; k++) {
            const EditItem &it = e.items[idx[k]];
            f32 x0 = ImMax(xu(it.start), bx0), x1 = ImMin(xu(item_end(it)), bx1);
            if (x1 <= x0) continue;
            dl->AddRectFilled(ImVec2(x0, y0 + kit_px(2)), ImVec2(x1, y1 - kit_px(2)), ed_rgba(0x6aa7ff, 18), kit_px(4));
            i32 np = 0;
            for (f32 x = x0; np < COUNT(pts); x += kit_px(3)) {
                if (x > x1) x = x1;
                pts[np++] = ImVec2(x, ln_y(l, value_at(idx[k], adv_src(it, u_of(x))), y0, y1));
                if (x >= x1) break;
            }
            dl->AddPolyline(pts, np, col32(ln ? COL_WARN : COL_BLUE), ImDrawFlags_None, kit_px(1.5f));
        }
        dl->PopClipRect();
    }

    // The keys, by their ease (the selected one lit)
    for (i32 k = 0; k < e.ikey_count; k++) {
        const ItemKey &q = e.ikeys[k];
        ImVec2 c;
        f32 y0, y1;
        if (!key_place(q, &c, &y0, &y1) || c.x < bx0 - kit_px(12) || c.x > bx1 + kit_px(12)) continue;
        bool on = k == ed.adv_key;
        u32 col = col32(on ? COL_ACCENT : q.what == IKEY_ZOOM ? COL_BLUE : q.what == IKEY_VOLUME ? 0xd6d6d6 : COL_WARN);
        dl->PushClipRect(ImVec2(bx0 - kit_px(6), q.what == IKEY_VOLUME ? area0 : y0), ImVec2(bx1 + kit_px(6), q.what == IKEY_VOLUME ? area1 : y1), true);
        key_shape(dl, c, kit_px(7), q.ease, on ? IM_COL32_WHITE : IM_COL32(0, 0, 0, 170));
        key_shape(dl, c, kit_px(5), q.ease, col);
        dl->PopClipRect();
    }

    // The playhead, and the time under the mouse (or at the playhead while it's dragged) on the ruler
    dl->PushClipRect(ImVec2(bx0 - kit_px(6), ruler_y), ImVec2(bx1 + kit_px(6), rows_y1), true);
    f32 px = xu(ed.adv_u);
    if (px >= bx0 - 1 && px <= bx1 + 1) wa_draw_playhead(dl, px, video_y, rows_y1);
    bool idle = lanes_hot && !ed.adv_drag;
    f64 pill_u = idle ? u_of(mx) : ed.adv_drag == ADV_DRAG_SEEK ? ed.adv_u : -1;
    if (pill_u >= 0) {
        f32 x = xu(pill_u);
        if (idle) dl->AddLine(ImVec2(x, video_y), ImVec2(x, rows_y1), col32(COL_TEXT, 0.3f));
        char when[32];
        fmt_time(pill_u, when, sizeof when, true);
        ImGui::PushFont(g_font_small);
        wa_pill(dl, x, ruler_y + kit_px(2), bx0, bx1, when, col32(COL_HOVER), col32(COL_TEXT));
        ImGui::PopFont();
    }
    dl->PopClipRect();
}

// ---- The side panel ----

// Zoom and colour for the picture item under the playhead, volume for the sound items there, and the fades and
// crossfade of the selected items (or those under the playhead). Each slider keys its value at the playhead.
static void adv_edit_panel(f32 w)
{
    Editor &ed = g_app.editor;
    EditRecord &e = ed.edit;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    f32 fh = ImGui::GetFrameHeight(), lh = ImGui::GetTextLineHeight(), val_w = kit_px(44), cname_w = kit_px(84);
    f64 u = ed.adv_u;
    auto value_text = [&](const char *s, bool on) {
        ImVec2 vp = ImGui::GetCursorScreenPos(), vs = ImGui::CalcTextSize(s);
        dl->AddText(ImVec2(vp.x + val_w - vs.x, floorf(vp.y + (fh - vs.y) / 2)), col32(on ? COL_TEXT : COL_FAINT), s);
        ImGui::Dummy(ImVec2(val_w, fh));
    };
    auto name_text = [&](const char *s, bool on) {
        ImVec2 cp = ImGui::GetCursorScreenPos();
        dl->AddText(ImVec2(cp.x, floorf(cp.y + (fh - lh) / 2)), col32(on ? COL_TEXT : COL_FAINT), s);
        ImGui::Dummy(ImVec2(cname_w, fh));
        ImGui::SameLine(0, 0);
    };
    // A slider that keys kind `what` of item i at the playhead; double-click puts it back to `rest`
    auto key_slider = [&](const char *id, i32 i, i32 what, f32 lo, f32 hi, f32 rest, f32 sw, const char *fmt, f32 scale) {
        bool on = i >= 0;
        f64 s = on ? adv_src(e.items[i], u) : 0;
        f32 v = on ? (f32)(what == IKEY_ZOOM ? 0 : item_level_at(e, e.items[i].id, what, s)) : rest;
        if (on && what == IKEY_ZOOM) {
            f64 z, x, y;
            item_zoom_at(e, e.items[i].id, s, &z, &x, &y);
            v = (f32)z;
        }
        bool set = lib_slider(id, &v, lo, hi, sw, fh, -1e9f, on);
        if (set && what != IKEY_ZOOM && v > rest - 0.03f && v < rest + 0.03f) v = rest; // caught by 100%
        if (on && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) v = rest, set = true;
        if (on && set) {
            i32 k = adv_key_at(e, i, what, s);
            if (k >= 0) {
                e.ikeys[k].v = v;
                ed.dirty = true;
                ed.heard_set = false;
            }
        }
        ImGui::SameLine(0, 0);
        char vt[16];
        snprintf(vt, sizeof vt, fmt, v * scale);
        value_text(vt, on);
    };

    i32 pic = adv_picture_item();
    ed_caption("ZOOM");
    key_slider("##adv_zoom", pic, IKEY_ZOOM, 1.f, 4.f, 1.f, w - val_w, "%.1fx", 1);
    static const char *const views[] = {"Crop", "Footage"};
    if (kit_segmented("##zoom_view", views, 2, &g_app.config.editor_zoom_view, (w - kit_px(6)) / 2))
        config_save(g_app.config, g_app.paths);
    kit_dim(pic < 0 ? "No picture at the playhead" : g_app.config.editor_zoom_view ? "Drag the footage to aim" : "Drag the frame to aim");
    ImGui::Dummy(ImVec2(1, kit_px(8)));

    ed_caption("COLOUR");
    for (i32 c = 0; c < EDIT_COLORS; c++) {
        ImGui::PushID(c);
        name_text(COLOR_NAMES[c], pic >= 0);
        key_slider("##adv_color", pic, IKEY_COLOR + c, 0.f, 2.f, 1.f, w - cname_w - val_w, "%.0f%%", 100);
        ImGui::PopID();
    }
    ImGui::Dummy(ImVec2(1, kit_px(8)));

    // The sound at the playhead: a volume for each track's item there
    ed_caption("VOLUME");
    i32 tracks[MEDIA_MAX_TRACKS];
    i32 nt = ed_tracks(ed.item.info, tracks, MEDIA_MAX_TRACKS);
    for (i32 k = 0; k < nt; k++) {
        ImGui::PushID(k);
        i32 t = tracks[k], a = item_at(e, 1 + t, u);
        name_text(ed.item.info.tracks[t], a >= 0 && !e.muted[t]);
        key_slider("##adv_vol", a, IKEY_VOLUME, 0.f, 2.f, 1.f, w - cname_w - val_w, "%.0f%%", 100);
        ImGui::PopID();
    }
    ImGui::Dummy(ImVec2(1, kit_px(8)));

    // The webcam: its size and corner, on the cam items adv_cam_targets picks
    const MediaInfo &m = ed.item.info;
    if (m.cam_w > 0) {
        ed_caption("WEBCAM");
        i32 k = item_at(e, EDIT_CAM_TRACK, u), idx[EDIT_MAX_ITEMS], n = adv_cam_targets(k, idx);
        bool on = n > 0;
        const EditItem *f = !on ? 0 : k >= 0 ? &e.items[k] : &e.items[idx[0]];
        i32 corner = f ? cam_corner_of(*f, m.width, m.height, m.cam_w, m.cam_h) : -1;
        name_text("Size", on);
        f32 v = f ? f->cam_w : CAM_DEFAULT_W;
        bool set = lib_slider("##cam_size", &v, CAM_MIN_W, 1.f, w - cname_w - val_w, fh, -1e9f, on);
        if (on && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) v = CAM_DEFAULT_W, set = true;
        if (on && set) { // a webcam in a corner stays in it
            adv_cam_set(k, f->cam_x, f->cam_y, v);
            for (i32 q = 0; q < n && corner >= 0; q++) cam_corner(&e.items[idx[q]], corner, m.width, m.height, m.cam_w, m.cam_h);
        }
        ImGui::SameLine(0, 0);
        char st[16];
        snprintf(st, sizeof st, "%.0f%%", v * 100);
        value_text(st, on);
        static const char *const top[] = {"Top left", "Top right"}, *const bottom[] = {"Bottom left", "Bottom right"};
        i32 tv = corner == CAM_TOP_LEFT ? 0 : corner == CAM_TOP_RIGHT ? 1 : -1;
        i32 bv = corner == CAM_BOTTOM_LEFT ? 0 : corner == CAM_BOTTOM_RIGHT ? 1 : -1;
        f32 half = (w - kit_px(6)) / 2;
        i32 to = -1;
        if (kit_segmented("##cam_top", top, 2, &tv, half)) to = tv ? CAM_TOP_RIGHT : CAM_TOP_LEFT;
        if (kit_segmented("##cam_bottom", bottom, 2, &bv, half)) to = bv ? CAM_BOTTOM_RIGHT : CAM_BOTTOM_LEFT;
        if (on && to >= 0) {
            for (i32 q = 0; q < n; q++) cam_corner(&e.items[idx[q]], to, m.width, m.height, m.cam_w, m.cam_h);
            ed.dirty = true;
        }
        kit_dim(!on ? "No webcam in the edit" : "Drag the webcam in the picture to move it");
        ImGui::Dummy(ImVec2(1, kit_px(8)));
    }

    // Transitions of the selected items (or those under the playhead): each takes the first one's value, and a change
    // goes to all of them
    ed_caption("TRANSITIONS");
    {
        i32 idx[EDIT_MAX_ITEMS], n = 0;
        for (i32 i = 0; i < e.item_count; i++)
            if (adv_selected(e.items[i].id)) idx[n++] = i;
        if (!n) n = adv_targets(u, idx);
        auto length_row = [&](const char *name, const char *id, i32 field) {
            bool on = n > 0;
            name_text(name, on);
            const EditItem *f = on ? &e.items[idx[0]] : 0;
            f32 v = !f ? 0 : field == 0 ? f->fade_in : field == 1 ? f->fade_out : f->cross;
            bool set = lib_slider(id, &v, 0.f, 3.f, w - cname_w - val_w, fh, -1e9f, on);
            if (on && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) v = 0, set = true;
            if (on && set) {
                v = v < TRANS_LEAST ? 0 : floorf(v * 20 + 0.5f) / 20; // in twentieths of a second
                for (i32 q = 0; q < n; q++) {
                    EditItem &it = e.items[idx[q]];
                    (field == 0 ? it.fade_in : field == 1 ? it.fade_out : it.cross) = v;
                }
                ed.dirty = true;
                ed.heard_set = false;
            }
            ImGui::SameLine(0, 0);
            char lt[16];
            if (v > 0) snprintf(lt, sizeof lt, "%.2f s", v);
            else str_set(lt, "Off");
            value_text(lt, on);
        };
        length_row("Fade in", "##adv_fade_in", 0);
        length_row("Fade out", "##adv_fade_out", 1);
        length_row("Crossfade", "##adv_cross", 2);
        kit_dim(!n ? "Select items to set their fades" : n == 1 ? "1 item" : "%d items", n);
    }
    ImGui::Dummy(ImVec2(1, kit_px(8)));

    // The selection: split, link, delete
    ed_caption(ed.adv_sel_count ? "SELECTED ITEMS" : "ITEMS");
    {
        f32 sp = ImGui::GetStyle().ItemSpacing.x, bw3 = (w - 2 * sp) / 3;
        i32 idx[EDIT_MAX_ITEMS];
        kit_next_tip("Split at the playhead", "S");
        if (kit_button("Split", KIT_GHOST, ImVec2(bw3, 0), adv_targets(u, idx) > 0)) adv_split(u);
        ImGui::SameLine();
        bool linked = adv_sel_linked();
        if (kit_button(linked ? ICON_LINK "  Unlink" : ICON_LINK "  Link", KIT_GHOST, ImVec2(bw3, 0),
                       linked ? ed.adv_sel_count > 0 : ed.adv_sel_count > 1))
            adv_link_selected(!linked);
        ImGui::SameLine();
        kit_next_tip("Delete the selected items", "Del");
        if (kit_button(ICON_DELETE "  Delete", KIT_GHOST, ImVec2(bw3, 0), ed.adv_sel_count > 0)) adv_delete_selected();
    }
    ImGui::Dummy(ImVec2(1, kit_px(8)));
}

// The keys in Advanced (never while typing or with a menu open).
static void adv_keys(bool *back)
{
    Editor &ed = g_app.editor;
    Player &p = ed.player;
    ImGuiIO &io = ImGui::GetIO();
    bool ctrl = io.KeyCtrl;
    if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) adv_toggle();
    if (!ctrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) adv_split(ed.adv_u);
    if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false)) {
        if (adv_key_ok()) adv_delete_key();
        else adv_delete_selected();
    }
    for (i32 i = 0; i < EASE_COUNT; i++)
        if (!ctrl && adv_key_ok() && ImGui::IsKeyPressed((ImGuiKey)(ImGuiKey_1 + i), false)) adv_set_ease(i);
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_A, false)) {
        ed.adv_sel_count = 0;
        for (i32 i = 0; i < ed.edit.item_count; i++) adv_sel_set(ed.edit.items[i].id, true);
    }
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) ed_undo(io.KeyShift);
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) ed_undo(true);
    if (!ctrl && (ImGui::IsKeyPressed(ImGuiKey_Equal) || ImGui::IsKeyPressed(ImGuiKey_KeypadAdd))) ed_view_zoom(0.5, ed.view_from + ed.view_span / 2);
    if (!ctrl && (ImGui::IsKeyPressed(ImGuiKey_Minus) || ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract))) ed_view_zoom(2, ed.view_from + ed.view_span / 2);
    for (i32 dir = -1; dir <= 1; dir += 2) {
        f64 u = ed.adv_u;
        if (ImGui::IsKeyPressed(dir < 0 ? ImGuiKey_LeftArrow : ImGuiKey_RightArrow)) {
            ed.adv_playing = false;
            adv_seek(io.KeyShift ? u + dir : adv_next_stop(u, dir));
        }
        if (!ctrl && ImGui::IsKeyPressed(dir < 0 ? ImGuiKey_Comma : ImGuiKey_Period)) {
            ed.adv_playing = false;
            adv_seek(u + dir * ed_frame());
        }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        if (ed.adv_key >= 0) ed.adv_key = -1;
        else if (ed.adv_sel_count) ed.adv_sel_count = 0;
        else *back = true;
    }
    (void)p;
}
