// The first-run setup: one screen on a first start, over the library, while recording already runs on the
// defaults. It shows the clip key and asks about tooltips and helping name games; the rest is in Options. Each
// answer changes the setting at once; Done marks it finished. Options > General runs it again.

enum { SETUP_BEG_RESOURCE = 10 };
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
    if (chosen) dl->AddCircleFilled(c, dp(4), col32(COL_ACCENT_INK));
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
    options_apply(0);
    log_line("setup %s: look-up %s, art service %s, own key %s, help name games %s, tooltips %s", how,
             c.game_lookup ? "on" : "off", c.art_service.s[0] ? c.art_service.s : "none", c.sgdb_key[0] ? "set" : "none",
             c.share_names ? "on" : "off", c.tooltips ? "on" : "off");
}

// The clip key: the first hotkey that saves, and how much it saves ("save 30s" -> 30). False with none.
static bool setup_clip_key(const Config &c, char *keys, i32 cap, i32 *seconds)
{
    for (i32 i = 0; i < c.hotkey_count; i++) {
        if (strncmp(c.hotkeys[i].action.s, "save", 4)) continue;
        snprintf(keys, cap, "%s", c.hotkeys[i].keys.s);
        *seconds = atoi(c.hotkeys[i].action.s + 4);
        return true;
    }
    return false;
}

// Two answers side by side, each half of w; returns the one clicked (0 or 1), else -1.
static i32 setup_pair(const char *id, const char *const titles[2], const char *const subs[2], i32 chosen, f32 w, f32 h)
{
    f32 gap = dp(10), half = floorf((w - gap) / 2);
    ImVec2 a = ImGui::GetCursorScreenPos();
    i32 hit = -1;
    ImGui::PushID(id);
    for (i32 i = 0; i < 2; i++) {
        ImGui::SetCursorScreenPos(ImVec2(a.x + i * (half + gap), a.y));
        ImGui::PushID(i);
        if (setup_choice("##c", titles[i], subs[i], chosen == i, half, h) && chosen != i) hit = i;
        ImGui::PopID();
    }
    ImGui::PopID();
    ImGui::SetCursorScreenPos(ImVec2(a.x, a.y + h));
    return hit;
}

