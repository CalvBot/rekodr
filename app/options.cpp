// Options: every setting.
//
// Every change applies at once: it's written to settings.txt (config_save) and handed to whatever uses it
// (options_apply). There's no Save button. Text and number boxes count only when the edit is finished (Enter or
// clicking away), so a half-typed bitrate never restarts the capture.
//
// Layout rules (SPEC.md, "UI rules"): fixed-width controls, fixed-size paged lists whose order is frozen while
// Options is open, and settings that don't apply are disabled, never hidden.

#include <shobjidl.h>
#include <math.h>

// What a change touches, for options_apply.
enum : u32 {
    APPLY_RECORDER = 1, // recorder_settings_changed: capture, encoder, buffer folder and limits
    APPLY_AUDIO = 2,    // audio_config_changed: app categories, names, the mic
    APPLY_HOTKEYS = 4,  // hotkeys_apply
};

enum AppsFilter : i32 { FILTER_ALL, FILTER_NEW, FILTER_GAMES, FILTER_COMMUNICATION, FILTER_APPS, FILTER_IGNORED, FILTER_FOLDERS };

static f32 dp(f32 v) { return v * g_dpi_scale; }

static void options_apply(u32 what)
{
    config_save(g_app.config, g_app.paths);
    if (what & APPLY_RECORDER) recorder_settings_changed();
    if (what & APPLY_AUDIO) audio_config_changed();
    if ((what & APPLY_HOTKEYS) && !is_test_copy()) hotkeys_apply(g_app.config, g_app.msg_hwnd);
    ui_wake();
}

// ---------------------------------------------------------------------------------------------------------------
// Start with Windows: a "clipback" value under HKCU\...\Run, the same name the Rust version used, so turning it on
// here replaces the old one.

static const wchar_t *RUN_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

static void startup_command(wchar_t *out, i32 cap)
{
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(0, exe, MAX_PATH);
    swprintf(out, cap, L"\"%s\"", exe);
}

// The Run value as it is now. Returns false if there's none.
static bool startup_read(wchar_t *out, i32 cap)
{
    DWORD bytes = (DWORD)(cap * sizeof(wchar_t));
    out[0] = 0;
    return RegGetValueW(HKEY_CURRENT_USER, RUN_KEY, L"clipback", RRF_RT_REG_SZ, 0, out, &bytes) == ERROR_SUCCESS;
}

static void startup_set(bool on)
{
    wchar_t cmd[MAX_PATH + 8];
    startup_command(cmd, COUNT(cmd));
    if (is_test_copy()) {
        log_line("start with Windows %s; a test copy leaves the Run key alone (it would %s HKCU\\%ls\\clipback%s%ls)",
                 on ? "on" : "off", on ? "set" : "delete", RUN_KEY, on ? " = " : "", on ? cmd : L"");
        return;
    }
    HKEY key = 0;
    LSTATUS r = RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &key);
    if (r == ERROR_SUCCESS) {
        if (on) r = RegSetValueExW(key, L"clipback", 0, REG_SZ, (const BYTE *)cmd, (DWORD)((wcslen(cmd) + 1) * sizeof(wchar_t)));
        else {
            r = RegDeleteValueW(key, L"clipback");
            if (r == ERROR_FILE_NOT_FOUND) r = ERROR_SUCCESS; // already gone
        }
        RegCloseKey(key);
    }
    if (r == ERROR_SUCCESS) log_line("start with Windows %s", on ? "on" : "off");
    else log_line("start with Windows: updating the Run key failed (%d)", (int)r);
}

// ---------------------------------------------------------------------------------------------------------------
// Monitors, in ddagrab's output_idx order (the outputs of the first GPU), with the names Display settings shows.

static void options_list_monitors()
{
    OptionsUi &o = g_app.options;
    o.monitor_count = 0;

    // GDI device name (\\.\DISPLAY1) → monitor name
    static DISPLAYCONFIG_PATH_INFO paths[32];
    static DISPLAYCONFIG_MODE_INFO modes[64];
    UINT32 np = COUNT(paths), nm = COUNT(modes);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths, &nm, modes, 0) != ERROR_SUCCESS) np = 0;

    IDXGIFactory1 *factory = 0;
    IDXGIAdapter1 *adapter = 0;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return;
    if (SUCCEEDED(factory->EnumAdapters1(0, &adapter))) {
        IDXGIOutput *out = 0;
        for (UINT i = 0; o.monitor_count < COUNT(o.monitors) && adapter->EnumOutputs(i, &out) == S_OK; i++) {
            DXGI_OUTPUT_DESC d = {};
            out->GetDesc(&d);
            out->Release();
            MonitorInfo *m = &o.monitors[o.monitor_count++];
            *m = {};
            m->width = d.DesktopCoordinates.right - d.DesktopCoordinates.left;
            m->height = d.DesktopCoordinates.bottom - d.DesktopCoordinates.top;
            m->primary = d.DesktopCoordinates.left == 0 && d.DesktopCoordinates.top == 0;
            DEVMODEW mode = {};
            mode.dmSize = sizeof mode;
            if (EnumDisplaySettingsW(d.DeviceName, ENUM_CURRENT_SETTINGS, &mode)) m->hz = (i32)mode.dmDisplayFrequency;
            wide_to_utf8(d.DeviceName[0] == '\\' ? d.DeviceName + 4 : d.DeviceName, m->name, sizeof m->name);
            for (UINT32 k = 0; k < np; k++) {
                DISPLAYCONFIG_SOURCE_DEVICE_NAME src = {};
                src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
                src.header.size = sizeof src;
                src.header.adapterId = paths[k].sourceInfo.adapterId;
                src.header.id = paths[k].sourceInfo.id;
                DISPLAYCONFIG_TARGET_DEVICE_NAME tgt = {};
                tgt.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
                tgt.header.size = sizeof tgt;
                tgt.header.adapterId = paths[k].targetInfo.adapterId;
                tgt.header.id = paths[k].targetInfo.id;
                if (DisplayConfigGetDeviceInfo(&src.header) || DisplayConfigGetDeviceInfo(&tgt.header)) continue;
                if (_wcsicmp(src.viewGdiDeviceName, d.DeviceName) || !tgt.monitorFriendlyDeviceName[0]) continue;
                wide_to_utf8(tgt.monitorFriendlyDeviceName, m->name, sizeof m->name);
                break;
            }
        }
        adapter->Release();
    }
    factory->Release();
}

// ---------------------------------------------------------------------------------------------------------------
// Controls. A page is built from cards (opt_card_begin/end) holding rows (opt_row_begin/end): the setting's name on
// the left with at most one short dim line under it, its control right-aligned. Rows have fixed heights and
// controls fixed widths, so nothing moves when a value changes. The opt_card, opt_row, opt_combo, opt_keycaps,
// opt_pill and opt_pager helpers are generic and could move to the kit in ui.cpp.

static const f32 OPT_PAD = 20;     // a card's padding left and right
static const f32 OPT_FIELD_W = 84; // a number box
static const f32 OPT_UNIT_W = 44;  // the unit after it ("Mbps", "hours")

// A text box whose edit counts only once it's finished. Returns true then, with the text in `out`.
static bool opt_text(const char *id, const char *current, f32 width, ImGuiInputTextFlags flags, const char *hint,
                     char *out, i32 cap)
{
    OptionsUi &o = g_app.options;
    bool active = ImGui::GetActiveID() == ImGui::GetID(id);
    char shown[MAX_PATH * 3];
    str_set(shown, current);
    char *buf = active ? o.edit : shown;
    ImGui::SetNextItemWidth(width);
    if (hint) ImGui::InputTextWithHint(id, hint, buf, sizeof o.edit, flags);
    else ImGui::InputText(id, buf, sizeof o.edit, flags);
    if (ImGui::IsItemActivated()) str_set(o.edit, current);
    if (!ImGui::IsItemDeactivatedAfterEdit()) return false;
    snprintf(out, cap, "%s", o.edit);
    return true;
}

// A number box. Returns true when an edit finished with a different number; it's clamped to lo..hi. With
// `blank_zero`, 0 shows as an empty box (and an empty box means 0).
static bool opt_number(const char *id, f64 *v, f64 lo, f64 hi, f32 width, bool blank_zero = false, const char *hint = 0)
{
    char cur[64] = "";
    if (!(blank_zero && *v == 0)) snprintf(cur, sizeof cur, "%g", *v);
    char text[64];
    if (!opt_text(id, cur, width, ImGuiInputTextFlags_CharsDecimal, hint, text, sizeof text)) return false;
    char *s = trim(text);
    f64 n = 0;
    if (*s) {
        char *end = 0;
        n = strtod(s, &end);
        if (end == s) return false;
        if (n < lo) n = lo;
        if (n > hi) n = hi;
    } else if (!blank_zero) {
        return false;
    }
    if (n == *v) return false;
    *v = n;
    return true;
}

static bool opt_f32(const char *id, f32 *v, f64 lo, f64 hi, f32 width)
{
    f64 d = *v;
    if (!opt_number(id, &d, lo, hi, width)) return false;
    *v = (f32)d;
    return true;
}

static bool opt_i32(const char *id, i32 *v, f64 lo, f64 hi, f32 width)
{
    f64 d = *v;
    if (!opt_number(id, &d, lo, hi, width)) return false;
    d = floor(d + 0.5);
    if ((i32)d == *v) return false;
    *v = (i32)d;
    return true;
}

// Text in `font` at p, cut short with an ellipsis before max_x. Fades inside BeginDisabled like the widgets.
static void opt_text_at(ImDrawList *dl, ImFont *font, ImVec2 p, u32 col, const char *text, f32 max_x)
{
    if (max_x <= p.x) return;
    ImGui::PushFont(font);
    ImGui::PushStyleColor(ImGuiCol_Text, rgb(col));
    ImGui::RenderTextEllipsis(dl, p, ImVec2(max_x, p.y + ImGui::GetFontSize()), max_x, max_x, text, 0, 0);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

// The same, ending at right_x (and starting no further left than min_x).
static void opt_text_right(ImDrawList *dl, ImFont *font, f32 right_x, f32 y, u32 col, const char *text, f32 min_x)
{
    ImGui::PushFont(font);
    f32 w = ImGui::CalcTextSize(text).x;
    ImGui::PopFont();
    opt_text_at(dl, font, ImVec2(fmaxf(min_x, right_x - w), y), col, text, right_x + 1);
}

static f32 text_w(ImFont *font, const char *text)
{
    ImGui::PushFont(font);
    f32 w = ImGui::CalcTextSize(text).x;
    ImGui::PopFont();
    return w;
}

// A small rounded tag: a count on a tab, "new" on an app. p is its left end, vertically centred.
static f32 opt_pill_w(const char *text) { return fmaxf(dp(18), text_w(g_font_small, text) + dp(12)); }
static void opt_pill(ImDrawList *dl, ImVec2 p, const char *text, u32 fill, u32 ink)
{
    f32 h = dp(18), w = opt_pill_w(text);
    dl->AddRectFilled(ImVec2(p.x, p.y - h / 2), ImVec2(p.x + w, p.y + h / 2), col32(fill), h / 2);
    f32 tw = text_w(g_font_small, text);
    opt_text_at(dl, g_font_small, ImVec2(floorf(p.x + (w - tw) / 2), floorf(p.y - g_font_small->FontSize / 2) - 1), ink,
                text, p.x + w);
}

// Cards: a raised panel as tall as what's drawn in it. The draw list is split so the panel goes under its rows.
struct OptCard { ImVec2 a; f32 w; i32 rows; };
static OptCard g_card;
static f32 g_card_max_w = FLT_MAX; // settings pages keep their cards to a readable width; lists use the whole page

static void opt_card_begin()
{
    g_card = {};
    g_card.a = ImGui::GetCursorScreenPos();
    g_card.w = fminf(ImGui::GetContentRegionAvail().x, g_card_max_w);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->ChannelsSplit(2);
    dl->ChannelsSetCurrent(1);
    ImGui::SetCursorScreenPos(ImVec2(g_card.a.x, g_card.a.y + dp(4)));
}

static void opt_card_end(f32 space_after = 18)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    f32 y = ImGui::GetCursorScreenPos().y + dp(4);
    dl->ChannelsSetCurrent(0);
    dl->AddRectFilled(g_card.a, ImVec2(g_card.a.x + g_card.w, y), col32(COL_PANEL), dp(10));
    dl->ChannelsMerge();
    ImGui::SetCursorScreenPos(ImVec2(g_card.a.x, y));
    ImGui::Dummy(ImVec2(g_card.w, dp(space_after) - ImGui::GetStyle().ItemSpacing.y));
}

// A small caption over a card: "VIDEO", "BUFFER".
static void opt_section(const char *text)
{
    ImGui::PushFont(g_font_small);
    ImGui::PushStyleColor(ImGuiCol_Text, rgb(COL_DIM));
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + dp(4));
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - dp(4));
}

// Rows: begin draws the name (and the dim line) and puts the cursor where a control `control_w` wide ends at the
// right edge, vertically centred; end moves to the next row. Rows after the first get a hairline above them
// (unless `line` is false: the empty rows that keep a paged list's size).
struct OptRow { f32 x0, x1, y, h; };
static OptRow g_row;

static void opt_row_begin(const char *name, const char *sub, f32 control_w, f32 h = 0, bool line = true)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    g_row.x0 = g_card.a.x + dp(OPT_PAD);
    g_row.x1 = g_card.a.x + g_card.w - dp(OPT_PAD);
    g_row.y = ImGui::GetCursorScreenPos().y;
    g_row.h = h > 0 ? h : dp(sub ? 64.f : 54.f);
    if (g_card.rows++ && line) dl->AddLine(ImVec2(g_row.x0, g_row.y), ImVec2(g_row.x1, g_row.y), col32(COL_LINE));
    if (name) {
        f32 max_x = g_row.x1 - control_w - dp(20), nx = g_row.x0;
        f32 nh = g_font->FontSize, sh = g_font_small->FontSize, gap = dp(3);
        f32 ty = floorf(g_row.y + (g_row.h - nh - (sub ? gap + sh : 0)) / 2);
        opt_text_at(dl, g_font, ImVec2(nx, ty), COL_TEXT, name, max_x);
        if (sub) opt_text_at(dl, g_font_small, ImVec2(nx, ty + nh + gap), COL_DIM, sub, max_x);
    }
    ImGui::SetCursorScreenPos(ImVec2(g_row.x1 - control_w, g_row.y + floorf((g_row.h - ImGui::GetFrameHeight()) / 2)));
}

static void opt_row_end() { ImGui::SetCursorScreenPos(ImVec2(g_card.a.x, g_row.y + g_row.h)); }

