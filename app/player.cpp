// The video player the library and the editor share: one file through Media Foundation into a texture on the UI's
// D3D11 device, drawn as an ImGui image.
//
// Why the Media Engine (IMFMediaEngine in frame-server mode) and not a Source Reader: the engine does the audio
// output, the A/V clock and seeking itself, and still decodes on the GPU straight into a texture through the DXGI
// device manager. A Source Reader hands over decoded frames and samples, and we'd have to write the WASAPI
// renderer, the clock and the frame pacing. The engine's one limit is that it plays one audio track at a time and
// can't make it louder than recorded; the editor works round the first by playing each track on its own
// audio-only engine (see player_set_tracks), and the second is fine for a preview.
//
// Media Foundation calls PlayerNotify on its own threads; that only posts EV_LIBRARY events, and everything else
// happens on the main thread (player_on_media, player_update).

// EV_LIBRARY sub-kinds (`a`), shared with library.cpp.
enum LibEventKind : i64 {
    LIB_EV_SCANNED, // b: scan gen; the scan worker has published new items
    LIB_EV_RESULTS, // thumbnails or strips are ready to upload
    LIB_EV_MEDIA,   // b: (tag << 32) | MF_MEDIA_ENGINE_EVENT; text: "<param1> <param2>"
    LIB_EV_DELETED, // b: (how many were to go << 32) | how many went; a Delete of the selection has finished
};

struct MediaFoundation {
    bool                        started, failed;
    IMFDXGIDeviceManager       *dxgi;
    UINT                        token;
    IMFMediaEngineClassFactory *factory;
};

static MediaFoundation g_mf;

// Media Foundation wants a COM object to call back. This is the one place with a base class, because COM's vtable
// needs it; the objects are static, so the reference count is a dummy.
struct PlayerNotify : IMFMediaEngineNotify {
    volatile u32 tag; // player id, engine slot and the player's gen, so stale events can be told apart

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override
    {
        if (riid == __uuidof(IMFMediaEngineNotify) || riid == __uuidof(IUnknown)) {
            *out = (IMFMediaEngineNotify *)this;
            return S_OK;
        }
        *out = 0;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE EventNotify(DWORD event, DWORD_PTR param1, DWORD param2) override
    {
        if (event == MF_MEDIA_ENGINE_EVENT_NOTIFYSTABLESTATE) { // the engine waits on this
            SetEvent((HANDLE)param1);
            return S_OK;
        }
        // Only what the main thread acts on: TIMEUPDATE and PROGRESS come many times a second.
        switch (event) {
        case MF_MEDIA_ENGINE_EVENT_LOADEDMETADATA:
        case MF_MEDIA_ENGINE_EVENT_ERROR:
        case MF_MEDIA_ENGINE_EVENT_ENDED:
        case MF_MEDIA_ENGINE_EVENT_SEEKED:
        case MF_MEDIA_ENGINE_EVENT_FIRSTFRAMEREADY:
        case MF_MEDIA_ENGINE_EVENT_PLAYING:
        case MF_MEDIA_ENGINE_EVENT_PAUSE: break;
        default: return S_OK;
        }
        Event e = {};
        e.kind = EV_LIBRARY;
        e.a = LIB_EV_MEDIA;
        e.b = ((i64)tag << 32) | (i64)event;
        snprintf(e.text, sizeof e.text, "%llu %u", (unsigned long long)param1, (u32)param2);
        event_post(e);
        return S_OK;
    }
};

// [player id: 0 library, 1 editor, 2 timeline, 3 card preview][0 = video, 1.. = track engines]
static PlayerNotify g_player_notify[4][1 + PLAYER_MAX_TRACK_ENGINES];

static u32 player_tag(const Player &p, i32 slot) { return (p.id & 15) | ((u32)slot << 4) | ((p.gen & 0xffffff) << 8); }

static bool mf_init()
{
    if (g_mf.started) return !g_mf.failed;
    g_mf.started = true;
    g_mf.failed = true;
    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
    if (FAILED(hr)) { log_line("player: MFStartup failed (0x%08x)", (u32)hr); return false; }
    // Media Foundation decodes on the UI's device from its own threads, so the device's context must lock.
    ID3D11Multithread *mt = 0;
    if (SUCCEEDED(g_gpu.context->QueryInterface(IID_PPV_ARGS(&mt)))) {
        mt->SetMultithreadProtected(TRUE);
        mt->Release();
    }
    hr = MFCreateDXGIDeviceManager(&g_mf.token, &g_mf.dxgi);
    if (SUCCEEDED(hr)) hr = g_mf.dxgi->ResetDevice(g_gpu.device, g_mf.token);
    if (FAILED(hr)) { log_line("player: the DXGI device manager failed (0x%08x)", (u32)hr); return false; }
    hr = CoCreateInstance(CLSID_MFMediaEngineClassFactory, 0, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_mf.factory));
    if (FAILED(hr)) { log_line("player: no Media Engine (0x%08x)", (u32)hr); return false; }
    g_mf.failed = false;
    log_line("player: Media Foundation ready");
    return true;
}

