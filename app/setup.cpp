// The first-run setup: one screen on a first start, over the library, while recording already runs on the
// defaults. It shows the clip key and asks about tooltips, helping name games, usage stats and sharing. Each
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

// One answer to a question: a card with a round mark, a bold title and a dim line under it. The picked one is
// raised, ringed in white, with the accent mark; the other is flat and dim. Both keep their size and place.
static bool setup_choice(const char *id, const char *title, const char *sub, bool chosen, f32 w, f32 h)
{
    ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + w, a.y + h);
    bool clicked = ImGui::InvisibleButton(id, ImVec2(w, h));
    if (clicked) dev_click("choice", title);
    bool hot = ImGui::IsItemHovered() && !chosen;
    if (hot) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    f32 rr = dp(12), r = dp(10);
    dl->AddRectFilled(a, b, col32(chosen ? COL_PANEL2 : hot ? COL_HOVER : COL_PANEL), rr);
    if (chosen) dl->AddRect(a, b, col32(COL_WHITE), rr, 0, dp(1.5f));
    ImVec2 c(a.x + dp(20) + r, a.y + h / 2);
    if (chosen) {
        dl->AddCircleFilled(c, r, col32(COL_ACCENT), 32);
        dl->AddCircleFilled(c, dp(4), col32(COL_ACCENT_INK), 24);
    } else {
        dl->AddCircle(c, r - dp(0.75f), col32(hot ? COL_DIM : COL_FAINT), 32, dp(1.5f));
    }
    f32 tx = c.x + r + dp(16), max_x = b.x - dp(16);
    f32 th = g_font_bold->FontSize, sh = sub ? g_font_small->FontSize : 0, gap = sub ? dp(5) : 0;
    f32 ty = floorf(a.y + (h - th - gap - sh) / 2);
    opt_text_at(dl, g_font_bold, ImVec2(tx, ty), chosen || hot ? COL_TEXT : COL_DIM, title, max_x);
    if (sub) opt_text_at(dl, g_font_small, ImVec2(tx, ty + th + gap), chosen ? COL_DIM : COL_FAINT, sub, max_x);
    return clicked;
}

// A section's heading: its icon in a round chip, then its name in bold. Leaves the cursor under it at x0.
static void setup_heading(ImDrawList *dl, f32 x0, f32 x1, const char *icon, const char *title)
{
    ImVec2 p = ImGui::GetCursorScreenPos();
    f32 d = dp(30), is = g_font_icon->FontSize;
    ImVec2 c(x0 + d / 2, p.y + d / 2), isz = g_font_icon->CalcTextSizeA(is, FLT_MAX, 0, icon);
    dl->AddCircleFilled(c, d / 2, col32(COL_PANEL2), 32);
    dl->AddText(g_font_icon, is, ImVec2(floorf(c.x - isz.x / 2), floorf(c.y - isz.y / 2)), col32(COL_TEXT), icon);
    opt_text_at(dl, g_font_big, ImVec2(x0 + d + dp(12), floorf(c.y - g_font_big->FontSize / 2)), COL_TEXT, title, x1);
    ImGui::SetCursorScreenPos(ImVec2(x0, p.y + d + dp(14)));
}

// Text with its letters `track` px further apart than the font sets them (negative: tighter). Returns the end.
static f32 setup_text_tracked(ImDrawList *dl, ImFont *f, ImVec2 p, u32 col, const char *s, f32 track)
{
    ImU32 c = col32(col);
    for (; *s; s++) {
        dl->AddText(f, f->FontSize, ImVec2(floorf(p.x), p.y), c, s, s + 1);
        p.x += f->GetCharAdvance((ImWchar)(u8)*s) + track;
    }
    return p.x;
}

// A key cap h tall at p: a raised top over a darker lip along its bottom edge, the key's name in the title font.
// Returns its width.
static f32 setup_keycap(ImDrawList *dl, ImVec2 p, f32 h, const char *name)
{
    f32 tw = text_w(g_font_title, name), w = fmaxf(h, tw + dp(36)), lip = dp(4), r = dp(10);
    ImVec2 q(p.x + w, p.y + h);
    dl->AddRectFilled(p, q, col32(0x050505), r);
    dl->AddRectFilled(p, ImVec2(q.x, q.y - lip), col32(COL_OFF), r);
    dl->AddRect(p, ImVec2(q.x, q.y - lip), col32(0x4a4a4a), r, 0, dp(1));
    opt_text_at(dl, g_font_title, ImVec2(floorf(p.x + (w - tw) / 2), floorf(p.y + (h - lip - g_font_title->FontSize) / 2)),
                COL_TEXT, name, q.x);
    return w;
}

