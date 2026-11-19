// The first-run setup: a few questions on a first start, before the library. Each step changes the same settings
// Options does, as it goes; Finish or Skip marks it done. Options > General runs it again.

static const char SETUP_REPO_URL[] = "https://github.com/CalvBot/clipback-api"; // the art service's code
static const char SETUP_SGDB_URL[] = "https://www.steamgriddb.com/profile/preferences/api";
enum { SETUP_BEG_RESOURCE = 10, SETUP_STEPS = 5 };
// The begging picture (assets/setup-beg.jpg): 15 frames of 240 x 230, 5 across and 3 down, at 10 a second.
static const i32 BEG_COLS = 5, BEG_ROWS = 3, BEG_FRAMES = 15, BEG_FPS = 10;

static void open_url(const char *url)
{
    wchar_t w[512];
    utf8_to_wide(url, w, COUNT(w));
    ShellExecuteW(0, L"open", w, 0, 0, SW_SHOWNORMAL);
    log_line("opened %s", url);
}

// A picture built into the exe (an RCDATA resource) as a texture.
static ID3D11ShaderResourceView *resource_texture(i32 id, i32 *w, i32 *h)
{
    HRSRC res = FindResourceW(0, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10)); // RT_RCDATA
    HGLOBAL mem = res ? LoadResource(0, res) : 0;
    BYTE *data = mem ? (BYTE *)LockResource(mem) : 0;
    DWORD size = res ? SizeofResource(0, res) : 0;
    IWICImagingFactory *wic = 0;
    IWICStream *stream = 0;
    IWICBitmapDecoder *dec = 0;
    IWICBitmapFrameDecode *frame = 0;
    IWICFormatConverter *conv = 0;
    ID3D11ShaderResourceView *srv = 0;
    u32 *pixels = 0;
    UINT fw = 0, fh = 0;
    if (!data || !size) goto done;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, 0, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) goto done;
    if (FAILED(wic->CreateStream(&stream)) || FAILED(stream->InitializeFromMemory(data, size))) goto done;
    if (FAILED(wic->CreateDecoderFromStream(stream, 0, WICDecodeMetadataCacheOnDemand, &dec))) goto done;
    if (FAILED(dec->GetFrame(0, &frame)) || FAILED(wic->CreateFormatConverter(&conv))) goto done;
    if (FAILED(conv->Initialize(frame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, 0, 0, WICBitmapPaletteTypeCustom)))
        goto done;
    conv->GetSize(&fw, &fh);
    if (!fw || !fh || fw > 8192 || fh > 8192) goto done;
    pixels = (u32 *)HeapAlloc(GetProcessHeap(), 0, (size_t)fw * fh * 4);
    if (pixels && SUCCEEDED(conv->CopyPixels(0, fw * 4, fw * fh * 4, (BYTE *)pixels))) {
        srv = gpu_texture(pixels, (i32)fw, (i32)fh);
        *w = (i32)fw;
        *h = (i32)fh;
    }
done:
    if (pixels) HeapFree(GetProcessHeap(), 0, pixels);
    if (conv) conv->Release();
    if (frame) frame->Release();
    if (dec) dec->Release();
    if (stream) stream->Release();
    if (wic) wic->Release();
    if (!srv) log_line("setup: picture %d couldn't be loaded", id);
    return srv;
}

// One answer to a question: a panel with a round mark, a bold title and a dim line under it. The chosen one is
// lighter with the mark filled.
static bool setup_choice(const char *id, const char *title, const char *sub, bool chosen, f32 w, f32 h)
{
    ImVec2 a = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::InvisibleButton(id, ImVec2(w, h));
    if (clicked) dev_click("choice", title);
    bool hot = ImGui::IsItemHovered();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(a, ImVec2(a.x + w, a.y + h), col32(chosen ? COL_PANEL2 : hot ? 0x1d1f23 : COL_PANEL), dp(10));
    f32 r = dp(9);
    ImVec2 c(a.x + dp(20) + r, a.y + h / 2);
    dl->AddCircleFilled(c, r, col32(chosen ? COL_ACCENT : 0x3a3d44));
    if (chosen) dl->AddCircleFilled(c, dp(4), col32(0xffffff));
    f32 tx = c.x + r + dp(16), max_x = a.x + w - dp(16);
    f32 th = g_font_bold->FontSize, sh = sub ? g_font_small->FontSize : 0, gap = sub ? dp(4) : 0;
    f32 ty = floorf(a.y + (h - th - gap - sh) / 2);
    opt_text_at(dl, g_font_bold, ImVec2(tx, ty), COL_TEXT, title, max_x);
    if (sub) opt_text_at(dl, g_font_small, ImVec2(tx, ty + th + gap), COL_DIM, sub, max_x);
    return clicked;
}