static IMFMediaEngine *player_engine(Player *p, i32 slot, bool audio_only)
{
    PlayerNotify *n = &g_player_notify[p->id < 4 ? p->id : 0][slot];
    n->tag = player_tag(*p, slot);
    IMFAttributes *attr = 0;
    MFCreateAttributes(&attr, 3);
    attr->SetUnknown(MF_MEDIA_ENGINE_CALLBACK, (IMFMediaEngineNotify *)n);
    if (!audio_only) {
        attr->SetUnknown(MF_MEDIA_ENGINE_DXGI_MANAGER, g_mf.dxgi);
        attr->SetUINT32(MF_MEDIA_ENGINE_VIDEO_OUTPUT_FORMAT, DXGI_FORMAT_B8G8R8A8_UNORM);
    }
    IMFMediaEngine *e = 0;
    HRESULT hr = g_mf.factory->CreateInstance(audio_only ? MF_MEDIA_ENGINE_AUDIOONLY : 0, attr, &e);
    attr->Release();
    if (FAILED(hr)) {
        log_line("player: couldn't create a media engine (0x%08x)", (u32)hr);
        return 0;
    }
    BSTR url = SysAllocString(p->path.s);
    e->SetPreload(MF_MEDIA_ENGINE_PRELOAD_AUTOMATIC);
    hr = e->SetSource(url);
    SysFreeString(url);
    if (FAILED(hr)) log_line("player: SetSource failed (0x%08x)", (u32)hr);
    return e;
}

static void engine_free(IMFMediaEngine **e)
{
    if (!*e) return;
    (*e)->Shutdown();
    (*e)->Release();
    *e = 0;
}

static void player_clear_tracks(Player *p)
{
    for (i32 i = 0; i < p->track_engine_count; i++) engine_free(&p->track_engine[i]);
    p->track_engine_count = 0;
}

static void player_close(Player *p)
{
    if (!p->open && !p->engine) return;
    player_clear_tracks(p);
    if (p->engine_ex) p->engine_ex->Release();
    engine_free(&p->engine);
    if (p->srv) p->srv->Release();
    if (p->tex) p->tex->Release();
    if (p->set_animating) g_app.ui.animating = false;
    u8 id = p->id;
    u32 gen = p->gen;
    *p = {};
    p->id = id;
    p->gen = gen + 1; // late events from this file are dropped
}

// Opens `path` and starts at `start` seconds; plays at once if `autoplay`. Returns false (with p->error) if Media
// Foundation isn't there.
static bool player_open(Player *p, const Path &path, f64 start, bool autoplay)
{
    player_close(p);
    p->gen++;
    p->open = true;
    p->path = path;
    p->start_at = start;
    p->autoplay = autoplay;
    p->paused = true;
    p->sound = 0;
    if (!mf_init()) {
        str_set(p->error, "Media Foundation isn't available, so this can't play here.");
        return false;
    }
    p->engine = player_engine(p, 0, false);
    if (!p->engine) {
        str_set(p->error, "Couldn't start the video player.");
        return false;
    }
    p->engine->QueryInterface(IID_PPV_ARGS(&p->engine_ex));
    char p8[MAX_PATH * 3];
    wide_to_utf8(path.s, p8, sizeof p8);
    log_line("player %d: open %s at %.2f s", p->id, p8, start);
    ui_wake();
    return true;
}