static void setup_window()
{
    Config &c = g_app.config;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(kit_px(28), kit_px(24)));
    ImGui::BeginChild("##setup", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding, 0);
    ImGui::PopStyleVar();
    ImVec2 avail = ImGui::GetContentRegionAvail(), top = ImGui::GetCursorScreenPos();
    f32 w = fminf(avail.x, dp(640)), x0 = floorf(top.x + (avail.x - w) / 2);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    f32 y = top.y + dp(12);

    opt_text_at(dl, g_font_title, ImVec2(x0, y), COL_TEXT, "Welcome to " APP_NAME, x0 + w);
    y += g_font_title->FontSize + dp(6);
    opt_text_at(dl, g_font, ImVec2(x0, y), COL_DIM,
                "It keeps the last while of your games, so you can save a clip after it happens.", x0 + w);
    y += g_font->FontSize + dp(22);

    // The clip key, as a key cap, and what it does
    {
        char keys[64] = "", line[96];
        i32 secs = 0;
        bool any = setup_clip_key(c, keys, sizeof keys, &secs);
        if (!any) str_set(line, "No key saves a clip yet. Set one in Options > Hotkeys.");
        else if (secs > 0) snprintf(line, sizeof line, "Press it in a game to save the last %d seconds as a clip.", secs);
        else str_set(line, "Press it in a game to save what just happened as a clip.");
        f32 ph = dp(76);
        dl->AddRectFilled(ImVec2(x0, y), ImVec2(x0 + w, y + ph), col32(COL_PANEL), dp(10));
        f32 kx = x0 + dp(20), kh = dp(40), kw = any ? text_w(g_font_bold, keys) + dp(32) : 0;
        if (any) {
            f32 ky = floorf(y + (ph - kh) / 2);
            dl->AddRectFilled(ImVec2(kx, ky), ImVec2(kx + kw, ky + kh), col32(COL_PANEL2), dp(8));
            dl->AddRect(ImVec2(kx, ky), ImVec2(kx + kw, ky + kh), col32(COL_ACCENT), dp(8), 0, dp(1.5f));
            opt_text_at(dl, g_font_bold, ImVec2(kx + dp(16), floorf(ky + (kh - g_font_bold->FontSize) / 2)), COL_TEXT,
                        keys, kx + kw);
            kx += kw + dp(18);
        }
        opt_text_at(dl, g_font, ImVec2(kx, floorf(y + (ph - g_font->FontSize) / 2)), COL_TEXT, line, x0 + w - dp(16));
        y += ph + dp(26);
    }

    // Tooltips
    ImGui::SetCursorScreenPos(ImVec2(x0, y));
    opt_section("TOOLTIPS");
    {
        static const char *const titles[] = {"Show tooltips", "I know what I'm doing"};
        static const char *const subs[] = {"Point at a button to see what it does", "Get them off my screen"};
        ImGui::SetCursorScreenPos(ImVec2(x0, ImGui::GetCursorScreenPos().y));
        i32 hit = setup_pair("tips", titles, subs, c.tooltips ? 0 : 1, w, dp(62));
        if (hit >= 0) {
            c.tooltips = hit == 0;
            log_line("setup: tooltips %s", c.tooltips ? "on" : "off");
            options_apply(0);
        }
    }
    ImGui::Dummy(ImVec2(0, dp(6)));
    ImGui::SetCursorScreenPos(ImVec2(x0, ImGui::GetCursorScreenPos().y));
    kit_dim("Either way, F1 lists every key.");
    ImGui::Dummy(ImVec2(0, dp(14)));

    // Helping name games, with the begging picture beside the question
    ImGui::SetCursorScreenPos(ImVec2(x0, ImGui::GetCursorScreenPos().y));
    opt_section("HELP NAME GAMES?");
    bool service = c.game_lookup && c.art_service.s[0];
    {
        static ID3D11ShaderResourceView *beg;
        static i32 beg_w, beg_h;
        static bool tried;
        if (!tried) {
            beg = resource_texture(SETUP_BEG_RESOURCE, &beg_w, &beg_h);
            tried = true;
        }
        ImVec2 a(x0, ImGui::GetCursorScreenPos().y);
        f32 fw = beg ? (f32)beg_w / BEG_COLS : 240, fh = beg ? (f32)beg_h / BEG_ROWS : 230;
        f32 pic_h = dp(124), pic_w = floorf(pic_h * fw / fh);
        if (beg) {
            i32 f = (i32)(ImGui::GetTime() * BEG_FPS) % BEG_FRAMES;
            ImVec2 uv0((f32)(f % BEG_COLS) / BEG_COLS, (f32)(f / BEG_COLS) / BEG_ROWS);
            ImVec2 uv1(uv0.x + 1.0f / BEG_COLS, uv0.y + 1.0f / BEG_ROWS);
            dl->AddImageRounded((ImTextureID)beg, a, ImVec2(a.x + pic_w, a.y + pic_h), uv0, uv1, IM_COL32_WHITE, dp(10));
            ui_wake(2); // keep drawing: it moves
        } else {
            dl->AddRectFilled(a, ImVec2(a.x + pic_w, a.y + pic_h), col32(COL_PANEL), dp(10));
        }
        f32 x = a.x + pic_w + dp(20), cw = x0 + w - x;
        ImGui::SetCursorScreenPos(ImVec2(x, a.y));
        ImGui::PushTextWrapPos(x + cw - ImGui::GetWindowPos().x);
        ImGui::PushStyleColor(ImGuiCol_Text, rgb(COL_DIM));
        ImGui::TextUnformatted("When you name a game " APP_NAME " couldn't place, its exe and your name for it go to "
                               APP_NAME "'s service, so the next player gets it named. Nothing else is sent.");
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
        ImGui::SetCursorScreenPos(ImVec2(x, a.y + pic_h - dp(62)));
        static const char *const titles[] = {"Sure, help", "No thanks"};
        static const char *const subs[] = {"Names you give help everyone", "Keep them to myself"};
        ImGui::BeginDisabled(!service);
        i32 hit = setup_pair("share", titles, subs, c.share_names && service ? 0 : 1, cw, dp(62));
        ImGui::EndDisabled();
        if (hit >= 0) {
            c.share_names = hit == 0;
            log_line("setup: help name games %s", c.share_names ? "on" : "off");
            options_apply(0);
        }
        ImGui::SetCursorScreenPos(ImVec2(x0, a.y + pic_h + dp(14)));
    }

    // Usage stats, off unless the answer is yes
    ImGui::SetCursorScreenPos(ImVec2(x0, ImGui::GetCursorScreenPos().y));
    opt_section("SEND USAGE STATS?");
    {
        ImGui::SetCursorScreenPos(ImVec2(x0, ImGui::GetCursorScreenPos().y));
        static const char *const titles[] = {"Sure, send", "No thanks"};
        static const char *const subs[] = {"Once a day: counts of clips, saves and links", "Send nothing"};
        ImGui::BeginDisabled(!c.game_lookup);
        i32 hit = setup_pair("stats", titles, subs, c.send_stats && c.game_lookup ? 0 : 1, w, dp(62));
        ImGui::EndDisabled();
        if (hit >= 0) {
            c.send_stats = hit == 0;
            log_line("setup: usage stats %s", c.send_stats ? "on" : "off");
            options_apply(0);
        }
        ImGui::Dummy(ImVec2(0, dp(6)));
        ImGui::SetCursorScreenPos(ImVec2(x0, ImGui::GetCursorScreenPos().y));
        kit_dim("With the version, Windows build and encoder. No names, no files.");
        ImGui::Dummy(ImVec2(0, dp(14)));
        ImGui::SetCursorScreenPos(ImVec2(x0, ImGui::GetCursorScreenPos().y));
    }

    // Where game art comes from, and the way to Options
    {
        const char *line = service ? "Game names and art come from " APP_NAME "'s service."
                                   : APP_NAME "'s service is off, so helping name games is too.";
        static const char link[] = "Change in Options";
        ImVec2 p = ImGui::GetCursorScreenPos();
        f32 lx = p.x + text_w(g_font, line) + dp(8), link_w = text_w(g_font, link);
        opt_text_at(dl, g_font, p, COL_DIM, line, x0 + w);
        ImGui::SetCursorScreenPos(ImVec2(lx, p.y));
        if (ImGui::InvisibleButton("##toopts", ImVec2(link_w, g_font->FontSize))) {
            dev_click("setup", link);
            setup_finish("left for Options");
            g_app.ui.screen = SCREEN_OPTIONS;
            g_app.options.tab = OPT_GENERAL;
        }
        bool hot = ImGui::IsItemHovered();
        if (hot) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        opt_text_at(dl, g_font, ImVec2(lx, p.y), hot ? 0xffffff : COL_ACCENT, link, lx + link_w + 1);
        ImGui::SetCursorScreenPos(ImVec2(x0, p.y + g_font->FontSize + dp(26)));
    }

    f32 bw = dp(120);
    ImGui::SetCursorScreenPos(ImVec2(x0 + w - bw, ImGui::GetCursorScreenPos().y));
    if (kit_button("Done##setupdone", KIT_PRIMARY, ImVec2(bw, 0))) setup_finish("finished");
    ImGui::Dummy(ImVec2(0, dp(8)));
    ImGui::EndChild();
    ffget_bar();
}