static void setup_finish(const char *how)
{
    Config &c = g_app.config;
    c.setup_done = true;
    g_app.ui.setup_open = false;
    g_app.ui.setup_step = 0;
    options_apply(APPLY_RECORDER);
    if (!g_gamedb.thread) gamedb_start(c.game_lookup); // a first start waits for the answer before looking games up
    log_line("setup %s: look-up %s, art service %s, own key %s, help name games %s, tooltips %s", how,
             c.game_lookup ? "on" : "off", c.art_service.s[0] ? c.art_service.s : "none", c.sgdb_key[0] ? "set" : "none",
             c.share_names ? "on" : "off", c.tooltips ? "on" : "off");
}

static void setup_recording(f32 w)
{
    opt_section("VIDEO");
    opt_card_begin();
    opt_rows_video(fminf(dp(300), w * 0.45f));
    opt_card_end();
    opt_section("BUFFER");
    opt_card_begin();
    opt_rows_buffer();
    opt_card_end(0);
}

static void setup_games(f32 w)
{
    Config &c = g_app.config;
    i32 source = !c.game_lookup ? 2 : c.art_service.s[0] ? 0 : 1;
    static const char *titles[] = {"clipback's art service", "Public lists only", "Nothing online"};
    static const char *subs[] = {
        "Discord's and Steam's lists, plus art they don't have from SteamGridDB and IGDB. Sends game names and store ids only",
        "Discord's and Steam's own lists. Some games get no art",
        "Only the names and pictures already on this PC",
    };
    f32 h = dp(66), gap = dp(8);
    for (i32 i = 0; i < 3; i++) {
        ImGui::PushID(i);
        if (setup_choice("##source", titles[i], subs[i], source == i, w, h) && source != i) {
            source = i;
            c.game_lookup = i != 2;
            str_set(c.art_service.s, i == 0 ? ART_SERVICE_HOST : "");
            log_line("setup: game info from %s", titles[i]);
            options_apply(0);
        }
        ImGui::PopID();
        ImGui::Dummy(ImVec2(0, gap - ImGui::GetStyle().ItemSpacing.y));
    }
    ImGui::Dummy(ImVec2(0, dp(10)));

    opt_section("YOUR OWN STEAMGRIDDB KEY (OPTIONAL)");
    ImGui::BeginDisabled(!c.game_lookup);
    opt_card_begin();
    f32 field_w = fminf(dp(300), w * 0.45f);
    opt_row_begin("API key", "For art the art service doesn't hand out yet. Stays in settings.txt", field_w);
    char key[sizeof c.sgdb_key];
    if (opt_text("##sgdbkey", c.sgdb_key, field_w, ImGuiInputTextFlags_Password, "Paste it here", key, sizeof key)) {
        str_set(c.sgdb_key, trim(key));
        log_line("setup: own SteamGridDB key %s", c.sgdb_key[0] ? "set" : "cleared");
        options_apply(0);
    }
    opt_row_end();
    f32 bw = text_w(g_font, "Open SteamGridDB") + dp(32);
    opt_row_begin("Get a key", "Sign in at steamgriddb.com, open Preferences > API, then Generate API Key", bw);
    if (kit_button("Open SteamGridDB##sgdb", KIT_SECONDARY, ImVec2(bw, 0))) open_url(SETUP_SGDB_URL);
    opt_row_end();
    opt_card_end();
    ImGui::EndDisabled();

    opt_section("IS IT SAFE?");
    opt_card_begin();
    bw = text_w(g_font, "View the code") + dp(32);
    opt_row_begin("The art service is open source", "Read exactly what it does with a request before you turn it on", bw);
    if (kit_button("View the code##repo", KIT_SECONDARY, ImVec2(bw, 0))) open_url(SETUP_REPO_URL);
    opt_row_end();
    opt_card_end(0);
}