// Which of the engine's streams are audio, in order. The MP4s clipback writes have the video first.
static void player_find_audio(Player *p)
{
    p->audio_count = 0;
    if (!p->engine_ex) return;
    DWORD n = 0;
    p->engine_ex->GetNumberOfStreams(&n);
    bool typed = false;
    for (DWORD i = 0; i < n && p->audio_count < MEDIA_MAX_TRACKS; i++) {
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(p->engine_ex->GetStreamAttribute(i, MF_MT_MAJOR_TYPE, &v)) && v.vt == VT_CLSID && v.puuid) {
            typed = true;
            if (*v.puuid == MFMediaType_Audio) p->audio_stream[p->audio_count++] = (i32)i;
        }
        PropVariantClear(&v);
    }
    if (!typed)
        for (DWORD i = 1; i < n && p->audio_count < MEDIA_MAX_TRACKS; i++) p->audio_stream[p->audio_count++] = (i32)i;
}

// Plays only audio track `track` on `e` (0 = the first, which is the Mix in clipback's files).
static void engine_pick_audio(IMFMediaEngine *e, const Player &p, i32 track)
{
    IMFMediaEngineEx *ex = 0;
    if (FAILED(e->QueryInterface(IID_PPV_ARGS(&ex)))) return;
    for (i32 i = 0; i < p.audio_count; i++) ex->SetStreamSelection(p.audio_stream[i], i == track);
    ex->ApplyStreamSelections();
    ex->Release();
}

static void player_set_sound(Player *p, i32 track)
{
    if (track < 0 || track >= p->audio_count) track = 0;
    p->sound = track;
    if (p->engine && p->loaded && p->audio_count > 1) engine_pick_audio(p->engine, *p, track);
}

static void player_seek(Player *p, f64 t)
{
    if (!p->engine) return;
    f64 end = p->duration > 0 ? p->duration : t;
    if (t > end) t = end;
    if (t < 0) t = 0;
    p->pos = t;
    p->ended = false;
    if (!p->loaded || p->engine->IsSeeking()) { // one at a time, the newest wins
        if (!p->loaded) p->start_at = t;
        else { p->has_seek = true; p->seek_to = t; }
        return;
    }
    p->has_seek = false;
    p->seeking = true;
    if (p->scrubbing && p->engine_ex) p->engine_ex->SetCurrentTimeEx(t, MF_MEDIA_ENGINE_SEEK_MODE_APPROXIMATE);
    else p->engine->SetCurrentTime(t);
    for (i32 i = 0; i < p->track_engine_count; i++)
        if (p->track_engine[i]) {
            p->track_engine[i]->SetCurrentTime(t);
            p->track_synced[i] = ImGui::GetTime();
        }
    ui_wake(6);
}

static void player_play(Player *p)
{
    if (!p->engine) return;
    p->autoplay = true;
    if (!p->loaded) return;
    // The editor plays its trimmed range: from outside it, start again at its start.
    if (p->stop_at > 0 && (p->pos >= p->stop_at - 0.02 || p->pos < p->loop_from - 0.02)) player_seek(p, p->loop_from);
    else if (p->ended || (p->duration > 0 && p->pos >= p->duration - 0.02)) player_seek(p, p->loop_from);
    p->engine->Play();
    p->paused = false;
    ui_wake();
}

static void player_pause(Player *p)
{
    p->autoplay = false;
    if (!p->engine) return;
    p->engine->Pause();
    p->paused = true;
    ui_wake();
}

static void player_toggle(Player *p)
{
    if (p->paused) player_play(p);
    else player_pause(p);
}