// Where a list row's text sits: vertically centred in the row.
static f32 opt_row_text_y(ImFont *font) { return floorf(g_row.y + (g_row.h - font->FontSize) / 2); }
static f32 opt_row_frame_y() { return g_row.y + floorf((g_row.h - ImGui::GetFrameHeight()) / 2); }

static f32 opt_number_w() { return dp(OPT_FIELD_W + 8 + OPT_UNIT_W); }

// The unit after the number box just drawn, in the slot kept for it.
static void opt_unit(const char *unit)
{
    ImVec2 a = ImGui::GetItemRectMin(), z = ImGui::GetItemRectMax();
    opt_text_at(ImGui::GetWindowDrawList(), g_font, ImVec2(z.x + dp(8), floorf(a.y + (z.y - a.y - g_font->FontSize) / 2)),
                COL_DIM, unit, z.x + dp(8 + OPT_UNIT_W));
}

static bool opt_row_f32(const char *name, const char *sub, const char *id, f32 *v, f64 lo, f64 hi, const char *unit)
{
    opt_row_begin(name, sub, opt_number_w());
    bool changed = opt_f32(id, v, lo, hi, dp(OPT_FIELD_W));
    opt_unit(unit);
    opt_row_end();
    return changed;
}

static bool opt_row_i32(const char *name, const char *sub, const char *id, i32 *v, f64 lo, f64 hi, const char *unit)
{
    opt_row_begin(name, sub, opt_number_w());
    bool changed = opt_i32(id, v, lo, hi, dp(OPT_FIELD_W));
    opt_unit(unit);
    opt_row_end();
    return changed;
}

static bool opt_row_toggle(const char *name, const char *sub, const char *id, bool *v)
{
    opt_row_begin(name, sub, dp(36));
    bool changed = kit_toggle(id, v);
    opt_row_end();
    return changed;
}

// A row whose right side is a read-only value.
static void opt_row_value(const char *name, const char *sub, const char *value, u32 col = COL_TEXT)
{
    f32 w = fminf(dp(360), g_card.w * 0.5f);
    opt_row_begin(name, sub, w);
    opt_text_right(ImGui::GetWindowDrawList(), g_font, g_row.x1, opt_row_text_y(g_font), col, value, g_row.x1 - w);
    opt_row_end();
}

// Drop-downs: a filled box of fixed width with its own chevron, and a popup that can be wider than the box so each
// item fits its note (a dim word or two on the right).
static bool opt_combo_begin(const char *id, const char *preview, f32 width, f32 popup_w = 0, u32 preview_col = COL_TEXT)
{
    ImDrawList *dl = ImGui::GetWindowDrawList(); // the box's window: once open, the current window is the popup
    ImVec2 a = ImGui::GetCursorScreenPos();
    f32 h = ImGui::GetFrameHeight(), pad = ImGui::GetStyle().FramePadding.x;
    ImGui::SetNextItemWidth(width);
    ImGui::SetNextWindowSizeConstraints(ImVec2(fmaxf(width, popup_w), 0), ImVec2(FLT_MAX, FLT_MAX));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(dp(6), dp(6)));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, rgb(0x202227));
    // Clicked while its list was open: the click closed the list (ui_popups_click_through) and must not open it again
    bool closed_here = kit_closed_here(ImGui::GetID(id));
    if (closed_here) {
        ImGui::PushStyleVar(ImGuiStyleVar_DisabledAlpha, 1);
        ImGui::BeginDisabled();
    }
    bool open = ImGui::BeginCombo(id, "", ImGuiComboFlags_NoArrowButton);
    if (closed_here) {
        ImGui::EndDisabled();
        ImGui::PopStyleVar();
    }
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    f32 chev = text_w(g_font_small, ICON_DOWN);
    opt_text_at(dl, g_font_small, ImVec2(a.x + width - pad - chev, floorf(a.y + (h - g_font_small->FontSize) / 2)),
                COL_DIM, ICON_DOWN, a.x + width);
    opt_text_at(dl, g_font, ImVec2(a.x + pad, floorf(a.y + (h - g_font->FontSize) / 2)), preview_col, preview,
                a.x + width - pad - chev - dp(6));
    if (open) {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(dp(6), dp(2)));
        ImGui::PushStyleColor(ImGuiCol_Header, rgb(0x2e3137));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, rgb(0x34373e));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, rgb(0x34373e));
    }
    return open;
}

static bool opt_combo_item(const char *label, const char *note, bool selected, bool disabled = false)
{
    ImGui::PushID(label);
    bool clicked = ImGui::Selectable("##item", selected, disabled ? ImGuiSelectableFlags_Disabled : 0, ImVec2(0, dp(30)));
    ImGui::PopID();
    ImVec2 a = ImGui::GetItemRectMin(), z = ImGui::GetItemRectMax();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    f32 ty = floorf(a.y + (z.y - a.y - g_font->FontSize) / 2);
    u32 ink = disabled ? COL_FAINT : COL_TEXT;
    opt_text_at(dl, selected ? g_font_bold : g_font, ImVec2(a.x + dp(10), ty), ink, label, z.x - dp(10));
    if (note && note[0]) opt_text_right(dl, g_font, z.x - dp(10), ty, disabled ? COL_FAINT : COL_DIM, note, a.x + dp(10));
    if (selected && ImGui::IsWindowAppearing()) ImGui::SetScrollHereY();
    return clicked && !disabled;
}

static void opt_combo_end()
{
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar();
    ImGui::EndCombo();
}

// The popup width that fits the widest label and note.
static f32 opt_combo_popup_w(const char *const *items, const char *const *notes, i32 count)
{
    f32 lw = 0, nw = 0;
    for (i32 i = 0; i < count; i++) {
        lw = fmaxf(lw, text_w(g_font_bold, items[i]));
        if (notes && notes[i]) nw = fmaxf(nw, text_w(g_font, notes[i]));
    }
    return lw + (nw > 0 ? dp(32) + nw : 0) + dp(20 + 12);
}

// A whole drop-down. Items whose bit is set in `disabled` show greyed out and can't be picked.
// Off for the rows of the Recording page: it scrolls, and a wheel meant for the page would change the monitor or
// the encoder and restart the recording.
static bool g_combo_wheel = true;

static bool opt_combo(const char *id, const char *const *items, const char *const *notes, i32 count, i32 *index,
                      f32 width, u32 disabled = 0, const char *preview = 0, u32 preview_col = COL_TEXT)
{
    const char *shown = preview ? preview : (*index >= 0 && *index < count ? items[*index] : "");
    bool changed = false;
    bool open = opt_combo_begin(id, shown, width, opt_combo_popup_w(items, notes, count), preview_col);
    if (!open && g_combo_wheel && kit_wheel_step(index, count, disabled)) {
        changed = true;
        dev_click(id, items[*index]);
    }
    if (open) {
        for (i32 i = 0; i < count; i++) {
            if (opt_combo_item(items[i], notes ? notes[i] : 0, i == *index, (disabled >> i) & 1) && i != *index) {
                *index = i;
                changed = true;
                dev_click(id, items[i]);
            }
        }
        opt_combo_end();
    }
    return changed;
}

static bool opt_row_combo(const char *name, const char *sub, const char *id, const char *const *items,
                          const char *const *notes, i32 count, i32 *index, f32 width, u32 disabled = 0,
                          const char *preview = 0)
{
    opt_row_begin(name, sub, width);
    g_combo_wheel = false;
    bool changed = opt_combo(id, items, notes, count, index, width, disabled, preview);
    g_combo_wheel = true;
    opt_row_end();
    return changed;
}

// Keys as caps: "Ctrl+Shift+S" draws [Ctrl] [Shift] [S] from p (top left), h high, stopping before max_x.
static void opt_keycaps(ImDrawList *dl, ImVec2 p, f32 h, const char *keys, f32 max_x)
{
    f32 x = p.x;
    for (const char *s = keys; *s;) {
        const char *e = strchr(s, '+');
        i32 n = e ? (i32)(e - s) : (i32)strlen(s);
        char part[32];
        snprintf(part, sizeof part, "%.*s", n < 31 ? n : 31, s);
        f32 tw = text_w(g_font_bold, part), w = fmaxf(h, tw + dp(18));
        if (x + w > max_x) break;
        dl->AddRectFilled(ImVec2(x, p.y + dp(2)), ImVec2(x + w, p.y + h), col32(0x101114), dp(6)); // the cap's edge
        dl->AddRectFilled(ImVec2(x, p.y), ImVec2(x + w, p.y + h - dp(2)), col32(0x2e3137), dp(6));
        opt_text_at(dl, g_font_bold, ImVec2(floorf(x + (w - tw) / 2), floorf(p.y + (h - dp(2) - g_font_bold->FontSize) / 2)),
                    COL_TEXT, part, x + w);
        x += w + dp(6);
        s = e ? e + 1 : s + n;
    }
}

static f32 opt_pager_h() { return ImGui::GetFrameHeight() + dp(14); }

// How many rows rh high fit in what's left of the page, leaving `below` for what follows (the pager).
static i32 opt_rows_fit(f32 rh, f32 below)
{
    f32 h = ImGui::GetContentRegionAvail().y - below - dp(8); // the card's own padding
    i32 rows = (i32)(h / rh);
    return rows < 3 ? 3 : rows > 30 ? 30 : rows;
}

// A footer line: `left` dim on the left, and  ‹  2 / 5  ›  on the right, all fixed sizes. The mouse wheel over the
// page turns pages too.
static void opt_pager(const char *id, i32 *page, i32 total, i32 per_page, const char *left, bool *chip = 0,
                      const char *chip_label = 0, i32 chip_count = 0)
{
    i32 pages = total > 0 ? (total + per_page - 1) / per_page : 1;
    if (ImGui::IsWindowHovered() && !ImGui::IsAnyItemActive()) {
        f32 wheel = ImGui::GetIO().MouseWheel;
        if (wheel < 0) (*page)++;
        else if (wheel > 0) (*page)--;
    }
    if (*page >= pages) *page = pages - 1;
    if (*page < 0) *page = 0;
    ImGui::PushID(id);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + dp(4));
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + dp(4));
    ImGui::AlignTextToFramePadding();
    kit_dim("%s", left);
    if (chip) { // a switch after the count, a fixed width so nothing moves when it's turned
        ImGui::SameLine(dp(150));
        char n[16], cid[64];
        snprintf(n, sizeof n, "%d", chip_count);
        snprintf(cid, sizeof cid, "%s##pagerchip", chip_label);
        f32 w = ImGui::CalcTextSize(chip_label).x + ImGui::CalcTextSize("000").x + dp(36);
        if (kit_chip(cid, *chip, n, w)) *chip = !*chip;
    }
    f32 bh = ImGui::GetFrameHeight(), slot = dp(64);
    kit_align_right(2 * bh + slot);
    if (kit_icon_button(ICON_LEFT, "##prev", KIT_GHOST, *page > 0)) (*page)--;
    ImGui::SameLine(0, 0);
    char text[32];
    snprintf(text, sizeof text, "%d / %d", *page + 1, pages);
    ImVec2 a = ImGui::GetCursorScreenPos();
    f32 tw = ImGui::CalcTextSize(text).x;
    opt_text_at(ImGui::GetWindowDrawList(), g_font, ImVec2(floorf(a.x + (slot - tw) / 2), floorf(a.y + (bh - g_font->FontSize) / 2)),
                COL_DIM, text, a.x + slot);
    ImGui::Dummy(ImVec2(slot, bh));
    ImGui::SameLine(0, 0);
    if (kit_icon_button(ICON_RIGHT, "##next", KIT_GHOST, *page < pages - 1)) (*page)++;
    ImGui::PopID();
}

// A list row's column caption (inside the card's first row).
static void opt_caption(f32 x, const char *text, bool right = false)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    f32 y = opt_row_text_y(g_font_small);
    if (right) opt_text_right(dl, g_font_small, x, y, COL_FAINT, text, x - dp(200));
    else opt_text_at(dl, g_font_small, ImVec2(x, y), COL_FAINT, text, x + dp(200));
}

// A list row's hover: a soft fill across the card. Returns whether the mouse is on the row.
static bool opt_row_hover()
{
    ImVec2 a(g_card.a.x + dp(6), g_row.y + 1), z(g_card.a.x + g_card.w - dp(6), g_row.y + g_row.h);
    bool hot = ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(a, z);
    if (hot) ImGui::GetWindowDrawList()->AddRectFilled(a, z, col32(0x1e2024), dp(6));
    return hot;
}

static void opt_empty_row(const char *text)
{
    opt_text_at(ImGui::GetWindowDrawList(), g_font, ImVec2(g_row.x0, opt_row_text_y(g_font)), COL_DIM, text, g_row.x1);
}

static void bytes_text(i64 bytes, char *out, i32 cap)
{
    if (bytes < 0) snprintf(out, cap, "unknown");
    else if (bytes >= 1000000000000LL) snprintf(out, cap, "%.2f TB", bytes / 1e12);
    else if (bytes >= 1000000000LL) snprintf(out, cap, "%.1f GB", bytes / 1e9);
    else snprintf(out, cap, "%.0f MB", bytes / 1e6);
}

static void ago_text(i64 us, char *out, i32 cap)
{
    if (!us) { snprintf(out, cap, "\xE2\x80\x94"); return; } // —: not since clipback started
    i64 s = (now_us() - us) / 1000000;
    if (s < 90) snprintf(out, cap, "just now");
    else if (s < 90 * 60) snprintf(out, cap, "%lld min ago", s / 60);
    else if (s < 36 * 3600) snprintf(out, cap, "%lld h ago", s / 3600);
    else snprintf(out, cap, "%lld days ago", s / 86400);
}

// ---------------------------------------------------------------------------------------------------------------
// Apps

// Each category has a colour and a short name, used for its button on every app row and its filter chip.
static const char *CATEGORY_LABELS[] = {"New", "App", "Game", "Chat", "Excluded"};
static const u32 CATEGORY_COLOURS[] = {COL_WARN, 0x5b9cff, 0x3fbf74, 0xb07cff, 0x8e939c};

// The name an app gets when it has none of its own: "Overwatch" for overwatch.exe (as audio_display_name).
static void default_app_name(const char *exe, char *out, i32 cap)
{
    snprintf(out, cap, "%s", exe);
    size_t n = strlen(out);
    if (n > 4 && !strcmp(out + n - 4, ".exe")) out[n - 4] = 0;
    out[0] = (char)toupper((u8)out[0]);
}

// Filters go by what an app ends up as, so an app sorted by its folder's rule is under that category, not New.
// An app with no category of its own that a folder rule sorts: one per person in a VoiceStreams call.
static bool app_folder_only(const AppInfo &a)
{
    return a.category == CAT_NONE && app_category(g_app.config, a) != CAT_NONE;
}