static void setup_finish(const char *how)
{
    Config &c = g_app.config;
    c.setup_done = true;
    g_app.ui.setup_open = false;
    options_apply(0);
    log_line("setup %s: look-up %s, art service %s, own key %s, help name games %s, tooltips %s, cloudflare %s", how,
             c.game_lookup ? "on" : "off", c.art_service.s[0] ? c.art_service.s : "none", c.sgdb_key[0] ? "set" : "none",
             c.share_names ? "on" : "off", c.tooltips ? "on" : "off", r2_set_up() ? "set up" : "none");
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
    f32 gap = dp(12), half = floorf((w - gap) / 2);
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

// A dim small line under a section. Leaves the cursor under it at x0.
static void setup_note(ImDrawList *dl, f32 x0, f32 x1, const char *text)
{
    ImVec2 p = ImGui::GetCursorScreenPos();
    opt_text_at(dl, g_font_small, ImVec2(x0, p.y + dp(10)), COL_DIM, text, x1);
    ImGui::SetCursorScreenPos(ImVec2(x0, p.y + dp(10) + g_font_small->FontSize));
}

static void setup_window()
{
    Config &c = g_app.config;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(kit_px(28), kit_px(24)));
    ImGui::BeginChild("##setup", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding, 0);
    ImGui::PopStyleVar();
    ImVec2 avail = ImGui::GetContentRegionAvail(), top = ImGui::GetCursorScreenPos();
    f32 w = fminf(avail.x, dp(640)), x0 = floorf(top.x + (avail.x - w) / 2), x1 = x0 + w;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const f32 SECTION_GAP = dp(36), PAIR_H = dp(68);
    f32 y = top.y + dp(28);

    // The greeting, large and tight
    setup_text_tracked(dl, g_font_hero, ImVec2(x0, y), COL_TEXT, "Welcome to " APP_NAME, -dp(0.6f));
    y += g_font_hero->FontSize + dp(8);
    opt_text_at(dl, g_font, ImVec2(x0, y), COL_DIM, "It keeps the last while of your games, so you can save a clip after it happens.",
                x1);
    y += g_font->FontSize + dp(24);

    // The clip key as key caps, and what it does
    {
        char keys[64] = "";
        i32 secs = 0;
        bool any = setup_clip_key(c, keys, sizeof keys, &secs);
        f32 ph = dp(108), kh = dp(60);
        dl->AddRectFilled(ImVec2(x0, y), ImVec2(x1, y + ph), col32(COL_PANEL), dp(12));
        f32 kx = x0 + dp(24), ky = floorf(y + (ph - kh) / 2);
        if (any) {
            // "Alt+F7" as Alt, +, F7; a + with nothing before it is the + key itself
            char part[64];
            i32 n = 0;
            bool first = true;
            for (const char *k = keys;; k++) {
                bool end = !*k;
                if (!end && (*k != '+' || n == 0) && n < (i32)sizeof part - 1) {
                    part[n++] = *k;
                    continue;
                }
                part[n] = 0;
                if (n) {
                    if (!first) {
                        opt_text_at(dl, g_font_big, ImVec2(kx, floorf(ky + (kh - dp(4) - g_font_big->FontSize) / 2)), COL_DIM,
                                    "+", kx + dp(30));
                        kx += text_w(g_font_big, "+") + dp(10);
                    }
                    kx += setup_keycap(dl, ImVec2(kx, ky), kh, part) + dp(10);
                    first = false;
                }
                n = 0;
                if (end) break;
            }
            kx += dp(14);
        }
        char head[96];
        const char *sub = any ? "Press it in a game" : "Set one in Options > Hotkeys";
        if (!any) str_set(head, "No key saves a clip yet");
        else if (secs > 0) snprintf(head, sizeof head, "Saves the last %d seconds", secs);
        else str_set(head, "Saves what just happened");
        f32 bh = g_font_big->FontSize, sh = g_font->FontSize, ty = floorf(y + (ph - bh - dp(6) - sh) / 2);
        opt_text_at(dl, g_font_big, ImVec2(kx, ty), COL_TEXT, head, x1 - dp(16));
        opt_text_at(dl, g_font, ImVec2(kx, ty + bh + dp(6)), COL_DIM, sub, x1 - dp(16));
        y += ph + SECTION_GAP;
    }

    // Tooltips
    ImGui::SetCursorScreenPos(ImVec2(x0, y));
    setup_heading(dl, x0, x1, ICON_COMMENT, "Tooltips");
    {
        static const char *const titles[] = {"Show tooltips", "I know what I'm doing"};
        static const char *const subs[] = {"Point at a button to see what it does", "Get them off my screen"};
        i32 hit = setup_pair("tips", titles, subs, c.tooltips ? 0 : 1, w, PAIR_H);
        if (hit >= 0) {
            c.tooltips = hit == 0;
            log_line("setup: tooltips %s", c.tooltips ? "on" : "off");
            options_apply(0);
        }
    }
    setup_note(dl, x0, x1, "Either way, F1 lists every key.");
    ImGui::SetCursorScreenPos(ImVec2(x0, ImGui::GetCursorScreenPos().y + SECTION_GAP));

    // Helping name games, with the begging picture beside the question
    setup_heading(dl, x0, x1, ICON_GAME, "Help name games?");
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
        f32 pic_h = dp(132), pic_w = floorf(pic_h * fw / fh);
        if (beg) {
            i32 f = (i32)(ImGui::GetTime() * BEG_FPS) % BEG_FRAMES;
            ImVec2 uv0((f32)(f % BEG_COLS) / BEG_COLS, (f32)(f / BEG_COLS) / BEG_ROWS);
            ImVec2 uv1(uv0.x + 1.0f / BEG_COLS, uv0.y + 1.0f / BEG_ROWS);
            dl->AddImageRounded((ImTextureID)beg, a, ImVec2(a.x + pic_w, a.y + pic_h), uv0, uv1, IM_COL32_WHITE, dp(4));
            ui_wake(2); // keep drawing: it moves
        } else {
            dl->AddRectFilled(a, ImVec2(a.x + pic_w, a.y + pic_h), col32(COL_PANEL), dp(4));
        }
        f32 x = a.x + pic_w + dp(20), cw = x1 - x;
        ImGui::SetCursorScreenPos(ImVec2(x, a.y));
        ImGui::PushTextWrapPos(x + cw - ImGui::GetWindowPos().x);
        ImGui::PushStyleColor(ImGuiCol_Text, rgb(COL_DIM));
        ImGui::TextUnformatted("When you name a game " APP_NAME " couldn't place, its exe and your name for it go to "
                               APP_NAME "'s service, so the next player gets it named. Nothing else is sent.");
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
        ImGui::SetCursorScreenPos(ImVec2(x, a.y + pic_h - PAIR_H));
        static const char *const titles[] = {"Sure, help", "No thanks"};
        static const char *const subs[] = {"Names you give help everyone", "Keep them to myself"};
        ImGui::BeginDisabled(!service);
        i32 hit = setup_pair("share", titles, subs, c.share_names && service ? 0 : 1, cw, PAIR_H);
        ImGui::EndDisabled();
        if (hit >= 0) {
            c.share_names = hit == 0;
            log_line("setup: help name games %s", c.share_names ? "on" : "off");
            options_apply(0);
        }
        ImGui::SetCursorScreenPos(ImVec2(x0, a.y + pic_h + SECTION_GAP));
    }

    // Usage stats, on unless the answer is no
    setup_heading(dl, x0, x1, ICON_UPLOAD, "Send usage stats?");
    {
        static const char *const titles[] = {"Sure, send", "No thanks"};
        static const char *const subs[] = {"Once a day: counts of clips, saves and links", "Send nothing"};
        ImGui::BeginDisabled(!c.game_lookup);
        i32 hit = setup_pair("stats", titles, subs, c.send_stats && c.game_lookup ? 0 : 1, w, PAIR_H);
        ImGui::EndDisabled();
        if (hit >= 0) {
            c.send_stats = hit == 0;
            log_line("setup: usage stats %s", c.send_stats ? "on" : "off");
            options_apply(0);
        }
    }
    setup_note(dl, x0, x1, "With the version, Windows build and encoder. No names, no files.");
    ImGui::SetCursorScreenPos(ImVec2(x0, ImGui::GetCursorScreenPos().y + SECTION_GAP));

    // Sharing: the player's own Cloudflare, signed in the same way as in Options. Skip leaves it for Options.
    setup_heading(dl, x0, x1, ICON_SHARE, "Sharing");
    {
        static bool skipped;
        R2Status st = r2_status();
        bool idle = st.state == R2_SETUP_IDLE && !r2_ready();
        char status[400];
        u32 col = r2_account_line(st, status, sizeof status);
        if (idle && skipped) str_set(status, "Later, in Options > Integrations");
        ImVec2 a(x0, ImGui::GetCursorScreenPos().y);
        f32 ph = dp(76), lg = dp(36), bars_h = dp(88);
        dl->AddRectFilled(a, ImVec2(a.x + w, a.y + ph + bars_h), col32(COL_PANEL), dp(12));
        host_logo_draw(dl, HOST_R2, ImVec2(a.x + dp(20), floorf(a.y + (ph - lg) / 2)), lg);
        f32 fh = ImGui::GetFrameHeight(), mw = text_w(g_font, "Sign in with Cloudflare") + dp(32);
        f32 sw = text_w(g_font, "Skip") + dp(32), gap = dp(8);
        f32 bx = a.x + w - dp(16) - mw, tx = a.x + dp(20) + lg + dp(16), max_x = bx - sw - gap - dp(16);
        f32 th = g_font->FontSize, sh = g_font_small->FontSize;
        f32 ty = floorf(a.y + (ph - th - dp(4) - sh) / 2);
        opt_text_at(dl, g_font, ImVec2(tx, ty), COL_TEXT, "Your own links that last, and they play in Discord", max_x);
        opt_text_at(dl, g_font_small, ImVec2(tx, ty + th + dp(4)), col, status, max_x);
        f32 by = floorf(a.y + (ph - fh) / 2);
        ImGui::SetCursorScreenPos(ImVec2(bx - sw - gap, by));
        bool can_skip = idle && !skipped && !r2_signed_in() && !r2_has_token();
        if (kit_button("Skip##setupr2", KIT_GHOST, ImVec2(sw, 0), can_skip)) {
            skipped = true;
            log_line("setup: sharing left for later");
        }
        ImGui::SetCursorScreenPos(ImVec2(bx, by));
        r2_account_button("setup", "Sign in with Cloudflare", mw);

        // What Cloudflare gives: the free host's cap on one clip, an arrow, and R2's free storage for all of them in
        // an accent pill under the sign-in button
        {
            char from[24], to[24];
            snprintf(from, sizeof from, "%lld MB", FREE_HOSTS[0].max_bytes / 1000000);
            snprintf(to, sizeof to, "%.0f GB", R2_FREE_GB);
            ImFont *big = g_font_number;
            f32 nh = big->FontSize, pill_h = nh + dp(10), pill_w = text_w(big, to) + dp(40);
            f32 right = a.x + w - dp(16), pill_x = right - pill_w, row_top = a.y + ph + dp(2);
            f32 mid = floorf(row_top + pill_h / 2), cap_y = row_top + pill_h + dp(4);
            opt_text_at(dl, big, ImVec2(tx, floorf(mid - nh / 2)), COL_DIM, from, pill_x);
            opt_text_at(dl, g_font_small, ImVec2(tx, cap_y), COL_DIM, "per clip", pill_x);
            f32 ax0 = tx + text_w(big, from) + dp(24), ax1 = pill_x - dp(16), head = dp(9), thick = dp(2.5f);
            if (ax1 - ax0 > head * 2) {
                dl->AddRectFilled(ImVec2(ax0, mid - thick / 2), ImVec2(ax1 - head, mid + thick / 2), col32(COL_TEXT));
                dl->AddTriangleFilled(ImVec2(ax1 - head * 1.4f, mid - head), ImVec2(ax1, mid), ImVec2(ax1 - head * 1.4f, mid + head),
                                      col32(COL_TEXT));
            }
            dl->AddRectFilled(ImVec2(pill_x, row_top), ImVec2(right, row_top + pill_h), col32(COL_ACCENT), pill_h / 2);
            opt_text_at(dl, big, ImVec2(pill_x + dp(20), floorf(mid - nh / 2)), COL_ACCENT_INK, to, right);
            static const char store[] = "free storage";
            opt_text_at(dl, g_font_small, ImVec2(floorf(pill_x + (pill_w - text_w(g_font_small, store)) / 2), cap_y), COL_DIM,
                        store, right + 1);
        }
        ImGui::SetCursorScreenPos(ImVec2(x0, a.y + ph + bars_h + SECTION_GAP));
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
        ImGui::SetCursorScreenPos(ImVec2(x0, p.y + g_font->FontSize + dp(28)));
    }

    f32 bw = dp(180), bh = dp(44);
    ImGui::SetCursorScreenPos(ImVec2(x1 - bw, ImGui::GetCursorScreenPos().y));
    if (kit_button("Done##setupdone", KIT_PRIMARY, ImVec2(bw, bh))) setup_finish("finished");
    ImGui::Dummy(ImVec2(0, dp(28)));
    ImGui::EndChild();
    ffget_bar();
}