// Playback speed (1 = normal), for the video and every track engine.
static void player_set_rate(Player *p, f32 rate)
{
    p->rate = rate;
    if (p->engine) p->engine->SetPlaybackRate(rate);
    for (i32 i = 0; i < p->track_engine_count; i++)
        if (p->track_engine[i]) p->track_engine[i]->SetPlaybackRate(rate);
}

// The editor: each of `tracks` gets its own audio-only engine, so each has its own volume; the video's engine is
// muted. Called once the video's audio streams are known (p->loaded).
static void player_set_tracks(Player *p, const i32 *tracks, i32 n)
{
    player_clear_tracks(p);
    if (!p->engine) return;
    p->engine->SetMuted(n > 0);
    for (i32 i = 0; i < n && i < PLAYER_MAX_TRACK_ENGINES; i++) {
        p->track_engine[i] = player_engine(p, 1 + i, true);
        p->track_of_engine[i] = tracks[i];
        p->track_engine_count = i + 1;
    }
}

static void player_track_volume(Player *p, i32 track, f32 volume, bool muted)
{
    for (i32 i = 0; i < p->track_engine_count; i++) {
        if (p->track_of_engine[i] != track || !p->track_engine[i]) continue;
        p->track_engine[i]->SetVolume(volume < 1 ? volume : 1); // the engine can't go louder than recorded
        p->track_engine[i]->SetMuted(muted);
    }
}

static void player_on_media(const Event &e)
{
    u32 tag = (u32)(e.b >> 32);
    u32 event = (u32)(e.b & 0xffffffff);
    u8 id = tag & 15;
    i32 slot = (tag >> 4) & 15;
    Player *p = id == 0   ? &g_app.library.player
                : id == 1 ? &g_app.editor.player
                : id == 2 ? &g_app.timeline.player
                          : &g_app.library.hover;
    if (!p->open || ((tag >> 8) & 0xffffff) != (p->gen & 0xffffff)) return;

    if (slot > 0) { // a track engine
        i32 i = slot - 1;
        if (i >= p->track_engine_count || !p->track_engine[i]) return;
        IMFMediaEngine *te = p->track_engine[i];
        if (event == MF_MEDIA_ENGINE_EVENT_LOADEDMETADATA) {
            engine_pick_audio(te, *p, p->track_of_engine[i]);
            if (p->rate > 0) te->SetPlaybackRate(p->rate);
            te->SetCurrentTime(p->pos);
            if (!p->paused) te->Play();
        }
        return;
    }
    switch (event) {
    case MF_MEDIA_ENGINE_EVENT_LOADEDMETADATA: {
        DWORD w = 0, h = 0;
        p->engine->GetNativeVideoSize(&w, &h);
        p->duration = p->engine->GetDuration();
        if (!(p->duration > 0)) p->duration = 0;
        if (w && h) {
            D3D11_TEXTURE2D_DESC d = {};
            d.Width = w;
            d.Height = h;
            d.MipLevels = 1;
            d.ArraySize = 1;
            d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            d.SampleDesc.Count = 1;
            d.Usage = D3D11_USAGE_DEFAULT;
            d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            if (SUCCEEDED(g_gpu.device->CreateTexture2D(&d, 0, &p->tex)))
                g_gpu.device->CreateShaderResourceView(p->tex, 0, &p->srv);
            p->width = w;
            p->height = h;
        }
        player_find_audio(p);
        p->loaded = true;
        if (p->audio_count > 1) engine_pick_audio(p->engine, *p, p->sound);
        if (p->start_at > 0.01) {
            f64 t = p->start_at;
            p->start_at = 0;
            player_seek(p, t);
        }
        if (p->autoplay) player_play(p);
        log_line("player %d: %ux%u, %.2f s, %d audio streams", p->id, w, h, p->duration, p->audio_count);
        break;
    }
    case MF_MEDIA_ENGINE_EVENT_ERROR: {
        unsigned long long code = 0;
        u32 hr = 0;
        sscanf_s(e.text, "%llu %u", &code, &hr);
        snprintf(p->error, sizeof p->error, "This file couldn't be played (Media Foundation error %llu, 0x%08x).", code,
                 hr);
        log_line("player %d: error %llu (0x%08x)", p->id, code, hr);
        break;
    }
    case MF_MEDIA_ENGINE_EVENT_ENDED: p->ended = true; break;
    case MF_MEDIA_ENGINE_EVENT_SEEKED:
        p->seeking = false;
        if (p->has_seek) player_seek(p, p->seek_to);
        ui_wake(6);
        break;
    default: break;
    }
}