static bool app_matches(const AppInfo &a, i32 filter, const char *search)
{
    if (!g_app.options.show_folder_apps && app_folder_only(a)) return false;
    AppCategory cat = app_category(g_app.config, a);
    switch (filter) {
    case FILTER_NEW: if (cat != CAT_NONE) return false; break;
    case FILTER_GAMES: if (cat != CAT_GAME) return false; break;
    case FILTER_COMMUNICATION: if (cat != CAT_COMMUNICATION) return false; break;
    case FILTER_APPS: if (cat != CAT_APP) return false; break;
    case FILTER_IGNORED: if (cat != CAT_IGNORED) return false; break;
    default: break;
    }
    char needle[64];
    str_set(needle, search);
    char *n = trim(needle);
    if (!n[0]) return true;
    for (char *p = n; *p; p++) *p = (char)tolower((u8)*p);
    char hay[256];
    snprintf(hay, sizeof hay, "%s %s %s", a.exe.s, a.name.s, a.desc.s);
    for (char *p = hay; *p; p++) *p = (char)tolower((u8)*p);
    return strstr(hay, n) != 0;
}

// New apps first (sorting them is like clearing an inbox), then the most recently heard, then by name.
static int app_order_cmp(const void *pa, const void *pb)
{
    const AppInfo &a = g_app.config.apps[*(const i16 *)pa], &b = g_app.config.apps[*(const i16 *)pb];
    bool na = app_category(g_app.config, a) == CAT_NONE, nb = app_category(g_app.config, b) == CAT_NONE;
    if (na != nb) return na ? -1 : 1;
    if (a.last_heard_us != b.last_heard_us) return a.last_heard_us > b.last_heard_us ? -1 : 1;
    return strcmp(a.exe.s, b.exe.s);
}

static void apps_order_make()
{
    OptionsUi &o = g_app.options;
    const Config &c = g_app.config;
    o.order_count = 0;
    for (i32 i = 0; i < c.app_count; i++)
        if (app_matches(c.apps[i], o.filter, o.search)) o.order[o.order_count++] = (i16)i;
    qsort(o.order, o.order_count, sizeof o.order[0], app_order_cmp);
    o.order_app_count = c.app_count;
    o.order_dirty = false;
}

// Apps heard (or added) since the order was made go on the end, so nothing already shown moves.
static void apps_order_extend()
{
    OptionsUi &o = g_app.options;
    const Config &c = g_app.config;
    for (i32 i = o.order_app_count; i < c.app_count; i++)
        if (app_matches(c.apps[i], o.filter, o.search) && o.order_count < COUNT(o.order)) o.order[o.order_count++] = (i16)i;
    o.order_app_count = c.app_count;
}

static bool app_playing(const char *exe)
{
    for (i32 i = 0; i < g_app.audio.stream_count; i++)
        if (str_eq(g_app.audio.streams[i].exe.s, exe)) return true;
    return false;
}

static void app_set_category(AppInfo *a, AppCategory cat)
{
    if (a->category == cat) return;
    log_line("options: %s is now %s", a->exe.s, category_name(cat));
    a->category = cat;
    options_apply(APPLY_AUDIO | (cat == CAT_GAME || a->rec.bitrate_mbps ? APPLY_RECORDER : 0));
}

// Forgets an app: its category, name and settings go, and it comes back as new the next time it plays.
static void app_dismiss(i32 idx)
{
    Config &c = g_app.config;
    if (idx < 0 || idx >= c.app_count) return;
    log_line("options: %s dismissed", c.apps[idx].exe.s);
    memmove(&c.apps[idx], &c.apps[idx + 1], (c.app_count - idx - 1) * sizeof c.apps[0]);
    c.app_count--;
    OptionsUi &o = g_app.options;
    o.menu_app = -1;
    o.order_dirty = true;
    options_apply(APPLY_AUDIO | APPLY_RECORDER);
}

// The folder an exe path is in ("" for none).
static void path_folder(const char *path, char *out, i32 cap)
{
    snprintf(out, cap, "%s", path);
    char *s = strrchr(out, '\\'), *f = strrchr(out, '/');
    if (f > s) s = f;
    if (s) *s = 0;
    else out[0] = 0;
}

static i32 folder_find(const char *path)
{
    const Config &c = g_app.config;
    for (i32 i = 0; i < c.folder_count; i++)
        if (!_stricmp(c.folders[i].path, path)) return i;
    return -1;
}

// Adds a rule (or changes the one for that folder). Returns its index, -1 if the list is full.
static i32 folder_set(const char *path, AppCategory cat)
{
    Config &c = g_app.config;
    i32 i = folder_find(path);
    if (i < 0) {
        if (c.folder_count == COUNT(c.folders)) return -1;
        i = c.folder_count++;
        str_set(c.folders[i].path, path);
    }
    c.folders[i].category = cat;
    log_line("options: folder %s is %s", path, category_name(cat));
    g_app.options.order_dirty = true;
    options_apply(APPLY_AUDIO | APPLY_RECORDER);
    return i;
}

static void folder_remove(i32 i)
{
    Config &c = g_app.config;
    if (i < 0 || i >= c.folder_count) return;
    log_line("options: folder rule %s removed", c.folders[i].path);
    memmove(&c.folders[i], &c.folders[i + 1], (c.folder_count - i - 1) * sizeof c.folders[0]);
    c.folder_count--;
    g_app.options.order_dirty = true;
    options_apply(APPLY_AUDIO | APPLY_RECORDER);
}

// How many known apps a rule decides (ones with their own category don't count).
static i32 folder_app_count(i32 rule)
{
    const Config &c = g_app.config;
    i32 n = 0;
    for (i32 i = 0; i < c.app_count; i++)
        n += c.apps[i].category == CAT_NONE && folder_rule_for(c.folders, c.folder_count, c.apps[i].path) == rule;
    return n;
}

// ---------------------------------------------------------------------------------------------------------------
// The icon in a square at a, or a tile with the first letter in the category's colour.
static void app_icon_draw(ImDrawList *dl, ImVec2 a, f32 size, const AppInfo &info, const char *name)
{
    // A game shows its art from the games database; anything else (and a game without any) its exe's icon
    ID3D11ShaderResourceView *srv = app_category(g_app.config, info) == CAT_GAME ? game_db_icon(name) : 0;
    if (!srv) srv = app_icon(info.exe.s);
    if (srv) {
        dl->AddImage((ImTextureID)srv, a, ImVec2(a.x + size, a.y + size));
        return;
    }
    u32 col = CATEGORY_COLOURS[info.category];
    dl->AddRectFilled(a, ImVec2(a.x + size, a.y + size), col32(col, 0.18f), dp(8));
    char letter[2] = {(char)toupper((u8)name[0]), 0};
    ImGui::PushFont(g_font_big);
    ImVec2 ls = ImGui::CalcTextSize(letter);
    dl->AddText(ImVec2(floorf(a.x + (size - ls.x) / 2), floorf(a.y + (size - ls.y) / 2)), col32(col), letter);
    ImGui::PopFont();
}

// ---------------------------------------------------------------------------------------------------------------
// A game's own recording settings (Recording settings in its row's menu). Empty fields use Recording's.

static const char *PRESET_LABELS[] = {"P1", "P2", "P3", "P4", "P5", "P6", "P7"};
static const char *PRESET_NOTES[] = {"fastest", 0, 0, "balanced", 0, 0, "best quality, more GPU"};
static const char *PRESET_KEYS[] = {"p1", "p2", "p3", "p4", "p5", "p6", "p7"};
static const char *RC_LABELS[] = {"VBR", "CBR", "Constant quality"};
static const char *RC_NOTES[] = {"peaks allowed", "fixed bitrate", "the bitrate is a cap"};
static const char *RC_KEYS[] = {"vbr", "cbr", "cq"};
static const char *SESSION_LABELS[] = {"Ask when it ends", "Always save", "Never"};

static i32 rc_index(const char *rc) { return !strcmp(rc, "cbr") ? 1 : !strcmp(rc, "cq") ? 2 : 0; }

// A line of the game popup: the name on the left, the cursor at x for the control.
static void game_line(const char *name, f32 x)
{
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(name);
    ImGui::SameLine(x);
}

// Resolutions offered, as output heights. The width follows the monitor's shape.
static const i32 RES_HEIGHTS[] = {2160, 1440, 1080, 720};
static const char *RES_LABELS[] = {"2160p", "1440p", "1080p", "720p"};

// The recorded monitor's height, 0 if it isn't known.
static i32 rec_monitor_h()
{
    const OptionsUi &o = g_app.options;
    i32 i = g_app.config.output_idx;
    return i >= 0 && i < o.monitor_count ? o.monitors[i].height : 0;
}

// "Native", or a height ("1080p"); one above the monitor says what it records instead.
static void res_label(i32 height, char *out, i32 cap)
{
    i32 mh = rec_monitor_h();
    if (height <= 0) snprintf(out, cap, "Native");
    else if (mh && height >= mh) snprintf(out, cap, "%dp \xC2\xB7 records %dp", height, mh);
    else snprintf(out, cap, "%dp", height);
}

// The items after `first` ones: Native, then each height. notes[] says when one is the monitor's own.
static void res_items(const char **items, const char **notes, char (*buf)[48], i32 first)
{
    i32 mh = rec_monitor_h();
    static char native_note[32];
    snprintf(native_note, sizeof native_note, mh ? "%dp" : "", mh);
    items[first] = "Native";
    notes[first] = mh ? native_note : 0;
    for (i32 i = 0; i < COUNT(RES_HEIGHTS); i++) {
        items[first + 1 + i] = RES_LABELS[i];
        notes[first + 1 + i] = 0;
        if (mh && RES_HEIGHTS[i] > mh) {
            snprintf(buf[i], 48, "records %dp", mh);
            notes[first + 1 + i] = buf[i];
        }
    }
}

static i32 res_index(i32 height)
{
    for (i32 i = 0; i < COUNT(RES_HEIGHTS); i++)
        if (RES_HEIGHTS[i] == height) return i + 1;
    return 0;
}

static void game_settings_popup()
{
    OptionsUi &o = g_app.options;
    Config &c = g_app.config;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(dp(20), dp(18)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(dp(8), dp(10)));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, rgb(0x202227));
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    bool open = ImGui::BeginPopup("game settings");
    ImGui::PopStyleColor();
    if (!open) {
        ImGui::PopStyleVar(2);
        return;
    }
    if (o.menu_app < 0 || o.menu_app >= c.app_count) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        ImGui::PopStyleVar(2);
        return;
    }
    AppInfo &a = c.apps[o.menu_app];
    RecOverride &r = a.rec;
    char name[64];
    audio_display_name(c, a.exe.s, name, sizeof name);

    // Its icon and name
    f32 is = dp(36);
    ImVec2 p = ImGui::GetCursorScreenPos();
    app_icon_draw(ImGui::GetWindowDrawList(), p, is, a, name);
    ImGui::SetCursorScreenPos(ImVec2(p.x + is + dp(12), p.y + (is - g_font_big->FontSize - g_font_small->FontSize - dp(2)) / 2));
    ImGui::PushFont(g_font_big);
    ImGui::TextUnformatted(name);
    ImGui::PopFont();
    ImGui::SetCursorScreenPos(ImVec2(p.x + is + dp(12), ImGui::GetCursorScreenPos().y - dp(8)));
    ImGui::PushFont(g_font_small);
    kit_dim("%s", a.exe.s);
    ImGui::PopFont();
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + is + dp(14)));

    f32 x = dp(150), cw = dp(220);
    bool rec_changed = false;
    char hint[48];

    ImGui::PushStyleColor(ImGuiCol_FrameBg, rgb(0x2a2c32));
    game_line("Bitrate", x);
    f64 v = r.bitrate_mbps;
    snprintf(hint, sizeof hint, "%g", c.bitrate_mbps);
    if (opt_number("##rate", &v, 1, 300, dp(OPT_FIELD_W), true, hint)) {
        r.bitrate_mbps = (f32)v;
        rec_changed = true;
    }
    opt_unit("Mbps");

    game_line("Frame rate", x);
    v = r.fps;
    snprintf(hint, sizeof hint, "%d", c.fps);
    if (opt_number("##fps", &v, 1, 240, dp(OPT_FIELD_W), true, hint)) {
        r.fps = (i32)floor(v + 0.5);
        rec_changed = true;
    }
    opt_unit("fps");

    // Drop-downs: "Default" first, which shows what the default is
    const char *items[8], *notes[8];
    char def[48];
    game_line("Resolution", x);
    {
        char rl[48], rbuf[COUNT(RES_HEIGHTS)][48];
        res_label(c.height, rl, sizeof rl);
        snprintf(def, sizeof def, "Default \xC2\xB7 %s", rl);
        items[0] = def;
        notes[0] = 0;
        res_items(items, notes, rbuf, 1);
        i32 rsel = r.height < 0 ? 1 : r.height ? res_index(r.height) + 1 : 0;
        if (r.height > 0 && rsel == 1) rsel = 0; // a height not offered: Default
        if (opt_combo("##res", items, notes, 2 + COUNT(RES_HEIGHTS), &rsel, cw)) {
            r.height = rsel == 0 ? 0 : rsel == 1 ? -1 : RES_HEIGHTS[rsel - 2];
            rec_changed = true;
        }
    }

    game_line("Preset", x);
    snprintf(def, sizeof def, "Default \xC2\xB7 %s", PRESET_LABELS[preset_step(c.preset.s)]);
    items[0] = def;
    notes[0] = 0;
    for (i32 i = 0; i < COUNT(PRESET_LABELS); i++) {
        items[i + 1] = PRESET_LABELS[i];
        notes[i + 1] = PRESET_NOTES[i];
    }
    i32 sel = r.preset.s[0] ? preset_step(r.preset.s) + 1 : 0;
    if (opt_combo("##preset", items, notes, COUNT(PRESET_LABELS) + 1, &sel, cw)) {
        str_set(r.preset.s, sel ? PRESET_KEYS[sel - 1] : "");
        rec_changed = true;
    }

    game_line("Rate control", x);
    snprintf(def, sizeof def, "Default \xC2\xB7 %s", RC_LABELS[rc_index(c.rate_control.s)]);
    items[0] = def;
    for (i32 i = 0; i < COUNT(RC_LABELS); i++) {
        items[i + 1] = RC_LABELS[i];
        notes[i + 1] = RC_NOTES[i];
    }
    sel = r.rate_control.s[0] ? rc_index(r.rate_control.s) + 1 : 0;
    if (opt_combo("##rc", items, notes, COUNT(RC_LABELS) + 1, &sel, cw)) {
        str_set(r.rate_control.s, sel ? RC_KEYS[sel - 1] : "");
        rec_changed = true;
    }

    bool cq_on = !strcmp(r.rate_control.s[0] ? r.rate_control.s : c.rate_control.s, "cq");
    ImGui::BeginDisabled(!cq_on);
    game_line("Quality level", x);
    v = r.cq;
    snprintf(hint, sizeof hint, "%d", c.cq);
    if (opt_number("##cq", &v, 1, 51, dp(OPT_FIELD_W), true, hint)) {
        r.cq = (i32)floor(v + 0.5);
        rec_changed = true;
    }
    opt_unit("0\xE2\x80\x93" "51");
    ImGui::EndDisabled();

    game_line("Sessions", x);
    i32 m = a.session_mode;
    if (opt_combo("##mode", SESSION_LABELS, 0, COUNT(SESSION_LABELS), &m, cw)) {
        a.session_mode = (SessionMode)m;
        log_line("options: %s sessions: %s", a.exe.s, session_mode_name(a.session_mode));
        options_apply(0);
    }
    ImGui::PopStyleColor();

    if (rec_changed) {
        log_line("options: %s records at %g Mbps, %d fps, height %d, preset %s, %s, quality %d (0 or empty = the default)",
                 a.exe.s, r.bitrate_mbps, r.fps, r.height, r.preset.s, r.rate_control.s, r.cq);
        options_apply(APPLY_RECORDER);
    }

    ImGui::Dummy(ImVec2(0, dp(2)));
    ImGui::PushFont(g_font_small);
    kit_dim("Empty uses Recording's. The recording restarts when this game starts.");
    ImGui::PopFont();
    f32 bw = dp(84);
    bool custom = r.bitrate_mbps || r.fps || r.height || r.preset.s[0] || r.rate_control.s[0] || r.cq;
    ImGui::SetCursorPosX(x + cw - 2 * bw - dp(8));
    if (kit_button("Reset", KIT_GHOST, ImVec2(bw, 0), custom)) {
        r = {};
        log_line("options: %s records with the default settings", a.exe.s);
        options_apply(APPLY_RECORDER);
    }
    ImGui::SameLine(0, dp(8));
    if (kit_button("Done", KIT_PRIMARY, ImVec2(bw, 0))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    ImGui::PopStyleVar(2);
}

