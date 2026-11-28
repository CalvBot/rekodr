// The first-run setup: a few short pages on a first start, over the library, while recording already runs on the
// defaults. They show the clip key and ask about tooltips, the clips folder, game detection, usage stats and
// sharing. Each answer changes the setting at once; Done on the last page marks it finished. Options > General runs
// it again.

enum { SETUP_BEG_RESOURCE = 10, SETUP_EXAMPLE_RESOURCE = 11 };
// The begging picture (assets/images/setup-beg.jpg): 15 frames of 240 x 230, 5 across and 3 down, at 10 a second.
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
// raised and edged in the accent, with the accent mark; the other is flat and dim. Both keep their size and place.
static bool setup_choice(const char *id, const char *title, const char *sub, bool chosen, f32 w, f32 h)
{
    ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + w, a.y + h);
    bool clicked = ImGui::InvisibleButton(id, ImVec2(w, h));
    if (clicked) dev_click("choice", title);
    bool hot = ImGui::IsItemHovered() && !chosen;
    if (hot) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    f32 rr = dp(12), r = dp(9);
    dl->AddRectFilled(a, b, col32(chosen ? COL_PANEL2 : hot ? COL_HOVER : COL_PANEL), rr);
    if (chosen) dl->AddRect(a, b, col32(COL_ACCENT), rr, 0, dp(1.5f));
    ImVec2 c(a.x + dp(20) + r, a.y + h / 2);
    if (chosen) {
        dl->AddCircleFilled(c, r, col32(COL_ACCENT), 32);
        dl->AddCircleFilled(c, dp(3.5f), col32(COL_ACCENT_INK), 24);
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

// The setup's pages, in order. Back and Next (Done on the last) sit in the same place on every page.
enum SetupPage { SETUP_WELCOME, SETUP_FOLDER, SETUP_DETECTION, SETUP_DEVELOPMENT, SETUP_SHARING, SETUP_PAGES };
static i32 g_setup_page;

static void setup_finish(const char *how)
{
    Config &c = g_app.config;
    c.setup_done = true;
    g_app.ui.setup_open = false;
    g_setup_page = SETUP_WELCOME;
    options_apply(0);
    log_line("setup %s: look-up %s, game detection %s, own key %s, usage stats %s, crash reports %s, tooltips %s, "
             "cloudflare %s", how, c.game_lookup ? "on" : "off", c.art_service.s[0] ? c.art_service.s : "off",
             c.sgdb_key[0] ? "set" : "none", c.send_stats ? "on" : "off", c.send_crashes ? "on" : "off",
             c.tooltips ? "on" : "off", r2_set_up() ? "set up" : "none");
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

// A page's title and the dim lines under it, wrapped to x1. Returns the y under them.
static f32 setup_page_head(ImDrawList *dl, f32 x0, f32 x1, f32 y, const char *title, const char *text)
{
    setup_text_tracked(dl, g_font_title, ImVec2(x0, y), COL_TEXT, title, -dp(0.4f));
    y += g_font_title->FontSize + dp(10);
    ImGui::SetCursorScreenPos(ImVec2(x0, y));
    ImGui::PushTextWrapPos(x1 - ImGui::GetWindowPos().x);
    ImGui::PushStyleColor(ImGuiCol_Text, rgb(COL_DIM));
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();
    return ImGui::GetCursorScreenPos().y + dp(20);
}

// Welcome: what Rekodr does, the clip key as key caps, tooltips and starting with Windows.
static void setup_page_welcome(ImDrawList *dl, f32 x0, f32 x1, f32 y, f32 pair_h)
{
    Config &c = g_app.config;
    setup_text_tracked(dl, g_font_hero, ImVec2(x0, y), COL_TEXT, "Welcome to " APP_NAME, -dp(0.6f));
    y += g_font_hero->FontSize + dp(8);
    opt_text_at(dl, g_font, ImVec2(x0, y), COL_DIM, "It keeps the last while of your games, so you can save a clip after it happens.",
                x1);
    y += g_font->FontSize + dp(24);

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
                    opt_text_at(dl, g_font_big, ImVec2(kx, floorf(ky + (kh - dp(4) - g_font_big->FontSize) / 2)), COL_DIM, "+",
                                kx + dp(30));
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
    y += ph + dp(32);

    opt_text_at(dl, g_font_big, ImVec2(x0, y), COL_TEXT, "Tooltips", x1);
    y += g_font_big->FontSize + dp(12);
    ImGui::SetCursorScreenPos(ImVec2(x0, y));
    static const char *const titles[] = {"Show tooltips", "I know what I'm doing"};
    static const char *const subs[] = {"Point at a button to see what it does", "Get them off my screen"};
    i32 hit = setup_pair("tips", titles, subs, c.tooltips ? 0 : 1, x1 - x0, pair_h);
    if (hit >= 0) {
        c.tooltips = hit == 0;
        log_line("setup: tooltips %s", c.tooltips ? "on" : "off");
        options_apply(0);
    }
    opt_text_at(dl, g_font_small, ImVec2(x0, y + pair_h + dp(10)), COL_DIM, "Either way, F1 lists every key.", x1);
    y += pair_h + dp(10) + g_font_small->FontSize + dp(24);

    // Start with Windows: the Run value, read when the page comes into view and after the switch is turned
    static bool on;
    static i32 read_frame;
    i32 frame = ImGui::GetFrameCount();
    if (!read_frame || frame - read_frame > 1) on = startup_on();
    read_frame = frame;
    f32 rh = dp(64), tw = kit_px(36);
    dl->AddRectFilled(ImVec2(x0, y), ImVec2(x1, y + rh), col32(COL_PANEL), dp(12));
    f32 sy = floorf(y + (rh - g_font_bold->FontSize - dp(5) - g_font_small->FontSize) / 2);
    opt_text_at(dl, g_font_bold, ImVec2(x0 + dp(20), sy), COL_TEXT, "Start with Windows", x1 - tw - dp(40));
    opt_text_at(dl, g_font_small, ImVec2(x0 + dp(20), sy + g_font_bold->FontSize + dp(5)), COL_DIM, "Ready when you sign in",
                x1 - tw - dp(40));
    ImGui::SetCursorScreenPos(ImVec2(x1 - dp(20) - tw, floorf(y + (rh - ImGui::GetFrameHeight()) / 2)));
    if (kit_toggle("##setupstartup", &on)) {
        startup_set(on);
        log_line("setup: start with Windows %s", on ? "on" : "off");
    }
}

// A drive clips can go on
struct SetupDrive {
    wchar_t letter;
    char name[48]; // its label, "" with none
    u64 total, free;
};
static const i32 SETUP_DRIVES = 4;                 // the page shows this many at most
static const u64 SETUP_LOW_BYTES = 20000000000ull; // less free than this is drawn as a warning

// The folder for clips on a drive: the default folder on the default's own drive, <drive>:\Rekodr on any other.
static Path setup_folder_on(const Path &def, wchar_t letter)
{
    if (def.s[0] && def.s[1] == L':' && towupper(def.s[0]) == towupper(letter)) return def;
    Path p = {};
    swprintf(p.s, MAX_PATH, L"%c:\\" APP_NAME, towupper(letter));
    return p;
}

// Keeps the `keep` drives with the most free space, in letter order. Returns how many are left.
static i32 setup_drives_keep(SetupDrive *d, i32 n, i32 keep)
{
    while (n > keep) {
        i32 least = 0;
        for (i32 i = 1; i < n; i++)
            if (d[i].free < d[least].free) least = i;
        for (i32 i = least; i + 1 < n; i++) d[i] = d[i + 1];
        n--;
    }
    return n;
}

// The PC's fixed drives, the SETUP_DRIVES with the most free space when there are more.
static i32 setup_drives(SetupDrive *out)
{
    SetupDrive all[26];
    i32 n = 0;
    DWORD mask = GetLogicalDrives();
    for (i32 i = 0; i < 26; i++) {
        if (!(mask & (1u << i))) continue;
        wchar_t root[] = {(wchar_t)(L'A' + i), L':', L'\\', 0}, label[64] = L"";
        ULARGE_INTEGER avail, total;
        if (GetDriveTypeW(root) != DRIVE_FIXED || !GetDiskFreeSpaceExW(root, &avail, &total, 0) || !total.QuadPart) continue;
        GetVolumeInformationW(root, label, COUNT(label), 0, 0, 0, 0, 0);
        SetupDrive &d = all[n++];
        d = {};
        d.letter = root[0];
        d.total = total.QuadPart;
        d.free = avail.QuadPart;
        wide_to_utf8(label, d.name, sizeof d.name);
    }
    n = setup_drives_keep(all, n, SETUP_DRIVES);
    memcpy(out, all, n * sizeof *out);
    return n;
}

// One drive: its letter and name, a bar of how full it is, and its free space. The one the clips folder is on is
// raised and edged in the accent, as a picked answer is; all keep their size and place.
static bool setup_drive_card(const SetupDrive &d, bool chosen, f32 w, f32 h)
{
    ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + w, a.y + h);
    char letter[8], free_text[32], line[48];
    snprintf(letter, sizeof letter, "%c:", (char)d.letter);
    bool clicked = ImGui::InvisibleButton("##drive", ImVec2(w, h));
    if (clicked) dev_click("drive", letter);
    bool hot = ImGui::IsItemHovered() && !chosen;
    if (hot) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    f32 rr = dp(12), pad = dp(16), x0 = a.x + pad, x1 = b.x - pad;
    dl->AddRectFilled(a, b, col32(chosen ? COL_PANEL2 : hot ? COL_HOVER : COL_PANEL), rr);
    if (chosen) dl->AddRect(a, b, col32(COL_ACCENT), rr, 0, dp(1.5f));
    f32 y = a.y + dp(14), lw = text_w(g_font_big, letter);
    opt_text_at(dl, g_font_big, ImVec2(x0, y), chosen || hot ? COL_TEXT : COL_DIM, letter, x1);
    opt_text_at(dl, g_font_small, ImVec2(x0 + lw + dp(8), floorf(y + g_font_big->FontSize - g_font_small->FontSize - dp(2))),
                COL_FAINT, d.name, x1);
    y += g_font_big->FontSize + dp(12);
    f32 bar_h = dp(6), used = d.total ? (f32)((f64)(d.total - d.free) / (f64)d.total) : 0;
    bool low = d.free < SETUP_LOW_BYTES;
    dl->AddRectFilled(ImVec2(x0, y), ImVec2(x1, y + bar_h), col32(COL_OFF), bar_h / 2);
    f32 ux = fmaxf(x0 + bar_h, floorf(x0 + (x1 - x0) * fminf(1, used)));
    dl->AddRectFilled(ImVec2(x0, y), ImVec2(ux, y + bar_h), col32(low ? COL_WARN : chosen ? COL_ACCENT : COL_DIM), bar_h / 2);
    y += bar_h + dp(10);
    bytes_text((i64)d.free, free_text, sizeof free_text);
    snprintf(line, sizeof line, "%s free", free_text);
    opt_text_at(dl, g_font_bold, ImVec2(x0, y), low ? COL_WARN : chosen || hot ? COL_TEXT : COL_DIM, line, x1);
    return clicked;
}

// The clips folder: the PC's drives side by side with their free space, and under them the folder and a button for
// any other one. A drive's card puts the folder on it (setup_folder_on).
static void setup_page_folder(ImDrawList *dl, f32 x0, f32 x1, f32 y)
{
    Config &c = g_app.config;
    y = setup_page_head(dl, x0, x1, y, "Where should clips go?", "Clips and sessions are saved here. Pick a drive with room.");
    // The drives, read when the page comes into view and after the folder changes
    static SetupDrive drives[SETUP_DRIVES];
    static i32 drive_count, read_frame;
    static u64 folder_free;
    i32 frame = ImGui::GetFrameCount();
    if (!read_frame || frame - read_frame > 1) {
        drive_count = setup_drives(drives);
        folder_free = backup_free(c.save_dir);
    }
    read_frame = frame;

    bool changed = false;
    wchar_t on = c.save_dir.s[1] == L':' ? (wchar_t)towupper(c.save_dir.s[0]) : 0;
    f32 gap = dp(12), card_h = dp(104);
    i32 cols = drive_count < 2 ? 2 : drive_count;
    f32 cw = floorf((x1 - x0 - gap * (cols - 1)) / cols);
    for (i32 i = 0; i < drive_count; i++) {
        ImGui::SetCursorScreenPos(ImVec2(x0 + i * (cw + gap), y));
        ImGui::PushID(i);
        if (setup_drive_card(drives[i], drives[i].letter == on, cw, card_h) && drives[i].letter != on) {
            c.save_dir = setup_folder_on(config_default_save_dir(), drives[i].letter);
            changed = true;
        }
        ImGui::PopID();
    }
    if (drive_count) y += card_h + dp(16);

    // The folder, and the picker for one that isn't a drive's own
    f32 rh = dp(64), fh = ImGui::GetFrameHeight(), pad = dp(20);
    dl->AddRectFilled(ImVec2(x0, y), ImVec2(x1, y + rh), col32(COL_PANEL), dp(12));
    char path[MAX_PATH * 3], free_text[32], sub[64];
    wide_to_utf8(c.save_dir.s, path, sizeof path);
    bytes_text((i64)folder_free, free_text, sizeof free_text);
    snprintf(sub, sizeof sub, "%s free", free_text);
    f32 ty = floorf(y + (rh - g_font_bold->FontSize - dp(5) - g_font_small->FontSize) / 2), tx1 = x1 - pad - fh - dp(16);
    opt_text_mid(dl, g_font_bold, ImVec2(x0 + pad, ty), COL_TEXT, path, tx1);
    opt_text_at(dl, g_font_small, ImVec2(x0 + pad, ty + g_font_bold->FontSize + dp(5)),
                folder_free < SETUP_LOW_BYTES ? COL_WARN : COL_DIM, sub, tx1);
    ImGui::SetCursorScreenPos(ImVec2(x1 - pad - fh, floorf(y + (rh - fh) / 2)));
    Path picked = {};
    kit_next_tip("Choose a folder");
    if (kit_icon_button(ICON_FOLDER, "##setupfolder", KIT_SECONDARY) && pick_folder(c.save_dir, &picked) &&
        wcscmp(picked.s, c.save_dir.s)) {
        c.save_dir = picked;
        changed = true;
    }
    if (changed) {
        wide_to_utf8(c.save_dir.s, path, sizeof path);
        log_line("setup: save folder %s", path);
        options_apply(0);
        read_frame = 0; // read the free space again
    }
    opt_text_at(dl, g_font_small, ImVec2(x0, y + rh + dp(12)), COL_DIM, "Or later, in Options > General.", x1);
}

// What No leaves to the player, shown while No is picked: the same clip as a library card with the service, filed
// under its game, beside the game's art, with its icon, and without, where the game isn't known and the card has only the
// exe's name; then what the player does by hand, and a box for their own SteamGridDB key. The clip's picture is a
// frame built into the exe (assets/images/setup-example.jpg).
static const char SETUP_EXAMPLE_GAME[] = "Counter-Strike 2", SETUP_EXAMPLE_EXE[] = "cs2.exe";
static const f32 SETUP_EXAMPLE_ART_X = 0.72f; // where across its art the example shows: the players

// One example card on a panel at a, w wide: the game's art shows on the panel's left, fading into it under the
// clip card on its right (a plain panel with a faint game glyph when there is no art), and the card is
// drawn as the library draws it, with its picture, length, game icon, name and dim line. Returns the panel's height.
static f32 setup_example_card(ImDrawList *dl, ImVec2 a, f32 w, bool known)
{
    static ID3D11ShaderResourceView *pic;
    static bool pic_tried;
    if (!pic_tried) {
        pic_tried = true;
        i32 pw, ph;
        pic = resource_texture(SETUP_EXAMPLE_RESOURCE, &pw, &ph);
    }
    f32 r = kit_px(4), pad = dp(12), cw = floorf(w * 0.62f), th = floorf(cw * 9 / 16);
    f32 isz = g_font_bold->FontSize + kit_px(2) + g_font_small->FontSize, ph = pad + th + kit_px(8) + isz + pad;
    ImVec2 p0 = a, p1(a.x + w, a.y + ph);
    ImVec2 t0(p1.x - pad - cw, a.y + pad), t1(t0.x + cw, t0.y + th);
    f32 aw = t0.x + dp(24) - p0.x, fade0 = p0.x + aw * 0.6f, fade1 = p0.x + aw;
    i32 gw = 0, gh = 0;
    ID3D11ShaderResourceView *art = known ? game_db_cover(SETUP_EXAMPLE_GAME, &gw, &gh) : 0;
    if (art && gw > 0 && gh > 0) { // on the panel's left, cut to that shape around SETUP_EXAMPLE_ART_X across
        dl->AddRectFilled(p0, p1, col32(COL_PANEL), dp(12));
        f32 want = aw / ph, have = (f32)gw / gh, u0 = 0, u1 = 1, v = 0;
        if (have > want) {
            f32 span = want / have;
            u0 = SETUP_EXAMPLE_ART_X - span / 2;
            u0 = u0 < 0 ? 0 : u0 > 1 - span ? 1 - span : u0;
            u1 = u0 + span;
        } else v = (1 - have / want) / 2;
        dl->AddImageRounded((ImTextureID)art, p0, ImVec2(fade1, p1.y), ImVec2(u0, v), ImVec2(u1, 1 - v), IM_COL32_WHITE, dp(12),
                            ImDrawFlags_RoundCornersLeft);
        ImU32 clear = col32(COL_PANEL, 0), solid = col32(COL_PANEL);
        dl->AddRectFilledMultiColor(ImVec2(fade0, p0.y), ImVec2(fade1, p1.y), clear, solid, solid, clear);
    } else {
        dl->AddRectFilled(p0, p1, col32(COL_PANEL), dp(12));
        f32 gs = kit_px(22);
        ImVec2 sz = g_font_icon->CalcTextSizeA(gs, FLT_MAX, 0, ICON_GAME);
        dl->AddText(g_font_icon, gs, ImVec2(floorf((p0.x + t0.x - sz.x) / 2), floorf(p0.y + (ph - sz.y) / 2)),
                    col32(COL_FAINT), ICON_GAME);
    }
    if (pic) dl->AddImageRounded((ImTextureID)pic, t0, t1, ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, r);
    else dl->AddRectFilled(t0, t1, col32(COL_PANEL2), r);
    badge(dl, ImVec2(t1.x - kit_px(8), t1.y - kit_px(8)), true, true, "0:22");
    // The caption: the game's icon (a plain glyph when no game is known), the name and one dim line
    f32 y = t1.y + kit_px(8);
    ID3D11ShaderResourceView *icon = known ? game_icon(SETUP_EXAMPLE_GAME) : 0;
    if (icon) dl->AddImageRounded((ImTextureID)icon, ImVec2(t0.x, y), ImVec2(t0.x + isz, y + isz), ImVec2(0, 0), ImVec2(1, 1),
                                  IM_COL32_WHITE, kit_px(3));
    else {
        dl->AddRectFilled(ImVec2(t0.x, y), ImVec2(t0.x + isz, y + isz), col32(COL_PANEL2), kit_px(3));
        ImVec2 is = g_font_icon->CalcTextSizeA(g_font_icon->FontSize, FLT_MAX, 0, ICON_VIDEO);
        dl->AddText(g_font_icon, g_font_icon->FontSize, ImVec2(floorf(t0.x + (isz - is.x) / 2), floorf(y + (isz - is.y) / 2)),
                    col32(COL_FAINT), ICON_VIDEO);
    }
    f32 tx = t0.x + isz + kit_px(10);
    opt_text_at(dl, g_font_bold, ImVec2(tx, y), COL_TEXT, "Example clip", t1.x);
    char line[96];
    snprintf(line, sizeof line, "%s  -  Today", known ? SETUP_EXAMPLE_GAME : SETUP_EXAMPLE_EXE);
    opt_text_at(dl, g_font_small, ImVec2(tx, y + g_font_bold->FontSize + kit_px(2)), COL_DIM, line, t1.x);
    return ph;
}

static void setup_detection_by_hand(ImDrawList *dl, f32 x0, f32 x1, f32 y)
{
    Config &c = g_app.config;
    f32 sh = g_font_small->FontSize, bh = g_font_bold->FontSize, fh = ImGui::GetFrameHeight();

    // The same clip with the service and without
    f32 gap = dp(24), ew = floorf((x1 - x0 - gap) / 2), card_h = 0;
    for (i32 i = 0; i < 2; i++) {
        f32 ex = x0 + i * (ew + gap);
        opt_text_at(dl, g_font_small, ImVec2(ex, y), COL_DIM, i == 0 ? "With our service" : "Without", ex + ew);
        card_h = setup_example_card(dl, ImVec2(ex, y + sh + dp(8)), ew, i == 0);
    }

    // What is then done by hand, under them in two columns
    static const char *const icons[] = {ICON_EDIT, ICON_APPS, ICON_GAME, ICON_TAG};
    static const char *const lines[] = {"Name each game Discord doesn't know", "Sort it in Options > Apps",
                                        "Add your own key for its art", "Go without modpack names and emojis"};
    f32 ly = y + sh + dp(8) + card_h + dp(20);
    opt_text_at(dl, g_font_bold, ImVec2(x0, ly), COL_TEXT, "Without it, you", x1);
    ly += bh + dp(10);
    for (i32 i = 0; i < 4; i++) {
        f32 lx = x0 + (i % 2) * (ew + gap), row = ly + (i / 2) * (sh + dp(10)), is = dp(14);
        dl->AddText(g_font_icon, is, ImVec2(lx, floorf(row + (sh - is) / 2 + dp(1))), col32(COL_DIM), icons[i]);
        opt_text_at(dl, g_font_small, ImVec2(lx + dp(24), row), COL_DIM, lines[i], lx + ew);
    }

    // The player's own SteamGridDB key, for art the service would have found
    f32 pad = dp(16), cy = ly + 2 * (sh + dp(10)) + dp(10);
    f32 key_h = pad + bh + dp(4) + sh + dp(12) + fh + pad;
    dl->AddRectFilled(ImVec2(x0, cy), ImVec2(x1, cy + key_h), col32(COL_PANEL), dp(12));
    f32 bw = text_w(g_font, "Get a key") + dp(36), text_end = x1 - pad - bw - dp(12);
    opt_text_at(dl, g_font_bold, ImVec2(x0 + pad, cy + pad), COL_TEXT, "Your own SteamGridDB key", text_end);
    opt_text_at(dl, g_font_small, ImVec2(x0 + pad, cy + pad + bh + dp(4)), COL_DIM,
                "Free: sign in at steamgriddb.com, then Preferences > API", text_end);
    ImGui::SetCursorScreenPos(ImVec2(x1 - pad - bw, floorf(cy + pad + (bh + dp(4) + sh - fh) / 2)));
    if (kit_button("Get a key##setupsgdb", KIT_SECONDARY, ImVec2(bw, 0))) open_url(OPT_SGDB_KEY_URL);
    ImGui::SetCursorScreenPos(ImVec2(x0 + pad, cy + pad + bh + dp(4) + sh + dp(12)));
    char key[sizeof c.sgdb_key];
    if (opt_text("##setupsgdbkey", c.sgdb_key, x1 - x0 - 2 * pad, ImGuiInputTextFlags_Password, "Paste your key here", key,
                 sizeof key)) {
        str_set(c.sgdb_key, trim(key));
        log_line("setup: own SteamGridDB key %s", c.sgdb_key[0] ? "set" : "cleared");
        options_apply(0);
    }
}

// Game detection: whether Rekodr's service is asked about the player's games (art_service) and told the games the
// player picks (gamedb_pick). No turns it off, so nothing about a game goes to the service; games are then named from
// Discord's game list and the names the player types.
static void setup_page_detection(ImDrawList *dl, f32 x0, f32 x1, f32 y, f32 pair_h)
{
    Config &c = g_app.config;
    y = setup_page_head(dl, x0, x1, y, "Use our game detection?",
                        APP_NAME " looks at a game's exe and checks it against our list, so your clips get the game's "
                        "name and art.");
    ImGui::SetCursorScreenPos(ImVec2(x0, y));
    static const char *const titles[] = {"Sure", "No thanks"};
    static const char *const subs[] = {"Names, art and sorting, filled in for you", "Only Discord's game list; the rest by hand"};
    bool on = c.game_lookup && c.art_service.s[0];
    ImGui::BeginDisabled(!c.game_lookup);
    i32 hit = setup_pair("detect", titles, subs, on ? 0 : 1, x1 - x0, pair_h);
    ImGui::EndDisabled();
    if (hit >= 0) {
        bool yes = hit == 0;
        str_set(c.art_service.s, yes ? ART_SERVICE_HOST : "");
        gamedb_set_service(c.art_service.s);
        log_line("setup: game detection %s", yes ? "on" : "off");
        options_apply(0);
    }
    const char *note = !c.game_lookup ? "Look up games online is off in Options, so this is too."
                       : on           ? "A game's exe, folder names, store id and the game you pick for it go to the service. Never a path."
                                      : "Nothing about your games goes to " APP_NAME "'s service. Games Discord knows keep their art.";
    opt_text_at(dl, g_font_small, ImVec2(x0, y + pair_h + dp(10)), COL_DIM, note, x1);
    if (!c.game_lookup || on) return;
    setup_detection_by_hand(dl, x0, x1, y + pair_h + dp(40));
}

// Help development: the daily usage counts, and under them crash reports. The begging picture shows only once the
// answer to the counts is no.
static void setup_page_development(ImDrawList *dl, f32 x0, f32 x1, f32 y, f32 pair_h)
{
    Config &c = g_app.config;
    y = setup_page_head(dl, x0, x1, y, "Help development?",
                        "Once a day, " APP_NAME " sends its version, your Windows build and encoder, counts of clips, "
                        "saves, shares and links, and per game the play time and clips. Your settings too, at most "
                        "weekly: values only, for folders, keys and hotkeys only whether you changed them. No paths or "
                        "files.");
    ImGui::SetCursorScreenPos(ImVec2(x0, y));
    static const char *const titles[] = {"Sure, send", "No thanks"};
    static const char *const subs[] = {"Counts, play time, settings", "Send nothing"};
    ImGui::BeginDisabled(!c.game_lookup);
    i32 hit = setup_pair("stats", titles, subs, c.send_stats && c.game_lookup ? 0 : 1, x1 - x0, pair_h);
    ImGui::EndDisabled();
    if (hit >= 0) {
        c.send_stats = hit == 0;
        log_line("setup: usage stats %s", c.send_stats ? "on" : "off");
        options_apply(0);
    }
    y += pair_h + dp(16);
    ImGui::SetCursorScreenPos(ImVec2(x0, y));
    static const char *const crash_titles[] = {"Send crash reports", "Keep them here"};
    static const char *const crash_subs[] = {"Where it stopped, after a crash", "Send nothing"};
    ImGui::BeginDisabled(!c.game_lookup);
    hit = setup_pair("crashes", crash_titles, crash_subs, c.send_crashes && c.game_lookup ? 0 : 1, x1 - x0, pair_h);
    ImGui::EndDisabled();
    if (hit >= 0) {
        c.send_crashes = hit == 0;
        log_line("setup: crash reports %s", c.send_crashes ? "on" : "off");
        options_apply(0);
    }
    if (c.send_stats && c.game_lookup) return;
    static ID3D11ShaderResourceView *beg;
    static i32 beg_w, beg_h;
    static bool tried;
    if (!tried) {
        beg = resource_texture(SETUP_BEG_RESOURCE, &beg_w, &beg_h);
        tried = true;
    }
    if (!beg) return;
    f32 pic_h = dp(150), pic_w = floorf(pic_h * ((f32)beg_w / BEG_COLS) / ((f32)beg_h / BEG_ROWS));
    ImVec2 a(floorf((x0 + x1 - pic_w) / 2), y + pair_h + dp(28));
    i32 f = (i32)(ImGui::GetTime() * BEG_FPS) % BEG_FRAMES;
    ImVec2 uv0((f32)(f % BEG_COLS) / BEG_COLS, (f32)(f / BEG_COLS) / BEG_ROWS);
    ImVec2 uv1(uv0.x + 1.0f / BEG_COLS, uv0.y + 1.0f / BEG_ROWS);
    dl->AddImageRounded((ImTextureID)beg, a, ImVec2(a.x + pic_w, a.y + pic_h), uv0, uv1, IM_COL32_WHITE, dp(4));
    ui_wake(2); // keep drawing: it moves
}

// Discord's upload limit on one file without Nitro, in MB.
static const i32 DISCORD_FREE_MB = 20;

// Sharing: the player's own Cloudflare, as the upgrade from posting the file in Discord. Discord's cap on one file,
// an arrow, and R2's free storage for links; under it the account, signed in the same way as in Options.
static void setup_page_sharing(ImDrawList *dl, f32 x0, f32 x1, f32 y)
{
    y = setup_page_head(dl, x0, x1, y, "Share with your own links",
                        "Free with a Cloudflare account. Your links last, and they play in Discord.");
    f32 pad = dp(28), sh = g_font_small->FontSize;
    f32 big_h = g_font_number->FontSize, pill_h = big_h + dp(10);
    f32 hero_h = pill_h + dp(8) + sh, acc_h = dp(64), card_h = pad + hero_h + pad + acc_h;
    ImVec2 a(x0, y), b(x1, y + card_h);
    dl->AddRectFilled(a, b, col32(COL_PANEL), dp(12));

    // 20 MB -> 10 GB
    {
        char from[24], to[24];
        snprintf(from, sizeof from, "%d MB", DISCORD_FREE_MB);
        snprintf(to, sizeof to, "%.0f GB", R2_FREE_GB);
        f32 left = x0 + pad, right = x1 - pad, top = y + pad, mid = floorf(top + pill_h / 2);
        f32 pill_w = text_w(g_font_number, to) + dp(40), pill_x = right - pill_w, cap_y = top + pill_h + dp(8);
        opt_text_at(dl, g_font_number, ImVec2(left, floorf(mid - big_h / 2)), COL_FAINT, from, pill_x);
        opt_text_at(dl, g_font_small, ImVec2(left, cap_y), COL_DIM, "a file in Discord", pill_x);
        f32 ax0 = left + text_w(g_font_number, from) + dp(24), ax1 = pill_x - dp(16), head = dp(8), thick = dp(2);
        if (ax1 - ax0 > head * 2) {
            dl->AddRectFilled(ImVec2(ax0, mid - thick / 2), ImVec2(ax1 - head, mid + thick / 2), col32(COL_DIM));
            dl->AddTriangleFilled(ImVec2(ax1 - head * 1.4f, mid - head), ImVec2(ax1, mid), ImVec2(ax1 - head * 1.4f, mid + head),
                                  col32(COL_DIM));
        }
        dl->AddRectFilled(ImVec2(pill_x, top), ImVec2(right, top + pill_h), col32(COL_ACCENT), pill_h / 2);
        opt_text_at(dl, g_font_number, ImVec2(pill_x + dp(20), floorf(mid - big_h / 2)), COL_ACCENT_INK, to, right);
        static const char store[] = "free storage";
        opt_text_at(dl, g_font_small, ImVec2(floorf(pill_x + (pill_w - text_w(g_font_small, store)) / 2), cap_y), COL_DIM,
                    store, right + 1);
    }

    // The account: Cloudflare's mark, its name and state, and the one button
    {
        f32 ay = y + pad + hero_h + pad;
        dl->AddLine(ImVec2(x0 + pad, ay), ImVec2(x1 - pad, ay), col32(COL_LINE), dp(1));
        f32 lg = dp(32), my = ay + acc_h / 2, tx = x0 + pad + lg + dp(14);
        host_logo_draw(dl, HOST_R2, ImVec2(x0 + pad, floorf(my - lg / 2)), lg);
        R2Status st = r2_status();
        char status[400];
        u32 col = r2_account_line(st, status, sizeof status);
        f32 th = g_font_bold->FontSize, ty = floorf(my - (th + dp(4) + sh) / 2);
        f32 bw = text_w(g_font, "Sign in with Cloudflare") + dp(36), fh = ImGui::GetFrameHeight();
        opt_text_at(dl, g_font_bold, ImVec2(tx, ty), COL_TEXT, "Cloudflare R2", x1 - pad - bw - dp(16));
        opt_text_at(dl, g_font_small, ImVec2(tx, ty + th + dp(4)), col, status, x1 - pad - bw - dp(16));
        ImGui::SetCursorScreenPos(ImVec2(x1 - pad - bw, floorf(my - fh / 2)));
        r2_account_button("setup", "Sign in with Cloudflare", bw);
    }
    opt_text_at(dl, g_font_small, ImVec2(x0, b.y + dp(12)), COL_DIM, "Or later, in Options > Integrations.", x1);
}

static void setup_window()
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(kit_px(28), kit_px(24)));
    ImGui::BeginChild("##setup", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding, 0);
    ImGui::PopStyleVar();
    ImVec2 avail = ImGui::GetContentRegionAvail(), top = ImGui::GetCursorScreenPos();
    f32 w = fminf(avail.x, dp(640)), x0 = floorf(top.x + (avail.x - w) / 2), x1 = x0 + w;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const f32 PAIR_H = dp(68), PAGE_H = dp(470);
    f32 y = top.y + dp(28);
    i32 page = g_setup_page;
    switch (page) {
    case SETUP_WELCOME: setup_page_welcome(dl, x0, x1, y, PAIR_H); break;
    case SETUP_FOLDER: setup_page_folder(dl, x0, x1, y); break;
    case SETUP_DETECTION: setup_page_detection(dl, x0, x1, y, PAIR_H); break;
    case SETUP_DEVELOPMENT: setup_page_development(dl, x0, x1, y, PAIR_H); break;
    default: setup_page_sharing(dl, x0, x1, y); break;
    }

    // Back, the page dots and Next or Done: at the window's foot, or under the tallest page if the window is short
    f32 fh = ImGui::GetFrameHeight(), bw = dp(120);
    f32 by = floorf(fmaxf(y + PAGE_H, top.y + avail.y - fh - dp(8)));
    ImGui::SetCursorScreenPos(ImVec2(x0, by));
    if (kit_button("Back##setupback", KIT_SECONDARY, ImVec2(bw, 0), page > 0)) g_setup_page = page - 1;
    {
        f32 d = dp(6), cur = dp(18), gap = dp(6), dw = (SETUP_PAGES - 1) * d + cur + (SETUP_PAGES - 1) * gap;
        f32 dx = floorf((x0 + x1 - dw) / 2), dy = floorf(by + (fh - d) / 2);
        for (i32 i = 0; i < SETUP_PAGES; i++) {
            f32 pw = i == page ? cur : d;
            dl->AddRectFilled(ImVec2(dx, dy), ImVec2(dx + pw, dy + d), col32(i == page ? COL_TEXT : COL_FAINT), d / 2);
            dx += pw + gap;
        }
    }
    ImGui::SetCursorScreenPos(ImVec2(x1 - bw, by));
    if (page < SETUP_PAGES - 1) {
        if (kit_button("Next##setupnext", KIT_PRIMARY, ImVec2(bw, 0))) g_setup_page = page + 1;
    } else if (kit_button("Done##setupdone", KIT_PRIMARY, ImVec2(bw, 0))) {
        setup_finish("finished");
    }
    ImGui::SetCursorScreenPos(ImVec2(x0, by + fh));
    ImGui::Dummy(ImVec2(0, dp(16)));
    kit_scroll_fade_window();
    ImGui::EndChild();
    ffget_bar();
}