// Every frame the player is on screen: the newest frame into the texture, the clock, the editor's range.
static void player_update(Player *p)
{
    FRAME_PART("player_update");
    Ui &ui = g_app.ui;
    bool want = false;
    if (p->open && p->engine) {
        if (p->loaded) {
            if (!p->scrubbing && !p->has_seek && !p->engine->IsSeeking()) p->pos = p->engine->GetCurrentTime();
            // An audio stream longer than the video runs the clock past the duration; the end is the end.
            if (p->duration > 0 && p->pos > p->duration) p->pos = p->duration;
            p->ended = p->engine->IsEnded() != FALSE;
            // At the end the engine reports neither paused nor playing; it's stopped, so Play shows.
            p->paused = p->engine->IsPaused() != FALSE || p->ended;
            if (p->has_seek && !p->engine->IsSeeking()) player_seek(p, p->seek_to);
            if (p->stop_at > 0 && !p->paused && p->pos >= p->stop_at) {
                player_pause(p);
                player_seek(p, p->stop_at);
            }
            // The track engines follow the video. One that's drifted is moved back only once it has settled: a
            // seek in a long file takes a while, and judging the engine again straight after it only seeks it
            // again, so on a big file it can keep seeking and never get to play.
            f64 now = ImGui::GetTime();
            for (i32 i = 0; i < p->track_engine_count; i++) {
                IMFMediaEngine *te = p->track_engine[i];
                if (!te || te->GetReadyState() < MF_MEDIA_ENGINE_READY_HAVE_METADATA) continue;
                bool tp = te->IsPaused() != FALSE;
                if (p->paused && !tp) te->Pause();
                if (!p->paused && tp) {
                    te->Play();
                    p->track_synced[i] = now;
                }
                if (p->paused || p->engine->IsSeeking() || te->IsSeeking() || now - p->track_synced[i] < 1.0 ||
                    te->GetReadyState() < MF_MEDIA_ENGINE_READY_HAVE_ENOUGH_DATA)
                    continue;
                f64 d = te->GetCurrentTime() - p->pos;
                if (d > 0.15 || d < -0.15) {
                    DEV_LOG("player %d: track engine %d off by %.0f ms; moving it", p->id, i, d * 1000);
                    te->SetCurrentTime(p->pos);
                    p->track_synced[i] = now;
                }
            }
        }
        LONGLONG pts = 0;
        if (p->tex && p->engine->OnVideoStreamTick(&pts) == S_OK) {
            MFVideoNormalizedRect src = {0, 0, 1, 1};
            RECT dst = {0, 0, (LONG)p->width, (LONG)p->height};
            MFARGB border = {0, 0, 0, 255};
            if (SUCCEEDED(p->engine->TransferVideoFrame(p->tex, &src, &dst, &border))) p->frame_ready = true;
        }
        want = !p->error[0] && (!p->paused || p->seeking || p->has_seek || !p->frame_ready || !p->loaded);
    }
    if (want) {
        ui.animating = true;
        p->set_animating = true;
    } else if (p->set_animating) {
        ui.animating = false;
        p->set_animating = false;
        ui_wake(2);
    }
}