// What a game records with, if it differs: "60 Mbps · 120 fps".
static void game_summary(const AppInfo &a, char *out, i32 cap)
{
    const RecOverride &r = a.rec;
    out[0] = 0;
    i32 n = 0;
    auto add = [&](const char *t) { n += snprintf(out + n, cap - n, "%s%s", n ? " \xC2\xB7 " : "", t); };
    char t[32];
    if (r.bitrate_mbps) { snprintf(t, sizeof t, "%g Mbps", r.bitrate_mbps); add(t); }
    if (r.height) { if (r.height < 0) add("native"); else { snprintf(t, sizeof t, "%dp", r.height); add(t); } }
    if (r.fps) { snprintf(t, sizeof t, "%d fps", r.fps); add(t); }
    if (r.preset.s[0]) add(PRESET_LABELS[preset_step(r.preset.s)]);
    if (r.rate_control.s[0]) add(RC_LABELS[rc_index(r.rate_control.s)]);
    if (a.session_mode == SESSION_ALWAYS) add("sessions saved");
    else if (a.session_mode == SESSION_NEVER) add("no sessions");
}

// ---------------------------------------------------------------------------------------------------------------
// Apps

// "Add app": a small popup under its button with the exe name and its category.
static void apps_add_popup(ImVec2 at, i32 rows)
{
    OptionsUi &o = g_app.options;
    Config &c = g_app.config;
    static const char *add_cats[] = {"Game", "App", "Chat", "Excluded"};
    static const AppCategory add_cat_values[] = {CAT_GAME, CAT_APP, CAT_COMMUNICATION, CAT_IGNORED};
    ImGui::SetNextWindowPos(at, ImGuiCond_Appearing, ImVec2(1, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(dp(18), dp(16)));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, rgb(0x202227));
    bool open = ImGui::BeginPopup("add app");
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    if (!open) return;
    f32 w = dp(300);
    ImGui::PushFont(g_font_bold);
    ImGui::TextUnformatted("Add an app");
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, dp(2)));
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::PushStyleColor(ImGuiCol_FrameBg, rgb(0x2a2c32));
    ImGui::SetNextItemWidth(w);
    bool enter = ImGui::InputTextWithHint("##add", "name.exe", o.add_exe, sizeof o.add_exe, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::PopStyleColor();
    if (o.add_category < 0 || o.add_category >= COUNT(add_cats)) o.add_category = 0;
    kit_segmented("##addcat", add_cats, COUNT(add_cats), &o.add_category, (w - 3 * dp(4)) / 4);
    ImGui::Dummy(ImVec2(0, dp(2)));

    char exe[64];
    str_set(exe, o.add_exe);
    char *ex = trim(exe);
    for (char *p = ex; *p; p++) *p = (char)tolower((u8)*p);
    f32 bw = dp(84), gap = dp(8);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + w - 2 * bw - gap);
    if (kit_button("Cancel", KIT_GHOST, ImVec2(bw, 0))) ImGui::CloseCurrentPopup();
    ImGui::SameLine(0, gap);
    bool add = kit_button("Add", KIT_PRIMARY, ImVec2(bw, 0), ex[0] && c.app_count < COUNT(c.apps));
    if ((add || enter) && ex[0] && c.app_count < COUNT(c.apps)) {
        char name[64];
        size_t n = strlen(ex);
        snprintf(name, sizeof name, "%s%s", ex, n < 4 || strcmp(ex + n - 4, ".exe") ? ".exe" : "");
        AppInfo *a = config_app(&c, name);
        if (a) {
            app_set_category(a, add_cat_values[o.add_category]);
            o.add_exe[0] = 0;
            apps_order_extend();
            for (i32 i = 0; i < o.order_count; i++) // turn to its page
                if (&c.apps[o.order[i]] == a) o.apps_page = i / rows;
        }
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// Renaming an app (the right-click menu): the name it gets in files and on tracks.
static void apps_rename_popup()
{
    OptionsUi &o = g_app.options;
    Config &c = g_app.config;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(dp(18), dp(16)));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, rgb(0x202227));
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    bool open = ImGui::BeginPopup("rename app");
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    if (!open) return;
    if (o.menu_app < 0 || o.menu_app >= c.app_count) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    AppInfo &a = c.apps[o.menu_app];
    f32 w = dp(300);
    ImGui::PushFont(g_font_bold);
    ImGui::Text("Name for %s", a.exe.s);
    ImGui::PopFont();
    char hint[64];
    if (a.desc.s[0]) str_set(hint, a.desc.s); // what it shows as with no name: its File description
    else default_app_name(a.exe.s, hint, sizeof hint);
    if (ImGui::IsWindowAppearing()) {
        str_set(o.edit, a.name.s);
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::PushStyleColor(ImGuiCol_FrameBg, rgb(0x2a2c32));
    ImGui::SetNextItemWidth(w);
    bool enter = ImGui::InputTextWithHint("##name", hint, o.edit, 64, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::PopStyleColor();

    // Games in the list that match what is typed: picking one names it that. Five rows, always, so the buttons
    // below stay where they are.
    static GameInfo found[5];
    i32 n = o.edit[0] ? gamedb_search(o.edit, found, COUNT(found)) : 0;
    bool picked = false;
    f32 rh = dp(30), is = dp(20);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 l0 = ImGui::GetCursorScreenPos();
    if (!n) {
        ImGui::PushFont(g_font_small);
        dl->AddText(ImVec2(l0.x + dp(2), l0.y + dp(8)), col32(COL_FAINT), o.edit[0] ? "No game in the list has that name; it is kept as typed"
                                                                                  : "Type a name, or pick the game it is");
        ImGui::PopFont();
    }
    for (i32 i = 0; i < n; i++) {
        ImVec2 r0(l0.x, l0.y + i * rh);
        ImGui::SetCursorScreenPos(r0);
        ImGui::PushID(i);
        if (ImGui::Selectable("##pick", false, 0, ImVec2(w, rh - dp(2)))) {
            snprintf(o.edit, sizeof o.edit, "%s", found[i].name);
            picked = true;
        }
        ImGui::PopID();
        ImVec2 i0(r0.x + dp(6), floorf(r0.y + (rh - dp(2) - is) / 2));
        ID3D11ShaderResourceView *icon = game_db_icon(found[i].name);
        if (icon) dl->AddImageRounded((ImTextureID)icon, i0, ImVec2(i0.x + is, i0.y + is), ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, dp(4));
        else dl->AddRectFilled(i0, ImVec2(i0.x + is, i0.y + is), col32(COL_HOVER), dp(4));
        opt_text_at(dl, g_font, ImVec2(i0.x + is + dp(10), floorf(r0.y + (rh - dp(2) - g_font->FontSize) / 2)), COL_TEXT, found[i].name, r0.x + w - dp(6));
    }
    ImGui::SetCursorScreenPos(ImVec2(l0.x, l0.y + COUNT(found) * rh + dp(6)));

    f32 bw = dp(84), gap = dp(8);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + w - 2 * bw - gap);
    if (kit_button("Cancel", KIT_GHOST, ImVec2(bw, 0))) ImGui::CloseCurrentPopup();
    ImGui::SameLine(0, gap);
    if (kit_button("Save", KIT_PRIMARY, ImVec2(bw, 0)) || enter || picked) {
        char name[64];
        str_set(name, o.edit);
        for (char *p = name; *p; p++) if (*p == '|') *p = ' '; // | separates fields in settings.txt
        str_set(a.name.s, trim(name));
        log_line("options: %s is shown as \"%s\"", a.exe.s, a.name.s[0] ? a.name.s : hint);
        sessions_game_renamed(a.exe.s);
        options_apply(APPLY_AUDIO);
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// The controls on an app row: a switch that includes or excludes it, then the three categories an included app
// can have, the app's own one filled in its colour. All of it keeps its place whatever is chosen.
static const AppCategory ROW_CATS[] = {CAT_GAME, CAT_APP, CAT_COMMUNICATION};
static f32 cat_pill_w() { return dp(84); }
static f32 cat_pills_w() { return kit_px(36) + dp(18) + COUNT(ROW_CATS) * cat_pill_w() + (COUNT(ROW_CATS) - 1) * dp(4); }

// `own` is filled; `inherited` (a folder's rule, when there's no own one) is only outlined. Switching it back on
// makes it an App; picking a category of an excluded one includes it too.
static bool cat_pills_draw(AppCategory own, AppCategory inherited, f32 x, f32 y, AppCategory *picked)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    f32 w = cat_pill_w(), h = ImGui::GetFrameHeight(), gap = dp(4);
    bool changed = false;
    AppCategory effective = own != CAT_NONE ? own : inherited;
    bool included = effective != CAT_IGNORED;
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    bool inc = included;
    if (kit_toggle("##include", &inc)) {
        *picked = inc ? CAT_APP : CAT_IGNORED;
        changed = true;
    }
    x += kit_px(36) + dp(18);
    for (i32 i = 0; i < COUNT(ROW_CATS); i++) {
        AppCategory cat = ROW_CATS[i];
        ImVec2 p(x + i * (w + gap), y);
        ImGui::SetCursorScreenPos(p);
        ImGui::PushID(i);
        bool clicked = ImGui::InvisibleButton("##cat", ImVec2(w, h));
        ImGui::PopID();
        bool hot = ImGui::IsItemHovered(), on = own == cat, via = !on && own == CAT_NONE && inherited == cat;
        u32 col = CATEGORY_COLOURS[cat];
        if (on) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), col32(col, 0.2f), h / 2);
        else if (hot) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), col32(COL_PANEL2), h / 2);
        if (on || via) dl->AddRect(p, ImVec2(p.x + w, p.y + h), col32(col, on ? 0.7f : 0.45f), h / 2, 0, dp(1));
        const char *label = CATEGORY_LABELS[cat];
        ImFont *font = on ? g_font_bold : g_font;
        f32 tw = text_w(font, label);
        opt_text_at(dl, font, ImVec2(floorf(p.x + (w - tw) / 2), floorf(p.y + (h - font->FontSize) / 2)),
                    on || via ? col : hot ? COL_TEXT : included ? COL_DIM : 0x4d5059, label, p.x + w);
        if (clicked && !on) {
            *picked = cat;
            changed = true;
        }
    }
    return changed;
}

static bool cat_pills(AppInfo *a, f32 x, f32 y)
{
    AppCategory picked;
    if (!cat_pills_draw(a->category, app_category(g_app.config, *a), x, y, &picked)) return false;
    app_set_category(a, picked);
    return true;
}

static bool pick_folder(const Path &start, Path *out);