static void setup_sharing(f32 w, f32 h)
{
    Config &c = g_app.config;
    static ID3D11ShaderResourceView *beg;
    static i32 beg_w, beg_h;
    static bool tried;
    if (!tried) {
        beg = resource_texture(SETUP_BEG_RESOURCE, &beg_w, &beg_h);
        tried = true;
    }
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 a = ImGui::GetCursorScreenPos();
    f32 fw = beg ? (f32)beg_w / BEG_COLS : 240, fh = beg ? (f32)beg_h / BEG_ROWS : 230;
    f32 pic_h = fminf(h, dp(300)), pic_w = pic_h * fw / fh;
    if (beg) {
        i32 f = (i32)(ImGui::GetTime() * BEG_FPS) % BEG_FRAMES;
        ImVec2 uv0((f32)(f % BEG_COLS) / BEG_COLS, (f32)(f / BEG_COLS) / BEG_ROWS);
        ImVec2 uv1(uv0.x + 1.0f / BEG_COLS, uv0.y + 1.0f / BEG_ROWS);
        dl->AddImageRounded((ImTextureID)beg, a, ImVec2(a.x + pic_w, a.y + pic_h), uv0, uv1, IM_COL32_WHITE, dp(10));
        ui_wake(2); // keep drawing: it moves
    } else {
        dl->AddRectFilled(a, ImVec2(a.x + pic_w, a.y + pic_h), col32(COL_PANEL), dp(10));
    }

    f32 x = a.x + pic_w + dp(28), cw = w - pic_w - dp(28);
    bool service = c.game_lookup && c.art_service.s[0];
    ImGui::SetCursorScreenPos(ImVec2(x, a.y));
    ImGui::PushTextWrapPos(x + cw - ImGui::GetWindowPos().x);
    ImGui::PushStyleColor(ImGuiCol_Text, rgb(COL_DIM));
    ImGui::TextUnformatted("When you name a game clipback couldn't place, it can tell the art service the game's exe "
                           "and the name you gave it, so the next player gets it named. A random id goes with it, so "
                           "each player counts once.\n\n"
                           "Nothing else: no clips, no pictures of your screen, no account.");
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(0, dp(12)));

    ImGui::BeginDisabled(!service);
    static const char *titles[] = {"Sure, share", "No thanks"};
    static const char *subs[] = {"Names you give games help everyone", "Keep them to myself"};
    bool on = c.share_names && service;
    for (i32 i = 0; i < 2; i++) {
        ImGui::SetCursorScreenPos(ImVec2(x, ImGui::GetCursorScreenPos().y));
        ImGui::PushID(i);
        if (setup_choice("##share", titles[i], subs[i], on == (i == 0), cw, dp(62)) && on != (i == 0)) {
            c.share_names = i == 0;
            log_line("setup: help name games %s", c.share_names ? "on" : "off");
            options_apply(0);
        }
        ImGui::PopID();
    }
    ImGui::EndDisabled();
    if (!service) {
        ImGui::SetCursorScreenPos(ImVec2(x, ImGui::GetCursorScreenPos().y + dp(4)));
        kit_dim("Needs clipback's art service (the step before)");
    }
    ImGui::SetCursorScreenPos(ImVec2(a.x, fmaxf(a.y + pic_h, ImGui::GetCursorScreenPos().y)));
}

static void setup_tooltips(f32 w)
{
    Config &c = g_app.config;
    static const char *titles[] = {"Show tooltips", "I know what I'm doing"};
    static const char *subs[] = {"Point at a button to see what it does and its key", "Get that off my screen"};
    for (i32 i = 0; i < 2; i++) {
        ImGui::PushID(i);
        if (setup_choice("##tips", titles[i], subs[i], c.tooltips == (i == 0), w, dp(66)) && c.tooltips != (i == 0)) {
            c.tooltips = i == 0;
            log_line("setup: tooltips %s", c.tooltips ? "on" : "off");
            options_apply(0);
        }
        ImGui::PopID();
        ImGui::Dummy(ImVec2(0, dp(8) - ImGui::GetStyle().ItemSpacing.y));
    }
    ImGui::Dummy(ImVec2(0, dp(6)));
    kit_dim("Either way, F1 lists every key. Change this any time in Options > General.");
}