// The largest rectangle of the video's shape that fits in `size`, centred.
static void player_fit(const Player &p, ImVec2 pos, ImVec2 size, ImVec2 *out_min, ImVec2 *out_max)
{
    f32 aspect = p.width && p.height ? (f32)p.width / (f32)p.height : 16.f / 9.f;
    f32 w = size.x, h = size.x / aspect;
    if (h > size.y) { h = size.y; w = h * aspect; }
    *out_min = ImVec2(pos.x + (size.x - w) * 0.5f, pos.y + (size.y - h) * 0.5f);
    *out_max = ImVec2(out_min->x + w, out_min->y + h);
}

// Draws the video (or black, or the error) into the given rectangle; uv0/uv1 crop it (the editor's zoom).
static void player_draw(const Player &p, ImVec2 a, ImVec2 b, ImVec2 uv0 = ImVec2(0, 0), ImVec2 uv1 = ImVec2(1, 1))
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(a, b, IM_COL32(0, 0, 0, 255));
    if (p.srv && p.frame_ready) dl->AddImage((ImTextureID)(intptr_t)p.srv, a, b, uv0, uv1);
    const char *msg = p.error[0] ? p.error : !p.loaded && p.open ? "Loading..." : 0;
    if (msg) {
        ImVec2 ts = ImGui::CalcTextSize(msg);
        dl->AddText(ImVec2((a.x + b.x - ts.x) * 0.5f, (a.y + b.y - ts.y) * 0.5f), ImGui::GetColorU32(rgb(COL_DIM)), msg);
    }
}

// "1:05", "1:02:03"; with tenths, "1:05.3".
static void fmt_time(f64 s, char *out, i32 cap, bool tenths = false)
{
    if (!(s > 0)) s = 0;
    i64 t = tenths ? (i64)(s * 10) : (i64)s;
    i64 secs = tenths ? t / 10 : t;
    i32 h = (i32)(secs / 3600), m = (i32)(secs / 60 % 60), sec = (i32)(secs % 60);
    if (h) snprintf(out, cap, "%d:%02d:%02d", h, m, sec);
    else snprintf(out, cap, "%d:%02d", m, sec);
    if (tenths) {
        size_t n = strlen(out);
        snprintf(out + n, cap - n, ".%d", (i32)(t % 10));
    }
}

// A seek bar: a thin track, the played part in the accent, a knob at the playhead; track and knob grow under the
// mouse. Dragging scrubs (approximate seeks while held, exact when let go). Returns true while held.
static bool scrub_bar(const char *id, Player *p, f32 width, f32 height)
{
    ImVec2 a = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, ImVec2(width, height));
    bool held = ImGui::IsItemActive();
    bool hot = held || ImGui::IsItemHovered();
    f32 pad = kit_px(7); // the knob stays inside the item
    f32 x0 = a.x + pad, x1 = a.x + width - pad, len = x1 - x0 > 1 ? x1 - x0 : 1;
    f64 dur = p->duration;
    if (held && dur > 0) {
        f32 x = ImGui::GetIO().MousePos.x - x0;
        f64 t = (x < 0 ? 0 : x > len ? len : x) / len * dur;
        p->scrubbing = true;
        if (t < p->pos - 0.03 || t > p->pos + 0.03) player_seek(p, t);
    } else if (p->scrubbing) {
        p->scrubbing = false;
        player_seek(p, p->pos); // the exact frame where it was let go
    }
    ImDrawList *dl = ImGui::GetWindowDrawList();
    f32 cy = floorf(a.y + height * 0.5f), th = kit_px(hot ? 6.f : 4.f);
    f32 frac = dur > 0 ? (f32)(p->pos / dur) : 0;
    if (frac > 1) frac = 1;
    f32 kx = x0 + len * frac;
    dl->AddRectFilled(ImVec2(x0, cy - th / 2), ImVec2(x1, cy + th / 2), col32(COL_HOVER), th);
    dl->AddRectFilled(ImVec2(x0, cy - th / 2), ImVec2(kx, cy + th / 2), col32(COL_ACCENT), th);
    dl->AddCircleFilled(ImVec2(kx, cy), kit_px(hot ? 7.f : 5.f), col32(0xffffff));
    return held;
}