// Editing a folder rule's path, where a * can stand for one folder of any name (VoiceStreams\*\clips-off).
static void folder_edit_popup()
{
    OptionsUi &o = g_app.options;
    Config &c = g_app.config;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(dp(18), dp(16)));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, rgb(0x202227));
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    bool open = ImGui::BeginPopup("edit folder");
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    if (!open) return;
    if (o.menu_folder < 0 || o.menu_folder >= c.folder_count) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    FolderRule &r = c.folders[o.menu_folder];
    f32 w = dp(520);
    ImGui::PushFont(g_font_bold);
    ImGui::TextUnformatted("Folder");
    ImGui::PopFont();
    if (ImGui::IsWindowAppearing()) {
        str_set(o.edit, r.path);
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::PushStyleColor(ImGuiCol_FrameBg, rgb(0x2a2c32));
    ImGui::SetNextItemWidth(w);
    bool enter = ImGui::InputText("##path", o.edit, sizeof r.path, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::PopStyleColor();
    ImGui::PushFont(g_font_small);
    kit_dim("A * matches any one folder");
    ImGui::PopFont();
    f32 bw = dp(84), gap = dp(8);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + w - 2 * bw - gap);
    if (kit_button("Cancel", KIT_GHOST, ImVec2(bw, 0))) ImGui::CloseCurrentPopup();
    ImGui::SameLine(0, gap);
    char path[MAX_PATH * 2];
    str_set(path, trim(o.edit));
    for (char *p = path; *p; p++) if (*p == '|') *p = ' '; // | separates fields in settings.txt
    i32 other = folder_find(path);
    bool ok = path[0] && (other < 0 || other == o.menu_folder);
    if ((kit_button("Save", KIT_PRIMARY, ImVec2(bw, 0), ok) || enter) && ok) {
        log_line("options: folder rule %s is now %s", r.path, path);
        str_set(r.path, path);
        o.order_dirty = true;
        options_apply(APPLY_AUDIO | APPLY_RECORDER);
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// Opens a rule's folder in Explorer (for one with a *, the part above it).
static void folder_open(const char *path)
{
    char u[MAX_PATH * 2];
    str_set(u, path);
    char *star = strchr(u, '*');
    if (star) *star = 0;
    wchar_t w[MAX_PATH];
    utf8_to_wide(u, w, MAX_PATH);
    ShellExecuteW(0, L"open", w, 0, 0, SW_SHOWNORMAL);
    log_line("showing %s in Explorer", u);
}

// The Folders list: one row per rule, the same size as an app row.
static void folders_rows(i32 rows, f32 rh, i32 *open_popup)
{
    OptionsUi &o = g_app.options;
    Config &c = g_app.config;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    f32 fh = ImGui::GetFrameHeight();
    f32 x0 = g_card.a.x + dp(OPT_PAD), x1 = g_card.a.x + g_card.w - dp(OPT_PAD);
    f32 more_x = x1 - fh, pills_x = more_x - dp(20) - cat_pills_w();
    f32 is = dp(34), text_x = x0 + is + dp(14), text_max = pills_x - dp(16);
    i16 shown[COUNT(c.folders)];
    i32 n = 0;
    for (i32 i = 0; i < c.folder_count; i++) {
        char hay[MAX_PATH * 2], needle[64];
        str_set(hay, c.folders[i].path);
        str_set(needle, o.search);
        char *nd = trim(needle);
        for (char *p = hay; *p; p++) *p = (char)tolower((u8)*p);
        for (char *p = nd; *p; p++) *p = (char)tolower((u8)*p);
        if (!nd[0] || strstr(hay, nd)) shown[n++] = (i16)i;
    }
    for (i32 r = 0; r < rows; r++) {
        i32 k = o.folders_page * rows + r;
        opt_row_begin(0, 0, 0, rh, k < n);
        if (k >= n) {
            if (n == 0 && r == 0) opt_empty_row(o.search[0] ? "No match" : "No folder rules. Add one with Add folder.");
            opt_row_end();
            continue;
        }
        i32 idx = shown[k];
        FolderRule &f = c.folders[idx];
        ImGui::PushID(idx);
        bool hot = opt_row_hover();
        f32 cy = opt_row_frame_y();
        ImVec2 ia(x0, floorf(g_row.y + (rh - is) / 2));
        u32 col = CATEGORY_COLOURS[f.category];
        dl->AddRectFilled(ia, ImVec2(ia.x + is, ia.y + is), col32(col, 0.18f), dp(8));
        ImGui::PushFont(g_font_icon);
        ImVec2 gs = ImGui::CalcTextSize(ICON_FOLDER);
        dl->AddText(ImVec2(floorf(ia.x + (is - gs.x) / 2), floorf(ia.y + (is - gs.y) / 2)), col32(col), ICON_FOLDER);
        ImGui::PopFont();

        // Its last folder name, and under it the whole path and how many apps it sorts
        const char *name = f.path;
        for (const char *p = f.path; *p; p++)
            if ((*p == '\\' || *p == '/') && p[1] && p[1] != '\\' && p[1] != '/') name = p + 1;
        f32 nh = g_font_bold->FontSize, sh = g_font_small->FontSize, ty = floorf(g_row.y + (rh - nh - dp(3) - sh) / 2);
        opt_text_at(dl, g_font_bold, ImVec2(text_x, ty), COL_TEXT, name, text_max);
        char line[MAX_PATH * 2 + 32];
        i32 apps = folder_app_count(idx);
        snprintf(line, sizeof line, "%s \xC2\xB7 %d app%s", f.path, apps, apps == 1 ? "" : "s");
        opt_text_at(dl, g_font_small, ImVec2(text_x, ty + nh + dp(3)), COL_DIM, line, text_max);

        AppCategory picked;
        if (cat_pills_draw(f.category, CAT_NONE, pills_x, cy, &picked)) folder_set(f.path, picked);

        ImGui::SetCursorScreenPos(ImVec2(more_x, cy));
        bool more = kit_icon_button(ICON_MORE, "##more");
        if (more || (hot && ImGui::IsMouseClicked(ImGuiMouseButton_Right))) {
            o.menu_folder = idx;
            *open_popup = 4;
        }
        ImGui::PopID();
        opt_row_end();
    }
    o.folders_shown = n;
}

static void options_apps()
{
    OptionsUi &o = g_app.options;
    Config &c = g_app.config;
    if (o.order_dirty) apps_order_make();
    apps_order_extend();

    // The filter (a button that drops the list, each with its count); search and Add on the right
    static const char *chip_icons[] = {ICON_APPS, ICON_STAR, ICON_GAME, ICON_WINDOW, ICON_CHAT, ICON_MUTE, ICON_FOLDER};
    static const char *chips[] = {"All", "New", "Games", "Apps", "Chat", "Excluded", "Folders"};
    static const i32 chip_filter[] = {FILTER_ALL, FILTER_NEW, FILTER_GAMES, FILTER_APPS, FILTER_COMMUNICATION, FILTER_IGNORED,
                                      FILTER_FOLDERS};
    static const i32 chip_cat[] = {-1, CAT_NONE, CAT_GAME, CAT_APP, CAT_COMMUNICATION, CAT_IGNORED};
    i32 counts[7] = {}, folder_only = 0;
    for (i32 i = 0; i < c.app_count; i++) {
        AppCategory cat = app_category(c, c.apps[i]);
        bool hidden = app_folder_only(c.apps[i]);
        folder_only += hidden;
        if (hidden && !o.show_folder_apps) continue;
        counts[0]++;
        for (i32 k = 1; k < 6; k++) counts[k] += cat == chip_cat[k];
    }
    counts[6] = c.folder_count;
    bool folders = o.filter == FILTER_FOLDERS;
    f32 fh = ImGui::GetFrameHeight();
    i32 cur = 0;
    for (i32 i = 0; i < COUNT(chips); i++)
        if (o.filter == chip_filter[i]) cur = i;
    f32 filter_w = dp(190), search_w = dp(220), add_w = dp(118);
    char filter_label[96];
    snprintf(filter_label, sizeof filter_label, "%s  %s   %d   " ICON_DOWN "##filter", chip_icons[cur], chips[cur], counts[cur]);
    ImVec2 filter_at = ImGui::GetCursorScreenPos();
    if (kit_button(filter_label, KIT_SECONDARY, ImVec2(filter_w, 0))) ImGui::OpenPopup("filter");
    if (kit_wheel_step(&cur, COUNT(chips))) {
        o.filter = chip_filter[cur];
        o.order_dirty = true;
        o.apps_page = 0;
        o.folders_page = 0;
    }
    ImGui::SetNextWindowPos(ImVec2(filter_at.x, filter_at.y + fh + dp(6)));
    lib_popup_style_push(8);
    bool filter_open = ImGui::BeginPopup("filter");
    lib_popup_style_pop();
    if (filter_open) {
        for (i32 i = 0; i < COUNT(chips); i++) {
            char item[64];
            snprintf(item, sizeof item, "%s   %d", chips[i], counts[i]);
            if (lib_menu_item(chip_icons[i], item) && o.filter != chip_filter[i]) {
                o.filter = chip_filter[i];
                o.order_dirty = true;
                o.apps_page = 0;
                o.folders_page = 0;
            }
        }
        ImGui::EndPopup();
    }
    // The search takes what the row has left, between a narrowest and a widest
    ImGui::SetCursorScreenPos(ImVec2(filter_at.x + filter_w + dp(10), filter_at.y));
    f32 room = ImGui::GetContentRegionAvail().x - add_w - dp(10);
    search_w = room < dp(120) ? dp(120) : room > search_w ? search_w : room;
    f32 right_w = search_w + dp(10) + add_w;
    if (ImGui::GetContentRegionAvail().x > right_w) kit_align_right(right_w);
    if (kit_search("##search", o.search, sizeof o.search, "Search", search_w)) {
        o.order_dirty = true;
        o.apps_page = 0;
        o.folders_page = 0;
    }
    ImGui::SameLine(0, dp(10));
    ImVec2 add_at = ImGui::GetCursorScreenPos();
    if (folders) {
        Path start = {}, picked = {};
        if (kit_button(ICON_ADD "  Add folder##add", KIT_SECONDARY, ImVec2(add_w, 0), c.folder_count < COUNT(c.folders)) &&
            pick_folder(start, &picked)) {
            char u[MAX_PATH * 2];
            wide_to_utf8(picked.s, u, sizeof u);
            if (folder_find(u) < 0) folder_set(u, CAT_APP);
        }
    } else if (kit_button(ICON_ADD "  Add app##add", KIT_SECONDARY, ImVec2(add_w, 0), c.app_count < COUNT(c.apps))) {
        str_set(o.add_exe, o.search);
        ImGui::OpenPopup("add app");
    }
    ImGui::Dummy(ImVec2(0, dp(2)));

    f32 rh = dp(58);
    i32 rows = opt_rows_fit(rh, opt_pager_h());
    i32 open_popup = 0; // 1 the menu, 2 a game's settings, 3 rename, 4 a folder's menu: opened outside the rows' ID scopes
    opt_card_begin();
    if (folders) folders_rows(rows, rh, &open_popup);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    f32 x0 = g_card.a.x + dp(OPT_PAD), x1 = g_card.a.x + g_card.w - dp(OPT_PAD);
    f32 more_x = x1 - fh, pills_x = more_x - dp(20) - cat_pills_w();
    f32 is = dp(34), text_x = x0 + is + dp(14), text_max = pills_x - dp(16);

    for (i32 r = 0; r < rows && !folders; r++) {
        i32 k = o.apps_page * rows + r;
        opt_row_begin(0, 0, 0, rh, k < o.order_count);
        if (k >= o.order_count) {
            if (o.order_count == 0 && r == 0) {
                char msg[96];
                if (o.search[0]) snprintf(msg, sizeof msg, "No match. Add %s with Add app.", o.search);
                else snprintf(msg, sizeof msg, "%s", o.filter ? "None here" : "No apps have played sound yet");
                opt_empty_row(msg);
            }
            opt_row_end();
            continue;
        }
        i32 idx = o.order[k];
        AppInfo &a = c.apps[idx];
        ImGui::PushID(idx);
        bool hot = opt_row_hover();
        f32 cy = opt_row_frame_y();

        char name[64];
        audio_display_name(c, a.exe.s, name, sizeof name);
        app_icon_draw(dl, ImVec2(x0, floorf(g_row.y + (rh - is) / 2)), is, a, name);

        // Name, with a "new" tag; under it the exe, when it was heard, and what a game does differently
        AppCategory cat = app_category(c, a);
        bool is_new = cat == CAT_NONE;
        f32 nh = g_font_bold->FontSize, sh = g_font_small->FontSize, ty = floorf(g_row.y + (rh - nh - dp(3) - sh) / 2);
        f32 tag_w = is_new ? opt_pill_w("new") + dp(8) : 0;
        opt_text_at(dl, g_font_bold, ImVec2(text_x, ty), COL_TEXT, name, text_max - tag_w);
        if (is_new) {
            f32 nw = fminf(text_w(g_font_bold, name), text_max - tag_w - text_x);
            opt_pill(dl, ImVec2(text_x + nw + dp(8), ty + nh / 2 + dp(1)), "new", 0x3a2e14, COL_WARN);
        }
        char line[192], when[32], summary[96] = "";
        bool playing = app_playing(a.exe.s);
        if (playing) str_set(when, "playing now");
        else ago_text(a.last_heard_us, when, sizeof when);
        if (cat == CAT_GAME) game_summary(a, summary, sizeof summary);
        snprintf(line, sizeof line, "%s \xC2\xB7 %s%s%s", a.exe.s, when, summary[0] ? " \xC2\xB7 " : "", summary);
        opt_text_at(dl, g_font_small, ImVec2(text_x, ty + nh + dp(3)), playing ? COL_OK : COL_DIM, line, text_max);

        cat_pills(&a, pills_x, cy);

        ImGui::SetCursorScreenPos(ImVec2(more_x, cy));
        bool more = kit_icon_button(ICON_MORE, "##more");
        if (more || (hot && ImGui::IsMouseClicked(ImGuiMouseButton_Right))) {
            o.menu_app = idx;
            app_find_path(a.exe.s, &o.menu_path);
            open_popup = 1;
        }
        ImGui::PopID();
        opt_row_end();
    }
    opt_card_end(10);

    if (open_popup == 1) ImGui::OpenPopup("app menu");
    lib_popup_style_push(8);
    bool menu = ImGui::BeginPopup("app menu");
    lib_popup_style_pop();
    if (menu) {
        AppInfo *a = o.menu_app >= 0 && o.menu_app < c.app_count ? &c.apps[o.menu_app] : 0;
        if (a && app_category(c, *a) == CAT_GAME && lib_menu_item(ICON_SETTINGS, "Recording settings")) open_popup = 2;
        if (a && lib_menu_item(ICON_RENAME, "Name or game")) open_popup = 3;
        ImGui::BeginDisabled(!o.menu_path.s[0]);
        if (lib_menu_item(ICON_FOLDER, "Open file location")) reveal_file(o.menu_path);
        ImGui::EndDisabled();
        if (!o.menu_path.s[0]) {
            ImGui::PushFont(g_font_small);
            kit_dim("  Not running, and lifelog hasn't seen where it is");
            ImGui::PopFont();
        }
        // Its folder: a rule for every app in it (shown under Folders), or back to following the one there is
        char dir[MAX_PATH * 2] = "";
        if (a) path_folder(a->path, dir, sizeof dir);
        i32 rule = a ? folder_rule_for(c.folders, c.folder_count, a->path) : -1;
        if (a && a->category != CAT_NONE && rule >= 0 && lib_menu_item(ICON_FOLDER, "Follow its folder's rule")) {
            app_set_category(a, CAT_NONE);
            o.order_dirty = true;
        }
        if (a && dir[0] && folder_find(dir) < 0 && c.folder_count < COUNT(c.folders) &&
            lib_menu_item(ICON_ADD, "Rule for its folder")) {
            AppCategory cat = app_category(c, *a);
            folder_set(dir, cat != CAT_NONE ? cat : CAT_APP);
            o.filter = FILTER_FOLDERS;
            o.folders_page = 0;
            o.search[0] = 0;
        }
        if (a && lib_menu_item(ICON_CLOSE, "Dismiss")) app_dismiss(o.menu_app);
        ImGui::EndPopup();
    }
    if (open_popup == 4) ImGui::OpenPopup("folder menu");
    lib_popup_style_push(8);
    bool fmenu = ImGui::BeginPopup("folder menu");
    lib_popup_style_pop();
    if (fmenu) {
        bool have = o.menu_folder >= 0 && o.menu_folder < c.folder_count;
        if (have && lib_menu_item(ICON_FOLDER, "Open in Explorer")) folder_open(c.folders[o.menu_folder].path);
        if (have && lib_menu_item(ICON_EDIT, "Edit path")) open_popup = 5;
        if (have && lib_menu_item(ICON_DELETE, "Remove", true)) folder_remove(o.menu_folder);
        ImGui::EndPopup();
    }
    if (open_popup == 5) ImGui::OpenPopup("edit folder");
    folder_edit_popup();
    if (open_popup == 2) ImGui::OpenPopup("game settings");
    if (open_popup == 3) ImGui::OpenPopup("rename app");
    game_settings_popup();
    apps_rename_popup();
    apps_add_popup(ImVec2(add_at.x + add_w, add_at.y + fh + dp(6)), rows);

    char left[48];
    if (folders) {
        snprintf(left, sizeof left, "%d folder%s", o.folders_shown, o.folders_shown == 1 ? "" : "s");
        opt_pager("folders", &o.folders_page, o.folders_shown, rows, left);
    } else {
        snprintf(left, sizeof left, "%d app%s", o.order_count, o.order_count == 1 ? "" : "s");
        bool shown = o.show_folder_apps;
        opt_pager("apps", &o.apps_page, o.order_count, rows, left, &o.show_folder_apps, ICON_FOLDER "  Sorted by a folder", folder_only);
        if (shown != o.show_folder_apps) {
            o.order_dirty = true;
            o.apps_page = 0;
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Hotkeys

static const char *ACTION_LABELS[] = {"Save the last", "Open the window", "Open the timeline", "Pause or resume",
                                      "Pause", "Resume"};
static const char *ACTION_COMMANDS[] = {"save", "open", "timeline", "toggle", "pause", "resume"};
static const char *LENGTH_UNITS[] = {"seconds", "minutes", "hours"};

static i32 action_index(const char *action)
{
    char cmd[64];
    str_set(cmd, action);
    char *sp = strchr(cmd, ' ');
    if (sp) *sp = 0;
    for (i32 i = 0; i < COUNT(ACTION_COMMANDS); i++)
        if (!strcmp(cmd, ACTION_COMMANDS[i])) return i;
    return -1;
}

// "save 90s" → 90 and seconds; "save 2m" → 2 and minutes.
static void save_length(const char *action, f64 *amount, i32 *unit)
{
    const char *arg = strchr(action, ' ');
    *amount = 30;
    *unit = 0;
    if (!arg) return;
    char *end = 0;
    f64 v = strtod(arg + 1, &end);
    if (end == arg + 1 || v <= 0) return;
    *amount = v;
    *unit = *end == 'm' ? 1 : *end == 'h' ? 2 : 0;
}

static void set_save_action(Hotkey *h, f64 amount, i32 unit)
{
    snprintf(h->action.s, sizeof h->action.s, "save %g%c", amount, "smh"[unit]);
}

// The name hotkey_parse knows for an ImGui key, or 0.
static const char *key_name(ImGuiKey k)
{
    static char buf[8];
    if (k >= ImGuiKey_A && k <= ImGuiKey_Z) { snprintf(buf, sizeof buf, "%c", 'A' + (k - ImGuiKey_A)); return buf; }
    if (k >= ImGuiKey_0 && k <= ImGuiKey_9) { snprintf(buf, sizeof buf, "%c", '0' + (k - ImGuiKey_0)); return buf; }
    if (k >= ImGuiKey_F1 && k <= ImGuiKey_F24) { snprintf(buf, sizeof buf, "F%d", 1 + (k - ImGuiKey_F1)); return buf; }
    if (k >= ImGuiKey_Keypad0 && k <= ImGuiKey_Keypad9) { snprintf(buf, sizeof buf, "Num%d", k - ImGuiKey_Keypad0); return buf; }
    switch (k) {
    case ImGuiKey_Space: return "Space";
    case ImGuiKey_Enter: return "Enter";
    case ImGuiKey_Tab: return "Tab";
    case ImGuiKey_Escape: return "Esc";
    case ImGuiKey_Backspace: return "Backspace";
    case ImGuiKey_Insert: return "Insert";
    case ImGuiKey_Delete: return "Delete";
    case ImGuiKey_Home: return "Home";
    case ImGuiKey_End: return "End";
    case ImGuiKey_PageUp: return "PageUp";
    case ImGuiKey_PageDown: return "PageDown";
    case ImGuiKey_UpArrow: return "Up";
    case ImGuiKey_DownArrow: return "Down";
    case ImGuiKey_LeftArrow: return "Left";
    case ImGuiKey_RightArrow: return "Right";
    case ImGuiKey_PrintScreen: return "PrintScreen";
    case ImGuiKey_Pause: return "Pause";
    case ImGuiKey_ScrollLock: return "ScrollLock";
    default: return 0;
    }
}

static void hotkeys_unregister_all()
{
    for (i32 i = 0; i < g_hotkeys_registered; i++) UnregisterHotKey(g_app.msg_hwnd, i + 1);
    g_hotkeys_registered = 0;
}

static void hotkey_remove(i32 i)
{
    Config &c = g_app.config;
    log_line("options: hotkey %s (%s) removed", c.hotkeys[i].keys.s[0] ? c.hotkeys[i].keys.s : "with no keys",
             c.hotkeys[i].action.s);
    memmove(&c.hotkeys[i], &c.hotkeys[i + 1], (c.hotkey_count - i - 1) * sizeof c.hotkeys[0]);
    c.hotkey_count--;
}

static void capture_start(i32 i, bool added)
{
    OptionsUi &o = g_app.options;
    o.capture_slot = i + 1;
    o.capture_added = added;
    // Our own hotkeys would swallow the keys being pressed (RegisterHotKey takes them before the window does).
    if (!is_test_copy()) hotkeys_unregister_all();
    // Nothing keeps the keyboard focus, so Enter or Space set the hotkey rather than press a button.
    ImGui::SetWindowFocus(0);
}

static void capture_end(bool cancelled)
{
    OptionsUi &o = g_app.options;
    i32 i = o.capture_slot - 1;
    o.capture_slot = 0;
    if (cancelled && o.capture_added && i < g_app.config.hotkey_count) hotkey_remove(i);
    options_apply(APPLY_HOTKEYS);
}

// While a hotkey's keys are being set: the first key pressed with whatever modifiers are held.
static void capture_keys()
{
    OptionsUi &o = g_app.options;
    if (!o.capture_slot) return;
    ImGuiIO &io = ImGui::GetIO();
    for (i32 k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; k++) {
        if (!ImGui::IsKeyPressed((ImGuiKey)k, false)) continue;
        const char *name = key_name((ImGuiKey)k);
        if (!name) continue; // modifiers, mouse buttons, keys hotkey_parse doesn't know
        bool any_mod = io.KeyCtrl || io.KeyAlt || io.KeyShift || io.KeySuper;
        if (k == ImGuiKey_Escape && !any_mod) {
            capture_end(true);
            return;
        }
        char keys[64];
        snprintf(keys, sizeof keys, "%s%s%s%s%s", io.KeyCtrl ? "Ctrl+" : "", io.KeyAlt ? "Alt+" : "",
                 io.KeyShift ? "Shift+" : "", io.KeySuper ? "Win+" : "", name);
        u32 mods, vk;
        if (!hotkey_parse(keys, &mods, &vk)) continue;
        Hotkey *h = &g_app.config.hotkeys[o.capture_slot - 1];
        str_set(h->keys.s, keys);
        log_line("options: hotkey %s does %s", h->keys.s, h->action.s);
        capture_end(false);
        return;
    }
}

static void options_hotkeys()
{
    OptionsUi &o = g_app.options;
    Config &c = g_app.config;
    capture_keys();

    f32 fh = ImGui::GetFrameHeight(), rh = dp(54), head_h = dp(34);
    i32 rows = opt_rows_fit(rh, head_h + opt_pager_h() + ImGui::GetFrameHeightWithSpacing() + dp(2));
    f32 add_w = dp(132);
    ImGui::AlignTextToFramePadding();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + dp(4));
    kit_dim("Keys that work anywhere, even in a game");
    kit_align_right(add_w);
    if (kit_button(ICON_ADD "  Add hotkey", KIT_SECONDARY, ImVec2(add_w, 0), c.hotkey_count < COUNT(c.hotkeys) && !o.capture_slot)) {
        Hotkey *h = &c.hotkeys[c.hotkey_count++];
        *h = {};
        str_set(h->action.s, "save 30s");
        o.hotkeys_page = (c.hotkey_count - 1) / rows;
        capture_start(c.hotkey_count - 1, true);
    }
    ImGui::Dummy(ImVec2(0, dp(2)));

    i32 remove = -1;
    opt_card_begin();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    f32 x0 = g_card.a.x + dp(OPT_PAD), x1 = g_card.a.x + g_card.w - dp(OPT_PAD);
    f32 keys_x = x0 - dp(6), keys_w = dp(230);
    f32 act_x = keys_x + keys_w + dp(14), act_w = dp(170);
    f32 amt_x = act_x + act_w + dp(10), amt_w = dp(64);
    f32 unit_x = amt_x + amt_w + dp(6), unit_w = dp(108);
    f32 warn_x = unit_x + unit_w + dp(16), del_x = x1 - fh + dp(6);
    f32 pad = ImGui::GetStyle().FramePadding.x;

    opt_row_begin(0, 0, 0, head_h);
    opt_caption(x0, "KEYS");
    opt_caption(act_x + pad, "ACTION");
    opt_caption(amt_x + pad, "LENGTH");
    opt_row_end();
    for (i32 r = 0; r < rows; r++) {
        i32 i = o.hotkeys_page * rows + r;
        opt_row_begin(0, 0, 0, rh, i < c.hotkey_count);
        if (i >= c.hotkey_count) {
            if (c.hotkey_count == 0 && r == 0) opt_empty_row("No hotkeys");
            opt_row_end();
            continue;
        }
        Hotkey *h = &c.hotkeys[i];
        ImGui::PushID(i);
        bool hot = opt_row_hover();
        f32 cy = opt_row_frame_y();
        bool capturing = o.capture_slot == i + 1;

        // The keys: caps in a fixed slot; click to press new ones ("Press keys…" in the same slot)
        ImGui::SetCursorScreenPos(ImVec2(keys_x, cy));
        ImGui::BeginDisabled(o.capture_slot && !capturing);
        if (ImGui::InvisibleButton("##keys", ImVec2(keys_w, fh))) {
            if (capturing) capture_end(true);
            else capture_start(i, false);
        }
        bool slot_hot = ImGui::IsItemHovered();
        f32 cap_h = dp(26), cap_y = cy + floorf((fh - cap_h) / 2);
        if (capturing) {
            dl->AddRectFilled(ImVec2(keys_x, cy), ImVec2(keys_x + keys_w, cy + fh), col32(COL_ACCENT_SOFT), ImGui::GetStyle().FrameRounding);
            opt_text_at(dl, g_font_bold, ImVec2(x0, floorf(cy + (fh - g_font_bold->FontSize) / 2)), 0xff8a92,
                        "Press keys\xE2\x80\xA6", keys_x + keys_w);
        } else {
            if (slot_hot) dl->AddRectFilled(ImVec2(keys_x, cy), ImVec2(keys_x + keys_w, cy + fh), col32(COL_PANEL2), ImGui::GetStyle().FrameRounding);
            if (h->keys.s[0]) opt_keycaps(dl, ImVec2(x0, cap_y), cap_h, h->keys.s, keys_x + keys_w - dp(6));
            else opt_text_at(dl, g_font, ImVec2(x0, floorf(cy + (fh - g_font->FontSize) / 2)), COL_DIM, "Set keys", keys_x + keys_w);
        }
        ImGui::EndDisabled();

        ImGui::SetCursorScreenPos(ImVec2(act_x, cy));
        i32 act = action_index(h->action.s);
        if (opt_combo("##action", ACTION_LABELS, 0, COUNT(ACTION_LABELS), &act, act_w, 0, act < 0 ? h->action.s : 0)) {
            if (act == 0) str_set(h->action.s, "save 30s");
            else str_set(h->action.s, ACTION_COMMANDS[act]);
            log_line("options: hotkey %s does %s", h->keys.s, h->action.s);
            options_apply(APPLY_HOTKEYS);
        }

        f64 amount = 30;
        i32 unit = 0;
        bool save = act == 0;
        if (save) save_length(h->action.s, &amount, &unit);
        ImGui::BeginDisabled(!save);
        ImGui::SetCursorScreenPos(ImVec2(amt_x, cy));
        if (opt_number("##amount", &amount, 0.1, 100000, amt_w)) {
            set_save_action(h, amount, unit);
            log_line("options: hotkey %s does %s", h->keys.s, h->action.s);
            options_apply(APPLY_HOTKEYS);
        }
        ImGui::SetCursorScreenPos(ImVec2(unit_x, cy));
        if (opt_combo("##unit", LENGTH_UNITS, 0, COUNT(LENGTH_UNITS), &unit, unit_w)) {
            set_save_action(h, amount, unit);
            log_line("options: hotkey %s does %s", h->keys.s, h->action.s);
            options_apply(APPLY_HOTKEYS);
        }
        ImGui::EndDisabled();

        u32 mods, vk;
        bool same = false;
        for (i32 k = 0; k < c.hotkey_count; k++) same |= k != i && h->keys.s[0] && !_stricmp(c.hotkeys[k].keys.s, h->keys.s);
        const char *warn = !capturing && !hotkey_parse(h->keys.s, &mods, &vk) ? "Not a key combination"
                           : same ? "Same keys as another hotkey" : 0;
        if (warn) opt_text_at(dl, g_font, ImVec2(warn_x, opt_row_text_y(g_font)), COL_WARN, warn, del_x - dp(8));

        if (hot && !o.capture_slot) {
            ImGui::SetCursorScreenPos(ImVec2(del_x, cy));
            if (kit_icon_button(ICON_DELETE, "##remove")) remove = i;
        }
        ImGui::PopID();
        opt_row_end();
    }
    opt_card_end(10);
    if (remove >= 0) {
        hotkey_remove(remove);
        options_apply(APPLY_HOTKEYS);
    }
    char left[96];
    if (is_test_copy()) snprintf(left, sizeof left, "Test copy: the real clipback keeps the hotkeys");
    else snprintf(left, sizeof left, "%d of %d", c.hotkey_count, COUNT(c.hotkeys));
    opt_pager("hotkeys", &o.hotkeys_page, c.hotkey_count, rows, left);
}

// ---------------------------------------------------------------------------------------------------------------
// Recording: what's captured, how it's encoded, and how much the buffer keeps.

static const char *VENDOR_LABELS[VENDOR_COUNT] = {"NVIDIA NVENC", "AMD AMF", "Intel Quick Sync", "CPU"};

// The vendor recording now, or the one that would record with these settings; -1 = not known yet.
static i32 options_vendor()
{
    const Recorder &r = g_app.recorder;
    if (r.recording) return r.vendor;
    i32 forced = vendor_parse(g_app.config.encoder.s);
    if (forced >= 0) return forced;
    if (!r.encoders_known) return -1;
    u8 works = r.encoders_ok[codec_index(g_app.config.codec.s)];
    for (i32 v = 0; v < VENDOR_COUNT; v++)
        if (works & (1 << v)) return v;
    return -1;
}

static void monitor_label(i32 i, char *out, i32 cap)
{
    snprintf(out, cap, "%d. %s", i + 1, g_app.options.monitors[i].name);
}

static void monitor_note(i32 i, char *out, i32 cap)
{
    const MonitorInfo &m = g_app.options.monitors[i];
    snprintf(out, cap, "%d\xC3\x97%d, %d Hz%s", m.width, m.height, m.hz, m.primary ? ", main" : "");
}

// The inputs plugged in (listed again every 2 s), then the ones turned on that aren't: each Off, Mono or Stereo.
// One that isn't off records to a track of its own.
static void opt_inputs(Config &c)
{
    static InputDevice live[16];
    static i32 live_count;
    static char default_id[128];
    static u64 listed;
    if (!listed || GetTickCount64() - listed > 2000) {
        live_count = audio_list_inputs(live, COUNT(live), default_id, sizeof default_id);
        listed = GetTickCount64();
    }
    InputDevice rows[COUNT(live) + COUNT(c.inputs)];
    bool plugged[COUNT(rows)] = {};
    i32 n = 0;
    for (i32 i = 0; i < live_count; i++) {
        plugged[n] = true;
        rows[n++] = live[i];
    }
    for (i32 i = 0; i < c.input_count; i++) {
        bool found = false;
        for (i32 k = 0; k < live_count && !found; k++) found = !_stricmp(live[k].id, c.inputs[i].id);
        if (!found && c.inputs[i].mode != INPUT_OFF) rows[n++] = c.inputs[i];
    }
    static const char *const modes[] = {"Off", "Mono", "Stereo"};
    for (i32 i = 0; i < n; i++) {
        const InputDevice *set = config_input(c.inputs, c.input_count, rows[i].id);
        i32 mode = set ? set->mode : INPUT_OFF;
        const char *sub = !plugged[i]                          ? "Not plugged in"
                          : _stricmp(rows[i].id, default_id)     ? 0
                          : c.mic                              ? "Windows' default: on the Mic track while it is"
                                                               : "Windows' default";
        ImGui::PushID(i);
        if (opt_row_combo(rows[i].name, sub, "##input", modes, 0, COUNT(modes), &mode, dp(200))) {
            InputDevice *d = (InputDevice *)config_input(c.inputs, c.input_count, rows[i].id);
            if (!d && c.input_count < COUNT(c.inputs)) d = &c.inputs[c.input_count++];
            if (d) {
                *d = rows[i];
                d->mode = (InputMode)mode;
            }
            // Turned off: dropped from the settings.
            for (i32 k = 0; k < c.input_count;)
                if (c.inputs[k].mode == INPUT_OFF) c.inputs[k] = c.inputs[--c.input_count];
                else k++;
            log_line("options: input %s %s", rows[i].name, modes[mode]);
            options_apply(APPLY_AUDIO);
        }
        ImGui::PopID();
    }
}

static void options_recording()
{
    OptionsUi &o = g_app.options;
    Config &c = g_app.config;
    const Recorder &rec = g_app.recorder;
    f32 wide = dp(320);

    opt_section("VIDEO");
    opt_card_begin();
    char preview[160], note[64];
    snprintf(preview, sizeof preview, "%d. not connected", c.output_idx + 1);
    if (c.output_idx >= 0 && c.output_idx < o.monitor_count) {
        monitor_label(c.output_idx, preview, sizeof preview);
        monitor_note(c.output_idx, note, sizeof note);
        size_t n = strlen(preview);
        snprintf(preview + n, sizeof preview - n, " \xC2\xB7 %s", note);
    }
    opt_row_begin("Monitor", 0, wide);
    f32 popup_w = 0;
    for (i32 i = 0; i < o.monitor_count; i++) {
        char label[96];
        monitor_label(i, label, sizeof label);
        monitor_note(i, note, sizeof note);
        popup_w = fmaxf(popup_w, text_w(g_font_bold, label) + dp(32) + text_w(g_font, note) + dp(32));
    }
    if (opt_combo_begin("##monitor", preview, wide, popup_w)) {
        if (ImGui::IsWindowAppearing()) options_list_monitors(); // monitors come and go
        for (i32 i = 0; i < o.monitor_count; i++) {
            char label[96];
            monitor_label(i, label, sizeof label);
            monitor_note(i, note, sizeof note);
            if (opt_combo_item(label, note, i == c.output_idx) && i != c.output_idx) {
                c.output_idx = i;
                log_line("options: monitor %s, %s", label, note);
                options_apply(APPLY_RECORDER);
            }
        }
        opt_combo_end();
    }
    opt_row_end();
    {
        const char *items[1 + COUNT(RES_HEIGHTS)], *notes[1 + COUNT(RES_HEIGHTS)];
        char buf[COUNT(RES_HEIGHTS)][48];
        res_items(items, notes, buf, 0);
        i32 sel = res_index(c.height);
        if (opt_row_combo("Resolution", "Never above the monitor's", "##res", items, notes, COUNT(items), &sel, wide)) {
            c.height = sel ? RES_HEIGHTS[sel - 1] : 0;
            log_line("options: resolution %d (0 = native)", c.height);
            options_apply(APPLY_RECORDER);
        }
    }
    if (opt_row_i32("Frame rate", "Never above the monitor's refresh rate", "##fps", &c.fps, 1, 240, "fps")) {
        log_line("options: %d fps", c.fps);
        options_apply(APPLY_RECORDER);
    }
    if (opt_row_toggle("Show the mouse pointer", 0, "##mouse", &c.draw_mouse)) {
        log_line("options: mouse %s", c.draw_mouse ? "shown" : "hidden");
        options_apply(APPLY_RECORDER);
    }
    opt_card_end();

    opt_section("BITRATE");
    opt_card_begin();
    if (opt_row_f32("While playing a game", "A game can have its own: Recording settings in its menu under Apps", "##bitrate",
                    &c.bitrate_mbps, 1, 300, "Mbps")) {
        log_line("options: game bitrate %g Mbps", c.bitrate_mbps);
        options_apply(APPLY_RECORDER);
    }
    if (opt_row_f32("On the desktop", "While no game is being played", "##desktop", &c.desktop_bitrate_mbps, 1, 300, "Mbps")) {
        log_line("options: desktop bitrate %g Mbps", c.desktop_bitrate_mbps);
        options_apply(APPLY_RECORDER);
    }
    opt_card_end();

    opt_section("ENCODER");
    opt_card_begin();
    static const char *codecs[CODEC_COUNT] = {"H.264", "HEVC", "AV1"};
    static const char *codec_notes[CODEC_COUNT] = {"plays everywhere", "smaller files", "smallest, not for Premiere"};
    u32 no_codec = 0;
    if (rec.encoders_known)
        for (i32 i = 0; i < CODEC_COUNT; i++)
            if (!rec.encoders_ok[i]) no_codec |= 1u << i;
    i32 codec = codec_index(c.codec.s);
    if (opt_row_combo("Format", 0, "##codec", codecs, codec_notes, CODEC_COUNT, &codec, wide, no_codec)) {
        str_set(c.codec.s, CODECS[codec]);
        log_line("options: format %s", c.codec.s);
        options_apply(APPLY_RECORDER);
    }

    // Encoders: Automatic, then each vendor; the ones whose test encode failed for this format are greyed out.
    u8 works = rec.encoders_ok[codec];
    i32 first = -1;
    for (i32 v = 0; v < VENDOR_COUNT && first < 0; v++)
        if (works & (1 << v)) first = v;
    const char *items[VENDOR_COUNT + 1], *notes[VENDOR_COUNT + 1];
    items[0] = "Automatic";
    notes[0] = !rec.encoders_known ? "testing\xE2\x80\xA6" : first < 0 ? "none works" : VENDOR_LABELS[first];
    u32 no_vendor = 0;
    for (i32 v = 0; v < VENDOR_COUNT; v++) {
        bool ok = !rec.encoders_known || (works & (1 << v));
        items[v + 1] = VENDOR_LABELS[v];
        notes[v + 1] = !ok ? "doesn't work here" : v == VENDOR_CPU ? "costs game frames" : 0;
        if (!ok) no_vendor |= 1u << (v + 1);
    }
    i32 enc = vendor_parse(c.encoder.s) + 1;
    char enc_preview[96];
    if (enc == 0) snprintf(enc_preview, sizeof enc_preview, "Automatic \xC2\xB7 %s", notes[0]);
    else snprintf(enc_preview, sizeof enc_preview, "%s", items[enc]);
    char status[160];
    if (rec.paused) snprintf(status, sizeof status, "Paused");
    else if (rec.recording) snprintf(status, sizeof status, "Recording with %s", rec.encoder_used.s);
    else if (rec.last_error.s[0]) snprintf(status, sizeof status, "Not recording: %s", rec.last_error.s);
    else snprintf(status, sizeof status, "Starting");
    if (opt_row_combo("Encoder", status, "##encoder", items, notes, VENDOR_COUNT + 1, &enc, wide, no_vendor, enc_preview)) {
        str_set(c.encoder.s, enc == 0 ? "auto" : VENDOR_KEYS[enc - 1]);
        log_line("options: encoder %s", c.encoder.s);
        options_apply(APPLY_RECORDER);
    }

    i32 vendor = options_vendor();
    static const char *vendor_notes[VENDOR_COUNT] = {"Speed against quality", "AMF has three speeds; P1 to P7 spread over them",
                                                     "Quick Sync's veryfast to veryslow", "CPU: fast presets only"};
    i32 preset = preset_step(c.preset.s);
    if (opt_row_combo("Preset", vendor >= 0 ? vendor_notes[vendor] : vendor_notes[0], "##preset", PRESET_LABELS, PRESET_NOTES,
                      COUNT(PRESET_LABELS), &preset, wide)) {
        str_set(c.preset.s, PRESET_KEYS[preset]);
        log_line("options: preset %s", c.preset.s);
        options_apply(APPLY_RECORDER);
    }
    i32 rc = rc_index(c.rate_control.s);
    if (opt_row_combo("Rate control", 0, "##rc", RC_LABELS, RC_NOTES, COUNT(RC_LABELS), &rc, wide)) {
        str_set(c.rate_control.s, RC_KEYS[rc]);
        log_line("options: rate control %s", c.rate_control.s);
        options_apply(APPLY_RECORDER);
    }

    // What record_args (recorder.cpp) does with each setting, per vendor
    bool cq_on = rc == 2;
    bool av1 = !strcmp(c.codec.s, "av1");
    bool peak_on = rc != 1 && !(vendor == VENDOR_AMF && rc == 2) && !(vendor == VENDOR_CPU && av1 && rc != 2);
    bool aq_on = vendor < 0 || vendor == VENDOR_NVENC || vendor == VENDOR_AMF;
    ImGui::BeginDisabled(!cq_on);
    if (opt_row_i32("Quality level", "0 to 51, lower is better. Constant quality only", "##cq", &c.cq, 0, 51, "")) {
        log_line("options: quality level %d", c.cq);
        options_apply(APPLY_RECORDER);
    }
    ImGui::EndDisabled();
    ImGui::BeginDisabled(!peak_on);
    if (opt_row_f32("Peak bitrate", "Times the bitrate, 1 to 4. Not with CBR", "##peak", &c.peak_factor, 1, 4, "\xC3\x97")) {
        log_line("options: peak %g times the bitrate", c.peak_factor);
        options_apply(APPLY_RECORDER);
    }
    ImGui::EndDisabled();
    ImGui::BeginDisabled(!aq_on);
    if (opt_row_toggle("Adaptive quantisation", "Fewer bands in flat areas. NVENC and AMF only", "##aq", &c.spatial_aq)) {
        log_line("options: adaptive quantisation %s", c.spatial_aq ? "on" : "off");
        options_apply(APPLY_RECORDER);
    }
    ImGui::EndDisabled();
    opt_card_end();

    // The buffer keeps either a length of time or an amount of space, whichever is chosen; the other is shown as
    // an estimate at the game bitrate (the highest a game records at, if one has its own).
    opt_section("BUFFER");
    opt_card_begin();
    static const char *limits[] = {"Length", "Size"};
    i32 by = c.buffer_by_size ? 1 : 0;
    opt_row_begin("Limit the buffer by", 0, dp(200));
    if (kit_segmented("##by", limits, 2, &by, dp(98))) {
        c.buffer_by_size = by == 1;
        log_line("options: buffer limited by %s", c.buffer_by_size ? "size" : "length");
        options_apply(APPLY_RECORDER);
    }
    opt_row_end();
    f32 rate = c.bitrate_mbps;
    for (i32 i = 0; i < c.app_count; i++)
        if (c.apps[i].category == CAT_GAME) rate = fmaxf(rate, c.apps[i].rec.bitrate_mbps);
    f64 bytes_per_hour = 3600.0 * rate / 8.0 * 1e6;
    char est[32], line[128];
    if (!c.buffer_by_size) {
        bytes_text((i64)(c.buffer_hours * bytes_per_hour), est, sizeof est);
        snprintf(line, sizeof line, "Up to about %s at %g Mbps", est, rate);
        if (opt_row_f32("Keep the last", line, "##hours", &c.buffer_hours, 0.1, 24 * 14, "hours")) {
            log_line("options: buffer keeps %g h", c.buffer_hours);
            options_apply(APPLY_RECORDER);
        }
    } else {
        f64 h = c.buffer_gb * 1e9 / bytes_per_hour;
        snprintf(line, sizeof line, "At least %.0f h %02.0f min at %g Mbps", floor(h), fmod(h * 60, 60), rate);
        if (opt_row_f32("Keep up to", line, "##gb", &c.buffer_gb, 1, 100000, "GB")) {
            log_line("options: buffer keeps %g GB", c.buffer_gb);
            options_apply(APPLY_RECORDER);
        }
    }
    if (opt_row_f32("Keep free on its drive", "The oldest footage goes early rather than fill the drive", "##free",
                    &c.min_free_gb, 0, 100000, "GB")) {
        log_line("options: keep %g GB free", c.min_free_gb);
        options_apply(APPLY_RECORDER);
    }
    char used[32], free_text[32], holds[96];
    bytes_text((i64)rec.buffer_bytes, used, sizeof used);
    bytes_text(rec.free_bytes, free_text, sizeof free_text);
    if (rec.oldest_us) {
        i64 mins = (now_us() - rec.oldest_us) / 60000000;
        snprintf(holds, sizeof holds, "%s, the last %lld h %02lld min", used, mins / 60, mins % 60);
    } else snprintf(holds, sizeof holds, "%s", rec.buffer_bytes ? used : "Nothing yet");
    opt_row_value("Holds now", 0, holds, rec.oldest_us ? COL_TEXT : COL_DIM);
    opt_row_value("Free on its drive", 0, free_text);
    opt_card_end();

    opt_section("AUDIO");
    opt_card_begin();
    if (opt_row_toggle("Default input", "Windows' recording device, on the Mic track", "##mic", &c.mic)) {
        log_line("options: mic %s", c.mic ? "on" : "off");
        options_apply(APPLY_AUDIO);
    }
    opt_inputs(c);
    if (opt_row_toggle("Voices in mono", "The Mic and chat tracks in saves, at half the size", "##voicemono", &c.voice_mono)) {
        log_line("options: voices %s", c.voice_mono ? "mono" : "stereo");
        options_apply(0);
    }
    opt_row_begin("ffmpeg.exe", 0, wide);
    char ff[MAX_PATH * 3], text[MAX_PATH * 3];
    wide_to_utf8(c.ffmpeg.s, ff, sizeof ff);
    if (opt_text("##ffmpeg", ff, wide, 0, "Found on the PATH", text, sizeof text)) {
        char *t = trim(text);
        utf8_to_wide(t, c.ffmpeg.s, MAX_PATH);
        log_line("options: ffmpeg %s", t[0] ? t : "found on the PATH");
        options_apply(APPLY_RECORDER);
    }
    opt_row_end();
    opt_card_end();
}

// ---------------------------------------------------------------------------------------------------------------
// General: sessions, folders, sharing, startup and the look.

static bool pick_folder(const Path &start, Path *out)
{
    IFileOpenDialog *d = 0;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, 0, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return false;
    DWORD opts = 0;
    d->GetOptions(&opts);
    d->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    IShellItem *folder = 0;
    if (SUCCEEDED(SHCreateItemFromParsingName(start.s, 0, IID_PPV_ARGS(&folder)))) {
        d->SetFolder(folder);
        folder->Release();
    }
    bool ok = false;
    if (SUCCEEDED(d->Show(g_app.ui.hwnd))) {
        IShellItem *item = 0;
        if (SUCCEEDED(d->GetResult(&item))) {
            wchar_t *p = 0;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                wcscpy_s(out->s, p);
                CoTaskMemFree(p);
                ok = true;
            }
            item->Release();
        }
    }
    d->Release();
    // The dialog took the button-up, so ImGui would think the mouse is still down.
    ImGui::GetIO().AddMouseButtonEvent(0, false);
    return ok;
}

// A folder row: the path in a box (edits count when finished) and a button that opens the folder picker.
static bool opt_row_folder(const char *name, const char *sub, const char *id, Path *p)
{
    f32 fh = ImGui::GetFrameHeight();
    f32 field_w = fmaxf(dp(220), fminf(dp(420), (g_card.w - 2 * dp(OPT_PAD)) * 0.55f));
    opt_row_begin(name, sub, field_w + dp(8) + fh);
    char cur[MAX_PATH * 3], text[MAX_PATH * 3];
    wide_to_utf8(p->s, cur, sizeof cur);
    bool changed = false;
    ImGui::PushID(id);
    if (opt_text("##path", cur, field_w, 0, 0, text, sizeof text) && trim(text)[0]) {
        wchar_t wide[MAX_PATH];
        utf8_to_wide(trim(text), wide, MAX_PATH);
        if (wcscmp(wide, p->s)) {
            wcscpy_s(p->s, wide);
            changed = true;
        }
    }
    ImGui::SameLine(0, dp(8));
    Path picked = {};
    if (kit_icon_button(ICON_FOLDER, "##choose", KIT_SECONDARY) && pick_folder(*p, &picked) && wcscmp(picked.s, p->s)) {
        *p = picked;
        changed = true;
    }
    ImGui::PopID();
    opt_row_end();
    if (changed) {
        wide_to_utf8(p->s, cur, sizeof cur);
        log_line("options: %s %s", name, cur);
    }
    return changed;
}

// The accent colours as a row of swatches; the one in use has a ring.
static bool accent_swatches(i32 *sel)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    f32 d = ImGui::GetFrameHeight(), gap = dp(8), r = d / 2 - dp(4);
    ImVec2 a = ImGui::GetCursorScreenPos();
    bool changed = false;
    for (i32 i = 0; i < ACCENT_COUNT; i++) {
        ImVec2 p(a.x + i * (d + gap), a.y);
        ImGui::SetCursorScreenPos(p);
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##accent", ImVec2(d, d)) && *sel != i) {
            *sel = i;
            changed = true;
        }
        bool hot = ImGui::IsItemHovered();
        ImGui::PopID();
        ImVec2 c(p.x + d / 2, p.y + d / 2);
        dl->AddCircleFilled(c, hot ? r + dp(1) : r, col32(ACCENTS[i].base));
        if (*sel == i) dl->AddCircle(c, d / 2 - dp(0.5f), col32(COL_TEXT), 0, dp(2));
    }
    return changed;
}

static f32 swatches_w() { return (f32)ACCENT_COUNT * ImGui::GetFrameHeight() + (f32)(ACCENT_COUNT - 1) * dp(8); }

static void options_general()
{
    Config &c = g_app.config;

    opt_section("LOOK");
    opt_card_begin();
    opt_row_begin("Accent colour", 0, swatches_w());
    if (accent_swatches(&c.accent)) {
        ui_apply_accent(c.accent);
        log_line("options: accent %s", ACCENTS[c.accent].name);
        options_apply(0);
    }
    opt_row_end();
    static const char *const clocks[] = {"12-hour", "24-hour"}, *const clock_notes[] = {"9:04 pm", "21:04"};
    i32 clock = c.clock_24h ? 1 : 0;
    if (opt_row_combo("Clock", 0, "##clock", clocks, clock_notes, 2, &clock, dp(200))) {
        c.clock_24h = clock == 1;
        log_line("options: %s clock", c.clock_24h ? "24-hour" : "12-hour");
        options_apply(0);
    }
    opt_card_end();

    opt_section("SESSIONS");
    opt_card_begin();
    if (opt_row_f32("Gap that ends a session", "How long a game can be away before its session ends", "##gap",
                    &c.session_gap_min, 0.5, 600, "min")) {
        log_line("options: session gap %g min", c.session_gap_min);
        options_apply(0);
    }
    if (opt_row_f32("Shortest session to offer", "Each game asks, always saves or never: Recording settings in its menu under Apps", "##minlen",
                    &c.session_min_length_min, 0, 1440, "min")) {
        log_line("options: shortest session %g min", c.session_min_length_min);
        options_apply(0);
    }
    opt_card_end();

    opt_section("FOLDERS");
    opt_card_begin();
    if (opt_row_folder("Save folder", "Clips and sessions, in a folder per year", "save", &c.save_dir)) options_apply(0);
    if (opt_row_folder("Buffer folder", 0, "buffer", &c.buffer_dir)) options_apply(APPLY_RECORDER);
    if (opt_row_f32("Keep share copies for", 0, "##share", &c.share_keep_days, 0.1, 365, "days")) {
        log_line("options: share copies kept %g days", c.share_keep_days);
        options_apply(0);
    }
    opt_card_end();

    opt_section("GAMES");
    opt_card_begin();
    if (opt_row_toggle("Look up games online", "Names and icons from Discord's public list of games, covers from Steam. Applies from the next start",
                       "##lookup", &c.game_lookup)) {
        log_line("options: look up games %s", c.game_lookup ? "on" : "off");
        options_apply(0);
    }
    opt_card_end();

    opt_section("STARTUP");
    // The Run value is read when this section comes into view and after the switch is turned, not every frame.
    static wchar_t now_value[MAX_PATH + 8];
    static bool set;
    static i32 read_frame;
    wchar_t cmd[MAX_PATH + 8];
    i32 frame_now = ImGui::GetFrameCount();
    if (!read_frame || frame_now - read_frame > 1) set = startup_read(now_value, COUNT(now_value));
    read_frame = frame_now;
    startup_command(cmd, COUNT(cmd));
    if (!g_app.options.startup_logged) {
        log_line("start with Windows: the Run value is %ls; on, it would be %ls", set ? now_value : L"not set", cmd);
        g_app.options.startup_logged = true;
    }
    char u[MAX_PATH * 3];
    wide_to_utf8(now_value, u, sizeof u);
    const char *runs = !set ? "Nothing runs at sign-in now" : !_wcsicmp(now_value, cmd) ? "This clipback runs at sign-in" : u;
    if (is_test_copy()) runs = "Test copy: the Run list is left alone";
    opt_card_begin();
    if (opt_row_toggle("Start with Windows", runs, "##startup", &c.start_with_windows)) {
        startup_set(c.start_with_windows);
        options_apply(0);
        read_frame = 0; // read it again
    }
    opt_card_end();

    opt_section("DEVELOPER");
    opt_card_begin();
    if (opt_row_toggle("Detailed log", "Every click, key, screen and event, for finding what went wrong",
                       "##devlog", &c.dev_log)) {
        g_dev = c.dev_log || dev_env();
        log_line("detailed log %s", g_dev ? "on" : "off");
        options_apply(0);
    }
    f32 bw = text_w(g_font, "Show the log") + dp(32);
    opt_row_begin("Log and crash dumps", "clipback-native.log, and crash-*.dmp after a crash", bw);
    if (kit_button("Show the log##showlog", KIT_SECONDARY, ImVec2(bw, 0))) {
        Path log = {};
        wcscpy_s(log.s, g_log_path);
        reveal_file(log);
    }
    opt_row_end();
    opt_card_end();
}

// ---------------------------------------------------------------------------------------------------------------

static void screen_options()
{
    OptionsUi &o = g_app.options;
    i32 frame = ImGui::GetFrameCount();
    if (!o.last_frame || frame - o.last_frame > 1) { // entered again: new apps may have arrived, monitors may have changed
        o.order_dirty = true;
        options_list_monitors();
        app_icons_retry();
        if (o.capture_slot) capture_end(true);
    }
    o.last_frame = frame;

    // The sections as an icon and a label over a hairline, the current one underlined in the accent. Each tab is
    // as wide as its bold label, so none moves; the new-apps count sits on the Apps icon and takes no room.
    static const char *names[OPT_TAB_COUNT] = {"Apps", "Recording", "Hotkeys", "General"};
    static const char *icons[OPT_TAB_COUNT] = {ICON_APPS, ICON_VIDEO, ICON_KEYBOARD, ICON_SLIDERS};
    if (o.tab < 0 || o.tab >= OPT_TAB_COUNT) o.tab = OPT_APPS;
    i32 new_apps = 0;
    for (i32 i = 0; i < g_app.config.app_count; i++) // as the New chip counts: not ones a folder rule sorts
        new_apps += app_category(g_app.config, g_app.config.apps[i]) == CAT_NONE;
    char count[8];
    snprintf(count, sizeof count, new_apps > 99 ? "99+" : "%d", new_apps);

    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 a = ImGui::GetCursorScreenPos();
    f32 avail = ImGui::GetContentRegionAvail().x, tab_h = dp(40), gap = dp(30);
    dl->AddLine(ImVec2(a.x, a.y + tab_h - 0.5f), ImVec2(a.x + avail, a.y + tab_h - 0.5f), col32(COL_LINE));
    f32 x = a.x;
    for (i32 t = 0; t < OPT_TAB_COUNT; t++) {
        f32 slot = g_font_icon->FontSize + dp(12), w = slot + text_w(g_font_bold, names[t]);
        bool active = o.tab == t;
        ImGui::SetCursorScreenPos(ImVec2(x - gap / 2, a.y));
        ImGui::PushID(t);
        ImGui::BeginDisabled(o.capture_slot != 0 && !active); // finish setting the hotkey first
        if (ImGui::InvisibleButton("##tab", ImVec2(w + gap, tab_h))) {
            o.tab = t;
            dev_click("tab", names[t]);
        }
        bool hot = ImGui::IsItemHovered();
        ImFont *font = active ? g_font_bold : g_font;
        f32 ty = floorf(a.y + (tab_h - dp(4) - font->FontSize) / 2);
        u32 ink = active ? COL_TEXT : hot ? 0xd4d7dc : COL_DIM;
        f32 iy = floorf(a.y + (tab_h - dp(4) - g_font_icon->FontSize) / 2);
        opt_text_at(dl, g_font_icon, ImVec2(x, iy), active ? COL_ACCENT : ink, icons[t], x + slot);
        opt_text_at(dl, font, ImVec2(x + slot, ty), ink, names[t], x + w + 1);
        f32 line_w = w;
        if (t == OPT_APPS && new_apps) { // a badge on the icon's top right corner, ringed in the page colour
            f32 pw = opt_pill_w(count), ph = dp(18), r = dp(2);
            ImVec2 p(x + g_font_icon->FontSize - dp(9), iy);
            dl->AddRectFilled(ImVec2(p.x - r, p.y - ph / 2 - r), ImVec2(p.x + pw + r, p.y + ph / 2 + r), col32(COL_BG),
                              ph / 2 + r);
            opt_pill(dl, p, count, COL_ACCENT, 0xffffff);
        }
        if (active) dl->AddRectFilled(ImVec2(x, a.y + tab_h - dp(2)), ImVec2(x + line_w, a.y + tab_h), col32(COL_ACCENT), dp(1));
        ImGui::EndDisabled();
        ImGui::PopID();
        x += w + gap;
    }
    ImGui::SetCursorScreenPos(ImVec2(a.x, a.y + tab_h));
    ImGui::Dummy(ImVec2(avail, dp(14)));

    // List pages fit their page and turn pages; the others scroll if the window is short.
    bool list = o.tab == OPT_APPS || o.tab == OPT_HOTKEYS;
    g_card_max_w = list ? FLT_MAX : dp(860);
    ImGui::BeginChild("page", ImVec2(0, 0), ImGuiChildFlags_None,
                      list ? ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse : 0);
    switch (o.tab) {
    case OPT_APPS: options_apps(); break;
    case OPT_RECORDING: options_recording(); break;
    case OPT_HOTKEYS: options_hotkeys(); break;
    case OPT_GENERAL: options_general(); break;
    default: break;
    }
    ImGui::EndChild();
    if (o.capture_slot) ui_wake(); // keep drawing while waiting for keys
}