static void setup_window()
{
    Ui &ui = g_app.ui;
    if (ui.setup_step < 0 || ui.setup_step >= SETUP_STEPS) ui.setup_step = 0;
    static i32 last_frame;
    i32 frame = ImGui::GetFrameCount();
    if (!last_frame || frame - last_frame > 1) options_list_monitors(); // opened (again): monitors may have changed
    last_frame = frame;
    static const char *names[SETUP_STEPS] = {"Recording", "Games", "Sharing", "Tooltips", "Hotkeys"};
    static const char *titles[SETUP_STEPS] = {"What to record", "Game names and art", "Share data about your games?",
                                              "Tooltips", "Hotkeys"};
    static const char *subs[SETUP_STEPS] = {
        "Which screen, how sharp, and how much the buffer keeps. All of it is in Options later.",
        "Where clipback gets names, icons and covers for the games you play.",
        "Pretty please. It's small, and it makes clipback better for everyone.",
        "The little labels that pop up when you point at a button.",
        "Press one to save what just happened. Change them any time in Options > Hotkeys.",
    };

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(kit_px(28), kit_px(24)));
    ImGui::BeginChild("##setup", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    ImVec2 avail = ImGui::GetContentRegionAvail(), top = ImGui::GetCursorScreenPos();
    f32 w = fminf(avail.x, dp(860)), x0 = floorf(top.x + (avail.x - w) / 2);
    ImDrawList *dl = ImGui::GetWindowDrawList();

    // The steps, as the Options tabs: each as wide as its bold name, the current one underlined.
    f32 tab_h = dp(36), gap = dp(26), x = x0;
    dl->AddLine(ImVec2(x0, top.y + tab_h - 0.5f), ImVec2(x0 + w, top.y + tab_h - 0.5f), col32(COL_LINE));
    for (i32 i = 0; i < SETUP_STEPS; i++) {
        char label[32];
        snprintf(label, sizeof label, "%d  %s", i + 1, names[i]);
        f32 lw = text_w(g_font_bold, label);
        bool active = i == ui.setup_step;
        ImGui::SetCursorScreenPos(ImVec2(x - gap / 2, top.y));
        ImGui::PushID(i);
        ImGui::BeginDisabled(g_app.options.capture_slot != 0 && !active);
        if (ImGui::InvisibleButton("##step", ImVec2(lw + gap, tab_h))) {
            ui.setup_step = i;
            dev_click("setup step", names[i]);
        }
        bool hot = ImGui::IsItemHovered();
        ImGui::EndDisabled();
        ImGui::PopID();
        ImFont *font = active ? g_font_bold : g_font;
        u32 ink = active ? COL_TEXT : hot ? 0xd4d7dc : COL_DIM;
        opt_text_at(dl, font, ImVec2(x, floorf(top.y + (tab_h - dp(4) - font->FontSize) / 2)), ink, label, x + lw + 1);
        if (active) dl->AddRectFilled(ImVec2(x, top.y + tab_h - dp(2)), ImVec2(x + lw, top.y + tab_h), col32(COL_ACCENT), dp(1));
        x += lw + gap;
    }

    // The title and what the step is about
    f32 y = top.y + tab_h + dp(22);
    opt_text_at(dl, g_font_title, ImVec2(x0, y), COL_TEXT, titles[ui.setup_step], x0 + w);
    y += g_font_title->FontSize + dp(6);
    opt_text_at(dl, g_font, ImVec2(x0, y), COL_DIM, subs[ui.setup_step], x0 + w);
    y += g_font->FontSize + dp(22);

    // The step itself, between the title and the buttons; it scrolls if the window is short.
    f32 foot_h = ImGui::GetFrameHeight() + dp(20);
    f32 body_h = fmaxf(dp(100), top.y + avail.y - foot_h - y);
    ImGui::SetCursorScreenPos(ImVec2(x0, y));
    bool hotkeys = ui.setup_step == SETUP_STEPS - 1;
    ImGui::BeginChild("##setupbody", ImVec2(w, body_h), ImGuiChildFlags_None,
                      hotkeys ? ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse : 0);
    f32 bw = ImGui::GetContentRegionAvail().x;
    switch (ui.setup_step) {
    case 0: setup_recording(bw); break;
    case 1: setup_games(bw); break;
    case 2: setup_sharing(bw, body_h); break;
    case 3: setup_tooltips(bw); break;
    case 4: options_hotkeys(); break;
    }
    ImGui::EndChild();
    if (g_app.options.capture_slot) ui_wake(); // keep drawing while waiting for keys

    // The buttons, always in the same places: Skip on the left, Back and Next (Finish on the last step) on the right.
    f32 by = top.y + avail.y - ImGui::GetFrameHeight();
    f32 nw = dp(120), back_w = dp(100);
    bool busy = g_app.options.capture_slot != 0;
    ImGui::SetCursorScreenPos(ImVec2(x0, by));
    if (kit_button("Skip setup", KIT_GHOST, ImVec2(dp(120), 0), !busy)) setup_finish("skipped");
    ImGui::SetCursorScreenPos(ImVec2(x0 + w - nw - dp(8) - back_w, by));
    if (kit_button("Back", KIT_SECONDARY, ImVec2(back_w, 0), ui.setup_step > 0 && !busy)) ui.setup_step--;
    ImGui::SetCursorScreenPos(ImVec2(x0 + w - nw, by));
    bool last = ui.setup_step == SETUP_STEPS - 1;
    if (kit_button(last ? "Finish##next" : "Next##next", KIT_PRIMARY, ImVec2(nw, 0), !busy)) {
        if (last) setup_finish("finished");
        else ui.setup_step++;
    }
    ImGui::EndChild();
}
